#include <catch2/catch.hpp>

#include "libslic3r/Layerling.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::layerling;
namespace fs = boost::filesystem;

namespace {

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("layerling_test_%%%%%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A zip with the given (name, content) entries; names are written as they are (no validation).
fs::path write_zip(const fs::path &dir, const std::string &file, const std::vector<std::pair<std::string, std::string>> &entries)
{
    const fs::path path = dir / file;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, path.string()));
    for (const auto &e : entries)
        REQUIRE(mz_zip_writer_add_mem_ex_v2(&zip, e.first.c_str(), e.second.data(), e.second.size(), nullptr, 0,
                                            MZ_ZIP_FLAG_ASCII_FILENAME | MZ_DEFAULT_COMPRESSION, 0, 0, nullptr, nullptr, 0, nullptr, 0));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    close_zip_writer(&zip);
    return path;
}

const char *RELEASE_JSON = R"({
  "tag_name": "v1.131.1",
  "name": "layerling 1.131.1",
  "draft": false,
  "prerelease": false,
  "published_at": "2026-11-01T20:29:29Z",
  "html_url": "https://github.com/henmedia/layerling/releases/tag/v1.131.1",
  "assets": [
    { "name": "layerling-static.zip", "size": 51037423,
      "digest": "sha256:a33fa4a0a1c6b7f3bf9f3d59087264c9347f1a1ba35626b3c96e4b1828642b66",
      "browser_download_url": "https://github.com/henmedia/layerling/releases/download/v1.131.1/layerling-static.zip" },
    { "name": "layerling-1.131.1-static.zip", "size": 51037423,
      "digest": "sha256:A33FA4A0A1C6B7F3BF9F3D59087264C9347F1A1BA35626B3C96E4B1828642B66",
      "browser_download_url": "https://github.com/henmedia/layerling/releases/download/v1.131.1/layerling-1.131.1-static.zip" }
  ]
})";

} // namespace

TEST_CASE("Layerling: release JSON picks the versioned static build", "[Layerling]")
{
    ReleaseInfo r;
    std::string err;
    REQUIRE(parse_release_json(RELEASE_JSON, r, &err));
    CHECK(r.version == "1.131.1");
    CHECK(r.tag == "v1.131.1");
    CHECK(r.asset_name == "layerling-1.131.1-static.zip");
    CHECK(r.asset_url == "https://github.com/henmedia/layerling/releases/download/v1.131.1/layerling-1.131.1-static.zip");
    CHECK(r.asset_size == 51037423);
    CHECK(r.sha256 == "a33fa4a0a1c6b7f3bf9f3d59087264c9347f1a1ba35626b3c96e4b1828642b66");
    CHECK(r.published_at == "2026-11-01T20:29:29Z");
}

