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
  loop: '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M3 7V6a2 2 0 0 1 2-2h7M13 9v1a2 2 0 0 1-2 2H4"/><path d="M10 2l2.5 2L10 6M6 10l-2.5 2L6 14" fill="currentColor"/></svg>',
};

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
  private readonly toasts = h('div', { class: 'toasts' });
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
      const dropdown = h('div', { class: 'dropdown' });
      const root = h('div', { class: 'menu' }, h('button', { text: title, attrs: { 'aria-haspopup': 'true' } }), dropdown);
      root.firstElementChild!.addEventListener('click', () => {
        const wasOpen = root.classList.contains('open');
        this.closeMenus();
        if (!wasOpen) {
          clear(dropdown);
          for (const e of entries()) {
            if (e.separator) dropdown.append(h('div', { class: 'sep' }));
            else {
              const item = h('button', { class: 'item' }, h('span', { text: e.label }), h('span', { class: 'key', text: e.key ?? '' }));
              item.disabled = !!e.disabled;
              item.addEventListener('click', () => {
                this.closeMenus();
                e.action?.();
              });
              dropdown.append(item);
            }
          }
          root.classList.add('open');
          this.openMenu = root;
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

  private closeMenus(): void {
    this.menubar.querySelectorAll('.menu.open').forEach((m) => m.classList.remove('open'));
    this.openMenu = null;
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
      mk('stop', ICONS.stop, 'Стоп', '', () => void app.stop()),
      mk('play', ICONS.play, 'Відтворення', 'play', () => void this.play()),
      mk('pause', ICONS.pause, 'Пауза', '', () => void app.stop()),
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
    app.toast('Відтворення недоступне (сервер запущено з --audio none). Граю render.preview: 30 с від курсора.');
    const start = p.transport.position_s;
    const r = await app.mcp.runJob('render.preview', { project_id: p.id, range: { start: { s: start }, length: { s: 30 } }, format: 'mp3' });
    const clip = (r.data as { result?: { clip?: { path?: string } } } | undefined)?.result?.clip;
    if (!r.ok || !clip?.path) {
      app.toast(r.error?.message ?? 'preview не вдався', 'error');
      return;
    }
    const audio = new Audio(`/api/projects/${encodeURIComponent(p.id)}/${clip.path}`);
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
      if (app.active?.isPlaying()) void app.stop();
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

  private renderTabs(): void {
    const app = this.app;
    const sig = app.order.map((id) => { const q = app.projects.get(id); return q ? `${id}:${q.name}:${app.tabStatus(q)}:${q.dirty}:${syncState(q) ?? ''}:${q.storage?.last_error ?? ''}:${id === app.activeId}` : id; }).join('|');
    if (sig === this.tabsSignature) return;
    this.tabsSignature = sig;
    clear(this.tabs);
    for (const id of app.order) {
      const p = app.projects.get(id);
      if (!p) continue;
      const st = app.tabStatus(p);
      const label = st === 'idle' ? (p.dirty ? 'edited' : 'idle') : st;
      const tab = h(
        'div',
        { class: 'tab', attrs: { role: 'tab', 'aria-selected': String(id === app.activeId), tabindex: '0' } },
        h('span', { class: `dot ${st}` }),
        h('span', { class: 'name', text: p.name || id, title: p.name }),
        h('span', { class: 'state', text: label }),
        p.dirty ? h('span', { class: 'dirty', text: '●', title: 'Є незбережені зміни' }) : null,
        syncBadge(p),
        h('button', { class: 'close', text: '×', title: 'Закрити проект', attrs: { 'aria-label': 'Закрити проект' }, on: { click: (e) => { e.stopPropagation(); void app.closeProject(id); } } }),
      );
      tab.addEventListener('click', () => app.setActive(id));
      tab.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') app.setActive(id);
      });
      this.tabs.append(tab);
    }
    this.tabs.append(h('button', { class: 'add', text: '+', title: 'Новий проект', attrs: { 'aria-label': 'Новий проект' }, on: { click: () => void this.promptNewProject() } }));
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

    const a = app.audio.stats;
    const mode = app.serverInfo.audio.mode;
    this.audioBtn.classList.toggle('on', a.enabled && a.state !== 'underrun');
    this.audioBtn.classList.toggle('warn', a.enabled && (a.state === 'underrun' || a.underruns > 0) && playing);
    if (mode === 'none') {
      this.audioBtn.textContent = 'Звук: preview';
      this.audioBtn.disabled = true;
      this.audioBtn.title = 'Сервер без аудіо-виходу (--audio none): відтворення через render.preview';
    } else if (!a.supported) {
      this.audioBtn.textContent = 'Звук: недоступний';
      this.audioBtn.disabled = true;
      this.audioBtn.title = a.error ?? 'Потрібен браузер з WebCodecs (Chrome, Edge, Firefox 130+)';
    } else {
      this.audioBtn.disabled = false;
      this.audioBtn.textContent = a.enabled ? (a.underruns > 0 && playing ? `Звук ● ${a.underruns} underrun` : 'Звук увімкнено') : 'Увімкнути звук';
      this.audioBtn.title = a.enabled ? `буфер ${Math.round(a.bufferedMs)} ms · затримка виходу ${Math.round(a.outputLatencyMs)} ms · underruns ${a.underruns}` : 'Браузер вмикає звук лише після кліку';
    }
    this.tickClock();
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
          { class: 'log-row' },
          h('div', { class: 'meta' }, h('span', { text: formatWallClock(new Date(l.at)) }), h('span', { class: `tool${agent ? ' agent' : ''}`, text: l.tool })),
          h('span', { class: 'text', text: `${agent ? l.origin.label + ': ' : ''}${l.text}` }),
        );
        row.addEventListener('click', () => this.arrange.reveal(l.ids));
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
    clear(this.toasts);
    for (const t of this.app.toasts) this.toasts.append(h('div', { class: `toast ${t.kind}`, text: t.text }));
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
