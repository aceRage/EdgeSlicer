#ifndef _common_func_hppp_
#define _common_func_hppp_
#include <iostream>


// The product name lives here and nowhere else. version.inc reads these two lines back
// out of this header for CMake, and libslic3r.h derives its own names from them, so the
// three definitions that used to drift apart are now one.
#define SLIC3R_APP_NAME "EdgeSlicer"
#define SLIC3R_APP_KEY "EdgeSlicer"
// The data directories we used to live in, newest first. There have been two renames now
// (Snapmaker_Orca -> UltraOne -> EdgeSlicer), so this is a list rather than a constant and
// the first one that actually exists on disk is the one migrated from: a user coming from
// UltraOne gets their UltraOne data, a user who skipped that release gets their
// Snapmaker_Orca data, and nobody gets the older of two directories they both have.
//
// Read from here by the first-start migration (DataDirMigration.cpp) and by nothing else,
// so a later release drops all of this by emptying the list.
#define SLIC3R_LEGACY_APP_KEYS \
    {                          \
        "UltraOne", "Snapmaker_Orca"  \
    }
// The oldest of them, kept as a single name for the few places that only need to say
// "the directory this fork started out in".
#define SLIC3R_LEGACY_APP_KEY "Snapmaker_Orca"
#define SLIC3R_VERSION "01.10.01.50"
#define Snapmaker_VERSION "2.3.6.5" // the one version number; version.inc reads it back for CMake/CPack
#define MIN_FIRM_VER "1.5.0"
#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "0000000" // 0000000 means uninitialized
#endif
#define SLIC3R_BUILD_ID Snapmaker_VERSION
// #define SLIC3R_RC_VERSION "01.10.01.50"
#define BBL_RELEASE_TO_PUBLIC 1
#define BBL_INTERNAL_TESTING 0
#define ORCA_CHECK_GCODE_PLACEHOLDERS 0

namespace common
{
	// Reads one string out of the "app" section of the app config in the default per-user
	// data directory. For code that runs before AppConfig exists; returns "" when the file
	// or the key is missing.
	std::string get_app_config_string(const std::string& key);

	std::string get_pc_name();

	std::string get_flutter_version();

	std::string get_profile_version();

	std::string getMachineId();

	std::string getLocalArea();

	std::string getLanguage();

    } // namespace common

#endif