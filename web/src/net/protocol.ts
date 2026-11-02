// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Wire types of the Rovox WebSocket (`/ws`) and the command results the UI uses. The command names and parameters are the
// MCP ones (command layer); nothing here is a second model of the project.

export interface Origin {
  kind: 'ui' | 'mcp' | 'cli' | 'system' | 'skill';
  session: string;
  label: string;
}

export interface TrackState {
  track_id: string;
  index: number;
  name: string;
  color: string;
  mute: boolean;
  solo: boolean;
  volume_db?: number;
  pan?: number;
  item_count?: number;
}

export interface ItemState {
  item_id: string;
  track_id: string;
  name: string;
  start_s: number;
  length_s: number;
  end_s: number;
  muted: boolean;
  file?: string;
  offset_s: number;
  fade_in_s?: number;
  fade_out_s?: number;
  gain_db?: number;
  start_bar_beat?: string;
}

export interface MarkerState {
  marker_id: string;
  name: string;
  time_s: number;
  bar_beat?: string;
}

export interface TempoPointState {
  bpm: number;
  time_s: number;
  qn: number;
  bar_beat?: string;
}

export interface PluginState {
  plugin_id: string;
  name: string;
  type: string;
  enabled: boolean;
  loaded?: boolean;
}

export interface MasterState {
  track_id: 'master';
  volume_db?: number;
  pan?: number;
}

/** Where the project is stored and how far the bucket is behind (`project.get`, `storage.changed`). `local` = no object storage. */
export interface StorageInfo {
  backend: 'local' | 's3';
  synced_revision: number | null;
  pending_uploads: number;
  last_error: string | null;
  lease?: 'held' | 'lost' | 'none';
}

/** Result of the `project.get` command. */
export interface ProjectGet {
  project_id: string;
  name: string;
  revision: number;
  sample_rate: number;
  length_s: number;
  can_undo: boolean;
  can_redo: boolean;
  dirty: boolean;
  storage?: StorageInfo;
  tracks?: TrackState[];
  master?: MasterState;
  items?: ItemState[];
  markers?: MarkerState[];
  tempo?: { points: TempoPointState[]; time_signature: { numerator: number; denominator: number } };
  fx?: Record<string, PluginState[]>;
}

export interface TransportState {
  state: 'playing' | 'stopped';
  position_s: number;
  epoch: number;
  loop: { enabled: boolean; start_s: number; end_s: number };
  length_s: number;
  audio: string;
}

export interface ChangedEntity {
  kind: string;
  id: string;
  op: 'add' | 'update' | 'remove';
  fields?: string[];
}

export interface HistoryStep {
  revision: number;
  label: string;
  time: string;
  origin: Origin;
}

export interface ActivityEntry {
  t: string;
  origin: Origin;
  name: string;
  params?: unknown;
  ok?: boolean;
  revision?: number;
  mutating?: boolean;
}

export interface ServerInfo {
  version: string;
  source_url: string;
  audio: { mode: string; available: boolean; sample_rate?: number; frame_samples?: number };
}

export type ServerMessage =
  | { t: 'welcome'; session: string; server_version: string; audio: ServerInfo['audio']; source_url?: string }
  | { t: 'snapshot'; project_id: string; pidx: number; revision: number; state: ProjectGet; transport: TransportState; meter_index: string[]; accepted_revision: number }
  | { t: 'result'; id: string; ok: boolean; revision?: number; data?: unknown; error?: { code: string; message: string; details?: unknown } }
  | ({ t: 'event'; type: string } & Record<string, unknown>)
  | { t: 'resync'; project_id: string }
  | { t: 'error'; message: string; project_id?: string; code?: string }
  | { t: 'pong'; n: number }
  | { t: 'stats'; [k: string]: unknown };

export interface ProjectChangedEvent {
  t: 'event';
  type: 'project.changed';
  project_id: string;
  revision: number;
  origin: Origin;
  undo_label: string;
  changes: ChangedEntity[];
}

//==============================================================================
// Binary frames (little endian)

export interface MeterFrame {
  pidx: number;
  entries: { index: number; l: number; r: number }[];
}

export interface PositionFrame {
  pidx: number;
  epoch: number;
  position_s: number;
  playing: boolean;
}

export interface AudioPacket {
  discontinuity: boolean;
  epoch: number;
  seq: number;
  samplePos: number;
  serverTimeUs: number;
  opus: Uint8Array;
}

export const FRAME_AUDIO = 1;
export const FRAME_METERS = 2;
export const FRAME_POSITION = 3;
export const AUDIO_HEADER_BYTES = 28;

export function frameType(buf: ArrayBuffer): number {
  return buf.byteLength > 0 ? new Uint8Array(buf)[0]! : 0;
}

export function parseMeters(buf: ArrayBuffer): MeterFrame | null {
  if (buf.byteLength < 4) return null;
  const v = new DataView(buf);
  if (v.getUint8(0) !== FRAME_METERS) return null;
  const count = v.getUint16(2, true);
  if (buf.byteLength < 4 + count * 12) return null;
  const entries: MeterFrame['entries'] = [];
  for (let i = 0; i < count; i++) {
    const o = 4 + i * 12;
    entries.push({ index: v.getUint32(o, true), l: v.getFloat32(o + 4, true), r: v.getFloat32(o + 8, true) });
  }
  return { pidx: v.getUint8(1), entries };
}

export function parsePosition(buf: ArrayBuffer): PositionFrame | null {
  if (buf.byteLength < 13) return null;
  const v = new DataView(buf);
  if (v.getUint8(0) !== FRAME_POSITION) return null;
  return { pidx: v.getUint8(1), epoch: v.getUint16(2, true), position_s: v.getFloat64(4, true), playing: v.getUint8(12) === 1 };
}

export function parseAudio(buf: ArrayBuffer): AudioPacket | null {
  if (buf.byteLength <= AUDIO_HEADER_BYTES) return null;
  const v = new DataView(buf);
  if (v.getUint8(0) !== FRAME_AUDIO) return null;
  return {
    discontinuity: (v.getUint8(1) & 1) === 1,
    epoch: v.getUint16(4, true),
    seq: v.getUint32(8, true),
    samplePos: Number(v.getBigUint64(12, true)),
    serverTimeUs: Number(v.getBigUint64(20, true)),
    opus: new Uint8Array(buf, AUDIO_HEADER_BYTES),
  };
}
