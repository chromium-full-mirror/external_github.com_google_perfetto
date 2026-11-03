// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {describe, expect, test, vi} from 'vitest';
import {BigtraceAsyncDataSource} from './bigtrace_async_data_source';
import type {BigtraceQueryClient} from './bigtrace_query_client';
import {PollingController} from './polling_controller';
import {queryStore} from './query_store';
import {QueryTabsState} from '../pages/query_tabs_state';

describe('PollingController', () => {
  test('populates preview tableName, schema, and lastProcessedRows on first non-zero poll', async () => {
    const tabsState = new QueryTabsState();
    const tab = tabsState.tabs[0]!;
    const uuid = 'poll-preview-uuid';
    const client = {
      getStatus: vi.fn().mockResolvedValue({
        status: 'IN_PROGRESS',
        processedRows: 100,
        processedTraces: 0,
        totalTraces: 50,
      }),
      getQueryExecution: vi.fn().mockResolvedValue({
        status: 'IN_PROGRESS',
        processedRows: 100,
        tableName: 'a_table_preview',
        tableLink: 'https://example.com/a_table_preview',
        schema: [{name: 'dur', type: 'INT64'}],
      }),
    } as unknown as BigtraceQueryClient;

    tab.materialize = true;
    tab.queryUuid = uuid;
    tab.queryClient = client;
    tab.isLoading = true;
    tab.execution = queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 0,
      tableName: 'a_table',
      tableLink: 'https://example.com/a_table',
    });

    const controller = new PollingController({
      redraw: () => {},
      onHistoryChanged: () => {},
    });
    controller.start(tab);
    for (let i = 0; i < 5; i++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    controller.stop(tab);

    expect(tab.lastProcessedRows).toBe(100);
    expect(tab.execution?.tableName).toBe('a_table_preview');
    expect(tab.execution?.tableLink).toBe(
      'https://example.com/a_table_preview',
    );
    expect(tab.execution?.schema).toEqual([{name: 'dur', type: 'INT64'}]);
    expect(tab.execution?.totalTraces).toBe(50);
  });

  test('clears submit-time tableName and tableLink when a zero-row query succeeds', async () => {
    const tabsState = new QueryTabsState();
    const tab = tabsState.tabs[0]!;
    const uuid = 'poll-zero-rows-uuid';
    const client = {
      getStatus: vi.fn().mockResolvedValue({
        status: 'SUCCESS',
        processedRows: 0,
        processedTraces: 12,
        totalTraces: 12,
      }),
      getQueryExecution: vi.fn().mockResolvedValue({
        status: 'SUCCESS',
        processedRows: 0,
        processedTraces: 12,
        totalTraces: 12,
        startTime: '2026-01-01T00:00:00Z',
        endTime: '2026-01-01T00:00:02Z',
      }),
    } as unknown as BigtraceQueryClient;

    tab.materialize = true;
    tab.queryUuid = uuid;
    tab.queryClient = client;
    tab.isLoading = true;
    tab.execution = queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 0,
      tableName: 'a_table',
      tableLink: 'https://example.com/a_table',
    });

    const controller = new PollingController({
      redraw: () => {},
      onHistoryChanged: () => {},
    });
    controller.start(tab);
    for (let i = 0; i < 5; i++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }

    expect(tab.execution?.status).toBe('SUCCESS');
    expect(tab.execution?.processedTraces).toBe(12);
    expect(tab.execution?.tableName).toBeUndefined();
    expect(tab.execution?.tableLink).toBeUndefined();
  });

  test('leaves new preview rows to the refresh button', async () => {
    vi.useFakeTimers();
    try {
      const tabsState = new QueryTabsState();
      const tab = tabsState.tabs[0]!;
      const uuid = 'poll-preview-growth-uuid';
      const client = {
        getStatus: vi
          .fn()
          .mockResolvedValueOnce({
            status: 'IN_PROGRESS',
            processedRows: 100,
            processedTraces: 0,
            totalTraces: 50,
          })
          .mockResolvedValue({
            status: 'IN_PROGRESS',
            processedRows: 300,
            processedTraces: 0,
            totalTraces: 50,
          }),
        getQueryExecution: vi.fn().mockResolvedValue({
          status: 'IN_PROGRESS',
          processedRows: 100,
          tableName: 'a_table_preview',
        }),
        fetchResults: vi.fn().mockResolvedValue({
          rows: [],
          columns: [],
          schema: [],
          availableColumnNames: [],
        }),
      } as unknown as BigtraceQueryClient;

      const ds = new BigtraceAsyncDataSource(uuid, client, () => 0);
      const refreshSpy = vi.spyOn(ds, 'refresh').mockResolvedValue();

      tab.materialize = true;
      tab.queryUuid = uuid;
      tab.queryClient = client;
      tab.dataSource = ds;
      tab.isLoading = true;
      tab.execution = queryStore.getOrCreate(uuid, {
        status: 'IN_PROGRESS',
        processedRows: 0,
      });

      const controller = new PollingController({
        redraw: () => {},
        onHistoryChanged: () => {},
      });
      controller.start(tab);

      // Poll 1: 0 -> 100 rows; the grid mounts and fetches them itself.
      await vi.advanceTimersByTimeAsync(0);
      expect(tab.lastProcessedRows).toBe(100);

      // Poll 2: 100 -> 300 rows; no refetch, so the refresh button lights up.
      await vi.advanceTimersByTimeAsync(3000);
      expect(refreshSpy).not.toHaveBeenCalled();
      expect(tab.execution?.processedRows).toBe(300);
      expect(tab.lastProcessedRows).toBe(100);

      controller.stop(tab);
    } finally {
      vi.useRealTimers();
    }
  });
});
