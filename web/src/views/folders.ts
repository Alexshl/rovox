// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project folders: the pure tree helpers (build, sort, flatten, remembered state), the folder commands behind text dialogs,
// the "⋯" row menu and the «Перемістити…» dialog. The picker tree itself lives in projects.ts.
import { getLocale, plural, t } from '../i18n';
import { clear, h } from '../lib/dom';
import type { App } from '../store/session';
import { openModal, textDialog } from './dialog';
import type { ServerProject } from './projects';

/** `folder_id` of the top level in commands (a project with folder_id null is there). */
export const ROOT = 'root';
/** The system folder «Тестові треки»: always present, always last, no subfolders, cannot be renamed, moved or deleted. */
export const TESTS = 'tests';

export interface Folder {
  folder_id: string;
  name: string;
  parent_id: string | null;
  system?: boolean;
  created_at?: string;
  updated_at?: string;
  project_count?: number;
  folder_count?: number;
}

export interface FolderNode {
  kind: 'folder';
  folder: Folder;
  /** Subfolders first (by name), then the projects (newest first), «Тестові треки» last at the top level. */
  children: TreeNode[];
  /** Direct projects and subfolders of the folder, from the loaded catalog. */
  projectCount: number;
  folderCount: number;
  system: boolean;
}

export interface ProjectNode {
  kind: 'project';
  project: ServerProject;
}

export type TreeNode = FolderNode | ProjectNode;

export interface FolderTree {
  roots: TreeNode[];
  folders: Map<string, FolderNode>;
  /** Folder id of every project (null = top level), after unknown folders were mapped to the top level. */
  projectFolder: Map<string, string | null>;
}

/** The shown name of a folder: «Тестові треки» is translated by its id, whatever name the server sends. */
export function folderName(f: Folder): string {
  return f.folder_id === TESTS ? t('folders.tests') : f.name;
}

export function isSystem(f: Folder): boolean {
  return f.folder_id === TESTS || f.system === true;
}

/** A folder can be deleted only when it holds neither projects nor subfolders (by the loaded catalog and by the server's counters). */
export function isEmptyFolder(n: FolderNode): boolean {
  return n.projectCount === 0 && n.folderCount === 0 && !(n.folder.project_count ?? 0) && !(n.folder.folder_count ?? 0);
}

/**
 * Builds the folder tree. Projects keep the catalog order (the server lists them newest first); folders are sorted by name
 * (case-insensitive, numbers by value). A project or folder whose parent is unknown (or a parent cycle) lands at the top level.
 */
