// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Project picker: the projects of the server (project.list) to open with a click or Enter, plus opening by path / id / name.
import { clear, h } from '../lib/dom';
import type { App } from '../store/session';
import { openModal } from './dialog';

export interface ServerProject {
  project_id: string;
  name: string;
  tracks?: number;
  path?: string;
  dirty?: boolean;
  duration_s?: number;
  modified?: number | string;
}

function formatDuration(s: number): string {
  const total = Math.round(s);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
}

function formatModified(v: number | string): string {
  const d = new Date(typeof v === 'number' && v < 1e11 ? v * 1000 : v);
  return Number.isNaN(d.getTime()) ? '' : d.toLocaleString('uk-UA');
}

/** The one-line description under a project name: tracks, duration and last modification when the server reports them. */
export function projectMeta(p: ServerProject): string {
  const parts: string[] = [];
  if (typeof p.tracks === 'number') parts.push(`${p.tracks} доріжок`);
  if (typeof p.duration_s === 'number') parts.push(formatDuration(p.duration_s));
  if (p.modified !== undefined) {
    const m = formatModified(p.modified);
    if (m) parts.push(`змінено ${m}`);
  }
  return parts.join(' · ');
}

export async function fetchServerProjects(app: App): Promise<ServerProject[]> {
  const r = await app.cmd<{ projects: ServerProject[] }>('project.list');
  return r.ok && r.data ? r.data.projects : [];
}

export interface ProjectPicker {
  el: HTMLElement;
  refresh: () => Promise<void>;
  focus: () => void;
}

/** `onOpened` runs after the user picked a project (e.g. to close the dialog around the picker). */
export function projectPicker(app: App, onOpened?: () => void): ProjectPicker {
  const list = h('div', { class: 'project-list', attrs: { role: 'listbox', 'aria-label': 'Проекти сервера' } });
  const path = h('input', { attrs: { type: 'text', placeholder: 'Шлях до .rvx, id або назва проекту', 'aria-label': 'Шлях до проекту' } });
  const go = h('button', { class: 'btn primary', text: 'Відкрити' });
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
    if (!projects.length) list.append(h('div', { class: 'none', text: 'На сервері немає відкритих проектів.' }));
    for (const p of projects) {
      const isOpen = app.projects.has(p.project_id);
      const meta = projectMeta(p);
      const row = h(
        'button',
        { class: `project-row${isOpen ? ' open' : ''}`, attrs: { role: 'option', 'aria-selected': String(isOpen), type: 'button' }, title: p.path ?? p.project_id },
        h('span', { class: 'pname', text: p.name || p.project_id }),
        isOpen ? h('span', { class: 'pbadge', text: 'відкрито' }) : null,
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
  const closeBtn = h('button', { class: 'btn', text: 'Закрити' });
  let close = () => {};
  const picker = projectPicker(app, () => close());
  const dialog = h('div', { class: 'dialog project-dialog' }, h('header', { text: 'Відкрити проект' }), h('div', { class: 'body' }, h('div', { class: 'full' }, picker.el)), h('footer', {}, closeBtn));
  const modal = openModal('Відкрити проект', dialog);
  close = modal.close;
  closeBtn.addEventListener('click', close);
  void picker.refresh().then(() => picker.focus());
}
