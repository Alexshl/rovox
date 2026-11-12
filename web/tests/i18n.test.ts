// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The message catalogs (same keys, same placeholders), plural selection, locale detection, fallback and the live language switch.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  availableLocales,
  detectLocale,
  errorDetail,
  errorText,
  fmtBytes,
  fmtDuration,
  fmtNumber,
  getCatalog,
  getLocale,
  onLocaleChange,
  plural,
  registerLocale,
  setLocale,
  t,
  type Catalog,
  type MessageKey,
} from '../src/i18n';
import { uk } from '../src/i18n/uk';

const tick = (ms = 80) => new Promise((r) => setTimeout(r, ms));
const placeholders = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]!).sort();

function stubNavigator(languages: string[]): void {
  vi.stubGlobal('navigator', { languages, language: languages[0] ?? '' });
}

beforeEach(() => {
  localStorage.clear();
  setLocale('uk');
  localStorage.clear();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  document.body.innerHTML = '';
});

describe('catalogs', () => {
  it('uk and en are registered, with native names', () => {
    expect(availableLocales).toEqual(expect.arrayContaining([{ code: 'uk', name: 'Українська' }, { code: 'en', name: 'English' }]));
  });

  it('every registered catalog has exactly the keys of the Ukrainian source', () => {
    const source = Object.keys(uk).sort();
    for (const { code } of availableLocales) {
      const catalog = getCatalog(code);
      expect(catalog, code).toBeDefined();
      expect(Object.keys(catalog!).sort(), code).toEqual(source);
      for (const [key, text] of Object.entries(catalog!)) expect(typeof text === 'string' && text.length > 0, `${code}:${key}`).toBe(true);
    }
  });

  it('placeholders of every message match the source', () => {
    for (const { code } of availableLocales) {
      const catalog = getCatalog(code)!;
      for (const key of Object.keys(uk) as MessageKey[]) expect(placeholders(catalog[key]), `${code}:${key}`).toEqual(placeholders(uk[key]));
    }
  });

  it('has a plain-text message for every error code of the server', () => {
    const codes = [
      'INVALID_PARAMS', 'NOT_FOUND', 'REVISION_CONFLICT', 'PRECONDITION_FAILED', 'BUSY', 'INTERNAL', 'PREFLIGHT_FAILED', 'UNSUPPORTED', 'IO_ERROR', 'CANCELLED',
      'NOT_IMPLEMENTED', 'SERVER_RESTARTED', 'RESYNC_REQUIRED', 'FORBIDDEN_PATH', 'PLUGIN_CRASHED', 'PLUGIN_BLACKLISTED', 'PLUGIN_LOAD_FAILED', 'FILE_TOO_LARGE',
      'FORBIDDEN_LICENSE', 'UNAUTHORIZED', 'FORBIDDEN_SCOPE', 'QUOTA_EXCEEDED', 'RATE_LIMITED',
    ];
    for (const code of codes) expect(`errors.${code}` in uk, code).toBe(true);
  });
});

describe('t and plural', () => {
  it('fills {name} placeholders', () => {
    expect(t('tracks.headN', { n: 3 })).toBe('ДОРІЖКИ · 3');
    expect(t('mcp.activity', { name: 'Пісня' })).toBe('ЖУРНАЛ · Пісня');
  });

  it('picks the Ukrainian plural form (one / few / many)', () => {
    const f = (n: number) => plural(n, 'picker.tracks');
    expect(f(1)).toBe('1 доріжка');
    expect(f(2)).toBe('2 доріжки');
    expect(f(5)).toBe('5 доріжок');
    expect(f(11)).toBe('11 доріжок');
    expect(f(21)).toBe('21 доріжка');
    expect(f(22)).toBe('22 доріжки');
  });

  it('picks the English plural form (one / other)', () => {
    setLocale('en');
    expect(plural(1, 'picker.tracks')).toBe('1 track');
    expect(plural(2, 'picker.tracks')).toBe('2 tracks');
  });

  it('picks the Russian plural form (one / few / many)', () => {
    setLocale('ru');
    const f = (n: number) => plural(n, 'picker.tracks');
    expect(f(1)).toBe('1 дорожка');
    expect(f(2)).toBe('2 дорожки');
    expect(f(5)).toBe('5 дорожек');
    expect(f(11)).toBe('11 дорожек');
    expect(f(21)).toBe('21 дорожка');
  });

  it('formats numbers and sizes with the active language', () => {
    expect(fmtNumber(-14, { minimumFractionDigits: 1, maximumFractionDigits: 1 })).toMatch(/^[-−]14,0$/);
    expect(fmtBytes(4.8 * 1024 ** 3)).toBe('4,8 ГБ');
    setLocale('en');
    expect(fmtNumber(1.5, { minimumFractionDigits: 1 })).toBe('1.5');
    expect(fmtBytes(2048)).toBe('2.0 KB');
    expect(fmtDuration(222)).toBe('3:42');
    expect(fmtDuration(3725)).toBe('1:02:05');
  });

  it('shows the plain text of an error code and keeps the raw message for a second line', () => {
    const err = { code: 'REVISION_CONFLICT', message: 'expected 5, got 7' };
    expect(errorText(err)).toBe(uk['errors.REVISION_CONFLICT']);
    expect(errorDetail(err)).toBe('REVISION_CONFLICT: expected 5, got 7');
    expect(errorText({ code: 'SOMETHING_NEW', message: 'raw' }, 'toast.command')).toBe(uk['toast.command']);
    expect(errorText(undefined)).toBe(uk['errors.UNKNOWN']);
  });
});

