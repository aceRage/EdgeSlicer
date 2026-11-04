#include <catch2/catch.hpp>

#include <string>
#include <type_traits>

#include "slic3r/GUI/ImGuiWrapper.hpp"

using Slic3r::GUI::ImGuiWrapper;

// OrcaSlicer #11078 (5B-4): ImGuiWrapper::tooltip had only const char* and wxString overloads, so a UTF-8
// std::string (every _u8L tooltip of the Text, Cut and move tools) was converted to wxString through the
// locale's charset and came out as '?' or garbage in non-Latin translations. The std::string overload passes
// the bytes through as UTF-8. Checked at compile time: the overload must exist and be the exact match.

namespace {

template<class T, class = void> struct has_std_string_tooltip : std::false_type {};
template<class T>
struct has_std_string_tooltip<T, std::void_t<decltype(static_cast<void (T::*)(const std::string &, float)>(&T::tooltip))>>
    : std::true_type {};

} // namespace

TEST_CASE("ImGuiWrapper::tooltip takes UTF-8 std::string without a wxString conversion", "[ImGuiFonts]")
{
    STATIC_REQUIRE(has_std_string_tooltip<ImGuiWrapper>::value);
    // The other two overloads stay.
    STATIC_REQUIRE(std::is_same<decltype(static_cast<void (ImGuiWrapper::*)(const char *, float)>(&ImGuiWrapper::tooltip)),
                                void (ImGuiWrapper::*)(const char *, float)>::value);
    STATIC_REQUIRE(std::is_same<decltype(static_cast<void (ImGuiWrapper::*)(const wxString &, float)>(&ImGuiWrapper::tooltip)),
                                void (ImGuiWrapper::*)(const wxString &, float)>::value);
}
