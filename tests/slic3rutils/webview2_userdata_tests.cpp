#include <catch2/catch.hpp>

#include "slic3r/Utils/WebView2UserData.hpp"

using namespace Slic3r::WebView2UserData;

// WebView2UserData.hpp: test copies get their own WebView2 user data folder so a copy whose
// browser arguments differ from the main install's no longer gets ERROR_INVALID_STATE (blank
// web pages) while both run. The main install keeps the shared default folder.

TEST_CASE("webview2 udf: the main install keeps the shared folder", "[WebView2UserData]")
{
    Inputs in;
    CHECK_FALSE(wants_own_folder(in));
}

TEST_CASE("webview2 udf: test-copy markers and env ask for an own folder", "[WebView2UserData]")
{
    Inputs a;
    a.no_hub_marker = true;
    CHECK(wants_own_folder(a));

    Inputs b;
    b.own_marker = true;
    CHECK(wants_own_folder(b));

    Inputs c;
    c.own_env = "1";
    CHECK(wants_own_folder(c));
    c.own_env = "TRUE";
    CHECK(wants_own_folder(c));
    c.own_env = "0";
    CHECK_FALSE(wants_own_folder(c));
}

TEST_CASE("webview2 udf: shared env and a user-set folder win", "[WebView2UserData]")
{
    Inputs in;
    in.no_hub_marker = true;
    in.own_marker    = true;
    in.shared_env    = "1";
    CHECK_FALSE(wants_own_folder(in));

    Inputs u;
    u.no_hub_marker = true;
    u.udf_env       = "D:/mine";
    CHECK_FALSE(wants_own_folder(u));
    u.udf_env = "";
    CHECK(wants_own_folder(u));
}

TEST_CASE("webview2 udf: folder name is stable per install directory", "[WebView2UserData]")
{
    const std::string a = copy_folder_name("C:\\Dev\\EdgeSlicerTest\\creality");
    CHECK(a.rfind("creality-", 0) == 0);
    CHECK(a.size() == std::string("creality-").size() + 8);
    // Same directory however it is spelled.
    CHECK(copy_folder_name("c:/dev/edgeslicertest/CREALITY/") == "CREALITY-" + a.substr(a.size() - 8));
    CHECK(copy_folder_name("C:/Dev/EdgeSlicerTest/creality") == a);
    // Different copies differ.
    CHECK(copy_folder_name("C:\\Dev\\EdgeSlicerTest\\heightpat1") != a);
    CHECK(copy_folder_name("D:\\Other\\creality").substr(a.size() - 8) != a.substr(a.size() - 8));
}

TEST_CASE("webview2 udf: folder name is filesystem-safe", "[WebView2UserData]")
{
    const std::string n = copy_folder_name("C:\\Dev\\Edge Slicer (copy)");
    CHECK(n.rfind("Edge_Slicer__copy_-", 0) == 0);
    CHECK(copy_folder_name("").rfind("copy-", 0) == 0);
    CHECK(copy_folder_name(std::string(100, 'x')).size() == 40 + 1 + 8);
}
