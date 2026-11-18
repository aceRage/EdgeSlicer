#include <catch2/catch.hpp>

#include <string>

#include "slic3r/Utils/NetworkAgent.hpp"

using Slic3r::NetworkAgent;

// What NetworkAgent::get_my_token / get_my_profile log about a failed Bambu sign-in call: the
// response body, short and scrubbed, never the ticket or a token.

namespace {
bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }
} // namespace

TEST_CASE("Login failure log: an empty body stays empty", "[LoginDiagnostics]")
{
    CHECK(NetworkAgent::login_failure_body_for_log("", "Ab12Cd").empty());
}

TEST_CASE("Login failure log: a plain server error stays readable", "[LoginDiagnostics]")
{
    const std::string out = NetworkAgent::login_failure_body_for_log(R"({"code":4,"error":"Ticket is invalid"})", "Ab12Cd");
    CHECK(contains(out, "Ticket is invalid"));
    CHECK(contains(out, "\"code\":4"));
}

TEST_CASE("Login failure log: the ticket and tokens never reach the log", "[LoginDiagnostics]")
{
    const std::string ticket = "Zq81Xw";
    const std::string body   = R"({"message":"ticket Zq81Xw rejected","accessToken":"eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2lnbmF0dXJl",)"
                               R"("refreshToken":"rt-7f3a9c2e5b1d4a8f","account":"someone@example.com"})";
    const std::string out    = NetworkAgent::login_failure_body_for_log(body, ticket);
    INFO("logged: " << out);
    CHECK_FALSE(contains(out, ticket));
    CHECK_FALSE(contains(out, "eyJhbGciOiJIUzI1NiJ9"));
    CHECK_FALSE(contains(out, "rt-7f3a9c2e5b1d4a8f"));
    CHECK_FALSE(contains(out, "someone@example.com"));
    CHECK(contains(out, "<credential>"));
    CHECK(contains(out, "rejected"));
}

TEST_CASE("Login failure log: long bodies are cut and kept on one line", "[LoginDiagnostics]")
{
    std::string body = "<html>\r\n<body>Internal Server Error</body>\r\n";
    body += std::string(2000, 'x');
    const std::string out = NetworkAgent::login_failure_body_for_log(body, "Ab12Cd", 120);
    CHECK(out.size() < 200);
    CHECK_FALSE(contains(out, "\n"));
    CHECK_FALSE(contains(out, "\r"));
    CHECK(contains(out, "Internal Server Error"));
}

// NetworkAgent::body_for_log: what the other calls log of a body or message that can carry a
// printer's LAN access code (the account's printer list, a printer's answer to get_access_code).

TEST_CASE("Body log: the account's printer list loses each printer's access code", "[LoginDiagnostics]")
{
    const std::string body = R"({"message":"success","devices":[{"dev_id":"01P00A451601234","name":"Lab P1S",)"
                             R"("online":true,"dev_access_code":"87654321","dev_model_name":"C12"}]})";
    const std::string out  = NetworkAgent::body_for_log(body);
    INFO("logged: " << out);
    CHECK_FALSE(contains(out, "87654321"));
    CHECK_FALSE(contains(out, "01P00A451601234"));
    CHECK(contains(out, "dev_access_code"));
    CHECK(contains(out, "C12"));
}

TEST_CASE("Body log: a printer's get_access_code answer loses the code, pretty-printed too", "[LoginDiagnostics]")
{
    const std::string compact = R"({"system":{"command":"get_access_code","sequence_id":"7","access_code":"13572468","result":"success"}})";
    const std::string pretty  = "{\n    \"system\": {\n        \"access_code\": \"13572468\",\n        \"command\": \"get_access_code\"\n    }\n}";
    for (const std::string& report : {compact, pretty}) {
        const std::string out = NetworkAgent::body_for_log(report, 0);
        INFO("logged: " << out);
        CHECK_FALSE(contains(out, "13572468"));
        CHECK(contains(out, "get_access_code"));
    }
}

TEST_CASE("Body log: the sign-in data loses its tokens", "[LoginDiagnostics]")
{
    const std::string user_info = R"({"data":{"refresh_token":"rt-1a2b3c4d5e6f7a8b","token":"at-9f8e7d6c5b4a3f2e",)"
                                  R"("expires_in":"7776000","user":{"uid":"1234567","name":"someone"}}})";
    const std::string out       = NetworkAgent::body_for_log(user_info);
    INFO("logged: " << out);
    CHECK_FALSE(contains(out, "rt-1a2b3c4d5e6f7a8b"));
    CHECK_FALSE(contains(out, "at-9f8e7d6c5b4a3f2e"));
    CHECK(contains(out, "expires_in"));
}
