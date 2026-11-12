// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The worklet's 30 Hz statistics must not re-render the whole shell: they only refresh the audio button.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));

beforeEach(() => {
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
  document.body.innerHTML = '';
  localStorage.clear();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('audio statistics', () => {
  it('do not trigger shell.render, only the audio button follows them', async () => {
    const { App } = await import('../src/store/session');
    const { Shell } = await import('../src/views/shell');
    const app = new App('ws://test/ws');
    const render = vi.spyOn(Shell.prototype as unknown as { render(): void }, 'render');
    const shell = new Shell(app);
    document.body.append(shell.el);
    await tick();
    render.mockClear();

    const audio = app.audio as unknown as { onWorkletMessage(m: Record<string, unknown>): void };
    app.audio.stats.enabled = true;
    app.audio.stats.supported = true;
    app.serverInfo = { ...app.serverInfo, audio: { mode: 'browser-stream', available: true } };
    const button = document.querySelector('.tbtn.audio')!;
    for (let i = 0; i < 90; i++) audio.onWorkletMessage({ type: 'stats', t: i * 0.033, buffered: 7200 + i, underruns: 0, playing: true, pos: i * 1584, newUnderrun: false });
    await tick();

    expect(render).not.toHaveBeenCalled();
    expect(button.getAttribute('title')).toContain('буфер');
    expect(button.textContent).toBe('Звук увімкнено');
  });
});
