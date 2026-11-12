// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The tus uploader against a fake server: batch creation, chunked PATCH, resume from the server offset, pause, retry with back-off,
// the disk-full and too-large errors, the 404 recreate, and the localStorage record for resuming after a reload.
import { describe, expect, it, vi } from 'vitest';
import { DiskFullError, MAX_FILE_BYTES, TusUploader, dropSaved, fingerprintOf, loadSaved, saveBatch, type SavedBatch } from '../src/net/tus';

const tick = () => new Promise((r) => setTimeout(r, 0));

function memoryStorage(): Storage {
  const m = new Map<string, string>();
  return {
    get length() {
      return m.size;
    },
    clear: () => m.clear(),
    getItem: (k) => m.get(k) ?? null,
    key: (i) => [...m.keys()][i] ?? null,
    removeItem: (k) => void m.delete(k),
    setItem: (k, v) => void m.set(k, String(v)),
  };
}

function file(name: string, size: number, lastModified = 1000): File {
  return new File([new Uint8Array(size)], name, { lastModified });
}

interface Call {
  url: string;
  method: string;
  headers: Record<string, string>;
  size?: number;
  body?: string;
}

interface Server {
  fetchImpl: typeof fetch;
  calls: Call[];
  offsets: Map<string, number>;
  /** Answers a PATCH instead of the server (return undefined to let the server handle it). */
  onPatch?: (url: string, n: number) => Response | Error | undefined;
  maxActive: number;
}

function fakeServer(): Server {
  const lengths = new Map<string, number>();
  const s: Server = { calls: [], offsets: new Map(), maxActive: 0, fetchImpl: undefined as unknown as typeof fetch };
  let active = 0;
  let patchN = 0;
  s.fetchImpl = (async (input: RequestInfo | URL, init: RequestInit = {}) => {
    const url = String(input);
    const method = init.method ?? 'GET';
    const headers = (init.headers ?? {}) as Record<string, string>;
    const body = init.body;
    if (init.signal?.aborted) throw new DOMException('aborted', 'AbortError');
    s.calls.push({ url, method, headers, ...(body instanceof Blob ? { size: body.size } : {}), ...(typeof body === 'string' ? { body } : {}) });
    if (url === '/api/upload-batches') {
      const files = (JSON.parse(String(body)) as { files: { relpath: string; size: number }[] }).files;
      const out = files.map((f, i) => {
        const id = `up_${s.offsets.size + i}_${f.relpath.replace(/\W/g, '')}`;
        lengths.set(`/api/uploads/${id}`, f.size);
        s.offsets.set(`/api/uploads/${id}`, 0);
        return { relpath: f.relpath, upload_id: id, url: `/api/uploads/${id}` };
      });
      return new Response(JSON.stringify({ batch_id: 'bt_1', files: out }), { status: 201 });
    }
    if (url === '/api/uploads' && method === 'POST') {
      const id = `/api/uploads/up_new${s.offsets.size}`;
      s.offsets.set(id, 0);
      lengths.set(id, Number(headers['Upload-Length']));
      return new Response(null, { status: 201, headers: { Location: id, 'Upload-Offset': '0' } });
    }
    if (method === 'HEAD') {
      if (!s.offsets.has(url)) return new Response(null, { status: 404 });
      return new Response(null, { status: 200, headers: { 'Upload-Offset': String(s.offsets.get(url)), 'Upload-Length': String(lengths.get(url) ?? 0) } });
    }
    if (method === 'DELETE') return new Response(null, { status: 204 });
    if (method === 'PATCH') {
      const n = patchN++;
      const custom = s.onPatch?.(url, n);
      if (custom instanceof Error) throw custom;
      if (custom) return custom;
      active++;
      s.maxActive = Math.max(s.maxActive, active);
      await tick();
      active--;
      if (init.signal?.aborted) throw new DOMException('aborted', 'AbortError');
      const at = s.offsets.get(url) ?? 0;
      if (Number(headers['Upload-Offset']) !== at) return new Response(null, { status: 409, headers: { 'Upload-Offset': String(at) } });
      const next = at + (body as Blob).size;
      s.offsets.set(url, next);
      return new Response(null, { status: 204, headers: { 'Upload-Offset': String(next) } });
    }
    return new Response(null, { status: 400 });
  }) as typeof fetch;
  return s;
}

const instant = () => vi.fn(async () => undefined);

