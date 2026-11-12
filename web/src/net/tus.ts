// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// A small resumable uploader for the server's tus 1.0 endpoints (/api/upload-batches, /api/uploads/<id>). One TusUploader is one batch:
// every file has its own state machine (queued / uploading / paused / done / error), three files go at once, a file is sent in chunks
// (PATCH with Upload-Offset), a lost network is retried with back-off, and after a reload the offset comes back from the server (HEAD).
// fetch only (no XHR): progress is reported per chunk. The cookie / bearer the UI already uses rides along (same-origin).

export const MAX_FILE_BYTES = 20 * 1024 ** 3;
export const CHUNK_BYTES = 8 * 1024 * 1024;
export const CONCURRENCY = 3;
/** Non-network failures retried by themselves before the file shows an error. */
const AUTO_RETRIES = 1;
const STORE_KEY = 'rovox.uploads';
const TUS = { 'Tus-Resumable': '1.0.0' };

export type FileState = 'queued' | 'uploading' | 'paused' | 'done' | 'error';
export type ErrorKind = 'too_large' | 'failed' | 'gone';

export interface TusEntry {
  file: File;
  relpath: string;
}

export interface TusFile {
  relpath: string;
  name: string;
  size: number;
  fingerprint: string;
  state: FileState;
  offset: number;
  /** "/api/uploads/up_…" once the server created the upload. */
  url: string;
  /** Failed attempts (automatic and manual retries). */
  failures: number;
  errorKind?: ErrorKind;
  /** True while the last request died on the network (the file stays "uploading" and retries by itself). */
  offline: boolean;
}

export interface SavedFile {
  relpath: string;
  url: string;
  fingerprint: string;
  size: number;
  done: boolean;
}

/** What is kept in localStorage to resume after a reload. */
export interface SavedBatch {
  batch_id: string;
  project_id: string;
  name: string;
  created: number;
  files: SavedFile[];
}

export class DiskFullError extends Error {
  constructor(
    readonly availableBytes: number,
    readonly neededBytes: number,
  ) {
    super('not enough free disk space on the server');
  }
}

export class TusError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly code = '',
  ) {
    super(message);
  }
}

export function fingerprintOf(file: Pick<File, 'name' | 'size' | 'lastModified'>): string {
  return `${file.name}|${file.size}|${file.lastModified}`;
}

//==============================================================================
// persistence (every access in try/catch: private mode, quota, blocked storage)
function storageOf(s?: Storage | null): Storage | null {
  if (s !== undefined) return s;
  try {
    return typeof localStorage === 'undefined' ? null : localStorage;
  } catch {
    return null;
  }
}

export function loadSaved(storage?: Storage | null): SavedBatch[] {
  try {
    const raw = storageOf(storage)?.getItem(STORE_KEY);
    const v: unknown = raw ? JSON.parse(raw) : [];
    if (!Array.isArray(v)) return [];
    return v.filter((b): b is SavedBatch => !!b && typeof b === 'object' && typeof (b as SavedBatch).batch_id === 'string' && Array.isArray((b as SavedBatch).files));
  } catch {
    return [];
  }
}

function writeSaved(list: SavedBatch[], storage?: Storage | null): void {
  try {
    const s = storageOf(storage);
    if (!s) return;
    if (list.length) s.setItem(STORE_KEY, JSON.stringify(list));
    else s.removeItem(STORE_KEY);
  } catch {
    /* nothing to remember */
  }
}

export function saveBatch(b: SavedBatch, storage?: Storage | null): void {
  writeSaved([...loadSaved(storage).filter((x) => x.batch_id !== b.batch_id), b], storage);
}

export function dropSaved(batchId: string, storage?: Storage | null): void {
  writeSaved(loadSaved(storage).filter((x) => x.batch_id !== batchId), storage);
}

//==============================================================================
export interface TusOptions {
  entries: TusEntry[];
  projectId?: string;
  name?: string;
  /** Continue a batch saved before a reload: `entries` are matched to it by relpath and fingerprint, no new batch is created. */
  resume?: SavedBatch;
  fetchImpl?: typeof fetch;
  chunkBytes?: number;
  concurrency?: number;
  /** Replaced in tests. */
  sleep?: (ms: number) => Promise<void>;
  storage?: Storage | null;
  onChange?: () => void;
}

const realSleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

