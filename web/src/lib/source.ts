// Documents to draw: PDF pages (pdf.js, loaded on first use) and images.
// Content units are points; an image of unknown resolution gets a nominal
// one that makes it A4-sized (`known: false`).

import { t } from './i18n.ts';
import type { PDFDocumentProxy } from 'pdfjs-dist/legacy/build/pdf.mjs';
import { contentMatrix, PAPERS, type Rotation } from './layout';

type PdfJs = typeof import('pdfjs-dist/legacy/build/pdf.mjs');
let lib: Promise<PdfJs> | null = null;

export function pdfjs(): Promise<PdfJs> {
  lib ||= Promise.all([
    import('pdfjs-dist/legacy/build/pdf.mjs'),
    import('pdfjs-dist/legacy/build/pdf.worker.min.mjs?url'),
  ]).then(([m, w]) => {
    m.GlobalWorkerOptions.workerSrc = w.default;
    return m;
  });
  return lib;
}

export interface PageSize {
  w: number; // points, after the page's own rotation
  h: number;
  known: boolean; // real physical size (PDF, or an image with a dpi)
}

export interface Source {
  kind: 'pdf' | 'image';
  pages: PageSize[];
  dpi: number | null; // images
  // Draws a page rotated by rot, s pixels per point, its rotated top left at
  // (tx, ty) canvas pixels. PDF pages repaint the whole canvas white first.
  draw(canvas: HTMLCanvasElement, page: number, rot: Rotation, s: number, tx: number, ty: number, print?: boolean): Promise<void>;
  close(): void;
}

export function isPdf(type: string, name = ''): boolean {
  return type === 'application/pdf' || /\.pdf$/i.test(name);
}

export async function openSource(blob: Blob, type: string, dpi: number | null = null): Promise<Source> {
  return isPdf(type) ? openPdf(blob) : openImage(blob, dpi);
}

async function openPdf(blob: Blob): Promise<Source> {
  const m = await pdfjs();
  const data = new Uint8Array(await blob.arrayBuffer());
  const task = m.getDocument({ data });
  let doc: PDFDocumentProxy;
  try {
    doc = await task.promise;
  } catch (e) {
    const name = (e as { name?: string }).name;
    throw new Error(name === 'PasswordException' ? t('the PDF is password protected') : t('not a readable PDF'));
  }
  const pages: PageSize[] = [];
  for (let i = 1; i <= doc.numPages; i++) {
    const p = await doc.getPage(i);
    const v = p.getViewport({ scale: 1 });
    pages.push({ w: v.width, h: v.height, known: true });
  }
  return {
    kind: 'pdf',
    pages,
    dpi: null,
    async draw(canvas, page, rot, s, tx, ty, print = false) {
      const p = await doc.getPage(page + 1);
      const viewport = p.getViewport({ scale: s, rotation: (p.rotate + rot) % 360 });
      await p.render({
        canvas,
        viewport,
        transform: [1, 0, 0, 1, tx, ty],
        intent: print ? 'print' : 'display',
        background: '#ffffff',
      }).promise;
    },
    close() {
      task.destroy();
    },
  };
}

async function openImage(blob: Blob, dpi: number | null): Promise<Source> {
  let bmp: ImageBitmap;
  try {
    bmp = await createImageBitmap(blob, { imageOrientation: 'from-image' });
  } catch {
    throw new Error(t('this browser cannot read that image'));
  }
  const nominal = Math.max(bmp.width, bmp.height) / PAPERS.a4.hIn; // fills A4
  const ptPerPx = 72 / (dpi || nominal);
  const size = { w: bmp.width * ptPerPx, h: bmp.height * ptPerPx, known: !!dpi };
  return {
    kind: 'image',
    pages: [size],
    dpi,
    async draw(canvas, _page, rot, s, tx, ty) {
      const ctx = canvas.getContext('2d')!;
      ctx.save();
      ctx.setTransform(...contentMatrix(rot, size.w, size.h, { scale: s, x: tx, y: ty }, 1));
      ctx.scale(ptPerPx, ptPerPx);
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = 'high';
      ctx.drawImage(bmp, 0, 0);
      ctx.restore();
    },
    close() {
      bmp.close();
    },
  };
}

// A few open documents, by key (file id): reopening a big PDF is slow.
const cache = new Map<string, Promise<Source>>();

export function cachedSource(key: string, open: () => Promise<Source>): Promise<Source> {
  let s = cache.get(key);
  if (s) {
    cache.delete(key);
    cache.set(key, s); // most recent last
    return s;
  }
  s = open();
  s.catch(() => cache.delete(key));
  cache.set(key, s);
  while (cache.size > 3) {
    const [k, old] = cache.entries().next().value!;
    cache.delete(k);
    old.then((o) => o.close()).catch(() => {});
  }
  return s;
}

export function forgetSource(key: string) {
  const s = cache.get(key);
  cache.delete(key);
  s?.then((o) => o.close()).catch(() => {});
}

export function newCanvas(w: number, h: number): HTMLCanvasElement {
  const c = document.createElement('canvas');
  c.width = Math.max(1, Math.round(w));
  c.height = Math.max(1, Math.round(h));
  return c;
}

export function whiteCanvas(w: number, h: number): HTMLCanvasElement {
  const c = newCanvas(w, h);
  const ctx = c.getContext('2d', { willReadFrequently: true })!;
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, c.width, c.height);
  return c;
}

// A page, rotated, at s px per point (whole, on its own canvas).
export async function renderPage(src: Source, page: number, rot: Rotation, s: number): Promise<HTMLCanvasElement> {
  const ps = src.pages[page];
  const [w, h] = rot % 180 ? [ps.h, ps.w] : [ps.w, ps.h];
  const c = whiteCanvas(w * s, h * s);
  await src.draw(c, page, rot, s, 0, 0);
  return c;
}

// Fits a page in max x max pixels.
export async function renderThumb(src: Source, page: number, max: number, rot: Rotation = 0): Promise<HTMLCanvasElement> {
  const ps = src.pages[page];
  const [w, h] = rot % 180 ? [ps.h, ps.w] : [ps.w, ps.h];
  return renderPage(src, page, rot, Math.min(max / w, max / h));
}

export function canvasBlob(c: HTMLCanvasElement, type = 'image/jpeg', quality = 0.85): Promise<Blob> {
  return new Promise((resolve, reject) =>
    c.toBlob((b) => (b ? resolve(b) : reject(new Error(t('cannot encode the image')))), type, quality),
  );
}
