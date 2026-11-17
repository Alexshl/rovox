// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// Project folders (E12): the tree helpers (build, sort, flatten), the remembered expanded folders and the keyboard of the picker tree.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setLocale } from '../src/i18n';
import { Emitter } from '../src/lib/dom';
import type { App } from '../src/store/session';
import { type Folder, TESTS, buildTree, countText, folderPath, loadTreeState, nodeKey, saveTreeState, stateKey, visibleRows } from '../src/views/folders';
import { type ServerProject, projectPicker } from '../src/views/projects';

const key = (el: Element, k: string, init: KeyboardEventInit = {}) => el.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...init }));
const tick = (ms = 0) => new Promise((r) => setTimeout(r, ms));

const folder = (folder_id: string, name: string, parent_id: string | null = null, extra: Partial<Folder> = {}): Folder => ({ folder_id, name, parent_id, system: false, ...extra });
const project = (project_id: string, name: string, folder_id: string | null = null, extra: Partial<ServerProject> = {}): ServerProject => ({ project_id, name, folder_id, tracks: 1, ...extra });

const FOLDERS: Folder[] = [
  folder(TESTS, 'Тестові треки (server name)', null, { system: true }),
  folder('fld_imp', 'Імпорт REAPER'),
  folder('fld_mix', 'Мікси 2026'),
  folder('fld_cli', 'Клієнти', 'fld_mix'),
  folder('fld_empty', 'Порожня'),
  folder('fld_10', 'mix 10'),
  folder('fld_9', 'Mix 9'),
];
const PROJECTS: ServerProject[] = [
  project('p_new', 'Новий проєкт'),
  project('p_kal', 'Kalyna — live', 'fld_mix'),
  project('p_demo', 'Demo vocals', 'fld_mix'),
  project('p_c1', 'Client one', 'fld_cli'),
  project('p_t1', 'mukola', TESTS),
  project('p_t2', 'mykola', TESTS),
  project('p_imp', 'Imported', 'fld_imp'),
  project('p_lost', 'Orphan', 'fld_gone'),
];

beforeEach(() => {
  setLocale('uk');
  localStorage.clear();
});

afterEach(() => {
  document.body.innerHTML = '';
  vi.restoreAllMocks();
});

describe('buildTree', () => {
  it('puts folders first sorted by name (numbers by value, case-insensitive), then projects in catalog order, «Тестові треки» last', () => {
    const tree = buildTree(FOLDERS, PROJECTS);
    const top = tree.roots.map((n) => nodeKey(n));
    expect(top).toEqual(['f:fld_imp', 'f:fld_mix', 'f:fld_empty', 'f:fld_9', 'f:fld_10', 'p:p_new', 'p:p_lost', `f:${TESTS}`]);
    const mix = tree.folders.get('fld_mix')!;
    expect(mix.children.map(nodeKey)).toEqual(['f:fld_cli', 'p:p_kal', 'p:p_demo']);
    expect(mix.projectCount).toBe(2);
    expect(mix.folderCount).toBe(1);
    expect(countText(mix)).toBe('2 проєкти · 1 папка');
    expect(countText(tree.folders.get('fld_empty')!)).toBe('0 проєктів');
    expect(tree.folders.get(TESTS)!.children.map(nodeKey)).toEqual(['p:p_t1', 'p:p_t2']);
    // a project in an unknown folder is shown at the top level
    expect(tree.projectFolder.get('p_lost')).toBeNull();
  });

  it('«Тестові треки» is last even when its name sorts first, and is never nested', () => {
    const tree = buildTree([folder('fld_a', 'AAA'), folder(TESTS, '000', 'fld_a', { system: true })], []);
    expect(tree.roots.map(nodeKey)).toEqual(['f:fld_a', `f:${TESTS}`]);
  });

  it('a parent cycle or unknown parent puts the folder at the top level', () => {
    const tree = buildTree([folder('a', 'A', 'b'), folder('b', 'B', 'a'), folder('c', 'C', 'nope')], []);
    expect(tree.roots.map(nodeKey).sort()).toEqual(['f:a', 'f:b', 'f:c']);
  });

  it('works without folders (a server without project folders): a flat list', () => {
    const tree = buildTree([], [project('a', 'A'), project('b', 'B')]);
    expect(tree.roots.map(nodeKey)).toEqual(['p:a', 'p:b']);
  });

  it('folderPath walks up to the top level', () => {
    const tree = buildTree(FOLDERS, PROJECTS);
    expect(folderPath(tree, 'fld_cli').map((f) => f.folder_id)).toEqual(['fld_mix', 'fld_cli']);
    expect(folderPath(tree, null)).toEqual([]);
  });
});

