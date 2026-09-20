#!/usr/bin/env python3
"""Refresh the bundled HMS error-text snapshot under resources/hms from Bambu's server.

Why this exists
---------------
resources/hms/hms_<lang>_<model>.json is what the slicer reads when it cannot reach Bambu's
cloud - which is most of the time, because stealth mode blocks the fetch until the setup wizard
is finished, and LAN-only owners never unblock it. When that snapshot goes stale the printer's
own words for a newer code are simply not available offline, and the user is shown the bare code.

What it does - a MERGE, never an overwrite
------------------------------------------
The union is taken per (model, language, table, ecode):

  * an ecode present only in the shipped file is KEPT (the cloud's device_hms table is currently
    smaller than the shipped one - 2828 entries vs 4994 for 31B - so an overwrite would silently
    delete thousands of codes that still resolve today);
  * an ecode present only in the cloud is ADDED;
  * an ecode in both takes the cloud's `intro` only when the cloud's is non-empty. A non-empty
    shipped string is never replaced by an empty cloud one, and an empty shipped string is filled
    in when the cloud has learned the text.

`ver` is bumped to the cloud's when the cloud is newer, so the runtime's version comparison in
HMSQuery::download_hms_related still does the right thing afterwards.

Note that some codes carry an empty `intro` in every language AND on the server - 0C00010000020015
("Nozzle Camera is malfunctioning" in Bambu Studio's UI) is one. Those are not fixable by
refreshing; the slicer falls back to naming the code (HMSQuery::format_error).

How to rerun
------------
    python scripts/hms/refresh_hms_snapshot.py                # merge every shipped lang+model
    python scripts/hms/refresh_hms_snapshot.py --dry-run      # report what would change
    python scripts/hms/refresh_hms_snapshot.py --models 31B 094
    python scripts/hms/refresh_hms_snapshot.py --langs en de

The set of languages and models is taken from the files already in resources/hms, so adding a
language there is enough to have it refreshed from then on. Re-run it before a release; the diff
is reviewable per file and the script is idempotent.

The server rate-limits: requests are made one at a time with a short delay, and a 403 or a
timeout leaves that file untouched rather than half-written.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HOST = "e.bambulab.com"
HMS_DIR = Path(__file__).resolve().parents[2] / "resources" / "hms"
# hms_<lang>_<model>.json, but not hms_action_<model>.json (the action/image table, not text).
NAME_RE = re.compile(r"^hms_(?!action_)(?P<lang>[A-Za-z-]+)_(?P<model>[0-9A-Za-z]+)\.json$")
TABLES = ("device_hms", "device_error")


def shipped_files(hms_dir: Path):
    """Every (path, lang, model) triple already in the snapshot."""
    out = []
    for path in sorted(hms_dir.glob("hms_*.json")):
        m = NAME_RE.match(path.name)
        if m:
            out.append((path, m.group("lang"), m.group("model")))
    return out


def fetch(lang: str, model: str, timeout: int = 30):
    """The cloud's table for one language and model, or None if it could not be had."""
    url = f"https://{HOST}/query.php?lang={lang}&d={model}"
    # The server answers 403 to urllib's default User-Agent; any ordinary one is accepted.
    req = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": "curl/8.0 (EdgeSlicer hms snapshot refresh)"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except (urllib.error.URLError, urllib.error.HTTPError, ValueError, TimeoutError) as e:
        print(f"    ! {lang}/{model}: {e}", file=sys.stderr)
        return None


def entries(doc, table: str, lang: str):
    """The [{ecode, intro}, ...] list for one table and language, whatever the envelope."""
    data = doc.get("data", doc) if isinstance(doc, dict) else {}
    t = data.get(table)
    if not isinstance(t, dict):
        return []
    arr = t.get(lang)
    return arr if isinstance(arr, list) else []


def merge_list(shipped: list, cloud: list):
    """Union by ecode. Returns (merged, added, filled) - never drops a shipped entry."""
    by_code = {}
    order = []
    for it in shipped:
        if not isinstance(it, dict):
            continue
        code = (it.get("ecode") or "").upper()
        if not code:
            continue
        if code not in by_code:
            order.append(code)
        by_code[code] = dict(it)

    added = filled = 0
    for it in cloud:
        if not isinstance(it, dict):
            continue
        code = (it.get("ecode") or "").upper()
        if not code:
            continue
        cloud_intro = (it.get("intro") or "").strip()
        if code not in by_code:
            by_code[code] = dict(it)
            order.append(code)
            if cloud_intro:
                added += 1
            continue
        # Present in both: take the cloud's text only when it actually says something.
        have = (by_code[code].get("intro") or "").strip()
        if cloud_intro and cloud_intro != have:
            by_code[code]["intro"] = it["intro"]
            if not have:
                filled += 1
    return [by_code[c] for c in order], added, filled


def ver_of(doc):
    v = doc.get("ver") if isinstance(doc, dict) else None
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def refresh_one(path: Path, lang: str, model: str, dry_run: bool):
    try:
        shipped = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"    ! {path.name}: unreadable ({e})", file=sys.stderr)
        return False, 0, 0

    cloud = fetch(lang, model)
    if cloud is None:
        return False, 0, 0

    total_added = total_filled = 0
    data = shipped.setdefault("data", {}) if "data" in shipped or "device_hms" not in shipped else shipped
    for table in TABLES:
        merged, added, filled = merge_list(entries(shipped, table, lang), entries(cloud, table, lang))
        if not merged:
            continue
        total_added += added
        total_filled += filled
        data.setdefault(table, {})
        # Keep the per-table `ver` the files carry, if it is there.
        if isinstance(cloud.get("data", {}).get(table), dict):
            cv = cloud["data"][table].get("ver")
            if cv is not None:
                data[table]["ver"] = cv
        data[table][lang] = merged

    if ver_of(cloud) > ver_of(shipped):
        shipped["ver"] = cloud["ver"]

    if total_added or total_filled:
        if not dry_run:
            # The shipped files are compact, not pretty-printed; keep them that way so the diff
            # is about content and not whitespace.
            path.write_text(json.dumps(shipped, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        print(f"    {path.name}: +{total_added} new, {total_filled} filled"
              f"{' (dry run)' if dry_run else ''}")
        return True, total_added, total_filled
    return False, 0, 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="report what would change, write nothing")
    ap.add_argument("--models", nargs="*", help="only these models (default: every one in resources/hms)")
    ap.add_argument("--langs", nargs="*", help="only these languages (default: every one in resources/hms)")
    ap.add_argument("--delay", type=float, default=1.0, help="seconds between requests (default 1.0)")
    ap.add_argument("--hms-dir", type=Path, default=HMS_DIR)
    args = ap.parse_args()

    files = shipped_files(args.hms_dir)
    if not files:
        print(f"no hms_<lang>_<model>.json under {args.hms_dir}", file=sys.stderr)
        return 1
    if args.models:
        files = [f for f in files if f[2] in set(args.models)]
    if args.langs:
        files = [f for f in files if f[1] in set(args.langs)]

    print(f"{len(files)} table(s) under {args.hms_dir}")
    changed = added = filled = 0
    for i, (path, lang, model) in enumerate(files):
        ok, a, f = refresh_one(path, lang, model, args.dry_run)
        changed += 1 if ok else 0
        added += a
        filled += f
        if i + 1 < len(files):
            time.sleep(args.delay)

    print(f"\n{changed} file(s) changed, {added} code(s) added, {filled} empty description(s) filled"
          f"{' (dry run, nothing written)' if args.dry_run else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
