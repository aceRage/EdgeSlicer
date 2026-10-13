#include <catch2/catch.hpp>

#include "slic3r/Utils/VaultProvider.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Vaults;
using json = nlohmann::json;

// The Home tab's Vaults engine, run against stand-in servers that answer the way WebDAV (Synology's
// WebDAV Server, Apache mod_dav), Azure Blob storage and S3 do. Nothing here touches the network. The
// slicer (src/slic3r/GUI/HomeVaults.cpp) makes the real HTTP calls and keeps the secrets in the OS
// credential store.

namespace {

const int64_t NOW = 1791849600; // 2026-10-13 00:00:00 UTC

std::string header(const Request& r, const std::string& name)
{
    for (const auto& [k, v] : r.headers)
        if (k.size() == name.size() && std::equal(k.begin(), k.end(), name.begin(), [](char a, char b) { return std::tolower((unsigned char) a) == std::tolower((unsigned char) b); }))
            return v;
    return std::string();
}

// A stand-in server: answers by "METHOD url", records what it was asked.
struct Server
{
    std::map<std::string, Response> routes;
    std::vector<Request>            seen;
    HttpFn fn()
    {
        return [this](const Request& r) {
            seen.push_back(r);
            auto it = routes.find(r.method + " " + r.url);
            if (it == routes.end()) {
                Response nf;
                nf.status = 404;
                return nf;
            }
            Response resp = it->second;
            if (r.sink && resp.status >= 200 && resp.status < 300) {
                r.sink(resp.body.data(), resp.body.size());
                resp.body.clear();
            }
            return resp;
        };
    }
    void add(const std::string& key, int status, const std::string& body, std::map<std::string, std::string> headers = {})
    {
        Response r;
        r.status  = status;
        r.body    = body;
        r.headers = std::move(headers);
        routes[key] = r;
    }
};

Spec webdav_spec(const std::string& url = "https://nas.example.com:5006/3d/", const std::string& auth = "basic")
{
    return spec_from_json({{"name", "DiskStation"}, {"kind", "webdav"}, {"url", url}, {"auth", auth}});
}

std::string dav_response(const std::string& href, bool collection, int64_t size = 0, const std::string& modified = "", const std::string& prefix = "D")
{
    const std::string p = prefix + ":";
    std::string       r = "<" + p + "response><" + p + "href>" + href + "</" + p + "href><" + p + "propstat><" + p + "prop>";
    r += collection ? "<" + p + "resourcetype><" + p + "collection/></" + p + "resourcetype>" : "<" + p + "resourcetype/>";
    if (!collection) {
        r += "<" + p + "getcontentlength>" + std::to_string(size) + "</" + p + "getcontentlength>";
        if (!modified.empty())
            r += "<" + p + "getlastmodified>" + modified + "</" + p + "getlastmodified>";
        r += "<" + p + "getetag>\"e-" + std::to_string(size) + "\"</" + p + "getetag>";
    }
    r += "</" + p + "prop><" + p + "status>HTTP/1.1 200 OK</" + p + "status></" + p + "propstat></" + p + "response>";
    return r;
}

std::string multistatus(const std::string& responses, const std::string& prefix = "D")
{
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><" + prefix + ":multistatus xmlns:" + prefix + "=\"DAV:\">" + responses + "</" +
           prefix + ":multistatus>";
}

} // namespace

// ------------------------------------------------------------------------------------------- spec ----

