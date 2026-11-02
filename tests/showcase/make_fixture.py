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
        blobs.append((IMAGE_DIR + sha + ".png", data))
        text = text.replace(("@IMAGE%d@" % n).encode("ascii"), sha.encode("ascii"))
    json.loads(text.decode("utf-8"))  # refuse a fixture that does not parse
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
