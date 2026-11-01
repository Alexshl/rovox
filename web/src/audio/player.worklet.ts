// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// AudioWorklet of the browser stream: pulls decoded stereo chunks (posted by the main thread) through the jitter ring.
import { JitterRing } from './jitter';

interface ChunkMessage {
  type: 'chunk';
  l: Float32Array;
  r: Float32Array;
  pos: number;
}

type InMessage = ChunkMessage | { type: 'flush' } | { type: 'target'; frames: number };

declare const sampleRate: number;
declare const currentTime: number;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
}
declare function registerProcessor(name: string, ctor: new () => AudioWorkletProcessor): void;

class RovoxPlayer extends AudioWorkletProcessor {
  private ring = new JitterRing();
  private lastReport = 0;
  private lastUnderruns = 0;

  constructor() {
    super();
    this.port.onmessage = (ev: MessageEvent<InMessage>) => {
      const m = ev.data;
      if (m.type === 'chunk') this.ring.push(m.l, m.r, m.pos);
      else if (m.type === 'flush') this.ring.flush();
      else if (m.type === 'target') this.ring.targetFrames = m.frames;
    };
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    if (!out || !out[0]) return true;
    const left = out[0];
    const right = out[1] ?? out[0];
    this.ring.pull(left, right);
    if (currentTime - this.lastReport >= 0.033) {
      this.lastReport = currentTime;
      const s = this.ring.stats();
      this.port.postMessage({
        type: 'stats',
        t: currentTime,
        buffered: s.buffered,
        underruns: s.underruns,
        playing: s.playing,
        newUnderrun: s.underruns !== this.lastUnderruns,
        pos: this.ring.position(),
        rate: sampleRate,
      });
      this.lastUnderruns = s.underruns;
    }
    return true;
  }
}

registerProcessor('rovox-player', RovoxPlayer as unknown as new () => AudioWorkletProcessor);
