// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The FX popover: expand a plugin, search its parameters, edit through fx.update, reset on double-click, toggle enabled.
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
  tracks: [{ track_id: 'trk_a', index: 0, name: 'Vocals', color: '#4cc9f0', mute: false, solo: false, volume_db: 0, pan: 0 }],
  master: { track_id: 'master', volume_db: 0, pan: 0 },
  items: [],
  markers: [],
  tempo: { points: [{ bpm: 120, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } },
  fx: {
    trk_a: [
      { plugin_id: 'fx_eq', name: 'Rovox EQ', type: 'eq', enabled: true },
      { plugin_id: 'fx_comp', name: 'Rovox Comp', type: 'comp', enabled: true },
    ],
  },
};
const transport = { state: 'stopped', position_s: 0, epoch: 0, loop: { enabled: false, start_s: 0, end_s: 0 }, length_s: 30, audio: 'browser-stream' };

const params = Array.from({ length: 15 }, (_, i) => ({
  ident: `p${i}`,
  name: i === 0 ? 'Band 1 gain' : `Param ${i}`,
  value: 2.5,
  default: 0,
  min: -30,
  max: 30,
  text: '+2.5 dB',
  display: '+2.5 dB',
  min_text: '-30 dB',
  max_text: '30 dB',
}));

// discrete parameters with named states (value = min + index)
const choiceParams = [
  { ident: 'on', name: 'Enabled', value: 1, default: 1, min: 0, max: 1, text: 'on', display: 'on', choices: ['off', 'on'] },
  { ident: 'mode', name: 'Mode', value: 0, default: 0, min: 0, max: 2, text: 'peak', display: 'peak', choices: ['peak', 'rms', 'feedforward'] },
  { ident: 'ratio', name: 'Ratio', value: 4, default: 2, min: 1, max: 20, text: '4.0', display: '4.0' },
];

function answer(msg: Record<string, any>, ws: FakeWebSocket): void {
  if (msg.t === 'hello') {
    ws.push({ t: 'welcome', session: 'ws_1', server_version: '0.1.0', audio: { mode: 'browser-stream', available: true }, source_url: 'https://example.invalid/src' });
  } else if (msg.t === 'sub') {
    ws.push({ t: 'snapshot', project_id: 'p_1', pidx: 1, revision: 10, state: project, transport, meter_index: ['master', 'trk_a'], accepted_revision: 0 });
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
      case 'fx.params':
        return reply({ fx: { plugin_id: msg.params.plugin_id, params: msg.params.plugin_id === 'fx_comp' ? choiceParams : params } });
      case 'fx.update': {
        const key = Object.keys(msg.params.params ?? {})[0];
        const out = params.map((p) => (p.ident === key ? { ...p, value: msg.params.params[key], text: `${msg.params.params[key]} dB` } : p));
        return reply({ fx: { plugin_id: msg.params.plugin_id, params: out } });
      }
      default:
        return reply({});
    }
  }
}

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));
const sentCmds = (ws: FakeWebSocket, name: string) => ws.sent.filter((m) => m.t === 'cmd' && m.name === name);

beforeEach(() => {
  setLocale('uk'); // the tests assert the Ukrainian texts, whatever navigator.language says
  FakeWebSocket.instances = [];
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) })));
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
  document.body.innerHTML = '';
  localStorage.clear();
});

afterEach(() => {
  vi.unstubAllGlobals();
});

async function openFx() {
  const { App } = await import('../src/store/session');
  const { Shell } = await import('../src/views/shell');
  const app = new App('ws://test/ws');
  const shell = new Shell(app);
  document.body.append(shell.el);
  await app.start();
  await tick(350);
  (document.querySelector('.tcp .sbtn.fx') as HTMLElement).click();
  const ws = FakeWebSocket.instances[0]!;
  const heads = [...document.querySelectorAll<HTMLElement>('.popover .fxname')];
  return { ws, heads };
}

async function expandFirst() {
  const o = await openFx();
  o.heads[0]!.click();
  await tick();
  return o;
}

