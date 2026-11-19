#include <catch2/catch.hpp>

#include <sstream>
#include <string>

#include "slic3r/GUI/HttpServer.hpp"

using Slic3r::GUI::HttpServer;

// The page the system browser shows at the end of a Bambu Lab sign-in through the loopback
// callback. It replaces the redirect to Bambu's studio-callback page, which handed off to Bambu
// Studio's URL scheme (not registered by EdgeSlicer) and ended on a browser error.

namespace {
bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

struct Reply
{
    std::string head, body;
};

Reply render(HttpServer::Response& r)
{
    std::stringstream ss;
    r.write_response(ss);
    const std::string all = ss.str();
    const size_t      sep = all.find("\r\n\r\n");
    REQUIRE(sep != std::string::npos);
    return {all.substr(0, sep + 2), all.substr(sep + 4)};
}

size_t content_length(const std::string& head)
{
    const std::string key = "Content-Length: ";
    const size_t      p   = head.find(key);
    REQUIRE(p != std::string::npos);
    return std::stoul(head.substr(p + key.size()));
}
} // namespace

TEST_CASE("Login result page: success is our own 200 page, not a redirect", "[LoginResultPage]")
{
    HttpServer::ResponseLoginResult page(true);
    const Reply r = render(page);
    CHECK(r.head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK_FALSE(contains(r.head, "Location:"));
    CHECK(contains(r.head, "Content-Type: text/html; charset=utf-8"));
    CHECK(contains(r.head, "Cache-Control: no-store"));
    CHECK(contains(r.head, "Referrer-Policy: no-referrer"));
    CHECK(content_length(r.head) == r.body.size());
    CHECK(contains(r.body, "Signed in to Bambu Lab"));
    CHECK(contains(r.body, "close this tab"));
    CHECK_FALSE(contains(r.body, "Details:"));
}

TEST_CASE("Login result page: failure names our error code", "[LoginResultPage]")
{
    HttpServer::ResponseLoginResult page(false, "get_user_profile_error_-1");
    const Reply r = render(page);
    CHECK(r.head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK_FALSE(contains(r.head, "Location:"));
    CHECK(content_length(r.head) == r.body.size());
    CHECK(contains(r.body, "did not complete"));
    CHECK(contains(r.body, "<code>get_user_profile_error_-1</code>"));

    HttpServer::ResponseLoginFailed bare;
    const Reply b = render(bare);
    CHECK(b.head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(contains(b.body, "did not complete"));
    CHECK_FALSE(contains(b.body, "Details:"));
}

TEST_CASE("Login result page: self-contained and never reflects markup", "[LoginResultPage]")
{
    for (bool ok : {true, false}) {
        const std::string html = HttpServer::ResponseLoginResult::page_html(ok, "x<script>alert(1)</script>\"'&");
        INFO("page: " << html);
        CHECK_FALSE(contains(html, "<script"));
        CHECK_FALSE(contains(html, "http://"));
        CHECK_FALSE(contains(html, "https://"));
        CHECK_FALSE(contains(html, "bambustudio"));
        CHECK(contains(html, "prefers-color-scheme:dark"));
        bool ascii = true;
        for (unsigned char c : html)
            if (c >= 0x80)
                ascii = false;
        CHECK(ascii); // plain ASCII: the symbols are HTML entities
    }
    CHECK(contains(HttpServer::ResponseLoginResult::page_html(false, "a<b>c"), "<code>abc</code>"));
}
