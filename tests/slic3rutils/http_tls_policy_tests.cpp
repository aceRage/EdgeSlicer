// TLS certificate policy of Slic3r::Http (2026-09-22).
//
// Until then every request ran with CURLOPT_SSL_VERIFYPEER/VERIFYHOST off, so the GitHub update
// check, the vendor clouds and everything else accepted any certificate. Now:
//  - TlsPolicy::Auto (the default) verifies internet hosts and leaves loopback / LAN hosts alone,
//  - TlsPolicy::Verify always verifies,
//  - TlsPolicy::PrintHost never does - printers and their LAN services use self-signed certificates.
//
// The first two cases pin the pure policy function. The rest run a real TLS server on 127.0.0.1
// with a certificate made up on the spot, which no system trusts: a Verify request must refuse it,
// a PrintHost (and an Auto) request must still get through, and a Verify request that is handed the
// certificate as its CA must get through too - so the refusal is the certificate check and not some
// other TLS failure.

#include <catch2/catch.hpp>

#include "slic3r/Utils/Http.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

using Slic3r::Http;
using Policy = Http::TlsPolicy;

TEST_CASE("TLS policy: which URLs verify the certificate", "[Http][TlsPolicy]")
{
    SECTION("internet hosts verify under Auto")
    {
        CHECK(Http::tls_verify_for("https://api.github.com/repos/aceRage/EdgeSlicer/releases/latest", Policy::Auto));
        CHECK(Http::tls_verify_for("https://github.com/aceRage/EdgeSlicer/releases/download/v1/x.zip", Policy::Auto));
        CHECK(Http::tls_verify_for("https://objects.githubusercontent.com/github-production-release-asset", Policy::Auto));
        CHECK(Http::tls_verify_for("https://api.bambulab.com/v1/iot-service/api/slicer/resource", Policy::Auto));
        CHECK(Http::tls_verify_for("https://id.snapmaker.com/api", Policy::Auto));
        CHECK(Http::tls_verify_for("https://auth.flashforge.com/", Policy::Auto));
        CHECK(Http::tls_verify_for("HTTPS://API.GITHUB.COM/", Policy::Auto));
        CHECK(Http::tls_verify_for("https://user:secret@octoprint.example.com:5000/api/version", Policy::Auto));
        CHECK(Http::tls_verify_for("https://8.8.8.8/", Policy::Auto));
        CHECK(Http::tls_verify_for("https://172.32.0.1/", Policy::Auto));  // just outside 172.16/12
        CHECK(Http::tls_verify_for("https://172.15.255.1/", Policy::Auto));
        CHECK(Http::tls_verify_for("https://100.128.0.1/", Policy::Auto)); // just outside 100.64/10
        CHECK(Http::tls_verify_for("https://[2606:4700:4700::1111]/", Policy::Auto));
        CHECK(Http::tls_verify_for("https://[::ffff:8.8.8.8]/", Policy::Auto));
        CHECK(Http::tls_verify_for("wss://push.example.com/socket", Policy::Auto));
    }

    SECTION("loopback, private and LAN hosts do not verify under Auto")
    {
        for (const char *url : {
                 "https://127.0.0.1:13640/hub/state", "https://127.8.9.10/", "https://localhost/", "https://LOCALHOST./x",
                 "https://app.localhost/", "https://10.0.0.2/", "https://172.16.5.4/", "https://172.31.255.1/",
                 "https://192.168.1.50:7125/printer/info", "https://169.254.3.4/", "https://100.64.0.1/",
                 "https://100.101.102.103/", "https://0.0.0.0/", "https://[::1]:7125/", "https://[::]/",
                 "https://[fd12:3456:789a::1]/", "https://[fe80::1%25eth0]/", "https://[::ffff:192.168.1.2]/",
                 "https://octopi/", "https://octopi.local/", "https://printer.lan/", "https://u1.home.arpa/",
                 "https://nas.internal/", "https://box.localdomain/", "https://mybox.tail1234.ts.net/",
                 "https://printer.home/" }) {
            INFO(url);
            CHECK_FALSE(Http::tls_verify_for(url, Policy::Auto));
        }
    }

    SECTION("plain http never verifies; Verify and PrintHost override the host")
    {
        CHECK_FALSE(Http::tls_verify_for("http://api.github.com/", Policy::Auto));
        CHECK_FALSE(Http::tls_verify_for("http://api.github.com/", Policy::Verify));
        CHECK_FALSE(Http::tls_verify_for("ws://api.example.com/", Policy::Auto));
        CHECK_FALSE(Http::tls_verify_for("api.github.com/no-scheme", Policy::Auto));
        CHECK(Http::tls_verify_for("https://127.0.0.1/", Policy::Verify));
        CHECK(Http::tls_verify_for("https://octopi.local/", Policy::Verify));
        CHECK_FALSE(Http::tls_verify_for("https://api.github.com/", Policy::PrintHost));
        CHECK_FALSE(Http::tls_verify_for("https://octoprint.example.com/", Policy::PrintHost));
    }
}

TEST_CASE("TLS policy: host extraction", "[Http][TlsPolicy]")
{
    CHECK(Http::url_host("https://User@Host.Example.COM.:443/p?q=1#f") == "host.example.com");
    CHECK(Http::url_host("https://[FE80::1%25eth0]:8443/x") == "fe80::1");
    CHECK(Http::url_host("http://192.168.1.2:7125") == "192.168.1.2");
    CHECK(Http::url_host("https://api.github.com?x=1") == "api.github.com");
    CHECK(Http::url_host("octopi.local/api") == "octopi.local");
    CHECK(Http::tls_host_is_private(""));
    CHECK_FALSE(Http::tls_host_is_private("github.com"));
}

