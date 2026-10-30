#!/bin/sh
# Multi-colour filaments (P3/P4): Showcase stills of the P0 calibration shapes in dual red/yellow silk,
# for the owner to put next to the P0 photos (tests/p0_multicolour_cc1). Renders headless through
# `--showcase-render --slice` (toolpath mode) or, with --mesh, without slicing (mesh mode, P4), with a
# scratch data folder; nothing is compared automatically. tests/design_multicolour_filament_view.md
# 7.2, 7.3 and 8.2.
#
#   sh tests/showcase/multicolour_stills.sh <install dir> <template.3mf> <out dir> [--size px] [--angle deg]
#                                           [--photos dir] [--golden dir] [--mesh] [--compare dir] [--dark]
#                                           [--gradient] [--cycle mm]
#     <template.3mf>  a project 3MF with project settings (its printer and process are used), e.g. the
#                     U1 project tests/data/support_corpus/onepart_group.3mf, as golden.sh uses (an H2C
#                     project slices on the command line but not in the hidden render instance)
#     --size          the stills' shorter side (default 1080)
#     --angle         slot 1's rotation, filament_colour_angle (default -45: front red, right yellow)
#     --photos        the P0 photo folder, linked from index.html (default tests/p0_multicolour_cc1)
#     --golden        also render with this install (e.g. the main baseline) for a before/after
#     --mesh          P4: the model in mesh mode, not sliced (the scenes' source is "model")
#     --compare       stills of the same names to show beside these in index.html (e.g. P3's
#                     tests/p0_multicolour_cc1/p3_stills next to --mesh stills)
#     --dark          the dark studio backdrop (the stills are otherwise on Studio white)
#     --gradient      P6: slot 1 is a gradient filament (Snapmaker's Pastel Rainbow palette) instead of a
#                     dual-colour split; --cycle sets mm of filament per palette cycle (default 150, so
#                     these small shapes show several cycles; the real default is 3000)
#
# Writes <out>/<shape>_<view>.png (+ .log, .report.json) and <out>/index.html.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SIZE=1080
ANGLE=-45
PHOTOS=$REPO/tests/p0_multicolour_cc1
GOLDEN=
MESH=
COMPARE=
DARK=
GRADIENT=
CYCLE=150
ARGS=
while [ $# -gt 0 ]; do
    case "$1" in
    --size) SIZE=$2; shift 2 ;;
    --angle) ANGLE=$2; shift 2 ;;
    --photos) PHOTOS=$2; shift 2 ;;
    --golden) GOLDEN=$2; shift 2 ;;
    --mesh) MESH=1; shift ;;
    --compare) COMPARE=$2; shift 2 ;;
    --dark) DARK=--dark; shift ;;
    --gradient) GRADIENT=--gradient; shift ;;
    --cycle) CYCLE=$2; shift 2 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
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
SOURCE=toolpaths
SLICE=--slice
LABEL=P3
if [ -n "$MESH" ]; then
    SOURCE=model
    SLICE=
    LABEL=P4
fi
[ -n "$GRADIENT" ] && LABEL=P6
"$PY" -I "$HERE/make_multicolour_fixtures.py" "$TEMPLATE" "$OUT/fixtures" --angle "$ANGLE" --source "$SOURCE" --cycle "$CYCLE" $GRADIENT $DARK > "$OUT/fixtures.txt" || exit 2

STATUS=0
render() {   # exe datadir fixture out
    timeout 900 "$1" --showcase-render "$3" --out "$4" --size "$SIZE" --quality high --still 0 $SLICE \
        --datadir "$2" --report "$4.report.json" > "$4.log" 2>&1
    grep -q '"ok": *true' "$4.report.json" 2>/dev/null
}
for fx in "$OUT"/fixtures/*.3mf; do
    name=$(basename "$fx" .3mf)
    if render "$EXE" "$OUT/datadir" "$fx" "$OUT/$name.png"; then
        note=
        if [ -z "$MESH" ]; then
            grep -q '"source": *"toolpaths"' "$OUT/$name.png.report.json" || note=" (NOT SLICED: the model was shown)"
        else
            grep -q '"source": *"toolpaths"' "$OUT/$name.png.report.json" && note=" (SLICED: toolpaths were shown, not the mesh)"
        fi
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

relpath() { "$PY" -I -c "import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]).replace(os.sep, '/'))" "$1" "$2" 2>/dev/null || echo "$1"; }
rel_photos=$(relpath "$PHOTOS" "$OUT")
rel_compare=
[ -n "$COMPARE" ] && rel_compare=$(relpath "$COMPARE" "$OUT")
{
    echo "<!doctype html><meta charset=utf-8><title>$LABEL multi-colour stills</title>"
    echo "<style>body{font:14px sans-serif;background:#eee;margin:16px}img{max-width:48%;margin:4px;vertical-align:top}h3{margin:18px 0 4px}</style>"
    if [ -n "$GRADIENT" ]; then echo "<p>Gradient (Pastel Rainbow palette), $CYCLE mm of filament per cycle."; else echo "<p>Dual red/yellow silk, slot rotation $ANGLE deg."; fi
    echo "P0 photos: <a href=\"$rel_photos/\">$rel_photos</a>"
    echo "(07/08 plaques, 09-12 cube, 13-16 cylinder, 01-06 vase).</p>"
    for p in "$OUT"/*.png; do
        n=$(basename "$p")
        echo "<h3>$n</h3><img src=\"$n\" title=\"$LABEL\">"
        [ -n "$COMPARE" ] && [ -f "$COMPARE/$n" ] && echo "<img src=\"$rel_compare/$n\" title=\"compared\">"
        [ -f "$OUT/before/$n" ] && echo "<img src=\"before/$n\" title=\"before (plain colour)\">"
    done
} > "$OUT/index.html"
echo "stills: $OUT (index.html)"
exit $STATUS
