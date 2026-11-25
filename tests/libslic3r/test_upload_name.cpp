#include <catch2/catch.hpp>

#include "libslic3r/UploadName.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::upload_name_rules;

TEST_CASE("upload name: print host set strips # * ; : \" < > | and backslash", "[UploadName]")
{
    const auto r = print_host();
    CHECK(sanitize_upload_filename("my#plate.gcode", r) == "myplate.gcode");
    CHECK(sanitize_upload_filename("a*b;c:d\"e<f>g|h.gcode", r) == "abcdefgh.gcode");
    CHECK(sanitize_upload_filename("a\\b.gcode", r) == "ab.gcode");
    // Characters that host does not warn about stay.
    CHECK(sanitize_upload_filename("a$b?c[1].gcode", r) == "a$b?c[1].gcode");
}

TEST_CASE("upload name: Bambu set", "[UploadName]")
{
    const auto r = bambu();
    CHECK(sanitize_upload_filename("a<b>c[d]e:f/g\\h|i?j*k\"l", r) == "abcdefghijkl");
    // '#', ';' and '$' are fine for Bambu.
    CHECK(sanitize_upload_filename("a#b;c$d", r) == "a#b;c$d");
    // No folders: '/' is just a forbidden character.
    CHECK(sanitize_upload_filename("dir/name.3mf", r) == "dirname.3mf");
}

TEST_CASE("upload name: folders keep '/' and clean each segment", "[UploadName]")
{
    const auto r = print_host();
    CHECK(sanitize_upload_filename("folder/name.gcode", r) == "folder/name.gcode");
    CHECK(sanitize_upload_filename("my#dir/my;file.gcode", r) == "mydir/myfile.gcode");
    CHECK(sanitize_upload_filename("a/b/c.gcode", r) == "a/b/c.gcode");
    CHECK(sanitize_upload_filename("/lead/name.gcode", r) == "/lead/name.gcode");
    // Empty and dot-only folder segments are dropped (no traversal, no "//").
    CHECK(sanitize_upload_filename("a//b/../c.gcode", r) == "a/b/c.gcode");
    CHECK(sanitize_upload_filename("###/x.gcode", r) == "x.gcode");
    // A trailing '/' leaves no file name: the fallback fills it in.
    CHECK(sanitize_upload_filename("folder/", r) == "folder/plate_1");
}

TEST_CASE("upload name: extension kept, empty falls back", "[UploadName]")
{
    const auto r = print_host();
    CHECK(sanitize_upload_filename("###.gcode", r) == "plate_1.gcode");
    CHECK(sanitize_upload_filename("###", r) == "plate_1");
    CHECK(sanitize_upload_filename("", r) == "plate_1");
    CHECK(sanitize_upload_filename("   ", r) == "plate_1");
    CHECK(sanitize_upload_filename("a#.gcode.3mf", r) == "a.gcode.3mf");
    CHECK(sanitize_upload_filename("###.gcode", r, "My Part") == "My Part.gcode");
    // A fallback that is itself junk still gives a usable name.
    CHECK(sanitize_upload_filename("###.gcode", r, "###") == "plate_1.gcode");
}

TEST_CASE("upload name: collapse and trim only where something was removed", "[UploadName]")
{
    const auto r = print_host();
    CHECK(sanitize_upload_filename("a #  b.gcode", r) == "a b.gcode");
    CHECK(sanitize_upload_filename("a_#_b.gcode", r) == "a_b.gcode");
    CHECK(sanitize_upload_filename(" #name# .gcode", r) == "name.gcode");
    CHECK(sanitize_upload_filename("..#name.gcode", r) == "name.gcode");
    // A clean name is returned exactly as given, even with doubled underscores.
    CHECK(sanitize_upload_filename("a__b  c.gcode", r) == "a__b  c.gcode");
}

TEST_CASE("upload name: control characters removed, unicode kept", "[UploadName]")
{
    const auto r = print_host();
    CHECK(sanitize_upload_filename(std::string("a\tb\nc\x7f" "d.gcode"), r) == "abcd.gcode");
    const std::string uni = "Dr\xc3\xa1\x63o \xe6\x97\xa5\xe6\x9c\xac#.gcode"; // "Dráco 日本#"
    CHECK(sanitize_upload_filename(uni, r) == "Dr\xc3\xa1\x63o \xe6\x97\xa5\xe6\x9c\xac.gcode");
    const std::string clean = "\xe6\x97\xa5\xe6\x9c\xac.3mf";
    CHECK(sanitize_upload_filename(clean, bambu()) == clean);
}
