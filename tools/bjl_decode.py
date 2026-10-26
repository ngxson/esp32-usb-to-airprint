#!/usr/bin/env python3
"""Decode a Canon PIXMA (BJ raster / "multiraster") print stream.

Parses the command stream produced by Gutenprint or by our ESP32 firmware,
validates its structure, and reconstructs every ink plane so the page can be
inspected as an image.

usage: bjl_decode.py stream.prn [--png out.png] [--quiet]
"""
import argparse
import struct
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:  # decoding still works without imaging libs
    np = None


class DecodeError(Exception):
    pass


def unpack_bits_line(buf, pos, end):
    """Decode one PackBits line terminated by 0x80. Returns (bytes, newpos)."""
    out = bytearray()
    while True:
        if pos >= end:
            raise DecodeError("raster line runs past end of F) block")
        n = buf[pos]
        pos += 1
        if n == 0x80:
            return bytes(out), pos
        if n < 0x80:
            cnt = n + 1
            if pos + cnt > end:
                raise DecodeError("literal run past end of block")
            out += buf[pos:pos + cnt]
            pos += cnt
        else:
            cnt = 257 - n
            if pos >= end:
                raise DecodeError("repeat run past end of block")
            out += bytes([buf[pos]]) * cnt
            pos += 1


def unpack_bits_plain(data):
    """Decode a PackBits buffer that is not 0x80-terminated (ESC (A lines)."""
    out = bytearray()
    pos = 0
    while pos < len(data):
        n = data[pos]
        pos += 1
        if n == 0x80:
            continue
        if n < 0x80:
            out += data[pos:pos + n + 1]
            pos += n + 1
        else:
            out += bytes([data[pos]]) * (257 - n)
            pos += 1
    return bytes(out)


class Page:
    def __init__(self, channels, bits):
        self.channels = channels          # e.g. "CMYK"
        self.bits = bits                  # dict letter -> bits per pixel
        self.rows = {c: [] for c in channels}   # list of raw line bytes
        self.y = 0                        # current raster line (multiraster)


