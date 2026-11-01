// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { ProjectStore } from '../src/store/project';
import type { ProjectChangedEvent, ProjectGet } from '../src/net/protocol';

const base: ProjectGet = {
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
    { track_id: 'trk_b', index: 1, name: 'Bass', color: '#ffb627', mute: false, solo: false, volume_db: 0, pan: 0 },
  ],
  items: [
    { item_id: 'itm_1', track_id: 'trk_a', name: 'flute.wav', start_s: 4, length_s: 6, end_s: 10, muted: false, offset_s: 0 },
    { item_id: 'itm_2', track_id: 'trk_b', name: 'bass.wav', start_s: 0, length_s: 30, end_s: 30, muted: false, offset_s: 0 },
  ],
  markers: [],
  tempo: { points: [{ bpm: 118, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } },
  fx: { trk_a: [{ plugin_id: 'fx_vol', name: 'Volume', type: 'volume', enabled: true }], trk_b: [] },
};

const event = (over: Partial<ProjectChangedEvent>): ProjectChangedEvent => ({
  t: 'event',
  type: 'project.changed',
  project_id: 'p_1',
  revision: 11,
  origin: { kind: 'mcp', session: 'mcp_1', label: 'Claude' },
  undo_label: 'Claude: item.split flute',
  changes: [{ kind: 'item', id: 'itm_1', op: 'update', fields: ['length'] }],
  ...over,
});

describe('project store', () => {
  it('builds the model from project.get and indexes items per track', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    expect(s.tracks).toHaveLength(2);
    expect(s.tempo.points[0]!.bpm).toBe(118);
    expect(s.index('trk_a')!.query(0, 5).map((i) => i.item_id)).toEqual(['itm_1']);
    expect(s.loaded).toBe(true);
  });

  it('marks what an agent touched in agent-cyan terms and asks for a refetch', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    const refetch = s.noteChanged(event({}), 'ws_9');
    expect(refetch).toBe(true);
    expect(s.itemMark('itm_1')?.label).toBe('Claude');
    expect(s.itemMark('itm_2')).toBeUndefined();
    expect(s.log[0]!.tool).toBe('item.split');
  });

  it('a fader change by an agent (reported as a plugin change) marks the track', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    s.noteChanged(event({ changes: [{ kind: 'plugin', id: 'fx_vol', op: 'update', fields: ['volume'] }] }), 'ws_9');
    expect(s.trackMark('trk_a')).toBeDefined();
    expect(s.trackMark('trk_b')).toBeUndefined();
  });

  it('own UI edits are not refetched and are not marked', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    const refetch = s.noteChanged(event({ origin: { kind: 'ui', session: 'ws_9', label: 'ui' } }), 'ws_9');
    expect(refetch).toBe(false);
    expect(s.marks.size).toBe(0);
    expect(s.revision).toBe(11);
  });

  it('accept clears the marks up to a revision and keeps newer ones', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    s.noteChanged(event({ revision: 11 }), 'ws_9');
    s.noteChanged(event({ revision: 12, changes: [{ kind: 'item', id: 'itm_2', op: 'update' }] }), 'ws_9');
    s.accept(11);
    expect(s.itemMark('itm_1')).toBeUndefined();
    expect(s.itemMark('itm_2')).toBeDefined();
    s.accept(12);
    expect(s.marks.size).toBe(0);
  });

  it('a removed entity loses its mark', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    s.noteChanged(event({}), 'ws_9');
    s.noteChanged(event({ revision: 12, changes: [{ kind: 'item', id: 'itm_1', op: 'remove' }] }), 'ws_9');
    expect(s.itemMark('itm_1')).toBeUndefined();
  });

  it('Undo agent counts the agent steps on top of the stack and stops at a person', () => {
    const s = new ProjectStore('p_1');
    const step = (kind: 'mcp' | 'ui', rev: number) => ({ revision: rev, label: 'x', time: '', origin: { kind, session: 's', label: kind } });
    s.setHistory({ undo: [step('ui', 1), step('mcp', 2), step('mcp', 3), step('mcp', 4)], redo: [] });
    expect(s.agentUndo()).toEqual({ count: 3, blockedByPerson: false });
    s.setHistory({ undo: [step('mcp', 1), step('mcp', 2), step('ui', 3)], redo: [] });
    expect(s.agentUndo()).toEqual({ count: 0, blockedByPerson: true });
    s.setHistory({ undo: [step('ui', 1)], redo: [] });
    expect(s.agentUndo()).toEqual({ count: 0, blockedByPerson: false });
  });

  it('optimistic item edits re-index', () => {
    const s = new ProjectStore('p_1');
    s.applyProjectGet(base);
    s.patchItem('itm_1', { start_s: 20 });
    expect(s.items.get('itm_1')!.end_s).toBe(26);
    expect(s.index('trk_a')!.query(0, 5)).toHaveLength(0);
    expect(s.index('trk_a')!.query(21, 22)).toHaveLength(1);
  });

  it('meters keep a peak hold that releases after 1.5 s', () => {
    const s = new ProjectStore('p_1');
    s.setMeter('trk_a', -6, -7, 1000);
    s.setMeter('trk_a', -20, -20, 1100);
    expect(s.meters.get('trk_a')!.holdL).toBe(-6);
    s.setMeter('trk_a', -20, -20, 3000);
    expect(s.meters.get('trk_a')!.holdL).toBe(-20);
  });
});
