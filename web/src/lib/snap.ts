// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Grid and snapping. The grid is a note value ("1/16"); snapping targets grid lines and markers.
import { type TempoMap, qnToSeconds, secondsToQn } from './time';

export const GRIDS = ['1/1', '1/2', '1/4', '1/8', '1/16', '1/32'] as const;
export type Grid = (typeof GRIDS)[number];

/** Quarter notes per grid step. */
export function gridQn(grid: Grid): number {
  return 4 / Number(grid.split('/')[1]);
}

export interface SnapOptions {
  tempo: TempoMap;
  grid: Grid;
  enabled: boolean;
  markers: number[];
  /** Snap only within this distance (seconds): a few pixels at the current zoom. */
  threshold: number;
}

export function snapTime(t: number, o: SnapOptions): number {
  if (!o.enabled) return t;
  const step = gridQn(o.grid);
  const qn = secondsToQn(o.tempo, t);
  const onGrid = qnToSeconds(o.tempo, Math.round(qn / step) * step);
  let best = onGrid;
  let bestD = Math.abs(onGrid - t);
  for (const m of o.markers) {
    const d = Math.abs(m - t);
    if (d < bestD && d <= o.threshold) {
      best = m;
      bestD = d;
    }
  }
  return Math.max(0, best);
}
