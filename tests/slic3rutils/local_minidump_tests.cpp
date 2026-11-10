#include <catch2/catch.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "sentry_wrapper/LocalMinidumpPolicy.hpp"

using namespace Slic3r::local_minidump;

namespace {
bool has(const std::vector<std::wstring>& v, const std::wstring& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

FileEntry dump(const std::wstring& name, std::uint64_t mtime, std::uint64_t size = 1000) { return FileEntry{name, size, mtime}; }
} // namespace

TEST_CASE("local minidump: file name", "[LocalMinidump]")
{
    wchar_t buf[96];
    REQUIRE(format_name(buf, 96, 51768, 2026, 11, 7, 13, 8, 3) > 0);
    CHECK(std::wstring(buf) == L"EdgeSlicer_51768_20261107_130803.dmp");
    CHECK(is_our_file(buf));

    SECTION("too small a buffer gives an empty name, not a cut one")
    {
        wchar_t small[16];
        CHECK(format_name(small, 16, 51768, 2026, 11, 7, 13, 8, 3) == 0);
        CHECK(small[0] == L'\0');
        CHECK(format_name(nullptr, 0, 1, 2026, 1, 1, 0, 0, 0) == 0);
    }
}

TEST_CASE("local minidump: only our files count", "[LocalMinidump]")
{
    CHECK(is_our_file(L"EdgeSlicer_1_20261107_113437.dmp"));
    CHECK(is_our_file(L"EdgeSlicer_1_20261107_113437.dmp.failed.txt"));
    CHECK_FALSE(is_our_file(L"920f74ed-7b03-4b95-bfd5-d49dca6c9460.dmp")); // a crashpad report
    CHECK_FALSE(is_our_file(L"EdgeSlicer_1_20261107_113437.txt"));
    CHECK_FALSE(is_our_file(L"notes_EdgeSlicer_1.dmp"));
    CHECK_FALSE(is_our_file(L"EdgeSlicer.conf"));
}

TEST_CASE("local minidump: rotation", "[LocalMinidump]")
{
    SECTION("under the limit nothing goes")
    {
        std::vector<FileEntry> files;
        for (int i = 0; i < 9; ++i)
            files.push_back(dump(L"EdgeSlicer_" + std::to_wstring(i) + L"_20261107_110000.dmp", 100 + i));
        CHECK(files_to_remove(files, kMaxFiles - 1).empty());
    }
    SECTION("over the limit the oldest go and the newest stay")
    {
        std::vector<FileEntry> files;
        for (int i = 0; i < 14; ++i)
            files.push_back(dump(L"EdgeSlicer_" + std::to_wstring(i) + L"_20261107_110000.dmp", 100 + i));
        // Listing order must not matter.
        std::reverse(files.begin(), files.end());
        const auto gone = files_to_remove(files, kMaxFiles - 1);
        REQUIRE(gone.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(has(gone, L"EdgeSlicer_" + std::to_wstring(i) + L"_20261107_110000.dmp"));
        CHECK_FALSE(has(gone, L"EdgeSlicer_13_20261107_110000.dmp"));
    }
    SECTION("empty dumps always go, and do not use up the quota")
    {
        std::vector<FileEntry> files = {
            dump(L"EdgeSlicer_11408_20261107_113437.dmp", 500, 0),
            dump(L"EdgeSlicer_13448_20261107_113136.dmp", 400, 0),
            dump(L"EdgeSlicer_2_20261107_120000.dmp", 300),
            dump(L"EdgeSlicer_3_20261107_120000.dmp.failed.txt", 200, 40),
        };
        const auto gone = files_to_remove(files, kMaxFiles - 1);
        REQUIRE(gone.size() == 2);
        CHECK(has(gone, L"EdgeSlicer_11408_20261107_113437.dmp"));
        CHECK(has(gone, L"EdgeSlicer_13448_20261107_113136.dmp"));
    }
    SECTION("files that are not ours are never listed")
    {
        std::vector<FileEntry> files = {
            dump(L"0063b93a-3ba5-4c01-91dd-3736e48c681d.dmp", 1, 0),
            dump(L"readme.txt", 2, 0),
        };
        for (int i = 0; i < 12; ++i)
            files.push_back(dump(L"EdgeSlicer_" + std::to_wstring(i) + L"_20261107_110000.dmp", 100 + i));
        const auto gone = files_to_remove(files, 2);
        CHECK(gone.size() == 10);
        CHECK_FALSE(has(gone, L"0063b93a-3ba5-4c01-91dd-3736e48c681d.dmp"));
        CHECK_FALSE(has(gone, L"readme.txt"));
    }
    SECTION("equal times are broken by name, so the result is stable")
    {
        std::vector<FileEntry> files = {dump(L"EdgeSlicer_1_a.dmp", 7), dump(L"EdgeSlicer_2_a.dmp", 7), dump(L"EdgeSlicer_3_a.dmp", 7)};
        const auto gone = files_to_remove(files, 2);
        REQUIRE(gone.size() == 1);
        CHECK(gone[0] == L"EdgeSlicer_1_a.dmp");
    }
}
