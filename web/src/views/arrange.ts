// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Arrange view: ruler + lanes drawn on Canvas (virtualised: only the visible tracks and the items in the visible time window),
// track control panels (TCP) as DOM. Edits become commands of the command layer; items touched by an agent are drawn in agent-cyan.
import { alpha, mix } from '../lib/color';
import { clear, h, rafThrottle, token } from '../lib/dom';
import { type Grid, snapTime } from '../lib/snap';
import { barQn, barToSeconds, beatQn, formatClock, qnToSeconds, secondsToBar, secondsToQn } from '../lib/time';
import { clamp, dbToFader, dbToLinear, faderToDb, formatDb } from '../lib/units';
import type { ItemState, MarkerState, TrackState } from '../net/protocol';
import type { App } from '../store/session';
import type { AgentMark, ProjectStore } from '../store/project';
import { textDialog } from './dialog';
import { PeakCache } from './peaks';
import { projectPicker } from './projects';

export const TRACK_H = 64;
export const TCP_W = 248;
const ITEM_TOP = 3;
const ITEM_H = 58;
const STRIP_H = 14;
const EDGE = 5;
const GAIN_MIN = -24;
const GAIN_MAX = 12;

type Zone = 'body' | 'trimL' | 'trimR' | 'fadeIn' | 'fadeOut' | 'gain';

interface Hit {
  item: ItemState;
  zone: Zone;
}

interface Palette {
  panel900: string;
  panel800: string;
  panel700: string;
  hairline: string;
  ink: string;
  inkMuted: string;
  orange: string;
  amber: string;
  cream: string;
  cyan: string;
  wave: string;
  laneAlt: string;
  shade: string;
}

interface Geometry {
  start: number;
  length: number;
  fadeIn: number;
  fadeOut: number;
  gain: number;
  offset: number;
}

type Drag =
  | { kind: 'item'; zone: Zone; ids: string[]; startX: number; startY: number; orig: Map<string, Geometry>; moved: boolean; anchor: string; delta: Geometry | null }
  | { kind: 'ruler-seek' }
  | { kind: 'ruler-loop'; from: number }
  | { kind: 'marker'; id: string; startX: number; orig: number; moved: boolean }
  | { kind: 'pan'; lastX: number };

export class ArrangeView {
  readonly el: HTMLElement;
  private readonly rulerCanvas = h('canvas');
  private readonly lanesCanvas = h('canvas');
  private readonly scroller = h('div', { class: 'lanes-scroll' });
  private readonly inner = h('div', { class: 'lanes-inner' });
  private readonly lanes = h('div', { class: 'lanes' });
  private readonly rulerBox = h('div', { class: 'ruler' });
  private readonly headLabel = h('span', { class: 'label', text: 'TRACKS' });
  private readonly empty = h('div', { class: 'empty-state' });
  private pickerOnline = false;
  private pickerShown = false;
  private readonly tip = h('div', { class: 'tip', style: { display: 'none' } });
  private readonly tcpRows = new Map<string, TcpRow>();
  private readonly peaks: PeakCache;
  private palette!: Palette;
  private drag: Drag | null = null;
  private hover: Hit | null = null;
  private tipTimer: ReturnType<typeof setTimeout> | undefined;
  private lastMouse = { x: 0, y: 0 };
  private store: ProjectStore | undefined;
  private storeSub: (() => void)[] = [];
  private trackSignature = '';
  private fitted = new Set<string>();
  private readonly draw = rafThrottle(() => this.paint());
  private frameCount = 0;
  private frameStart = performance.now();
  private lastStoppedPlayhead = Number.NaN;
  fps = 0;

  constructor(private readonly app: App) {
    this.peaks = new PeakCache(() => this.draw());
    this.rulerBox.append(this.rulerCanvas);
    this.lanes.append(this.lanesCanvas);
    const lanesWrap = h('div', { class: 'lanes-wrap' }, this.scroller, this.lanes, this.empty);
    this.scroller.append(this.inner);
    this.el = h(
      'section',
      { class: 'arrange', attrs: { 'aria-label': 'Arrange' } },
      h('div', { class: 'ruler-row' }, h('div', { class: 'head' }, this.headLabel), this.rulerBox),
      lanesWrap,
      this.tip,
    );

    this.scroller.addEventListener('scroll', () => {
      if (this.store) this.store.view.scrollY = this.scroller.scrollTop;
      this.draw();
    });
    new ResizeObserver(() => this.draw()).observe(this.el);
    this.lanesCanvas.addEventListener('pointerdown', (e) => this.onLanesDown(e));
    this.lanesCanvas.addEventListener('pointermove', (e) => this.onLanesMove(e));
    this.lanesCanvas.addEventListener('pointerup', (e) => this.onUp(e));
    this.lanesCanvas.addEventListener('pointerleave', () => this.hideTip());
    this.lanesCanvas.addEventListener('dblclick', (e) => this.onLanesDouble(e));
    this.lanesCanvas.addEventListener('wheel', (e) => this.onWheel(e), { passive: false });
    this.rulerCanvas.addEventListener('pointerdown', (e) => this.onRulerDown(e));
    this.rulerCanvas.addEventListener('pointermove', (e) => this.onRulerMove(e));
    this.rulerCanvas.addEventListener('pointerup', (e) => this.onUp(e));
    this.rulerCanvas.addEventListener('dblclick', (e) => this.onRulerDouble(e));
    this.rulerCanvas.addEventListener('wheel', (e) => this.onWheel(e), { passive: false });
    this.refreshPalette();
    void document.fonts?.ready.then(() => this.draw());
    requestAnimationFrame(() => this.tickAnimations());
  }

  //==========================================================================
  /** Called when the app state changed (tabs, theme, store updates). */
  sync(): void {
    const s = this.app.active;

    if (s !== this.store) {
      for (const u of this.storeSub) u();
      this.storeSub = [];
      this.store = s;
      this.trackSignature = '';
      this.tcpRows.clear();
      clear(this.inner);
      if (s) {
        this.storeSub.push(s.state.on(() => this.onStoreChanged()), s.agentChanged.on(() => this.draw()), s.transportChanged.on(() => this.draw()));
        this.scroller.scrollTop = s.view.scrollY;
      }
    }

    this.refreshPalette();
    this.onStoreChanged();
  }

