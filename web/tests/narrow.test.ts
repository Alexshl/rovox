// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The narrow-screen layout: with matchMedia mocked for 390 px the menubar, transport, MCP drawer, agent bar and the TCP width follow
// the phone layout; at desktop width the markup is the one the other tests rely on.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setLocale, t } from '../src/i18n';

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
      default:
        return reply({});
    }
  }
}

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));

/** matchMedia for a window of `width` px: "(max-width: Npx)" matches when width <= N; `resize` re-evaluates and tells the listeners. */
function mockMedia(initial: number) {
  let width = initial;
  const queries: { q: string; mql: { matches: boolean }; fns: Set<() => void> }[] = [];
  const evaluate = (q: string) => {
    const m = /max-width:\s*([\d.]+)px/.exec(q);
    return m ? width <= Number(m[1]) : false;
  };
  const fn = (q: string) => {
    const entry = { q, mql: { matches: evaluate(q) }, fns: new Set<() => void>() };
    queries.push(entry);
    return {
      get matches() {
        return entry.mql.matches;
      },
      media: q,
      addEventListener: (_: string, f: () => void) => entry.fns.add(f),
      removeEventListener: (_: string, f: () => void) => entry.fns.delete(f),
    };
  };
  vi.stubGlobal('matchMedia', fn);
  (window as unknown as { matchMedia: typeof fn }).matchMedia = fn;
  return {
    resize(w: number) {
      width = w;
      for (const e of queries) {
        e.mql.matches = evaluate(e.q);
        for (const f of [...e.fns]) f();
      }
    },
  };
}

beforeEach(() => {
  setLocale('uk');
  FakeWebSocket.instances = [];
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) })));
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
  document.body.innerHTML = '';
  document.documentElement.removeAttribute('style');
  localStorage.clear();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  delete (window as unknown as { matchMedia?: unknown }).matchMedia;
});

async function boot() {
  const { App } = await import('../src/store/session');
  const { Shell } = await import('../src/views/shell');
  const app = new App('ws://test/ws');
  const shell = new Shell(app);
  document.body.append(shell.el);
  await app.start();
  await tick(350);
  return { app, shell };
}

const key = (el: Element, k: string, init: KeyboardEventInit = {}) => el.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...init }));
const down = (el: Element) => el.dispatchEvent(new Event('pointerdown', { bubbles: true }));
const q = <T extends Element = HTMLElement>(sel: string) => document.querySelector<T>(sel);

