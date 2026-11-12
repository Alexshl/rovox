// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The main window of the approved design: menu bar, project tabs, transport, arrange + mixer, MCP panel on the right.
import { clear, h, rafThrottle } from '../lib/dom';
import { GRIDS } from '../lib/snap';
import { formatBBT, formatBpm, formatClock, formatWallClock } from '../lib/time';
import type { App } from '../store/session';
import { type ProjectStore, isAgent } from '../store/project';
import { ArrangeView } from './arrange';
import { MixerView } from './mixer';
import { textDialog } from './dialog';
import { openProjectDialog } from './projects';
import { openRenderDialog } from './render';

const ICONS = {
  start: '<svg viewBox="0 0 16 16"><rect x="2" y="3" width="2" height="10"/><path d="M14 3v10L5.5 8z"/></svg>',
  stop: '<svg viewBox="0 0 16 16"><rect x="3" y="3" width="10" height="10"/></svg>',
  play: '<svg viewBox="0 0 16 16"><path d="M4 2.5v11L13.5 8z"/></svg>',
  pause: '<svg viewBox="0 0 16 16"><rect x="3.5" y="3" width="3" height="10"/><rect x="9.5" y="3" width="3" height="10"/></svg>',
  rec: '<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="5"/></svg>',
  speaker: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M2.5 6h2.5L8.5 3v10L5 10H2.5z" fill="currentColor"/><path d="M10.7 5.8a3.2 3.2 0 0 1 0 4.4M12.4 4.2a5.5 5.5 0 0 1 0 7.6" stroke-linecap="round"/></svg>',
  speakerMuted: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"><path d="M2.5 6h2.5L8.5 3v10L5 10H2.5z" fill="currentColor"/><path d="M10.8 6l3.4 4M14.2 6l-3.4 4" stroke-linecap="round"/></svg>',
  loop: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M3 7V6a2 2 0 0 1 2-2h7M13 9v1a2 2 0 0 1-2 2H4"/><path d="M10 2l2.5 2L10 6M6 10l-2.5 2L6 14" fill="currentColor"/></svg>',
};

function setText(el: HTMLElement, text: string): void {
  if (el.textContent !== text) el.textContent = text;
}

function setTitle(el: HTMLElement, title: string): void {
  if (el.title !== title) el.title = title;
}

function icon(svg: string): HTMLElement {
  const span = document.createElement('span');
  span.innerHTML = svg;
  span.style.display = 'inline-flex';
  return span;
}

interface MenuEntry {
  label: string;
  key?: string;
  action?: () => void;
  disabled?: boolean;
  separator?: boolean;
}

/** Object-storage sync state of a project for its tab: cloud ok / uploading / error. Nothing for local storage. */
export function syncState(p: ProjectStore): 'ok' | 'uploading' | 'error' | undefined {
  const s = p.storage;
  if (!s || s.backend !== 's3') return undefined;
  if (s.last_error) return 'error';
  return s.pending_uploads > 0 ? 'uploading' : 'ok';
}

function syncBadge(p: ProjectStore): HTMLElement | null {
  const st = syncState(p);
  if (!st) return null;
  const s = p.storage!;
  const title =
    st === 'error'
      ? `Помилка синхронізації з хмарою: ${s.last_error}`
      : st === 'uploading'
        ? 'Вивантаження змін у хмару…'
        : s.synced_revision !== null
          ? `Збережено в хмарі (ревізія ${s.synced_revision})`
          : 'Підключено до хмари';
  return h('span', { class: `sync ${st}`, text: '☁', title, attrs: { 'aria-label': title, role: 'img' } });
}

