// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// Keyboard and screen reader access of the window: volume sliders, menus, tabs, activity rows, toasts, arrange items.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setLocale } from '../src/i18n';

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
  can_undo: true,
  can_redo: false,
  dirty: false,
  tracks: [
    { track_id: 'trk_a', index: 0, name: 'Flute', color: '#4cc9f0', mute: false, solo: false, volume_db: -3, pan: 0 },
    { track_id: 'trk_b', index: 1, name: 'Bass DI', color: '#ffb627', mute: false, solo: false, volume_db: 0, pan: 0 },
  ],
  master: { track_id: 'master', volume_db: 0, pan: 0 },
  items: [{ item_id: 'itm_1', track_id: 'trk_a', name: 'flute_solo.wav', start_s: 4, length_s: 6, end_s: 10, muted: false, offset_s: 0 }],
  markers: [],
  tempo: { points: [{ bpm: 118, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } },
  fx: { trk_a: [], trk_b: [] },
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
      case 'history.list':
        return reply({ undo: [], redo: [] });
      case 'project.activity':
        return reply({ activity: [] });
      case 'project.mix': {
        const params = (msg.params ?? {}) as Record<string, unknown>;
        if (typeof params.muted === 'boolean') mix.muted = params.muted;
        return reply({ project_id: 'p_1', gain_db: 0, muted: mix.muted, solo: false });
      }
      default:
        return reply({});
    }
  }
}

const mix = { muted: false };

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));

beforeEach(() => {
  setLocale('uk'); // the tests assert the Ukrainian texts, whatever navigator.language says
  FakeWebSocket.instances = [];
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) })));
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
  mix.muted = false;
  document.body.innerHTML = '';
  localStorage.clear();
});

afterEach(() => {
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

const key = (el: Element, k: string, init: KeyboardEventInit = {}) => el.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...init }));

function volumeSent(ws: FakeWebSocket, trackId: string) {
  return ws.sent.filter((m) => m.name === 'track.update' && m.params.track_id === trackId && m.params.volume_db !== undefined);
}

describe('volume sliders', () => {
  it('the TCP volume is a labelled slider and the keys send one track.update each', async () => {
    const { ws } = await boot();
    const vol = document.querySelector<HTMLElement>('.tcp .vol')!;
    expect(vol.getAttribute('role')).toBe('slider');
    expect(vol.tabIndex).toBe(0);
    expect(vol.getAttribute('aria-label')).toBe('Гучність: Flute');
    expect(vol.getAttribute('aria-valuemax')).toBe('12');
    expect(vol.getAttribute('aria-valuenow')).toBe('-3');
    expect(vol.getAttribute('aria-valuetext')).toBe('-3.0 dB');

    key(vol, 'ArrowUp'); // -3 -> -2.5
    key(vol, 'ArrowDown', { shiftKey: true }); // -> -2.6
    key(vol, 'PageUp'); // -> 0.4
    key(vol, 'PageDown'); // -> -2.6
    key(vol, 'Home'); // -> minimum
    key(vol, 'End'); // -> +12
    key(vol, 'Delete'); // -> 0
    await tick(100);
    const sent = volumeSent(ws, 'trk_a');
    expect(sent.map((m) => m.params.volume_db)).toEqual([-2.5, -2.6, 0.4, -2.6, -100, 12, 0]);
    expect(sent.every((m) => m.coalesce_key === undefined)).toBe(true); // a single press is not a gesture
    expect(vol.getAttribute('aria-valuenow')).toBe('0');
    expect(vol.getAttribute('aria-valuetext')).toBe('0.0 dB');
  });

  it('a held key (auto-repeat) is coalesced into one gesture', async () => {
    const { ws } = await boot();
    const vol = document.querySelector<HTMLElement>('.tcp .vol')!;
    key(vol, 'ArrowUp', { repeat: true });
    key(vol, 'ArrowUp', { repeat: true });
    await tick(100);
    const sent = volumeSent(ws, 'trk_a');
    expect(sent.map((m) => m.params.volume_db)).toEqual([-2.5, -2]);
    expect(sent.every((m) => m.coalesce_key === 'vol:trk_a')).toBe(true);
  });

  it('a slider key does not reach the global shortcuts (Delete would delete the selection)', async () => {
    const { app, ws } = await boot();
    app.active!.selection.add('itm_1');
    key(document.querySelector('.tcp .vol')!, 'Delete');
    await tick(100);
    expect(app.active!.selection.has('itm_1')).toBe(true);
    expect(ws.sent.some((m) => m.ops)).toBe(false);
  });

  it('the mixer fader is a slider with the same keys', async () => {
    const { ws } = await boot();
    const fader = document.querySelectorAll<HTMLElement>('.strip canvas')[1]!; // Bass DI, 0 dB
    expect(fader.getAttribute('role')).toBe('slider');
    expect(fader.tabIndex).toBe(0);
    expect(fader.getAttribute('aria-label')).toBe('Гучність: Bass DI');
    key(fader, 'ArrowDown');
    key(fader, 'ArrowDown', { shiftKey: true });
    key(fader, 'Backspace');
    await tick(100);
    expect(volumeSent(ws, 'trk_b').map((m) => m.params.volume_db)).toEqual([-0.5, -0.6, 0]);
    expect(fader.getAttribute('aria-valuetext')).toBe('0.0 dB');
    const master = document.querySelector<HTMLElement>('.strip.master canvas')!;
    expect(master.getAttribute('aria-label')).toBe('Гучність: МАСТЕР');
  });
});

