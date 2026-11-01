// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { RovoxSocket, socketUrl } from '../src/net/ws';
import { entrySuffices, geomKey, peakWindow } from '../src/views/peaks';

class FakeWebSocket {
  static instances: FakeWebSocket[] = [];
  static OPEN = 1;
  readyState = 0;
  binaryType = '';
  sent: string[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  constructor(readonly url: string) {
    FakeWebSocket.instances.push(this);
  }
  send(d: string) {
    this.sent.push(d);
  }
  close() {
    this.readyState = 3;
    this.onclose?.();
  }
  open() {
    this.readyState = 1;
    this.onopen?.();
  }
  receive(obj: unknown) {
    this.onmessage?.({ data: JSON.stringify(obj) });
  }
}

beforeEach(() => {
  FakeWebSocket.instances = [];
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.useFakeTimers();
});

const handlers = () => ({ onStatus: vi.fn(), onMessage: vi.fn(), onBinary: vi.fn() });

describe('socket', () => {
  it('builds ws / wss urls', () => {
    expect(socketUrl({ protocol: 'http:', host: 'localhost:4810' })).toBe('ws://localhost:4810/ws');
    expect(socketUrl({ protocol: 'https:', host: 'x.example' })).toBe('wss://x.example/ws');
  });

  it('says hello on open and correlates cmd with result', async () => {
    const h = handlers();
    const s = new RovoxSocket('ws://x/ws', h, (u) => new FakeWebSocket(u) as unknown as WebSocket);
    s.connect();
    const ws = FakeWebSocket.instances[0]!;
    ws.open();
    expect(JSON.parse(ws.sent[0]!).t).toBe('hello');
    const p = s.cmd('track.update', { track_id: 'trk_a', volume_db: -3 }, { projectId: 'p_1', coalesceKey: 'vol:trk_a' });
    const sent = JSON.parse(ws.sent[1]!);
    expect(sent).toMatchObject({ t: 'cmd', name: 'track.update', project_id: 'p_1', coalesce_key: 'vol:trk_a' });
    ws.receive({ t: 'result', id: sent.id, ok: true, revision: 12 });
    await expect(p).resolves.toMatchObject({ ok: true, revision: 12 });
    expect(h.onMessage).not.toHaveBeenCalled(); // results are consumed by cmd()
    ws.receive({ t: 'event', type: 'project.changed' });
    expect(h.onMessage).toHaveBeenCalledTimes(1);
  });

  it('reconnects with back-off and fails pending commands when the socket drops', async () => {
    const h = handlers();
    const s = new RovoxSocket('ws://x/ws', h, (u) => new FakeWebSocket(u) as unknown as WebSocket);
    s.connect();
    FakeWebSocket.instances[0]!.open();
    const p = s.cmd('undo');
    FakeWebSocket.instances[0]!.close();
    await expect(p).resolves.toMatchObject({ ok: false, error: { code: 'DISCONNECTED' } });
    expect(FakeWebSocket.instances).toHaveLength(1);
    vi.advanceTimersByTime(600);
    expect(FakeWebSocket.instances).toHaveLength(2);
    FakeWebSocket.instances[1]!.close();
    vi.advanceTimersByTime(600);
    expect(FakeWebSocket.instances).toHaveLength(2); // second back-off is 1 s
    vi.advanceTimersByTime(600);
    expect(FakeWebSocket.instances).toHaveLength(3);
  });

  it('a command on a closed socket fails at once', async () => {
    const s = new RovoxSocket('ws://x/ws', handlers(), (u) => new FakeWebSocket(u) as unknown as WebSocket);
    await expect(s.cmd('undo')).resolves.toMatchObject({ ok: false, error: { code: 'DISCONNECTED' } });
  });
});

describe('peak windows', () => {
  const item = { item_id: 'i', track_id: 't', name: 'n', start_s: 0, length_s: 100, end_s: 100, muted: false, offset_s: 2, file: 'a.wav' };

  it('asks for the visible part widened by a quarter on each side, one bin per pixel', () => {
    const w = peakWindow(item, 40, 60, 50);
    expect(w.t0).toBe(35);
    expect(w.t1).toBe(65);
    expect(w.bins).toBe(1500);
  });

  it('clamps to the item and to 4096 bins', () => {
    const w = peakWindow(item, 0, 100, 1000);
    expect(w.t0).toBe(0);
    expect(w.t1).toBe(100);
    expect(w.bins).toBe(4096);
  });

  it('an entry suffices only for its geometry, its window and a density near the one needed', () => {
    const e = { geom: geomKey(item), t0: 35, t1: 65, bins: 1500, data: new Int8Array(3000) };
    expect(entrySuffices(e, item, 40, 60, 50)).toBe(true);
    expect(entrySuffices(e, item, 30, 60, 50)).toBe(false); // outside the window
    expect(entrySuffices(e, item, 40, 60, 200)).toBe(false); // zoomed in: too coarse
    expect(entrySuffices(e, { ...item, offset_s: 3 }, 40, 60, 50)).toBe(false); // the source window moved
  });
});
