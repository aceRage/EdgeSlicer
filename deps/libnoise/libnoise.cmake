# Edge: 0001-value-noise-unsigned.patch does IntValueNoise3D's arithmetic (src/noisegen.cpp)
# on unsigned int. The function hashes with int products that overflow by design and masks
# the result to 31 bits; signed overflow is undefined behaviour, so an optimizing compiler
# may conclude the product is non-negative and drop the mask. AppleClang does (measured:
# -O1 and above, arm64 and x86_64): IntValueNoise3D(1000, -1000, 7, 2) came back as
# -1598971919 instead of 548511729, and ValueNoise3D ranged over (-3, 1] on arm64 and
# [-1, 3) on x86_64 instead of the documented [-1, 1]. module::Voronoi is the only user of
# value noise, so "Voronoi" fuzzy skin moved walls by up to three times
# fuzzy_skin_thickness, outwards on arm64. Unsigned arithmetic wraps by definition, so the
# patched function gives the masked result with every compiler; Perlin, Billow and
# RidgedMulti use gradient noise and are unchanged.
# tests/libslic3r/test_fuzzy_skin.cpp ([FuzzySkin][VoronoiNoise]) fails against an unpatched
# library that drops the mask. The upstream zip (SoftFever/Orca-deps-libnoise 1.0) is as
# libnoise 1.0.0 left it; there is no upstream fix to pin instead.
#
# A TRADITIONAL diff ("--- a/src/noisegen.cpp"), so no --directory: see
# deps/apply_patch.cmake.

Snapmaker_Orca_add_cmake_project(libnoise
    URL https://github.com/SoftFever/Orca-deps-libnoise/archive/refs/tags/1.0.zip
        URL_HASH SHA256=96ffd6cc47898dd8147aab53d7d1b1911b507d9dbaecd5613ca2649468afd8b6
    PATCH_COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DLOOSE_WS=ON
                  -DP1=${CMAKE_CURRENT_LIST_DIR}/0001-value-noise-unsigned.patch
                  -P ${CMAKE_CURRENT_LIST_DIR}/../apply_patch.cmake
)
