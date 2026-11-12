// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The peaks fetcher: failures back off, one request per item (a newer window replaces the queued one), keys carry the project,
// the cache is bounded.
import { describe, expect, it, vi } from 'vitest';
import { PEAK_BACKOFF_MS, PEAK_CACHE_LIMIT, PEAK_RETRY_MS, PeakCache } from '../src/views/peaks';
import type { ItemState } from '../src/net/protocol';

const item = (id: string, length = 10): ItemState => ({ item_id: id, track_id: 't', name: id, start_s: 0, length_s: length, end_s: length, muted: false, file: 'a.wav', offset_s: 0 });
const flush = () => new Promise((r) => setTimeout(r, 0));

function okResponse(): Response {
  return { ok: true, headers: new Headers(), arrayBuffer: async () => new ArrayBuffer(8) } as unknown as Response;
}

describe('peak cache', () => {
  it('does not repeat a failed request before the back-off passes, then waits longer', async () => {
    let t = 1000;
    const fetchImpl = vi.fn(async () => ({ ok: false, status: 404 }) as unknown as Response);
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch, () => t);
    const it1 = item('a');
    for (let i = 0; i < 100; i++) c.get('p', it1, 0, 10, 100);
    await flush();
    for (let i = 0; i < 100; i++) c.get('p', it1, 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(1);

    t += PEAK_BACKOFF_MS[0]! + 1;
    c.get('p', it1, 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2);
    t += PEAK_BACKOFF_MS[0]! + 1; // the second pause is longer
    c.get('p', it1, 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2);
    t += PEAK_BACKOFF_MS[1]!;
    c.get('p', it1, 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(3);
  });

  it('a network error backs off too', async () => {
    const fetchImpl = vi.fn(async () => {
      throw new Error('offline');
    });
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch, () => 5);
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(1);
  });

  it('keeps at most one waiting request per item while zooming', async () => {
    const resolvers: (() => void)[] = [];
    const fetchImpl = vi.fn(() => new Promise<Response>((res) => resolvers.push(() => res(okResponse()))));
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch);
    const a = item('a', 1000);
    for (let z = 1; z <= 40; z++) c.get('p', a, 0, 1000, z * 3);
    expect(fetchImpl).toHaveBeenCalledTimes(1); // the first one is in flight
    expect(c.stats().queued).toBe(1); // the 39 newer windows replaced each other
    expect(c.stats().inflight).toBe(1);
    resolvers[0]!();
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2); // only the newest window follows
    resolvers[1]!();
    await flush();
    expect(c.stats().queued).toBe(0);
    expect(c.stats().inflight).toBe(0);
  });

  it('separates projects: the same item id in two projects is two entries and two requests', async () => {
    const fetchImpl = vi.fn(async () => okResponse());
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch);
    c.get('p1', item('a'), 0, 10, 100);
    c.get('p2', item('a'), 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2);
    expect(c.get('p1', item('a'), 0, 10, 100)).toBeDefined();
    expect(c.get('p2', item('a'), 0, 10, 100)).toBeDefined();
    expect(c.stats().entries).toBe(2);
    c.forget('p1', 'a');
    expect(c.stats().entries).toBe(1);
  });

  it('is bounded: the least recently used entries are evicted', async () => {
    const fetchImpl = vi.fn(async () => okResponse());
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch);
    for (let i = 0; i < PEAK_CACHE_LIMIT + 50; i++) {
      c.get('p', item(`i${i}`), 0, 10, 100);
      if (i % 20 === 0) await flush();
    }
    await flush();
    await flush();
    expect(c.stats().entries).toBeLessThanOrEqual(PEAK_CACHE_LIMIT);
  });
});

describe('peak cache failure recovery', () => {
  const status = (n: number) => vi.fn(async () => ({ ok: false, status: n }) as unknown as Response);

  it('resetFailures lets every item retry at once (after a reconnect)', async () => {
    let t = 1000;
    const fetchImpl = status(404);
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch, () => t);
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(1); // backing off
    c.resetFailures();
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2);
  });

  it('a change of the item geometry clears its back-off', async () => {
    const t = 1000;
    const fetchImpl = status(404);
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch, () => t);
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(1);
    c.get('p', { ...item('a'), offset_s: 1 }, 0, 10, 100); // trimmed: a different source window
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2);
  });

  it('retries 5xx and network errors after a short capped delay, not the long back-off', async () => {
    let t = 1000;
    const fetchImpl = vi.fn(async () => {
      if (fetchImpl.mock.calls.length === 1) throw new TypeError('network');
      return { ok: false, status: 504 } as unknown as Response;
    });
    const c = new PeakCache(() => undefined, fetchImpl as unknown as typeof fetch, () => t);
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    t += PEAK_RETRY_MS[0]! + 1;
    c.get('p', item('a'), 0, 10, 100);
    await flush();
    expect(fetchImpl).toHaveBeenCalledTimes(2);
    for (let i = 0; i < 6; i++) {
      t += PEAK_RETRY_MS[PEAK_RETRY_MS.length - 1]! + 1; // the delay never grows past the cap
      c.get('p', item('a'), 0, 10, 100);
      await flush();
    }
    expect(fetchImpl).toHaveBeenCalledTimes(8);
    expect(PEAK_RETRY_MS[PEAK_RETRY_MS.length - 1]).toBeLessThan(PEAK_BACKOFF_MS[0]!);
  });
});
