// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Mixer: one strip per track and the master. Fader (dB law, double click = 0 dB, Shift-drag = fine), pan, M/S, FX list and peak
// meters (green to -12 dBFS, amber to -3, red above; fed by the 0x02 frames at 30 Hz). Changes go through track.update with a
// coalesce key, so a drag is one undo step.
import { clear, h, token } from '../lib/dom';
import { alpha } from '../lib/color';
import { SILENCE_DB, clamp, dbToFader, faderToDb, formatDb, formatPan, meterPosition } from '../lib/units';
import type { PluginState, TrackState } from '../net/protocol';
import type { MasterState } from '../net/protocol';
import type { App } from '../store/session';
import type { ProjectStore } from '../store/project';

const SCALE_DB = [12, 6, 0, -6, -12, -20, -30, -40, -60];

class Strip {
  readonly el: HTMLElement;
  private readonly nameEl = h('span');
  private readonly colorEl = h('i');
  private readonly fxList = h('div', { class: 'fxlist' });
  private readonly pan = h('input', { class: 'pan', attrs: { type: 'range', min: '-100', max: '100', step: '1', 'aria-label': 'Pan' } });
  private readonly panRead = h('div', { class: 'panread' });
  private readonly dbRead = h('div', { class: 'dbread' });
  private readonly canvas = h('canvas');
  private readonly mute = h('button', { class: 'sbtn mute', text: 'M', attrs: { 'aria-label': 'Mute' } });
  private readonly solo = h('button', { class: 'sbtn solo', text: 'S', attrs: { 'aria-label': 'Solo' } });
  private dragging = false;
  private lastSent = 0;

  constructor(
    private readonly app: App,
    private readonly store: ProjectStore,
    readonly id: string,
  ) {
    const isMaster = id === 'master';
    this.el = h(
      'div',
      { class: `strip${isMaster ? ' master' : ''}` },
      h('div', { class: 'stripname' }, this.colorEl, this.nameEl),
      this.fxList,
      this.pan,
      this.panRead,
      this.canvas,
      this.dbRead,
      h('div', { class: 'ms' }, ...(isMaster ? [] : [this.mute, this.solo])),
    );
    this.pan.addEventListener('input', () => {
      const v = Number(this.pan.value) / 100;
      this.store.patchTrack(this.id, { pan: v });
      this.send({ pan: v }, `pan:${this.id}`);
    });
    this.pan.addEventListener('dblclick', () => {
      this.store.patchTrack(this.id, { pan: 0 });
      this.send({ pan: 0 });
    });
    this.mute.addEventListener('click', () => this.toggle('mute'));
    this.solo.addEventListener('click', () => this.toggle('solo'));
    this.canvas.addEventListener('pointerdown', (e) => this.faderDown(e));
    this.canvas.addEventListener('dblclick', () => {
      this.store.patchTrack(this.id, { volume_db: 0 });
      this.send({ volume_db: 0 });
    });
  }

  private send(patch: Record<string, unknown>, coalesceKey?: string): void {
    void this.app.edit('track.update', { track_id: this.id, ...patch }, coalesceKey ? { coalesceKey } : {});
  }

  private toggle(key: 'mute' | 'solo'): void {
    const t = this.store.track(this.id);
    if (!t) return;
    this.store.patchTrack(this.id, { [key]: !t[key] });
    this.send({ [key]: !t[key] });
  }

