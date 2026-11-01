// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// dB, pan and fader helpers. Number formats are fixed (never locale dependent): "-6.0 dB", "0:34.875", "118.00".

/** What the server calls silence. */
export const SILENCE_DB = -100;

/** Fader law: position 0..1 -> dB through fixed breakpoints (more resolution around 0 dB), 0 dB at 75 %. */
const BREAKS: ReadonlyArray<readonly [number, number]> = [
  [-60, 0],
  [-40, 0.12],
  [-20, 0.35],
  [-10, 0.55],
  [0, 0.75],
  [6, 0.9],
  [12, 1],
];

export function dbToFader(db: number): number {
  if (!Number.isFinite(db) || db <= BREAKS[0]![0]) return 0;
  if (db >= 12) return 1;
  for (let i = 1; i < BREAKS.length; i++) {
    const [d1, p1] = BREAKS[i]!;
    const [d0, p0] = BREAKS[i - 1]!;
    if (db <= d1) return p0 + ((db - d0) / (d1 - d0)) * (p1 - p0);
  }
  return 1;
}

export function faderToDb(pos: number): number {
  if (pos <= 0) return SILENCE_DB;
  if (pos >= 1) return 12;
  for (let i = 1; i < BREAKS.length; i++) {
    const [d1, p1] = BREAKS[i]!;
    const [d0, p0] = BREAKS[i - 1]!;
    if (pos <= p1) return d0 + ((pos - p0) / (p1 - p0)) * (d1 - d0);
  }
  return 12;
}

export function dbToLinear(db: number): number {
  return db <= SILENCE_DB ? 0 : Math.pow(10, db / 20);
}

/** Meter bar position 0..1 for a peak in dBFS (-60 .. +6 dB). */
export function meterPosition(db: number): number {
  return Math.min(1, Math.max(0, (db + 60) / 66));
}

/** green up to -12 dBFS, amber to -3, red above (Rovox design system). */
export function meterZone(db: number): 'green' | 'amber' | 'red' {
  return db > -3 ? 'red' : db > -12 ? 'amber' : 'green';
}

export function formatDb(db: number | undefined, unit = false): string {
  if (db === undefined || db <= SILENCE_DB + 0.05) return unit ? '-inf dB' : '-inf';
  const s = (Math.round(db * 10) / 10).toFixed(1);
  return unit ? `${s} dB` : s;
}

/** pan -1..1 -> "C", "18% L", "40% R" */
export function formatPan(pan: number | undefined): string {
  const p = Math.round((pan ?? 0) * 100);
  if (p === 0) return 'C';
  return p < 0 ? `${-p}% L` : `${p}% R`;
}

export function clamp(v: number, lo: number, hi: number): number {
  return Math.min(hi, Math.max(lo, v));
}
