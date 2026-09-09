#ifndef slic3r_LoginUserAgent_hpp_
#define slic3r_LoginUserAgent_hpp_

#include <string>

namespace Slic3r {

// The Bambu Studio version that bambulab.com currently gates its in-slicer login flavour
// on. bambulab.com/sign-in parses the BBL-Slicer/v<version> token out of the webview's
// User-Agent: below the gate it serves the retired legacy /sign-in/callback flow (which
// 404s), at or above it the live studio-callback + ticket flow that posts the ticket back
// to our loopback server.
//
// !! This constant MUST track the Bambu Studio version bambulab.com gates on. It is NOT
// our own SLIC3R_VERSION: EdgeSlicer's version (01.10.x / 02.03.x) is unrelated to and
// far below whatever Bambu ships, so reporting SLIC3R_VERSION here fails the gate. When
// third-party (Google) sign-in starts 404ing again, bump this to the current Bambu Studio
// SLIC3R_VERSION (see the reference clone's version.inc) before looking anywhere else.
// Current reference: BambuStudio 66e405477 (2026-08-31), SLIC3R_VERSION 02.08.02.61.
static const char *const BBL_LOGIN_UA_VERSION = "02.08.02.61";

enum class LoginUAPlatform { Windows, MacOS, Linux };

// Build the User-Agent for the Bambu login webview, byte-for-byte in Bambu Studio's own
// token order: the Mozilla/browser prefix first, then BBL-Slicer/v<ver> (<theme>) and
// BBL-Language/<lang>. The fork used to put BBL-Slicer FIRST, which some of bambulab's
// UA parsing does not accept, and omitted BBL-Language entirely.
//
// `brand_tag` keeps the fork's ability to advertise SM-Slicer on non-login webviews; only
// the "BBL-Slicer" tag gets BBL_LOGIN_UA_VERSION, everything else reports slicer_version.
std::string bbl_login_user_agent(LoginUAPlatform platform,
                                 bool            dark,
                                 const std::string &language_code,
                                 const std::string &brand_tag      = "BBL-Slicer",
                                 const std::string &slicer_version = std::string());

} // namespace Slic3r

#endif
