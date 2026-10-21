#!/bin/bash
# Sign a macOS .app bundle for Developer ID distribution with the hardened runtime, inside out.
#
#   scripts/macos_sign_app.sh <App.app> "<Developer ID Application: Name (TEAMID)>" <entitlements.plist>
#   ... [<keychain>]  optional 4th argument: look the identity up only in that keychain file
#                     (codesign --keychain), for runners whose search list cannot include it.
#
# Why not a single `codesign --deep`: --deep stamps the *main* entitlements onto every nested
# executable and only visits code in the locations it knows about, so a dylib or helper that
# lives somewhere else (Contents/Resources, a file without the executable bit) stays unsigned -
# and Apple's notary service then rejects the whole submission with "The binary is not signed
# with a valid Developer ID certificate" / "The signature does not include a secure timestamp".
# Signing every Mach-O file we can find, deepest path first, and sealing the bundle last covers
# all of them, whatever the build adds later (a network plugin, a bundled ffmpeg, ...).
#
# That includes Bambu's camera files in Contents/Resources/cameratools (libBambuSource.dylib,
# liblive555.dylib; CAMERA_BIN_DIR), which arrive signed by Bambu's team (T3UBR9Y3B2): --force
# replaces that signature with ours. Notarization is the reason - every Mach-O in the submission
# must carry a Developer ID signature with a secure timestamp, and re-signing guarantees both
# instead of depending on how Bambu signed a given build. Loading them needs nothing extra: the
# app's com.apple.security.cs.disable-library-validation entitlement already admits libraries of
# another team (or ours).
set -euo pipefail

APP=${1:?usage: macos_sign_app.sh <App.app> <identity> <entitlements.plist>}
IDENTITY=${2:?usage: macos_sign_app.sh <App.app> <identity> <entitlements.plist>}
ENTITLEMENTS=${3:?usage: macos_sign_app.sh <App.app> <identity> <entitlements.plist>}
KEYCHAIN=${4:-}

# The self-hosted Mac runner is a LaunchDaemon: codesign there searches the system-domain keychain
# list, which cannot include the per-run build keychain, so the workflow names it here.
# (`${arr[@]+...}` keeps an empty array safe under set -u in macOS's bash 3.2.)
keychain_args=()
if [ -n "$KEYCHAIN" ]; then
    [ -f "$KEYCHAIN" ] || { echo "error: keychain $KEYCHAIN not found" >&2; exit 1; }
    keychain_args=(--keychain "$KEYCHAIN")
fi

[ -d "$APP/Contents" ] || { echo "error: $APP is not an app bundle" >&2; exit 1; }
[ -f "$ENTITLEMENTS" ] || { echo "error: entitlements file $ENTITLEMENTS not found" >&2; exit 1; }

exe_name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist")
main_exe="$APP/Contents/MacOS/$exe_name"
[ -f "$main_exe" ] || { echo "error: main executable $main_exe is missing" >&2; exit 1; }

# Finder metadata and resource forks make codesign refuse the bundle outright
# ("resource fork, Finder information, or similar detritus not allowed").
xattr -cr "$APP"
find "$APP" -name '.DS_Store' -delete

# 1. Nested code first. `find -depth` is post-order, so the contents of a framework or a plugin
#    bundle are signed before the bundle itself. The main executable is left to step 2: the
#    bundle signature covers it and is the one that carries the entitlements.
nested=0
while IFS= read -r -d '' f; do
    [ "$f" = "$main_exe" ] && continue
    case "$(file -b "$f")" in
        *Mach-O*) ;;
        *) continue ;;
    esac
    echo "signing nested: ${f#"$APP"/}"
    codesign --force --options runtime --timestamp ${keychain_args[@]+"${keychain_args[@]}"} --sign "$IDENTITY" "$f"
    nested=$((nested + 1))
done < <(find "$APP/Contents" -depth -type f -print0)

# Frameworks are signed as bundles (their Versions/Current symlink layout needs it), after their
# contents above. None ship today, but a wx or OpenSSL framework would land here.
while IFS= read -r -d '' fw; do
    echo "signing framework: ${fw#"$APP"/}"
    codesign --force --options runtime --timestamp ${keychain_args[@]+"${keychain_args[@]}"} --sign "$IDENTITY" "$fw"
done < <(find "$APP/Contents" -depth -type d -name '*.framework' -print0)

# 2. The bundle itself: hardened runtime, secure timestamp and the app's entitlements.
echo "signing bundle: $APP ($nested nested Mach-O files signed)"
codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" ${keychain_args[@]+"${keychain_args[@]}"} --sign "$IDENTITY" "$APP"

# 3. Verify the way Gatekeeper will, and show what was applied.
codesign --verify --deep --strict --verbose=2 "$APP"
codesign --display --verbose=2 "$APP" 2>&1 | grep -E '^(Identifier|Authority|TeamIdentifier|Timestamp|CodeDirectory)' || true
codesign --display --entitlements - "$APP" 2>/dev/null || true
if codesign --display --entitlements - "$APP" 2>/dev/null | grep -q 'get-task-allow'; then
    echo "error: the entitlements carry com.apple.security.get-task-allow, which the notary service rejects" >&2
    exit 1
fi
echo "signed: $APP"
