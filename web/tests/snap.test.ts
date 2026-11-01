// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { gridQn, snapTime } from '../src/lib/snap';
import type { TempoMap } from '../src/lib/time';
import { TrackItemIndex } from '../src/lib/itemIndex';

const tempo: TempoMap = { points: [{ bpm: 120, time_s: 0, qn: 0 }], numerator: 4, denominator: 4 };

describe('snap', () => {
  it('grid sizes in quarter notes', () => {
    expect(gridQn('1/4')).toBe(1);
    expect(gridQn('1/16')).toBe(0.25);
    expect(gridQn('1/1')).toBe(4);
  });

  it('snaps to the nearest grid line', () => {
    // 1/16 at 120 bpm = 0.125 s
    expect(snapTime(0.33, { tempo, grid: '1/16', enabled: true, markers: [], threshold: 0.01 })).toBeCloseTo(0.375, 9);
    expect(snapTime(0.33, { tempo, grid: '1/16', enabled: false, markers: [], threshold: 0.01 })).toBe(0.33);
  });

  it('prefers a marker that is closer than the grid line, within the threshold', () => {
    const o = { tempo, grid: '1/4' as const, enabled: true, markers: [1.2], threshold: 0.05 };
    expect(snapTime(1.21, o)).toBe(1.2);
    expect(snapTime(1.3, o)).toBe(1.5);
  });
});

describe('item index', () => {
  const item = (id: string, s: number, e: number) => ({ id, start_s: s, end_s: e });
  it('finds the items of a window, including long overlapping ones', () => {
    const idx = new TrackItemIndex([item('long', 0, 100), item('a', 10, 12), item('b', 20, 22), item('c', 50, 51)]);
    expect(idx.query(11, 21).map((i) => i.id)).toEqual(['long', 'a', 'b']);
    expect(idx.query(60, 70).map((i) => i.id)).toEqual(['long']);
    expect(idx.query(200, 300)).toEqual([]);
  });

  it('hit test returns the topmost item', () => {
    const idx = new TrackItemIndex([item('under', 0, 10), item('over', 4, 6)]);
    expect(idx.hit(5)?.id).toBe('over');
    expect(idx.hit(8)?.id).toBe('under');
    expect(idx.hit(11)).toBeUndefined();
  });

  it('answers a window query on 5000 items fast', () => {
    const items = Array.from({ length: 5000 }, (_, i) => item(`i${i}`, i * 2, i * 2 + 1.5));
    const idx = new TrackItemIndex(items);
    const t0 = performance.now();
    let n = 0;
    for (let k = 0; k < 2000; k++) n += idx.query(k * 3, k * 3 + 60).length;
    expect(performance.now() - t0).toBeLessThan(250);
    expect(n).toBeGreaterThan(0);
  });
});
