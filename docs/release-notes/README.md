# Release notes

One file per release, named after the version: `2.4.0.0.md`, `2.4.1.0.md`, and so on. Each file is
the GitHub release body, passed as-is to `gh release create --notes-file docs/release-notes/<version>.md`.

## The update-notice block

EdgeSlicer's "New version of EdgeSlicer" dialog reads the latest GitHub release and shows only
the part of the body between these two comments:

```
<!-- update-notice -->
...
<!-- /update-notice -->
```

The comments are invisible on the GitHub page, so the block also works as the opening summary of
the release. Keep it short, because the dialog shows it as plain text: headings, bold and links
are flattened, and `- ` bullets become dots. Aim for one or two lines on what's new, then one line
per fix.

Without the block, the dialog falls back to the first 12 non-empty lines of the body (at most 800
characters). The release scripts refuse to publish a body that has no block.

Start a new release from `scripts/release/RELEASE_BODY.template.md`. The parser and dialog are
described in `docs/update-server/README.md`.
