# cmake --install the plugin-host component to a temp prefix, then run the
# installed helper's --version so DT_NEEDED libpython must load via INSTALL_RPATH.
# readelf RUNPATH mentioning "libpython" is not enough (BUILD_RPATH already does).
# ldd must resolve libpython under this prefix, not a system path.
if (NOT CMAKE_COMMAND OR NOT BUILD_DIR)
    message(FATAL_ERROR "CMAKE_COMMAND and BUILD_DIR are required")
endif ()
if (NOT BINDIR)
    set(BINDIR "bin")
endif ()

set(PREFIX "${BUILD_DIR}/plugin-host-install-prefix")
file(REMOVE_RECURSE "${PREFIX}")
file(MAKE_DIRECTORY "${PREFIX}")

set(_install_cmd ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${PREFIX}" --component plugin-host)
if (CONFIG)
    list(APPEND _install_cmd --config "${CONFIG}")
endif ()
execute_process(
    COMMAND ${_install_cmd}
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

get_filename_component(PREFIX_ABS "${PREFIX}" ABSOLUTE)

execute_process(COMMAND readelf -d "${HELPER}"
    RESULT_VARIABLE elf_rc
    OUTPUT_VARIABLE elf_out
    ERROR_VARIABLE elf_err)
if (elf_rc EQUAL 0)
    message(STATUS "installed helper readelf:\n${elf_out}")
endif ()

find_program(LDD_EXECUTABLE ldd)
if (NOT LDD_EXECUTABLE)
    message(FATAL_ERROR "ldd not found; cannot check that the installed helper resolves libpython")
endif ()
execute_process(
    COMMAND ${LDD_EXECUTABLE} "${HELPER}"
    RESULT_VARIABLE ldd_rc
    OUTPUT_VARIABLE ldd_out
    ERROR_VARIABLE ldd_err)
if (NOT ldd_rc EQUAL 0)
    message(FATAL_ERROR "ldd failed (${ldd_rc}): ${ldd_err}\n${ldd_out}")
endif ()
message(STATUS "installed helper ldd:\n${ldd_out}")

set(_python_resolved "")
string(REPLACE "\n" ";" _ldd_lines "${ldd_out}")
foreach (_line IN LISTS _ldd_lines)
    if (_line MATCHES "libpython[^ ]* => ([^ ]+)")
        set(_python_resolved "${CMAKE_MATCH_1}")
        break()
    endif ()
endforeach ()
if (_python_resolved STREQUAL "" OR _python_resolved STREQUAL "not")
    message(FATAL_ERROR "ldd did not resolve libpython under the install prefix:\n${ldd_out}")
endif ()
get_filename_component(_python_abs "${_python_resolved}" ABSOLUTE)
string(FIND "${_python_abs}" "${PREFIX_ABS}" _prefix_pos)
if (NOT _prefix_pos EQUAL 0)
    message(FATAL_ERROR
        "libpython resolved outside the temp install prefix:\n  ${_python_abs}\n  prefix: ${PREFIX_ABS}\n${ldd_out}")
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
message(STATUS "libpython resolved at ${_python_abs}")
