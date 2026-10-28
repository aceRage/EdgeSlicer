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
#                        toolpaths, toolpaths_finishes, toolpaths_silk, silk_walls, silk_top,
#                        video)
#     --size <px>        the stills' shorter side (default 540)
#     --keep-goldens     reuse goldens already in <out>/golden
#     --max-delta <n>    largest per-channel difference in 8-bit steps (default 2)
#     --mean-delta <x>   mean per-channel difference (default 0.25)
#     --expect-change <names>   fixtures (comma separated) that change on purpose: reported and
#                        shown side by side, but they do not fail the run
#     --golden-env <NAME=VALUE>, --candidate-env <NAME=VALUE>
#                        an environment variable for one side's renders only, e.g. the same install
#                        on both sides with --golden-env EDGESLICER_SHOWCASE_NO_HDR=1
#
# Exit 0 when every fixture is within tolerance (or expected to change), 1 when one is not, 2 when
# a render failed. <out>/report.txt has one line per fixture, <out>/index.html shows them side by
# side.

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
EXPECT=
GENV=
CENV=
while [ $# -gt 0 ]; do
    case "$1" in
    --golden) GOLDEN_DIR=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --only) ONLY=$2; shift 2 ;;
    --size) SIZE=$2; shift 2 ;;
    --keep-goldens) KEEP=1; shift ;;
    --max-delta) MAXD=$2; shift 2 ;;
    --mean-delta) MEAND=$2; shift 2 ;;
    --expect-change) EXPECT=$2; shift 2 ;;
    --golden-env) GENV=$2; shift 2 ;;
    --candidate-env) CENV=$2; shift 2 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
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
# A U1 project's printer, filament and process: a fresh datadir's "Default Printer" does not slice.
SLICE_SETTINGS="$REPO/tests/data/support_corpus/onepart_group.3mf"
REPORT="$OUT/report.txt"
: > "$REPORT"
echo "golden:    $GEXE ($(head -1 "$GOLDEN_DIR/COMMIT.txt" 2>/dev/null))" | tee -a "$REPORT"
echo "candidate: $CEXE" | tee -a "$REPORT"
echo "size $SIZE, tolerance max $MAXD / mean $MEAND (8-bit steps)" | tee -a "$REPORT"
[ -n "$GENV" ] && echo "golden env:    $GENV" | tee -a "$REPORT"
[ -n "$CENV" ] && echo "candidate env: $CENV" | tee -a "$REPORT"
[ -n "$EXPECT" ] && echo "expected to change: $EXPECT" | tee -a "$REPORT"

# Whether fixture $1 is listed in --expect-change.
expected() {
    case ",$(echo "$EXPECT" | tr -d ' ')," in
    *",$1,"*) return 0 ;;
    esac
    return 1
}

# One fixture's verdict: report line $2 with compare.py's code $3, for fixture $1 (a video frame
# is <name>/<frame>).
verdict() {   # (v_ names: sh has no locals, and the callers' name/line/rc must survive)
    v_name=$1; v_line=$2; v_rc=$3
    if expected "${v_name%%/*}"; then
        if [ "$v_rc" -eq 1 ]; then
            v_line="$v_line (CHANGED ON PURPOSE: review it in index.html)"
            v_rc=0
        elif [ "$v_rc" -eq 0 ]; then
            v_line="$v_line (expected a change, none seen)"
        fi
    fi
    echo "$v_name: $v_line" | tee -a "$REPORT"
    [ "$v_rc" -ne 0 ] && [ "$STATUS" -eq 0 ] && STATUS=1
    return 0
}

