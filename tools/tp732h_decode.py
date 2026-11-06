#!/usr/bin/env python3
"""Decodes a TP732H label printer stream (components/printers/tp_label.c),
checks its framing and writes each page as a PNG (level-2 dots dark gray).

    tools/tp732h_decode.py job.prn [--png out.png]   (out-1.png, out-2.png... for more pages)
"""
import argparse
import struct
import sys

from PIL import Image

HEADER = (b'@SMS\x02\xd5\r\n@S~~E@SGS\x02\r\n@STPZ\n'
          b'\x14\x14\x14\x14\x14\x14\x5a\x64\x50\x50\x50\x50\x50\x50\r\n@SPP\x00')


def pages(b):
    i = 0
    while i < len(b):
        if b[i:i + 50] != bytes(50) or b[i + 50:i + 50 + len(HEADER) + 2] != HEADER + b'@<':
            raise ValueError(f'offset {i}: no page header')
        i += 50 + len(HEADER) + 2
        length, nrows = struct.unpack('>I', b[i:i + 4])[0], struct.unpack('<H', b[i + 4:i + 6])[0]
        i += 6
        start, rows = i, []
        for y in range(nrows):
            if b[i:i + 2] != b'#<':
                raise ValueError(f'row {y}: no "#<" at {i}')
            i += 2
            row = bytearray()
            while b[i:i + 2] != b'#>':
                c = b[i]
                n = b[i + 1] + 1 if c & 0x80 else 1
                row += bytes([c & 0x7f]) * n
                i += 2 if c & 0x80 else 1
            if len(row) != 640 or max(row) > 2:
                raise ValueError(f'row {y}: {len(row)} dots, levels up to {max(row)}')
            rows.append(row)
            i += 2
        if b[i:i + 4] != b'@>\r\n' or i + 4 - start - 92 != length:
            raise ValueError(f'page end at {i}: bad trailer or length field {length}')
        i += 4
        yield rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prn')
    ap.add_argument('--png')
    a = ap.parse_args()
    data = open(a.prn, 'rb').read()
    all_pages = list(pages(data))
    for n, rows in enumerate(all_pages, 1):
        black = sum(v != 0 for r in rows for v in r)
        print(f'page {n}: {len(rows)} rows ({len(rows) / 8:.1f} mm), {black} black dots')
        if a.png:
            img = Image.frombytes('L', (640, len(rows)), b''.join(rows).translate(bytes([255, 0, 90]) + bytes(253)))
            out = a.png if len(all_pages) == 1 else a.png.replace('.png', f'-{n}.png')
            img.save(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
