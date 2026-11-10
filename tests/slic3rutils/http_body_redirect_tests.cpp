// Http::follow_redirects(false) and Http::on_body(), which the Home tab's Vendors rely on: a vendor's
// credentials travel in custom headers, and curl hands those to wherever a redirect points, so the
// Vendors engine follows redirects itself (Utils/VendorConnector fetch()); and a downloaded model is
// written to disk as it arrives rather than held in memory.

#include <catch2/catch.hpp>

#include "slic3r/Utils/Http.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

// Answers one connection per canned response, and records how many it accepted. Give it exactly as
// many answers as the test makes requests: it waits for each.
struct LoopbackServer
{
    asio::io_context         ioc;
    tcp::acceptor            acceptor { ioc };
    std::thread              thread;
    std::vector<std::string> responses;
    std::atomic<int>         served { 0 };
    unsigned short           port { 0 };

    explicit LoopbackServer(std::vector<std::string> answers) : responses(std::move(answers))
    {
        acceptor.open(tcp::v4());
        acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        acceptor.listen();
        port   = acceptor.local_endpoint().port();
        thread = std::thread([this]() {
            for (const std::string& answer : responses) {
                boost::system::error_code ec;
                tcp::socket               socket(ioc);
                acceptor.accept(socket, ec);
                if (ec)
                    return;
                std::string request;
                char        buf[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const size_t n = socket.read_some(asio::buffer(buf), ec);
                    if (ec || n == 0)
                        break;
                    request.append(buf, n);
                }
                ++served;
                asio::write(socket, asio::buffer(answer), ec);
                socket.shutdown(tcp::socket::shutdown_both, ec);
            }
        });
    }
    ~LoopbackServer()
    {
        boost::system::error_code ec;
        acceptor.close(ec);
        if (thread.joinable())
            thread.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

std::string ok_response(const std::string& body)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

std::string redirect_response(const std::string& to)
{
    return "HTTP/1.1 302 Found\r\nLocation: " + to + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

// A throwaway self-signed certificate for localhost / 127.0.0.1, the same shape
// http_tls_policy_tests.cpp makes. No system trusts it, and none needs to: TlsPolicy::Auto does not
// verify a loopback host, which is what lets these tests speak TLS to 127.0.0.1.
struct SelfSigned
{
    std::string cert_pem;
    std::string key_pem;

    SelfSigned()
    {
        EVP_PKEY     *pkey = nullptr;
        EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        REQUIRE(kctx != nullptr);
        REQUIRE(EVP_PKEY_keygen_init(kctx) == 1);
        REQUIRE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) == 1);
        REQUIRE(EVP_PKEY_keygen(kctx, &pkey) == 1);
        EVP_PKEY_CTX_free(kctx);

        X509 *x = X509_new();
        X509_set_version(x, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x), 0x5eed01);
        X509_gmtime_adj(X509_getm_notBefore(x), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
        X509_set_pubkey(x, pkey);
        X509_NAME *name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x, name);

        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
        X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name,
                                                 const_cast<char *>("DNS:localhost,IP:127.0.0.1"));
        REQUIRE(ext != nullptr);
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
        REQUIRE(X509_sign(x, pkey, EVP_sha256()) > 0);

        auto to_string = [](BIO *bio) {
            char *data = nullptr;
            long  len  = BIO_get_mem_data(bio, &data);
            std::string out(data, size_t(len));
            BIO_free(bio);
            return out;
        };
        BIO *cb = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(cb, x);
        cert_pem = to_string(cb);
        BIO *kb = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        key_pem = to_string(kb);

        X509_free(x);
        EVP_PKEY_free(pkey);
    }
};

// HTTPS on 127.0.0.1, one canned answer per connection, counting what it accepted.
//
// These cases need TLS and not the plain-HTTP LoopbackServer above: restrict_redirects() holds
// every redirect to https, so from a plain-http start curl refuses the very first hop with
// CURLE_UNSUPPORTED_PROTOCOL and never gets to a second one. Only an https start can reach the
// hop limit or an https -> https redirect at all.
struct TlsRedirectServer
{
    asio::io_context      ioc;
    boost::asio::ssl::context ctx { boost::asio::ssl::context::tls_server };
    tcp::acceptor         acceptor { ioc };
    std::thread           thread;
    std::vector<std::string> responses;
    std::atomic<int>      served { 0 };
    unsigned short        port { 0 };

