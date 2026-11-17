// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project picker: every project of the server, open or only on disk (project.catalog), as a tree of project folders
// (folders first by name, projects newest first, «Тестові треки» last), to open with a click or Enter, plus opening by path / id / name.
import { clear, h } from '../lib/dom';
import { fmtDuration, fmtShortDateTime, plural, t } from '../i18n';
import type { App } from '../store/session';
import { openModal } from './dialog';
import {
  type Folder,
  type FolderNode,
  type FolderTree,
  type RowMenuEntry,
  type TreeState,
  type VisibleRow,
  TESTS,
  buildTree,
  closeRowMenu,
  countText,
  createFolder,
  deleteFolder,
  folderName,
  folderPath,
  isEmptyFolder,
  loadTreeState,
  nodeKey,
  openMoveDialog,
  openRowMenu,
  renameFolder,
  saveTreeState,
  visibleRows,
} from './folders';
import { importController } from './importwizard';

export interface ServerProject {
  project_id: string;
  name: string;
  tracks?: number;
  path?: string;
  dirty?: boolean;
  /** the newest content exists only in an autosave (closed) or is not saved yet (open). */
  unsaved?: boolean;
  created_at?: string;
  duration_s?: number;
  modified?: number | string;
  /** true while the server has the project open; false for a project that only exists on disk. */
  open?: boolean;
  /** The folder of the project (null = top level, "tests" = «Тестові треки»); from servers with project folders. */
  folder_id?: string | null;
}

function formatModified(v: number | string, seconds = false): string {
  return fmtShortDateTime(new Date(typeof v === 'number' && v < 1e11 ? v * 1000 : v), seconds);
}

/**
 * The one-line description under a project name: tracks, duration and last modification when the server reports them.
 * `showCreated` adds the creation time (for same-named projects); `seconds` adds seconds to the times (when rows would still look alike).
 */
export function projectMeta(p: ServerProject, showCreated = false, seconds = false): string {
  const parts: string[] = [];
  if (typeof p.tracks === 'number') parts.push(plural(p.tracks, 'picker.tracks'));
  if (typeof p.duration_s === 'number') parts.push(fmtDuration(p.duration_s));
  const modified = p.modified !== undefined ? formatModified(p.modified, seconds) : '';
  if (modified) parts.push(t('picker.modified', { time: modified }));
  if (showCreated && p.created_at) {
    // same-named projects are told apart by creation time; skipped when it says the same as "modified"
    const created = fmtShortDateTime(new Date(p.created_at), seconds);
    if (created && created !== modified) parts.push(t('picker.created', { time: created }));
  }
  if (p.unsaved) parts.push(t('picker.unsaved'));
  // a part never breaks inside ("created 02.11 18:05" stays on one line); the row wraps only between parts
  return parts.map((s) => s.replace(/ /g, '\u00a0')).join(' · ');
}

export async function fetchServerProjects(app: App): Promise<ServerProject[]> {
  return (await fetchCatalog(app)).projects;
}

export interface Catalog {
  projects: ServerProject[];
  folders: Folder[];
}

/** The whole catalog with the folders, «Тестові треки» included (without `include_system` the server hides its projects). */
export async function fetchCatalog(app: App): Promise<Catalog> {
  const r = await app.cmd<{ projects: ServerProject[]; folders?: Folder[] }>('project.catalog', { include_folders: true, include_system: true });
  return r.ok && r.data ? { projects: r.data.projects ?? [], folders: r.data.folders ?? [] } : { projects: [], folders: [] };
}

export interface ProjectPicker {
  el: HTMLElement;
  refresh: () => Promise<void>;
  focus: () => void;
  /** Stops listening to catalog changes (the dialog around the picker closed). */
  dispose: () => void;
}

/** Catalog events (`projects.changed`, `folders.changed`) refresh an open picker at most this often. */
export const PICKER_REFRESH_MS = 250;

