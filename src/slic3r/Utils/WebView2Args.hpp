#ifndef slic3r_Utils_WebView2Args_hpp_
#define slic3r_Utils_WebView2Args_hpp_

// WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS for the WebView2 152+ HideCursorWhileTyping bug
// (WebView2Feedback#5687, OrcaSlicer #16880). The runtime hides the pointer while typing and
// only restores it on a mouse event inside the webview, so closing one from the keyboard leaves
// the cursor hidden over the whole app.
//
// Orca sets the env with replace=false, which leaves an already-set value entirely alone. Edge
// may already have this env (the user, Stream/hub, a future path), so the flag is appended to
// that value instead of replacing it. If HideCursorWhileTyping is already listed under
// --disable-features, the string is left unchanged (Orca's "leave alone" intent).
//
// Header-only and free of wx / WebView2 so tests/slic3rutils/webview2_args_tests.cpp can pin the
// append rules without starting a webview.

#include <cctype>
#include <string>

namespace Slic3r {
namespace WebView2Args {

inline constexpr const char *ENV_NAME = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
inline constexpr const char *FEATURE  = "HideCursorWhileTyping";
inline constexpr const char *FLAG     = "--disable-features=HideCursorWhileTyping";

inline bool feature_in_list(const std::string &list, const std::string &feature)
{
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos)
            j = list.size();
        size_t a = i;
        size_t b = j;
        while (a < b && std::isspace(static_cast<unsigned char>(list[a])))
            ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(list[b - 1])))
            --b;
        if (b - a == feature.size() && list.compare(a, b - a, feature) == 0)
            return true;
        if (j == list.size())
            break;
        i = j + 1;
    }
    return false;
}

// One `--disable-features=` token in a Chromium additional-args string. `value_start`/`value_end`
// skip wrapping quotes so a comma-append lands inside them.
struct DisableFeaturesSpan
{
    size_t switch_start { 0 };
    size_t value_start { 0 };
    size_t value_end { 0 };
    bool   found { false };
};

inline bool is_arg_boundary(const std::string &s, size_t pos)
{
    return pos == 0 || std::isspace(static_cast<unsigned char>(s[pos - 1]));
}

inline DisableFeaturesSpan find_disable_features(const std::string &s, size_t from = 0)
{
    static const std::string prefix = "--disable-features=";
    size_t                   pos    = from;
    while (true) {
        pos = s.find(prefix, pos);
        if (pos == std::string::npos)
            return {};
        if (!is_arg_boundary(s, pos)) {
            pos += prefix.size();
            continue;
        }
        size_t vs = pos + prefix.size();
        if (vs < s.size() && (s[vs] == '"' || s[vs] == '\'')) {
            const char   q     = s[vs];
            const size_t close = s.find(q, vs + 1);
            return { pos, vs + 1, close == std::string::npos ? s.size() : close, true };
        }
        // Chromium ends an unquoted switch at a space. A human-written list often has a space
        // after each comma (`Foo, Bar`); keep those tokens so "already listed" still matches.
        size_t ve = vs;
        while (ve < s.size()) {
            while (ve < s.size() && !std::isspace(static_cast<unsigned char>(s[ve])))
                ++ve;
            size_t look = ve;
            while (look < s.size() && std::isspace(static_cast<unsigned char>(s[look])))
                ++look;
            if (look == s.size() || s[look] == '-')
                break;
            size_t comma = ve;
            while (comma > vs && std::isspace(static_cast<unsigned char>(s[comma - 1])))
                --comma;
            if (comma == vs || s[comma - 1] != ',')
                break;
            ve = look;
        }
        return { pos, vs, ve, true };
    }
}

// The value to write to WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS. `current` is getenv (nullptr if unset).
inline std::string with_hide_cursor_while_typing(const char *current)
{
    if (current == nullptr || *current == '\0')
        return FLAG;

    const std::string value(current);
    bool              only_space = true;
    for (unsigned char c : value) {
        if (!std::isspace(c)) {
            only_space = false;
            break;
        }
    }
    if (only_space)
        return FLAG;

    DisableFeaturesSpan first;
    size_t              from = 0;
    while (true) {
        const DisableFeaturesSpan span = find_disable_features(value, from);
        if (!span.found)
            break;
        if (!first.found)
            first = span;
        if (feature_in_list(value.substr(span.value_start, span.value_end - span.value_start), FEATURE))
            return value;
        from = span.value_end > from ? span.value_end : from + 1;
    }

    if (first.found) {
        const std::string list = value.substr(first.value_start, first.value_end - first.value_start);
        std::string       insert;
        if (list.empty()) {
            insert = FEATURE;
        } else {
            size_t e = list.size();
            while (e > 0 && std::isspace(static_cast<unsigned char>(list[e - 1])))
                --e;
            insert = (e > 0 && list[e - 1] == ',') ? std::string(FEATURE) : ("," + std::string(FEATURE));
        }
        std::string out = value;
        out.insert(first.value_end, insert);
        return out;
    }

    if (std::isspace(static_cast<unsigned char>(value.back())))
        return value + FLAG;
    return value + " " + FLAG;
}

} // namespace WebView2Args
} // namespace Slic3r

#endif // slic3r_Utils_WebView2Args_hpp_