  private onStoreChanged(): void {
    const s = this.store;
    this.empty.style.display = 'none';
    if (s) this.pickerShown = false;

    if (!s || !s.loaded) {
      if (s) this.showEmpty('Завантаження проекту...');
      else this.showProjectPicker();
      this.draw();
      return;
    }

    this.headLabel.textContent = `TRACKS · ${s.tracks.length}`;

    if (s.tracks.length === 0) this.showEmpty('Порожній проект: імпортуйте аудіо через rovox import або попросіть агента додати доріжки.');

    const signature = s.tracks.map((t) => t.track_id).join(',');
    this.inner.style.height = `${s.tracks.length * TRACK_H}px`;
    if (signature !== this.trackSignature) {
      this.trackSignature = signature;
      this.rebuildTcp(s);
    }
    this.updateTcp(s);

    if (!this.fitted.has(s.id) && this.lanesCanvas.clientWidth > 0) {
      this.fitted.add(s.id);
      this.fit();
    }
    this.draw();
  }

  private showProjectPicker(): void {
    // a picker built before the socket opened has an empty list: rebuild it once connected
    if (this.pickerShown && !this.pickerOnline && this.app.status === 'open') this.pickerShown = false;
    if (this.pickerShown) {
      this.empty.style.display = 'flex';
      return;
    }
    this.pickerShown = true;
    this.pickerOnline = this.app.status === 'open';
    clear(this.empty);
    const picker = projectPicker(this.app);
    this.empty.append(h('div', { class: 'card' }, h('div', { text: 'Немає відкритих проектів. Створіть проект кнопкою + або відкрийте існуючий:' }), picker.el));
    this.empty.style.display = 'flex';
  }

  private showEmpty(text: string): void {
    this.pickerShown = false;
    clear(this.empty);
    this.empty.append(h('div', { class: 'card', text }));
    this.empty.style.display = 'flex';
  }

  private refreshPalette(): void {
    const t = (n: string) => token(n);
    this.palette = {
      panel900: t('panel-900'),
      panel800: t('panel-800'),
      panel700: t('panel-700'),
      hairline: t('hairline'),
      ink: t('ink'),
      inkMuted: t('ink-muted'),
      orange: t('orange'),
      amber: t('amber'),
      cream: t('cream'),
      cyan: t('agent-cyan'),
      wave: t('wave'),
      laneAlt: mix(t('panel-800'), t('panel-900'), 0.4),
      shade: alpha(t('panel-900'), 0.55),
    };
  }

  //==========================================================================
  // view window
  private get pps(): number {
    return this.store?.view.pps ?? 40;
  }

  private get t0(): number {
    return this.store?.view.start ?? 0;
  }

  private xOf(t: number): number {
    return (t - this.t0) * this.pps;
  }

  private tOf(x: number): number {
    return x / this.pps + this.t0;
  }

  fit(): void {
    const s = this.store;
    if (!s) return;
    const w = Math.max(200, this.lanesCanvas.clientWidth);
    const end = Math.max(4, ...s.tracks.flatMap((t) => (s.index(t.track_id) ? [lastEnd(s, t.track_id)] : [])), s.lengthS);
    s.view.start = 0;
    s.view.pps = clamp((w - 24) / end, 2, 4000);
    this.draw();
  }

  zoomBy(factor: number, anchorX?: number): void {
    const s = this.store;
    if (!s) return;
    const w = this.lanesCanvas.clientWidth;
    const ax = anchorX ?? w / 2;
    const tAnchor = this.tOf(ax);
    s.view.pps = clamp(s.view.pps * factor, 2, 4000);
    s.view.start = Math.max(0, tAnchor - ax / s.view.pps);
    this.draw();
  }

  scrollBy(dx: number): void {
    const s = this.store;
    if (!s) return;
    s.view.start = Math.max(0, s.view.start + dx / s.view.pps);
    this.draw();
  }

  //==========================================================================
  // TCP
  private rebuildTcp(s: ProjectStore): void {
    clear(this.inner);
    this.tcpRows.clear();
    s.tracks.forEach((t, i) => {
      const row = new TcpRow(this.app, s, t.track_id, i);
      this.tcpRows.set(t.track_id, row);
      this.inner.append(row.el);
    });
  }

  private updateTcp(s: ProjectStore): void {
    s.tracks.forEach((t, i) => this.tcpRows.get(t.track_id)?.update(t, i));
  }

