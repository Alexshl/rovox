// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The upload and import wizard: 1/3 pick files or a folder (or drop them), 2/3 upload with progress, pause and resume, 3/3 review the
// import plan (names editable) and import. One or more loose audio files without warnings skip step 3 and are applied at once, with a
// toast that offers Undo. A REAPER .RPP has no plan step: it is applied and a result dialog lists what was carried over.
import { errorText, fmtBytes, fmtDuration, fmtNumber, hasKey, plural, t, type MessageKey } from '../i18n';
import { clear, h } from '../lib/dom';
import { FILE_ACCEPT, type Flattened, collectDrop, dragSummary, flattenFiles, folderPickerSupported, hasFiles } from '../lib/dropfiles';
import { DiskFullError, TusError, TusUploader, type SavedBatch, type TusFile, dropSaved, loadSaved } from '../net/tus';
import type { App } from '../store/session';
import { type Modal, openModal } from './dialog';

/** What the wizard needs of the app (a fake in tests). */
export type ImportHost = Pick<App, 'cmd' | 'edit' | 'active' | 'activeId' | 'toast' | 'listProjects' | 'setActive' | 'undo'> & {
  /** `upload.finish` is an MCP tool (it is not on the WebSocket command bus), so the wizard reaches it the way the render dialog reaches its tools. */
  mcp: Pick<App['mcp'], 'call'>;
};

export interface ImportDeps {
  fetchImpl?: typeof fetch;
  storage?: Storage | null;
  sleep?: (ms: number) => Promise<void>;
  chunkBytes?: number;
}

interface PlanClip {
  file?: string;
  channels: number;
  sample_rate: number;
  bit_depth: number;
  duration_s: number;
}
interface PlanTrack {
  name: string;
  source_key: string;
  color: string;
  role_guess: string;
  clips: PlanClip[];
}
interface PlanWarning {
  code: string;
  message: string;
  file?: string;
}
interface Plan {
  plan_id: string;
  detected_source: string;
  tracks: PlanTrack[];
  warnings: PlanWarning[];
}
interface FinishResult {
  path: string;
  files: { relpath: string; bytes: number; sha256?: string }[];
  detected: string;
}
interface RppReport {
  tracks?: number;
  items?: number;
  mapped?: Record<string, unknown>[];
  unsupported?: Record<string, unknown>[];
  missing_media?: Record<string, unknown>[];
}

type Step = 'pick' | 'upload' | 'plan' | 'rpp';
type Align = 'start' | 'bwf' | 'none';

//==============================================================================
// formatting
function fmtPct(p: number): string {
  return fmtNumber(Math.max(0, Math.min(100, p)) / 100, { style: 'percent', maximumFractionDigits: 0 });
}

function unitDivisor(total: number): number {
  let d = 1;
  for (let i = 0; i < 3 && total / d >= 1024; i++) d *= 1024;
  return d;
}

/** "2,1 з 4,8 ГБ": the done part as a bare number in the unit of the total. */
export function progressText(done: number, total: number): string {
  const d = unitDivisor(total);
  const digits = d === 1 ? 0 : 1;
  const num = fmtNumber(done / d, { minimumFractionDigits: digits, maximumFractionDigits: digits });
  return t('import.bytesOf', { done: num, total: fmtBytes(total) });
}

export function fmtEta(sec: number): string {
  const s = Math.max(1, Math.round(sec));
  if (s < 60) return t('import.etaSec', { n: s });
  const min = Math.ceil(s / 60);
  if (min < 60) return t('import.etaMin', { n: min });
  return t('import.etaHour', { h: Math.floor(min / 60), m: min % 60 });
}

function planMeta(track: PlanTrack): string {
  const c = track.clips[0];
  if (!c) return '';
  const ch = c.channels === 1 ? t('import.ch.1') : c.channels === 2 ? t('import.ch.2') : t('import.ch.n', { n: c.channels });
  const dur = Math.max(0, ...track.clips.map((x) => x.duration_s));
  return [ch, t('import.rate', { n: fmtNumber(c.sample_rate / 1000, { maximumFractionDigits: 1 }) }), `${c.bit_depth} ${t('unit.bit')}`, fmtDuration(dur)].join(' ');
}

function projectNameOf(flat: Flattened): string {
  return flat.name.replace(/\.(zprj|rpp|wav|aiff?|flac|mp3)$/i, '').trim();
}

//==============================================================================
// toasts that stay (the shared toast stack of the shell, or our own when there is none)
interface ToastAction {
  label: string;
  run: () => void;
}

function toastHost(): HTMLElement {
  let host = document.querySelector<HTMLElement>('.toasts');
  if (!host) {
    host = h('div', { class: 'toasts', attrs: { role: 'status', 'aria-live': 'polite' } });
    document.body.append(host);
  }
  return host;
}

/** A toast with buttons; `timeoutMs` 0 keeps it until `remove()`. */
function actionToast(text: string, actions: ToastAction[], timeoutMs = 0): { el: HTMLElement; remove: () => void } {
  const el = h('div', { class: 'toast action' }, h('span', { class: 'text', text }));
  let timer: ReturnType<typeof setTimeout> | undefined;
  const remove = () => {
    if (timer) clearTimeout(timer);
    el.remove();
  };
  for (const a of actions) {
    el.append(
      h('button', {
        class: 'btn',
        text: a.label,
        on: {
          click: () => {
            remove();
            a.run();
          },
        },
      }),
    );
  }
  toastHost().append(el);
  if (timeoutMs > 0) timer = setTimeout(remove, timeoutMs);
  return { el, remove };
}

//==============================================================================
// drop targets
export interface DropTargetOptions {
  onDrop: (dt: DataTransfer) => void;
  /** Called when a file drag enters (true, with what it carries) and leaves or drops (false). */
  onActive?: (active: boolean, dt: DataTransfer) => void;
}

