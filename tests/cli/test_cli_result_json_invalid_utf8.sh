#!/usr/bin/env bash
# CLI smoke: a non-UTF-8 object name / path must still produce result.json.
#
# nlohmann dump() throws type_error.316 on invalid UTF-8. record_exit_reson()
# serialises result.json inside a catch-all, so a throw used to leave no file.
# Object names from STL paths on Linux can be arbitrary bytes and reach
# error_string via Print::validate() when --no-check keeps the raw message.
#
# This run stops at validate (object taller than the bed). No G-code is written.
#
# usage: sh tests/cli/test_cli_result_json_invalid_utf8.sh <path to EdgeSlicer> [python]
set -u
BIN="${1:-}"
PY="${2:-python3}"
[ -f "$BIN" ] || { echo "SKIP: binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/edge-cli-utf8-result.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ASCII STL box. 500 mm tall so U1 (256 mm) validate fails before process().
"$PY" -I - "$WORK/src.stl" <<'PYEOF'
import sys
out, sx, sy, sz = sys.argv[1], 20.0, 20.0, 500.0
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

# Linux path / object name: arbitrary bytes, here a lone 0xFF.
BAD_STL="$WORK/bad"$'\xff'".stl"
cp "$WORK/src.stl" "$BAD_STL"
OUT="$WORK/out"
mkdir -p "$OUT"

# --no-check keeps validate's err.string (includes the object name) as error_string.
set +e
"$BIN" --slice 0 --allow-newer-file --no-thumbnails --progress-json --no-check \
    --outputdir "$OUT" \
    --printer-preset "Snapmaker U1 (0.4 nozzle)" \
    --process-preset "0.20mm Standard @Snapmaker U1 (0.4 nozzle)" \
    --filament-presets "Snapmaker PLA Matte @U1" \
    "$BAD_STL"
rc=$?
set -e

RESULT="$OUT/result.json"
if [ ! -f "$RESULT" ]; then
    echo "FAIL: result.json missing after non-UTF-8 object name (cli exit $rc)"
    exit 1
fi
if [ -e "$OUT"/plate_*.gcode ]; then
    echo "FAIL: G-code was written; smoke must stop at validate"
    exit 1
fi

"$PY" -I - "$RESULT" <<'PYEOF'
import json, sys
path = sys.argv[1]
with open(path, "rb") as f:
    raw = f.read()
doc = json.loads(raw.decode("utf-8"))
err = doc.get("error_string") or ""
if "\ufffd" not in err:
    sys.stderr.write("FAIL: error_string has no U+FFFD replacement: %r\n" % err[:200])
    sys.exit(1)
if b"\xff" in raw:
    sys.stderr.write("FAIL: result.json still contains a raw 0xFF byte\n")
    sys.exit(1)
print("PASS: result.json written; error_string carries U+FFFD")
PYEOF
