#pragma once

// Upload / send file-name clean-up shared by the send dialogs.
//
// A send dialog used to warn (or refuse) when the pre-filled name - which comes from the project
// name - held a character the host dislikes (# * ; ... or < > [ ] : ...), and asked the user to
// fix it. The owner's rule is that we strip those characters ourselves instead.
//
// Only the characters a host already rejected or warned about are removed (never widened), plus
// control characters. Bytes >= 0x80 are never touched: they are the bytes of a UTF-8 name, and a
// forbidden character is always ASCII, so a non-ASCII title survives whole.

#include <algorithm>
#include <string>
#include <vector>

namespace Slic3r {

struct UploadNameRules
{
    // Every character here is removed from the name.
    std::string forbidden;
    // true: '/' separates folders ("folder/name.gcode"), each segment is cleaned on its own and
    // the '/' is kept. false: the name is a single file name ('/' belongs in `forbidden`).
    bool allow_folders = false;
};

namespace upload_name_rules {
// The generic print-host send dialog (PrintHostSendDialog: Moonraker / Snapmaker U1 / OctoPrint ...).
// The set its warning prompt used; '/' is a folder separator there.
inline UploadNameRules print_host() { return {"#*;\\:\"<>|", true}; }
// Bambu send / print dialogs (SelectMachine, SendToPrinter, SendMultiMachinePage).
inline UploadNameRules bambu() { return {"<>[]:/\\|?*\"", false}; }
} // namespace upload_name_rules

namespace detail {

inline bool upload_name_is_junk(unsigned char c, const std::string& forbidden)
{
    return c < 0x20 || c == 0x7F || forbidden.find(static_cast<char>(c)) != std::string::npos;
}

// Cleans one path segment. `changed` is set when something was removed. Returns "" for a segment
// that is nothing once cleaned (the caller drops it or falls back).
inline std::string upload_name_clean_segment(const std::string& seg, const std::string& forbidden, bool& changed)
{
    std::string out;
    out.reserve(seg.size());
    bool removed = false;
    for (char ch : seg) {
        if (upload_name_is_junk(static_cast<unsigned char>(ch), forbidden))
            removed = true;
        else
            out += ch;
    }
    if (!removed)
        return seg; // clean as it came: do not touch spacing or underscores the user chose
    changed = true;

    // Where characters were removed, runs of space / underscore they leave behind are collapsed.
    std::string collapsed;
    collapsed.reserve(out.size());
    for (char ch : out) {
        if ((ch == ' ' || ch == '_') && !collapsed.empty() && collapsed.back() == ch)
            continue;
        collapsed += ch;
    }
    return collapsed;
}

inline std::string upload_name_trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" .");
    if (b == std::string::npos)
        return std::string();
    const size_t e = s.find_last_not_of(" .");
    return s.substr(b, e - b + 1);
}

} // namespace detail

// Returns `name` with the host's forbidden characters and control characters removed.
//  - The extension of the file name (from its last '.') is kept.
//  - Where something was removed, doubled spaces / underscores are collapsed and leading/trailing
//    spaces and dots are trimmed. A name with nothing to remove comes back unchanged.
//  - With rules.allow_folders each '/'-separated segment is cleaned and the '/' kept; empty and
//    dot-only segments are dropped (no "..", no "//"); a leading '/' is kept.
//  - The result is never empty: a file name that cleans down to nothing becomes
//    `fallback_stem` + the original extension (or just `fallback_stem`).
inline std::string sanitize_upload_filename(const std::string&     name,
                                            const UploadNameRules& rules,
                                            const std::string&     fallback_stem = "plate_1")
{
    std::vector<std::string> segments;
    {
        size_t start = 0;
        if (rules.allow_folders) {
            for (;;) {
                const size_t slash = name.find('/', start);
                if (slash == std::string::npos) {
                    segments.push_back(name.substr(start));
                    break;
                }
                segments.push_back(name.substr(start, slash - start));
                start = slash + 1;
            }
        } else {
            segments.push_back(name);
        }
    }

    const bool leading_slash = rules.allow_folders && !name.empty() && name.front() == '/';
    std::string forbidden    = rules.forbidden;
    if (rules.allow_folders) {
        // '/' was consumed as the separator above; make sure it cannot be treated as junk in a segment.
        forbidden.erase(std::remove(forbidden.begin(), forbidden.end(), '/'), forbidden.end());
    }

    std::string out;
    for (size_t i = 0; i < segments.size(); ++i) {
        const bool  leaf    = (i + 1 == segments.size());
        bool        changed = false;
        std::string seg;
        if (!leaf) {
            seg = detail::upload_name_clean_segment(segments[i], forbidden, changed);
            if (changed)
                seg = detail::upload_name_trim(seg);
            // ".", ".." and anything that is only dots / spaces is not a usable folder name.
            if (seg.find_first_not_of(" .") == std::string::npos)
                continue;
            out += seg;
            out += '/';
            continue;
        }

        // The file name: split the extension off first so a name that cleans to nothing keeps it.
        const std::string& raw = segments[i];
        const size_t       dot = raw.rfind('.');
        std::string        stem = dot == std::string::npos ? raw : raw.substr(0, dot);
        std::string        ext  = dot == std::string::npos ? std::string() : raw.substr(dot);
        stem = detail::upload_name_clean_segment(stem, forbidden, changed);
        ext  = detail::upload_name_clean_segment(ext, forbidden, changed);
        if (changed) {
            stem = detail::upload_name_trim(stem);
            // "a#.gcode" -> stem "a"; an extension reduced to a bare "." or spaces is dropped.
            if (ext.find_first_not_of(" .") == std::string::npos)
                ext.clear();
            else if (ext.front() != '.')
                ext.insert(ext.begin(), '.');
            while (!ext.empty() && (ext.back() == ' ' || ext.back() == '.'))
                ext.pop_back();
        }
        if (stem.find_first_not_of(" .") == std::string::npos) {
            std::string fb = detail::upload_name_clean_segment(fallback_stem, forbidden, changed);
            fb             = detail::upload_name_trim(fb);
            stem           = fb.empty() ? std::string("plate_1") : fb;
        }
        out += stem + ext;
    }

    if (leading_slash)
        out.insert(out.begin(), '/');
    return out;
}

} // namespace Slic3r
