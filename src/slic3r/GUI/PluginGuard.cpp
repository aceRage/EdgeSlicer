#include "PluginGuard.hpp"

namespace Slic3r { namespace GUI {

const char *const kUltraNetMarkerName = "ultranet.txt";

bool is_ultranet_plugin(bool plugin_present, bool ultranet_marker)
{
    // The marker alone proves nothing - a stale ultranet.txt left behind after the user replaced
    // the plug-in with Bambu's must not lock the CDN path out. Both, or it is not ours.
    return plugin_present && ultranet_marker;
}

bool bambu_cdn_download_allowed(bool plugin_present, bool ultranet_marker)
{
    return ! is_ultranet_plugin(plugin_present, ultranet_marker);
}

LoginGuardAction plugin_guard_decision(bool plugin_present,
                                       bool ultranet_marker,
                                       bool installed_networking,
                                       bool agent_loaded)
{
    // An agent is loaded: the ticket exchange has somewhere to go. Nothing to guard. The
    // preference is deliberately not consulted here - the running agent is the ground truth, and a
    // stale `installed_networking=false` must not block a session that is already working.
    if (agent_loaded)
        return LoginGuardAction::ShowLogin;

    // No agent, but our plug-in is sitting in the plug-ins folder. Offering Bambu's CDN download
    // here would overwrite it, and it would not help anyway: the usual cause is the first-run copy
    // landing after the plug-in load point, which a restart fixes.
    if (is_ultranet_plugin(plugin_present, ultranet_marker))
        return LoginGuardAction::RestartRequired;

    // No agent and no plug-in of ours. Either networking is switched off, or the folder is empty,
    // or a Bambu-original plug-in failed to load - the download dialog is the right answer to all
    // three, and it is also where the user turns the preference back on.
    (void) installed_networking;
    return LoginGuardAction::OfferPluginDownload;
}

} } // namespace Slic3r::GUI
