// Live device state: the JSON snapshot the server pushes on GET /events
// (components/airprint/events.c) whenever something changes.

import { t, i18n } from './i18n.svelte';

export interface Supply {
  name: string;
  color: string;
  level: number; // 0..100, < 0 unknown
}

export interface Job {
  id: number;
  name: string;
  user: string;
  state: 'pending' | 'held' | 'processing' | 'canceled' | 'aborted' | 'completed';
  reason: string;
  pages: number;
  bytes: number;
  time: number; // when it was created or ended, s since 1970 (device clock)
}

export interface PrinterState {
  name: string;
  model: string;
  connected: boolean;
  state: 'idle' | 'processing' | 'stopped';
  message: string;
  reasons: string[];
  supplies: Supply[];
  active: number;
  jobs: Job[];
}

export interface ScanState {
  scanner: boolean;
  state: 'idle' | 'scanning' | 'done' | 'failed' | 'canceled';
  mode: 'stream' | 'last';
  id: string;
  source: string; // web, stream, button, escl
  dpi: number;
  paper: string;
  width: number;
  height: number;
  lines: number;
  bytes: number;
  elapsed: number;
  recent: boolean; // ended in the last 10 minutes
  error: string;
  last: number; // bytes of the kept scan, 0 = none
  last_id: string;
  last_paper: string;
  last_dpi: number;
  button: { dpi: number; paper: string };
}

export const device = $state({
  online: false, // the event stream is up
  loaded: false,
  printer: null as PrinterState | null,
  scan: null as ScanState | null,
});

type Listener = (prev: { printer: PrinterState | null; scan: ScanState | null }) => void;
const listeners: Listener[] = [];

// called after every snapshot with the previous one
export function onState(fn: Listener): () => void {
  listeners.push(fn);
  return () => listeners.splice(listeners.indexOf(fn), 1);
}

function apply(js: { printer?: PrinterState; scan?: ScanState }) {
  const prev = { printer: device.printer, scan: device.scan };
  device.printer = js.printer ?? null;
  device.scan = js.scan ?? null;
  device.loaded = true;
  for (const fn of [...listeners]) fn(prev);
}

let es: EventSource | null = null;
let retry: ReturnType<typeof setTimeout> | undefined;

export function connect() {
  clearTimeout(retry);
  es?.close();
  es = new EventSource('/events');
  es.addEventListener('state', (e) => {
    device.online = true;
    try {
      apply(JSON.parse((e as MessageEvent).data));
    } catch {
      /* a corrupt event: the next one fixes it */
    }
  });
  es.onerror = () => {
    device.online = false;
    // EventSource retries by itself unless the server refused the stream
    if (es?.readyState === EventSource.CLOSED) retry = setTimeout(connect, 3000);
  };
}

// a phone page coming back from the background may have lost the stream
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && (!es || es.readyState === EventSource.CLOSED)) connect();
});

export async function refresh() {
  try {
    const r = await fetch('/api/state', { cache: 'no-store' });
    if (r.ok) apply(await r.json());
  } catch {
    /* offline */
  }
}

export function inkSummary(p: PrinterState | null): string {
  if (!p || !p.supplies.length) return '';
  return p.supplies.map((s) => {
    const name = deviceLabel(s.name);
    const level = s.level >= 0 ? s.level.toLocaleString(i18n.locale) + '%' : '?';
    return `${name} ${level}`;
  }).join(' · ');
}

// Translate known device labels; preserve unknown device/vendor messages.
const DEVICE_LABELS = [
  'Ready', 'Printing', 'Scanning', 'Ink cartridge empty or missing', 'Ink low',
  'Out of paper', 'Paper jam', 'Cover open', 'Printer not connected',
  'Dev mode: jobs are not printed', 'Black', 'Color', 'Cyan', 'Magenta', 'Yellow',
] as const;

export function deviceLabel(value: string): string {
  const key = DEVICE_LABELS.find((key) => key === value);
  return key ? t(key) : value;
}