  //==========================================================================
  // painting
  private fitCanvas(c: HTMLCanvasElement): CanvasRenderingContext2D | null {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(c.clientWidth * dpr));
    const hh = Math.max(1, Math.round(c.clientHeight * dpr));
    if (c.width !== w || c.height !== hh) {
      c.width = w;
      c.height = hh;
    }
    const ctx = c.getContext('2d');
    ctx?.setTransform(dpr, 0, 0, dpr, 0, 0);
    return ctx;
  }

  private paint(): void {
    const s = this.store;
    const ctx = this.fitCanvas(this.lanesCanvas);
    const rctx = this.fitCanvas(this.rulerCanvas);
    if (!ctx || !rctx) return;
    const W = this.lanesCanvas.clientWidth;
    const H = this.lanesCanvas.clientHeight;
    const pal = this.palette;
    ctx.clearRect(0, 0, W, H);
    rctx.clearRect(0, 0, this.rulerCanvas.clientWidth, this.rulerCanvas.clientHeight);
    this.frameCount++;

    if (!s || !s.loaded) return;

    const scrollY = this.scroller.scrollTop;
    const tStart = this.t0;
    const tEnd = this.tOf(W);
    const first = Math.max(0, Math.floor(scrollY / TRACK_H));
    const last = Math.min(s.tracks.length - 1, Math.floor((scrollY + H) / TRACK_H));

    // lanes
    for (let i = first; i <= last; i++) {
      const y = i * TRACK_H - scrollY;
      ctx.fillStyle = i % 2 ? pal.laneAlt : pal.panel900;
      ctx.fillRect(0, y, W, TRACK_H);
      ctx.fillStyle = pal.hairline;
      ctx.fillRect(0, y + TRACK_H - 1, W, 1);
    }
    const rest = (last + 1) * TRACK_H - scrollY;
    if (rest < H) {
      ctx.fillStyle = pal.panel900;
      ctx.fillRect(0, rest, W, H - rest);
    }

    this.paintGrid(ctx, W, H, tStart, tEnd, s);

    // loop
    const loop = s.transport.loop;
    if (loop.enabled && loop.end_s > loop.start_s) {
      const x0 = this.xOf(loop.start_s);
      const x1 = this.xOf(loop.end_s);
      ctx.fillStyle = alpha(pal.orange, 0.08);
      ctx.fillRect(x0, 0, x1 - x0, H);
    }

    // items
    for (let i = first; i <= last; i++) {
      const track = s.tracks[i]!;
      const y = i * TRACK_H - scrollY;
      const idx = s.index(track.track_id);
      if (!idx) continue;
      for (const item of idx.query(tStart - 1, tEnd + 1)) this.paintItem(ctx, s, track, item, y);
    }

    // markers (lines through the lanes)
    for (const m of s.markers) {
      const x = this.xOf(m.time_s);
      if (x < -1 || x > W + 1) continue;
      ctx.fillStyle = alpha(m.marker_id && s.marks.has(m.marker_id) ? pal.cyan : pal.cream, 0.35);
      ctx.fillRect(Math.round(x), 0, 1, H);
    }

    this.paintAgentCursor(ctx, s, scrollY, W, H);

    // playhead
    const ph = this.app.playhead(s);
    const px = Math.round(this.xOf(ph));
    if (px >= 0 && px <= W) {
      ctx.fillStyle = pal.orange;
      ctx.fillRect(px, 0, 1, H);
    }

    // drag guides
    if (this.drag?.kind === 'item' && this.drag.moved && this.drag.delta) {
      const d = this.drag.delta;
      const a = this.drag.orig.get(this.drag.anchor);
      if (a) {
        const x = Math.round(this.xOf(d.start));
        ctx.fillStyle = alpha(pal.orange, 0.7);
        ctx.fillRect(x, 0, 1, H);
      }
    }

    this.paintRuler(rctx, s, ph);
  }

  private paintGrid(ctx: CanvasRenderingContext2D, W: number, H: number, tStart: number, tEnd: number, s: ProjectStore): void {
    const pal = this.palette;
    const bq = barQn(s.tempo);
    const bt = beatQn(s.tempo);
    const q0 = Math.max(0, secondsToQn(s.tempo, tStart));
    const q1 = secondsToQn(s.tempo, tEnd);
    const barPx = bq * (this.pps * (60 / (s.tempo.points[0]?.bpm ?? 120)));
    const barStep = barPx < 24 ? Math.ceil(24 / barPx) : 1;
    const bar0 = Math.floor(q0 / bq);

    ctx.lineWidth = 1;
    for (let b = bar0; b * bq <= q1 + bq; b += 1) {
      if (b % barStep !== 0) continue;
      const x = Math.round(this.xOf(qnToSeconds(s.tempo, b * bq)));
      ctx.fillStyle = alpha(pal.hairline, 0.9);
      ctx.fillRect(x, 0, 1, H);
    }
    if (barStep === 1 && barPx > 140) {
      for (let q = Math.floor(q0 / bt) * bt; q <= q1; q += bt) {
        if (Math.abs(q / bq - Math.round(q / bq)) < 1e-6) continue;
        ctx.fillStyle = alpha(pal.hairline, 0.35);
        ctx.fillRect(Math.round(this.xOf(qnToSeconds(s.tempo, q))), 0, 1, H);
      }
    }
    void W;
  }

  private itemGeometry(item: ItemState, drag: Drag | null = this.drag): Geometry {
    const d = drag;
    if (d?.kind === 'item' && d.moved && d.delta && d.orig.has(item.item_id)) {
      const o = d.orig.get(item.item_id)!;
      const delta = d.delta;
      const a = d.orig.get(d.anchor)!;
      switch (d.zone) {
        case 'body':
          return { ...o, start: Math.max(0, o.start + (delta.start - a.start)) };
        case 'trimL': {
          const ns = clamp(delta.start, 0, o.start + o.length - 0.01);
          return { ...o, start: ns, length: o.start + o.length - ns, offset: o.offset + (ns - o.start) };
        }
        case 'trimR':
          return { ...o, length: Math.max(0.01, delta.start - o.start) };
        case 'fadeIn':
          return { ...o, fadeIn: clamp(delta.fadeIn, 0, o.length) };
        case 'fadeOut':
          return { ...o, fadeOut: clamp(delta.fadeOut, 0, o.length) };
        case 'gain':
          return { ...o, gain: clamp(delta.gain, GAIN_MIN, GAIN_MAX) };
      }
    }
    return { start: item.start_s, length: item.length_s, fadeIn: item.fade_in_s ?? 0, fadeOut: item.fade_out_s ?? 0, gain: item.gain_db ?? 0, offset: item.offset_s };
  }

  private paintItem(ctx: CanvasRenderingContext2D, s: ProjectStore, track: TrackState, item: ItemState, rowY: number): void {
    const pal = this.palette;
    const g = this.itemGeometry(item);
    const x = this.xOf(g.start);
    const w = g.length * this.pps;
    const W = this.lanesCanvas.clientWidth;
    if (x > W || x + w < 0 || w < 0.5) return;
    const y = rowY + ITEM_TOP;
    const mark = s.itemMark(item.item_id);
    const agent = mark !== undefined;
    const selected = s.selection.has(item.item_id);
    const base = agent ? pal.cyan : track.color || pal.wave;
    const bg = mix(base, pal.panel900, agent ? 0.18 : 0.22);
    const dim = item.muted ? 0.45 : 1;

    ctx.save();
    ctx.globalAlpha = dim;
    ctx.beginPath();
    ctx.roundRect(x, y, Math.max(1, w), ITEM_H, 2);
    ctx.clip();
    ctx.fillStyle = bg;
    ctx.fillRect(x, y, w, ITEM_H);

    // label strip
    ctx.fillStyle = pal.shade;
    ctx.fillRect(x, y, w, STRIP_H);
    if (w > 24) {
      ctx.fillStyle = pal.ink;
      ctx.font = '10px "IBM Plex Sans", system-ui, sans-serif';
      ctx.textBaseline = 'middle';
      const label = agent && mark ? `${item.name}  · edited by ${mark.label}` : item.name;
      ctx.save();
      ctx.beginPath();
      ctx.rect(x, y, w - 2, STRIP_H);
      ctx.clip();
      ctx.fillText(label, Math.max(x, 0) + 6, y + STRIP_H / 2 + 0.5);
      ctx.restore();
    }

    // waveform
    const areaTop = y + STRIP_H + 2;
    const areaH = ITEM_H - STRIP_H - 4;
    const mid = areaTop + areaH / 2;
    const vis0 = Math.max(0, (Math.max(0, -x)) / this.pps);
    const vis1 = Math.min(g.length, (W - x) / this.pps);
    const pk = vis1 > vis0 ? this.peaks.get(s.id, item, vis0 + (g.offset - item.offset_s), vis1 + (g.offset - item.offset_s), this.pps) : undefined;
    ctx.fillStyle = base;
    if (pk && w > 2) {
      const gainLin = dbToLinear(g.gain);
      const xs = Math.max(0, Math.floor(x));
      const xe = Math.min(W, Math.ceil(x + w));
      const span = pk.t1 - pk.t0;
      ctx.beginPath();
      for (let px = xs; px < xe; px++) {
        // item-relative time of this pixel column (with a trim in progress the audio keeps its place on the timeline)
        const tRel = (px - x) / this.pps + (g.offset - item.offset_s);
        const b0 = Math.floor(((tRel - pk.t0) / span) * pk.bins);
        const b1 = Math.max(b0 + 1, Math.floor(((tRel + 1 / this.pps - pk.t0) / span) * pk.bins));
        if (b1 <= 0 || b0 >= pk.bins) continue;
        let lo = 0;
        let hi = 0;
        for (let b = Math.max(0, b0); b < Math.min(pk.bins, b1); b++) {
          lo = Math.min(lo, pk.data[b * 2]!);
          hi = Math.max(hi, pk.data[b * 2 + 1]!);
        }
        const top = mid - (Math.min(1, (hi / 127) * gainLin) * areaH) / 2;
        const bottom = mid - (Math.max(-1, (lo / 127) * gainLin) * areaH) / 2;
        ctx.rect(px, top, 1, Math.max(1, bottom - top));
      }
      ctx.fill();
    }

    // fades and gain
    if (w > 30) {
      ctx.fillStyle = pal.shade;
      const fi = g.fadeIn * this.pps;
      const fo = g.fadeOut * this.pps;
      if (fi > 1) {
        ctx.beginPath();
        ctx.moveTo(x, areaTop);
        ctx.lineTo(x + fi, areaTop);
        ctx.lineTo(x, areaTop + areaH);
        ctx.closePath();
        ctx.fill();
      }
      if (fo > 1) {
        ctx.beginPath();
        ctx.moveTo(x + w, areaTop);
        ctx.lineTo(x + w - fo, areaTop);
        ctx.lineTo(x + w, areaTop + areaH);
        ctx.closePath();
        ctx.fill();
      }
      const gy = areaTop + areaH * (1 - (clamp(g.gain, GAIN_MIN, GAIN_MAX) - GAIN_MIN) / (GAIN_MAX - GAIN_MIN));
      const hot = this.hover?.item.item_id === item.item_id || selected;
      ctx.fillStyle = alpha(pal.ink, hot ? 0.55 : 0.18);
      ctx.fillRect(x, Math.round(gy), w, 1);
      if (hot) {
        ctx.fillStyle = pal.ink;
        ctx.fillRect(x + w / 2 - 3, gy - 3, 6, 6);
        ctx.fillRect(x + fi - 3, y + 2, 6, 6);
        ctx.fillRect(x + w - fo - 3, y + 2, 6, 6);
      }
    }
    ctx.restore();

    ctx.globalAlpha = 1;
    ctx.lineWidth = selected ? 2 : 1;
    ctx.strokeStyle = selected ? pal.orange : agent ? pal.cyan : pal.shade;
    ctx.beginPath();
    ctx.roundRect(x + 0.5, y + 0.5, Math.max(1, w - 1), ITEM_H - 1, 2);
    ctx.stroke();
  }

  private paintAgentCursor(ctx: CanvasRenderingContext2D, s: ProjectStore, scrollY: number, W: number, H: number): void {
    const focus = this.agentFocus(s);
    if (!focus) return;
    const i = s.tracks.findIndex((t) => t.track_id === focus.trackId);
    if (i < 0) return;
    const x = this.xOf(focus.timeS);
    const y = i * TRACK_H - scrollY;
    if (x < 0 || x > W || y + TRACK_H < 0 || y > H) return;
    const pal = this.palette;
    const age = (Date.now() - focus.at) / 1000;
    ctx.save();
    ctx.globalAlpha = age < 20 ? 1 : Math.max(0, 1 - (age - 20) / 10);
    ctx.fillStyle = pal.cyan;
    ctx.fillRect(Math.round(x), y, 2, TRACK_H);
    ctx.font = '600 11px "IBM Plex Sans", system-ui, sans-serif';
    ctx.textBaseline = 'middle';
    const label = focus.label;
    const tw = ctx.measureText(label).width + 12;
    const lx = Math.min(x + 4, W - tw - 2);
    const ly = y + TRACK_H - 18;
    ctx.beginPath();
    ctx.roundRect(lx, ly, tw, 16, 2);
    ctx.fill();
    ctx.fillStyle = pal.panel900;
    ctx.fillText(label, lx + 6, ly + 8.5);
    ctx.restore();
  }

  /** Where the agent last worked: the newest marked item (its start) or track. */
  private agentFocus(s: ProjectStore): { trackId: string; timeS: number; label: string; at: number } | undefined {
    let best: { id: string; mark: AgentMark } | undefined;
    for (const [id, mark] of s.marks) if (!best || mark.at > best.mark.at) best = { id, mark };
    if (!best || Date.now() - best.mark.at > 30000) return undefined;
    const item = s.items.get(best.id);
    const verb = best.mark.undoLabel.replace(`${best.mark.label}: `, '');
    const label = `${best.mark.label} · ${verb}`;
    if (item) return { trackId: item.track_id, timeS: item.start_s, label, at: best.mark.at };
    const track = s.track(best.id);
    if (track) return { trackId: track.track_id, timeS: this.tOf(8), label, at: best.mark.at };
    for (const [trackId, list] of s.fx) if (list.some((p) => p.plugin_id === best!.id)) return { trackId, timeS: this.tOf(8), label, at: best.mark.at };
    return undefined;
  }

  private paintRuler(ctx: CanvasRenderingContext2D, s: ProjectStore, playhead: number): void {
    const pal = this.palette;
    const W = this.rulerCanvas.clientWidth;
    const Hh = this.rulerCanvas.clientHeight;
    const tStart = this.t0;
    const tEnd = this.tOf(W);
    ctx.font = '10px "IBM Plex Mono", ui-monospace, monospace';
    ctx.textBaseline = 'middle';

    const bar0 = Math.max(1, Math.floor(secondsToBar(s.tempo, tStart)));
    const bar1 = Math.ceil(secondsToBar(s.tempo, tEnd)) + 1;
    const barPx = (barToSeconds(s.tempo, 2) - barToSeconds(s.tempo, 1)) * this.pps;
    const step = barPx < 30 ? Math.pow(2, Math.ceil(Math.log2(30 / barPx))) : 1;
    for (let b = bar0 - ((bar0 - 1) % step); b <= bar1; b += step) {
      const x = Math.round(this.xOf(barToSeconds(s.tempo, b)));
      ctx.fillStyle = pal.hairline;
      ctx.fillRect(x, 0, 1, Hh);
      ctx.fillStyle = pal.inkMuted;
      ctx.fillText(String(b), x + 4, Hh / 2 + 1);
    }

    // loop range (orange bar on top, like the design)
    const loop = s.transport.loop;
    if (loop.end_s > loop.start_s) {
      ctx.fillStyle = alpha(pal.orange, loop.enabled ? 0.55 : 0.2);
      const x0 = this.xOf(loop.start_s);
      ctx.fillRect(x0, 0, this.xOf(loop.end_s) - x0, 6);
    }

    // markers
    for (const m of s.markers) {
      const x = Math.round(this.xOf(m.time_s));
      if (x < -60 || x > W) continue;
      const agent = s.marks.has(m.marker_id);
      ctx.fillStyle = agent ? pal.cyan : pal.cream;
      ctx.fillRect(x, 6, 1, Hh - 6);
      ctx.beginPath();
      ctx.moveTo(x, 6);
      ctx.lineTo(x + 6, 9);
      ctx.lineTo(x, 12);
      ctx.fill();
      ctx.fillStyle = agent ? pal.cyan : pal.inkMuted;
      ctx.fillText(m.name, x + 9, 15);
    }

    const px = Math.round(this.xOf(playhead));
    if (px >= 0 && px <= W) {
      ctx.fillStyle = pal.orange;
      ctx.beginPath();
      ctx.moveTo(px - 5, 0);
      ctx.lineTo(px + 6, 0);
      ctx.lineTo(px, 8);
      ctx.fill();
      ctx.fillRect(px, 0, 1, Hh);
    }
  }

  /** Animation: keeps repainting while something moves (playing, agent cursor fading, drag). */
  private tickAnimations(): void {
    const s = this.store;
    const now = performance.now();
    if (now - this.frameStart >= 1000) {
      this.fps = Math.round((this.frameCount * 1000) / (now - this.frameStart));
      this.frameCount = 0;
      this.frameStart = now;
    }
    if (s?.loaded) {
      const recentAgent = Date.now() - s.lastAgentAt < 32000;
      if (s.isPlaying() || recentAgent) {
        this.followPlayhead(s);
        this.draw();
      } else if (this.app.playhead(s) !== this.lastStoppedPlayhead) {
        // a seek while stopped (rewind, go to start, ruler click): bring the cursor into view once
        this.lastStoppedPlayhead = this.app.playhead(s);
        if (this.revealPlayhead(s)) this.draw();
      }
    }
    requestAnimationFrame(() => this.tickAnimations());
  }

  private followPlayhead(s: ProjectStore): void {
    if (s.isPlaying()) this.revealPlayhead(s);
  }

  /** Scrolls so the playhead sits near the left edge when it is off screen; true if the view moved. */
  private revealPlayhead(s: ProjectStore): boolean {
    const W = this.lanesCanvas.clientWidth;
    const x = this.xOf(this.app.playhead(s));
    if (x <= W - 40 && x >= 0) return false;
    s.view.start = Math.max(0, this.app.playhead(s) - (W * 0.1) / s.view.pps);
    return true;
  }

  //==========================================================================
  // hit testing
  private trackAt(y: number): { track: TrackState; index: number; rowY: number } | undefined {
    const s = this.store;
    if (!s) return undefined;
    const i = Math.floor((y + this.scroller.scrollTop) / TRACK_H);
    const track = s.tracks[i];
    return track ? { track, index: i, rowY: i * TRACK_H - this.scroller.scrollTop } : undefined;
  }

  private hit(x: number, y: number): Hit | null {
    const s = this.store;
    const row = this.trackAt(y);
    if (!s || !row) return null;
    const idx = s.index(row.track.track_id);
    if (!idx) return null;
    const t = this.tOf(x);
    const slack = 8 / this.pps;
    const candidates = idx.query(t - slack, t + slack);
    for (let k = candidates.length - 1; k >= 0; k--) {
      const item = candidates[k]!;
      const g = this.itemGeometry(item);
      const x0 = this.xOf(g.start);
      const x1 = x0 + g.length * this.pps;
      if (x < x0 - 2 || x > x1 + 2) continue;
      const top = row.rowY + ITEM_TOP;
      const inStrip = y < top + STRIP_H + 2;
      const areaTop = top + STRIP_H + 2;
      const areaH = ITEM_H - STRIP_H - 4;
      const gy = areaTop + areaH * (1 - (clamp(g.gain, GAIN_MIN, GAIN_MAX) - GAIN_MIN) / (GAIN_MAX - GAIN_MIN));
      const w = x1 - x0;
      if (w > 30 && inStrip && Math.abs(x - (x0 + g.fadeIn * this.pps)) <= 6) return { item, zone: 'fadeIn' };
      if (w > 30 && inStrip && Math.abs(x - (x1 - g.fadeOut * this.pps)) <= 6) return { item, zone: 'fadeOut' };
      if (x - x0 <= EDGE) return { item, zone: 'trimL' };
      if (x1 - x <= EDGE) return { item, zone: 'trimR' };
      if (w > 30 && !inStrip && Math.abs(y - gy) <= 4 && Math.abs(x - (x0 + w / 2)) <= 14) return { item, zone: 'gain' };
      return { item, zone: 'body' };
    }
    return null;
  }

  private cursorFor(hit: Hit | null): string {
    if (!hit) return 'default';
    switch (hit.zone) {
      case 'trimL':
      case 'trimR':
        return 'ew-resize';
      case 'fadeIn':
      case 'fadeOut':
        return 'col-resize';
      case 'gain':
        return 'ns-resize';
      default:
        return 'grab';
    }
  }

  //==========================================================================
  // pointer: lanes
  private local(e: PointerEvent | MouseEvent | WheelEvent, c: HTMLCanvasElement): { x: number; y: number } {
    const r = c.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  private snapOptions(e: { altKey: boolean }): Parameters<typeof snapTime>[1] {
    const s = this.store!;
    return { tempo: s.tempo, grid: this.app.prefs.grid as Grid, enabled: this.app.prefs.snap && !e.altKey, markers: s.markersSeconds(), threshold: 8 / s.view.pps };
  }

  private onLanesDown(e: PointerEvent): void {
    const s = this.store;
    if (!s || e.button !== 0) return;
    this.lanesCanvas.setPointerCapture(e.pointerId);
    const { x, y } = this.local(e, this.lanesCanvas);
    const hit = this.hit(x, y);
    const row = this.trackAt(y);
    if (row) s.selectedTrack = row.track.track_id;

    if (!hit) {
      if (!e.shiftKey) s.selection.clear();
      this.app.seek(this.snappedTime(x, e));
      s.state.emit();
      return;
    }

    const id = hit.item.item_id;
    if (e.shiftKey) {
      if (s.selection.has(id)) s.selection.delete(id);
      else s.selection.add(id);
    } else if (!s.selection.has(id)) {
      s.selection.clear();
      s.selection.add(id);
    }
    const ids = hit.zone === 'body' || !s.selection.has(id) ? [...s.selection] : [id];
    const orig = new Map<string, Geometry>();
    for (const sid of ids.length ? ids : [id]) {
      const it = s.items.get(sid);
      if (it) orig.set(sid, { start: it.start_s, length: it.length_s, fadeIn: it.fade_in_s ?? 0, fadeOut: it.fade_out_s ?? 0, gain: it.gain_db ?? 0, offset: it.offset_s });
    }
    this.drag = { kind: 'item', zone: hit.zone, ids: [...orig.keys()], startX: x, startY: y, orig, moved: false, anchor: id, delta: null };
    this.draw();
  }

  private snappedTime(x: number, e: { altKey: boolean }): number {
    return snapTime(Math.max(0, this.tOf(x)), this.snapOptions(e));
  }

  private onLanesMove(e: PointerEvent): void {
    const s = this.store;
    if (!s) return;
    const { x, y } = this.local(e, this.lanesCanvas);
    this.lastMouse = { x: e.clientX, y: e.clientY };
    const d = this.drag;

    if (!d) {
      const hit = this.hit(x, y);
      if (hit?.item.item_id !== this.hover?.item.item_id || hit?.zone !== this.hover?.zone) {
        this.hover = hit;
        this.lanesCanvas.style.cursor = this.cursorFor(hit);
        this.draw();
      }
      this.scheduleTip(hit);
      return;
    }

    if (d.kind !== 'item') return;
    if (!d.moved && Math.abs(x - d.startX) < 3 && Math.abs(y - d.startY) < 3) return;
    d.moved = true;
    this.hideTip();
    const a = d.orig.get(d.anchor)!;
    const dt = (x - d.startX) / this.pps;
    const delta: Geometry = { start: a.start, length: a.length, fadeIn: a.fadeIn, fadeOut: a.fadeOut, gain: a.gain, offset: a.offset };
    const opts = this.snapOptions(e);
    switch (d.zone) {
      case 'body':
        delta.start = snapTime(Math.max(0, a.start + dt), opts);
        break;
      case 'trimL':
        delta.start = snapTime(Math.max(0, a.start + dt), opts);
        break;
      case 'trimR':
        delta.start = snapTime(a.start + a.length + dt, opts);
        break;
      case 'fadeIn':
        delta.fadeIn = clamp(a.fadeIn + dt, 0, a.length);
        break;
      case 'fadeOut':
        delta.fadeOut = clamp(a.fadeOut - dt, 0, a.length);
        break;
      case 'gain': {
        const per = (ITEM_H - STRIP_H - 4) / (GAIN_MAX - GAIN_MIN);
        delta.gain = clamp(a.gain - (y - d.startY) / per, GAIN_MIN, GAIN_MAX);
        break;
      }
    }
    d.delta = delta;
    this.draw();
  }

  private onUp(e: PointerEvent): void {
    const d = this.drag;
    this.drag = null;
    const s = this.store;
    if (!s || !d) return;

    if (d.kind === 'item') {
      if (d.moved && d.delta) void this.commitItemDrag(d, s);
      this.draw();
    } else if (d.kind === 'marker') {
      if (d.moved) {
        const x = this.local(e, this.rulerCanvas).x;
        const t = this.snappedTime(x, e);
        const mk = s.markers.find((m) => m.marker_id === d.id);
        if (mk) mk.time_s = t;
        void this.app.edit('marker.upsert', { markers: [{ marker_id: d.id, time_s: t }] });
        s.state.emit();
      }
    } else if (d.kind === 'ruler-loop') {
      const t = this.snappedTime(this.local(e, this.rulerCanvas).x, e);
      this.app.setLoop(Math.min(d.from, t), Math.max(d.from, t));
    }
  }

  private async commitItemDrag(d: Extract<Drag, { kind: 'item' }>, s: ProjectStore): Promise<void> {
    const ops: { name: string; params: Record<string, unknown> }[] = [];
    const newGeo = new Map<string, Geometry>();
    for (const id of d.ids) {
      // the geometry the drag drew
      const it = s.items.get(id);
      if (!it) continue;
      const g = this.itemGeometry(it, d);        // the geometry the drag drew (this.drag is already cleared)
      newGeo.set(id, g);
    }
    const label = `${d.zone === 'body' ? 'move' : d.zone} ${d.ids.length} item${d.ids.length > 1 ? 's' : ''}`;
    for (const [id, g] of newGeo) {
      const o = d.orig.get(id)!;
      const params: Record<string, unknown> = { item_id: id };
      if (d.zone === 'body') params.start_s = g.start;
      else if (d.zone === 'trimL') params.trim_start_s = g.start;
      else if (d.zone === 'trimR') params.trim_end_s = g.start + g.length;
      else if (d.zone === 'fadeIn') params.fade_in_s = g.fadeIn;
      else if (d.zone === 'fadeOut') params.fade_out_s = g.fadeOut;
      else if (d.zone === 'gain') params.gain_db = Math.round(g.gain * 10) / 10;
      if (Object.keys(params).length > 1 && (g.start !== o.start || g.length !== o.length || g.fadeIn !== o.fadeIn || g.fadeOut !== o.fadeOut || g.gain !== o.gain))
        ops.push({ name: 'item.update', params });
    }
    if (!ops.length) return;

    // optimistic: the model takes the new geometry at once
    this.drag = null;
    for (const [id, g] of newGeo) {
      const it = s.items.get(id);
      if (!it) continue;
      Object.assign(it, { start_s: g.start, length_s: g.length, end_s: g.start + g.length, fade_in_s: g.fadeIn, fade_out_s: g.fadeOut, gain_db: g.gain, offset_s: g.offset });
    }
    s.lengthS = Math.max(s.lengthS, ...[...s.items.values()].map((i) => i.end_s));
    s.refreshIndex();
    s.state.emit();
    await this.app.editBatch(ops, { undoLabel: label });
  }

  private onLanesDouble(e: MouseEvent): void {
    const { x, y } = this.local(e, this.lanesCanvas);
    const hit = this.hit(x, y);
    if (hit?.zone === 'gain') void this.app.edit('item.update', { item_id: hit.item.item_id, gain_db: 0 });
    else if (hit && hit.zone === 'body') {
      const item = hit.item;
      void textDialog({ title: 'Назва айтема', label: 'Назва', value: item.name }).then((name) => {
        if (name && name !== item.name) void this.app.edit('item.update', { item_id: item.item_id, name });
      });
    }
  }

  private onWheel(e: WheelEvent): void {
    const s = this.store;
    if (!s) return;
    e.preventDefault();
    const x = this.local(e, this.lanesCanvas).x;
    if (e.ctrlKey || e.metaKey) this.zoomBy(Math.exp(-e.deltaY * 0.0025), x);
    else if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) this.scrollBy(e.shiftKey && e.deltaX === 0 ? e.deltaY : e.deltaX);
    else this.scroller.scrollTop += e.deltaY;
  }

  //==========================================================================
  // pointer: ruler
  private onRulerDown(e: PointerEvent): void {
    const s = this.store;
    if (!s || e.button !== 0) return;
    this.rulerCanvas.setPointerCapture(e.pointerId);
    const { x, y } = this.local(e, this.rulerCanvas);
    const marker = y > 6 ? this.markerAt(x) : undefined;
    if (marker) {
      this.drag = { kind: 'marker', id: marker.marker_id, startX: x, orig: marker.time_s, moved: false };
      return;
    }
    if (e.shiftKey) {
      this.drag = { kind: 'ruler-loop', from: this.snappedTime(x, e) };
      return;
    }
    this.drag = { kind: 'ruler-seek' };
    this.app.seek(this.snappedTime(x, e));
  }

  private onRulerMove(e: PointerEvent): void {
    const d = this.drag;
    const { x } = this.local(e, this.rulerCanvas);
    if (d?.kind === 'ruler-seek') this.app.seek(this.snappedTime(x, e));
    else if (d?.kind === 'marker') {
      d.moved = true;
      const mk = this.store?.markers.find((m) => m.marker_id === d.id);
      if (mk) mk.time_s = this.snappedTime(x, e);
      this.draw();
    }
  }

  private markerAt(x: number): MarkerState | undefined {
    return this.store?.markers.find((m) => Math.abs(this.xOf(m.time_s) - x) <= 6);
  }

  private onRulerDouble(e: MouseEvent): void {
    const { x, y } = this.local(e, this.rulerCanvas);
    const marker = y > 6 ? this.markerAt(x) : undefined;
    if (marker) {
      void textDialog({ title: 'Назва маркера', label: 'Назва маркера (порожньо = видалити)', value: marker.name }).then((name) => {
        if (name === null) return;
        void (name === '' ? this.app.edit('marker.delete', { marker_id: marker.marker_id }, { refetch: true }) : this.app.edit('marker.upsert', { markers: [{ marker_id: marker.marker_id, name }] }, { refetch: true }));
      });
    } else {
      void this.app.edit('marker.create', { time_s: this.snappedTime(x, e), name: `M${(this.store?.markers.length ?? 0) + 1}` }, { refetch: true });
    }
  }

  //==========================================================================
  // tooltip: who changed the item last
  private scheduleTip(hit: Hit | null): void {
    this.hideTip();
    if (!hit || !this.store) return;
    const s = this.store;
    this.tipTimer = setTimeout(() => {
      const it = hit.item;
      const mark = s.itemMark(it.item_id);
      const lines = [h('div', { text: it.name }), h('div', { class: 'mono', text: `${formatClock(it.start_s)} + ${formatClock(it.length_s)}  ${formatDb(it.gain_db ?? 0, true)}` })];
      if (mark) {
        const t = new Date(mark.at);
        const hh = (n: number) => String(n).padStart(2, '0');
        lines.push(h('div', { class: 'by', text: `${mark.label} · ${hh(t.getHours())}:${hh(t.getMinutes())}:${hh(t.getSeconds())} · ${mark.undoLabel.replace(`${mark.label}: `, '')}` }));
      }
      clear(this.tip);
      this.tip.append(...lines);
      this.tip.style.display = 'block';
      this.tip.style.left = `${this.lastMouse.x + 14}px`;
      this.tip.style.top = `${this.lastMouse.y + 14}px`;
    }, 450);
  }

  private hideTip(): void {
    if (this.tipTimer) clearTimeout(this.tipTimer);
    this.tip.style.display = 'none';
  }

  //==========================================================================
  // actions used by the shell shortcuts
  splitAtPlayhead(): void {
    const s = this.store;
    if (!s) return;
    const at = this.app.playhead(s);
    const targets: ItemState[] = [];
    const pool = s.selection.size ? [...s.selection].map((id) => s.items.get(id)).filter((i): i is ItemState => !!i) : [...s.items.values()];
    for (const it of pool) if (it.start_s + 0.001 < at && it.end_s - 0.001 > at) targets.push(it);
    if (!targets.length) return;
    void this.app.editBatch(
      targets.map((it) => ({ name: 'item.split', params: { item_id: it.item_id, at_s: at } })),
      { refetch: true, undoLabel: `split ${targets.length} item${targets.length > 1 ? 's' : ''}` },
    );
  }

  deleteSelection(): void {
    const s = this.store;
    if (!s || !s.selection.size) return;
    const ids = [...s.selection];
    s.selection.clear();
    void this.app.editBatch(
      ids.map((id) => ({ name: 'item.delete', params: { item_id: id } })),
      { refetch: true, undoLabel: `delete ${ids.length} item${ids.length > 1 ? 's' : ''}` },
    );
  }

  /** Scrolls so that an entity of the activity log is visible (click on a log row). */
  reveal(ids: string[]): void {
    const s = this.store;
    if (!s) return;
    for (const id of ids) {
      const item = s.items.get(id);
      if (!item) continue;
      const i = s.tracks.findIndex((t) => t.track_id === item.track_id);
      this.scroller.scrollTop = Math.max(0, i * TRACK_H - TRACK_H);
      s.view.start = Math.max(0, item.start_s - 1);
      s.selection = new Set([item.item_id]);
      this.draw();
      return;
    }
  }

  /** Test hook: how many items are drawn at the current window (virtualisation). */
  visibleItemCount(): number {
    const s = this.store;
    if (!s) return 0;
    const tEnd = this.tOf(this.lanesCanvas.clientWidth);
    let n = 0;
    for (const t of s.tracks) n += s.index(t.track_id)?.query(this.t0, tEnd).length ?? 0;
    return n;
  }

  redraw(): void {
    this.draw();
  }
}

