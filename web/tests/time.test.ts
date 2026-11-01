// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { barBeatTick, barToSeconds, formatBBT, formatClock, formatWallClock, qnToSeconds, secondsToBar, secondsToQn, type TempoMap } from '../src/lib/time';

const t120: TempoMap = { points: [{ bpm: 120, time_s: 0, qn: 0 }], numerator: 4, denominator: 4 };
const changing: TempoMap = {
  points: [
    { bpm: 120, time_s: 0, qn: 0 },
    { bpm: 60, time_s: 8, qn: 16 },
  ],
  numerator: 4,
  denominator: 4,
};

describe('tempo map', () => {
  it('converts seconds and quarter notes at a constant tempo', () => {
    expect(secondsToQn(t120, 2)).toBe(4);
    expect(qnToSeconds(t120, 4)).toBe(2);
  });

  it('follows a tempo change', () => {
    expect(secondsToQn(changing, 8)).toBe(16);
    expect(secondsToQn(changing, 10)).toBe(18);
    expect(qnToSeconds(changing, 20)).toBe(12);
    for (const t of [0, 3.3, 8, 11.7]) expect(qnToSeconds(changing, secondsToQn(changing, t))).toBeCloseTo(t, 9);
  });

  it('counts bars from 1', () => {
    expect(barToSeconds(t120, 1)).toBe(0);
    expect(barToSeconds(t120, 3)).toBe(4);
    expect(secondsToBar(t120, 4)).toBe(3);
  });

  it('supports other time signatures', () => {
    const sixEight: TempoMap = { points: [{ bpm: 120, time_s: 0, qn: 0 }], numerator: 6, denominator: 8 };
    // a 6/8 bar is 3 quarter notes = 1.5 s at 120 bpm
    expect(barToSeconds(sixEight, 2)).toBeCloseTo(1.5, 9);
  });
});

describe('readouts', () => {
  it('formats bar.beat.hundredths like the transport of the design', () => {
    expect(formatBBT(t120, 0)).toBe('1.1.00');
    // 17.3 = 16 bars + 2 beats = 66 quarter notes = 33 s
    expect(formatBBT(t120, 33)).toBe('17.3.00');
    expect(formatBBT(t120, 33.25)).toBe('17.3.50');
  });

  it('splits ticks', () => {
    expect(barBeatTick(t120, 0.75)).toEqual({ bar: 1, beat: 2, tick: 480 });
  });

  it('formats the clock as m:ss.mmm with a dot', () => {
    expect(formatClock(34.875)).toBe('0:34.875');
    expect(formatClock(192)).toBe('3:12.000');
    expect(formatClock(-1)).toBe('0:00.000');
  });

  it('formats the wall clock with 24 hours', () => {
    expect(formatWallClock(new Date(2026, 10, 1, 0, 5, 9))).toBe('00:05:09');
    expect(formatWallClock(new Date(2026, 10, 1, 12, 41, 8))).toBe('12:41:08');
    expect(formatWallClock(new Date(2026, 10, 1, 23, 59, 59))).toBe('23:59:59');
  });
});
