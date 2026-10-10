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
