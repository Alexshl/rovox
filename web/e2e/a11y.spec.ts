// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Accessibility smoke: a keyboard-only walk (menu -> tab -> transport -> a fader) and an axe-core scan of the main screen.
import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';
import { openApp } from './lib/app';
import { readSeed } from './seed';

test.beforeEach(async ({ page }) => {
  const seed = readSeed();
  await openApp(page, { open: [seed.demo.id, seed.second.id], active: seed.demo.id });
});

test('keyboard only: menu, tab, transport, fader', async ({ page }) => {
  // menu: open File with the keyboard, walk the items, close with Escape
  await page.getByRole('button', { name: 'Файл', exact: true }).focus();
  await page.keyboard.press('Enter');
  await expect(page.getByRole('menuitem').first()).toBeVisible();
  await page.keyboard.press('ArrowDown');
  await expect(page.getByRole('menuitem').first()).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('menuitem')).toHaveCount(0);

  // project tabs: roving focus with the arrows, selection follows the focus
  await page.getByRole('tab', { selected: true }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(page.getByRole('tab').nth(1)).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.getByRole('tab', { selected: true })).toContainText('e2e-second');

  // transport: buttons reachable by name and operable by Space
  const play = page.getByRole('button', { name: /Відтворення/ });
  await play.focus();
  await expect(play).toBeFocused();
  await page.keyboard.press('Space');
  await page.getByRole('button', { name: 'Стоп', exact: true }).focus();
  await page.keyboard.press('Enter');

  // a fader is a role=slider and moves with the arrows
  const fader = page.getByRole('slider').first();
  await fader.focus();
  const before = await fader.getAttribute('aria-valuenow');
  await page.keyboard.press('ArrowDown');
  await expect.poll(() => fader.getAttribute('aria-valuenow')).not.toBe(before);
});

test('axe-core: no serious or critical violations on the main screen', async ({ page }, info) => {
  const results = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa']).analyze();
  const bad = results.violations.filter((v) => v.impact === 'serious' || v.impact === 'critical');
  await info.attach('axe.json', { body: JSON.stringify(results.violations, null, 2), contentType: 'application/json' });
  expect(bad.map((v) => `${v.id}: ${v.help} (${v.nodes.length})`)).toEqual([]);
});
