// Finds the object on a scan (a card or a photo on the glass): the box of the
// pixels that differ from the background, ignoring specks. The background is
// estimated from the image border (the scanner's lid comes out light grey).

export interface Box {
  x: number; // normalized 0..1
  y: number;
  w: number;
  h: number;
}

export function detectContent(px: Uint8ClampedArray, w: number, h: number): Box | null {
  const lum = new Uint8Array(w * h);
  const sat = new Uint8Array(w * h);
  for (let i = 0, j = 0; i < w * h; i++, j += 4) {
    const r = px[j],
      g = px[j + 1],
      b = px[j + 2];
    lum[i] = (77 * r + 150 * g + 29 * b) >> 8;
    sat[i] = Math.max(r, g, b) - Math.min(r, g, b);
  }
  // background: median of the border
  const border: number[] = [];
  for (let x = 0; x < w; x++) border.push(lum[x], lum[(h - 1) * w + x]);
  for (let y = 0; y < h; y++) border.push(lum[y * w], lum[y * w + w - 1]);
  border.sort((a, b) => a - b);
  const bg = border[border.length >> 1];
  const thr = 28;
  const fg = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) fg[i] = Math.abs(lum[i] - bg) > thr || sat[i] > 40 ? 1 : 0;

  // rows / columns with enough foreground
  const rows = new Uint32Array(h);
  const cols = new Uint32Array(w);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++)
      if (fg[y * w + x]) {
        rows[y]++;
        cols[x]++;
      }
  const minRow = Math.max(2, w * 0.02);
  const minCol = Math.max(2, h * 0.02);
  const span = (a: Uint32Array, min: number): [number, number] | null => {
    // the longest run of busy lines, bridging small gaps (e.g. blank card areas)
    let best: [number, number] | null = null;
    let start = -1,
      gap = 0;
    const maxGap = Math.max(2, Math.round(a.length * 0.04));
    for (let i = 0; i <= a.length; i++) {
      const busy = i < a.length && a[i] >= min;
      if (busy) {
        if (start < 0) start = i;
        gap = 0;
      } else if (start >= 0 && (++gap > maxGap || i === a.length)) {
        const end = i - gap;
        if (!best || end - start > best[1] - best[0]) best = [start, end];
        start = -1;
        gap = 0;
      }
    }
    return best;
  };
  const ys = span(rows, minRow);
  const xs = span(cols, minCol);
  if (!ys || !xs) return null;
  const [x0, x1] = xs;
  const [y0, y1] = ys;
  if (x1 - x0 < w * 0.05 || y1 - y0 < h * 0.05) return null;
  return { x: x0 / w, y: y0 / h, w: (x1 - x0 + 1) / w, h: (y1 - y0 + 1) / h };
}

// Grows a box to an aspect ratio (w / h, in pixels of a wImg x hImg image)
// around its centre, staying within the image when it can.
export function fitAspect(b: Box, aspect: number, wImg: number, hImg: number): Box {
  let w = b.w * wImg,
    h = b.h * hImg;
  if (w / h > aspect) h = w / aspect;
  else w = h * aspect;
  const scale = Math.min(1, wImg / w, hImg / h);
  w *= scale;
  h *= scale;
  const cx = (b.x + b.w / 2) * wImg,
    cy = (b.y + b.h / 2) * hImg;
  const x = Math.min(Math.max(0, cx - w / 2), wImg - w);
  const y = Math.min(Math.max(0, cy - h / 2), hImg - h);
  return { x: x / wImg, y: y / hImg, w: w / wImg, h: h / hImg };
}
