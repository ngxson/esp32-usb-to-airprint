#!/usr/bin/env python3
"""Claims the documentation must make (run by tools/run_tests.sh)."""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []


def read(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as f:
        return f.read()


readme, agents = read("README.md"), read("AGENTS.md")

# the MF221d (MF220 Series) in the supported table, with its scanner
row = next((l for l in readme.splitlines() if l.startswith("|") and "MF220" in l), "")
if not row:
    fails.append("README: no MF220 row in the supported printers")
elif "MF221d" not in row or not re.search(r"\|\s*yes\b", row):
    fails.append("README: the MF220 row does not list the MF221d with its scanner: " + row)

# AGENTS.md: the MF220 scanner protocol and how it was verified
m = re.search(r"^## Scanner protocol \(MF220 series\)\n(.*?)(?=^## )", agents, re.S | re.M)
if not m:
    fails.append("AGENTS.md: no 'Scanner protocol (MF220 series)' section")
else:
    sec = m.group(1)
    for needed in ("pixma_imageclass.c", "0xd460", "fake_iclass", "On the MF221d"):
        if needed not in sec:
            fails.append("AGENTS.md MF220 scanner section lacks " + needed)

for f in fails:
    print("FAIL:", f)
print("docs: ok" if not fails else "docs: %d FAILED" % len(fails))
sys.exit(1 if fails else 0)
