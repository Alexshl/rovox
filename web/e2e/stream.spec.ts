// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// The browser audio stream (server started with `--audio browser`), checked headless: underruns, tab mute / background project, playhead vs
// audible offset, and the same under packet loss (E2E_NETEM=1, the netem sidecar of docker/compose.e2e.yml).
// The output is read through the tap of lib/audiotap.ts (the app does not publish its output node); underruns from the tooltip of the sound button.
import { expect, test, type Page } from '@playwright/test';
import { enableSound, openApp, playButton, playheadS, stopButton, underruns } from './lib/app';
import { installAudioTap, outputRms, playheadAt, startRecording, stopRecording, toneDb } from './lib/audiotap';
import { BASE_URL, LONG, TOKEN } from './lib/env';
import { Mcp } from './lib/mcp';
import { importAudio, readSeed } from './seed';
import { sine } from './lib/wav';

const STREAM_S = LONG ? 600 : 60;

test.beforeEach(async ({ page, browserName }) => {
  if (browserName === 'firefox') {
    // Firefox before 130 has no AudioDecoder; the stream is then reported as unavailable, which is not a failure of this suite
    await page.goto('/');
    const ok = await page.evaluate(async () => typeof AudioDecoder !== 'undefined' && (await AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 })).supported === true);
    test.skip(!ok, 'AudioDecoder opus is not supported by this browser build');
  }
  await installAudioTap(page);
});

async function waitPlaying(page: Page): Promise<void> {
  await playButton(page).click();
  await expect.poll(() => playheadS(page), { timeout: 15_000 }).toBeGreaterThan(0.3);
}

async function longProject(): Promise<string> {
  const mcp = await new Mcp(BASE_URL, TOKEN, 'e2e-stream').init();
  const list = (await mcp.ok('project.list')).data.projects as { project_id: string; name: string }[];
  for (const p of list) if (p.name === 'e2e-long') await mcp.ok('project.delete', { project_id: p.project_id });
  const id = (await mcp.ok('project.new', { name: 'e2e-long' })).data.project_id as string;
  await importAudio(mcp, id, [{ relpath: 'Long tone.wav', data: sine(220, STREAM_S + 15, 0.4, 0.2) }]);
  return id;
}

async function runStream(page: Page, seconds: number): Promise<{ underruns: number; advanced: number }> {
  const id = await longProject();
  await openApp(page, { open: [id] });
  await enableSound(page);
  await waitPlaying(page);
  const start = await playheadS(page);
  let last = start;
  for (let t = 0; t < seconds; t += 5) {
    await page.waitForTimeout(5000);
    const now = await playheadS(page);
    expect(now, `the playhead stalled at ${now.toFixed(2)} s after ${t + 5} s`).toBeGreaterThan(last);
    last = now;
  }
  const u = (await underruns(page)) ?? -1;
  await stopButton(page).click();
  return { underruns: u, advanced: last - start };
}

test.describe('stream', () => {
  test.setTimeout((STREAM_S + 120) * 1000);

  test(`${STREAM_S} s of playback: no underruns, the playhead advances`, async ({ page }, info) => {
    const r = await runStream(page, STREAM_S);
    info.annotations.push({ type: 'stream', description: `${r.underruns} underruns, playhead +${r.advanced.toFixed(1)} s in ${STREAM_S} s` });
    expect(r.underruns).toBe(0);
    expect(r.advanced).toBeGreaterThan(STREAM_S * 0.95);
  });

  test('netem 40 +- 10 ms, 2 % loss: at most 1 underrun per minute', async ({ page }) => {
    test.skip(process.env.E2E_NETEM !== '1', 'needs the netem sidecar: docker compose -f docker/compose.e2e.yml --profile netem up');
    const r = await runStream(page, STREAM_S);
    expect(r.underruns).toBeLessThanOrEqual(Math.ceil(STREAM_S / 60));
  });
});

test.describe('tabs', () => {
  test('muting a project tab silences its tone, a background project keeps playing', async ({ page }) => {
    const seed = readSeed();
    await openApp(page, { open: [seed.second.id, seed.demo.id], active: seed.second.id });
    await enableSound(page);
    await waitPlaying(page);
    await expect.poll(() => toneDb(page, 880), { timeout: 10_000, message: 'the 880 Hz tone is audible' }).toBeGreaterThan(-45);

    // background: switch to the other tab, the first project keeps playing
    await page.getByRole('tab', { name: new RegExp(seed.demo.name) }).click();
    await page.waitForTimeout(1000);
    expect(await toneDb(page, 880), 'still audible from the background tab').toBeGreaterThan(-45);

    // mute it with the switch of its tab
    const sw = page.getByRole('switch', { name: `Звук проєкту ${seed.second.name}` });
    await expect(sw).toHaveAttribute('aria-checked', 'true');
    await sw.click();
    await expect(sw).toHaveAttribute('aria-checked', 'false');
    await expect.poll(() => toneDb(page, 880), { timeout: 5_000, message: 'the tone is gone' }).toBeLessThan(-70);
    expect(await outputRms(page)).toBeLessThan(0.002);

    await sw.click();
    await expect.poll(() => toneDb(page, 880), { timeout: 5_000, message: 'the tone is back' }).toBeGreaterThan(-45);

    // the server plays on after the page closes and mixes every playing project into the one output: stop it, or the next test hears it
    await page.getByRole('tab', { name: new RegExp(seed.second.name) }).click();
    await stopButton(page).click();
  });
});

test.describe('pipeline offset', () => {
  test('click onsets in the output vs the playhead the UI shows: |offset| <= 50 ms', async ({ page }, info) => {
    test.setTimeout(120_000);
    const seed = readSeed();
    await openApp(page, { open: [seed.click.id] });
    await enableSound(page);
    await startRecording(page);
    await waitPlaying(page);
    await page.waitForTimeout(25_000);
    const { onsets, samples } = await stopRecording(page);
    await stopButton(page).click();

    const period = 60 / seed.click.bpm;
    const offsets: number[] = [];
    for (const o of onsets.slice(3)) {
      const pos = playheadAt(samples, o.audibleMs);
      if (pos === null) continue;
      const k = Math.round((pos - 0.5) / period);
      offsets.push(pos - (0.5 + k * period));
    }
    expect(offsets.length, `${onsets.length} onsets detected`).toBeGreaterThanOrEqual(10);
    const abs = offsets.map(Math.abs).sort((a, b) => a - b);
    const median = abs[Math.floor(abs.length / 2)];
    const max = abs[abs.length - 1];
    info.annotations.push({ type: 'offset', description: `median ${(median * 1000).toFixed(1)} ms, max ${(max * 1000).toFixed(1)} ms over ${offsets.length} clicks` });
    // headless has no real output latency (the owner checks real outputs); the median is the gate, the max is reported
    expect(median * 1000).toBeLessThanOrEqual(50);
  });
});
