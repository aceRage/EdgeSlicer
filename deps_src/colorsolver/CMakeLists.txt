cmake_minimum_required(VERSION 3.13)
project(colorsolver)

add_library(colorsolver STATIC
    ColorSolver.cpp
    ColorSolver.hpp
)

target_include_directories(colorsolver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The mix model is the fork's own header-only filament_mixer_model.h. It has no
# dependencies of its own, so this is an include path and not a link edge; that
# keeps colorsolver buildable before libslic3r and free of any other library.
target_include_directories(colorsolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_compile_features(colorsolver PUBLIC cxx_std_17)
