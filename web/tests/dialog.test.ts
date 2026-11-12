// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The in-app text dialog (replaces window.prompt) and the project picker.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setLocale } from '../src/i18n';
import { textDialog } from '../src/views/dialog';
import { projectMeta, projectPicker } from '../src/views/projects';
import type { App } from '../src/store/session';

const key = (el: Element, k: string, init: KeyboardEventInit = {}) => el.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true, ...init }));
const tick = () => new Promise((r) => setTimeout(r, 0));

beforeEach(() => {
  setLocale('uk'); // the tests assert the Ukrainian texts, whatever navigator.language says
});

afterEach(() => {
  document.body.innerHTML = '';
});

describe('textDialog', () => {
  it('is modal, focuses and selects the input, Enter returns the text', async () => {
    const p = textDialog({ title: 'Новий проект', label: 'Назва', value: 'Untitled' });
    const scrim = document.querySelector('.scrim')!;
    expect(scrim.getAttribute('aria-modal')).toBe('true');
    const input = scrim.querySelector('input')!;
    expect(document.activeElement).toBe(input);
    expect(input.selectionStart).toBe(0);
    expect(input.selectionEnd).toBe('Untitled'.length);
    input.value = 'Song';
    key(input, 'Enter');
    expect(await p).toBe('Song');
    expect(document.querySelector('.scrim')).toBeNull();
  });

  it('Esc and Скасувати resolve null', async () => {
    const p = textDialog({ title: 't', label: 'l' });
    key(document.querySelector('input')!, 'Escape');
    expect(await p).toBeNull();
    const q = textDialog({ title: 't', label: 'l' });
    (document.querySelectorAll('button')[0] as HTMLButtonElement).click();
    expect(await q).toBeNull();
  });

  it('OK button returns the value; empty string is allowed', async () => {
    const p = textDialog({ title: 't', label: 'l', value: '' });
    (document.querySelector('button.primary') as HTMLButtonElement).click();
    expect(await p).toBe('');
  });

  it('traps Tab inside the dialog', async () => {
    const p = textDialog({ title: 't', label: 'l' });
    const buttons = document.querySelectorAll('button');
    const ok = buttons[buttons.length - 1]!;
    (ok as HTMLElement).focus();
    key(ok, 'Tab');
    expect(document.activeElement).toBe(document.querySelector('input'));
    key(document.querySelector('input')!, 'Tab', { shiftKey: true });
    expect(document.activeElement).toBe(ok);
    key(ok, 'Escape');
    await p;
  });
});

