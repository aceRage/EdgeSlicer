// Nozzle diameters are written "0.4" (3MF, printer reports, config). Under a UI language with a
// decimal comma (German, French) std::stod / strtod read that as 0. Upstream OrcaSlicer #17079 fixed
// its own parses; these cover the Edge multi-nozzle code that had the same bug. Catch2 v2.
#include <catch2/catch.hpp>

#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/BambuNozzleMappingRequest.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/lexical_cast.hpp>

#include <clocale>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Switches this thread's LC_NUMERIC to a locale with a decimal comma, when the host has one.
struct CommaNumericLocale
{
    bool        active = false;
    std::string previous;
#ifdef _WIN32
    int previous_threadlocale = 0;
#endif

    CommaNumericLocale()
    {
#ifdef _WIN32
        // Per-thread locale first, so the comma locale cannot leak into later tests.
        previous_threadlocale = _configthreadlocale(0);
        _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#endif
        const char *cur = std::setlocale(LC_NUMERIC, nullptr);
        previous        = cur ? cur : "C";
#ifdef _WIN32
        static const char *const names[] = {"de-DE", "German", "fr-FR", "French"};
#else
        static const char *const names[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "nl_NL.UTF-8"};
#endif
        for (const char *name : names) {
            if (std::setlocale(LC_NUMERIC, name) == nullptr)
                continue;
            if (!is_decimal_separator_point()) {
                active = true;
                return;
            }
        }
        std::setlocale(LC_NUMERIC, previous.c_str());
    }
    ~CommaNumericLocale()
    {
        std::setlocale(LC_NUMERIC, previous.c_str());
#ifdef _WIN32
        _configthreadlocale(previous_threadlocale);
#endif
    }
    CommaNumericLocale(const CommaNumericLocale &)            = delete;
    CommaNumericLocale &operator=(const CommaNumericLocale &) = delete;
};

} // namespace

TEST_CASE("Nozzle diameter strings read back under a decimal-comma locale", "[LocalesUtils][Nozzle]")
{
    CommaNumericLocale comma;
    if (!comma.active) {
        WARN("no locale with a comma decimal separator is installed");
        return;
    }
    // The bug being guarded against: the C runtime stops at the '.'.
    CHECK(std::strtod("0.4", nullptr) == Approx(0.));

    SECTION("upstream #17079's parses")
    {
        const std::string diameter = format_diameter_to_str(0.4);
        CHECK(diameter == "0.4");
        CHECK(string_to_double_decimal_point(diameter) == Approx(0.4));
        CHECK(boost::lexical_cast<double>(diameter) == Approx(0.4));
        // A comma is not read as the decimal separator.
        CHECK(string_to_double_decimal_point("0,4") == Approx(0.));
        // Callers that skip a diameter they cannot read rely on lexical_cast throwing for it.
        for (const char *text : {"", "abc", "0.4x", "0,4"}) {
            INFO(text);
            CHECK_THROWS_AS(boost::lexical_cast<double>(text), boost::bad_lexical_cast);
        }
    }
    SECTION("the nozzle_diameter_override list")
    {
        CHECK(parse_nozzle_diameter_override("0.4,0.6") == std::vector<double>{0.4, 0.6});
        CHECK(parse_nozzle_diameter_override("0.4,x").empty());
    }
    SECTION("a rack nozzle-size tool id")
    {
        size_t extruder = 9;
        double diameter = 0.;
        REQUIRE(parse_nozzle_size_group_id(nozzle_size_group_id(0, 0.6), extruder, diameter));
        CHECK(extruder == 0);
        CHECK(diameter == Approx(0.6));
    }
    SECTION("the printer's reported nozzles")
    {
        DualNozzleSync::PrinterState state;
        state.physical_extruder_map = {0};
        DualNozzleSync::PrinterNozzle mounted;
        mounted.pos      = 0;
        mounted.diameter = "0.4";
        state.nozzles.push_back(mounted);
        CHECK(reported_extruder_nozzle_sizes(state, 1) == std::vector<double>{0.4});
        // The nozzle-mapping request sends Bambu's two-decimal form, not "0.00".
        CHECK(BambuNozzleMapping::diameter_str(std::string("0.4")) == "0.40");
    }
}
