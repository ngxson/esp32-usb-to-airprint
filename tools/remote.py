#!/usr/bin/env python3
"""Remote access to a board running MODE=debug (see src/debug.h).

  tools/remote.py info                 firmware, memory, tasks, OTA state
  tools/remote.py log [-f]             the log kept on the board; -f follows it live
  tools/remote.py upload [image.bin]   update the firmware over WiFi (default: the last build)
  tools/remote.py reboot
  tools/remote.py crash [--erase]      fetch and decode the last crash's core dump (with the
                                       build's ELF if it is here, else the board's symbol table)
  tools/remote.py crash --file dump.bin [--table firmware.bin]   a saved dump, offline
  tools/remote.py panic                crash the board on purpose (tests the crash tools)
  tools/remote.py level <tag|*> <none|error|warn|info>

The board is <HOSTNAME>.local (HOSTNAME from .env), or --host.
Also as PlatformIO targets: pio run -t ota, pio run -t logs.
"""
import argparse
import glob
import http.client
import shutil
import os
import re
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTTP_PORT = int(os.environ.get("REMOTE_HTTP_PORT", 8080))     # other ports: tests through QEMU
LOG_PORT = int(os.environ.get("REMOTE_LOG_PORT", 2323))
BUILD = os.path.join(ROOT, ".pio", "build", "esp32s3")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_symbols  # noqa: E402


def config_value(key):
    """KEY from .env, else .env.example (only this key is read)."""
    for name in (".env", ".env.example"):
        try:
            with open(os.path.join(ROOT, name), encoding="utf-8") as f:
                for line in f:
                    m = re.match(r"\s*%s\s*=\s*(.*?)\s*$" % key, line)
                    if m:
                        return m.group(1).strip("\"'")
        except OSError:
            pass
    return None


def resolve(host):
    try:
        return socket.gethostbyname(host)      # once: .local lookups are slow
    except OSError as e:
        sys.exit("cannot find %s: %s (is the board on the network?)" % (host, e))


def request(ip, method, path, body=None, timeout=15):
    c = http.client.HTTPConnection(ip, HTTP_PORT, timeout=timeout)
    c.request(method, path, body=body)
    r = c.getresponse()
    return r.status, r.read()


def info_dict(ip):
    status, body = request(ip, "GET", "/info")
    text = body.decode(errors="replace")
    return {m.group(1): m.group(2) for m in re.finditer(r"^(\w+)\s+(\S+)", text, re.M)}, text


def image_elf_sha(path):
    """First 8 bytes (hex) of the ELF SHA-256 in the image's app description."""
    with open(path, "rb") as f:
        head = f.read(32 + 256)
    if len(head) < 32 + 176 or head[0] != 0xE9:
        sys.exit("%s is not an ESP firmware image" % path)
    return head[32 + 144:32 + 152].hex()


# ---------------------------------------------------------------------------

def cmd_info(ip, a):
    print(info_dict(ip)[1], end="")


