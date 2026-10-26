#!/usr/bin/env python3
"""Puts the status screens rendered by host/oled_host into one PNG, drawn
like the OLED (light pixels on black, enlarged)."""
import glob
import os
import sys

from PIL import Image, ImageDraw

SCALE = 3
COLS = 3
PAD = 24


def main():
    src, out = sys.argv[1], sys.argv[2]
    files = sorted(glob.glob(os.path.join(src, 'oled-*.pbm')))
    if not files:
        sys.exit('no oled-*.pbm in ' + src)
    w, h = 128 * SCALE, 64 * SCALE
    rows = (len(files) + COLS - 1) // COLS
    sheet = Image.new('RGB', (COLS * (w + PAD) + PAD, rows * (h + PAD + 14) + PAD), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, f in enumerate(files):
        img = Image.open(f).convert('L').point(lambda v: 255 - v)     # PBM: 1 = black
        lit = Image.new('RGB', img.size, (0, 0, 0))
        lit.paste((150, 220, 255), mask=img)
        x = PAD + (i % COLS) * (w + PAD)
        y = PAD + (i // COLS) * (h + PAD + 14)
        sheet.paste(lit.resize((w, h), Image.NEAREST), (x, y + 14))
        draw.text((x, y), os.path.basename(f)[5:-4], fill=(200, 200, 200))
    sheet.save(out)
    print(f'{len(files)} screens -> {out}')


if __name__ == '__main__':
    main()
