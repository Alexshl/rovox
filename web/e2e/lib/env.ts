// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

/** The server started with `--audio browser`. */
export const BASE_URL = process.env.E2E_URL ?? 'http://127.0.0.1:4810';
/** The server started with `--audio none`. */
export const NOAUDIO_URL = process.env.E2E_NOAUDIO_URL ?? 'http://127.0.0.1:4811';
export const TOKEN = process.env.E2E_TOKEN ?? process.env.ROVOX_TOKEN ?? '';
export const LONG = process.env.E2E_LONG === '1';
export const AUTH_FILE = join(here, '..', '.state', 'auth.json');
