#include "ShowcaseHdriLibrary.hpp"

#include "../ImageFill.hpp"   // image_fill_sha256_hex

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Slic3r {
namespace Showcase {

std::string hdri_mirror_base(const char *env_value)
{
    std::string base = env_value != nullptr && *env_value != '\0' ? std::string(env_value) : std::string(HDRI_MIRROR_BASE_URL);
    if (!base.empty() && base.back() != '/')
        base.push_back('/');
    return base;
}

std::string hdri_download_url(const std::string &base, const HdriInfo &hdri)
{
    const std::string &file = hdri.download_file.empty() ? hdri.file : hdri.download_file;
    if (file.empty())
        return std::string();
    return base + file;
}

bool hdri_download_published(const HdriInfo &hdri) { return hdri.downloadable && hdri.download_size > 0 && hdri.sha256.size() == 64; }

const char *to_string(DownloadCheck v)
{
    switch (v) {
    case DownloadCheck::Ok: return "ok";
    case DownloadCheck::NotPublished: return "the catalogue has no size and checksum for it yet";
    case DownloadCheck::Empty: return "nothing was downloaded";
    case DownloadCheck::SizeMismatch: return "the download has the wrong size";
    case DownloadCheck::HashMismatch: return "the download's checksum does not match the catalogue";
    case DownloadCheck::TooLarge: return "the download is larger than allowed";
    case DownloadCheck::NotAnHdri: return "the download is not a panorama this version can read";
    }
    return "unknown";
}

std::string sha256_hex(const uint8_t *data, size_t size) { return image_fill_sha256_hex(data, size); }

DownloadCheck verify_hdri_download(const uint8_t *data, size_t size, const HdriInfo &hdri)
{
    if (!hdri_download_published(hdri))
        return DownloadCheck::NotPublished;
    if (data == nullptr || size == 0)
        return DownloadCheck::Empty;
    if (uint64_t(size) > HDRI_DOWNLOAD_MAX_BYTES)
        return DownloadCheck::TooLarge;
    if (uint64_t(size) != hdri.download_size)
        return DownloadCheck::SizeMismatch;
    if (sha256_hex(data, size) != hdri.sha256)
        return DownloadCheck::HashMismatch;
    // Last, the costly one: it must read as a panorama.
    HdrImage image;
    if (!decode_hdri_any(data, size, image) || image.empty())
        return DownloadCheck::NotAnHdri;
    return DownloadCheck::Ok;
}

bool parse_hdri_catalogue(const std::string &text, HdriManifest &out, std::string *error)
{
    HdriManifest all;
    if (!parse_hdri_manifest(text, all, error)) {
        out = HdriManifest();
        return false;
    }
    HdriManifest m;
    for (HdriInfo &h : all.hdris)
        if (h.downloadable && !h.procedural)
            m.hdris.push_back(std::move(h));
    out = std::move(m);
    return true;
}

namespace {
std::string hdri_stem(const std::string &file_name)
{
    std::string name = file_name;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name = name.substr(0, dot);
    return name;
}
} // namespace

std::string user_hdri_slug(const std::string &file_name)
{
    std::string slug;
    bool        sep = false;
    for (unsigned char c : hdri_stem(file_name)) {
        if (std::isalnum(c) && c < 128) {
            if (sep && !slug.empty())
                slug.push_back('_');
            slug.push_back(char(std::tolower(c)));
            sep = false;
        } else
            sep = true;
        if (slug.size() >= 48)
            break;
    }
    return slug.empty() ? std::string("hdri") : slug;
}

std::string unique_user_hdri_id(const std::string &slug, const std::vector<HdriInfo> &existing)
{
    auto taken = [&existing](const std::string &id) {
        return std::any_of(existing.begin(), existing.end(), [&id](const HdriInfo &h) { return h.id == id; });
    };
    const std::string base = "user_" + (slug.empty() ? std::string("hdri") : slug);
    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const std::string id = base + "_" + std::to_string(n);
        if (!taken(id))
            return id;
    }
}

std::string user_hdri_display_name(const std::string &file_name)
{
    const std::string stem = hdri_stem(file_name);
    return stem.empty() ? std::string("HDRI") : stem;
}

bool prepare_user_hdri(const uint8_t *data, size_t size, HdrImage &out, std::string *error)
{
    out = HdrImage();
    HdrImage image;
    if (!decode_hdri_any(data, size, image, error))
        return false;
    if (image.width < 2 * image.height - 2 || image.width > 2 * image.height + 2) {
        // Not 2:1: still usable (the mapping stretches it), but most likely not a panorama.
        if (image.width < image.height) {
            if (error != nullptr)
                *error = "the picture is not a 2:1 equirectangular panorama";
            return false;
        }
    }
    double peak = 0.;
    for (size_t i = 0; i + 2 < image.rgb.size(); i += 3)
        peak = std::max(peak, double(0.2126f * image.rgb[i] + 0.7152f * image.rgb[i + 1] + 0.0722f * image.rgb[i + 2]));
    if (!(peak > 0.)) {
        if (error != nullptr)
            *error = "the panorama is black: it has no light";
        return false;
    }
    out = downscale_hdr(image, USER_HDRI_MAX_WIDTH);
    return !out.empty();
}

} // namespace Showcase
} // namespace Slic3r
