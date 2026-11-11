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

namespace orca_api_version_detail {

inline void trim_inplace(std::string& value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
}

inline std::string pad_dotted_version(std::string value)
{
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

inline bool has_prerelease_token(const std::string& value)
{
    // Semver pre-release and build metadata (`1.0.0-beta`, `1.0.0+build`).
    return value.find('-') != std::string::npos || value.find('+') != std::string::npos;
}

enum class Cmp { Eq, Ge, Gt, Le, Lt };

inline bool apply_cmp(Cmp op, const Semver& have, const Semver& want)
{
    switch (op) {
    case Cmp::Eq: return have == want;
    case Cmp::Ge: return have >= want;
    case Cmp::Gt: return have > want;
    case Cmp::Le: return have <= want;
    case Cmp::Lt: return have < want;
    }
    return false;
}

inline bool parse_clause(std::string clause, Cmp& op, Semver& ver, std::string& error, const std::string& original)
{
    trim_inplace(clause);
    if (clause.empty()) {
        error = "Plugin requires orca API version '" + original +
                "', which this Edge build does not understand (advertises " + std::string(k_orca_api_version) + ").";
        return false;
    }

    op = Cmp::Ge;
    if (clause.compare(0, 2, ">=") == 0) {
        op = Cmp::Ge;
        clause.erase(0, 2);
    } else if (clause.compare(0, 2, "<=") == 0) {
        op = Cmp::Le;
        clause.erase(0, 2);
    } else if (clause.compare(0, 2, "==") == 0) {
        op = Cmp::Eq;
        clause.erase(0, 2);
    } else if (clause.compare(0, 2, "~=") == 0) {
        error = "Plugin requires orca API version '" + original +
                "', which this Edge build does not understand (advertises " + std::string(k_orca_api_version) + ").";
        return false;
    } else if (clause.front() == '>') {
        op = Cmp::Gt;
        clause.erase(clause.begin());
    } else if (clause.front() == '<') {
        op = Cmp::Lt;
        clause.erase(clause.begin());
    } else if (clause.front() == '=') {
        op = Cmp::Eq;
        clause.erase(clause.begin());
    }

    trim_inplace(clause);
    if (clause.empty()) {
        error = "Plugin requires orca API version '" + original +
                "', which this Edge build does not understand (advertises " + std::string(k_orca_api_version) + ").";
        return false;
    }

    if (has_prerelease_token(clause)) {
        error = "Plugin requires orca API version '" + original +
                "', but this Edge build does not accept pre-release API requirements (advertises " +
                std::string(k_orca_api_version) + ").";
        return false;
    }

    const std::string padded = pad_dotted_version(clause);
    const auto        parsed = Semver::parse(padded);
    if (!parsed) {
        error = "Plugin requires orca API version '" + original +
                "', which this Edge build does not understand (advertises " + std::string(k_orca_api_version) + ").";
        return false;
    }
    ver = *parsed;
    return true;
}

} // namespace orca_api_version_detail

// Kept for tests and diagnostics: strip a single leading operator and pad x / x.y to semver.
// Prefer orca_api_version_supported(), which keeps the operator for comparison.
inline std::string normalize_orca_api_version(std::string value)
{
    orca_api_version_detail::trim_inplace(value);

    while (!value.empty()) {
        const char ch = value.front();
        if (ch == '>' || ch == '<' || ch == '=' || ch == '~' || ch == '!')
            value.erase(value.begin());
        else
            break;
    }
    orca_api_version_detail::trim_inplace(value);
    if (value.empty())
        return {};
    return orca_api_version_detail::pad_dotted_version(value);
}

// Empty `required` means the plugin did not ask for a version and is accepted.
// An unparseable requirement is refused (fail closed). A pre-release requirement
// (`1.0.0-beta`) is refused. Operators (`<`, `<=`, `>`, `>=`, `==`, bare) are
// honoured; comma-separated clauses are ANDed. A requirement newer than the
// advertised version is refused.
inline bool orca_api_version_supported(const std::string& required, std::string& error)
{
    if (required.empty())
        return true;

    const auto have = Semver::parse(k_orca_api_version);
    if (!have) {
        error = "Plugin requires orca API version '" + required +
                "', which this Edge build does not understand (advertises " + std::string(k_orca_api_version) + ").";
        return false;
    }

    std::string rest = required;
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        std::string clause = comma == std::string::npos ? rest : rest.substr(0, comma);
        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);

        orca_api_version_detail::Cmp op;
        Semver                       want;
        if (!orca_api_version_detail::parse_clause(clause, op, want, error, required))
            return false;
        if (!orca_api_version_detail::apply_cmp(op, *have, want)) {
            error = "Plugin requires orca API version " + required + ", but this Edge build advertises " +
                    std::string(k_orca_api_version) + ".";
            return false;
        }
    }
    return true;
}

inline std::runtime_error edge_plugin_api_unavailable(const std::string& name)
{
    return std::runtime_error(name + " is not available in this Edge build");
}

} // namespace Slic3r