export function buildTree(folders: readonly Folder[], projects: readonly ServerProject[]): FolderTree {
  const nodes = new Map<string, FolderNode>();
  for (const f of folders) {
    if (!f.folder_id || f.folder_id === ROOT || nodes.has(f.folder_id)) continue;
    nodes.set(f.folder_id, { kind: 'folder', folder: f, children: [], projectCount: 0, folderCount: 0, system: isSystem(f) });
  }
  // projects in «Тестові треки» from a server that did not list the folder still get it
  if (!nodes.has(TESTS) && projects.some((p) => p.folder_id === TESTS)) {
    nodes.set(TESTS, { kind: 'folder', folder: { folder_id: TESTS, name: 'tests', parent_id: null, system: true }, children: [], projectCount: 0, folderCount: 0, system: true });
  }
  const parentOf = (n: FolderNode): string | null => {
    if (n.folder.folder_id === TESTS) return null;
    const pid = n.folder.parent_id;
    if (!pid || pid === ROOT || pid === TESTS || !nodes.has(pid)) return null;
    // a parent cycle (never sent by a healthy server) would hide the folders: put the folder at the top level
    const seen = new Set([n.folder.folder_id]);
    for (let at: string | null = pid; at; ) {
      if (seen.has(at)) return null;
      seen.add(at);
      const up: string | null | undefined = nodes.get(at)?.folder.parent_id;
      at = up && up !== ROOT && nodes.has(up) ? up : null;
    }
    return pid;
  };
  const coll = new Intl.Collator(getLocale(), { sensitivity: 'base', numeric: true });
  const roots: TreeNode[] = [];
  const childFolders = new Map<string | null, FolderNode[]>();
  for (const n of nodes.values()) {
    const pid = parentOf(n);
    const list = childFolders.get(pid) ?? [];
    list.push(n);
    childFolders.set(pid, list);
  }
  const projectFolder = new Map<string, string | null>();
  const childProjects = new Map<string | null, ServerProject[]>();
  for (const p of projects) {
    const fid = p.folder_id && p.folder_id !== ROOT && nodes.has(p.folder_id) ? p.folder_id : null;
    projectFolder.set(p.project_id, fid);
    const list = childProjects.get(fid) ?? [];
    list.push(p);
    childProjects.set(fid, list);
  }
  const fill = (pid: string | null, into: TreeNode[]) => {
    const fs = (childFolders.get(pid) ?? []).filter((n) => n.folder.folder_id !== TESTS);
    fs.sort((a, b) => coll.compare(folderName(a.folder), folderName(b.folder)) || a.folder.folder_id.localeCompare(b.folder.folder_id));
    for (const f of fs) {
      fill(f.folder.folder_id, f.children);
      into.push(f);
    }
    for (const p of childProjects.get(pid) ?? []) into.push({ kind: 'project', project: p });
  };
  fill(null, roots);
  const tests = nodes.get(TESTS);
  if (tests) {
    fill(TESTS, tests.children);
    roots.push(tests);
  }
  for (const n of nodes.values()) {
    n.folderCount = n.children.filter((c) => c.kind === 'folder').length;
    n.projectCount = n.children.length - n.folderCount;
  }
  return { roots, folders: nodes, projectFolder };
}

export interface VisibleRow {
  node: TreeNode;
  /** 0 at the top level; the row is indented 16 px per level. */
  depth: number;
  /** The folder the row is in (null = top level). */
  parentId: string | null;
  /** A hairline above the row («Тестові треки» after the other rows). */
  divider: boolean;
}

export function nodeKey(n: TreeNode): string {
  return n.kind === 'folder' ? `f:${n.folder.folder_id}` : `p:${n.project.project_id}`;
}

/** The rows on screen: the children of expanded folders follow their folder. */
export function visibleRows(roots: readonly TreeNode[], expanded: ReadonlySet<string>): VisibleRow[] {
  const out: VisibleRow[] = [];
  const walk = (nodes: readonly TreeNode[], depth: number, parentId: string | null) => {
    for (const node of nodes) {
      const divider = depth === 0 && node.kind === 'folder' && node.folder.folder_id === TESTS && out.length > 0;
      out.push({ node, depth, parentId, divider });
      if (node.kind === 'folder' && expanded.has(node.folder.folder_id)) walk(node.children, depth + 1, node.folder.folder_id);
    }
  };
  walk(roots, 0, null);
  return out;
}

/** The folders from the top level down to `id` (empty for the top level). */
export function folderPath(tree: FolderTree, id: string | null): Folder[] {
  const out: Folder[] = [];
  const seen = new Set<string>();
  for (let at = id; at && !seen.has(at); ) {
    seen.add(at);
    const n = tree.folders.get(at);
    if (!n) break;
    out.unshift(n.folder);
    at = n.folder.folder_id === TESTS ? null : n.folder.parent_id;
  }
  return out;
}

export function countText(n: FolderNode): string {
  const parts: string[] = [];
  if (n.projectCount || !n.folderCount) parts.push(plural(n.projectCount, 'folders.projects'));
  if (n.folderCount) parts.push(plural(n.folderCount, 'folders.folders'));
  return parts.join(' · ');
}

//==============================================================================
// remembered tree state (per tenant, in this browser)

export interface TreeState {
  expanded: Set<string>;
  /** The folder the user worked in last (opened a project from, created or expanded); the picker starts there. */
  last?: string;
}

export const stateKey = (tenant: string) => `rovox.folders.expanded.${tenant}`;

