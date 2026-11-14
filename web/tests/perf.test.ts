// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// P5 (task 310): 5000 items on 64 tracks. Drawing is virtualised (only the visible tracks and the items of the visible time window), so
// the cost of a frame does not grow with the project. node-canvas is a software rasteriser, much slower than a browser's GPU canvas:
// the bound here is generous; the number printed is the figure to compare between versions.
import { describe, expect, it, vi } from 'vitest';

let hasCanvas = false;
try {
  await import('canvas');
  hasCanvas = true;
} catch (e) {
  // the Docker web stage sets ROVOX_REQUIRE_CANVAS=1: a missing node-canvas fails the run there instead of skipping the test
  if (process.env.ROVOX_REQUIRE_CANVAS === '1') throw new Error(`ROVOX_REQUIRE_CANVAS=1 but node-canvas does not load: ${e instanceof Error ? e.message : String(e)}`);
}

// E5-02 asks for >= 50 fps in Chrome, i.e. 20 ms per frame. This test draws with node-canvas (a software rasteriser, no GPU) inside
// jsdom, which is several times slower than Chrome's canvas, so the bound is 3x the target: 60 ms per frame (~16 fps here). A frame that
// costs more than that is an algorithmic regression (the drawing is virtualised), not a slow rasteriser; the number printed is the
// figure to compare between versions.
// ROVOX_PERF_BUDGET_MS: the Docker web stage shares the CPU with the C++ compile, so it only guards against gross regressions there
const FRAME_BUDGET_MS = Number(process.env.ROVOX_PERF_BUDGET_MS ?? '') || 20 * 3;

describe.skipIf(!hasCanvas)('arrange performance', () => {
  it('draws a frame of a 5000 item, 64 track project quickly and queries the index in microseconds', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
    // real peak data (int8 min/max pairs) for every item, so the waveforms are drawn and part of the frame cost
    vi.stubGlobal('fetch', vi.fn(async (url: string) => {
      const bins = Number(/bins=(\d+)/.exec(String(url))?.[1] ?? 8);
      const data = new Int8Array(bins * 2);
      for (let i = 0; i < bins; i++) {
        const a = Math.round(100 * Math.abs(Math.sin(i / 11)) * (0.4 + 0.6 * Math.abs(Math.sin(i / 70))));
        data[i * 2] = -a;
        data[i * 2 + 1] = a;
      }
      return { ok: true, headers: { get: (k: string) => (k === 'X-Rovox-T0' ? '0' : k === 'X-Rovox-T1' ? '4.5' : null) }, arrayBuffer: async () => data.buffer };
    }));
    Object.defineProperty(HTMLCanvasElement.prototype, 'clientWidth', { configurable: true, get: () => 1200 });
    Object.defineProperty(HTMLCanvasElement.prototype, 'clientHeight', { configurable: true, get: () => 640 });
    const { App } = await import('../src/store/session');
    const { ProjectStore } = await import('../src/store/project');
    const { ArrangeView } = await import('../src/views/arrange');

    const app = new App('ws://test/ws');
    const store = new ProjectStore('p_big');
    const tracks = Array.from({ length: 64 }, (_, i) => ({ track_id: `trk_${i}`, index: i, name: `Track ${i + 1}`, color: '#8fb3a3', mute: false, solo: false, volume_db: 0, pan: 0 }));
    const items = Array.from({ length: 5000 }, (_, i) => {
      const t = i % 64;
      const n = Math.floor(i / 64);
      return { item_id: `itm_${i}`, track_id: `trk_${t}`, name: `clip ${i}.wav`, start_s: n * 5, length_s: 4.5, end_s: n * 5 + 4.5, muted: false, offset_s: 0, gain_db: 0 };
    });
    store.applyProjectGet({ project_id: 'p_big', name: 'big', revision: 1, sample_rate: 44100, length_s: 400, can_undo: false, can_redo: false, dirty: false, tracks, items, markers: [],
      tempo: { points: [{ bpm: 120, time_s: 0, qn: 0 }], time_signature: { numerator: 4, denominator: 4 } }, fx: {} });
    app.projects.set('p_big', store);
    app.order = ['p_big'];
    app.activeId = 'p_big';

    const view = new ArrangeView(app);
    document.body.append(view.el);
    view.sync();
    store.view.pps = 20;
    store.view.start = 100;

    // the index: windows of 60 s on every track
    const t0 = performance.now();
    let n = 0;
    for (let k = 0; k < 64 * 50; k++) n += store.index(`trk_${k % 64}`)!.query(100 + (k % 200), 160 + (k % 200)).length;
    const perQuery = (performance.now() - t0) / (64 * 50);
    console.log(`index query: ${(perQuery * 1000).toFixed(1)} us per track window (${n} hits)`);
    expect(perQuery).toBeLessThan(0.5);

    // only the visible window is drawn: ~10 tracks x 60 s / 5 s per item
    expect(view.visibleItemCount()).toBeLessThan(64 * 14);

    // a frame: the first paint starts the peak fetches, the wait lets them land, so the measured frames draw the waveforms
    (view as unknown as { paint(): void }).paint();
    await new Promise((r) => setTimeout(r, 200));
    (view as unknown as { paint(): void }).paint();
    // the median frame: a shared CI runner's GC pauses or noisy neighbours spike single frames, a slow code path moves them all
    const frames = 21;
    const times: number[] = [];
    for (let i = 0; i < frames; i++) {
      store.view.start = 100 + i * 3;
      const f0 = performance.now();
      (view as unknown as { paint(): void }).paint();
      times.push(performance.now() - f0);
    }
    times.sort((x, y) => x - y);
    const perFrame = times[Math.floor(frames / 2)]!;
    console.log(`paint: ${perFrame.toFixed(1)} ms per frame (node-canvas, 1200x640, ${items.length} items in the project)`);
    expect(perFrame).toBeLessThan(FRAME_BUDGET_MS);
  });
});
