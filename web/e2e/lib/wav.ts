// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Deterministic WAV generators (16-bit mono PCM): the same bytes on every run, so the waveforms in the screenshots are stable.

const RATE = 44100;

function wav(samples: Int16Array): Buffer {
  const data = Buffer.alloc(samples.length * 2);
  samples.forEach((s, i) => data.writeInt16LE(s, i * 2));
  const head = Buffer.alloc(44);
  head.write('RIFF', 0);
  head.writeUInt32LE(36 + data.length, 4);
  head.write('WAVEfmt ', 8);
  head.writeUInt32LE(16, 16);
  head.writeUInt16LE(1, 20); // PCM
  head.writeUInt16LE(1, 22); // mono
  head.writeUInt32LE(RATE, 24);
  head.writeUInt32LE(RATE * 2, 28);
  head.writeUInt16LE(2, 32);
  head.writeUInt16LE(16, 34);
  head.write('data', 36);
  head.writeUInt32LE(data.length, 40);
  return Buffer.concat([head, data]);
}

/** A sine with a slow tremolo so the waveform is not a flat block. */
export function sine(freq: number, seconds: number, amp = 0.5, tremoloHz = 0.5): Buffer {
  const n = Math.round(seconds * RATE);
  const out = new Int16Array(n);
  for (let i = 0; i < n; i++) {
    const t = i / RATE;
    out[i] = Math.round(32767 * amp * (0.65 + 0.35 * Math.sin(2 * Math.PI * tremoloHz * t)) * Math.sin(2 * Math.PI * freq * t));
  }
  return wav(out);
}

/** Seeded linear congruential noise bursts on every beat (a hi-hat-like pattern). */
export function noise(seconds: number, seed = 1, amp = 0.4, bpm = 120): Buffer {
  const n = Math.round(seconds * RATE);
  const out = new Int16Array(n);
  let s = seed >>> 0;
  const beat = (60 / bpm) * RATE;
  for (let i = 0; i < n; i++) {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    const phase = (i % beat) / beat;
    out[i] = Math.round(32767 * amp * Math.exp(-6 * phase) * ((s / 0xffffffff) * 2 - 1));
  }
  return wav(out);
}

/** A click track: a 2 ms 1 kHz burst on every beat, silence between. The first click is at `offsetS`. */
export function clicks(seconds: number, bpm = 120, offsetS = 0): Buffer {
  const n = Math.round(seconds * RATE);
  const out = new Int16Array(n);
  const period = (60 / bpm) * RATE;
  const len = Math.round(0.002 * RATE);
  for (let b = 0; offsetS * RATE + b * period < n; b++) {
    const at = Math.round(offsetS * RATE + b * period);
    for (let k = 0; k < len && at + k < n; k++) out[at + k] = Math.round(32767 * 0.9 * Math.sin((2 * Math.PI * 1000 * k) / RATE));
  }
  return wav(out);
}
