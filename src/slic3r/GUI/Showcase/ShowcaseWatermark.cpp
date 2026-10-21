#include "ShowcaseWatermark.hpp"

#include "libslic3r/ImageFill.hpp"   // image_fill_sha256_hex
#include "libslic3r/Utils.hpp"       // data_dir

#include <wx/font.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/wfstream.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>

namespace Slic3r {
namespace GUI {

namespace {

void showcase_wx_to_rgba(const wxImage &image, ShowcaseImage &out)
{
    out.width  = image.GetWidth();
    out.height = image.GetHeight();
    out.rgba.assign(size_t(out.width) * size_t(out.height) * 4, 0);
    const unsigned char *rgb   = image.GetData();
    const unsigned char *alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    const bool           mask  = !image.HasAlpha() && image.HasMask();
    const unsigned char  mr = mask ? image.GetMaskRed() : 0, mg = mask ? image.GetMaskGreen() : 0, mb = mask ? image.GetMaskBlue() : 0;
    for (size_t i = 0; i < size_t(out.width) * size_t(out.height); ++i) {
        out.rgba[i * 4 + 0] = rgb[i * 3 + 0];
        out.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        out.rgba[i * 4 + 2] = rgb[i * 3 + 2];
        unsigned char a = alpha ? alpha[i] : 255;
        if (mask && rgb[i * 3 + 0] == mr && rgb[i * 3 + 1] == mg && rgb[i * 3 + 2] == mb)
            a = 0;
        out.rgba[i * 4 + 3] = a;
    }
}

wxImage showcase_rgba_to_wx(const ShowcaseImage &in)
{
    wxImage image(in.width, in.height, false);
    image.InitAlpha();
    unsigned char *rgb   = image.GetData();
    unsigned char *alpha = image.GetAlpha();
    for (size_t i = 0; i < size_t(in.width) * size_t(in.height); ++i) {
        rgb[i * 3 + 0] = in.rgba[i * 4 + 0];
        rgb[i * 3 + 1] = in.rgba[i * 4 + 1];
        rgb[i * 3 + 2] = in.rgba[i * 4 + 2];
        alpha[i]       = in.rgba[i * 4 + 3];
    }
    return image;
}

wxFont showcase_watermark_font(const Showcase::Watermark &wm, int font_px)
{
    wxFontInfo info(wxSize(0, std::max(1, font_px)));
    if (!wm.font_face.empty())
        info.FaceName(wxString::FromUTF8(wm.font_face));
    info.Bold(wm.bold);
    wxFont font(info);
    if (!font.IsOk())
        font = wxFont(wxFontInfo(wxSize(0, std::max(1, font_px))).Bold(wm.bold));
    return font;
}

// The size the text takes at that font (through a graphics context, as it is then drawn).
bool showcase_measure_text(const wxString &text, const wxFont &font, int &w, int &h)
{
    w = h = 0;
    if (text.empty())
        return false;
    wxImage probe(1, 1, true);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(probe));
    if (!gc)
        return false;
    gc->SetFont(font, *wxBLACK);
    wxDouble tw = 0, th = 0, descent = 0, leading = 0;
    gc->GetTextExtent(text, &tw, &th, &descent, &leading);
    w = int(std::ceil(tw)) + 2;
    h = int(std::ceil(th)) + 2;
    return w > 2 && h > 2;
}

} // namespace

bool showcase_decode_image(const std::vector<uint8_t> &bytes, ShowcaseImage &out)
{
    out = ShowcaseImage();
    if (bytes.empty())
        return false;
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY) || !image.IsOk())
        return false;
    showcase_wx_to_rgba(image, out);
    return !out.empty();
}

