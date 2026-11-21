#!/usr/bin/env python3
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
"""Emit displayxr-detect.json, this plug-in's self-description for the DisplayXR
component catalog (displayxr-runtime#2737; schema in displayxr-runtime
docs/specs/runtime/component-catalog.md, "Display plug-ins").

Single source: the detect rules come from src/drv_leia/leia_edid_table.h (the
same table every platform's panel probe reads), and the runtime floor from the
DXR_RUNTIME_GIT_TAG pin that installer/CMakeLists.txt derives
MIN_RUNTIME_VERSION from. So "the installer thinks this is a Leia panel" and
"the plug-in claims it" cannot diverge.

The release workflow attaches the output to every v* release; the catalog
generator ingests it, and falls back to parsing these same files for older
releases that do not carry it.

    python3 scripts/gen_detect_manifest.py [--out displayxr-detect.json]

Stdlib only.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TABLE = os.path.join(ROOT, "src", "drv_leia", "leia_edid_table.h")
TOP_CMAKE = os.path.join(ROOT, "CMakeLists.txt")

# The schema-2 catalog item id (= the display processor's registered id).
ITEM_ID = "leia-sr"
LABEL = "Leia display plug-in"
# Where installer/DisplayXRLeiaSRInstaller.nsi writes the installed version.
REGISTRY_VERSION = "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version"

# The SR platform reports a connected SR device: positive evidence even when
# the panel's EDID is not in the table yet.
SR_REGISTRY_RULE = {
    "kind": "registry_dword_min",
    "key": "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality",
    "value": "NConnectedDevices",
    "min": 1,
    "confidence": "high",
    "note": "SR platform reports a connected SR device",
}

EDID_ROW_RE = re.compile(r"^\s*\{\s*(\d+)\s*,\s*(\d+)\s*\}\s*,?\s*(?://\s*(.*))?$")
# Anchored to the start of a line: a comment quoting the set() call must not win.
PIN_RE = re.compile(r'^set\(DXR_RUNTIME_GIT_TAG\s+"v?(\d+\.\d+\.\d+)"', re.M)
# Lines inside the array that are not rows: blank, or a whole-line // comment.
SKIP_RE = re.compile(r"^\s*(?://.*)?$")


def edid_rules(text):
    body = text.split("leia_edid_table[][2] = {", 1)
    if len(body) != 2:
        sys.exit("error: leia_edid_table array not found")
    rules, note = [], ""
    for line in body[1].split("};", 1)[0].splitlines():
        m = EDID_ROW_RE.match(line)
        if not m:
            if SKIP_RE.match(line):
                continue
            # A row this parser cannot read (hex literal, /* */ comment, two rows
            # on one line) would otherwise be dropped silently, and the partial
            # manifest would override the catalog's own full parse.
            sys.exit(f"error: leia_edid_table line not understood: {line.strip()!r}")
        man, prod = int(m.group(1)), int(m.group(2))
        if man > 0xFFFF or prod > 0xFFFF:
            sys.exit(f"error: EDID row {{{man}, {prod}}} is not two 16-bit words")
        # A row without its own comment continues the previous row's range
        # (e.g. the Samsung product-id runs), so it inherits that note.
        if m.group(3):
            note = m.group(3).strip()
        rules.append({
            "kind": "edid",
            "man": man,
            "prod": prod,
            # Product ids 0-15 are the generic Acer DS ids, shared with ordinary
            # monitors: reported, never an unattended install.
            "confidence": "low" if prod < 16 else "high",
            "note": note,
        })
    if not rules:
        sys.exit("error: leia_edid_table parsed to zero rows")
    return rules


def build(require_floor=False):
    with open(TABLE, encoding="utf-8") as f:
        rules = edid_rules(f.read())
    rules.append(dict(SR_REGISTRY_RULE))
    manifest = {"schema": 1, "id": ITEM_ID, "label": LABEL, "registry_version": REGISTRY_VERSION}
    with open(TOP_CMAKE, encoding="utf-8") as f:
        m = PIN_RE.search(f.read())
    if m:
        manifest["requires"] = {"runtime": ">=" + m.group(1)}
    elif require_floor:
        sys.exit("error: DXR_RUNTIME_GIT_TAG is not a vX.Y.Z tag; a release must carry requires.runtime")
    else:
        # Same rule as installer/CMakeLists.txt: a non-release pin has no floor.
        print("WARNING: DXR_RUNTIME_GIT_TAG is not a vX.Y.Z tag; requires.runtime omitted", file=sys.stderr)
    manifest["detect"] = rules
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="displayxr-detect.json")
    ap.add_argument("--require-floor", action="store_true",
                    help="fail instead of warning when the runtime pin is not a vX.Y.Z tag (release builds)")
    args = ap.parse_args()
    manifest = build(require_floor=args.require_floor)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n")
    n_edid = sum(1 for r in manifest["detect"] if r["kind"] == "edid")
    print(f"wrote {args.out}: {n_edid} EDID rules + SR registry rule, "
          f"requires {manifest.get('requires', {}).get('runtime', 'none')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