export class TusUploader {
  files: TusFile[] = [];
  batchId = '';
  paused = false;
  /** Saved files that the user did not pick again (on resume): ended on the server. */
  missing: string[] = [];
  cancelled = false;
  private readonly fetchImpl: typeof fetch;
  private readonly chunk: number;
  private readonly concurrency: number;
  private readonly sleep: (ms: number) => Promise<void>;
  private readonly sources = new Map<string, File>();
  private readonly aborts = new Map<string, AbortController>();
  private readonly wake = new Set<() => void>();
  private active = 0;
  private settledResolve: () => void = () => undefined;
  private readonly settledPromise = new Promise<void>((r) => (this.settledResolve = r));
  private onlineHandler = () => {
    for (const w of [...this.wake]) w();
  };

  constructor(private readonly o: TusOptions) {
    this.fetchImpl = o.fetchImpl ?? ((...a) => fetch(...a));
    this.chunk = Math.max(1, o.chunkBytes ?? CHUNK_BYTES);
    this.concurrency = Math.max(1, o.concurrency ?? CONCURRENCY);
    this.sleep = o.sleep ?? realSleep;
  }

  //---------------------------------------------------------------- numbers
  get totalBytes(): number {
    return this.files.filter((f) => f.errorKind !== 'too_large').reduce((n, f) => n + f.size, 0);
  }
  get uploadedBytes(): number {
    return this.files.filter((f) => f.errorKind !== 'too_large').reduce((n, f) => n + (f.state === 'done' ? f.size : Math.min(f.offset, f.size)), 0);
  }
  get offline(): boolean {
    return this.files.some((f) => f.offline);
  }
  /** Every file is done (and there is at least one). */
  get allDone(): boolean {
    return this.files.length > 0 && this.files.every((f) => f.state === 'done');
  }
  /** Resolves when no file is queued, uploading or paused any more (all done or failed), or after cancel(). */
  whenSettled(): Promise<void> {
    return this.settledPromise;
  }

  private changed(): void {
    this.o.onChange?.();
  }

  private persist(): void {
    if (!this.batchId) return;
    if (this.allDone || this.cancelled) return dropSaved(this.batchId, this.o.storage);
    saveBatch(
      {
        batch_id: this.batchId,
        project_id: this.o.projectId ?? '',
        name: this.o.name ?? '',
        created: Date.now(),
        files: this.files.filter((f) => f.url).map((f) => ({ relpath: f.relpath, url: f.url, fingerprint: f.fingerprint, size: f.size, done: f.state === 'done' })),
      },
      this.o.storage,
    );
  }

  private checkSettled(): void {
    if (this.files.some((f) => f.state === 'queued' || f.state === 'uploading' || f.state === 'paused')) return;
    this.settledResolve();
  }

  //---------------------------------------------------------------- start
  private makeFile(e: TusEntry): TusFile {
    this.sources.set(e.relpath, e.file);
    return { relpath: e.relpath, name: e.file.name, size: e.file.size, fingerprint: fingerprintOf(e.file), state: 'queued', offset: 0, url: '', failures: 0, offline: false };
  }

  /**
   * Creates the batch (one request: the free disk space is checked for all files before anything is created; a short disk throws
   * DiskFullError) or picks up the saved one, then uploads. Resolves when the uploads are running; whenSettled() tells when they ended.
   */
  async start(): Promise<void> {
    if (typeof window !== 'undefined') window.addEventListener('online', this.onlineHandler);
    if (this.o.resume) this.startResumed(this.o.resume);
    else await this.startNew();
    this.changed();
    this.pump();
    this.checkSettled();
  }

  private startResumed(saved: SavedBatch): void {
    this.batchId = saved.batch_id;
    const byRel = new Map(this.o.entries.map((e) => [e.relpath, e]));
    for (const s of saved.files) {
      const e = byRel.get(s.relpath);
      if (s.done) {
        // already complete on the server: nothing to send, the user does not have to pick it again
        this.files.push({ relpath: s.relpath, name: s.relpath.split('/').pop() ?? s.relpath, size: s.size, fingerprint: s.fingerprint, state: 'done', offset: s.size, url: s.url, failures: 0, offline: false });
      } else if (e && fingerprintOf(e.file) === s.fingerprint) {
        const f = this.makeFile(e);
        f.url = s.url;
        this.files.push(f);
      } else {
        this.missing.push(s.relpath);
        void this.terminate(s.url);
      }
    }
    this.persist();
  }

