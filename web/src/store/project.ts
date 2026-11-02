// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Client-side mirror of one open project: normalised by id, kept in step with the server by snapshot + events, with the
// optimistic edits of the UI and the "touched by an agent" marks (agent-cyan).
import { Emitter } from '../lib/dom';
import { type TempoMap, DEFAULT_TEMPO } from '../lib/time';
import { TrackItemIndex } from '../lib/itemIndex';
import type {
  ActivityEntry,
  ChangedEntity,
  HistoryStep,
  ItemState,
  MarkerState,
  MasterState,
  Origin,
  PluginState,
  ProjectChangedEvent,
  ProjectGet,
  StorageInfo,
  TrackState,
  TransportState,
} from '../net/protocol';

export interface AgentMark {
  session: string;
  label: string;
  at: number;
  revision: number;
  undoLabel: string;
  op: ChangedEntity['op'];
}

export interface LogEntry {
  at: number;
  origin: Origin;
  tool: string;
  text: string;
  revision: number;
  ids: string[];
}

export interface MeterValue {
  l: number;
  r: number;
  holdL: number;
  holdR: number;
  holdAt: number;
  at: number;
}

export const EMPTY_TRANSPORT: TransportState = {
  state: 'stopped',
  position_s: 0,
  epoch: 0,
  loop: { enabled: false, start_s: 0, end_s: 0 },
  length_s: 0,
  audio: 'none',
};

export function isAgent(o: Origin | undefined): boolean {
  return o !== undefined && (o.kind === 'mcp' || o.kind === 'skill');
}

export class ProjectStore {
  readonly state = new Emitter();
  readonly transportChanged = new Emitter();
  readonly agentChanged = new Emitter();

  pidx = 0;
  name = '';
  revision = 0;
  sampleRate = 44100;
  lengthS = 0;
  dirty = false;
  storage: StorageInfo | undefined;
  loaded = false;
  canUndo = false;
  canRedo = false;
  tracks: TrackState[] = [];
  master: MasterState | undefined;
  items = new Map<string, ItemState>();
  markers: MarkerState[] = [];
  tempo: TempoMap = DEFAULT_TEMPO;
  fx = new Map<string, PluginState[]>();
  transport: TransportState = { ...EMPTY_TRANSPORT };
  /** Playhead as drawn: from the audible audio position when the stream plays, else from the server position. */
  meterIndex: string[] = [];
  meters = new Map<string, MeterValue>();
  acceptedRevision = 0;
  marks = new Map<string, AgentMark>();
  history: HistoryStep[] = [];
  log: LogEntry[] = [];
  selection = new Set<string>();
  selectedTrack: string | undefined;
  /** Arrange view window: seconds at the left edge, pixels per second, vertical scroll. Kept per project (tab). */
  view = { start: 0, pps: 40, scrollY: 0 };
  lastPositionAt = 0;
  lastAgentAt = 0;
  lastAgentFocus: { trackId: string; timeS: number; label: string; at: number } | undefined;

  private indexes = new Map<string, TrackItemIndex<ItemState>>();
  private pluginTrack = new Map<string, string>();

  constructor(readonly id: string) {}

  //==========================================================================
  applyProjectGet(data: ProjectGet): void {
    this.name = data.name;
    this.revision = data.revision;
    this.sampleRate = data.sample_rate;
    this.lengthS = data.length_s;
    this.dirty = data.dirty;
    this.storage = data.storage;
    this.canUndo = data.can_undo;
    this.canRedo = data.can_redo;
    this.tracks = data.tracks ?? [];
    this.master = data.master;
    this.markers = data.markers ?? [];

    if (data.tempo) {
      this.tempo = {
        points: data.tempo.points.map((p) => ({ bpm: p.bpm, time_s: p.time_s, qn: p.qn })),
        numerator: data.tempo.time_signature.numerator,
        denominator: data.tempo.time_signature.denominator,
      };
    }

    this.items = new Map((data.items ?? []).map((i) => [i.item_id, i]));
    this.fx = new Map(Object.entries(data.fx ?? {}));
    this.pluginTrack.clear();
    for (const [trackId, list] of this.fx) for (const p of list) this.pluginTrack.set(p.plugin_id, trackId);

    for (const id of [...this.selection]) if (!this.items.has(id)) this.selection.delete(id);
    if (this.selectedTrack && !this.tracks.some((t) => t.track_id === this.selectedTrack)) this.selectedTrack = undefined;

    this.reindex();
    this.loaded = true;
    this.state.emit();
  }