/** Everything collapsed (so «Тестові треки» too) when nothing is remembered or storage is unavailable. */
export function loadTreeState(tenant: string): TreeState {
  try {
    const v = JSON.parse(localStorage.getItem(stateKey(tenant)) ?? 'null') as unknown;
    const raw = Array.isArray(v) ? v : v && typeof v === 'object' ? (v as { expanded?: unknown }).expanded : [];
    const expanded = new Set(Array.isArray(raw) ? raw.filter((x): x is string => typeof x === 'string') : []);
    const last = v && typeof v === 'object' && !Array.isArray(v) ? (v as { last?: unknown }).last : undefined;
    return typeof last === 'string' ? { expanded, last } : { expanded };
  } catch {
    return { expanded: new Set() };
  }
}

export function saveTreeState(tenant: string, s: TreeState): void {
  try {
    localStorage.setItem(stateKey(tenant), JSON.stringify({ expanded: [...s.expanded], ...(s.last ? { last: s.last } : {}) }));
  } catch {
    /* private mode / quota: the tree simply starts collapsed next time */
  }
}

//==============================================================================
// folder commands

const cleanName = (v: string | null) => (v ?? '').trim();

/** Asks for a name and creates a folder in `parentId` (null / tests = top level). Resolves with the new folder or null. */
export async function createFolder(app: App, parent: Folder | null): Promise<Folder | null> {
  const p = parent && parent.folder_id !== TESTS ? parent : null;
  const name = cleanName(
    await textDialog({ title: p ? t('folders.newSubTitle', { name: folderName(p) }) : t('folders.newTitle'), label: t('folders.nameLabel'), okText: t('folders.create') }),
  );
  if (!name) return null;
  const r = await app.cmd<{ folder: Folder }>('folder.create', p ? { name, parent_id: p.folder_id } : { name });
  return r.ok && r.data ? r.data.folder : null;
}

export async function renameFolder(app: App, f: Folder): Promise<boolean> {
  if (isSystem(f)) return false;
  const name = cleanName(await textDialog({ title: t('folders.renameTitle', { name: f.name }), label: t('folders.nameLabel'), value: f.name, okText: t('folders.menu.rename') }));
  if (!name || name === f.name) return false;
  return (await app.cmd('folder.rename', { folder_id: f.folder_id, name })).ok;
}

export async function deleteFolder(app: App, n: FolderNode): Promise<boolean> {
  if (n.system || !isEmptyFolder(n)) return false;
  return (await app.cmd('folder.delete', { folder_id: n.folder.folder_id })).ok;
}

//==============================================================================
// the "⋯" row menu

export interface RowMenuEntry {
  label: string;
  key?: string;
  disabled?: boolean;
  title?: string;
  separator?: boolean;
  action?: () => void;
}

let openMenuClose: (() => void) | null = null;

export function closeRowMenu(): void {
  openMenuClose?.();
}

