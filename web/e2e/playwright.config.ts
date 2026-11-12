// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// Playwright config. Run it in the pinned container (docker/compose.e2e.yml): the reference screenshots are only valid for the OS, fonts and
// browser build they were taken with.
import { defineConfig, devices } from '@playwright/test';
import { AUTH_FILE, BASE_URL } from './lib/env';

const CI = !!process.env.CI;
const desktop = { viewport: { width: 1440, height: 900 } };
// WebCodecs / AudioWorklet need a secure context: http://localhost is one, a compose service name (http://rovox:4810) is not, so it is allow-listed.
const secure = (process.env.E2E_SECURE_ORIGINS ?? '').split(',').filter(Boolean);
const secureArgs = secure.length ? [`--unsafely-treat-insecure-origin-as-secure=${secure.join(',')}`] : [];
const securePrefs = secure.length ? { 'dom.securecontext.allowlist': secure.map((o) => new URL(o).hostname).join(',') } : {};

export default defineConfig({
  testDir: '.',
  testMatch: '*.spec.ts',
  outputDir: 'test-results',
  globalSetup: './globalSetup.ts',
  // one server, one audio device model, shared project state: no parallelism
  workers: 1,
  fullyParallel: false,
  retries: CI ? 1 : 0,
  timeout: 120_000,
  expect: { timeout: 10_000, toHaveScreenshot: { maxDiffPixelRatio: 0.01, animations: 'disabled', caret: 'hide' } },
  reporter: CI ? [['list'], ['html', { open: 'never' }]] : [['list']],
  snapshotPathTemplate: '{testDir}/__screenshots__/{projectName}-{platform}/{testFilePath}/{arg}{ext}',
  use: {
    baseURL: BASE_URL,
    storageState: AUTH_FILE,
    locale: 'uk-UA',
    timezoneId: 'UTC',
    colorScheme: 'dark',
    reducedMotion: 'reduce',
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
    ...desktop,
  },
  projects: [
    {
      name: 'chromium',
      use: {
        ...devices['Desktop Chrome'],
        ...desktop,
        launchOptions: { args: ['--autoplay-policy=no-user-gesture-required', '--force-color-profile=srgb', '--font-render-hinting=none', '--disable-lcd-text', ...secureArgs] },
      },
    },
    {
      name: 'firefox',
      use: {
        ...devices['Desktop Firefox'],
        ...desktop,
        launchOptions: { firefoxUserPrefs: { 'media.autoplay.default': 0, 'media.autoplay.blocking_policy': 0, ...securePrefs } },
      },
      testIgnore: /perf\.spec\.ts/,
    },
    {
      name: 'phone',
      use: {
        ...devices['Desktop Chrome'],
        viewport: { width: 390, height: 844 },
        isMobile: true,
        hasTouch: true,
        deviceScaleFactor: 2,
        launchOptions: { args: ['--force-color-profile=srgb', '--font-render-hinting=none', '--disable-lcd-text', ...secureArgs] },
      },
      testMatch: /visual\.spec\.ts/,
    },
    // the owner's run in a real, headed Chrome (the 50 fps target): `E2E_HEADED=1 npx playwright test --project=chrome-headed perf`
    ...(process.env.E2E_HEADED === '1'
      ? [{ name: 'chrome-headed', use: { ...devices['Desktop Chrome'], ...desktop, channel: 'chrome', headless: false }, testMatch: /perf\.spec\.ts/ }]
      : []),
  ],
});
