#!/usr/bin/env python3
"""Function names inside the firmware image, so a crash can be decoded without
the build's ELF (the firmware names its own backtraces, tools/remote.py crash
falls back to them).

  gen_symbols.py add <firmware.elf> <firmware.bin> [--archive DIR]
      appends the symbol table to firmware.bin (again: replaced), and copies
      the ELF to DIR/<elf sha256>.elf (default .pio/elf)
  gen_symbols.py check <firmware.elf> <firmware.bin>
      reads the table back and looks up every function of the ELF
  gen_symbols.py lookup <firmware.bin> <addr>...

The table follows the ESP image (header, segments, checksum padding, SHA-256)
in the same file, so the image itself is unchanged: the bootloader and OTA
validate only the image and ignore what follows. Each OTA slot carries the
table of the build it holds. Format (little endian):

  0   "ESYM", u16 version (1), u16 header size (64)
  8   u32 count, u32 names size, u32 CRC-32 of the rest (addresses, sizes, names)
  20  ELF SHA-256 of the build (32 bytes, as in the app description), 12 zero bytes
  64  u32 address[count], ascending
      u16 size[count] (0: unknown, 65535: that or more)
      names: count NUL-terminated strings, in the same order

Functions (IRAM, flash, RTC) come from nm with their sizes; ROM functions are
the absolute symbols the linker scripts give for the ROM range (no size).
C++ names are demangled without their parameters.
"""
import glob
import os
import shutil
import struct
import subprocess
import sys
import zlib

MAGIC = b"ESYM"
VERSION = 1
HDR = 64
NAME_MAX = 63
# code address ranges of the ESP32-S3
ROM = (0x40000000, 0x40060000)
CODE = [ROM, (0x40370000, 0x403E0000), (0x42000000, 0x44000000), (0x600FE000, 0x60100000)]


def in_code(addr):
    return any(lo <= addr < hi for lo, hi in CODE)


def find_nm():
    name = "xtensa-esp32s3-elf-nm"
    found = shutil.which(name) or sorted(glob.glob(os.path.expanduser(
        "~/.platformio/packages/toolchain-xtensa-esp32s3/bin/" + name)))
    if not found:
        sys.exit("gen_symbols: %s not found" % name)
    return found if isinstance(found, str) else found[-1]


def short_name(name):
    # C++: drop the parameter list (first top-level parenthesis)
    depth = 0
    for i, c in enumerate(name):
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
        elif c == "(" and depth == 0 and i > 0:
            name = name[:i]
            break
    return name[:NAME_MAX]


def elf_functions(elf):
    """[(address, size, name)] sorted by address, one name per address."""
    out = subprocess.run([find_nm(), "-S", "-C", "--defined-only", elf], check=True,
                         capture_output=True, text=True).stdout
    best = {}
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4:
            addr, size, kind, name = parts
            size = int(size, 16)
        elif len(parts) == 3:
            addr, kind, name = parts
            size = 0
        else:
            continue
        addr = int(addr, 16)
        if kind in "tTwW":
            if not in_code(addr):
                continue
        elif kind == "A":
            if not (ROM[0] <= addr < ROM[1]):
                continue
            size = 0        # the sizes nm gives these belong to other code (the ROM's jump table)
        else:
            continue
        name = short_name(name)
        if not name:
            continue
        # aliases: prefer a sized symbol, then a name without a leading underscore, then the shortest
        rank = (size == 0, name.startswith("_"), len(name), name)
        if addr not in best or rank < best[addr][0]:
            best[addr] = (rank, size, name)
    funcs = []
    for a in sorted(best):
        size, name = best[a][1], best[a][2]
        if funcs and not size and a < funcs[-1][0] + funcs[-1][1]:
            continue        # a label inside a function (assembly): the function's name is the useful one
        funcs.append((a, size, name))
    return funcs


def image_length(data):
    """Length of the ESP app image at the start of data (what the bootloader checks)."""
    if len(data) < 24 or data[0] != 0xE9:
        raise ValueError("not an ESP firmware image")
    nseg = data[1]
    pos = 24
    for _ in range(nseg):
        if pos + 8 > len(data):
            raise ValueError("truncated image")
        _, n = struct.unpack_from("<II", data, pos)
        pos += 8 + n
    pos = (pos + 1 + 15) & ~15          # checksum byte, padded to 16
    if data[23]:                        # hash_appended
        pos += 32
    if pos > len(data):
        raise ValueError("truncated image")
    return pos


