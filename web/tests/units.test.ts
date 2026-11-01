// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { dbToFader, faderToDb, formatDb, formatPan, meterPosition, meterZone, SILENCE_DB } from '../src/lib/units';

describe('fader law', () => {
  it('maps 0 dB to 75 percent and +12 dB to the top', () => {
    expect(dbToFader(0)).toBeCloseTo(0.75, 6);
    expect(dbToFader(12)).toBe(1);
    expect(dbToFader(-100)).toBe(0);
    expect(faderToDb(0)).toBe(SILENCE_DB);
    expect(faderToDb(1)).toBe(12);
  });

  it('round-trips through the whole range', () => {
    for (let db = -58; db <= 12; db += 0.5) expect(faderToDb(dbToFader(db))).toBeCloseTo(db, 6);
  });

  it('is monotonic', () => {
    let last = -Infinity;
    for (let p = 0; p <= 1; p += 0.01) {
      const db = faderToDb(p);
      expect(db).toBeGreaterThanOrEqual(last);
      last = db;
    }
  });
});

describe('number formats never depend on the locale', () => {
  it('formats dB with a dot and one decimal', () => {
    expect(formatDb(-6)).toBe('-6.0');
    expect(formatDb(-6, true)).toBe('-6.0 dB');
    expect(formatDb(3.14159)).toBe('3.1');
    expect(formatDb(SILENCE_DB)).toBe('-inf');
    expect(formatDb(undefined)).toBe('-inf');
  });

  it('formats pan as C / L / R percent', () => {
    expect(formatPan(0)).toBe('C');
    expect(formatPan(-0.18)).toBe('18% L');
    expect(formatPan(0.4)).toBe('40% R');
  });
});

describe('meters', () => {
  it('colours by zone: green to -12, amber to -3, red above', () => {
    expect(meterZone(-30)).toBe('green');
    expect(meterZone(-12)).toBe('green');
    expect(meterZone(-6)).toBe('amber');
    expect(meterZone(-3)).toBe('amber');
    expect(meterZone(-2.9)).toBe('red');
  });

  it('positions peaks between -60 and +6 dB', () => {
    expect(meterPosition(-100)).toBe(0);
    expect(meterPosition(6)).toBe(1);
    expect(meterPosition(-27)).toBeCloseTo(0.5, 6);
  });
});
