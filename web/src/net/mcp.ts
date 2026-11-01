// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// A small MCP client over Streamable HTTP (POST /mcp): the render dialog uses the same tools an agent does
// (render.preflight, render.start, job.get). Only tools/call is needed.

interface RpcResponse {
  result?: { structuredContent?: ToolEnvelope; isError?: boolean; content?: { type: string; text?: string }[] };
  error?: { code: number; message: string };
}

export interface ToolEnvelope {
  ok: boolean;
  data?: any;
  error?: { code: string; message: string; details?: unknown };
  revision?: number;
}

export class McpClient {
  private session: string | null = null;
  private nextId = 1;
  private initializing: Promise<void> | null = null;

  constructor(
    private readonly endpoint = '/mcp',
    private readonly fetchImpl: typeof fetch = (...a) => fetch(...a),
  ) {}

  private async post(body: unknown, withSession = true): Promise<Response> {
    const headers: Record<string, string> = { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream' };
    if (withSession && this.session) headers['Mcp-Session-Id'] = this.session;
    return this.fetchImpl(this.endpoint, { method: 'POST', headers, body: JSON.stringify(body), credentials: 'same-origin' });
  }

  private ensureSession(): Promise<void> {
    if (this.session) return Promise.resolve();
    this.initializing ??= (async () => {
      const res = await this.post(
        { jsonrpc: '2.0', id: this.nextId++, method: 'initialize', params: { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'rovox-web', version: '0.1' } } },
        false,
      );
      if (!res.ok) throw new Error(`MCP initialize failed: HTTP ${res.status}`);
      this.session = res.headers.get('Mcp-Session-Id');
      await this.post({ jsonrpc: '2.0', method: 'notifications/initialized' });
    })().finally(() => {
      this.initializing = null;
    });
    return this.initializing;
  }

  async call(tool: string, args: Record<string, unknown>): Promise<ToolEnvelope> {
    await this.ensureSession();
    const res = await this.post({ jsonrpc: '2.0', id: this.nextId++, method: 'tools/call', params: { name: tool, arguments: args } });
    if (res.status === 404) {
      this.session = null; // the session expired on the server
      return this.call(tool, args);
    }
    const text = await res.text();
    const json = parseRpc(text);
    if (json.error) return { ok: false, error: { code: 'MCP_ERROR', message: json.error.message } };
    return json.result?.structuredContent ?? { ok: false, error: { code: 'BAD_RESPONSE', message: 'no structured content' } };
  }

  /** Calls a job tool and long-polls job.get until the job is done. `onProgress` gets 0..1. */
  async runJob(tool: string, args: Record<string, unknown>, onProgress?: (p: number, note: string) => void): Promise<ToolEnvelope> {
    let out = await this.call(tool, { ...args, wait_s: 2 });
    for (;;) {
      if (!out.ok) return out;
      const job = out.data as { job_id?: string; state?: string; progress?: number; stage?: string } | undefined;
      if (!job || !job.job_id || job.state === 'succeeded' || job.state === 'failed' || job.state === 'cancelled') return out;
      onProgress?.(job.progress ?? 0, job.stage ?? '');
      out = await this.call('job.get', { job_id: job.job_id, wait_s: 2 });
    }
  }
}

/** The Streamable HTTP answer is JSON, or a short SSE stream whose last `data:` line is the response. */
export function parseRpc(text: string): RpcResponse {
  const trimmed = text.trim();
  if (trimmed.startsWith('{')) return JSON.parse(trimmed) as RpcResponse;
  let last: RpcResponse = { error: { code: -1, message: 'empty response' } };
  for (const line of trimmed.split('\n')) {
    if (line.startsWith('data:')) {
      const payload = line.slice(5).trim();
      if (payload.startsWith('{')) {
        const parsed = JSON.parse(payload) as RpcResponse & { method?: string };
        if (parsed.result || parsed.error) last = parsed;
      }
    }
  }
  return last;
}