describe('projectPicker', () => {
  const fakeApp = (open: string[]) => {
    const openProject = vi.fn();
    const setActive = vi.fn();
    const app = {
      projects: new Map(open.map((id) => [id, {}])),
      cmd: async () => ({ ok: true, data: { projects: [{ project_id: 'a', name: 'Alpha', tracks: 3, modified: 1_700_000_000 }, { project_id: 'b', name: 'Beta', tracks: 0 }] } }),
      openProject,
      setActive,
    } as unknown as App;
    return { app, openProject, setActive };
  };

  it('lists projects, marks open ones, opens on click', async () => {
    const { app, openProject, setActive } = fakeApp(['b']);
    const onOpened = vi.fn();
    const picker = projectPicker(app, onOpened);
    document.body.append(picker.el);
    await tick();
    const rows = picker.el.querySelectorAll<HTMLButtonElement>('.project-row');
    expect(rows).toHaveLength(2);
    expect(rows[0]!.textContent).toContain('Alpha');
    expect(rows[0]!.textContent?.replace(/\u00a0/g, ' ')).toContain('3 доріжки');
    expect(rows[1]!.classList.contains('open')).toBe(true);
    expect(rows[1]!.textContent).toContain('відкрито');
    rows[0]!.click();
    expect(openProject).toHaveBeenCalledWith('a');
    rows[1]!.click();
    expect(setActive).toHaveBeenCalledWith('b');
    expect(onOpened).toHaveBeenCalledTimes(2);
  });

  it('asks for the catalog, so projects that exist only on disk are listed and opened by id', async () => {
    const cmd = vi.fn(async (_name: string) => ({
      ok: true,
      data: { projects: [{ project_id: 'a', name: 'On disk', tracks: 2, modified: 1_700_000_000, open: false }, { project_id: 'b', name: 'Running', tracks: 1, open: true }] },
    }));
    const openProject = vi.fn();
    const app = { projects: new Map(), cmd, openProject, setActive: vi.fn() } as unknown as App;
    const picker = projectPicker(app);
    document.body.append(picker.el);
    await tick();
    expect(cmd).toHaveBeenCalledWith('project.catalog');
    const rows = picker.el.querySelectorAll<HTMLButtonElement>('.project-row');
    expect(rows).toHaveLength(2);
    expect(rows[0]!.classList.contains('open')).toBe(false);
    expect(rows[0]!.textContent).not.toContain('відкрито');
    expect(rows[1]!.textContent).toContain('відкрито');
    rows[0]!.click();
    expect(openProject).toHaveBeenCalledWith('a');
  });

  it('tells same-named projects apart by creation time and marks unsaved ones; unique names get no date', async () => {
    const projects = [
      { project_id: 'a', name: 'mykola', tracks: 10, duration_s: 222, created_at: '2026-11-02T16:33:00', unsaved: true },
      { project_id: 'b', name: 'mykola', tracks: 1, created_at: '2026-11-03T09:05:00' },
      { project_id: 'c', name: 'solo', tracks: 2, created_at: '2026-11-04T10:00:00' },
    ];
    const app = { projects: new Map(), cmd: async () => ({ ok: true, data: { projects } }), openProject: vi.fn(), setActive: vi.fn() } as unknown as App;
    const picker = projectPicker(app);
    document.body.append(picker.el);
    await tick();
    const meta = [...picker.el.querySelectorAll('.project-row .pmeta')].map((e) => (e.textContent ?? '').replace(/\u00a0/g, ' '));
    expect(meta[0]).toBe('10 доріжок · 3:42 · створено 02.11 16:33 · незбережено');
    expect(meta[1]).toBe('1 доріжка · створено 03.11 09:05');
    expect(meta[2]).toBe('2 доріжки');
  });

  it('opens by path with Enter and ArrowDown moves focus', async () => {
    const { app, openProject } = fakeApp([]);
    const picker = projectPicker(app);
    document.body.append(picker.el);
    await tick();
    const rows = picker.el.querySelectorAll<HTMLButtonElement>('.project-row');
    rows[0]!.focus();
    key(rows[0]!, 'ArrowDown');
    expect(document.activeElement).toBe(rows[1]);
    const input = picker.el.querySelector('.open-path input') as HTMLInputElement;
    input.value = '  /tmp/x.rvx ';
    key(input, 'Enter');
    expect(openProject).toHaveBeenCalledWith('/tmp/x.rvx');
  });

  it('projectMeta shows "змінено" as DD.MM HH:MM and drops a creation time that says the same', () => {
    const created = '2026-11-02T16:05:00';
    const modified = new Date(created).getTime() / 1000 + 20;   // same minute
    const later = new Date('2026-11-03T09:30:00').getTime() / 1000;
    const nb = (s: string) => s.replace(/\u00a0/g, ' ');
    expect(nb(projectMeta({ project_id: 'a', name: 'A', modified, created_at: created }, true))).toBe('змінено 02.11 16:05');
    expect(nb(projectMeta({ project_id: 'a', name: 'A', modified: later, created_at: created }, true))).toBe('змінено 03.11 09:30 · створено 02.11 16:05');
  });

  it('same-named rows that read alike at minute precision get seconds', async () => {
    const at = (s: string) => new Date(s).getTime() / 1000;
    const projects = [
      { project_id: 'a', name: 'mykola', tracks: 10, modified: at('2026-11-02T16:04:19'), created_at: '2026-11-02T16:04:19' },
      { project_id: 'b', name: 'mykola', tracks: 10, modified: at('2026-11-02T16:04:23'), created_at: '2026-11-02T16:04:23' },
      { project_id: 'c', name: 'mykola', tracks: 10, modified: at('2026-11-02T16:00:56'), created_at: '2026-11-02T16:00:56' },
    ];
    const app = { projects: new Map(), cmd: async () => ({ ok: true, data: { projects } }), openProject: vi.fn(), setActive: vi.fn() } as unknown as App;
    const picker = projectPicker(app);
    document.body.append(picker.el);
    await tick();
    const meta = [...picker.el.querySelectorAll('.project-row .pmeta')].map((e) => (e.textContent ?? '').replace(/\u00a0/g, ' '));
    expect(meta[0]).toBe('10 доріжок · змінено 02.11 16:04:19');
    expect(meta[1]).toBe('10 доріжок · змінено 02.11 16:04:23');
    expect(meta[2]).toBe('10 доріжок · змінено 02.11 16:00');
  });

  it('projectMeta skips fields the server does not send', () => {
    expect(projectMeta({ project_id: 'a', name: 'A', tracks: 2 }).replace(/\u00a0/g, ' ')).toBe('2 доріжки');
    expect(projectMeta({ project_id: 'a', name: 'A', tracks: 2, duration_s: 75 }).replace(/\u00a0/g, ' ')).toBe('2 доріжки · 1:15');
  });
});