    explicit TlsRedirectServer(const SelfSigned &cert, std::vector<std::string> answers) : responses(std::move(answers))
    {
        ctx.use_certificate_chain(asio::buffer(cert.cert_pem));
        ctx.use_private_key(asio::buffer(cert.key_pem), boost::asio::ssl::context::pem);
        acceptor.open(tcp::v4());
        acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        acceptor.listen();
        port   = acceptor.local_endpoint().port();
        thread = std::thread([this]() {
            for (const std::string& answer : responses) {
                boost::system::error_code ec;
                tcp::socket               socket(ioc);
                acceptor.accept(socket, ec);
                if (ec)
                    return;
                boost::asio::ssl::stream<tcp::socket> stream(std::move(socket), ctx);
                stream.handshake(boost::asio::ssl::stream_base::server, ec);
                if (ec)
                    return;
                std::string request;
                char        buf[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const size_t n = stream.read_some(asio::buffer(buf), ec);
                    if (ec || n == 0)
                        break;
                    request.append(buf, n);
                }
                ++served;
                asio::write(stream, asio::buffer(answer), ec);
                stream.shutdown(ec);
            }
        });
    }
    ~TlsRedirectServer()
    {
        boost::system::error_code ec;
        acceptor.close(ec);
        if (thread.joinable())
            thread.join();
    }
    std::string url(const std::string& path) const { return "https://127.0.0.1:" + std::to_string(port) + path; }
};

} // namespace

TEST_CASE("http: a redirect is reported, not followed, when asked", "[Http]")
{
    // One answer only: a client that followed the redirect would get no answer and fail.
    LoopbackServer server({"HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    unsigned    status = 0;
    std::string body, headers;
    Slic3r::Http::get(server.url("/start"))
        .follow_redirects(false)
        .on_header_callback([&headers](std::string h) { headers = std::move(h); })
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string, unsigned s) { body = std::move(b); status = s; })
        .perform_sync();
    CHECK(status == 302);
    CHECK(body.empty());
    CHECK(headers.find("Location: /elsewhere") != std::string::npos);
    CHECK(server.served == 1);
}

TEST_CASE("http: restrict_redirects refuses a redirect off https", "[Http]")
{
    // Two answers: a client that followed the redirect would take the second one. A restricted
    // request must stop at the first, with an error and no completion.
    LoopbackServer server({"HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                           ok_response("followed")});
    std::string body, error;
    unsigned    status = 0;
    bool        completed = false;
    Slic3r::Http::get(server.url("/start"))
        .restrict_redirects(10)
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; completed = true; })
        .on_error([&](std::string b, std::string e, unsigned s) { body = std::move(b); error = std::move(e); status = s; })
        .perform_sync();
    CHECK_FALSE(completed);
    CHECK_FALSE(error.empty());
    CHECK(server.served == 1);
}

TEST_CASE("http: restrict_redirects stops at its hop limit", "[Http]")
{
    // Two 302s and then a 200, with room for only one hop. curl must stop at the second answer
    // rather than following it, and it must have asked the server exactly twice: once for /start
    // and once for /one. Three hits would mean the limit was off by one; one would mean the first
    // hop never happened. Over TLS, because from a plain-http start restrict_redirects() refuses
    // the first hop at the protocol level and the limit is never reached.
    SelfSigned                cert;
    TlsRedirectServer         server(cert, {redirect_response("/one"), redirect_response("/two"), ok_response("too far")});
    std::string               body, error;
    unsigned                  status    = 0;
    bool                      completed = false;
    Slic3r::Http::get(server.url("/start"))
        .restrict_redirects(1)
        .on_complete([&](std::string b, unsigned st) { body = std::move(b); status = st; completed = true; })
        .on_error([&](std::string b, std::string e, unsigned st) { body = std::move(b); error = std::move(e); status = st; })
        .perform_sync();
    CHECK_FALSE(completed);
    CHECK_FALSE(error.empty());
    CHECK(server.served == 2);
}