class Decoder:
    def __init__(self, data, verbose=True):
        self.d = data
        self.p = 0
        self.v = verbose
        self.cmds = []            # (name, payload)
        self.xml = []
        self.pages = []
        self.page = None
        self.state = {}
        self.ff_count = 0

    def log(self, *a):
        if self.v:
            print(*a)

    def run(self):
        d = self.d
        while self.p < len(d):
            b = d[self.p]
            if d.startswith(b"<?xml", self.p):
                end = d.find(b"</cmd>", self.p)
                if end < 0:
                    raise DecodeError("unterminated XML at %d" % self.p)
                end += len(b"</cmd>")
                x = d[self.p:end].decode()
                self.xml.append(x)
                op = x.split("<ivec:operation>")[1].split("<")[0]
                extra = ""
                if "<vcn:ijoperation>" in x:
                    extra = " " + x.split("<vcn:ijoperation>")[1].split("<")[0]
                    extra += " mode=" + x.split("<vcn:ijmode>")[1].split("<")[0]
                self.log("@%06x XML %s%s" % (self.p, op, extra))
                self.cmds.append(("XML", op + extra))
                self.p = end
            elif d.startswith(b"BJLSTART\n", self.p):
                # BJL control block after ESC [K 00 1f (iP2700 and others)
                end = d.find(b"BJLEND\n", self.p)
                if end < 0:
                    raise DecodeError("unterminated BJL block at %d" % self.p)
                end += len(b"BJLEND\n")
                text = d[self.p:end].decode()
                self.log("@%06x BJL %s" % (self.p, text.strip().replace("\n", "; ")))
                self.cmds.append(("BJL", text))
                self.p = end
            elif b == 0x1B:
                self.esc()
            elif b == 0x0C:
                self.log("@%06x FF (eject page)" % self.p)
                self.cmds.append(("FF", b""))
                self.ff_count += 1
                if self.page:
                    self.pages.append(self.page)
                    self.page = None
                self.p += 1
            elif b == 0x0D:
                self.p += 1
            else:
                raise DecodeError("unexpected byte 0x%02x at offset %d" % (b, self.p))
        if self.page:
            raise DecodeError("stream ended without form feed (page not ejected)")
        return self

    def esc(self):
        d, p = self.d, self.p
        if d[p + 1] == ord("@"):
            self.log("@%06x ESC @ (end of job)" % p)
            self.cmds.append(("ESC@", b""))
            self.p += 2
            return
        if d[p + 1] not in (ord("("), ord("[")):
            raise DecodeError("unknown escape 0x%02x at %d" % (d[p + 1], p))
        intro = chr(d[p + 1])
        cmd = chr(d[p + 2])
        ln = struct.unpack_from("<H", d, p + 3)[0]
        payload = d[p + 5:p + 5 + ln]
        if len(payload) != ln:
            raise DecodeError("truncated ESC %s%s at %d" % (intro, cmd, p))
        self.p = p + 5 + ln
        name = "ESC %s%s" % (intro, cmd)
        self.cmds.append((name, payload))
        handler = getattr(self, "cmd_" + {"[": "sq_", "(": ""}[intro] + cmd, None)
        desc = handler(payload) if handler else None
        if cmd != "F":
            self.log("@%06x %s len=%d %s%s" % (p, name, ln, payload[:48].hex(" "),
                                              ("  -> " + desc) if desc else ""))

    # --- header commands -------------------------------------------------
    def cmd_sq_K(self, a):
        return "BJL control block follows" if a[-1:] == b"\x1f" else "reset printer"

    def cmd_b(self, a):
        self.state["compression"] = a[0]
        return "data compression %s" % ("ON" if a[0] else "OFF")

    def cmd_q(self, a):
        return "page id %d" % a[0]

    def cmd_d(self, a):
        y, x = struct.unpack(">HH", a)
        self.state["dpi"] = (x, y)
        return "resolution %dx%d dpi" % (x, y)

    def cmd_t(self, a):
        inks = []
        for i in range(3, len(a), 3):
            bits, _, levels = a[i:i + 3]
            if bits:
                inks.append((bits, levels))
        self.state["t_inks"] = inks
        return "image format %02x %02x %02x, inks(bits,levels)=%s" % (a[0], a[1], a[2], inks)

    def cmd_c(self, a):
        return "model/color %02x media %02x quality %d" % (a[0], a[1], a[2])

    def cmd_p(self, a):
        f = struct.unpack(">HHHHIHIIIIIIII", a)
        self.state["page"] = f
        return ("printable %dx%d pt, unit 1/%d, area off (%d,%d) size %dx%d, paper %dx%d"
                % (f[2], f[0], f[5], f[6], f[7], f[8], f[9], f[12], f[13]))

    def cmd_P(self, a):
        sizes = {0x03: "A4", 0x0d: "Letter"}
        if len(a) < 4:
            return ""
        return "paper size 0x%02x (%s) media 0x%02x (%s) orientation %d/%d" % (
            a[1], sizes.get(a[1], "?"), a[3], "plain" if a[3] == 0 else "?", a[4], a[5])

    def cmd_T(self, a):
        return "cartridge"

    def cmd_l(self, a):
        return "tray/media"

    def cmd_w(self, a):
        return "media %02x" % a[0]

    def cmd_v(self, a):
        return "page orientation %d" % a[0]

    def cmd_I(self, a):
        self.state["multiraster"] = a[0]
        return "multiraster %d" % a[0]

    def cmd_J(self, a):
        self.state["lines_per_block"] = a[0]
        return "%d raster lines per block" % a[0]

    def cmd_L(self, a):
        chans = a.decode()
        self.state["channels"] = chans
        inks = self.state.get("t_inks", [])
        if len(inks) != len(chans):
            raise DecodeError("ESC (L lists %d channels but ESC (t declares %d inks" % (len(chans), len(inks)))
        bits = {c: inks[i][0] for i, c in enumerate(chans)}
        self.page = Page(chans, bits)
        self.block_idx = 0
        return "channel order %s" % chans

    def cmd_e(self, a):
        n = struct.unpack(">H", a)[0] if len(a) == 2 else struct.unpack(">I", a)[0]
        pg = self.page
        if pg is None:
            raise DecodeError("ESC (e before page setup")
        if self.state.get("multiraster"):
            if self.block_idx % len(pg.channels):
                raise DecodeError("ESC (e in the middle of a channel cycle")
            lines = n * self.state["lines_per_block"]
            for c in pg.channels:
                pg.rows[c] += [b""] * lines
            return "advance %d blocks (%d lines)" % (n, lines)
        return "advance %d lines" % n

    def cmd_F(self, a):
        pg = self.page
        if pg is None:
            raise DecodeError("ESC (F before ESC (L")
        chan = pg.channels[self.block_idx % len(pg.channels)]
        self.block_idx += 1
        pos, lines = 0, 0
        while pos < len(a):
            line, pos = unpack_bits_line(a, pos, len(a))
            pg.rows[chan].append(line)
            lines += 1
        if lines != self.state["lines_per_block"]:
            raise DecodeError("F) block for %s has %d lines, expected %d"
                              % (chan, lines, self.state["lines_per_block"]))
        return None


