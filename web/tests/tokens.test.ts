// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Design tokens: one source (design/tokens.json) -> tokens.css; colours in the views and in base.css come from the variables only.
import { readFileSync, readdirSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

const root = process.cwd();
const tokens = JSON.parse(readFileSync(`${root}/design/tokens.json`, 'utf8'));
const css = readFileSync(`${root}/src/styles/tokens.css`, 'utf8');
const name = (n: string) => (n.startsWith('rx-') ? `--${n}` : `--rx-${n}`);

describe('design tokens', () => {
  it('tokens.css carries every colour of the design system in both themes', () => {
    const dark = css.slice(0, css.indexOf("[data-theme='light']"));
    const light = css.slice(css.indexOf("[data-theme='light']"));
    for (const t of tokens.color.tokens) {
      expect(dark).toContain(`${name(t.name)}: ${t.value.dark};`);
      expect(light).toContain(`${name(t.name)}: ${t.value.light};`);
    }
    for (const required of ['panel-900', 'panel-800', 'panel-700', 'hairline', 'ink', 'ink-muted', 'rx-orange', 'rx-red', 'rx-amber', 'rx-cream', 'signal-green', 'agent-cyan', 'wave'])
      expect(tokens.color.tokens.some((t: { name: string }) => t.name === required)).toBe(true);
  });

  it('carries the type families, radii and spacing', () => {
    expect(css).toContain('--rx-font-display: "Barlow Condensed"');
    expect(css).toContain('--rx-font-sans: "IBM Plex Sans"');
    expect(css).toContain('--rx-font-mono: "IBM Plex Mono"');
    expect(css).toContain('--rx-radius-sm: 2px;');
    expect(css).toContain('--rx-radius-md: 4px;');
  });

  it('no hard-coded colour in the views or in base.css', () => {
    const files = readdirSync(`${root}/src/views`).filter((f) => f.endsWith('.ts')).map((f) => `${root}/src/views/${f}`);
    files.push(`${root}/src/styles/base.css`);
    for (const f of files) {
      const text = readFileSync(f, 'utf8').replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '');
      expect(text, f).not.toMatch(/#[0-9a-fA-F]{3,8}\b/);
      expect(text, f).not.toMatch(/\brgba?\(/);
    }
  });
});
