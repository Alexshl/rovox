// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Screenshots of the screens (desktop 1440x900 in the chromium and firefox projects, 390x844 in the phone project), compared with the reference
// images of the same container. The references are NOT in git until the owner approves them (see README.md): without them these tests skip.
import { expect, test, type Page, type TestInfo } from '@playwright/test';
import { existsSync, mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { openApp, settle, volatile } from './lib/app';
import { sine } from './lib/wav';
import { agentEdits, readSeed } from './seed';

const here = dirname(fileURLToPath(import.meta.url));

function hasBaselines(info: TestInfo): boolean {
  if (info.config.updateSnapshots !== 'none') return true; // generating them
  return existsSync(join(here, '__screenshots__', `${info.project.name}-${process.platform}`));
}

test.beforeEach(({}, info) => {
  test.skip(!hasBaselines(info), 'no reference screenshots yet: generate them with --update-snapshots and have the owner approve them (web/e2e/README.md)');
});

const phone = (info: TestInfo) => info.project.name === 'phone';

async function shot(page: Page, name: string, target?: string): Promise<void> {
  await settle(page);
  const opts = { mask: volatile(page), maxDiffPixelRatio: 0.01 };
  if (target) await expect(page.locator(target).first()).toHaveScreenshot(`${name}.png`, opts);
  else await expect(page).toHaveScreenshot(`${name}.png`, opts);
}

async function openMenu(page: Page, title: string, item: string): Promise<void> {
  await page.getByRole('button', { name: title, exact: true }).click();
  await page.getByRole('menuitem', { name: item }).click();
}

for (const theme of ['dark', 'light'] as const) {
  test.describe(`desktop ${theme}`, () => {
    test.beforeEach(async ({ page }, info) => {
      test.skip(phone(info), 'desktop only');
      const seed = readSeed();
      await agentEdits(seed.base, seed.token, seed.demo);
      await openApp(page, { open: [seed.demo.id, seed.second.id], active: seed.demo.id, theme });
      await expect(page.locator('.mcp-panel')).toContainText('Claude Code');
    });

    test('arrange view', async ({ page }) => {
      await shot(page, `arrange-${theme}`);
    });

    test('mixer', async ({ page }) => {
      await shot(page, `mixer-${theme}`, '.mixer');
    });

    test('MCP panel with agent highlights', async ({ page }) => {
      await shot(page, `mcp-panel-${theme}`, '.mcp-panel');
    });

    test('FX panel popover, plugin expanded', async ({ page }) => {
      await page.locator('.sbtn.fx').first().click();
      await page.locator('.popover .fxname').first().click();
      await shot(page, `fx-popover-${theme}`, '.popover');
    });

    test('project picker', async ({ page }) => {
      await openMenu(page, 'Файл', 'Відкрити проєкт…');
      await shot(page, `project-picker-${theme}`, '.scrim');
    });

    test('render dialog', async ({ page }) => {
      await openMenu(page, 'Файл', 'Рендер…');
      await shot(page, `render-dialog-${theme}`, '.scrim');
    });

    test('import wizard: step 1', async ({ page }) => {
      await openMenu(page, 'Файл', 'Імпорт аудіо…');
      await shot(page, `import-step1-${theme}`, '.import-dialog');
    });

    // the import creates a throwaway project that is never applied (the e2e volume is disposable)
    test('import wizard: plan step (small real upload)', async ({ page }) => {
      const dir = mkdtempSync(join(tmpdir(), 'e2e-import-'));
      const song = join(dir, 'Song');
      mkdirSync(song);
      writeFileSync(join(song, 'Guitar.wav'), sine(196, 2));
      writeFileSync(join(song, 'Bass.wav'), sine(55, 2));
      await openMenu(page, 'Файл', 'Імпорт аудіо…');
      await page.locator('.import-dialog input[webkitdirectory]').setInputFiles(song);
      await expect(page.locator('.import-dialog')).toContainText('3/3', { timeout: 60_000 });
      await shot(page, `import-plan-${theme}`, '.import-dialog');
    });
  });
}

test.describe('phone layout', () => {
  test.beforeEach(async ({ page }, info) => {
    test.skip(!phone(info), 'phone project only');
    const seed = readSeed();
    await agentEdits(seed.base, seed.token, seed.demo);
    await openApp(page, { open: [seed.demo.id], active: seed.demo.id });
  });

  test('main screen', async ({ page }) => {
    await shot(page, 'phone-main');
  });

  test('menu sheet', async ({ page }) => {
    await page.getByRole('button', { name: 'Меню', exact: true }).click();
    await shot(page, 'phone-menu-sheet');
  });

  test('mixer sheet', async ({ page }) => {
    await page.getByRole('button', { name: /Мікшер/ }).first().click();
    await shot(page, 'phone-mixer-sheet');
  });

  test('agents drawer', async ({ page }) => {
    await page.getByRole('button', { name: /Агенти/ }).first().click();
    await shot(page, 'phone-agents-drawer');
  });
});