  applySnapshot(msg: { pidx: number; state: ProjectGet; transport: TransportState; meter_index: string[]; accepted_revision: number }): void {
    this.pidx = msg.pidx;
    this.transport = msg.transport;
    this.meterIndex = msg.meter_index;
    this.acceptedRevision = msg.accepted_revision;
    this.pruneMarks();
    this.applyProjectGet(msg.state);
    this.transportChanged.emit();
  }

  /** Rebuilds the per-track indexes after items were edited in place. */
  refreshIndex(): void {
    this.reindex();
  }

  private reindex(): void {
    const byTrack = new Map<string, ItemState[]>();
    for (const it of this.items.values()) {
      const list = byTrack.get(it.track_id);
      if (list) list.push(it);
      else byTrack.set(it.track_id, [it]);
    }
    this.indexes = new Map([...byTrack].map(([k, v]) => [k, new TrackItemIndex(v)]));
  }

  index(trackId: string): TrackItemIndex<ItemState> | undefined {
    return this.indexes.get(trackId);
  }

  track(id: string): TrackState | undefined {
    return this.tracks.find((t) => t.track_id === id);
  }

  //==========================================================================
  // optimistic edits: applied locally at once, the server answer / event confirms
  patchTrack(id: string, patch: Partial<TrackState>): void {
    if (id === 'master' && this.master) {
      this.master = { ...this.master, ...(patch as Partial<MasterState>) };
    } else {
      const t = this.track(id);
      if (t) Object.assign(t, patch);
    }
    this.state.emit();
  }

  patchItem(id: string, patch: Partial<ItemState>): void {
    const it = this.items.get(id);
    if (!it) return;
    Object.assign(it, patch);
    it.end_s = it.start_s + it.length_s;
    this.reindex();
    this.state.emit();
  }

  //==========================================================================
  // events
  /** Records an event; returns true if the state has to be fetched again (the change was not made by this very session). */
  noteChanged(ev: ProjectChangedEvent, mySession: string): boolean {
    const own = ev.origin.kind === 'ui' && ev.origin.session === mySession;
    this.revision = Math.max(this.revision, ev.revision);

    if (isAgent(ev.origin)) {
      const at = Date.now();
      this.lastAgentAt = at;
      for (const c of ev.changes) {
        if (c.op === 'remove') {
          this.marks.delete(c.id);
          continue;
        }
        if (c.kind === 'item' || c.kind === 'track' || c.kind === 'plugin' || c.kind === 'marker') {
          this.marks.set(c.id, { session: ev.origin.session, label: ev.origin.label, at, revision: ev.revision, undoLabel: ev.undo_label, op: c.op });
        }
      }
      this.agentChanged.emit();
    }

    this.pushLog(ev);
    return !own;
  }

  private pushLog(ev: ProjectChangedEvent): void {
    const prefix = `${ev.origin.label}: `;
    const label = ev.undo_label.startsWith(prefix) ? ev.undo_label.slice(prefix.length) : ev.undo_label;
    const tool = label.split(' ')[0] ?? label;
    const fields = new Set<string>();
    for (const c of ev.changes) for (const f of c.fields ?? []) fields.add(f);
    const kinds = [...new Set(ev.changes.map((c) => c.kind))].join(', ');
    const rest = label.slice(tool.length).trim();
    const text = rest || `${kinds}${fields.size ? ': ' + [...fields].slice(0, 4).join(', ') : ''}`;
    this.log.unshift({ at: Date.now(), origin: ev.origin, tool, text, revision: ev.revision, ids: ev.changes.map((c) => c.id) });
    if (this.log.length > 200) this.log.length = 200;
    this.agentChanged.emit();
  }

