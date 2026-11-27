// A test page drawn here and printed like any document (renderJob + sendJob),
// so it works for every printer driver: the printable area's border, the
// printer's name, text sizes, a gray ramp, fine lines and patterns.

import { PAPERS, type PaperId } from './layout';
import type { Source } from './source';

const MARGIN = 14.17; // 5 mm, in points

export function testPageSource(paper: PaperId, info: string[]): Source {
  const { w, h } = PAPERS[paper];
  return {
    kind: 'image',
    pages: [{ w, h, known: true }],
    dpi: null,
    async draw(canvas, _page, _rot, s, tx, ty) {
      const c = canvas.getContext('2d')!;
      c.save();
      c.setTransform(s, 0, 0, s, tx, ty);
      drawPage(c, w, h, info);
      c.restore();
    },
    close() {},
  };
}

function drawPage(c: CanvasRenderingContext2D, w: number, h: number, info: string[]) {
  c.fillStyle = '#000';
  c.strokeStyle = '#000';
  // the printable area (5 mm from every edge)
  c.lineWidth = 1;
  c.strokeRect(MARGIN, MARGIN, w - 2 * MARGIN, h - 2 * MARGIN);
  const x = 50;
  let y = 70;
  c.font = 'bold 28px sans-serif';
  c.fillText('Test page', x, y);
  c.font = '12px sans-serif';
  for (const line of info) {
    y += 18;
    c.fillText(line, x, y);
  }

  // text sizes
  y += 34;
  for (const size of [6, 8, 10, 12, 16, 24]) {
    c.font = `${size}px sans-serif`;
    const sample = size < 20 ? 'The quick brown fox jumps over the lazy dog 0123456789' : 'Quick brown fox 0123456789';
    c.fillText(`${size} pt: ${sample}`, x, y);
    y += size + 8;
  }

  // gray ramp, 0 to 100 % in 10 % steps
  y += 20;
  c.font = '9px sans-serif';
  const step = (w - 2 * x) / 11;
  for (let i = 0; i <= 10; i++) {
    const v = Math.round(255 * (1 - i / 10));
    c.fillStyle = `rgb(${v},${v},${v})`;
    c.fillRect(x + i * step, y, step, 50);
    c.fillStyle = '#000';
    c.fillText(`${i * 10}%`, x + i * step + 4, y + 62);
  }
  c.strokeRect(x, y, 11 * step, 50);

  // a smooth ramp
  y += 80;
  const grad = c.createLinearGradient(x, 0, w - x, 0);
  grad.addColorStop(0, '#fff');
  grad.addColorStop(1, '#000');
  c.fillStyle = grad;
  c.fillRect(x, y, w - 2 * x, 30);
  c.strokeRect(x, y, w - 2 * x, 30);

  // fine lines: 0.25, 0.5, 1 and 2 pt, horizontal and vertical
  y += 60;
  c.fillStyle = '#000';
  const widths = [0.25, 0.5, 1, 2];
  widths.forEach((lw, i) => {
    c.lineWidth = lw;
    c.beginPath();
    c.moveTo(x, y + i * 14);
    c.lineTo(x + 220, y + i * 14);
    c.stroke();
    c.beginPath();
    c.moveTo(x + 260 + i * 20, y - 6);
    c.lineTo(x + 260 + i * 20, y + 48);
    c.stroke();
  });
  c.font = '9px sans-serif';
  c.fillText('0.25 / 0.5 / 1 / 2 pt', x + 360, y + 24);

  // checkerboards (1 and 2 pt squares) and a circle
  y += 80;
  for (const [i, sq] of [1, 2].entries()) {
    const ox = x + i * 120;
    for (let r = 0; r < 80 / sq; r++)
      for (let k = 0; k < 80 / sq; k++) if ((r + k) % 2 === 0) c.fillRect(ox + k * sq, y + r * sq, sq, sq);
    c.strokeRect(ox, y, 80, 80);
  }
  c.lineWidth = 1;
  c.beginPath();
  c.arc(x + 330, y + 40, 40, 0, 2 * Math.PI);
  c.moveTo(x + 290, y + 40);
  c.lineTo(x + 370, y + 40);
  c.moveTo(x + 330, y);
  c.lineTo(x + 330, y + 80);
  c.stroke();

  // corner marks: the printable area's corners should all be on the sheet
  c.font = '8px sans-serif';
  c.fillText('top left', MARGIN + 4, MARGIN + 10);
  c.fillText('bottom right', w - MARGIN - 52, h - MARGIN - 4);
}
