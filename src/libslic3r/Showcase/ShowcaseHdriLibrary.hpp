#ifndef slic3r_Showcase_ShowcaseHdriLibrary_hpp_
#define slic3r_Showcase_ShowcaseHdriLibrary_hpp_

// Showcase realism 1h-3 (tests/design_showcase_realism.md 2.8): more HDRI environments than the
// bundled ones.
//
//   * Downloads: a catalogue (resources/showcase/hdri/hdri_catalogue.json) of Poly Haven CC0 panoramas
//     the owner chose, fetched on request from OUR OWN mirror (decision D-R3), never from the Poly
//     Haven API. Every download is checked against the catalogue's size and SHA-256 before it is kept;
//     an entry whose checksum is not published yet cannot be downloaded at all.
//   * The user's own HDRI (.hdr, or a scanline .exr): read, checked, made at most 4096 px wide and kept
//     as a Radiance .hdr in the data folder, listed in a small manifest next to it.
//
// Both live in <data folder>/showcase/hdri and appear in the Environment lists like the bundled ones.
// GL- and wxWidgets-free (the GUI downloads, copies and shows them; the unit tests check the rules).

#include "ShowcaseHdr.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// The six catalogue files (hdri_catalogue.json, "download.file") are release assets under this URL in
// the edgeslicer-deps mirror (release showcase-hdri-1, published 2026-11-02). An entry without both
// "size" and "sha256" shows as "not available yet" and downloads nothing.
inline const char *HDRI_MIRROR_BASE_URL = "https://github.com/aceRage/edgeslicer-deps/releases/download/showcase-hdri-1/";
// An environment variable that replaces the base URL (a local mock server for tests, or a staging mirror).
inline const char *HDRI_MIRROR_ENV      = "EDGESLICER_HDRI_MIRROR";
// The catalogue next to the bundled manifest (HDRI_DIR in the resources).
inline const char *HDRI_CATALOGUE       = "hdri_catalogue.json";
// In the data folder: downloads and the user's own HDRIs, and the manifest of the latter.
inline const char *HDRI_DATA_DIR        = "showcase/hdri";
inline const char *USER_HDRI_MANIFEST   = "user_hdri.json";
// The user's HDRIs are stored at most this wide (a 4k panorama is ~200 MB as floats while it is baked).
inline constexpr int USER_HDRI_MAX_WIDTH = 4096;
// A download larger than this is refused before it starts (the 2k catalogue files are 5-8 MB).
inline constexpr uint64_t HDRI_DOWNLOAD_MAX_BYTES = 64ull << 20;

// The base URL: `env_value` (the environment variable's value) when set, else HDRI_MIRROR_BASE_URL;
// always ending in '/'.
std::string hdri_mirror_base(const char *env_value);
// base + the entry's mirror file name.
std::string hdri_download_url(const std::string &base, const HdriInfo &hdri);
// The entry can be downloaded: it is in the catalogue with a published size and checksum.
bool        hdri_download_published(const HdriInfo &hdri);

enum class DownloadCheck : int { Ok = 0, NotPublished, Empty, SizeMismatch, HashMismatch, TooLarge, NotAnHdri };
const char *to_string(DownloadCheck v);
// Checks a finished download against the catalogue entry: size, SHA-256, and that it reads as a panorama.
DownloadCheck verify_hdri_download(const uint8_t *data, size_t size, const HdriInfo &hdri);
// SHA-256 of a buffer, 64 lower-case hex digits.
std::string sha256_hex(const uint8_t *data, size_t size);

// The catalogue: every entry must be downloadable (others are dropped). Never throws.
bool parse_hdri_catalogue(const std::string &text, HdriManifest &out, std::string *error = nullptr);

// --- the user's own HDRIs ------------------------------------------------------------------------

// A safe file stem from a file name: "My Garage (2).EXR" -> "my_garage_2"; "" -> "hdri".
std::string user_hdri_slug(const std::string &file_name);
// The id of a new user HDRI: "user_" + slug, with "_2", "_3" ... when taken.
std::string unique_user_hdri_id(const std::string &slug, const std::vector<HdriInfo> &existing);
// The display name from a file name: the stem, as typed ("My Garage (2)").
std::string user_hdri_display_name(const std::string &file_name);
// Reads the user's file (Radiance or OpenEXR), checks it holds light, and returns it at most
// USER_HDRI_MAX_WIDTH wide, ready to be kept as a .hdr (encode_rgbe). False with `error` otherwise.
bool prepare_user_hdri(const uint8_t *data, size_t size, HdrImage &out, std::string *error = nullptr);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseHdriLibrary_hpp_
