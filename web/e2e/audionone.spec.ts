// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// A server started with `--audio none` (E2E_NOAUDIO_URL): the UI loads and Play renders a 30 s preview that an <audio> element plays.
import { expect, test } from '@playwright/test';
import { audioButton, playButton, stopButton } from './lib/app';
import { NOAUDIO_URL, TOKEN } from './lib/env';
import { Mcp, waitHealthy } from './lib/mcp';
import { importAudio } from './seed';
import { sine } from './lib/wav';

test.use({ baseURL: NOAUDIO_URL, storageState: { cookies: [], origins: [] } });

test('--audio none: Play prepares a preview and an audio element plays it', async ({ page }) => {
  await waitHealthy(NOAUDIO_URL);
  const mcp = await new Mcp(NOAUDIO_URL, TOKEN, 'e2e-noaudio').init();
  const list = (await mcp.ok('project.list')).data.projects as { project_id: string; name: string }[];
  for (const p of list) if (p.name === 'e2e-noaudio') await mcp.ok('project.delete', { project_id: p.project_id });
  const id = (await mcp.ok('project.new', { name: 'e2e-noaudio' })).data.project_id as string;
  await importAudio(mcp, id, [{ relpath: 'Tone.wav', data: sine(330, 40, 0.4) }]);

  // the preview is an `new Audio(...)` that never enters the DOM: record the elements whose play() is called
  await page.addInitScript(
    ({ open, active }) => {
      localStorage.setItem('rovox.locale', 'uk');
      localStorage.setItem('rovox.open', JSON.stringify(open));
      localStorage.setItem('rovox.active', active);
      const w = window as any;
      w.__plays = [] as HTMLMediaElement[];
      const play = HTMLMediaElement.prototype.play;
      HTMLMediaElement.prototype.play = function (this: HTMLMediaElement) {
        w.__plays.push(this);
        return play.call(this);
      };
    },
    { open: [id], active: id },
  );
  await page.goto(`/?token=${encodeURIComponent(TOKEN)}`);
  await page.waitForSelector('[role=tab]');

  await expect(audioButton(page)).toHaveText('Звук: прослуховування');
  await playButton(page).click();
  await page.waitForFunction(() => (window as any).__plays.length > 0, undefined, { timeout: 60_000 });
  const state = await page.evaluate(async () => {
    const a = (window as any).__plays[0] as HTMLMediaElement;
    // the first start of an audio output can take a moment (Firefox): wait for the clock to move rather than for a fixed time
    for (let i = 0; i < 100 && a.currentTime <= 0.5 && !a.error; i++) await new Promise((r) => setTimeout(r, 100));
    return { src: a.currentSrc || a.src, paused: a.paused, currentTime: a.currentTime, error: a.error?.code ?? null };
  });
  expect(state.src).toContain('/api/projects/');
  expect(state.error).toBeNull();
  expect(state.paused).toBe(false);
  expect(state.currentTime).toBeGreaterThan(0.5);
  await stopButton(page).click();
});
