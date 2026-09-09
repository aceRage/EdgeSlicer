#include "LoginUserAgent.hpp"

namespace Slic3r {

std::string bbl_login_user_agent(LoginUAPlatform platform,
                                 bool            dark,
                                 const std::string &language_code,
                                 const std::string &brand_tag,
                                 const std::string &slicer_version)
{
    // Only the Bambu login flavour reports the gated version; other webviews keep ours.
    const std::string version = (brand_tag == "BBL-Slicer") ? std::string(BBL_LOGIN_UA_VERSION) : slicer_version;

    // Language token: bambulab expects the bare primary subtag ("en", "de"), never the
    // full canonical name ("en_US"). Empty is tolerated by the site, so fall back to "en"
    // rather than emitting a dangling "BBL-Language/".
    std::string lang = language_code;
    const std::size_t sep = lang.find_first_of("_-");
    if (sep != std::string::npos)
        lang = lang.substr(0, sep);
    if (lang.empty())
        lang = "en";

    std::string prefix;
    switch (platform) {
    case LoginUAPlatform::Windows:
        prefix = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                 "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52";
        break;
    case LoginUAPlatform::MacOS:
    case LoginUAPlatform::Linux:
    default:
        prefix = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko)";
        break;
    }

    return prefix + " " + brand_tag + "/v" + version + " (" + (dark ? "dark" : "light") + ") BBL-Language/" + lang;
}

} // namespace Slic3r
