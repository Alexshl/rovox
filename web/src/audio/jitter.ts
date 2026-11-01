// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Jitter buffer of the browser stream: a ring of decoded stereo samples that starts playing once `targetFrames` are buffered
// (150 ms by default, the figure from spike P3), goes back to buffering on an underrun, and knows which timeline sample it is
// about to output. Pure logic: used by the AudioWorklet and by the unit tests.

export const STREAM_RATE = 48000;
export const DEFAULT_TARGET_MS = 150;

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
  underruns = 0;
  overflows = 0;
  pushed = 0;

  constructor(
    readonly capacity = STREAM_RATE * 2,
    public targetFrames = Math.round((STREAM_RATE * DEFAULT_TARGET_MS) / 1000),
  ) {
    this.l = new Float32Array(capacity);
    this.r = new Float32Array(capacity);
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
    const have = Math.min(n, this.buffered);
    for (let i = 0; i < have; i++) {
      const k = (this.readIdx + i) % this.capacity;
      outL[i] = this.l[k]!;
      outR[i] = this.r[k]!;
    }
    for (let i = have; i < n; i++) {
      outL[i] = 0;
      outR[i] = 0;
    }
    this.readIdx += have;
    this.dropOldChunks();
    if (have < n) {
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
  }

  stats(): JitterStats {
    return { buffered: this.buffered, underruns: this.underruns, overflows: this.overflows, playing: this._playing, pushed: this.pushed };
  }

  private dropOldChunks(): void {
    // keep the chunk that contains readIdx as the first element
    while (this.chunks.length > 1 && this.chunks[1]!.start <= this.readIdx) this.chunks.shift();
  }
}