/** A small popup menu under `anchor` (fixed position, so a scrolling list does not clip it). ↑↓ move, Esc closes and returns the focus. */
export function openRowMenu(anchor: HTMLElement, host: HTMLElement, entries: readonly RowMenuEntry[], label: string, returnFocus?: HTMLElement): HTMLElement {
  closeRowMenu();
  const menu = h('div', { class: 'row-menu', attrs: { role: 'menu', 'aria-label': label } });
  for (const e of entries) {
    if (e.separator) {
      menu.append(h('div', { class: 'sep', attrs: { role: 'separator' } }));
      continue;
    }
    const item = h('button', { class: 'item', attrs: { role: 'menuitem', type: 'button' }, ...(e.title ? { title: e.title } : {}) }, h('span', { text: e.label }), h('span', { class: 'key', text: e.key ?? '' }));
    item.disabled = !!e.disabled;
    item.addEventListener('click', () => {
      close(false);
      e.action?.();
    });
    menu.append(item);
  }
  const items = () => [...menu.querySelectorAll<HTMLButtonElement>('button.item:not(:disabled)')];
  const onDoc = (ev: PointerEvent) => {
    if (!menu.contains(ev.target as Node) && ev.target !== anchor && !anchor.contains(ev.target as Node)) close(false);
  };
  const close = (refocus: boolean) => {
    if (!menu.isConnected) return;
    menu.remove();
    document.removeEventListener('pointerdown', onDoc, true);
    anchor.setAttribute('aria-expanded', 'false');
    if (openMenuClose === closer) openMenuClose = null;
    if (refocus) (returnFocus ?? anchor).focus();
  };
  const closer = () => close(false);
  menu.addEventListener('keydown', (e) => {
    const list = items();
    const at = list.indexOf(document.activeElement as HTMLButtonElement);
    let handled = true;
    if (e.key === 'ArrowDown') list[(at + 1) % list.length]?.focus();
    else if (e.key === 'ArrowUp') list[(at <= 0 ? list.length : at) - 1]?.focus();
    else if (e.key === 'Home') list[0]?.focus();
    else if (e.key === 'End') list[list.length - 1]?.focus();
    else if (e.key === 'Escape' || e.key === 'Tab') close(true);
    else handled = false;
    if (handled) {
      e.preventDefault();
      e.stopPropagation(); // Esc must not close the dialog around the picker
    }
  });
  host.append(menu);
  const r = anchor.getBoundingClientRect();
  const w = menu.offsetWidth || 220;
  const hgt = menu.offsetHeight || 0;
  const vw = window.innerWidth || 1024;
  const vh = window.innerHeight || 768;
  menu.style.left = `${Math.max(8, Math.min(r.right - w, vw - w - 8))}px`;
  menu.style.top = `${r.bottom + 2 + hgt > vh && r.top - hgt - 2 > 0 ? r.top - hgt - 2 : r.bottom + 2}px`;
  anchor.setAttribute('aria-expanded', 'true');
  document.addEventListener('pointerdown', onDoc, true);
  openMenuClose = closer;
  items()[0]?.focus();
  return menu;
}

//==============================================================================
// «Перемістити «name»» dialog

export interface MoveTarget {
  kind: 'project' | 'folder';
  id: string;
  name: string;
  /** Where it is now (null = top level); undefined = not known yet (looked up from the catalog). */
  current?: string | null;
}

interface Catalog {
  projects: ServerProject[];
  folders?: Folder[];
}

async function loadFolders(app: App, target: MoveTarget): Promise<{ folders: Folder[]; current: string | null } | null> {
  if (target.kind === 'project' && target.current === undefined) {
    const r = await app.cmd<Catalog>('project.catalog', { include_folders: true, include_system: true });
    if (!r.ok || !r.data) return null;
    const p = r.data.projects.find((x) => x.project_id === target.id);
    return { folders: r.data.folders ?? [], current: p?.folder_id && p.folder_id !== ROOT ? p.folder_id : null };
  }
  const r = await app.cmd<{ folders: Folder[] }>('folder.list');
  if (!r.ok || !r.data) return null;
  return { folders: r.data.folders, current: target.current ?? null };
}

/**
 * The move dialog: a radio tree (Верхній рівень, the folders, «Тестові треки» for a project), the current place marked
 * "зараз тут", «＋ Нова папка» and Перемістити. A project goes through `project.move`, a folder through `folder.move`
 * (its own subtree and «Тестові треки» are not offered). Resolves true after a successful move.
 */