  private async startNew(): Promise<void> {
    const sendable: TusFile[] = [];
    for (const e of this.o.entries) {
      const f = this.makeFile(e);
      if (f.size > MAX_FILE_BYTES) {
        f.state = 'error';
        f.errorKind = 'too_large';
      } else sendable.push(f);
      this.files.push(f);
    }
    if (!sendable.length) return;
    let res: Response;
    try {
      res = await this.fetchImpl('/api/upload-batches', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ files: sendable.map((f) => ({ relpath: f.relpath, size: f.size })) }),
        credentials: 'same-origin',
      });
    } catch (e) {
      throw new TusError(e instanceof Error ? e.message : 'network error', 0, 'NETWORK');
    }
    const body = (await res.json().catch(() => ({}))) as {
      batch_id?: string;
      files?: { relpath: string; upload_id: string; url: string }[];
      error?: { code?: string; message?: string; details?: { available_bytes?: number; needed_bytes?: number } };
    };
    if (!res.ok || !body.batch_id) {
      const d = body.error?.details;
      if (body.error?.code === 'PRECONDITION_FAILED' && typeof d?.needed_bytes === 'number') throw new DiskFullError(d.available_bytes ?? 0, d.needed_bytes);
      throw new TusError(body.error?.message ?? `HTTP ${res.status}`, res.status, body.error?.code ?? '');
    }
    this.batchId = body.batch_id;
    const urls = new Map((body.files ?? []).map((f) => [f.relpath, f.url]));
    for (const f of sendable) f.url = urls.get(f.relpath) ?? '';
    this.persist();
  }

  //---------------------------------------------------------------- scheduler
  private pump(): void {
    if (this.cancelled) return;
    for (const f of this.files) {
      if (this.active >= this.concurrency) break;
      if (f.state !== 'queued' || this.paused) continue;
      this.active++;
      f.state = 'uploading';
      void this.runFile(f).finally(() => {
        this.active--;
        this.changed();
        this.pump();
        this.checkSettled();
      });
    }
    this.changed();
  }

  private async wait(ms: number): Promise<void> {
    await Promise.race([this.sleep(ms), new Promise<void>((r) => this.wake.add(r))]);
    this.wake.clear();
  }

  private async runFile(f: TusFile): Promise<void> {
    const ctl = new AbortController();
    this.aborts.set(f.relpath, ctl);
    let netTries = 0;
    try {
      while (f.state === 'uploading') {
        try {
          await this.transfer(f, ctl.signal);
          f.state = 'done';
          f.offset = f.size;
          f.offline = false;
          this.persist();
          return;
        } catch (e) {
          if (ctl.signal.aborted || f.state !== 'uploading') return; // paused, removed or cancelled
          const status = e instanceof TusError ? e.status : 0;
          if (status === 0) {
            // the network (or the server restarting): keep trying, nothing to click
            f.offline = true;
            this.changed();
            await this.wait(Math.min(15000, 500 * 2 ** Math.min(netTries++, 5)));
          } else if (status === 409 || status === 423 || status === 460) {
            await this.wait(250); // busy / offset moved / chunk corrupted: ask for the offset again
          } else if (status === 404) {
            f.state = 'error';
            f.errorKind = 'gone';
            f.failures++;
          } else if (status === 413) {
            f.state = 'error';
            f.errorKind = 'too_large';
          } else {
            f.failures++;
            if (f.failures > AUTO_RETRIES) {
              f.state = 'error';
              f.errorKind = 'failed';
            } else await this.wait(1000);
          }
        }
      }
    } finally {
      if (this.aborts.get(f.relpath) === ctl) this.aborts.delete(f.relpath);
      this.persist();
    }
  }

  private async request(f: TusFile, signal: AbortSignal, init: RequestInit): Promise<Response> {
    let res: Response;
    try {
      res = await this.fetchImpl(f.url, { credentials: 'same-origin', signal, ...init });
    } catch (e) {
      if (signal.aborted) throw e;
      throw new TusError(e instanceof Error ? e.message : 'network error', 0, 'NETWORK');
    }
    f.offline = false;
    return res;
  }

  private async transfer(f: TusFile, signal: AbortSignal): Promise<void> {
    const file = this.sources.get(f.relpath);
    if (!file) throw new TusError('no file', 500);
    // the server knows how much it has: after a pause, a reload or a dropped connection the offset comes from it
    const head = await this.request(f, signal, { method: 'HEAD', headers: { ...TUS } });
    if (!head.ok) throw new TusError(`HEAD ${head.status}`, head.status);
    let offset = Number(head.headers.get('Upload-Offset') ?? '0') || 0;
    f.offset = offset;
    this.changed();
    while (offset < f.size) {
      const end = Math.min(f.size, offset + this.chunk);
      const res = await this.request(f, signal, {
        method: 'PATCH',
        headers: { ...TUS, 'Content-Type': 'application/offset+octet-stream', 'Upload-Offset': String(offset) },
        body: file.slice(offset, end),
      });
      if (res.status === 409) {
        const server = Number(res.headers.get('Upload-Offset') ?? NaN);
        if (Number.isFinite(server) && server !== offset) {
          offset = server;
          f.offset = offset;
          this.changed();
          continue;
        }
      }
      if (res.status !== 204 && res.status !== 200) throw new TusError(`PATCH ${res.status}`, res.status);
      const next = Number(res.headers.get('Upload-Offset') ?? end);
      offset = Number.isFinite(next) && next > offset ? next : end;
      f.offset = offset;
      f.failures = 0;
      this.changed();
    }
  }

  //---------------------------------------------------------------- controls
  pauseAll(): void {
    this.paused = true;
    for (const f of this.files) if (f.state === 'uploading' || f.state === 'queued') this.pauseFile(f);
    this.changed();
  }

  resumeAll(): void {
    this.paused = false;
    for (const f of this.files) if (f.state === 'paused') f.state = 'queued';
    this.pump();
  }

  private find(relpath: string): TusFile | undefined {
    return this.files.find((f) => f.relpath === relpath);
  }

  private pauseFile(f: TusFile): void {
    f.state = 'paused';
    this.aborts.get(f.relpath)?.abort();
  }

  pause(relpath: string): void {
    const f = this.find(relpath);
    if (f && (f.state === 'uploading' || f.state === 'queued')) {
      this.pauseFile(f);
      this.changed();
    }
  }

  resume(relpath: string): void {
    const f = this.find(relpath);
    if (f?.state !== 'paused') return;
    f.state = 'queued';
    this.paused = false;
    this.pump();
  }

  /** After an error: a fresh upload when the old one expired on the server, otherwise the same one from its offset. */
  async retry(relpath: string): Promise<void> {
    const f = this.find(relpath);
    if (!f || f.state !== 'error' || f.errorKind === 'too_large') return;
    if (f.errorKind === 'gone') {
      const created = await this.recreate(f);
      if (!created) {
        f.failures++;
        this.changed();
        return;
      }
    }
    delete f.errorKind;
    f.state = 'queued';
    this.pump();
  }

  private async recreate(f: TusFile): Promise<boolean> {
    const b64 = (s: string) => btoa(String.fromCharCode(...new TextEncoder().encode(s)));
    try {
      const res = await this.fetchImpl('/api/uploads', {
        method: 'POST',
        headers: {
          ...TUS,
          'Upload-Length': String(f.size),
          'Upload-Metadata': `filename ${b64(f.name)},relpath ${b64(f.relpath)},batch_id ${b64(this.batchId)}`,
        },
        credentials: 'same-origin',
      });
      const loc = res.headers.get('Location');
      if (!res.ok || !loc) return false;
      f.url = loc;
      f.offset = 0;
      this.persist();
      return true;
    } catch {
      return false;
    }
  }

  /** Drops a file from the batch (also on the server, so the batch can complete without it). */
  remove(relpath: string): void {
    const f = this.find(relpath);
    if (!f) return;
    this.aborts.get(relpath)?.abort();
    this.files = this.files.filter((x) => x !== f);
    this.sources.delete(relpath);
    if (f.url) void this.terminate(f.url);
    this.persist();
    this.changed();
    this.checkSettled();
    this.pump();
  }

  private async terminate(url: string): Promise<void> {
    try {
      await this.fetchImpl(url, { method: 'DELETE', headers: { ...TUS }, credentials: 'same-origin' });
    } catch {
      /* the server removes unfinished uploads after 7 days anyway */
    }
  }

  /** Stops everything and ends the uploads on the server. */
  async cancel(): Promise<void> {
    this.cancelled = true;
    for (const c of this.aborts.values()) c.abort();
    const urls = this.files.filter((f) => f.url && f.state !== 'done').map((f) => f.url);
    this.dispose();
    if (this.batchId) dropSaved(this.batchId, this.o.storage);
    await Promise.all(urls.map((u) => this.terminate(u)));
    this.settledResolve();
    this.changed();
  }

  /** Detaches the 'online' listener (after the batch ended). */
  dispose(): void {
    if (typeof window !== 'undefined') window.removeEventListener('online', this.onlineHandler);
  }
}