/** Makes `el` a drop target for files (also folders). Returns the function that removes the listeners. */
export function attachDropTarget(el: HTMLElement, o: DropTargetOptions): () => void {
  let depth = 0;
  const set = (active: boolean, dt: DataTransfer) => o.onActive?.(active, dt);
  const enter = (e: DragEvent) => {
    if (!hasFiles(e.dataTransfer)) return;
    e.preventDefault();
    if (depth++ === 0) set(true, e.dataTransfer!);
  };
  const over = (e: DragEvent) => {
    if (!hasFiles(e.dataTransfer)) return;
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
  };
  const leave = (e: DragEvent) => {
    if (!hasFiles(e.dataTransfer) || depth === 0) return;
    // dragleave also fires when the pointer moves onto a child: only leaving the area itself counts
    const to = e.relatedTarget as Node | null;
    depth = to && !el.contains(to) ? 0 : depth - 1;
    if (depth === 0) set(false, e.dataTransfer!);
  };
  const drop = (e: DragEvent) => {
    if (!hasFiles(e.dataTransfer)) return;
    e.preventDefault();
    depth = 0;
    set(false, e.dataTransfer!);
    o.onDrop(e.dataTransfer!);
  };
  el.addEventListener('dragenter', enter);
  el.addEventListener('dragover', over);
  el.addEventListener('dragleave', leave);
  el.addEventListener('drop', drop);
  return () => {
    el.removeEventListener('dragenter', enter);
    el.removeEventListener('dragover', over);
    el.removeEventListener('dragleave', leave);
    el.removeEventListener('drop', drop);
  };
}

//==============================================================================
export interface BeginOptions {
  /** Always import into a new project (a drop on the project picker). */
  forceNew?: boolean;
  resume?: SavedBatch;
}

export class ImportController {
  private wizard: Wizard | null = null;
  private live: HTMLElement | null = null;
  private unloadWired = false;
  private resumeChecked = false;

  constructor(
    readonly app: ImportHost,
    readonly deps: ImportDeps = {},
  ) {}

  /** The wizard is open (or collapsed into the pill). */
  get busy(): boolean {
    return this.wizard !== null;
  }

  /** True while bytes are moving: the page asks before it is closed. */
  isUploading(): boolean {
    const u = this.wizard?.uploader;
    return !!u && !u.cancelled && u.files.some((f) => f.state === 'uploading' || (f.state === 'queued' && !u.paused));
  }

  wireUnload(): void {
    if (this.unloadWired || typeof window === 'undefined') return;
    this.unloadWired = true;
    window.addEventListener('beforeunload', (e) => {
      if (!this.isUploading()) return;
      e.preventDefault();
      e.returnValue = t('import.leave');
    });
  }

  announce(text: string): void {
    if (!this.live) {
      this.live = h('div', { class: 'sr-only', attrs: { 'aria-live': 'polite', role: 'status' } });
      document.body.append(this.live);
    }
    // a new text node each time so that a repeated message is read again
    this.live.textContent = '';
    this.live.append(document.createTextNode(text));
  }

  /** File menu / Ctrl+I: the picking step, or back to the running wizard. */
  openDialog(): void {
    if (this.wizard) {
      this.wizard.expand();
      return;
    }
    this.wizard = new Wizard(this);
    this.wizard.showPick();
  }

  /** [Вибрати файли] / [Вибрати теку] outside the dialog (the empty-project card). */
  pickFiles(opts: BeginOptions = {}): void {
    this.pick(false, opts);
  }

  pickFolder(opts: BeginOptions = {}): void {
    this.pick(true, opts);
  }

  pick(folder: boolean, opts: BeginOptions = {}): void {
    const input = h('input', { attrs: { type: 'file', multiple: '', ...(folder ? { webkitdirectory: '' } : { accept: FILE_ACCEPT }) } });
    input.addEventListener('change', () => {
      if (input.files?.length) this.begin(flattenFiles(input.files), opts);
    });
    input.click();
  }

  /** A drop (or any DataTransfer with files): read it now, then start. */
  handleDrop(dt: DataTransfer, opts: BeginOptions = {}): void {
    const flat = collectDrop(dt);
    void flat.then((f) => this.begin(f, opts));
  }

  begin(flat: Flattened, opts: BeginOptions = {}): void {
    if (this.wizard && !this.wizard.canAcceptFiles()) {
      this.app.toast(t('import.busy'), 'error');
      return;
    }
    this.wizard ??= new Wizard(this);
    this.wizard.accept(flat, opts);
  }

  /** Makes an area (the arrange view, a dialog) accept dropped files, with the overlay card while a drag is over it. */
  attachArea(el: HTMLElement, opts: BeginOptions & { overlay?: boolean; onDropped?: () => void } = {}): () => void {
    const { overlay = true, onDropped, ...begin } = opts;
    return attachDropTarget(el, {
      onActive: (active, dt) => {
        if (!overlay) return;
        el.classList.toggle('dragover', active);
        if (active) {
          const s = dragSummary(dt);
          const parts = [s.files ? plural(s.files, 'import.nFiles') : '', s.folders ? plural(s.folders, 'import.nFolders') : ''].filter(Boolean);
          el.dataset['dropLabel'] = t('import.overlay', { what: parts.join(', ') });
        }
      },
      onDrop: (dt) => {
        this.handleDrop(dt, begin);
        onDropped?.();
      },
    });
  }

