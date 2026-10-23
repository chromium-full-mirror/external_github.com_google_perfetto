/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/core/exec/tree_from_intervals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr char kName[] = "TREE FROM INTERVALS";
constexpr uint32_t kNoGroup = std::numeric_limits<uint32_t>::max();

enum class VisitState : uint8_t {
  kUnvisited = 0,
  kVisiting = 1,
  kVisited = 2,
};

struct Edge {
  Ts start;
  Ts end;
  uint32_t next_group;
  uint32_t store_row;
};

struct OutputRow {
  Ts start;
  Ts end;
  uint32_t store_row;
  uint32_t parent_id;
};

struct DfsFrame {
  uint32_t edge_idx;
  uint32_t first_target;
  uint32_t cur_target;
  uint32_t last_target;
};

}  // namespace

struct TreeFromIntervals::State : Breaker::State {
  explicit State(const TreeFromIntervalsSpec& spec)
      : input(spec.input, {kName, "the input"}) {}
  ~State() override;
  void Reset() override;

  GroupTable lanes;
  LaneReader input;
  std::vector<uint32_t> batch_next_groups;

  RowBatch projected;
  RowStore store;
  std::vector<Edge> edges;
  std::vector<uint32_t> edge_lanes;

  // CSR offsets into `edges`: lane `g`'s intervals are
  // `edges[lane_offsets[g] .. lane_offsets[g + 1])`.
  std::vector<uint32_t> lane_offsets;
  std::vector<VisitState> visit_state;
  std::vector<uint32_t> slice_begin;
  std::vector<uint32_t> slice_end;
  std::vector<DfsFrame> dfs_stack;

  std::vector<OutputRow> results;
  size_t served = 0;

  std::vector<uint32_t> gather_rows;
  RowBatch gathered;
};

TreeFromIntervals::State::~State() = default;

void TreeFromIntervals::State::Reset() {
  Breaker::State::Reset();
  lanes.Clear();
  input.Reset();
  batch_next_groups.clear();
  projected.Reset();
  store.Clear();
  edges.clear();
  edge_lanes.clear();
  lane_offsets.clear();
  visit_state.clear();
  slice_begin.clear();
  slice_end.clear();
  dfs_stack.clear();
  results.clear();
  served = 0;
  gather_rows.clear();
  gathered.Reset();
}

TreeFromIntervals::TreeFromIntervals(TreeFromIntervalsSpec spec)
    : spec_(std::move(spec)) {
  PERFETTO_CHECK(spec_.next_key_columns.size() ==
                 spec_.input.key_columns.size());
}

TreeFromIntervals::~TreeFromIntervals() = default;

std::unique_ptr<Breaker::State> TreeFromIntervals::CreateState() const {
  return std::make_unique<State>(spec_);
}

bool TreeFromIntervals::Consume(const RowBatch& in,
                                Breaker::State& base) const {
  auto& s = static_cast<State&>(base);
  uint32_t count = in.size();
  if (count == 0) {
    return true;
  }
  s.status = s.input.Start(in, &s.lanes);
  if (!s.status.ok()) {
    return false;
  }
  const auto key_count = static_cast<uint32_t>(spec_.next_key_columns.size());
  if (key_count > 0) {
    s.batch_next_groups.resize(count);
    if (std::optional<uint32_t> bad = s.lanes.FindOrCreate(
            in, spec_.next_key_columns, s.batch_next_groups.data())) {
      s.status = base::ErrStatus(
          "%s: child key column %u in ON must hold one type, matching parent",
          kName, *bad + 1);
      return false;
    }
  }

  LaneBatch lanes = s.input.batch();
  uint32_t first_store_row = s.store.size();
  for (uint32_t row = 0; row < count; ++row) {
    int64_t start;
    int64_t end;
    LaneBatch::Result result = lanes.Read(row, &start, &end);
    if (result != LaneBatch::Result::kOk) {
      if (result == LaneBatch::Result::kSkip) {
        continue;
      }
      s.status = lanes.Error(result, s.input.name());
      return false;
    }
    if (start == end) {
      continue;
    }

    uint32_t lane = lanes.lane(row);
    for (uint32_t k = 0; k < key_count; ++k) {
      if (s.lanes.IsNull(lane, k)) {
        lane = kNoGroup;
        break;
      }
    }

    uint32_t next_group = key_count > 0 ? s.batch_next_groups[row] : 0;
    for (uint32_t k = 0; k < key_count; ++k) {
      if (s.lanes.IsNull(next_group, k)) {
        next_group = kNoGroup;
        break;
      }
    }

    s.edges.push_back({static_cast<Ts>(start), static_cast<Ts>(end), next_group,
                       first_store_row + row});
    s.edge_lanes.push_back(lane);
  }

  if (!spec_.retained_columns.empty()) {
    s.projected.Project(in, spec_.retained_columns);
    s.status = s.store.Append(s.projected);
    if (!s.status.ok()) {
      return false;
    }
  }
  return true;
}

