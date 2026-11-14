// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The main window of the approved design: menu bar, project tabs, transport, arrange + mixer, MCP panel on the right.
import { clear, h, rafThrottle } from '../lib/dom';
import { GRIDS } from '../lib/snap';
import { formatBBT, formatBpm, formatClock } from '../lib/time';
import { availableLocales, errorDetail, fmtDate, getLocale, onLocaleChange, plural, setLocale, t } from '../i18n';
import type { App } from '../store/session';
import { type ProjectStore, isAgent } from '../store/project';
import { ArrangeView } from './arrange';
import { MixerView } from './mixer';
import { textDialog } from './dialog';
import { importController } from './importwizard';
import { openProjectDialog } from './projects';
import { openRenderDialog } from './render';
import { NARROW_QUERY, PHONE_QUERY, type Sheet, isNarrow, isPhone, openSheet, watchMedia } from './sheet';

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
  /** A menuitemradio (the language list): `checked` says which one is chosen. */
  checked?: boolean;
  /** A caption of a group, not an item. */
  heading?: boolean;
  action?: () => void;
  disabled?: boolean;
  separator?: boolean;
}

/** One menu entry as DOM (separator, group heading or item); `onPick` runs when an item is chosen, before its action. */
function menuEntryEl(e: MenuEntry, onPick: () => void): HTMLElement {
  if (e.separator) return h('div', { class: 'sep', attrs: { role: 'separator' } });
  if (e.heading) return h('div', { class: 'heading', text: e.label, attrs: { role: 'presentation' } });
  const radio = e.checked !== undefined;
  const item = h('button', { class: 'item', attrs: { role: radio ? 'menuitemradio' : 'menuitem', ...(radio ? { 'aria-checked': String(e.checked) } : {}) } }, h('span', { text: e.label }), h('span', { class: 'key', text: radio ? (e.checked ? '✓' : '') : (e.key ?? '') }));
  item.disabled = !!e.disabled;
  item.addEventListener('click', () => {
    onPick();
    e.action?.();
  });
  return item;
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
      ? t('tabs.sync.error', { error: s.last_error ?? '' })
      : st === 'uploading'
        ? t('tabs.sync.uploading')
        : s.synced_revision !== null
          ? t('tabs.sync.saved', { revision: s.synced_revision })
          : t('tabs.sync.connected');
  return h('span', { class: `sync ${st}`, text: '☁', title, attrs: { 'aria-label': title, role: 'img' } });
}

export class Shell {
  readonly el: HTMLElement;
  private readonly arrange: ArrangeView;
  private readonly mixer: MixerView;
  private readonly menubar = h('div', { class: 'menubar' });
  private readonly tabs = h('div', { class: 'tabs' });
  // The tablist is an empty element that owns the tabs through aria-owns: the mute switch and close button sit between the tabs in the DOM, and a
  // tablist may only contain tabs (axe aria-required-children).
  private readonly tablist = h('div', { class: 'sr-only', attrs: { role: 'tablist' } });
  private readonly transport = h('div', { class: 'transport' });
  private readonly mcp = h('aside', { class: 'mcp-panel' });
  private readonly banner = h('div', { class: 'banner', style: { display: 'none' } });
  private readonly toasts = h('div', { class: 'toasts', attrs: { role: 'status', 'aria-live': 'polite' } });
  private readonly toastEls = new Map<number, HTMLElement>();
  private readonly status = h('span', { class: 'status' });
  private clockBig = h('span', { class: 'big' });
  private clockSmall = h('span', { class: 'small' });
  private bpmEl = h('span', { class: 'readout' });
  private sigEl = h('span', { class: 'readout muted' });
  private btn: Record<string, HTMLButtonElement> = {};
  private snapBtn = h('button', { class: 'toggle' });
  private gridSel = h('select', {}, ...GRIDS.map((g) => h('option', { text: g, attrs: { value: g } })));
  private audioBtn = h('button', { class: 'tbtn wide audio' });
  private undoAgentBtn = h('button', {});
  private acceptBtn = h('button', { class: 'primary' });
  private agentsBox = h('div', { class: 'agents' });
  private logBox = h('div', { class: 'activity' });
  private activityTitle = h('div', { class: 'activity-title label' });
  private connEl = h('span', { class: 'conn' });
  private openMenu: HTMLElement | null = null;
  // narrow layout: the layout the DOM was built for (changes on a breakpoint), and the parts that exist only there
  private narrow = isNarrow();
  private phone = isPhone();
  private readonly menuModels: { title: string; entries: () => MenuEntry[] }[] = [];
  private readonly menuBtn = h('button', { class: 'menu-btn', text: '☰', attrs: { 'aria-haspopup': 'dialog', 'aria-expanded': 'false' } });
  private readonly agentsLabel = h('span', { class: 'agents-label' });
  private readonly agentsBtn = h('button', { class: 'agents-btn', attrs: { 'aria-haspopup': 'dialog', 'aria-expanded': 'false' } }, this.agentsLabel, h('i', { class: 'adot' }));
  private readonly moreBtn = h('button', { class: 'tbtn more-btn', text: '⋯', attrs: { 'aria-haspopup': 'dialog', 'aria-expanded': 'false' } });
  private readonly agentBarText = h('span', { class: 'text' });
  private readonly agentBarUndo = h('button', { class: 'btn' });
  private readonly agentBarAccept = h('button', { class: 'btn primary' });
  private readonly agentBar = h('div', { class: 'agent-bar', attrs: { role: 'status' } }, this.agentBarText, this.agentBarUndo, this.agentBarAccept);
  private menuSheet: Sheet | null = null;
  private morePop: { el: HTMLElement; close: (refocus?: boolean) => void } | null = null;
  private drawerScrim: HTMLElement | null = null;
  private readonly menuOpeners = new Map<HTMLElement, (focus: 'first' | 'last' | null) => void>();
  private tabFocusId: string | undefined;
  private logSignature = '';
  private transportWired = false;
  private mcpWired = false;
  private readonly schedule = rafThrottle(() => this.render());

