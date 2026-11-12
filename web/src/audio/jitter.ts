// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Jitter buffer of the browser stream: a ring of decoded stereo samples that starts playing once `targetFrames` are buffered
// (150 ms by default, the figure from spike P3), goes back to buffering on an underrun, and knows which timeline sample it is
// about to output. Pure logic: used by the AudioWorklet and by the unit tests.

export const STREAM_RATE = 48000;
export const DEFAULT_TARGET_MS = 150;

/** Largest deviation of the playback rate from 1 the drift controller may ask for (0.5 %, about 8 cents of pitch). */
export const MAX_RATE_DEVIATION = 0.005;

/** Fill error (ms) under which the rate stays exactly 1, and the smaller error at which a correction ends again (hysteresis). */
export const DEADBAND_MS = 15;
export const DEADBAND_RELEASE_MS = 5;

/**
 * Keeps the buffer fill near the target against the clock drift between the server (48 kHz) and the sound card: a slow
 * proportional controller on the smoothed fill returns the playback rate to resample with (1 = untouched). Pure: it only sees
 * the fill and the number of frames output since the last call. A fill above the target speeds playback up, below slows it
 * down; the gain makes a drift of d need only d / gain of offset (200 ppm -> 2 ms), the time constant is 1 / gain (10 s).
 * A deadband keeps the rate exactly 1 (bit-exact playback) while the fill is within DEADBAND_MS of the target; a correction
 * starts outside it and ends once the fill is back within DEADBAND_RELEASE_MS.
 */
/** Smallest rate deviation used while a correction is running (0.2 %). */
export const MIN_ACTIVE_DEVIATION = 0.002;

export class DriftController {
  private smooth: number | null = null;
  private correcting = false;

  constructor(
    public targetFrames: number,
    private readonly gain = 0.1, // rate deviation per second of fill error
    private readonly smoothingS = 1, // time constant of the fill low-pass (chunks arrive in 20 ms steps)
    private readonly maxDeviation = MAX_RATE_DEVIATION,
    private readonly rate = STREAM_RATE,
    private readonly deadbandMs = DEADBAND_MS,
    private readonly releaseMs = DEADBAND_RELEASE_MS,
  ) {}

  /** `buffered` frames now, `frames` output since the last call. Returns the rate for the next block. */
  update(buffered: number, frames: number): number {
    const dt = frames / this.rate;
    if (this.smooth === null) this.smooth = buffered;
    else this.smooth += (buffered - this.smooth) * Math.min(1, dt / this.smoothingS);
    const errS = (this.smooth - this.targetFrames) / this.rate;
    const absMs = Math.abs(errS) * 1000;
    if (this.correcting) {
      if (absMs <= this.releaseMs) this.correcting = false;
    } else if (absMs >= this.deadbandMs) this.correcting = true;
    if (!this.correcting) return 1;
    // while correcting, move at least MIN_ACTIVE_DEVIATION: a proportional-only pull gets slower and slower near the release point
    // and would keep the interpolator running a fifth of the time; a firmer pull finishes in seconds and goes back to bit-exact
    const dev = Math.min(this.maxDeviation, Math.max(MIN_ACTIVE_DEVIATION, this.gain * Math.abs(errS)));
    return 1 + Math.sign(errS) * dev;
  }

  reset(): void {
    this.smooth = null;
    this.correcting = false;
  }
}

/** 4-point cubic Hermite (Catmull-Rom) between y1 and y2 at fraction f. */
export function hermite(y0: number, y1: number, y2: number, y3: number, f: number): number {
  return y1 + 0.5 * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
}

/** Serial-number arithmetic on the 16-bit epoch: `a` is newer than `b` if it is ahead by less than half the range. */
export function epochNewer(a: number, b: number): boolean {
  const d = (a - b) & 0xffff;
  return d !== 0 && d < 0x8000;
}

export interface JitterStats {
  buffered: number;
  underruns: number;
  overflows: number;
  playing: boolean;
  pushed: number;
}

export class JitterRing {
  private readonly l: Float32Array;
  private readonly r: Float32Array;
  private writeIdx = 0; // absolute frame counters (never wrap, doubles are exact far beyond a day of audio)
  private readIdx = 0;
  private chunks: { start: number; pos: number }[] = [];
  private _playing = false;
  private lastDev = 0; // rate deviation of the last correction
  private frac = 0; // fractional read offset (frames after readIdx) while resampling
  private readonly drift: DriftController;
  /** Adaptive resampling against clock drift; off by default so plain playback stays bit-exact. */
  adaptive = false;
  rate = 1;
  underruns = 0;
  overflows = 0;
  pushed = 0;

