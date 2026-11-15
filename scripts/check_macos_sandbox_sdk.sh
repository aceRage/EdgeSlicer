#!/usr/bin/env bash
# Compile-check the macOS Seatbelt probe against a stub of current MacOSX.sdk
# (sandbox_init deprecated, named-profile string constants removed). Runs on
# Linux CI; does not require Xcode. A real macOS link/run is still owed on a
# signed helper.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
STUB_DIR="${ROOT}/tests/slic3rutils/macos_sdk_stubs"
CHECK_CC="${ROOT}/tests/slic3rutils/compile_check_macos_seatbelt.cpp"
HEADER="${ROOT}/src/slic3r/plugin/ipc/PluginSandboxMacSeatbelt.hpp"
MAC_CC="${ROOT}/src/slic3r/plugin/ipc/PluginSandboxMac.cpp"

die() {
    echo "check_macos_sandbox_sdk: $*" >&2
    exit 1
}

can_compile_cstdint() {
    local cxx="$1"
    shift
    echo '#include <cstdint>
int main() { return 0; }' | "${cxx}" -fsyntax-only -std=c++17 "$@" -x c++ - >/dev/null 2>&1
}

[[ -f "${CHECK_CC}" && -f "${HEADER}" && -f "${MAC_CC}" && -f "${STUB_DIR}/sandbox.h" ]] ||
    die "missing Seatbelt probe sources or SDK stub"

if grep -nE 'kSBXProfile(NoWrite|NoInternet|NoNetwork|PureComputation)' \
    "${HEADER}" "${MAC_CC}" "${CHECK_CC}" "${STUB_DIR}/sandbox.h"; then
    die "kSBXProfile* must not be referenced; current MacOSX.sdk dropped those identifiers"
fi

CXX_BIN=""
declare -a CXX_EXTRA=()
GCC13_INSTALL="/usr/lib/gcc/x86_64-linux-gnu/13"
# Prefer a compiler that can actually see <cstdint>. Ubuntu clang++ 18 may
# select a GCC 14 install that has no libstdc++ headers.
for candidate in "${CXX:-}" clang++ clang++-18 clang++-17 g++ g++-13 c++; do
    [[ -n "${candidate}" ]] || continue
    command -v "${candidate}" >/dev/null 2>&1 || continue
    resolved="$(command -v "${candidate}")"
    if can_compile_cstdint "${resolved}"; then
        CXX_BIN="${resolved}"
        CXX_EXTRA=()
        break
    fi
    if [[ -d "${GCC13_INSTALL}" ]] && can_compile_cstdint "${resolved}" --gcc-install-dir="${GCC13_INSTALL}"; then
        CXX_BIN="${resolved}"
        CXX_EXTRA=(--gcc-install-dir="${GCC13_INSTALL}")
        break
    fi
done
[[ -n "${CXX_BIN}" ]] || die "no C++ compiler on PATH can parse <cstdint> for -fsyntax-only"

echo "check_macos_sandbox_sdk: using ${CXX_BIN} ${CXX_EXTRA[*]:-} with stub ${STUB_DIR}/sandbox.h"

"${CXX_BIN}" -fsyntax-only -std=c++17 \
    -Werror -Wdeprecated-declarations \
    "${CXX_EXTRA[@]}" \
    -I "${ROOT}/src" \
    -I "${STUB_DIR}" \
    "${CHECK_CC}"

# Also compile without a sandbox.h so the guarded fallback prototype is checked.
NO_STUB_DIR="$(mktemp -d)"
trap 'rm -rf "${NO_STUB_DIR}"' EXIT
"${CXX_BIN}" -fsyntax-only -std=c++17 \
    -Werror -Wdeprecated-declarations \
    "${CXX_EXTRA[@]}" \
    -I "${ROOT}/src" \
    -I "${NO_STUB_DIR}" \
    "${CHECK_CC}"

echo "check_macos_sandbox_sdk: ok (stub SDK + missing sandbox.h, both -Werror=deprecated-declarations)"
