// The Process tab: a list of pages put together from Files (rotated,
// cropped) and ID card copies, exported as one PDF.

import { t } from './i18n.svelte';
import * as db from './db';
import { CARD_MM, MM, PAPERS, type PaperId, type Rotation } from './layout';
import { buildPdf, type PdfImage, type PdfPage } from './pdfwriter';
import { canvasBlob, newCanvas, renderPage, whiteCanvas, type Source } from './source';
import { addFile, fileSource, findFile, stamp, type StoredFile } from './files.svelte';

export interface Crop {
  x: number; // normalized, in the rotated page
  y: number;
  w: number;
  h: number;
}

export interface PageRef {
  fileId: string;
  page: number;
  rot: Rotation;
  crop: Crop | null;
}

export type Item =
  | { id: string; kind: 'page'; ref: PageRef }
  | { id: string; kind: 'card'; front: PageRef | null; back: PageRef | null; vertical: boolean };

class Work {
  items = $state.raw<Item[]>([]);
  loaded = $state(false);
  version = $state(0); // bumps when any item changes (thumbnails)
}
export const work = new Work();

export async function loadWork() {
  const saved = await db.kvGet<Item[]>('workspace').catch(() => undefined);
  work.items = saved || [];
  work.loaded = true;
}

function save() {
  work.version++;
  db.kvSet('workspace', work.items).catch(() => {});
}

export function setItems(items: Item[]) {
  work.items = items;
  save();
}

export function updateItem(id: string, fn: (it: Item) => Item) {
  setItems(work.items.map((it) => (it.id === id ? fn(it) : it)));
}

// every page of the files, in order
export function pageItems(fs: StoredFile[]): Item[] {
  const items: Item[] = [];
  for (const f of fs)
    for (let p = 0; p < (f.pages || 1); p++)
      items.push({ id: db.newId('p'), kind: 'page', ref: { fileId: f.id, page: p, rot: 0, crop: null } });
  return items;
}

export async function addFilesToWork(fs: StoredFile[]) {
  const items = pageItems(fs);
  setItems([...work.items, ...items]);
  return items.length;
}

export function addCard(): string {
  const id = db.newId('c');
  setItems([...work.items, { id, kind: 'card', front: null, back: null, vertical: false }]);
  return id;
}

export function rotateRef(r: PageRef, by: 90 | -90): PageRef {
  const rot = ((r.rot + by + 360) % 360) as Rotation;
  let crop = r.crop;
  if (crop) {
    // keep the same part of the page: turn the crop box with it
    crop = by === 90 ? { x: 1 - crop.y - crop.h, y: crop.x, w: crop.h, h: crop.w } : { x: crop.y, y: 1 - crop.x - crop.w, w: crop.h, h: crop.w };
  }
  return { ...r, rot, crop };
}

export async function refSource(r: PageRef): Promise<{ src: Source; file: StoredFile } | null> {
  const file = findFile(r.fileId);
  if (!file) return null;
  return { src: await fileSource(file), file };
}

// Size in points of the cropped, rotated page; known: real size (not nominal).
export async function refSize(r: PageRef): Promise<{ w: number; h: number; known: boolean } | null> {
  const s = await refSource(r);
  if (!s) return null;
  const ps = s.src.pages[r.page] || s.src.pages[0];
  let [w, h] = r.rot % 180 ? [ps.h, ps.w] : [ps.w, ps.h];
  if (r.crop) {
    w *= r.crop.w;
    h *= r.crop.h;
  }
  return { w, h, known: ps.known };
}

// The cropped, rotated page at s px per point (at most maxPx on a side).
export async function renderRef(r: PageRef, s: number, maxPx = 6000): Promise<HTMLCanvasElement | null> {
  const st = await refSource(r);
  if (!st) return null;
  const ps = st.src.pages[r.page] || st.src.pages[0];
  const c = r.crop || { x: 0, y: 0, w: 1, h: 1 };
  const [fw, fh] = r.rot % 180 ? [ps.h, ps.w] : [ps.w, ps.h];
  s = Math.min(s, maxPx / (fw * c.w), maxPx / (fh * c.h));
  // render the whole page, then cut (fine for pages and scans)
  const full = await renderPage(st.src, Math.min(r.page, st.src.pages.length - 1), r.rot, s);
  if (!r.crop) return full;
  const out = newCanvas(fw * c.w * s, fh * c.h * s);
  out.getContext('2d')!.drawImage(full, c.x * full.width, c.y * full.height, c.w * full.width, c.h * full.height, 0, 0, out.width, out.height);
  full.width = full.height = 1;
  return out;
}

