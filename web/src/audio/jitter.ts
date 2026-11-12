// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Jitter buffer of the browser stream: a ring of decoded stereo samples that starts playing once `targetFrames` are buffered
// (150 ms by default, the figure from spike P3), goes back to buffering on an underrun, and knows which timeline sample it is
// about to output. Pure logic: used by the AudioWorklet and by the unit tests.

export const STREAM_RATE = 48000;
export const DEFAULT_TARGET_MS = 150;

/** Largest deviation of the playback rate from 1 the drift controller may ask for (0.5 %, about 8 cents of pitch). */
export const MAX_RATE_DEVIATION = 0.005;

/**
 * Keeps the buffer fill near the target against the clock drift between the server (48 kHz) and the sound card: a slow
 * proportional controller on the smoothed fill returns the playback rate to resample with (1 = untouched). Pure: it only sees
 * the fill and the number of frames output since the last call. A fill above the target speeds playback up, below slows it
 * down; the gain makes a drift of d need only d / gain of offset (200 ppm -> 2 ms), the time constant is 1 / gain (10 s).
 */
export class DriftController {
  private smooth: number | null = null;

  constructor(
    public targetFrames: number,
    private readonly gain = 0.1, // rate deviation per second of fill error
    private readonly smoothingS = 1, // time constant of the fill low-pass (chunks arrive in 20 ms steps)
    private readonly maxDeviation = MAX_RATE_DEVIATION,
    private readonly rate = STREAM_RATE,
  ) {}

  /** `buffered` frames now, `frames` output since the last call. Returns the rate for the next block. */
  update(buffered: number, frames: number): number {
    const dt = frames / this.rate;
    if (this.smooth === null) this.smooth = buffered;
    else this.smooth += (buffered - this.smooth) * Math.min(1, dt / this.smoothingS);
    const errS = (this.smooth - this.targetFrames) / this.rate;
    return 1 + Math.max(-this.maxDeviation, Math.min(this.maxDeviation, this.gain * errS));
  }

  reset(): void {
    this.smooth = null;
  }
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
    // linear interpolation between neighbours; at rate 1 the fractional part stays 0 and the samples are copied as they are
    let p = this.frac;
    let have = 0;
    for (; have < n; have++) {
      const i0 = Math.floor(p);
      const f = p - i0;
      if (i0 >= this.buffered || (f > 0 && i0 + 1 >= this.buffered)) break;
      const k = (this.readIdx + i0) % this.capacity;
      if (f === 0) {
        outL[have] = this.l[k]!;
        outR[have] = this.r[k]!;
      } else {
        const k1 = (k + 1) % this.capacity;
        outL[have] = this.l[k]! * (1 - f) + this.l[k1]! * f;
        outR[have] = this.r[k]! * (1 - f) + this.r[k1]! * f;
      }
      p += this.rate;
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

  private dropOldChunks(): void {
    // keep the chunk that contains readIdx as the first element
    while (this.chunks.length > 1 && this.chunks[1]!.start <= this.readIdx) this.chunks.shift();
  }
}
