# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Strategies for acquiring a TraceProcessor per trace."""

import collections
from concurrent.futures import ThreadPoolExecutor
import dataclasses as dc
import logging
import re
import threading
from typing import List, Optional
import weakref

from perfetto.trace_processor.api import (
    TraceProcessor,
    TraceProcessorConfig,
    TraceProcessorException,
)
from perfetto.bigtrace.server import query_result
from perfetto.bigtrace.server.snapshot import NEEDS_RAW_TRACE, SnapshotStore

logger = logging.getLogger(__name__)

# Large traces can take minutes to parse.
_LOAD_TIMEOUT_SECS = 600

# Statements whose effects later queries may depend on.
_STATEFUL_SQL = re.compile(
    r"\b(create|drop|include|insert|update|delete|alter)\b", re.IGNORECASE)


def load_trace(
    trace_path: str,
    tp_config: TraceProcessorConfig,
    cancel_event: Optional[threading.Event] = None,
) -> TraceProcessor:
  """Starts a TraceProcessor with the trace loaded.

  The shell reads the file itself: streaming it through the Python API costs
  Python CPU time per chunk, which serializes parallel loads on the GIL.
  """
  config = dc.replace(
      tp_config,
      cancel_event=cancel_event,
      extra_flags=list(tp_config.extra_flags or []) + [trace_path],
      load_timeout=max(tp_config.load_timeout, _LOAD_TIMEOUT_SECS))
  try:
    return TraceProcessor(config=config)
  except TraceProcessorException as e:
    # Keep the shell's own error rather than the generic startup message.
    match = re.search(r"stderr: (.+)", str(e))
    raise TraceProcessorException(match.group(1) if match else str(e)) from None


def close_quietly(tp: TraceProcessor) -> None:
  try:
    tp.close()
  except Exception:  # pylint: disable=broad-except
    logger.debug("Error closing TraceProcessor", exc_info=True)


@dc.dataclass(frozen=True)
class PoolQuery:
  """A query as the pool sees it; see TraceProcessorPool.start_query."""
  needs_raw: bool = False
  # Session statements to run before the query.
  session_end: int = 0
  stateful: bool = False
  epoch: int = 0


@dc.dataclass
class _Instance:
  from_snapshot: bool
  epoch: int
  # Session statements this instance has run.
  applied: int = 0


