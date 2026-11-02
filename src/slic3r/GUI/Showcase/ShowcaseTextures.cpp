#include <glad/gl.h>

#include "ShowcaseTextures.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include "../3DScene.hpp"   // glsafe
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../Plater.hpp"

#include <wx/image.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <sstream>

namespace Slic3r {
namespace GUI {

namespace {

std::string showcase_read_text(const std::string &path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string showcase_asset_path(const char *dir, const std::string &file)
{
    return (boost::filesystem::path(resources_dir()) / dir / file).string();
}

} // namespace

const Showcase::EnvironmentManifest &showcase_environments()
{
    static Showcase::EnvironmentManifest manifest;
    static bool                          loaded = false;
    if (!loaded) {
        loaded = true;
        std::string error;
        const std::string path = showcase_asset_path(Showcase::ENVIRONMENTS_DIR, Showcase::ENVIRONMENTS_MANIFEST);
        if (!Showcase::parse_environment_manifest(showcase_read_text(path), manifest, &error))
            BOOST_LOG_TRIVIAL(warning) << "Showcase: no environments (" << path << ": " << error << ")";
    }
    return manifest;
}

const Showcase::FloorManifest &showcase_floors()
{
    static Showcase::FloorManifest manifest;
    static bool                    loaded = false;
    if (!loaded) {
        loaded = true;
        std::string error;
        const std::string path = showcase_asset_path(Showcase::FLOORS_DIR, Showcase::FLOORS_MANIFEST);
        if (!Showcase::parse_floor_manifest(showcase_read_text(path), manifest, &error))
            BOOST_LOG_TRIVIAL(warning) << "Showcase: no photo floors (" << path << ": " << error << ")";
    }
    return manifest;
}

namespace {
Showcase::HdriManifest g_hdris;
bool                   g_hdris_loaded     = false;
unsigned int           g_hdris_generation = 1;
} // namespace

std::string showcase_hdri_data_dir() { return (boost::filesystem::path(data_dir()) / Showcase::HDRI_DATA_DIR).string(); }

const Showcase::HdriManifest &showcase_hdri_catalogue()
{
    static Showcase::HdriManifest catalogue;
    static bool                   loaded = false;
    if (!loaded) {
        loaded = true;
        std::string       error;
        const std::string path = showcase_asset_path(Showcase::HDRI_DIR, Showcase::HDRI_CATALOGUE);
        if (!Showcase::parse_hdri_catalogue(showcase_read_text(path), catalogue, &error))
            BOOST_LOG_TRIVIAL(warning) << "Showcase: no HDRI download catalogue (" << path << ": " << error << ")";
    }
    return catalogue;
}

bool showcase_hdri_installed(const Showcase::HdriInfo &hdri)
{
    if (!hdri.downloadable || hdri.file.empty())
        return false;
    boost::system::error_code ec;
    return boost::filesystem::is_regular_file(boost::filesystem::path(showcase_hdri_data_dir()) / hdri.file, ec);
}

std::vector<Showcase::HdriInfo> showcase_user_hdris()
{
    std::vector<Showcase::HdriInfo> out;
    const boost::filesystem::path dir = showcase_hdri_data_dir();
    const std::string             text = showcase_read_text((dir / Showcase::USER_HDRI_MANIFEST).string());
    if (text.empty())
        return out;
    Showcase::HdriManifest m;
    std::string            error;
    if (!Showcase::parse_hdri_manifest(text, m, &error)) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the list of your own HDRIs could not be read (" << error << ")";
        return out;
    }
    for (Showcase::HdriInfo &h : m.hdris) {
        if (h.procedural || h.downloadable)
            continue;
        h.user     = true;
        h.base_dir = dir.string();
        out.push_back(std::move(h));
    }
    return out;
}

bool showcase_save_user_hdris(const std::vector<Showcase::HdriInfo> &hdris)
{
    const boost::filesystem::path dir = showcase_hdri_data_dir();
    boost::system::error_code     ec;
    boost::filesystem::create_directories(dir, ec);
    const boost::filesystem::path path = dir / Showcase::USER_HDRI_MANIFEST;
    const boost::filesystem::path tmp  = dir / (std::string(Showcase::USER_HDRI_MANIFEST) + ".tmp");
    {
        boost::nowide::ofstream f(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << Showcase::hdri_manifest_to_json(hdris);
        if (!f)
            return false;
    }
    boost::filesystem::rename(tmp, path, ec);
    return !ec;
}

void showcase_hdris_reload()
{
    g_hdris_loaded = false;
    ++g_hdris_generation;
}

unsigned int showcase_hdris_generation() { return g_hdris_generation; }

const Showcase::HdriManifest &showcase_hdris()
{
    if (!g_hdris_loaded) {
        g_hdris_loaded = true;
        g_hdris        = Showcase::HdriManifest();
        std::string error;
        const std::string path = showcase_asset_path(Showcase::HDRI_DIR, Showcase::HDRI_MANIFEST);
        if (!Showcase::parse_hdri_manifest(showcase_read_text(path), g_hdris, &error))
            BOOST_LOG_TRIVIAL(warning) << "Showcase: no HDRI environments (" << path << ": " << error << ")";
        // 1h-3: the downloaded catalogue entries, then the user's own (ids never repeat a bundled one).
        const std::string data = showcase_hdri_data_dir();
        for (const Showcase::HdriInfo &h : showcase_hdri_catalogue().hdris)
            if (g_hdris.find(h.id) == nullptr && showcase_hdri_installed(h)) {
                Showcase::HdriInfo installed = h;
                installed.base_dir           = data;
                g_hdris.hdris.push_back(std::move(installed));
            }
        for (Showcase::HdriInfo &h : showcase_user_hdris())
            if (g_hdris.find(h.id) == nullptr)
                g_hdris.hdris.push_back(std::move(h));
    }
    return g_hdris;
}

std::string showcase_hdri_path(const Showcase::HdriInfo &hdri)
{
    if (hdri.procedural)
        return std::string();
    if (!hdri.base_dir.empty())
        return (boost::filesystem::path(hdri.base_dir) / hdri.file).string();
    return showcase_asset_path(Showcase::HDRI_DIR, hdri.file);
}

wxString showcase_hdri_name(const Showcase::HdriInfo &hdri)
{
    if (hdri.id == "hdri_studio") return _L("Studio softbox (HDRI)");
    if (hdri.id == "hdri_outdoor") return _L("Outdoor sky with sun (HDRI)");
    if (hdri.id == "hdri_workshop") return _L("Workshop window (HDRI)");
    if (hdri.id == "hdri_loft") return _L("Loft studio (HDRI)");
    if (hdri.id == "hdri_sunset") return _L("Golden hour (HDRI)");
    // 1h-3: the download catalogue.
    if (hdri.id == "hdri_brown_studio") return _L("Brown photo studio (HDRI)");
    if (hdri.id == "hdri_blue_studio") return _L("Blue photo studio (HDRI)");
    if (hdri.id == "hdri_industrial_sunset") return _L("Industrial sunset sky (HDRI)");
    if (hdri.id == "hdri_kiara_dawn") return _L("Dawn in the park (HDRI)");
    if (hdri.id == "hdri_warehouse") return _L("Empty warehouse (HDRI)");
    if (hdri.id == "hdri_abandoned_workshop") return _L("Abandoned workshop (HDRI)");
    if (hdri.user)
        return wxString::Format(_L("Mine: %s"), wxString::FromUTF8(hdri.name));
    return wxString::FromUTF8(hdri.name);
}

std::string showcase_environment_path(const Showcase::EnvironmentInfo &env) { return showcase_asset_path(Showcase::ENVIRONMENTS_DIR, env.file); }
std::string showcase_floor_path(const Showcase::FloorTextureInfo &floor) { return showcase_asset_path(Showcase::FLOORS_DIR, floor.file); }

wxString showcase_environment_name(const Showcase::EnvironmentInfo &env)
{
    if (env.id == "studio") return _L("Studio softbox");
    if (env.id == "workshop") return _L("Workshop window");
    if (env.id == "dark_studio") return _L("Dark studio");
    if (env.id == "outdoor") return _L("Outdoor sky");
    if (env.id == "overcast") return _L("Overcast");
    if (env.id == "sunset") return _L("Sunset");
    return wxString::FromUTF8(env.name);
}

wxString showcase_floor_name(const Showcase::FloorTextureInfo &floor)
{
    if (floor.id == "oak") return _L("Oak");
    if (floor.id == "walnut") return _L("Walnut");
    if (floor.id == "concrete") return _L("Concrete");
    if (floor.id == "marble") return _L("Marble");
    if (floor.id == "linen") return _L("Linen");
    if (floor.id == "slate") return _L("Slate");
    return wxString::FromUTF8(floor.name);
}

wxString showcase_finish_name(Showcase::Finish finish)
{
    switch (finish) {
    case Showcase::Finish::Matte: return _L("Matte");
    case Showcase::Finish::Silk: return _L("Silk");
    case Showcase::Finish::Gloss: return _L("Gloss");
    case Showcase::Finish::Transparent: return _L("Transparent");
    case Showcase::Finish::Metallic: return _L("Metallic");
    case Showcase::Finish::CarbonFiber: return _L("Carbon / glass fibre");
    case Showcase::Finish::Translucent: return _L("Translucent");
    case Showcase::Finish::Satin: return _L("Satin");
    case Showcase::Finish::Sparkle: return _L("Sparkle / galaxy");
    case Showcase::Finish::Marble: return _L("Marble / stone fill");
    case Showcase::Finish::Wood: return _L("Wood fill");
    case Showcase::Finish::MatteTextured: return _L("Textured matte");
    default: return _L("Standard");
    }
}

bool showcase_load_rgba_file(const std::string &path, ShowcaseImage &out)
{
    out = ShowcaseImage();
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(path), wxBITMAP_TYPE_ANY) || !image.IsOk())
        return false;
    const int w = image.GetWidth(), h = image.GetHeight();
    out.width  = w;
    out.height = h;
    out.rgba.resize(size_t(w) * size_t(h) * 4);
    const unsigned char *rgb   = image.GetData();
    const unsigned char *alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        out.rgba[i * 4 + 0] = rgb[i * 3 + 0];
        out.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        out.rgba[i * 4 + 2] = rgb[i * 3 + 2];
        out.rgba[i * 4 + 3] = alpha ? alpha[i] : 255;
    }
    return !out.empty();
}

