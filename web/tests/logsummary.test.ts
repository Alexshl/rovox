// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// @vitest-environment jsdom
// The activity-log line of a change without a label: counts per operation and kind in the UI language, never internal field names.
import { afterEach, describe, expect, it } from 'vitest';
import { setLocale } from '../src/i18n';
import { describeParams, summarizeChanges } from '../src/store/project';

const add = (kind: string, n: number) => Array.from({ length: n }, (_, i) => ({ kind, id: `${kind}${i}`, op: 'add' as const, fields: ['id', 'name', 'colour'] }));

describe('summarizeChanges', () => {
  afterEach(() => setLocale('uk'));

  it('counts an import in Ukrainian plural forms', () => {
    setLocale('uk');
    const s = summarizeChanges([...add('track', 2), ...add('item', 5), ...add('edit', 1)]);
    expect(s).toBe('+2 доріжки, 5 кліпів, 1 об’єкт');
    expect(s).not.toMatch(/colour|name|track|item/);
  });

  it('groups operations in a fixed order and switches language', () => {
    setLocale('en');
    const s = summarizeChanges([{ kind: 'plugin', op: 'update' }, { kind: 'item', op: 'remove' }, ...add('marker', 1)]);
    expect(s).toBe('+1 marker · −1 clip · changed: 1 FX');
  });

  it('uses the Lithuanian four-way plural', () => {
    setLocale('lt');
    expect(summarizeChanges(add('track', 12))).toBe('+12 takelių');
    expect(summarizeChanges(add('track', 3))).toBe('+3 takeliai');
  });

  it('is empty for no changes', () => {
    expect(summarizeChanges([])).toBe('');
  });
});

describe('describeParams', () => {
  it('keeps plain arguments and drops ids and nested objects', () => {
    expect(describeParams({ project_id: 'p1', plan_id: 'ipl_01', track_ids: ['a'], name: 'Bass', gain_db: -3, mute: true, range: { s: 1 } })).toBe(
      'name: Bass, gain_db: -3, mute: true',
    );
    expect(describeParams({ plan_id: 'ipl_01' })).toBe('');
    expect(describeParams(null)).toBe('');
  });
});
