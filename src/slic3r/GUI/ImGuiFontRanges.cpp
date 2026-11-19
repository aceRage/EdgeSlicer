#include "ImGuiFontRanges.hpp"

namespace Slic3r { namespace GUI { namespace ImGuiFonts {

const ImWchar *language_independent_ranges()
{
    // Names drawn through the atlas come from file names and CAD data, not from the UI language.
    // GetGlyphRangesDefault() already gives every language the CJK ideographs, which is why a
    // Chinese file name renders under an English UI; these are the alphabetic scripts it omits.
    // Codepoints the font lacks are skipped at build time, so only existing glyphs cost anything.
    static const ImWchar ranges[] = {
        0x0100, 0x024F, // Latin Extended-A and Extended-B
        0x0370, 0x03FF, // Greek and Coptic
        0x0400, 0x04FF, // Cyrillic
        0x1E00, 0x1EFF, // Latin Extended Additional (Vietnamese)
        0,
    };
    return ranges;
}

const ImWchar *currency_ranges()
{
    static const ImWchar ranges[] = { 0x20A0, 0x20BF, 0 };
    return ranges;
}

void build_text_glyph_ranges(ImFontAtlas &atlas, const ImWchar *ui_ranges, const ImWchar *extra_ranges, ImVector<ImWchar> &out)
{
    ImFontAtlas::GlyphRangesBuilder builder;
    if (ui_ranges != nullptr)
        builder.AddRanges(ui_ranges);
    builder.AddRanges(atlas.GetGlyphRangesDefault());
    builder.AddRanges(language_independent_ranges());
    builder.AddRanges(currency_ranges());
    if (extra_ranges != nullptr)
        builder.AddRanges(extra_ranges);
    out.clear();
    builder.BuildRanges(&out); // ordered ranges with all the unique characters submitted
}

TextFontFiles text_font_files(ImFontAtlas &atlas, const ImWchar *ui_ranges)
{
    // Orca: temp fix for Korean font
    if (ui_ranges == atlas.GetGlyphRangesKorean())
        // NanumGothic has Hangul, some Hanja and Cyrillic, but no Latin Extended, Vietnamese,
        // simplified Chinese or most currency signs.
        return { "NanumGothic-Regular.ttf", "NanumGothic-Bold.ttf", true };
    return { "HarmonyOS_Sans_SC_Regular.ttf", "HarmonyOS_Sans_SC_Bold.ttf", false };
}

static ImFont *add_text_font(ImFontAtlas &atlas, const std::string &fonts_dir, const char *file, const char *fallback_file,
                             const ImVector<ImWchar> &ranges, float font_size, const ImFontConfig &cfg)
{
    ImFont *font = atlas.AddFontFromFileTTF((fonts_dir + "/" + file).c_str(), font_size, &cfg, ranges.Data);
    if (font == nullptr)
        font = atlas.AddFontDefault();
    if (font != nullptr && fallback_file != nullptr) {
        // A merged font only supplies glyphs the font ahead of it lacks, so this fills the gaps
        // without restyling anything the script font already covers.
        ImFontConfig fallback_cfg = cfg;
        fallback_cfg.MergeMode    = true;
        atlas.AddFontFromFileTTF((fonts_dir + "/" + fallback_file).c_str(), font_size, &fallback_cfg, ranges.Data);
    }
    return font;
}

bool add_text_fonts(ImFontAtlas &atlas, const std::string &fonts_dir, const ImWchar *ui_ranges, const ImVector<ImWchar> &ranges,
                    float font_size, const ImFontConfig &cfg, ImFont *&regular, ImFont *&bold)
{
    const TextFontFiles files = text_font_files(atlas, ui_ranges);
    regular = add_text_font(atlas, fonts_dir, files.regular, files.needs_glyph_fallback ? "HarmonyOS_Sans_SC_Regular.ttf" : nullptr,
                            ranges, font_size, cfg);
    if (regular == nullptr)
        return false;
    bold = add_text_font(atlas, fonts_dir, files.bold, files.needs_glyph_fallback ? "HarmonyOS_Sans_SC_Bold.ttf" : nullptr,
                         ranges, font_size, cfg);
    return bold != nullptr;
}

}}} // namespace Slic3r::GUI::ImGuiFonts
