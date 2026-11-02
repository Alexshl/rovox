// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The whole window against a scripted server: tabs, TCP rows, agent highlight (agent-cyan class), MCP panel, Undo agent / Accept.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

let hasCanvas = false;
try {
  await import('canvas');
  hasCanvas = true;
} catch {
  /* node-canvas is an optional dependency: the canvas test is skipped without it */
}

class FakeWebSocket {
  static instances: FakeWebSocket[] = [];
  static OPEN = 1;
  readyState = 0;
  binaryType = '';
  sent: Record<string, any>[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  static handler: (msg: Record<string, any>, ws: FakeWebSocket) => void = () => undefined;
  constructor(readonly url: string) {
    FakeWebSocket.instances.push(this);
    setTimeout(() => {
      this.readyState = 1;
      this.onopen?.();
    }, 0);
  }
  send(d: string) {
    const msg = JSON.parse(d);
    this.sent.push(msg);
    setTimeout(() => FakeWebSocket.handler(msg, this), 0);
  }
  close() {
    this.readyState = 3;
    this.onclose?.();
  }
  push(obj: unknown) {
    this.onmessage?.({ data: JSON.stringify(obj) });
  }
}

const project = {
  project_id: 'p_1',
  name: 'rahiv',
  revision: 10,
  sample_rate: 44100,
  length_s: 30,
  can_undo: true,
  can_redo: false,
  dirty: false,
  tracks: [
    { track_id: 'trk_a', index: 0, name: 'Flute', color: '#4cc9f0', mute: false, solo: false, volume_db: -3, pan: 0 },
    { track_id: 'trk_b', index: 1, name: 'Bass DI', color: '#ffb627', mute: true, solo: false, volume_db: 0, pan: -0.2 },
  ],
  master: { track_id: 'master', volume_db: 0, pan: 0 },
  items: [{ item_id: 'itm_1', track_id: 'trk_a', name: 'flute_solo.wav', start_s: 4, length_s: 6, end_s: 10, muted: false, offset_s: 0 }],
  markers: [],
  tempo: { points: [{ bpm: 118, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } },
  fx: { trk_a: [{ plugin_id: 'fx_rev', name: 'ReaVerb', type: 'reverb', enabled: true }], trk_b: [] },
};

const transport = { state: 'stopped', position_s: 0, epoch: 0, loop: { enabled: false, start_s: 0, end_s: 0 }, length_s: 30, audio: 'browser-stream' };

function answer(msg: Record<string, any>, ws: FakeWebSocket): void {
  if (msg.t === 'hello') {
    ws.push({ t: 'welcome', session: 'ws_1', server_version: '0.1.0', audio: { mode: 'browser-stream', available: true }, source_url: 'https://example.invalid/src' });
  } else if (msg.t === 'sub') {
    ws.push({ t: 'snapshot', project_id: 'p_1', pidx: 1, revision: 10, state: project, transport, meter_index: ['master', 'trk_a', 'trk_b'], accepted_revision: 0 });
  } else if (msg.t === 'cmd') {
    const reply = (data: unknown) => ws.push({ t: 'result', id: msg.id, ok: true, revision: 10, data });
    switch (msg.name) {
      case 'project.list':
      case 'project.catalog':
        return reply({ projects: [{ project_id: 'p_1', name: 'rahiv' }] });
      case 'project.get':
        return reply(project);
      case 'history.list': {
        const step = (rev: number, kind: string) => ({ revision: rev, label: 'x', time: '', origin: { kind, session: 's', label: kind === 'mcp' ? 'Claude' : 'ui' } });
        return reply({ undo: [step(8, 'ui'), step(11, 'mcp'), step(12, 'mcp')], redo: [] });
      }
      case 'project.activity':
        return reply({ activity: [] });
      default:
        return reply({});
    }
  }
}

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));

beforeEach(() => {
  FakeWebSocket.instances = [];
  FakeWebSocket.handler = answer;
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) })));
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
  if (!hasCanvas) vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
  project.revision = 10;
  document.body.innerHTML = '';
  localStorage.clear();
});

afterEach(() => {
  vi.unstubAllGlobals();
});

async function boot() {
  const { App } = await import('../src/store/session');
  const { Shell } = await import('../src/views/shell');
  const app = new App('ws://test/ws');
  const shell = new Shell(app);
  document.body.append(shell.el);
  await app.start();
  await tick(350);
  return { app, shell, ws: FakeWebSocket.instances[0]! };
}