  //---------------------------------------------------------------- resume after a reload
  /** On the first connect after a load: offer to continue an upload that was cut off. */
  checkResume(): void {
    if (this.resumeChecked) return;
    this.resumeChecked = true;
    const saved = loadSaved(this.deps.storage).find((b) => b.files.some((f) => !f.done));
    if (!saved) return;
    const done = saved.files.filter((f) => f.done).length;
    actionToast(plural(saved.files.length, 'import.resume.text', { done }), [
      { label: t('import.resume.continue'), run: () => this.resume(saved) },
      { label: t('import.resume.discard'), run: () => this.discard(saved) },
    ]);
  }

  private resume(saved: SavedBatch): void {
    if (!this.wizard) this.wizard = new Wizard(this);
    this.wizard.showPick({ resume: saved });
  }

  private discard(saved: SavedBatch): void {
    const f = this.deps.fetchImpl ?? ((...a: Parameters<typeof fetch>) => fetch(...a));
    for (const file of saved.files) if (!file.done) void Promise.resolve(f(file.url, { method: 'DELETE', headers: { 'Tus-Resumable': '1.0.0' }, credentials: 'same-origin' })).catch(() => undefined);
    dropSaved(saved.batch_id, this.deps.storage);
  }

  /** The wizard reports that it is gone. */
  release(w: Wizard): void {
    if (this.wizard === w) this.wizard = null;
  }

  /** Scrolls the new tracks into view and flashes them for a second (they appear after the project refetch). */
  highlightTracks(ids: string[]): void {
    let tries = 0;
    const tick = () => {
      if (typeof document === 'undefined') return;
      const rows = [...document.querySelectorAll<HTMLElement>('.tcp[data-track-id]')].filter((r) => ids.includes(r.dataset['trackId'] ?? ''));
      if (!rows.length) {
        if (++tries < 20) setTimeout(tick, 150);
        return;
      }
      rows[0]!.scrollIntoView?.({ block: 'nearest' });
      for (const r of rows) r.classList.add('flash');
      setTimeout(() => rows.forEach((r) => r.classList.remove('flash')), 1000);
    };
    tick();
  }

  successToast(tracks: number): void {
    actionToast(plural(tracks, 'import.done'), [{ label: t('menu.edit.undo'), run: () => void this.app.undo() }], 10000);
  }
}

const controllers = new WeakMap<object, ImportController>();

/** The one controller of an app (the menu, the arrange view and the picker share it). */
export function importController(app: App): ImportController {
  let c = controllers.get(app);
  if (!c) {
    c = new ImportController(app);
    controllers.set(app, c);
  }
  return c;
}

//==============================================================================
interface Row {
  el: HTMLElement;
  icon: HTMLElement;
  status: HTMLElement;
  action: HTMLElement;
  bar?: HTMLElement;
  pct?: HTMLElement;
  sig: string;
}

class Wizard {
  step: Step = 'pick';
  uploader: TusUploader | null = null;
  private closed = false;
  private collapsed = false;
  private flat: Flattened | null = null;
  private forceNew = false;
  private projectId = '';
  private createdProject = false;
  private target: 'current' | 'new' = 'current';
  private align: Align = 'start';
  private plan: Plan | null = null;
  private finish: FinishResult | null = null;
  private names = new Map<string, string>();
  private hadErrors = false;
  private advancing = false;
  private lastMilestone = 0;
  private lastStates = new Map<string, string>();
  private startedAt = 0;
  private startBytes = 0;
  private resumeFrom: SavedBatch | null = null;
  private readonly modal: Modal;
  private readonly dialog: HTMLElement;
  private readonly titleEl = h('span', { class: 'title' });
  private readonly stepEl = h('span', { class: 'step mono' });
  private readonly header = h('header', {}, this.titleEl, this.stepEl);
  private readonly body = h('div', { class: 'body' });
  private readonly footer = h('footer', {});
  private pill: HTMLElement | null = null;
  private pillBar: HTMLElement | null = null;
  private pillText: HTMLElement | null = null;
  private rows = new Map<string, Row>();
  private totalBar: HTMLElement | null = null;
  private totalEl: HTMLElement | null = null;
  private totalText: HTMLElement | null = null;
  private offlineEl: HTMLElement | null = null;
  private pauseBtn: HTMLButtonElement | null = null;
  private nextBtn: HTMLButtonElement | null = null;
  private dropzone: HTMLElement | null = null;
  private removeDrop: () => void;

  constructor(private readonly ctl: ImportController) {
    this.dialog = h('div', { class: 'dialog import-dialog' }, this.header, this.body, this.footer);
    this.modal = openModal(t('import.title'), this.dialog, () => this.onModalClosed(), { onEscape: () => this.escape() });
    // a drop that misses the zone must not make the browser open the file; the zone lights up only on the picking step
    this.removeDrop = attachDropTarget(this.modal.scrim, {
      onActive: (active) => this.dropzone?.classList.toggle('dragover', active),
      onDrop: (dt) => {
        if (this.step === 'pick') ctl.handleDrop(dt, this.options());
        else ctl.app.toast(t('import.busy'), 'error');
      },
    });
  }

  private get app(): ImportHost {
    return this.ctl.app;
  }

  private options(): BeginOptions {
    return { ...(this.forceNew ? { forceNew: true } : {}), ...(this.resumeFrom ? { resume: this.resumeFrom } : {}) };
  }

  //---------------------------------------------------------------- lifecycle
  canAcceptFiles(): boolean {
    return this.step === 'pick';
  }

  /** Esc: steps 1 and 3 close; while a batch is uploading the dialog collapses into the pill (only [Скасувати] stops an upload). */
  private escape(): void {
    if (this.step === 'upload' && this.uploader?.batchId) this.collapse();
    else this.close();
  }

  close(): void {
    this.modal.close();
  }

  private onModalClosed(): void {
    if (this.closed) return;
    this.closed = true;
    this.removeDrop();
    this.pill?.remove();
    this.uploader?.dispose();
    this.ctl.release(this);
  }

