#!/usr/bin/env python3
"""Remote access to a board running MODE=debug (see src/debug.h).

  tools/remote.py info                 firmware, memory, tasks, OTA state
  tools/remote.py log [-f]             the log kept on the board; -f follows it live
  tools/remote.py upload [image.bin]   update the firmware over WiFi (default: the last build)
  tools/remote.py reboot
  tools/remote.py crash [--erase]      fetch and decode the last crash's core dump
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
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTTP_PORT = int(os.environ.get("REMOTE_HTTP_PORT", 8080))     # other ports: tests through QEMU
LOG_PORT = int(os.environ.get("REMOTE_LOG_PORT", 2323))
BUILD = os.path.join(ROOT, ".pio", "build", "esp32s3")


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


def cmd_crash(ip, a):
    status, body = request(ip, "GET", "/coredump", timeout=60)
    if status != 200:
        print(body.decode(errors="replace").strip())
        return
    path = os.path.join(BUILD if os.path.isdir(BUILD) else ".", "coredump-%s.bin" % time.strftime("%Y%m%d-%H%M%S"))
    with open(path, "wb") as f:
        f.write(body)
    print("core dump: %s (%d bytes)" % (path, len(body)))
    elf = a.elf or os.path.join(BUILD, "firmware.elf")
    py = os.path.expanduser("~/.platformio/penv/bin/python")
    if not os.path.exists(py):
        py = sys.executable
    gdb = find_gdb()
    cmd = [py, "-m", "esp_coredump", "info_corefile", "-t", "raw", "-c", path] + (["--gdb", gdb] if gdb else [])
    if os.path.exists(elf):
        print("decoding with %s (must be the build that crashed) ...\n" % elf)
        if not gdb:
            print("(no xtensa-esp32s3-elf-gdb found: install it with the ESP-IDF tools)")
        subprocess.run(cmd + [elf])
    else:
        print("decode: " + " ".join(cmd + ["firmware.elf"]))
    if a.erase:
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
    p.add_argument("--elf", help="firmware.elf of the build that crashed (default: the last build)")
    sub.add_parser("panic")
    p = sub.add_parser("level")
    p.add_argument("tag")
    p.add_argument("level")
    a = ap.parse_args()

    host = a.host or os.environ.get("REMOTE_HOST") or "%s.local" % (config_value("HOSTNAME") or "canon-mg2500")
    ip = resolve(host)
    try:
        globals()["cmd_" + a.cmd](ip, a)
    except (OSError, http.client.HTTPException) as e:
        sys.exit("%s (%s): %s -- is the board running MODE=debug?" % (host, ip, e))


if __name__ == "__main__":
    main()