class TraceProcessorPool:
  """Hands out TraceProcessor instances. Every acquire must be released.

  Snapshots: with a SnapshotStore, a trace loads from its snapshot if it has
  one. Otherwise it is parsed, and release() schedules a snapshot, which a
  separate shell makes once no query is running (see start_query).

  Fidelity: snapshots rewrite metadata, stats and trace_file, so a query using
  them (NEEDS_RAW_TRACE) gets an instance parsed from the raw trace.

  Session: statements that create, include or change anything are appended to
  a session log by start_query(). Each instance remembers how many entries it
  has run and runs the rest when acquired, so the session survives eviction
  and reloads. reset_session() clears the log and closes idle instances.
  """

  def __init__(self,
               tp_config: Optional[TraceProcessorConfig] = None,
               snapshots: Optional[SnapshotStore] = None):
    self.tp_config = tp_config or TraceProcessorConfig()
    self.snapshots = snapshots
    self._lock = threading.Lock()
    self._instances: "weakref.WeakKeyDictionary[TraceProcessor, _Instance]" = (
        weakref.WeakKeyDictionary())
    self._session: List[Optional[str]] = []
    self._epoch = 0

  def start_query(self, sql: str) -> PoolQuery:
    """Registers a query that is about to run on many traces.

    Every start_query must be followed by end_query.
    """
    if self.snapshots:
      self.snapshots.pause()
    stateful = bool(_STATEFUL_SQL.search(sql))
    with self._lock:
      end = len(self._session)
      if stateful:
        self._session.append(sql)
      return PoolQuery(
          bool(NEEDS_RAW_TRACE.search(sql)), end, stateful, self._epoch)

  def end_query(self, query: PoolQuery, succeeded: bool) -> None:
    """Drops a stateful query that failed everywhere from the session."""
    if self.snapshots:
      self.snapshots.resume()
    with self._lock:
      if query.stateful and not succeeded and query.epoch == self._epoch:
        self._session[query.session_end] = None

  def reset_session(self) -> None:
    with self._lock:
      self._session.clear()
      self._epoch += 1
    self._close_idle()

  def acquire(
      self,
      trace_path: str,
      cancel_event: Optional[threading.Event] = None,
      query: PoolQuery = PoolQuery()
  ) -> TraceProcessor:
    tp = self._take_idle(trace_path, query.needs_raw)
    if tp is None:
      tp = self._load(trace_path, query.needs_raw, cancel_event)
    with self._lock:
      inst = self._instances[tp]
      todo = [s for s in self._session[inst.applied:query.session_end] if s]
      inst.applied = max(inst.applied, query.session_end + query.stateful)
    try:
      for sql in todo:
        try:
          query_result.query(tp, sql)
        except TraceProcessorException as e:
          # It failed on this trace when it first ran too.
          logger.debug("Replaying session SQL on %s failed: %s", trace_path, e)
    except BaseException:
      close_quietly(tp)
      raise
    return tp

  def _load(self, trace_path: str, raw: bool,
            cancel_event: Optional[threading.Event]) -> TraceProcessor:
    snapshot = None
    if self.snapshots and not raw:
      snapshot = self.snapshots.find(trace_path)
    if snapshot:
      try:
        tp = load_trace(snapshot, self.tp_config, cancel_event)
        return self._track(tp, from_snapshot=True)
      except TraceProcessorException as e:
        if cancel_event and cancel_event.is_set():
          raise
        logger.warning("Deleting unloadable snapshot %s: %s", snapshot, e)
        self.snapshots.discard(snapshot)
    tp = load_trace(trace_path, self.tp_config, cancel_event)
    return self._track(tp, from_snapshot=False)

  def _track(self, tp: TraceProcessor, from_snapshot: bool) -> TraceProcessor:
    with self._lock:
      self._instances[tp] = _Instance(from_snapshot, self._epoch)
    return tp

  def loaded_from_snapshot(self, tp: TraceProcessor) -> bool:
    with self._lock:
      return self._instances[tp].from_snapshot

  def release(self,
              trace_path: str,
              tp: TraceProcessor,
              discard: bool = False) -> None:
    with self._lock:
      inst = self._instances.get(tp)
      # Instances from before reset_session() have the old session's state.
      stale = inst is None or inst.epoch != self._epoch
    if self.snapshots and inst and not inst.from_snapshot and not discard:
      self.snapshots.schedule(trace_path, self.shell_flags())
    if discard or stale:
      close_quietly(tp)
    else:
      self._keep(trace_path, tp)

  def shell_flags(self) -> List[str]:
    """Flags of the shells this pool starts, which snapshots must match."""
    cfg = self.tp_config
    return ([] if cfg.ingest_ftrace_in_raw else ["--no-ftrace-raw"]) + (
        ["--dev"] if cfg.enable_dev_features else []) + list(cfg.extra_flags or
                                                             [])

  def _take_idle(self, trace_path: str, raw: bool) -> Optional[TraceProcessor]:
    return None

  def _keep(self, trace_path: str, tp: TraceProcessor) -> None:
    close_quietly(tp)

  def _close_idle(self) -> None:
    pass

  def is_loaded(self, trace_path: str) -> bool:
    """Whether acquiring the trace would skip loading it."""
    return False

  def close(self) -> None:
    self._close_idle()


class EphemeralTraceProcessorPool(TraceProcessorPool):
  """Loads the trace for every query. Lowest memory use."""


class KeepAliveTraceProcessorPool(TraceProcessorPool):
  """Keeps loaded traces between queries so repeated queries skip parsing.

  Only idle instances live in the pool, so an instance is never shared. At most
  `max_size` idle instances are kept; the least recently used are closed.
  Queries run loaded traces first (see is_loaded), so a query over more traces
  than `max_size` still reuses the loaded ones instead of evicting them all.
  """

  def __init__(self,
               tp_config: Optional[TraceProcessorConfig] = None,
               max_size: int = 50,
               snapshots: Optional[SnapshotStore] = None):
    super().__init__(tp_config, snapshots)
    self.max_size = max(1, max_size)
    self._idle: "collections.OrderedDict[str, TraceProcessor]" = (
        collections.OrderedDict())
    self._closed = False

  def _take_idle(self, trace_path: str, raw: bool) -> Optional[TraceProcessor]:
    with self._lock:
      tp = self._idle.pop(trace_path, None)
      if tp is None or not raw or not self._instances[tp].from_snapshot:
        return tp
    close_quietly(tp)  # The query needs the raw trace.
    return None

  def is_loaded(self, trace_path: str) -> bool:
    with self._lock:
      return trace_path in self._idle

  def _keep(self, trace_path: str, tp: TraceProcessor) -> None:
    to_close = []
    with self._lock:
      # A concurrent query may already have returned one for the same trace.
      if self._closed or trace_path in self._idle:
        to_close.append(tp)
      else:
        self._idle[trace_path] = tp
        while len(self._idle) > self.max_size:
          to_close.append(self._idle.popitem(last=False)[1])
    for t in to_close:
      close_quietly(t)

  def close(self) -> None:
    with self._lock:
      self._closed = True
    super().close()

  def _close_idle(self) -> None:
    with self._lock:
      to_close = list(self._idle.values())
      self._idle.clear()
    # Each close waits for its shell to exit; do them together.
    with ThreadPoolExecutor(max_workers=32) as closer:
      closer.map(close_quietly, to_close)
