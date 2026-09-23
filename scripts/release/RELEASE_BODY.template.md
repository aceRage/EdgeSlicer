<!--
  Release notes template for EdgeSlicer GitHub releases (gh release create --notes-file).
  Copy to docs/release-notes/<version>.md (one file per release, committed) and fill in;
  that file is the --notes-file for gh release create.

  The block between the two update-notice comments is what the in-app "New version of
  EdgeSlicer" dialog shows (as plain text: headings, bold and links are flattened, "- "
  bullets become dots). Keep it short: 3-8 lines, one line per highlight, under ~600
  characters. The comments are invisible on the GitHub page, so the block also reads as
  the opening summary of the release. Without the block the dialog falls back to the first
  12 non-empty lines of the body (max 800 characters), which is rarely what you want.
  The release orchestrator refuses a RELEASE_BODY.md without the block.
  Details: docs/update-server/README.md
-->
> Your existing EdgeSlicer / Snapmaker Orca data is copied, never moved, so the previous install keeps working.

# EdgeSlicer X.Y.Z.W

<!-- update-notice -->
**EdgeSlicer X.Y.Z.W** - one sentence on what this release is about.
- Highlight one, in plain words
- Highlight two
- Highlight three
- Fixes: the one or two fixes people were waiting for
<!-- /update-notice -->

## What's new

### Area

**Headline.** Details.

## Fixes

- ...

## Downloads

| Platform | File |
|---|---|
| Windows installer | `EdgeSlicer_Windows_Installer_VX.Y.Z.W.exe` |
| Windows portable | `EdgeSlicer_Windows_VX.Y.Z.W_portable.zip` |
| macOS (universal) | `EdgeSlicer_Mac_universal_VX.Y.Z.W.dmg` |
| Linux AppImage | `EdgeSlicer_Linux_Ubuntu2404_VX.Y.Z.W.AppImage` |

Checksums: `SHA256SUMS.txt`.
