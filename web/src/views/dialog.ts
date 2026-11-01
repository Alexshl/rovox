// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// In-app modal dialogs instead of window.prompt(): a scrim, focus trap, Esc to cancel, aria-modal.
import { h } from '../lib/dom';

const FOCUSABLE = 'button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

export interface Modal {
  scrim: HTMLElement;
  close: () => void;
}

/** Shows `dialog` (an element with class "dialog") on a scrim. Esc and a click on the scrim close it; Tab stays inside. */
export function openModal(title: string, dialog: HTMLElement, onClose?: () => void): Modal {
  const previous = document.activeElement as HTMLElement | null;
  const scrim = h('div', { class: 'scrim', attrs: { role: 'dialog', 'aria-modal': 'true', 'aria-label': title } }, dialog);
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    scrim.remove();
    previous?.focus?.();
    onClose?.();
  };
  scrim.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      close();
    } else if (e.key === 'Tab') {
      const items = [...dialog.querySelectorAll<HTMLElement>(FOCUSABLE)].filter((el) => !el.hidden);
      if (!items.length) {
        e.preventDefault();
        return;
      }
      const first = items[0]!;
      const last = items[items.length - 1]!;
      const active = document.activeElement;
      if (e.shiftKey && (active === first || !dialog.contains(active))) {
        e.preventDefault();
        last.focus();
      } else if (!e.shiftKey && (active === last || !dialog.contains(active))) {
        e.preventDefault();
        first.focus();
      }
    }
  });
  scrim.addEventListener('pointerdown', (e) => {
    if (e.target === scrim) close();
  });
  document.body.append(scrim);
  return { scrim, close };
}

export interface TextDialogOptions {
  title: string;
  label: string;
  value?: string;
  placeholder?: string;
  okText?: string;
}

/** Asks for one line of text. Resolves with the text (Enter / OK) or null (Esc / Скасувати / click outside). */
export function textDialog(o: TextDialogOptions): Promise<string | null> {
  return new Promise((resolve) => {
    let result: string | null = null;
    const input = h('input', { attrs: { type: 'text', value: o.value ?? '', placeholder: o.placeholder ?? '', 'aria-label': o.label, autocomplete: 'off' } });
    const ok = h('button', { class: 'btn primary', text: o.okText ?? 'OK' });
    const cancel = h('button', { class: 'btn', text: 'Скасувати' });
    const dialog = h(
      'div',
      { class: 'dialog text-dialog' },
      h('header', { text: o.title }),
      h('div', { class: 'body' }, h('label', { class: 'label full', text: o.label }), h('div', { class: 'full' }, input)),
      h('footer', {}, cancel, ok),
    );
    const modal = openModal(o.title, dialog, () => resolve(result));
    const submit = () => {
      result = input.value;
      modal.close();
    };
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        submit();
      }
    });
    ok.addEventListener('click', submit);
    cancel.addEventListener('click', () => modal.close());
    input.focus();
    input.select();
  });
}
