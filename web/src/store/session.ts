// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The application controller: the socket, the open projects (tabs), the audio stream and every action of the UI.
// Views read state from here and call its methods; nothing else talks to the server.
import { Emitter } from '../lib/dom';
import type { Grid } from '../lib/snap';
import { McpClient } from '../net/mcp';
import {
  FRAME_AUDIO,
  FRAME_METERS,
  FRAME_POSITION,
  type ActivityEntry,
  type HistoryStep,
  type ProjectChangedEvent,
  type ProjectGet,
  type ServerInfo,
  type ServerMessage,
  type StorageInfo,
  type TransportState,
  frameType,
  parseAudio,
  parseMeters,
  parsePosition,
} from '../net/protocol';
import { type CmdOptions, type CmdResult, type SocketStatus, RovoxSocket, socketUrl } from '../net/ws';
import { BrowserAudio } from '../audio/stream';
import { ProjectStore } from './project';

export type TabStatus = 'playing' | 'background' | 'agent' | 'idle';

export interface Toast {
  id: number;
  text: string;
  kind: 'info' | 'error';
}

export interface UiPrefs {
  grid: Grid;
  snap: boolean;
  mixerOpen: boolean;
  theme: 'dark' | 'light';
}

const PREFS_KEY = 'rovox.ui';

function loadPrefs(): UiPrefs {
  const d: UiPrefs = { grid: '1/16', snap: true, mixerOpen: true, theme: 'dark' };
  try {
    return { ...d, ...(JSON.parse(localStorage.getItem(PREFS_KEY) ?? '{}') as Partial<UiPrefs>) };
  } catch {
    return d;
  }
}

const OPEN_KEY = 'rovox.open';

function readRemembered(): string[] {
  try {
    const v: unknown = JSON.parse(localStorage.getItem(OPEN_KEY) ?? '[]');
    return Array.isArray(v) ? v.filter((x): x is string => typeof x === 'string') : [];
  } catch {
    return [];
  }
}

export class App {
  readonly changed = new Emitter(); // tabs, connection, prefs, toasts
  readonly welcomed = new Emitter(); // the server said `welcome` (first connect or reconnect)
  readonly projects = new Map<string, ProjectStore>();
  order: string[] = [];
  activeId: string | undefined;
  status: SocketStatus = 'closed';
  sessionId = '';
  serverInfo: ServerInfo = { version: '', source_url: '', audio: { mode: 'none', available: false } };
  needsLogin = false;
  prefs: UiPrefs = loadPrefs();
  toasts: Toast[] = [];
  readonly socket: RovoxSocket;
  readonly audio: BrowserAudio;
  readonly mcp = new McpClient();
  private refetchTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private historyTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private toastId = 1;

  constructor(url = socketUrl()) {
    this.audio = new BrowserAudio((on) => this.socket.send({ t: 'audio', on }));
    this.socket = new RovoxSocket(url, {
      onStatus: (s) => this.onStatus(s),
      onMessage: (m) => this.onMessage(m),
      onBinary: (b) => this.onBinary(b),
      onHandshakeFailed: () => void this.recheckLogin(),
    });
    document.documentElement.dataset.theme = this.prefs.theme;
  }

  /** Asks the server whether this browser still has to log in (true) — false when it does not or the server cannot be reached. */
  private async queryLogin(): Promise<boolean> {
    const res = await fetch('/api/session', { credentials: 'same-origin' }).catch(() => null);
    if (!res?.ok) return false;
    const s = (await res.json()) as { token_required: boolean; authenticated: boolean; source_url: string; version: string };
    this.serverInfo.source_url = s.source_url;
    this.serverInfo.version = s.version;
    return s.token_required && !s.authenticated;
  }

  async start(): Promise<void> {
    if (await this.queryLogin()) {
      this.needsLogin = true;
      this.changed.emit();
      return;
    }
    this.socket.connect();
  }

  private recheckingLogin = false;

  /** The socket keeps closing before `welcome`: after a server restart with a new token that is a 401 — show the login screen instead of retrying forever. */
  private async recheckLogin(): Promise<void> {
    if (this.recheckingLogin || this.needsLogin) return;
    this.recheckingLogin = true;
    try {
      if (await this.queryLogin()) {
        this.socket.close();
        this.needsLogin = true;
        this.changed.emit();
      }
    } finally {
      this.recheckingLogin = false;
    }
  }

