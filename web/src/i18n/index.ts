// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Message catalogs and locale-aware formatting. Ukrainian (uk.ts) is the source catalog: its keys are the typed MessageKey set and
// every other catalog must be a Record<MessageKey, string>. Adding a language = one catalog file + one import and one registerLocale
// line at the bottom of this file.
import { en } from './en';
import { uk } from './uk';
import type { MessageKey } from './uk';

export type { MessageKey } from './uk';

export type Catalog = Readonly<Record<MessageKey, string>>;
export type Params = Readonly<Record<string, string | number>>;

/** Base of a plural group: every key `<base>.other` stands for the sub-keys `<base>.one|few|many|other`. */
export type PluralKey = MessageKey extends infer K ? (K extends `${infer B}.other` ? B : never) : never;

export interface LocaleInfo {
  code: string;
  name: string;
}

const STORE_KEY = 'rovox.locale';
const FALLBACK = 'uk';

const catalogs = new Map<string, { name: string; catalog: Catalog }>();
const listeners = new Set<(code: string) => void>();
let current = FALLBACK;
const warned = new Set<string>();

/** The languages the UI can switch to, in registration order (code + native name). */
export const availableLocales: LocaleInfo[] = [];

export function registerLocale(code: string, nativeName: string, catalog: Catalog): void {
  if (!catalogs.has(code)) availableLocales.push({ code, name: nativeName });
  else availableLocales.splice(availableLocales.findIndex((l) => l.code === code), 1, { code, name: nativeName });
  catalogs.set(code, { name: nativeName, catalog });
}

/** The catalog registered for `code` (undefined for an unknown code); tests use it to compare catalogs with the source. */
export function getCatalog(code: string): Catalog | undefined {
  return catalogs.get(code)?.catalog;
}

/** Saved preference, else the first of navigator.languages that is available, else Ukrainian. */
export function detectLocale(): string {
  try {
    const saved = typeof localStorage !== 'undefined' ? localStorage.getItem(STORE_KEY) : null;
    if (saved && catalogs.has(saved)) return saved;
  } catch {
    // storage blocked: fall through to the browser language
  }
  const nav = typeof navigator !== 'undefined' ? navigator : undefined;
  const langs = nav?.languages?.length ? nav.languages : nav?.language ? [nav.language] : [];
  for (const l of langs) {
    const base = l.toLowerCase().split('-')[0] ?? '';
    if (catalogs.has(base)) return base;
  }
  return FALLBACK;
}

function applyLang(): void {
  if (typeof document !== 'undefined') document.documentElement.lang = current;
}

export function getLocale(): string {
  return current;
}

/** Switches the language, saves the choice and tells the subscribers. An unknown code is ignored. */
export function setLocale(code: string): void {
  if (!catalogs.has(code)) return;
  current = code;
  try {
    if (typeof localStorage !== 'undefined') localStorage.setItem(STORE_KEY, code);
  } catch {
    // storage blocked: the choice lasts until reload
  }
  applyLang();
  for (const fn of [...listeners]) fn(code);
}

