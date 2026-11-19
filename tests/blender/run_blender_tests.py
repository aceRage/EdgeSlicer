#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Blender add-on's test (design 5.7): headless import and a 16-sample still.

    python tests/blender/run_blender_tests.py [--blender <exe>] [--package <folder or zip> ...] [--keep <dir>]

Without --package it writes fixture packages with make_fixture_package.py (a still with three
finishes, and a 12-frame video with a build-up). Every package's scene.glb is checked first (the GLB
container, accessors and indices, in plain Python). Then each Blender found runs import_and_render.py.

Blender is looked for in BLENDER, then the newest "Blender X.Y" in Program Files (Windows),
/Applications/Blender.app (macOS) and `blender` on PATH; --blender may be given several times. No
Blender: the test says so and passes as skipped (exit 0). Blender runs with --factory-startup and
its user folders pointed at a temporary folder, so the user's own Blender setup is never read or
written.
"""

import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def find_blenders():
    found = []
    if os.environ.get("BLENDER"):
        found.append(os.environ["BLENDER"])
    if sys.platform.startswith("win"):
        for root in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", "")):
            if not root:
                continue
            dirs = glob.glob(os.path.join(root, "Blender Foundation", "Blender *"))

            def ver(d):
                m = re.search(r"Blender (\d+)\.(\d+)", d)
                return (int(m.group(1)), int(m.group(2))) if m else (0, 0)

            for d in sorted(dirs, key=ver, reverse=True):
                exe = os.path.join(d, "blender.exe")
                if os.path.isfile(exe) and ver(d) >= (4, 2):
                    found.append(exe)
    elif sys.platform == "darwin":
        exe = "/Applications/Blender.app/Contents/MacOS/Blender"
        if os.path.isfile(exe):
            found.append(exe)
    which = shutil.which("blender")
    if which:
        found.append(which)
    out = []
    for f in found:
        if f not in out:
            out.append(f)
    return out


def check_glb(path):
    """Container, JSON, buffer views, accessors, indices: what a strict loader checks."""
    data = open(path, "rb").read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    assert magic == 0x46546C67 and version == 2, "not a glTF 2.0 GLB"
    assert length == len(data), "the header's length is not the file size"
    jlen, jtype = struct.unpack_from("<II", data, 12)
    assert jtype == 0x4E4F534A and jlen % 4 == 0
    doc = json.loads(data[20:20 + jlen].decode("utf-8"))
    pos = 20 + jlen
    blen, btype = struct.unpack_from("<II", data, pos)
    assert btype == 0x004E4942
    bin_ = data[pos + 8:pos + 8 + blen]
    assert doc["asset"]["version"] == "2.0"
    assert doc["buffers"][0]["byteLength"] <= len(bin_)
    for v in doc["bufferViews"]:
        assert v["byteOffset"] + v["byteLength"] <= len(bin_), "a buffer view leaves the buffer"
    sizes = {5121: 1, 5123: 2, 5125: 4, 5126: 4}
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
    for a in doc["accessors"]:
        v = doc["bufferViews"][a["bufferView"]]
        assert a.get("byteOffset", 0) + a["count"] * sizes[a["componentType"]] * comps[a["type"]] <= v["byteLength"]
    tris = 0
    for m in doc["meshes"]:
        for p in m["primitives"]:
            pa = doc["accessors"][p["attributes"]["POSITION"]]
            assert pa["type"] == "VEC3" and "min" in pa and "max" in pa
            for name, ai in p["attributes"].items():
                assert doc["accessors"][ai]["count"] == pa["count"], name
                assert name in ("POSITION", "NORMAL", "TANGENT") or name.startswith(("TEXCOORD_", "COLOR_", "_")), name
            ia = doc["accessors"][p["indices"]]
            v = doc["bufferViews"][ia["bufferView"]]
            off = v["byteOffset"] + ia.get("byteOffset", 0)
            idx = struct.unpack_from("<%dI" % ia["count"], bin_, off)
            assert max(idx) < pa["count"], "an index past the last vertex"
            assert ia["count"] % 3 == 0
            tris += ia["count"] // 3
    for n in doc["nodes"]:
        assert n.get("mesh", 0) < len(doc["meshes"])
    return {"meshes": len(doc["meshes"]), "materials": len(doc.get("materials", [])), "triangles": tris,
            "extensions": doc.get("extensionsUsed", [])}


def run_one(blender, package, out_dir, user_dir):
    env = dict(os.environ)
    for var in ("BLENDER_USER_RESOURCES", "BLENDER_USER_CONFIG", "BLENDER_USER_SCRIPTS", "BLENDER_USER_DATAFILES",
                "BLENDER_USER_EXTENSIONS"):
        env[var] = os.path.join(user_dir, var.lower())
    cmd = [blender, "-b", "--factory-startup", "--python", os.path.join(HERE, "import_and_render.py"), "--",
           "--package", package, "--out", out_dir]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1800)
    log = os.path.join(out_dir, "blender.log")
    os.makedirs(out_dir, exist_ok=True)
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write(proc.stdout + "\n" + proc.stderr)
    report_path = os.path.join(out_dir, "report.json")
    report = json.load(open(report_path, encoding="utf-8")) if os.path.isfile(report_path) else None
    return proc.returncode, report, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blender", action="append")
    ap.add_argument("--package", action="append")
    ap.add_argument("--keep", help="keep the fixtures, renders and reports here")
    a = ap.parse_args()

    work = a.keep or tempfile.mkdtemp(prefix="edgeslicer_blender_test_")
    os.makedirs(work, exist_ok=True)
    packages = list(a.package or [])
    if not packages:
        for name, extra in (("fixture_still", []), ("fixture_video", ["--video"])):
            out = os.path.join(work, name)
            subprocess.run([sys.executable, os.path.join(HERE, "make_fixture_package.py"), out] + extra, check=True)
            packages.append(out)

    failures = 0
    for p in packages:
        glb = os.path.join(p, "scene.glb") if os.path.isdir(p) else None
        if glb and os.path.isfile(glb):
            try:
                info = check_glb(glb)
                print("GLB ok   %s: %s" % (p, info))
            except (AssertionError, KeyError, struct.error, ValueError) as exc:
                print("GLB FAIL %s: %s" % (p, exc))
                failures += 1

    blenders = a.blender or find_blenders()
    if not blenders:
        print("SKIPPED: no Blender 4.2 or newer found (set BLENDER=<path to blender>); the GLB checks ran.")
        return 1 if failures else 0
    user_dir = os.path.join(work, "blender_user")
    for blender in blenders:
        for p in packages:
            tag = "%s_%s" % (os.path.basename(os.path.dirname(blender)) or "blender", os.path.basename(p.rstrip("/\\")))
            out = os.path.join(work, re.sub(r"[^A-Za-z0-9_.-]", "_", tag))
            code, report, log = run_one(blender, p, out, user_dir)
            if code != 0 or report is None:
                failures += 1
                print("FAIL %s on %s: exit %s, failed checks %s (log %s)" % (p, blender, code, report and report.get("failed"), log))
            else:
                print("ok   %s on %s (Blender %s): %d checks, mean luma %.3f, %s" % (
                    p, blender, report["blender"], len(report["checks"]), report["render"]["mean_luma"], report["render"]["file"]))
    print("work folder: %s" % work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
