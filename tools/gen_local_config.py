"""Generate src/local_config.h from .env.example (committed defaults) and .env
(git-ignored local settings, e.g. WiFi credentials). .env is created from
.env.example if missing.

Runs automatically before every PlatformIO build (extra_scripts in
platformio.ini); can also be run by hand: python3 tools/gen_local_config.py

Format: KEY=VALUE per line, '#' comments, optional quotes around VALUE.
Every key becomes `#define KEY "VALUE"`.
"""
import os
import re
import shutil

try:
    Import("env")  # noqa: F821  (PlatformIO / SCons)
    ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
except NameError:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXAMPLE_FILE = os.path.join(ROOT, ".env.example")
LOCAL_FILE = os.path.join(ROOT, ".env")
OUT_FILE = os.path.join(ROOT, "src", "local_config.h")

KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def parse(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise SystemExit("%s:%d: expected KEY=VALUE" % (path, n))
            key, value = line.split("=", 1)
            key, value = key.strip(), value.strip()
            if key.startswith("export "):
                key = key[len("export "):].strip()
            if not KEY_RE.match(key):
                raise SystemExit("%s:%d: invalid key %r" % (path, n, key))
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            values[key] = value
    return values


def c_string(s):
    out = []
    for ch in s.encode("utf-8"):
        c = chr(ch)
        if c in '"\\':
            out.append("\\" + c)
        elif 0x20 <= ch < 0x7F and c != "?":   # '?' avoids trigraphs
            out.append(c)
        else:
            out.append("\\%03o" % ch)
    return '"' + "".join(out) + '"'


def main():
    if not os.path.exists(LOCAL_FILE):
        shutil.copyfile(EXAMPLE_FILE, LOCAL_FILE)
        print("Created .env from .env.example: put your WiFi credentials in it")

    values = parse(EXAMPLE_FILE) if os.path.exists(EXAMPLE_FILE) else {}
    values.update(parse(LOCAL_FILE))

    lines = ["// Generated from .env.example and .env by tools/gen_local_config.py. Do not edit.",
             "#pragma once", ""]
    for key in sorted(values):
        lines.append("#define %s %s" % (key, c_string(values[key])))
    text = "\n".join(lines) + "\n"

    old = open(OUT_FILE, encoding="utf-8").read() if os.path.exists(OUT_FILE) else None
    if text != old:     # only touch the file when something changed (avoids rebuilds)
        with open(OUT_FILE, "w", encoding="utf-8") as f:
            f.write(text)
    if not values.get("WIFI_SSID"):
        print("Note: WIFI_SSID is empty: set the network from the web installer (or in .env)")


main()
