#!/usr/bin/env python3
"""Decode a URF (UNIRAST) or PWG raster (RaS2) file into PNG pages, to see
exactly what an AirPrint client sent.

usage: raster_to_png.py doc.urf out.png [--scale N] [--page N]
"""
import argparse
import struct

import numpy as np
from PIL import Image


def decode_lines(data, pos, width, height, bpp):
    """Yield decoded lines (bytes) for one page; returns new position via list."""
    out = np.empty((height, width * bpp), dtype=np.uint8)
    y = 0
    line_bytes = width * bpp
    while y < height:
        rep = data[pos]
        pos += 1
        line = bytearray()
        while len(line) < line_bytes:
            n = data[pos]
            pos += 1
            if n == 128:
                line += b"\xff" * (line_bytes - len(line))
            elif n < 128:
                line += data[pos:pos + bpp] * (n + 1)
                pos += bpp
            else:
                cnt = (257 - n) * bpp
                line += data[pos:pos + cnt]
                pos += cnt
        if len(line) != line_bytes:
            raise ValueError("line overrun at y=%d" % y)
        for _ in range(rep + 1):
            if y < height:
                out[y] = np.frombuffer(bytes(line), dtype=np.uint8)
                y += 1
    return out, pos


def pages(data):
    if data[:8] == b"UNIRAST\0":
        npages = struct.unpack(">I", data[8:12])[0]
        pos = 12
        for _ in range(npages):
            h = data[pos:pos + 32]
            pos += 32
            bpp = h[0] // 8
            width, height, dpi = struct.unpack(">III", h[12:24])
            img, pos = decode_lines(data, pos, width, height, bpp)
            yield img, width, height, bpp, dpi
    elif data[:4] == b"RaS2":
        pos = 4
        while pos < len(data):
            h = data[pos:pos + 1796]
            pos += 1796
            dpi = struct.unpack(">I", h[276:280])[0]
            width, height = struct.unpack(">II", h[372:380])
            bpp = struct.unpack(">I", h[388:392])[0] // 8
            img, pos = decode_lines(data, pos, width, height, bpp)
            yield img, width, height, bpp, dpi
    else:
        raise SystemExit("not URF or PWG raster")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("png")
    ap.add_argument("--scale", type=int, default=6)
    ap.add_argument("--page", type=int, default=1)
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    for i, (img, w, h, bpp, dpi) in enumerate(pages(data), 1):
        if i != a.page:
            continue
        mode = "RGB" if bpp == 3 else "L"
        im = Image.fromarray(img.reshape(h, w, bpp) if bpp == 3 else img, mode)
        im = im.resize((w // a.scale, h // a.scale), Image.BOX)
        im.save(a.png)
        print("page %d: %dx%d @ %d dpi, %d bytes/pixel -> %s" % (i, w, h, dpi, bpp, a.png))
        return


if __name__ == "__main__":
    main()