  async login(token: string): Promise<boolean> {
    const res = await fetch('/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ token }), credentials: 'same-origin' });
    if (!res.ok) return false;
    this.needsLogin = false;
    this.socket.connect();
    this.changed.emit();
    return true;
  }

  //==========================================================================
  get active(): ProjectStore | undefined {
    return this.activeId ? this.projects.get(this.activeId) : undefined;
  }

  setActive(id: string): void {
    if (!this.projects.has(id)) return;
    this.activeId = id;
    localStorage.setItem('rovox.active', id);
    this.changed.emit();
  }

  updatePrefs(patch: Partial<UiPrefs>): void {
    this.prefs = { ...this.prefs, ...patch };
    localStorage.setItem(PREFS_KEY, JSON.stringify(this.prefs));
    document.documentElement.dataset.theme = this.prefs.theme;
    this.changed.emit();
  }

  toast(text: string, kind: Toast['kind'] = 'info'): void {
    const t = { id: this.toastId++, text, kind };
    this.toasts.push(t);
    this.changed.emit();
    setTimeout(() => {
      this.toasts = this.toasts.filter((x) => x !== t);
      this.changed.emit();
    }, kind === 'error' ? 6000 : 3000);
  }

  tabStatus(p: ProjectStore): TabStatus {
    if (Date.now() - p.lastAgentAt < 15000) return 'agent';
    if (p.isPlaying()) return p.id === this.activeId ? 'playing' : 'background';
    return 'idle';
  }

  //==========================================================================
  // commands
  async cmd<T = unknown>(name: string, params: Record<string, unknown> = {}, o: CmdOptions = {}): Promise<CmdResult<T>> {
    const r = await this.socket.cmd<T>(name, params, { projectId: this.activeId, ...o });
    if (!r.ok && r.error?.code !== 'DISCONNECTED') this.toast(`${name}: ${r.error?.message ?? 'error'}`, 'error');
    return r;
  }

  /** A command on the active project that changes the model, followed by a refetch when `refetch` is set (new ids, structure). */
  async edit(name: string, params: Record<string, unknown>, o: CmdOptions & { refetch?: boolean } = {}): Promise<CmdResult> {
    const p = this.active;
    const r = await this.cmd(name, params, o);
    if (p) {
      if (r.ok && r.revision) p.revision = Math.max(p.revision, r.revision);
      if (!r.ok || o.refetch) this.refetchSoon(p.id, 0); // roll the optimistic state back / pick up new ids
      this.refreshHistorySoon(p.id);
    }
    return r;
  }

  /** Several edits of the active project as one undo step. */
  async editBatch(ops: { name: string; params: Record<string, unknown> }[], o: CmdOptions & { refetch?: boolean } = {}): Promise<CmdResult> {
    const p = this.active;
    const r = await this.socket.batch(ops, { projectId: this.activeId, ...o });
    if (!r.ok) this.toast(`${r.error?.message ?? 'batch failed'}`, 'error');
    if (p) {
      if (r.ok && r.revision) p.revision = Math.max(p.revision, r.revision);
      if (!r.ok || o.refetch) this.refetchSoon(p.id, 0);
      this.refreshHistorySoon(p.id);
    }
    return r;
  }

  /** Ids of the projects that were open in this browser last time (localStorage may be unavailable). */
  private rememberedIds: string[] = readRemembered();
  private restored = false;

  private rememberOpen(): void {
    try {
      localStorage.setItem(OPEN_KEY, JSON.stringify(this.order));
    } catch {
      /* private mode / quota: nothing to remember */
    }
  }

  /** After a server restart nothing is open: reopen what this browser had open, quietly (a project that is gone is skipped). */
  private async restoreRemembered(): Promise<void> {
    if (this.restored) return;
    this.restored = true;
    if (this.order.length || !this.rememberedIds.length) {
      this.rememberOpen();
      return;
    }
    let opened = false;
    for (const target of this.rememberedIds) {
      const r = await this.socket.cmd<{ project_id: string }>('project.open', { target });
      if (r.ok && r.data) opened = true;
    }
    if (opened) await this.listProjects();
    else this.rememberOpen();
  }

  async listProjects(): Promise<void> {
    const r = await this.socket.cmd<{ projects: { project_id: string; name: string }[] }>('project.list');
    if (!r.ok || !r.data) return;
    const ids = r.data.projects.map((p) => p.project_id);
    for (const id of ids) if (id && !this.projects.has(id)) this.adopt(id);
    for (const id of [...this.projects.keys()]) if (!ids.includes(id)) this.drop(id);
    if (!this.activeId || !this.projects.has(this.activeId)) {
      const remembered = localStorage.getItem('rovox.active');
      this.activeId = remembered && this.projects.has(remembered) ? remembered : this.order[0];
    }
    if (this.restored) this.rememberOpen();
    this.changed.emit();
  }

  /** Reads the project's mix strip (also after a reconnect) and shows it on the tab. */
  private async loadMix(p: ProjectStore): Promise<void> {
    const r = await this.socket.cmd<{ muted?: boolean }>('project.mix', {}, { projectId: p.id });
    if (r.ok && typeof r.data?.muted === 'boolean' && r.data.muted !== p.muted) {
      p.muted = r.data.muted;
      this.changed.emit();
    }
  }

  /** Mutes / unmutes the project in the live mix; does not touch the active tab. */
  async toggleMute(id: string): Promise<void> {
    const p = this.projects.get(id);
    if (!p) return;
    const r = await this.cmd<{ muted?: boolean }>('project.mix', { muted: !p.muted }, { projectId: id });
    if (r.ok) {
      p.muted = typeof r.data?.muted === 'boolean' ? r.data.muted : !p.muted;
      this.changed.emit();
    }
  }

  private adopt(id: string): ProjectStore {
    const p = new ProjectStore(id);
    this.projects.set(id, p);
    this.order.push(id);
    p.state.on(() => this.changed.emit());
    this.socket.send({ t: 'sub', project_id: id, channels: ['state', 'transport', 'meters'] });
    return p;
  }

  private drop(id: string): void {
    this.projects.delete(id);
    this.order = this.order.filter((x) => x !== id);
    if (this.activeId === id) this.activeId = this.order[0];
  }

  async newProject(name = 'Untitled'): Promise<void> {
    const r = await this.socket.cmd<{ project_id: string }>('project.new', { name });
    if (r.ok && r.data) {
      await this.listProjects();
      this.setActive(r.data.project_id);
    } else this.toast(r.error?.message ?? 'не вдалося створити проект', 'error');
  }

  async openProject(target: string): Promise<void> {
    const r = await this.socket.cmd<{ project_id: string }>('project.open', { target });
    if (r.ok && r.data) {
      await this.listProjects();
      this.setActive(r.data.project_id);
    } else this.toast(r.error?.message ?? 'не вдалося відкрити проект', 'error');
  }

  async closeProject(id: string): Promise<void> {
    const r = await this.socket.cmd('project.close', { project_id: id, save: true }, { projectId: id });
    if (r.ok) await this.listProjects();
    else this.toast(r.error?.message ?? 'не вдалося закрити проект', 'error');
  }

  //==========================================================================
  // fetching
  refetchSoon(id: string, delayMs = 25): void {
    if (this.refetchTimers.has(id)) return;
    this.refetchTimers.set(
      id,
      setTimeout(async () => {
        this.refetchTimers.delete(id);
        const r = await this.socket.cmd<ProjectGet>('project.get', {}, { projectId: id });
        if (r.ok && r.data) this.projects.get(id)?.applyProjectGet(r.data);
      }, delayMs),
    );
  }

  refreshHistorySoon(id: string): void {
    if (this.historyTimers.has(id)) return;
    this.historyTimers.set(
      id,
      setTimeout(async () => {
        this.historyTimers.delete(id);
        const r = await this.socket.cmd<{ undo: HistoryStep[]; redo: HistoryStep[] }>('history.list', {}, { projectId: id });
        if (r.ok && r.data) this.projects.get(id)?.setHistory(r.data);
      }, 120),
    );
  }

  //==========================================================================
  // transport
  async setTransport(params: Record<string, unknown>): Promise<void> {
    const p = this.active;
    if (!p) return;
    const r = await this.socket.cmd<TransportState>('transport.set', params, { projectId: p.id });

    if (!r.ok) {
      this.toast(r.error?.message ?? 'transport', 'error');
      return;
    }
    if (r.data) this.applyTransport(p, r.data);
  }

  private applyTransport(p: ProjectStore, t: TransportState): void {
    p.transport = { ...p.transport, ...t, loop: t.loop ?? p.transport.loop };
    p.lastPositionAt = performance.now();
    if (this.audio.stats.enabled) this.audio.setEpoch(t.epoch);
    p.transportChanged.emit();
    this.changed.emit();
  }

  play = () => this.setTransport({ state: 'playing' });
  stop = () => this.setTransport({ state: 'stopped' });
  seek = (t: number) => this.setTransport({ position_s: Math.max(0, t) });
  toStart = () => this.seek(0);

  togglePlay(): void {
    void (this.active?.isPlaying() ? this.stop() : this.play());
  }

  toggleLoop(): void {
    const p = this.active;
    if (!p) return;
    const l = p.transport.loop;
    if (l.enabled) return void this.setTransport({ loop: { enabled: false } });
    let start = l.start_s;
    let end = l.end_s;
    if (end <= start) {
      start = 0;
      end = Math.max(2, Math.min(p.lengthS || 8, 8));
    }
    void this.setTransport({ loop: { enabled: true, start_s: start, end_s: end } });
  }

  setLoop(start: number, end: number): void {
    if (end - start < 0.05) return;
    void this.setTransport({ loop: { enabled: true, start_s: start, end_s: end } });
  }

  /** The playhead as drawn: audible position when the stream plays, else interpolated from the 30 Hz position frames. */
  playhead(p: ProjectStore): number {
    if (!p.isPlaying()) return p.transport.position_s;
    if (this.audio.stats.enabled) {
      const a = this.audio.audiblePosition();
      if (a !== null) return a;
    }
    return p.transport.position_s + Math.max(0, performance.now() - p.lastPositionAt) / 1000;
  }

  async enableAudio(): Promise<void> {
    if (this.audio.stats.enabled) {
      this.audio.disable();
      return;
    }
    if (!(await this.audio.enable())) this.toast(this.audio.stats.error ?? 'звук недоступний', 'error');
    const p = this.active;
    if (p) this.audio.setEpoch(p.transport.epoch);
  }

  //==========================================================================
  async undo(): Promise<void> {
    await this.edit('undo', {}, { refetch: true });
  }

  async redo(): Promise<void> {
    await this.edit('redo', {}, { refetch: true });
  }

  /** Undoes the newest run of agent steps of the active project, no further than a step by a person: every undo is pinned to the
   *  project and to the revision the previous answer reported, so a step made in between stops the loop. */
  async undoAgent(): Promise<void> {
    const p = this.active;
    if (!p) return;
    const id = p.id;
    const hist = await this.socket.cmd<{ undo: HistoryStep[]; redo: HistoryStep[] }>('history.list', {}, { projectId: id });
    if (!hist.ok || !hist.data) {
      this.toast(hist.error?.message ?? 'не вдалося отримати історію', 'error');
      return;
    }
    p.setHistory(hist.data);
    const { count } = p.agentUndo();
    let revision = hist.revision;
    let done = 0;
    for (let i = 0; i < count; i++) {
      const r = await this.socket.cmd('undo', {}, { projectId: id, ifRevision: revision });
      if (!r.ok) {
        if (r.error?.code === 'REVISION_CONFLICT') this.toast(`Проект змінився під час відкату (скасовано кроків: ${done}) — решту не чіпаю`, 'error');
        else if (r.error?.code !== 'DISCONNECTED') this.toast(`undo: ${r.error?.message ?? 'error'}`, 'error');
        break;
      }
      done++;
      revision = r.revision;
    }
    this.refetchSoon(id, 0);
    this.refreshHistorySoon(id);
  }

  async acceptAgent(): Promise<void> {
    const p = this.active;
    if (!p) return;
    p.accept(p.revision);
    await this.cmd('agent.accept', { up_to_revision: p.revision });
  }

  //==========================================================================
  private onStatus(s: SocketStatus): void {
    this.status = s;
    if (s === 'closed') {
      // keep the last state on screen, banner says "offline"; the server may restart, so the old pidx must not match anything
      for (const p of this.projects.values()) p.pidx = -1;
    }
    this.changed.emit();
  }

  private onMessage(m: ServerMessage): void {
    switch (m.t) {
      case 'welcome':
        this.sessionId = m.session;
        this.serverInfo = { version: m.server_version, source_url: m.source_url ?? this.serverInfo.source_url, audio: m.audio };
        // after a reconnect the old subscriptions are gone: keep the stores (zoom, scroll, selection, marks) and subscribe again,
        // the fresh snapshots refresh them; the list reconciles by id
        for (const id of this.projects.keys()) this.socket.send({ t: 'sub', project_id: id, channels: ['state', 'transport', 'meters'] });
        // the server forgot that this client listens: ask for the stream again, the old buffer is from before the gap
        if (this.audio.stats.enabled) {
          this.audio.flush();
          this.socket.send({ t: 'audio', on: true });
        }
        void this.listProjects().then(() => this.restoreRemembered());
        this.welcomed.emit();
        this.changed.emit();
        break;
      case 'snapshot': {
        const p = this.projects.get(m.project_id);
        if (!p) break;
        p.applySnapshot(m);
        this.refreshHistorySoon(p.id);
        void this.loadMix(p);
        void this.socket.cmd<{ activity: ActivityEntry[] }>('project.activity', { limit: 60 }, { projectId: p.id }).then((r) => {
          if (r.ok && r.data) p.applyActivity(r.data.activity);
        });
        this.changed.emit();
        break;
      }
      case 'event':
        this.onEvent(m);
        break;
      case 'resync':
        this.refetchSoon(m.project_id, 0);
        break;
      case 'error':
        if (m.message) this.toast(m.message, 'error');
        break;
      default:
        break;
    }
  }

  private onEvent(ev: ServerMessage & { t: 'event' }): void {
    const type = ev.type;
    const pid = ev['project_id'] as string | undefined;
    const p = pid ? this.projects.get(pid) : undefined;

    if (type === 'project.changed' && p) {
      const e = ev as unknown as ProjectChangedEvent;
      if (p.noteChanged(e, this.sessionId)) this.refetchSoon(p.id);
      else p.state.emit();
      this.refreshHistorySoon(p.id);
      this.changed.emit();
    } else if (type === 'transport.changed' && p) {
      this.applyTransport(p, ev as unknown as TransportState);
    } else if (type === 'storage.changed' && p) {
      p.storage = ev['storage'] as StorageInfo;
      p.state.emit();
      this.changed.emit();
    } else if (type === 'agent.accepted' && p) {
      p.accept(ev['accepted_revision'] as number);
    } else if (type === 'projects.changed') {
      void this.listProjects();
    }
  }

  private onBinary(buf: ArrayBuffer): void {
    switch (frameType(buf)) {
      case FRAME_AUDIO: {
        const pkt = parseAudio(buf);
        if (pkt) this.audio.onPacket(pkt);
        break;
      }
      case FRAME_METERS: {
        const f = parseMeters(buf);
        const p = f && this.byPidx(f.pidx);
        if (f && p) {
          const now = performance.now();
          for (const e of f.entries) {
            const id = p.meterIndex[e.index];
            if (id) p.setMeter(id, e.l, e.r, now);
          }
        }
        break;
      }
      case FRAME_POSITION: {
        const f = parsePosition(buf);
        const p = f && this.byPidx(f.pidx);
        if (f && p) {
          p.transport.position_s = f.position_s;
          p.transport.state = f.playing ? 'playing' : 'stopped';
          p.transport.epoch = f.epoch;
          p.lastPositionAt = performance.now();
          if (this.audio.stats.enabled && p.id === this.activeId) this.audio.setEpoch(f.epoch);
        }
        break;
      }
      default:
        break;
    }
  }

  private byPidx(pidx: number): ProjectStore | undefined {
    for (const p of this.projects.values()) if (p.pidx === pidx) return p;
    return undefined;
  }
}
