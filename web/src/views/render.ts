// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Render dialog: the same preflight, loudness normalisation and post-render checks an agent gets from render.preflight /
// render.start, driven through the MCP endpoint. Shows findings, progress, LUFS before / after and the file.
import { errorText, fmtBytes, fmtNumber, plural, t } from '../i18n';
import { clear, h } from '../lib/dom';
import type { ToolEnvelope } from '../net/mcp';
import type { App } from '../store/session';
import { openModal } from './dialog';

interface Finding {
  check: string;
  severity: string;
  message: string;
  name?: string;
}

/** A readout number in the active language ("−14,0"): fixed digits, a real minus sign; '-' when there is no value. */
function num(v: unknown, digits = 1): string {
  if (typeof v !== 'number' || !Number.isFinite(v)) return '-';
  return fmtNumber(v, { minimumFractionDigits: digits, maximumFractionDigits: digits }).replace('-', '\u2212');
}

let fieldSeq = 0;

function field(label: string, control: HTMLElement): HTMLElement[] {
  const id = `render-f${++fieldSeq}`;
  control.id = id;
  return [h('label', { class: 'label', text: label, attrs: { for: id } }), control];
}

export function openRenderDialog(app: App): void {
  const project = app.active;
  if (!project) return;

  const format = h('select', {}, ...['wav', 'flac', 'mp3'].map((f) => h('option', { text: f, attrs: { value: f } })));
  const depth = h('select', {}, ...['24', '16', '32'].map((d) => h('option', { text: `${d} ${t('unit.bit')}`, attrs: { value: d } })));
  const target = h('input', { attrs: { type: 'number', step: '0.5', placeholder: t('render.noNormalize') } });
  const name = h('input', { attrs: { type: 'text', value: 'mix' } });
  const check = h('input', { attrs: { type: 'checkbox', checked: '' } });
  const bitrate = h('select', {}, ...['320', '256', '192', '128'].map((b) => h('option', { text: `${b} ${t('unit.kbps')}`, attrs: { value: b } })));
  const output = h('div', { class: 'full', attrs: { 'aria-live': 'polite' } });
  const bar = h('i');
  const progress = h('div', { class: 'progress full', style: { display: 'none' } }, bar);
  const preflightBtn = h('button', { class: 'btn', text: t('render.preflight') });
  const renderBtn = h('button', { class: 'btn primary', text: t('render.start') });
  const closeBtn = h('button', { class: 'btn', text: t('render.close') });
  let busy = false;

  const args = (): Record<string, unknown> => {
    const a: Record<string, unknown> = {
      project_id: project.id,
      name: name.value.trim() || 'mix',
      check: check.checked,
      format: { container: format.value, bit_depth: Number(depth.value), bitrate_kbps: Number(bitrate.value) },
    };
    if (target.value !== '') {
      a.target_lufs = Number(target.value);
      a.normalize = true;
    }
    return a;
  };

  const showFindings = (preflight: { passed?: boolean; warnings?: Finding[]; failures?: Finding[] } | undefined) => {
    const list = h('div', { class: 'findings' });
    if (preflight) {
      for (const f of preflight.failures ?? []) list.append(h('div', { class: 'finding error' }, h('div', { text: f.message }), h('div', { class: 'code', text: `${f.check}${f.name ? ' · ' + f.name : ''}` })));
      for (const f of preflight.warnings ?? []) list.append(h('div', { class: 'finding warning' }, h('div', { text: f.message }), h('div', { class: 'code', text: `${f.check}${f.name ? ' · ' + f.name : ''}` })));
      if (!(preflight.failures?.length || preflight.warnings?.length)) list.append(h('div', { class: 'finding ok', text: t('render.noProblems') }));
    }
    return list;
  };

  const run = async (kind: 'preflight' | 'render') => {
    if (busy) return;
    busy = true;
    for (const b of [preflightBtn, renderBtn]) b.disabled = true;
    clear(output);
    progress.style.display = 'block';
    bar.style.width = '0%';
    output.append(h('div', { class: 'readout muted', text: kind === 'preflight' ? t('render.checking') : t('render.rendering') }));

    let res: ToolEnvelope;
    try {
      res = await app.mcp.runJob(kind === 'preflight' ? 'render.preflight' : 'render.start', args(), (p, stage) => {
        bar.style.width = `${Math.round(p * 100)}%`;
        if (stage && output.firstElementChild) output.firstElementChild.textContent = stage;
      });
    } catch (e) {
      res = { ok: false, error: { code: 'NETWORK', message: e instanceof Error ? e.message : 'network error' } };
    } finally {
      progress.style.display = 'none';
      clear(output);
      busy = false;
      for (const b of [preflightBtn, renderBtn]) b.disabled = false;
    }

    const job = res.data as { state?: string; result?: Record<string, any>; error?: { code: string; message: string; details?: any } } | undefined;
    if (!res.ok || !job || job.state === 'failed' || job.state === 'cancelled') {
      const err = job?.error ?? res.error;
      // the plain text first; the raw server message and code stay below it
      const raw = [err?.code, err?.message].filter((x) => x).join(': ');
      output.append(h('div', { class: 'finding error' }, h('div', { text: errorText(err, 'render.failed') }), h('div', { class: 'code', text: raw })));
      const details = (err?.details as { preflight?: any } | undefined)?.preflight;
      if (details) output.append(showFindings(details));
      return;
    }

    const result = job.result ?? {};
    if (result.preflight) output.append(showFindings(result.preflight));
    if (kind === 'render' && result.file) {
      const l = result.loudness ?? {};
      const st = result.stats ?? {};
      const file = result.file as { path?: string; url?: string; bytes?: number; duration_s?: number };
      const fileName = String(file.path ?? '').split('/').pop() ?? '';
      const normalised = l.mode === 'normalize';
      const stats = h(
        'dl',
        { class: 'stat-grid' },
        ...[
          [t('render.integrated'), `${num(normalised ? l.after_lufs : st.integrated_lufs, 1)} LUFS`],
          [t('render.before'), normalised ? `${num(l.before_lufs, 1)} LUFS` : '-'],
          [t('render.gain'), normalised ? `${num(l.gain_db, 2)} dB${l.limited_by_true_peak ? ` ${t('render.limitedByPeak')}` : ''}` : '-'],
          [t('render.truePeak'), `${num(st.true_peak_dbtp, 1)} dBTP`],
          [t('render.clipped'), plural(Number(st.clipped_samples ?? 0), 'render.clipped')],
          [t('render.duration'), `${num(file.duration_s, 1)} ${t('unit.s')}`],
          [t('render.size'), file.bytes !== undefined ? fmtBytes(file.bytes) : '-'],
        ].flatMap(([k, v]) => [h('dt', { text: k as string }), h('dd', { text: v as string })]),
      );
      output.append(
        h('div', { class: 'finding ok' }, h('div', { text: t('render.done') })),
        stats,
        h('div', {}, h('a', { text: fileName, attrs: { href: file.url ?? `/api/projects/${encodeURIComponent(project.id)}/renders/${encodeURIComponent(fileName)}`, download: fileName } })),
      );
    }
  };

  preflightBtn.addEventListener('click', () => void run('preflight'));
  renderBtn.addEventListener('click', () => void run('render'));

  const dialog = h(
    'div',
    { class: 'dialog' },
    h('header', { text: t('render.title', { name: project.name }) }),
    h(
      'div',
      { class: 'body' },
      ...field(t('render.format'), format),
      ...field(t('render.depth'), depth),
      ...field(t('render.bitrate'), bitrate),
      ...field(t('render.target'), target),
      ...field(t('render.fileName'), name),
      ...field(t('render.check'), check),
      progress,
      output,
    ),
    h('footer', {}, closeBtn, preflightBtn, renderBtn),
  );
  const modal = openModal(t('render.dialog'), dialog);
  closeBtn.addEventListener('click', () => modal.close());
  renderBtn.focus();
}
