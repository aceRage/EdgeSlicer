// The network plug-in guards.
//
// EdgeSlicer ships its own clean-room plug-in ("UltraNet") under Bambu's library name, so two
// things have to be decided from the same handful of facts:
//
//  * Account > Login must not open a sign-in page when no agent is loaded - the Google flow ends
//    with the ticket being handed to the plug-in, and with none loaded the user just watches it
//    fail. Offer the plug-in download instead.
//  * Nothing may replace UltraNet. install_plugin() unzips Bambu's CDN package over
//    data_dir/plugins, so every download and update entry point is gated on "is the installed
//    plug-in ours?" - a question the DLL name alone cannot answer, hence the marker file.
//
// The trap the marker exists for: a leftover marker with no library beside it, and a Bambu-original
// library with no marker. Neither is UltraNet, and both must keep the CDN path fully working.
//
// Nothing here touches wx, a config file, the filesystem, the network, or a printer.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PluginGuard.hpp"

using Slic3r::GUI::LoginGuardAction;
using Slic3r::GUI::bambu_cdn_download_allowed;
using Slic3r::GUI::is_ultranet_plugin;
using Slic3r::GUI::plugin_guard_decision;

TEST_CASE("UltraNet is a library AND a marker, never one alone", "[PluginGuard]")
{
    CHECK(is_ultranet_plugin(/*plugin_present*/ true, /*ultranet_marker*/ true));
    // A Bambu-original (or CDN-updated) plug-in: no marker was written beside it.
    CHECK_FALSE(is_ultranet_plugin(true, false));
    // A marker left behind after the library was removed or replaced by hand.
    CHECK_FALSE(is_ultranet_plugin(false, true));
    // Empty plug-ins folder.
    CHECK_FALSE(is_ultranet_plugin(false, false));
}

TEST_CASE("The Bambu CDN path is disabled exactly when UltraNet is installed", "[PluginGuard]")
{
    CHECK_FALSE(bambu_cdn_download_allowed(true, true));
    // The regression this guards: an empty folder or a Bambu-original plug-in must keep the
    // download and update path working in full.
    CHECK(bambu_cdn_download_allowed(false, false));
    CHECK(bambu_cdn_download_allowed(true, false));
    CHECK(bambu_cdn_download_allowed(false, true));
}

TEST_CASE("A loaded agent is left alone", "[PluginGuard]")
{
    // The running agent is the ground truth - it can exchange the sign-in ticket, so sign-in opens.
    CHECK(plugin_guard_decision(true, true, true, /*agent_loaded*/ true) == LoginGuardAction::ShowLogin);
    CHECK(plugin_guard_decision(true, false, true, true) == LoginGuardAction::ShowLogin);
    // A stale installed_networking=false must not block a session that is already working.
    CHECK(plugin_guard_decision(true, true, /*installed_networking*/ false, true) == LoginGuardAction::ShowLogin);
    CHECK(plugin_guard_decision(false, false, false, true) == LoginGuardAction::ShowLogin);
}

TEST_CASE("A fresh install with no plug-in is offered the download, not a dead sign-in page", "[PluginGuard]")
{
    // The reported bug: no plugins folder at all, so the loopback callback has nothing to hand the
    // ticket to.
    CHECK(plugin_guard_decision(/*plugin_present*/ false, /*ultranet_marker*/ false,
                                /*installed_networking*/ false, /*agent_loaded*/ false)
          == LoginGuardAction::OfferPluginDownload);
    // Same answer with the preference on: the preference is not what makes sign-in work.
    CHECK(plugin_guard_decision(false, false, true, false) == LoginGuardAction::OfferPluginDownload);
    // A Bambu-original plug-in that failed to load: the download is still the right offer.
    CHECK(plugin_guard_decision(true, false, true, false) == LoginGuardAction::OfferPluginDownload);
    // A leftover marker is not a plug-in.
    CHECK(plugin_guard_decision(false, true, true, false) == LoginGuardAction::OfferPluginDownload);
}

TEST_CASE("UltraNet installed but not loaded asks for a restart, never a download", "[PluginGuard]")
{
    // The dangerous case: offering Bambu's CDN here would unzip their package over ours. The usual
    // cause is the first-run copy landing after the plug-in load point, which a restart fixes.
    CHECK(plugin_guard_decision(true, true, true, /*agent_loaded*/ false) == LoginGuardAction::RestartRequired);
    CHECK(plugin_guard_decision(true, true, /*installed_networking*/ false, false) == LoginGuardAction::RestartRequired);
}
