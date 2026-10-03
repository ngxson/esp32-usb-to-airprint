// Apple URF ("UNIRAST") raster, the format the printer server decodes
// (components/airprint/raster.c). Per line: a repeat byte (the line is used
// r + 1 times), then packets until the line is full:
//   n < 128   the next pixel is repeated n + 1 times
//   n = 128   the rest of the line is white
//   n > 128   257 - n literal pixels follow
// Pixels are 3 bytes (sRGB) or 1 byte (gray).

export interface UrfPage {
  width: number;
  height: number;
  dpi: number;
  gray: boolean;
}

export function urfFileHeader(pages: number): Uint8Array {
  const h = new Uint8Array(12);
  h.set([0x55, 0x4e, 0x49, 0x52, 0x41, 0x53, 0x54, 0]); // "UNIRAST\0"
  new DataView(h.buffer).setUint32(8, pages);
  return h;
}

export function urfPageHeader(p: UrfPage): Uint8Array {
  const h = new Uint8Array(32);
  const v = new DataView(h.buffer);
  h[0] = p.gray ? 8 : 24; // bits per pixel
  h[1] = p.gray ? 0 : 1; // colour space: sGray / sRGB
  h[2] = 1; // simplex
  h[3] = 4; // normal quality
  v.setUint32(12, p.width);
  v.setUint32(16, p.height);
  v.setUint32(20, p.dpi);
  return h;
}

const CHUNK = 1 << 20;

// Compresses the lines of one page, fed in bands of RGBA pixels (as
// getImageData returns them; alpha is ignored).
export class UrfLineEncoder {
  private cur: Uint32Array;
  private next: Uint32Array;
  private have = false;
  private repeat = 0;
  private buf = new Uint8Array(CHUNK + 1024);
  private pos = 0;
  private chunks: Uint8Array[] = [];
  private readonly white: number;
  lines = 0;
  bytes = 0;

  readonly width: number;
  readonly gray: boolean;

  constructor(width: number, gray: boolean) {
    this.width = width;
    this.gray = gray;
    this.cur = new Uint32Array(width);
    this.next = new Uint32Array(width);
    this.white = gray ? 0xff : 0xffffff;
  }

  // rgba: rows * width * 4 bytes
  addRows(rgba: Uint8Array | Uint8ClampedArray, rows: number): void {
    const w = this.width;
    const px = new Uint32Array(rgba.buffer, rgba.byteOffset, rows * w);
    for (let y = 0; y < rows; y++) {
      const row = this.next;
      const o = y * w;
      if (this.gray) {
        for (let x = 0; x < w; x++) {
          const v = px[o + x];
          // Rec. 601 luma; white stays 255
          row[x] = (77 * (v & 0xff) + 150 * ((v >>> 8) & 0xff) + 29 * ((v >>> 16) & 0xff)) >>> 8;
        }
      } else {
        for (let x = 0; x < w; x++) row[x] = px[o + x] & 0xffffff;
      }
      this.pushLine();
    }
  }

  // a line of white
  addWhite(rows: number): void {
    for (let y = 0; y < rows; y++) {
      this.next.fill(this.white);
      this.pushLine();
    }
  }

  private pushLine(): void {
    this.lines++;
    if (this.have && this.repeat < 255 && equal(this.cur, this.next)) {
      this.repeat++;
      return;
    }
    if (this.have) this.emit();
    const t = this.cur;
    this.cur = this.next;
    this.next = t;
    this.have = true;
    this.repeat = 0;
  }

  // Ends the page; returns its compressed lines.
  finish(): Uint8Array[] {
    if (this.have) this.emit();
    this.have = false;
    this.flushChunk();
    const out = this.chunks;
    this.chunks = [];
    return out;
  }

  // Compressed data so far (lets the caller hand it to a Blob early).
  take(): Uint8Array[] {
    const out = this.chunks;
    this.chunks = [];
    return out;
  }

  private flushChunk(): void {
    if (this.pos) {
      this.chunks.push(this.buf.slice(0, this.pos));
      this.bytes += this.pos;
      this.pos = 0;
    }
  }

  private emit(): void {
    const line = this.cur;
    const w = this.width;
    const white = this.white;
    const gray = this.gray;
    // worst case: a packet byte per 128 pixels + the pixels + repeat + 128
    const worst = 2 + Math.ceil(w / 128) + w * (gray ? 1 : 3);
    if (this.pos + worst > this.buf.length) {
      this.flushChunk();
      if (worst > this.buf.length) this.buf = new Uint8Array(worst);
    }
    const b = this.buf;
    let p = this.pos;
    b[p++] = this.repeat;
    let last = w - 1;
    while (last >= 0 && line[last] === white) last--;
    let i = 0;
    while (i <= last) {
      const v = line[i];
      let j = i + 1;
      while (j <= last && j - i < 128 && line[j] === v) j++;
      if (j - i >= 2) {
        b[p++] = j - i - 1;
        if (gray) b[p++] = v;
        else {
          b[p++] = v & 0xff;
          b[p++] = (v >>> 8) & 0xff;
          b[p++] = v >>> 16;
        }
        i = j;
        continue;
      }
      // literals up to the next pair of equal pixels
      j = i + 1;
      while (j <= last && j - i < 128 && !(j < last && line[j] === line[j + 1])) j++;
      const n = j - i;
      b[p++] = n === 1 ? 0 : 257 - n;
      for (let k = i; k < j; k++) {
        const u = line[k];
        if (gray) b[p++] = u;
        else {
          b[p++] = u & 0xff;
          b[p++] = (u >>> 8) & 0xff;
          b[p++] = u >>> 16;
        }
      }
      i = j;
    }
    if (last < w - 1) b[p++] = 128;
    this.pos = p;
    if (this.pos >= CHUNK) this.flushChunk();
  }
}

function equal(a: Uint32Array, b: Uint32Array): boolean {
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}