export type ExportPaper = PaperId | 'original';

export interface Sheet {
  w: number;
  h: number;
  parts: { ref: PageRef; x: number; y: number; w: number; h: number }[];
}

// Where everything goes, in points
export async function layoutItem(it: Item, paper: ExportPaper, fallback: PaperId): Promise<Sheet | null> {
  if (it.kind === 'card') {
    const P = PAPERS[paper === 'original' ? fallback : paper];
    const [cw, ch] = it.vertical ? [CARD_MM.h * MM, CARD_MM.w * MM] : [CARD_MM.w * MM, CARD_MM.h * MM];
    const parts: Sheet['parts'] = [];
    // front in the upper half, back in the lower half
    for (const [ref, cy] of [
      [it.front, P.h / 4],
      [it.back, (3 * P.h) / 4],
    ] as const) {
      const sz = ref && (await refSize(ref));
      if (!ref || !sz) continue;
      // the crop has the card's shape; anything else is fitted into it
      const s = Math.min(cw / sz.w, ch / sz.h);
      const w = sz.w * s,
        h = sz.h * s;
      parts.push({ ref, x: (P.w - w) / 2, y: cy - h / 2, w, h });
    }
    return { w: P.w, h: P.h, parts };
  }
  const sz = await refSize(it.ref);
  if (!sz) return null;
  if (paper === 'original') return { w: sz.w, h: sz.h, parts: [{ ref: it.ref, x: 0, y: 0, w: sz.w, h: sz.h }] };
  // the sheet turns with the page: a landscape page gets a landscape sheet
  const P0 = PAPERS[paper];
  const P = sz.w > sz.h ? { w: P0.h, h: P0.w } : P0;
  // real size when it fits, else as large as the sheet allows
  let s = Math.min(P.w / sz.w, P.h / sz.h);
  if (sz.known && s >= 1) s = 1;
  const w = sz.w * s,
    h = sz.h * s;
  return { w: P.w, h: P.h, parts: [{ ref: it.ref, x: (P.w - w) / 2, y: (P.h - h) / 2, w, h }] };
}

// A sheet as one picture (thumbnails, previews)
export async function renderSheet(sheet: Sheet, s: number): Promise<HTMLCanvasElement> {
  const c = whiteCanvas(sheet.w * s, sheet.h * s);
  const ctx = c.getContext('2d')!;
  for (const p of sheet.parts) {
    const sz = await refSize(p.ref);
    const img = sz && (await renderRef(p.ref, (p.w * s) / sz.w));
    if (!img) continue;
    ctx.drawImage(img, p.x * s, p.y * s, p.w * s, p.h * s);
    if (sheet.parts.length > 1) {
      ctx.strokeStyle = 'rgba(0,0,0,.15)';
      ctx.strokeRect(p.x * s, p.y * s, p.w * s, p.h * s);
    }
  }
  return c;
}

export async function exportPdf(
  items: Item[],
  opts: { paper: ExportPaper; dpi: number; fallback: PaperId },
  progress: (done: number, total: number) => void,
): Promise<StoredFile> {
  const pages: PdfPage[] = [];
  let done = 0;
  for (const it of items) {
    const sheet = await layoutItem(it, opts.paper, opts.fallback);
    if (!sheet) continue;
    const images: PdfImage[] = [];
    for (const p of sheet.parts) {
      // the part's size on paper decides its resolution
      const sz = await refSize(p.ref);
      if (!sz) continue;
      const img = await renderRef(p.ref, (opts.dpi / 72) * (p.w / sz.w));
      if (!img) continue;
      const jpeg = new Uint8Array(await (await canvasBlob(img, 'image/jpeg', 0.88)).arrayBuffer());
      images.push({ jpeg, px: img.width, py: img.height, x: p.x, y: p.y, w: p.w, h: p.h });
      img.width = img.height = 1;
    }
    pages.push({ w: sheet.w, h: sheet.h, images });
    progress(++done, items.length);
  }
  if (!pages.length) throw new Error(t('nothing to export'));
  const name = t('Document {stamp}.pdf', { stamp: stamp() });
  const blob = buildPdf(pages, name.replace(/\.pdf$/, ''));
  return addFile({ name, blob, source: 'process' });
}

export function itemFiles(it: Item): string[] {
  return it.kind === 'page' ? [it.ref.fileId] : [it.front?.fileId, it.back?.fileId].filter((x): x is string => !!x);
}