  applyActivity(entries: ActivityEntry[]): void {
    const rows: LogEntry[] = [];
    for (const e of [...entries].reverse()) {
      if (e.ok === false || !e.revision || e.mutating === false) continue;
      const params = e.params && typeof e.params === 'object' ? JSON.stringify(e.params) : '';
      rows.push({ at: Date.parse(e.t), origin: e.origin, tool: e.name, text: params.length > 90 ? params.slice(0, 87) + '...' : params, revision: e.revision, ids: [] });
    }
    // events seen since the page opened are newer than the history
    this.log = [...this.log, ...rows.filter((r) => !this.log.some((l) => l.revision === r.revision))].slice(0, 200);
    this.agentChanged.emit();
  }

  accept(upTo: number): void {
    this.acceptedRevision = Math.max(this.acceptedRevision, upTo);
    this.pruneMarks();
    this.agentChanged.emit();
    this.state.emit();
  }

  private pruneMarks(): void {
    for (const [id, m] of this.marks) if (m.revision <= this.acceptedRevision) this.marks.delete(id);
  }

  /** The mark of a track: the track itself or one of its plugins (a fader change is reported as a plugin change). */
  trackMark(trackId: string): AgentMark | undefined {
    const own = this.marks.get(trackId);
    if (own) return own;
    for (const p of this.fx.get(trackId) ?? []) {
      const m = this.marks.get(p.plugin_id);
      if (m) return m;
    }
    return undefined;
  }

  itemMark(itemId: string): AgentMark | undefined {
    return this.marks.get(itemId);
  }

  setHistory(h: { undo: HistoryStep[]; redo: HistoryStep[] }): void {
    this.history = h.undo;
    this.canUndo = h.undo.length > 0;
    this.canRedo = h.redo.length > 0;
    this.agentChanged.emit();
  }

  /** How many of the newest undo steps were made by an agent, and whether a person's step sits among them (then none can be undone as a group). */
  agentUndo(): { count: number; blockedByPerson: boolean } {
    let count = 0;
    for (let i = this.history.length - 1; i >= 0; i--) {
      if (isAgent(this.history[i]!.origin)) count++;
      else break;
    }
    const newest = this.history[this.history.length - 1];
    const blocked = count === 0 && this.history.some((s) => isAgent(s.origin)) && newest !== undefined && !isAgent(newest.origin);
    return { count, blockedByPerson: blocked };
  }

  //==========================================================================
  // meters: peak with a decaying bar and a held peak
  setMeter(entityId: string, l: number, r: number, now: number): void {
    const prev = this.meters.get(entityId);
    const v: MeterValue = prev ?? { l: -100, r: -100, holdL: -100, holdR: -100, holdAt: 0, at: now };
    // the bar rises at once and falls at 36 dB/s, like a hardware ballistic
    const fall = (Math.max(0, now - v.at) / 1000) * 36;
    v.at = now;
    v.l = Math.max(l, v.l - fall);
    v.r = Math.max(r, v.r - fall);
    const peak = Math.max(l, r);
    if (peak >= Math.max(v.holdL, v.holdR) || now - v.holdAt > 1500) {
      v.holdL = l;
      v.holdR = r;
      v.holdAt = now;
    }
    this.meters.set(entityId, v);
  }

  clearMeters(): void {
    this.meters.clear();
  }

  isPlaying(): boolean {
    return this.transport.state === 'playing';
  }

  markersSeconds(): number[] {
    return this.markers.map((m) => m.time_s);
  }
}