export class Shell {
  readonly el: HTMLElement;
  private readonly arrange: ArrangeView;
  private readonly mixer: MixerView;
  private readonly menubar = h('div', { class: 'menubar' });
  private readonly tabs = h('div', { class: 'tabs', attrs: { role: 'tablist', 'aria-label': 'Відкриті проекти' } });
  private readonly transport = h('div', { class: 'transport' });
  private readonly mcp = h('aside', { class: 'mcp-panel', attrs: { 'aria-label': 'MCP-агенти' } });
  private readonly banner = h('div', { class: 'banner', style: { display: 'none' } });
  private readonly toasts = h('div', { class: 'toasts', attrs: { role: 'status', 'aria-live': 'polite' } });
  private readonly toastEls = new Map<number, HTMLElement>();
  private readonly status = h('span', { class: 'status' });
  private clockBig = h('span', { class: 'big' });
  private clockSmall = h('span', { class: 'small' });
  private bpmEl = h('span', { class: 'readout' });
  private sigEl = h('span', { class: 'readout muted' });
  private btn: Record<string, HTMLButtonElement> = {};
  private snapBtn = h('button', { class: 'toggle', attrs: { 'aria-label': 'Snap' } });
  private gridSel = h('select', { attrs: { 'aria-label': 'Grid' } }, ...GRIDS.map((g) => h('option', { text: g, attrs: { value: g } })));
  private audioBtn = h('button', { class: 'tbtn wide audio' });
  private undoAgentBtn = h('button', { text: 'Undo агента' });
  private acceptBtn = h('button', { class: 'primary', text: 'Прийняти' });
  private agentsBox = h('div', { class: 'agents' });
  private logBox = h('div', { class: 'activity' });
  private activityTitle = h('div', { class: 'activity-title label' });
  private connEl = h('span', { class: 'conn' });
  private openMenu: HTMLElement | null = null;
  private readonly menuOpeners = new Map<HTMLElement, (focus: 'first' | 'last' | null) => void>();
  private tabFocusId: string | undefined;
  private logSignature = '';
  private readonly schedule = rafThrottle(() => this.render());

  constructor(private readonly app: App) {
    this.arrange = new ArrangeView(app);
    this.mixer = new MixerView(app);
    this.buildMenubar();
    this.buildTransport();
    this.buildMcpPanel();

    const main = h('div', { class: 'main' }, this.arrange.el, this.mixer.el);
    this.el = h('div', { id: 'app' }, this.menubar, this.tabs, this.transport, this.banner, h('div', { class: 'workspace' }, main, this.mcp), this.toasts);

    app.changed.on(() => this.schedule());
    app.audio.onChange(() => this.schedule());
    // the worklet reports ~30 times a second: that touches only the audio button, never the whole shell
    app.audio.onStats(() => this.renderAudioButton());
    document.addEventListener('keydown', (e) => this.onKey(e));
    document.addEventListener('pointerdown', (e) => {
      if (this.openMenu && !this.openMenu.contains(e.target as Node)) this.closeMenus();
    });
    setInterval(() => this.tickClock(), 33);
    this.render();
  }

  get arrangeView(): ArrangeView {
    return this.arrange;
  }

