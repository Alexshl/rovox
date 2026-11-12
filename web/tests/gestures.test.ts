// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// Gesture robustness: shift-drag on a selected item, Escape while renaming, pointercancel, a fader that ignores outside updates
// while dragged, one pan command per frame, and the FX popover that closes on Escape without leaking listeners.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

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
    setTimeout(() => answer(msg, this), 0);
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
  can_undo: false,
  can_redo: false,
  dirty: false,
  tracks: [
    { track_id: 'trk_a', index: 0, name: 'Flute', color: '#4cc9f0', mute: false, solo: false, volume_db: -3, pan: 0 },
    { track_id: 'trk_b', index: 1, name: 'Bass DI', color: '#ffb627', mute: false, solo: false, volume_db: 0, pan: 0 },
  ],
  master: { track_id: 'master', volume_db: 0, pan: 0 },
  items: [{ item_id: 'itm_1', track_id: 'trk_a', name: 'flute_solo.wav', start_s: 4, length_s: 6, end_s: 10, muted: false, offset_s: 0 },
    { item_id: 'itm_2', track_id: 'trk_a', name: 'flute_two.wav', start_s: 12, length_s: 6, end_s: 18, muted: false, offset_s: 0 },
  ],
  markers: [],
  tempo: { points: [{ bpm: 118, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } },
  fx: { trk_a: [{ plugin_id: 'fx_rev', name: 'ReaVerb', type: 'reverb', enabled: true }], trk_b: [] },
};
const transport = { state: 'stopped', position_s: 0, epoch: 0, loop: { enabled: false, start_s: 0, end_s: 0 }, length_s: 30, audio: 'browser-stream' };

function answer(msg: Record<string, any>, ws: FakeWebSocket): void {
  if (msg.t === 'hello') {
    ws.push({ t: 'welcome', session: 'ws_1', server_version: '0.1.0', audio: { mode: 'browser-stream', available: true }, source_url: '' });
  } else if (msg.t === 'sub') {
    ws.push({ t: 'snapshot', project_id: 'p_1', pidx: 1, revision: 10, state: project, transport, meter_index: ['master', 'trk_a', 'trk_b'], accepted_revision: 0 });
  } else if (msg.t === 'cmd') {
    const reply = (data: unknown) => ws.push({ t: 'result', id: msg.id, ok: true, revision: 10, data });
    if (msg.name === 'project.list' || msg.name === 'project.catalog') return reply({ projects: [{ project_id: 'p_1', name: 'rahiv' }] });
    if (msg.name === 'project.get') return reply(project);
    if (msg.name === 'history.list') return reply({ undo: [], redo: [] });
    if (msg.name === 'project.activity') return reply({ activity: [] });
    reply({});
  }
}

const tick = (ms = 30) => new Promise((r) => setTimeout(r, ms));
let errors: unknown[] = [];
const onError = (e: ErrorEvent) => errors.push(e.error ?? e.message);

beforeEach(() => {
  FakeWebSocket.instances = [];
  errors = [];
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) })));
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
  Element.prototype.setPointerCapture = () => undefined;
  document.body.innerHTML = '';
  localStorage.clear();
  window.addEventListener('error', onError);
});