  private setView(step: Step, title: string, counter: string, body: (Node | null | false)[], footer: Node[]): void {
    this.step = step;
    this.titleEl.textContent = title;
    this.stepEl.textContent = counter;
    this.stepEl.hidden = !counter;
    clear(this.body);
    clear(this.footer);
    this.rows.clear();
    this.dropzone = null;
    this.totalBar = this.totalEl = this.totalText = this.offlineEl = this.pauseBtn = this.nextBtn = null;
    for (const n of body) if (n) this.body.append(n);
    for (const n of footer) this.footer.append(n);
  }

  private button(text: string, onClick: () => void, cls = 'btn'): HTMLButtonElement {
    return h('button', { class: cls, text, attrs: { type: 'button' }, on: { click: onClick } });
  }

  private alert(text: string, kind: 'error' | 'warning' | 'ok' | '' = 'error'): HTMLElement {
    return h('div', { class: `finding ${kind}`.trim(), text, attrs: kind === 'error' ? { role: 'alert' } : {} });
  }

  //---------------------------------------------------------------- step 1: pick
  showPick(state: { nothing?: boolean; skipped?: number; resume?: SavedBatch } = {}): void {
    this.resumeFrom = state.resume ?? this.resumeFrom;
    const fileInput = h('input', { attrs: { type: 'file', multiple: '', accept: FILE_ACCEPT, hidden: '' } });
    const dirInput = h('input', { attrs: { type: 'file', multiple: '', webkitdirectory: '', hidden: '' } });
    const onPicked = (input: HTMLInputElement) => () => {
      if (input.files?.length) this.accept(flattenFiles(input.files), this.options());
    };
    fileInput.addEventListener('change', onPicked(fileInput));
    dirInput.addEventListener('change', onPicked(dirInput));
    const files = this.button(state.nothing ? t('import.pickOther') : t('import.pickFiles'), () => fileInput.click(), 'btn primary');
    const folder = folderPickerSupported() ? this.button(t('import.pickFolder'), () => dirInput.click()) : null;
    this.dropzone = h(
      'div',
      { class: 'dropzone' },
      h('div', { text: t('import.drop') }),
      h('div', { class: 'label', text: t('import.formats') }),
      h('div', { class: 'buttons' }, files, folder, fileInput, dirInput),
    );
    const notes: HTMLElement[] = [];
    if (state.resume) notes.push(h('div', { class: 'finding warning', text: t('import.resume.pick') }));
    if (state.nothing) notes.push(this.alert(t('import.nothing')));
    if (state.skipped) notes.push(h('div', { class: 'finding', text: plural(state.skipped, 'import.skipped') }));
    this.setView('pick', t('import.title'), t('import.step', { n: 1 }), [h('div', { class: 'full' }, this.dropzone), notes.length ? h('div', { class: 'full findings' }, ...notes) : null], [this.button(t('dialog.cancel'), () => this.close())]);
    files.focus();
  }

  //---------------------------------------------------------------- the files are chosen
  accept(flat: Flattened, opts: BeginOptions): void {
    this.forceNew = !!opts.forceNew;
    if (!flat.items.length) {
      this.showPick({ nothing: true, skipped: flat.skipped, ...(opts.resume ? { resume: opts.resume } : {}) });
      return;
    }
    this.flat = flat;
    this.resumeFrom = opts.resume ?? null;
    void this.upload(flat, opts.resume);
  }

  //---------------------------------------------------------------- step 2: upload
  private async upload(flat: Flattened, resume?: SavedBatch): Promise<void> {
    this.hadErrors = false;
    this.advancing = false;
    this.lastMilestone = 0;
    this.lastStates.clear();
    this.startedAt = 0;
    this.projectId = this.app.activeId ?? '';
    this.createdProject = false;
    this.target = 'current';
    this.plan = null;
    this.finish = null;
    this.names.clear();
    const name = flat.folders ? projectNameOf(flat) : flat.items.length === 1 ? flat.items[0]!.file.name : plural(flat.items.length, 'import.nFiles');
    this.showUpload(name);
    if (flat.skipped) this.body.prepend(h('div', { class: 'full finding', text: plural(flat.skipped, 'import.skipped') }));
    for (const d of flat.duplicates) this.body.prepend(h('div', { class: 'full finding', text: t('import.dup', { name: d }) }));
    this.ctl.wireUnload();
    const uploader = new TusUploader({
      entries: flat.items,
      projectId: this.app.activeId ?? '',
      name,
      ...(resume ? { resume } : {}),
      ...(this.ctl.deps.fetchImpl ? { fetchImpl: this.ctl.deps.fetchImpl } : {}),
      ...(this.ctl.deps.storage !== undefined ? { storage: this.ctl.deps.storage } : {}),
      ...(this.ctl.deps.sleep ? { sleep: this.ctl.deps.sleep } : {}),
      ...(this.ctl.deps.chunkBytes ? { chunkBytes: this.ctl.deps.chunkBytes } : {}),
      onChange: () => this.onUploadChange(),
    });
    this.uploader = uploader;
    try {
      await uploader.start();
    } catch (e) {
      this.uploader = null;
      this.showUploadError(e);
      return;
    }
    if (this.closed) return;
    if (uploader.missing.length) this.body.prepend(h('div', { class: 'full finding warning', text: t('import.resume.missing', { names: uploader.missing.join(', ') }) }));
    this.onUploadChange();
    await uploader.whenSettled();
    if (this.closed || uploader.cancelled || this.uploader !== uploader) return;
    this.onUploadChange();
    if (uploader.allDone && !this.hadErrors) void this.afterUpload();
  }

