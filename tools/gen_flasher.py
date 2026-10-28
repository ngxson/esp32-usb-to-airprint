#!/usr/bin/env python3
"""Builds the web installer site (flasher/) from a firmware build.

  tools/gen_flasher.py OUT_DIR VARIANT [--label TEXT]

Copies the firmware of the last `pio run -e esp32s3` into
OUT_DIR/firmware/VARIANT/, writes OUT_DIR/manifest-VARIANT.json for
esp-web-tools, and puts the page (flasher/index.html) and esp-web-tools
itself (flasher/node_modules, `cd flasher && npm ci`) next to them. Run it once
per variant (normal, debug, ...) into the same OUT_DIR; the page lists the
manifests it finds in OUT_DIR/variants.json.

The parts are flashed separately, not as one merged image: a merged image
fills the gaps with 0xff, which would erase the NVS partition and with it the
WiFi network on every update. ota_data_initial.bin (blank OTA data) makes the
board start the app just written, even after OTA updates moved it to ota_1.

Try it locally: python3 -m http.server -d OUT_DIR (Web Serial needs
localhost or https).
"""
import argparse
import json
import os
import re
import shutil
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, ".pio", "build", "esp32s3")
# PlatformIO's names for what ESP-IDF's flasher_args.json lists
PARTS = [("bootloader", "bootloader.bin"), ("partition-table", "partitions.bin"),
         ("otadata", "ota_data_initial.bin"), ("app", "firmware.bin")]


def firmware_name():
    """IMPROV_FIRMWARE_NAME: the installer compares the board's with the manifest's."""
    text = open(os.path.join(ROOT, "src", "improv_esp.h"), encoding="utf-8").read()
    return re.search(r'#define IMPROV_FIRMWARE_NAME "([^"]+)"', text).group(1)


def app_version(path):
    """The version in the image's esp_app_desc_t (after the 24-byte image and 8-byte segment headers)."""
    with open(path, "rb") as f:
        head = f.read(32 + 256)
    magic, = struct.unpack_from("<I", head, 32)
    if magic != 0xABCD5432:
        raise SystemExit("%s: no app description (not an ESP-IDF app image?)" % path)
    return head[48:80].split(b"\0")[0].decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("variant", help="normal, debug, ...: the MODE the firmware was built with")
    ap.add_argument("--label", help="what the page calls it (default: the variant)")
    args = ap.parse_args()

    flasher_args = json.load(open(os.path.join(BUILD, "flasher_args.json")))
    fw_dir = os.path.join(args.out, "firmware", args.variant)
    os.makedirs(fw_dir, exist_ok=True)
    parts = []
    for key, name in PARTS:
        shutil.copyfile(os.path.join(BUILD, name), os.path.join(fw_dir, name))
        parts.append({"path": "firmware/%s/%s" % (args.variant, name),
                      "offset": int(flasher_args[key]["offset"], 16)})
    version = app_version(os.path.join(BUILD, "firmware.bin"))
    manifest = {
        "name": firmware_name(),
        "version": version,
        "new_install_prompt_erase": True,
        # the firmware answers Improv a few seconds after the reset; generous,
        # since the installer asks every second and stops at the first answer
        "new_install_improv_wait_time": 20,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    with open(os.path.join(args.out, "manifest-%s.json" % args.variant), "w") as f:
        json.dump(manifest, f, indent=2)

    # the variants the page offers, in the order they were added
    vpath = os.path.join(args.out, "variants.json")
    variants = json.load(open(vpath)) if os.path.exists(vpath) else []
    variants = [v for v in variants if v["id"] != args.variant]
    variants.append({"id": args.variant, "label": args.label or args.variant, "version": version,
                     "manifest": "manifest-%s.json" % args.variant})
    with open(vpath, "w") as f:
        json.dump(variants, f, indent=2)

    shutil.copyfile(os.path.join(ROOT, "flasher", "index.html"), os.path.join(args.out, "index.html"))
    ewt = os.path.join(ROOT, "flasher", "node_modules", "esp-web-tools", "dist", "web")
    if not os.path.isdir(ewt):
        raise SystemExit("esp-web-tools missing: cd flasher && npm ci")
    shutil.copytree(ewt, os.path.join(args.out, "esp-web-tools"), dirs_exist_ok=True)
    print("gen_flasher: %s %s -> %s" % (args.variant, version, args.out))


main()
