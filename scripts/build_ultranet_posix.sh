#!/usr/bin/env bash
# Build UltraNet - EdgeSlicer's private, clean-room Bambu network plug-in - for macOS or Linux and
# stage the binaries the packaging scripts bundle (ULTRANET_BIN_DIR, see build_release_macos.sh and
# src/dev-utils/platform/unix/build_linux_image.sh.in).
#
#   scripts/build_ultranet_posix.sh <ultranet_checkout> <out_dir>
#
# The UltraNet source is private and this repository is public, so the script is written for a
# public CI log:
#   * compiler output goes to a log file under ULTRANET_LOG_DIR and is never printed - on a failure
#     only the file:line locations of the errors are shown;
#     diagnostics are built without source excerpts (-DULTRANET_QUIET_DIAGNOSTICS=ON) as well;
#   * the build trees are deleted when it finishes (the checkout is the caller's to delete);
#   * <out_dir> receives binaries and the marker only, stripped of local symbols.
#
# It links the OpenSSL of the host's own deps prefix (static), so the plug-in carries the same
# OpenSSL the app ships and exports nothing but its plug-in API.
#
# Environment (all optional):
#   EDGESLICER_SRC        this checkout (default: the repository this script lives in)
#   ULTRANET_DEPS_PREFIX  Linux: the deps prefix (default <src>/deps/build/destdir/usr/local)
#   ULTRANET_MAC_ARCHS    macOS: architectures to build and lipo together (default "arm64 x86_64");
#                         each uses <src>/deps/build/<arch>/OrcaSlicer_dep/usr/local
#   OSX_DEPLOYMENT_TARGET macOS deployment target (default 10.15, like the app in CI)
#   EDGESLICER_VERSION    written into the marker (default: read from common_func.hpp)
#   ULTRANET_LOG_DIR      where the build logs go (default: a temp dir, removed on exit)
set -euo pipefail

UN_SRC=${1:?usage: build_ultranet_posix.sh <ultranet_checkout> <out_dir>}
OUT=${2:?usage: build_ultranet_posix.sh <ultranet_checkout> <out_dir>}
UN_SRC=$(cd "$UN_SRC" && pwd)
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=${EDGESLICER_SRC:-$HERE}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ultranet_build.XXXXXX")
LOG_DIR=${ULTRANET_LOG_DIR:-$WORK/logs}
mkdir -p "$LOG_DIR" "$OUT"
trap 'rm -rf "$WORK/b_"*' EXIT

say() { printf '[ultranet] %s\n' "$*"; }

[ -f "$UN_SRC/CMakeLists.txt" ] || { say "no UltraNet CMakeLists.txt in $UN_SRC"; exit 1; }
UN_SHA=$(git -C "$UN_SRC" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
VERSION=${EDGESLICER_VERSION:-$(grep -E '^#define[[:space:]]+Snapmaker_VERSION[[:space:]]+"' "$SRC/src/common_func/common_func.hpp" | cut -d '"' -f2)}
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

case "$(uname -s)" in
    Darwin) OS=mac;   EXT=dylib ;;
    Linux)  OS=linux; EXT=so ;;
    *) say "unsupported OS $(uname -s)"; exit 1 ;;
esac

# Only file:line of each error, never the source text or the message.
report_failure() {
    local log=$1 what=$2
    say "$what FAILED (full log kept out of the CI output: $log)"
    local n
    n=$(grep -cE '(^|[ :])(fatal )?error[: ]' "$log" || true)
    say "error lines: $n; locations:"
    grep -oE '[A-Za-z0-9_./-]+\.(cpp|hpp|h|c):[0-9]+' "$log" | sed -E 's#^.*/##' | sort -u | head -40 | sed 's/^/[ultranet]   /' || true
    grep -E 'CMake Error' "$log" | sed -E 's/ at .*$//' | head -5 | sed 's/^/[ultranet]   /' || true
}

# build_one <tag> <deps_prefix> [extra cmake args...] -> $WORK/b_<tag>
build_one() {
    local tag=$1 prefix=$2; shift 2
    local b="$WORK/b_$tag" log="$LOG_DIR/ultranet_$tag.log"
    [ -d "$prefix" ] || { say "deps prefix missing: $prefix"; return 1; }
    say "building $tag against $prefix"
    if ! cmake -S "$UN_SRC" -B "$b" ${GEN[@]+"${GEN[@]}"} \
            -DCMAKE_BUILD_TYPE=Release \
            -DEDGESLICER_SRC_DIR="$SRC" \
            -DCMAKE_PREFIX_PATH="$prefix" \
            -DOPENSSL_ROOT_DIR="$prefix" \
            -DOPENSSL_USE_STATIC_LIBS=ON \
            -DCMAKE_DISABLE_FIND_PACKAGE_Boost=ON \
            -DULTRANET_TESTS=ON \
            -DULTRANET_QUIET_DIAGNOSTICS=ON \
            "$@" > "$log" 2>&1; then
        report_failure "$log" "configure ($tag)"; return 1
    fi
    if ! cmake --build "$b" --config Release --target bambu_networking BambuSource loadtest >> "$log" 2>&1; then
        report_failure "$log" "build ($tag)"; return 1
    fi
    [ -f "$b/libbambu_networking.$EXT" ] && [ -f "$b/libBambuSource.$EXT" ] || { say "build ($tag) produced no modules"; return 1; }
}

