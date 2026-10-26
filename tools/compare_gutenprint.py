#!/usr/bin/env python3
"""Cross-check our generator against a Gutenprint reference stream.

Decodes the Gutenprint stream into raw ink planes, re-encodes those planes with
host/bjl_host (our C code), then compares the two streams command by command.
They must be byte-identical, except that Gutenprint drops the last raster
block when the page height is a multiple of 8 (print-canon.c
canon_write_multiraster never flushes it), so our stream may carry extra
trailing all-blank ESC (F blocks.

usage: compare_gutenprint.py ref.prn <300|600> [--paper a4|letter] [--model MG2400] [--host host/bjl_host]

--model: the canon_bjl.h stream variant to encode with (the reference must
come from a Gutenprint model that uses it, see tools/gutenprint_ref/build_ref.sh).
"""
import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from bjl_decode import Decoder  # noqa: E402

AREA_W_PT = {"a4": 575, "letter": 592}     # paper width - 20 pt of borders


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("dpi", type=int)
    ap.add_argument("--paper", default="a4", choices=["a4", "letter"])
    ap.add_argument("--model", default="MG2400")
    ap.add_argument("--host", default=os.path.join(os.path.dirname(__file__), "..", "host", "bjl_host"))
    args = ap.parse_args()

    ref = open(args.ref, "rb").read()
    dec = Decoder(ref, verbose=False).run()
    width = AREA_W_PT[args.paper] * args.dpi // 72

    with tempfile.TemporaryDirectory() as td:
        raws = []
        for pi, page in enumerate(dec.pages):
            line_bytes = {c: (width + 8 // page.bits[c] - 1) // (8 // page.bits[c]) for c in "CMYK"}
            raw = os.path.join(td, "page%d.raw" % pi)
            with open(raw, "wb") as f:
                for y in range(len(page.rows["C"])):
                    for c in "CMYK":
                        row = page.rows[c][y]
                        if len(row) > line_bytes[c]:
                            sys.exit("reference line wider than expected: %d > %d" % (len(row), line_bytes[c]))
                        f.write(row + bytes(line_bytes[c] - len(row)))
            raws.append(raw)
        out = os.path.join(td, "ours.prn")
        subprocess.run([args.host, "encode", "--model", args.model, str(args.dpi), args.paper, out] + raws,
                       check=True, stdout=subprocess.DEVNULL)
        ours = open(out, "rb").read()

    a = Decoder(ref, verbose=False).run().cmds
    b = Decoder(ours, verbose=False).run().cmds
    print("reference: %d bytes %d commands, ours: %d bytes %d commands" % (len(ref), len(a), len(ours), len(b)))

    i = j = 0
    extra_blank = 0
    while i < len(a) and j < len(b):
        if a[i] == b[j]:
            i += 1
            j += 1
            continue
        # tolerate our extra blank blocks just before each form feed
        if b[j][0] == "ESC (F" and set(b[j][1]) == {0x80} and a[i][0] == "FF":
            extra_blank += 1
            j += 1
            continue
        print("MISMATCH at reference cmd #%d vs ours #%d:" % (i, j))
        print("  ref : %s %s" % (a[i][0], bytes(a[i][1])[:64].hex(" ")))
        print("  ours: %s %s" % (b[j][0], bytes(b[j][1])[:64].hex(" ")))
        return 1
    if i != len(a) or j != len(b):
        print("MISMATCH: trailing commands differ (ref %d/%d, ours %d/%d)" % (i, len(a), j, len(b)))
        return 1
    print("IDENTICAL: every command matches byte-for-byte (%d page(s), %d extra blank F blocks = %d lines of padding)"
          % (len(dec.pages), extra_blank, extra_blank // 4 * 8))
    return 0


if __name__ == "__main__":
    sys.exit(main())