TEST_CASE("Layerling: release JSON falls back and refuses what it must", "[Layerling]")
{
    ReleaseInfo r;
    std::string err;
    // Only the unversioned asset, no digest: accepted, no checksum.
    REQUIRE(parse_release_json(R"({"tag_name":"v2.0.0","assets":[{"name":"layerling-static.zip","size":10,
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
    CHECK(r.asset_name == "layerling-static.zip");
    CHECK(r.sha256.empty());

    CHECK_FALSE(parse_release_json("not json", r, &err));
    CHECK_FALSE(parse_release_json("[]", r, &err));
    // A draft or pre-release is never offered.
    CHECK_FALSE(parse_release_json(R"({"tag_name":"v2.0.0","prerelease":true,"assets":[{"name":"layerling-static.zip","size":10,
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
    // No static build.
    CHECK_FALSE(parse_release_json(R"({"tag_name":"v2.0.0","assets":[{"name":"source.tar.gz","size":10,
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/source.tar.gz"}]})", r, &err));
    // Another host, another repo, or a path trick.
    for (const char *url : {"https://evil.example/layerling-static.zip",
                            "https://github.com/someone/layerling/releases/download/v2/layerling-static.zip",
                            "http://github.com/henmedia/layerling/releases/download/v2/layerling-static.zip",
                            "https://github.com/henmedia/layerling/releases/download/../../x/layerling-static.zip"}) {
        const std::string json = std::string(R"({"tag_name":"v2.0.0","assets":[{"name":"layerling-static.zip","size":10,"browser_download_url":")") +
                                 url + "\"}]}";
        CHECK_FALSE(parse_release_json(json, r, &err));
    }
    // A version that cannot name a folder.
    CHECK_FALSE(parse_release_json(R"({"tag_name":"../x","assets":[{"name":"layerling-static.zip","size":10,
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
    // No size, a zero size, or too large.
    CHECK_FALSE(parse_release_json(R"({"tag_name":"v2.0.0","assets":[{"name":"layerling-static.zip",
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
    CHECK_FALSE(parse_release_json(R"({"tag_name":"v2.0.0","assets":[{"name":"layerling-static.zip","size":0,
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
    CHECK_FALSE(parse_release_json(R"({"tag_name":"v2.0.0","assets":[{"name":"layerling-static.zip","size":9999999999,
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
    // A malformed sha256 digest is an error, not "no digest".
    CHECK_FALSE(parse_release_json(R"({"tag_name":"v2.0.0","assets":[{"name":"layerling-static.zip","size":10,"digest":"sha256:1234",
        "browser_download_url":"https://github.com/henmedia/layerling/releases/download/v2.0.0/layerling-static.zip"}]})", r, &err));
}

TEST_CASE("Layerling: digests", "[Layerling]")
{
    CHECK(parse_sha256_digest("sha256:" + std::string(64, 'A')) == std::string(64, 'a'));
    CHECK(parse_sha256_digest("sha256:" + std::string(63, 'a')).empty());
    CHECK(parse_sha256_digest("sha256:" + std::string(63, 'a') + "g").empty());
    CHECK(parse_sha256_digest("sha1:" + std::string(40, 'a')).empty());
    CHECK(parse_sha256_digest("").empty());
}

TEST_CASE("Layerling: version comparison", "[Layerling]")
{
    CHECK(compare_versions("1.131.1", "1.131.0") > 0);
    CHECK(compare_versions("v1.131.1", "1.131.1") == 0);
    CHECK(compare_versions("1.131.10", "1.131.9") > 0);
    CHECK(compare_versions("1.2", "1.2.0") == 0);
    CHECK(compare_versions("1.2.0-beta.1", "1.2.0") < 0);
    CHECK(compare_versions("1.2.0", "1.2.0-beta.1") > 0);
    CHECK(compare_versions("1.2.0-beta.2", "1.2.0-beta.1") > 0);
    CHECK(compare_versions("1.2.0+build5", "1.2.0") == 0);
    CHECK(compare_versions("2.0.0", "10.0.0") < 0);
    CHECK(compare_versions("", "0.0.1") < 0);

    CHECK(normalize_version(" v1.2.3 ") == "1.2.3");
    CHECK(is_safe_version("1.131.1"));
    CHECK(is_safe_version("1.2.0-beta_1"));
    for (const char *bad : {"", ".", "..", "../1", "1/2", "1\\2", "1..2", ".1", "-1", "1 2", "C:1", "1.2\n"})
        CHECK_FALSE(is_safe_version(bad));
    CHECK_FALSE(is_safe_version(std::string(65, '1')));
}

TEST_CASE("Layerling: once-a-day update check", "[Layerling]")
{
    const std::int64_t now = 1'800'000'000;
    CHECK(update_check_due(0, now));
    CHECK_FALSE(update_check_due(now - 3600, now));
    CHECK(update_check_due(now - 24 * 3600, now));
    CHECK(update_check_due(now + 3600, now)); // a clock set back
}

TEST_CASE("Layerling: MIME types and cache headers", "[Layerling]")
{
    CHECK(std::string(mime_type_for("manifold.wasm")) == "application/wasm");
    CHECK(std::string(mime_type_for("a/b/chunk.js")) == "text/javascript; charset=utf-8");
    CHECK(std::string(mime_type_for("worker.MJS")) == "text/javascript; charset=utf-8");
    CHECK(std::string(mime_type_for("index.html")) == "text/html; charset=utf-8");
    CHECK(std::string(mime_type_for("version.json")) == "application/json; charset=utf-8");
    CHECK(std::string(mime_type_for("manifest.webmanifest")) == "application/manifest+json; charset=utf-8");
    CHECK(std::string(mime_type_for("icon.svg")) == "image/svg+xml");
    CHECK(std::string(mime_type_for("x.webp")) == "image/webp");
    CHECK(std::string(mime_type_for("f.woff2")) == "font/woff2");
    CHECK(std::string(mime_type_for("f.ttf")) == "font/ttf");
    CHECK(std::string(mime_type_for("favicon.ico")) == "image/x-icon");
    CHECK(std::string(mime_type_for("style.css")) == "text/css; charset=utf-8");
    CHECK(std::string(mime_type_for("noext")) == "application/octet-stream");
    CHECK(std::string(mime_type_for(".hidden")) == "application/octet-stream");

    CHECK(std::string(cache_control_for("_next/static/chunks/1043.c2deddd4aabc9d68.js")).find("immutable") != std::string::npos);
    CHECK(std::string(cache_control_for("index.html")) == "no-cache");
    CHECK(std::string(cache_control_for("sw.js")) == "no-cache");
    CHECK(std::string(cache_control_for("version.json")) == "no-cache");
}

TEST_CASE("Layerling: request paths stay inside the app folder", "[Layerling]")
{
    bool forbidden = true;
    CHECK(candidate_files("/", &forbidden) == std::vector<std::string>{"index.html"});
    CHECK_FALSE(forbidden);
    CHECK(candidate_files("/?editor=1") == std::vector<std::string>{"index.html"});
    CHECK(candidate_files("/guide") == std::vector<std::string>{"guide", "guide.html", "guide/index.html"});
    CHECK(candidate_files("/guide/") == std::vector<std::string>{"guide/index.html"});
    CHECK(candidate_files("/_next/static/chunks/a.js#x") == std::vector<std::string>{"_next/static/chunks/a.js", "_next/static/chunks/a.js/index.html"});
    CHECK(candidate_files("//a//b.css") == std::vector<std::string>{"a/b.css", "a/b.css/index.html"});

    for (const char *bad : {"", "index.html", "/../secret", "/a/../../b", "/..", "/./x", "/a\\..\\b", "/C:/Windows/win.ini",
                            "/a\x01", "/store.php", "/STORE.PHP", "/x.phtml", "/store.php/x", "/... /x", "/.. "}) {
        forbidden = false;
        CHECK(candidate_files(bad, &forbidden).empty());
        CHECK(forbidden);
    }
}

TEST_CASE("Layerling: origins", "[Layerling]")
{
    CHECK(origin_of("http://127.0.0.1:23456/?editor=1") == "http://127.0.0.1:23456");
    CHECK(origin_of("HTTPS://Layerling.COM/x") == "https://layerling.com");
    CHECK(origin_of("https://layerling.com:443/") == "https://layerling.com");
    CHECK(origin_of("http://nas.local:80/") == "http://nas.local");
    CHECK(origin_of("file:///C:/x").empty());
    CHECK(origin_of("blob:http://127.0.0.1:1/abc").empty());
    CHECK(origin_of("http://user@host/").empty());
}

TEST_CASE("Layerling: FUNDING.yml", "[Layerling]")
{
    const std::string yml = "# These are supported funding model platforms\n"
                            "\n"
                            "github: [henmedia]\n"
                            "patreon: # Replace with a single Patreon username\n"
                            "open_collective: # Replace with a single Open Collective username\n"
                            "ko_fi: henmedia\n"
                            "liberapay: # Replace\r\n"
                            "# polar: someone\n"
                            "custom: 'https://www.paypal.com/donate?business=TX5T9PET4XKWN&item_name=layerling&currency_code=EUR&no_recurring=0'\n";
    const auto links = parse_funding_yml(yml);
    REQUIRE(links.size() == 3);
    CHECK(links[0].label == "GitHub Sponsors");
    CHECK(links[0].url == "https://github.com/sponsors/henmedia");
    CHECK(links[1].label == "Ko-fi");
    CHECK(links[1].url == "https://ko-fi.com/henmedia");
    CHECK(links[2].label == "PayPal");
    CHECK(links[2].url == "https://www.paypal.com/donate?business=TX5T9PET4XKWN&item_name=layerling&currency_code=EUR&no_recurring=0");
    // The built-in fallback is the same list.
    const auto fallback = default_funding_links();
    REQUIRE(fallback.size() == links.size());
    for (size_t i = 0; i < links.size(); ++i)
        CHECK(fallback[i].url == links[i].url);

    // Lists, other platforms, and what is dropped.
    const auto more = parse_funding_yml("github: [a, \"b\"]\npatreon: p1\nbuy_me_a_coffee: c1\npolar: po\nopen_collective: oc\n"
                                        "liberapay: lp\ncustom: [\"https://example.org/give\", \"http://insecure.example\", \"javascript:alert(1)\"]\n"
                                        "ko_fi: bad/name\ngithub: ../x\n  indented: ignored\n");
    std::vector<std::string> urls;
    for (const auto &l : more)
        urls.push_back(l.url);
    CHECK(urls == std::vector<std::string>{"https://github.com/sponsors/a", "https://github.com/sponsors/b", "https://www.patreon.com/p1",
                                           "https://www.buymeacoffee.com/c1", "https://polar.sh/po", "https://opencollective.com/oc",
                                           "https://liberapay.com/lp", "https://example.org/give"});
    CHECK(more.back().label == "example.org");
    CHECK(parse_funding_yml("").empty());
}

TEST_CASE("Layerling: what the tab exports", "[Layerling]")
{
    CHECK(classify_export("Body.3MF") == ExportKind::Model);
    CHECK(classify_export("a.stl") == ExportKind::Model);
    CHECK(classify_export("a.obj") == ExportKind::Model);
    CHECK(classify_export("a.step") == ExportKind::Model);
    CHECK(classify_export("a.stp") == ExportKind::Model);
    CHECK(classify_export("design.lyl") == ExportKind::Project);
    CHECK(classify_export("old.skf") == ExportKind::Project);
    CHECK(classify_export("manual.pdf") == ExportKind::Other);
    CHECK(classify_export("noext") == ExportKind::Other);

    CHECK(sanitize_export_name("Bracket.3mf") == "Bracket.3mf");
    CHECK(sanitize_export_name("../../evil.3mf") == "evil.3mf");
    CHECK(sanitize_export_name("C:\\x\\y.stl") == "y.stl");
    CHECK(sanitize_export_name("a<b>:c?.stl") == "a_b__c_.stl");
    CHECK(sanitize_export_name("") == "layerling-export");
    CHECK(sanitize_export_name(".3mf") == "layerling-export.3mf");
    CHECK(sanitize_export_name("...") == "layerling-export");
    const std::string longname = sanitize_export_name(std::string(300, 'x') + ".3mf");
    CHECK(longname.size() <= 120);
    CHECK(longname.substr(longname.size() - 4) == ".3mf");
}

TEST_CASE("Layerling: 3MF body colours", "[Layerling]")
{
    // As Layerling 1.131 writes it: objects point at basematerials, triangles at a colorgroup.
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02">
  <metadata name="Application">layerling</metadata>
 <resources>
  <basematerials id="1"><base name="Color 1" displaycolor="#d97813"/><base name="Color 2" displaycolor="#1E90FF"/></basematerials>
  <m:colorgroup id="5"><m:color color="#D97813FF"/><m:color color="#1E90FFFF"/></m:colorgroup>
  <object id="2" type="model" name="Base" pid="1" pindex="0"><mesh><vertices><vertex x="0" y="0" z="0"/></vertices><triangles><triangle v1="0" v2="0" v3="0" pid="5" p1="0"/></triangles></mesh></object>
  <object id="3" type="model" name="Lid &amp; knob" pid="1" pindex="1"><mesh><vertices/><triangles><triangle v1="0" v2="0" v3="0" pid="5" p1="1"/></triangles></mesh></object>
  <object id="4" type="model" name="Pin" pid="1" pindex="0"><mesh><vertices/><triangles/></mesh></object>
 </resources>
 <build>
  <item objectid="2"/>
  <item objectid="3"/>
  <item objectid="4"/>
 </build>
</model>
)";
    std::vector<BodyColour> bodies;
    std::string             err;
    REQUIRE(parse_3mf_body_colours(xml, bodies, &err));
    REQUIRE(bodies.size() == 3);
    CHECK(bodies[0].name == "Base");
    CHECK(bodies[0].colour == "#D97813");
    CHECK(bodies[1].name == "Lid & knob");
    CHECK(bodies[1].colour == "#1E90FF");
    CHECK(bodies[2].colour == "#D97813");
    CHECK(distinct_colours(bodies) == std::vector<std::string>{"#D97813", "#1E90FF"});

    // No object-level pid: the first triangle's colour. Components and objects outside the build are skipped.
    const std::string xml2 = R"(<model xmlns:m="m"><resources>
  <m:colorgroup id="9"><m:color color="#00FF00"/></m:colorgroup>
  <object id="1"><mesh><triangles><triangle v1="0" v2="1" v3="2" pid="9" p1="0"/></triangles></mesh></object>
  <object id="2"><mesh><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object>
  <object id="3"><components><component objectid="2"/></components></object>
  <object id="7" pid="9" pindex="5"><mesh/></object>
 </resources><build><item objectid="1"/><item objectid="3"/><item objectid="7"/></build></model>)";
    REQUIRE(parse_3mf_body_colours(xml2, bodies, &err));
    REQUIRE(bodies.size() == 3);
    CHECK(bodies[0].colour == "#00FF00");
    CHECK(bodies[1].object_id == "3");
    CHECK(bodies[1].colour.empty());
    CHECK(bodies[2].colour.empty()); // pindex out of range

    CHECK_FALSE(parse_3mf_body_colours("<model><resources>", bodies, &err));
}

TEST_CASE("Layerling: 3MF body colours from a file", "[Layerling]")
{
    TempDir t;
    const std::string model = R"(<model><resources><basematerials id="1"><base displaycolor="#112233"/></basematerials>
<object id="2" name="A" pid="1" pindex="0"><mesh/></object></resources><build><item objectid="2"/></build></model>)";
    const fs::path f = write_zip(t.path, "a.3mf", {{"[Content_Types].xml", "<Types/>"}, {"3D/3dmodel.model", model}});
    std::vector<BodyColour> bodies;
    REQUIRE(read_3mf_body_colours(f, bodies));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].colour == "#112233");
    const fs::path g = write_zip(t.path, "b.3mf", {{"other.txt", "x"}});
    CHECK_FALSE(read_3mf_body_colours(g, bodies));
}

TEST_CASE("Layerling: colours to filaments", "[Layerling]")
{
    std::vector<std::string> added;
    // Reuse an exact match (case and alpha ignored), add the rest.
    auto ids = map_colours_to_filaments({"#D97813", "#1E90FF", "#FFFFFF"}, {"#ffffffff", "#d97813"}, 16, added);
    CHECK(ids == std::vector<int>{2, 3, 1});
    CHECK(added == std::vector<std::string>{"#1E90FF"});
    // Full: the nearest one.
    ids = map_colours_to_filaments({"#FE0000", "#0000F0"}, {"#FF0000", "#0000FF"}, 2, added);
    CHECK(ids == std::vector<int>{1, 2});
    CHECK(added.empty());
    ids = map_colours_to_filaments({"#00FF00"}, {"#000000", "#00EE00"}, 2, added);
    CHECK(ids == std::vector<int>{2});
    ids = map_colours_to_filaments({}, {"#000000"}, 4, added);
    CHECK(ids.empty());
}

TEST_CASE("Layerling: archives are checked before anything is unpacked", "[Layerling]")
{
    TempDir t;
    // Files at the root.
    ArchiveCheck c = check_archive(write_zip(t.path, "ok.zip", {{"index.html", "<html/>"}, {"_next/a.js", "x"}}));
    CHECK(c.ok);
    CHECK(c.root_prefix.empty());
    CHECK(c.entries == 2);
    // Under one top folder.
    c = check_archive(write_zip(t.path, "nested.zip", {{"layerling/index.html", "<html/>"}, {"layerling/sw.js", "x"}}));
    CHECK(c.ok);
    CHECK(c.root_prefix == "layerling/");
    // No index.html, or one buried too deep.
    CHECK_FALSE(check_archive(write_zip(t.path, "noindex.zip", {{"readme.txt", "x"}})).ok);
    CHECK_FALSE(check_archive(write_zip(t.path, "deep.zip", {{"a/b/index.html", "x"}})).ok);
    // Unsafe names reject the whole archive.
    for (const char *bad : {"../evil.txt", "a/../../evil.txt", "..\\evil.txt", "a/..\\..\\evil.txt"}) {
        c = check_archive(write_zip(t.path, "bad.zip", {{"index.html", "<html/>"}, {bad, "x"}}));
        CHECK_FALSE(c.ok);
    }
    // Not a zip.
    {
        boost::nowide::ofstream f((t.path / "not.zip").string(), std::ios::binary);
        f << "hello";
    }
    CHECK_FALSE(check_archive(t.path / "not.zip").ok);
}

TEST_CASE("Layerling: install unpacks into the version folder only", "[Layerling]")
{
    TempDir t;
    const fs::path versions = t.path / "layerling";
    fs::path       root;
    std::string    err;

    const fs::path good = write_zip(t.path, "good.zip", {{"index.html", "<html>1</html>"}, {"_next/static/chunks/a.js", "x"}});
    REQUIRE(install_archive(good, versions, "1.0.0", root, err));
    CHECK(root == versions / "1.0.0");
    CHECK(fs::is_regular_file(versions / "1.0.0" / "index.html"));
    CHECK(fs::is_regular_file(versions / "1.0.0" / "_next" / "static" / "chunks" / "a.js"));
    CHECK(find_app_root(versions / "1.0.0") == versions / "1.0.0");

    // A traversal entry: refused, and nothing written anywhere (no new version folder, no file
    // next to the versions folder, no leftover staging folder).
    const fs::path evil = write_zip(t.path, "evil.zip", {{"index.html", "<html/>"}, {"../../escaped.txt", "x"}});
    CHECK_FALSE(install_archive(evil, versions, "2.0.0", root, err));
    CHECK_FALSE(fs::exists(versions / "2.0.0"));
    CHECK_FALSE(fs::exists(t.path / "escaped.txt"));
    CHECK_FALSE(fs::exists(t.path.parent_path() / "escaped.txt"));
    int leftovers = 0;
    for (fs::directory_iterator it(versions), end; it != end; ++it)
        if (it->path().filename() != "1.0.0")
            ++leftovers;
    CHECK(leftovers == 0);

    // An unsafe version name never becomes a folder.
    CHECK_FALSE(install_archive(good, versions, "../1.0.0", root, err));

    // A nested build: the app root is the subfolder. The earlier version stays untouched.
    const fs::path nested = write_zip(t.path, "nested.zip", {{"site/index.html", "<html>2</html>"}});
    REQUIRE(install_archive(nested, versions, "2.0.0", root, err));
    CHECK(root == versions / "2.0.0" / "site");
    CHECK(fs::is_regular_file(versions / "1.0.0" / "index.html"));

    // Reinstalling the same version replaces it.
    const fs::path again = write_zip(t.path, "again.zip", {{"index.html", "<html>3</html>"}});
    REQUIRE(install_archive(again, versions, "1.0.0", root, err));
    CHECK_FALSE(fs::exists(versions / "1.0.0" / "_next"));
}

TEST_CASE("Layerling: what stays in the tab", "[Layerling]")
{
    const std::string origin = "http://127.0.0.1:23456";
    CHECK(decide_navigation("http://127.0.0.1:23456/?editor=1", origin) == Navigation::Allow);
    CHECK(decide_navigation("http://127.0.0.1:23456/guide/", origin) == Navigation::Allow);
    CHECK(decide_navigation("about:blank", origin) == Navigation::Allow);
    CHECK(decide_navigation("blob:http://127.0.0.1:23456/0c1d", origin) == Navigation::Allow);
    CHECK(decide_navigation("data:application/octet-stream;base64,AAAA", origin) == Navigation::Allow);
    // Another port of this computer is another origin (the page server, the phone hub).
    CHECK(decide_navigation("http://127.0.0.1:13619/", origin) == Navigation::External);
    CHECK(decide_navigation("https://github.com/henmedia/layerling", origin) == Navigation::External);
    CHECK(decide_navigation("https://ko-fi.com/henmedia", origin) == Navigation::External);
    CHECK(decide_navigation("mailto:someone@example.org", origin) == Navigation::External);
    CHECK(decide_navigation("file:///C:/Windows/win.ini", origin) == Navigation::Block);
    CHECK(decide_navigation("javascript:alert(1)", origin) == Navigation::Block);
    CHECK(decide_navigation("bambustudio://open?file=x", origin) == Navigation::Block);
    // A self-hosted copy (a later phase): the same rules on its origin.
    CHECK(decide_navigation("https://layerling.example.org/x", "https://layerling.example.org") == Navigation::Allow);

    CHECK(start_url("http://127.0.0.1:23456") == "http://127.0.0.1:23456/");
    CHECK(start_url("http://127.0.0.1:23456/") == "http://127.0.0.1:23456/");
    CHECK(start_url("https://nas.local:3000/?mode=simple#x") == "https://nas.local:3000/?mode=simple");
    CHECK(start_url("ftp://x/").empty());
    CHECK(std::string(hide_save_picker_script()).find("showSaveFilePicker") != std::string::npos);
}

TEST_CASE("Layerling: the document-start script hides the save picker and the install banner", "[Layerling]")
{
    const std::string hint = suppress_install_hint_script();
    // Layerling 1.131.1 skips its "Install layerling as an app" banner when this key holds "1".
    CHECK(std::string(INSTALL_HINT_DISMISSED_KEY) == "layerling.installHintDismissed");
    CHECK(hint.find(std::string("'") + INSTALL_HINT_DISMISSED_KEY + "'") != std::string::npos);
    CHECK(hint.find("setItem(k,'1')") != std::string::npos);
    CHECK(hint.find("window.top!==window") != std::string::npos); // top frame only
    CHECK(hint.find("try{") != std::string::npos);                // storage errors never break the page
    // Not by pretending to be an installed app: that changes Layerling's tooltips too.
    CHECK(hint.find("display-mode") == std::string::npos);
    const std::string all = document_start_script();
    CHECK(all.find("showSaveFilePicker") != std::string::npos);
    CHECK(all.find(INSTALL_HINT_DISMISSED_KEY) != std::string::npos);
    // Two independent statements: one failing cannot skip the other.
    CHECK(all == std::string(hide_save_picker_script()) + hint);
    CHECK(all.find("})();(function(){") != std::string::npos);
}

TEST_CASE("Layerling: SHA-256 of a stream", "[Layerling]")
{
    Sha256 a;
    a.update("ab", 2);
    a.update("c", 1);
    CHECK(a.hex_digest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 e;
    CHECK(e.hex_digest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
