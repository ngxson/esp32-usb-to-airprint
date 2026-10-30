// A minimal PDF writer: pages made of JPEG images (what the Process tab
// exports). Images are embedded as they are (DCTDecode), no re-encoding.

import { t } from './i18n.ts';

export interface PdfImage {
  jpeg: Uint8Array;
  px: number; // pixel size
  py: number;
  x: number; // placement in points, origin top left
  y: number;
  w: number;
  h: number;
}

export interface PdfPage {
  w: number; // points
  h: number;
  images: PdfImage[];
}

const enc = new TextEncoder();
const n2 = (v: number) => (Math.round(v * 100) / 100).toString();

export function buildPdf(pages: PdfPage[], title = t('Document')): Blob {
  const parts: BlobPart[] = [];
  const offsets: number[] = [];
  let pos = 0;
  const put = (x: string | Uint8Array) => {
    const b = typeof x === 'string' ? enc.encode(x) : x;
    parts.push(b as BlobPart);
    pos += b.length;
  };
  const obj = (id: number, body: string) => {
    offsets[id] = pos;
    put(`${id} 0 obj\n${body}\nendobj\n`);
  };

  put('%PDF-1.4\n%\xe2\xe3\xcf\xd3\n');
  // ids: 1 catalog, 2 pages, 3 info, then per page: page, content, images
  let next = 4;
  const pageIds: number[] = [];
  const plan = pages.map((pg) => {
    const id = next++;
    const content = next++;
    const imgs = pg.images.map(() => next++);
    pageIds.push(id);
    return { pg, id, content, imgs };
  });
  obj(1, '<< /Type /Catalog /Pages 2 0 R >>');
  obj(2, `<< /Type /Pages /Kids [${pageIds.map((i) => `${i} 0 R`).join(' ')}] /Count ${pages.length} >>`);
  const safeTitle = title.replace(/[\\()]/g, (c) => '\\' + c).replace(/[^\x20-\x7e]/g, '?');
  // PDF text strings use UTF-16BE with a BOM for non-ASCII titles.
  const pdfTitle = /[^\x20-\x7e]/.test(title)
    ? '<feff' + Array.from({ length: title.length }, (_, i) => title.charCodeAt(i).toString(16).padStart(4, '0')).join('') + '>'
    : `(${safeTitle})`;
  obj(3, `<< /Title ${pdfTitle} /Producer (Printer web UI) >>`);
  for (const { pg, id, content, imgs } of plan) {
    const xobjs = imgs.map((iid, i) => `/Im${i} ${iid} 0 R`).join(' ');
    obj(
      id,
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${n2(pg.w)} ${n2(pg.h)}] ` +
        `/Resources << /XObject << ${xobjs} >> >> /Contents ${content} 0 R >>`,
    );
    // PDF's origin is bottom left
    const ops = pg.images
      .map((im, i) => `q ${n2(im.w)} 0 0 ${n2(im.h)} ${n2(im.x)} ${n2(pg.h - im.y - im.h)} cm /Im${i} Do Q`)
      .join('\n');
    const stream = enc.encode(ops);
    offsets[content] = pos;
    put(`${content} 0 obj\n<< /Length ${stream.length} >>\nstream\n`);
    put(stream);
    put('\nendstream\nendobj\n');
    pg.images.forEach((im, i) => {
      const iid = imgs[i];
      offsets[iid] = pos;
      put(
        `${iid} 0 obj\n<< /Type /XObject /Subtype /Image /Width ${im.px} /Height ${im.py} ` +
          `/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length ${im.jpeg.length} >>\nstream\n`,
      );
      put(im.jpeg);
      put('\nendstream\nendobj\n');
    });
  }
  const xref = pos;
  let x = `xref\n0 ${next}\n0000000000 65535 f \n`;
  for (let i = 1; i < next; i++) x += `${String(offsets[i]).padStart(10, '0')} 00000 n \n`;
  put(x);
  put(`trailer\n<< /Size ${next} /Root 1 0 R /Info 3 0 R >>\nstartxref\n${xref}\n%%EOF\n`);
  return new Blob(parts, { type: 'application/pdf' });
}
