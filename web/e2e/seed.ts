// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Deterministic seed over the MCP HTTP API. Every run deletes and recreates the e2e projects, with generated audio (fixed sine/noise/click
// WAVs, so the waveforms in the screenshots do not change), a fixed tempo map, markers and effects. The ids are written to .state/seed.json.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Mcp, uploadFolder, waitHealthy } from './lib/mcp';
import { clicks, noise, sine } from './lib/wav';

const here = dirname(fileURLToPath(import.meta.url));
export const STATE_FILE = join(here, '.state', 'seed.json');

export interface SeedState {
  base: string;
  token: string;
  demo: { id: string; name: string; tracks: { id: string; name: string }[] };
  second: { id: string; name: string };
  click: { id: string; name: string; bpm: number };
}

export const NAMES = { demo: 'e2e-demo', second: 'e2e-second', click: 'e2e-click', perf: 'e2e-perf' };

export function readSeed(): SeedState {
  return JSON.parse(readFileSync(STATE_FILE, 'utf8')) as SeedState;
}

async function deleteByName(mcp: Mcp, name: string): Promise<void> {
  const list = (await mcp.ok('project.list')).data.projects as { project_id: string; name: string }[];
  for (const p of list) if (p.name === name) await mcp.ok('project.delete', { project_id: p.project_id });
}

async function newProject(mcp: Mcp, name: string): Promise<string> {
  await deleteByName(mcp, name);
  return (await mcp.ok('project.new', { name })).data.project_id as string;
}

/** Uploads WAVs, plans the import (preset auto: one track per file) and applies it. */
export async function importAudio(mcp: Mcp, projectId: string, files: { relpath: string; data: Buffer }[]): Promise<void> {
  const folder = await uploadFolder(mcp, files);
  const plan = await mcp.ok('import.plan', { project_id: projectId, paths: [folder], preset: 'auto', takes_mode: 'separate' });
  await mcp.ok('import.apply', { project_id: projectId, plan_id: plan.data.plan_id });
}

const DEMO_TRACKS: [string, Buffer][] = [
  ['Drums', noise(16, 1, 0.5)],
  ['Bass', sine(55, 16, 0.6, 0.25)],
  ['Keys', sine(220, 16, 0.4, 0.5)],
  ['Pad', sine(330, 16, 0.35, 0.125)],
  ['Lead', sine(440, 16, 0.4, 1)],
  ['Vocal', sine(262, 16, 0.45, 0.75)],
  ['Guitar', sine(196, 16, 0.4, 0.33)],
  ['Percussion', noise(16, 7, 0.3, 90)],
];

async function seedDemo(mcp: Mcp): Promise<SeedState['demo']> {
  const id = await newProject(mcp, NAMES.demo);
  await importAudio(mcp, id, DEMO_TRACKS.map(([n, data], i) => ({ relpath: `${String(i + 1).padStart(2, '0')} ${n}.wav`, data })));
  await mcp.ok('tempo.set', { project_id: id, points: [{ bpm: 120 }, { bpm: 126, bar: 5 }] });
  await mcp.ok('marker.upsert', { project_id: id, markers: [{ name: 'Verse', time_s: 0 }, { name: 'Chorus', time_s: 8 }] });
  const tracks = (await mcp.ok('track.list', { project_id: id })).data.tracks as { track_id: string; name: string }[];
  const volumes = [-3, -6, -9, -12, -4.5, -7.5, -10, -15];
  await mcp.batch(id, tracks.map((t, i) => ({ tool: 'track.update', arguments: { track_id: t.track_id, volume_db: volumes[i % volumes.length], pan: [-0.5, 0, 0.3, -0.2, 0.6, 0, -0.4, 0.2][i % 8] } })), 'e2e mix');
  await mcp.ok('fx.add', { project_id: id, track_id: tracks[0].track_id, builtin: 'equaliser' });
  await mcp.ok('fx.add', { project_id: id, track_id: tracks[0].track_id, builtin: 'compressor' });
  await mcp.ok('fx.add', { project_id: id, track_id: tracks[2].track_id, builtin: 'reverb' });
  await mcp.ok('project.save', { project_id: id });
  return { id, name: NAMES.demo, tracks: tracks.map((t) => ({ id: t.track_id, name: t.name })) };
}

async function seedSecond(mcp: Mcp): Promise<SeedState['second']> {
  const id = await newProject(mcp, NAMES.second);
  // a different, constant tone: the tab-mute test looks for 880 Hz appearing and disappearing
  await importAudio(mcp, id, [{ relpath: 'Tone 880.wav', data: sine(880, 60, 0.5, 0) }]);
  await mcp.ok('project.save', { project_id: id });
  return { id, name: NAMES.second };
}

async function seedClick(mcp: Mcp, bpm: number): Promise<SeedState['click']> {
  const id = await newProject(mcp, NAMES.click);
  await importAudio(mcp, id, [{ relpath: 'Click.wav', data: clicks(120, bpm, 0.5) }]);
  await mcp.ok('tempo.set', { project_id: id, points: [{ bpm }] });
  await mcp.ok('project.save', { project_id: id });
  return { id, name: NAMES.click, bpm };
}

/** Two edits by an agent ("Claude Code"): the agent highlight, the MCP panel log and the undo / accept bar need content. Call right before a screenshot. */
export async function agentEdits(base: string, token: string, demo: SeedState['demo']): Promise<void> {
  const agent = await new Mcp(base, token, 'Claude Code').init();
  await agent.ok('agent.accept', { project_id: demo.id });
  await agent.ok('track.update', { project_id: demo.id, track_id: demo.tracks[1].id, volume_db: -4, undo_label: 'Bass louder' });
  await agent.ok('marker.upsert', { project_id: demo.id, markers: [{ name: 'Bridge', time_s: 12 }], undo_label: 'Add Bridge marker' });
}

/** 64 tracks, 5000 MIDI items (cheap to create, no audio needed): the arrange view stress case. */
export async function seedPerf(base: string, token: string): Promise<string> {
  const mcp = await new Mcp(base, token, 'e2e-perf').init();
  const id = await newProject(mcp, NAMES.perf);
  const tracks: string[] = [];
  for (let i = 0; i < 64; i += 16) {
    await mcp.batch(id, Array.from({ length: 16 }, (_, k) => ({ tool: 'track.create', arguments: { name: `T${String(i + k + 1).padStart(2, '0')}` } })));
  }
  for (const t of (await mcp.ok('track.list', { project_id: id })).data.tracks as { track_id: string }[]) tracks.push(t.track_id);
  const ops: { tool: string; arguments: Record<string, unknown> }[] = [];
  for (let n = 0; n < 5000; n++) ops.push({ tool: 'item.create_midi', arguments: { track_id: tracks[n % 64], start_s: Math.floor(n / 64) * 2, length_s: 1.5, name: `i${n}` } });
  for (let i = 0; i < ops.length; i += 500) await mcp.batch(id, ops.slice(i, i + 500));
  return id;
}

export async function seedAll(base: string, token: string): Promise<SeedState> {
  await waitHealthy(base);
  const mcp = await new Mcp(base, token, 'e2e-seed').init();
  const demo = await seedDemo(mcp);
  const second = await seedSecond(mcp);
  const click = await seedClick(mcp, 120);
  const state: SeedState = { base, token, demo, second, click };
  mkdirSync(dirname(STATE_FILE), { recursive: true });
  writeFileSync(STATE_FILE, JSON.stringify(state, null, 2));
  return state;
}

