// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Render dialog: the same preflight, loudness normalisation and post-render checks an agent gets from render.preflight /
// render.start, driven through the MCP endpoint. Shows findings, progress, LUFS before / after and the file.
import { clear, h } from '../lib/dom';
import type { App } from '../store/session';

interface Finding {
  check: string;
  severity: string;
  message: string;
  name?: string;
}

function num(v: unknown, digits = 1): string {
  return typeof v === 'number' && Number.isFinite(v) ? v.toFixed(digits) : '-';
}

function field(label: string, control: HTMLElement): HTMLElement[] {
  return [h('label', { class: 'label', text: label }), control];
}

export function openRenderDialog(app: App): void {
  const project = app.active;
  if (!project) return;

  const format = h('select', {}, ...['wav', 'flac', 'mp3'].map((f) => h('option', { text: f, attrs: { value: f } })));
  const depth = h('select', {}, ...['24', '16', '32'].map((d) => h('option', { text: `${d} bit`, attrs: { value: d } })));
  const target = h('input', { attrs: { type: 'number', step: '0.5', placeholder: 'без нормалізації', 'aria-label': 'Target LUFS' } });
  const name = h('input', { attrs: { type: 'text', value: 'mix' } });
  const check = h('input', { attrs: { type: 'checkbox', checked: '' } });
  const bitrate = h('select', {}, ...['320', '256', '192', '128'].map((b) => h('option', { text: `${b} kbps`, attrs: { value: b } })));
  const output = h('div', { class: 'full' });
  const bar = h('i');
  const progress = h('div', { class: 'progress full', style: { display: 'none' } }, bar);
  const preflightBtn = h('button', { class: 'btn', text: 'Preflight' });
  const renderBtn = h('button', { class: 'btn primary', text: 'Render' });
  const closeBtn = h('button', { class: 'btn', text: 'Закрити' });
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
      if (!(preflight.failures?.length || preflight.warnings?.length)) list.append(h('div', { class: 'finding ok', text: 'Preflight: проблем не знайдено' }));
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
    output.append(h('div', { class: 'readout muted', text: kind === 'preflight' ? 'Перевірка...' : 'Рендер...' }));

    const res = await app.mcp.runJob(kind === 'preflight' ? 'render.preflight' : 'render.start', args(), (p, stage) => {
      bar.style.width = `${Math.round(p * 100)}%`;
      if (stage) output.firstElementChild!.textContent = stage;
    });

    progress.style.display = 'none';
    clear(output);
    busy = false;
    for (const b of [preflightBtn, renderBtn]) b.disabled = false;

    const job = res.data as { state?: string; result?: Record<string, any>; error?: { code: string; message: string; details?: any } } | undefined;
    if (!res.ok || !job || job.state === 'failed' || job.state === 'cancelled') {
      const err = job?.error ?? res.error;
      output.append(h('div', { class: 'finding error' }, h('div', { text: err?.message ?? 'Не вдалося' }), h('div', { class: 'code', text: err?.code ?? '' })));
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
          ['integrated', `${num(normalised ? l.after_lufs : st.integrated_lufs, 1)} LUFS`],
          ['до нормалізації', normalised ? `${num(l.before_lufs, 1)} LUFS` : '-'],
          ['gain', normalised ? `${num(l.gain_db, 2)} dB${l.limited_by_true_peak ? ' (обмежено true peak)' : ''}` : '-'],
          ['true peak', `${num(st.true_peak_dbtp, 1)} dBTP`],
          ['clipped', `${st.clipped_samples ?? 0} samples`],
          ['тривалість', `${num(file.duration_s, 1)} s`],
          ['розмір', file.bytes !== undefined ? `${num(file.bytes / 1048576, 1)} MB` : '-'],
        ].flatMap(([k, v]) => [h('dt', { text: k as string }), h('dd', { text: v as string })]),
      );
      output.append(
        h('div', { class: 'finding ok' }, h('div', { text: 'Рендер готовий' })),
        stats,
        h('div', {}, h('a', { text: fileName, attrs: { href: file.url ?? `/api/projects/${encodeURIComponent(project.id)}/renders/${encodeURIComponent(fileName)}`, download: fileName } })),
      );
    }
  };

  preflightBtn.addEventListener('click', () => void run('preflight'));
  renderBtn.addEventListener('click', () => void run('render'));

  const scrim = h(
    'div',
    { class: 'scrim', attrs: { role: 'dialog', 'aria-label': 'Render' } },
    h(
      'div',
      { class: 'dialog' },
      h('header', { text: `Render · ${project.name}` }),
      h(
        'div',
        { class: 'body' },
        ...field('Формат', format),
        ...field('Розрядність', depth),
        ...field('MP3 bitrate', bitrate),
        ...field('Target LUFS', target),
        ...field('Назва файлу', name),
        ...field('Check (fail on problems)', check),
        progress,
        output,
      ),
      h('footer', {}, closeBtn, preflightBtn, renderBtn),
    ),
  );
  const close = () => scrim.remove();
  closeBtn.addEventListener('click', close);
  scrim.addEventListener('pointerdown', (e) => {
    if (e.target === scrim) close();
  });
  scrim.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') close();
  });
  document.body.append(scrim);
  renderBtn.focus();
}
