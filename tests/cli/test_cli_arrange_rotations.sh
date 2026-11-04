#!/usr/bin/env bash
# End-to-end check of the CLI's arrange rotation policy (Orca #15033, #16339). Manual, not wired into ctest:
# it needs an installed EdgeSlicer.exe, like the gcode-gate skill.
#
# Arrange is translate-only unless --allow-rotations is passed, so a bar that fits the bed only
# diagonally is not placed and the plate ends up with nothing fully inside the print volume. The
# run must then say that rotations were off, on stderr as well as in result.json (written on Linux only).
# Passing the flag must place the same bar and slice it, and an object that fits axis-aligned must be
# unaffected by the default.
#
# usage: sh tests/cli/test_cli_arrange_rotations.sh <path to EdgeSlicer.exe> [python] [resources/profiles/BBL]
set -u
BIN="${1:-}"
PY="${2:-python}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROFILES="${3:-$HERE/../../resources/profiles/BBL}"
[ -f "$BIN" ] || { echo "SKIP: binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/edge-cli-arrange-rotations.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }

# box <file> <x> <y> <z>: an ASCII STL box, so the test needs no fixture.
box() {
    "$PY" -I - "$@" <<'PYEOF'
import sys
out, sx, sy, sz = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
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
}
# --load-settings with only a machine and process file leaves the CLI on its 200 mm default bed (usable edge about
# 193 mm). 230 mm is longer than that, but fits turned about 45 degrees: it fits only rotated.
box "$(w "$WORK/bar.stl")" 230 10 5
box "$(w "$WORK/cube.stl")" 10 10 10

slice() { # <tag> <input> [option...]
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    "$BIN" --datadir "$(w "$out/datadir")" \
        --load-settings "$(w "$PROFILES/machine/Bambu Lab X1 Carbon 0.4 nozzle.json");$(w "$PROFILES/process/0.20mm Standard @BBL X1C.json")" \
        --load-defaultfila "$@" --slice 0 --allow-newer-file --no-thumbnails --outputdir "$(w "$out")" "$(w "$input")" \
        < /dev/null > "$out/stdout" 2> "$out/stderr"
    echo "$?" > "$out/rc"
}
exit_code() { "$PY" -I -c "import sys; rc = int(open(sys.argv[1]).read()); print(rc - 256 if rc > 127 else rc)" "$WORK/$1/rc"; }
fail() { echo "FAIL: $*"; exit 1; }

slice default "$WORK/bar.stl"
# The shell sees a truncated process exit code on Windows, so -50 is read from result.json when it is written.
[ "$(exit_code default)" != "0" ] || fail "default: expected a non-zero exit, got 0"
grep -q "Arrange ran without rotations" "$WORK/default/stderr" || fail "default: stderr does not mention rotations"
if [ -f "$WORK/default/result.json" ]; then
    grep -q "Arrange ran without rotations" "$WORK/default/result.json" || fail "default: result.json does not mention rotations"
    grep -q '"return_code": -50' "$WORK/default/result.json" || fail "default: result.json return_code is not -50"
fi

slice rotations "$WORK/bar.stl" --allow-rotations
[ "$(exit_code rotations)" = "0" ] || fail "--allow-rotations: expected 0, got $(exit_code rotations)"
ls "$WORK"/rotations/*.gcode >/dev/null 2>&1 || fail "--allow-rotations: no g-code written"

slice cube "$WORK/cube.stl"
[ "$(exit_code cube)" = "0" ] || fail "cube: expected 0, got $(exit_code cube)"
ls "$WORK"/cube/*.gcode >/dev/null 2>&1 || fail "cube: no g-code written"
grep -q "Arrange ran without rotations" "$WORK/cube/stderr" && fail "cube: unexpected rotation message on a successful run"

echo "PASS: arrange rotation default, its failure message, and the opt-in flag"
