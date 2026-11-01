// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Tempo map, bar.beat.tick and clock formatting. Seconds <-> quarter notes through the tempo points the server reports.

export interface TempoPoint {
  bpm: number;
  time_s: number;
  qn: number;
}

export interface TempoMap {
  points: TempoPoint[];
  numerator: number;
  denominator: number;
}

export const DEFAULT_TEMPO: TempoMap = { points: [{ bpm: 120, time_s: 0, qn: 0 }], numerator: 4, denominator: 4 };

function pointAtTime(map: TempoMap, t: number): TempoPoint {
  let p = map.points[0] ?? DEFAULT_TEMPO.points[0]!;
  for (const q of map.points) {
    if (q.time_s <= t) p = q;
    else break;
  }
  return p;
}

function pointAtQn(map: TempoMap, qn: number): TempoPoint {
  let p = map.points[0] ?? DEFAULT_TEMPO.points[0]!;
  for (const q of map.points) {
    if (q.qn <= qn) p = q;
    else break;
  }
  return p;
}

export function secondsToQn(map: TempoMap, t: number): number {
  const p = pointAtTime(map, t);
  return p.qn + ((t - p.time_s) * p.bpm) / 60;
}

export function qnToSeconds(map: TempoMap, qn: number): number {
  const p = pointAtQn(map, qn);
  return p.time_s + ((qn - p.qn) * 60) / p.bpm;
}

/** Quarter notes in one beat of the time signature denominator (4/4 -> 1, 6/8 -> 0.5). */
export function beatQn(map: TempoMap): number {
  return 4 / map.denominator;
}

export function barQn(map: TempoMap): number {
  return map.numerator * beatQn(map);
}

export interface BarBeat {
  bar: number;
  beat: number;
  tick: number;
}

/** 1-based bar and beat, tick 0..959 (960 per quarter note, as the server's TimePos). */
export function barBeatTick(map: TempoMap, t: number): BarBeat {
  const qn = Math.max(0, secondsToQn(map, t));
  const bq = barQn(map);
  const bar = Math.floor(qn / bq + 1e-9);
  const inBar = qn - bar * bq;
  const bt = beatQn(map);
  const beat = Math.floor(inBar / bt + 1e-9);
  const tick = Math.round(((inBar - beat * bt) / bt) * 960 * bt);
  return { bar: bar + 1, beat: beat + 1, tick: Math.min(959, tick) };
}

/** "17.3.00": bar.beat.hundredths of a beat, like the transport readout of the design. */
export function formatBBT(map: TempoMap, t: number): string {
  const { bar, beat, tick } = barBeatTick(map, t);
  const hundredths = Math.floor((tick / (960 * beatQn(map))) * 100);
  return `${bar}.${beat}.${String(Math.min(99, hundredths)).padStart(2, '0')}`;
}

/** "0:34.875" */
export function formatClock(t: number): string {
  const ms = Math.max(0, Math.round(t * 1000));
  const m = Math.floor(ms / 60000);
  const s = Math.floor((ms % 60000) / 1000);
  return `${m}:${String(s).padStart(2, '0')}.${String(ms % 1000).padStart(3, '0')}`;
}

/** "118.00" */
export function formatBpm(bpm: number): string {
  return bpm.toFixed(2);
}

export function barToSeconds(map: TempoMap, bar: number): number {
  return qnToSeconds(map, (bar - 1) * barQn(map));
}

export function secondsToBar(map: TempoMap, t: number): number {
  return secondsToQn(map, t) / barQn(map) + 1;
}

/** Wall clock "12:41:08", always 24 hours. */
export function formatWallClock(iso: string | Date): string {
  const d = typeof iso === 'string' ? new Date(iso) : iso;
  const p = (n: number) => String(n).padStart(2, '0');
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
