#pragma once

// The kinds of preset that can be starred, and where each kind's favourites are stored.
//
// Favourite filaments (Orca #13946, extended) came first; favourite process (print) presets reuse the
// very same rules (FavoriteFilaments.hpp: keys, toggle, rename migration, grouping) and the same
// machinery (FavoriteFilamentsUI, PresetComboBox::enable_favorites). Each kind has its own stored set,
// so a filament and a process preset that happen to share a name never share a star.
//
// GUI-free so the storage keys can be unit-tested.

namespace Slic3r { namespace GUI { namespace FavoritePresets {

enum class Kind
{
    Filament,
    Process,
};

// The app config key ("app" section) holding the escaped list of keys for a kind.
// "favorite_filaments" is the key favourite filaments have always used; do not rename it.
inline const char *config_key(Kind kind)
{
    switch (kind) {
    case Kind::Process: return "favorite_process_presets";
    case Kind::Filament:
    default:            return "favorite_filaments";
    }
}

}}} // namespace Slic3r::GUI::FavoritePresets
