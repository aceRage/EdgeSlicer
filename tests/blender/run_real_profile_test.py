#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Open in Blender against a NON-factory profile (the owner's hand test of #841 found what
--factory-startup hides): a scratch Blender profile with Cycles and the glTF importer switched off
and the add-on installed permanently (Blender's own `extension install-file`), then the
launch script on a package whose path has spaces, rendering a still.

    python tests/blender/run_real_profile_test.py <blender exe> <package folder or zip> <work dir>
           [--installed-version same|older|none]

Every Blender user folder points into <work dir>: the user's own Blender profile is never read or
written. Exit 0 when the still was rendered and is not black.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ADDON = os.path.join(ROOT, "resources", "blender", "edgeslicer_render")
LAUNCH = os.path.join(ROOT, "resources", "blender", "edgeslicer_launch.py")


def env_for(work):
    env = dict(os.environ)
    prof = os.path.join(work, "profile")
    for var, sub in (("BLENDER_USER_RESOURCES", ""), ("BLENDER_USER_CONFIG", "config"), ("BLENDER_USER_SCRIPTS", "scripts"),
                     ("BLENDER_USER_DATAFILES", "datafiles"), ("BLENDER_USER_EXTENSIONS", "extensions")):
        env[var] = os.path.join(prof, sub) if sub else prof
    return env


def run(cmd, env, log, timeout=1800):
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
    with open(log, "a", encoding="utf-8", errors="replace") as f:
        f.write("$ %s\n%s\n%s\n" % (" ".join(cmd), proc.stdout, proc.stderr))
    return proc


def addon_zip(path, version=None):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for base, dirs, files in os.walk(ADDON):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for name in files:
                if name.endswith(".pyc"):
                    continue
                full = os.path.join(base, name)
                data = open(full, "rb").read()
                if version and name == "__init__.py":
                    data = re.sub(rb'"version": \(\d+, \d+, \d+\)', b'"version": (%d, %d, %d)' % version, data)
                if version and name == "blender_manifest.toml":
                    data = re.sub(rb'\nversion = "[^"]+"', b'\nversion = "%d.%d.%d"' % version, data)
                z.writestr("edgeslicer_render/" + os.path.relpath(full, ADDON).replace("\\", "/"), data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blender")
    ap.add_argument("package")
    ap.add_argument("work")
    ap.add_argument("--installed-version", choices=["same", "older", "none"], default="same")
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    env = env_for(work)
    log = os.path.join(work, "blender.log")

    # 1. A profile with Cycles and the glTF importer switched off, saved like a user would.
    setup = ("import addon_utils, bpy\n"
             "for m in ('cycles', 'io_scene_gltf2'):\n"
             "    addon_utils.disable(m, default_set=True)\n"
             "bpy.ops.wm.save_userpref()\n"
             "print('PROFILE', sorted(bpy.context.preferences.addons.keys()))\n")
    p = run([a.blender, "-b", "--factory-startup", "--python-expr", setup], env, log)
    if "PROFILE" not in p.stdout:
        print("FAIL: could not make the scratch profile (%s)" % log)
        return 1
    print([l for l in p.stdout.splitlines() if l.startswith("PROFILE")][0])

    # 2. The add-on installed permanently by Blender itself (same version, or an older one).
    if a.installed_version != "none":
        z = os.path.join(work, "edgeslicer_render.zip")
        addon_zip(z, (1, 0, 0) if a.installed_version == "older" else None)
        p = run([a.blender, "--command", "extension", "install-file", "-r", "user_default", "-e", z], env, log)
        print("install-file exit %d" % p.returncode)

    # 3. Open in Blender's command line, on a copy of the package under a path with spaces.
    spaced = os.path.join(work, "Edge Slicer temp", "plate 1")
    if os.path.isdir(a.package):
        shutil.copytree(a.package, spaced)
    else:
        os.makedirs(os.path.dirname(spaced), exist_ok=True)
        spaced += os.path.splitext(a.package)[1]
        shutil.copyfile(a.package, spaced)
    still = os.path.join(work, "out still", "still.png")
    report = os.path.join(work, "report.json")
    os.makedirs(os.path.dirname(still), exist_ok=True)
    # No --factory-startup: the scratch profile (Cycles off, the add-on installed) is what Blender loads.
    p = run([a.blender, "-b", "--python", LAUNCH, "--", "--package", spaced, "--render", still, "--samples", "16",
             "--device", "CPU", "--resolution-percentage", "50", "--report", report], env, log)
    lines = [l for l in p.stdout.splitlines() if l.startswith("EdgeSlicer:")]
    for l in lines:
        print(l[:240])
    if p.returncode != 0 or not os.path.isfile(still) or not os.path.isfile(report):
        print("FAIL: exit %d (%s)" % (p.returncode, log))
        return 1
    rep = json.load(open(report, encoding="utf-8"))
    # Brightness through Blender (no image library needed here).
    probe = ("import bpy, sys\nimg = bpy.data.images.load(sys.argv[-1])\npx = img.pixels[:]\n"
             "n = len(px) // 4\nprint('MEAN', sum(px[i*4] + px[i*4+1] + px[i*4+2] for i in range(n)) / (3 * max(1, n)))\n")
    p = run([a.blender, "-b", "--factory-startup", "--python-expr", probe, "--", still], env, log)
    mean = float(next((l.split()[1] for l in p.stdout.splitlines() if l.startswith("MEAN")), "0"))
    ok = rep.get("objects", 0) > 0 and mean > 0.02
    print("%s: %d objects, %d materials, view transform %s, still %s mean %.3f" % (
        "ok" if ok else "FAIL", rep.get("objects", 0), rep.get("materials", 0), rep.get("view_transform"), still, mean))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