describe('menus', () => {
  const triggers = () => [...document.querySelectorAll<HTMLButtonElement>('.menubar .menu > button')];
  const open = () => document.querySelector('.menubar .menu.open > button')?.textContent;

  it('opens with the keyboard, moves with the arrows and closes with Escape returning the focus', async () => {
    await boot();
    const file = triggers()[0]!;
    expect(file.getAttribute('aria-haspopup')).toBe('menu');
    expect(file.getAttribute('aria-expanded')).toBe('false');
    expect(file.nextElementSibling!.getAttribute('role')).toBe('menu');

    key(file, 'ArrowDown');
    expect(file.getAttribute('aria-expanded')).toBe('true');
    const items = [...file.nextElementSibling!.querySelectorAll<HTMLButtonElement>('[role=menuitem]:not(:disabled)')];
    expect(items.length).toBeGreaterThan(2);
    expect(document.activeElement).toBe(items[0]);
    key(document.activeElement!, 'ArrowDown');
    expect(document.activeElement).toBe(items[1]);
    key(document.activeElement!, 'End');
    expect(document.activeElement).toBe(items[items.length - 1]);
    key(document.activeElement!, 'ArrowDown'); // wraps
    expect(document.activeElement).toBe(items[0]);
    key(document.activeElement!, 'ArrowUp'); // wraps back
    expect(document.activeElement).toBe(items[items.length - 1]);
    key(document.activeElement!, 'Home');
    expect(document.activeElement).toBe(items[0]);

    key(document.activeElement!, 'Escape');
    expect(file.getAttribute('aria-expanded')).toBe('false');
    expect(open()).toBeUndefined();
    expect(document.activeElement).toBe(file);
  });

  it('ArrowLeft / ArrowRight switch to the neighbouring menu', async () => {
    await boot();
    const [file, edit] = triggers();
    key(file!, 'ArrowDown');
    key(document.activeElement!, 'ArrowRight');
    expect(open()).toBe(edit!.textContent);
    expect(edit!.getAttribute('aria-expanded')).toBe('true');
    expect(file!.getAttribute('aria-expanded')).toBe('false');
    key(document.activeElement!, 'ArrowLeft');
    expect(open()).toBe(file!.textContent);
    key(document.activeElement!, 'Escape');
    // on a closed menu button the arrows only move the focus
    key(file!, 'ArrowRight');
    expect(document.activeElement).toBe(edit);
    expect(open()).toBeUndefined();
  });

  it('Enter on an item runs it, closes the menu and returns the focus to the button', async () => {
    const { app } = await boot();
    const toggleMixer = vi.spyOn(app, 'updatePrefs');
    const view = triggers()[2]!; // View: the first item toggles the mixer
    key(view, 'ArrowDown');
    const first = document.activeElement as HTMLButtonElement;
    first.click(); // Enter / Space on a button is a click
    expect(toggleMixer).toHaveBeenCalled();
    expect(open()).toBeUndefined();
    expect(document.activeElement).toBe(view);
  });

  it('a click outside closes the open menu', async () => {
    await boot();
    const file = triggers()[0]!;
    file.click();
    expect(file.getAttribute('aria-expanded')).toBe('true');
    document.body.dispatchEvent(new MouseEvent('pointerdown', { bubbles: true }));
    expect(file.getAttribute('aria-expanded')).toBe('false');
  });
});

