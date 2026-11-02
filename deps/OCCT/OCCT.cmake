if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

# Two OCCT recipes, picked by the deps-level SLIC3R_CAD option (deps/CMakeLists.txt):
#   SLIC3R_CAD OFF (the default, every shared deps prefix, CI and release builds): OCCT 7.6.0, the
#                  recipe main has always used. Do not change it here; moving every build to OCCT 8
#                  is a separate step.
#   SLIC3R_CAD ON  (the Design tab, e.g. edgebuild's "cad" deps profile, built with
#                  -DSLIC3R_CAD=ON): OCCT 8.0.1 + SolveSpace (deps/SLVS). The main project must then
#                  be configured with SLIC3R_CAD=ON too; src/libslic3r/CMakeLists.txt checks that the
#                  OCCT it finds matches.
#
# ModelingAlgorithms is ON for the B-rep tools (STEP export, mesh -> B-rep, and the fillet /
# offset / feature operations the CAD work builds on). DataExchange already pulled in most of
# the module (TKBO TKBool TKGeomAlgo TKHLR TKMesh TKPrim TKShHealing TKTopAlgo); turning the
# module on adds exactly four toolkits: TKFillet, TKOffset, TKFeat and TKXMesh. Static on
# macOS/Linux (only referenced objects are linked); shared on Windows, where the app ships
# TKFillet/TKOffset/TKFeat plus TKBool (their dependency). Toolkit walk: OCCT adm/MODULES and
# src/<TK>/EXTERNLIB; the same analysis as Orca-Cad's docs/CAD/cad_dependency_weight.md.
if (NOT SLIC3R_CAD)

# 0001-OCCT-fix.patch is a GIT-FORMAT diff. deps/apply_patch.cmake works out the
# --directory it needs (inside a git work tree) from the source dir at patch time.
Snapmaker_Orca_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V7_6_0.zip
    URL_HASH SHA256=28334f0e98f1b1629799783e9b4d21e05349d89e695809d7e6dfa45ea43e1dbc
    PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DLOOSE_WS=ON
                  -DP1=${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch
                  -P ${CMAKE_CURRENT_LIST_DIR}/../apply_patch.cmake
    #DEPENDS dep_Boost
    DEPENDS ${FREETYPE_PKG}
    CMAKE_ARGS
        -DBUILD_LIBRARY_TYPE=${library_build_type}
        -DUSE_TK=OFF
        -DUSE_TBB=OFF
	#-DUSE_FREETYPE=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF
        #-DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_FoundationClasses=OFF
        -DBUILD_MODULE_ModelingAlgorithms=ON
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
)

else ()

# OCCT 8.0.1 exposes the install directories as cache variables, so the 7.6 0001-OCCT-fix.patch
# is not applied. Hunks that patch replaced (and why they are gone) are listed in the
# Spike A PR: install-dir / build-dir / LICENSE / custom / env / resources CMakeLists edits,
# Font_FTFont HAVE_FREETYPE guards (upstream already has them), and StdPrs_BRepFont FT_Outline
# tags (upstream 0033808). RelWithDebInfo / Debug still land in bin/occti and bin/occtd;
# Windows packaging reads the directory from the imported TKernel target.
#
# 0001-BRepMesh-seam-pcurve-at-edge-parameter.patch is a GIT-FORMAT diff. Without a correct
# --directory, git apply inside a work tree resolves the paths against the repo root, skips
# every hunk and exits 0. deps/apply_patch.cmake works that --directory out at patch time
# from `git rev-parse --show-prefix` in the dep source dir (same as OpenCV); do not pass
# -DDIRECTORY from here — the wrapper fatals if you do. The patch evaluates a seam pcurve at
# the edge parameter instead of Geom2d_Line::FirstParameter() (Open-Cascade-SAS/OCCT#572 /
# Orca #16290). Remove it once an OCCT release includes the fix.
Snapmaker_Orca_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V8_0_1.zip
    URL_HASH SHA256=7c033d917ee8f040c0512d289dcc5f02c148889d5bac17c3e25639accb44f0da
    PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DLOOSE_WS=ON
                  -DP1=${CMAKE_CURRENT_LIST_DIR}/0001-BRepMesh-seam-pcurve-at-edge-parameter.patch
                  -P ${CMAKE_CURRENT_LIST_DIR}/../apply_patch.cmake
    #DEPENDS dep_Boost
    DEPENDS ${FREETYPE_PKG}
    CMAKE_ARGS
        -DCMAKE_CXX_STANDARD=17
        -DBUILD_LIBRARY_TYPE=${library_build_type}
        # With the Unix layout, OCCT's resources and licenses go under share/ and its scripts
        # into bin/occt on Windows too. libslic3r finds the CMake package in lib/cmake/occt.
        -DINSTALL_DIR_LAYOUT=Unix
        -DINSTALL_DIR_BIN=bin/occt
        -DINSTALL_DIR_LIB=lib/occt
        -DINSTALL_DIR_INCLUDE=include/occt
        -DINSTALL_DIR_CMAKE=lib/cmake/occt
        -DUSE_TK=OFF
        -DUSE_TBB=OFF
	#-DUSE_FREETYPE=OFF
        -DUSE_FFMPEG=OFF
        -DUSE_VTK=OFF
        -DBUILD_DOC_Overview=OFF
        -DBUILD_MODULE_ApplicationFramework=OFF
        #-DBUILD_MODULE_DataExchange=OFF
        -DBUILD_MODULE_Draw=OFF
        -DBUILD_MODULE_FoundationClasses=OFF
        -DBUILD_MODULE_ModelingAlgorithms=ON
        -DBUILD_MODULE_ModelingData=OFF
        -DBUILD_MODULE_Visualization=OFF
)

endif ()

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