  //==========================================================================
  private buildMenubar(): void {
    const app = this.app;
    const menu = (title: string, entries: () => MenuEntry[]) => {
      const dropdown = h('div', { class: 'dropdown', attrs: { role: 'menu', 'aria-label': title } });
      const trigger = h('button', { text: title, attrs: { 'aria-haspopup': 'menu', 'aria-expanded': 'false' } });
      const root = h('div', { class: 'menu' }, trigger, dropdown);
      const open = (focus: 'first' | 'last' | null) => {
        this.closeMenus();
        clear(dropdown);
        for (const e of entries()) {
          if (e.separator) dropdown.append(h('div', { class: 'sep', attrs: { role: 'separator' } }));
          else {
            const item = h('button', { class: 'item', attrs: { role: 'menuitem' } }, h('span', { text: e.label }), h('span', { class: 'key', text: e.key ?? '' }));
            item.disabled = !!e.disabled;
            item.addEventListener('click', () => {
              this.closeMenus(true);
              e.action?.();
            });
            dropdown.append(item);
          }
        }
        root.classList.add('open');
        trigger.setAttribute('aria-expanded', 'true');
        this.openMenu = root;
        const items = this.menuItems(root);
        (focus === 'first' ? items[0] : focus === 'last' ? items[items.length - 1] : undefined)?.focus();
      };
      this.menuOpeners.set(root, open);
      trigger.addEventListener('click', (e) => {
        if (root.classList.contains('open')) this.closeMenus();
        else open(e.detail === 0 ? 'first' : null); // Enter / Space on the button (no pointer): focus goes into the menu
      });
      trigger.addEventListener('keydown', (e) => {
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
          e.preventDefault();
          e.stopPropagation();
          open(e.key === 'ArrowDown' ? 'first' : 'last');
        } else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
          e.preventDefault();
          e.stopPropagation();
          this.switchMenu(root, e.key === 'ArrowRight' ? 1 : -1, root.classList.contains('open'));
        } else if (e.key === 'Escape' && root.classList.contains('open')) {
          e.preventDefault();
          e.stopPropagation();
          this.closeMenus(true);
        }
      });
      dropdown.addEventListener('keydown', (e) => {
        const items = this.menuItems(root);
        const at = items.indexOf(document.activeElement as HTMLButtonElement);
        let handled = true;
        if (e.key === 'ArrowDown') items[(at + 1) % items.length]?.focus();
        else if (e.key === 'ArrowUp') items[(at <= 0 ? items.length : at) - 1]?.focus();
        else if (e.key === 'Home') items[0]?.focus();
        else if (e.key === 'End') items[items.length - 1]?.focus();
        else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') this.switchMenu(root, e.key === 'ArrowRight' ? 1 : -1, true);
        else if (e.key === 'Escape') this.closeMenus(true);
        else if (e.key === 'Tab') {
          this.closeMenus(true); // the default Tab then moves on from the menu button
          handled = false;
        } else handled = false;
        if (handled) {
          e.preventDefault();
          e.stopPropagation();
        }
      });
      return root;
    };
    const p = () => app.active;

    this.menubar.append(
      h('span', { class: 'logo', text: 'ROVOX' }),
      menu('File', () => [
        { label: 'Новий проект', action: () => void this.promptNewProject() },
        { label: 'Відкрити проект...', action: () => openProjectDialog(app) },
        { label: 'Зберегти', key: 'Ctrl+S', disabled: !p(), action: () => void app.cmd('project.save') },
        { separator: true, label: '' },
        { label: 'Render...', disabled: !p(), action: () => openRenderDialog(app) },
        { separator: true, label: '' },
        { label: 'Закрити проект', disabled: !p(), action: () => p() && void app.closeProject(p()!.id) },
      ]),
      menu('Edit', () => [
        { label: 'Undo', key: 'Ctrl+Z', disabled: !p()?.canUndo, action: () => void app.undo() },
        { label: 'Redo', key: 'Ctrl+Shift+Z', disabled: !p()?.canRedo, action: () => void app.redo() },
        { separator: true, label: '' },
        { label: 'Розрізати на курсорі', key: 'S', action: () => this.arrange.splitAtPlayhead() },
        { label: 'Видалити вибране', key: 'Del', action: () => this.arrange.deleteSelection() },
      ]),
      menu('View', () => [
        { label: app.prefs.mixerOpen ? 'Сховати мікшер' : 'Показати мікшер', key: 'Ctrl+M', action: () => app.updatePrefs({ mixerOpen: !app.prefs.mixerOpen }) },
        { label: 'Збільшити', key: '+', action: () => this.arrange.zoomBy(1.5) },
        { label: 'Зменшити', key: '-', action: () => this.arrange.zoomBy(1 / 1.5) },
        { label: 'Весь проект', key: 'Ctrl+0', action: () => this.arrange.fit() },
        { separator: true, label: '' },
        { label: app.prefs.theme === 'dark' ? 'Світла тема (Daylight)' : 'Темна тема (Studio)', action: () => app.updatePrefs({ theme: app.prefs.theme === 'dark' ? 'light' : 'dark' }) },
      ]),
      menu('Track', () => [
        { label: 'Нова доріжка', disabled: !p(), action: () => void app.edit('track.create', { name: `Track ${(p()?.tracks.length ?? 0) + 1}` }, { refetch: true }) },
        { label: 'Видалити вибрану доріжку', disabled: !p()?.selectedTrack, action: () => p()?.selectedTrack && void app.edit('track.delete', { track_id: p()!.selectedTrack }, { refetch: true }) },
      ]),
      menu('Agents', () => [
        { label: 'Undo агента', disabled: !p() || p()!.agentUndo().count === 0, action: () => void app.undoAgent() },
        { label: 'Прийняти зміни агента', disabled: !p() || p()!.marks.size === 0, action: () => void app.acceptAgent() },
      ]),
      menu('Help', () => [
        { label: 'Source code (AGPL-3.0)', action: () => window.open(app.serverInfo.source_url || '/source', '_blank', 'noopener') },
        { label: `Rovox ${app.serverInfo.version}`, disabled: true },
      ]),
      h('span', { class: 'spacer' }),
      this.status,
    );
  }

  /** Closes the open menu; `refocus` returns focus to its button (Escape, after an item was chosen). */
  private closeMenus(refocus = false): void {
    const trigger = this.openMenu?.querySelector('button');
    this.menubar.querySelectorAll('.menu.open').forEach((m) => {
      m.classList.remove('open');
      m.firstElementChild?.setAttribute('aria-expanded', 'false');
    });
    this.openMenu = null;
    if (refocus) trigger?.focus();
  }

  private menuItems(root: HTMLElement): HTMLButtonElement[] {
    return [...root.querySelectorAll<HTMLButtonElement>('.dropdown button.item:not(:disabled)')];
  }

  /** ArrowLeft / ArrowRight: the neighbouring menu; an open menu stays open (and gets the focus on its first item). */
  private switchMenu(from: HTMLElement, dir: number, keepOpen: boolean): void {
    const roots = [...this.menubar.querySelectorAll<HTMLElement>('.menu')];
    const target = roots[(roots.indexOf(from) + dir + roots.length) % roots.length];
    if (!target || target === from) return;
    if (keepOpen) this.menuOpeners.get(target)?.('first');
    else target.querySelector('button')?.focus();
  }

  private buildTransport(): void {
    const app = this.app;
    const mk = (key: string, svg: string, label: string, cls: string, fn: () => void): HTMLButtonElement => {
      const b = h('button', { class: `tbtn ${cls}`.trim(), title: label, attrs: { 'aria-label': label } }, icon(svg));
      b.addEventListener('click', fn);
      this.btn[key] = b;
      return b;
    };
    this.snapBtn.addEventListener('click', () => app.updatePrefs({ snap: !app.prefs.snap }));
    this.gridSel.addEventListener('change', () => app.updatePrefs({ grid: this.gridSel.value as (typeof GRIDS)[number] }));
    this.audioBtn.addEventListener('click', () => void app.enableAudio());
    this.transport.append(
      mk('start', ICONS.start, 'На початок', '', () => void app.toStart()),
      mk('stop', ICONS.stop, 'Стоп', '', () => void this.stop()),
      mk('play', ICONS.play, 'Відтворення', 'play', () => void this.play()),
      mk('pause', ICONS.pause, 'Пауза', '', () => void this.stop()),
      mk('rec', ICONS.rec, 'Запис недоступний у цій версії', 'rec', () => undefined),
      mk('loop', ICONS.loop, 'Цикл', 'loop', () => app.toggleLoop()),
      h('div', { class: 'clock' }, this.clockBig, this.clockSmall),
      h('span', { class: 'readout muted', text: 'BPM' }),
      this.bpmEl,
      this.sigEl,
      h('span', { class: 'spacer' }),
      this.audioBtn,
      h('button', { class: 'tbtn wide', text: 'Render', on: { click: () => openRenderDialog(app) } }),
      h('span', { class: 'label', text: 'GRID' }),
      this.gridSel,
      h('span', { class: 'label', text: 'SNAP' }),
      this.snapBtn,
    );
    this.btn['rec']!.disabled = true;
  }

  /** The --audio none preview player: one element at most, so repeated Play never stacks previews. */
  private previewAudio: HTMLAudioElement | null = null;

  private async stop(): Promise<void> {
    this.stopPreview();
    await this.app.stop();
  }

  /** Bumped by every stopPreview(): a render.preview that finishes after a Stop (or a newer preview) must not start playing. */
  private previewGen = 0;
  private previewPending = false;

  private stopPreview(): void {
    this.previewGen++;
    this.previewPending = false;
    this.previewAudio?.pause();
    this.previewAudio = null;
  }

  private async play(): Promise<void> {
    const app = this.app;
    const p = app.active;
    if (!p) return;
    if (app.serverInfo.audio.mode === 'none') {
      await this.previewFallback();
      return;
    }
    if (!app.audio.stats.enabled) {
      // the click is the user gesture the browser wants before it lets a page make sound
      await app.enableAudio();
    }
    await app.play();
  }

  /** Without a server stream (--audio none): render a short preview and play that file locally. */
  private async previewFallback(): Promise<void> {
    const app = this.app;
    const p = app.active;
    if (!p) return;
    this.stopPreview();
    const gen = this.previewGen;
    app.toast('Відтворення недоступне (сервер запущено з --audio none). Граю render.preview: 30 с від курсора.');
    const start = p.transport.position_s;
    this.previewPending = true;
    let r: Awaited<ReturnType<typeof app.mcp.runJob>>;
    try {
      r = await app.mcp.runJob('render.preview', { project_id: p.id, range: { start: { s: start }, length: { s: 30 } }, format: 'mp3' });
    } finally {
      if (gen === this.previewGen) this.previewPending = false;
    }
    if (gen !== this.previewGen) return; // Stop (or another preview) came while the render was running
    const clip = (r.data as { result?: { clip?: { path?: string } } } | undefined)?.result?.clip;
    if (!r.ok || !clip?.path) {
      app.toast(r.error?.message ?? 'preview не вдався', 'error');
      return;
    }
    const audio = new Audio(`/api/projects/${encodeURIComponent(p.id)}/${clip.path}`);
    this.previewAudio = audio;
    audio.addEventListener('ended', () => {
      if (this.previewAudio === audio) this.previewAudio = null;
    });
    await audio.play().catch(() => app.toast('Браузер не дозволив відтворення', 'error'));
  }

  private buildMcpPanel(): void {
    const app = this.app;
    this.undoAgentBtn.addEventListener('click', () => void app.undoAgent());
    this.acceptBtn.addEventListener('click', () => void app.acceptAgent());
    this.mcp.append(
      h('div', { class: 'mcp-head' }, h('span', { class: 'label', text: 'MCP · AGENTS' }), this.connEl),
      this.agentsBox,
      this.activityTitle,
      this.logBox,
      h('div', { class: 'mcp-actions' }, this.undoAgentBtn, this.acceptBtn),
    );
  }

  //==========================================================================
  private onKey(e: KeyboardEvent): void {
    const target = e.target as HTMLElement;
    if (target.closest('input, textarea, select') || document.querySelector('.scrim')) return;
    // a menu button or item takes its own keys; only the Ctrl/Cmd shortcuts stay global there
    if (!(e.ctrlKey || e.metaKey) && target.closest('.menubar')) return;
    // Space / Enter activate a focused button (tab mute, FX panel) or move a slider: not the global play shortcut then
    if ((e.key === ' ' || e.key === 'Enter') && target.closest('button, [role=button], [role=slider], input[type=range]')) return;
    const app = this.app;
    const mod = e.ctrlKey || e.metaKey;

    if (mod && e.key.toLowerCase() === 'z') {
      e.preventDefault();
      void (e.shiftKey ? app.redo() : app.undo());
    } else if (mod && e.key.toLowerCase() === 'y') {
      e.preventDefault();
      void app.redo();
    } else if (mod && e.key.toLowerCase() === 's') {
      e.preventDefault();
      void app.cmd('project.save');
    } else if (mod && e.key.toLowerCase() === 'm') {
      e.preventDefault();
      app.updatePrefs({ mixerOpen: !app.prefs.mixerOpen });
    } else if (mod && e.key === '0') {
      e.preventDefault();
      this.arrange.fit();
    } else if (!mod && e.key === ' ') {
      e.preventDefault();
      if (app.active?.isPlaying() || this.previewAudio || this.previewPending) void this.stop();
      else void this.play();
    } else if (!mod && e.key.toLowerCase() === 's') {
      this.arrange.splitAtPlayhead();
    } else if (e.key === 'Delete' || e.key === 'Backspace') {
      this.arrange.deleteSelection();
    } else if (e.key === 'Home') {
      void app.toStart();
    } else if (!mod && e.key.toLowerCase() === 'l') {
      app.toggleLoop();
    } else if (!mod && e.key.toLowerCase() === 'm') {
      const p = app.active;
      if (p) void app.edit('marker.create', { time_s: app.playhead(p), name: `M${p.markers.length + 1}` }, { refetch: true });
    } else if (e.key === '+' || e.key === '=') {
      this.arrange.zoomBy(1.4);
    } else if (e.key === '-') {
      this.arrange.zoomBy(1 / 1.4);
    }
  }

  //==========================================================================
  private tickClock(): void {
    const p = this.app.active;
    if (!p) {
      this.clockBig.textContent = '0.1.00';
      this.clockSmall.textContent = '0:00.000 / 0:00.000';
      return;
    }
    const t = this.app.playhead(p);
    this.clockBig.textContent = formatBBT(p.tempo, t);
    this.clockSmall.textContent = `${formatClock(t)} / ${formatClock(p.lengthS)}`;
  }

  private render(): void {
    const app = this.app;
    const p = app.active;
    this.arrange.sync();
    this.mixer.sync();

    // banner
    const offline = app.status !== 'open' && !app.needsLogin;
    this.banner.style.display = offline ? 'block' : 'none';
    this.banner.textContent = app.status === 'connecting' ? 'Підключення до сервера...' : 'Зв’язок із сервером втрачено, перепідключення...';

    // status
    const a = app.audio.stats;
    const audioPart = app.serverInfo.audio.mode === 'none' ? 'audio: none' : `${a.enabled ? 'Opus 48 kHz' : 'звук вимкнено'} · буфер 150 ms`;
    this.status.textContent = `${audioPart} · rovox ${app.serverInfo.version}`;

    this.renderTabs();
    this.renderTransport(p);
    this.renderMcp(p);
    this.renderToasts();
  }

  private tabsSignature = '';
  private readonly mixPending = new Set<string>();

  private renderTabs(): void {
    const app = this.app;
    const focusKey = (document.activeElement as HTMLElement | null)?.closest?.('.tabs [data-tab-focus]')?.getAttribute('data-tab-focus') ?? null;
    const sig = app.order.map((id) => { const q = app.projects.get(id); return q ? `${id}:${q.name}:${app.tabStatus(q)}:${q.dirty}:${q.muted}:${syncState(q) ?? ''}:${q.storage?.last_error ?? ''}:${id === app.activeId}` : id; }).join('|');
    if (sig === this.tabsSignature) return;
    this.tabsSignature = sig;
    clear(this.tabs);
    // the tab that is in the Tab order: the one last moved to, else the active one, else the first
    const rovingId = [this.tabFocusId, app.activeId, app.order[0]].find((x) => x !== undefined && app.projects.has(x));
    for (const id of app.order) {
      const p = app.projects.get(id);
      if (!p) continue;
      const st = app.tabStatus(p);
      const label = st === 'idle' ? (p.dirty ? 'edited' : 'idle') : st;
      const muteTitle = p.muted ? 'Увімкнути звук' : 'Вимкнути звук';
      const tabName = p.name || id;
      // a sibling of the tab, not a child: no interactive element inside role=tab
      const muteButton = h(
        'button',
        {
          class: `mute${p.muted ? ' on' : ''}`,
          title: muteTitle,
          attrs: { role: 'switch', 'aria-label': `Звук проєкту ${tabName}`, 'aria-checked': String(!p.muted), 'data-tab-focus': `mute:${id}` },
          on: {
            click: (e) => {
              e.stopPropagation();
              if (this.mixPending.has(id)) return; // a project.mix request is in flight
              this.mixPending.add(id);
              void app.toggleMute(id).finally(() => this.mixPending.delete(id));
            },
          },
        },
        icon(p.muted ? ICONS.speakerMuted : ICONS.speaker),
      );
      const tab = h(
        'div',
        { class: 'tab', attrs: { role: 'tab', 'aria-selected': String(id === app.activeId), tabindex: id === rovingId ? '0' : '-1', 'data-tab-focus': `tab:${id}` } },
        h('span', { class: 'name', text: p.name || id, title: p.name }),
        h('span', { class: 'state', text: label }),
        p.dirty ? h('span', { class: 'dirty', text: '●', title: 'Є незбережені зміни' }) : null,
        syncBadge(p),
      );
      // a sibling of the tab like the mute button: no interactive element inside role=tab
      const closeButton = h('button', { class: 'close', text: '×', title: 'Закрити проект', attrs: { 'aria-label': 'Закрити проект', 'data-tab-focus': `close:${id}` }, on: { click: (e) => { e.stopPropagation(); void app.closeProject(id); } } });
      const item = h('div', { class: `tabitem${id === app.activeId ? ' active' : ''}`, attrs: { role: 'presentation' } }, h('span', { class: `dot ${st}` }), muteButton, tab, closeButton);
      item.addEventListener('click', () => {
        this.tabFocusId = id;
        app.setActive(id);
      });
      tab.addEventListener('keydown', (e) => {
        if (e.target !== tab) return;
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          this.tabFocusId = id;
          app.setActive(id);
          return;
        }
        // roving tabindex: the arrows move the focus, Enter / Space activates
        const ids = app.order.filter((x) => app.projects.has(x));
        const at = ids.indexOf(id);
        const to = e.key === 'ArrowRight' ? ids[(at + 1) % ids.length] : e.key === 'ArrowLeft' ? ids[(at - 1 + ids.length) % ids.length] : e.key === 'Home' ? ids[0] : e.key === 'End' ? ids[ids.length - 1] : undefined;
        if (to === undefined) return;
        e.preventDefault();
        this.tabFocusId = to;
        this.tabs.querySelectorAll<HTMLElement>('[role=tab]').forEach((t) => t.setAttribute('tabindex', t.getAttribute('data-tab-focus') === `tab:${to}` ? '0' : '-1'));
        this.tabs.querySelector<HTMLElement>(`[data-tab-focus="tab:${to}"]`)?.focus();
      });
      this.tabs.append(item);
    }
    this.tabs.append(h('button', { class: 'add', text: '+', title: 'Новий проект', attrs: { 'aria-label': 'Новий проект', 'data-tab-focus': 'add' }, on: { click: () => void this.promptNewProject() } }));
    // the tabs are rebuilt on every change: keep the keyboard focus on the same control
    if (focusKey) this.tabs.querySelector<HTMLElement>(`[data-tab-focus="${focusKey}"]`)?.focus();
  }

  private async promptNewProject(): Promise<void> {
    const name = await textDialog({ title: 'Новий проект', label: 'Назва проекту', value: 'Untitled', okText: 'Створити' });
    if (name !== null) await this.app.newProject(name.trim() || 'Untitled');
  }

  private renderTransport(p: ReturnType<App['projects']['get']>): void {
    const app = this.app;
    const playing = !!p?.isPlaying();
    this.btn['play']!.classList.toggle('on', playing);
    this.btn['loop']!.classList.toggle('on', !!p?.transport.loop.enabled);
    for (const k of ['start', 'stop', 'play', 'pause', 'loop']) this.btn[k]!.disabled = !p;
    this.bpmEl.textContent = p ? formatBpm(p.tempo.points[0]?.bpm ?? 120) : '120.00';
    this.sigEl.textContent = p ? `${p.tempo.numerator}/${p.tempo.denominator}` : '4/4';
    this.gridSel.value = app.prefs.grid;
    this.snapBtn.textContent = app.prefs.snap ? 'ON' : 'OFF';
    this.snapBtn.classList.toggle('on', app.prefs.snap);

    this.renderAudioButton();
    this.tickClock();
  }

  /** The audio button and its readout: the only part of the shell that follows the 30 Hz stream statistics. */
  private renderAudioButton(): void {
    const app = this.app;
    const playing = !!app.active?.isPlaying();
    const a = app.audio.stats;
    const mode = app.serverInfo.audio.mode;
    this.audioBtn.classList.toggle('on', a.enabled && a.state !== 'underrun');
    this.audioBtn.classList.toggle('warn', a.enabled && (a.state === 'underrun' || a.underruns > 0) && playing);
    if (mode === 'none') {
      setText(this.audioBtn, 'Звук: preview');
      this.audioBtn.disabled = true;
      setTitle(this.audioBtn, 'Сервер без аудіо-виходу (--audio none): відтворення через render.preview');
    } else if (!a.supported) {
      setText(this.audioBtn, 'Звук: недоступний');
      this.audioBtn.disabled = true;
      setTitle(this.audioBtn, a.error ?? 'Потрібен браузер з WebCodecs (Chrome, Edge, Firefox 130+)');
    } else {
      this.audioBtn.disabled = false;
      setText(this.audioBtn, a.enabled ? (a.underruns > 0 && playing ? `Звук ● ${a.underruns} underrun` : 'Звук увімкнено') : 'Увімкнути звук');
      setTitle(this.audioBtn, a.enabled ? `буфер ${Math.round(a.bufferedMs)} ms · затримка виходу ${Math.round(a.outputLatencyMs)} ms · underruns ${a.underruns}` : 'Браузер вмикає звук лише після кліку');
    }
  }

  private renderMcp(p: ReturnType<App['projects']['get']>): void {
    const app = this.app;
    this.connEl.className = `conn${app.status === 'open' ? '' : ' offline'}`;
    clear(this.connEl);
    this.connEl.append(h('i'), document.createTextNode(app.status === 'open' ? location.host : 'offline'));

    // agents: every MCP/skill origin seen in the logs of the open projects
    const agents = new Map<string, { label: string; at: number; projects: Set<string> }>();
    for (const proj of app.projects.values())
      for (const l of proj.log) {
        if (!isAgent(l.origin)) continue;
        const key = l.origin.session || l.origin.label;
        const cur = agents.get(key) ?? { label: l.origin.label, at: 0, projects: new Set<string>() };
        cur.at = Math.max(cur.at, l.at);
        cur.projects.add(proj.name || proj.id);
        agents.set(key, cur);
      }
    clear(this.agentsBox);
    const live = [...agents.values()].filter((a) => Date.now() - a.at < 10 * 60 * 1000).sort((x, y) => y.at - x.at);
    if (!live.length) this.agentsBox.append(h('div', { class: 'none', text: 'Агенти не підключені. Підключіть Claude Code до /mcp.' }));
    for (const a of live.slice(0, 4)) {
      const active = Date.now() - a.at < 20000;
      this.agentsBox.append(h('div', { class: `agent-row${active ? ' active' : ''}` }, h('i'), h('b', { text: a.label }), h('span', { text: [...a.projects].join(' · '), title: [...a.projects].join(', ') })));
    }

    this.activityTitle.textContent = p ? `ACTIVITY · ${p.name}` : 'ACTIVITY';
    const sig = p ? `${p.id}|${p.log.length}|${p.log[0]?.revision ?? 0}|${p.log[0]?.at ?? 0}` : '';
    if (sig !== this.logSignature) {
      this.logSignature = sig;
      clear(this.logBox);
      for (const l of (p?.log ?? []).slice(0, 60)) {
        const agent = isAgent(l.origin);
        const row = h(
          'div',
          { class: 'log-row', attrs: { role: 'button', tabindex: '0' } },
          h('div', { class: 'meta' }, h('span', { text: formatWallClock(new Date(l.at)) }), h('span', { class: `tool${agent ? ' agent' : ''}`, text: l.tool })),
          h('span', { class: 'text', text: `${agent ? l.origin.label + ': ' : ''}${l.text}` }),
        );
        row.addEventListener('click', () => this.arrange.reveal(l.ids));
        row.addEventListener('keydown', (e) => {
          if (e.key !== 'Enter' && e.key !== ' ') return;
          e.preventDefault();
          this.arrange.reveal(l.ids);
        });
        this.logBox.append(row);
      }
      if (!p?.log.length) this.logBox.append(h('div', { class: 'agents' }, h('div', { class: 'none', text: 'Дій ще не було.' })));
    }

    const u = p?.agentUndo() ?? { count: 0, blockedByPerson: false };
    this.undoAgentBtn.textContent = u.count > 0 ? `Undo агента (${u.count})` : 'Undo агента';
    this.undoAgentBtn.disabled = u.count === 0;
    this.undoAgentBtn.title =
      u.count > 0 ? 'Відкотити останні кроки агента' : u.blockedByPerson ? 'Після кроків агента були ваші кроки: відкат лише лінійний (Ctrl+Z по черзі)' : 'У журналі немає кроків агента';
    this.acceptBtn.disabled = !p || p.marks.size === 0;
    this.acceptBtn.title = 'Прибрати підсвітку змін агента (історію undo не змінює)';
  }

  private renderToasts(): void {
    // only new toasts are added (and removed ones dropped): rebuilding the list would make a screen reader read all of them again
    const live = new Set(this.app.toasts.map((t) => t.id));
    for (const [id, el] of this.toastEls)
      if (!live.has(id)) {
        el.remove();
        this.toastEls.delete(id);
      }
    for (const t of this.app.toasts) {
      if (this.toastEls.has(t.id)) continue;
      const el = h('div', { class: `toast ${t.kind}`, text: t.text, attrs: t.kind === 'error' ? { role: 'alert' } : {} });
      this.toastEls.set(t.id, el);
      this.toasts.append(el);
    }
  }
}

/** The login form shown when the server wants a token. */
export function loginView(app: App): HTMLElement {
  const input = h('input', { attrs: { type: 'password', placeholder: 'token', 'aria-label': 'Token', autocomplete: 'off' } });
  const err = h('div', { class: 'err' });
  const go = async () => {
    err.textContent = '';
    if (!(await app.login(input.value.trim()))) err.textContent = 'Невірний токен';
  };
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') void go();
  });
  return h(
    'div',
    { id: 'app' },
    h(
      'div',
      { class: 'login' },
      h('span', { class: 'label', text: 'ROVOX' }),
      h('div', { text: 'Сервер вимагає токен доступу (його надруковано в логу при старті).' }),
      input,
      err,
      h('button', { class: 'btn primary', text: 'Увійти', on: { click: () => void go() } }),
    ),
  );
}
