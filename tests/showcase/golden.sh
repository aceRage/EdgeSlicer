#!/bin/sh
# Showcase golden-image harness (design: tests/design_showcase_realism.md, section 6).
#
# Renders the fixture scenes in tests/showcase/fixtures through `--showcase-render` with a golden
# install (by default the shared baseline of main, C:/Dev/EdgeSlicerTest/_baseline) and a candidate
# install, and compares each pair with compare.py. Goldens are made fresh from the golden install on
# every run: no reference pixels are kept in the repository, and Windows and Mac pixels are never
# compared with each other (run both installs on the same machine).
#
#   sh tests/showcase/golden.sh <candidate install dir> [options]
#     --golden <dir>     the golden install (default C:/Dev/EdgeSlicerTest/_baseline)
#     --out <dir>        where fixtures, renders, diffs and the report go (default ./showcase_golden)
#     --only <name>      one fixture (studio_white, dark_gradient, build_plate, desk_wood, transparent,
#                        toolpaths, video)
#     --size <px>        the stills' shorter side (default 540)
#     --keep-goldens     reuse goldens already in <out>/golden
#     --max-delta <n>    largest per-channel difference in 8-bit steps (default 2)
#     --mean-delta <x>   mean per-channel difference (default 0.25)
#
# Exit 0 when every fixture is within tolerance, 1 when one is not, 2 when a render failed.
# <out>/report.txt has one line per fixture, <out>/index.html shows them side by side.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
GOLDEN_DIR=C:/Dev/EdgeSlicerTest/_baseline
OUT=./showcase_golden
ONLY=
SIZE=540
KEEP=0
MAXD=2
MEAND=0.25
CAND=
while [ $# -gt 0 ]; do
    case "$1" in
    --golden) GOLDEN_DIR=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --only) ONLY=$2; shift 2 ;;
    --size) SIZE=$2; shift 2 ;;
    --keep-goldens) KEEP=1; shift ;;
    --max-delta) MAXD=$2; shift 2 ;;
    --mean-delta) MEAND=$2; shift 2 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) CAND=$1; shift ;;
    esac
done
if [ -z "$CAND" ]; then
    echo "usage: golden.sh <candidate install dir> [--golden dir] [--out dir] [--only name]" >&2
    exit 2
fi
exe_in() {
    for n in EdgeSlicer.exe EdgeSlicer edge-slicer; do
        [ -x "$1/$n" ] && { echo "$1/$n"; return 0; }
    done
    [ -x "$1/Contents/MacOS/EdgeSlicer" ] && { echo "$1/Contents/MacOS/EdgeSlicer"; return 0; }
    return 1
}
GEXE=$(exe_in "$GOLDEN_DIR") || { echo "no EdgeSlicer executable in $GOLDEN_DIR" >&2; exit 2; }
CEXE=$(exe_in "$CAND") || { echo "no EdgeSlicer executable in $CAND" >&2; exit 2; }
PY=${PYTHON:-python}

mkdir -p "$OUT/fixtures" "$OUT/golden" "$OUT/candidate" "$OUT/diff" "$OUT/datadir_golden" "$OUT/datadir_candidate"
OUT=$(cd "$OUT" && pwd)
MODEL="$REPO/resources/handy_models/3DBenchy.3mf"
REPORT="$OUT/report.txt"
: > "$REPORT"
echo "golden:    $GEXE ($(head -1 "$GOLDEN_DIR/COMMIT.txt" 2>/dev/null))" | tee -a "$REPORT"
echo "candidate: $CEXE" | tee -a "$REPORT"
echo "size $SIZE, tolerance max $MAXD / mean $MEAND (8-bit steps)" | tee -a "$REPORT"

STATUS=0
render() {   # exe datadir fixture out extra-args...
    exe=$1; dd=$2; fx=$3; out=$4; shift 4
    timeout 900 "$exe" --showcase-render "$fx" --out "$out" --size "$SIZE" --quality high \
        --datadir "$dd" --report "$out.report.json" "$@" > "$out.log" 2>&1
    grep -q '"ok": *true' "$out.report.json" 2>/dev/null
}

