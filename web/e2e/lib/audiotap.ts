// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// The app does not publish its output node, so the tests tap it from outside: an init script wraps AudioNode.connect and, whenever something is
// connected to an AudioDestinationNode, also feeds it into an AnalyserNode and a ScriptProcessor in the page. Nothing in web/src is touched.
import type { Page } from '@playwright/test';

export interface Onset {
  /** performance.now() at which the click is audible (output timestamp of the block it was in). */
  audibleMs: number;
}

export interface PlayheadSample {
  ms: number;
  pos: number;
}

export async function installAudioTap(page: Page): Promise<void> {
  await page.addInitScript(() => {
    const w = window as any;
    w.__tap = { analysers: [] as AnalyserNode[], onsets: [] as { audibleMs: number }[], samples: [] as { ms: number; pos: number }[], recording: false };
    const orig = AudioNode.prototype.connect as (...a: unknown[]) => unknown;
    (AudioNode.prototype as any).connect = function (this: AudioNode, dest: unknown, ...rest: unknown[]) {
      if (typeof AudioDestinationNode !== 'undefined' && dest instanceof AudioDestinationNode) {
        const ctx = this.context as AudioContext & { __tapped?: boolean };
        if (!ctx.__tapped) {
          ctx.__tapped = true;
          const an = ctx.createAnalyser();
          an.fftSize = 4096;
          an.smoothingTimeConstant = 0;
          w.__tap.analysers.push(an);
          const sp = ctx.createScriptProcessor(256, 2, 1);
          let quiet = true;
          sp.onaudioprocess = (e: AudioProcessingEvent) => {
            const d = e.inputBuffer.getChannelData(0);
            let peak = 0;
            for (let i = 0; i < d.length; i++) peak = Math.max(peak, Math.abs(d[i]));
            if (w.__tap.recording && quiet && peak > 0.15) {
              const ts = ctx.getOutputTimestamp();
              // the block starts at e.playbackTime on the context clock; map it to the wall clock the way the speaker hears it
              w.__tap.onsets.push({ audibleMs: (ts.performanceTime ?? performance.now()) + (e.playbackTime - (ts.contextTime ?? ctx.currentTime)) * 1000 });
            }
            quiet = peak < 0.02;
          };
          orig.call(this, an);
          orig.call(this, sp);
          const mute = ctx.createGain();
          mute.gain.value = 0;
          orig.call(sp, mute);
          orig.call(mute, ctx.destination);
        }
      }
      return orig.call(this, dest, ...rest);
    };
  });
}

/** Level of the page's output in dBFS at a frequency (max over the bins around it), -Infinity when there is no tapped context. */
export async function toneDb(page: Page, hz: number): Promise<number> {
  return page.evaluate((f) => {
    const an = (window as any).__tap?.analysers?.[0] as AnalyserNode | undefined;
    if (!an) return -Infinity;
    const bins = new Float32Array(an.frequencyBinCount);
    an.getFloatFrequencyData(bins);
    const k = Math.round((f / an.context.sampleRate) * an.fftSize);
    return Math.max(bins[k - 2], bins[k - 1], bins[k], bins[k + 1], bins[k + 2]);
  }, hz);
}

/** Overall RMS of the page's output (linear). */
export async function outputRms(page: Page): Promise<number> {
  return page.evaluate(() => {
    const an = (window as any).__tap?.analysers?.[0] as AnalyserNode | undefined;
    if (!an) return 0;
    const buf = new Float32Array(an.fftSize);
    an.getFloatTimeDomainData(buf);
    return Math.sqrt(buf.reduce((s, v) => s + v * v, 0) / buf.length);
  });
}

/** Starts sampling the transport clock every animation frame and recording click onsets. */
export async function startRecording(page: Page): Promise<void> {
  await page.evaluate(() => {
    const w = window as any;
    w.__tap.onsets.length = 0;
    w.__tap.samples.length = 0;
    w.__tap.recording = true;
    const read = () => {
      const m = /(\d+):(\d+)\.(\d+)/.exec(document.querySelector('.clock .small')?.textContent ?? '');
      if (m) w.__tap.samples.push({ ms: performance.now(), pos: Number(m[1]) * 60 + Number(m[2]) + Number(m[3]) / 10 ** m[3].length });
      if (w.__tap.recording) requestAnimationFrame(read);
    };
    requestAnimationFrame(read);
  });
}

export async function stopRecording(page: Page): Promise<{ onsets: Onset[]; samples: PlayheadSample[] }> {
  return page.evaluate(() => {
    const t = (window as any).__tap;
    t.recording = false;
    return { onsets: [...t.onsets], samples: [...t.samples] };
  });
}

/** The UI playhead at a wall-clock instant, interpolated between the two surrounding samples. */
export function playheadAt(samples: PlayheadSample[], ms: number): number | null {
  for (let i = 1; i < samples.length; i++) {
    const a = samples[i - 1];
    const b = samples[i];
    if (ms >= a.ms && ms <= b.ms) return b.ms === a.ms ? b.pos : a.pos + ((b.pos - a.pos) * (ms - a.ms)) / (b.ms - a.ms);
  }
  return null;
}
