# cmake --install the plugin-host component to a temp prefix, then run the
# installed helper's --version so DT_NEEDED libpython must load via INSTALL_RPATH.
if (NOT CMAKE_COMMAND OR NOT BUILD_DIR)
    message(FATAL_ERROR "CMAKE_COMMAND and BUILD_DIR are required")
endif ()
if (NOT BINDIR)
    set(BINDIR "bin")
endif ()

set(PREFIX "${BUILD_DIR}/plugin-host-install-prefix")
file(REMOVE_RECURSE "${PREFIX}")
file(MAKE_DIRECTORY "${PREFIX}")

execute_process(
    COMMAND ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${PREFIX}" --component plugin-host
    RESULT_VARIABLE install_rc
    OUTPUT_VARIABLE install_out
    ERROR_VARIABLE install_err)
if (NOT install_rc EQUAL 0)
    message(FATAL_ERROR "cmake --install --component plugin-host failed (${install_rc}):\n${install_out}\n${install_err}")
endif ()

set(HELPER "${PREFIX}/${BINDIR}/edge-plugin-host")
if (NOT EXISTS "${HELPER}")
    set(HELPER "${PREFIX}/edge-plugin-host")
endif ()
if (NOT EXISTS "${HELPER}")
    message(FATAL_ERROR "installed helper not found under ${PREFIX} (BINDIR=${BINDIR})")
endif ()

execute_process(COMMAND readelf -d "${HELPER}"
    RESULT_VARIABLE elf_rc
    OUTPUT_VARIABLE elf_out
    ERROR_VARIABLE elf_err)
if (elf_rc EQUAL 0)
    if (NOT elf_out MATCHES "libpython" AND NOT elf_out MATCHES "python/lib")
        message(FATAL_ERROR "installed helper RUNPATH/RPATH does not mention libpython or python/lib:\n${elf_out}")
    endif ()
endif ()

execute_process(
    COMMAND "${HELPER}" --version
    RESULT_VARIABLE ver_rc
    OUTPUT_VARIABLE ver_out
    ERROR_VARIABLE ver_err)
if (NOT ver_rc EQUAL 0)
    message(FATAL_ERROR "installed helper --version failed (${ver_rc}): ${ver_err}\n${ver_out}")
endif ()
if (NOT ver_out MATCHES "edge-plugin-host")
    message(FATAL_ERROR "installed helper --version output unexpected: ${ver_out}")
endif ()
message(STATUS "plugin-host install check: ${ver_out}")
