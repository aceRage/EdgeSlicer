# Merge Runbook — EdgeSlicer

How we batch-merge PRs into `main`, and the guards that keep a bad merge from
silently eating someone else's work. Complements `docs/catch2-lockstep-smoke.md`
(test discipline) — this file is about *merge* discipline.

## Background: the failure this runbook exists to prevent

On 2026-09-21, while merging the #62–#72 PR batch, conflict resolution on the
#72 branch silently dropped **PR #71's entire changeset** (~700 lines across 11
files: a new GUI component, its CMake registration, tests, i18n entries, and a
doc). Nothing flagged it:

- The tree merged with **zero conflict markers** — the loss looked like a clean
  auto-merge.
- The **build stayed green**, because the dropped files' CMakeLists references
  were dropped consistently with them.
- The **unit tests stayed green**, because the dropped test files were also
  removed from their CMakeLists.
- The failure was only discovered when a user looked for the UI and it was gone.

Any single PR's file set can be reverted by a later merge without any tool
complaining. Compile + test green is **not** sufficient evidence that a merge
batch is sound.

## Mandatory guard: file-set reconciliation after every merge batch

After merging a batch of PRs into `main` — *before* the rebuild — reconcile the
file sets. For each merge commit in the batch, list the files it touched and
confirm every added/modified path is still present (or intentionally changed)
at `HEAD`:

```bash
# From the repo root. Run after the batch merges, before rebuilding.
BASE=<commit-before-batch>   # e.g. 3c9a36a7d2
git fetch origin --prune && git checkout origin/main

# 1. Union of all paths added by any merge in the batch:
git log --merges --first-parent --format='%H' $BASE..HEAD | while read m; do
    git diff-tree -r --diff-filter=A --name-only "$m^1" "$m"
done | sort -u > /tmp/added_paths.txt

# 2. Any of those paths missing at HEAD is a silent drop — investigate:
while read p; do
    git cat-file -e "HEAD:$p" 2>/dev/null || echo "MISSING: $p"
done < /tmp/added_paths.txt

# 3. Sanity: a merge whose diff vs first parent exceeds ~200 deletions
#    deserves a human look (merges should mostly add lines):
git log --merges --first-parent --format='%H' $BASE..HEAD | while read m; do
    dels=$(git diff --numstat "$m^1" "$m" | awk '{s+=$2} END {print s}')
    [ "$dels" -gt 200 ] && echo "LARGE DELETE SET ($dels): $m $(git log -1 --format=%s $m)"
done
```

Rule of thumb: if step 2 prints anything or step 3 fires, **stop** and diff the
suspect merge against both parents before rebuilding. The 2026-09-21 incident
would have been caught by step 2 in seconds (`FlowVariantEdit.cpp: MISSING`).

## Merge batch procedure (updated)

1. Review each PR (correctness, conflicts vs siblings, merge order — profile
   lockstep before profile consumers, infra first).
2. Merge in dependency order via the GitHub API. Draft PRs must be marked ready
   first; the API rejects merges of drafts and conflicted branches.
3. When the API reports conflicts, resolve **in a local temp branch**, merge
   `origin/main` into the PR branch, push, and re-check with
   `git merge-tree --write-tree origin/main <branch>` before merging via API.
   After resolving, eyeball `git status`/staged deletions — the 2026-09-21 drop
   happened during exactly this kind of resolution.
4. **Run the file-set reconciliation above.** Do not skip; it is the only guard
   that catches a dropped PR.
5. Rebuild (`start-build.ps1` detached flow), then run the affected Catch2
   suites plus `slic3rutils_tests`.
6. Deploy (copy `EdgeSlicer.exe`/`.dll`, mirror `resources/` with the
   `FLASHNETWORK9.DAT` / `go2rtc` runtime-downloaded files excluded from
   deletion), then hand over the smoke checklist.

## Other accumulated lessons

- **Semantic merge collisions survive textual merges.** The #62–#72 batch
  needed six code fixes after the first green-looking merge (Float→Floats
  migration vs older scalar code, transposed `)`/`}` in auto-merged lines, a
  template misuse surfaced only by compile). Always do a full rebuild of the
  combined tree, never assume per-PR green implies batch green.
- **`renamed_from` in profile JSON is the user-upgrade path.** When presets are
  renamed, every old name must map to exactly one new name or old projects
  degrade (and user presets with dangling `inherits` are silently skipped at
  startup). Keep coverage complete; see PR #73.
- **Per-filament gating surprises testers.** The Standard/High-flow selector
  only appears for filaments whose `filament_flow_support` includes both modes.
  When a UI element "should" be universal, either make it universal or document
  the gate where testers will see it (see `smoke-test-checklist.md`).
