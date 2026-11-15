# SPDX-License-Identifier: GPL-3.0-or-later
# EdgeSlicer Render - reading an "edgeslicer-blender-package" (a folder, or the zip of one).

import hashlib
import json
import os
import tempfile
import zipfile

FORMAT = "edgeslicer-blender-package"
CAMERA_PATH_FORMAT = "edgeslicer-blender-camera-path"
# The newest package version this add-on reads (design 5.5: every version up to its own).
SUPPORTED_VERSION = 1


class PackageError(Exception):
    pass


class Package:
    """A package on disk: `root` is the folder holding showcase.json."""

    def __init__(self, root, data, camera_path=None, source=None):
        self.root = root
        self.data = data
        self.camera_path = camera_path
        self.source = source or root

    def path(self, rel):
        if not rel:
            return None
        full = os.path.normpath(os.path.join(self.root, rel))
        # Never outside the package.
        if os.path.commonpath([os.path.abspath(full), os.path.abspath(self.root)]) != os.path.abspath(self.root):
            raise PackageError("a path leaves the package: %s" % rel)
        return full if os.path.exists(full) else None

    def get(self, *keys, default=None):
        node = self.data
        for k in keys:
            if not isinstance(node, dict) or k not in node:
                return default
            node = node[k]
        return default if node is None else node

    @property
    def frames(self):
        return (self.camera_path or {}).get("frames") or []


def _extract_zip(path):
    """A zip is unpacked once into the temp folder (by its content hash) and read from there."""
    with open(path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()[:16]
    stem = os.path.splitext(os.path.basename(path))[0]
    target = os.path.join(tempfile.gettempdir(), "edgeslicer_packages", "%s_%s" % (stem, digest))
    if os.path.isfile(os.path.join(target, "showcase.json")):
        return target
    os.makedirs(target, exist_ok=True)
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            if name.startswith("/") or ".." in name.split("/"):
                raise PackageError("the zip holds an unsafe path: %s" % name)
            z.extract(info, target)
    return target


def find_root(path):
    """The package folder for a folder, a zip, or showcase.json / scene.glb inside one."""
    if not path:
        raise PackageError("no package given")
    path = os.path.abspath(os.path.expanduser(path))
    if os.path.isfile(path) and path.lower().endswith((".zip", ".edgeshow")):
        path = _extract_zip(path)
    elif os.path.isfile(path):
        path = os.path.dirname(path)
    if not os.path.isfile(os.path.join(path, "showcase.json")):
        # A zip of a folder: one level down.
        subs = [os.path.join(path, d) for d in sorted(os.listdir(path))] if os.path.isdir(path) else []
        subs = [d for d in subs if os.path.isfile(os.path.join(d, "showcase.json"))]
        if len(subs) == 1:
            path = subs[0]
        else:
            raise PackageError("no showcase.json in %s" % path)
    return path


def load(path):
    source = path
    root = find_root(path)
    with open(os.path.join(root, "showcase.json"), "r", encoding="utf-8") as f:
        data = json.load(f)
    if data.get("format") != FORMAT:
        raise PackageError("not an EdgeSlicer Blender package (format %r)" % data.get("format"))
    version = int(data.get("version", 0))
    if version < 1:
        raise PackageError("the package has no version")
    if version > SUPPORTED_VERSION:
        raise PackageError(
            "This package is version %d; this add-on reads up to version %d. Update the EdgeSlicer add-on "
            "(EdgeSlicer installs the matching one with Open in Blender)." % (version, SUPPORTED_VERSION))
    camera_path = None
    if data.get("camera_path"):
        cp = os.path.join(root, data["camera_path"])
        if os.path.isfile(cp):
            with open(cp, "r", encoding="utf-8") as f:
                camera_path = json.load(f)
            if camera_path.get("format") != CAMERA_PATH_FORMAT or int(camera_path.get("version", 0)) > SUPPORTED_VERSION:
                camera_path = None
    if not os.path.isfile(os.path.join(root, data.get("glb", "scene.glb"))):
        raise PackageError("the package has no %s" % data.get("glb", "scene.glb"))
    return Package(root, data, camera_path, source)
