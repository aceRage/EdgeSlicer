#!/usr/bin/env python3
"""Write resources/info/legacy_compatible_printers.json.

Adapted from OrcaSlicer #16367 (orca_profile_tool.py legacy-compat). EdgeSlicer has no
orca_profile_tool.py, so this is a standalone copy of that command with its own small
profile reader, and it compares the tree with any number of older releases (EdgeSlicer
tags), not just Orca 2.4.2.

User presets store only their diff from their parent, so they take their parent's
compatible_printers. When a profile update renames a system filament preset (with
renamed_from) and narrows its printer list, or narrows the list of a preset that kept its
name, a user preset made against the older release no longer shows on printers it was
made for. The app offers to write those printers into such user presets
(PresetBundle::legacy_user_filament_presets / migrate_legacy_user_filament_presets).

For every instantiated filament preset of an older release, the map lists the printers it
covered then that the preset it now loads as no longer lists, keeping only printers that a
current preset with the same filament_id still covers and that share an extruder variant
with it. Keyed by the older release's preset name, which is what a user preset's inherits
still says. Several older releases are merged: a name keeps the union of what it lost.

usage:
  scripts/legacy_compatible_printers.py --base-tag v2.4.5.5-edge --base-tag v2.4.0.0-edge ...
  scripts/legacy_compatible_printers.py --base /path/to/old/resources/profiles
  scripts/legacy_compatible_printers.py --check --base-tag ...   (fail if the map is stale)
"""

import argparse
import io
import json
import os
import subprocess
import sys
import tarfile

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.normpath(os.path.join(SCRIPTS_DIR, ".."))
PROFILES_DIR = os.path.join(REPO_DIR, "resources", "profiles")
OUTPUT = os.path.join(REPO_DIR, "resources", "info", "legacy_compatible_printers.json")
OFL = "OrcaFilamentLibrary"

# The releases the shipped map is generated against: every EdgeSlicer release a user can have
# made presets on. Add the new tag here at each release.
DEFAULT_BASE_TAGS = [
    "v2.3.6.5-edge", "v2.3.7.0-edge", "v2.3.8.0-edge", "v2.3.8.1-edge", "v2.3.8.5-edge",
    "v2.4.0.0-edge", "v2.4.0.1-edge", "v2.4.1.0-edge", "v2.4.1.1-edge", "v2.4.1.2-edge",
    "v2.4.2.0-edge", "v2.4.3.0-edge", "v2.4.4.0-edge", "v2.4.4.1-edge", "v2.4.4.2-edge",
    "v2.4.5.0-edge", "v2.4.5.1-edge", "v2.4.5.2-edge", "v2.4.5.3-edge", "v2.4.5.4-edge",
    "v2.4.5.5-edge",
]


def _as_list(value):
    if value is None:
        return []
    return value if isinstance(value, list) else [value]


def _renamed_from(data):
    """renamed_from as the loader reads it: one ';'-separated string (or a list)."""
    return [n for v in _as_list(data.get("renamed_from")) for n in str(v).split(";") if n]


def _instantiated(data):
    return str(data.get("instantiation", "")).lower() == "true"


def _parse(raw):
    try:
        return json.loads(raw.decode("utf-8-sig") if isinstance(raw, bytes) else raw)
    except (UnicodeDecodeError, ValueError):
        return None


def _read_json(path):
    try:
        with open(path, "rb") as f:
            return _parse(f.read())
    except OSError:
        return None


class DirTree:
    """A resources/profiles directory on disk."""

    def __init__(self, root):
        self.root = root

    def vendor_files(self):
        return [e for e in os.listdir(self.root)
                if e.endswith(".json") and os.path.isdir(os.path.join(self.root, e[:-5]))]

    def read(self, rel):
        return _read_json(os.path.join(self.root, rel))


class GitTree:
    """resources/profiles of a git revision, its JSON files read into memory (no checkout on disk)."""

    def __init__(self, rev):
        archive = subprocess.run(["git", "-C", REPO_DIR, "archive", "--format=tar", rev, "--",
                                  ":(glob)resources/profiles/**/*.json", ":(glob)resources/profiles/*.json"],
                                 check=True, capture_output=True).stdout
        prefix = "resources/profiles/"
        self.files = {}
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            for member in tar:
                if member.isfile() and member.name.startswith(prefix):
                    self.files[member.name[len(prefix):]] = tar.extractfile(member).read()

    def vendor_files(self):
        dirs = {rel.split("/", 1)[0] for rel in self.files if "/" in rel}
        return [rel for rel in self.files if "/" not in rel and rel.endswith(".json") and rel[:-5] in dirs]

    def read(self, rel):
        raw = self.files.get(rel.replace("\\", "/"))
        return None if raw is None else _parse(raw)


def _vendor_names(tree):
    names = sorted(e[:-5] for e in tree.vendor_files())
    # The library first, as the app loads it first and other vendors inherit from it.
    if OFL in names:
        names.remove(OFL)
        names.insert(0, OFL)
    return names


def _load_section(tree, vendor, section):
    """{preset name: raw json} for one *_list of a vendor bundle."""
    index = tree.read(vendor + ".json") or {}
    out = {}
    for item in index.get(section, []):
        data = tree.read(vendor + "/" + item.get("sub_path", ""))
        if isinstance(data, dict):
            out[item.get("name") or data.get("name")] = data
    return out