  constructor(private readonly app: App) {
    this.arrange = new ArrangeView(app);
    this.mixer = new MixerView(app);
    this.applyLabels();
    this.buildMenubar();
    this.buildTransport();
    this.buildMcpPanel();

    const main = h('div', { class: 'main' }, this.arrange.el, this.mixer.el);
    this.tabs.append(this.tablist);
    this.el = h('div', { id: 'app' }, this.menubar, this.tabs, this.transport, this.banner, h('div', { class: 'workspace' }, main, this.mcp), this.toasts);

    onLocaleChange(() => this.relabel());
    app.welcomed.on(() => importController(app).checkResume());
    this.menuBtn.addEventListener('click', () => this.openMenuSheet());
    this.agentsBtn.addEventListener('click', () => (this.mcp.classList.contains('open') ? this.closeDrawer(true) : this.openDrawer()));
    this.moreBtn.addEventListener('click', () => this.toggleMore());
    this.agentBarUndo.addEventListener('click', () => void app.undoAgent());
    this.agentBarAccept.addEventListener('click', () => void app.acceptAgent());
    watchMedia(NARROW_QUERY, () => this.onMode());
    watchMedia(PHONE_QUERY, () => this.onMode());
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

  /** The labels of elements that live as long as the shell (the rest is rebuilt by buildMenubar / buildTransport / buildMcpPanel). */
  private applyLabels(): void {
    this.tablist.setAttribute('aria-label', t('a11y.tabs'));
    this.mcp.setAttribute('aria-label', t('a11y.mcp'));
    this.snapBtn.setAttribute('aria-label', t('a11y.snap'));
    this.gridSel.setAttribute('aria-label', t('a11y.grid'));
    this.acceptBtn.textContent = t('mcp.accept');
    this.agentsLabel.textContent = t('narrow.agents');
    this.menuBtn.setAttribute('aria-label', t('narrow.menu'));
    this.menuBtn.title = t('narrow.menu');
    this.moreBtn.setAttribute('aria-label', t('narrow.more'));
    this.moreBtn.title = t('narrow.moreTitle');
    this.agentBarUndo.textContent = t('narrow.agentUndo');
    this.agentBarAccept.textContent = t('narrow.agentAccept');
  }

  /** A breakpoint was crossed: the menubar, transport and MCP header are rebuilt for the new layout; open sheets, popovers and the drawer close. */
  private onMode(): void {
    const narrow = isNarrow();
    const phone = isPhone();
    if (narrow === this.narrow && phone === this.phone) return;
    this.narrow = narrow;
    this.phone = phone;
    this.closeMenus();
    this.menuSheet?.close();
    this.closeMore();
    this.closeDrawer(false);
    this.menuOpeners.clear();
    clear(this.menubar);
    this.buildMenubar();
    this.buildTransport();
    this.buildMcpPanel();
    this.tabsSignature = '';
    this.render();
  }

  //==========================================================================
  // the MCP panel as a right drawer below 960 px
  private openDrawer(): void {
    if (!this.narrow || this.mcp.classList.contains('open')) return;
    this.drawerScrim = h('div', { class: 'drawer-scrim', on: { pointerdown: () => this.closeDrawer(true) } });
    this.mcp.before(this.drawerScrim);
    this.mcp.classList.add('open');
    this.mcp.setAttribute('role', 'dialog');
    this.mcp.tabIndex = -1;
    this.agentsBtn.setAttribute('aria-expanded', 'true');
    const first = this.mcp.querySelector<HTMLElement>('.drawer-close, [tabindex="0"], button:not(:disabled)');
    (first ?? this.mcp).focus();
  }

  /** Closes the drawer; `refocus` returns the focus to the agents button. */
  private closeDrawer(refocus: boolean): void {
    this.drawerScrim?.remove();
    this.drawerScrim = null;
    const wasOpen = this.mcp.classList.contains('open');
    this.mcp.classList.remove('open');
    this.mcp.removeAttribute('role');
    this.mcp.removeAttribute('tabindex');
    this.agentsBtn.setAttribute('aria-expanded', 'false');
    if (wasOpen && refocus) this.agentsBtn.focus();
  }

  //==========================================================================
  // phone: the popover of the transport's more button (Render, grid, snap)
  private closeMore(refocus = false): void {
    this.morePop?.close(refocus);
  }

  private toggleMore(): void {
    if (this.morePop) {
      this.closeMore(true);
      return;
    }
    const render = h('button', { class: 'tbtn wide', text: t('transport.render') });
    const pop = h(
      'div',
      { class: 'popover more-pop', attrs: { role: 'dialog', 'aria-label': t('narrow.moreTitle') } },
      h('div', { class: 'row' }, render),
      h('div', { class: 'row' }, h('span', { class: 'label', text: t('transport.grid') }), this.gridSel),
      h('div', { class: 'row' }, h('span', { class: 'label', text: t('transport.snap') }), this.snapBtn),
    );
    const r = this.moreBtn.getBoundingClientRect();
    pop.style.top = `${r.bottom + 4}px`;
    pop.style.left = `${Math.max(8, r.right - 260)}px`;
    document.body.append(pop);
    this.moreBtn.setAttribute('aria-expanded', 'true');
    const onDown = (ev: Event) => {
      if (!pop.contains(ev.target as Node) && ev.target !== this.moreBtn) close(false);
    };
    const onKey = (ev: KeyboardEvent) => {
      if (ev.key !== 'Escape') return;
      ev.stopPropagation();
      close(true);
    };
    const timer = setTimeout(() => document.addEventListener('pointerdown', onDown, true), 0);
    document.addEventListener('keydown', onKey, true);
    const close = (refocus = false) => {
      clearTimeout(timer);
      document.removeEventListener('pointerdown', onDown, true);
      document.removeEventListener('keydown', onKey, true);
      pop.remove();
      this.morePop = null;
      this.moreBtn.setAttribute('aria-expanded', 'false');
      if (refocus) this.moreBtn.focus();
    };
    render.addEventListener('click', () => {
      close(false);
      openRenderDialog(this.app);
    });
    this.morePop = { el: pop, close };
    render.focus();
  }

  /** After a language switch: rebuilds every static label of the shell and its views, then renders. */
  relabel(): void {
    this.closeMenus();
    this.menuSheet?.close();
    this.closeMore();
    this.applyLabels();
    clear(this.menubar);
    this.menuOpeners.clear();
    this.buildMenubar();
    this.buildTransport();
    this.buildMcpPanel();
    this.arrange.relabel();
    this.mixer.relabel();
    this.tabsSignature = '';
    this.logSignature = '';
    this.render();
  }

  //==========================================================================
  private buildMenubar(): void {
    const app = this.app;
    this.menuModels.length = 0;
    const menu = (title: string, entries: () => MenuEntry[]) => {
      const dropdown = h('div', { class: 'dropdown', attrs: { role: 'menu', 'aria-label': title } });
      const trigger = h('button', { text: title, attrs: { 'aria-haspopup': 'menu', 'aria-expanded': 'false' } });
      const root = h('div', { class: 'menu' }, trigger, dropdown);
      const open = (focus: 'first' | 'last' | null) => {
        this.closeMenus();
        clear(dropdown);
        for (const e of entries()) dropdown.append(menuEntryEl(e, () => this.closeMenus(true)));
        root.classList.add('open');
        trigger.setAttribute('aria-expanded', 'true');
        this.openMenu = root;
        const items = this.menuItems(root);
        (focus === 'first' ? items[0] : focus === 'last' ? items[items.length - 1] : undefined)?.focus();
      };
      this.menuOpeners.set(root, open);
      this.menuModels.push({ title, entries });
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
      menu(t('menu.file'), () => [
        { label: t('menu.file.new'), action: () => void this.promptNewProject() },
        { label: t('menu.file.open'), action: () => openProjectDialog(app) },
        { label: t('import.menu'), key: 'Ctrl+I', action: () => importController(app).openDialog() },
        { label: t('menu.file.save'), key: 'Ctrl+S', disabled: !p(), action: () => void app.cmd('project.save') },
        { separator: true, label: '' },
        { label: t('menu.file.render'), disabled: !p(), action: () => openRenderDialog(app) },
        { separator: true, label: '' },
        { label: t('menu.file.close'), disabled: !p(), action: () => p() && void app.closeProject(p()!.id) },
      ]),
      menu(t('menu.edit'), () => [
        { label: t('menu.edit.undo'), key: 'Ctrl+Z', disabled: !p()?.canUndo, action: () => void app.undo() },
        { label: t('menu.edit.redo'), key: 'Ctrl+Shift+Z', disabled: !p()?.canRedo, action: () => void app.redo() },
        { separator: true, label: '' },
        { label: t('menu.edit.split'), key: 'S', action: () => this.arrange.splitAtPlayhead() },
        { label: t('menu.edit.delete'), key: 'Del', action: () => this.arrange.deleteSelection() },
      ]),
      menu(t('menu.view'), () => [
        { label: app.prefs.mixerOpen ? t('menu.view.mixerHide') : t('menu.view.mixerShow'), key: 'Ctrl+M', action: () => app.updatePrefs({ mixerOpen: !app.prefs.mixerOpen }) },
        { label: t('menu.view.zoomIn'), key: '+', action: () => this.arrange.zoomBy(1.5) },
        { label: t('menu.view.zoomOut'), key: '-', action: () => this.arrange.zoomBy(1 / 1.5) },
        { label: t('menu.view.fit'), key: 'Ctrl+0', action: () => this.arrange.fit() },
        { separator: true, label: '' },
        { label: app.prefs.theme === 'dark' ? t('menu.view.themeLight') : t('menu.view.themeDark'), action: () => app.updatePrefs({ theme: app.prefs.theme === 'dark' ? 'light' : 'dark' }) },
        { separator: true, label: '' },
        { heading: true, label: t('menu.view.language') },
        ...availableLocales.map((l) => ({ label: l.name, checked: l.code === getLocale(), action: () => setLocale(l.code) })),
      ]),
      menu(t('menu.track'), () => [
        { label: t('menu.track.new'), disabled: !p(), action: () => void app.edit('track.create', { name: t('tracks.defaultName', { n: (p()?.tracks.length ?? 0) + 1 }) }, { refetch: true }) },
        { label: t('menu.track.delete'), disabled: !p()?.selectedTrack, action: () => p()?.selectedTrack && void app.edit('track.delete', { track_id: p()!.selectedTrack }, { refetch: true }) },
      ]),
      menu(t('menu.agents'), () => [
        { label: t('menu.agents.undo'), disabled: !p() || p()!.agentUndo().count === 0, action: () => void app.undoAgent() },
        { label: t('menu.agents.accept'), disabled: !p() || p()!.marks.size === 0, action: () => void app.acceptAgent() },
      ]),
      menu(t('menu.help'), () => [
        { label: t('menu.help.source'), action: () => window.open(app.serverInfo.source_url || '/source', '_blank', 'noopener') },
        { label: `Rovox ${app.serverInfo.version}`, disabled: true },
      ]),
      h('span', { class: 'spacer' }),
      this.status,
    );
    this.adaptMenubar();
  }

  /** Below 960 px the menubar gains the agents button; on a phone the menus move into the sheet of the menu button and the status text goes. */
  private adaptMenubar(): void {
    if (this.phone) {
      for (const m of [...this.menubar.children]) if (m.classList.contains('menu')) m.remove();
      this.status.remove();
      this.menubar.querySelector('.logo')?.after(this.menuBtn);
    }
    if (this.narrow) this.menubar.append(this.agentsBtn);
  }

  /** The menu sheet: the menus of the menubar as sections (same entries, same markup); arrows / Home / End move between the items. */
  private openMenuSheet(): void {
    this.closeMenus();
    this.closeMore();
    if (this.menuSheet) {
      this.menuSheet.close();
      return;
    }
    const root = h('div', { class: 'menu open menu-sheet' });
    let sheet: Sheet | null = null;
    for (const m of this.menuModels) {
      const dropdown = h('div', { class: 'dropdown', attrs: { role: 'menu', 'aria-label': m.title } }, h('div', { class: 'heading', text: m.title, attrs: { role: 'presentation' } }));
      for (const e of m.entries()) dropdown.append(menuEntryEl(e, () => sheet?.close()));
      root.append(dropdown);
    }
    root.addEventListener('keydown', (e) => {
      const items = this.menuItems(root);
      const at = items.indexOf(document.activeElement as HTMLButtonElement);
      if (e.key === 'ArrowDown') items[(at + 1) % items.length]?.focus();
      else if (e.key === 'ArrowUp') items[(at <= 0 ? items.length : at) - 1]?.focus();
      else if (e.key === 'Home') items[0]?.focus();
      else if (e.key === 'End') items[items.length - 1]?.focus();
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
    this.menuBtn.setAttribute('aria-expanded', 'true');
    sheet = openSheet(t('narrow.menu'), root, () => {
      this.menuSheet = null;
      this.menuBtn.setAttribute('aria-expanded', 'false');
    });
    this.menuSheet = sheet;
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
    // the buttons below are rebuilt on a language switch; these three live on, so they are wired once
    if (!this.transportWired) {
      this.transportWired = true;
      this.snapBtn.addEventListener('click', () => app.updatePrefs({ snap: !app.prefs.snap }));
      this.gridSel.addEventListener('change', () => app.updatePrefs({ grid: this.gridSel.value as (typeof GRIDS)[number] }));
      this.audioBtn.addEventListener('click', () => void app.enableAudio());
    }
    clear(this.transport);
    if (this.phone) {
      this.buildTransportPhone(mk);
      return;
    }
    this.transport.append(
      mk('start', ICONS.start, t('transport.start'), '', () => void app.toStart()),
      mk('stop', ICONS.stop, t('transport.stop'), '', () => void this.stop()),
      mk('play', ICONS.play, t('transport.play'), 'play', () => void this.play()),
      mk('pause', ICONS.pause, t('transport.pause'), '', () => void this.stop()),
      mk('rec', ICONS.rec, t('transport.rec'), 'rec', () => undefined),
      mk('loop', ICONS.loop, t('transport.loop'), 'loop', () => app.toggleLoop()),
      h('div', { class: 'clock' }, this.clockBig, this.clockSmall),
      h('span', { class: 'readout muted', text: t('transport.bpm') }),
      this.bpmEl,
      this.sigEl,
      h('span', { class: 'spacer' }),
      this.audioBtn,
      h('button', { class: 'tbtn wide', text: t('transport.render'), on: { click: () => openRenderDialog(app) } }),
      h('span', { class: 'label', text: t('transport.grid') }),
      this.gridSel,
      h('span', { class: 'label', text: t('transport.snap') }),
      this.snapBtn,
    );
    this.btn['rec']!.disabled = true;
  }

  /** Two rows: transport buttons and the clock, then tempo, the sound button and the more button (Render, grid and snap sit in its popover). */
  private buildTransportPhone(mk: (key: string, svg: string, label: string, cls: string, fn: () => void) => HTMLButtonElement): void {
    const app = this.app;
    const start = mk('start', ICONS.start, t('transport.start'), '', () => void app.toStart());
    const stop = mk('stop', ICONS.stop, t('transport.stop'), '', () => void this.stop());
    const play = mk('play', ICONS.play, t('transport.play'), 'play', () => void this.play());
    mk('pause', ICONS.pause, t('transport.pause'), '', () => void this.stop());
    const rec = mk('rec', ICONS.rec, t('transport.rec'), 'rec', () => undefined);
    const loop = mk('loop', ICONS.loop, t('transport.loop'), 'loop', () => app.toggleLoop());
    rec.disabled = true;
    this.transport.append(
      h('div', { class: 'trow' }, start, play, stop, loop, h('div', { class: 'clock' }, this.clockBig, this.clockSmall)),
      h('div', { class: 'trow' }, h('span', { class: 'readout muted', text: t('transport.bpm') }), this.bpmEl, this.sigEl, h('span', { class: 'spacer' }), this.audioBtn, this.moreBtn),
    );
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
    app.toast(t('transport.preview.start'));
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
      app.toast(t('transport.preview.failed'), 'error', errorDetail(r.error));
      return;
    }
    const audio = new Audio(`/api/projects/${encodeURIComponent(p.id)}/${clip.path}`);
    this.previewAudio = audio;
    audio.addEventListener('ended', () => {
      if (this.previewAudio === audio) this.previewAudio = null;
    });
    await audio.play().catch(() => app.toast(t('transport.preview.blocked'), 'error'));
  }

  private buildMcpPanel(): void {
    const app = this.app;
    if (!this.mcpWired) {
      this.mcpWired = true;
      this.undoAgentBtn.addEventListener('click', () => void app.undoAgent());
      this.acceptBtn.addEventListener('click', () => void app.acceptAgent());
    }
    clear(this.mcp);
    this.mcp.append(
      h(
        'div',
        { class: 'mcp-head' },
        h('span', { class: 'label', text: t('mcp.head') }),
        this.connEl,
        this.narrow ? h('button', { class: 'drawer-close', text: '×', title: t('narrow.agentsClose'), attrs: { 'aria-label': t('narrow.agentsClose') }, on: { click: () => this.closeDrawer(true) } }) : null,
      ),
      this.agentsBox,
      this.activityTitle,
      this.logBox,
      h('div', { class: 'mcp-actions' }, this.undoAgentBtn, this.acceptBtn),
    );
  }

  //==========================================================================
  private onKey(e: KeyboardEvent): void {
    const target = e.target as HTMLElement;
    if (e.key === 'Escape' && this.mcp.classList.contains('open') && !document.querySelector('.scrim, .sheet-scrim')) {
      e.preventDefault();
      this.closeDrawer(true);
      return;
    }
    if (target.closest('input, textarea, select') || document.querySelector('.scrim, .sheet-scrim')) return;
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
    } else if (mod && e.key.toLowerCase() === 'i') {
      e.preventDefault();
      importController(app).openDialog();
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
    this.banner.textContent = app.status === 'connecting' ? t('banner.connecting') : t('banner.lost');

    // status
    const a = app.audio.stats;
    const audioPart = app.serverInfo.audio.mode === 'none' ? t('status.audioNone') : a.enabled ? t('status.audioOn') : t('status.audioOff');
    this.status.textContent = t('status.line', { audio: audioPart, version: app.serverInfo.version });

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
    this.tabs.append(this.tablist);
    const tabIds: string[] = [];
    // the tab that is in the Tab order: the one last moved to, else the active one, else the first
    const rovingId = [this.tabFocusId, app.activeId, app.order[0]].find((x) => x !== undefined && app.projects.has(x));
    for (const id of app.order) {
      const p = app.projects.get(id);
      if (!p) continue;
      const st = app.tabStatus(p);
      // an idle project has no label (the dot says it); an unsaved one says "змінено"
      const label = st === 'idle' ? (p.dirty ? t('tabs.state.edited') : '') : t(`tabs.state.${st}`);
      const muteTitle = p.muted ? t('tabs.unmute') : t('tabs.mute');
      const tabName = p.name || id;
      // a sibling of the tab, not a child: no interactive element inside role=tab
      const muteButton = h(
        'button',
        {
          class: `mute${p.muted ? ' on' : ''}`,
          title: muteTitle,
          attrs: { role: 'switch', 'aria-label': t('tabs.soundOf', { name: tabName }), 'aria-checked': String(!p.muted), 'data-tab-focus': `mute:${id}` },
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
        { class: 'tab', id: `rx-tab-${id}`, attrs: { role: 'tab', 'aria-selected': String(id === app.activeId), tabindex: id === rovingId ? '0' : '-1', 'data-tab-focus': `tab:${id}` } },
        h('span', { class: 'name', text: p.name || id, title: p.name }),
        h('span', { class: 'state', text: label }),
        p.dirty ? h('span', { class: 'dirty', text: '●', title: t('tabs.dirty') }) : null,
        syncBadge(p),
      );
      // a sibling of the tab like the mute button: no interactive element inside role=tab
      const closeButton = h('button', { class: 'close', text: '×', title: t('tabs.close'), attrs: { 'aria-label': t('tabs.close'), 'data-tab-focus': `close:${id}` }, on: { click: (e) => { e.stopPropagation(); void app.closeProject(id); } } });
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
      tabIds.push(`rx-tab-${id}`);
      this.tabs.append(item);
    }
    this.tablist.setAttribute('aria-owns', tabIds.join(' '));
    this.tabs.append(h('button', { class: 'add', text: '+', title: t('tabs.new'), attrs: { 'aria-label': t('tabs.new'), 'data-tab-focus': 'add' }, on: { click: () => void this.promptNewProject() } }));
    // the tabs are rebuilt on every change: keep the keyboard focus on the same control
    if (focusKey) this.tabs.querySelector<HTMLElement>(`[data-tab-focus="${focusKey}"]`)?.focus();
    if (this.phone) this.revealActiveTab();
  }

  /** On a phone the tabs scroll sideways: the active one is kept in view. */
  private revealActiveTab(): void {
    const active = this.tabs.querySelector<HTMLElement>('.tabitem.active');
    if (!active) return;
    const a = active.getBoundingClientRect();
    const box = this.tabs.getBoundingClientRect();
    if (a.left < box.left) this.tabs.scrollLeft += a.left - box.left - 8;
    else if (a.right > box.right) this.tabs.scrollLeft += a.right - box.right + 8;
  }

  private async promptNewProject(): Promise<void> {
    const untitled = t('project.untitled');
    const name = await textDialog({ title: t('project.new.title'), label: t('project.new.label'), value: untitled, okText: t('project.new.ok') });
    if (name !== null) await this.app.newProject(name.trim() || untitled);
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
    this.snapBtn.textContent = app.prefs.snap ? t('transport.snapOn') : t('transport.snapOff');
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
      setText(this.audioBtn, t('transport.audio.none'));
      this.audioBtn.disabled = true;
      setTitle(this.audioBtn, t('transport.audio.noneHint'));
    } else if (!a.supported) {
      setText(this.audioBtn, t('transport.audio.unsupported'));
      this.audioBtn.disabled = true;
      setTitle(this.audioBtn, a.error ?? t('transport.audio.needBrowser'));
    } else {
      this.audioBtn.disabled = false;
      setText(this.audioBtn, a.enabled ? (a.underruns > 0 && playing ? plural(a.underruns, 'transport.audio.underruns') : t('transport.audio.on')) : t('transport.audio.enable'));
      setTitle(this.audioBtn, a.enabled ? t('transport.audio.stats', { underruns: a.underruns, buffer: Math.round(a.bufferedMs), latency: Math.round(a.outputLatencyMs) }) : t('transport.audio.enableHint'));
    }
  }

  private renderMcp(p: ReturnType<App['projects']['get']>): void {
    const app = this.app;
    this.connEl.className = `conn${app.status === 'open' ? '' : ' offline'}`;
    clear(this.connEl);
    this.connEl.append(h('i'), document.createTextNode(app.status === 'open' ? location.host : t('mcp.offline')));

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
    if (!live.length) this.agentsBox.append(h('div', { class: 'none', text: t('mcp.noAgents') }));
    for (const a of live.slice(0, 4)) {
      const active = Date.now() - a.at < 20000;
      this.agentsBox.append(h('div', { class: `agent-row${active ? ' active' : ''}` }, h('i'), h('b', { text: a.label }), h('span', { text: [...a.projects].join(' · '), title: [...a.projects].join(', ') })));
    }

    const anyActive = live.some((a) => Date.now() - a.at < 20000);
    this.agentsBtn.classList.toggle('active', anyActive);
    this.agentsBtn.title = anyActive ? t('narrow.agentsActive') : t('narrow.agents');
    this.agentsBtn.setAttribute('aria-label', anyActive ? t('narrow.agentsActive') : t('narrow.agents'));

    this.activityTitle.textContent = p ? t('mcp.activity', { name: p.name }) : t('mcp.activityEmpty');
    const sig = p ? `${p.id}|${p.log.length}|${p.log[0]?.revision ?? 0}|${p.log[0]?.at ?? 0}` : '';
    if (sig !== this.logSignature) {
      this.logSignature = sig;
      clear(this.logBox);
      for (const l of (p?.log ?? []).slice(0, 60)) {
        const agent = isAgent(l.origin);
        const row = h(
          'div',
          { class: 'log-row', attrs: { role: 'button', tabindex: '0' } },
          h('div', { class: 'meta' }, h('span', { text: fmtDate(new Date(l.at), { hour: '2-digit', minute: '2-digit' }) }), h('span', { class: `tool${agent ? ' agent' : ''}`, text: l.tool })),
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
      if (!p?.log.length) this.logBox.append(h('div', { class: 'agents' }, h('div', { class: 'none', text: t('mcp.noActions') })));
    }

    const u = p?.agentUndo() ?? { count: 0, blockedByPerson: false };
    this.undoAgentBtn.textContent = u.count > 0 ? t('mcp.undoAgentN', { n: u.count }) : t('mcp.undoAgent');
    this.undoAgentBtn.disabled = u.count === 0;
    this.undoAgentBtn.title = u.count > 0 ? t('mcp.undoHint') : u.blockedByPerson ? t('mcp.undoBlocked') : t('mcp.undoNone');
    this.acceptBtn.disabled = !p || p.marks.size === 0;
    this.acceptBtn.title = t('mcp.acceptHint');
    this.renderAgentBar(p, u.count);
  }

  /** Phone: a bar above the sheets while the agent has changes the person has not accepted: «Агент змінив N доріжок · Скасувати / Прийняти». */
  private renderAgentBar(p: ReturnType<App['projects']['get']>, undoable: number): void {
    const root = document.documentElement.style;
    if (!this.phone || !p || p.marks.size === 0) {
      if (this.agentBar.isConnected) this.agentBar.remove();
      root.removeProperty('--rx-bar-h');
      return;
    }
    const tracks = new Set<string>();
    for (const tr of p.tracks) if (p.trackMark(tr.track_id)) tracks.add(tr.track_id);
    for (const it of p.items.values()) if (p.marks.has(it.item_id)) tracks.add(it.track_id);
    setText(this.agentBarText, plural(Math.max(1, tracks.size), 'narrow.agentChanged'));
    this.agentBarUndo.disabled = undoable === 0;
    this.agentBarUndo.title = undoable > 0 ? t('mcp.undoHint') : t('mcp.undoNone');
    this.agentBarAccept.title = t('mcp.acceptHint');
    if (!this.agentBar.isConnected) this.el.append(this.agentBar);
    root.setProperty('--rx-bar-h', '48px');
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
      // the raw server message stays one line below the plain text (and in the tooltip)
      if (t.detail && t.detail !== t.text) {
        el.title = t.detail;
        el.append(h('div', { class: 'detail', text: t.detail }));
      }
      this.toastEls.set(t.id, el);
      this.toasts.append(el);
    }
  }
}

/** The login form shown when the server wants a token. */
export function loginView(app: App): HTMLElement {
  const input = h('input', { attrs: { type: 'password', placeholder: t('login.token'), 'aria-label': t('login.token'), autocomplete: 'off' } });
  const err = h('div', { class: 'err' });
  const go = async () => {
    err.textContent = '';
    if (!(await app.login(input.value.trim()))) err.textContent = t('login.invalid');
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
      h('div', { text: t('login.prompt') }),
      input,
      err,
      h('button', { class: 'btn primary', text: t('login.submit'), on: { click: () => void go() } }),
    ),
  );
}
