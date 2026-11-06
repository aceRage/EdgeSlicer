# Phase 0 spike. Linked into its own probe and tests only — never into libslic3r or the slicer
# binary — so a default (OFF) build is unchanged and G-code stays identical.

add_library(edge_plugin_spike STATIC
    PluginPathPolicy.cpp
    PluginGrantPolicy.cpp
    PluginSandboxPolicy.cpp
    PluginHostSpawnModel.cpp
    PluginSandboxProbe.cpp
    PluginCatalogPolicy.cpp
)
target_include_directories(edge_plugin_spike PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(edge_plugin_spike PUBLIC cxx_std_17)
set_property(TARGET edge_plugin_spike PROPERTY FOLDER "spike")

add_executable(edge_plugin_spike_probe edge_plugin_spike_main.cpp)
target_link_libraries(edge_plugin_spike_probe PRIVATE edge_plugin_spike)
set_property(TARGET edge_plugin_spike_probe PROPERTY FOLDER "spike")

add_executable(plugin_spike_tests
    plugin_spike_tests_main.cpp
    plugin_spike_tests.cpp
)
target_include_directories(plugin_spike_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(plugin_spike_tests PRIVATE edge_plugin_spike)
target_compile_features(plugin_spike_tests PRIVATE cxx_std_17)
set_property(TARGET plugin_spike_tests PROPERTY FOLDER "spike")

enable_testing()
add_test(NAME plugin_spike COMMAND plugin_spike_tests)
