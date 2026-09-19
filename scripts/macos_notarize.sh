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
json_field() {
    python3 -c 'import json, sys
try:
    print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))
except Exception:
    print("")' "$result" "$1"
}
# Submit WITHOUT --wait, then poll. `submit --wait` is one long HTTP session that dies on any
# network hiccup and takes the whole verdict with it: on 2026-09-19 the 296 MB dmg was
# uploaded (submission 5f34f3a6) and the wait then failed with "The Internet connection
# appears to be offline" 53 minutes in, so the run failed although Apple was still working
# on a perfectly good submission. Polling `notarytool info` tolerates that: a failed poll is
# just retried. Apple's queue is also unpredictable (a 241 MB app sat unanswered for 45 min,
# submission e48d9c7c), so wait long: the job has a 6 h ceiling and the build before this
# step takes ~3.6 h, so ~2 h is the most that fits. NOTARY_TIMEOUT (minutes) overrides.
tries=0
until xcrun notarytool submit "$SUBMIT" "${auth[@]}" --output-format json > "$result" 2> "$result.err"; do
    tries=$((tries+1))
    cat "$result.err"
    [ $tries -ge 3 ] && { echo "::error::notarytool submit failed 3 times" >&2; exit 1; }
    echo "submit failed, retrying in 60 s ($tries/3)"; sleep 60
done
cat "$result"; echo
id=$(json_field id)
[ -n "$id" ] || { echo "::error::notarytool submit returned no submission id" >&2; exit 1; }
deadline=$(( $(date +%s) + ${NOTARY_TIMEOUT:-120} * 60 ))
status=$(json_field status)
while [ "$status" = "In Progress" ] || [ -z "$status" ]; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
        # Exit 2 = PENDING, not rejected. Apple keeps processing; once the submission is
        # Accepted the ticket is served online and the signed file passes Gatekeeper on any
        # machine with internet even though it was never stapled. The caller may therefore
        # still ship the signed file and check later (workflow "Notary status").
        echo "::warning::Notarization of $(basename "$SUBMIT") still '$status' after ${NOTARY_TIMEOUT:-120} min (submission $id). Not stapled. Check later with the 'Notary status' workflow; the ticket is served online once Apple accepts." >&2
        echo "$id" > "$work/notary_pending_id"
        exit 2
    fi
    sleep 60
    if xcrun notarytool info "$id" "${auth[@]}" --output-format json > "$result.poll" 2> "$result.err"; then
        mv -f "$result.poll" "$result"
        status=$(json_field status)
        echo "$(date -u +%H:%M) submission $id: ${status:-?}"
    else
        echo "$(date -u +%H:%M) poll failed (network?), retrying: $(head -c 200 "$result.err")"
    fi
done

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
