// The PDF writer: every xref offset points at its object, and pdf.js reads
// the document back (pages, sizes, one image per page).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { buildPdf } from '../src/lib/pdfwriter.ts';

// a 1x1 baseline JPEG
const JPEG = Uint8Array.from(
  Buffer.from(
    '/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAABAAEBAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxA=',
    'base64',
  ),
);

test('structure and pdf.js', async () => {
  const blob = buildPdf(
    [
      { w: 595.28, h: 841.89, images: [{ jpeg: JPEG, px: 1, py: 1, x: 10, y: 20, w: 300, h: 400 }] },
      { w: 300, h: 200, images: [{ jpeg: JPEG, px: 1, py: 1, x: 0, y: 0, w: 150, h: 100 }, { jpeg: JPEG, px: 1, py: 1, x: 150, y: 100, w: 150, h: 100 }] },
    ],
    'Test (doc)',
  );
  const data = new Uint8Array(await blob.arrayBuffer());
  const text = Buffer.from(data).toString('latin1');
  const startxref = parseInt(text.match(/startxref\n(\d+)/)![1], 10);
  assert.ok(text.startsWith('xref', startxref));
  const entries = [...text.slice(startxref).matchAll(/^(\d{10}) 00000 n $/gm)].map((m) => parseInt(m[1], 10));
  entries.forEach((off, i) => assert.ok(text.startsWith(`${i + 1} 0 obj`, off), `object ${i + 1} at ${off}`));
  mkdirSync('../host/out', { recursive: true });
  writeFileSync('../host/out/web-test.pdf', data);

  const pdfjs = await import('pdfjs-dist/legacy/build/pdf.mjs');
  const task = pdfjs.getDocument({ data: new Uint8Array(readFileSync('../host/out/web-test.pdf')) });
  const doc = await task.promise;
  assert.equal(doc.numPages, 2);
  const p2 = await doc.getPage(2);
  const v = p2.getViewport({ scale: 1 });
  assert.deepEqual([v.width, v.height], [300, 200]);
  const ops = await p2.getOperatorList();
  const images = ops.fnArray.filter((f: number) => f === pdfjs.OPS.paintImageXObject).length;
  assert.equal(images, 2);
  await task.destroy();
});