def cmd_log(ip, a):
    if not a.follow:
        sys.stdout.write(request(ip, "GET", "/log")[1].decode(errors="replace"))
        return
    s = socket.create_connection((ip, LOG_PORT), timeout=10)
    s.settimeout(None)
    try:
        while True:
            data = s.recv(4096)
            if not data:
                print("\n[connection closed: the board rebooted or another client is connected]")
                return
            sys.stdout.write(data.decode(errors="replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass


def wait_for(ip, cond, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            d = info_dict(ip)[0]
            if cond(d):
                return d
        except OSError:
            pass
        time.sleep(2)
    return None


def cmd_upload(ip, a):
    path = a.image or os.path.join(BUILD, "firmware.bin")
    if not os.path.exists(path):
        sys.exit("%s not found: build first (pio run)" % path)
    new_sha = image_elf_sha(path)
    before = info_dict(ip)[0]
    print("board: %s on %s (%s), mode %s" % (before.get("elf_sha256"), before.get("partition"),
                                               before.get("ota_state"), before.get("mode")))
    if before.get("elf_sha256") == new_sha:
        print("note: the board already runs this build")
    data = open(path, "rb").read()

    c = http.client.HTTPConnection(ip, HTTP_PORT, timeout=60)
    c.putrequest("POST", "/ota")
    c.putheader("Content-Length", str(len(data)))
    c.putheader("Content-Type", "application/octet-stream")
    c.endheaders()
    t0 = time.time()
    for off in range(0, len(data), 16384):
        c.send(data[off:off + 16384])
        sys.stdout.write("\rupload %3d%%" % ((off + 16384) * 100 // len(data) if off + 16384 < len(data) else 100))
        sys.stdout.flush()
    r = c.getresponse()
    msg = r.read().decode(errors="replace").strip()
    print("  %d KB in %.0f s: %s" % (len(data) // 1024, time.time() - t0, msg))
    if r.status != 200:
        sys.exit(1)

    print("waiting for the board to restart ...")
    d = wait_for(ip, lambda d: d.get("boot_id") != before.get("boot_id"), 120)
    if not d:
        sys.exit("the board did not come back within 2 minutes (it rolls back by itself if the new firmware "
                 "fails; check again later with: tools/remote.py info)")
    if d.get("elf_sha256") != new_sha:
        sys.exit("the board is running the previous firmware again: the update was rolled back")
    print("new firmware running on %s, waiting for it to confirm itself ..." % d.get("partition"))
    d = wait_for(ip, lambda d: d.get("ota_state") != "pending" or d.get("elf_sha256") != new_sha, 240)
    if not d or d.get("elf_sha256") != new_sha or d.get("ota_state") != "valid":
        sys.exit("the new firmware did not confirm itself (state %s): the board goes back to the previous one"
                 % (d or {}).get("ota_state"))
    print("done: firmware %s confirmed" % new_sha)


def cmd_reboot(ip, a):
    print(request(ip, "POST", "/reboot")[1].decode().strip())


def find_gdb():
    """GDB for the ESP32-S3 (esp_coredump needs it): PATH, ESP-IDF tools, PlatformIO."""
    name = "xtensa-esp32s3-elf-gdb"
    found = shutil.which(name) or sorted(
        glob.glob(os.path.expanduser("~/.espressif/tools/xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin/" + name)) +
        glob.glob(os.path.expanduser("~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/" + name)))
    return found if isinstance(found, str) else (found[-1] if found else None)


# --- core dumps (ESP-IDF ELF format) without gdb: enough to name every task's backtrace

EXCCAUSE = {0: "IllegalInstruction", 1: "Syscall", 2: "InstructionFetchError", 3: "LoadStoreError",
            4: "Level1Interrupt", 5: "Alloca", 6: "IntegerDivideByZero", 8: "Privileged",
            9: "LoadStoreAlignment", 12: "InstrPIFDataError", 13: "LoadStorePIFDataError",
            14: "InstrPIFAddrError", 15: "LoadStorePIFAddrError", 16: "InstTLBMiss", 17: "InstTLBMultiHit",
            18: "InstFetchPrivilege", 20: "InstFetchProhibited", 24: "LoadStoreTLBMiss",
            25: "LoadStoreTLBMultiHit", 26: "LoadStorePrivilege", 28: "LoadProhibited", 29: "StoreProhibited"}
# ESP-IDF's pseudo causes (64 + PANIC_RSN_*)
PSEUDO = ["unknown reason", "unhandled debug exception", "double exception", "unhandled kernel exception",
          "coprocessor exception", "interrupt watchdog on CPU0", "interrupt watchdog on CPU1", "cache error"]
REG_EXCCAUSE, REG_EXCVADDR = 232, 238


class CoreDump:
    def __init__(self, data):
        start = data.find(b"\x7fELF")      # after ESP-IDF's header
        if start < 0:
            raise ValueError("no ELF in the core dump")
        e = data[start:]
        phoff, = struct.unpack_from("<I", e, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", e, 0x2A)
        self.mem, self.tasks, self.sha, self.crashed_tcb, self.extra = [], [], "", None, {}
        for i in range(phnum):
            ptype, off, vaddr, _, filesz = struct.unpack_from("<5I", e, phoff + i * phentsize)
            if ptype == 1:                  # PT_LOAD
                self.mem.append((vaddr, e[off:off + filesz]))
            elif ptype == 4:                # PT_NOTE
                self._notes(e[off:off + filesz])

    def _notes(self, d):
        pos = 0
        while pos + 12 <= len(d):
            namesz, descsz, ntype = struct.unpack_from("<III", d, pos)
            pos += 12
            name = d[pos:pos + namesz].rstrip(b"\0").decode(errors="replace")
            pos += (namesz + 3) & ~3
            desc = d[pos:pos + descsz]
            pos += (descsz + 3) & ~3
            if name == "CORE" and ntype == 1 and len(desc) >= 72 + 128 * 4:      # NT_PRSTATUS
                tcb, = struct.unpack_from("<I", desc, 24)
                regs = struct.unpack_from("<128I", desc, 72)
                self.tasks.append({"tcb": tcb, "pc": regs[0], "ar": regs[64:128]})
            elif name == "ESP_CORE_DUMP_INFO":
                self.sha = desc[4:].split(b"\0")[0].decode(errors="replace")
            elif name == "ESP_EXTRA_INFO" and len(desc) >= 4:
                self.crashed_tcb, = struct.unpack_from("<I", desc, 0)
                for k in range(4, len(desc) - 7, 8):
                    idx, val = struct.unpack_from("<II", desc, k)
                    self.extra[idx] = val

    def read32(self, addr):
        for base, d in self.mem:
            if base <= addr and addr + 4 <= base + len(d):
                return struct.unpack_from("<I", d, addr - base)[0]
        return None

    def task_name(self, tcb):
        for base, d in self.mem:
            if base == tcb and len(d) >= 0x44:
                return d[0x34:0x44].split(b"\0")[0].decode(errors="replace") or "?"
        return "?"

    def backtrace(self, task, depth=24):
        """PCs of the call sites, as esp_backtrace walks the windowed ABI's base save areas."""
        def code(ra):
            return ((ra & 0x3FFFFFFF) | 0x40000000) - 3
        pcs = [task["pc"]]
        next_pc, sp = task["ar"][0], task["ar"][1]
        while next_pc and len(pcs) < depth:
            pcs.append(code(next_pc))
            ra, nsp = self.read32(sp - 16), self.read32(sp - 12)
            if ra is None or nsp is None or nsp <= sp:
                break
            next_pc, sp = ra, nsp
        return pcs

    def cause(self):
        c = self.extra.get(REG_EXCCAUSE)
        if c is None or c == 0xFFFF:
            return "unknown"
        if 64 <= c < 64 + len(PSEUDO):
            return PSEUDO[c - 64]
        return "%s (EXCCAUSE %d)" % (EXCCAUSE.get(c, "exception"), c)


def decode_with_symbols(dump, table):
    print("crash: %s, EXCVADDR 0x%08x" % (dump.cause(), dump.extra.get(REG_EXCVADDR, 0)))
    if dump.extra.get(REG_EXCCAUSE) in (64 + 7,):
        print("  (a cache error is an illegal access to flash or PSRAM; it may have come from the other core,\n"
              "   so the crashed task can be a bystander: see what the other running task was doing)")
    order = sorted(dump.tasks, key=lambda t: t["tcb"] != dump.crashed_tcb)
    for t in order:
        mark = " <- crashed" if t["tcb"] == dump.crashed_tcb else ""
        print("\ntask %s (TCB 0x%08x)%s" % (dump.task_name(t["tcb"]), t["tcb"], mark))
        for i, pc in enumerate(dump.backtrace(t)):
            print("  #%-2d 0x%08x %s" % (i, pc, table.name(pc)))


def find_elf(sha, given):
    """The ELF of the build with that ELF SHA-256 prefix: --elf, .pio/elf/, or the last build's."""
    if given:
        return given
    if sha:
        found = sorted(glob.glob(os.path.join(ROOT, ".pio", "elf", sha + "*.elf")))
        if found:
            return found[0]
        binfile = os.path.join(BUILD, "firmware.bin")
        if os.path.exists(binfile) and image_elf_sha(binfile).startswith(sha[:16]):
            return os.path.join(BUILD, "firmware.elf")
    return None


def cmd_crash(ip, a):
    if a.file:
        body = open(a.file, "rb").read()
        path = a.file
    else:
        status, body = request(ip, "GET", "/coredump", timeout=60)
        if status != 200:
            print(body.decode(errors="replace").strip())
            return
        path = os.path.join(BUILD if os.path.isdir(BUILD) else ".", "coredump-%s.bin" % time.strftime("%Y%m%d-%H%M%S"))
        with open(path, "wb") as f:
            f.write(body)
    dump = CoreDump(body)
    crashed = [t for t in dump.tasks if t["tcb"] == dump.crashed_tcb]
    print("core dump: %s (%d bytes), build %s, task %s" % (path, len(body), dump.sha or "?",
                                                         dump.task_name(crashed[0]["tcb"]) if crashed else "?"))
    elf = None if a.symbols else find_elf(dump.sha, a.elf)
    if elf and os.path.exists(elf):
        py = os.path.expanduser("~/.platformio/penv/bin/python")
        if not os.path.exists(py):
            py = sys.executable
        gdb = find_gdb()
        cmd = [py, "-m", "esp_coredump", "info_corefile", "-t", "raw", "-c", path] + (["--gdb", gdb] if gdb else [])
        print("decoding with %s ...\n" % elf)
        if not gdb:
            print("(no xtensa-esp32s3-elf-gdb found: install it with the ESP-IDF tools)")
        core = os.path.splitext(path)[0] + ".core.elf"
        subprocess.run(cmd + ["--save-core", core, elf])
        if gdb and os.path.exists(core):
            # the variables kept in every dump (COREDUMP_DRAM_ATTR)
            r = subprocess.run([gdb, "-batch", "-nx", "-ex", "set print pretty on",
                                "-ex", "print 'scan_service.c'::S", elf, core], capture_output=True, text=True)
            m = re.search(r"^\$1 = (.*)", r.stdout, re.M | re.S)
            if m:
                print("\nscan service state (scan_service.c S):\n" + m.group(1).strip())
    else:
        # no ELF of that build: the function names the board keeps with each build
        table = None
        if a.table:
            table = gen_symbols.Table(open(a.table, "rb").read())
        elif ip:
            status, data = request(ip, "GET", "/symbols?sha=" + dump.sha, timeout=60)
            if status == 200:
                table = gen_symbols.Table(data)
            else:
                print("no symbol table on the board: %s" % data.decode(errors="replace").strip())
        if table and dump.sha and not table.elf_sha.hex().startswith(dump.sha):
            print("WARNING: the symbol table is of build %s, not %s: the names are wrong"
                  % (table.elf_sha.hex()[:16], dump.sha))
        if table:
            print("no ELF of build %s here: names from its symbol table (functions only)\n" % dump.sha)
            decode_with_symbols(dump, table)
        else:
            print("no ELF and no symbol table of build %s: addresses only" % dump.sha)
            decode_with_symbols(dump, gen_symbols.Table(gen_symbols.build_table([], bytes(32))))
    if a.erase and not a.file:
        print(request(ip, "POST", "/coredump/erase")[1].decode().strip())


def cmd_panic(ip, a):
    print(request(ip, "POST", "/panic")[1].decode().strip())


def cmd_level(ip, a):
    print(request(ip, "POST", "/loglevel?tag=%s&level=%s" % (a.tag, a.level))[1].decode().strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="board address (default: <HOSTNAME>.local from .env)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("log")
    p.add_argument("-f", "--follow", action="store_true")
    p = sub.add_parser("upload")
    p.add_argument("image", nargs="?")
    sub.add_parser("reboot")
    p = sub.add_parser("crash")
    p.add_argument("--erase", action="store_true", help="erase the dump on the board afterwards")
    p.add_argument("--elf", help="firmware.elf of the build that crashed (default: .pio/elf/<sha>.elf, the last build)")
    p.add_argument("--symbols", action="store_true", help="decode with the board's symbol table even if the ELF is here")
    p.add_argument("--file", help="a saved core dump instead of the board's")
    p.add_argument("--table", help="the symbol table (or a firmware.bin with one) instead of the board's")
    sub.add_parser("panic")
    p = sub.add_parser("level")
    p.add_argument("tag")
    p.add_argument("level")
    a = ap.parse_args()

    host = a.host or os.environ.get("REMOTE_HOST") or "%s.local" % (config_value("HOSTNAME") or "canon-mg2500")
    offline = a.cmd == "crash" and a.file and (a.table or a.elf)
    ip = None if offline else resolve(host)
    try:
        globals()["cmd_" + a.cmd](ip, a)
    except (OSError, http.client.HTTPException) as e:
        sys.exit("%s (%s): %s -- is the board running MODE=debug?" % (host, ip, e))


if __name__ == "__main__":
    main()