describe('FX popover', () => {
  it('lists plugins collapsed and fetches fx.params on expand', async () => {
    const { ws, heads } = await openFx();
    expect(heads).toHaveLength(2);
    expect(heads[0]!.getAttribute('aria-expanded')).toBe('false');
    expect(sentCmds(ws, 'fx.params')).toHaveLength(0);
    heads[0]!.click();
    await tick();
    expect(heads[0]!.getAttribute('aria-expanded')).toBe('true');
    expect(sentCmds(ws, 'fx.params')[0]!.params).toMatchObject({ plugin_id: 'fx_eq' });
    expect(document.querySelectorAll('.popover .fxparam')).toHaveLength(12);
    const first = document.querySelector('.popover .fxparam')!;
    expect(first.querySelector('.fxval')!.textContent).toBe('+2.5 dB');
    expect(first.querySelector('input')!.getAttribute('aria-label')).toBe('Rovox EQ — Band 1 gain');
  });

  it('reveals the rest with the "ще N" button', async () => {
    await expandFirst();
    const more = document.querySelector('.popover .fxmore') as HTMLElement;
    expect(more.textContent).toBe('… ще 3');
    more.click();
    expect(document.querySelectorAll('.popover .fxparam')).toHaveLength(15);
    expect(document.querySelector('.popover .fxmore')).toBeNull();
  });

  it('filters by name, case-insensitively', async () => {
    await expandFirst();
    const search = document.querySelector('.popover .fxsearch') as HTMLInputElement;
    search.value = 'BAND 1';
    search.dispatchEvent(new Event('input'));
    expect(document.querySelectorAll('.popover .fxparam')).toHaveLength(1);
  });

  it('sends fx.update with the param id and value on change and shows the server text', async () => {
    const { ws } = await expandFirst();
    const range = document.querySelector('.popover .fxrange') as HTMLInputElement;
    range.value = '5';
    range.dispatchEvent(new Event('change'));
    await tick();
    const upd = sentCmds(ws, 'fx.update');
    expect(upd.at(-1)!.params).toEqual({ plugin_id: 'fx_eq', params: { p0: 5 } });
    expect(document.querySelector('.popover .fxval')!.textContent).toBe('5 dB');
  });

  it('resets to the default on double-click', async () => {
    const { ws } = await expandFirst();
    document.querySelector('.popover .fxparam')!.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
    await tick();
    expect(sentCmds(ws, 'fx.update').at(-1)!.params).toEqual({ plugin_id: 'fx_eq', params: { p0: 0 } });
  });

  it('sends a key press once (input, then the equal change) and the final change keeps the coalesce key', async () => {
    const { ws } = await expandFirst();
    const range = document.querySelector('.popover .fxrange') as HTMLInputElement;
    range.value = '5';
    range.dispatchEvent(new Event('input'));
    range.dispatchEvent(new Event('change'));
    await tick();
    expect(sentCmds(ws, 'fx.update')).toHaveLength(1);
    range.value = '7';
    range.dispatchEvent(new Event('change'));
    await tick();
    const upd = sentCmds(ws, 'fx.update');
    expect(upd).toHaveLength(2);
    expect(upd[1]!.coalesce_key).toBe(upd[0]!.coalesce_key);
    expect(upd[0]!.coalesce_key).toBe('fx:fx_eq:p0');
  });

  it('resets a focused slider to the plugin default with Delete', async () => {
    const { ws } = await expandFirst();
    const range = document.querySelector('.popover .fxrange') as HTMLInputElement;
    expect(range.title).toContain('Delete');
    range.dispatchEvent(new KeyboardEvent('keydown', { key: 'Delete', bubbles: true }));
    await tick();
    expect(sentCmds(ws, 'fx.update').at(-1)!.params).toEqual({ plugin_id: 'fx_eq', params: { p0: 0 } });
  });

  it('has no reset for a parameter without a plugin default', async () => {
    const saved = (params[0] as { default?: number }).default;
    delete (params[0] as { default?: number }).default;
    try {
      const { ws } = await expandFirst();
      const range = document.querySelector('.popover .fxrange') as HTMLInputElement;
      expect(range.title).not.toContain('Delete');
      range.dispatchEvent(new KeyboardEvent('keydown', { key: 'Delete', bubbles: true }));
      document.querySelector('.popover .fxparam')!.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
      await tick();
      expect(sentCmds(ws, 'fx.update')).toHaveLength(0);
    } finally {
      (params[0] as { default?: number }).default = saved;
    }
  });

  it('toggles enabled', async () => {
    const { ws } = await openFx();
    (document.querySelector('.popover .fxtoggle') as HTMLElement).click();
    await tick();
    expect(sentCmds(ws, 'fx.update').at(-1)!.params).toEqual({ plugin_id: 'fx_eq', enabled: false });
    expect(document.querySelector('.popover .fxtoggle')!.textContent).toBe('вимк.');
  });
  it('shows named states as a switch or a select instead of a slider and sends value = min + index', async () => {
    const { ws, heads } = await openFx();
    heads[1]!.click();
    await tick();
    const rows = [...document.querySelectorAll<HTMLElement>('.popover .fxparam')];
    expect(rows).toHaveLength(3);
    const sw = rows[0]!.querySelector<HTMLButtonElement>('button.fxswitch')!;
    expect(rows[0]!.querySelector('input[type=range]')).toBeNull();
    expect(sw.getAttribute('aria-pressed')).toBe('true');
    sw.click();
    await tick();
    expect(sentCmds(ws, 'fx.update').at(-1)!.params).toMatchObject({ plugin_id: 'fx_comp', params: { on: 0 } });
    expect(sw.getAttribute('aria-pressed')).toBe('false');
    expect(sw.textContent).toBe('вимк.');

    const sel = rows[1]!.querySelector<HTMLSelectElement>('select.fxselect')!;
    expect([...sel.options].map((o) => o.textContent)).toEqual(['peak', 'rms', 'feedforward']);
    sel.value = '2';
    sel.dispatchEvent(new Event('change'));
    await tick();
    expect(sentCmds(ws, 'fx.update').at(-1)!.params).toMatchObject({ params: { mode: 2 } });

    expect(rows[2]!.querySelector('input[type=range]')).not.toBeNull(); // a plain number stays a slider
  });
});