TEST_CASE("vaults: a spec is checked and kept without secrets", "[Vaults]")
{
    const Spec s = webdav_spec("https://nas.example.com:5006/3d");
    CHECK(s.kind == Kind::WebDav);
    CHECK(s.url == "https://nas.example.com:5006/3d/"); // a WebDAV folder ends in '/'
    CHECK(s.auth == "basic");
    CHECK(s.recursive);
    CHECK(s.max_depth == 6);
    const json j = spec_to_json(s);
    CHECK(j["format"] == "edgeslicer-vault");
    CHECK(j["version"] == 1);
    CHECK_FALSE(j.contains("password"));
    const Spec back = spec_from_json(j);
    CHECK(back.url == s.url);
    CHECK(back.name == "DiskStation");

    SECTION("S3 needs a bucket; region and addressing are kept")
    {
        CHECK_THROWS(spec_from_json({{"name", "B"}, {"kind", "s3"}, {"url", "https://s3.eu-west-1.amazonaws.com"}}));
        const Spec s3 = spec_from_json({{"name", "B"}, {"kind", "s3"}, {"url", "https://s3.eu-west-1.amazonaws.com/"}, {"bucket", "My.Models"},
                                        {"region", "eu-west-1"}, {"addressing", "virtual"}, {"prefix", "/prints/"}});
        CHECK(s3.bucket == "my.models");
        CHECK(s3.url == "https://s3.eu-west-1.amazonaws.com");
        CHECK(s3.addressing == "virtual");
        CHECK(s3.prefix == "prints/");
        CHECK(s3.auth == "sigv4");
    }
    SECTION("Azure: the container is in the address, the SAS never is")
    {
        CHECK_THROWS_WITH(spec_from_json({{"name", "A"}, {"kind", "azure_blob"}, {"url", "https://acct.blob.core.windows.net/models?sv=1&sig=x"}}),
                          Catch::Matchers::Contains("Set SAS link"));
        CHECK_THROWS(spec_from_json({{"name", "A"}, {"kind", "azure_blob"}, {"url", "https://acct.blob.core.windows.net/"}}));
        const Spec a = spec_from_json({{"name", "A"}, {"kind", "azure_blob"}, {"url", "https://acct.blob.core.windows.net/models/"}});
        CHECK(a.url == "https://acct.blob.core.windows.net/models");
        CHECK(a.auth == "sas");
    }
    SECTION("refused")
    {
        CHECK_THROWS_WITH(spec_from_json({{"name", "X"}, {"kind", "ftp"}, {"url", "https://a.example"}}), Catch::Matchers::Contains("Unknown kind"));
        CHECK_THROWS_WITH(spec_from_json({{"format", "edgeslicer-vault"}, {"version", 2}, {"name", "X"}, {"kind", "webdav"}, {"url", "https://a.example"}}),
                          Catch::Matchers::Contains("newer"));
        CHECK_THROWS_WITH(spec_from_json({{"name", "X"}, {"kind", "webdav"}, {"url", "http://files.example.com/dav/"}}), Catch::Matchers::Contains("local network"));
        CHECK_THROWS_WITH(spec_from_json({{"name", "X"}, {"kind", "webdav"}, {"url", "https://me:pw@nas.example.com/"}}), Catch::Matchers::Contains("credentials"));
        CHECK_THROWS(spec_from_json({{"name", ""}, {"kind", "webdav"}, {"url", "https://a.example"}}));
        CHECK_THROWS(spec_from_json({{"name", "X"}, {"kind", "webdav"}, {"url", "https://a.example"}, {"auth", "digest"}}));
        CHECK_THROWS(spec_from_json({{"name", "X"}, {"kind", "webdav"}, {"url", "file:///C:/models"}}));
    }
    SECTION("depth is clamped, a bad builtin id dropped")
    {
        const Spec d = spec_from_json({{"name", "X"}, {"kind", "webdav"}, {"url", "https://a.example/"}, {"max_depth", 99}, {"builtin", "../evil"}});
        CHECK(d.max_depth == 20);
        CHECK(d.builtin.empty());
    }
}

TEST_CASE("vaults: secret slots by kind and sign-in", "[Vaults]")
{
    CHECK(secret_slots(webdav_spec()).size() == 2);
    CHECK(secret_slots(webdav_spec("https://a.example/", "bearer")).front().first == "token");
    CHECK(secret_slots(webdav_spec("https://a.example/", "none")).empty());
    const Spec s3 = spec_from_json({{"name", "B"}, {"kind", "s3"}, {"url", "https://s3.amazonaws.com"}, {"bucket", "models"}});
    CHECK(secret_slots(s3).size() == 2);
    CHECK(secret_slots(s3)[0].first == "access_key_id");
    const Spec az = spec_from_json({{"name", "A"}, {"kind", "azure_blob"}, {"url", "https://a.blob.core.windows.net/c"}});
    CHECK(secret_slots(az).front().first == "sas");
}

