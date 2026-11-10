#pragma once

#include <libslic3r/Semver.hpp>

#include <cctype>
#include <stdexcept>
#include <string>

namespace Slic3r {

// Advertised `orca` API version for this Edge build. Phase 3 is the v1 subset:
// script capabilities plus read-only host bindings. Bump this when the public
// surface grows (plan section 11 item 2).
inline constexpr const char* k_orca_api_version = "1.0.0";

inline std::string normalize_orca_api_version(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();

    while (!value.empty()) {
        const char ch = value.front();
        if (ch == '>' || ch == '<' || ch == '=' || ch == '~' || ch == '!')
            value.erase(value.begin());
        else
            break;
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());

    if (value.empty())
        return {};

    int dots = 0;
    for (char ch : value)
        if (ch == '.')
            ++dots;
    if (dots == 0)
        value += ".0.0";
    else if (dots == 1)
        value += ".0";
    return value;
}

// Empty `required` means the plugin did not ask for a version and is accepted.
// An unparseable requirement is refused (fail closed). A requirement newer than
// the advertised version is refused.
inline bool orca_api_version_supported(const std::string& required, std::string& error)
{
    if (required.empty())
        return true;

    const std::string want = normalize_orca_api_version(required);
    const auto        req  = Semver::parse(want);
    const auto        have = Semver::parse(k_orca_api_version);
    if (!req || !have) {
        error = "Plugin requires orca API version '" + required +
                "', which this Edge build does not understand (advertises " + std::string(k_orca_api_version) + ").";
        return false;
    }
    if (*have < *req) {
        error = "Plugin requires orca API version " + required + ", but this Edge build advertises " +
                std::string(k_orca_api_version) + ".";
        return false;
    }
    return true;
}

inline std::runtime_error edge_plugin_api_unavailable(const std::string& name)
{
    return std::runtime_error(name + " is not available in this Edge build");
}

} // namespace Slic3r
