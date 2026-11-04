#pragma once

// What MqttClient may say in the log about its secrets: lengths and presence, never the content.
//
// The SSL constructor used to write the CA certificate, the client certificate, the username and
// the password verbatim at info level. That put the Snapmaker cloud CA, the printer-issued LAN
// certificate and any MQTT password into every log users zip up and share for diagnostics. A
// length is enough to tell "the printer handed us nothing" from "it handed us a certificate".
//
// Header-only and Paho-free so a unit test can check the line without a broker.

#include <string>

namespace Slic3r {
namespace MqttLogRedact {

// "none" for an empty value, otherwise "<n> bytes". Never any of the value itself.
inline std::string describe_secret(const std::string& value)
{
    return value.empty() ? std::string("none") : std::to_string(value.size()) + " bytes";
}

// The info line the SSL constructor logs. The username and password are reported as set or not.
inline std::string ssl_init_summary(const std::string& server_address,
                                    const std::string& client_id,
                                    const std::string& ca_content,
                                    const std::string& cert_content,
                                    const std::string& key_content,
                                    const std::string& username,
                                    const std::string& password)
{
    return "initializing MQTT SSL connection, server_address: " + server_address + ", client_id: " + client_id +
           ", ca: " + describe_secret(ca_content) + ", cert: " + describe_secret(cert_content) +
           ", key: " + describe_secret(key_content) + ", username: " + (username.empty() ? "none" : "set") +
           ", password: " + (password.empty() ? "none" : "set");
}

} // namespace MqttLogRedact
} // namespace Slic3r