  private faderDown(e: PointerEvent): void {
    e.preventDefault();
    this.canvas.setPointerCapture(e.pointerId);
    this.dragging = true;
    const rect = () => this.canvas.getBoundingClientRect();
    const trackTop = 8;
    const trackBottom = () => rect().height - 8;
    const startDb = this.current().volume_db ?? 0;
    const startY = e.clientY;
    const fine = e.shiftKey || e.ctrlKey;
    const posOf = (y: number) => clamp(1 - (y - rect().top - trackTop) / (trackBottom() - trackTop), 0, 1);
    const apply = (ev: PointerEvent) => {
      let db: number;
      if (fine) db = clamp(startDb - (ev.clientY - startY) * 0.1, SILENCE_DB, 12);
      else db = faderToDb(posOf(ev.clientY));
      db = Math.round(db * 10) / 10;
      this.store.patchTrack(this.id, { volume_db: db });
      const now = performance.now();
      if (now - this.lastSent >= 33) {
        this.lastSent = now;
        this.send({ volume_db: db }, `vol:${this.id}`);
      }
      return db;
    };
    let db = fine ? startDb : apply(e);
    const move = (ev: PointerEvent) => {
      db = apply(ev);
    };
    const up = () => {
      this.dragging = false;
      this.canvas.removeEventListener('pointermove', move);
      this.canvas.removeEventListener('pointerup', up);
      this.send({ volume_db: db }, `vol:${this.id}`);
    };
    this.canvas.addEventListener('pointermove', move);
    this.canvas.addEventListener('pointerup', up);
  }

  private current(): { name: string; color?: string; volume_db?: number; pan?: number; mute?: boolean; solo?: boolean } {
    if (this.id === 'master') {
      const m: MasterState | undefined = this.store.master;
      return { name: 'MASTER', volume_db: m?.volume_db, pan: m?.pan };
    }
    return this.store.track(this.id) ?? { name: '' };
  }

  update(t: TrackState | undefined): void {
    const c = this.current();
    const name = this.id === 'master' ? 'MASTER' : (t?.name ?? c.name);
    this.nameEl.textContent = name;
    this.nameEl.title = name;
    if (this.id === 'master') this.nameEl.style.fontFamily = 'var(--rx-font-display)';
    this.colorEl.style.background = this.id === 'master' ? 'var(--rx-orange)' : (t?.color ?? 'var(--rx-ink-muted)');
    this.el.classList.toggle('agent', this.store.trackMark(this.id) !== undefined);
    this.mute.classList.toggle('on', !!t?.mute);
    this.solo.classList.toggle('on', !!t?.solo);
    if (document.activeElement !== this.pan) this.pan.value = String(Math.round((c.pan ?? 0) * 100));
    this.panRead.textContent = formatPan(c.pan);
    this.dbRead.textContent = formatDb(c.volume_db ?? 0, true);

    const list: PluginState[] = (this.store.fx.get(this.id) ?? []).filter((p) => p.type !== 'volume' && p.type !== 'level');
    clear(this.fxList);
    if (!list.length) this.fxList.append(h('div', { class: 'none', text: this.id === 'master' ? 'no master FX' : 'no FX' }));
    for (const p of list.slice(0, 4)) this.fxList.append(h('div', { class: p.enabled ? 'on' : 'off', text: p.name, title: `${p.name} (${p.type})` }));
    if (list.length > 4) this.fxList.append(h('div', { class: 'none', text: `+${list.length - 4} ще` }));
  }

  paint(now: number): void {
    const dpr = window.devicePixelRatio || 1;
    const w = this.canvas.clientWidth;
    const hh = this.canvas.clientHeight;
    if (w < 4 || hh < 4) return;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(hh * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(hh * dpr);
    }
    const ctx = this.canvas.getContext('2d')!;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, hh);

    const top = 8;
    const bottom = hh - 8;
    const trackH = bottom - top;
    const green = token('signal-green');
    const amber = token('amber');
    const red = token('red');
    const hairline = token('hairline');
    const muted = token('ink-muted');
    const ink = token('ink');
    const panel700 = token('panel-700');
    const cyan = token('agent-cyan');

    // dB scale (left), fader slot (middle), meters (right)
    ctx.font = '9px "IBM Plex Mono", ui-monospace, monospace';
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'right';
    for (const db of SCALE_DB) {
      const y = bottom - dbToFader(db) * trackH;
      ctx.fillStyle = muted;
      ctx.fillText(String(db), 16, y);
      ctx.fillStyle = hairline;
      ctx.fillRect(18, Math.round(y), 4, 1);
    }
    ctx.textAlign = 'left';