bool TreeFromIntervals::Finalize(Breaker::State& base) const {
  auto& s = static_cast<State&>(base);
  const auto edge_count = static_cast<uint32_t>(s.edges.size());
  if (edge_count == 0) {
    return true;
  }

  // Pack edges by lane into a CSR array using a two-pass counting sort.
  // Bucket `lane_count` holds rows with a NULL parent key (`kNoGroup`), which
  // can never be matched as parents.
  const uint32_t lane_count = std::max<uint32_t>(s.lanes.size(), 1);
  s.lane_offsets.assign(lane_count + 2, 0);
  for (uint32_t lane : s.edge_lanes) {
    uint32_t bucket = lane < lane_count ? lane : lane_count;
    ++s.lane_offsets[bucket + 1];
  }
  for (uint32_t g = 0; g <= lane_count; ++g) {
    s.lane_offsets[g + 1] += s.lane_offsets[g];
  }
  {
    std::vector<uint32_t> next(s.lane_offsets.begin(),
                               s.lane_offsets.end() - 1);
    std::vector<Edge> csr_edges(edge_count);
    for (uint32_t i = 0; i < edge_count; ++i) {
      uint32_t lane = s.edge_lanes[i];
      uint32_t bucket = lane < lane_count ? lane : lane_count;
      csr_edges[next[bucket]++] = s.edges[i];
    }
    s.edges = std::move(csr_edges);
    s.edge_lanes = {};
  }

  // RequireIntervalLanes sorts each lane by `ts`; only sort a lane if raw
  // ungrouped input arrived out of `start` order.
  for (uint32_t g = 0; g < lane_count; ++g) {
    uint32_t begin = s.lane_offsets[g];
    uint32_t end = s.lane_offsets[g + 1];
    if (end - begin > 1 &&
        !std::is_sorted(s.edges.begin() + begin, s.edges.begin() + end,
                        [](const Edge& a, const Edge& b) {
                          return std::tie(a.start, a.end) <
                                 std::tie(b.start, b.end);
                        })) {
      std::stable_sort(s.edges.begin() + begin, s.edges.begin() + end,
                       [](const Edge& a, const Edge& b) {
                         return std::tie(a.start, a.end) <
                                std::tie(b.start, b.end);
                       });
    }
  }

  s.lanes.Clear();
  s.batch_next_groups = {};
  s.visit_state.assign(edge_count, VisitState::kUnvisited);
  s.slice_begin.assign(edge_count, 0);
  s.slice_end.assign(edge_count, 0);
  s.results.reserve(edge_count);

  auto make_frame = [&](uint32_t edge_idx) -> DfsFrame {
    const Edge& e = s.edges[edge_idx];
    if (e.next_group >= lane_count) {
      return {edge_idx, 0, 0, 0};
    }
    uint32_t target_begin = s.lane_offsets[e.next_group];
    uint32_t target_end = s.lane_offsets[e.next_group + 1];
    if (target_begin == target_end) {
      return {edge_idx, 0, 0, 0};
    }
    auto it = std::partition_point(
        s.edges.begin() + target_begin, s.edges.begin() + target_end,
        [start = e.start](const Edge& f) { return f.end <= start; });
    auto first = static_cast<uint32_t>(it - s.edges.begin());
    auto end_it = std::partition_point(
        s.edges.begin() + first, s.edges.begin() + target_end,
        [end = e.end](const Edge& f) { return f.start < end; });
    auto last = static_cast<uint32_t>(end_it - s.edges.begin());
    return {edge_idx, first, first, last};
  };

  for (uint32_t r0 = 0; r0 < edge_count; ++r0) {
    if (s.visit_state[r0] == VisitState::kVisited) {
      continue;
    }
    s.visit_state[r0] = VisitState::kVisiting;
    s.dfs_stack.push_back(make_frame(r0));

    while (!s.dfs_stack.empty()) {
      DfsFrame& top = s.dfs_stack.back();
      const Edge& e = s.edges[top.edge_idx];

      bool pushed_child = false;
      while (top.cur_target < top.last_target) {
        uint32_t f_idx = top.cur_target++;
        if (f_idx == top.edge_idx) {
          continue;
        }
        const Edge& f = s.edges[f_idx];
        if (f.end <= e.start || f.start >= e.end) {
          continue;
        }
        if (s.visit_state[f_idx] == VisitState::kUnvisited) {
          s.visit_state[f_idx] = VisitState::kVisiting;
          s.dfs_stack.push_back(make_frame(f_idx));
          pushed_child = true;
          break;
        }
      }
      if (pushed_child) {
        continue;
      }

      uint32_t edge_idx = top.edge_idx;
      uint32_t first_target = top.first_target;
      uint32_t last_target = top.last_target;
      s.dfs_stack.pop_back();

      s.slice_begin[edge_idx] = static_cast<uint32_t>(s.results.size());
      Ts pos = e.start;
      for (uint32_t f_idx = first_target; f_idx < last_target && pos < e.end;
           ++f_idx) {
        if (f_idx == edge_idx) {
          continue;
        }
        const Edge& f = s.edges[f_idx];
        if (f.end <= pos) {
          continue;
        }
        if (f.start >= e.end) {
          break;
        }
        if (f.start > pos) {
          s.results.push_back({pos, f.start, e.store_row, kNoNode});
          pos = f.start;
        }
        Ts ov_end = std::min(e.end, f.end);
        if (s.visit_state[f_idx] == VisitState::kVisiting) {
          // Break cycle by treating the active ancestor as a root slice.
          s.results.push_back({pos, ov_end, e.store_row, kNoNode});
        } else {
          uint32_t sb = s.slice_begin[f_idx];
          uint32_t se = s.slice_end[f_idx];
          auto slice_it = std::partition_point(
              s.results.begin() + sb, s.results.begin() + se,
              [pos](const OutputRow& sl) { return sl.end <= pos; });
          for (auto idx = static_cast<uint32_t>(slice_it - s.results.begin());
               idx < se; ++idx) {
            const OutputRow sl = s.results[idx];
            if (sl.start >= ov_end) {
              break;
            }
            Ts sub_start = std::max(pos, sl.start);
            Ts sub_end = std::min(ov_end, sl.end);
            if (sub_end > sub_start) {
              s.results.push_back({sub_start, sub_end, e.store_row, idx});
            }
          }
        }
        pos = ov_end;
      }
      if (pos < e.end) {
        s.results.push_back({pos, e.end, e.store_row, kNoNode});
      }
      s.slice_end[edge_idx] = static_cast<uint32_t>(s.results.size());
      s.visit_state[edge_idx] = VisitState::kVisited;
    }
  }

  s.edges = {};
  s.lane_offsets = {};
  s.visit_state = {};
  s.slice_begin = {};
  s.slice_end = {};
  s.dfs_stack = {};

  if (s.results.size() >= kNoNode) {
    s.status = base::ErrStatus("%s: the relation has too many nodes", kName);
    return false;
  }
  return true;
}

