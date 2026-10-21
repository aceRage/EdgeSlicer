#pragma once

// A sliced job sent from another PC (remote printing, design: research 2b A1): the checks a
// .gcode.3mf must pass before it enters this PC's G-code archive, where the existing Reprint
// path (/api/archive/{id}/preview and /send) picks it up unchanged.
//
// The file arrives through the hub from a client whose token carries the "print" permission -
// trusted to ask for a print, never trusted for what it sends. So, with the same rules the Models
// browser's downloads follow (libslic3r/UntrustedInput.hpp):
//   * the name is sanitized and must end in .gcode.3mf;
//   * the size is capped at untrusted::MODEL_DOWNLOAD_SIZE_LIMIT (500 MB) on arrival;
//   * its first bytes must be a ZIP's;
//   * every entry name must be a plain relative path (no "..", no drive, no absolute path, no
//     symlink entry), there are at most MAX_ENTRIES of them, and together they inflate to at most
//     MAX_UNCOMPRESSED - a zip bomb is refused before anything reads it;
//   * exactly one plate carries G-code (Metadata/plate_<n>.gcode), as a file the send dialog
//     exports does.
// After this the desktop's own reader (BambuReprint::load_job) still has the last word on whether
// it is a sliced Bambu job it can send.
//
// Pure apart from reading the file: no app config, no GUI, any thread.

#include <cstdint>
#include <string>

namespace Slic3r { namespace GUI { namespace ArchiveImport {

constexpr std::uint64_t MAX_UNCOMPRESSED = std::uint64_t(4) * 1024 * 1024 * 1024; // all entries together
constexpr unsigned      MAX_ENTRIES      = 4096;
constexpr std::uint64_t MAX_THUMBNAIL    = 4 * 1024 * 1024;

// The name the file is stored under: sanitize_download_filename(), and it must end in
// ".gcode.3mf" (any case). "" when it cannot be used.
std::string clean_name(const std::string& name);

struct Package
{
    std::string error;          // non-empty: refuse the file, for the person who sent it
    int         plate { -1 };   // 1-based plate number that carries the G-code
    std::string thumbnail_png;  // Metadata/plate_<n>.png when the file has one (<= MAX_THUMBNAIL)
    std::uint64_t uncompressed { 0 };
    unsigned    entries { 0 };
};

// Opens `path` (UTF-8) as a ZIP and checks it as above. Never throws.
Package check_package(const std::string& path);

}}} // namespace Slic3r::GUI::ArchiveImport