# The module must load through dlopen, answer the version handshake, and export only its API.
check_module() {
    local tag=$1 runner=${2:-}
    local b="$WORK/b_$tag"
    if ! $runner "$b/loadtest" "$b/libbambu_networking.$EXT" > "$LOG_DIR/loadtest_$tag.log" 2>&1; then
        say "loadtest ($tag) FAILED:"; sed 's/^/[ultranet]   /' "$LOG_DIR/loadtest_$tag.log" | head -20
        return 1
    fi
    say "loadtest ($tag): $(grep -E 'get_version' "$LOG_DIR/loadtest_$tag.log" | tr -d '\r')"
    local leaked
    if [ "$OS" = mac ]; then
        leaked=$(nm -gU "$b/libbambu_networking.dylib" | grep -ciE ' _(SSL|EVP|MQTT|OPENSSL)' || true)
    else
        leaked=$(nm -D --defined-only "$b/libbambu_networking.so" | grep -ciE ' (SSL|EVP|MQTT|OPENSSL)' || true)
    fi
    if [ "$leaked" != 0 ]; then say "$leaked OpenSSL/paho symbols exported from the module ($tag)"; return 1; fi
}

rm -f "$OUT/libbambu_networking.$EXT" "$OUT/libBambuSource.$EXT" "$OUT/ultranet.txt"

if [ "$OS" = linux ]; then
    build_one linux "${ULTRANET_DEPS_PREFIX:-$SRC/deps/build/destdir/usr/local}"
    check_module linux
    cp "$WORK/b_linux/libbambu_networking.so" "$WORK/b_linux/libBambuSource.so" "$OUT/"
    strip --strip-unneeded "$OUT/libbambu_networking.so" "$OUT/libBambuSource.so"
else
    ARCHS=${ULTRANET_MAC_ARCHS:-arm64 x86_64}
    TARGET=${OSX_DEPLOYMENT_TARGET:-10.15}
    nets=(); srcs=()
    for a in $ARCHS; do
        build_one "mac_$a" "$SRC/deps/build/$a/OrcaSlicer_dep/usr/local" \
                  -DCMAKE_OSX_ARCHITECTURES="$a" -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET"
        if [ "$a" = "$(uname -m)" ]; then
            check_module "mac_$a"
        elif [ "$a" = x86_64 ] && arch -x86_64 /usr/bin/true 2>/dev/null; then
            check_module "mac_$a" "arch -x86_64"
        else
            say "loadtest (mac_$a): skipped, this machine cannot run $a"
        fi
        nets+=("$WORK/b_mac_$a/libbambu_networking.dylib")
        srcs+=("$WORK/b_mac_$a/libBambuSource.dylib")
    done
    lipo -create "${nets[@]}" -output "$OUT/libbambu_networking.dylib"
    lipo -create "${srcs[@]}" -output "$OUT/libBambuSource.dylib"
    strip -x "$OUT/libbambu_networking.dylib" "$OUT/libBambuSource.dylib"
    # Ad-hoc signature (arm64 needs one to load at all); a Developer ID build re-signs it.
    codesign --force --sign - "$OUT/libbambu_networking.dylib" "$OUT/libBambuSource.dylib"
    say "architectures: $(lipo -archs "$OUT/libbambu_networking.dylib")"
fi

# Same first line as the Windows ship pipeline's marker; the host only checks it is there.
{
    printf '%s\n' "UltraNet network plugin (EdgeSlicer). This marker tells the host the plugin in this folder is EdgeSlicer's own build, not the Bambu CDN download."
    printf 'version: ultranet@%s  built %s  for EdgeSlicer %s (%s)\n' "$UN_SHA" "$(date -u +%F)" "$VERSION" "$OS"
} > "$OUT/ultranet.txt"

for f in "$OUT"/*; do
    if command -v sha256sum >/dev/null 2>&1; then h=$(sha256sum "$f" | cut -d' ' -f1); else h=$(shasum -a 256 "$f" | cut -d' ' -f1); fi
    say "staged $(basename "$f")  $(wc -c < "$f" | tr -d ' ') bytes  sha256=$h"
done
say "ultranet@$UN_SHA staged in $OUT"