STATUS=0
render() {   # env exe datadir fixture out extra-args...  (env: NAME=VALUE or empty)
    renv=$1; exe=$2; dd=$3; fx=$4; out=$5; shift 5
    if [ -n "$renv" ]; then
        timeout 900 env "$renv" "$exe" --showcase-render "$fx" --out "$out" --size "$SIZE" --quality high \
            --datadir "$dd" --report "$out.report.json" "$@" > "$out.log" 2>&1
    else
        timeout 900 "$exe" --showcase-render "$fx" --out "$out" --size "$SIZE" --quality high \
            --datadir "$dd" --report "$out.report.json" "$@" > "$out.log" 2>&1
    fi
    grep -q '"ok": *true' "$out.report.json" 2>/dev/null
}

run_still() {   # name [--model <handy model file>] [--alpha-exact] [render args...]
    name=$1; shift
    alpha=
    premul=
    model=$MODEL
    if [ "${1:-}" = "--model" ]; then model="$REPO/resources/handy_models/$2"; shift 2; fi
    if [ "${1:-}" = "--alpha-exact" ]; then alpha=--alpha-exact; premul=--premultiplied; shift; fi
    [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && return 0
    fx="$OUT/fixtures/$name.3mf"
    settings=
    [ "${1:-}" = "--slice" ] && settings=$SLICE_SETTINGS
    "$PY" "$HERE/make_fixture.py" "$model" "$HERE/fixtures/$name.json" "$fx" ${settings:+"$settings"} || { STATUS=2; return; }
    g="$OUT/golden/$name.png"
    c="$OUT/candidate/$name.png"
    if [ "$KEEP" = 0 ] || [ ! -f "$g" ]; then
        rm -f "$g"
        render "$GENV" "$GEXE" "$OUT/datadir_golden" "$fx" "$g" --still 0 "$@" || { echo "$name: golden render FAILED (see $g.log)" | tee -a "$REPORT"; STATUS=2; return; }
    fi
    rm -f "$c"
    render "$CENV" "$CEXE" "$OUT/datadir_candidate" "$fx" "$c" --still 0 "$@" || { echo "$name: candidate render FAILED (see $c.log)" | tee -a "$REPORT"; STATUS=2; return; }
    line=$("$PY" "$HERE/compare.py" "$g" "$c" --max-delta "$MAXD" --mean-delta "$MEAND" ${alpha:+"$alpha"} ${premul:+"$premul"} \
        --diff "$OUT/diff/$name.png" --json "$OUT/diff/$name.json")
    rc=$?
    # A toolpath scene whose plate did not slice falls back to the model: say so.
    note=
    if grep -q '"source": *"toolpaths"' "$HERE/fixtures/$name.json" && ! grep -q '"source": *"toolpaths"' "$c.report.json"; then
        note=" (NOT SLICED: the model was shown, see $c.report.json)"
    fi
    verdict "$name" "$line$note" "$rc"
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
        render "$GENV" "$GEXE" "$OUT/datadir_golden" "$fx" "$gd" --format png --path "$HERE/fixtures/orbit_short.path.json" ||
            { echo "$name: golden render FAILED (see $gd.log)" | tee -a "$REPORT"; STATUS=2; return; }
    fi
    rm -rf "$cd_"
    render "$CENV" "$CEXE" "$OUT/datadir_candidate" "$fx" "$cd_" --format png --path "$HERE/fixtures/orbit_short.path.json" ||
        { echo "$name: candidate render FAILED (see $cd_.log)" | tee -a "$REPORT"; STATUS=2; return; }
    for g in "$gd"/*.png; do
        f=$(basename "$g")
        line=$("$PY" "$HERE/compare.py" "$g" "$cd_/$f" --max-delta "$MAXD" --mean-delta "$MEAND" \
            --diff "$OUT/diff/${name}_$f" --json "$OUT/diff/${name}_$f.json")
        rc=$?
        verdict "$name/$f" "$line" "$rc"
    done
}

run_still studio_white
run_still dark_gradient
run_still build_plate
run_still desk_wood
run_still transparent --alpha-exact
run_still toolpaths --slice
run_still toolpaths_finishes --slice
run_still toolpaths_silk --slice
run_still silk_walls
run_still silk_top --model OrcaCube_v2.3mf
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
