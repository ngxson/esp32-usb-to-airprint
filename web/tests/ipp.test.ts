import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decodeResponse, encodeRequest, OP } from '../src/lib/ipp.ts';
import { imageDpi } from '../src/lib/imageinfo.ts';
import { detectContent } from '../src/lib/autocrop.ts';

test('request layout', () => {
  const r = encodeRequest(OP.PRINT_JOB, [['job-name', { tag: 0x42, value: 'a' }], ['job-id', { tag: 0x21, value: 258 }]]);
  assert.deepEqual([...r.subarray(0, 4)], [2, 0, 0, 2]);
  assert.equal(r[8], 1); // operation group
  assert.equal(r[r.length - 1], 3); // end
  // decodes as a response would
  const d = decodeResponse(r.buffer as ArrayBuffer);
  assert.deepEqual(d.attrs['attributes-charset'], ['utf-8']);
  assert.deepEqual(d.attrs['job-id'], [258]);
});

test('JFIF dpi', () => {
  const j = new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 0, 16, 0x4a, 0x46, 0x49, 0x46, 0, 1, 1, 1, 1, 44, 1, 44, 0, 0, 0xff, 0xda]);
  assert.equal(imageDpi(j.buffer), 300);
  j[13] = 0; j[14] = 72; // 72 dpi: a camera, unknown
  assert.equal(imageDpi(j.buffer), null);
});

test('auto-crop finds a card on a grey lid', () => {
  const w = 200, h = 150;
  const px = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const card = x >= 20 && x < 120 && y >= 10 && y < 73;
      const v = card ? ((x * 7 + y * 3) % 90) + 40 : 228 + ((x + y) % 5);
      px.set([v, card ? v + 30 : v, v, 255], (y * w + x) * 4);
    }
  px.set([0, 0, 0, 255], (140 * w + 190) * 4); // a speck
  const b = detectContent(px, w, h)!;
  assert.ok(Math.abs(b.x * w - 20) <= 1 && Math.abs(b.y * h - 10) <= 1, JSON.stringify(b));
  assert.ok(Math.abs(b.w * w - 100) <= 2 && Math.abs(b.h * h - 63) <= 2, JSON.stringify(b));
});
