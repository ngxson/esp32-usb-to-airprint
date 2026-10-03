// Printing from the browser: pages are rendered here, one band of lines at a
// time (phones limit the canvas size), compressed to URF and sent to the
// device as an IPP Print-Job, which streams them straight to the printer.

import { decodeResponse, printJobRequest, statusOk, statusText } from './ipp';
import { paperPixels, place, type Fit, type Orientation, type PaperId } from './layout';
import type { Source } from './source';
import { UrfLineEncoder, urfFileHeader, urfPageHeader } from './urf';

export interface PrintOptions {
  paper: PaperId;
  fit: Fit;
  orientation: Orientation;
  pages: number[]; // 0-based, in print order
  copies: number;
  gray: boolean;
  dpi: 300 | 600;
}

const BAND_PIXELS = 2_500_000; // per band canvas

export type Progress = (done: number, total: number) => void;

// The URF document. Copies reuse the rendered pages.
export async function renderJob(src: Source, o: PrintOptions, progress: Progress, signal: AbortSignal): Promise<Blob> {
  const { w: W, h: H } = paperPixels(o.paper, o.dpi);
  const k = o.dpi / 72;
  const band = Math.max(16, Math.floor(BAND_PIXELS / W));
  const canvas = document.createElement('canvas');
  canvas.width = W;
  canvas.height = band;
  const ctx = canvas.getContext('2d', { willReadFrequently: true, alpha: false })!;
  const header = urfPageHeader({ width: W, height: H, dpi: o.dpi, gray: o.gray });
  const pages: Blob[] = [];
  let done = 0;
  for (const i of o.pages) {
    const ps = src.pages[i];
    const pl = place(ps.w, ps.h, o.paper, o.fit, o.orientation);
    const top = Math.max(0, Math.floor(pl.y * k));
    const bottom = Math.min(H, Math.ceil((pl.y + pl.h) * k));
    const enc = new UrfLineEncoder(W, o.gray);
    const parts: BlobPart[] = [header as BlobPart];
    for (let y0 = 0; y0 < H; y0 += band) {
      if (signal.aborted) throw new DOMException('canceled', 'AbortError');
      const rows = Math.min(band, H - y0);
      if (y0 + rows <= top || y0 >= bottom) {
        enc.addWhite(rows);
      } else {
        ctx.setTransform(1, 0, 0, 1, 0, 0);
        ctx.fillStyle = '#fff';
        ctx.fillRect(0, 0, W, band);
        await src.draw(canvas, i, pl.rot, pl.scale * k, pl.x * k, pl.y * k - y0, true);
        enc.addRows(ctx.getImageData(0, 0, W, rows).data, rows);
      }
      for (const c of enc.take()) parts.push(c as BlobPart);
      progress(done + (y0 + rows) / H, o.pages.length);
      await new Promise((r) => setTimeout(r)); // keep the page responsive
    }
    for (const c of enc.finish()) parts.push(c as BlobPart);
    pages.push(new Blob(parts));
    done++;
  }
  canvas.width = canvas.height = 1;
  const all: BlobPart[] = [urfFileHeader(o.pages.length * o.copies) as BlobPart];
  for (let c = 0; c < o.copies; c++) all.push(...pages);
  return new Blob(all);
}

export interface SendResult {
  jobId: number;
}

// POSTs the job; upload(fraction) follows the transfer, which runs at the
// printer's pace (the server prints while it receives). Resolves when the
// server is done with it.
export function sendJob(
  doc: Blob,
  name: string,
  upload: (fraction: number) => void,
  signal: AbortSignal,
): Promise<SendResult> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/ipp/print');
    xhr.setRequestHeader('Content-Type', 'application/ipp');
    xhr.responseType = 'arraybuffer';
    xhr.upload.onprogress = (e) => e.lengthComputable && upload(e.loaded / e.total);
    xhr.onload = () => {
      if (xhr.status !== 200) return reject(new Error(`the printer answered HTTP ${xhr.status}`));
      try {
        const r = decodeResponse(xhr.response);
        if (!statusOk(r.status)) return reject(new Error(statusText(r)));
        resolve({ jobId: Number(r.attrs['job-id']?.[0] || 0) });
      } catch (e) {
        reject(e);
      }
    };
    xhr.onerror = () => reject(new Error('the connection to the printer was lost'));
    xhr.onabort = () => reject(new DOMException('canceled', 'AbortError'));
    signal.addEventListener('abort', () => xhr.abort());
    xhr.send(new Blob([printJobRequest(name, 'image/urf') as BlobPart, doc]));
  });
}