unsigned int showcase_upload_texture(const ShowcaseImage &image, bool repeat)
{
    if (image.empty())
        return 0;
    GLuint tex = 0;
    glsafe(::glGenTextures(1, &tex));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data()));
    glsafe(::glGenerateMipmap(GL_TEXTURE_2D));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    return tex;
}

unsigned int showcase_upload_environment(const ShowcaseImage &atlas, int face_size, int levels)
{
    if (atlas.empty() || face_size <= 0 || levels <= 0 || atlas.width < Showcase::env_atlas_width(face_size) ||
        atlas.height < Showcase::env_atlas_height(face_size, levels)) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: an environment atlas is " << atlas.width << "x" << atlas.height << ", not the manifest's layout";
        return 0;
    }
    GLuint tex = 0;
    glsafe(::glGenTextures(1, &tex));
    glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, tex));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    std::vector<uint8_t> face;
    for (int level = 0; level < levels; ++level)
        for (int f = 0; f < 6; ++f) {
            const Showcase::RectI r = Showcase::env_atlas_face(face_size, levels, level, f);
            face.resize(size_t(r.w) * size_t(r.h) * 4);
            for (int y = 0; y < r.h; ++y)
                std::copy_n(atlas.rgba.data() + (size_t(r.y + y) * size_t(atlas.width) + size_t(r.x)) * 4, size_t(r.w) * 4,
                            face.data() + size_t(y) * size_t(r.w) * 4);
            glsafe(::glTexImage2D(GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), level, GL_RGBA8, r.w, r.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                  face.data()));
        }
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, levels - 1));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
    // Core since 3.2: filter across the faces' edges (no seams in the blurry levels).
    glsafe(::glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS));
    return tex;
}

