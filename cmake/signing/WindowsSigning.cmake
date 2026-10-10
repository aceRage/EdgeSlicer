# Authenticode signing hooks for the Windows NSIS package (docs/windows-signing.md).
#
# OFF by default, and with the option OFF this file defines no CPACK_* variable at all, so the
# generated CPackConfig.cmake and project.nsi are exactly what they were before it existed.
#
# With -DEDGESLICER_SIGN_WINDOWS=ON (the self-hosted Windows workflow passes it only when the
# repository variable EDGESLICER_SIGN_WINDOWS is '1'):
#   - !uninstfinalize signs Uninstall.exe after makensis generates it and before it is embedded
#     in the installer (NSIS 3.08+);
#   - !finalize signs the finished installer exe, before cpack copies it out and checksums it;
#   - a CPack pre-build script overlays the already signed build tree (the portable zip's tree)
#     onto CPack's staging copy, so the installer carries byte-identical signed files, and then
#     verifies that every PE file in the staging tree is signed.
# All three call scripts/windows_sign.ps1 / windows_verify_signatures.ps1, which do nothing unless
# the EDGESLICER_SIGN_WINDOWS environment variable is '1' when cpack runs, so a local cpack with
# the option ON but no signing environment still produces today's (unsigned) installer.
#
# NSIS compile-time commands take no variables, so the command lines are fixed here at configure
# time; everything that varies (account, profile, endpoint, credentials) reaches the scripts
# through the environment cpack and makensis inherit.

option(EDGESLICER_SIGN_WINDOWS "Hook Authenticode signing into the NSIS package (signs only when the EDGESLICER_SIGN_WINDOWS environment variable is 1 at cpack time)" OFF)

if (EDGESLICER_SIGN_WINDOWS AND WIN32)
    if (CMAKE_VERSION VERSION_LESS 3.21)
        # CPACK_PRE_BUILD_SCRIPTS is 3.19+, file(COPY_FILE) in the staging script 3.21+.
        message(FATAL_ERROR "EDGESLICER_SIGN_WINDOWS needs CMake 3.21 or newer (this is ${CMAKE_VERSION})")
    endif ()

    get_filename_component(_edgeslicer_sign_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(_edgeslicer_sign_ps1 "${_edgeslicer_sign_root}/scripts/windows_sign.ps1")
    set(_edgeslicer_verify_ps1 "${_edgeslicer_sign_root}/scripts/windows_verify_signatures.ps1")
    foreach (_f IN ITEMS "${_edgeslicer_sign_ps1}" "${_edgeslicer_verify_ps1}")
        if (NOT EXISTS "${_f}")
            message(FATAL_ERROR "EDGESLICER_SIGN_WINDOWS: ${_f} is missing")
        endif ()
    endforeach ()
    if (_edgeslicer_sign_ps1 MATCHES "[$'%]")
        message(FATAL_ERROR "EDGESLICER_SIGN_WINDOWS: the source path ${_edgeslicer_sign_root} contains a character NSIS or cmd would reinterpret ($ ' %)")
    endif ()

    # makensis runs the command through system() (cmd.exe) and replaces %1 with the file it just
    # wrote; "= 0" makes makensis stop with an error when the command does not return 0, so a
    # signing failure fails cpack instead of producing an unsigned package.
    set(_edgeslicer_sign_cmd "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"${_edgeslicer_sign_ps1}\" -Path \"%1\"")
    set(_edgeslicer_nsis_defines "!uninstfinalize '${_edgeslicer_sign_cmd}' = 0\n!finalize '${_edgeslicer_sign_cmd}' = 0")
    if (CPACK_NSIS_DEFINES)
        string(APPEND CPACK_NSIS_DEFINES "\n${_edgeslicer_nsis_defines}")
    else ()
        set(CPACK_NSIS_DEFINES "${_edgeslicer_nsis_defines}")
    endif ()

    list(APPEND CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/cpack_sign_staging.cmake")
    # Read by the pre-build script (CPACK_* variables are forwarded into CPackConfig.cmake).
    # The signed tree is the install prefix the workflow signs; EDGESLICER_SIGNED_TREE in the
    # environment overrides it.
    get_filename_component(CPACK_EDGESLICER_SIGNED_TREE "${CMAKE_INSTALL_PREFIX}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
    set(CPACK_EDGESLICER_VERIFY_SCRIPT "${_edgeslicer_verify_ps1}")

    message(STATUS "EdgeSlicer: Authenticode signing hooks are in the NSIS package (active only with EDGESLICER_SIGN_WINDOWS=1 in the cpack environment)")
endif ()
