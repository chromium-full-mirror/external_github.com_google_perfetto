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

import {isValidDriveId, parseGDriveStateParam} from './gdrive_client';

describe('isValidDriveId', () => {
  test('accepts valid drive IDs and resource keys', () => {
    expect(isValidDriveId('abc123')).toBe(true);
    expect(isValidDriveId('0-aZ_9')).toBe(true);
    expect(isValidDriveId('1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs74OgvE2upms')).toBe(
      true,
    );
  });

  test('rejects empty string', () => {
    expect(isValidDriveId('')).toBe(false);
  });

  test('rejects path traversal', () => {
    expect(isValidDriveId('..')).toBe(false);
    expect(isValidDriveId('../other')).toBe(false);
    expect(isValidDriveId('abc/def')).toBe(false);
    expect(isValidDriveId('abc\\def')).toBe(false);
    expect(isValidDriveId('..%2Fother')).toBe(false);
  });

  test('rejects query and fragment injection', () => {
    expect(isValidDriveId('abc?fields=*')).toBe(false);
    expect(isValidDriveId('abc&alt=media')).toBe(false);
    expect(isValidDriveId('abc#fragment')).toBe(false);
  });

  test('rejects header injection and whitespace', () => {
    expect(isValidDriveId('abc\r\nX-Injected: true')).toBe(false);
    expect(isValidDriveId('abc\ndef')).toBe(false);
    expect(isValidDriveId('abc def')).toBe(false);
  });
});

describe('parseGDriveStateParam', () => {
  test('parses valid state with file ID only', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({ids: ['file_123'], action: 'open'}),
      ),
    ).toEqual({
      ok: true,
      value: {fileId: 'file_123', resourceKey: undefined},
    });
  });

  test('parses valid state with matching resource key', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({
          ids: ['file_123', 'other_456'],
          resourceKeys: {file_123: 'rk-ABC_9'},
        }),
      ),
    ).toEqual({
      ok: true,
      value: {fileId: 'file_123', resourceKey: 'rk-ABC_9'},
    });
  });

  test('ignores resource keys for other file IDs', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({
          ids: ['file_123'],
          resourceKeys: {other_456: 'bad/key'},
        }),
      ),
    ).toEqual({
      ok: true,
      value: {fileId: 'file_123', resourceKey: undefined},
    });
  });

  test('rejects invalid JSON', () => {
    expect(parseGDriveStateParam('not-json')).toEqual({
      ok: false,
      error: 'Invalid JSON',
    });
  });

  test('rejects payloads not matching schema', () => {
    expect(parseGDriveStateParam(JSON.stringify({ids: 'file_123'}))).toEqual({
      ok: false,
      error: 'Invalid GDrive state',
    });
    expect(parseGDriveStateParam(JSON.stringify([]))).toEqual({
      ok: false,
      error: 'Invalid GDrive state',
    });
  });

  test('rejects missing or empty file IDs', () => {
    expect(parseGDriveStateParam(JSON.stringify({}))).toEqual({
      ok: false,
      error: 'No file IDs provided',
    });
    expect(parseGDriveStateParam(JSON.stringify({ids: []}))).toEqual({
      ok: false,
      error: 'No file IDs provided',
    });
    expect(parseGDriveStateParam(JSON.stringify({ids: ['']}))).toEqual({
      ok: false,
      error: 'No file IDs provided',
    });
  });

  test('rejects invalid file ID', () => {
    expect(parseGDriveStateParam(JSON.stringify({ids: ['../secret']}))).toEqual(
      {
        ok: false,
        error: 'Invalid file ID',
      },
    );
  });

  test('rejects invalid resource key', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({
          ids: ['file_123'],
          resourceKeys: {file_123: 'bad\r\nheader'},
        }),
      ),
    ).toEqual({
      ok: false,
      error: 'Invalid resource key',
    });
  });
});
