# Ultra: `git apply` wrapper for deps PATCH_COMMAND steps that FAILS when a patch is
# silently skipped. Run from the dependency's source dir (ExternalProject runs
# PATCH_COMMAND there) as
#
#   ${CMAKE_COMMAND} -DGIT=<git> -DP1=<a.patch> [-DP2=<b.patch> ... -DP8=...]
#                    [-DLOOSE_WS=ON]
#                    -P deps/apply_patch.cmake
#
# One -DPn= per patch, applied in order. LOOSE_WS adds the
# `--ignore-space-change --whitespace=fix` pair the recipes historically passed.
# Separate -DPn variables rather than a single list: a ";"-separated -D value has to
# survive ExternalProject's own command splitting, and that is more trouble than eight
# slots are worth.
#
# Why this exists
# ---------------
# `git apply` run inside a git work tree resolves a GIT-FORMAT patch ("diff --git a/x
# b/x") against the REPO ROOT, not the current directory. When the deps build tree lives
# inside the checkout (hosted CI, every developer machine) the paths never exist, git
# prints "Skipped patch 'src/foo.cpp'." for every file - and EXITS 0. The build then
# compiles the unpatched tree and fails later, somewhere unrelated, or (worse) succeeds
# with the bug the patch was meant to fix still in it. That cost a CI cycle for
# QuadriFlow on 2026-09-18 (run 35365103460). So this wrapper parses git's own output and
# turns "Skipped patch" (or no "Applied patch" at all) into a hard error.
#
# --directory: worked out HERE, at patch time - it follows the patch FORMAT and where
# the cwd actually is, not the dep:
#
#   GIT-FORMAT ("diff --git a/x b/x")  :  OCCT, OpenCV
#       Inside a work tree git resolves these against the repo root, so they need
#       --directory=<cwd relative to the repo root>, which is exactly what
#       `git rev-parse --show-prefix` prints from the cwd. Outside a work tree (or at its
#       top) git resolves them against the cwd and they need no flag at all - passing one
#       there would point git at a path that does not exist.
#       Measured: OCCT inside a work tree without the flag => "Skipped patch
#       'CMakeLists.txt'", 0 hunks applied, rc 0. That is the original bug.
#
#   TRADITIONAL ("--- a/x", no diff --git)  ->  NEVER --directory : CGAL, GMP, OpenVDB
#       git resolves the -p1-stripped path against the CWD, which ExternalProject has
#       already set to the dep source dir - so it is already correct, in a work tree or
#       not. Adding --directory prepends the build path a second time: git then dies with
#       "unable to find filename in patch at line 1" (GMP, OpenVDB) or builds a doubled
#       path (CGAL). Measured on CI: run 35374725461 (GMP) and run 35376108670 (OpenVDB).
#
# Why not decide this in deps/CMakeLists.txt at configure time (the old IN_GIT_REPO /
# BINARY_DIR_REL pair, passed in as -DDIRECTORY)? Because that asked git about the
# CONFIGURE cwd and derived the path from CMake's view of the build dir, while git apply
# sees the PATCH cwd's physical path. They differ whenever the build tree is not plainly
# inside the checkout: the self-hosted Mac runner links deps/build to a persistent tree
# outside the workspace, so git found no repo, no DIRECTORY was passed, and the old
# "git-format patch but no DIRECTORY" guard here failed OCCT's patch step (run
# 38950771996) even though git would have applied it correctly against the cwd. Asking
# git from the cwd the patch is applied in gives the answer git apply will act on.
#
# Each case was verified BOTH ways against the pinned upstream tarball/zip by counting
# the marker the patch is meant to remove, so a patch that silently does nothing cannot
# pass as success. Do not "harmonise" these flags without re-running that check.

if (NOT GIT)
    message(FATAL_ERROR "apply_patch: GIT not set")
endif ()

set(_patches "")
foreach (_n RANGE 1 8)
    set(_val "${P${_n}}")
    if (NOT _val STREQUAL "")
        list(APPEND _patches "${_val}")
    endif ()
endforeach ()
if (NOT _patches)
    message(FATAL_ERROR "apply_patch: no patches given (expected -DP1=...)")
