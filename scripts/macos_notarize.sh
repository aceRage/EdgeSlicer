#!/bin/bash
# Submit a zip, dmg or pkg to Apple's notary service, wait for the verdict, print the notary log,
# fail loudly if it was not accepted, and staple the ticket.
#
#   scripts/macos_notarize.sh <file-to-submit> [staple-target]
#
# The staple target defaults to the submitted file (a dmg or pkg). A zip cannot be stapled: pass
# the .app the zip was made from so the ticket lands on the app.
#
# Credentials come from the environment. The App Store Connect API key is preferred:
#   APP_STORE_CONNECT_KEY_P8      the AuthKey_<KEY_ID>.p8 - base64 (whitespace and line breaks
#                                 are tolerated, so a PowerShell paste works) or the raw PEM text
#   APP_STORE_CONNECT_KEY_ID      e.g. ABC123DEFG
#   APP_STORE_CONNECT_ISSUER_ID   the UUID shown on App Store Connect > Users and Access > Integrations
# Fallback, an Apple ID with an app-specific password:
#   APPLE_DEV_ACCOUNT, TEAM_ID, APP_PWD
set -euo pipefail

SUBMIT=${1:?usage: macos_notarize.sh <file-to-submit> [staple-target]}
STAPLE=${2:-$SUBMIT}
[ -f "$SUBMIT" ] || { echo "error: $SUBMIT not found" >&2; exit 1; }
[ -e "$STAPLE" ] || { echo "error: staple target $STAPLE not found" >&2; exit 1; }

work=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
key=""
cleanup() { [ -n "$key" ] && rm -f "$key"; return 0; }
trap cleanup EXIT

auth=()
if [ -n "${APP_STORE_CONNECT_KEY_P8:-}" ]; then
    : "${APP_STORE_CONNECT_KEY_ID:?APP_STORE_CONNECT_KEY_ID is required with APP_STORE_CONNECT_KEY_P8}"
    : "${APP_STORE_CONNECT_ISSUER_ID:?APP_STORE_CONNECT_ISSUER_ID is required with APP_STORE_CONNECT_KEY_P8}"
    key="$work/AuthKey_${APP_STORE_CONNECT_KEY_ID}.p8"
    if [[ "$APP_STORE_CONNECT_KEY_P8" == *"BEGIN PRIVATE KEY"* ]]; then
        printf '%s\n' "$APP_STORE_CONNECT_KEY_P8" > "$key"
    else
        printf '%s' "$APP_STORE_CONNECT_KEY_P8" | tr -d ' \t\r\n' | base64 --decode > "$key"
    fi
    chmod 600 "$key"
    if ! grep -q 'BEGIN PRIVATE KEY' "$key"; then
        echo "error: APP_STORE_CONNECT_KEY_P8 does not decode to a .p8 private key (paste the file base64-encoded, or its PEM text)" >&2
        exit 1
    fi
    auth=(--key "$key" --key-id "$APP_STORE_CONNECT_KEY_ID" --issuer "$APP_STORE_CONNECT_ISSUER_ID")
    echo "notarizing with App Store Connect API key $APP_STORE_CONNECT_KEY_ID"
elif [ -n "${APPLE_DEV_ACCOUNT:-}" ]; then
    auth=(--apple-id "$APPLE_DEV_ACCOUNT" --team-id "${TEAM_ID:?TEAM_ID is required with APPLE_DEV_ACCOUNT}" --password "${APP_PWD:?APP_PWD is required with APPLE_DEV_ACCOUNT}")
    echo "notarizing with Apple ID $APPLE_DEV_ACCOUNT (team $TEAM_ID)"
else
    echo "error: no notarization credentials in the environment (APP_STORE_CONNECT_KEY_P8/KEY_ID/ISSUER_ID or APPLE_DEV_ACCOUNT/TEAM_ID/APP_PWD)" >&2
    exit 1
fi

result="$work/notary_$(basename "$SUBMIT").json"
echo "submitting $(basename "$SUBMIT") ($(du -h "$SUBMIT" | cut -f1)) ..."
# Do not trust the exit code alone: read the verdict back from the JSON.
# Apple's queue is unpredictable: a 241 MB universal app sat unanswered for 45 minutes on
# 2026-09-19 (submission e48d9c7c). Wait long by default; the job has a 6 h ceiling and the
# build before this step takes ~3.6 h, so 2 h is the most that fits. NOTARY_TIMEOUT overrides.
xcrun notarytool submit "$SUBMIT" "${auth[@]}" --wait --timeout "${NOTARY_TIMEOUT:-120m}" --output-format json > "$result" || true
cat "$result"
echo
json_field() {
    python3 -c 'import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))
except Exception:
    print("")' "$result" "$1"
}
id=$(json_field id)
status=$(json_field status)

if [ -n "$id" ]; then
    echo "notary log for submission $id:"
    xcrun notarytool log "$id" "${auth[@]}" || echo "(could not fetch the notary log)"
fi

if [ "$status" != "Accepted" ]; then
    echo "::error::Notarization of $(basename "$SUBMIT") was not accepted (status: ${status:-no response}). The notary log above lists each rejected file and why." >&2
    exit 1
fi

echo "stapling $STAPLE"
xcrun stapler staple "$STAPLE"
xcrun stapler validate "$STAPLE"
echo "notarized and stapled: $STAPLE"