describe('phone layout (390 px)', () => {
  beforeEach(() => {
    mockMedia(390);
  });

  it('the menubar is the logo, the menu button and the agents button; no menus, no status text', async () => {
    await boot();
    const bar = q('.menubar')!;
    expect(bar.querySelector('.logo')).not.toBeNull();
    expect(bar.querySelector('.menu-btn')!.textContent).toBe('☰');
    expect(bar.querySelector('.agents-btn')!.textContent).toContain(t('narrow.agents'));
    expect(bar.querySelectorAll('.menu')).toHaveLength(0);
    expect(bar.querySelector('.status')).toBeNull();
  });

  it('the menu button opens one sheet with the menus as sections, usable from the keyboard', async () => {
    await boot();
    const btn = q<HTMLButtonElement>('.menu-btn')!;
    btn.focus();
    btn.click();
    const sheet = q('.sheet')!;
    expect(sheet).not.toBeNull();
    expect(btn.getAttribute('aria-expanded')).toBe('true');
    const headings = [...sheet.querySelectorAll('.menu-sheet .dropdown > .heading')].map((x) => x.textContent);
    expect(headings).toEqual([t('menu.file'), t('menu.edit'), t('menu.view'), t('menu.track'), t('menu.agents'), t('menu.help')]);
    const items = [...sheet.querySelectorAll<HTMLButtonElement>('button.item:not(:disabled)')];
    expect(items.length).toBeGreaterThan(6);
    expect(document.activeElement).toBe(items[0]);
    key(document.activeElement!, 'ArrowDown');
    expect(document.activeElement).toBe(items[1]);
    key(document.activeElement!, 'End');
    expect(document.activeElement).toBe(items[items.length - 1]);
    key(document.activeElement!, 'Home');
    expect(document.activeElement).toBe(items[0]);
    key(document.activeElement!, 'Escape');
    expect(q('.sheet')).toBeNull();
    expect(btn.getAttribute('aria-expanded')).toBe('false');
    expect(document.activeElement).toBe(btn);
  });

  it('choosing an item in the sheet closes it and runs the action', async () => {
    const { app } = await boot();
    const save = vi.spyOn(app, 'cmd').mockResolvedValue(undefined as never);
    q<HTMLButtonElement>('.menu-btn')!.click();
    const item = [...document.querySelectorAll<HTMLButtonElement>('.sheet button.item')].find((b) => b.textContent?.startsWith(t('menu.file.save')))!;
    item.click();
    expect(q('.sheet')).toBeNull();
    expect(save).toHaveBeenCalledWith('project.save');
  });

  it('the agents button opens the MCP panel as a drawer: focus goes in, Escape and the scrim close it and focus returns', async () => {
    await boot();
    const btn = q<HTMLButtonElement>('.agents-btn')!;
    const mcp = q('.mcp-panel')!;
    btn.focus();
    btn.click();
    expect(mcp.classList.contains('open')).toBe(true);
    expect(mcp.contains(document.activeElement)).toBe(true);
    expect(q('.drawer-scrim')).not.toBeNull();
    expect(btn.getAttribute('aria-expanded')).toBe('true');
    key(document.activeElement!, 'Escape');
    expect(mcp.classList.contains('open')).toBe(false);
    expect(q('.drawer-scrim')).toBeNull();
    expect(document.activeElement).toBe(btn);

    btn.click();
    expect(mcp.classList.contains('open')).toBe(true);
    down(q('.drawer-scrim')!);
    expect(mcp.classList.contains('open')).toBe(false);
    expect(document.activeElement).toBe(btn);
  });

  it('the agents button shows the cyan dot while an agent is active', async () => {
    const { app } = await boot();
    const btn = q('.agents-btn')!;
    expect(btn.classList.contains('active')).toBe(false);
    app.active!.log.unshift({ at: Date.now(), revision: 11, tool: 'item.split', text: 'split', ids: [], origin: { kind: 'mcp', session: 's1', label: 'Claude' } } as never);
    app.changed.emit();
    await tick();
    expect(btn.classList.contains('active')).toBe(true);
  });

  it('the transport has two rows and the more popover holds Render, grid and snap', async () => {
    await boot();
    const rows = document.querySelectorAll('.transport .trow');
    expect(rows).toHaveLength(2);
    expect(rows[0]!.querySelector('.clock')).not.toBeNull();
    expect(rows[1]!.textContent).toContain('118.00');
    expect(rows[1]!.querySelector('.more-btn')).not.toBeNull();
    expect(q('.transport select')).toBeNull(); // the grid selector is not in the bar

    const more = q<HTMLButtonElement>('.more-btn')!;
    more.focus();
    more.click();
    const pop = q('.more-pop')!;
    expect(pop).not.toBeNull();
    expect(pop.textContent).toContain(t('transport.render'));
    expect(pop.querySelector('select')).not.toBeNull();
    expect(pop.querySelector('button.toggle')).not.toBeNull();
    key(pop, 'Escape');
    expect(q('.more-pop')).toBeNull();
    expect(document.activeElement).toBe(more);
  });

  it('the agent bar appears while the agent has unaccepted changes and calls Undo-agent and Accept', async () => {
    const { app } = await boot();
    const p = app.active!;
    expect(q('.agent-bar')).toBeNull();
    const origin = { kind: 'mcp', session: 's1', label: 'Claude' } as const;
    p.marks.set('trk_a', { session: 's1', label: 'Claude', at: Date.now(), revision: 11, undoLabel: 'Claude: track.update', op: 'update' });
    p.marks.set('itm_1', { session: 's1', label: 'Claude', at: Date.now(), revision: 11, undoLabel: 'Claude: item.split', op: 'update' });
    p.history = [{ revision: 11, label: 'Claude: track.update', time: '', origin }];
    app.changed.emit();
    await tick();
    const bar = q('.agent-bar')!;
    expect(bar).not.toBeNull();
    expect(bar.textContent).toContain('Агент змінив 1 доріжку');
    expect(document.documentElement.style.getPropertyValue('--rx-bar-h')).not.toBe('');

    const undo = vi.spyOn(app, 'undoAgent').mockResolvedValue(undefined);
    const accept = vi.spyOn(app, 'acceptAgent').mockResolvedValue(undefined);
    const [undoBtn, acceptBtn] = [...bar.querySelectorAll<HTMLButtonElement>('button')];
    expect(undoBtn!.textContent).toBe(t('narrow.agentUndo'));
    expect(acceptBtn!.textContent).toBe(t('narrow.agentAccept'));
    undoBtn!.click();
    acceptBtn!.click();
    expect(undo).toHaveBeenCalledTimes(1);
    expect(accept).toHaveBeenCalledTimes(1);

    p.marks.clear();
    app.changed.emit();
    await tick();
    expect(q('.agent-bar')).toBeNull();
  });

  it('the track FX opens from a tap on the name as a bottom sheet', async () => {
    await boot();
    q('.tcp .name')!.click();
    const sheet = q('.sheet')!;
    expect(sheet).not.toBeNull();
    expect(sheet.querySelector('.popover .head')!.textContent).toContain('Flute');
    key(document.activeElement ?? sheet, 'Escape');
    expect(q('.sheet')).toBeNull();
  });

  it('the mixer is a bottom sheet that starts peeking and toggles from its handle; the closed toggle still hides it', async () => {
    const { app } = await boot();
    const mixer = q('.mixer')!;
    expect(mixer.classList.contains('sheet')).toBe(true);
    expect(mixer.classList.contains('peek')).toBe(true);
    const handle = mixer.querySelector<HTMLButtonElement>('.sheet-handle')!;
    expect(handle.getAttribute('aria-expanded')).toBe('false');
    handle.click();
    expect(mixer.classList.contains('peek')).toBe(false);
    expect(handle.getAttribute('aria-expanded')).toBe('true');
    expect(document.documentElement.style.getPropertyValue('--rx-sheet-h')).toBe('60dvh');
    handle.click();
    expect(mixer.classList.contains('peek')).toBe(true);
    app.updatePrefs({ mixerOpen: false });
    await tick();
    expect(mixer.classList.contains('closed')).toBe(true);
  });

  it('the TCP width follows --rx-tcp-w in hit testing', async () => {
    const { shell } = await boot();
    const arrange = shell.arrangeView;
    const el = q('.arrange')!;
    el.style.setProperty('--rx-tcp-w', '112px');
    expect(arrange.regionAt(120)).toBe('lanes');
    expect(arrange.tcpWidth).toBe(112);
    el.style.setProperty('--rx-tcp-w', '248px');
    expect(arrange.regionAt(120)).toBe('tcp');
    expect(arrange.tcpWidth).toBe(248);
    el.style.setProperty('--rx-tcp-w', '176px');
    expect(arrange.regionAt(180)).toBe('lanes');
    expect(arrange.regionAt(170)).toBe('tcp');
  });
});