run_still() {   # name [--alpha-exact] [render args...]
    name=$1; shift
    alpha=
    premul=
    if [ "${1:-}" = "--alpha-exact" ]; then alpha=--alpha-exact; premul=--premultiplied; shift; fi
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && return 0
    fx="$OUT/fixtures/$name.3mf"
    "$PY" "$HERE/make_fixture.py" "$MODEL" "$HERE/fixtures/$name.json" "$fx" || { STATUS=2; return; }
    g="$OUT/golden/$name.png"
    c="$OUT/candidate/$name.png"
    if [ "$KEEP" = 0 ] || [ ! -f "$g" ]; then
        rm -f "$g"
        render "$GEXE" "$OUT/datadir_golden" "$fx" "$g" --still 0 "$@" || { echo "$name: golden render FAILED (see $g.log)" | tee -a "$REPORT"; STATUS=2; return; }
    fi
    rm -f "$c"
    render "$CEXE" "$OUT/datadir_candidate" "$fx" "$c" --still 0 "$@" || { echo "$name: candidate render FAILED (see $c.log)" | tee -a "$REPORT"; STATUS=2; return; }
    line=$("$PY" "$HERE/compare.py" "$g" "$c" --max-delta "$MAXD" --mean-delta "$MEAND" ${alpha:+"$alpha"} ${premul:+"$premul"} \
        --diff "$OUT/diff/$name.png" --json "$OUT/diff/$name.json")
    rc=$?
    # A toolpath scene whose plate did not slice falls back to the model: say so.
    note=
    if grep -q '"source": *"toolpaths"' "$HERE/fixtures/$name.json" && ! grep -q '"source": *"toolpaths"' "$c.report.json"; then
        note=" (NOT SLICED: the model was shown, see $c.report.json)"
    fi
    echo "$name: $line$note" | tee -a "$REPORT"
    [ $rc -ne 0 ] && [ $STATUS -eq 0 ] && STATUS=1
}

run_video() {   # a few frames of a short orbit, as a PNG sequence
    name=video
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && return 0
    fx="$OUT/fixtures/$name.3mf"
    "$PY" "$HERE/make_fixture.py" "$MODEL" "$HERE/fixtures/studio_white.json" "$fx" || { STATUS=2; return; }
    gd="$OUT/golden/$name"
    cd_="$OUT/candidate/$name"
    if [ "$KEEP" = 0 ] || [ ! -d "$gd" ]; then
        rm -rf "$gd"
        render "$GEXE" "$OUT/datadir_golden" "$fx" "$gd" --format png --path "$HERE/fixtures/orbit_short.path.json" ||
            { echo "$name: golden render FAILED (see $gd.log)" | tee -a "$REPORT"; STATUS=2; return; }
    fi
    rm -rf "$cd_"
    render "$CEXE" "$OUT/datadir_candidate" "$fx" "$cd_" --format png --path "$HERE/fixtures/orbit_short.path.json" ||
        { echo "$name: candidate render FAILED (see $cd_.log)" | tee -a "$REPORT"; STATUS=2; return; }
    for g in "$gd"/*.png; do
        f=$(basename "$g")
        line=$("$PY" "$HERE/compare.py" "$g" "$cd_/$f" --max-delta "$MAXD" --mean-delta "$MEAND" \
            --diff "$OUT/diff/${name}_$f" --json "$OUT/diff/${name}_$f.json")
        rc=$?
        echo "$name/$f: $line" | tee -a "$REPORT"
        [ $rc -ne 0 ] && [ $STATUS -eq 0 ] && STATUS=1
    done
}

run_still studio_white
run_still dark_gradient
run_still build_plate
run_still desk_wood
run_still transparent --alpha-exact
run_still toolpaths --slice
run_video

# Side by side for a person: golden | candidate | difference x32.
{
    echo "<!doctype html><meta charset=utf-8><title>Showcase goldens</title>"
    echo "<style>body{font:14px sans-serif;background:#888}img{width:32%;margin:0 .5%;background:repeating-conic-gradient(#ccc 0 25%,#eee 0 50%) 0 0/16px 16px}</style>"
    echo "<pre>"; cat "$REPORT"; echo "</pre>"
    for c in "$OUT"/candidate/*.png "$OUT"/candidate/video/*.png; do
        [ -f "$c" ] || continue
        rel=${c#"$OUT/candidate/"}
        d=$(echo "$rel" | tr / _)
        [ -f "$OUT/diff/$d" ] || d=$(basename "$rel")
        echo "<h3>$rel</h3><img src=\"golden/$rel\"><img src=\"candidate/$rel\"><img src=\"diff/$d\">"
    done
} > "$OUT/index.html"

echo "report: $REPORT  (side by side: $OUT/index.html)"
exit $STATUS
