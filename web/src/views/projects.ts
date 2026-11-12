// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project picker: every project of the server, open or only on disk (project.catalog), to open with a click or Enter, plus opening by path / id / name.
import { clear, h } from '../lib/dom';
import { fmtDuration, fmtShortDateTime, plural, t } from '../i18n';
import type { App } from '../store/session';
import { openModal } from './dialog';

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
  const r = await app.cmd<{ projects: ServerProject[] }>('project.catalog');
  return r.ok && r.data ? r.data.projects : [];
}

export interface ProjectPicker {
  el: HTMLElement;
  refresh: () => Promise<void>;
  focus: () => void;
}

/** `onOpened` runs after the user picked a project (e.g. to close the dialog around the picker). */
export function projectPicker(app: App, onOpened?: () => void): ProjectPicker {
  const list = h('div', { class: 'project-list', attrs: { role: 'listbox', 'aria-label': t('picker.list') } });
  const path = h('input', { attrs: { type: 'text', placeholder: t('picker.pathPlaceholder'), 'aria-label': t('picker.pathLabel') } });
  const go = h('button', { class: 'btn primary', text: t('picker.open') });
  const el = h('div', { class: 'project-picker' }, list, h('div', { class: 'open-path' }, path, go));

  const open = (p: ServerProject) => {
    if (app.projects.has(p.project_id)) app.setActive(p.project_id);
    else void app.openProject(p.project_id);
    onOpened?.();
  };
  const openPath = () => {
    const t = path.value.trim();
    if (!t) return;
    void app.openProject(t);
    onOpened?.();
  };
  go.addEventListener('click', openPath);
  path.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      openPath();
    }
  });

  list.addEventListener('keydown', (e) => {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    const rows = [...list.querySelectorAll<HTMLElement>('button.project-row')];
    const i = rows.indexOf(document.activeElement as HTMLElement);
    const next = rows[e.key === 'ArrowDown' ? Math.min(rows.length - 1, i + 1) : Math.max(0, i - 1)];
    if (next) {
      e.preventDefault();
      next.focus();
    }
  });

  const refresh = async () => {
    const projects = await fetchServerProjects(app);
    clear(list);
    if (!projects.length) list.append(h('div', { class: 'none', text: t('picker.empty') }));
    const nameCount = new Map<string, number>();
    for (const p of projects) nameCount.set(p.name, (nameCount.get(p.name) ?? 0) + 1);
    // same-named rows that still read alike at minute precision (imports seconds apart) get seconds
    const shortMeta = new Map(projects.map((p) => [p.project_id, projectMeta(p, (nameCount.get(p.name) ?? 0) > 1)]));
    const rowCount = new Map<string, number>();
    for (const p of projects) {
      const k = `${p.name}\n${shortMeta.get(p.project_id)}`;
      rowCount.set(k, (rowCount.get(k) ?? 0) + 1);
    }
    for (const p of projects) {
      const isOpen = app.projects.has(p.project_id) || p.open === true;
      const dup = (nameCount.get(p.name) ?? 0) > 1;
      const alike = (rowCount.get(`${p.name}\n${shortMeta.get(p.project_id)}`) ?? 0) > 1;
      const meta = alike ? projectMeta(p, dup, true) : shortMeta.get(p.project_id)!;
      const row = h(
        'button',
        { class: `project-row${isOpen ? ' open' : ''}`, attrs: { role: 'option', 'aria-selected': String(isOpen), type: 'button' }, title: p.path ?? p.project_id },
        h('span', { class: 'pname', text: p.name || p.project_id }),
        isOpen ? h('span', { class: 'pbadge', text: t('picker.opened') }) : null,
        meta ? h('span', { class: 'pmeta', text: meta }) : null,
      );
      row.addEventListener('click', () => open(p));
      list.append(row);
    }
  };
  void refresh();

  return { el, refresh, focus: () => (list.querySelector<HTMLElement>('button.project-row') ?? path).focus() };
}

export function openProjectDialog(app: App): void {
  const closeBtn = h('button', { class: 'btn', text: t('picker.close') });
  let close = () => {};
  const picker = projectPicker(app, () => close());
  const dialog = h('div', { class: 'dialog project-dialog' }, h('header', { text: t('picker.title') }), h('div', { class: 'body' }, h('div', { class: 'full' }, picker.el)), h('footer', {}, closeBtn));
  const modal = openModal(t('picker.title'), dialog);
  close = modal.close;
  closeBtn.addEventListener('click', close);
  void picker.refresh().then(() => picker.focus());
}
