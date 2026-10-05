if(WIN32)
    set(library_build_type "Shared")
else()
    set(library_build_type "Static")
endif()

# ModelingAlgorithms is ON for the B-rep tools (STEP export, mesh -> B-rep, and the fillet /
# offset / feature operations the CAD work builds on). DataExchange already pulled in most of
# the module (TKBO TKBool TKGeomAlgo TKHLR TKMesh TKPrim TKShHealing TKTopAlgo); turning the
# module on adds exactly four toolkits: TKFillet, TKOffset, TKFeat and TKXMesh. Static on
# macOS/Linux (only referenced objects are linked); shared on Windows, where the app ships
# TKFillet/TKOffset/TKFeat plus TKBool (their dependency). Toolkit walk: OCCT adm/MODULES and
# src/<TK>/EXTERNLIB; the same analysis as Orca-Cad's docs/CAD/cad_dependency_weight.md.
#
# OCCT 8.0.1 exposes the install directories as cache variables, so the 7.6 0001-OCCT-fix.patch
# is no longer applied. Hunks that patch replaced (and why they are gone) are listed in the
# Spike A PR: install-dir / build-dir / LICENSE / custom / env / resources CMakeLists edits,
# Font_FTFont HAVE_FREETYPE guards (upstream already has them), and StdPrs_BRepFont FT_Outline
# tags (upstream 0033808). RelWithDebInfo / Debug still land in bin/occti and bin/occtd;
# Windows packaging reads the directory from the imported TKernel target.
Snapmaker_Orca_add_cmake_project(OCCT
    URL https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/V8_0_1.zip
    URL_HASH SHA256=7c033d917ee8f040c0512d289dcc5f02c148889d5bac17c3e25639accb44f0da
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

# add_dependencies(dep_OCCT ${FREETYPE_PKG})