TEST_CASE("vaults: plain http only on the local network", "[Vaults]")
{
    CHECK(address_check("https://files.example.com/dav/") == Transport::Https);
    CHECK(address_check("http://192.168.1.20:5005/3d/") == Transport::LanHttp);
    CHECK(address_check("http://10.0.0.5/") == Transport::LanHttp);
    CHECK(address_check("http://172.20.1.1/") == Transport::LanHttp);
    CHECK(address_check("http://100.101.102.103/") == Transport::LanHttp); // Tailscale (CGNAT)
    CHECK(address_check("http://diskstation.local:5005/") == Transport::LanHttp);
    CHECK(address_check("http://diskstation:5005/") == Transport::LanHttp);
    CHECK(address_check("http://nas.home.arpa/") == Transport::LanHttp);
    CHECK(address_check("http://[fd12:3456::1]/") == Transport::LanHttp);
    CHECK(address_check("http://localhost:9000/") == Transport::LanHttp);
    CHECK(address_check("http://8.8.8.8/") == Transport::Refused);
    CHECK(address_check("http://172.32.0.1/") == Transport::Refused);
    CHECK(address_check("http://nas.example.com/") == Transport::Refused);
    CHECK(address_check("http://[2001:db8::1]/") == Transport::Refused);
    CHECK(address_check("ftp://nas.local/") == Transport::Refused);
    CHECK(address_check("https://nas.local/?x=1") == Transport::Refused);
    CHECK(display_address("https://nas.example.com:5006/3d/") == "https://nas.example.com:5006/3d/");
    CHECK(origin_of("https://NAS.example.com:443/a") == "https://nas.example.com");
}

// ----------------------------------------------------------------------------------------- WebDAV ----

TEST_CASE("vaults: WebDAV lists model files folder by folder", "[Vaults][WebDAV]")
{
    Server srv;
    // The root: itself, a file with a space and an umlaut, a non-model file, two folders, and hrefs that
    // must be ignored (another host, a path outside the vault, a dot-dot path).
    srv.add("PROPFIND https://nas.example.com:5006/3d/", 207,
            multistatus(dav_response("/3d/", true) + dav_response("/3d/arm%20v2.3mf", false, 1234, "Tue, 13 Oct 2026 08:00:00 GMT") +
                        dav_response("/3d/notes.txt", false, 10) + dav_response("/3d/parts/", true) + dav_response("/3d/deep/", true) +
                        dav_response("https://evil.example.com/3d/x.stl", false, 5) + dav_response("/other/y.stl", false, 5) +
                        dav_response("/3d/../etc/z.stl", false, 5) + dav_response("https://nas.example.com:5006/3d/K%C3%A4fer.stl", false, 99)));
    // A subfolder answering with another namespace prefix and absolute hrefs.
    srv.add("PROPFIND https://nas.example.com:5006/3d/parts/", 207,
            multistatus(dav_response("/3d/parts/", true, 0, "", "d") + dav_response("/3d/parts/gear.STL", false, 77, "", "d"), "d"));
    srv.add("PROPFIND https://nas.example.com:5006/3d/deep/", 207, multistatus(dav_response("/3d/deep/", true) + dav_response("/3d/deep/more/", true)));
    srv.add("PROPFIND https://nas.example.com:5006/3d/deep/more/", 207, multistatus(dav_response("/3d/deep/more/", true) + dav_response("/3d/deep/more/far.obj", false, 3)));

    Spec spec = webdav_spec();
    const Secrets secrets {{"user", "anna"}, {"password", "s3cr3t!"}};
    std::atomic<bool> cancel { false };
    const Listing l = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW);
    REQUIRE(l.ok);
    CHECK_FALSE(l.partial);
    std::map<std::string, File> by_id;
    for (const File& f : l.files)
        by_id[f.id] = f;
    CHECK(by_id.size() == 4);
    REQUIRE(by_id.count("arm v2.3mf"));
    CHECK(by_id["arm v2.3mf"].size == 1234);
    CHECK(by_id["arm v2.3mf"].mtime == 1791878400);
    CHECK(by_id["arm v2.3mf"].folder.empty());
    CHECK(by_id.count("K\xC3\xA4" "fer.stl"));
    REQUIRE(by_id.count("parts/gear.STL"));
    CHECK(by_id["parts/gear.STL"].folder == "parts");
    CHECK(by_id["parts/gear.STL"].name == "gear.STL");
    CHECK(by_id.count("deep/more/far.obj"));
    CHECK_FALSE(by_id.count("notes.txt"));
    for (const auto& [id, f] : by_id)
        CHECK(id.find("..") == std::string::npos);
    // Every request was a PROPFIND Depth 1 with the Basic credentials, to the vault's own host only.
    for (const Request& r : srv.seen) {
        CHECK(r.method == "PROPFIND");
        CHECK(header(r, "Depth") == "1");
        CHECK(header(r, "Authorization") == "Basic YW5uYTpzM2NyM3Qh");
        CHECK(r.url.rfind("https://nas.example.com:5006/3d/", 0) == 0);
        CHECK(r.body.find("propfind") != std::string::npos);
    }

    SECTION("depth and recursion limits")
    {
        spec.max_depth = 1;
        srv.seen.clear();
        const Listing l1 = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW);
        CHECK(l1.files.size() == 3); // not deep/more/far.obj
        spec.recursive = false;
        srv.seen.clear();
        const Listing l0 = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW);
        CHECK(l0.files.size() == 2);
        CHECK(srv.seen.size() == 1);
    }
    SECTION("download request")
    {
        File f;
        f.id = "parts/gear v2.stl";
        const Request d = make_provider(spec)->download_request(secrets, f, NOW);
        CHECK(d.method == "GET");
        CHECK(d.url == "https://nas.example.com:5006/3d/parts/gear%20v2.stl");
        CHECK(header(d, "Authorization") == "Basic YW5uYTpzM2NyM3Qh");
    }
    SECTION("the file and request limits stop early with what came")
    {
        ListOptions o;
        o.max_files = 2;
        const Listing lf = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW, o);
        CHECK(lf.ok);
        CHECK(lf.partial);
        CHECK(lf.files.size() == 2);
        o           = ListOptions();
        o.max_requests = 1;
        const Listing lr = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW, o);
        CHECK(lr.partial);
        CHECK(lr.requests == 1);
    }
}

