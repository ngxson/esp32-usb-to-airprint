// URF encoder: round trip through a decoder written from the format
// description (not from the encoder), on images that exercise every packet
// kind. Also writes the documents for tools/check_urf.py (decoded by
// tools/raster_to_png.py, which the server's own decoder agrees with).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { UrfLineEncoder, urfFileHeader, urfPageHeader } from '../src/lib/urf.ts';

function decode(data: Uint8Array) {
  const dv = new DataView(data.buffer, data.byteOffset);
  assert.equal(new TextDecoder().decode(data.subarray(0, 7)), 'UNIRAST');
  const npages = dv.getUint32(8);
  let p = 12;
  const pages = [];
  for (let n = 0; n < npages; n++) {
    const bpp = data[p] / 8;
    const w = dv.getUint32(p + 12), h = dv.getUint32(p + 16), dpi = dv.getUint32(p + 20);
    p += 32;
    const px = new Uint8Array(w * h * bpp);
    let y = 0;
    while (y < h) {
      const rep = data[p++];
      const line = new Uint8Array(w * bpp);
      let x = 0;
      while (x < w * bpp) {
        const c = data[p++];
        if (c === 128) {
          line.fill(255, x);
          x = w * bpp;
        } else if (c < 128) {
          for (let k = 0; k <= c; k++) for (let b = 0; b < bpp; b++) line[x++] = data[p + b];
          p += bpp;
        } else {
          const cnt = (257 - c) * bpp;
          line.set(data.subarray(p, p + cnt), x);
          x += cnt;
          p += cnt;
        }
        assert.ok(x <= w * bpp, 'packet overruns the line');
      }
      for (let k = 0; k <= rep; k++) px.set(line, (y++) * w * bpp);
      assert.ok(y <= h, 'repeat overruns the page');
    }
    pages.push({ w, h, dpi, bpp, px });
  }
  assert.equal(p, data.length, 'trailing data');
  return pages;
}

function encode(rgba: Uint8Array, w: number, h: number, gray: boolean, band = 7) {
  const enc = new UrfLineEncoder(w, gray);
  const parts: Uint8Array[] = [urfFileHeader(1), urfPageHeader({ width: w, height: h, dpi: 300, gray })];
  for (let y = 0; y < h; y += band) {
    const rows = Math.min(band, h - y);
    enc.addRows(rgba.subarray(y * w * 4, (y + rows) * w * 4), rows);
    parts.push(...enc.take());
  }
  parts.push(...enc.finish());
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

// a page with white margins, runs, noise, a gradient and > 256 equal lines
function image(w: number, h: number) {
  const px = new Uint8Array(w * h * 4);
  let seed = 7;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) >> 16) & 255;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      let r = 255, g = 255, b = 255;
      if (y >= 10 && y < 40 && x >= 5 && x < w - 20) [r, g, b] = [rnd(), rnd(), rnd()]; // literals
      else if (y >= 40 && y < 60) [r, g, b] = [(x >> 3) * 8, 100, 200]; // short runs
      else if (y >= 60 && y < 400 && x < w / 2) [r, g, b] = [20, 30, 40]; // 340 equal lines
      else if (y >= 400 && y < 420) [r, g, b] = x % 2 ? [0, 0, 0] : [255, 255, 255]; // alternating
      else if (y >= 420 && y < 440 && x === w - 1) [r, g, b] = [1, 2, 3]; // last pixel only
      else if (y >= 440 && y < 460) [r, g, b] = x % 3 === 0 ? [9, 9, 9] : [9, 9, 9 + (y & 1)];
      px.set([r, g, b, 255], i);
    }
  return px;
}

const W = 333, H = 500;

test('colour round trip', () => {
  const src = image(W, H);
  const data = encode(src, W, H, false);
  const [pg] = decode(data);
  assert.deepEqual([pg.w, pg.h, pg.bpp, pg.dpi], [W, H, 3, 300]);
  for (let i = 0; i < W * H; i++)
    for (let c = 0; c < 3; c++) if (pg.px[i * 3 + c] !== src[i * 4 + c]) assert.fail(`pixel ${i} differs`);
  assert.ok(data.length < W * H * 3 * 0.6, `compresses (${data.length} bytes)`);
  mkdirSync('../host/out', { recursive: true });
  writeFileSync('../host/out/web-color.urf', data);
  writeFileSync('../host/out/web-color.rgba', src);
});

test('gray round trip', () => {
  const src = image(W, H);
  const data = encode(src, W, H, true, 64);
  const [pg] = decode(data);
  assert.equal(pg.bpp, 1);
  for (let i = 0; i < W * H; i++) {
    const v = (77 * src[i * 4] + 150 * src[i * 4 + 1] + 29 * src[i * 4 + 2]) >> 8;
    if (pg.px[i] !== v) assert.fail(`pixel ${i}: ${pg.px[i]} != ${v}`);
  }
  writeFileSync('../host/out/web-gray.urf', data);
});

test('white page: one byte per 256 lines', () => {
  const enc = new UrfLineEncoder(4961, false);
  enc.addWhite(7016);
  const data = enc.finish().reduce((n, p) => n + p.length, 0);
  assert.equal(data, Math.ceil(7016 / 256) * 2);
});

test('a line of 4961 different pixels', () => {
  const w = 4961;
  const src = new Uint8Array(w * 4);
  for (let x = 0; x < w; x++) src.set([x & 255, (x >> 8) & 255, (x * 7) & 255, 255], x * 4);
  const [pg] = decode(encode(src, w, 1, false));
  for (let x = 0; x < w; x++) assert.equal(pg.px[x * 3], x & 255);
});
