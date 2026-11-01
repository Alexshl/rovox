// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { AUDIO_HEADER_BYTES, parseAudio, parseMeters, parsePosition } from '../src/net/protocol';
import { parseRpc } from '../src/net/mcp';

describe('binary frames', () => {
  it('parses a 0x03 position frame', () => {
    const b = new ArrayBuffer(13);
    const v = new DataView(b);
    v.setUint8(0, 3);
    v.setUint8(1, 7);
    v.setUint16(2, 513, true);
    v.setFloat64(4, 12.375, true);
    v.setUint8(12, 1);
    expect(parsePosition(b)).toEqual({ pidx: 7, epoch: 513, position_s: 12.375, playing: true });
    expect(parsePosition(new ArrayBuffer(5))).toBeNull();
  });

  it('parses a 0x02 meter frame', () => {
    const b = new ArrayBuffer(4 + 2 * 12);
    const v = new DataView(b);
    v.setUint8(0, 2);
    v.setUint8(1, 3);
    v.setUint16(2, 2, true);
    v.setUint32(4, 0, true);
    v.setFloat32(8, -6, true);
    v.setFloat32(12, -7, true);
    v.setUint32(16, 5, true);
    v.setFloat32(20, -100, true);
    v.setFloat32(24, -100, true);
    const f = parseMeters(b)!;
    expect(f.pidx).toBe(3);
    expect(f.entries).toEqual([
      { index: 0, l: -6, r: -7 },
      { index: 5, l: -100, r: -100 },
    ]);
    expect(parseMeters(b.slice(0, 10))).toBeNull();
  });

  it('parses a 0x01 audio packet with the epoch in the header', () => {
    const b = new ArrayBuffer(AUDIO_HEADER_BYTES + 3);
    const v = new DataView(b);
    v.setUint8(0, 1);
    v.setUint8(1, 1);
    v.setUint16(4, 9, true);
    v.setUint32(8, 4242, true);
    v.setBigUint64(12, 96000n, true);
    v.setBigUint64(20, 5000000n, true);
    new Uint8Array(b, AUDIO_HEADER_BYTES).set([1, 2, 3]);
    const p = parseAudio(b)!;
    expect(p.discontinuity).toBe(true);
    expect(p.epoch).toBe(9);
    expect(p.seq).toBe(4242);
    expect(p.samplePos).toBe(96000);
    expect([...p.opus]).toEqual([1, 2, 3]);
    expect(parseAudio(new ArrayBuffer(AUDIO_HEADER_BYTES))).toBeNull();
  });
});

describe('MCP over HTTP', () => {
  it('reads a plain JSON answer and the last data line of an SSE answer', () => {
    expect(parseRpc('{"jsonrpc":"2.0","id":1,"result":{"structuredContent":{"ok":true}}}').result?.structuredContent?.ok).toBe(true);
    const sse = 'event: message\ndata: {"jsonrpc":"2.0","method":"notifications/progress","params":{}}\n\nevent: message\ndata: {"jsonrpc":"2.0","id":2,"result":{"structuredContent":{"ok":false}}}\n\n';
    expect(parseRpc(sse).result?.structuredContent?.ok).toBe(false);
    expect(parseRpc('').error).toBeDefined();
  });
});