function lastEnd(s: ProjectStore, trackId: string): number {
  let e = 0;
  for (const it of s.items.values()) if (it.track_id === trackId) e = Math.max(e, it.end_s);
  return e;
}

//==============================================================================
// TCP row
class TcpRow {
  readonly el: HTMLElement;
  private readonly num = h('span', { class: 'num' });
  private readonly color = h('span', { class: 'color' });
  private readonly nameEl = h('span', { class: 'name' });
  private readonly vol = h('div', { class: 'vol' }, h('i'));
  private readonly mute = h('button', { class: 'sbtn mute', text: 'M', title: 'Mute', attrs: { 'aria-label': 'Mute' } });
  private readonly solo = h('button', { class: 'sbtn solo', text: 'S', title: 'Solo', attrs: { 'aria-label': 'Solo' } });
  private readonly fx = h('button', { class: 'sbtn fx', text: 'FX', title: 'FX', attrs: { 'aria-label': 'FX' } });
  private readonly arm = h('button', { class: 'sbtn arm', text: '●', title: 'Запис недоступний у цій версії', attrs: { 'aria-label': 'Запис', disabled: '' } });
  private popover: HTMLElement | null = null;

  constructor(
    private readonly app: App,
    private readonly store: ProjectStore,
    private readonly id: string,
    index: number,
  ) {
    this.el = h(
      'div',
      { class: 'tcp', style: { top: `${index * TRACK_H}px` } },
      this.color,
      this.num,
      h('div', { class: 'body' }, this.nameEl, this.vol),
      h('div', { class: 'btns' }, this.arm, this.mute, this.solo, this.fx),
    );
    this.arm.style.color = 'var(--rx-red)';
    this.el.addEventListener('pointerdown', (e) => {
      if ((e.target as HTMLElement).closest('button, .vol, input')) return;
      this.store.selectedTrack = this.id;
      this.store.state.emit();
    });
    this.nameEl.addEventListener('dblclick', () => this.rename());
    this.mute.addEventListener('click', () => this.toggle('mute'));
    this.solo.addEventListener('click', () => this.toggle('solo'));
    this.fx.addEventListener('click', () => this.showFx());
    this.vol.addEventListener('pointerdown', (e) => this.dragVolume(e));
    this.vol.addEventListener('dblclick', () => this.setVolume(0, false));
  }

