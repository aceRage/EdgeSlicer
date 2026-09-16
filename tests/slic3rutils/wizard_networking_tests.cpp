// The setup wizard's network-plug-in decision.
//
// A first-time user who picks a Bambu Lab printer in the wizard used to land in a dead end: the
// `installed_networking` preference has no default, so it reads false, the Bambu network plug-in is
// never loaded and never offered (the only two download entry points are the home-page banner and a
// Device-tab link), and Account > Login then 404s on our loopback because there is no plug-in to
// exchange the sign-in ticket with.
//
// The fix turns the preference on from the wizard - but only for a user who never expressed an
// opinion. AppConfig::has() is what separates "never set" from "set to false", and the whole point
// of the helper under test is that those two are NOT the same input.
//
// Nothing here touches wx, a config file, the network, or a printer.

#include <catch2/catch.hpp>

#include "slic3r/GUI/WizardNetworking.hpp"

using Slic3r::GUI::wizard_should_enable_networking;

TEST_CASE("A first run that installs a Bambu printer switches networking on", "[WizardNetworking]")
{
    // The bug: key absent (so it read as false), a BBL printer selected. This is the only
    // combination that changes anything.
    CHECK(wizard_should_enable_networking(/*key_present*/ false, /*key_value*/ false, /*any_bbl*/ true));
    // key_value is meaningless when the key is absent, and must not swing the answer.
    CHECK(wizard_should_enable_networking(false, true, true));
}

TEST_CASE("An explicit preference is never overridden", "[WizardNetworking]")
{
    // Set to false is a user who turned the plug-in off on purpose. Selecting a Bambu printer in a
    // later wizard run does not get to undo that - this is the regression the has()/get() split
    // exists to prevent.
    CHECK_FALSE(wizard_should_enable_networking(/*key_present*/ true, /*key_value*/ false, /*any_bbl*/ true));
    // Already on: nothing to do, and no second write.
    CHECK_FALSE(wizard_should_enable_networking(true, true, true));
}

TEST_CASE("A wizard with no Bambu printer leaves the preference alone", "[WizardNetworking]")
{
    // Snapmaker-only, Prusa-only, custom-printer-only: the Bambu plug-in is none of their business,
    // in every state of the key.
    CHECK_FALSE(wizard_should_enable_networking(false, false, /*any_bbl*/ false));
    CHECK_FALSE(wizard_should_enable_networking(false, true, false));
    CHECK_FALSE(wizard_should_enable_networking(true, false, false));
    CHECK_FALSE(wizard_should_enable_networking(true, true, false));
}
