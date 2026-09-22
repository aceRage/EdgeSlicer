get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    printhost_devices_tests.cpp
    snapmaker_unload_tests.cpp
    wizard_networking_tests.cpp
    plugin_guard_tests.cpp
    device_model_code_tests.cpp
    login_useragent_tests.cpp
    ff_diagnostics_tests.cpp
    elegoo_canvas_tests.cpp
    splash_animation_tests.cpp
    remote_events_tests.cpp
    remote_hub_tests.cpp
    flashforge_tests.cpp
    mqtt_reconnect_tests.cpp
    dock_width_settle_tests.cpp
    http_empty_post_tests.cpp
    paint_cli_tests.cpp
    hms_lookup_tests.cpp
    hms_actions_tests.cpp
    remote_error_actions_tests.cpp
    plate_focus_hide_tests.cpp
    preset_mirror_tests.cpp
    bambu_config_compat_tests.cpp
    preset_quarantine_tests.cpp
    )

# The watcher's transition rule lives in libslic3r_gui, and reaching it pulls GUI_App and the
# handful of translation units next to it that call the crash-reporter and version wrappers. Those
# wrappers are compiled straight into the app targets rather than into a library (src/CMakeLists.txt
# does the same), so the test executable has to carry the same four files or it cannot link.
if (WIN32)
    target_sources(${_TEST_NAME}_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/sentry_wrapper/SentryWrapper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/bury_cfg/bury_point.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common_func/common_func.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/dev-utils/BaseException.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/dev-utils/StackWalker.cpp
        )
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE BURY_EXPORTS)
    target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r)
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

# hms_lookup_tests.cpp includes slic3r/GUI/HMS.hpp, which pulls GUI_App.hpp and so the wxWidgets
# headers. Every other test in this suite uses a header that does not, which is why this suite
# never needed the wx include path before. wx/platform.h reaches for wx/setup.h, which on MSVC
# lives in <prefix>/include/msvc rather than <prefix>/include, and only wxWidgets_USE_FILE adds it.
# find_package(wxWidgets) runs in src/, whose variables do not reach this sibling directory, so it
# is repeated here - it is cached, so this costs nothing.
find_package(wxWidgets 3.1 QUIET COMPONENTS html adv gl core base webview aui net media)
if (wxWidgets_FOUND)
    # Set on the target, not through wxWidgets_USE_FILE: that one calls include_directories(),
    # which only affects targets created after it, and this target already exists above.
    target_include_directories(${_TEST_NAME}_tests PRIVATE ${wxWidgets_INCLUDE_DIRS})
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE ${wxWidgets_DEFINITIONS})
endif ()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
    Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests "${CATCH_EXTRA_ARGS} exclude:[NotWorking]")
