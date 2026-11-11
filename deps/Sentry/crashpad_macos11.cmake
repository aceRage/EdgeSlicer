# Lets crashpad (inside sentry-native) build for macOS 11. Run from Sentry.cmake's PATCH_COMMAND:
#   cmake -DCRASHPAD_DIR=<sentry source>/external/crashpad -P crashpad_macos11.cmake
#
# crashpad's MacModelAndBoard() passes kIOMainPortDefault to IOServiceGetMatchingService. That
# constant only exists from macOS 12 (IOKitLib.h: API_AVAILABLE(macos(12.0))), and IOKitLib.h
# documents it as "a synonym for NULL", i.e. MACH_PORT_NULL, which every macOS version accepts as
# "use the default main port". Passing MACH_PORT_NULL keeps the behaviour and drops the macOS 12
# symbol, so libsentry.dylib and crashpad_handler can be built for the app's own target.
#
# Safe to run again on an already patched tree. Fails if the line is gone, so a sentry-native
# bump that changes it is noticed rather than silently building something else.

if (NOT CRASHPAD_DIR)
  message(FATAL_ERROR "crashpad_macos11.cmake: pass -DCRASHPAD_DIR=<sentry source>/external/crashpad")
endif ()

set(_file "${CRASHPAD_DIR}/util/mac/mac_util.cc")
set(_old "IOServiceGetMatchingService(kIOMainPortDefault,")
set(_new "IOServiceGetMatchingService(MACH_PORT_NULL,")

if (NOT EXISTS "${_file}")
  message(FATAL_ERROR "crashpad_macos11.cmake: ${_file} not found")
endif ()
file(READ "${_file}" _src)

string(FIND "${_src}" "${_old}" _at)
if (_at EQUAL -1)
  string(FIND "${_src}" "${_new}" _done)
  if (_done EQUAL -1)
    message(FATAL_ERROR "crashpad_macos11.cmake: the kIOMainPortDefault call in ${_file} has changed; check the patch")
  endif ()
  message(STATUS "crashpad_macos11.cmake: already patched")
  return()
endif ()

string(REPLACE "${_old}" "${_new}" _src "${_src}")
file(WRITE "${_file}" "${_src}")
message(STATUS "crashpad_macos11.cmake: kIOMainPortDefault -> MACH_PORT_NULL in util/mac/mac_util.cc")
