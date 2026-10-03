// Just enough IPP (RFC 8010/8011) to print from the browser: requests are
// built here and POSTed to /ipp/print, the device's own printer URI.

export const OP = {
  PRINT_JOB: 0x0002,
  CANCEL_JOB: 0x0008,
  GET_JOB_ATTRIBUTES: 0x0009,
  IDENTIFY_PRINTER: 0x003c,
} as const;

const TAG = {
  OPERATION: 0x01,
  JOB: 0x02,
  END: 0x03,
  INTEGER: 0x21,
  BOOLEAN: 0x22,
  ENUM: 0x23,
  TEXT: 0x41,
  NAME: 0x42,
  KEYWORD: 0x44,
  URI: 0x45,
  CHARSET: 0x47,
  LANGUAGE: 0x48,
  MIMETYPE: 0x49,
} as const;

type Value = { tag: number; value: string | number };

const enc = new TextEncoder();
let requestId = 1;

export function printerUri(): string {
  return `ipp://${location.host}/ipp/print`;
}

// operation attributes after charset / language, in order
export function encodeRequest(op: number, attrs: [string, Value][], jobAttrs: [string, Value][] = []): Uint8Array {
  const parts: number[] = [];
  const u16 = (v: number) => parts.push((v >> 8) & 0xff, v & 0xff);
  const bytes = (b: Uint8Array) => {
    for (const x of b) parts.push(x);
  };
  const attr = (name: string, v: Value) => {
    parts.push(v.tag);
    const n = enc.encode(name);
    u16(n.length);
    bytes(n);
    if (typeof v.value === 'number') {
      u16(4);
      const x = v.value;
      parts.push((x >>> 24) & 0xff, (x >>> 16) & 0xff, (x >>> 8) & 0xff, x & 0xff);
    } else {
      const s = enc.encode(v.value);
      u16(s.length);
      bytes(s);
    }
  };
  parts.push(2, 0); // IPP 2.0
  u16(op);
  const id = requestId++;
  parts.push((id >>> 24) & 0xff, (id >>> 16) & 0xff, (id >>> 8) & 0xff, id & 0xff);
  parts.push(TAG.OPERATION);
  attr('attributes-charset', { tag: TAG.CHARSET, value: 'utf-8' });
  attr('attributes-natural-language', { tag: TAG.LANGUAGE, value: 'en' });
  for (const [n, v] of attrs) attr(n, v);
  if (jobAttrs.length) {
    parts.push(TAG.JOB);
    for (const [n, v] of jobAttrs) attr(n, v);
  }
  parts.push(TAG.END);
  return new Uint8Array(parts);
}

export interface IppResponse {
  status: number;
  attrs: Record<string, (string | number | boolean)[]>;
}

export function decodeResponse(buf: ArrayBuffer): IppResponse {
  const d = new DataView(buf);
  const b = new Uint8Array(buf);
  const dec = new TextDecoder();
  if (b.length < 9) throw new Error('short IPP response');
  const status = d.getUint16(2);
  const attrs: IppResponse['attrs'] = {};
  let p = 8;
  let name = '';
  while (p < b.length) {
    const tag = b[p++];
    if (tag === TAG.END) break;
    if (tag < 0x10) continue; // group delimiter
    const nl = d.getUint16(p);
    p += 2;
    if (nl) name = dec.decode(b.subarray(p, p + nl));
    p += nl;
    const vl = d.getUint16(p);
    p += 2;
    const raw = b.subarray(p, p + vl);
    p += vl;
    let v: string | number | boolean;
    if ((tag === TAG.INTEGER || tag === TAG.ENUM) && vl === 4) v = d.getInt32(p - 4);
    else if (tag === TAG.BOOLEAN) v = raw[0] !== 0;
    else if (tag >= 0x40) v = dec.decode(raw);
    else continue; // collections etc.: not needed here
    (attrs[name] ||= []).push(v);
  }
  return { status, attrs };
}

export function statusOk(status: number): boolean {
  return status < 0x100;
}

const STATUS_TEXT: Record<number, string> = {
  0x0400: 'bad request',
  0x0406: 'not found',
  0x040a: 'document format not supported',
  0x0506: 'the printer is not accepting jobs',
  0x0507: 'the printer is busy',
  0x0504: 'the printer reported an error',
};

export function statusText(r: IppResponse): string {
  const msg = r.attrs['status-message']?.[0];
  return msg ? String(msg) : STATUS_TEXT[r.status] || `IPP status 0x${r.status.toString(16).padStart(4, '0')}`;
}

function user(): string {
  return 'Web browser';
}

export function printJobRequest(jobName: string, format: string): Uint8Array {
  return encodeRequest(OP.PRINT_JOB, [
    ['printer-uri', { tag: TAG.URI, value: printerUri() }],
    ['requesting-user-name', { tag: TAG.NAME, value: user() }],
    ['job-name', { tag: TAG.NAME, value: jobName.slice(0, 60) }],
    ['document-format', { tag: TAG.MIMETYPE, value: format }],
  ]);
}

async function call(body: Uint8Array): Promise<IppResponse> {
  const r = await fetch('/ipp/print', {
    method: 'POST',
    headers: { 'Content-Type': 'application/ipp' },
    body: body as BodyInit,
  });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return decodeResponse(await r.arrayBuffer());
}

export async function cancelJob(id: number): Promise<IppResponse> {
  return call(
    encodeRequest(OP.CANCEL_JOB, [
      ['printer-uri', { tag: TAG.URI, value: printerUri() }],
      ['job-id', { tag: TAG.INTEGER, value: id }],
      ['requesting-user-name', { tag: TAG.NAME, value: user() }],
    ]),
  );
}

export async function identifyPrinter(): Promise<IppResponse> {
  return call(
    encodeRequest(OP.IDENTIFY_PRINTER, [
      ['printer-uri', { tag: TAG.URI, value: printerUri() }],
      ['requesting-user-name', { tag: TAG.NAME, value: user() }],
    ]),
  );
}
