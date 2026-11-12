// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Seeds the projects over MCP, then signs in once (`/?token=` sets the cookie) and stores the browser state, with the UI language forced to
// Ukrainian, for every test.
import { chromium, type FullConfig } from '@playwright/test';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { AUTH_FILE, BASE_URL, TOKEN } from './lib/env';
import { seedAll } from './seed';

export default async function globalSetup(_config: FullConfig): Promise<void> {
  await seedAll(BASE_URL, TOKEN);
  mkdirSync(dirname(AUTH_FILE), { recursive: true });
  const browser = await chromium.launch();
  const context = await browser.newContext({ baseURL: BASE_URL });
  const page = await context.newPage();
  await page.goto(`/?token=${encodeURIComponent(TOKEN)}`);
  await page.waitForLoadState('networkidle');
  await page.evaluate(() => localStorage.setItem('rovox.locale', 'uk'));
  await context.storageState({ path: AUTH_FILE });
  await browser.close();
}