bool TreeFromIntervals::Serve(RowBatch& out, Breaker::State& base) const {
  auto& s = static_cast<State&>(base);
  if (s.served >= s.results.size()) {
    return false;
  }
  auto start_id = static_cast<uint32_t>(s.served);
  auto count = static_cast<uint32_t>(
      std::min<size_t>(s.results.size() - s.served, kMaxBatchRows));
  const OutputRow* rows = s.results.data() + s.served;

  BoundsColumns bounds;
  bounds.Take(*s.context);
  ColumnBuffer parent_buffer = s.context->TakeBuffer();
  ColumnBuffer numbers = s.context->TakeBuffer();
  ColumnChunk& parent_chunk = parent_buffer.chunk();
  uint32_t* parent_vals = parent_chunk.Values<uint32_t>();
  uint32_t* number_vals = numbers.chunk().Values<uint32_t>();
  parent_chunk.SizeValidity();
  parent_chunk.validity.FillBits(0, count, true);

  s.gather_rows.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    bounds.ts[i] = static_cast<int64_t>(rows[i].start);
    bounds.dur[i] = static_cast<int64_t>(rows[i].end - rows[i].start);
    s.gather_rows[i] = rows[i].store_row;
    number_vals[i] = start_id + i;
    uint32_t pid = rows[i].parent_id;
    parent_vals[i] = pid;
    if (pid == kNoNode) {
      parent_chunk.validity.clear(i);
    }
  }

  if (!spec_.retained_columns.empty()) {
    const uint32_t* idx = s.gather_rows.data();
    s.store.Gather(&s.gathered, {idx, idx + count}, *s.context);
  }

  for (const TreeFromIntervalsSpec::Output& output : spec_.outputs) {
    switch (output.source) {
      case TreeFromIntervalsSpec::Source::kTs:
        bounds.AddTsTo(&out);
        break;
      case TreeFromIntervalsSpec::Source::kDur:
        bounds.AddDurTo(&out);
        break;
      case TreeFromIntervalsSpec::Source::kId:
        out.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr,
                                            start_id),
                      ColumnBuffer{});
        break;
      case TreeFromIntervalsSpec::Source::kParentId:
        out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parent_vals,
                                            &parent_chunk.validity),
                      parent_buffer);
        break;
      case TreeFromIntervalsSpec::Source::kInput:
        out.AddColumn(s.gathered.column(output.index),
                      s.gathered.buffer(output.index));
        break;
    }
  }
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, number_vals),
                std::move(numbers));
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parent_vals),
                std::move(parent_buffer));
  out.SetRowCount(count);
  s.served += count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
