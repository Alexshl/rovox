// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors
//
// A minimal MCP-over-HTTP client (Node fetch, no dependencies) for seeding projects and driving the server from tests.

export interface ToolResult {
  ok: boolean;
  data?: any;
  error?: { code: string; message: string };
  revision?: number;
}

export class Mcp {
  private session: string | null = null;
  private nextId = 0;

  constructor(
    readonly base: string,
    readonly token: string,
    readonly clientName = 'e2e-seed',
  ) {}

  private headers(): Record<string, string> {
    const h: Record<string, string> = { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream', 'MCP-Protocol-Version': '2025-06-18' };
    if (this.token) h.Authorization = `Bearer ${this.token}`;
    if (this.session) h['Mcp-Session-Id'] = this.session;
    return h;
  }

  private async post(msg: Record<string, unknown>): Promise<any> {
    const res = await fetch(`${this.base}/mcp`, { method: 'POST', headers: this.headers(), body: JSON.stringify(msg) });
    this.session = res.headers.get('Mcp-Session-Id') ?? this.session;
    const body = await res.text();
    if (!res.ok) throw new Error(`MCP HTTP ${res.status}: ${body.slice(0, 300)}`);
    if (!('id' in msg)) return null;
    if ((res.headers.get('Content-Type') ?? '').includes('text/event-stream')) {
      for (const block of body.split('\n\n'))
        for (const line of block.split('\n'))
          if (line.startsWith('data:')) {
            const m = JSON.parse(line.slice(5));
            if (m.id === msg.id) return m;
          }
      throw new Error('no response in the event stream');
    }
    return JSON.parse(body);
  }

  async rpc(method: string, params: Record<string, unknown> = {}): Promise<any> {
    const id = ++this.nextId;
    const m = await this.post({ jsonrpc: '2.0', id, method, params });
    if (m.error) throw new Error(`${method}: ${JSON.stringify(m.error)}`);
    return m.result;
  }

  async init(): Promise<this> {
    await this.rpc('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: this.clientName, version: '1' } });
    await this.post({ jsonrpc: '2.0', method: 'notifications/initialized' });
    return this;
  }

  /** Calls a tool and returns the structured result; throws when ok is false. */
  async ok(name: string, args: Record<string, unknown> = {}): Promise<ToolResult> {
    const r = (await this.rpc('tools/call', { name, arguments: args })).structuredContent as ToolResult;
    if (!r?.ok) throw new Error(`${name} failed: ${JSON.stringify(r?.error)}`);
    return r;
  }

  async batch(projectId: string, ops: { tool: string; arguments: Record<string, unknown> }[], undoLabel?: string): Promise<ToolResult> {
    return this.ok('batch', { project_id: projectId, ops, ...(undoLabel ? { undo_label: undoLabel } : {}) });
  }
}

export async function waitHealthy(base: string, seconds = 90): Promise<void> {
  const end = Date.now() + seconds * 1000;
  for (;;) {
    try {
      if ((await fetch(`${base}/healthz`)).ok) return;
    } catch {
      /* not up yet */
    }
    if (Date.now() > end) throw new Error(`${base}/healthz did not answer in ${seconds} s`);
    await new Promise((r) => setTimeout(r, 1000));
  }
}

/** Uploads files over the tus batch API the UI uses, finishes the batch and returns the server-side folder. */
export async function uploadFolder(mcp: Mcp, files: { relpath: string; data: Buffer }[]): Promise<string> {
  const auth: Record<string, string> = mcp.token ? { Authorization: `Bearer ${mcp.token}` } : {};
  const created = await fetch(`${mcp.base}/api/upload-batches`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...auth },
    body: JSON.stringify({ files: files.map((f) => ({ relpath: f.relpath, size: f.data.length })) }),
  });
  const batch = (await created.json()) as { batch_id: string; files: { relpath: string; url: string }[] };
  if (!created.ok || !batch.batch_id) throw new Error(`upload-batches: HTTP ${created.status}`);
  for (const f of files) {
    const url = batch.files.find((x) => x.relpath === f.relpath)!.url;
    const res = await fetch(new URL(url, mcp.base), {
      method: 'PATCH',
      headers: { 'Tus-Resumable': '1.0.0', 'Content-Type': 'application/offset+octet-stream', 'Upload-Offset': '0', ...auth },
      body: new Uint8Array(f.data),
    });
    if (!res.ok) throw new Error(`upload ${f.relpath}: HTTP ${res.status}`);
  }
  const fin = await mcp.ok('upload.finish', { batch_id: batch.batch_id });
  return fin.data.path as string;
}
