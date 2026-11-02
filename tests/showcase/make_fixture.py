#!/usr/bin/env python3
"""Showcase golden-image harness: build a fixture project.

Copies a model 3MF (resources/handy_models/...) and adds a Showcase scene file to it as
Metadata/showcase/scenes.json, the entry the 3MF importer reads (Showcase::ARCHIVE_SCENES).

    python -I make_fixture.py [--image <file.png>]... <model.3mf> <scenes.json> <out.3mf> [<settings.3mf>]

<settings.3mf>: another project whose Metadata/project_settings.config (printer, filament and
process) is copied in, so a fresh datadir can slice the plate (its "Default Printer" has no
filament and does not validate). golden.sh uses a U1 project from tests/data for --slice fixtures.

--image (1h-2): a PNG stored with the scenes as Metadata/showcase/<sha256>.png, as the app stores a
scene's pictures; "@IMAGE0@", "@IMAGE1@", ... in the scenes text become the first, second, ... sha.
A JPEG is stored as .jpg (as the app stores one), anything else as .png.

"_fixture" (1h-2b, top level of the scenes file, removed before it is stored): {"settings": {key: value}}
overrides keys of the copied project_settings.config, e.g. supports on for a toolpath fixture.
"""
import hashlib
import json
import sys
import zipfile

SCENES_ENTRY = "Metadata/showcase/scenes.json"
SETTINGS_ENTRY = "Metadata/project_settings.config"
IMAGE_DIR = "Metadata/showcase/"


def main(argv):
    images = []
    args = []
    i = 1
    while i < len(argv):
        if argv[i] == "--image" and i + 1 < len(argv):
            images.append(argv[i + 1])
            i += 2
        else:
            args.append(argv[i])
            i += 1
    if len(args) not in (3, 4):
        sys.stderr.write(__doc__)
        return 2
    model, scenes, out = args[0], args[1], args[2]
    settings = None
    if len(args) == 4:
        with zipfile.ZipFile(args[3]) as s:
            settings = s.read(SETTINGS_ENTRY)
        json.loads(settings.decode("utf-8"))  # refuse settings that do not parse
    with open(scenes, "rb") as f:
        text = f.read()
    blobs = []
    for n, path in enumerate(images):
        with open(path, "rb") as f:
            data = f.read()
        sha = hashlib.sha256(data).hexdigest()
        ext = ".jpg" if data[:2] == bytes((0xFF, 0xD8)) else ".png"
        blobs.append((IMAGE_DIR + sha + ext, data))
        text = text.replace(("@IMAGE%d@" % n).encode("ascii"), sha.encode("ascii"))
    doc = json.loads(text.decode("utf-8"))  # refuse a fixture that does not parse
    fixture = doc.pop("_fixture", None) if isinstance(doc, dict) else None
    if fixture is not None:
        text = json.dumps(doc, indent=2).encode("utf-8")
        overrides = fixture.get("settings", {})
        if overrides:
            if settings is None:
                sys.stderr.write("_fixture.settings needs <settings.3mf>" + chr(10))
                return 2
            cfg = json.loads(settings.decode("utf-8"))
            cfg.update({k: str(v) for k, v in overrides.items()})
            # The project keeps only the process keys listed as changed from its system preset.
            diff = cfg.get("different_settings_to_system")
            if isinstance(diff, list) and diff:
                listed = [k for k in diff[0].split(";") if k]
                diff[0] = ";".join(listed + [k for k in overrides if k not in listed])
            settings = json.dumps(cfg, indent=4).encode("utf-8")
    skip = {SCENES_ENTRY.lower()}
    if settings is not None:
        skip.add(SETTINGS_ENTRY.lower())
    with zipfile.ZipFile(model) as src, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            if info.filename.lower() in skip:
                continue
            dst.writestr(info, src.read(info.filename))
        if settings is not None:
            dst.writestr(SETTINGS_ENTRY, settings)
        dst.writestr(SCENES_ENTRY, text)
        for name, data in blobs:
            dst.writestr(name, data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
