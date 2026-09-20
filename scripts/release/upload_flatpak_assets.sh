#!/usr/bin/env bash
# Attach the two Flatpak bundles from a build_all.yml run to an EdgeSlicer release.
#
# The Flatpak jobs are slow (60-90 min) and independent of the mac/Linux/Windows builds,
# so they normally finish well after the release assets have already gone up. This script
# is the second pass: it picks the bundles out of a finished run, uploads them with
# --clobber, and appends their sha256 lines to the release's SHA256SUMS.txt instead of
# rewriting it (the first pass owns the other entries).
#
# Usage:
#   scripts/release/upload_flatpak_assets.sh <run-id> <version> [repo] [tag]
#
#   run-id   build_all.yml run that produced the Flatpak artifacts (e.g. 35347096397)
#   version  release version without the leading V (e.g. 2.3.8.5)
#   repo     default aceRage/EdgeSlicer
#   tag      default v<version>-edge
#
# Example:
#   scripts/release/upload_flatpak_assets.sh 35400000000 2.3.8.5
#
# Safe to re-run: --clobber replaces the assets, and SHA256SUMS.txt lines for the same
# file names are replaced rather than duplicated. If only one arch went green, it uploads
# that one and reports the other as missing without failing.
set -uo pipefail

RUN_ID="${1:?usage: upload_flatpak_assets.sh <run-id> <version> [repo] [tag]}"
VER="${2:?usage: upload_flatpak_assets.sh <run-id> <version> [repo] [tag]}"
REPO="${3:-aceRage/EdgeSlicer}"
TAG="${4:-v${VER}-edge}"

say() { echo "$(date +%H:%M) $*"; }

command -v gh >/dev/null || { say "UFA_ABORT gh not on PATH"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say "UFA_RUN $RUN_ID repo=$REPO tag=$TAG ver=$VER"

status=$(gh run view "$RUN_ID" -R "$REPO" --json status -q .status 2>/dev/null) || {
  say "UFA_ABORT cannot read run $RUN_ID"; exit 1; }
[ "$status" = "completed" ] || { say "UFA_ABORT run $RUN_ID is '$status', not completed"; exit 1; }

# Per-arch job results, so a half-green run is visible in the log.
gh run view "$RUN_ID" -R "$REPO" --json jobs \
  -q '.jobs[] | select(.name | test("Flatpak")) | .name + ": " + (.conclusion // "?")' || true

DL="$WORK/artifacts"; mkdir -p "$DL"
# Artifact names match the workflow: EdgeSlicer_Linux_flatpak_V<ver>_<arch>.flatpak
gh run download "$RUN_ID" -R "$REPO" -D "$DL" \
  -p "EdgeSlicer_Linux_flatpak_V${VER}_x86_64.flatpak" \
  -p "EdgeSlicer_Linux_flatpak_V${VER}_aarch64.flatpak" 2>&1 | tail -3

STAGE="$WORK/stage"; mkdir -p "$STAGE"
uploads=()
for arch in x86_64 aarch64; do
  name="EdgeSlicer_Linux_flatpak_V${VER}_${arch}.flatpak"
  # The artifact is a directory containing the bundle; the bundle inside may be named
  # slightly differently, so take the first .flatpak under that artifact's folder.
  found=$(find "$DL" -path "*${arch}*" -name '*.flatpak' -type f 2>/dev/null | head -1)
  if [ -z "$found" ]; then
    say "UFA_MISSING $arch (no bundle downloaded; job likely failed)"
    continue
  fi
  cp -f "$found" "$STAGE/$name"
  uploads+=("$STAGE/$name")
  say "UFA_STAGED $name ($(du -h "$STAGE/$name" | cut -f1))"
done

[ "${#uploads[@]}" -gt 0 ] || { say "UFA_ABORT no Flatpak bundles found in run $RUN_ID"; exit 1; }

gh release upload "$TAG" -R "$REPO" --clobber "${uploads[@]}" || {
  say "UFA_ABORT bundle upload failed"; exit 1; }
say "UFA_UPLOADED ${#uploads[@]} bundle(s)"

# Merge the new sha256 lines into the release's existing SHA256SUMS.txt.
SUMS="$WORK/SHA256SUMS.txt"
if gh release download "$TAG" -R "$REPO" -p SHA256SUMS.txt -D "$WORK" 2>/dev/null && [ -f "$SUMS" ]; then
  say "UFA_SUMS existing $(wc -l < "$SUMS") lines"
else
  : > "$SUMS"
  say "UFA_SUMS none on the release yet, starting fresh"
fi

( cd "$STAGE" && sha256sum -b "${uploads[@]##*/}" ) > "$WORK/new.txt" || {
  say "UFA_ABORT sha256sum failed"; exit 1; }

# Drop any prior lines for these exact file names, then append the fresh ones.
while read -r _ fname; do
  fname="${fname#\*}"
  grep -v -F -- " *${fname}" "$SUMS" > "$SUMS.tmp" 2>/dev/null || : > "$SUMS.tmp"
  mv "$SUMS.tmp" "$SUMS"
done < "$WORK/new.txt"
cat "$WORK/new.txt" >> "$SUMS"

gh release upload "$TAG" -R "$REPO" --clobber "$SUMS" || {
  say "UFA_ABORT SHA256SUMS.txt upload failed"; exit 1; }
say "UFA_SUMS_UPLOADED $(wc -l < "$SUMS") lines total"

gh release view "$TAG" -R "$REPO" --json assets -q '.assets[].name' | sed 's/^/  /'
say "UFA_DONE"