TEST_CASE("http: restrict_redirects refuses where an https transfer ended on loopback", "[Http]")
{
    // The end-to-end version of the post-transfer check: an https -> https redirect that stays on
    // 127.0.0.1, which curl follows happily and which the protocol cap alone does not stop. The
    // body arrives from the second answer before the refusal is decided, so this is the one case
    // where an refused transfer did have an answer.
    SelfSigned        cert;
    TlsRedirectServer server(cert, {redirect_response("/elsewhere"), ok_response("payload")});
    std::string       body, error;
    unsigned          status    = 0;
    bool              completed = false;
    Slic3r::Http::get(server.url("/start"))
        // No https requirement on the hop for this one: 127.0.0.1 is https on both legs, so the
        // cap is satisfied and it is the post-transfer host check that has to catch it.
        .restrict_redirects(10)
        .on_complete([&](std::string b, unsigned st) { body = std::move(b); status = st; completed = true; })
        .on_error([&](std::string b, std::string e, unsigned st) { body = std::move(b); error = std::move(e); status = st; })
        .perform_sync();
    CHECK_FALSE(completed);
    CHECK_FALSE(error.empty());
    CHECK(error.find("refused") != std::string::npos);
    CHECK(Slic3r::Http::error_is_redirect_refusal(error));
    CHECK(server.served == 2);
}

TEST_CASE("http: the post-transfer redirect check on its own", "[Http]")
{
    // redirect_end_refusal() is the function http_perform() calls after a restricted transfer, so
    // this exercises the exact logic without needing a server that can be made to answer a bad
    // address. The cases are the ways an effective URL can be unusable.
    using Slic3r::Http;

    // Nothing to re-check: no redirect happened, so the caller's own check of the address stands.
    CHECK(Http::redirect_end_refusal(0, "https://files.printables.com/a.3mf").empty());
    CHECK(Http::redirect_end_refusal(0, "http://192.168.1.9/a.3mf").empty());

    // A redirect that ended somewhere fine.
    CHECK(Http::redirect_end_refusal(2, "https://cdn.example.org/a.3mf").empty());

    // A redirect that ended on this machine or the LAN, an address hiding its host, or a scheme
    // that is not https.
    for (const char *u : {"https://127.0.0.1/a.3mf", "https://localhost/a.3mf", "https://[::1]/a.3mf",
                          "https://192.168.1.20/a.3mf", "https://169.254.169.254/latest/meta-data/",
                          "https://printer.local/a.3mf", "https://a@b@127.0.0.1/a.3mf",
                          "http://files.printables.com/a.3mf", "file:///C:/Windows/evil.3mf",
                          "ftp://files.printables.com/a.3mf"}) {
        INFO(u);
        CHECK_FALSE(Http::redirect_end_refusal(1, u).empty());
    }

    // curl reports no effective URL, which is not something to treat as a success.
    CHECK_FALSE(Http::redirect_end_refusal(1, "").empty());

    // The refusal is recognisable as one, so a retry loop can tell it from a transport failure.
    CHECK(Http::error_is_redirect_refusal("the download was refused: " + Http::redirect_end_refusal(1, "https://127.0.0.1/x")));
    CHECK_FALSE(Http::error_is_redirect_refusal("curl:Timeout was reached"));
    CHECK_FALSE(Http::error_is_redirect_refusal(""));
}

TEST_CASE("http: on_body takes the body in pieces", "[Http]")
{
    const std::string payload(300000, 'x');

    SECTION("all of it, and on_complete gets none")
    {
        LoopbackServer server({ok_response(payload)});
        size_t         got = 0;
        unsigned       seen_status = 0;
        std::string    completed = "unset";
        Slic3r::Http::get(server.url("/file"))
            .on_body([&](unsigned s, const char*, size_t n) { seen_status = s; got += n; return true; })
            .on_complete([&](std::string b, unsigned) { completed = std::move(b); })
            .perform_sync();
        CHECK(got == payload.size());
        CHECK(seen_status == 200);
        CHECK(completed.empty());
    }
    SECTION("false stops the transfer")
    {
        LoopbackServer server({ok_response(payload)});
        std::string    error;
        Slic3r::Http::get(server.url("/file"))
            .on_body([](unsigned, const char*, size_t) { return false; })
            .on_error([&error](std::string, std::string e, unsigned) { error = std::move(e); })
            .perform_sync();
        CHECK(error == "The transfer was stopped");
    }
    SECTION("the size limit still holds")
    {
        LoopbackServer server({ok_response(payload)});
        std::string    error;
        size_t         got = 0;
        Slic3r::Http::get(server.url("/file"))
            .size_limit(1000)
            .on_body([&got](unsigned, const char*, size_t n) { got += n; return true; })
            .on_error([&error](std::string, std::string e, unsigned) { error = std::move(e); })
            .perform_sync();
        CHECK(got <= 1000);
        CHECK(error.find("limit") != std::string::npos);
    }
}