def image_elf_sha(data):
    return bytes(data[32 + 144:32 + 176])


def build_table(funcs, elf_sha):
    addrs = struct.pack("<%dI" % len(funcs), *(a for a, _, _ in funcs))
    sizes = struct.pack("<%dH" % len(funcs), *(min(s, 0xFFFF) for _, s, _ in funcs))
    names = b"".join(n.encode("utf-8", "replace") + b"\0" for _, _, n in funcs)
    body = addrs + sizes + names
    hdr = MAGIC + struct.pack("<HHIII", VERSION, HDR, len(funcs), len(names), zlib.crc32(body)) + elf_sha
    hdr += bytes(HDR - len(hdr))
    return hdr + body


class Table:
    def __init__(self, data):
        """data: the table (or a firmware.bin with one after its image)."""
        if data[:1] == b"\xe9":
            data = data[image_length(data):]
        if len(data) < HDR or data[:4] != MAGIC:
            raise ValueError("no symbol table")
        version, hdr, count, names_size, crc = struct.unpack_from("<HHIII", data, 4)
        if version != VERSION:
            raise ValueError("symbol table version %d" % version)
        self.elf_sha = bytes(data[20:52])
        body = data[hdr:hdr + count * 6 + names_size]
        if len(body) != count * 6 + names_size or zlib.crc32(body) != crc:
            raise ValueError("symbol table is damaged")
        self.addrs = struct.unpack_from("<%dI" % count, body, 0)
        self.sizes = struct.unpack_from("<%dH" % count, body, 4 * count)
        self.names = body[6 * count:].split(b"\0")[:count]
        self.names = [n.decode("utf-8", "replace") for n in self.names]
        self.size = hdr + len(body)

    def find(self, addr):
        """(name, offset) of the function holding addr, or None."""
        lo, hi = 0, len(self.addrs)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.addrs[mid] <= addr:
                lo = mid + 1
            else:
                hi = mid
        i = lo - 1
        if i < 0 or not in_code(addr):
            return None
        off, size = addr - self.addrs[i], self.sizes[i]
        if (size and size < 0xFFFF and off >= size) or (not size and off >= 0x1000):
            return None
        return self.names[i], off

    def name(self, addr):
        f = self.find(addr)
        return "%s+0x%x" % f if f else "?"


def cmd_add(elf, binfile, archive):
    data = open(binfile, "rb").read()
    end = image_length(data)
    sha = image_elf_sha(data)
    funcs = elf_functions(elf)
    table = build_table(funcs, sha)
    with open(binfile, "wb") as f:
        f.write(data[:end] + table)
    if archive:
        os.makedirs(archive, exist_ok=True)
        shutil.copyfile(elf, os.path.join(archive, sha.hex() + ".elf"))
    print("symbol table: %d functions, %d KB after the %d KB image (ELF %s)"
          % (len(funcs), (len(table) + 1023) // 1024, end // 1024, sha.hex()[:16]))


def cmd_check(elf, binfile):
    data = open(binfile, "rb").read()
    t = Table(data)
    assert t.elf_sha == image_elf_sha(data), "table of another build"
    funcs = elf_functions(elf)
    assert len(t.addrs) == len(funcs), "%d functions in the table, %d in the ELF" % (len(t.addrs), len(funcs))
    bad = 0
    for a, s, n in funcs:
        for probe in (a, a + max(s, 1) - 1):
            got = t.find(probe)
            if not got or got[0] != n or got[1] != probe - a:
                bad += 1
                if bad < 5:
                    print("  0x%08x: %r, expected %s+0x%x" % (probe, got, n, probe - a))
    if t.find(0x3FC90000) is not None or t.find(0x12345678) is not None:
        bad += 1
        print("  a data address got a name")
    print("symbol table: %d functions, %d KB, %s" % (len(funcs), t.size // 1024, "OK" if not bad else "%d WRONG" % bad))
    return 1 if bad else 0


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "add":
        archive = a[a.index("--archive") + 1] if "--archive" in a else os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".pio", "elf")
        cmd_add(a[1], a[2], archive)
    elif len(a) == 3 and a[0] == "check":
        sys.exit(cmd_check(a[1], a[2]))
    elif len(a) >= 3 and a[0] == "lookup":
        t = Table(open(a[1], "rb").read())
        for x in a[2:]:
            print("0x%08x %s" % (int(x, 16), t.name(int(x, 16))))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
