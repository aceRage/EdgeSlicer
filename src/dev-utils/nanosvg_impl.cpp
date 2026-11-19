// libslic3r parses SVG with nanosvg but only the GUI library defines its implementation, so a tool that
// links libslic3r alone (the profile validator) needs one translation unit that does.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
