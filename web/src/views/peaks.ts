// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Waveform peaks of items, fetched from GET /api/projects/{id}/peaks/{item_id}: int8 (min, max) pairs for a window of the item.
// An entry stays valid while the item's source window is unchanged and it still covers what is on screen at enough density.
import type { ItemState } from '../net/protocol';

export interface PeakEntry {
  geom: string;
  t0: number; // item-relative seconds covered
  t1: number;
  bins: number;
  data: Int8Array;
}

export function geomKey(it: ItemState): string {
  return `${it.file ?? ''}|${it.offset_s.toFixed(4)}|${it.length_s.toFixed(4)}`;
}

/** Window to request: what is visible, widened by 25 % on each side, with one bin per device pixel. */
export function peakWindow(it: ItemState, visT0: number, visT1: number, pixelsPerSecond: number): { t0: number; t1: number; bins: number } {
  const span = Math.max(1e-3, visT1 - visT0);
  const t0 = Math.max(0, visT0 - span * 0.25);
  const t1 = Math.min(it.length_s, visT1 + span * 0.25);
  const bins = Math.min(4096, Math.max(8, Math.ceil((t1 - t0) * pixelsPerSecond)));
  return { t0, t1, bins };
}

export function entrySuffices(e: PeakEntry, it: ItemState, visT0: number, visT1: number, pixelsPerSecond: number): boolean {
  if (e.geom !== geomKey(it)) return false;
  if (e.t0 > visT0 + 1e-6 || e.t1 < visT1 - 1e-6) return false;
  const density = e.bins / Math.max(1e-6, e.t1 - e.t0); // bins per second
  return density >= pixelsPerSecond * 0.7;
}

/** Entries kept (least recently used go first) and the pauses after consecutive failures of one item. */
export const PEAK_CACHE_LIMIT = 500;
export const PEAK_BACKOFF_MS = [5_000, 30_000, 300_000];
/** Short capped retry delays after a server (5xx, 504) or network error: the server is likely busy or restarting, not refusing. */
export const PEAK_RETRY_MS = [1_000, 2_000, 4_000];
const MAX_PARALLEL = 6;

interface Job {
  sig: string;
  run: () => Promise<void>;
}

export class PeakCache {
  private entries = new Map<string, PeakEntry>(); // insertion order = recency
  private inflight = new Map<string, string>(); // key -> window signature being fetched
  private queued = new Map<string, Job>(); // at most one waiting request per item: a newer window replaces it
  private failures = new Map<string, { count: number; until: number; geom: string }>();
  private active = 0;

  constructor(
    private readonly onReady: () => void,
    private readonly fetchImpl: typeof fetch = (...a) => fetch(...a),
    private readonly now: () => number = () => Date.now(),
  ) {}

  /** The best entry available now (maybe stale) and, if it is not good enough, a fetch is started. */
  get(projectId: string, it: ItemState, visT0: number, visT1: number, pixelsPerSecond: number): PeakEntry | undefined {
    const key = cacheKey(projectId, it.item_id);
    const e = this.entries.get(key);
    const usable = e && e.geom === geomKey(it) ? e : undefined;
    if (usable) {
      this.entries.delete(key); // touch
      this.entries.set(key, usable);
      if (entrySuffices(usable, it, visT0, visT1, pixelsPerSecond)) return usable;
    }
    this.request(projectId, key, it, visT0, visT1, pixelsPerSecond);
    return usable;
  }

  private request(projectId: string, key: string, it: ItemState, visT0: number, visT1: number, pps: number): void {
    const geom = geomKey(it);
    const fail = this.failures.get(key);
    if (fail && fail.geom !== geom) this.failures.delete(key); // the item changed: an earlier failure says nothing about it
    else if (fail && this.now() < fail.until) return;
    const w = peakWindow(it, visT0, visT1, pps);
    if (w.t1 <= w.t0) return;
    const sig = `${geom}|${w.t0.toFixed(2)}|${w.bins}`;
    if (this.inflight.get(key) === sig || this.queued.get(key)?.sig === sig) return;
    const run = async () => {
      let ok = false;
      let clientError = false; // 4xx: the server refuses this request, retrying soon will not help
      try {
        const url = `/api/projects/${encodeURIComponent(projectId)}/peaks/${encodeURIComponent(it.item_id)}?bins=${w.bins}&t0=${w.t0}&t1=${w.t1}`;
        const res = await this.fetchImpl(url, { credentials: 'same-origin' });
        if (res.ok) {
          const t0 = Number(res.headers.get('X-Rovox-T0') ?? w.t0);
          const t1 = Number(res.headers.get('X-Rovox-T1') ?? w.t1);
          this.store(key, { geom, t0, t1, bins: w.bins, data: new Int8Array(await res.arrayBuffer()) });
          ok = true;
        } else clientError = res.status >= 400 && res.status < 500;
      } catch {
        /* counted as a failure below */
      }
      if (ok) {
        this.failures.delete(key);
        this.onReady();
      } else this.fail(key, geom, clientError);
    };
    this.queued.set(key, { sig, run });
    this.next();
  }

  private store(key: string, e: PeakEntry): void {
    this.entries.delete(key);
    this.entries.set(key, e);
    while (this.entries.size > PEAK_CACHE_LIMIT) this.entries.delete(this.entries.keys().next().value!);
  }

  private fail(key: string, geom: string, clientError: boolean): void {
    const prev = this.failures.get(key);
    const count = (prev && prev.geom === geom ? prev.count : 0) + 1;
    const delays = clientError ? PEAK_BACKOFF_MS : PEAK_RETRY_MS;
    this.failures.delete(key);
    this.failures.set(key, { count, until: this.now() + delays[Math.min(count, delays.length) - 1]!, geom });
    while (this.failures.size > PEAK_CACHE_LIMIT) this.failures.delete(this.failures.keys().next().value!);
  }

  /** Forget all failures (a reconnect or a fresh `welcome`): the server may have been down, so everything is worth retrying. */
  resetFailures(): void {
    this.failures.clear();
  }

  private next(): void {
    for (const [key, job] of this.queued) {
      if (this.active >= MAX_PARALLEL) return;
      if (this.inflight.has(key)) continue; // one request per item at a time
      this.queued.delete(key);
      this.inflight.set(key, job.sig);
      this.active++;
      void job.run().finally(() => {
        this.active--;
        this.inflight.delete(key);
        this.next();
      });
    }
  }

  forget(projectId: string, itemId: string): void {
    const key = cacheKey(projectId, itemId);
    this.entries.delete(key);
    this.failures.delete(key);
    this.queued.delete(key);
  }

  clear(): void {
    this.entries.clear();
    this.failures.clear();
    this.queued.clear();
  }

  /** Test hook. */
  stats(): { entries: number; queued: number; inflight: number } {
    return { entries: this.entries.size, queued: this.queued.size, inflight: this.inflight.size };
  }
}

function cacheKey(projectId: string, itemId: string): string {
  return `${projectId}\u0000${itemId}`;
}