  update(t: TrackState, index: number): void {
    this.el.style.top = `${index * TRACK_H}px`;
    this.num.textContent = String(index + 1);
    this.color.style.background = t.color;
    if (this.nameEl.textContent !== t.name) this.nameEl.textContent = t.name;
    this.nameEl.title = t.name;
    this.mute.classList.toggle('on', t.mute);
    this.solo.classList.toggle('on', t.solo);
    const plugins = (this.store.fx.get(t.track_id) ?? []).filter((p) => p.type !== 'volume');
    this.fx.classList.toggle('has', plugins.length > 0);
    this.el.classList.toggle('selected', this.store.selectedTrack === t.track_id);
    this.el.classList.toggle('muted', t.mute);
    this.el.classList.toggle('agent', this.store.trackMark(t.track_id) !== undefined);
    const bar = this.vol.firstElementChild as HTMLElement;
    bar.style.width = `${dbToFader(t.volume_db ?? 0) * 100}%`;
    this.vol.title = `Гучність ${formatDb(t.volume_db ?? 0, true)} (двічі клік = 0 dB)`;
  }

  private rename(): void {
    const input = h('input', { class: 'name-input', attrs: { value: this.nameEl.textContent ?? '' } });
    const done = (commit: boolean) => {
      const v = input.value.trim();
      input.replaceWith(this.nameEl);
      if (commit && v && v !== this.nameEl.textContent) {
        this.nameEl.textContent = v;
        void this.app.edit('track.update', { track_id: this.id, name: v });
      }
    };
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') done(true);
      else if (e.key === 'Escape') done(false);
      e.stopPropagation();
    });
    input.addEventListener('blur', () => done(true));
    this.nameEl.replaceWith(input);
    input.focus();
    input.select();
  }

  private toggle(key: 'mute' | 'solo'): void {
    const t = this.store.track(this.id);
    if (!t) return;
    this.store.patchTrack(this.id, { [key]: !t[key] });
    void this.app.edit('track.update', { track_id: this.id, [key]: !t[key] });
  }

  private setVolume(db: number, gesture: boolean): void {
    this.store.patchTrack(this.id, { volume_db: db });
    void this.app.edit('track.update', { track_id: this.id, volume_db: Math.round(db * 10) / 10 }, gesture ? { coalesceKey: `vol:${this.id}` } : {});
  }

  private dragVolume(e: PointerEvent): void {
    e.preventDefault();
    this.vol.setPointerCapture(e.pointerId);
    let last = 0;
    const apply = (ev: PointerEvent) => {
      const r = this.vol.getBoundingClientRect();
      const db = faderToDb(clamp((ev.clientX - r.left) / r.width, 0, 1));
      this.store.patchTrack(this.id, { volume_db: db });
      const now = performance.now();
      if (now - last >= 33) {
        last = now;
        void this.app.edit('track.update', { track_id: this.id, volume_db: Math.round(db * 10) / 10 }, { coalesceKey: `vol:${this.id}` });
      }
      return db;
    };
    let db = apply(e);
    const move = (ev: PointerEvent) => {
      db = apply(ev);
    };
    const up = () => {
      this.vol.removeEventListener('pointermove', move);
      this.vol.removeEventListener('pointerup', up);
      this.setVolume(db, true);
    };
    this.vol.addEventListener('pointermove', move);
    this.vol.addEventListener('pointerup', up);
  }

  private showFx(): void {
    this.popover?.remove();
    const list = this.store.fx.get(this.id) ?? [];
    const rows = list.map((p) =>
      h('div', { class: 'row' }, h('span', { text: p.name, style: { color: p.enabled ? 'var(--rx-ink)' : 'var(--rx-ink-muted)' } }), h('span', { class: 'type', text: p.type })),
    );
    const r = this.fx.getBoundingClientRect();
    const pop = h(
      'div',
      { class: 'popover', style: { left: `${Math.min(r.left, window.innerWidth - 330)}px`, top: `${r.bottom + 4}px` } },
      h('div', { class: 'head label', text: `FX · ${this.store.track(this.id)?.name ?? ''}` }),
      ...(rows.length ? rows : [h('div', { class: 'row', text: 'Немає плагінів' })]),
    );
    this.popover = pop;
    document.body.append(pop);
    const close = (ev: Event) => {
      if (!pop.contains(ev.target as Node)) {
        pop.remove();
        document.removeEventListener('pointerdown', close, true);
      }
    };
    setTimeout(() => document.addEventListener('pointerdown', close, true), 0);
  }
}

