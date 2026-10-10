#include "BambuSyncPolicy.hpp"

namespace Slic3r {
namespace BambuSync {

bool bambu_set_up(const Inputs& in)
{
    return in.bbl_printer_preset || in.bambu_device || in.bambu_login;
}

Plan plan(const Inputs& in)
{
    Plan p;
    if (in.stealth_mode) {
        p.reason = "stealth mode";
        return p;
    }
    if (!bambu_set_up(in)) {
        p.reason = "no Bambu printer or Bambu login";
        return p;
    }
    p.run            = true;
    p.printer_config = true;
    p.reason         = in.bambu_login ? "Bambu login" : (in.bambu_device ? "Bambu printer in the device list" : "Bambu printer preset");
    return p;
}

} // namespace BambuSync
} // namespace Slic3r
