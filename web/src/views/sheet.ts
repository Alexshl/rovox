// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Narrow-screen helpers: the two layout breakpoints (as media queries and as live JS state) and the bottom sheet that carries
// the menus, the FX panel of a track and other popovers on a phone.
import { t } from '../i18n';
import { h } from '../lib/dom';

/** Below 960 px: TCP 176 px, the MCP panel is a right drawer. */
export const NARROW_QUERY = '(max-width: 959.98px)';
/** 600 px and below: the phone layout (menu sheet, two-row transport, mixer sheet, 112 px TCP). */
export const PHONE_QUERY = '(max-width: 600px)';

const FOCUSABLE = 'button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

/** True while the media query matches; false where matchMedia does not exist (the desktop layout is the default). */
export function mediaMatches(query: string): boolean {
  return typeof window !== 'undefined' && typeof window.matchMedia === 'function' && !!window.matchMedia(query)?.matches;
}

/** Calls `fn(matches)` whenever the query starts or stops matching; returns the unsubscribe function. */
export function watchMedia(query: string, fn: (matches: boolean) => void): () => void {
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return () => undefined;
  const mql = window.matchMedia(query);
  if (!mql) return () => undefined;
  const listener = () => fn(mql.matches);
  mql.addEventListener?.('change', listener);
  return () => mql.removeEventListener?.('change', listener);
}

export const isNarrow = (): boolean => mediaMatches(NARROW_QUERY);
export const isPhone = (): boolean => mediaMatches(PHONE_QUERY);

export interface Sheet {
  scrim: HTMLElement;
  el: HTMLElement;
  close: () => void;
}

/**
 * A bottom sheet over a dimmed scrim: Escape, a tap on the scrim or the grip close it; Tab stays inside; focus moves into it and
 * returns to the element that had it. `content` is shown as it is (it keeps its own classes).
 */
export function openSheet(title: string, content: HTMLElement, onClose?: () => void): Sheet {
  const previous = document.activeElement as HTMLElement | null;
  const grip = h('button', { class: 'sheet-grip', attrs: { 'aria-label': t('narrow.sheetClose') } }, h('span'));
  const el = h('div', { class: 'sheet', attrs: { role: 'dialog', 'aria-modal': 'true', 'aria-label': title, tabindex: '-1' } }, grip, h('div', { class: 'sheet-body' }, content));
  const scrim = h('div', { class: 'sheet-scrim' }, el);
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    scrim.remove();
    previous?.focus?.();
    onClose?.();
  };
  grip.addEventListener('click', close);
  scrim.addEventListener('pointerdown', (e) => {
    if (e.target === scrim) close();
  });
  scrim.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      close();
    } else if (e.key === 'Tab') {
      const items = [...el.querySelectorAll<HTMLElement>(FOCUSABLE)].filter((x) => !x.hidden);
      if (!items.length) {
        e.preventDefault();
        return;
      }
      const first = items[0]!;
      const last = items[items.length - 1]!;
      const active = document.activeElement;
      if (e.shiftKey && (active === first || !el.contains(active))) {
        e.preventDefault();
        last.focus();
      } else if (!e.shiftKey && (active === last || !el.contains(active))) {
        e.preventDefault();
        first.focus();
      }
    }
  });
  document.body.append(scrim);
  const first = [...content.querySelectorAll<HTMLElement>(FOCUSABLE)].find((x) => !x.hidden);
  (first ?? el).focus();
  return { scrim, el, close };
}