unsigned int showcase_upload_equirect(const Showcase::HdrImage &image, float scale)
{
    if (image.empty() || image.width > 16384)
        return 0;
    GLint max_size = 0;
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size));
    if (image.width > max_size || image.height > max_size) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: an HDRI of " << image.width << "x" << image.height << " is over this GPU's texture limit (" << max_size << ")";
        return 0;
    }
    // RGBA floats; a half float tops out at 65504 (a sun disc in a photograph is well over that).
    std::vector<float> rgba(size_t(image.width) * size_t(image.height) * 4);
    for (size_t i = 0; i < size_t(image.width) * size_t(image.height); ++i) {
        for (size_t c = 0; c < 3; ++c)
            rgba[i * 4 + c] = std::min(image.rgb[i * 3 + c] * scale, 60000.f);
        rgba[i * 4 + 3] = 1.f;
    }
    GLuint tex = 0;
    glsafe(::glGenTextures(1, &tex));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, image.width, image.height, 0, GL_RGBA, GL_FLOAT, rgba.data()));
    glsafe(::glGenerateMipmap(GL_TEXTURE_2D));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    return tex;
}

std::vector<ShowcaseFilament> showcase_filaments()
{
    std::vector<ShowcaseFilament> out;
    Plater       *plater = wxGetApp().plater();
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (plater == nullptr || bundle == nullptr)
        return out;
    const std::vector<std::string> colors = plater->get_extruder_colors_from_plater_config(nullptr, false);
    const size_t count = std::max(colors.size(), bundle->filament_presets.size());
    for (size_t i = 0; i < count; ++i) {
        ShowcaseFilament f;
        f.slot  = int(i) + 1;
        f.color = i < colors.size() ? colors[i] : std::string("#FFFFFF");
        if (i < bundle->filament_presets.size()) {
            f.preset_name = bundle->filament_presets[i];
            if (const Preset *p = bundle->filaments.find_preset(f.preset_name, false); p != nullptr && p->config.has("filament_type"))
                f.type = p->config.opt_string("filament_type", 0u);
        }
        f.inferred = Showcase::infer_finish(f.preset_name, f.type);
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<ShowcaseFilament> showcase_mixed_filaments()
{
    std::vector<ShowcaseFilament> out;
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return out;
    // The slicer numbers the virtual filaments after the physical ones, in list order.
    int slot = std::max(wxGetApp().filaments_cnt(), 0);
    for (const MixedFilament &mf : bundle->mixed_filaments.mixed_filaments()) {
        if (!mf.enabled || mf.deleted)
            continue;
        ShowcaseFilament f;
        f.slot        = ++slot;
        f.mixed       = true;
        f.color       = mf.display_color.empty() ? std::string("#808080") : mf.display_color;
        const int b   = std::clamp(mf.mix_b_percent, 0, 100);
        f.mixed_label = "F" + std::to_string(mf.component_a) + " " + std::to_string(100 - b) + "%+F" + std::to_string(mf.component_b) + " " +
                        std::to_string(b) + "%";
        f.inferred    = Showcase::Finish::Standard;
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace GUI
} // namespace Slic3r