/** Calls `fn` after every language switch; returns the unsubscribe function. */
export function onLocaleChange(fn: (code: string) => void): () => void {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

function interpolate(msg: string, params?: Params): string {
  if (!params) return msg;
  return msg.replace(/\{(\w+)\}/g, (m, name: string) => (name in params ? String(params[name]) : m));
}

function lookup(key: string): string {
  const own: Readonly<Record<string, string | undefined>> | undefined = catalogs.get(current)?.catalog;
  const hit = own?.[key];
  if (hit !== undefined) return hit;
  if (!warned.has(`${current}:${key}`)) {
    warned.add(`${current}:${key}`);
    if (import.meta.env?.DEV) console.warn(`[i18n] "${key}" is missing in "${current}", using "${FALLBACK}"`);
  }
  const src: Readonly<Record<string, string | undefined>> = uk;
  return src[key] ?? key;
}

/** True when `key` is a message key of the source catalog. */
export function hasKey(key: string): key is MessageKey {
  return Object.prototype.hasOwnProperty.call(uk, key);
}

/** The message `key` in the active language, `{name}` placeholders filled from `params`. */
export function t(key: MessageKey, params?: Params): string {
  return interpolate(lookup(key), params);
}

/** The plural form of group `key` for `n` (Intl.PluralRules of the active language, `other` as the fallback); `{n}` is the formatted count. */
export function plural(n: number, key: PluralKey, params?: Params): string {
  const cat = new Intl.PluralRules(current).select(n);
  const own: Readonly<Record<string, string | undefined>> | undefined = catalogs.get(current)?.catalog;
  const k = own?.[`${key}.${cat}`] !== undefined ? `${key}.${cat}` : `${key}.other`;
  return interpolate(lookup(k), { n: fmtNumber(n), ...params });
}

//=============================================================================
// formatting through Intl with the active language

const nfCache = new Map<string, Intl.NumberFormat>();

export function fmtNumber(n: number, opts?: Intl.NumberFormatOptions): string {
  const k = `${current}|${opts ? JSON.stringify(opts) : ''}`;
  let f = nfCache.get(k);
  if (!f) {
    f = new Intl.NumberFormat(current, opts);
    nfCache.set(k, f);
  }
  return f.format(n);
}

const dfCache = new Map<string, Intl.DateTimeFormat>();

/** Date and / or time in the active language; 24-hour clock unless `opts` says otherwise. '' for an invalid date. */
export function fmtDate(d: Date, opts: Intl.DateTimeFormatOptions = { dateStyle: 'short' }): string {
  if (Number.isNaN(d.getTime())) return '';
  const k = `${current}|${JSON.stringify(opts)}`;
  let f = dfCache.get(k);
  if (!f) {
    f = new Intl.DateTimeFormat(current, { hourCycle: 'h23', ...opts });
    dfCache.set(k, f);
  }
  return f.format(d);
}

/** "dd.MM hh:mm" in the active language's order (local time, no seconds unless asked). '' for an invalid date. */
export function fmtShortDateTime(d: Date, seconds = false): string {
  if (Number.isNaN(d.getTime())) return '';
  const date = fmtDate(d, { day: '2-digit', month: '2-digit' });
  const time = fmtDate(d, { hour: '2-digit', minute: '2-digit', ...(seconds ? { second: '2-digit' } : {}) });
  return `${date} ${time}`;
}

/** Size with a localised unit: "4,8 МБ" (binary multiples). */
export function fmtBytes(n: number): string {
  const units = ['unit.B', 'unit.KB', 'unit.MB', 'unit.GB'] as const;
  let v = Math.max(0, n);
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  const digits = i === 0 ? 0 : 1;
  return `${fmtNumber(v, { minimumFractionDigits: digits, maximumFractionDigits: digits })} ${t(units[i]!)}`;
}

/** "3:42" (h:mm:ss from an hour): a technical readout, the same in every language. */
export function fmtDuration(s: number): string {
  const total = Math.max(0, Math.round(s));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const sec = String(total % 60).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

//=============================================================================
// server and client error codes

export interface ErrorLike {
  code?: string | undefined;
  message?: string | undefined;
}

/** What to show a person about an error: the localised text for its code (with the next action), else `fallback`, else a generic text. */
export function errorText(err: ErrorLike | null | undefined, fallback?: MessageKey): string {
  const key = err?.code ? `errors.${err.code}` : '';
  if (key && hasKey(key)) return t(key);
  return t(fallback ?? 'errors.UNKNOWN');
}

/** The raw message of the error for a secondary line or a tooltip ('' when there is none). */
export function errorDetail(err: ErrorLike | null | undefined): string {
  const code = err?.code ?? '';
  const msg = err?.message ?? '';
  return msg && code ? `${code}: ${msg}` : msg || code;
}

//=============================================================================
// the languages; a new one is one file + these two lines
registerLocale('uk', 'Українська', uk);
registerLocale('en', 'English', en);

current = detectLocale();
applyLang();
