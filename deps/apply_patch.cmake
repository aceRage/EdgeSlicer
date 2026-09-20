# Ultra: `git apply` wrapper for deps PATCH_COMMAND steps that FAILS when a patch is
# silently skipped. Run from the dependency's source dir (ExternalProject runs
# PATCH_COMMAND there) as
#
#   ${CMAKE_COMMAND} -DGIT=<git> -DP1=<a.patch> [-DP2=<b.patch> ... -DP8=...]
#                    [-DDIRECTORY=<rel>] [-DLOOSE_WS=ON]
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
# b/x") against the REPO ROOT, not the current directory. The deps build tree lives
# inside the checkout both on CI and on every developer machine, so the paths never
# exist, git prints "Skipped patch 'src/foo.cpp'." for every file - and EXITS 0. The
# build then compiles the unpatched tree and fails later, somewhere unrelated, or
# (worse) succeeds with the bug the patch was meant to fix still in it. That cost a CI
# cycle for QuadriFlow on 2026-09-18 (run 35365103460).
#
# The workaround is the `--directory ${BINARY_DIR_REL}/...` flag, guarded by IN_GIT_REPO,
# which deps/CMakeLists.txt used to define BELOW four of the recipes that read it. That
# block now sits above the first include(), but one moved line could put it back and
# `git apply` will keep failing open when it does. Hence this wrapper: it parses git's
# own output and turns "Skipped patch" into a hard error.
#
# WHICH RECIPES PASS DIRECTORY, AND WHY - it follows the patch FORMAT, not the dep:
#
#   GIT-FORMAT ("diff --git a/x b/x")  ->  DIRECTORY REQUIRED  :  OCCT, OpenCV
#       git resolves these against the REPO ROOT, so without the flag every hunk is
#       skipped and git STILL EXITS 0. Measured: OCCT without it => "Skipped patch
#       'CMakeLists.txt'", 0 hunks applied, rc 0. That is the original bug.
#
#   TRADITIONAL ("--- a/x", no diff --git)  ->  DIRECTORY MUST BE ABSENT :
#                                              CGAL, GMP, OpenVDB
#       git resolves the -p1-stripped path against the CWD, which ExternalProject has
#       already set to the dep source dir - so it is already correct. Adding --directory
#       prepends the build path a second time: git then dies with "unable to find
#       filename in patch at line 1" (GMP, OpenVDB) or builds a doubled path (CGAL).
#       Measured on CI: run 35374725461 (GMP) and run 35376108670 (OpenVDB).
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

set(_dir_flag "")
if (DIRECTORY)
    set(_dir_flag --directory "${DIRECTORY}")
endif ()

set(_ws_flags "")
if (LOOSE_WS)
    set(_ws_flags --ignore-space-change --whitespace=fix)
endif ()

foreach (_patch IN LISTS _patches)
    if (NOT EXISTS "${_patch}")
        message(FATAL_ERROR "apply_patch: patch file not found: ${_patch}")
    endif ()

    # Guard the format/DIRECTORY pairing documented at the top, so a mismatch fails here
    # with an explanation rather than as a puzzling git error (or, worse, a silent skip).
    file(READ "${_patch}" _ptext)
    if (_ptext MATCHES "(^|\n)diff --git ")
        set(_is_git_format TRUE)
    else ()
        set(_is_git_format FALSE)
    endif ()
    if (_is_git_format AND NOT DIRECTORY)
        message(FATAL_ERROR
            "apply_patch: ${_patch} is a GIT-FORMAT patch but no DIRECTORY was given.\n"
            "git resolves those against the repo root, so every hunk would be skipped\n"
            "while git still exits 0. Pass -DDIRECTORY=<build-dir path relative to the\n"
            "repo root> from the recipe (see deps/OCCT/OCCT.cmake).")
    endif ()
    if (NOT _is_git_format AND DIRECTORY)
        message(FATAL_ERROR
            "apply_patch: ${_patch} is a TRADITIONAL diff but DIRECTORY was given.\n"
            "git resolves those against the cwd, which is already the dep source dir;\n"
            "--directory prepends the build path a second time and the apply fails.\n"
            "Drop -DDIRECTORY from the recipe (see deps/CGAL/CGAL.cmake).")
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
            "applied inside a work tree without a correct --directory. Check that\n"
            "IN_GIT_REPO/BINARY_DIR_REL are defined before this dep's include() in\n"
            "deps/CMakeLists.txt and that the --directory path is right.")
    endif ()

    # A patch that matched nothing at all is just as bad and does not say "Skipped".
    if (NOT _log MATCHES "Applied patch")
        message(FATAL_ERROR
            "apply_patch: git apply reported no applied hunks for ${_patch}.\n"
            "Expected at least one 'Applied patch ... cleanly.' line with --verbose.")
    endif ()
endforeach ()

message(STATUS "apply_patch: all patches applied: ${_patches}")