export function openMoveDialog(app: App, target: MoveTarget): Promise<boolean> {
  return new Promise((resolve) => {
    let moved = false;
    let folders: Folder[] = [];
    let current: string | null = target.current ?? null;
    let selected: string = ROOT;
    const title = t('folders.moveTitle', { name: target.name });
    const list = h('div', { class: 'move-tree', attrs: { role: 'radiogroup', 'aria-label': t('folders.moveTargets') } });
    const newBtn = h('button', { class: 'btn new-folder', text: t('folders.new'), attrs: { type: 'button' } });
    const cancel = h('button', { class: 'btn', text: t('dialog.cancel'), attrs: { type: 'button' } });
    const ok = h('button', { class: 'btn primary', text: t('folders.moveOk'), attrs: { type: 'button' } });
    const dialog = h('div', { class: 'dialog move-dialog' }, h('header', { text: title }), h('div', { class: 'body' }, h('div', { class: 'full' }, list)), h('footer', {}, newBtn, cancel, ok));
    const modal = openModal(title, dialog, () => resolve(moved));
    const group = `move-${Math.random().toString(36).slice(2)}`;

    const render = () => {
      const tree = buildTree(folders, []);
      // a folder cannot go into itself, its subfolders or «Тестові треки»
      const banned = new Set<string>();
      if (target.kind === 'folder') {
        banned.add(TESTS);
        const walk = (n: FolderNode) => {
          banned.add(n.folder.folder_id);
          for (const c of n.children) if (c.kind === 'folder') walk(c);
        };
        const self = tree.folders.get(target.id);
        if (self) walk(self);
      }
      clear(list);
      const option = (id: string, label: string, depth: number, count: number | null, cls = '') => {
        const input = h('input', { attrs: { type: 'radio', name: group, value: id } });
        input.checked = id === selected;
        const isCur = id === (current ?? ROOT);
        const row = h(
          'label',
          { class: `pick${cls}${input.checked ? ' sel' : ''}${isCur ? ' cur' : ''}` },
          input,
          h('span', { class: 'pname', text: label }),
          h('span', { class: 'cnt', text: [count === null ? '' : String(count), isCur ? t('folders.moveHere') : ''].filter(Boolean).join(' · ') }),
        );
        row.style.marginLeft = `${depth * 16}px`;
        input.addEventListener('change', () => {
          selected = id;
          for (const el of list.querySelectorAll('.pick')) el.classList.toggle('sel', el === row);
          ok.disabled = selected === (current ?? ROOT);
        });
        list.append(row);
      };
      option(ROOT, t('folders.moveTop'), 0, null, ' top');
      for (const row of visibleRows(tree.roots, new Set(tree.folders.keys()))) {
        if (row.node.kind !== 'folder') continue;
        const f = row.node.folder;
        if (banned.has(f.folder_id)) continue;
        if (row.divider) list.append(h('div', { class: 'divider', attrs: { 'aria-hidden': 'true' } }));
        option(f.folder_id, folderName(f), row.depth, f.project_count ?? row.node.projectCount, row.node.system ? ' folder system' : ' folder');
      }
      ok.disabled = selected === (current ?? ROOT);
    };

    let first = true;
    const load = async () => {
      const r = await loadFolders(app, { ...target, current: first ? target.current : current });
      if (!r) return;
      folders = r.folders;
      if (first && target.current === undefined) {
        current = r.current;
        selected = current ?? ROOT;
      }
      first = false;
      render();
    };

    newBtn.addEventListener('click', () => {
      const parent = selected !== ROOT && selected !== TESTS ? (folders.find((f) => f.folder_id === selected) ?? null) : null;
      void createFolder(app, parent).then(async (f) => {
        if (!f) return;
        selected = f.folder_id;
        await load();
        [...list.querySelectorAll<HTMLInputElement>('input')].find((i) => i.value === f.folder_id)?.focus();
      });
    });
    cancel.addEventListener('click', () => modal.close());
    ok.addEventListener('click', async () => {
      if (selected === (current ?? ROOT)) return;
      ok.disabled = true;
      const r =
        target.kind === 'project'
          ? await app.cmd('project.move', { project_id: target.id, folder_id: selected })
          : await app.cmd('folder.move', { folder_id: target.id, parent_id: selected });
      if (!r.ok) {
        ok.disabled = false;
        return; // app.cmd showed the error; the dialog stays for another choice
      }
      moved = true;
      const dest = selected === ROOT ? t('folders.moveTop') : folderName(folders.find((f) => f.folder_id === selected) ?? { folder_id: selected, name: selected, parent_id: null });
      app.toast(t('folders.moved', { name: target.name, folder: dest }));
      modal.close();
    });

    selected = current ?? ROOT;
    render();
    void load().then(() => (list.querySelector<HTMLInputElement>('input:checked') ?? list.querySelector('input'))?.focus());
  });
}