describe('visibleRows', () => {
  it('everything collapsed by default; «Тестові треки» last after a divider', () => {
    const rows = visibleRows(buildTree(FOLDERS, PROJECTS).roots, new Set());
    expect(rows.map((r) => nodeKey(r.node))).toEqual(['f:fld_imp', 'f:fld_mix', 'f:fld_empty', 'f:fld_9', 'f:fld_10', 'p:p_new', 'p:p_lost', `f:${TESTS}`]);
    expect(rows.filter((r) => r.divider).map((r) => nodeKey(r.node))).toEqual([`f:${TESTS}`]);
  });

  it('expanded folders show their children one level deeper', () => {
    const rows = visibleRows(buildTree(FOLDERS, PROJECTS).roots, new Set(['fld_mix', 'fld_cli']));
    const at = rows.findIndex((r) => nodeKey(r.node) === 'f:fld_mix');
    expect(rows.slice(at, at + 5).map((r) => [nodeKey(r.node), r.depth, r.parentId])).toEqual([
      ['f:fld_mix', 0, null],
      ['f:fld_cli', 1, 'fld_mix'],
      ['p:p_c1', 2, 'fld_cli'],
      ['p:p_kal', 1, 'fld_mix'],
      ['p:p_demo', 1, 'fld_mix'],
    ]);
  });
});

describe('remembered state', () => {
  it('round-trips the expanded folders and the last folder per tenant', () => {
    saveTreeState('acme', { expanded: new Set(['fld_mix', TESTS]), last: 'fld_mix' });
    expect(localStorage.getItem(stateKey('acme'))).toBeTruthy();
    expect(stateKey('acme')).toBe('rovox.folders.expanded.acme');
    const s = loadTreeState('acme');
    expect([...s.expanded].sort()).toEqual(['fld_mix', TESTS].sort());
    expect(s.last).toBe('fld_mix');
    expect(loadTreeState('other').expanded.size).toBe(0);
  });

  it('broken JSON or unavailable storage give a collapsed tree', () => {
    localStorage.setItem(stateKey('t'), '{nope');
    expect(loadTreeState('t').expanded.size).toBe(0);
    vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
      throw new Error('denied');
    });
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new Error('denied');
    });
    expect(loadTreeState('t').expanded.size).toBe(0);
    expect(() => saveTreeState('t', { expanded: new Set(['x']) })).not.toThrow();
  });
});

