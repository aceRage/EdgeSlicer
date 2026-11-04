// The MQTT SSL connect log line (MqttLogRedact): it must say how big the certificates are and
// whether a password was given, and never carry any of their content. Users zip their logs and
// share them; the old line put the cloud CA, the printer's LAN certificate and the password there.

#include <catch2/catch.hpp>

#include "slic3r/Utils/MqttLogRedact.hpp"

using namespace Slic3r::MqttLogRedact;

namespace {
const std::string CA   = "-----BEGIN CERTIFICATE-----\nMIIBcaFAKEcaFAKEcaFAKE\n-----END CERTIFICATE-----\n";
const std::string CERT = "-----BEGIN CERTIFICATE-----\nMIIBcertFAKEcertFAKE\n-----END CERTIFICATE-----\n";
const std::string KEY  = "-----BEGIN PRIVATE KEY-----\nMIIEkeyFAKEkeyFAKE\n-----END PRIVATE KEY-----\n";
const std::string USER = "fake-user-7731";
const std::string PASS = "fake-pass-4242";
} // namespace

TEST_CASE("MqttLogRedact: a secret is described by its length only", "[MqttLogRedact]")
{
    CHECK(describe_secret("") == "none");
    CHECK(describe_secret("abc") == "3 bytes");
    CHECK(describe_secret(CA) == std::to_string(CA.size()) + " bytes");
}

TEST_CASE("MqttLogRedact: the SSL line carries no certificate, key, username or password", "[MqttLogRedact]")
{
    const std::string line = ssl_init_summary("mqtts://192.0.2.10:8883", "client-1", CA, CERT, KEY, USER, PASS);

    CHECK(line.find("BEGIN") == std::string::npos);
    CHECK(line.find("MIIB") == std::string::npos);
    CHECK(line.find("MIIE") == std::string::npos);
    CHECK(line.find(USER) == std::string::npos);
    CHECK(line.find(PASS) == std::string::npos);

    // What is left is still useful for diagnostics.
    CHECK(line.find("mqtts://192.0.2.10:8883") != std::string::npos);
    CHECK(line.find("client-1") != std::string::npos);
    CHECK(line.find("ca: " + std::to_string(CA.size()) + " bytes") != std::string::npos);
    CHECK(line.find("cert: " + std::to_string(CERT.size()) + " bytes") != std::string::npos);
    CHECK(line.find("key: " + std::to_string(KEY.size()) + " bytes") != std::string::npos);
    CHECK(line.find("username: set") != std::string::npos);
    CHECK(line.find("password: set") != std::string::npos);
}

TEST_CASE("MqttLogRedact: missing values read as none", "[MqttLogRedact]")
{
    const std::string line = ssl_init_summary("mqtts://192.0.2.10:8883", "client-1", "", "", "", "", "");
    CHECK(line.find("ca: none") != std::string::npos);
    CHECK(line.find("cert: none") != std::string::npos);
    CHECK(line.find("key: none") != std::string::npos);
    CHECK(line.find("username: none") != std::string::npos);
    CHECK(line.find("password: none") != std::string::npos);
}