describe('project tabs', () => {
  async function twoTabs() {
    const b = await boot();
    const { ProjectStore } = await import('../src/store/project');
    b.app.projects.set('p_2', new ProjectStore('p_2'));
    b.app.order = ['p_1', 'p_2'];
    b.app.changed.emit();
    await tick(80);
    return b;
  }
  const tabs = () => [...document.querySelectorAll<HTMLElement>('[role=tab]')];

  it('is a tablist with a roving tabindex: the arrows move the focus, Enter activates', async () => {
    const { app } = await twoTabs();
    expect(document.querySelector('.tabs')!.getAttribute('role')).toBe('tablist');
    expect(tabs().map((t) => t.tabIndex)).toEqual([0, -1]);
    tabs()[0]!.focus();

    key(tabs()[0]!, 'ArrowRight');
    expect(document.activeElement).toBe(tabs()[1]);
    expect(tabs().map((t) => t.tabIndex)).toEqual([-1, 0]);
    key(tabs()[1]!, 'ArrowRight'); // wraps
    expect(document.activeElement).toBe(tabs()[0]);
    key(tabs()[0]!, 'End');
    expect(document.activeElement).toBe(tabs()[1]);
    key(tabs()[1]!, 'Home');
    expect(document.activeElement).toBe(tabs()[0]);
    key(tabs()[0]!, 'ArrowLeft');
    expect(document.activeElement).toBe(tabs()[1]);

    const setActive = vi.spyOn(app, 'setActive').mockImplementation(() => undefined);
    key(tabs()[1]!, 'Enter');
    expect(setActive).toHaveBeenCalledWith('p_2');
    key(tabs()[1]!, ' ');
    expect(setActive).toHaveBeenCalledTimes(2);
  });

  it('the mute and close buttons are separate controls outside the tab, the mute one is a switch', async () => {
    await twoTabs();
    for (const item of document.querySelectorAll('.tabitem')) {
      const tab = item.querySelector('[role=tab]')!;
      const mute = item.querySelector<HTMLElement>('.mute')!;
      const close = item.querySelector<HTMLElement>('.close')!;
      expect(tab.contains(mute) || tab.contains(close)).toBe(false);
      expect(mute.tabIndex).toBe(0);
      expect(close.tabIndex).toBe(0);
      expect(mute.getAttribute('role')).toBe('switch');
      expect(mute.getAttribute('aria-checked')).toBe('true');
    }
  });

  it('keeps the keyboard focus on the same control when the tabs are rebuilt', async () => {
    const { app } = await boot();
    const mute = document.querySelector<HTMLButtonElement>('.tabitem .mute')!;
    mute.focus();
    mute.click();
    await tick(150);
    expect(app.projects.get('p_1')!.muted).toBe(true);
    const now = document.querySelector<HTMLButtonElement>('.tabitem .mute')!;
    expect(now.getAttribute('aria-checked')).toBe('false');
    expect(document.activeElement).toBe(now);
  });
});

describe('activity log', () => {
  it('rows are focusable buttons that Enter and Space activate', async () => {
    const { shell, ws } = await boot();
    ws.push({
      t: 'event',
      type: 'project.changed',
      project_id: 'p_1',
      revision: 12,
      origin: { kind: 'mcp', session: 'mcp_1', label: 'Claude' },
      undo_label: 'Claude: item.split flute_solo.wav',
      changes: [{ kind: 'item', id: 'itm_1', op: 'update', fields: ['length'] }],
    });
    await tick(300);
    const row = document.querySelector<HTMLElement>('.log-row')!;
    expect(row.getAttribute('role')).toBe('button');
    expect(row.tabIndex).toBe(0);
    const reveal = vi.spyOn(shell.arrangeView, 'reveal');
    key(row, 'Enter');
    key(row, ' ');
    key(row, 'a');
    expect(reveal).toHaveBeenCalledTimes(2);
  });
});

describe('toasts', () => {
  it('are a polite live region; errors are alerts; a new toast does not rebuild the old ones', async () => {
    const { app } = await boot();
    const box = document.querySelector('.toasts')!;
    expect(box.getAttribute('role')).toBe('status');
    expect(box.getAttribute('aria-live')).toBe('polite');
    app.toast('збережено');
    await tick(100);
    const first = box.querySelector('.toast')!;
    expect(first.getAttribute('role')).toBeNull();
    app.toast('помилка', 'error');
    await tick(100);
    expect(box.querySelectorAll('.toast')).toHaveLength(2);
    expect(box.querySelector('.toast')).toBe(first);
    expect(box.querySelector('.toast.error')!.getAttribute('role')).toBe('alert');
  });
});

describe('arrange items by keyboard', () => {
  it('Tab reaches the lanes, the arrows select items and the selection is announced', async () => {
    const { app } = await boot();
    const lanes = document.querySelector<HTMLCanvasElement>('.lanes canvas')!;
    expect(lanes.tabIndex).toBe(0);
    expect(lanes.getAttribute('role')).toBe('application');
    expect(lanes.getAttribute('aria-label')).toBe('Аранжування');
    expect(document.getElementById(lanes.getAttribute('aria-describedby')!)).not.toBeNull();
    const live = document.querySelector('.arrange [aria-live=polite]')!;

    key(lanes, 'ArrowRight');
    const p = app.active!;
    expect([...p.selection]).toEqual(['itm_1']);
    expect(p.selectedTrack).toBe('trk_a');
    expect(live.textContent).toContain('flute_solo.wav');
    expect(live.textContent).toContain('доріжка Flute');

    key(lanes, 'ArrowRight'); // the last item of the track: the selection stays
    expect([...p.selection]).toEqual(['itm_1']);
    key(lanes, 'ArrowDown'); // the next track has no items: the selection stays
    expect([...p.selection]).toEqual(['itm_1']);
  });
});
