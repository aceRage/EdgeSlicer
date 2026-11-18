// Header-only nanosvg: one translation unit in libslic3r defines the implementation so every
// consumer (GUI, tests, profile validator, CLI tools) links nsvgParse / nsvgParseFromFile /
// nsvgDelete. Keep this file alone: the implementation must not share a unity batch or follow a
// libslic3r header that already included nanosvg.h without NANOSVG_IMPLEMENTATION.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
