# CPack pre-build script (CPACK_PRE_BUILD_SCRIPTS), registered by cmake/signing/WindowsSigning.cmake
# only when the build was configured with -DEDGESLICER_SIGN_WINDOWS=ON.
#
# CPack does not package the build's install tree: it re-runs the install into its own staging
# directory (CPACK_TEMPORARY_DIRECTORY, under _CPack_Packages) and packs that. The workflow has
# already signed the install tree (build\EdgeSlicer, which becomes the portable zip), so here every
# PE file of the staging copy is replaced by the signed file at the same relative path. The
# installer and the portable zip then ship byte-identical files, and no second set of signatures
# is spent. Finally every PE file in the staging tree is verified; anything left unsigned fails
# the package.
#
# Does nothing unless the EDGESLICER_SIGN_WINDOWS environment variable is 1.

if (NOT "$ENV{EDGESLICER_SIGN_WINDOWS}" STREQUAL "1")
    message(STATUS "EdgeSlicer signing: EDGESLICER_SIGN_WINDOWS is not 1 - packaging the staging tree as installed")
else ()
    set(_signed "$ENV{EDGESLICER_SIGNED_TREE}")
    if (NOT _signed)
        set(_signed "${CPACK_EDGESLICER_SIGNED_TREE}")
    endif ()
    file(TO_CMAKE_PATH "${_signed}" _signed)
    set(_stage "${CPACK_TEMPORARY_DIRECTORY}")
    file(TO_CMAKE_PATH "${_stage}" _stage)

    if (NOT IS_DIRECTORY "${_signed}")
        message(FATAL_ERROR "EdgeSlicer signing: the signed tree ${_signed} does not exist (set EDGESLICER_SIGNED_TREE)")
    endif ()
    if (NOT IS_DIRECTORY "${_stage}")
        message(FATAL_ERROR "EdgeSlicer signing: CPack's staging directory '${_stage}' does not exist")
    endif ()
    if (_signed STREQUAL _stage)
        message(FATAL_ERROR "EdgeSlicer signing: the signed tree and the staging tree are the same directory")
    endif ()
    message(STATUS "EdgeSlicer signing: overlaying signed PE files from ${_signed} onto ${_stage}")

    file(GLOB_RECURSE _pe LIST_DIRECTORIES false RELATIVE "${_stage}" "${_stage}/*.exe" "${_stage}/*.dll")
    set(_replaced 0)
    set(_same 0)
    set(_missing "")
    foreach (_rel IN LISTS _pe)
        set(_src "${_signed}/${_rel}")
        set(_dst "${_stage}/${_rel}")
        if (EXISTS "${_src}")
            file(SHA256 "${_src}" _h_src)
            file(SHA256 "${_dst}" _h_dst)
            if (_h_src STREQUAL _h_dst)
                math(EXPR _same "${_same} + 1")
            else ()
                file(COPY_FILE "${_src}" "${_dst}")
                math(EXPR _replaced "${_replaced} + 1")
            endif ()
        else ()
            list(APPEND _missing "${_rel}")
        endif ()
    endforeach ()
    list(LENGTH _pe _n)
    message(STATUS "EdgeSlicer signing: ${_n} PE files staged, ${_replaced} replaced by the signed copy, ${_same} already identical")
    foreach (_rel IN LISTS _missing)
        message(STATUS "EdgeSlicer signing: no signed counterpart for ${_rel} (the check below decides)")
    endforeach ()

    set(_allow "")
    if (NOT "$ENV{EDGESLICER_SIGN_FLASHNETWORK}" STREQUAL "1")
        set(_allow -AllowUnsigned FlashNetwork.dll)
    endif ()
    file(TO_NATIVE_PATH "${_stage}" _stage_native)
    execute_process(
        COMMAND powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CPACK_EDGESLICER_VERIFY_SCRIPT}" -Path "${_stage_native}" ${_allow}
        RESULT_VARIABLE _rc)
    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "EdgeSlicer signing: the staging tree has unsigned or invalid PE files (verify exit ${_rc}) - not packaging")
    endif ()
endif ()
