// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Browser side of the audio stream (task 330): Opus packets from the WebSocket -> WebCodecs AudioDecoder -> AudioWorklet
// with a 150 ms jitter buffer. The audio context is created from a user gesture (autoplay policy).
import workletUrl from './player.worklet.ts?worker&url';
import { STREAM_RATE, DEFAULT_TARGET_MS } from './jitter';
import type { AudioPacket } from '../net/protocol';

export interface AudioStats {
  supported: boolean;
  enabled: boolean;
  state: 'off' | 'buffering' | 'playing' | 'underrun';
  packets: number;
  decoded: number;
  stale: number;
  gaps: number;
  underruns: number;
  bufferedMs: number;
  outputLatencyMs: number;
  epoch: number;
  lastPacketAt: number;
  error?: string;
}

export function webCodecsOpusSupported(): boolean {
  return typeof AudioDecoder !== 'undefined' && typeof EncodedAudioChunk !== 'undefined' && typeof AudioWorkletNode !== 'undefined';
}

export class BrowserAudio {
  readonly stats: AudioStats = {
    supported: webCodecsOpusSupported(),
    enabled: false,
    state: 'off',
    packets: 0,
    decoded: 0,
    stale: 0,
    gaps: 0,
    underruns: 0,
    bufferedMs: 0,
    outputLatencyMs: 0,
    epoch: 0,
    lastPacketAt: 0,
  };

  private ctx: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
  private decoder: AudioDecoder | null = null;
  // Chrome's Opus AudioDecoder restamps its output, so packet metadata is matched by order (decoder output is FIFO).
  private meta: { epoch: number; pos: number }[] = [];
  private lastSeq = -1;
  private report: { t: number; pos: number | null; playing: boolean } | null = null;
  private listeners = new Set<() => void>();

  constructor(private readonly setServerAudio: (on: boolean) => void) {}

  onChange(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private emit(): void {
    for (const l of this.listeners) l();
  }

  /** Must be called from a click handler. */
  async enable(): Promise<boolean> {
    if (!this.stats.supported) {
      this.stats.error = 'WebCodecs Opus недоступний у цьому браузері';
      this.emit();
      return false;
    }
    try {
      this.ctx ??= new AudioContext({ sampleRate: STREAM_RATE, latencyHint: 'interactive' });
      if (!this.node) {
        await this.ctx.audioWorklet.addModule(workletUrl);
        this.node = new AudioWorkletNode(this.ctx, 'rovox-player', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
        this.node.connect(this.ctx.destination);
        this.node.port.postMessage({ type: 'target', frames: Math.round((STREAM_RATE * DEFAULT_TARGET_MS) / 1000) });
        this.node.port.onmessage = (ev: MessageEvent) => this.onWorkletMessage(ev.data as Record<string, unknown>);
      }
      await this.ctx.resume();
      this.resetDecoder();
      this.stats.enabled = true;
      this.stats.state = 'buffering';
      this.stats.error = undefined;
      this.setServerAudio(true);
    } catch (e) {
      this.stats.error = e instanceof Error ? e.message : String(e);
      this.stats.enabled = false;
    }
    this.emit();
    return this.stats.enabled;
  }

  disable(): void {
    this.stats.enabled = false;
    this.stats.state = 'off';
    this.setServerAudio(false);
    this.node?.port.postMessage({ type: 'flush' });
    this.report = null;
    void this.ctx?.suspend();
    this.emit();
  }

  /** Called when the transport epoch changes (play, stop, seek): whatever is buffered belongs to the old timeline. */
  setEpoch(epoch: number): void {
    if (epoch === this.stats.epoch) return;
    this.stats.epoch = epoch;
    this.flush();
  }

  flush(): void {
    this.node?.port.postMessage({ type: 'flush' });
    this.meta = [];
    this.lastSeq = -1;
    this.report = null;
    this.resetDecoder();
  }

  onPacket(p: AudioPacket): void {
    if (!this.stats.enabled || !this.decoder) return;
    this.stats.packets++;
    this.stats.lastPacketAt = performance.now();
    if (p.epoch < this.stats.epoch) {
      this.stats.stale++;
      return;
    }
    if (p.epoch > this.stats.epoch) this.setEpoch(p.epoch);
    if (this.lastSeq >= 0 && p.seq !== this.lastSeq + 1 && !p.discontinuity) this.stats.gaps++;
    this.lastSeq = p.seq;
    if (this.decoder.state !== 'configured') return;
    try {
      this.decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: p.seq, data: p.opus }));
      this.meta.push({ epoch: p.epoch, pos: p.samplePos });
    } catch (e) {
      this.stats.error = e instanceof Error ? e.message : String(e);
    }
  }

  /** What the listener hears right now, on the project timeline (seconds), or null when the stream is not playing. */
  audiblePosition(): number | null {
    const r = this.report;
    if (!this.ctx || !r || !r.playing || r.pos === null) return null;
    const latency = (this.ctx.outputLatency || this.ctx.baseLatency || 0) as number;
    const elapsed = this.ctx.currentTime - r.t;
    return Math.max(0, r.pos / STREAM_RATE + elapsed - latency);
  }

  private resetDecoder(): void {
    try {
      this.decoder?.close();
    } catch {
      /* already closed */
    }
    this.decoder = null;
    this.meta = [];
    if (!this.stats.supported) return;
    const d = new AudioDecoder({
      output: (data) => this.onDecoded(data),
      error: (e) => {
        this.stats.error = `decoder: ${e.message}`;
        this.emit();
      },
    });
    d.configure({ codec: 'opus', sampleRate: STREAM_RATE, numberOfChannels: 2 });
    this.decoder = d;
  }

  private onDecoded(data: AudioData): void {
    const m = this.meta.shift();
    const frames = data.numberOfFrames;
    const l = new Float32Array(frames);
    const r = new Float32Array(frames);
    data.copyTo(l, { planeIndex: 0, format: 'f32-planar' });
    data.copyTo(data.numberOfChannels > 1 ? r : l, { planeIndex: data.numberOfChannels > 1 ? 1 : 0, format: 'f32-planar' });
    data.close();
    if (!m || m.epoch !== this.stats.epoch || !this.node) return;
    this.stats.decoded++;
    this.node.port.postMessage({ type: 'chunk', l, r, pos: m.pos }, [l.buffer, r.buffer]);
  }

  private onWorkletMessage(m: Record<string, unknown>): void {
    if (m.type !== 'stats') return;
    const playing = m.playing === true;
    this.stats.underruns = m.underruns as number;
    this.stats.bufferedMs = ((m.buffered as number) / STREAM_RATE) * 1000;
    this.stats.outputLatencyMs = ((this.ctx?.outputLatency ?? this.ctx?.baseLatency ?? 0) as number) * 1000;
    this.stats.state = !this.stats.enabled ? 'off' : m.newUnderrun === true ? 'underrun' : playing ? 'playing' : 'buffering';
    this.report = { t: m.t as number, pos: m.pos as number | null, playing };
    this.emit();
  }
}