describe('the window', () => {
  it('shows the project tab, the track control panels, the transport readouts and the mixer strips', async () => {
    const { app } = await boot();
    expect(app.activeId).toBe('p_1');
    const tab = document.querySelector('.tab')!;
    expect(tab.getAttribute('aria-selected')).toBe('true');
    expect(tab.textContent).toContain('rahiv');
    const rows = [...document.querySelectorAll('.tcp')];
    expect(rows).toHaveLength(2);
    expect(rows[0]!.querySelector('.name')!.textContent).toBe('Flute');
    expect(rows[1]!.querySelector('.sbtn.mute')!.classList.contains('on')).toBe(true);
    expect(rows[0]!.querySelector('.sbtn.fx')!.classList.contains('has')).toBe(true);
    expect(document.querySelector('.ruler-row .head')!.textContent).toBe('TRACKS · 2');
    expect(document.querySelector('.clock .big')!.textContent).toBe('1.1.00');
    expect(document.querySelector('.clock .small')!.textContent).toBe('0:00.000 / 0:30.000');
    expect(document.querySelector('.transport')!.textContent).toContain('118.00');
    expect(document.querySelectorAll('.strip')).toHaveLength(3); // two tracks + master
    expect(document.querySelector('.strip.master .dbread')!.textContent).toBe('0.0 dB');
    expect(document.querySelector('.strip .panread')!.textContent).toBe('C');
    expect(document.querySelectorAll('.strip .panread')[1]!.textContent).toBe('20% L');
    // Ukrainian UI, no locale formatted numbers
    expect(document.body.textContent).not.toMatch(/\d,\d{3}\b/);
    expect(document.body.textContent).not.toMatch(/\b(AM|PM)\b/);
  });

  it('tints what an MCP agent changed, lists the agent, counts Undo agent and clears it on Accept', async () => {
    const { app, ws } = await boot();
    project.revision = 12; // what the server reports after the agent's step
    ws.push({
      t: 'event',
      type: 'project.changed',
      project_id: 'p_1',
      revision: 12,
      origin: { kind: 'mcp', session: 'mcp_1', label: 'Claude' },
      undo_label: 'Claude: item.split flute_solo.wav',
      changes: [{ kind: 'item', id: 'itm_1', op: 'update', fields: ['length'] }, { kind: 'plugin', id: 'fx_rev', op: 'update', fields: ['wet'] }],
    });
    await tick(300);

    const rows = [...document.querySelectorAll('.tcp')];
    expect(rows[0]!.classList.contains('agent')).toBe(true);
    expect(rows[1]!.classList.contains('agent')).toBe(false);
    expect(document.querySelector('.agent-row b')!.textContent).toBe('Claude');
    expect(document.querySelector('.agent-row')!.classList.contains('active')).toBe(true);
    expect(document.querySelector('.log-row .tool.agent')!.textContent).toBe('item.split');
    expect(document.querySelector('.tab .dot')!.classList.contains('agent')).toBe(true);

    const undoAgent = [...document.querySelectorAll<HTMLButtonElement>('.mcp-actions button')].find((b) => b.textContent!.startsWith('Undo агента'))!;
    expect(undoAgent.textContent).toBe('Undo агента (2)');
    expect(undoAgent.disabled).toBe(false);
    const accept = document.querySelector<HTMLButtonElement>('.mcp-actions button.primary')!;
    expect(accept.disabled).toBe(false);

    accept.click();
    await tick(100);
    expect(app.active!.marks.size).toBe(0);
    expect(document.querySelectorAll('.tcp.agent')).toHaveLength(0);
    expect(accept.disabled).toBe(true);
    expect(ws.sent.some((m) => m.name === 'agent.accept' && m.params.up_to_revision === 12)).toBe(true);
  });

  it('shows the object-storage sync state next to the project tab: nothing for local, cloud ok / uploading / error with a tooltip', async () => {
    const { app, ws } = await boot();
    expect(document.querySelector('.tab .sync')).toBeNull(); // local storage: no indicator

    ws.push({ t: 'event', type: 'storage.changed', project_id: 'p_1', storage: { backend: 's3', synced_revision: 11, pending_uploads: 0, last_error: null } });
    await tick(300);
    const ok = document.querySelector('.tab .sync')!;
    expect(ok.classList.contains('ok')).toBe(true);
    expect(ok.getAttribute('title')).toContain('11');

    ws.push({ t: 'event', type: 'storage.changed', project_id: 'p_1', storage: { backend: 's3', synced_revision: 11, pending_uploads: 1, last_error: null } });
    await tick(300);
    expect(document.querySelector('.tab .sync')!.classList.contains('uploading')).toBe(true);

    ws.push({ t: 'event', type: 'storage.changed', project_id: 'p_1', storage: { backend: 's3', synced_revision: 11, pending_uploads: 1, last_error: 'the bucket refused the credentials' } });
    await tick(300);
    const bad = document.querySelector('.tab .sync')!;
    expect(bad.classList.contains('error')).toBe(true);
    expect(bad.getAttribute('title')).toContain('the bucket refused the credentials');
    expect(app.active!.storage?.backend).toBe('s3');
  });

  it('Undo agent sends one undo per agent step and nothing for a person step', async () => {
    const { ws } = await boot();
    project.revision = 12;
    ws.push({ t: 'event', type: 'project.changed', project_id: 'p_1', revision: 12, origin: { kind: 'mcp', session: 'mcp_1', label: 'Claude' }, undo_label: 'Claude: track.update', changes: [{ kind: 'track', id: 'trk_a', op: 'update' }] });
    await tick(300);
    const before = ws.sent.filter((m) => m.name === 'undo').length;
    [...document.querySelectorAll<HTMLButtonElement>('.mcp-actions button')].find((b) => b.textContent!.startsWith('Undo агента'))!.click();
    await tick(200);
    expect(ws.sent.filter((m) => m.name === 'undo').length - before).toBe(2);
  });

  it('a fader move by the person is sent as one coalesced gesture with the ui origin implied by the session', async () => {
    const { ws } = await boot();
    const vol = document.querySelector<HTMLElement>('.tcp .vol')!;
    vol.getBoundingClientRect = () => ({ left: 0, width: 100, top: 0, height: 4, right: 100, bottom: 4, x: 0, y: 0, toJSON: () => ({}) });
    vol.setPointerCapture = () => undefined;
    vol.dispatchEvent(new MouseEvent('pointerdown', { clientX: 75, bubbles: true }));
    vol.dispatchEvent(new MouseEvent('pointerup', { clientX: 75, bubbles: true }));
    await tick(100);
    const sent = ws.sent.filter((m) => m.name === 'track.update');
    expect(sent.length).toBeGreaterThan(0);
    expect(sent.every((m) => m.coalesce_key === 'vol:trk_a')).toBe(true);
    expect(sent[sent.length - 1]!.params.volume_db).toBeCloseTo(0, 1);
  });

  it('offline: a banner, and the last state stays on screen', async () => {
    const { ws } = await boot();
    ws.close();
    await tick(100);
    expect(document.querySelector('.banner')!.textContent).toMatch(/перепідключення|Підключення/);
    expect(document.querySelectorAll('.tcp')).toHaveLength(2);
  });
});

