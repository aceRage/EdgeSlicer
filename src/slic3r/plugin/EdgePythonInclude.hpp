#ifndef slic3r_EdgePythonInclude_hpp_
#define slic3r_EdgePythonInclude_hpp_

// wxWidgets' Windows setup.h (lib/vc_x64_lib/mswu/wx/setup.h) does `#define pid_t int`.
// The libslic3r_gui PCH includes wx first, so a later Python.h `typedef int pid_t;`
// becomes `typedef int int;` (MSVC C2632). CMake-generated wx setup.h has also
// historically `#define`d mode_t and off_t the same way. Undef those macros before
// Python.h. Do not restore them: Python's typedefs are the types the rest of the TU
// should see, and wx does not use pid_t on MSW.

#if defined(_WIN32)
#    if defined(pid_t)
#        undef pid_t
#    endif
#    if defined(mode_t)
#        undef mode_t
#    endif
#    if defined(off_t)
#        undef off_t
#    endif
#    if defined(uid_t)
#        undef uid_t
#    endif
#    if defined(gid_t)
#        undef gid_t
#    endif
#    if defined(ssize_t)
#        undef ssize_t
#    endif
#endif

// Via pybind11 so this TU requests the same python3xx.lib as everything else.
#include <pybind11/conduit/wrap_include_python_h.h>

#endif // slic3r_EdgePythonInclude_hpp_
