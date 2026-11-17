// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Project folders (E12): in File → Open create a folder, move a seeded project into it through the row menu, reload — the project is
// still in the folder (folders.json on the server, the expanded state in localStorage). Cleans up over MCP before and after.
import { expect, test } from '@playwright/test';
import { openApp } from './lib/app';
import { BASE_URL, TOKEN } from './lib/env';
import { Mcp } from './lib/mcp';
import { readSeed } from './seed';

const FOLDER = 'e2e-folder';

async function cleanup(projectId: string): Promise<void> {
  const mcp = await new Mcp(BASE_URL, TOKEN, 'e2e-folders').init();
  await mcp.ok('project.move', { project_id: projectId, folder_id: 'root' });
  const folders = (await mcp.ok('folder.list')).data.folders as { folder_id: string; name: string; system?: boolean }[];
  for (const f of folders) if (f.name === FOLDER && !f.system) await mcp.ok('folder.delete', { folder_id: f.folder_id });
}

test.describe('project folders', () => {
  test.beforeEach(({}, info) => {
    test.skip(info.project.name === 'phone', 'desktop only (the narrow tree is covered by the unit tests)');
  });

  test('create a folder, move a project into it, reload: the project is in the folder', async ({ page }) => {
    const seed = readSeed();
    await cleanup(seed.second.id);
    try {
      await openApp(page, { open: [seed.demo.id], active: seed.demo.id });
      const openPicker = async () => {
        await page.getByRole('button', { name: 'Файл', exact: true }).click();
        await page.getByRole('menuitem', { name: 'Відкрити проєкт…' }).click();
        await expect(page.getByRole('tree')).toBeVisible();
      };
      await openPicker();

      // ＋ Нова папка at the top level
      await page.getByRole('treeitem').first().focus();
      await page.getByRole('button', { name: '＋ Нова папка' }).click();
      await page.getByRole('textbox', { name: 'Назва папки' }).fill(FOLDER);
      await page.keyboard.press('Enter');
      const folderRow = page.getByRole('treeitem', { name: new RegExp(`^${FOLDER},`) });
      await expect(folderRow).toBeVisible();

      // ⋯ → Перемістити в папку… → the new folder → Перемістити
      const projectRow = page.getByRole('treeitem').filter({ hasText: seed.second.name }).first();
      await projectRow.hover();
      await projectRow.getByRole('button', { name: `Дії: ${seed.second.name}` }).click();
      await page.getByRole('menuitem', { name: /Перемістити в папку/ }).click();
      const move = page.locator('.move-dialog');
      await expect(move.locator('header')).toHaveText(`Перемістити «${seed.second.name}»`);
      await move.getByRole('radio', { name: FOLDER }).check();
      await move.getByRole('button', { name: 'Перемістити', exact: true }).click();
      await expect(move).toBeHidden();

      // after a reload the project is inside the folder
      await page.reload();
      await page.waitForSelector('[role=tab]');
      await openPicker();
      const row = page.getByRole('treeitem', { name: new RegExp(`^${FOLDER},`) });
      if ((await row.getAttribute('aria-expanded')) !== 'true') await row.click();
      const inside = page.getByRole('treeitem').filter({ hasText: seed.second.name }).first();
      await expect(inside).toHaveAttribute('aria-level', '2');
    } finally {
      await cleanup(seed.second.id);
    }
  });
});