  private showUpload(name: string): void {
    this.totalBar = h('i');
    this.totalEl = h('div', { class: 'progress', attrs: { role: 'progressbar', 'aria-label': t('import.progress'), 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': '0', 'aria-valuetext': '' } }, this.totalBar);
    this.totalText = h('div', { class: 'total mono', text: t('import.connecting') });
    this.offlineEl = h('div', { class: 'finding warning', text: t('import.offline'), attrs: { role: 'status' }, style: { display: 'none' } });
    const list = h('div', { class: 'project-list upload-list', attrs: { role: 'list' } });
    this.pauseBtn = this.button(t('import.pauseAll'), () => this.togglePause());
    this.pauseBtn.classList.add('left');
    this.nextBtn = this.button(t('import.next'), () => void this.afterUpload(), 'btn primary');
    this.nextBtn.disabled = true;
    this.setView(
      'upload',
      t('import.uploading', { name }),
      t('import.step', { n: 2 }),
      [h('div', { class: 'full findings' }, this.offlineEl), h('div', { class: 'full' }, this.totalEl), h('div', { class: 'full' }, this.totalText), h('div', { class: 'full' }, list)],
      [this.pauseBtn, this.button(t('dialog.cancel'), () => void this.cancelUpload()), this.nextBtn],
    );
    this.pauseBtn.focus();
  }

  private togglePause(): void {
    const u = this.uploader;
    if (!u) return;
    if (u.files.some((f) => f.state === 'paused')) u.resumeAll();
    else u.pauseAll();
  }

  private makeRow(f: TusFile): Row {
    const icon = h('span', { class: 'icon', attrs: { 'aria-hidden': 'true' } });
    const status = h('span', { class: 'status' });
    const action = h('span', { class: 'action' });
    const el = h('div', { class: 'upload-row', attrs: { role: 'listitem' } }, icon, h('span', { class: 'name', text: f.name, title: f.relpath }), h('span', { class: 'size mono', text: fmtBytes(f.size) }), status, action);
    this.body.querySelector('.upload-list')?.append(el);
    const row: Row = { el, icon, status, action, sig: '' };
    this.rows.set(f.relpath, row);
    return row;
  }

  private updateRow(f: TusFile): void {
    const u = this.uploader!;
    const row = this.rows.get(f.relpath) ?? this.makeRow(f);
    const sig = `${f.state}|${f.errorKind ?? ''}|${f.failures >= 3}`;
    if (sig !== row.sig) {
      row.sig = sig;
      clear(row.status);
      clear(row.action);
      delete row.bar;
      delete row.pct;
      row.el.className = `upload-row ${f.state}`;
      const bar = () => {
        row.bar = h('i');
        row.pct = h('span', { class: 'pct mono' });
        row.status.append(h('div', { class: 'progress' }, row.bar), row.pct);
      };
      switch (f.state) {
        case 'done':
          row.icon.textContent = '●';
          row.status.append(t('import.state.done'));
          break;
        case 'uploading':
          row.icon.textContent = '◐';
          bar();
          row.action.append(h('button', { class: 'btn icon-btn', text: '⏸', title: t('import.pauseFile', { name: f.name }), attrs: { type: 'button', 'aria-label': t('import.pauseFile', { name: f.name }) }, on: { click: () => u.pause(f.relpath) } }));
          break;
        case 'paused':
          row.icon.textContent = '◐';
          row.status.append(t('import.state.paused'));
          row.action.append(h('button', { class: 'btn icon-btn', text: '▶', title: t('import.resumeFile', { name: f.name }), attrs: { type: 'button', 'aria-label': t('import.resumeFile', { name: f.name }) }, on: { click: () => u.resume(f.relpath) } }));
          break;
        case 'queued':
          row.icon.textContent = '○';
          row.status.append(t('import.state.queued'));
          break;
        case 'error': {
          row.icon.textContent = '✕';
          const key: MessageKey = f.errorKind === 'too_large' ? 'import.err.tooLarge' : f.errorKind === 'gone' ? 'import.err.gone' : 'import.err.failed';
          const text = t(key, { name: f.name }) + (f.errorKind !== 'too_large' && f.failures >= 3 ? ` ${t('import.err.hint')}` : '');
          // a fresh element each time: role=alert is read when it is inserted
          row.status.append(h('span', { class: 'err', text, attrs: { role: 'alert' } }));
          if (f.errorKind !== 'too_large') row.action.append(this.button(t('import.retry'), () => void u.retry(f.relpath)));
          row.action.append(this.button(t('import.remove'), () => this.removeFile(f.relpath)));
          break;
        }
      }
    }
    const pct = f.size ? Math.floor((Math.min(f.offset, f.size) / f.size) * 100) : 100;
    if (row.bar) row.bar.style.width = `${pct}%`;
    if (row.pct) row.pct.textContent = fmtPct(pct);
  }

  private removeFile(relpath: string): void {
    const u = this.uploader;
    if (!u) return;
    u.remove(relpath);
    this.rows.get(relpath)?.el.remove();
    this.rows.delete(relpath);
    if (!u.files.length) {
      this.uploader = null;
      this.showPick();
    }
  }

  private onUploadChange(): void {
    const u = this.uploader;
    if (!u || this.closed) return;
    const total = u.totalBytes;
    const done = u.uploadedBytes;
    const pct = total ? Math.floor((done / total) * 100) : u.allDone ? 100 : 0;
    if (!this.startedAt && done > 0) {
      this.startedAt = Date.now();
      this.startBytes = done;
    }

    // milestones for the screen reader: every 25 %, a file finished, a file failed (never every tick)
    const milestone = Math.floor(pct / 25) * 25;
    if (milestone > this.lastMilestone) {
      this.lastMilestone = milestone;
      this.ctl.announce(t('import.live.pct', { pct: fmtPct(milestone) }));
    }
    for (const f of u.files) {
      const prev = this.lastStates.get(f.relpath);
      if (prev !== f.state) {
        if (f.state === 'done' && prev) this.ctl.announce(t('import.live.fileDone', { name: f.name }));
        if (f.state === 'error') {
          this.hadErrors = true;
          this.ctl.announce(t('import.live.fileFailed', { name: f.name }));
          if (this.collapsed) this.expand();
        }
        this.lastStates.set(f.relpath, f.state);
      }
    }

    if (this.pill && this.pillBar && this.pillText) {
      this.pillBar.style.width = `${pct}%`;
      this.pillText.textContent = t('import.pill', { pct: fmtPct(pct) });
    }
    if (this.step !== 'upload') return;

    for (const f of u.files) this.updateRow(f);
    for (const rel of [...this.rows.keys()]) if (!u.files.some((f) => f.relpath === rel)) this.rows.delete(rel);
    if (this.totalBar && this.totalEl && this.totalText) {
      this.totalBar.style.width = `${pct}%`;
      this.totalEl.setAttribute('aria-valuenow', String(pct));
      const text = progressText(done, total);
      this.totalEl.setAttribute('aria-valuetext', t('import.progressText', { pct: fmtPct(pct), bytes: text }));
      const parts = [text, fmtPct(pct)];
      const elapsed = (Date.now() - this.startedAt) / 1000;
      // the estimate only after 5 s of data, and not while nothing moves
      if (this.startedAt && elapsed >= 5 && done > this.startBytes && done < total) parts.push(fmtEta((total - done) / ((done - this.startBytes) / elapsed)));
      this.totalText.textContent = parts.join(' · ');
    }
    if (this.offlineEl) this.offlineEl.style.display = u.offline ? '' : 'none';
    if (this.pauseBtn) {
      const anyPaused = u.files.some((f) => f.state === 'paused');
      this.pauseBtn.textContent = anyPaused ? t('import.resumeAll') : t('import.pauseAll');
      this.pauseBtn.disabled = u.files.every((f) => f.state === 'done' || f.state === 'error');
    }
    if (this.nextBtn) this.nextBtn.disabled = !u.allDone;
  }

  private showUploadError(e: unknown): void {
    this.hadErrors = true;
    const body: HTMLElement[] = [];
    if (e instanceof DiskFullError) body.push(this.alert(t('import.err.disk', { needed: fmtBytes(e.neededBytes), available: fmtBytes(e.availableBytes) })));
    else body.push(this.alert(e instanceof TusError && e.code ? errorText({ code: e.code, message: e.message }, 'import.err.create') : t('import.err.create')));
    this.setView('upload', t('import.uploading', { name: this.flat?.name ?? '' }), t('import.step', { n: 2 }), [h('div', { class: 'full findings' }, ...body)], [this.button(t('import.changeSelection'), () => this.showPick(), 'btn primary')]);
    if (this.collapsed) this.expand();
    this.footer.querySelector('button')?.focus();
  }

  //---------------------------------------------------------------- collapse into the pill / expand
  private collapse(): void {
    if (this.collapsed) return;
    this.collapsed = true;
    this.modal.scrim.remove();
    const bar = h('i');
    const text = h('span', { class: 'text' });
    this.pillBar = bar;
    this.pillBar.style.width = '0%';
    this.pillText = text;
    this.pill = h(
      'button',
      { class: 'toast pill', attrs: { type: 'button', 'aria-live': 'off' }, on: { click: () => this.expand() } },
      text,
      h('div', { class: 'progress' }, bar),
    );
    toastHost().append(this.pill);
    this.onUploadChange();
  }

  expand(): void {
    if (!this.collapsed) return;
    this.collapsed = false;
    this.pill?.remove();
    this.pill = this.pillBar = this.pillText = null;
    document.body.append(this.modal.scrim);
    const target = this.footer.querySelector<HTMLElement>('button.primary:not(:disabled), button:not(:disabled)') ?? this.dialog;
    target.focus();
    this.onUploadChange();
  }

  private async cancelUpload(): Promise<void> {
    const u = this.uploader;
    if (!u || !u.batchId) {
      this.close();
      return;
    }
    const stop = await this.confirmStop();
    if (!stop) return;
    this.uploader = null;
    await u.cancel();
    this.close();
  }

  /** «Зупинити завантаження? …» with focus on the safe answer. */
  private confirmStop(): Promise<boolean> {
    return new Promise((resolve) => {
      let answer = false;
      const keep = h('button', { class: 'btn primary', text: t('import.cancelKeep'), attrs: { type: 'button' } });
      const stop = h('button', { class: 'btn', text: t('import.cancelStop'), attrs: { type: 'button' } });
      const dialog = h('div', { class: 'dialog text-dialog' }, h('header', { text: t('import.cancelTitle') }), h('div', { class: 'body' }, h('div', { class: 'full', text: t('import.cancelText') })), h('footer', {}, stop, keep));
      const m = openModal(t('import.cancelTitle'), dialog, () => resolve(answer));
      stop.addEventListener('click', () => {
        answer = true;
        m.close();
      });
      keep.addEventListener('click', () => m.close());
      keep.focus();
    });
  }

  //---------------------------------------------------------------- after the upload: finish, plan
  private async afterUpload(): Promise<void> {
    const u = this.uploader;
    if (!u || this.advancing || !u.allDone) return;
    this.advancing = true;
    const busy = h('div', { class: 'full', text: t('import.planning'), attrs: { role: 'status' } });
    this.setView('plan', t('import.plan.title'), t('import.step', { n: 3 }), [busy], [this.button(t('dialog.cancel'), () => this.close())]);
    const fin = (await this.app.mcp.call('upload.finish', { batch_id: u.batchId })) as { ok: boolean; data?: FinishResult; error?: { code: string; message: string } };
    if (this.closed) return;
    if (!fin.ok || !fin.data) {
      this.advancing = false;
      this.showFailure(errorText(fin.error, 'import.err.finish'));
      return;
    }
    this.finish = fin.data;
    if (!(await this.ensureProject())) return;
    if (fin.data.detected === 'rpp') return void (await this.applyRpp(fin.data));
    await this.makePlan(true);
  }

  private showFailure(text: string): void {
    this.setView('plan', t('import.plan.title'), t('import.step', { n: 3 }), [this.alert(text)], [this.button(t('import.back'), () => this.showPick()), this.button(t('dialog.cancel'), () => this.close())]);
    this.footer.querySelector('button')?.focus();
    if (this.collapsed) this.expand();
  }

  /** The project the plan is made in: the open one, or a new one named after the folder or the first file. */
  private async ensureProject(): Promise<boolean> {
    const open = this.forceNew ? '' : (this.projectId || this.app.activeId) ?? '';
    if (open) {
      this.projectId = open;
      this.target = 'current';
      return true;
    }
    return this.createProject();
  }

  private async createProject(): Promise<boolean> {
    const name = this.flat ? projectNameOf(this.flat) : '';
    const r = await this.app.cmd<{ project_id: string }>('project.new', name ? { name } : {}, { projectId: undefined });
    if (!r.ok || !r.data) {
      this.advancing = false;
      this.showFailure(errorText(r.error, 'import.err.plan'));
      return false;
    }
    await this.app.listProjects();
    this.app.setActive(r.data.project_id);
    this.projectId = r.data.project_id;
    this.createdProject = true;
    this.target = 'new';
    return true;
  }

  private async planIn(projectId: string): Promise<Plan | null> {
    const fin = this.finish;
    if (!fin) return null;
    const r = await this.app.cmd<Plan>('import.plan', { paths: [fin.path], preset: 'auto', align: this.align, takes_mode: 'separate' }, { projectId, timeoutMs: 120000 });
    if (!r.ok || !r.data) {
      this.advancing = false;
      this.showFailure(errorText(r.error, 'import.err.plan'));
      return null;
    }
    return r.data;
  }

  /** `first`: the first plan after the upload (a clean loose-file plan is applied at once); later plans come from the alignment select. */
  private async makePlan(first: boolean): Promise<void> {
    const plan = await this.planIn(this.projectId);
    if (!plan || this.closed) return;
    this.plan = plan;
    const loose = !!this.flat && this.flat.folders === 0 && this.finish?.detected === 'audio';
    if (first && loose && !plan.warnings.length) {
      await this.apply();
      return;
    }
    this.ctl.announce(t('import.live.planReady'));
    this.showPlan();
    if (this.collapsed) this.expand();
  }

  //---------------------------------------------------------------- step 3: plan review
  private showPlan(): void {
    const plan = this.plan!;
    const inputs: HTMLInputElement[] = [];
    const rows = plan.tracks.map((tr, i) => {
      const key = tr.source_key || tr.name;
      const input = h('input', { attrs: { type: 'text', maxlength: '64', value: this.names.get(key) ?? tr.name, 'aria-label': t('import.nameLabel', { key }), autocomplete: 'off' } });
      input.addEventListener('input', () => this.names.set(key, input.value));
      input.addEventListener('keydown', (e) => {
        if (e.key !== 'Enter') return;
        e.preventDefault();
        (inputs[i + 1] ?? this.footer.querySelector<HTMLElement>('button.primary'))?.focus();
      });
      inputs.push(input);
      return h('div', { class: 'plan-row' }, h('span', { class: 'cbar', style: tr.color ? { background: tr.color } : {} }), h('span', { class: 'key mono', text: tr.source_key }), input, h('span', { class: 'meta mono', text: planMeta(tr) }));
    });

    const rates = new Set(plan.tracks.flatMap((x) => x.clips.map((c) => c.sample_rate)));
    const dur = Math.max(0, ...plan.tracks.flatMap((x) => x.clips.map((c) => c.duration_s)));
    const sourceKey = `import.src.${plan.detected_source}`;
    const detected = t('import.detected', {
      source: hasKey(sourceKey) ? t(sourceKey) : t('import.src.generic'),
      tracks: plural(plan.tracks.length, 'picker.tracks'),
      rate: rates.size === 1 ? t('import.rate', { n: fmtNumber([...rates][0]! / 1000, { maximumFractionDigits: 1 }) }) : t('import.rate.mixed'),
      dur: fmtDuration(dur),
    });

    const alignSel = h('select', { attrs: { 'aria-label': t('import.align') } }, ...(['start', 'bwf', 'none'] as const).map((a) => h('option', { text: t(`import.align.${a}`), attrs: { value: a, ...(a === this.align ? { selected: '' } : {}) } })));
    alignSel.addEventListener('change', () => {
      this.align = alignSel.value as Align;
      void this.replan();
    });

    const active = this.app.active;
    const targetRow = h('div', { class: 'full target' }, h('span', { class: 'label', text: t('import.target') }));
    if (this.createdProject || !active) {
      targetRow.append(h('span', { text: t('import.target.new') }));
    } else {
      const radio = (value: 'current' | 'new', label: string) => {
        const input = h('input', { attrs: { type: 'radio', name: 'import-target', value, ...(this.target === value ? { checked: '' } : {}) } });
        input.addEventListener('change', () => {
          if (input.checked) this.target = value;
        });
        return h('label', { class: 'radio' }, input, h('span', { text: label }));
      };
      targetRow.append(radio('current', t('import.target.current', { name: active.name })), radio('new', t('import.target.new')));
    }

    const warnings = plan.warnings.map((w) => {
      const key = `import.warn.${w.code}`;
      const text = hasKey(key) ? t(key, { f: w.file ?? '' }) : w.message;
      return h('div', { class: `finding ${w.code === 'UNREADABLE_AUDIO' ? 'error' : 'warning'}` }, h('span', { text }), ' ', h('span', { class: 'code', text: w.code }));
    });

    const apply = this.button(plural(plan.tracks.length, 'import.apply'), () => void this.apply(), 'btn primary');
    this.setView(
      'plan',
      t('import.plan.title'),
      t('import.step', { n: 3 }),
      [
        h('div', { class: 'full finding ok', text: detected }),
        h('div', { class: 'full plan-list' }, ...rows),
        targetRow,
        h('div', { class: 'full target' }, h('label', { class: 'label', text: t('import.align') }), alignSel),
        warnings.length ? h('div', { class: 'full findings' }, ...warnings) : null,
      ],
      [this.button(t('import.back'), () => this.showPick()), this.button(t('dialog.cancel'), () => this.close()), apply],
    );
    this.footer.firstElementChild?.classList.add('left');
    (inputs[0] ?? apply).focus();
  }

  /** The alignment changed: the plan is made again (the names typed so far are kept by source key). */
  private async replan(): Promise<void> {
    for (const el of this.body.querySelectorAll<HTMLElement>('input, select')) el.setAttribute('disabled', '');
    const plan = await this.planIn(this.projectId);
    if (!plan || this.closed) return;
    this.plan = plan;
    this.showPlan();
  }

  //---------------------------------------------------------------- apply
  private renameMap(plan: Plan): Record<string, string> {
    const rename: Record<string, string> = {};
    for (const tr of plan.tracks) {
      const key = tr.source_key || tr.name;
      const v = (this.names.get(key) ?? '').trim();
      if (v && v !== tr.name) rename[key] = v;
    }
    return rename;
  }

  private async apply(): Promise<void> {
    let plan = this.plan;
    if (!plan) return;
    const rename = this.renameMap(plan);
    this.setView('plan', t('import.plan.title'), t('import.step', { n: 3 }), [h('div', { class: 'full', text: t('import.applying'), attrs: { role: 'status' } })], []);
    if (this.target === 'new' && !this.createdProject) {
      // the plan belongs to the project it was made in: a new project needs its own plan (same options, same files)
      if (!(await this.createProject())) return;
      const again = await this.planIn(this.projectId);
      if (!again) return;
      plan = again;
    }
    this.app.setActive(this.projectId);
    const r = await this.app.edit('import.apply', { plan_id: plan.plan_id, ...(Object.keys(rename).length ? { rename } : {}) }, { refetch: true, undoLabel: 'import', timeoutMs: 120000 });
    if (this.closed) return;
    if (!r.ok) {
      this.showFailure(errorText(r.error, 'import.err.apply'));
      return;
    }
    const tracks = (r.data as { tracks?: { track_id: string }[] } | undefined)?.tracks ?? [];
    this.done();
    this.ctl.successToast(tracks.length || plan.tracks.length);
    this.ctl.highlightTracks(tracks.map((x) => x.track_id));
  }

  private done(): void {
    this.uploader?.dispose();
    this.close();
  }

  //---------------------------------------------------------------- .RPP
  private async applyRpp(fin: FinishResult): Promise<void> {
    const file = fin.files.find((f) => /\.rpp$/i.test(f.relpath));
    if (!file) {
      this.showFailure(t('import.err.apply'));
      return;
    }
    this.setView('plan', t('import.rpp.title'), '', [h('div', { class: 'full', text: t('import.applying'), attrs: { role: 'status' } })], []);
    this.app.setActive(this.projectId);
    const r = await this.app.edit('import.rpp', { path: `${fin.path.replace(/\/+$/, '')}/${file.relpath}` }, { refetch: true, undoLabel: 'import', timeoutMs: 120000 });
    if (this.closed) return;
    if (!r.ok) {
      this.showFailure(errorText(r.error, 'import.err.apply'));
      return;
    }
    this.showRpp((r.data ?? {}) as RppReport);
  }

  private showRpp(report: RppReport): void {
    const describe = (o: Record<string, unknown>): string => {
      const first = (...keys: string[]) => keys.map((k) => o[k]).find((v) => typeof v === 'string' && v) as string | undefined;
      const main = first('name', 'path') ?? first('kind') ?? '';
      const to = first('mapped_to');
      return to ? `${main} → ${to}` : main;
    };
    const section = (title: MessageKey, items: Record<string, unknown>[] | undefined, kind: 'ok' | 'warning' | 'error') => {
      const list = items ?? [];
      return h(
        'div',
        { class: 'full' },
        h('div', { class: 'label', text: `${t(title)} · ${fmtNumber(list.length)}` }),
        h('div', { class: 'findings rpp-list' }, ...list.map((o) => h('div', { class: `finding ${kind}` }, h('span', { text: describe(o) }), typeof o['kind'] === 'string' ? h('span', { class: 'code', text: ` ${o['kind']}` }) : null))),
      );
    };
    const close = this.button(t('import.close'), () => this.close(), 'btn primary');
    this.setView(
      'rpp',
      t('import.rpp.title'),
      '',
      [
        h('div', { class: 'full', text: t('import.rpp.summary', { tracks: report.tracks ?? 0, items: report.items ?? 0 }) }),
        section('import.rpp.mapped', report.mapped, 'ok'),
        section('import.rpp.unsupported', report.unsupported, 'warning'),
        section('import.rpp.missing', report.missing_media, 'error'),
      ],
      [close],
    );
    close.focus();
    if (this.collapsed) this.expand();
  }
}
