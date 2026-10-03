// Scanning. Kept scans (started here, from another browser, or with the
// device's button) are read while they are scanned by every open page, which
// shows them growing and saves the finished JPEG to Files. 600 dpi pages may
// not fit the device's memory, so they are streamed to the browser that asked.

import * as db from './db';
import { device, onState, type ScanState } from './device.svelte';
import { addFile, stamp, storedFile, type StoredFile } from './files.svelte';
import { toast } from './ui.svelte';

export type ScanPaper = 'a4' | 'letter' | 'card';

class ScanUi {
  preview = $state(''); // object URL of the (partial) JPEG being shown
  previewId = $state('');
  received = $state(0);
  streaming = $state(false); // a 600 dpi stream to this page
  streamLines = $state(0);
  error = $state('');
  lastFile = $state.raw<StoredFile | null>(null); // the newest scan saved here
}
export const scanUi = new ScanUi();

const POLL_MS = 1200;

let following = ''; // the kept scan this page reads
const waiters = new Map<string, ((f: StoredFile | null) => void)[]>();

// Resolves with the file once the scan `id` is in Files (null if it failed).
export function whenSaved(id: string): Promise<StoredFile | null> {
  return new Promise((resolve) => {
    const w = waiters.get(id) || [];
    w.push(resolve);
    waiters.set(id, w);
  });
}

function settle(id: string, f: StoredFile | null) {
  for (const fn of waiters.get(id) || []) fn(f);
  waiters.delete(id);
}

async function imported(): Promise<string[]> {
  return (await db.kvGet<string[]>('importedScans')) || [];
}

async function markImported(id: string) {
  const list = await imported();
  if (!list.includes(id)) await db.kvSet('importedScans', [...list.slice(-49), id]);
}

function setPreview(parts: BlobPart[], id: string) {
  const old = scanUi.preview;
  scanUi.preview = URL.createObjectURL(new Blob(parts, { type: 'image/jpeg' }));
  scanUi.previewId = id;
  if (old) setTimeout(() => URL.revokeObjectURL(old), 3000);
}

function scanName() {
  return `Scan ${stamp()}.jpg`;
}

// Saves a kept scan once per browser; fresh: this tab stored it.
async function save(id: string, blob: Blob): Promise<{ file: StoredFile | null; fresh: boolean }> {
  const fid = `scan-${id}`;
  if ((await imported()).includes(id)) return { file: await storedFile(fid), fresh: false };
  try {
    const f = await addFile({ id: fid, name: scanName(), blob, source: 'scan', scanId: id });
    await markImported(id);
    scanUi.lastFile = f;
    return { file: f, fresh: true };
  } catch (e) {
    if ((e as DOMException).name !== 'ConstraintError') throw e;
    await markImported(id); // another tab of this browser saved it
    const f = await storedFile(fid);
    if (f) scanUi.lastFile = f;
    return { file: f, fresh: false };
  }
}

// Reads a kept scan as it grows, then saves it.
async function follow(id: string) {
  if (following === id) return;
  following = id;
  scanUi.error = '';
  const parts: BlobPart[] = [];
  let from = 0;
  let state = 'scanning';
  let shown = 0;
  try {
    for (;;) {
      const r = await fetch(`/scan/last.jpg?id=${encodeURIComponent(id)}&from=${from}`, { cache: 'no-store' });
      if (r.status === 404) {
        state = 'gone';
        break;
      }
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      state = r.headers.get('X-Scan-State') || 'done';
      const b = new Uint8Array(await r.arrayBuffer());
      if (b.length) {
        parts.push(b);
        from += b.length;
        scanUi.received = from;
      }
      if (from && from !== shown) {
        setPreview(parts, id);
        shown = from;
      }
      if (state !== 'scanning') break;
      await new Promise((res) => setTimeout(res, POLL_MS));
      if (following !== id) return;
    }
    if (state === 'done' && from) {
      const { file, fresh } = await save(id, new Blob(parts, { type: 'image/jpeg' }));
      if (fresh) toast('Scan saved to Files', 'ok');
      settle(id, file);
    } else {
      settle(id, null);
    }
  } catch (e) {
    scanUi.error = `Lost the scan: ${(e as Error).message}`;
    settle(id, null);
  } finally {
    if (following === id) following = '';
  }
}

async function check(st: ScanState | null) {
  if (!st) return;
  if (st.mode === 'last' && st.id && (st.state === 'scanning' || (st.state === 'done' && st.last_id === st.id))) {
    if (st.state === 'scanning' || !(await imported()).includes(st.id)) {
      if (st.state === 'done' && !st.recent) return; // older: offered, not saved by itself
      follow(st.id);
    }
  } else if (st.mode === 'last' && st.id && st.state !== 'scanning') {
    settle(st.id, null); // failed or canceled
  }
}

export function initScanSync() {
  onState(() => check(device.scan));
}

export async function startKept(paper: ScanPaper, dpi: number): Promise<string> {
  scanUi.error = '';
  const r = await fetch(`/scan/last?paper=${paper}&dpi=${dpi}`, { method: 'POST' });
  if (!r.ok) throw new Error((await r.text()) || `HTTP ${r.status}`);
  const js = await r.json();
  return js.id as string;
}

let streamAbort: AbortController | null = null;

// 600 dpi: the JPEG comes straight to this page
export async function startStream(paper: ScanPaper, dpi: number): Promise<StoredFile | null> {
  scanUi.error = '';
  streamAbort = new AbortController();
  scanUi.streaming = true;
  const parts: BlobPart[] = [];
  let got = 0;
  try {
    const r = await fetch(`/scan/stream.jpg?dpi=${dpi}&paper=${paper}`, { cache: 'no-store', signal: streamAbort.signal });
    if (!r.ok) throw new Error((await r.text()) || `HTTP ${r.status}`);
    const id = `stream-${Date.now().toString(36)}`;
    const rd = r.body!.getReader();
    let t = 0;
    for (;;) {
      const { done, value } = await rd.read();
      if (done) break;
      parts.push(value);
      got += value.length;
      scanUi.received = got;
      if (Date.now() - t > 1500) {
        t = Date.now();
        setPreview(parts, id);
      }
    }
    setPreview(parts, id);
    if (device.scan?.state === 'canceled') return null;
    const f = await addFile({ name: scanName(), blob: new Blob(parts, { type: 'image/jpeg' }), source: 'scan' });
    scanUi.lastFile = f;
    toast('Scan saved to Files', 'ok');
    return f;
  } catch (e) {
    if ((e as Error).name !== 'AbortError') scanUi.error = (e as Error).message;
    return null;
  } finally {
    scanUi.streaming = false;
    streamAbort = null;
  }
}

export async function cancelScan() {
  await fetch('/scan/cancel', { method: 'POST' }).catch(() => {});
}

export function scanProgress(st: ScanState | null): number {
  return st && st.height ? Math.min(1, st.lines / st.height) : 0;
}

export function sourceLabel(s: string): string {
  return s === 'button' ? 'the button on the printer' : s === 'escl' ? 'a computer (AirScan)' : 'the web page';
}
