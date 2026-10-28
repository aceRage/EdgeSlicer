#!/usr/bin/env python3
"""Showcase golden-image harness: build a fixture project.

Copies a model 3MF (resources/handy_models/...) and adds a Showcase scene file to it as
Metadata/showcase/scenes.json, the entry the 3MF importer reads (Showcase::ARCHIVE_SCENES).

    python -I make_fixture.py <model.3mf> <scenes.json> <out.3mf> [<settings.3mf>]

<settings.3mf>: another project whose Metadata/project_settings.config (printer, filament and
process) is copied in, so a fresh datadir can slice the plate (its "Default Printer" has no
filament and does not validate). golden.sh uses a U1 project from tests/data for --slice fixtures.
"""
import json
import sys
import zipfile

SCENES_ENTRY = "Metadata/showcase/scenes.json"
SETTINGS_ENTRY = "Metadata/project_settings.config"


def main(argv):
    if len(argv) not in (4, 5):
        sys.stderr.write(__doc__)
        return 2
    model, scenes, out = argv[1], argv[2], argv[3]
    settings = None
    if len(argv) == 5:
        with zipfile.ZipFile(argv[4]) as s:
            settings = s.read(SETTINGS_ENTRY)
        json.loads(settings.decode("utf-8"))  # refuse settings that do not parse
    with open(scenes, "rb") as f:
        text = f.read()
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
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
