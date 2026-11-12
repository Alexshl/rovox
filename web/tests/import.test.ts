// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// Dropped files flattened and filtered, and the upload + import wizard end to end with a fake server and a fake app:
// one WAV applied at once with an Undo toast, an R20 folder with an editable plan, a per-file error with retry, Esc collapsing into
// the pill, resume after a reload, the .RPP result dialog, disk full.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setLocale } from '../src/i18n';
import { collectDrop, flattenEntries, flattenFiles, folderPickerSupported, isImportable } from '../src/lib/dropfiles';
import { fingerprintOf, loadSaved, saveBatch, type SavedBatch } from '../src/net/tus';
import { type ImportHost, ImportController } from '../src/views/importwizard';

const tick = () => new Promise((r) => setTimeout(r, 0));
const until = (fn: () => void) => vi.waitFor(fn, { timeout: 3000, interval: 5 });
const key = (el: Element, k: string) => el.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));

function memoryStorage(): Storage {
  const m = new Map<string, string>();
  return {
    get length() {
      return m.size;
    },
    clear: () => m.clear(),
    getItem: (k) => m.get(k) ?? null,
    key: (i) => [...m.keys()][i] ?? null,
    removeItem: (k) => void m.delete(k),
    setItem: (k, v) => void m.set(k, String(v)),
  };
}

function wav(name: string, size = 8, rel = '', lastModified = 100): File {
  const f = new File([new Uint8Array(size)], name, { lastModified });
  if (rel) Object.defineProperty(f, 'webkitRelativePath', { value: rel });
  return f;
}

//==============================================================================
// dropfiles
function fileEntry(path: string, f: File): FileSystemFileEntry {
  return { isFile: true, isDirectory: false, name: f.name, fullPath: path, file: (ok: (x: File) => void) => ok(f) } as unknown as FileSystemFileEntry;
}

function dirEntry(name: string, children: FileSystemEntry[], batch = 2): FileSystemDirectoryEntry {
  return {
    isFile: false,
    isDirectory: true,
    name,
    fullPath: `/${name}`,
    createReader: () => {
      let i = 0;
      return {
        readEntries: (ok: (e: FileSystemEntry[]) => void) => {
          ok(children.slice(i, i + batch));
          i += batch;
        },
      };
    },
  } as unknown as FileSystemDirectoryEntry;
}

