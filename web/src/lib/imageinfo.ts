// Resolution of an image file, so scans print and export at their real size.
// JPEG: the JFIF density (our scans carry their dpi there). PNG: pHYs.
// Below 75 dpi (72 is what cameras write) means "unknown".

export function imageDpi(buf: ArrayBuffer): number | null {
  const b = new Uint8Array(buf);
  const d = new DataView(buf);
  if (b[0] === 0xff && b[1] === 0xd8) {
    let p = 2;
    while (p + 4 <= b.length && b[p] === 0xff) {
      const marker = b[p + 1];
      const len = d.getUint16(p + 2);
      if (marker === 0xe0 && len >= 14 && String.fromCharCode(...b.subarray(p + 4, p + 9)) === 'JFIF\0') {
        const units = b[p + 11];
        const x = d.getUint16(p + 12);
        const dpi = units === 1 ? x : units === 2 ? x * 2.54 : 0;
        return dpi >= 75 ? Math.round(dpi) : null;
      }
      if (marker === 0xda) break; // image data
      p += 2 + len;
    }
    return null;
  }
  if (b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) {
    let p = 8;
    while (p + 12 <= b.length) {
      const len = d.getUint32(p);
      const type = String.fromCharCode(...b.subarray(p + 4, p + 8));
      if (type === 'pHYs' && len >= 9) {
        const ppm = d.getUint32(p + 8);
        const unit = b[p + 16];
        const dpi = unit === 1 ? ppm * 0.0254 : 0;
        return dpi >= 75 ? Math.round(dpi) : null;
      }
      if (type === 'IDAT') break;
      p += 12 + len;
    }
  }
  return null;
}
