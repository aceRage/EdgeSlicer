if(BUILD_SHARED_LIBS)
    set(_build_shared ON)
    set(_build_static OFF)
else()
    set(_build_shared OFF)
    set(_build_static ON)
endif()

# 0001-clang19.patch drops an ill-formed `OpT::template eval(...)` from NodeManager.h:
# `template` there names no template, which clang 19 diagnoses as an error. Upstream
# OpenVDB fixed it in 930c3acb8e0c (AcademySoftwareFoundation/openvdb); this fork pins a
# 2021 tamasmeszaros snapshot that predates it, so the patch is still required to build
# under clang 19+. The CI runners are AppleClang 15, MSVC 19.44 and GCC 13, none of which
# trips on it - which is exactly why its absence went unnoticed.
#
# The PATCH_COMMAND line was dropped by accident in the "Merge: Snapmaker Orca 2.1.2"
# merge (e89263e51a): the conflict resolution kept this IN_GIT_REPO block and the .patch
# file but lost the step that used them, so every build since has logged "No patch step
# for 'dep_OpenVDB'" and built unpatched. Restored here.
#
# No --directory here, deliberately - see deps/apply_patch.cmake for the whole rule.
# 0001-clang19.patch is a TRADITIONAL diff ("--- a/openvdb/..."), and git resolves those
# against the cwd, which ExternalProject has already set to the dep source dir. Adding
# --directory makes git fail with "unable to find filename in patch at line 1" - that is
# exactly what run 35376108670 did on macos-14. Only the GIT-FORMAT patches (OCCT,
# OpenCV) need it. Verified both ways against the pinned tamasmeszaros snapshot: without
# the flag all three `OpT::template eval` call sites are fixed; with it, none are.

Snapmaker_Orca_add_cmake_project(OpenVDB
    #  support vs2022, update to 8.2
    URL https://github.com/tamasmeszaros/openvdb/archive/a68fd58d0e2b85f01adeb8b13d7555183ab10aa5.zip
    URL_HASH SHA256=f353e7b99bd0cbfc27ac9082de51acf32a8bc0b3e21ff9661ecca6f205ec1d81
    PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DLOOSE_WS=ON
                  -DP1=${CMAKE_CURRENT_LIST_DIR}/0001-clang19.patch
                  -P ${CMAKE_CURRENT_LIST_DIR}/../apply_patch.cmake
    DEPENDS dep_TBB dep_Blosc dep_OpenEXR dep_Boost
    CMAKE_ARGS
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON 
        -DOPENVDB_BUILD_PYTHON_MODULE=OFF
        -DUSE_BLOSC=ON
        -DOPENVDB_CORE_SHARED=${_build_shared} 
        -DOPENVDB_CORE_STATIC=${_build_static}
        -DOPENVDB_ENABLE_RPATH:BOOL=OFF
        -DTBB_STATIC=${_build_static}
        -DOPENVDB_BUILD_VDB_PRINT=ON
        -DDISABLE_DEPENDENCY_VERSION_CHECKS=ON # Centos6 has old zlib
)

if (MSVC)
    if (${DEP_DEBUG})
        ExternalProject_Get_Property(dep_OpenVDB BINARY_DIR)
        ExternalProject_Add_Step(dep_OpenVDB build_debug
            DEPENDEES build
            DEPENDERS install
            COMMAND ${CMAKE_COMMAND} ../dep_OpenVDB -DOPENVDB_BUILD_VDB_PRINT=OFF
            COMMAND msbuild /m /P:Configuration=Debug INSTALL.vcxproj
            WORKING_DIRECTORY "${BINARY_DIR}"
        )
    endif ()
endif ()