namespace {

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
using asio::ip::tcp;

// A throwaway self-signed certificate for localhost / 127.0.0.1 (EC P-256, valid for a day).
// CA:TRUE so that the same certificate can serve as its own trust anchor via Http::ca_file().
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
        ASN1_INTEGER_set(X509_get_serialNumber(x), 0x5eed);
        X509_gmtime_adj(X509_getm_notBefore(x), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x), 24 * 3600);
        X509_set_pubkey(x, pkey);
        X509_NAME *name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x, name);

        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
        for (const auto &[nid, value] : { std::pair<int, const char *>{ NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1" },
                                          std::pair<int, const char *>{ NID_basic_constraints, "critical,CA:TRUE" } }) {
            X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &v3, nid, const_cast<char *>(value));
            REQUIRE(ext != nullptr);
            X509_add_ext(x, ext, -1);
            X509_EXTENSION_free(ext);
        }
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

// HTTPS on 127.0.0.1:<ephemeral>, answering every request with 200 {"ok":true}. Connections whose
// handshake fails (the client refused the certificate) are counted and dropped.
struct TlsServer
{
    asio::io_context ioc;
    ssl::context     ctx { ssl::context::tls_server };
    tcp::acceptor    acceptor { ioc };
    std::thread      thread;
    unsigned short   port { 0 };
    std::atomic<int> served { 0 };
    std::atomic<int> refused { 0 };

    explicit TlsServer(const SelfSigned &cert)
    {
        ctx.use_certificate_chain(asio::buffer(cert.cert_pem));
        ctx.use_private_key(asio::buffer(cert.key_pem), ssl::context::pem);
        acceptor.open(tcp::v4());
        acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        acceptor.listen();
        port   = acceptor.local_endpoint().port();
        thread = std::thread([this]() {
            for (;;) {
                boost::system::error_code ec;
                tcp::socket socket(ioc);
                acceptor.accept(socket, ec);
                if (ec)
                    return; // closed by the destructor
                ssl::stream<tcp::socket> stream(std::move(socket), ctx);
                stream.handshake(ssl::stream_base::server, ec);
                if (ec) {
                    ++refused;
                    continue;
                }
                std::string request;
                char        buf[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const size_t n = stream.read_some(asio::buffer(buf), ec);
                    if (ec || n == 0)
                        break;
                    request.append(buf, n);
                }
                const std::string body = "{\"ok\":true}";
                const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
                asio::write(stream, asio::buffer(head + body), ec);
                stream.shutdown(ec);
                ++served;
            }
        });
    }

    ~TlsServer()
    {
        boost::system::error_code ig;
        acceptor.close(ig);
        if (thread.joinable())
            thread.join();
    }

    std::string url(const std::string &host = "127.0.0.1") const { return "https://" + host + ":" + std::to_string(port) + "/ping"; }
};

struct Result
{
    unsigned    status { 0 };
    std::string body;
    std::string error;
};

Result fetch(Http &&http)
{
    Result r;
    http.timeout_connect(3)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) { r.body = std::move(body); r.status = status; })
        .on_error([&](std::string body, std::string error, unsigned status) { r.body = std::move(body); r.error = std::move(error); r.status = status; })
        .perform_sync();
    return r;
}

} // namespace

TEST_CASE("TLS policy against a local self-signed server", "[Http][TlsPolicy][socket]")
{
    const SelfSigned cert;
    TlsServer        server(cert);

    SECTION("an internet-policy request refuses the certificate")
    {
        Result r = fetch(std::move(Http::get(server.url()).tls_policy(Policy::Verify)));
        INFO("error: " << r.error);
        CHECK(r.status == 0);
        CHECK(r.body.empty());
        // CURLE_PEER_FAILED_VERIFICATION (60): the certificate check, not a connection problem.
        CHECK(r.error.find("[Error 60]") != std::string::npos);
    }

    SECTION("a print-host request still connects")
    {
        Result r = fetch(std::move(Http::get(server.url()).tls_policy(Policy::PrintHost)));
        INFO("error: " << r.error);
        CHECK(r.status == 200);
        CHECK(r.body == "{\"ok\":true}");
    }

    SECTION("the default policy leaves a loopback host alone")
    {
        Result r = fetch(Http::get(server.url()));
        INFO("error: " << r.error);
        CHECK(r.status == 200);
        CHECK(r.body == "{\"ok\":true}");
    }

    SECTION("verification passes once the certificate is trusted, by name and by IP")
    {
        const boost::filesystem::path ca = boost::filesystem::temp_directory_path() /
                                           boost::filesystem::unique_path("edgeslicer-tls-test-%%%%-%%%%.pem");
        {
            boost::nowide::ofstream f(ca.string(), std::ios::binary);
            f << cert.cert_pem;
        }
        for (const char *host : { "127.0.0.1", "localhost" }) {
            INFO(host);
            Result r = fetch(std::move(Http::get(server.url(host)).tls_policy(Policy::Verify).ca_file(ca.string())));
            INFO("error: " << r.error);
            CHECK(r.status == 200);
            CHECK(r.body == "{\"ok\":true}");
        }
        boost::system::error_code ig;
        boost::filesystem::remove(ca, ig);
    }
}