describe('creation and chunked upload', () => {
  it('creates one batch with relpath and size, then PATCHes 8-byte chunks with Upload-Offset', async () => {
    const server = fakeServer();
    const storage = memoryStorage();
    const up = new TusUploader({ entries: [{ file: file('REC01.WAV', 20), relpath: 'ZOOM0001.zprj/REC01.WAV' }], chunkBytes: 8, fetchImpl: server.fetchImpl, sleep: instant(), storage });
    await up.start();
    expect(up.batchId).toBe('bt_1');
    expect(server.calls[0]!.url).toBe('/api/upload-batches');
    expect(JSON.parse(server.calls[0]!.body!)).toEqual({ files: [{ relpath: 'ZOOM0001.zprj/REC01.WAV', size: 20 }] });
    expect(loadSaved(storage)).toHaveLength(1);
    await up.whenSettled();
    const patches = server.calls.filter((c) => c.method === 'PATCH');
    expect(patches.map((p) => p.headers['Upload-Offset'])).toEqual(['0', '8', '16']);
    expect(patches.map((p) => p.size)).toEqual([8, 8, 4]);
    expect(patches[0]!.headers['Content-Type']).toBe('application/offset+octet-stream');
    expect(patches[0]!.headers['Tus-Resumable']).toBe('1.0.0');
    expect(up.files[0]!.state).toBe('done');
    expect(up.allDone).toBe(true);
    expect(up.uploadedBytes).toBe(20);
    expect(loadSaved(storage)).toHaveLength(0); // a finished batch is forgotten
  });

  it('runs at most three files at once', async () => {
    const server = fakeServer();
    const entries = Array.from({ length: 5 }, (_, i) => ({ file: file(`R${i}.WAV`, 4), relpath: `R${i}.WAV` }));
    const up = new TusUploader({ entries, chunkBytes: 4, fetchImpl: server.fetchImpl, sleep: instant(), storage: null });
    await up.start();
    await up.whenSettled();
    expect(up.allDone).toBe(true);
    expect(server.maxActive).toBe(3);
  });
});

describe('resume', () => {
  it('continues from the offset the server reports and creates no batch', async () => {
    const server = fakeServer();
    server.offsets.set('/api/uploads/up_a', 10);
    const f = file('REC01.WAV', 20, 5);
    const saved: SavedBatch = { batch_id: 'bt_9', project_id: 'p_1', name: 'x', created: 1, files: [{ relpath: 'REC01.WAV', url: '/api/uploads/up_a', fingerprint: fingerprintOf(f), size: 20, done: false }] };
    const up = new TusUploader({ entries: [{ file: f, relpath: 'REC01.WAV' }], resume: saved, chunkBytes: 8, fetchImpl: server.fetchImpl, sleep: instant(), storage: memoryStorage() });
    await up.start();
    await up.whenSettled();
    expect(server.calls.some((c) => c.url === '/api/upload-batches')).toBe(false);
    expect(server.calls.filter((c) => c.method === 'PATCH').map((c) => c.headers['Upload-Offset'])).toEqual(['10', '18']);
    expect(up.batchId).toBe('bt_9');
    expect(up.allDone).toBe(true);
  });

  it('ends on the server the saved files that were not picked again, and ignores a changed file', async () => {
    const server = fakeServer();
    server.offsets.set('/api/uploads/up_a', 0);
    server.offsets.set('/api/uploads/up_b', 0);
    const a = file('A.WAV', 4, 5);
    const saved: SavedBatch = {
      batch_id: 'bt_9',
      project_id: '',
      name: '',
      created: 1,
      files: [
        { relpath: 'A.WAV', url: '/api/uploads/up_a', fingerprint: fingerprintOf(a), size: 4, done: false },
        { relpath: 'B.WAV', url: '/api/uploads/up_b', fingerprint: 'B.WAV|4|5', size: 4, done: false },
      ],
    };
    const up = new TusUploader({ entries: [{ file: a, relpath: 'A.WAV' }], resume: saved, chunkBytes: 8, fetchImpl: server.fetchImpl, sleep: instant(), storage: null });
    await up.start();
    await up.whenSettled();
    expect(up.missing).toEqual(['B.WAV']);
    expect(server.calls.some((c) => c.method === 'DELETE' && c.url === '/api/uploads/up_b')).toBe(true);
  });

  it('keeps {batch_id, relpath -> url, fingerprint, project_id} in localStorage, tolerating a broken storage', () => {
    const storage = memoryStorage();
    const b: SavedBatch = { batch_id: 'bt_1', project_id: 'p_1', name: 'n', created: 1, files: [{ relpath: 'a.wav', url: '/api/uploads/up_1', fingerprint: 'a.wav|1|2', size: 1, done: false }] };
    saveBatch(b, storage);
    expect(loadSaved(storage)).toEqual([b]);
    dropSaved('bt_1', storage);
    expect(loadSaved(storage)).toEqual([]);
    const broken = { getItem: () => { throw new Error('denied'); }, setItem: () => { throw new Error('denied'); }, removeItem: () => { throw new Error('denied'); } } as unknown as Storage;
    expect(() => saveBatch(b, broken)).not.toThrow();
    expect(loadSaved(broken)).toEqual([]);
  });
});

