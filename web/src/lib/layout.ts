// Paper sizes and where content goes on the sheet. Units: PostScript points
// (1/72 in), origin top left, y down.

export type PaperId = 'a4' | 'letter';
export type Fit = 'fit' | 'fill' | 'actual';
export type Orientation = 'auto' | 'portrait' | 'landscape';
export type Rotation = 0 | 90 | 180 | 270; // clockwise

export const PAPERS: Record<PaperId, { name: string; w: number; h: number; wIn: number; hIn: number }> = {
  a4: { name: 'A4', w: (210 / 25.4) * 72, h: (297 / 25.4) * 72, wIn: 210 / 25.4, hIn: 297 / 25.4 },
  letter: { name: 'Letter', w: 612, h: 792, wIn: 8.5, hIn: 11 },
};

// what the MG2500 cannot print (canon_bjl: 10/10/9/15 pt)
export const MARGINS = { left: 10, right: 10, top: 9, bottom: 15 };

// ID-1 cards (bank cards, most ID cards)
export const CARD_MM = { w: 85.6, h: 53.98 };
export const MM = 72 / 25.4;

// The raster size the server expects (canon_bjl: A4 4961 x 7016 at 600 dpi).
export function paperPixels(paper: PaperId, dpi: number): { w: number; h: number } {
  const p = PAPERS[paper];
  return { w: Math.round(p.wIn * dpi), h: Math.round(p.hIn * dpi) };
}

// A4 everywhere, whatever the browser's language
export function defaultPaper(): PaperId {
  return 'a4';
}

export interface Placement {
  rot: Rotation;
  scale: number; // content units -> points
  x: number; // top left of the rotated, scaled content on the sheet
  y: number;
  w: number; // rotated, scaled size
  h: number;
}

export function rotatedSize(w: number, h: number, rot: Rotation): [number, number] {
  return rot === 90 || rot === 270 ? [h, w] : [w, h];
}

// content: w x h points (as the source says, e.g. a PDF page or a scan at its dpi)
export function place(
  w: number,
  h: number,
  paper: PaperId,
  fit: Fit,
  orientation: Orientation,
): Placement {
  const P = PAPERS[paper];
  const landscape = orientation === 'landscape' || (orientation === 'auto' && w > h * 1.02);
  // landscape: turned a quarter counter-clockwise, as PostScript does
  const rot: Rotation = landscape ? 270 : 0;
  const [rw, rh] = rotatedSize(w, h, rot);
  const aw = P.w - MARGINS.left - MARGINS.right;
  const ah = P.h - MARGINS.top - MARGINS.bottom;
  let scale: number, x: number, y: number;
  if (fit === 'fit') {
    scale = Math.min(aw / rw, ah / rh);
    x = MARGINS.left + (aw - rw * scale) / 2;
    y = MARGINS.top + (ah - rh * scale) / 2;
  } else if (fit === 'fill') {
    scale = Math.max(P.w / rw, P.h / rh);
    x = (P.w - rw * scale) / 2;
    y = (P.h - rh * scale) / 2;
  } else {
    // actual size: a full-page scan lands exactly where the original was
    scale = 1;
    x = Math.max(0, (P.w - rw) / 2);
    y = Math.max(0, (P.h - rh) / 2);
    if (rw > P.w) x = 0;
    if (rh > P.h) y = 0;
  }
  return { rot, scale, x, y, w: rw * scale, h: rh * scale };
}

// 2D matrix [a, b, c, d, e, f] taking content units to sheet pixels at k px/pt
export function contentMatrix(
  rot: Rotation,
  cw: number,
  ch: number,
  pl: { scale: number; x: number; y: number },
  k: number,
  yOffset = 0,
): [number, number, number, number, number, number] {
  const s = pl.scale * k;
  const ex = pl.x * k;
  const ey = pl.y * k - yOffset;
  switch (rot) {
    case 90:
      return [0, s, -s, 0, ex + ch * s, ey];
    case 180:
      return [-s, 0, 0, -s, ex + cw * s, ey + ch * s];
    case 270:
      return [0, -s, s, 0, ex, ey + cw * s];
    default:
      return [s, 0, 0, s, ex, ey];
  }
}

// "1-3, 5" -> [0, 1, 2, 4] (0-based, in order, within count); null if invalid
export function parseRanges(text: string, count: number): number[] | null {
  const t = text.trim();
  if (!t) return Array.from({ length: count }, (_, i) => i);
  const out: number[] = [];
  for (const part of t.split(',')) {
    const m = part.trim().match(/^(\d+)\s*(?:-\s*(\d*))?$/);
    if (!m) return null;
    const a = parseInt(m[1], 10);
    const b = m[2] === undefined ? a : m[2] === '' ? count : parseInt(m[2], 10);
    if (a < 1 || b < a || a > count) return null;
    for (let i = a; i <= Math.min(b, count); i++) out.push(i - 1);
  }
  return out;
}
