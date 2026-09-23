#ifndef slic3r_SentryScrub_hpp_
#define slic3r_SentryScrub_hpp_

// Personal-data scrubber for crash reports.
//
// Everything EdgeSlicer puts into a crash report as text (the recent log lines kept as
// breadcrumbs, and any message on the event itself) goes through scrub_for_crash_report()
// first. It is plain standard C++ with no GUI, Sentry or Boost dependency, so the unit tests
// (tests/slic3rutils/sentry_scrub_tests.cpp) exercise exactly the code the app runs, and the
// launcher executable, the app library and the tests can all compile it in.
//
// What it removes, in this order:
//   - URLs: user:password@ credentials, the host of anything that is not a known public
//     service (printers, NAS boxes, Tailscale names), every query-string value and fragment
//   - "Bearer/Basic/Digest <credential>" authorisation values
//   - values of secret-looking keys in key=value, "key": "value", key: value, header and
//     --command-line-flag form (access codes, passwords, tokens, API keys, check codes,
//     cookies, serial numbers / dev_id, user ids and names, e-mail, SSID, host / IP keys)
//   - JWTs, e-mail addresses, UUIDs, MAC addresses, printer serial numbers, the device
//     segment of Bambu MQTT topics, IPv4 and IPv6 addresses, LAN host names (*.local,
//     *.lan, *.ts.net ...), and long random-looking tokens
//   - the user name in home-directory paths: C:\Users\<name>\, /Users/<name>/, /home/<name>/
//
// Replacements are short markers such as <ip>, <host>, <serial>, <email>, <user>,
// <redacted>, so a scrubbed line still reads as a log line.

#include <cstddef>
#include <string>

namespace Slic3r {

// Scrubs one piece of text (usually one log line). Input longer than max_len bytes is cut to
// max_len first and marked with "...[truncated]"; 0 means no limit.
std::string scrub_for_crash_report(const std::string& text, size_t max_len = 1024);

} // namespace Slic3r

#endif // slic3r_SentryScrub_hpp_
