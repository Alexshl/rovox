// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The one WebSocket to the server: reconnects with back-off, correlates `cmd` / `result`, hands out events and binary frames.
import type { ServerMessage } from './protocol';

export type SocketStatus = 'connecting' | 'open' | 'closed';

export interface CmdOptions {
  projectId?: string;
  ifRevision?: number;
  coalesceKey?: string;
  undoLabel?: string;
  timeoutMs?: number;
}

export interface CmdResult<T = unknown> {
  ok: boolean;
  revision?: number;
  data?: T;
  error?: { code: string; message: string; details?: unknown };
}

export interface SocketHandlers {
  onStatus(status: SocketStatus): void;
  onMessage(msg: ServerMessage): void;
  onBinary(buf: ArrayBuffer): void;
  /** The socket closed `failures` times in a row before the server said `welcome` (a rejected token looks like this). */
  onHandshakeFailed?(failures: number): void;
}

export interface Heartbeat {
  /** How often a `ping` is sent. */
  pingMs: number;
  /** No frame at all for this long: the connection is dead (sleep, dropped network) and is closed by force. */
  deadMs: number;
}

export function socketUrl(loc: Pick<Location, 'protocol' | 'host'> = location): string {
  return `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/ws`;
}

export class RovoxSocket {
  status: SocketStatus = 'closed';
  private ws: WebSocket | null = null;
  private pending = new Map<string, { resolve: (r: CmdResult) => void; timer: ReturnType<typeof setTimeout> }>();
  private nextId = 1;
  private backoff = 500;
  private closedByUser = false;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;
  private beatTimer: ReturnType<typeof setInterval> | null = null;
  private lastRx = 0;
  private pingN = 1;
  private welcomed = false;
  private handshakeFailures = 0;

  constructor(
    private readonly url: string,
    private readonly handlers: SocketHandlers,
    private readonly makeSocket: (url: string) => WebSocket = (u) => new WebSocket(u),
    private readonly heartbeat: Heartbeat = { pingMs: 10000, deadMs: 25000 },
  ) {}

  connect(): void {
    this.closedByUser = false;
    if (this.ws) return;
    this.setStatus('connecting');
    const ws = this.makeSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    this.welcomed = false;
    ws.onopen = () => {
      this.backoff = 500;
      this.startHeartbeat(ws);
      this.setStatus('open');
      this.send({ t: 'hello', client: 'web/0.1' });
    };
    ws.onmessage = (ev: MessageEvent) => {
      this.lastRx = Date.now();
      if (typeof ev.data === 'string') {
        let msg: ServerMessage;
        try {
          msg = JSON.parse(ev.data) as ServerMessage;
        } catch {
          return;
        }
        if (msg.t === 'result') {
          const p = this.pending.get(msg.id);
          if (p) {
            clearTimeout(p.timer);
            this.pending.delete(msg.id);
            p.resolve({ ok: msg.ok, revision: msg.revision, data: msg.data, error: msg.error });
          }
          return;
        }
        if (msg.t === 'welcome') {
          this.welcomed = true;
          this.handshakeFailures = 0;
        }
        this.handlers.onMessage(msg);
      } else {
        this.handlers.onBinary(ev.data as ArrayBuffer);
      }
    };
    ws.onclose = () => this.handleClose(ws);
    ws.onerror = () => ws.close();
  }

  private handleClose(ws: WebSocket): void {
    if (this.ws !== ws) return;
    this.stopHeartbeat();
    this.ws = null;
    this.failPending();
    this.setStatus('closed');
    if (!this.welcomed) this.handshakeFailures++;
    if (!this.closedByUser) {
      this.retryTimer = setTimeout(() => this.connect(), this.backoff);
      this.backoff = Math.min(5000, this.backoff * 2);
      if (this.handshakeFailures >= 2) this.handlers.onHandshakeFailed?.(this.handshakeFailures);
    }
  }

  /** App-level ping / pong: a half-open socket (laptop woke up, network dropped) never fires `close`, so silence is the signal. */
  private startHeartbeat(ws: WebSocket): void {
    this.stopHeartbeat();
    this.lastRx = Date.now();
    this.beatTimer = setInterval(() => {
      if (Date.now() - this.lastRx > this.heartbeat.deadMs) {
        ws.onclose = null;
        ws.onmessage = null;
        try {
          ws.close();
        } catch {
          /* already gone */
        }
        this.handleClose(ws);
        return;
      }
      this.send({ t: 'ping', n: this.pingN++ });
    }, this.heartbeat.pingMs);
  }

  private stopHeartbeat(): void {
    if (this.beatTimer) clearInterval(this.beatTimer);
    this.beatTimer = null;
  }

  close(): void {
    this.closedByUser = true;
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.stopHeartbeat();
    this.handshakeFailures = 0;
    this.ws?.close();
  }

  send(obj: Record<string, unknown>): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    this.ws.send(JSON.stringify(obj));
    return true;
  }

  cmd<T = unknown>(name: string, params: Record<string, unknown> = {}, o: CmdOptions = {}): Promise<CmdResult<T>> {
    const id = `c_${this.nextId++}`;
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        resolve({ ok: false, error: { code: 'TIMEOUT', message: `${name} timed out` } });
      }, o.timeoutMs ?? 60000);
      this.pending.set(id, { resolve: resolve as (r: CmdResult) => void, timer });
      const sent = this.send({
        t: 'cmd',
        id,
        name,
        params,
        project_id: o.projectId,
        if_revision: o.ifRevision,
        coalesce_key: o.coalesceKey,
        undo_label: o.undoLabel,
      });
      if (!sent) {
        clearTimeout(timer);
        this.pending.delete(id);
        resolve({ ok: false, error: { code: 'DISCONNECTED', message: 'not connected to the server' } });
      }
    });
  }

  /** Several commands as one atomic undo step. */
  batch(ops: { name: string; params: Record<string, unknown> }[], o: CmdOptions = {}): Promise<CmdResult> {
    const id = `c_${this.nextId++}`;
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        resolve({ ok: false, error: { code: 'TIMEOUT', message: 'batch timed out' } });
      }, o.timeoutMs ?? 60000);
      this.pending.set(id, { resolve: resolve as (r: CmdResult) => void, timer });
      const sent = this.send({ t: 'cmd', id, ops, project_id: o.projectId, if_revision: o.ifRevision, undo_label: o.undoLabel });
      if (!sent) {
        clearTimeout(timer);
        this.pending.delete(id);
        resolve({ ok: false, error: { code: 'DISCONNECTED', message: 'not connected to the server' } });
      }
    });
  }

  private failPending(): void {
    for (const [id, p] of this.pending) {
      clearTimeout(p.timer);
      p.resolve({ ok: false, error: { code: 'DISCONNECTED', message: 'connection lost' } });
      this.pending.delete(id);
    }
  }

  private setStatus(s: SocketStatus): void {
    this.status = s;
    this.handlers.onStatus(s);
  }
}
