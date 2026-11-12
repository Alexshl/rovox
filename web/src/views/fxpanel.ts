// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The FX popover body: one disclosure per plugin; expanding it fetches fx.params and shows a searchable list of range sliders.
import { clear, h } from '../lib/dom';
import type { PluginState } from '../net/protocol';
import type { App } from '../store/session';

const FIRST = 12;
const THROTTLE_MS = 33;

interface FxParam {
  ident: string;
  name: string;
  value: number;
  min: number;
  max: number;
  default?: number;
  unit?: string;
  label?: string;
  text?: string;
  display?: string;
  /** Named states of a discrete parameter; value = min + index. */
  choices?: string[];
}

function valueText(p: FxParam): string {
  if (p.text) return p.text;
  if (p.display) return p.display;
  const unit = p.unit ?? p.label ?? '';
  return `${Math.round(p.value * 1000) / 1000}${unit ? ` ${unit}` : ''}`;
}

/** One plugin row (disclosure + on/off toggle) with its parameter list below. `onOpen` lets the popover collapse the others. */
export class FxPluginSection {
  readonly el: HTMLElement;
  private readonly twist: HTMLButtonElement;
  private readonly toggle: HTMLButtonElement;
  private readonly body = h('div', { class: 'fxbody' });
  private params: FxParam[] | null = null;
  private defaults = new Map<string, number>();
  private query = '';
  private showAll = false;
  private open = false;

  constructor(
    private readonly app: App,
    private readonly plugin: PluginState,
    private readonly onOpen: (self: FxPluginSection) => void,
  ) {
    this.twist = h('button', { class: 'fxname', attrs: { 'aria-expanded': 'false' }, title: plugin.name });
    this.toggle = h('button', { class: 'fxtoggle' });
    this.paintHead();
    this.body.hidden = true;
    this.el = h('div', { class: 'fxplugin' }, h('div', { class: 'row fxhead' }, this.twist, this.toggle), this.body);
    this.twist.addEventListener('click', () => (this.open ? this.collapse() : void this.expand()));
    this.toggle.addEventListener('click', () => {
      const wanted = !this.plugin.enabled;
      this.plugin.enabled = wanted;
      this.paintHead();
      void this.app.edit('fx.update', { plugin_id: this.plugin.plugin_id, enabled: wanted }).then((r) => {
        if (r.ok) return;
        this.plugin.enabled = !wanted; // the server refused: roll the UI back
        this.paintHead();
      });
    });
  }

  private paintHead(): void {
    this.twist.textContent = `${this.open ? '▾' : '▸'} ${this.plugin.name}`;
    this.twist.style.color = this.plugin.enabled ? 'var(--rx-ink)' : 'var(--rx-ink-muted)';
    this.toggle.textContent = this.plugin.enabled ? 'on' : 'off';
    this.toggle.classList.toggle('on', this.plugin.enabled);
    this.toggle.setAttribute('aria-pressed', String(this.plugin.enabled));
    this.toggle.setAttribute('aria-label', `${this.plugin.name}: ${this.plugin.enabled ? 'увімкнено' : 'вимкнено'}`);
  }

  collapse(): void {
    this.open = false;
    this.body.hidden = true;
    this.twist.setAttribute('aria-expanded', 'false');
    this.paintHead();
  }

  async expand(): Promise<void> {
    this.onOpen(this);
    this.open = true;
    this.body.hidden = false;
    this.twist.setAttribute('aria-expanded', 'true');
    this.paintHead();
    if (!this.params) {
      clear(this.body);
      this.body.append(h('div', { class: 'row fxnote', text: 'Завантаження…' }));
      const r = await this.app.cmd<{ fx?: { params?: FxParam[] } }>('fx.params', { plugin_id: this.plugin.plugin_id });
      if (!r.ok) {
        clear(this.body);
        this.body.append(h('div', { class: 'row fxnote', text: 'Не вдалося отримати параметри' }));
        return;
      }
      this.params = r.data?.fx?.params ?? [];
      for (const p of this.params) if (typeof p.default === 'number') this.defaults.set(p.ident, p.default); // no default from the plugin: no reset
    }
    this.render();
  }

  private render(): void {
    clear(this.body);
    const params = this.params ?? [];
    if (!params.length) {
      this.body.append(h('div', { class: 'row fxnote', text: 'Немає параметрів' }));
      return;
    }
    const search = h('input', { class: 'fxsearch', attrs: { type: 'search', placeholder: 'пошук параметра', 'aria-label': 'Пошук параметра' } });
    search.value = this.query;
    const list = h('div', { class: 'fxlist' });
    this.body.append(h('div', { class: 'row' }, search), list);
    const fill = () => {
      clear(list);
      const q = this.query.trim().toLowerCase();
      const hits = q ? params.filter((p) => p.name.toLowerCase().includes(q)) : params;
      const shown = this.showAll || q ? hits : hits.slice(0, FIRST);
      for (const p of shown) list.append(this.paramRow(p));
      if (shown.length < hits.length) {
        const more = h('button', { class: 'fxmore', text: `… ще ${hits.length - shown.length}` });
        more.addEventListener('click', () => {
          this.showAll = true;
          fill();
        });
        list.append(h('div', { class: 'row' }, more));
      }
      if (!hits.length) list.append(h('div', { class: 'row fxnote', text: 'Нічого не знайдено' }));
    };
    search.addEventListener('input', () => {
      this.query = search.value;
      fill();
    });
    fill();
  }

