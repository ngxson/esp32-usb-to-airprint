#!/usr/bin/env python3
"""Draws docs/sd_card_pinout.png: full-size SD card pinout + ESP32-S3 wiring."""
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 1700, 1000
im = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(im)


def font(size, bold=False):
    for name in (["/System/Library/Fonts/Supplemental/Arial Bold.ttf"] if bold else []) + [
            "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"]:
        if os.path.exists(name):
            return ImageFont.truetype(name, size)
    return ImageFont.load_default()


F_T, F_B, F, F_S = font(34, True), font(22, True), font(21), font(17)
INK, GREY, GOLD = (30, 30, 35), (120, 120, 128), (212, 168, 55)

d.text((40, 28), "SD card (full size) pinout", font=F_T, fill=INK)
d.text((40, 74), "Viewed from the contact side (gold pads facing you), notched corner top-left", font=F, fill=GREY)

# card outline, 24 x 32 mm scaled, chamfered top-left corner
s = 18
x0, y0 = 60, 140
cw, ch, cut = 24 * s, 32 * s, 4 * s
d.polygon([(x0 + cut, y0), (x0 + cw, y0), (x0 + cw, y0 + ch), (x0, y0 + ch), (x0, y0 + cut)],
          fill=(52, 58, 70), outline=INK)
# write-protect slider: on the edge opposite the notch
d.rectangle([x0 + cw - 4, y0 + 9 * s, x0 + cw + 10, y0 + 14 * s], fill=(235, 235, 235), outline=INK)
d.text((x0 + cw + 18, y0 + 10.5 * s), "write\nprotect\n(mechanical,\nnot a pin)", font=F_S, fill=GREY)

# pads: pin 9 sits in the chamfer, then 1..8
pins = [9, 1, 2, 3, 4, 5, 6, 7, 8]
pad_w, gap = 1.7 * s, 0.75 * s
px = x0 + 1.2 * s
for i, p in enumerate(pins):
    top = y0 + (2.6 * s if p == 9 else 1.2 * s)
    d.rectangle([px, top, px + pad_w, top + 5 * s], fill=GOLD, outline=(150, 115, 30))
    tw = d.textlength(str(p), font=F_B)
    d.text((px + pad_w / 2 - tw / 2, top + 5 * s + 8), str(p), font=F_B, fill="white")
    px += pad_w + gap
d.text((x0 + 4 * s, y0 + 16 * s), "SD", font=font(90, True), fill=(80, 88, 104))

# table
rows = [
    ("Pin", "SD bus (SDMMC)", "SPI mode", "ESP32-S3 4-bit", "ESP32-S3 SPI"),
    ("1", "DAT3 / CD", "CS", "GPIO10", "GPIO10"),
    ("2", "CMD", "MOSI (DI)", "GPIO11", "GPIO11"),
    ("3", "GND (VSS1)", "GND", "GND", "GND"),
    ("4", "VDD 3.3 V", "VDD 3.3 V", "3V3", "3V3"),
    ("5", "CLK", "SCK", "GPIO12", "GPIO12"),
    ("6", "GND (VSS2)", "GND", "GND", "GND"),
    ("7", "DAT0", "MISO (DO)", "GPIO13", "GPIO13"),
    ("8", "DAT1", "- (unused)", "GPIO14", "-"),
    ("9", "DAT2", "- (unused)", "GPIO9", "-"),
]
cols = [640, 720, 950, 1160, 1380]
ty = 150
for r, row in enumerate(rows):
    y = ty + r * 54
    if r == 0:
        d.rectangle([cols[0] - 14, y - 10, W - 40, y + 38], fill=(238, 240, 245))
    elif row[1].startswith("GND") or row[1].startswith("VDD"):
        d.rectangle([cols[0] - 14, y - 10, W - 40, y + 38], fill=(250, 246, 232))
    for c, txt in enumerate(row):
        d.text((cols[c], y), txt, font=F_B if r == 0 or c == 0 else F, fill=INK)
    d.line([cols[0] - 14, y + 40, W - 40, y + 40], fill=(225, 225, 230))

notes = [
    "3.3 V only. Modules with an on-board regulator: power them from 5Vin instead.",
    "SD bus mode needs ~10 kΩ pull-ups to 3.3 V on CMD and DAT0-DAT3 (bare sockets have none).",
    "Avoid GPIO19/20 (printer USB), 48 (LED), 0 (BOOT), 43/44 (COM), 35-37 (octal PSRAM).",
]
for i, n in enumerate(notes):
    d.text((cols[0] - 14, ty + 10 * 54 + 30 + i * 36), "• " + n, font=F_S, fill=GREY)

out = os.path.join(os.path.dirname(__file__), "..", "docs", "sd_card_pinout.png")
im.save(out)
print(out)
