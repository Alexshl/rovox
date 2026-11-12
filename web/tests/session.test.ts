// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// App controller: Undo agent pinned to its project and revision, login re-check after failed handshakes, state kept across
// reconnects, the socket heartbeat and the render dialog.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setLocale } from '../src/i18n';
import { RovoxSocket } from '../src/net/ws';

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
  name: 'one',
  revision: 10,
  sample_rate: 44100,
  length_s: 30,
  can_undo: true,
  can_redo: false,
  dirty: false,
  tracks: [{ track_id: 'trk_a', index: 0, name: 'Flute', color: '#4cc9f0', mute: false, solo: false, volume_db: 0, pan: 0 }],
  master: { track_id: 'master', volume_db: 0, pan: 0 },
  items: [],
  markers: [],
  tempo: { points: [{ bpm: 120, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } },
  fx: {},
};
const transport = { state: 'stopped', position_s: 0, epoch: 0, loop: { enabled: false, start_s: 0, end_s: 0 }, length_s: 30, audio: 'none' };

let undoAnswer: (msg: Record<string, any>, ws: FakeWebSocket) => void;
let activity: unknown[] = [];

function answer(msg: Record<string, any>, ws: FakeWebSocket): void {
  if (msg.t === 'hello') {
    ws.push({ t: 'welcome', session: 'ws_1', server_version: '0.1.0', audio: { mode: 'none', available: false } });
  } else if (msg.t === 'sub') {
    ws.push({ t: 'snapshot', project_id: msg.project_id, pidx: 1, revision: 10, state: { ...project, project_id: msg.project_id }, transport, meter_index: [], accepted_revision: 0 });
  } else if (msg.t === 'cmd') {
    const reply = (data: unknown, revision = 20) => ws.push({ t: 'result', id: msg.id, ok: true, revision, data });
    switch (msg.name) {
      case 'project.list':
        return reply({ projects: [{ project_id: 'p_1', name: 'one' }, { project_id: 'p_2', name: 'two' }] });
      case 'history.list': {
        const o = (kind: string) => ({ kind, session: 's', label: kind });
        return reply({ undo: [{ revision: 8, label: 'x', time: '', origin: o('ui') }, { revision: 19, label: 'x', time: '', origin: o('mcp') }, { revision: 20, label: 'x', time: '', origin: o('mcp') }], redo: [] }, 20);
      }
      case 'project.activity':
        return reply({ activity });
      case 'undo':
        return undoAnswer(msg, ws);
      default:
        return reply({});
    }
  }
}

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));

beforeEach(() => {
  setLocale('uk'); // the tests assert the Ukrainian texts, whatever navigator.language says
  FakeWebSocket.instances = [];
  FakeWebSocket.handler = answer;
  activity = [];
  undoAnswer = (msg, ws) => ws.push({ t: 'result', id: msg.id, ok: true, revision: 1, data: {} });
  vi.stubGlobal('WebSocket', FakeWebSocket);
  vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, json: async () => ({ token_required: false, authenticated: true, source_url: '', version: '0.1.0' }) })));
  localStorage.clear();
  document.body.innerHTML = '';
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

async function boot() {
  const { App } = await import('../src/store/session');
  const app = new App('ws://test/ws');
  await app.start();
  await tick(350);
  return { app, ws: FakeWebSocket.instances[0]! };
}

describe('undoAgent', () => {
  it('pins every undo to the project it was started on, even if the tab changes meanwhile', async () => {
    const { app, ws } = await boot();
    let first = true;
    undoAnswer = (msg, w) => {
      if (first) {
        first = false;
        app.setActive('p_2'); // the person switches tab during the loop
      }
      w.push({ t: 'result', id: msg.id, ok: true, revision: 30, data: {} });
    };
    await app.undoAgent();
    const undos = ws.sent.filter((m) => m.name === 'undo');
    expect(undos).toHaveLength(2);
    expect(undos.every((m) => m.project_id === 'p_1')).toBe(true);
  });

  it('chains if_revision from the history answer and from each undo result', async () => {
    const { app, ws } = await boot();
    let rev = 20;
    undoAnswer = (msg, w) => w.push({ t: 'result', id: msg.id, ok: true, revision: ++rev, data: {} });
    await app.undoAgent();
    const undos = ws.sent.filter((m) => m.name === 'undo');
    expect(undos.map((m) => m.if_revision)).toEqual([20, 21]);
  });

  it('stops on a revision conflict (a step by a person in between) with a toast', async () => {
    const { app, ws } = await boot();
    undoAnswer = (msg, w) => w.push({ t: 'result', id: msg.id, ok: false, error: { code: 'REVISION_CONFLICT', message: 'moved on' } });
    await app.undoAgent();
    expect(ws.sent.filter((m) => m.name === 'undo')).toHaveLength(1);
    expect(app.toasts.some((t) => t.kind === 'error' && t.text.includes('змінився'))).toBe(true);
  });
});

describe('login re-check', () => {
  it('switches to the login screen after two closes before welcome when the server now wants a token', async () => {
    const { App } = await import('../src/store/session');
    FakeWebSocket.handler = () => undefined; // the server never says welcome
    const app = new App('ws://test/ws');
    await app.start();
    await tick(10);
    vi.mocked(fetch).mockImplementation((async () => ({ ok: true, json: async () => ({ token_required: true, authenticated: false, source_url: '', version: '0.1.0' }) })) as any);
    FakeWebSocket.instances[0]!.close();
    await tick(700);
    FakeWebSocket.instances[1]!.close();
    await tick(50);
    expect(app.needsLogin).toBe(true);
  });
});

