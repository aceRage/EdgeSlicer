#pragma once

#include <string>

#include <imgui/imgui.h>

// Glyph set and text fonts of the ImGui atlas (ImGuiWrapper::init_font), kept free of GL and wx so a test
// can build an atlas with them. OrcaSlicer #15419: names drawn through the atlas (file, object, preset and
// vendor names) do not follow the UI language, so every UI language gets the common alphabetic scripts, and
// the script fonts that cover little besides their own script get the default font merged in behind them.

namespace Slic3r { namespace GUI { namespace ImGuiFonts {

// Scripts added to every UI language on top of ImFontAtlas::GetGlyphRangesDefault() (which already carries the
// CJK ideographs): Latin Extended-A/B, Greek, Cyrillic and Latin Extended Additional (Vietnamese).
const ImWchar *language_independent_ranges();

// Currency symbols (EUR, INR, KRW, RUB, ...) for the cost section's Preferences > Currency symbol (Edge).
const ImWchar *currency_ranges();

// Ranges of the text fonts: the UI language's ranges, the default ranges, the language independent and the
// currency ranges, plus `extra_ranges` when not null (the macOS keyboard shortcut symbols).
void build_text_glyph_ranges(ImFontAtlas &atlas, const ImWchar *ui_ranges, const ImWchar *extra_ranges, ImVector<ImWchar> &out);

struct TextFontFiles
{
    const char *regular;
    const char *bold;
    // The script font covers its own script and little else, so the default font is merged in behind it.
    bool        needs_glyph_fallback;
};

// Font files for the UI language whose ranges are `ui_ranges` (one of the ImFontAtlas::GetGlyphRanges*()).
TextFontFiles text_font_files(ImFontAtlas &atlas, const ImWchar *ui_ranges);

// Adds the regular and the bold text font from `fonts_dir` with `ranges` (see build_text_glyph_ranges()),
// each followed by the default font merged in for the glyphs it lacks when the language needs it.
// A font file that does not load is replaced by ImGui's built-in font. Returns false if even that fails.
bool add_text_fonts(ImFontAtlas &atlas, const std::string &fonts_dir, const ImWchar *ui_ranges, const ImVector<ImWchar> &ranges,
                    float font_size, const ImFontConfig &cfg, ImFont *&regular, ImFont *&bold);

}}} // namespace Slic3r::GUI::ImGuiFonts
