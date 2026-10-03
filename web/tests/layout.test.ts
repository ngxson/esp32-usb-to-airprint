import { test } from 'node:test';
import assert from 'node:assert/strict';
import { contentMatrix, paperPixels, parseRanges, place, PAPERS } from '../src/lib/layout.ts';

const near = (a: number, b: number, eps = 1e-6) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b}`);

test('paper pixels match the server (canon_bjl)', () => {
  assert.deepEqual(paperPixels('a4', 600), { w: 4961, h: 7016 });
  assert.deepEqual(paperPixels('letter', 600), { w: 5100, h: 6600 });
  assert.deepEqual(paperPixels('a4', 300), { w: 2480, h: 3508 });
});

test('a full-page scan at actual size lands where the original was', () => {
  const pl = place(PAPERS.a4.w, PAPERS.a4.h, 'a4', 'actual', 'auto');
  assert.deepEqual([pl.rot, pl.scale], [0, 1]);
  near(pl.x, 0);
  near(pl.y, 0);
});

test('fit stays inside the printable area', () => {
  const pl = place(1000, 500, 'a4', 'fit', 'auto'); // landscape: turned
  assert.equal(pl.rot, 270);
  assert.ok(pl.x >= 10 - 1e-9 && pl.y >= 9 - 1e-9);
  assert.ok(pl.x + pl.w <= PAPERS.a4.w - 10 + 1e-9 && pl.y + pl.h <= PAPERS.a4.h - 15 + 1e-9);
});

test('rotation matrices map the content box onto the placement', () => {
  for (const rot of [0, 90, 180, 270] as const) {
    const cw = 200, ch = 100;
    const pl = { scale: 2, x: 30, y: 40 };
    const m = contentMatrix(rot, cw, ch, pl, 1);
    const pts = [[0, 0], [cw, 0], [0, ch], [cw, ch]].map(([x, y]) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]]);
    const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
    const [rw, rh] = rot % 180 ? [ch, cw] : [cw, ch];
    near(Math.min(...xs), 30);
    near(Math.min(...ys), 40);
    near(Math.max(...xs), 30 + rw * 2);
    near(Math.max(...ys), 40 + rh * 2);
  }
  // 270: the content's top edge goes to the left
  const m = contentMatrix(270, 200, 100, { scale: 1, x: 0, y: 0 }, 1);
  near(m[0] * 100 + m[4], 0);
});

test('page ranges', () => {
  assert.deepEqual(parseRanges('', 3), [0, 1, 2]);
  assert.deepEqual(parseRanges('1-2, 4', 5), [0, 1, 3]);
  assert.deepEqual(parseRanges('3-', 5), [2, 3, 4]);
  assert.equal(parseRanges('0', 5), null);
  assert.equal(parseRanges('6', 5), null);
  assert.equal(parseRanges('x', 5), null);
});
