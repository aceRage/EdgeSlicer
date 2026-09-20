# 0001-clang19.patch drops the pre-C++11 "safe bool" idiom from three halfedge iterators.
# clang 19 rejects it (https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=281880); CGAL 5.4
# predates the upstream fix and we pin that version. The CI runners are AppleClang 15,
# MSVC 19.44 and GCC 13, so the patch is not exercised there today - it is what keeps a
# newer Xcode, a Homebrew clang, or a Linux distro on clang 19+ building. Keep it.
#
# No --directory here, deliberately - see deps/apply_patch.cmake for the whole rule.
# 0001-clang19.patch is a TRADITIONAL diff ("--- a/BGL/..."), and for those git resolves
# the -p1-stripped path against the cwd, which ExternalProject has already set to the dep
# source dir. Passing --directory on top of that prepends the build path a second time:
# "deps/build/dep_CGAL-prefix/src/dep_CGAL/deps/build/.../graph/iterator.h: No such file".
# Only the GIT-FORMAT patches (OCCT, OpenCV) need --directory, because git resolves those
# against the repo root instead. Verified both ways against the pinned v5.4 zip.
#
# This has always effectively run without the flag: run 35365103460 logs "Applied patch
# deps/build/dep_CGAL-prefix/src/dep_CGAL/BGL/include/.../iterator.h cleanly" - the plain
# cwd-relative path, with no doubling - because IN_GIT_REPO was still undefined this far
# up the file. Making the flag actually reach CGAL is what broke it.

Snapmaker_Orca_add_cmake_project(
    CGAL
    # GIT_REPOSITORY https://github.com/CGAL/cgal.git
    # GIT_TAG        bec70a6d52d8aacb0b3d82a7b4edc3caa899184b # releases/CGAL-5.0
    # For whatever reason, this keeps downloading forever (repeats downloads if finished)
    URL      https://github.com/CGAL/cgal/archive/refs/tags/v5.4.zip
    URL_HASH SHA256=d7605e0a5a5ca17da7547592f6f6e4a59430a0bc861948974254d0de43eab4c0
    PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DLOOSE_WS=ON
                  -DP1=${CMAKE_CURRENT_LIST_DIR}/0001-clang19.patch
                  -P ${CMAKE_CURRENT_LIST_DIR}/../apply_patch.cmake
    DEPENDS dep_Boost dep_GMP dep_MPFR
)

include(GNUInstallDirs)
