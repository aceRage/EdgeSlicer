#!/usr/bin/env bash
# Fetch the pinned LGPL ffmpeg for Linux into a directory (the Linux packages bundle it beside go2rtc, the
# way the Windows package bundles its LGPL ffmpeg.exe: go2rtc re-encodes camera streams for the phone's
# Quality steps by running it).
#
#   scripts/fetch_ffmpeg_linux.sh <dest-dir> [amd64|arm64]
#
# Writes <dest-dir>/ffmpeg (0755), <dest-dir>/FFMPEG-LICENSE.txt (the build's LGPL-3.0 text) and
# <dest-dir>/FFMPEG-NOTICE-LINUX.txt (what it is, where the source is). The architecture defaults to the
# machine's. The download is checked against the sha256 below; a mismatch is an error, nothing is written.
#
# FFMPEG_LINUX_ARCHIVE=<file> uses an already downloaded copy of the archive instead (offline builds,
# flatpak-builder); the sha256 is still checked.
#
# The build: BtbN/FFmpeg-Builds, static, `--enable-version3` without `--enable-gpl` (LGPL-3.0), with
# --disable-libx264 --disable-libx265; its software H.264 encoder is libopenh264 (Cisco, BSD-2-Clause),
# which is what RemoteHub.cpp's ffmpeg_h264_template() asks for. Same family as the Windows ffmpeg.exe
# (docs/superpowers/specs/2026-09-12-bundled-ffmpeg.md); this one is the FFmpeg 8.1 release branch.
#
# PINNING AND THE MIRROR. BtbN deletes its dated autobuild tags after a couple of weeks (the Windows pin,
# autobuild-2026-09-12-13-12, was already gone on 2026-10-03), so the BtbN URL below will stop working. The
# archives are therefore mirrored, byte-identical, in the release ${MIRROR_TAG} of aceRage/edgeslicer-deps and
# fetched from there first; BtbN is only the fallback. To move to a newer build: pick a current tag at
# https://github.com/BtbN/FFmpeg-Builds/releases, take the `ffmpeg-n8.1.*-linux64-lgpl-8.1.tar.xz` and
# `...linuxarm64-lgpl-8.1.tar.xz` assets and their sha256, upload them (with the FFmpeg source at that commit and the
# BtbN recipe at the tag's commit) to a new release of edgeslicer-deps, then update TAG, MIRROR_TAG, ASSET, both
# hashes and the commit here, in the Flatpak manifest, and in FFMPEG-NOTICE-LINUX.txt's template.
set -euo pipefail

TAG="autobuild-2026-10-01-13-06"
# The same files, kept for good in https://github.com/aceRage/edgeslicer-deps (release below), byte-identical
# to BtbN's, with the FFmpeg source and the BtbN recipe they were built from.
MIRROR_TAG="ffmpeg-n8.1.3-btbn-2026-10-01"
FFMPEG_DESC="n8.1.3-14-g330caae0c1"
FFMPEG_COMMIT="330caae0c1"
ASSET_amd64="ffmpeg-${FFMPEG_DESC}-linux64-lgpl-8.1.tar.xz"
ASSET_arm64="ffmpeg-${FFMPEG_DESC}-linuxarm64-lgpl-8.1.tar.xz"
SHA256_amd64="3c2c4d6066432b2eab54be830d3ce1f5b68a3f34ecaa6d9f2d0230a49e778560"
SHA256_arm64="d407cc9405caf75d67b3a10c9793f1db38d55705d09ac47d279d36d2823cb73d"

DEST="${1:?usage: fetch_ffmpeg_linux.sh <dest-dir> [amd64|arm64]}"
ARCH="${2:-}"
if [ -z "$ARCH" ]; then
    case "$(uname -m)" in
        x86_64|amd64)  ARCH=amd64 ;;
        aarch64|arm64) ARCH=arm64 ;;
        *) echo "ffmpeg: no pinned build for $(uname -m); not bundled" >&2; exit 3 ;;
    esac
fi
case "$ARCH" in
    amd64) ASSET="$ASSET_amd64"; WANT="$SHA256_amd64" ;;
    arm64) ASSET="$ASSET_arm64"; WANT="$SHA256_arm64" ;;
    *) echo "ffmpeg: unsupported architecture '$ARCH' (amd64 or arm64)" >&2; exit 2 ;;
esac

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if [ -n "${FFMPEG_LINUX_ARCHIVE:-}" ]; then
    cp "$FFMPEG_LINUX_ARCHIVE" "$TMP/$ASSET"
else
    # The durable mirror first, then BtbN itself; the sha256 below is checked either way.
    got=0
    for url in "https://github.com/aceRage/edgeslicer-deps/releases/download/${MIRROR_TAG}/${ASSET}"                "https://github.com/BtbN/FFmpeg-Builds/releases/download/${TAG}/${ASSET}"; do
        if curl -fSL --retry 3 -o "$TMP/$ASSET" "$url"; then got=1; break; fi
        echo "ffmpeg: could not download $url" >&2
    done
    if [ "$got" != 1 ]; then
        echo "ffmpeg: download failed from the mirror and from BtbN; see the PINNING note in this script" >&2
        exit 4
    fi
fi
GOT="$(sha256_of "$TMP/$ASSET")"
if [ "$GOT" != "$WANT" ]; then
    echo "ffmpeg: sha256 mismatch for ${ASSET}: got ${GOT}, want ${WANT}" >&2
    exit 1
fi

# Only the ffmpeg binary and the licence: ffplay/ffprobe, the docs and the presets are not shipped.
mkdir -p "$TMP/x"
tar -xJf "$TMP/$ASSET" -C "$TMP/x" --strip-components=1 --wildcards '*/bin/ffmpeg' '*/LICENSE.txt'
mkdir -p "$DEST"
install -m 0755 "$TMP/x/bin/ffmpeg" "$DEST/ffmpeg"
cp "$TMP/x/LICENSE.txt" "$DEST/FFMPEG-LICENSE.txt"

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
sed -e "s|@TAG@|${TAG}|g" -e "s|@ASSET@|${ASSET}|g" -e "s|@SHA256@|${GOT}|g" \
    -e "s|@DESC@|${FFMPEG_DESC}|g" -e "s|@COMMIT@|${FFMPEG_COMMIT}|g" -e "s|@ARCH@|${ARCH}|g" \
    "$HERE/ffmpeg_linux_notice.txt.in" > "$DEST/FFMPEG-NOTICE-LINUX.txt"
echo "ffmpeg ${FFMPEG_DESC} LGPL (${ARCH}, archive sha256 ${GOT}) -> ${DEST}/ffmpeg"
