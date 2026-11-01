// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { JitterRing } from '../src/audio/jitter';

const chunk = (n: number, v = 0.5) => [new Float32Array(n).fill(v), new Float32Array(n).fill(-v)] as const;

describe('jitter ring', () => {
  it('stays silent until the target is buffered, then plays', () => {
    const r = new JitterRing(48000, 7200);
    const outL = new Float32Array(128);
    const outR = new Float32Array(128);
    for (let i = 0; i < 6; i++) r.push(...chunk(960), i * 960); // 5760 < 7200
    expect(r.pull(outL, outR)).toBe(false);
    expect(r.playing).toBe(false);
    expect(outL[0]).toBe(0);
    r.push(...chunk(960), 6 * 960); // 6720 still below
    r.push(...chunk(960), 7 * 960); // 7680 >= 7200
    expect(r.playing).toBe(true);
    r.pull(outL, outR);
    expect(outL[0]).toBe(0.5);
    expect(outR[0]).toBe(-0.5);
  });

  it('counts an underrun, outputs silence for the missing part and re-buffers', () => {
    const r = new JitterRing(48000, 480);
    r.push(...chunk(480), 0);
    const outL = new Float32Array(480);
    const outR = new Float32Array(480);
    expect(r.pull(outL, outR)).toBe(false); // exactly enough
    const small = new Float32Array(128);
    expect(r.pull(small, new Float32Array(128))).toBe(true);
    expect(r.underruns).toBe(1);
    expect(r.playing).toBe(false);
    r.push(...chunk(480), 480);
    expect(r.playing).toBe(true);
  });

  it('plays 10 s of 20 ms packets without an underrun when the jitter stays below 150 ms', () => {
    const r = new JitterRing(96000, 7200);
    const outL = new Float32Array(128);
    const outR = new Float32Array(128);
    let produced = 0;
    let consumed = 0;
    // 128-frame render quanta consume 48000 frames per second; a packet of 960 arrives every 20 ms but in bursts of up to 100 ms
    for (let ms = 0; ms < 10000; ms++) {
      const burst = ms % 100 === 99; // everything of the last 100 ms arrives at once
      if (burst) for (let k = 0; k < 5; k++, produced += 960) r.push(...chunk(960), produced);
      const quanta = 48 / 0.128 / 1000; // 0.375 per ms
      consumed += quanta;
      while (consumed >= 1) {
        consumed -= 1;
        r.pull(outL, outR);
      }
    }
    expect(r.underruns).toBe(0);
  });

  it('reports the timeline position of the next frame, across chunks and seeks', () => {
    const r = new JitterRing(48000, 240);
    r.push(...chunk(240), 1000);
    r.push(...chunk(240), 1240);
    expect(r.position()).toBe(1000);
    r.pull(new Float32Array(100), new Float32Array(100));
    expect(r.position()).toBe(1100);
    r.pull(new Float32Array(200), new Float32Array(200));
    expect(r.position()).toBe(1300);
    r.flush();
    expect(r.position()).toBeNull();
    r.push(...chunk(240), 96000);
    expect(r.position()).toBe(96000);
  });

  it('drops the oldest audio instead of growing without bound', () => {
    const r = new JitterRing(1000, 100);
    for (let i = 0; i < 5; i++) r.push(...chunk(400), i * 400);
    expect(r.buffered).toBeLessThanOrEqual(1000);
    expect(r.overflows).toBeGreaterThan(0);
  });
});