  constructor(
    readonly capacity = STREAM_RATE * 2,
    public targetFrames = Math.round((STREAM_RATE * DEFAULT_TARGET_MS) / 1000),
  ) {
    this.l = new Float32Array(capacity);
    this.r = new Float32Array(capacity);
    this.drift = new DriftController(targetFrames);
  }

  get buffered(): number {
    return this.writeIdx - this.readIdx;
  }

  get playing(): boolean {
    return this._playing;
  }

  /** Appends a decoded chunk; `pos` is the timeline sample of its first frame. */
  push(l: Float32Array, r: Float32Array, pos: number): void {
    const n = Math.min(l.length, r.length);
    if (n > this.capacity) return;
    if (this.buffered + n > this.capacity) {
      // the consumer is far behind: drop the oldest audio, never block
      const drop = this.buffered + n - this.capacity;
      this.readIdx += drop;
      this.overflows++;
    }
    for (let i = 0; i < n; i++) {
      const k = (this.writeIdx + i) % this.capacity;
      this.l[k] = l[i]!;
      this.r[k] = r[i]!;
    }
    this.chunks.push({ start: this.writeIdx, pos });
    this.writeIdx += n;
    this.pushed += n;
    if (!this._playing && this.buffered >= this.targetFrames) this._playing = true;
  }

  /** Fills `outL` / `outR`. Returns true if it had to insert silence (an underrun). */
  pull(outL: Float32Array, outR: Float32Array): boolean {
    const n = outL.length;
    if (!this._playing) {
      outL.fill(0);
      outR.fill(0);
      return false;
    }
    if (this.adaptive) {
      this.drift.targetFrames = this.targetFrames;
      this.rate = this.drift.update(this.buffered, n);
    } else this.rate = 1;
    // cubic Hermite between neighbours while the drift controller corrects; at rate 1 the fractional part stays 0 and the samples
    // are copied as they are. When a correction ends, its remaining fraction is run out (tiny steps) and snapped to a whole frame.
    const rate = this.rate;
    if (rate !== 1) this.lastDev = rate - 1;
    let settling = rate === 1 && this.frac !== 0;
    let p = this.frac;
    let have = 0;
    for (; have < n; have++) {
      const i0 = Math.floor(p);
      const f = p - i0;
      if (i0 >= this.buffered || (f > 0 && i0 + 1 >= this.buffered)) break;
      const base = this.readIdx + i0;
      const k = base % this.capacity;
      if (f === 0) {
        outL[have] = this.l[k]!;
        outR[have] = this.r[k]!;
      } else {
        const km = this.slot(base - 1, base);
        const k1 = (base + 1) % this.capacity;
        const k2 = base + 2 < this.writeIdx ? (base + 2) % this.capacity : k1;
        outL[have] = hermite(this.l[km]!, this.l[k]!, this.l[k1]!, this.l[k2]!, f);
        outR[have] = hermite(this.r[km]!, this.r[k]!, this.r[k1]!, this.r[k2]!, f);
      }
      if (settling) {
        p += 1 + this.lastDev;
        const near = Math.round(p);
        if (Math.abs(p - near) <= Math.abs(this.lastDev)) {
          p = near;
          settling = false;
        }
      } else p += rate;
    }
    for (let i = have; i < n; i++) {
      outL[i] = 0;
      outR[i] = 0;
    }
    const whole = Math.min(Math.floor(p), this.buffered);
    this.readIdx += whole;
    this.frac = whole === Math.floor(p) ? p - whole : 0;
    this.dropOldChunks();
    if (have < n) {
      this.frac = 0;
      this.drift.reset();
      this.underruns++;
      this._playing = false; // buffer up to the target again
      return true;
    }
    return false;
  }

  /** Timeline sample of the next frame to be output, or null if nothing was ever pushed. */
  position(): number | null {
    this.dropOldChunks();
    const c = this.chunks[0];
    if (!c) return null;
    return c.pos + (this.readIdx - c.start);
  }

  flush(): void {
    this.writeIdx = 0;
    this.readIdx = 0;
    this.chunks = [];
    this._playing = false;
    this.frac = 0;
    this.drift.reset();
  }

  stats(): JitterStats {
    return { buffered: this.buffered, underruns: this.underruns, overflows: this.overflows, playing: this._playing, pushed: this.pushed };
  }

  /** Ring slot of the absolute frame `abs` (the one before the read position), or of `fallback` if it is gone. */
  private slot(abs: number, fallback: number): number {
    return (abs >= 0 && abs >= this.writeIdx - this.capacity ? abs : fallback) % this.capacity;
  }

  private dropOldChunks(): void {
    // keep the chunk that contains readIdx as the first element
    while (this.chunks.length > 1 && this.chunks[1]!.start <= this.readIdx) this.chunks.shift();
  }
}
