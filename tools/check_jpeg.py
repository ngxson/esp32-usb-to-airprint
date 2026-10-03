#!/usr/bin/env python3
"""Checks a JPEG from the scanner code: strict decode (djpeg, when installed,
reports any corrupt data), size and dpi, and PSNR against the reference image.

  check_jpeg.py scan.jpg [--ref ref.ppm] [--min-psnr 30] [--size WxH] [--dpi N]
"""
import argparse
import math
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jpeg")
    ap.add_argument("--ref", help="reference image (PPM/PNG) of the same size")
    ap.add_argument("--min-psnr", type=float, default=30.0)
    ap.add_argument("--size", help="expected WxH")
    ap.add_argument("--dpi", type=int, help="expected JFIF density")
    a = ap.parse_args()
    problems = []

    if shutil.which("djpeg"):
        r = subprocess.run(["djpeg", "-outfile", "/dev/null", a.jpeg], capture_output=True, text=True)
        if r.returncode or r.stderr.strip():
            problems.append("djpeg: " + (r.stderr.strip() or "exit %d" % r.returncode))

    im = Image.open(a.jpeg)
    info = "%s %dx%d %s" % (im.format, im.width, im.height, im.mode)
    if im.format != "JPEG" or im.mode != "RGB":
        problems.append("not an RGB JPEG: " + info)
    if a.size and "%dx%d" % im.size != a.size:
        problems.append("size %dx%d, expected %s" % (im.width, im.height, a.size))
    dpi = im.info.get("dpi")
    if a.dpi and (not dpi or round(dpi[0]) != a.dpi):
        problems.append("dpi %s, expected %d" % (dpi, a.dpi))

    if a.ref:
        got = np.asarray(im.convert("RGB"), dtype=np.float64)
        ref = np.asarray(Image.open(a.ref).convert("RGB"), dtype=np.float64)
        if got.shape != ref.shape:
            problems.append("shape %s vs reference %s" % (got.shape, ref.shape))
        else:
            mse = np.mean((got - ref) ** 2)
            psnr = 99.0 if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)
            info += ", PSNR %.1f dB" % psnr
            if psnr < a.min_psnr:
                problems.append("PSNR %.1f dB < %.1f" % (psnr, a.min_psnr))

    print("%s: %s%s" % (a.jpeg, info, "" if not problems else " -- " + "; ".join(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
