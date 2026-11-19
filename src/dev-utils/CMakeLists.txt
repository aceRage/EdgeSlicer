
option(SLIC3R_ENC_CHECK "Verify encoding of source files" 1)

if (IS_CROSS_COMPILE)
    # Force disable due to cross compilation. This fact is already printed on cli for users
    set(SLIC3R_ENC_CHECK OFF CACHE BOOL "" FORCE)
endif ()

if (SLIC3R_ENC_CHECK)
    add_executable(encoding-check encoding-check.cpp)

    # A global no-op target which depends on all encodings checks,
    # and on which in turn all checked targets depend.
    # This is done to make encoding checks the first thing to be
    # performed before actually compiling any sources of the checked targets
    # to make the check fail as early as possible.
    add_custom_target(global-encoding-check
        ALL
        DEPENDS encoding-check
    )
endif()

# Function that adds source file encoding check to a target
# using the above encoding-check binary

function(encoding_check TARGET)
    if (SLIC3R_ENC_CHECK)
        # Obtain target source files
        get_target_property(T_SOURCES ${TARGET} SOURCES)

        # Stamp file so the check is not an always-out-of-date UTILITY target.
        # add_dependencies(libslic3r_gui global-encoding-check) used to relink
        # edge-plugin-host on every build (PE timestamp → hash churn on Windows).
        set(_enc_stamp "${CMAKE_CURRENT_BINARY_DIR}/encoding-check-${TARGET}.stamp")
        set(_enc_cmds)
        foreach(file ${T_SOURCES})
            list(APPEND _enc_cmds COMMAND $<TARGET_FILE:encoding-check> ${TARGET} ${file})
        endforeach()
        add_custom_command(
            OUTPUT ${_enc_stamp}
            ${_enc_cmds}
            COMMAND ${CMAKE_COMMAND} -E touch ${_enc_stamp}
            DEPENDS encoding-check ${T_SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Checking source files encodings for target ${TARGET}"
            VERBATIM
        )
        add_custom_target(encoding-check-${TARGET} DEPENDS ${_enc_stamp})

        add_dependencies(global-encoding-check encoding-check-${TARGET})
        add_dependencies(${TARGET} encoding-check-${TARGET})
    endif()
endfunction()