describe('pause and resume', () => {
  it('aborts the running chunk on pause and continues from the server offset on resume', async () => {
    const server = fakeServer();
    let up!: TusUploader;
    let paused = false;
    up = new TusUploader({
      entries: [{ file: file('A.WAV', 24), relpath: 'A.WAV' }],
      chunkBytes: 8,
      fetchImpl: server.fetchImpl,
      sleep: instant(),
      storage: null,
      onChange: () => {
        if (!paused && up?.files[0]?.offset === 8) {
          paused = true;
          up.pause('A.WAV');
        }
      },
    });
    await up.start();
    for (let i = 0; i < 5; i++) await tick();
    expect(up.files[0]!.state).toBe('paused');
    expect(up.files[0]!.offset).toBe(8);
    up.resume('A.WAV');
    await up.whenSettled();
    expect(up.allDone).toBe(true);
    // after the pause the offset came back from the server (HEAD), not from memory
    expect(server.calls.filter((c) => c.method === 'HEAD')).toHaveLength(2);
    expect(server.calls.filter((c) => c.method === 'PATCH').map((c) => c.headers['Upload-Offset'])).toEqual(['0', '8', '16']);
  });

  it('pauseAll / resumeAll freeze and restart the queue', async () => {
    const server = fakeServer();
    const entries = Array.from({ length: 4 }, (_, i) => ({ file: file(`R${i}.WAV`, 4), relpath: `R${i}.WAV` }));
    const up = new TusUploader({ entries, chunkBytes: 4, fetchImpl: server.fetchImpl, sleep: instant(), storage: null, concurrency: 1 });
    up.paused = true;
    await up.start();
    expect(up.files.every((f) => f.state === 'queued')).toBe(true);
    up.pauseAll();
    expect(up.files.every((f) => f.state === 'paused')).toBe(true);
    up.resumeAll();
    await up.whenSettled();
    expect(up.allDone).toBe(true);
  });
});