endif ()

if (DEFINED DIRECTORY)
    message(FATAL_ERROR
        "apply_patch: DIRECTORY is no longer a parameter. --directory for git-format\n"
        "patches is worked out here from `git rev-parse --show-prefix` in the dep source\n"
        "dir (see the top of deps/apply_patch.cmake). Drop -DDIRECTORY from the recipe.")
endif ()

# Where git apply will resolve a git-format patch from: ask git itself, from the cwd the
# patch is applied in (the dep source dir), so symlinked or out-of-tree build dirs get
# the same answer git apply acts on. Not in a work tree (rc != 0) => no prefix.
set(_git_prefix "")
set(_in_work_tree FALSE)
execute_process(
    COMMAND "${GIT}" rev-parse --is-inside-work-tree
    RESULT_VARIABLE _rp_rc
    OUTPUT_VARIABLE _rp_out
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (_rp_rc EQUAL 0 AND _rp_out STREQUAL "true")
    set(_in_work_tree TRUE)
    execute_process(
        COMMAND "${GIT}" rev-parse --show-prefix
        RESULT_VARIABLE _rp_rc
        OUTPUT_VARIABLE _git_prefix
        ERROR_VARIABLE  _rp_err
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (NOT _rp_rc EQUAL 0)
        message(FATAL_ERROR "apply_patch: git rev-parse --show-prefix failed (exit ${_rp_rc}): ${_rp_err}")
    endif ()
    string(REGEX REPLACE "/$" "" _git_prefix "${_git_prefix}")
endif ()

set(_ws_flags "")
if (LOOSE_WS)
    set(_ws_flags --ignore-space-change --whitespace=fix)
endif ()

foreach (_patch IN LISTS _patches)
    if (NOT EXISTS "${_patch}")
        message(FATAL_ERROR "apply_patch: patch file not found: ${_patch}")
    endif ()

    # Only a git-format patch inside a work tree (below its top) needs --directory; see
    # the top of this file. A traditional diff never gets it.
    file(READ "${_patch}" _ptext)
    set(_dir_flag "")
    if (_ptext MATCHES "(^|\n)diff --git ")
        if (NOT _git_prefix STREQUAL "")
            set(_dir_flag --directory "${_git_prefix}")
            message(STATUS "apply_patch: git-format patch, cwd is '${_git_prefix}' in a git work tree: --directory=${_git_prefix}")
        elseif (_in_work_tree)
            message(STATUS "apply_patch: git-format patch, cwd is the work tree top: no --directory")
        else ()
            message(STATUS "apply_patch: git-format patch, cwd is not in a git work tree: paths resolve against the cwd, no --directory")
        endif ()
    endif ()

    # --verbose makes git print "Checking patch <p>..." / "Applied patch <p> cleanly."
    # per file, which is what we inspect below.
    execute_process(
        COMMAND "${GIT}" apply ${_dir_flag} --verbose ${_ws_flags} "${_patch}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err
    )
    set(_log "${_out}${_err}")
    message(STATUS "apply_patch: ${_patch}")
    if (_log)
        message("${_log}")
    endif ()

    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "apply_patch: git apply failed (exit ${_rc}) for ${_patch}")
    endif ()

    # The whole point: git exits 0 after skipping every hunk.
    if (_log MATCHES "Skipped patch")
        message(FATAL_ERROR
            "apply_patch: git SKIPPED hunks in ${_patch} (and still exited 0).\n"
            "The patch did not reach the source tree. This is the repo-root-relative\n"
            "trap described at the top of deps/apply_patch.cmake: a git-format patch\n"
            "applied inside a work tree without a correct --directory. The\n"
            "--directory used (if any) is in the apply_patch status line above.")
    endif ()

    # A patch that matched nothing at all is just as bad and does not say "Skipped".
    if (NOT _log MATCHES "Applied patch")
        message(FATAL_ERROR
            "apply_patch: git apply reported no applied hunks for ${_patch}.\n"
            "Expected at least one 'Applied patch ... cleanly.' line with --verbose.")
    endif ()
endforeach ()

message(STATUS "apply_patch: all patches applied: ${_patches}")
