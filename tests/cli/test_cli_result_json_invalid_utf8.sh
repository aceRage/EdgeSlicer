#!/usr/bin/env bash
# CLI smoke: a path that is not valid UTF-8 must still produce result.json.
#
# nlohmann dump() throws type_error.316 on invalid UTF-8. record_exit_reson()
# serialises result.json inside a catch-all, so a throw used to leave no file.
# On Linux a directory name can be arbitrary bytes; the output directory reaches
# result.json as sliced_plates[].gcode, the path of the written G-code.
#
# Linux only: Windows (NTFS) and macOS (APFS) store names as Unicode, so they
# cannot hold the invalid byte this test needs. Manual, not wired into ctest:
# it needs an installed EdgeSlicer binary, like tests/cli/test_cli_arrange_rotations.sh.
#
# usage: sh tests/cli/test_cli_result_json_invalid_utf8.sh <path to EdgeSlicer> [python]
set -u
BIN="${1:-}"
PY="${2:-python3}"
[ -f "$BIN" ] || { echo "SKIP: binary not found: $BIN"; exit 77; }
[ "$(uname -s)" = "Linux" ] || { echo "SKIP: needs a Linux file system for a non-UTF-8 directory name"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/edge-cli-utf8-result.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# A 20 mm ASCII STL box: it slices on any printer, so the run reaches the success result.
"$PY" -I - "$WORK/box.stl" <<'PYEOF'
import sys
out, sx, sy, sz = sys.argv[1], 20.0, 20.0, 20.0
v = [(x, y, z) for z in (0, sz) for y in (0, sy) for x in (0, sx)]
with open(out, "w") as f:
    f.write("solid box\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid box\n")
PYEOF

# The output directory carries a lone 0xFF byte.
OUT="$WORK/out"$'\xff'
mkdir -p "$OUT" "$WORK/datadir"

set +e
"$BIN" --slice 0 --allow-newer-file --no-thumbnails \
    --outputdir "$OUT" --datadir "$WORK/datadir" \
    --printer-preset "Snapmaker U1 (0.4 nozzle)" \
    --process-preset "0.20mm Standard @Snapmaker U1 (0.4 nozzle)" \
    --filament-presets "Snapmaker PLA Matte @U1" \
    "$WORK/box.stl"
rc=$?
set -e

RESULT="$OUT/result.json"
if [ ! -f "$RESULT" ]; then
    echo "FAIL: result.json missing with a non-UTF-8 output directory (cli exit $rc)"
    exit 1
fi

"$PY" -I - "$RESULT" <<'PYEOF'
import json, sys
path = sys.argv[1]
with open(path, "rb") as f:
    raw = f.read()
if b"\xff" in raw:
    sys.stderr.write("FAIL: result.json still contains a raw 0xFF byte\n")
    sys.exit(1)
doc = json.loads(raw.decode("utf-8"))
if doc.get("return_code") != 0:
    sys.stderr.write("FAIL: slice did not succeed: %r\n" % doc.get("error_string"))
    sys.exit(1)
gcode = (doc.get("sliced_plates") or [{}])[0].get("gcode") or ""
if "�" not in gcode:
    sys.stderr.write("FAIL: sliced_plates[0].gcode has no U+FFFD replacement: %r\n" % gcode[:200])
    sys.exit(1)
print("PASS: result.json written; the G-code path carries U+FFFD")
PYEOF