TEST_CASE("vaults: WebDAV errors are said plainly and never hold a secret", "[Vaults][WebDAV]")
{
    Server srv;
    srv.add("PROPFIND https://nas.example.com:5006/3d/", 401, "<html>login</html>");
    const Secrets     secrets {{"user", "anna"}, {"password", "s3cr3t!"}};
    std::atomic<bool> cancel { false };
    Listing           l = make_provider(webdav_spec())->list(secrets, srv.fn(), cancel, NOW);
    CHECK_FALSE(l.ok);
    CHECK(l.error.find("401") != std::string::npos);
    CHECK(l.error.find("s3cr3t") == std::string::npos);

    l = make_provider(webdav_spec())->list({{"user", "anna"}}, srv.fn(), cancel, NOW);
    CHECK(l.error == "Set the user name and password first.");

    srv.add("PROPFIND https://nas.example.com:5006/3d/", 207, "<D:multistatus xmlns:D=\"DAV:\"><D:response>");
    l = make_provider(webdav_spec())->list(secrets, srv.fn(), cancel, NOW);
    CHECK_FALSE(l.ok);
    CHECK(l.error.find("WebDAV") != std::string::npos);

    srv.add("PROPFIND https://nas.example.com:5006/3d/", 405, "");
    const TestResult t = test_connection(webdav_spec(), secrets, srv.fn(), NOW);
    CHECK_FALSE(t.ok);
    CHECK(t.error.find("WebDAV") != std::string::npos);
}

