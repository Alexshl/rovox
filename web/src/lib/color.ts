// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Colour maths for canvas drawing (items are tinted versions of the track colour over the panel colour).

export interface Rgb {
  r: number;
  g: number;
  b: number;
}

export function parseHex(hex: string): Rgb {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  if (!m) return { r: 128, g: 128, b: 128 };
  const n = parseInt(m[1]!, 16);
  return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
}

export function toHex(c: Rgb): string {
  const p = (v: number) => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0');
  return `#${p(c.r)}${p(c.g)}${p(c.b)}`;
}

/** `amount` of `a` over `b` (0..1). */
export function mix(a: string, b: string, amount: number): string {
  const x = parseHex(a);
  const y = parseHex(b);
  return toHex({ r: x.r * amount + y.r * (1 - amount), g: x.g * amount + y.g * (1 - amount), b: x.b * amount + y.b * (1 - amount) });
}

export function alpha(hex: string, a: number): string {
  const c = parseHex(hex);
  return `rgba(${c.r},${c.g},${c.b},${a})`;
}
