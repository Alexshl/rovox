// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { defineConfig } from 'vitest/config';

const server = process.env.ROVOX_DEV_SERVER ?? 'http://127.0.0.1:4810';

export default defineConfig({
  base: './',
  build: { target: 'es2022', outDir: 'dist', assetsDir: 'assets', sourcemap: false },
  worker: { format: 'es' },
  server: {
    headers: { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' },
    proxy: {
      '/api': server,
      '/mcp': server,
      '/source': server,
      '/ws': { target: server.replace('http', 'ws'), ws: true },
    },
  },
  test: { include: ['tests/**/*.test.ts'], environment: 'node' },
});
