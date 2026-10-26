#!/usr/bin/env python3
"""Showcase golden-image harness: build a fixture project.

Copies a model 3MF (resources/handy_models/...) and adds a Showcase scene file to it as
Metadata/showcase/scenes.json, the entry the 3MF importer reads (Showcase::ARCHIVE_SCENES).

    python -I make_fixture.py <model.3mf> <scenes.json> <out.3mf>
"""
import json
import sys
import zipfile

SCENES_ENTRY = "Metadata/showcase/scenes.json"


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    model, scenes, out = argv[1], argv[2], argv[3]
    with open(scenes, "rb") as f:
        text = f.read()
    json.loads(text.decode("utf-8"))  # refuse a fixture that does not parse
    with zipfile.ZipFile(model) as src, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as dst:
        for info in src.infolist():
            if info.filename.lower() == SCENES_ENTRY.lower():
                continue
            dst.writestr(info, src.read(info.filename))
        dst.writestr(SCENES_ENTRY, text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
