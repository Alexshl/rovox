// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { DriftController, JitterRing, MAX_RATE_DEVIATION, STREAM_RATE } from '../src/audio/jitter';

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

/** Server clock (producer) vs sound card clock (consumer): the producer makes `1 + ppm * 1e-6` seconds of audio per real second. */
function simulate(ppm: number, minutes: number, adaptive: boolean) {
  const target = 7200;
  const r = new JitterRing(STREAM_RATE * 2, target);
  r.adaptive = adaptive;
  const outL = new Float32Array(128);
  const outR = new Float32Array(128);
  const packet = 960;
  const speed = 1 + ppm * 1e-6;
  const l = new Float32Array(packet);
  const rr = new Float32Array(packet);
  let produced = 0; // frames of audio the server has made
  let sent = 0;
  let minBuf = Infinity;
  let maxBuf = -Infinity;
  const quantaPerMinute = (60 * STREAM_RATE) / 128;
  const quanta = Math.round(minutes * quantaPerMinute);
  for (let q = 0; q < quanta; q++) {
    produced += 128 * speed;
    while (sent + packet <= produced) {
      r.push(l, rr, sent);
      sent += packet;
    }
    r.pull(outL, outR);
    if (q > quantaPerMinute && r.playing) {
      // after the first minute
      minBuf = Math.min(minBuf, r.buffered);
      maxBuf = Math.max(maxBuf, r.buffered);
    }
  }
  return { r, minBuf, maxBuf, target };
}

describe('clock drift compensation', () => {
  it('the controller asks for a faster rate above the target, a slower one below, never beyond 0.5 %', () => {
    const c = new DriftController(7200);
    expect(c.update(7200, 128)).toBeCloseTo(1, 6);
    const hi = new DriftController(7200);
    expect(hi.update(7200 + 48000, 128)).toBeCloseTo(1 + MAX_RATE_DEVIATION, 9);
    const lo = new DriftController(7200);
    expect(lo.update(0, 128)).toBeLessThan(1);
    expect(lo.update(-48000, 128)).toBeGreaterThanOrEqual(1 - MAX_RATE_DEVIATION - 1e-12);
  });

  for (const ppm of [200, -200]) {
    it(`holds the fill within the target +-50 ms with no underrun over 30 minutes at ${ppm > 0 ? '+' : ''}${ppm} ppm`, () => {
      const { r, minBuf, maxBuf, target } = simulate(ppm, 30, true);
      expect(r.underruns).toBe(0);
      expect(r.overflows).toBe(0);
      expect(minBuf).toBeGreaterThanOrEqual(target - 2400);
      expect(maxBuf).toBeLessThanOrEqual(target + 2400);
    });
  }

  it('without compensation the same drift runs the buffer dry (the problem this fixes)', () => {
    const { r } = simulate(-200, 30, false);
    expect(r.underruns).toBeGreaterThan(0);
  });

  it('resampling a constant signal gives the same constant', () => {
    const r = new JitterRing(STREAM_RATE, 480);
    r.adaptive = true;
    for (let i = 0; i < 20; i++) r.push(new Float32Array(960).fill(0.25), new Float32Array(960).fill(-0.25), i * 960);
    const l = new Float32Array(128);
    const rr = new Float32Array(128);
    r.pull(l, rr);
    expect(r.rate).toBeGreaterThan(1); // the fill is far above the target
    for (const v of l) expect(v).toBeCloseTo(0.25, 6);
    for (const v of rr) expect(v).toBeCloseTo(-0.25, 6);
  });
});
