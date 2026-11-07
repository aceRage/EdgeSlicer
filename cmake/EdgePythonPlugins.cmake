# Phase 1 Python plugin runtime: bundled CPython + pybind11 + uv.
# Included from the root CMakeLists when EDGE_PYTHON_PLUGINS is ON. When OFF this
# file still defines empty GeneratedConfig placeholders so configure_file succeeds.

if(NOT DEFINED _bundled_python_root)
    set(_bundled_python_root "")
endif()
if(NOT DEFINED ORCA_BUNDLED_UV_EXECUTABLE_CONFIG)
    set(ORCA_BUNDLED_UV_EXECUTABLE_CONFIG "")
endif()
if(NOT DEFINED ORCA_BUNDLED_UV_EXECUTABLE)
    set(ORCA_BUNDLED_UV_EXECUTABLE "")
endif()
if(NOT DEFINED ORCA_BUNDLED_UV_FILENAME)
    set(ORCA_BUNDLED_UV_FILENAME "")
endif()

if(NOT EDGE_PYTHON_PLUGINS)
    return()
endif()

# --- uv 0.11.21 (same pin as OrcaSlicer) --------------------------------------
set(ORCA_BUNDLED_UV_EXECUTABLE "" CACHE FILEPATH "Path to a uv executable to bundle for Python package installation. Leave empty to auto-download.")
set(ORCA_UV_VERSION "0.11.21")
set(ORCA_UV_SHA256_aarch64-apple-darwin      "1f921d491ba5ffeea774eb04d6681ecee379101341cbb1500394993b541bf3f4")
set(ORCA_UV_SHA256_x86_64-apple-darwin       "f3c8e5708a84b920c18b691214d54d2b0da6b984789caae95d47c95120cb7765")
set(ORCA_UV_SHA256_aarch64-unknown-linux-gnu "88e800834007cc5efd4675f166eb2a51e7e3ad19876d85fa8805a6fb5c922397")
set(ORCA_UV_SHA256_x86_64-unknown-linux-gnu  "8c88519b0ef0af9801fcdee419bbb12116bd9e6b18e162ae093c932d8b264050")
set(ORCA_UV_SHA256_aarch64-pc-windows-msvc   "74e443f8004022dde57a1bd0d10c097830f9ea8feb4ec927db52cd5d805c2f48")
set(ORCA_UV_SHA256_x86_64-pc-windows-msvc    "ace861f360c6de2babedc1607d0f454b6b09a820dbc8182dc15af927e4df9589")