/** The meta lines of all projects: same-named projects get their creation time, rows that still read alike get seconds. */
function metaLines(projects: readonly ServerProject[]): Map<string, string> {
  const nameCount = new Map<string, number>();
  for (const p of projects) nameCount.set(p.name, (nameCount.get(p.name) ?? 0) + 1);
  const shortMeta = new Map(projects.map((p) => [p.project_id, projectMeta(p, (nameCount.get(p.name) ?? 0) > 1)]));
  const rowCount = new Map<string, number>();
  for (const p of projects) {
    const k = `${p.name}\n${shortMeta.get(p.project_id)}`;
    rowCount.set(k, (rowCount.get(k) ?? 0) + 1);
  }
  const out = new Map<string, string>();
  for (const p of projects) {
    const dup = (nameCount.get(p.name) ?? 0) > 1;
    const alike = (rowCount.get(`${p.name}\n${shortMeta.get(p.project_id)}`) ?? 0) > 1;
    out.set(p.project_id, alike ? projectMeta(p, dup, true) : shortMeta.get(p.project_id)!);
  }
  return out;
}

/** `onOpened` runs after the user picked a project (e.g. to close the dialog around the picker). */
export function projectPicker(app: App, onOpened?: () => void): ProjectPicker {
  const tenant = app.tenant ?? 'default';
  const state: TreeState = loadTreeState(tenant);
  const newFolder = h('button', { class: 'btn folder-new', text: t('folders.new'), attrs: { type: 'button' } });
  const crumbs = h('nav', { class: 'crumbs', attrs: { 'aria-label': t('folders.path') } });
  const list = h('div', { class: 'project-list project-tree', attrs: { role: 'tree', 'aria-label': t('picker.list') } });
  const path = h('input', { attrs: { type: 'text', placeholder: t('picker.pathPlaceholder'), 'aria-label': t('picker.pathLabel') } });
  const go = h('button', { class: 'btn primary', text: t('picker.open') });
  const el = h('div', { class: 'project-picker' }, h('div', { class: 'tree-toolbar' }, newFolder, crumbs), list, h('div', { class: 'open-path' }, path, go));

  let catalog: Catalog = { projects: [], folders: [] };
  let tree: FolderTree = buildTree([], []);
  let rows: VisibleRow[] = [];
  let loaded = false;
  /** The row with the roving tabindex (also where «＋ Нова папка» creates). */
  let currentKey: string | null = null;

  const save = () => saveTreeState(tenant, state);
  const rowEls = () => [...list.querySelectorAll<HTMLElement>('[role=treeitem]')];
  const rowOf = (key: string | null) => (key ? (rowEls().find((r) => r.dataset.key === key) ?? null) : null);
  const visibleOf = (key: string | null) => rows.find((r) => nodeKey(r.node) === key);

  /** The folder a row stands for when creating a folder or showing the path: a folder itself, else the folder the project is in. */
  const locationOf = (row: VisibleRow | undefined): string | null => {
    if (!row) return state.last && tree.folders.has(state.last) ? state.last : null;
    return row.node.kind === 'folder' ? row.node.folder.folder_id : row.parentId;
  };

  const renderCrumbs = () => {
    clear(crumbs);
    const trail = folderPath(tree, locationOf(visibleOf(currentKey)));
    crumbs.append(trail.length ? h('span', { text: t('folders.all') }) : h('b', { text: t('folders.all') }));
    trail.forEach((f, i) => {
      crumbs.append(h('span', { class: 'sep', text: ' › ', attrs: { 'aria-hidden': 'true' } }));
      crumbs.append(h(i === trail.length - 1 ? 'b' : 'span', { text: folderName(f) }));
    });
  };

  const setCurrent = (key: string | null, focus = false) => {
    currentKey = key;
    for (const r of rowEls()) {
      const on = r.dataset.key === key;
      r.tabIndex = on ? 0 : -1;
      r.setAttribute('aria-selected', String(on));
    }
    renderCrumbs();
    if (focus) rowOf(key)?.focus();
  };

  const open = (p: ServerProject) => {
    const fid = tree.projectFolder.get(p.project_id) ?? null;
    if (fid) {
      state.last = fid;
      save();
    }
    if (app.projects.has(p.project_id)) app.setActive(p.project_id);
    else void app.openProject(p.project_id);
    onOpened?.();
  };

  const toggle = (n: FolderNode, expand = !state.expanded.has(n.folder.folder_id)) => {
    const id = n.folder.folder_id;
    if (expand === state.expanded.has(id)) return;
    if (expand) {
      state.expanded.add(id);
      state.last = id;
    } else state.expanded.delete(id);
    save();
    render();
  };

  const moveProject = (p: ServerProject) => {
    void openMoveDialog(app, { kind: 'project', id: p.project_id, name: p.name || p.project_id, current: tree.projectFolder.get(p.project_id) ?? null }).then((moved) => {
      if (moved) void refresh();
    });
  };

  const afterChange = (ok: boolean | Folder | null) => {
    if (ok) void refresh();
  };

  const addFolder = (parent: Folder | null) => {
    void createFolder(app, parent).then((f) => {
      if (!f) return;
      if (parent && parent.folder_id !== TESTS) state.expanded.add(parent.folder_id);
      state.last = f.folder_id;
      save();
      currentKey = `f:${f.folder_id}`;
      void refresh().then(() => setCurrent(currentKey, true));
    });
  };

  const remove = (n: FolderNode) => {
    void deleteFolder(app, n).then((ok) => {
      if (!ok) return;
      state.expanded.delete(n.folder.folder_id);
      if (state.last === n.folder.folder_id) delete state.last;
      save();
      // the focus moves to the folder above (or the top)
      const at = rows.findIndex((r) => r.node === n);
      currentKey = at > 0 ? nodeKey(rows[at - 1]!.node) : null;
      void refresh().then(() => setCurrent(currentKey, true));
    });
  };

  const menuEntries = (row: VisibleRow): RowMenuEntry[] => {
    const n = row.node;
    if (n.kind === 'project') {
      // no "Перейменувати" for projects: renaming a project that is not open has no command yet
      return [
        { label: t('folders.menu.open'), key: 'Enter', action: () => open(n.project) },
        { label: t('folders.menu.move'), key: 'M', action: () => moveProject(n.project) },
      ];
    }
    const expanded = state.expanded.has(n.folder.folder_id);
    if (n.system) return [{ label: expanded ? t('folders.menu.collapse') : t('folders.menu.expand'), action: () => toggle(n) }];
    const empty = isEmptyFolder(n);
    return [
      { label: t('folders.menu.newSub'), action: () => addFolder(n.folder) },
      { label: t('folders.menu.rename'), key: 'F2', action: () => void renameFolder(app, n.folder).then(afterChange) },
      { label: t('folders.menu.moveFolder'), action: () => void openMoveDialog(app, { kind: 'folder', id: n.folder.folder_id, name: n.folder.name, current: n.folder.parent_id }).then(afterChange) },
      { separator: true, label: '' },
      { label: t('folders.menu.delete'), key: empty ? 'Del' : t('folders.menu.notEmpty'), disabled: !empty, action: () => remove(n) },
    ];
  };

  const showMenu = (rowEl: HTMLElement) => {
    const row = visibleOf(rowEl.dataset.key ?? null);
    const more = rowEl.querySelector<HTMLElement>('.more');
    if (!row || !more) return;
    setCurrent(nodeKey(row.node));
    const name = row.node.kind === 'folder' ? folderName(row.node.folder) : row.node.project.name || row.node.project.project_id;
    openRowMenu(more, el, menuEntries(row), t('folders.more', { name }), rowEl);
  };

  const rowEl = (row: VisibleRow, meta: Map<string, string>, setSize: number, pos: number): HTMLElement => {
    const n = row.node;
    const key = nodeKey(n);
    const attrs: Record<string, string> = { role: 'treeitem', 'aria-level': String(row.depth + 1), 'aria-setsize': String(setSize), 'aria-posinset': String(pos), 'data-key': key, tabindex: '-1', 'aria-selected': 'false' };
    if (n.kind === 'folder') {
      const expanded = state.expanded.has(n.folder.folder_id);
      attrs['aria-expanded'] = String(expanded);
      const name = folderName(n.folder);
      const count = countText(n);
      const el = h(
        'div',
        { class: `tree-row folder-row${n.system ? ' system' : ''}`, attrs: { ...attrs, 'aria-label': `${name}, ${count}` } },
        h('span', { class: 'tw', text: expanded ? '▼' : '▶', attrs: { 'aria-hidden': 'true' } }),
        h('span', { class: 'pname', text: name }),
        h('span', { class: 'cnt', text: count }),
        h('button', { class: 'more', text: '⋯', attrs: { type: 'button', tabindex: '-1', 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'aria-label': t('folders.more', { name }) } }),
      );
      el.style.marginLeft = `${row.depth * 16}px`;
      return el;
    }
    const p = n.project;
    const isOpen = app.projects.has(p.project_id) || p.open === true;
    const m = meta.get(p.project_id) ?? '';
    const el = h(
      'div',
      { class: `tree-row project-row${isOpen ? ' open' : ''}`, attrs: attrs, title: p.path ?? p.project_id },
      h('span', { class: 'tw', attrs: { 'aria-hidden': 'true' } }),
      h('span', { class: 'pname', text: p.name || p.project_id }),
      isOpen ? h('span', { class: 'pbadge', text: t('picker.opened') }) : h('span', { class: 'cnt' }),
      h('button', { class: 'more', text: '⋯', attrs: { type: 'button', tabindex: '-1', 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'aria-label': t('folders.more', { name: p.name || p.project_id }) } }),
      m ? h('span', { class: 'pmeta', text: m }) : null,
    );
    el.style.marginLeft = `${row.depth * 16}px`;
    return el;
  };

  function render(): void {
    const hadFocus = list.contains(document.activeElement);
    closeRowMenu();
    tree = buildTree(catalog.folders, catalog.projects);
    // folders that are gone are forgotten (only once the catalog is in: the first, empty render knows no folders)
    if (loaded) {
      let pruned = false;
      for (const id of [...state.expanded]) if (!tree.folders.has(id)) pruned = state.expanded.delete(id) || pruned;
      if (pruned) save();
    }
    rows = visibleRows(tree.roots, state.expanded);
    const meta = metaLines(catalog.projects);
    clear(list);
    if (!rows.length && loaded) list.append(h('div', { class: 'none', text: t('picker.empty') }));
    // aria-setsize / aria-posinset per level
    const sizes = new Map<string, number>();
    const level = (r: VisibleRow) => `${r.depth}:${r.parentId ?? ''}`;
    for (const r of rows) sizes.set(level(r), (sizes.get(level(r)) ?? 0) + 1);
    const pos = new Map<string, number>();
    rows.forEach((r) => {
      if (r.divider) list.append(h('div', { class: 'tree-divider', attrs: { 'aria-hidden': 'true' } }));
      const at = (pos.get(level(r)) ?? 0) + 1;
      pos.set(level(r), at);
      list.append(rowEl(r, meta, sizes.get(level(r))!, at));
      // an expanded folder with nothing in it says so
      if (r.node.kind === 'folder' && state.expanded.has(r.node.folder.folder_id) && !r.node.children.length) {
        const hint = h('div', { class: 'tree-empty', text: t('folders.emptyFolder'), attrs: { 'aria-hidden': 'true' } });
        hint.style.marginLeft = `${(r.depth + 1) * 16}px`;
        list.append(hint);
      }
    });
    if (!visibleOf(currentKey)) {
      const last = state.last && tree.folders.has(state.last) ? `f:${state.last}` : null;
      currentKey = last && visibleOf(last) ? last : rows[0] ? nodeKey(rows[0].node) : null;
    }
    setCurrent(currentKey, hadFocus);
  }

  const openPath = () => {
    const v = path.value.trim();
    if (!v) return;
    void app.openProject(v);
    onOpened?.();
  };
  go.addEventListener('click', openPath);
  path.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      openPath();
    }
  });
  newFolder.addEventListener('click', () => {
    const loc = locationOf(visibleOf(currentKey));
    addFolder(loc ? (tree.folders.get(loc)?.folder ?? null) : null);
  });

  const activate = (row: VisibleRow) => {
    if (row.node.kind === 'project') open(row.node.project);
    else toggle(row.node);
  };

  list.addEventListener('click', (e) => {
    const target = e.target as HTMLElement;
    const r = target.closest<HTMLElement>('[role=treeitem]');
    if (!r) return;
    if (target.closest('.more')) {
      e.stopPropagation();
      showMenu(r);
      return;
    }
    const row = visibleOf(r.dataset.key ?? null);
    if (!row) return;
    setCurrent(nodeKey(row.node));
    activate(row);
  });
  list.addEventListener('contextmenu', (e) => {
    const r = (e.target as HTMLElement).closest<HTMLElement>('[role=treeitem]');
    if (!r) return;
    e.preventDefault();
    showMenu(r);
  });
  list.addEventListener('focusin', (e) => {
    const r = (e.target as HTMLElement).closest<HTMLElement>('[role=treeitem]');
    if (r && r.dataset.key !== currentKey) setCurrent(r.dataset.key ?? null);
  });

  list.addEventListener('keydown', (e) => {
    const r = (e.target as HTMLElement).closest<HTMLElement>('[role=treeitem]');
    if (!r || e.target !== r || e.altKey || e.ctrlKey || e.metaKey) return;
    const i = rows.findIndex((x) => nodeKey(x.node) === r.dataset.key);
    const row = rows[i];
    if (!row) return;
    const n = row.node;
    const go = (j: number) => {
      const target = rows[Math.max(0, Math.min(rows.length - 1, j))];
      if (target) setCurrent(nodeKey(target.node), true);
    };
    let handled = true;
    switch (e.key) {
      case 'ArrowDown':
        go(i + 1);
        break;
      case 'ArrowUp':
        go(i - 1);
        break;
      case 'Home':
        go(0);
        break;
      case 'End':
        go(rows.length - 1);
        break;
      case 'ArrowRight':
        if (n.kind === 'folder') {
          if (!state.expanded.has(n.folder.folder_id)) toggle(n, true);
          else if (n.children.length) go(i + 1);
        }
        break;
      case 'ArrowLeft':
        if (n.kind === 'folder' && state.expanded.has(n.folder.folder_id)) toggle(n, false);
        else if (row.parentId) setCurrent(`f:${row.parentId}`, true);
        break;
      case 'Enter':
        activate(row);
        break;
      case 'F2':
        if (n.kind === 'folder' && !n.system) void renameFolder(app, n.folder).then(afterChange);
        break;
      case 'Delete':
        if (n.kind === 'folder' && !n.system && isEmptyFolder(n)) remove(n);
        break;
      case 'ContextMenu':
        showMenu(r);
        break;
      default:
        if (e.key === 'F10' && e.shiftKey) showMenu(r);
        else if (n.kind === 'project' && !e.shiftKey && (e.key === 'm' || e.key === 'M')) moveProject(n.project);
        else handled = false;
    }
    if (handled) {
      e.preventDefault();
      e.stopPropagation(); // Enter / Delete must not reach the global shortcuts (play, delete the selection)
    }
  });

  let seq = 0;
  async function refresh(): Promise<void> {
    const mine = ++seq;
    const c = await fetchCatalog(app);
    if (mine !== seq) return; // a newer refresh is on its way
    catalog = c;
    loaded = true;
    render();
  }

  // catalog events refresh the picker, throttled; a picker that left the page stops listening
  let timer: ReturnType<typeof setTimeout> | undefined;
  let wasConnected = false;
  const off = app.catalogChanged?.on(() => {
    if (el.isConnected) wasConnected = true;
    else if (wasConnected) return dispose();
    if (timer) return;
    timer = setTimeout(() => {
      timer = undefined;
      void refresh();
    }, PICKER_REFRESH_MS);
  });
  function dispose(): void {
    off?.();
    if (timer) clearTimeout(timer);
    timer = undefined;
    closeRowMenu();
  }

  render();
  void refresh();

  return { el, refresh, dispose, focus: () => (rowOf(currentKey) ?? list.querySelector<HTMLElement>('[role=treeitem]') ?? path).focus() };
}

export function openProjectDialog(app: App): void {
  const closeBtn = h('button', { class: 'btn', text: t('picker.close') });
  let close = () => {};
  const picker = projectPicker(app, () => close());
  const dialog = h('div', { class: 'dialog project-dialog' }, h('header', { text: t('picker.title') }), h('div', { class: 'body' }, h('div', { class: 'full' }, picker.el)), h('footer', {}, closeBtn));
  const modal = openModal(t('picker.title'), dialog, () => picker.dispose());
  close = modal.close;
  closeBtn.addEventListener('click', close);
  // files dropped on the picker go into a new project named after the folder or the first file
  importController(app).attachArea(dialog, { forceNew: true, overlay: false, onDropped: () => close() });
  void picker.refresh().then(() => picker.focus());
}
