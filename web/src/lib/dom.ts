// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// A tiny element builder; the UI is plain DOM + Canvas (no framework).

type Child = Node | string | null | undefined | false;

export interface Props {
  id?: string;
  class?: string;
  text?: string;
  title?: string;
  attrs?: Record<string, string>;
  style?: Partial<CSSStyleDeclaration> & Record<string, string>;
  on?: { [K in keyof HTMLElementEventMap]?: (ev: HTMLElementEventMap[K]) => void };
}

export function h<K extends keyof HTMLElementTagNameMap>(tag: K, props: Props = {}, ...children: Child[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  if (props.id) el.id = props.id;
  if (props.class) el.className = props.class;
  if (props.text !== undefined) el.textContent = props.text;
  if (props.title) el.title = props.title;
  if (props.attrs) for (const [k, v] of Object.entries(props.attrs)) el.setAttribute(k, v);
  if (props.style) for (const [k, v] of Object.entries(props.style)) (el.style as unknown as Record<string, string>)[k] = v as string;
  if (props.on) for (const [k, fn] of Object.entries(props.on)) el.addEventListener(k, fn as EventListener);
  for (const c of children) if (c) el.append(c);
  return el;
}

export function clear(el: Element): void {
  while (el.firstChild) el.removeChild(el.firstChild);
}

/** Tiny pub/sub. */
export class Emitter {
  private fns = new Set<() => void>();
  on(fn: () => void): () => void {
    this.fns.add(fn);
    return () => this.fns.delete(fn);
  }
  emit(): void {
    for (const f of [...this.fns]) f();
  }
}

/** Calls `fn` at most once per animation frame. */
export function rafThrottle(fn: () => void): () => void {
  let queued = false;
  return () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => {
      queued = false;
      fn();
    });
  };
}

/** A CSS custom property of the design tokens as a string ("#ff6a13"). Canvas code reads colours from here, never from literals. */
export function token(name: string, el: Element = document.documentElement): string {
  return getComputedStyle(el).getPropertyValue(`--rx-${name}`).trim();
}

/**
 * The new dB value for a key press on a volume slider, or null when the key is not a slider key: Arrow = 0.5 dB (Shift 0.1),
 * Page = 3 dB, Home / End = min / max, Delete / Backspace = 0 dB.
 */
export function sliderKeyDb(e: KeyboardEvent, db: number, min: number, max: number): number | null {
  if (e.ctrlKey || e.metaKey || e.altKey) return null;
  const step = e.shiftKey ? 0.1 : 0.5;
  let v: number;
  switch (e.key) {
    case 'ArrowUp':
    case 'ArrowRight':
      v = db + step;
      break;
    case 'ArrowDown':
    case 'ArrowLeft':
      v = db - step;
      break;
    case 'PageUp':
      v = db + 3;
      break;
    case 'PageDown':
      v = db - 3;
      break;
    case 'Home':
      v = min;
      break;
    case 'End':
      v = max;
      break;
    case 'Delete':
    case 'Backspace':
      v = 0;
      break;
    default:
      return null;
  }
  return Math.round(Math.min(max, Math.max(min, v)) * 10) / 10;
}
