#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the EdgeSlicer Render add-on's installable zip from resources/blender/edgeslicer_render
(design 5.6: the zip for the GitHub release assets; EdgeSlicer itself packs the same folder when the
user clicks "Install the Blender add-on").

    python tools/blender_addon/build_addon_zip.py [<out.zip>] [--validate <blender exe>]

The zip holds one top folder, edgeslicer_render/, with blender_manifest.toml (Blender 4.2+
extensions) and bl_info in __init__.py (3.6 legacy add-ons), so both installs accept it. Entries are
sorted and time-stamped 2024-01-01, so the same sources give the same zip. --validate runs
`blender --command extension validate <zip>` (read-only) on it.
"""

import argparse
import os
import subprocess
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SRC = os.path.join(ROOT, "resources", "blender", "edgeslicer_render")


def build(out):
    entries = []
    for base, dirs, files in os.walk(SRC):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__")
        for name in sorted(files):
            if name.endswith(".pyc"):
                continue
            full = os.path.join(base, name)
            entries.append((os.path.join("edgeslicer_render", os.path.relpath(full, SRC)).replace("\\", "/"), full))
    entries.sort()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, full in entries:
            info = zipfile.ZipInfo(arc, date_time=(2024, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "edgeslicer_render.zip"))
    ap.add_argument("--validate", metavar="BLENDER")
    a = ap.parse_args()
    out = build(os.path.abspath(a.out))
    print("wrote %s" % out)
    if a.validate:
        proc = subprocess.run([a.validate, "--factory-startup", "--command", "extension", "validate", out], capture_output=True, text=True)
        print(proc.stdout.strip() or proc.stderr.strip())
        return proc.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