def _compose(name, presets, fallback, cache, depth=0):
    """A preset's keys with its inherits (and includes) applied, as the loader composes them."""
    if name in cache:
        return cache[name]
    data = presets.get(name)
    if data is None:
        data = fallback.get(name)
    if data is None or depth > 32:
        return {}
    config = {}
    parent = data.get("inherits")
    if parent:
        config.update(_compose(parent, presets, fallback, cache, depth + 1))
    for inc in _as_list(data.get("include")):
        config.update({k: v for k, v in _compose(inc, presets, fallback, cache, depth + 1).items()
                       if k not in ("name", "inherits", "instantiation", "renamed_from")})
    config.update(data)
    cache[name] = config
    return config


def flatten(tree):
    """(filaments, printers) of a profiles tree (DirTree or GitTree).

    filaments: {(vendor, name): {compatible_printers, filament_id, variants, instantiation, renamed_from}}
    printers:  {name: {renamed_from, variants}} for instantiated machine presets.
    """
    ofl_raw = _load_section(tree, OFL, "filament_list")
    filaments, printers = {}, {}
    for vendor in _vendor_names(tree):
        raw = _load_section(tree, vendor, "filament_list")
        cache = {}
        for name, data in raw.items():
            config = _compose(name, raw, ofl_raw if vendor != OFL else {}, cache)
            filaments[(vendor, name)] = {
                "compatible_printers": set(_as_list(config.get("compatible_printers"))),
                "filament_id": config.get("filament_id"),
                "variants": set(_as_list(config.get("filament_extruder_variant"))),
                "instantiation": _instantiated(data),
                "renamed_from": _renamed_from(data),
            }
        raw = _load_section(tree, vendor, "machine_list")
        cache = {}
        for name, data in raw.items():
            if _instantiated(data):
                config = _compose(name, raw, {}, cache)
                printers[name] = {
                    "renamed_from": _renamed_from(data),
                    "variants": set(_as_list(config.get("printer_extruder_variant"))),
                }
    return filaments, printers


def legacy_compatible_printers(base_trees, current_tree):
    """{old preset name: sorted printers} merged over every base release."""
    new_filaments, new_printers = flatten(current_tree)
    renamed_printer = {old: new for new, rec in new_printers.items() for old in rec["renamed_from"]}

    by_name, by_old_name = {}, {}
    for (vendor, name), rec in new_filaments.items():
        if rec["instantiation"]:
            by_name.setdefault(name, []).append((vendor, rec))
            for old_name in rec["renamed_from"]:
                by_old_name.setdefault(old_name, []).append((vendor, rec))

    covering = {}  # filament_id -> printers some current instantiated preset covers
    for rec in new_filaments.values():
        if rec["instantiation"] and rec["filament_id"]:
            covering.setdefault(rec["filament_id"], set()).update(rec["compatible_printers"])

    def shares_variant(filament, printer):
        variants = new_printers[printer]["variants"]
        return not filament["variants"] or not variants or bool(filament["variants"] & variants)

    result = {}
    for base_tree in base_trees:
        old_filaments, _ = flatten(base_tree)
        for (vendor, name), old in sorted(old_filaments.items()):
            # Only a selectable preset can be a user preset's parent.
            if not old["instantiation"]:
                continue
            # The loader resolves an inherits to the preset of that name, else to the one
            # listing it in renamed_from.
            candidates = by_name.get(name) or by_old_name.get(name, [])
            same_vendor = [r for v, r in candidates if v == vendor]
            # Edge: a preset that moved to the filament library (Panchroma PLA Stain @System) still
            # counts; one that now resolves to another vendor's preset of the same name (an old U1
            # "Generic PA" against BBL's "Generic PA") does not: its printers say nothing about the
            # preset the user's file was made on.
            library = [r for v, r in candidates if v == OFL]
            rec = (same_vendor or library or [None])[0]
            # An empty list means every printer, so nothing can have been lost.
            if rec is None or not rec["compatible_printers"]:
                continue
            lost = {renamed_printer.get(p, p) for p in old["compatible_printers"]} - rec["compatible_printers"]
            lost &= covering.get(rec["filament_id"], set()) & set(new_printers)
            lost = {p for p in lost if shares_variant(rec, p)}
            if lost:
                result.setdefault(name, set()).update(lost)
    return {name: sorted(printers) for name, printers in sorted(result.items())}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", action="append", default=[], metavar="DIR",
                        help="an older release's resources/profiles directory (repeatable)")
    parser.add_argument("--base-tag", action="append", default=[], metavar="TAG",
                        help="a git tag whose resources/profiles to compare with (repeatable)")
    parser.add_argument("--profiles", default=PROFILES_DIR, metavar="DIR", help="the current profiles tree")
    parser.add_argument("--output", default=OUTPUT, metavar="FILE")
    parser.add_argument("--check", action="store_true", help="fail if the map differs; write nothing")
    args = parser.parse_args(argv)

    tags = args.base_tag or ([] if args.base else DEFAULT_BASE_TAGS)
    bases = [DirTree(d) for d in args.base]
    # Releases often share one profiles tree: read each distinct tree once.
    seen = set()
    for tag in tags:
        tree_id = subprocess.run(["git", "-C", REPO_DIR, "rev-parse", tag + ":resources/profiles"],
                                 check=True, capture_output=True, text=True).stdout.strip()
        if tree_id not in seen:
            seen.add(tree_id)
            bases.append(GitTree(tag))
    presets = legacy_compatible_printers(bases, DirTree(args.profiles))

    if args.check:
        current = (_read_json(args.output) or {}).get("presets")
        if current != presets:
            print(f"{args.output} is out of date; rerun scripts/legacy_compatible_printers.py", file=sys.stderr)
            return 1
        print(f"{args.output} is up to date")
        return 0
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps({"presets": presets}, indent=4, sort_keys=True, ensure_ascii=False) + "\n")
    print(f"wrote {len(presets)} presets / {sum(map(len, presets.values()))} printers to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