describe('breakpoint change', () => {
  it('switches the menubar and the transport between the phone and the desktop markup', async () => {
    const media = mockMedia(390);
    await boot();
    expect(q('.menu-btn')).not.toBeNull();
    media.resize(1280);
    await tick();
    expect(q('.menu-btn')).toBeNull();
    expect(q('.agents-btn')).toBeNull();
    expect(document.querySelectorAll('.menubar .menu')).toHaveLength(6);
    expect(q('.transport .trow')).toBeNull();
    expect(q('.mixer')!.classList.contains('sheet')).toBe(false);
    media.resize(700);
    await tick();
    expect(q('.menu-btn')).toBeNull();
    expect(q('.agents-btn')).not.toBeNull(); // below 960 px the agents button comes, the menus stay
    expect(document.querySelectorAll('.menubar .menu')).toHaveLength(6);
  });
});

describe('desktop layout (1280 px)', () => {
  it('keeps the default markup: menus, one-row transport, inline mixer, no narrow controls', async () => {
    mockMedia(1280);
    await boot();
    expect(document.querySelectorAll('.menubar .menu')).toHaveLength(6);
    expect(q('.menubar .status')).not.toBeNull();
    expect(q('.menu-btn')).toBeNull();
    expect(q('.agents-btn')).toBeNull();
    expect(q('.transport .trow')).toBeNull();
    expect(q('.transport select')).not.toBeNull();
    expect(q('.more-btn')).toBeNull();
    expect(q('.mixer')!.classList.contains('sheet')).toBe(false);
    expect(q('.mixer .sheet-handle')).toBeNull();
    expect(q('.agent-bar')).toBeNull();
    expect(q('.drawer-close')).toBeNull();
    expect(document.documentElement.style.getPropertyValue('--rx-sheet-h')).toBe('');
  });

  it('works where matchMedia does not exist at all', async () => {
    await boot();
    expect(document.querySelectorAll('.menubar .menu')).toHaveLength(6);
    expect(q('.menu-btn')).toBeNull();
  });
});
