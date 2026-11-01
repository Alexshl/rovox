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

export class PeakCache {
  private entries = new Map<string, PeakEntry>();
  private pending = new Set<string>();
  private queue: (() => void)[] = [];
  private active = 0;

  constructor(
    private readonly onReady: () => void,
    private readonly fetchImpl: typeof fetch = (...a) => fetch(...a),
  ) {}

  /** The best entry available now (maybe stale) and, if it is not good enough, a fetch is started. */
  get(projectId: string, it: ItemState, visT0: number, visT1: number, pixelsPerSecond: number): PeakEntry | undefined {
    const e = this.entries.get(it.item_id);
    const usable = e && e.geom === geomKey(it) ? e : undefined;
    if (usable && entrySuffices(usable, it, visT0, visT1, pixelsPerSecond)) return usable;
    this.request(projectId, it, visT0, visT1, pixelsPerSecond);
    return usable;
  }

  private request(projectId: string, it: ItemState, visT0: number, visT1: number, pps: number): void {
    const w = peakWindow(it, visT0, visT1, pps);
    const key = `${it.item_id}|${geomKey(it)}|${w.t0.toFixed(2)}|${w.bins}`;
    if (this.pending.has(key) || w.t1 <= w.t0) return;
    this.pending.add(key);
    const geom = geomKey(it);
    const run = async () => {
      this.active++;
      try {
        const url = `/api/projects/${encodeURIComponent(projectId)}/peaks/${encodeURIComponent(it.item_id)}?bins=${w.bins}&t0=${w.t0}&t1=${w.t1}`;
        const res = await this.fetchImpl(url, { credentials: 'same-origin' });
        if (res.ok) {
          const t0 = Number(res.headers.get('X-Rovox-T0') ?? w.t0);
          const t1 = Number(res.headers.get('X-Rovox-T1') ?? w.t1);
          this.entries.set(it.item_id, { geom, t0, t1, bins: w.bins, data: new Int8Array(await res.arrayBuffer()) });
          this.onReady();
        }
      } catch {
        /* the next draw asks again */
      } finally {
        this.active--;
        this.pending.delete(key);
        this.next();
      }
    };
    this.queue.push(run);
    this.next();
  }

  private next(): void {
    while (this.active < 6 && this.queue.length) this.queue.shift()!();
  }

  forget(itemId: string): void {
    this.entries.delete(itemId);
  }

  clear(): void {
    this.entries.clear();
  }
}
