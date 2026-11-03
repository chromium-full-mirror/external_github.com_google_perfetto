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

import {describe, expect, test} from 'vitest';
import {queryStore} from './query_store';

describe('queryStore', () => {
  test('ignores empty schema arrays from history merges', () => {
    const uuid = 'schema-preserve-uuid';
    queryStore.getOrCreate(uuid, {
      status: 'SUCCESS',
      processedRows: 50,
      schema: [{name: 'dur', type: 'INT64'}],
    });
    // History list responses carry an empty schema.
    queryStore.getOrCreate(uuid, {
      status: 'SUCCESS',
      processedRows: 50,
      schema: [],
    });
    queryStore.update(uuid, {schema: []});

    expect(queryStore.getOrCreate(uuid).schema).toEqual([
      {name: 'dur', type: 'INT64'},
    ]);
  });

  test('advances to buildingFullTable without regressing on stale Phase 1 history', () => {
    const uuid = 'stage-monotonic-uuid';
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 1000,
      processedTraces: 850,
      totalTraces: 10000,
      buildingFullTable: false,
    });
    // Phase 2 starts: processedTraces resets to 0 while buildingFullTable flips true.
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 1000,
      processedTraces: 3,
      totalTraces: 10000,
      buildingFullTable: true,
    });
    expect(queryStore.getOrCreate(uuid).buildingFullTable).toBe(true);
    expect(queryStore.getOrCreate(uuid).processedTraces).toBe(3);

    // A stale Phase 1 list entry with equal processedRows must not rewind stage or traces.
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 1000,
      processedTraces: 850,
      totalTraces: 10000,
      buildingFullTable: false,
    });
    expect(queryStore.getOrCreate(uuid).buildingFullTable).toBe(true);
    expect(queryStore.getOrCreate(uuid).processedTraces).toBe(3);
  });
});