describe('picker tree', () => {
  const fakeApp = (folders = FOLDERS, projects = PROJECTS) => {
    const cmd = vi.fn(async (name: string, _params?: Record<string, unknown>): Promise<{ ok: boolean; data: unknown }> => {
      if (name === 'project.catalog') return { ok: true, data: { projects, folders, folders_version: 1 } };
      if (name === 'folder.rename') return { ok: true, data: { folder: {} } };
      return { ok: true, data: {} };
    });
    const openProject = vi.fn();
    const app = { tenant: 'acme', projects: new Map(), cmd, openProject, setActive: vi.fn(), catalogChanged: new Emitter(), toast: vi.fn() } as unknown as App;
    return { app, cmd, openProject };
  };
  const mount = async (app: App) => {
    const picker = projectPicker(app);
    document.body.append(picker.el);
    await tick();
    return picker;
  };
  const items = (root: Element) => [...root.querySelectorAll<HTMLElement>('[role=treeitem]')];
  const labelOf = (el: HTMLElement) => el.querySelector('.pname')!.textContent;

  it('renders the tree: ARIA, folders with counts, «Тестові треки» translated by id, muted, collapsed, after a divider', async () => {
    const { app, cmd } = fakeApp();
    const picker = await mount(app);
    expect(cmd).toHaveBeenCalledWith('project.catalog', { include_folders: true, include_system: true });
    expect(picker.el.querySelector('[role=tree]')).not.toBeNull();
    const rows = items(picker.el);
    expect(rows.map(labelOf)).toEqual(['Імпорт REAPER', 'Мікси 2026', 'Порожня', 'Mix 9', 'mix 10', 'Новий проєкт', 'Orphan', 'Тестові треки']);
    const tests = rows[rows.length - 1]!;
    expect(tests.classList.contains('system')).toBe(true);
    expect(tests.getAttribute('aria-expanded')).toBe('false');
    expect(tests.previousElementSibling?.classList.contains('tree-divider')).toBe(true);
    expect(rows[1]!.querySelector('.cnt')!.textContent).toBe('2 проєкти · 1 папка');
    expect(rows[1]!.getAttribute('aria-level')).toBe('1');
    // one tab stop
    expect(rows.filter((r) => r.tabIndex === 0)).toHaveLength(1);
  });

  it('keyboard: ↑↓ move, → expands (remembered), → again enters, ← goes to the parent and collapses, Enter opens / toggles', async () => {
    const { app, openProject } = fakeApp();
    const picker = await mount(app);
    let rows = items(picker.el);
    rows[0]!.focus();
    key(rows[0]!, 'ArrowDown');
    key(document.activeElement!, 'ArrowDown');
    key(document.activeElement!, 'ArrowUp');
    expect(labelOf(document.activeElement as HTMLElement)).toBe('Мікси 2026');
    expect(picker.el.querySelector('.crumbs')!.textContent).toBe('Усі проєкти › Мікси 2026');
    key(document.activeElement!, 'ArrowRight');
    expect(document.activeElement!.getAttribute('aria-expanded')).toBe('true');
    expect(JSON.parse(localStorage.getItem('rovox.folders.expanded.acme')!).expanded).toEqual(['fld_mix']);
    rows = items(picker.el);
    expect(rows.map(labelOf).slice(1, 5)).toEqual(['Мікси 2026', 'Клієнти', 'Kalyna — live', 'Demo vocals']);
    expect(rows[2]!.style.marginLeft).toBe('16px');
    key(document.activeElement!, 'ArrowRight'); // into the first child
    expect(labelOf(document.activeElement as HTMLElement)).toBe('Клієнти');
    key(document.activeElement!, 'Enter'); // a folder toggles
    expect(document.activeElement!.getAttribute('aria-expanded')).toBe('true');
    expect(items(picker.el).map(labelOf)).toContain('Client one');
    key(document.activeElement!, 'ArrowDown');
    expect(labelOf(document.activeElement as HTMLElement)).toBe('Client one');
    expect((document.activeElement as HTMLElement).style.marginLeft).toBe('32px');
    key(document.activeElement!, 'Enter');
    expect(openProject).toHaveBeenCalledWith('p_c1');
    key(document.activeElement!, 'ArrowLeft'); // project -> its folder
    expect(labelOf(document.activeElement as HTMLElement)).toBe('Клієнти');
    key(document.activeElement!, 'ArrowLeft'); // collapse
    expect(document.activeElement!.getAttribute('aria-expanded')).toBe('false');
    key(document.activeElement!, 'ArrowLeft'); // -> parent
    expect(labelOf(document.activeElement as HTMLElement)).toBe('Мікси 2026');
    key(document.activeElement!, 'End');
    expect(labelOf(document.activeElement as HTMLElement)).toBe('Тестові треки');
  });

  it('restores the expanded folders from localStorage (per tenant)', async () => {
    localStorage.setItem('rovox.folders.expanded.acme', JSON.stringify({ expanded: ['fld_mix', TESTS, 'fld_gone'] }));
    const { app } = fakeApp();
    const picker = await mount(app);
    const names = items(picker.el).map(labelOf);
    expect(names).toContain('Kalyna — live');
    expect(names.slice(-3)).toEqual(['Тестові треки', 'mukola', 'mykola']);
    expect(names).not.toContain('Client one'); // Клієнти stays collapsed
    // a folder that is gone is forgotten
    expect(JSON.parse(localStorage.getItem('rovox.folders.expanded.acme')!).expanded.sort()).toEqual(['fld_mix', TESTS].sort());
  });

  it('F2 renames a folder, Delete deletes only an empty folder and never «Тестові треки»', async () => {
    const { app, cmd } = fakeApp();
    const picker = await mount(app);
    const row = (name: string) => items(picker.el).find((r) => labelOf(r) === name)!;
    row('Імпорт REAPER').focus();
    key(row('Імпорт REAPER'), 'F2');
    const input = document.querySelector<HTMLInputElement>('.scrim input')!;
    expect(input.value).toBe('Імпорт REAPER');
    input.value = '  REAPER  ';
    key(input, 'Enter');
    await tick();
    expect(cmd).toHaveBeenCalledWith('folder.rename', { folder_id: 'fld_imp', name: 'REAPER' });

    row('Мікси 2026').focus();
    key(row('Мікси 2026'), 'Delete');
    row('Тестові треки').focus();
    key(row('Тестові треки'), 'Delete');
    key(row('Тестові треки'), 'F2');
    expect(document.querySelector('.scrim')).toBeNull();
    expect(cmd.mock.calls.filter((c) => c[0] === 'folder.delete')).toHaveLength(0);
    row('Порожня').focus();
    key(row('Порожня'), 'Delete');
    await tick();
    expect(cmd).toHaveBeenCalledWith('folder.delete', { folder_id: 'fld_empty' });
  });

  it('«＋ Нова папка» creates in the folder of the focused row', async () => {
    const { app, cmd } = fakeApp();
    cmd.mockImplementation(async (name: string) => {
      if (name === 'project.catalog') return { ok: true, data: { projects: PROJECTS, folders: FOLDERS } };
      if (name === 'folder.create') return { ok: true, data: { folder: folder('fld_new', 'Нова', 'fld_mix') } };
      return { ok: true, data: {} };
    });
    const picker = await mount(app);
    items(picker.el).find((r) => labelOf(r) === 'Мікси 2026')!.focus();
    picker.el.querySelector<HTMLButtonElement>('.folder-new')!.click();
    const input = document.querySelector<HTMLInputElement>('.scrim input')!;
    expect(document.querySelector('.scrim header')!.textContent).toBe('Нова папка в «Мікси 2026»');
    input.value = 'Нова';
    key(input, 'Enter');
    await tick();
    expect(cmd).toHaveBeenCalledWith('folder.create', { name: 'Нова', parent_id: 'fld_mix' });
    // the parent opens so the new folder is visible
    expect(JSON.parse(localStorage.getItem('rovox.folders.expanded.acme')!).expanded).toContain('fld_mix');
  });
});