describe('dropfiles', () => {
  it('flattens a dropped folder (reading until readEntries is empty), keeps relpaths and counts what it skipped', async () => {
    const tree = dirEntry('ZOOM0001.zprj', [
      fileEntry('/ZOOM0001.zprj/REC01.WAV', wav('REC01.WAV', 4, '', 1)),
      fileEntry('/ZOOM0001.zprj/REC02.WAV', wav('REC02.WAV', 4, '', 2)),
      fileEntry('/ZOOM0001.zprj/R20PRJDATA.r20prj', wav('R20PRJDATA.r20prj', 2)),
      fileEntry('/ZOOM0001.zprj/.DS_Store', wav('.DS_Store', 1)),
      dirEntry('AUDIO', [fileEntry('/ZOOM0001.zprj/AUDIO/REC03.flac', wav('REC03.flac', 4)), fileEntry('/ZOOM0001.zprj/AUDIO/readme.txt', wav('readme.txt', 1))]),
    ]);
    const flat = await flattenEntries([tree]);
    expect(flat.items.map((i) => i.relpath)).toEqual(['ZOOM0001.zprj/REC01.WAV', 'ZOOM0001.zprj/REC02.WAV', 'ZOOM0001.zprj/R20PRJDATA.r20prj', 'ZOOM0001.zprj/AUDIO/REC03.flac']);
    expect(flat.skipped).toBe(2);
    expect(flat.folders).toBe(1);
    expect(flat.name).toBe('ZOOM0001.zprj');
  });

  it('gives loose files their bare name and drops repeats of the same file', async () => {
    const a = wav('take.wav', 4, '', 7);
    const flat = await flattenEntries([fileEntry('/take.wav', a), fileEntry('/take.wav', a), fileEntry('/notes.pdf', wav('notes.pdf'))]);
    expect(flat.items.map((i) => i.relpath)).toEqual(['take.wav']);
    expect(flat.duplicates).toEqual(['take.wav']);
    expect(flat.skipped).toBe(1);
    expect(flat.folders).toBe(0);
    expect(flat.name).toBe('take.wav');
  });

  it('uses webkitRelativePath for a folder chosen with the input, and the extension filter is case-insensitive', () => {
    const flat = flattenFiles([wav('REC01.WAV', 4, 'Song/REC01.WAV'), wav('mix.MP3', 4, 'Song/mix.MP3'), wav('cover.jpg', 4, 'Song/cover.jpg'), wav('x.rpp', 4)]);
    expect(flat.items.map((i) => i.relpath)).toEqual(['Song/REC01.WAV', 'Song/mix.MP3', 'x.rpp']);
    expect(flat.skipped).toBe(1);
    expect(flat.folders).toBe(1);
    expect(isImportable('a.AIFF')).toBe(true);
    expect(isImportable('a.aif')).toBe(true);
    expect(isImportable('a.ogg')).toBe(false);
  });

  it('collectDrop reads the entries of a DataTransfer and falls back to plain files', async () => {
    const tree = dirEntry('Song', [fileEntry('/Song/a.wav', wav('a.wav'))]);
    const withEntries = { items: [{ kind: 'file', webkitGetAsEntry: () => tree, getAsFile: () => null }], files: [] } as unknown as DataTransfer;
    expect((await collectDrop(withEntries)).items.map((i) => i.relpath)).toEqual(['Song/a.wav']);
    const plain = { items: [{ kind: 'file', webkitGetAsEntry: () => null, getAsFile: () => wav('b.flac') }], files: [] } as unknown as DataTransfer;
    expect((await collectDrop(plain)).items.map((i) => i.relpath)).toEqual(['b.flac']);
  });

  it('hides the folder picker on iPad Safari', () => {
    expect(folderPickerSupported({ userAgent: 'Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X)', platform: 'iPad', maxTouchPoints: 5 })).toBe(false);
    expect(folderPickerSupported({ userAgent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X)', platform: 'MacIntel', maxTouchPoints: 5 })).toBe(false);
    expect(folderPickerSupported({ userAgent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X)', platform: 'MacIntel', maxTouchPoints: 0 })).toBe(true);
  });
});

//==============================================================================
// a fake tus server and a fake app
interface FakeServer {
  fetchImpl: typeof fetch;
  calls: { url: string; method: string; headers: Record<string, string> }[];
  offsets: Map<string, number>;
  /** Decides a PATCH: a Response to answer with, or undefined for the normal behaviour. */
  patch?: (url: string) => Response | undefined;
  /** While set, PATCHes wait for it (an upload that is in progress). */
  hold: Promise<void> | null;
  batch: 'ok' | 'disk';
}

function fakeServer(): FakeServer {
  const s: FakeServer = { calls: [], offsets: new Map(), hold: null, batch: 'ok', fetchImpl: undefined as unknown as typeof fetch };
  s.fetchImpl = (async (input: RequestInfo | URL, init: RequestInit = {}) => {
    const url = String(input);
    const method = init.method ?? 'GET';
    const headers = (init.headers ?? {}) as Record<string, string>;
    s.calls.push({ url, method, headers });
    if (url === '/api/upload-batches') {
      if (s.batch === 'disk') {
        const needed = Math.round(4.8 * 1024 ** 3);
        const available = Math.round(1.2 * 1024 ** 3);
        return new Response(JSON.stringify({ ok: false, error: { code: 'PRECONDITION_FAILED', message: 'not enough free disk space for this batch', details: { available_bytes: available, needed_bytes: needed } } }), { status: 412 });
      }
      const files = (JSON.parse(String(init.body)) as { files: { relpath: string }[] }).files;
      const out = files.map((f, i) => {
        const u = `/api/uploads/up_${i}_${f.relpath.replace(/\W/g, '_')}`;
        s.offsets.set(u, 0);
        return { relpath: f.relpath, upload_id: `up_${i}`, url: u };
      });
      return new Response(JSON.stringify({ batch_id: 'bt_1', files: out }), { status: 201 });
    }
    if (method === 'HEAD') return s.offsets.has(url) ? new Response(null, { status: 200, headers: { 'Upload-Offset': String(s.offsets.get(url)) } }) : new Response(null, { status: 404 });
    if (method === 'DELETE') return new Response(null, { status: 204 });
    if (method === 'PATCH') {
      if (s.hold) await s.hold;
      if (init.signal?.aborted) throw new DOMException('aborted', 'AbortError');
      const custom = s.patch?.(url);
      if (custom) return custom;
      const at = s.offsets.get(url) ?? 0;
      const next = at + (init.body as Blob).size;
      s.offsets.set(url, next);
      return new Response(null, { status: 204, headers: { 'Upload-Offset': String(next) } });
    }
    return new Response(null, { status: 400 });
  }) as typeof fetch;
  return s;
}

type Handler = (params: Record<string, any>) => { ok: boolean; data?: unknown; error?: { code: string; message: string } };
const ok = (data: unknown) => ({ ok: true, data });

function planOf(tracks: { name: string; key: string; channels?: number }[], warnings: { code: string; message: string; file?: string }[] = [], detected = 'zoom_r20') {
  return {
    plan_id: 'pl_1',
    detected_source: detected,
    tracks: tracks.map((t) => ({ name: t.name, source_key: t.key, color: '#c0392b', role_guess: '', clips: [{ file: `${t.key}.WAV`, channels: t.channels ?? 1, sample_rate: 48000, bit_depth: 24, duration_s: 222 }] })),
    warnings,
  };
}

function setup(handlers: Record<string, Handler>, deps: { server?: FakeServer; storage?: Storage } = {}) {
  const server = deps.server ?? fakeServer();
  const calls: { name: string; params: Record<string, any>; o: unknown }[] = [];
  const run = async (name: string, params: Record<string, any>, o?: unknown) => {
    calls.push({ name, params, o });
    return handlers[name]?.(params) ?? ok({});
  };
  const host = {
    activeId: 'p_1',
    active: { name: 'Пісня 3' },
    cmd: vi.fn(run),
    mcp: { call: vi.fn((name: string, params: Record<string, any>) => run(name, params)) },
    edit: vi.fn(run),
    toast: vi.fn(),
    listProjects: vi.fn(async () => undefined),
    setActive: vi.fn(),
    undo: vi.fn(async () => undefined),
  } as unknown as ImportHost;
  const storage = deps.storage ?? memoryStorage();
  const ctl = new ImportController(host, { fetchImpl: server.fetchImpl, storage, sleep: async () => undefined, chunkBytes: 4 });
  return { host, ctl, server, calls, storage };
}

const text = (sel: string) => document.querySelector(sel)?.textContent ?? '';
const buttons = (): HTMLButtonElement[] => [...document.querySelectorAll<HTMLButtonElement>('.import-dialog button')];
const buttonWith = (label: string) => buttons().find((b) => b.textContent === label)!;

beforeEach(() => {
  setLocale('uk');
});

afterEach(() => {
  document.body.innerHTML = '';
});

//==============================================================================
describe('wizard: one WAV', () => {
  it('uploads, finishes, plans without warnings and applies at once, with a toast that offers Undo', async () => {
    const { host, ctl, calls } = setup({
      'upload.finish': () => ok({ path: '/data/uploads/bt_1', files: [{ relpath: 'take.wav', bytes: 8 }], detected: 'audio' }),
      'import.plan': () => ok(planOf([{ name: 'take', key: 'take' }], [], 'generic')),
      'import.apply': () => ok({ tracks: [{ track_id: 't_1', name: 'take', item_ids: ['i_1'] }] }),
    });
    ctl.begin(flattenFiles([wav('take.wav')]));
    // step 2 is on screen at once, with its counter and a progressbar
    expect(text('.import-dialog > header')).toContain('2/3');
    const bar = document.querySelector('[role=progressbar]')!;
    expect(bar.getAttribute('aria-valuemin')).toBe('0');
    await until(() => expect(host.edit).toHaveBeenCalled());
    expect(calls.find((c) => c.name === 'upload.finish')!.params).toEqual({ batch_id: 'bt_1' });
    expect(calls.find((c) => c.name === 'import.plan')!.params).toMatchObject({ paths: ['/data/uploads/bt_1'], preset: 'auto', align: 'start' });
    expect(calls.find((c) => c.name === 'import.apply')!.params).toEqual({ plan_id: 'pl_1' });
    expect(host.setActive).toHaveBeenCalledWith('p_1');
    // no review step: the dialog is gone and a toast says what happened
    await until(() => expect(document.querySelector('.scrim')).toBeNull());
    const toast = document.querySelector('.toast.action')!;
    expect(toast.textContent).toContain('Імпортовано 1 доріжку.');
    const undo = toast.querySelector('button')!;
    expect(undo.textContent).toBe('Скасувати');
    undo.click();
    expect(host.undo).toHaveBeenCalled();
    expect(document.querySelector('.toast.action')).toBeNull();
  });

  it('announces milestones in a polite live region, not every tick', async () => {
    const { host, ctl } = setup({ 'upload.finish': () => ok({ path: '/p', files: [], detected: 'audio' }), 'import.plan': () => ok(planOf([{ name: 'a', key: 'a' }], [], 'generic')) });
    ctl.begin(flattenFiles([wav('a.wav', 8)]));
    await until(() => expect(host.edit).toHaveBeenCalled());
    const live = document.querySelector('.sr-only[aria-live=polite]')!;
    expect(live).not.toBeNull();
    expect(live.textContent).not.toBe('');
  });
});

//==============================================================================
describe('wizard: an R20 folder', () => {
  const folder = () => flattenFiles([wav('REC01.WAV', 8, 'ZOOM0001.zprj/REC01.WAV'), wav('REC02.WAV', 8, 'ZOOM0001.zprj/REC02.WAV'), wav('R20PRJDATA.r20prj', 4, 'ZOOM0001.zprj/R20PRJDATA.r20prj')]);
  const handlers = (): Record<string, Handler> => ({
    'upload.finish': () => ok({ path: '/data/uploads/bt_1', files: [], detected: 'zoom_r20' }),
    'import.plan': () =>
      ok(
        planOf(
          [
            { name: 'REC01', key: 'REC01' },
            { name: 'REC02', key: 'REC02', channels: 2 },
          ],
          [
            { code: 'MISSING_FILE', message: 'x', file: 'REC07.WAV' },
            { code: 'UNREADABLE_AUDIO', message: 'x', file: 'BAD.WAV' },
          ],
        ),
      ),
    'import.apply': () => ok({ tracks: [{ track_id: 't_1' }, { track_id: 't_2' }] }),
  });

  it('shows the detected source, editable names, the warnings with their codes; sends changed names as rename', async () => {
    const { host, ctl, calls } = setup(handlers());
    ctl.begin(folder());
    await until(() => expect(document.querySelector('.plan-row')).not.toBeNull());
    expect(text('.import-dialog > header')).toContain('3/3');
    expect(text('.finding.ok')).toBe('Розпізнано: Zoom R20 · 2 доріжки · 48 кГц · 3:42');
    const inputs = [...document.querySelectorAll<HTMLInputElement>('.plan-row input')];
    expect(inputs.map((i) => i.value)).toEqual(['REC01', 'REC02']);
    expect(inputs[0]!.maxLength).toBe(64);
    expect(document.activeElement).toBe(inputs[0]);
    expect(document.querySelector('.plan-row .key')!.textContent).toBe('REC01');
    expect(document.querySelectorAll('.plan-row .meta')[1]!.textContent).toBe('стерео 48 кГц 24 біт 3:42');
    // the target: new tracks in the open project, or a new project
    expect(text('.target')).toContain('Нові доріжки в «Пісня 3»');
    expect(text('.target')).toContain('Новий проєкт');

    const warn = document.querySelector('.finding.warning')!;
    expect(warn.textContent).toContain('Файл REC07.WAV є в проєкті R20, але його немає в теці. Доріжку буде створено без нього.');
    expect(warn.querySelector('.code')!.textContent).toBe('MISSING_FILE');
    const bad = document.querySelector('.finding.error')!;
    expect(bad.textContent).toContain('Не вдалося прочитати BAD.WAV');
    expect(bad.querySelector('.code')!.textContent).toBe('UNREADABLE_AUDIO');

    // Enter moves to the next name; an emptied name falls back to the original
    inputs[0]!.value = 'Вокал';
    inputs[0]!.dispatchEvent(new Event('input'));
    key(inputs[0]!, 'Enter');
    expect(document.activeElement).toBe(inputs[1]);
    expect(buttonWith('Імпортувати 2 доріжки')).toBeDefined();
    buttonWith('Імпортувати 2 доріжки').click();
    await until(() => expect(host.edit).toHaveBeenCalled());
    expect(calls.find((c) => c.name === 'import.apply')!.params).toEqual({ plan_id: 'pl_1', rename: { REC01: 'Вокал' } });
    await until(() => expect(text('.toast.action')).toContain('Імпортовано 2 доріжки.'));
  });

  it('plans again when the alignment changes and keeps the typed names', async () => {
    const { ctl, calls } = setup(handlers());
    ctl.begin(folder());
    await until(() => expect(document.querySelector('.plan-row')).not.toBeNull());
    const first = document.querySelector<HTMLInputElement>('.plan-row input')!;
    first.value = 'Гітара';
    first.dispatchEvent(new Event('input'));
    const select = document.querySelector<HTMLSelectElement>('.import-dialog select')!;
    select.value = 'bwf';
    select.dispatchEvent(new Event('change'));
    await until(() => expect(calls.filter((c) => c.name === 'import.plan')).toHaveLength(2));
    expect(calls.filter((c) => c.name === 'import.plan')[1]!.params.align).toBe('bwf');
    await until(() => expect(document.querySelector<HTMLInputElement>('.plan-row input')!.value).toBe('Гітара'));
  });
});

//==============================================================================
describe('wizard: errors and disk', () => {
  it('a failed file shows an error with Повторити while the others finish; retrying completes the batch', async () => {
    const server = fakeServer();
    let failA = true;
    server.patch = (url) => (failA && url.includes('A_WAV') ? new Response(null, { status: 500 }) : undefined);
    const { host, ctl } = setup({ 'upload.finish': () => ok({ path: '/p', files: [], detected: 'audio' }), 'import.plan': () => ok(planOf([{ name: 'a', key: 'a' }], [], 'generic')) }, { server });
    ctl.begin(flattenFiles([wav('A.WAV'), wav('B.WAV')]));
    await until(() => expect(document.querySelector('.upload-row.error')).not.toBeNull());
    const row = document.querySelector('.upload-row.error')!;
    const err = row.querySelector('[role=alert]')!;
    expect(err.textContent).toBe('Не вдалося завантажити A.WAV.');
    await until(() => expect(document.querySelectorAll('.upload-row.done')).toHaveLength(1));
    expect(document.querySelector('.upload-row.done .name')!.textContent).toBe('B.WAV');
    // it does not move on by itself, [Далі →] waits
    expect(buttonWith('Далі →').disabled).toBe(true);
    expect(host.edit).not.toHaveBeenCalled();
    failA = false;
    [...row.querySelectorAll('button')].find((b) => b.textContent === 'Повторити')!.click();
    await until(() => expect(buttonWith('Далі →').disabled).toBe(false));
    buttonWith('Далі →').click();
    await until(() => expect(host.edit).toHaveBeenCalled());
  });

  it('too little disk space is shown before any byte is sent, with Змінити вибір back to step 1', async () => {
    const server = fakeServer();
    server.batch = 'disk';
    const { ctl } = setup({}, { server });
    ctl.begin(flattenFiles([wav('A.WAV')]));
    await until(() => expect(document.querySelector('[role=alert]')).not.toBeNull());
    expect(text('[role=alert]')).toBe('На сервері бракує місця: потрібно 4,8 ГБ, вільно 1,2 ГБ. Звільніть місце або виберіть менше файлів.');
    expect(server.calls.some((c) => c.method === 'PATCH')).toBe(false);
    buttonWith('Змінити вибір').click();
    expect(text('.import-dialog > header')).toContain('1/3');
    expect(document.querySelector('.dropzone')).not.toBeNull();
  });

  it('says so when nothing in the selection is audio', () => {
    const { ctl } = setup({});
    ctl.begin(flattenFiles([wav('notes.txt')]));
    expect(text('[role=alert]')).toBe('Серед вибраного немає аудіофайлів. Підтримуються WAV, AIFF, FLAC, MP3.');
    expect(text('.finding:not(.error)')).toBe('Пропущено 1 файл (не аудіо)');
    expect(document.activeElement!.textContent).toBe('Вибрати інші');
  });
});

//==============================================================================
describe('wizard: Esc, the pill and cancelling', () => {
  it('Esc during an upload collapses into a pill; the pill expands it; Скасувати asks first', async () => {
    const server = fakeServer();
    let release!: () => void;
    server.hold = new Promise<void>((r) => (release = r));
    const { ctl } = setup({}, { server });
    ctl.begin(flattenFiles([wav('A.WAV')]));
    await until(() => expect(document.querySelector('.upload-row.uploading')).not.toBeNull());
    expect(ctl.isUploading()).toBe(true);

    // the page asks before it is closed, only while uploading
    const unload = new Event('beforeunload', { cancelable: true });
    window.dispatchEvent(unload);
    expect(unload.defaultPrevented).toBe(true);

    key(document.querySelector('.scrim')!, 'Escape');
    expect(document.querySelector('.scrim')).toBeNull();
    const pill = document.querySelector<HTMLElement>('.toast.pill')!;
    expect(pill.textContent).toContain('Завантаження');
    expect(pill.textContent).toContain('Розгорнути');
    expect(pill.getAttribute('aria-live')).toBe('off');
    expect(pill.querySelector('.progress')).not.toBeNull();

    pill.click();
    expect(document.querySelector('.toast.pill')).toBeNull();
    expect(document.querySelector('.scrim')).not.toBeNull();

    // a click on the scrim collapses as well; it never cancels
    document.querySelector('.scrim')!.dispatchEvent(new Event('pointerdown', { bubbles: true }));
    expect(document.querySelector('.toast.pill')).not.toBeNull();
    document.querySelector<HTMLElement>('.toast.pill')!.click();

    buttonWith('Скасувати').click();
    const confirm = document.querySelectorAll('.scrim')[1]!;
    expect(confirm.textContent).toContain('Зупинити завантаження? Завантажені частини збережуться 7 днів.');
    expect(document.activeElement!.textContent).toBe('Продовжити завантаження');
    (document.activeElement as HTMLElement).click();
    await tick();
    expect(document.querySelectorAll('.scrim')).toHaveLength(1);
    expect(ctl.busy).toBe(true);

    buttonWith('Скасувати').click();
    [...document.querySelectorAll<HTMLButtonElement>('.scrim')[1]!.querySelectorAll('button')].find((b) => b.textContent === 'Зупинити')!.click();
    await until(() => expect(document.querySelector('.scrim')).toBeNull());
    expect(server.calls.some((c) => c.method === 'DELETE')).toBe(true);
    expect(ctl.busy).toBe(false);
    release();
  });

  it('Esc closes on the picking step', () => {
    const { ctl } = setup({});
    ctl.openDialog();
    expect(document.activeElement!.textContent).toBe('Вибрати файли');
    key(document.querySelector('.scrim')!, 'Escape');
    expect(document.querySelector('.scrim')).toBeNull();
  });
});

//==============================================================================
describe('wizard: resume after a reload', () => {
  const saved = (): SavedBatch => ({
    batch_id: 'bt_7',
    project_id: 'p_1',
    name: 'ZOOM0001.zprj',
    created: 1,
    files: [
      { relpath: 'REC01.WAV', url: '/api/uploads/up_a', fingerprint: fingerprintOf(wav('REC01.WAV', 8)), size: 8, done: false },
      { relpath: 'REC02.WAV', url: '/api/uploads/up_b', fingerprint: fingerprintOf(wav('REC02.WAV', 8)), size: 8, done: true },
    ],
  });

  it('offers to continue, asks for the same files again and resumes from the server offset', async () => {
    const storage = memoryStorage();
    saveBatch(saved(), storage);
    const server = fakeServer();
    server.offsets.set('/api/uploads/up_a', 4);
    const { host, ctl, calls } = setup({ 'upload.finish': () => ok({ path: '/p', files: [], detected: 'audio' }), 'import.plan': () => ok(planOf([{ name: 'a', key: 'a' }], [], 'generic')) }, { server, storage });
    ctl.checkResume();
    const toast = document.querySelector('.toast.action')!;
    expect(toast.textContent).toContain('Завантаження не завершено: 1 з 2 файлів. Продовжити?');
    [...toast.querySelectorAll('button')].find((b) => b.textContent === 'Продовжити')!.click();
    expect(document.querySelector('.toast.action')).toBeNull();
    expect(text('.import-dialog')).toContain('Виберіть ту саму теку — продовжимо з місця зупинки.');

    const input = document.querySelector<HTMLInputElement>('.dropzone input[accept]')!;
    Object.defineProperty(input, 'files', { value: [wav('REC01.WAV', 8)] });
    input.dispatchEvent(new Event('change'));
    await until(() => expect(host.edit).toHaveBeenCalled());
    expect(server.calls.some((c) => c.url === '/api/upload-batches')).toBe(false);
    const patches = server.calls.filter((c) => c.method === 'PATCH');
    expect(patches.map((p) => p.headers['Upload-Offset'])).toEqual(['4']);
    expect(calls.find((c) => c.name === 'upload.finish')!.params).toEqual({ batch_id: 'bt_7' });
    expect(loadSaved(storage)).toHaveLength(0);
  });

  it('Відкинути ends the uploads on the server and forgets the record; no toast without a record', () => {
    const storage = memoryStorage();
    saveBatch(saved(), storage);
    const server = fakeServer();
    const { ctl } = setup({}, { server, storage });
    ctl.checkResume();
    [...document.querySelectorAll('.toast.action button')].find((b) => b.textContent === 'Відкинути')!.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    expect(server.calls.filter((c) => c.method === 'DELETE').map((c) => c.url)).toEqual(['/api/uploads/up_a']);
    expect(loadSaved(storage)).toHaveLength(0);

    const other = setup({}, { storage: memoryStorage() });
    other.ctl.checkResume();
    expect(document.querySelector('.toast.action')).toBeNull();
  });
});

//==============================================================================
describe('wizard: REAPER project', () => {
  it('applies the .RPP directly and lists what was carried over, what is unsupported and which media is missing', async () => {
    const { host, ctl, calls } = setup({
      'upload.finish': () => ok({ path: '/data/uploads/bt_1', files: [{ relpath: 'song.rpp', bytes: 4 }, { relpath: 'a.wav', bytes: 8 }], detected: 'rpp' }),
      'import.rpp': () =>
        ok({
          tracks: 3,
          items: 5,
          mapped: [{ kind: 'plugin', name: 'ReaEQ', mapped_to: 'EQ' }],
          unsupported: [{ kind: 'fx', name: 'Vendor Synth' }],
          missing_media: [{ path: 'audio/b.wav' }],
        }),
    });
    ctl.begin(flattenFiles([wav('song.rpp', 4), wav('a.wav')]));
    await until(() => expect(document.querySelector('.rpp-list')).not.toBeNull());
    expect(host.edit).toHaveBeenCalledWith('import.rpp', { path: '/data/uploads/bt_1/song.rpp' }, expect.anything());
    expect(text('.import-dialog > header')).toContain('Імпорт проєкту REAPER');
    const labels = [...document.querySelectorAll('.import-dialog .label')].map((l) => l.textContent);
    expect(labels).toEqual(['Перенесено · 1', 'Не підтримується · 1', 'Бракує медіа · 1']);
    expect(document.querySelector('.finding.ok')!.textContent).toContain('ReaEQ → EQ');
    expect(document.querySelector('.finding.warning')!.textContent).toContain('Vendor Synth');
    expect(document.querySelector('.finding.error')!.textContent).toContain('audio/b.wav');
    expect(calls.some((c) => c.name === 'import.plan')).toBe(false);
    expect(document.activeElement!.textContent).toBe('Закрити');
    (document.activeElement as HTMLElement).click();
    expect(document.querySelector('.scrim')).toBeNull();
  });
});

//==============================================================================
describe('drop target', () => {
  function dragEvent(type: string, dt: unknown, relatedTarget: Element | null = null): Event {
    const e = new Event(type, { bubbles: true, cancelable: true });
    Object.defineProperty(e, 'dataTransfer', { value: dt });
    Object.defineProperty(e, 'relatedTarget', { value: relatedTarget });
    return e;
  }
  const files = (n: number) => ({ types: ['Files'], items: Array.from({ length: n }, () => ({ kind: 'file', webkitGetAsEntry: () => null, getAsFile: () => wav('a.wav') })), files: [] });

  it('shows the overlay card while files are dragged over, hides it when the drag leaves the area itself, and imports on drop', async () => {
    const { ctl } = setup({ 'upload.finish': () => ok({ path: '/p', files: [], detected: 'audio' }), 'import.plan': () => ok(planOf([{ name: 'a', key: 'a' }], [], 'generic')) });
    const area = document.createElement('section');
    const child = document.createElement('div');
    area.append(child);
    document.body.append(area);
    ctl.attachArea(area);

    area.dispatchEvent(dragEvent('dragenter', files(8)));
    expect(area.classList.contains('dragover')).toBe(true);
    expect(area.dataset['dropLabel']).toBe('Відпустіть, щоб імпортувати · 8 файлів');
    // moving onto a child fires dragenter(child) then dragleave(area): still over
    child.dispatchEvent(dragEvent('dragenter', files(8)));
    area.dispatchEvent(dragEvent('dragleave', files(8), child));
    expect(area.classList.contains('dragover')).toBe(true);
    child.dispatchEvent(dragEvent('dragleave', files(8), null));
    expect(area.classList.contains('dragover')).toBe(false);

    // a drag of text is not ours
    area.dispatchEvent(dragEvent('dragenter', { types: ['text/plain'], items: [], files: [] }));
    expect(area.classList.contains('dragover')).toBe(false);

    area.dispatchEvent(dragEvent('dragenter', files(1)));
    const drop = dragEvent('drop', files(1));
    area.dispatchEvent(drop);
    expect(drop.defaultPrevented).toBe(true);
    expect(area.classList.contains('dragover')).toBe(false);
    await until(() => expect(document.querySelector('.import-dialog')).not.toBeNull());
    expect(text('.import-dialog > header')).toContain('2/3');
  });
});