bool showcase_rasterize_watermark(const Showcase::Watermark &wm, const Showcase::Project &project, int image_w, int image_h,
                                  ShowcaseImage &mark, Showcase::RectI &where)
{
    mark  = ShowcaseImage();
    where = Showcase::RectI();
    if (!wm.enabled || !wm.has_content() || image_w <= 0 || image_h <= 0)
        return false;

    ShowcaseImage logo;
    double        logo_aspect = 0.;
    if (!wm.logo.empty())
        if (const Showcase::ImageBlob *blob = project.find_image(wm.logo); blob != nullptr && showcase_decode_image(blob->bytes, logo))
            logo_aspect = double(logo.width) / double(std::max(1, logo.height));
    const bool has_logo = logo_aspect > 0.;

    const wxString text = wxString::FromUTF8(wm.text);
    int            font_px = Showcase::watermark_font_px(wm, image_h, has_logo);
    wxFont         font    = showcase_watermark_font(wm, font_px);
    int            text_w = 0, text_h = 0;
    showcase_measure_text(text, font, text_w, text_h);

    Showcase::WatermarkLayout layout = Showcase::layout_watermark(wm, image_w, image_h, logo_aspect, text_w, text_h);
    if (layout.block.empty())
        return false;
    // The layout shrank the mark to fit: set the text at the size it now has.
    if (!layout.text.empty() && layout.font_px != font_px) {
        font_px = layout.font_px;
        font    = showcase_watermark_font(wm, font_px);
    }

    mark.width  = layout.block.w;
    mark.height = layout.block.h;
    mark.rgba.assign(size_t(mark.width) * size_t(mark.height) * 4, 0);

    if (has_logo && !layout.logo.empty()) {
        wxImage scaled = showcase_rgba_to_wx(logo);
        scaled.Rescale(layout.logo.w, layout.logo.h, wxIMAGE_QUALITY_HIGH);
        ShowcaseImage s;
        showcase_wx_to_rgba(scaled, s);
        Showcase::blend_over_rgba(mark.rgba, mark.width, mark.height, s.rgba, s.width, s.height, layout.logo.x - layout.block.x,
                                  layout.logo.y - layout.block.y, 1.f);
    }

    if (!layout.text.empty()) {
        wxImage canvas(layout.text.w, layout.text.h, true);
        canvas.InitAlpha();
        std::memset(canvas.GetAlpha(), 0, size_t(layout.text.w) * size_t(layout.text.h));
        {
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(canvas));
            if (gc) {
                std::array<float, 4> c{ 1.f, 1.f, 1.f, 1.f };
                Showcase::parse_hex_color(wm.text_color, c);
                gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
                gc->SetFont(font, wxColour((unsigned char) std::lround(c[0] * 255.f), (unsigned char) std::lround(c[1] * 255.f),
                                           (unsigned char) std::lround(c[2] * 255.f)));
                gc->DrawText(text, 1., 1.);
            }
            // The context writes its pixels back into `canvas` when it is destroyed, here.
        }
        ShowcaseImage t;
        showcase_wx_to_rgba(canvas, t);
        Showcase::blend_over_rgba(mark.rgba, mark.width, mark.height, t.rgba, t.width, t.height, layout.text.x - layout.block.x,
                                  layout.text.y - layout.block.y, 1.f);
    }
    where = layout.block;
    return true;
}

void showcase_apply_watermark(ShowcaseImage &image, const Showcase::Watermark &wm, const Showcase::Project &project)
{
    ShowcaseImage   mark;
    Showcase::RectI where;
    if (image.empty() || !showcase_rasterize_watermark(wm, project, image.width, image.height, mark, where))
        return;
    Showcase::blend_over_rgba(image.rgba, image.width, image.height, mark.rgba, mark.width, mark.height, where.x, where.y, wm.opacity);
}

bool showcase_load_image_file(const std::string &path, int max_side, bool always_png, std::vector<uint8_t> &bytes, std::string &ext,
                              std::string &error)
{
    bytes.clear();
    ext.clear();
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(path), wxBITMAP_TYPE_ANY) || !image.IsOk()) {
        error = "The image could not be read.";
        return false;
    }
    const int w = image.GetWidth(), h = image.GetHeight();
    if (w <= 0 || h <= 0) {
        error = "The image is empty.";
        return false;
    }
    if (std::max(w, h) > max_side) {
        const double s = double(max_side) / double(std::max(w, h));
        image.Rescale(std::max(1, int(std::lround(w * s))), std::max(1, int(std::lround(h * s))), wxIMAGE_QUALITY_HIGH);
    }
    bool transparent = image.HasMask();
    if (image.HasAlpha()) {
        const unsigned char *a = image.GetAlpha();
        for (size_t i = 0; i < size_t(image.GetWidth()) * size_t(image.GetHeight()) && !transparent; ++i)
            transparent = a[i] < 255;
    }
    const bool        png = always_png || transparent;
    wxMemoryOutputStream stream;
    if (!png) {
        image.SetOption(wxIMAGE_OPTION_QUALITY, 90);
        if (image.HasAlpha())
            image.ClearAlpha();
    }
    if (!image.SaveFile(stream, png ? wxBITMAP_TYPE_PNG : wxBITMAP_TYPE_JPEG)) {
        error = "The image could not be encoded.";
        return false;
    }
    bytes.resize(size_t(stream.GetSize()));
    if (!bytes.empty())
        stream.CopyTo(bytes.data(), bytes.size());
    ext = png ? "png" : "jpg";
    return !bytes.empty();
}

