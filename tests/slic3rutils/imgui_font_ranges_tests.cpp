#include <catch2/catch.hpp>

#include <string>

#include "slic3r/GUI/ImGuiFontRanges.hpp"

using namespace Slic3r::GUI::ImGuiFonts;

// OrcaSlicer #15419 (5B-4): Cyrillic and other non-Latin names showed as '?' in ImGui panels when the UI
// language did not cover their script (a Russian object name under an English UI), and the Korean UI font
// (NanumGothic) has no simplified Chinese, Latin Extended or most currency signs at all. The ranges now carry
// the common alphabetic scripts for every language, and the default font is merged in behind NanumGothic.

namespace {

bool in_ranges(const ImVector<ImWchar> &ranges, ImWchar c)
{
    for (int i = 0; i + 1 < ranges.Size && ranges[i] != 0; i += 2)
        if (ranges[i] <= c && c <= ranges[i + 1])
            return true;
    return false;
}

const std::string fonts_dir = std::string(SLIC3R_TEST_RESOURCES_DIR) + "/fonts";

constexpr ImWchar cyrillic_zhe   = 0x0416; // Zhe
constexpr ImWchar greek_omega    = 0x03A9;
constexpr ImWchar latin_o_dacute = 0x0151; // Hungarian o with double acute (Latin Extended-A)
constexpr ImWchar viet_a_dot     = 0x1EA1; // Vietnamese a with dot below (Latin Extended Additional)
constexpr ImWchar ruble          = 0x20BD; // Edge: Preferences > Currency symbol
constexpr ImWchar cjk_zhong      = 0x4E2D;
constexpr ImWchar simplified_zhe = 0x8FD9; // simplified Chinese only, not in NanumGothic
constexpr ImWchar hangul_han     = 0xD55C;

} // namespace

TEST_CASE("Every UI language's glyph ranges carry the common alphabetic scripts", "[ImGuiFonts]")
{
    ImFontAtlas atlas;
    const ImWchar *ui_languages[] = { atlas.GetGlyphRangesEnglish(), atlas.GetGlyphRangesKorean(), atlas.GetGlyphRangesJapanese(),
                                      atlas.GetGlyphRangesThai(), atlas.GetGlyphRangesOthers() };
    for (const ImWchar *ui : ui_languages) {
        ImVector<ImWchar> ranges;
        build_text_glyph_ranges(atlas, ui, nullptr, ranges);
        CHECK(in_ranges(ranges, 'A'));
        CHECK(in_ranges(ranges, cyrillic_zhe));
        CHECK(in_ranges(ranges, greek_omega));
        CHECK(in_ranges(ranges, latin_o_dacute));
        CHECK(in_ranges(ranges, viet_a_dot));
        CHECK(in_ranges(ranges, cjk_zhong));
        // Edge: the currency symbols stay in for every language.
        CHECK(in_ranges(ranges, ruble));
        CHECK(in_ranges(ranges, 0x20AC));
    }
}

TEST_CASE("UI language ranges and the macOS extra ranges are kept", "[ImGuiFonts]")
{
    ImFontAtlas atlas;
    ImVector<ImWchar> ranges;
    static const ImWchar shortcuts[] = { 0x2318, 0x2318, 0 };
    build_text_glyph_ranges(atlas, atlas.GetGlyphRangesKorean(), shortcuts, ranges);
    CHECK(in_ranges(ranges, hangul_han));
    CHECK(in_ranges(ranges, 0x2318));

    ImVector<ImWchar> no_extra;
    build_text_glyph_ranges(atlas, atlas.GetGlyphRangesEnglish(), nullptr, no_extra);
    CHECK_FALSE(in_ranges(no_extra, hangul_han));
    CHECK_FALSE(in_ranges(no_extra, 0x2318));
}

TEST_CASE("Only the Korean UI font needs the default font merged in", "[ImGuiFonts]")
{
    ImFontAtlas atlas;
    const TextFontFiles korean = text_font_files(atlas, atlas.GetGlyphRangesKorean());
    CHECK(std::string(korean.regular) == "NanumGothic-Regular.ttf");
    CHECK(std::string(korean.bold) == "NanumGothic-Bold.ttf");
    CHECK(korean.needs_glyph_fallback);

    const TextFontFiles english = text_font_files(atlas, atlas.GetGlyphRangesEnglish());
    CHECK(std::string(english.regular) == "HarmonyOS_Sans_SC_Regular.ttf");
    CHECK(std::string(english.bold) == "HarmonyOS_Sans_SC_Bold.ttf");
    CHECK_FALSE(english.needs_glyph_fallback);
    CHECK_FALSE(text_font_files(atlas, atlas.GetGlyphRangesCyrillic()).needs_glyph_fallback);
}

TEST_CASE("English UI atlas has Cyrillic, Latin Extended and Vietnamese glyphs", "[ImGuiFonts]")
{
    ImFontAtlas atlas;
    ImVector<ImWchar> ranges;
    build_text_glyph_ranges(atlas, atlas.GetGlyphRangesEnglish(), nullptr, ranges);
    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    ImFont *regular = nullptr;
    ImFont *bold    = nullptr;
    REQUIRE(add_text_fonts(atlas, fonts_dir, atlas.GetGlyphRangesEnglish(), ranges, 13.f, cfg, regular, bold));
    REQUIRE(regular != nullptr);
    REQUIRE(bold != nullptr);
    CHECK(regular != bold);
    CHECK(atlas.Fonts.Size == 2);
    REQUIRE(atlas.Build());
    for (ImFont *font : { regular, bold }) {
        CHECK(font->FindGlyphNoFallback(cyrillic_zhe) != nullptr);
        CHECK(font->FindGlyphNoFallback(greek_omega) != nullptr);
        CHECK(font->FindGlyphNoFallback(latin_o_dacute) != nullptr);
        CHECK(font->FindGlyphNoFallback(viet_a_dot) != nullptr);
        CHECK(font->FindGlyphNoFallback(ruble) != nullptr);
    }
}

TEST_CASE("Korean UI atlas falls back to the default font for glyphs NanumGothic lacks", "[ImGuiFonts]")
{
    ImFontAtlas atlas;
    ImVector<ImWchar> ranges;
    build_text_glyph_ranges(atlas, atlas.GetGlyphRangesKorean(), nullptr, ranges);
    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    ImFont *regular = nullptr;
    ImFont *bold    = nullptr;
    REQUIRE(add_text_fonts(atlas, fonts_dir, atlas.GetGlyphRangesKorean(), ranges, 13.f, cfg, regular, bold));
    REQUIRE(regular != nullptr);
    REQUIRE(bold != nullptr);
    // The merged fonts add glyphs to the regular and the bold font; they are no fonts of their own.
    CHECK(atlas.Fonts.Size == 2);
    CHECK(atlas.ConfigData.Size == 4);
    REQUIRE(atlas.Build());
    for (ImFont *font : { regular, bold }) {
        CHECK(font->FindGlyphNoFallback(hangul_han) != nullptr);
        CHECK(font->FindGlyphNoFallback(cyrillic_zhe) != nullptr);
        CHECK(font->FindGlyphNoFallback(simplified_zhe) != nullptr);
        CHECK(font->FindGlyphNoFallback(latin_o_dacute) != nullptr);
        CHECK(font->FindGlyphNoFallback(viet_a_dot) != nullptr);
        CHECK(font->FindGlyphNoFallback(ruble) != nullptr);
    }
}