afterEach(() => {
  window.removeEventListener('error', onError);
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
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

function ptr(el: Element, type: string, init: { x?: number; y?: number; shift?: boolean } = {}): void {
  const ev = new MouseEvent(type, { bubbles: true, cancelable: true, button: 0, clientX: init.x ?? 0, clientY: init.y ?? 0, shiftKey: !!init.shift });
  Object.defineProperty(ev, 'pointerId', { value: 1 });
  el.dispatchEvent(ev);
}

const updates = (ws: FakeWebSocket, key: string) => ws.sent.filter((m) => m.t === 'cmd' && m.name === 'track.update' && key in (m.params ?? {}));

describe('arrange gestures', () => {
  it('shift-drag on an item that shift-click just took out of the selection does not throw or move anything', async () => {
    const { app } = await boot();
    const lanes = document.querySelector('.lanes canvas')!;
    ptr(lanes, 'pointerdown', { x: 200, y: 30 }); // itm_1 at 4 s * 40 px/s = 160..400
    ptr(lanes, 'pointerup', { x: 200, y: 30 });
    ptr(lanes, 'pointerdown', { x: 600, y: 30, shift: true }); // itm_2 at 12 s = 480..720 joins the selection
    ptr(lanes, 'pointerup', { x: 600, y: 30, shift: true });
    expect([...app.active!.selection].sort()).toEqual(['itm_1', 'itm_2']);
    ptr(lanes, 'pointerdown', { x: 200, y: 30, shift: true }); // itm_1 leaves: the anchor is missing from the dragged set
    for (let i = 1; i <= 5; i++) ptr(lanes, 'pointermove', { x: 200 + i * 10, y: 30, shift: true });
    ptr(lanes, 'pointerup', { x: 250, y: 30, shift: true });
    await tick();
    expect(errors).toEqual([]);
    expect([...app.active!.selection]).toEqual(['itm_2']);
    expect(app.active!.items.get('itm_2')!.start_s).toBe(12);
  });

  it('Escape while renaming cancels even when the blur that follows tries to commit', async () => {
    const { ws } = await boot();
    const name = document.querySelector('.tcp .name')!;
    name.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    const input = document.querySelector<HTMLInputElement>('.tcp input.name-input')!;
    input.value = 'Renamed';
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    input.dispatchEvent(new FocusEvent('blur'));
    await tick();
    expect(ws.sent.some((m) => m.name === 'track.update' && m.params.name === 'Renamed')).toBe(false);
    expect(document.querySelector('.tcp .name')!.textContent).toBe('Flute');
  });

  it('pointercancel on the TCP volume ends the drag and sends the final value', async () => {
    const { ws } = await boot();
    const vol = document.querySelector<HTMLElement>('.tcp .vol')!;
    vol.getBoundingClientRect = () => ({ left: 0, top: 0, width: 100, height: 10, right: 100, bottom: 10, x: 0, y: 0, toJSON: () => ({}) });
    ptr(vol, 'pointerdown', { x: 60 });
    ptr(vol, 'pointermove', { x: 70 });
    const before = updates(ws, 'volume_db').length;
    ptr(vol, 'pointercancel', { x: 70 });
    await tick();
    const after = updates(ws, 'volume_db');
    expect(after.length).toBeGreaterThan(before);
    ptr(vol, 'pointermove', { x: 90 }); // the gesture is over: no more commands
    await tick();
    expect(updates(ws, 'volume_db').length).toBe(after.length);
  });

  it('the FX popover closes on Escape, returns focus to the FX button and leaves no document listeners', async () => {
    await boot();
    const removed = vi.spyOn(document, 'removeEventListener');
    const fx = document.querySelector<HTMLButtonElement>('.tcp .sbtn.fx')!;
    fx.click();
    await tick();
    expect(document.querySelector('.popover')).not.toBeNull();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    expect(document.querySelector('.popover')).toBeNull();
    expect(document.activeElement).toBe(fx);
    expect(removed.mock.calls.some((c) => c[0] === 'pointerdown')).toBe(true);
    expect(removed.mock.calls.some((c) => c[0] === 'keydown')).toBe(true);
  });

  it('a click outside the FX popover closes it and removes its listeners', async () => {
    await boot();
    const removed = vi.spyOn(document, 'removeEventListener');
    document.querySelector<HTMLButtonElement>('.tcp .sbtn.fx')!.click();
    await tick();
    ptr(document.body, 'pointerdown');
    expect(document.querySelector('.popover')).toBeNull();
    expect(removed.mock.calls.filter((c) => c[0] === 'pointerdown' || c[0] === 'keydown').length).toBeGreaterThanOrEqual(2);
  });
});

describe('mixer gestures', () => {
  it('the fader keeps the dragged value when an outside update arrives mid-drag, and pointercancel sends the final one', async () => {
    const { app, ws } = await boot();
    const strip = document.querySelector('.strip')!;
    const canvas = strip.querySelector('canvas')!;
    canvas.getBoundingClientRect = () => ({ left: 0, top: 0, width: 60, height: 216, right: 60, bottom: 216, x: 0, y: 0, toJSON: () => ({}) });
    ptr(canvas, 'pointerdown', { y: 100 });
    ptr(canvas, 'pointermove', { y: 90 });
    await tick(60); // the readout follows the store on the next render
    const shown = strip.querySelector('.dbread')!.textContent;
    expect(shown).not.toBe('-40.0 dB');
    app.active!.patchTrack('trk_a', { volume_db: -40 }); // an agent moves the same fader
    await tick(60);
    expect(strip.querySelector('.dbread')!.textContent).toBe(shown);
    const before = updates(ws, 'volume_db').length;
    ptr(canvas, 'pointercancel', { y: 90 });
    await tick();
    expect(updates(ws, 'volume_db').length).toBe(before + 1);
  });

  it('rebuilds the FX rows when the same plugin is removed and added again under a new id', async () => {
    const { app } = await boot();
    const strip = document.querySelector('.strip')!;
    const row = strip.querySelector('.fxlist > div')!;
    expect(row.textContent).toBe('ReaVerb');
    app.active!.fx.set('trk_a', [{ plugin_id: 'fx_rev2', name: 'ReaVerb', type: 'reverb', enabled: true } as any]);
    app.active!.state.emit();
    await tick(60);
    const after = strip.querySelector('.fxlist > div')!;
    expect(after.textContent).toBe('ReaVerb');
    expect(after).not.toBe(row);
  });

  it('pan input sends one command per animation frame and the change event sends the exact final value', async () => {
    const { ws } = await boot();
    const pan = document.querySelector<HTMLInputElement>('.strip input.pan')!;
    const before = updates(ws, 'pan').length;
    for (let v = 10; v <= 50; v += 10) {
      pan.value = String(v);
      pan.dispatchEvent(new Event('input', { bubbles: true }));
    }
    expect(updates(ws, 'pan').length).toBe(before); // nothing yet: all inputs of the frame are merged
    pan.dispatchEvent(new Event('change', { bubbles: true }));
    await tick(60);
    const sent = updates(ws, 'pan');
    expect(sent.length).toBe(before + 1);
    expect(sent[sent.length - 1]!.params.pan).toBeCloseTo(0.5, 6);
  });
});
