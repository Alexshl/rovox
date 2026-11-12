// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Arrange view under load: 5000 items on 64 tracks, scroll and zoom for 5 s, frame times from requestAnimationFrame in the page.
import { expect, test } from '@playwright/test';
import { openApp } from './lib/app';
import { seedPerf } from './seed';
import { BASE_URL, TOKEN } from './lib/env';

test.setTimeout(300_000);

test('arrange with 5000 items / 64 tracks: >= 30 fps headless (regression gate; the 50 fps target is the owner run in real Chrome)', async ({ page }, info) => {
  test.info().annotations.push({ type: 'target', description: '50 fps in a real Chrome: E2E_HEADED=1 npx playwright test --project=chrome-headed perf' });
  const id = await seedPerf(BASE_URL, TOKEN);
  await openApp(page, { open: [id] });
  await expect(page.locator('.arrange canvas').first()).toBeVisible();

  await page.evaluate(() => {
    const w = window as any;
    w.__frames = [] as number[];
    w.__run = true;
    const tick = (t: number) => {
      w.__frames.push(t);
      if (w.__run) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  });

  const box = (await page.locator('.arrange').first().boundingBox())!;
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  const end = Date.now() + 5000;
  let n = 0;
  while (Date.now() < end) {
    // scroll right/down and back, with a zoom step (ctrl + wheel) every few events
    const dir = Math.floor(n / 10) % 2 === 0 ? 1 : -1;
    if (n % 6 === 5) {
      await page.keyboard.down('Control');
      await page.mouse.wheel(0, -120 * dir);
      await page.keyboard.up('Control');
    } else if (n % 2 === 0) await page.mouse.wheel(160 * dir, 0);
    else await page.mouse.wheel(0, 120 * dir);
    n++;
    await page.waitForTimeout(16);
  }

  const frames = await page.evaluate(() => {
    (window as any).__run = false;
    return (window as any).__frames as number[];
  });
  const dts = frames.slice(1).map((t, i) => t - frames[i]).sort((a, b) => a - b);
  const seconds = (frames[frames.length - 1] - frames[0]) / 1000;
  const fps = (frames.length - 1) / seconds;
  const p95 = dts[Math.floor(dts.length * 0.95)];
  const line = `perf [${info.project.name}]: ${fps.toFixed(1)} fps over ${seconds.toFixed(1)} s, frame p50 ${dts[Math.floor(dts.length / 2)].toFixed(1)} ms, p95 ${p95.toFixed(1)} ms, ${n} wheel events`;
  console.log(line);
  info.annotations.push({ type: 'fps', description: line });
  expect(fps, line).toBeGreaterThanOrEqual(30);
});