TEST_CASE("vaults: redirects keep credentials on the vault's own server", "[Vaults]")
{
    Server srv;
    srv.add("GET https://nas.example.com/3d/a.stl", 302, "", {{"location", "https://cdn.example.net/a.stl?sig=1"}});
    srv.add("GET https://cdn.example.net/a.stl?sig=1", 200, "solid a");
    srv.add("GET https://nas.example.com/3d/b.stl", 301, "", {{"location", "/3d/b2.stl"}});
    srv.add("GET https://nas.example.com/3d/b2.stl", 200, "solid b");
    srv.add("GET https://nas.example.com/3d/c.stl", 302, "", {{"location", "http://8.8.8.8/c.stl"}});

    Request r;
    r.url = "https://nas.example.com/3d/a.stl";
    r.headers.emplace_back("Authorization", "Basic xyz");
    Response resp = fetch(srv.fn(), r);
    CHECK(resp.status == 200);
    REQUIRE(srv.seen.size() == 2);
    CHECK(header(srv.seen[1], "Authorization").empty());

    srv.seen.clear();
    r.url = "https://nas.example.com/3d/b.stl";
    resp  = fetch(srv.fn(), r);
    CHECK(resp.status == 200);
    REQUIRE(srv.seen.size() == 2);
    CHECK(header(srv.seen[1], "Authorization") == "Basic xyz");

    r.url = "https://nas.example.com/3d/c.stl";
    resp  = fetch(srv.fn(), r);
    CHECK(resp.status == 0);
    CHECK(resp.error.find("not allowed") != std::string::npos);
}

// ------------------------------------------------------------------------------------- Azure Blob ----

TEST_CASE("vaults: Azure Blob lists a container page by page with the SAS", "[Vaults][Azure]")
{
    const std::string sas = "sv=2022-11-02&sr=c&sp=rl&se=2026-12-31T00%3A00%3A00Z&sig=abc%2Bdef%3D";
    Server            srv;
    srv.add("GET https://acct.blob.core.windows.net/models?restype=container&comp=list&maxresults=5000&prefix=prints%2F&" + sas, 200,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?><EnumerationResults ContainerName=\"models\"><Prefix>prints/</Prefix><Blobs>"
            "<Blob><Name>prints/arm.3mf</Name><Properties><Last-Modified>Tue, 13 Oct 2026 08:00:00 GMT</Last-Modified><Etag>0x1</Etag>"
            "<Content-Length>2048</Content-Length></Properties></Blob>"
            "<Blob><Name>prints/readme.md</Name><Properties><Content-Length>5</Content-Length></Properties></Blob>"
            "</Blobs><NextMarker>2!72!MDAw</NextMarker></EnumerationResults>");
    srv.add("GET https://acct.blob.core.windows.net/models?restype=container&comp=list&maxresults=5000&prefix=prints%2F&marker=2%2172%21MDAw&" + sas,
            200,
            "<EnumerationResults><Blobs><Blob><Name>prints/tools/clip &amp; hook.stl</Name><Properties><Content-Length>99</Content-Length>"
            "</Properties></Blob></Blobs><NextMarker/></EnumerationResults>");
    const Spec spec = spec_from_json({{"name", "Azure"}, {"kind", "azure_blob"}, {"url", "https://acct.blob.core.windows.net/models"}, {"prefix", "prints/"}});
    const Secrets     secrets {{"sas", "?" + sas}};
    std::atomic<bool> cancel { false };
    const Listing     l = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW);
    REQUIRE(l.ok);
    REQUIRE(l.files.size() == 2);
    CHECK(l.files[0].id == "prints/arm.3mf");
    CHECK(l.files[0].folder.empty());
    CHECK(l.files[0].size == 2048);
    CHECK(l.files[0].mtime == 1791878400);
    CHECK(l.files[1].id == "prints/tools/clip & hook.stl");
    CHECK(l.files[1].folder == "tools");
    CHECK(l.requests == 2);
    CHECK(header(srv.seen[0], "x-ms-version") == "2021-12-02");

    const Request d = make_provider(spec)->download_request(secrets, l.files[1], NOW);
    CHECK(d.url == "https://acct.blob.core.windows.net/models/prints/tools/clip%20%26%20hook.stl?" + sas);

    SECTION("without the SAS set")
    {
        const Listing e = make_provider(spec)->list({}, srv.fn(), cancel, NOW);
        CHECK(e.error == "Set the SAS link first.");
    }
    SECTION("an Azure error code is shown, the token is not")
    {
        Server bad;
        bad.add("GET https://acct.blob.core.windows.net/models?restype=container&comp=list&maxresults=5000&prefix=prints%2F&" + sas, 403,
                "<?xml version=\"1.0\"?><Error><Code>AuthenticationFailed</Code><Message>sig=abc did not match</Message></Error>");
        const Listing e = make_provider(spec)->list(secrets, bad.fn(), cancel, NOW);
        CHECK(e.error.find("AuthenticationFailed") != std::string::npos);
        CHECK(e.error.find("sig") == std::string::npos);
    }
}