// ------------------------------------------------------------------------------------------------
// Presets
// ------------------------------------------------------------------------------------------------

std::string WatermarkPresets::file_path() { return (boost::filesystem::path(data_dir()) / "showcase" / "watermarks.json").string(); }

const WatermarkPresets::Preset *WatermarkPresets::find(const std::string &name) const
{
    for (const Preset &p : m_presets)
        if (p.name == name)
            return &p;
    return nullptr;
}

void WatermarkPresets::put(Preset preset)
{
    for (Preset &p : m_presets)
        if (p.name == preset.name) {
            p = std::move(preset);
            return;
        }
    m_presets.push_back(std::move(preset));
}

void WatermarkPresets::remove(const std::string &name)
{
    m_presets.erase(std::remove_if(m_presets.begin(), m_presets.end(), [&name](const Preset &p) { return p.name == name; }), m_presets.end());
}

void WatermarkPresets::load()
{
    m_presets.clear();
    namespace fs = boost::filesystem;
    const fs::path file = file_path();
    boost::system::error_code ec;
    if (!fs::exists(file, ec))
        return;
    std::string text;
    {
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        if (!in)
            return;
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("presets") || !j["presets"].is_array()) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: " << file.string() << " is not a watermark preset file; ignored";
        return;
    }
    const fs::path logos = file.parent_path() / "logos";
    for (const nlohmann::json &pj : j["presets"]) {
        if (!pj.is_object() || !pj.contains("name") || !pj["name"].is_string() || !pj.contains("watermark"))
            continue;
        Preset p;
        p.name = pj["name"].get<std::string>();
        if (!Showcase::watermark_from_json(pj["watermark"].dump(), p.watermark))
            continue;
        p.watermark.name = p.name;
        p.watermark.logo.clear();
        if (pj.contains("logo") && pj["logo"].is_string()) {
            const std::string logo_file = pj["logo"].get<std::string>();
            // Only a bare file name inside logos/: the file is ours, but never follow a path out of it.
            if (!logo_file.empty() && logo_file.find_first_of("/\\:") == std::string::npos) {
                boost::nowide::ifstream in((logos / logo_file).string(), std::ios::binary);
                if (in) {
                    p.logo.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    const size_t dot = logo_file.find_last_of('.');
                    p.logo_ext       = dot == std::string::npos ? "png" : logo_file.substr(dot + 1);
                    if (p.logo_ext != "jpg")
                        p.logo_ext = "png";
                    if (!p.logo.empty())
                        p.watermark.logo = image_fill_sha256_hex(p.logo);
                }
            }
        }
        put(std::move(p));
    }
}

bool WatermarkPresets::save(std::string *error) const
{
    namespace fs = boost::filesystem;
    const fs::path file  = file_path();
    const fs::path logos = file.parent_path() / "logos";
    boost::system::error_code ec;
    fs::create_directories(logos, ec);
    if (ec) {
        if (error)
            *error = ec.message();
        return false;
    }
    nlohmann::json list = nlohmann::json::array();
    for (const Preset &p : m_presets) {
        nlohmann::json pj;
        pj["name"] = p.name;
        Showcase::Watermark wm = p.watermark;
        wm.logo.clear();
        pj["watermark"] = nlohmann::json::parse(Showcase::watermark_to_json(wm), nullptr, false);
        if (!p.logo.empty()) {
            const std::string logo_file = image_fill_sha256_hex(p.logo) + "." + (p.logo_ext == "jpg" ? "jpg" : "png");
            const fs::path    logo_path = logos / logo_file;
            if (!fs::exists(logo_path, ec)) {
                boost::nowide::ofstream out(logo_path.string(), std::ios::binary);
                out.write(reinterpret_cast<const char *>(p.logo.data()), std::streamsize(p.logo.size()));
                if (!out) {
                    if (error)
                        *error = "Could not write " + logo_path.string();
                    return false;
                }
            }
            pj["logo"] = logo_file;
        }
        list.push_back(pj);
    }
    nlohmann::json j;
    j["version"] = 1;
    j["presets"] = list;
    const std::string text = j.dump(2);
    const fs::path    tmp  = file.string() + ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary);
        out << text;
        if (!out) {
            if (error)
                *error = "Could not write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        if (error)
            *error = ec.message();
        return false;
    }
    return true;
}

} // namespace GUI
} // namespace Slic3r