// Real 2D drawing through node-canvas: exercises the canvas code of the arrange view and the mixer, and writes PNGs when ROVOX_PNG_DIR is set.
describe.skipIf(!hasCanvas)('canvas drawing', () => {
  it('draws lanes, items with waveforms, fades, the playhead, the agent cursor and the meters without errors', async () => {
    const tokens = JSON.parse(readFileSync(`${process.cwd()}/design/tokens.json`, 'utf8'));
    for (const t of tokens.color.tokens) document.documentElement.style.setProperty(t.name.startsWith('rx-') ? `--${t.name}` : `--rx-${t.name}`, t.value.dark);
    Object.defineProperty(HTMLCanvasElement.prototype, 'clientWidth', { configurable: true, get: () => 900 });
    Object.defineProperty(HTMLCanvasElement.prototype, 'clientHeight', { configurable: true, get: () => 300 });

    vi.stubGlobal('fetch', vi.fn(async (url: string) => {
      if (String(url).includes('/peaks/')) {
        const bins = Number(/bins=(\d+)/.exec(String(url))![1]);
        const data = new Int8Array(bins * 2);
        for (let i = 0; i < bins; i++) {
          const a = Math.round(100 * Math.abs(Math.sin(i / 17)) * (0.4 + 0.6 * Math.abs(Math.sin(i / 90))));
          data[i * 2] = -a;
          data[i * 2 + 1] = a;
        }
        return { ok: true, headers: { get: (k: string) => (k === 'X-Rovox-T0' ? '0' : k === 'X-Rovox-T1' ? '6' : null) }, arrayBuffer: async () => data.buffer };
      }
      return { ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) };
    }));

    project.items.push({ item_id: 'itm_2', track_id: 'trk_b', name: 'bass_take3.wav', start_s: 0, length_s: 28, end_s: 28, muted: false, offset_s: 0, fade_in_s: 1, fade_out_s: 2, gain_db: -4 } as never);
    (project as any).markers = [{ marker_id: 'mrk_1', name: 'Verse', time_s: 2 }];
    const { app, ws } = await boot();
    const p = app.active!;
    p.view.pps = 28;
    p.selection.add('itm_2');
    p.transport = { ...p.transport, state: 'playing', position_s: 7.5, loop: { enabled: true, start_s: 4, end_s: 12 } };
    p.lastPositionAt = performance.now();
    p.setMeter('trk_a', -9, -10, performance.now());
    p.setMeter('trk_b', -2, -4, performance.now());
    p.setMeter('master', -5, -6, performance.now());
    ws.push({ t: 'event', type: 'project.changed', project_id: 'p_1', revision: 12, origin: { kind: 'mcp', session: 'mcp_1', label: 'Claude' }, undo_label: 'Claude: item.split flute_solo.wav', changes: [{ kind: 'item', id: 'itm_1', op: 'update' }] });
    project.items.pop();
    await tick(400);

    const canvases = [...document.querySelectorAll('canvas')];
    expect(canvases.length).toBeGreaterThanOrEqual(5);
    const dir = process.env.ROVOX_PNG_DIR;
    if (dir) {
      mkdirSync(dir, { recursive: true });
      const lanes = document.querySelector<HTMLCanvasElement>('.lanes canvas')!;
      writeFileSync(`${dir}/lanes.png`, Buffer.from(lanes.toDataURL().split(',')[1]!, 'base64'));
      writeFileSync(`${dir}/ruler.png`, Buffer.from(document.querySelector<HTMLCanvasElement>('.ruler canvas')!.toDataURL().split(',')[1]!, 'base64'));
      const strip = document.querySelectorAll<HTMLCanvasElement>('.strip canvas');
      strip.forEach((c, i) => writeFileSync(`${dir}/strip${i}.png`, Buffer.from(c.toDataURL().split(',')[1]!, 'base64')));
    }
    // gestures on the lanes become one batch of item.update commands
    const canvas = document.querySelector<HTMLCanvasElement>('.lanes canvas')!;
    canvas.setPointerCapture = () => undefined;
    const fire = (type: string, x: number, y: number) => canvas.dispatchEvent(new MouseEvent(type, { clientX: x, clientY: y, bubbles: true }));
    const batches = () => ws.sent.filter((m) => m.ops);
    p.view.start = 0;
    p.view.pps = 28; // item 1 spans x = 112 .. 280 on the first lane
    p.selection.clear();
    fire('pointerdown', 150, 40);
    fire('pointermove', 150 + 56, 40); // two seconds later
    fire('pointerup', 150 + 56, 40);
    await tick(100);
    expect(batches()).toHaveLength(1);
    const move = batches()[0]!.ops[0];
    expect(move.name).toBe('item.update');
    expect(move.params.item_id).toBe('itm_1');
    expect(Math.abs(move.params.start_s - 6)).toBeLessThan(0.14); // snapped to the 1/16 grid at 118 bpm
    expect(p.items.get('itm_1')!.start_s).toBeCloseTo(move.params.start_s, 6); // optimistic: the model already has it

    const end = p.items.get('itm_1')!.end_s;
    const rightEdge = end * 28 - 2;
    fire('pointerdown', rightEdge, 40);
    fire('pointermove', rightEdge + 28, 40);
    fire('pointerup', rightEdge + 28, 40);
    await tick(100);
    expect(batches()).toHaveLength(2);
    expect(batches()[1]!.ops[0].params.trim_end_s).toBeGreaterThan(end);
    expect(batches()[1]!.ops[0].params.item_id).toBe('itm_1');

    // a plain click on empty lane space seeks
    p.selection.clear();
    fire('pointerdown', 700, 200);
    fire('pointerup', 700, 200);
    await tick(80);
    expect(ws.sent.some((m) => m.name === 'transport.set' && typeof m.params.position_s === 'number')).toBe(true);

    // something was drawn on the lanes (not a blank canvas)
    const lanes = document.querySelector<HTMLCanvasElement>('.lanes canvas')!;
    const px = lanes.getContext('2d')!.getImageData(0, 0, 900, 300).data;
    let colored = 0;
    for (let i = 0; i < px.length; i += 4) if (px[i]! > 40 || px[i + 1]! > 40 || px[i + 2]! > 40) colored++;
    expect(colored).toBeGreaterThan(1000);
  });
});
