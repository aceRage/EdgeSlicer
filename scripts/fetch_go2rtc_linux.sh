#!/usr/bin/env bash
# Fetch the pinned, official go2rtc Linux binary into a directory (the Linux packages bundle it the
# way the Windows package bundles go2rtc.exe: the phone hub's camera relay runs it as a child).
#
#   scripts/fetch_go2rtc_linux.sh <dest-dir> [amd64|arm64]
#
# Writes <dest-dir>/go2rtc (mode 0755) and <dest-dir>/go2rtc-LICENSE.txt. The architecture defaults
# to the machine's (uname -m). The download is checked against the sha256 below; a mismatch is an
# error and nothing is written.
#
# GO2RTC_LINUX_BIN_DIR=<dir> uses <dir>/go2rtc_linux_<arch> instead of downloading (offline builds,
# flatpak-builder with a pre-fetched source); the sha256 is still checked.
#
# Pinned release: https://github.com/AlexxIT/go2rtc/releases/tag/v1.9.14 (MIT licence, AlexxIT).
# Same version as resources/tools/go2rtc/go2rtc.exe, which is what the hub's go2rtc.yaml and
# allow_paths handling were written and tested against. To bump it: change the version and both
# hashes together (the release page lists the digest of each asset), then re-run the gates.
set -euo pipefail

GO2RTC_VERSION="1.9.14"
GO2RTC_SHA256_amd64="32d616af226bd731678ffde328b94cfb94e30339bfefc469cfb76323144615a6"
GO2RTC_SHA256_arm64="359fabade8a7a51e81a55fe6df6b0ef81764a5e1d63179577534eaaa71904b50"

DEST="${1:?usage: fetch_go2rtc_linux.sh <dest-dir> [amd64|arm64]}"
ARCH="${2:-}"
if [ -z "$ARCH" ]; then
    case "$(uname -m)" in
        x86_64|amd64)  ARCH=amd64 ;;
        aarch64|arm64) ARCH=arm64 ;;
        *) echo "go2rtc: no pinned binary for $(uname -m); not bundled" >&2; exit 3 ;;
    esac
fi
case "$ARCH" in
    amd64) WANT="$GO2RTC_SHA256_amd64" ;;
    arm64) WANT="$GO2RTC_SHA256_arm64" ;;
    *) echo "go2rtc: unsupported architecture '$ARCH' (amd64 or arm64)" >&2; exit 2 ;;
esac

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

NAME="go2rtc_linux_${ARCH}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if [ -n "${GO2RTC_LINUX_BIN_DIR:-}" ]; then
    cp "${GO2RTC_LINUX_BIN_DIR}/${NAME}" "$TMP/${NAME}"
else
    curl -fsSL --retry 3 -o "$TMP/${NAME}" "https://github.com/AlexxIT/go2rtc/releases/download/v${GO2RTC_VERSION}/${NAME}"
fi
GOT="$(sha256_of "$TMP/${NAME}")"
if [ "$GOT" != "$WANT" ]; then
    echo "go2rtc: sha256 mismatch for ${NAME} v${GO2RTC_VERSION}: got ${GOT}, want ${WANT}" >&2
    exit 1
fi

mkdir -p "$DEST"
install -m 0755 "$TMP/${NAME}" "$DEST/go2rtc"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cp "$HERE/../resources/tools/go2rtc/go2rtc-LICENSE.txt" "$DEST/go2rtc-LICENSE.txt"
echo "go2rtc v${GO2RTC_VERSION} (${ARCH}, sha256 ${GOT}) -> ${DEST}/go2rtc"