describe('reconnect', () => {
  it('keeps the project stores (view, selection) and subscribes again', async () => {
    const { app, ws } = await boot();
    const store = app.projects.get('p_1')!;
    store.view.pps = 123;
    store.selection.add('itm_x');
    ws.close();
    await tick(1500);
    const ws2 = FakeWebSocket.instances[1]!;
    await tick(300);
    expect(app.projects.get('p_1')).toBe(store);
    expect(store.view.pps).toBe(123);
    expect(ws2.sent.some((m) => m.t === 'sub' && m.project_id === 'p_1')).toBe(true);
  });

  it('forgets the old pidx on close and emits `welcomed` again after the reconnect', async () => {
    const { app, ws } = await boot();
    const store = app.projects.get('p_1')!;
    expect(store.pidx).toBe(1);
    let welcomes = 0;
    app.welcomed.on(() => welcomes++);
    ws.close();
    expect(store.pidx).toBe(-1); // meters of a restarted server cannot land on this project
    await tick(1500);
    await tick(300);
    expect(welcomes).toBe(1);
    expect(store.pidx).toBe(1); // the fresh snapshot
  });

  it('drops marks above the revision of the new snapshot (the server lost that work), keeps the others', async () => {
    const { app, ws } = await boot();
    const store = app.projects.get('p_1')!;
    const mark = (revision: number) => ({ session: 'm1', label: 'Claude', at: 1, revision, undoLabel: 'x', op: 'update' as const });
    store.marks.set('trk_old', mark(5));
    store.marks.set('trk_lost', mark(99)); // the snapshot says revision 10
    ws.close();
    await tick(1500);
    await tick(300);
    expect(store.marks.has('trk_old')).toBe(true);
    expect(store.marks.has('trk_lost')).toBe(false);
  });

  it('rebuilds the agent marks from the activity log so Accept is enabled after a reload', async () => {
    activity = [{ t: '2026-11-12T10:00:00Z', origin: { kind: 'mcp', session: 'm1', label: 'Claude' }, name: 'track.update', params: { track_id: 'trk_a', volume_db: -3 }, ok: true, revision: 12, mutating: true }];
    const { app } = await boot();
    expect(app.projects.get('p_1')!.marks.has('trk_a')).toBe(true);
  });
});

describe('socket heartbeat', () => {
  it('pings and force-closes a silent connection, then reconnects', () => {
    vi.useFakeTimers();
    const made: FakeWebSocket[] = [];
    const s = new RovoxSocket(
      'ws://x/ws',
      { onStatus: vi.fn(), onMessage: vi.fn(), onBinary: vi.fn() },
      (u) => {
        const w = new FakeWebSocket(u);
        made.push(w);
        return w as unknown as WebSocket;
      },
      { pingMs: 1000, deadMs: 3500 },
    );
    s.connect();
    vi.advanceTimersByTime(1000);
    expect(made[0]!.sent.some((m) => m.t === 'ping')).toBe(true);
    vi.advanceTimersByTime(3000); // the 4th beat (t = 4 s) sees 4 s of silence > deadMs and closes; the retry comes after the backoff
    expect(s.status).toBe('closed');
    vi.advanceTimersByTime(600);
    expect(made).toHaveLength(2);
    s.close();
  });
});

describe('render dialog', () => {
  it('shows an error row and re-enables the buttons when the job throws; labels are tied to controls', async () => {
    const { openRenderDialog } = await import('../src/views/render');
    const app = {
      active: { id: 'p_1', name: 'one' },
      mcp: { runJob: vi.fn(async () => { throw new Error('network down'); }) },
    } as any;
    openRenderDialog(app);
    const scrim = document.querySelector('.scrim')!;
    expect(scrim.getAttribute('aria-modal')).toBe('true');
    const label = scrim.querySelector('label')!;
    expect(scrim.querySelector(`#${label.getAttribute('for')}`)).not.toBeNull();
    const buttons = [...scrim.querySelectorAll('button')] as HTMLButtonElement[];
    const render = buttons.find((b) => b.textContent === 'Рендер')!;
    render.click();
    await tick(10);
    expect(scrim.querySelector('.finding.error')!.textContent).toContain('network down');
    expect(buttons.every((b) => !b.disabled)).toBe(true);
    expect((scrim.querySelector('.progress') as HTMLElement).style.display).toBe('none');
  });

  it('traps Tab inside the dialog and returns the focus to the opener on close', async () => {
    const { openRenderDialog } = await import('../src/views/render');
    const opener = document.createElement('button');
    document.body.append(opener);
    opener.focus();
    openRenderDialog({ active: { id: 'p_1', name: 'one' }, mcp: { runJob: vi.fn() } } as any);
    const scrim = document.querySelector('.scrim')!;
    const dialog = scrim.querySelector('.dialog')!;
    expect(dialog.contains(document.activeElement)).toBe(true);
    const focusable = [...dialog.querySelectorAll<HTMLElement>('button:not([disabled]), input:not([disabled]), select:not([disabled])')];
    const first = focusable[0]!;
    const last = focusable[focusable.length - 1]!;
    const tab = (el: Element, shiftKey = false) => {
      const ev = new KeyboardEvent('keydown', { key: 'Tab', shiftKey, bubbles: true, cancelable: true });
      el.dispatchEvent(ev);
      return ev.defaultPrevented;
    };
    last.focus();
    expect(tab(last)).toBe(true); // wraps instead of leaving the dialog
    expect(document.activeElement).toBe(first);
    expect(tab(first, true)).toBe(true);
    expect(document.activeElement).toBe(last);
    scrim.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    expect(document.querySelector('.scrim')).toBeNull();
    expect(document.activeElement).toBe(opener);
  });
});