    const slotX = 28;
    ctx.fillStyle = panel700;
    ctx.fillRect(slotX + 2, top, 3, trackH);

    const vol = this.current().volume_db ?? 0;
    const fy = bottom - dbToFader(vol) * trackH;
    const marked = this.store.trackMark(this.id) !== undefined;
    ctx.fillStyle = marked ? cyan : ink;
    ctx.fillRect(slotX - 4, fy - 5, 15, 10); // fader cap, hardware-square
    ctx.fillStyle = token('panel-900');
    ctx.fillRect(slotX - 4, fy - 0.5, 15, 1);

    // meters: two bars
    const mx = slotX + 18;
    const barW = Math.max(6, Math.floor((w - mx - 2) / 2) - 2);
    const m = this.store.meters.get(this.id);
    const decay = m ? Math.min(1, (now - m.holdAt) / 1500) : 1;
    void decay;
    for (let c = 0; c < 2; c++) {
      const x = mx + c * (barW + 3);
      ctx.fillStyle = panel700;
      ctx.fillRect(x, top, barW, trackH);
      const db = m ? (c === 0 ? m.l : m.r) : SILENCE_DB;
      const hold = m ? (c === 0 ? m.holdL : m.holdR) : SILENCE_DB;
      const level = meterPosition(db) * trackH;
      // segments by zone
      const zones: [number, string][] = [
        [meterPosition(-12) * trackH, green],
        [meterPosition(-3) * trackH, amber],
        [trackH, red],
      ];
      let from = 0;
      for (const [limit, color] of zones) {
        const seg = Math.min(level, limit) - from;
        if (seg > 0) {
          ctx.fillStyle = color;
          ctx.fillRect(x, bottom - from - seg, barW, seg);
        }
        from = limit;
      }
      if (hold > -59) {
        const hy = bottom - meterPosition(hold) * trackH;
        ctx.fillStyle = hold > -3 ? red : hold > -12 ? amber : ink;
        ctx.fillRect(x, Math.round(hy), barW, 2);
      }
    }
    ctx.fillStyle = alpha(muted, 0.0);
    void this.dragging;
  }
}

export class MixerView {
  readonly el: HTMLElement;
  private readonly strips = h('div', { class: 'strips' });
  private readonly title = h('span', { class: 'label', text: 'MIXER' });
  private readonly rows = new Map<string, Strip>();
  private signature = '';
  private store: ProjectStore | undefined;
  private raf = 0;

  constructor(private readonly app: App) {
    this.el = h(
      'section',
      { class: 'mixer', attrs: { 'aria-label': 'Мікшер' } },
      h('div', { class: 'head' }, this.title, h('span', { class: 'readout muted', text: 'peak, dBFS' })),
      this.strips,
    );
    this.loop();
  }

  sync(): void {
    const s = this.app.active;
    this.store = s;
    this.el.classList.toggle('closed', !this.app.prefs.mixerOpen);
    if (!s || !s.loaded) {
      clear(this.strips);
      this.rows.clear();
      this.signature = '';
      return;
    }
    const sig = ['master', ...s.tracks.map((t) => t.track_id)].join(',') + '|' + s.id;
    if (sig !== this.signature) {
      this.signature = sig;
      clear(this.strips);
      this.rows.clear();
      for (const t of s.tracks) {
        const strip = new Strip(this.app, s, t.track_id);
        this.rows.set(t.track_id, strip);
        this.strips.append(strip.el);
      }
      const master = new Strip(this.app, s, 'master');
      this.rows.set('master', master);
      this.strips.append(master.el);
    }
    for (const [id, strip] of this.rows) strip.update(id === 'master' ? undefined : s.track(id));
    this.title.textContent = `MIXER · ${s.tracks.length}`;
  }

  private loop = () => {
    if (this.app.prefs.mixerOpen && this.store?.loaded) {
      const now = performance.now();
      for (const strip of this.rows.values()) strip.paint(now);
    }
    this.raf = requestAnimationFrame(this.loop);
  };

  dispose(): void {
    cancelAnimationFrame(this.raf);
  }
}
