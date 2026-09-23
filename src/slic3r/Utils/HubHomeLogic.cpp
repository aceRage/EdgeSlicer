#include "HubHomeLogic.hpp"

namespace Slic3r {
namespace HubHome {

bool valid_token(const std::string& token)
{
    return token.size() >= 10 && token.size() <= 32 &&
           token.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
}

static bool valid_port(int port) { return port > 0 && port <= 65535; }

const char* theme_name(bool dark) { return dark ? "dark" : "light"; }

static std::string hub_page_prefix(int port, const std::string& token)
{
    return "http://127.0.0.1:" + std::to_string(port) + "/r/" + token + "/";
}

std::string hub_page_url(int port, const std::string& token, bool dark)
{
    if (!valid_port(port) || !valid_token(token))
        return std::string();
    return hub_page_prefix(port, token) + "?embed=1&theme=" + theme_name(dark);
}

bool is_hub_page_url(const std::string& url, int port, const std::string& token)
{
    if (!valid_port(port) || !valid_token(token))
        return false;
    const std::string prefix = hub_page_prefix(port, token);
    if (url.compare(0, prefix.size(), prefix) == 0)
        return true;
    // The bare /r/<token> (no trailing slash) redirects to the page; allow it, and only exactly it
    // or it followed by a query/fragment.
    const std::string bare = prefix.substr(0, prefix.size() - 1);
    return url == bare || (url.size() > bare.size() && url.compare(0, bare.size(), bare) == 0 &&
                           (url[bare.size()] == '?' || url[bare.size()] == '#'));
}

bool is_allowed_navigation(const std::string& url, int port, const std::string& token)
{
    if (url.empty() || url == "about:blank" || url.compare(0, 5, "data:") == 0)
        return true;
    return is_hub_page_url(url, port, token);
}

bool is_external_browser_url(const std::string& url)
{
    auto starts_ci = [&url](const char* p) {
        size_t i = 0;
        for (; p[i] != 0; ++i) {
            if (i >= url.size())
                return false;
            char c = url[i];
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
            if (c != p[i])
                return false;
        }
        return url.size() > i; // something after the scheme
    };
    if (!(starts_ci("http://") || starts_ci("https://")))
        return false;
    // No whitespace or control characters: nothing that could be read as a second argument.
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7f)
            return false;
    return true;
}

Recheck recheck(bool hub_alive, int hub_port, const std::string& hub_token, int loaded_port,
                const std::string& loaded_token)
{
    if (!hub_alive || !valid_port(hub_port) || !valid_token(hub_token))
        return Recheck::HubDown;
    if (hub_port != loaded_port || hub_token != loaded_token)
        return Recheck::Reload;
    return Recheck::Keep;
}

std::string theme_script(bool dark)
{
    return std::string("if (window.__edgeTheme) window.__edgeTheme('") + theme_name(dark) + "');";
}

} // namespace HubHome
} // namespace Slic3r