describe('locale detection', () => {
  it('uses the saved preference first', () => {
    stubNavigator(['en-US']);
    localStorage.setItem('rovox.locale', 'uk');
    expect(detectLocale()).toBe('uk');
  });

  it('then the first of navigator.languages that is available', () => {
    stubNavigator(['xx-YY', 'en-GB', 'uk']);
    expect(detectLocale()).toBe('en');
    stubNavigator(['uk-UA', 'en']);
    expect(detectLocale()).toBe('uk');
  });

  it('else Ukrainian; an unknown saved value is ignored', () => {
    stubNavigator(['xx', 'yy-ZZ']);
    expect(detectLocale()).toBe('uk');
    localStorage.setItem('rovox.locale', 'zz');
    expect(detectLocale()).toBe('uk');
  });

  it('survives a blocked localStorage', () => {
    stubNavigator(['en']);
    vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new Error('blocked');
    });
    expect(detectLocale()).toBe('en');
  });
});

describe('switching', () => {
  it('persists the choice, sets <html lang> and notifies subscribers', () => {
    const seen: string[] = [];
    const off = onLocaleChange((c) => seen.push(c));
    setLocale('en');
    expect(getLocale()).toBe('en');
    expect(localStorage.getItem('rovox.locale')).toBe('en');
    expect(document.documentElement.lang).toBe('en');
    setLocale('nope'); // unknown: ignored
    expect(getLocale()).toBe('en');
    off();
    setLocale('uk');
    expect(seen).toEqual(['en']);
    expect(document.documentElement.lang).toBe('uk');
  });

  it('changes a rendered menu label without a reload, and the View menu lists the languages with radio semantics', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} unobserve() {} });
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
    const { App } = await import('../src/store/session');
    const { Shell } = await import('../src/views/shell');
    const app = new App('ws://test/ws');
    const shell = new Shell(app);
    document.body.append(shell.el);
    await tick();

    const titles = () => [...document.querySelectorAll('.menubar .menu > button')].map((b) => b.textContent);
    expect(titles()[0]).toBe('Файл');
    expect(document.querySelector('.tabs')!.getAttribute('aria-label')).toBe('Відкриті проєкти');

    const view = document.querySelectorAll<HTMLButtonElement>('.menubar .menu > button')[2]!;
    view.click();
    const radios = [...document.querySelectorAll<HTMLElement>('.menubar [role=menuitemradio]')];
    expect(radios.map((r) => r.textContent?.replace('✓', ''))).toEqual(availableLocales.map((l) => l.name));
    expect(radios.map((r) => r.getAttribute('aria-checked'))).toEqual(availableLocales.map((l) => String(l.code === 'uk')));
    radios[availableLocales.findIndex((l) => l.code === 'en')]!.click();
    await tick();

    expect(getLocale()).toBe('en');
    expect(localStorage.getItem('rovox.locale')).toBe('en');
    expect(titles()[0]).toBe('File');
    expect(document.querySelector('.tabs')!.getAttribute('aria-label')).toBe('Open projects');
    expect(document.querySelector('.transport')!.textContent).toContain('GRID');
    expect(document.querySelector('.mixer .head')!.textContent).toContain('MIXER');
    setLocale('uk');
    expect(titles()[0]).toBe('Файл');
    expect(document.querySelector('.mixer .head')!.textContent).toContain('МІКШЕР');
  });
});

// Last on purpose: it registers a throw-away language, which the catalog completeness checks above must not see.
describe('fallback', () => {
  it('a key missing in a language falls back to Ukrainian and warns once', () => {
    const partial = { ...uk } as Record<string, string>;
    delete partial['menu.file'];
    registerLocale('xx', 'Test', partial as unknown as Catalog);
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    setLocale('xx');
    expect(t('menu.file')).toBe(uk['menu.file']);
    expect(t('menu.file')).toBe(uk['menu.file']);
    expect(t('menu.edit')).toBe(uk['menu.edit']);
    expect(warn).toHaveBeenCalledTimes(1); // dev warning, once per missing key
    setLocale('uk');
  });
});
