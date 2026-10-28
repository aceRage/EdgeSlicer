#!/bin/sh
# Multi-colour filaments (P3): Showcase stills of the P0 calibration shapes in dual red/yellow silk,
# for the owner to put next to the P0 photos (tests/p0_multicolour_cc1). Renders headless through
# `--showcase-render --slice` with a scratch data folder; nothing is compared automatically.
# tests/design_multicolour_filament_view.md 7.2 and 8.2.
#
#   sh tests/showcase/multicolour_stills.sh <install dir> <template.3mf> <out dir> [--size px] [--angle deg]
#                                           [--photos dir] [--golden dir]
#     <template.3mf>  any project 3MF with project settings (its printer and process are used)
#     --size          the stills' shorter side (default 1080)
#     --angle         slot 1's rotation, filament_colour_angle (default -45: front red, right yellow)
#     --photos        the P0 photo folder, linked from index.html (default tests/p0_multicolour_cc1)
#     --golden        also render with this install (e.g. the main baseline) for a before/after
#
# Writes <out>/<shape>_<view>.png (+ .log, .report.json) and <out>/index.html.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SIZE=1080
ANGLE=-45
PHOTOS=$REPO/tests/p0_multicolour_cc1
GOLDEN=
ARGS=
while [ $# -gt 0 ]; do
    case "$1" in
    --size) SIZE=$2; shift 2 ;;
    --angle) ANGLE=$2; shift 2 ;;
    --photos) PHOTOS=$2; shift 2 ;;
    --golden) GOLDEN=$2; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) ARGS="$ARGS $1"; shift ;;
    esac
done
set -- $ARGS
if [ $# -ne 3 ]; then
    echo "usage: multicolour_stills.sh <install dir> <template.3mf> <out dir> [--size px] [--angle deg]" >&2
    exit 2
fi
INSTALL=$1
TEMPLATE=$2
OUT=$3
exe_in() {
    for n in EdgeSlicer.exe EdgeSlicer edge-slicer; do
        [ -x "$1/$n" ] && { echo "$1/$n"; return 0; }
    done
    [ -x "$1/Contents/MacOS/EdgeSlicer" ] && { echo "$1/Contents/MacOS/EdgeSlicer"; return 0; }
    return 1
}
EXE=$(exe_in "$INSTALL") || { echo "no EdgeSlicer executable in $INSTALL" >&2; exit 2; }
PY=${PYTHON:-python}
mkdir -p "$OUT/fixtures" "$OUT/datadir"
OUT=$(cd "$OUT" && pwd)
"$PY" -I "$HERE/make_multicolour_fixtures.py" "$TEMPLATE" "$OUT/fixtures" --angle "$ANGLE" > "$OUT/fixtures.txt" || exit 2

STATUS=0
render() {   # exe datadir fixture out
    timeout 900 "$1" --showcase-render "$3" --out "$4" --size "$SIZE" --quality high --still 0 --slice \
        --datadir "$2" --report "$4.report.json" > "$4.log" 2>&1
    grep -q '"ok": *true' "$4.report.json" 2>/dev/null
}
for fx in "$OUT"/fixtures/*.3mf; do
    name=$(basename "$fx" .3mf)
    if render "$EXE" "$OUT/datadir" "$fx" "$OUT/$name.png"; then
        note=
        grep -q '"source": *"toolpaths"' "$OUT/$name.png.report.json" || note=" (NOT SLICED: the model was shown)"
        echo "$name: ok$note"
    else
        echo "$name: FAILED (see $OUT/$name.png.log)"; STATUS=1
    fi
    if [ -n "$GOLDEN" ]; then
        GEXE=$(exe_in "$GOLDEN") || { echo "no EdgeSlicer executable in $GOLDEN" >&2; exit 2; }
        mkdir -p "$OUT/before" "$OUT/datadir_before"
        render "$GEXE" "$OUT/datadir_before" "$fx" "$OUT/before/$name.png" || echo "$name: before render FAILED"
    fi
done

rel_photos=$("$PY" -I -c "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]).replace(os.sep, '/'))" "$PHOTOS" "$OUT" 2>/dev/null || echo "$PHOTOS")
{
    echo "<!doctype html><meta charset=utf-8><title>P3 multi-colour stills</title>"
    echo "<style>body{font:14px sans-serif;background:#eee;margin:16px}img{max-width:48%;margin:4px;vertical-align:top}h3{margin:18px 0 4px}</style>"
    echo "<p>Dual red/yellow silk, slot rotation $ANGLE deg. P0 photos: <a href=\"$rel_photos/\">$rel_photos</a>"
    echo "(07/08 plaques, 09-12 cube, 13-16 cylinder, 01-06 vase).</p>"
    for p in "$OUT"/*.png; do
        n=$(basename "$p")
        echo "<h3>$n</h3><img src=\"$n\">"
        [ -f "$OUT/before/$n" ] && echo "<img src=\"before/$n\" title=\"before (plain colour)\">"
    done
} > "$OUT/index.html"
echo "stills: $OUT (index.html)"
exit $STATUS