set(ORCA_UV_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/.uv/${ORCA_UV_VERSION}")
set(ORCA_UV_BINARY "${ORCA_UV_DOWNLOAD_DIR}/uv")
if(WIN32)
    set(ORCA_UV_BINARY "${ORCA_UV_DOWNLOAD_DIR}/uv.exe")
endif()

if(ORCA_BUNDLED_UV_EXECUTABLE MATCHES "/\\.uv/")
    set(ORCA_BUNDLED_UV_EXECUTABLE "" CACHE FILEPATH "Path to the auto-downloaded uv executable" FORCE)
endif()

# Flatpak already runs inside bwrap: never nest a second sandbox, and never
# download at configure time. The Flatpak manifest stages uv next to resources.
set(ORCA_UV_FLATPAK_BINARY "${CMAKE_INSTALL_FULL_DATAROOTDIR}/${SLIC3R_APP_KEY}/tools/uv/uv")
if(FLATPAK AND NOT ORCA_BUNDLED_UV_EXECUTABLE AND EXISTS "${ORCA_UV_FLATPAK_BINARY}")
    message(STATUS "Using Flatpak-provided uv: ${ORCA_UV_FLATPAK_BINARY}")
    set(ORCA_BUNDLED_UV_EXECUTABLE "${ORCA_UV_FLATPAK_BINARY}")
endif()

if(NOT ORCA_BUNDLED_UV_EXECUTABLE)
    if(NOT EXISTS "${ORCA_UV_BINARY}")
        set(ORCA_UV_EXT "tar.gz")
        if(WIN32)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
                set(ORCA_UV_ARCH "aarch64-pc-windows-msvc")
            else()
                set(ORCA_UV_ARCH "x86_64-pc-windows-msvc")
            endif()
            set(ORCA_UV_EXT "zip")
        elseif(APPLE)
            if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(ORCA_UV_ARCH "aarch64-apple-darwin")
            else()
                set(ORCA_UV_ARCH "x86_64-apple-darwin")
            endif()
        else()
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
                set(ORCA_UV_ARCH "aarch64-unknown-linux-gnu")
            else()
                set(ORCA_UV_ARCH "x86_64-unknown-linux-gnu")
            endif()
        endif()

        if(ORCA_UV_ARCH)
            set(ORCA_UV_URL "https://github.com/astral-sh/uv/releases/download/${ORCA_UV_VERSION}/uv-${ORCA_UV_ARCH}.${ORCA_UV_EXT}")
            set(ORCA_UV_ARCHIVE "${ORCA_UV_DOWNLOAD_DIR}/uv-archive.${ORCA_UV_EXT}")
            file(MAKE_DIRECTORY "${ORCA_UV_DOWNLOAD_DIR}")
            message(STATUS "Downloading uv ${ORCA_UV_VERSION} from ${ORCA_UV_URL} ...")
            file(DOWNLOAD "${ORCA_UV_URL}" "${ORCA_UV_ARCHIVE}" STATUS ORCA_UV_DL_STATUS TLS_VERIFY ON)
            list(GET ORCA_UV_DL_STATUS 0 ORCA_UV_DL_CODE)
            list(GET ORCA_UV_DL_STATUS 1 ORCA_UV_DL_MSG)
            if(NOT ORCA_UV_DL_CODE EQUAL 0)
                file(REMOVE "${ORCA_UV_ARCHIVE}")
                message(FATAL_ERROR "Failed to download uv: ${ORCA_UV_DL_MSG}")
            else()
                file(SHA256 "${ORCA_UV_ARCHIVE}" _orca_uv_actual_sha256)
                if(NOT _orca_uv_actual_sha256 STREQUAL "${ORCA_UV_SHA256_${ORCA_UV_ARCH}}")
                    file(REMOVE "${ORCA_UV_ARCHIVE}")
                    message(FATAL_ERROR
                        "uv archive checksum mismatch for ${ORCA_UV_ARCH} ${ORCA_UV_VERSION}:\n"
                        "  expected ${ORCA_UV_SHA256_${ORCA_UV_ARCH}}\n"
                        "  actual   ${_orca_uv_actual_sha256}")
                endif()
                file(ARCHIVE_EXTRACT INPUT "${ORCA_UV_ARCHIVE}" DESTINATION "${ORCA_UV_DOWNLOAD_DIR}")
                file(REMOVE "${ORCA_UV_ARCHIVE}")
                if(EXISTS "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}/uv")
                    file(RENAME "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}/uv" "${ORCA_UV_BINARY}")
                    file(REMOVE_RECURSE "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}")
                elseif(EXISTS "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}/uv.exe")
                    file(RENAME "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}/uv.exe" "${ORCA_UV_BINARY}")
                    file(REMOVE_RECURSE "${ORCA_UV_DOWNLOAD_DIR}/uv-${ORCA_UV_ARCH}")
                endif()
            endif()
        endif()
    endif()
    if(EXISTS "${ORCA_UV_BINARY}")
        set(ORCA_BUNDLED_UV_EXECUTABLE "${ORCA_UV_BINARY}")
    endif()
endif()

if(ORCA_BUNDLED_UV_EXECUTABLE)
    if(EXISTS "${ORCA_BUNDLED_UV_EXECUTABLE}")
        file(TO_CMAKE_PATH "${ORCA_BUNDLED_UV_EXECUTABLE}" ORCA_BUNDLED_UV_EXECUTABLE_CONFIG)
        get_filename_component(ORCA_BUNDLED_UV_FILENAME "${ORCA_BUNDLED_UV_EXECUTABLE}" NAME)
    else()
        message(WARNING "ORCA_BUNDLED_UV_EXECUTABLE does not exist: ${ORCA_BUNDLED_UV_EXECUTABLE}")
        set(ORCA_BUNDLED_UV_EXECUTABLE "")
        set(ORCA_BUNDLED_UV_EXECUTABLE_CONFIG "")
        set(ORCA_BUNDLED_UV_FILENAME "")
    endif()
else()
    set(ORCA_BUNDLED_UV_EXECUTABLE_CONFIG "")
    set(ORCA_BUNDLED_UV_FILENAME "")
endif()

# --- Bundled CPython 3.12.13 --------------------------------------------------
set(_bundled_python_version "3.12.13")
string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\..*$" "\\1\\2" _bundled_python_abi "${_bundled_python_version}")
string(REGEX REPLACE "^([0-9]+\\.[0-9]+)\\..*$" "\\1" _bundled_python_version_short "${_bundled_python_version}")
set(_bundled_python_root "${CMAKE_PREFIX_PATH}/libpython")
set(Python3_ROOT_DIR "${_bundled_python_root}" CACHE PATH "Root directory for bundled Python" FORCE)
set(Python3_USE_STATIC_LIBS OFF)
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_IMPLEMENTATIONS CPython)