TEST_CASE("vaults: a pasted SAS link is split and checked", "[Vaults][Azure]")
{
    std::string base, token;
    REQUIRE(split_sas_url("  https://acct.blob.core.windows.net/models/?sv=2022-11-02&sr=c&sp=rl&sig=x  ", base, token));
    CHECK(base == "https://acct.blob.core.windows.net/models");
    CHECK(token == "sv=2022-11-02&sr=c&sp=rl&sig=x");
    REQUIRE(split_sas_url("?sv=1&sig=y", base, token));
    CHECK(base.empty());
    CHECK(token == "sv=1&sig=y");
    CHECK_FALSE(split_sas_url("https://acct.blob.core.windows.net/models", base, token));

    const SasCheck good = check_sas("sv=2022-11-02&sr=c&sp=rl&se=2026-12-31T00%3A00%3A00Z&sig=x", NOW);
    CHECK(good.ok);
    CHECK(good.warnings.empty());
    CHECK(good.expires_at == 1798675200);

    CHECK(check_sas("sv=1&sp=rl&sr=c", NOW).error.find("signature") != std::string::npos);
    CHECK(check_sas("sv=1&ss=b&srt=sco&sp=rl&sig=x", NOW).error.find("account SAS") != std::string::npos);
    CHECK(check_sas("sv=1&sr=b&sp=r&sig=x", NOW).error.find("one file") != std::string::npos);
    CHECK(check_sas("sv=1&sr=c&sp=r&sig=x", NOW).error.find("Read and List") != std::string::npos);
    CHECK(check_sas("sv=1&sr=c&sp=rl&se=2026-10-01&sig=x", NOW).error.find("expired") != std::string::npos);

    const SasCheck wide = check_sas("sv=1&sr=c&sp=racwdl&se=2026-10-15T00:00:00Z&sig=x", NOW);
    CHECK(wide.ok);
    REQUIRE(wide.warnings.size() == 2);
    CHECK(wide.warnings[0].find("change or delete") != std::string::npos);
    CHECK(wide.warnings[1].find("expires on 2026-10-15") != std::string::npos);
}

// --------------------------------------------------------------------------------------------- S3 ----

TEST_CASE("vaults: SigV4 matches AWS's published examples", "[Vaults][S3]")
{
    // https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
    const std::string ak = "AKIAIOSFODNN7EXAMPLE", sk = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    CHECK(amz_date(1369353600) == "20130524T000000Z");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    Request get;
    get.url = "https://examplebucket.s3.amazonaws.com/test.txt";
    get.headers.emplace_back("Range", "bytes=0-9");
    sign_v4(get, ak, sk, "us-east-1", "20130524T000000Z");
    CHECK(header(get, "Authorization") ==
          "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request,"
          "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,"
          "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");

    Request list;
    list.url = "https://examplebucket.s3.amazonaws.com/?max-keys=2&prefix=J";
    sign_v4(list, ak, sk, "us-east-1", "20130524T000000Z");
    CHECK(header(list, "Authorization").find("Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7") != std::string::npos);

    Request lifecycle;
    lifecycle.url = "https://examplebucket.s3.amazonaws.com/?lifecycle=";
    sign_v4(lifecycle, ak, sk, "us-east-1", "20130524T000000Z");
    CHECK(header(lifecycle, "Authorization").find("Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543") != std::string::npos);

    CHECK(uri_encode("a b/c~d+e", true) == "a%20b/c~d%2Be");
    CHECK(uri_encode("a/b", false) == "a%2Fb");
}