def plane_levels(rows, bits, width_px):
    """Expand packed rows into a 2-D numpy array of ink levels."""
    h = len(rows)
    out = np.zeros((h, width_px), dtype=np.uint8)
    for y, r in enumerate(rows):
        if not r:
            continue
        arr = np.frombuffer(r, dtype=np.uint8)
        if bits == 1:
            px = np.unpackbits(arr)
        elif bits == 2:
            px = np.stack([(arr >> 6) & 3, (arr >> 4) & 3, (arr >> 2) & 3, arr & 3], 1).reshape(-1)
        else:
            raise DecodeError("unsupported bits %d" % bits)
        n = min(len(px), width_px)
        out[y, :n] = px[:n]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--png", help="write a composite preview PNG")
    ap.add_argument("--scale", type=int, default=4, help="downscale factor for the PNG")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    dec = Decoder(data, verbose=not args.quiet).run()
    print("\n== summary: %d bytes, %d commands, %d page(s), %d XML blocks"
          % (len(data), len(dec.cmds), len(dec.pages), len(dec.xml)))
    if not dec.pages:
        print("no pages decoded")
        return 1

    for i, pg in enumerate(dec.pages):
        heights = {c: len(r) for c, r in pg.rows.items()}
        wbytes = {c: max((len(r) for r in pg.rows[c]), default=0) for c in pg.channels}
        print("page %d: channels %s bits %s lines %s max line bytes %s"
              % (i + 1, pg.channels, pg.bits, heights, wbytes))
        if len(set(heights.values())) != 1:
            raise DecodeError("channels have different line counts")
        if np is None:
            continue
        dpi = dec.state["dpi"][0]
        width_px = max(wbytes[c] * 8 // pg.bits[c] for c in pg.channels)
        planes = {c: plane_levels(pg.rows[c], pg.bits[c], width_px) for c in pg.channels}
        for c in pg.channels:
            maxlvl = 2 if pg.bits[c] == 2 else 1   # 2-bit inks are 3-level (0..2)
            levels_used = sorted(set(np.unique(planes[c]).tolist()))
            cov = planes[c].astype(np.float64).mean() / maxlvl * 100
            print("  %s: levels used %s, coverage %.2f%%" % (c, levels_used, cov))
        if args.png:
            # Ink density -> subtractive RGB preview
            h = heights[pg.channels[0]]
            ink = {}
            for c in "CMYK":
                if c in planes:
                    p = planes[c].astype(np.float32)
                    maxlvl = 2 if pg.bits[c] == 2 else 1   # 2-bit inks are 3-level
                    ink[c] = np.clip(p / maxlvl, 0, 1)
                else:
                    ink[c] = np.zeros((h, width_px), np.float32)
            r = (1 - ink["C"]) * (1 - ink["K"])
            g = (1 - ink["M"]) * (1 - ink["K"])
            b = (1 - ink["Y"]) * (1 - ink["K"])
            rgb = (np.stack([r, g, b], -1) * 255).astype(np.uint8)
            im = Image.fromarray(rgb)
            if args.scale > 1:
                im = im.resize((width_px // args.scale, h // args.scale), Image.BOX)
            out = args.png if len(dec.pages) == 1 else args.png.replace(".png", "_p%d.png" % (i + 1))
            im.save(out)
            print("  preview -> %s (%dx%d px @ %d dpi, page %.1fx%.1f mm)"
                  % (out, width_px, h, dpi, width_px / dpi * 25.4, h / dpi * 25.4))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except DecodeError as e:
        print("DECODE ERROR:", e)
        sys.exit(2)