if(WIN32)
    set(_bundled_python_executable "${_bundled_python_root}/python.exe")
    set(_bundled_python_library "${_bundled_python_root}/libs/python${_bundled_python_abi}.lib")
    if(EXISTS "${_bundled_python_root}/python_d.exe")
        set(_bundled_python_executable "${_bundled_python_root}/python_d.exe")
    endif()
    if(EXISTS "${_bundled_python_root}/libs/python${_bundled_python_abi}_d.lib")
        set(_bundled_python_library "${_bundled_python_root}/libs/python${_bundled_python_abi}_d.lib")
    endif()

    set(Python3_FIND_REGISTRY NEVER)
    set(Python3_EXECUTABLE "${_bundled_python_executable}" CACHE FILEPATH "Bundled Python executable" FORCE)
    set(Python3_INCLUDE_DIR "${_bundled_python_root}/include" CACHE PATH "Bundled Python include directory" FORCE)
    set(Python3_LIBRARY "${_bundled_python_library}" CACHE FILEPATH "Bundled Python embed import library" FORCE)
elseif(APPLE)
    set(Python3_FIND_FRAMEWORK NEVER)

    find_program(_bundled_python_executable
        NAMES python${_bundled_python_version_short} python3
        PATHS "${_bundled_python_root}/bin"
        NO_DEFAULT_PATH
    )
    find_path(_bundled_python_include_dir
        NAMES Python.h
        PATHS
            "${_bundled_python_root}/include/python${_bundled_python_version_short}"
            "${_bundled_python_root}/include"
        NO_DEFAULT_PATH
    )
    find_library(_bundled_python_library
        NAMES python${_bundled_python_version_short} libpython${_bundled_python_version_short}
        PATHS "${_bundled_python_root}/lib"
        NO_DEFAULT_PATH
    )

    if(NOT _bundled_python_executable)
        message(FATAL_ERROR "Bundled Python executable not found under ${_bundled_python_root}/bin")
    endif()
    if(NOT _bundled_python_include_dir)
        message(FATAL_ERROR "Bundled Python headers not found under ${_bundled_python_root}/include")
    endif()
    if(NOT _bundled_python_library)
        message(FATAL_ERROR "Bundled Python library not found under ${_bundled_python_root}/lib")
    endif()

    set(Python3_EXECUTABLE "${_bundled_python_executable}" CACHE FILEPATH "Bundled Python executable" FORCE)
    set(Python3_INCLUDE_DIR "${_bundled_python_include_dir}" CACHE PATH "Bundled Python include directory" FORCE)
    set(Python3_LIBRARY "${_bundled_python_library}" CACHE FILEPATH "Bundled Python embed library" FORCE)
endif()

find_package(Python3 ${_bundled_python_version} EXACT REQUIRED
    COMPONENTS Interpreter Development.Embed
)

# --- pybind11 3.0.1 (vendored headers) ----------------------------------------
set(PYBIND11_SOURCE_DIR "${CMAKE_SOURCE_DIR}/deps_src/pybind11")
if(NOT EXISTS "${PYBIND11_SOURCE_DIR}/include/pybind11/pybind11.h")
    message(FATAL_ERROR "pybind11 headers not found in ${PYBIND11_SOURCE_DIR}.")
endif()

add_library(pybind11_headers INTERFACE)
target_include_directories(pybind11_headers INTERFACE "${PYBIND11_SOURCE_DIR}/include")
add_library(pybind11::headers ALIAS pybind11_headers)
add_library(pybind11::pybind11 ALIAS pybind11_headers)

add_library(pybind11_embed INTERFACE)
target_link_libraries(pybind11_embed INTERFACE pybind11_headers Python3::Python)
target_compile_definitions(pybind11_embed INTERFACE PYBIND11_SIMPLE_GIL_MANAGEMENT)
add_library(pybind11::embed ALIAS pybind11_embed)

message(STATUS "EDGE_PYTHON_PLUGINS: CPython ${Python3_VERSION} from ${_bundled_python_root}")
if(ORCA_BUNDLED_UV_EXECUTABLE)
    message(STATUS "EDGE_PYTHON_PLUGINS: uv ${ORCA_UV_VERSION} from ${ORCA_BUNDLED_UV_EXECUTABLE}")
endif()
