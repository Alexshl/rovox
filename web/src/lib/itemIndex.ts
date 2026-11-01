// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Per-track index of items for virtualised drawing and hit testing: sorted by start with a prefix maximum of the end time,
// so a time window is found with a binary search and overlaps are handled.

export interface Span {
  start_s: number;
  end_s: number;
}

export class TrackItemIndex<T extends Span> {
  private items: T[];
  private maxEnd: number[];

  constructor(items: T[]) {
    this.items = [...items].sort((a, b) => a.start_s - b.start_s);
    this.maxEnd = [];
    let m = -Infinity;
    for (const it of this.items) {
      m = Math.max(m, it.end_s);
      this.maxEnd.push(m);
    }
  }

  get size(): number {
    return this.items.length;
  }

  /** Items that intersect [t0, t1), in start order. */
  query(t0: number, t1: number): T[] {
    // first index whose prefix max end is > t0: nothing before it can be visible
    let lo = 0;
    let hi = this.items.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.maxEnd[mid]! > t0) hi = mid;
      else lo = mid + 1;
    }
    const out: T[] = [];
    for (let i = lo; i < this.items.length; i++) {
      const it = this.items[i]!;
      if (it.start_s >= t1) break;
      if (it.end_s > t0) out.push(it);
    }
    return out;
  }

  /** The topmost (latest starting) item that contains t. */
  hit(t: number): T | undefined {
    const found = this.query(t, t + 1e-9);
    return found.length ? found[found.length - 1] : undefined;
  }
}