describe('failures', () => {
  it('retries a 500 once by itself after a back-off', async () => {
    const server = fakeServer();
    const sleep = instant();
    server.onPatch = (_u, n) => (n === 0 ? new Response(null, { status: 500 }) : undefined);
    const up = new TusUploader({ entries: [{ file: file('A.WAV', 4), relpath: 'A.WAV' }], chunkBytes: 8, fetchImpl: server.fetchImpl, sleep, storage: null });
    await up.start();
    await up.whenSettled();
    expect(sleep).toHaveBeenCalledWith(1000);
    expect(up.files[0]!.state).toBe('done');
    expect(up.files[0]!.failures).toBe(0);
  });

  it('a lost network keeps the file uploading and offline, with growing back-off, and needs no click', async () => {
    const server = fakeServer();
    const sleep = instant();
    let offlineSeen = false;
    server.onPatch = (_u, n) => (n < 2 ? new TypeError('Failed to fetch') : undefined);
    const up = new TusUploader({
      entries: [{ file: file('A.WAV', 4), relpath: 'A.WAV' }],
      chunkBytes: 8,
      fetchImpl: server.fetchImpl,
      sleep,
      storage: null,
      onChange: () => {
        if (up?.offline) offlineSeen = true;
      },
    });
    await up.start();
    await up.whenSettled();
    expect(offlineSeen).toBe(true);
    expect((sleep.mock.calls as unknown as number[][]).map((c) => c[0])).toEqual([500, 1000]);
    expect(up.files[0]!.state).toBe('done');
    expect(up.offline).toBe(false);
  });

  it('shows an error after repeated server failures; Повторити starts it again, and failures accumulate', async () => {
    const server = fakeServer();
    const up = new TusUploader({ entries: [{ file: file('A.WAV', 4), relpath: 'A.WAV' }, { file: file('B.WAV', 4), relpath: 'B.WAV' }], chunkBytes: 8, fetchImpl: server.fetchImpl, sleep: instant(), storage: null });
    server.onPatch = (url) => (url.includes('_AWAV') ? new Response(null, { status: 500 }) : undefined);
    await up.start();
    await up.whenSettled();
    const a = up.files.find((f) => f.relpath === 'A.WAV')!;
    expect(a.state).toBe('error');
    expect(a.errorKind).toBe('failed');
    expect(a.failures).toBe(2);
    // the other file was not held back by it
    expect(up.files.find((f) => f.relpath === 'B.WAV')!.state).toBe('done');
    await up.retry('A.WAV');
    await tick();
    for (let i = 0; i < 5; i++) await tick();
    expect(a.state).toBe('error');
    expect(a.failures).toBe(3);
  });

  it('maps PRECONDITION_FAILED with available_bytes / needed_bytes to DiskFullError, before any byte is sent', async () => {
    const fetchImpl = vi.fn(async () => new Response(JSON.stringify({ ok: false, error: { code: 'PRECONDITION_FAILED', message: 'not enough free disk space', details: { available_bytes: 1200, needed_bytes: 4800 } } }), { status: 412 })) as unknown as typeof fetch;
    const up = new TusUploader({ entries: [{ file: file('A.WAV', 4), relpath: 'A.WAV' }], fetchImpl, sleep: instant(), storage: null });
    const err = await up.start().then(
      () => null,
      (e: unknown) => e,
    );
    expect(err).toBeInstanceOf(DiskFullError);
    expect((err as DiskFullError).availableBytes).toBe(1200);
    expect((err as DiskFullError).neededBytes).toBe(4800);
    expect(fetchImpl).toHaveBeenCalledTimes(1);
  });

  it('marks a file over 20 GB as too large without asking the server', async () => {
    const fetchImpl = vi.fn() as unknown as typeof fetch;
    const huge = { name: 'REC06.WAV', size: MAX_FILE_BYTES + 1, lastModified: 1, slice: () => new Blob() } as unknown as File;
    const up = new TusUploader({ entries: [{ file: huge, relpath: 'REC06.WAV' }], fetchImpl, sleep: instant(), storage: null });
    await up.start();
    await up.whenSettled();
    expect(up.files[0]!.state).toBe('error');
    expect(up.files[0]!.errorKind).toBe('too_large');
    expect(fetchImpl).not.toHaveBeenCalled();
    expect(up.totalBytes).toBe(0);
  });

  it('a 404 (the upload expired) is an error whose retry creates a new upload with the metadata', async () => {
    const server = fakeServer();
    const up = new TusUploader({ entries: [{ file: file('A.WAV', 4), relpath: 'dir/A.WAV' }], chunkBytes: 8, fetchImpl: server.fetchImpl, sleep: instant(), storage: null });
    await up.start();
    await up.whenSettled();
    // forget the upload on the server and send the file again
    const f = up.files[0]!;
    server.offsets.delete(f.url);
    f.state = 'error';
    f.errorKind = 'gone';
    await up.retry('dir/A.WAV');
    for (let i = 0; i < 5; i++) await tick();
    const created = server.calls.find((c) => c.url === '/api/uploads' && c.method === 'POST')!;
    expect(created.headers['Upload-Length']).toBe('4');
    expect(created.headers['Upload-Metadata']).toContain(`relpath ${btoa('dir/A.WAV')}`);
    expect(created.headers['Upload-Metadata']).toContain(`batch_id ${btoa('bt_1')}`);
    expect(up.files[0]!.url).not.toBe('');
  });
});

describe('cancel', () => {
  it('ends the unfinished uploads on the server and forgets the saved batch', async () => {
    const server = fakeServer();
    const storage = memoryStorage();
    server.onPatch = () => new Response(null, { status: 500 });
    const up = new TusUploader({ entries: [{ file: file('A.WAV', 4), relpath: 'A.WAV' }], chunkBytes: 8, fetchImpl: server.fetchImpl, sleep: instant(), storage });
    await up.start();
    await up.whenSettled();
    expect(up.files[0]!.state).toBe('error');
    await up.cancel();
    expect(up.cancelled).toBe(true);
    expect(server.calls.some((c) => c.method === 'DELETE')).toBe(true);
    expect(loadSaved(storage)).toHaveLength(0);
  });
});
