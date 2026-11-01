// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

import { describe, expect, it } from 'vitest';
import { McpClient } from '../src/net/mcp';

function fakeServer(script: (tool: string, args: any, n: number) => unknown) {
  const calls: { method: string; session: string | null; body: any }[] = [];
  let toolCalls = 0;
  const fetchImpl = (async (_url: string, init: RequestInit) => {
    const body = JSON.parse(String(init.body));
    const session = new Headers(init.headers).get('Mcp-Session-Id');
    calls.push({ method: body.method, session, body });
    if (body.method === 'initialize') return new Response(JSON.stringify({ jsonrpc: '2.0', id: body.id, result: {} }), { headers: { 'Mcp-Session-Id': 'sess-1' } });
    if (body.method === 'notifications/initialized') return new Response('', { status: 202 });
    const structured = script(body.params.name, body.params.arguments, ++toolCalls);
    return new Response(JSON.stringify({ jsonrpc: '2.0', id: body.id, result: { structuredContent: structured } }));
  }) as unknown as typeof fetch;
  return { fetchImpl, calls };
}

describe('MCP client of the render dialog', () => {
  it('initialises once, keeps the session id and long-polls the job until it is done', async () => {
    const progress: number[] = [];
    const { fetchImpl, calls } = fakeServer((tool, args, n) => {
      if (tool === 'render.start') return { ok: true, data: { job_id: 'job_1', state: 'running', progress: 0.1, stage: 'rendering' } };
      if (tool === 'job.get') return { ok: true, data: { job_id: args.job_id, state: n < 4 ? 'running' : 'succeeded', progress: n / 4, result: { file: { path: 'renders/mix.wav' } } } };
      return { ok: false };
    });
    const c = new McpClient('/mcp', fetchImpl);
    const out = await c.runJob('render.start', { project_id: 'p_1', target_lufs: -14 }, (p) => progress.push(p));
    expect(out.data.state).toBe('succeeded');
    expect(out.data.result.file.path).toBe('renders/mix.wav');
    expect(progress.length).toBeGreaterThan(0);
    expect(calls.filter((x) => x.method === 'initialize')).toHaveLength(1);
    expect(calls.filter((x) => x.method === 'tools/call').every((x) => x.session === 'sess-1')).toBe(true);
    expect(calls.find((x) => x.method === 'tools/call')!.body.params.arguments.wait_s).toBe(2);
  });

  it('returns a tool failure as is (the dialog shows its findings)', async () => {
    const { fetchImpl } = fakeServer(() => ({ ok: false, error: { code: 'PREFLIGHT_FAILED', message: 'checks failed', details: { preflight: { passed: false } } } }));
    const out = await new McpClient('/mcp', fetchImpl).runJob('render.start', {});
    expect(out.ok).toBe(false);
    expect(out.error!.code).toBe('PREFLIGHT_FAILED');
  });
});