  /** A discrete parameter: an on/off switch for two states named off/on, a select otherwise. */
  private choiceRow(p: FxParam, choices: string[]): HTMLElement {
    const label = `${this.plugin.name} — ${p.name}`;
    const name = h('span', { class: 'fxpname', text: p.name, title: p.name });
    const index = () => Math.max(0, Math.min(choices.length - 1, Math.round(p.value - p.min)));
    const send = (i: number, done: (ok: boolean) => void) => {
      const before = p.value;
      p.value = p.min + i;
      void this.app.edit('fx.update', { plugin_id: this.plugin.plugin_id, params: { [p.ident]: p.value } }).then((r) => {
        if (!r.ok) p.value = before;
        done(r.ok);
      });
    };
    const isSwitch = choices.length === 2 && choices[0] === 'off' && choices[1] === 'on';
    if (isSwitch) {
      const sw = h('button', { class: 'fxtoggle fxswitch', attrs: { type: 'button', 'aria-label': label } });
      const paint = () => {
        const on = index() === 1;
        sw.textContent = on ? 'on' : 'off';
        sw.classList.toggle('on', on);
        sw.setAttribute('aria-pressed', String(on));
      };
      paint();
      sw.addEventListener('click', () => {
        send(index() === 1 ? 0 : 1, paint);
        paint();
      });
      return h('div', { class: 'row fxparam' }, name, h('span', { class: 'fxfill' }), sw);
    }
    const sel = h('select', { class: 'fxselect', attrs: { 'aria-label': label } });
    choices.forEach((c, i) => sel.append(h('option', { text: c, attrs: { value: String(i) } })));
    sel.value = String(index());
    sel.addEventListener('change', () => send(Number(sel.value), () => (sel.value = String(index()))));
    return h('div', { class: 'row fxparam' }, name, sel);
  }

  private paramRow(p: FxParam): HTMLElement {
    if (p.choices && p.choices.length >= 2) return this.choiceRow(p, p.choices);
    const label = `${this.plugin.name} — ${p.name}`;
    const hasDefault = this.defaults.has(p.ident);
    const range = h('input', { class: 'fxrange', attrs: { type: 'range', 'aria-label': label } });
    range.title = hasDefault ? `${p.name} — подвійний клік або Delete/Backspace: типове значення` : p.name;
    range.min = String(p.min);
    range.max = String(p.max);
    range.step = String((p.max - p.min) / 1000 || 0.001);
    range.value = String(p.value);
    const val = h('span', { class: 'fxval' });
    const paint = () => {
      const t = valueText(p);
      val.textContent = t;
      val.title = t;
      range.setAttribute('aria-valuetext', t);
    };
    paint();
    let last = 0;
    let lastSent: number | null = null;
    // input events and the final change share one coalesce key: one undo step per drag / per key press
    const coalesceKey = `fx:${this.plugin.plugin_id}:${p.ident}`;
    const send = (v: number) => {
      p.value = v;
      lastSent = v;
      const params = { [p.ident]: v };
      void this.app.edit('fx.update', { plugin_id: this.plugin.plugin_id, params }, { coalesceKey }).then((r) => {
        const back = (r.data as { fx?: { params?: FxParam[] } } | undefined)?.fx?.params?.find((q) => q.ident === p.ident);
        if (!back || v !== lastSent) return; // an answer to an older value must not overwrite the newer one
        p.text = back.text;
        p.display = back.display;
        p.value = back.value;
        paint();
      });
    };
    range.addEventListener('input', () => {
      p.value = Number(range.value);
      p.text = undefined;
      p.display = undefined;
      paint();
      const now = performance.now();
      if (now - last >= THROTTLE_MS) {
        last = now;
        send(Number(range.value));
      }
    });
    range.addEventListener('change', () => {
      const v = Number(range.value);
      if (v !== lastSent) send(v); // a key press already sent by `input` is not sent twice
    });
    const reset = () => {
      const d = this.defaults.get(p.ident);
      if (d === undefined) return;
      range.value = String(d);
      p.text = undefined;
      p.display = undefined;
      paint();
      send(d);
    };
    range.addEventListener('keydown', (e) => {
      if (e.key !== 'Delete' && e.key !== 'Backspace') return;
      e.preventDefault();
      reset();
    });
    const row = h('div', { class: 'row fxparam' }, h('span', { class: 'fxpname', text: p.name, title: p.name }), range, val);
    row.addEventListener('dblclick', reset);
    return row;
  }
}