TEST_CASE("vaults: S3 lists a bucket with continuation tokens, signed", "[Vaults][S3]")
{
    Server srv;
    srv.add("GET https://minio.lan:9000/models/?list-type=2&max-keys=1000&prefix=3d%2F", 200,
            "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><IsTruncated>true</IsTruncated>"
            "<Contents><Key>3d/benchy.3mf</Key><LastModified>2026-10-13T08:00:00.000Z</LastModified><ETag>&quot;abc&quot;</ETag><Size>5000</Size></Contents>"
            "<Contents><Key>3d/a/b/c/d/e/f/g/too-deep.stl</Key><Size>1</Size></Contents>"
            "<NextContinuationToken>1/2+3=</NextContinuationToken></ListBucketResult>");
    srv.add("GET https://minio.lan:9000/models/?continuation-token=1%2F2%2B3%3D&list-type=2&max-keys=1000&prefix=3d%2F", 200,
            "<ListBucketResult><IsTruncated>false</IsTruncated><Contents><Key>3d/parts/gear.step</Key><Size>42</Size></Contents></ListBucketResult>");
    const Spec spec = spec_from_json({{"name", "MinIO"}, {"kind", "s3"}, {"url", "http://minio.lan:9000"}, {"bucket", "models"}, {"prefix", "3d/"}});
    const Secrets     secrets {{"access_key_id", "AKIDEXAMPLE"}, {"secret_access_key", "SECRETEXAMPLE"}};
    std::atomic<bool> cancel { false };
    const Listing     l = make_provider(spec)->list(secrets, srv.fn(), cancel, NOW);
    REQUIRE(l.ok);
    REQUIRE(l.files.size() == 2);
    CHECK(l.files[0].id == "3d/benchy.3mf");
    CHECK(l.files[0].etag == "\"abc\"");
    CHECK(l.files[0].mtime == 1791878400);
    CHECK(l.files[1].id == "3d/parts/gear.step");
    CHECK(l.files[1].folder == "parts");
    for (const Request& r : srv.seen) {
        CHECK(header(r, "Authorization").rfind("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20261013/us-east-1/s3/aws4_request", 0) == 0);
        CHECK(header(r, "x-amz-date") == "20261013T000000Z");
        CHECK(header(r, "Authorization").find("SECRETEXAMPLE") == std::string::npos);
    }
    const Request d = make_provider(spec)->download_request(secrets, l.files[1], NOW);
    CHECK(d.url == "http://minio.lan:9000/models/3d/parts/gear.step");
    CHECK_FALSE(header(d, "Authorization").empty());

    SECTION("virtual-hosted addressing and a public bucket")
    {
        Spec v = spec_from_json({{"name", "R2"}, {"kind", "s3"}, {"url", "https://acct.r2.cloudflarestorage.com"}, {"bucket", "prints"},
                                 {"region", "auto"}, {"addressing", "virtual"}, {"auth", "none"}});
        File f;
        f.id = "x.stl";
        const Request pub = make_provider(v)->download_request({}, f, NOW);
        CHECK(pub.url == "https://prints.acct.r2.cloudflarestorage.com/x.stl");
        CHECK(header(pub, "Authorization").empty());
    }
    SECTION("missing keys")
    {
        CHECK(make_provider(spec)->list({}, srv.fn(), cancel, NOW).error.find("access key") != std::string::npos);
    }
}

TEST_CASE("vaults: times and cached files", "[Vaults]")
{
    CHECK(parse_time("Tue, 13 Oct 2026 08:00:00 GMT") == 1791878400);
    CHECK(parse_time("2026-10-13T08:00:00Z") == 1791878400);
    CHECK(parse_time("2026-10-13T08:00:00.123Z") == 1791878400);
    CHECK(parse_time("2026-10-13") == 1791849600);
    CHECK(parse_time("yesterday") == 0);
    File f;
    f.id     = "a/b.stl";
    f.name   = "b.stl";
    f.folder = "a";
    f.size   = 5;
    f.mtime  = 7;
    const File g = file_from_json(file_to_json(f));
    CHECK(g.id == f.id);
    CHECK(g.size == 5);
    CHECK(g.mtime == 7);
    CHECK(is_listed_file("x.3MF"));
    CHECK(is_listed_file("x.gcode.3mf"));
    CHECK_FALSE(is_listed_file("x.exe"));
    CHECK_FALSE(is_listed_file(".stl"));
}
