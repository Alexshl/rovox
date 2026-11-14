// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Helpers shared by the specs: opening the UI on seeded projects, waiting until it is settled, reading the clock and the audio stats.
import type { Locator, Page } from '@playwright/test';

export interface OpenOptions {
  /** Project ids to open as tabs. */
  open: string[];
  /** The active one (defaults to the first). */
  active?: string;
  theme?: 'dark' | 'light';
}

/** The UI keeps the open tabs, the active project and the preferences in localStorage; set them before the app starts, once per page. */
export async function openApp(page: Page, o: OpenOptions): Promise<void> {
  await page.addInitScript(
    ({ open, active, theme }) => {
      if (sessionStorage.getItem('e2e.init')) return;
      sessionStorage.setItem('e2e.init', '1');
      localStorage.setItem('rovox.locale', 'uk');
      localStorage.setItem('rovox.open', JSON.stringify(open));
      localStorage.setItem('rovox.active', active);
      localStorage.setItem('rovox.ui', JSON.stringify({ grid: '1/16', snap: true, mixerOpen: true, theme }));
    },
    { open: o.open, active: o.active ?? o.open[0], theme: o.theme ?? 'dark' },
  );
  await page.goto('/');
  await settle(page);
}

/** Fonts loaded, tabs rendered, nothing animating. */
export async function settle(page: Page): Promise<void> {
  await page.waitForSelector('[role=tab]');
  await page.evaluate(() => document.fonts.ready.then(() => undefined));
  await page.addStyleTag({ content: '*, *::before, *::after { animation: none !important; transition: none !important; caret-color: transparent !important; }' });
  await page.waitForTimeout(500);
}

/** Things that change between runs: the clock, the status text, the time of the activity log, toasts. */
export function volatile(page: Page): Locator[] {
  return ['.clock', '.status', '.toasts', '.mcp-panel .time', '.mcp-panel time', '.sync'].map((s) => page.locator(s));
}

export const audioButton = (page: Page): Locator => page.locator('button.audio');
export const playButton = (page: Page): Locator => page.getByRole('button', { name: /Відтворення/ });
export const stopButton = (page: Page): Locator => page.getByRole('button', { name: 'Стоп', exact: true });

/** The playhead in seconds, from the transport clock ("m:ss.mmm / m:ss.mmm"). */
export async function playheadS(page: Page): Promise<number> {
  const text = (await page.locator('.clock .small').first().textContent()) ?? '';
  return parseClock(text.split('/')[0]);
}

export function parseClock(s: string): number {
  const m = /(\d+):(\d+)\.(\d+)/.exec(s.trim());
  return m ? Number(m[1]) * 60 + Number(m[2]) + Number(m[3]) / 10 ** m[3].length : NaN;
}

/** Underruns the UI reports in the tooltip of the sound button ("Обриви звуку (underrun): N · ..."). Null while sound is off. */
export async function underruns(page: Page): Promise<number | null> {
  const title = (await audioButton(page).getAttribute('title')) ?? '';
  const m = /underrun\):\s*(\d+)/.exec(title);
  return m ? Number(m[1]) : null;
}

/** Clicks "Увімкнути звук" (the user gesture the autoplay policy wants) and waits until the stream is on. */
export async function enableSound(page: Page): Promise<void> {
  await audioButton(page).click();
  await page.waitForFunction(() => /underrun\)/.test(document.querySelector('button.audio')?.getAttribute('title') ?? ''), undefined, { timeout: 20_000 });
}
