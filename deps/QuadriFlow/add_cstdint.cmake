# Ultra: run from the QuadriFlow source dir as a PATCH_COMMAND step
# (`cmake -P add_cstdint.cmake`). Prepends `#include <cstdint>` to the five sources
# that use uint8_t/uint32_t/uint64_t without including anything that declares them.
#
# Why a script and not `git apply` + a .patch: git treats a git-format patch as
# repo-root-relative when it runs inside a work tree (the deps build dir lives inside
# the checkout on CI and on every developer machine), and then SKIPS every hunk while
# still exiting 0. The --directory workaround the other patched deps carry depends on
# IN_GIT_REPO, which deps/CMakeLists.txt only defines after this file is included, so
# on 2026-09-18 CI silently built the unpatched tree and failed at loader.cpp:31 again.
# This script has no git semantics, needs no patch tool, is idempotent, and fails loud.

set(_files
    src/dedge.cpp
    src/dset.hpp
    src/hierarchy.cpp
    src/loader.cpp
    src/parametrizer-sing.cpp
)

foreach (_f IN LISTS _files)
    if (NOT EXISTS "${_f}")
        message(FATAL_ERROR "QuadriFlow patch: ${_f} not found (cwd must be the QuadriFlow source dir)")
    endif ()
    file(READ "${_f}" _content)
    if (_content MATCHES "#include <cstdint>")
        continue()
    endif ()
    file(WRITE "${_f}" "#include <cstdint>\n${_content}")
endforeach ()

# Fail loud if the edit did not land - a patch step that fails open cost a CI cycle.
foreach (_f IN LISTS _files)
    file(READ "${_f}" _content)
    if (NOT _content MATCHES "#include <cstdint>")
        message(FATAL_ERROR "QuadriFlow patch: ${_f} still lacks #include <cstdint>")
    endif ()
endforeach ()
message(STATUS "QuadriFlow patch: <cstdint> present in ${_files}")
