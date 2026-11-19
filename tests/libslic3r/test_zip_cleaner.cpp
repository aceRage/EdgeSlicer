#include <catch2/catch.hpp>

#include "libslic3r/ZipWorkCleaner.hpp"
#include "libslic3r/ZipWorkDir.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

constexpr std::time_t DAY = 24 * 60 * 60;
const std::string     ID  = "0123456789abcdef0123456789abcdef";

struct TempRoot
{
    fs::path base;
    fs::path root;
    TempRoot()
    {
        base = fs::temp_directory_path() / fs::unique_path("edgeslicer_zip_clean_%%%%%%%%");
        root = base / "Zip";
        fs::create_directories(base);
        std::string err;
        REQUIRE(ZipWorkDir::ensure_root(root, err));
    }
    ~TempRoot()
    {
        boost::system::error_code ec;
        fs::remove_all(base, ec);
    }
};

void write_file(const fs::path& p, const std::string& bytes)
{
    fs::create_directories(p.parent_path());
    boost::nowide::ofstream out(p.string(), std::ios::binary | std::ios::trunc);
    out << bytes;
}

// A session folder with one file and a record `age_days` old.
fs::path make_session(const fs::path& root, const std::string& name, std::time_t now, std::time_t age_days, const std::string& id = ID)
{
    const fs::path s = root / name;
    fs::create_directories(s / "parts");
    write_file(s / "parts" / "cube.stl", "solid cube");
    std::string err;
    if (!id.empty())
        REQUIRE(ZipWorkCleaner::write_session_record(root, s, id, err, now - age_days * DAY));
    return s;
}

ZipWorkCleaner::Options options(const fs::path& root, std::time_t now, unsigned days = 30)
{
    ZipWorkCleaner::Options o;
    o.root         = root;
    o.install_id   = ID;
    o.max_age_days = days;
    o.now          = now;
    return o;
}

// Directory symlink, or on Windows a junction (mklink /J needs no privilege). False when the
// platform refuses (then the case is skipped, not failed).
bool make_dir_link(const fs::path& target, const fs::path& link)
{
    boost::system::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (!ec)
        return true;
#ifdef _WIN32
    const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    if (std::system(cmd.c_str()) == 0 && fs::exists(link))
        return true;
#endif
    return false;
}

} // namespace

TEST_CASE("cleaner settings: days parse and install id", "[ZipClean]")
{
    CHECK(zip_cleanup_days_from_value("30") == 30);
    CHECK(zip_cleanup_days_from_value("7") == 7);
    CHECK(zip_cleanup_days_from_value("") == ZIP_WORK_CLEANUP_DEFAULT_DAYS);
    CHECK(zip_cleanup_days_from_value("0") == ZIP_WORK_CLEANUP_DEFAULT_DAYS);
    CHECK(zip_cleanup_days_from_value("x5") == ZIP_WORK_CLEANUP_DEFAULT_DAYS);
    CHECK(zip_cleanup_days_from_value("99999999") == ZIP_WORK_CLEANUP_MAX_DAYS);

    const std::string a = generate_zip_install_id();
    const std::string b = generate_zip_install_id();
    CHECK(a.size() == 32);
    CHECK(a != b);
    for (char c : a)
        CHECK(std::isxdigit(static_cast<unsigned char>(c)));
}

TEST_CASE("cleaner removes recorded sessions older than the limit only", "[ZipClean]")
{
    TempRoot          t;
    const std::time_t now   = 2000000000;
    const fs::path    old   = make_session(t.root, "old_20260101-000000", now, 31);
    const fs::path    young = make_session(t.root, "young_20261101-000000", now, 5);
    const fs::path    edge  = make_session(t.root, "edge_20261004-000000", now, 30);

    const ZipWorkCleaner::Result r = ZipWorkCleaner::clean(options(t.root, now));
    CHECK(r.root_ok);
    CHECK(r.removed == 2);
    CHECK(r.kept_young == 1);
    CHECK(r.failed == 0);
    CHECK(r.bytes_freed == 2 * std::string("solid cube").size());
    CHECK_FALSE(fs::exists(old));
    CHECK_FALSE(fs::exists(edge));
    CHECK(fs::exists(young / "parts" / "cube.stl"));
    // The removed sessions' records go too; the young one keeps its record.
    const fs::path index = t.root / ZipWorkCleaner::index_dir_name();
    CHECK_FALSE(fs::exists(index / "old_20260101-000000.rec"));
    CHECK(fs::exists(index / "young_20261101-000000.rec"));
    // The root and its marker stay.
    CHECK(fs::exists(t.root / ZipWorkDir::marker_name()));
}

TEST_CASE("cleaner trusts only this install's records in the root index", "[ZipClean]")
{
    TempRoot          t;
    const std::time_t now = 2000000000;

    SECTION("a folder without a record is never touched")
    {
        const fs::path stray = make_session(t.root, "stray_20200101-000000", now, 400, "");
        const auto     r     = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.removed == 0);
        CHECK(fs::exists(stray / "parts" / "cube.stl"));
    }
    SECTION("a record with another install's id is ignored")
    {
        const fs::path other = make_session(t.root, "other_20200101-000000", now, 400, "ffffffffffffffffffffffffffffffff");
        const auto     r     = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.removed == 0);
        CHECK(r.untrusted == 1);
        CHECK(fs::exists(other / "parts" / "cube.stl"));
    }
    SECTION("a record or marker extracted from an archive inside a session is not read")
    {
        const fs::path victim = make_session(t.root, "victim_20200101-000000", now, 400, "");
        const fs::path s      = make_session(t.root, "s_20261101-000000", now, 1);
        // What a hostile archive could place inside its own session folder.
        write_file(s / ZipWorkCleaner::index_dir_name() / "victim_20200101-000000.rec",
                   "EdgeSlicer zip session v1\nid=" + ID + "\ncreated=1\n");
        write_file(s / ZipWorkDir::marker_name(), "EdgeSlicer zip working directory\n");
        const auto r = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.removed == 0);
        CHECK(fs::exists(victim / "parts" / "cube.stl"));
        CHECK(fs::exists(s / "parts" / "cube.stl"));
    }
    SECTION("a record whose name is not a plain session name is refused")
    {
        write_file(t.root / ZipWorkCleaner::index_dir_name() / ".hidden.rec", "EdgeSlicer zip session v1\nid=" + ID + "\ncreated=1\n");
        write_file(t.root / ".hidden" / "keep.txt", "x");
        const auto r = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.untrusted == 1);
        CHECK(fs::exists(t.root / ".hidden" / "keep.txt"));
    }
    SECTION("a garbled record is refused")
    {
        const fs::path s = make_session(t.root, "garbled_20200101-000000", now, 400, "");
        write_file(t.root / ZipWorkCleaner::index_dir_name() / "garbled_20200101-000000.rec", "id=" + ID + "\ncreated=1\n");
        const auto r = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.untrusted == 1);
        CHECK(fs::exists(s / "parts" / "cube.stl"));
    }
    SECTION("an empty install id cleans nothing")
    {
        const fs::path s = make_session(t.root, "a_20200101-000000", now, 400);
        auto           o = options(t.root, now);
        o.install_id.clear();
        const auto r = ZipWorkCleaner::clean(o);
        CHECK_FALSE(r.root_ok);
        CHECK(fs::exists(s / "parts" / "cube.stl"));
    }
}

TEST_CASE("cleaner refuses a root without its marker", "[ZipClean]")
{
    TempRoot          t;
    const std::time_t now = 2000000000;
    const fs::path    s   = make_session(t.root, "a_20200101-000000", now, 400);
    fs::remove(t.root / ZipWorkDir::marker_name());
    const auto r = ZipWorkCleaner::clean(options(t.root, now));
    CHECK_FALSE(r.root_ok);
    CHECK(r.removed == 0);
    CHECK(fs::exists(s / "parts" / "cube.stl"));
}

TEST_CASE("write_session_record only records direct children of the root", "[ZipClean]")
{
    TempRoot    t;
    std::string err;
    CHECK_FALSE(ZipWorkCleaner::write_session_record(t.root, t.base / "elsewhere", ID, err));
    CHECK_FALSE(ZipWorkCleaner::write_session_record(t.root, t.root / "a" / "b", ID, err));
    CHECK_FALSE(ZipWorkCleaner::write_session_record(t.root, t.root / "ok", "", err));
    CHECK(ZipWorkCleaner::write_session_record(t.root, t.root / "ok", ID, err));
}

TEST_CASE("cleaner skips sessions in use and sessions newer than the pass", "[ZipClean]")
{
    TempRoot          t;
    const std::time_t now  = 2000000000;
    const fs::path    used = make_session(t.root, "used_20200101-000000", now, 400);
    const fs::path    idle = make_session(t.root, "idle_20200101-000000", now, 400);
    const fs::path    late = make_session(t.root, "late_20330518-000000", now, 0); // created at `now`

    auto o = options(t.root, now, 0); // Clean now: no age limit
    o.in_use.push_back(used / "parts" / "cube.stl");
    const auto r = ZipWorkCleaner::clean(o);
    CHECK(r.removed == 1);
    CHECK(r.kept_in_use == 1);
    CHECK(r.kept_young == 1);
    CHECK(fs::exists(used / "parts" / "cube.stl"));
    CHECK_FALSE(fs::exists(idle));
    CHECK(fs::exists(late / "parts" / "cube.stl"));
}

TEST_CASE("cleaner drops records whose folder is gone", "[ZipClean]")
{
    TempRoot          t;
    const std::time_t now = 2000000000;
    const fs::path    s   = make_session(t.root, "gone_20261101-000000", now, 1);
    fs::remove_all(s);
    const auto r = ZipWorkCleaner::clean(options(t.root, now));
    CHECK(r.orphan_records == 1);
    CHECK_FALSE(fs::exists(t.root / ZipWorkCleaner::index_dir_name() / "gone_20261101-000000.rec"));
}

TEST_CASE("cleaner never follows a link out of the root", "[ZipClean][ZipSlip]")
{
    TempRoot          t;
    const std::time_t now     = 2000000000;
    const fs::path    outside = t.base / "outside";
    write_file(outside / "precious.txt", "keep me");

    SECTION("a link inside a session is removed as a link; its target stays")
    {
        const fs::path s = make_session(t.root, "s_20200101-000000", now, 400);
        if (!make_dir_link(outside, s / "parts" / "link")) {
            WARN("directory links cannot be created here; case skipped");
            return;
        }
        const auto r = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.removed == 1);
        CHECK_FALSE(fs::exists(s));
        CHECK(fs::exists(outside / "precious.txt"));
    }
    SECTION("a session folder replaced by a link is left alone")
    {
        make_session(t.root, "swap_20200101-000000", now, 400);
        fs::remove_all(t.root / "swap_20200101-000000");
        if (!make_dir_link(outside, t.root / "swap_20200101-000000")) {
            WARN("directory links cannot be created here; case skipped");
            return;
        }
        CHECK(ZipWorkCleaner::is_link_or_reparse(t.root / "swap_20200101-000000"));
        const auto r = ZipWorkCleaner::clean(options(t.root, now));
        CHECK(r.removed == 0);
        CHECK(r.untrusted == 1);
        CHECK(fs::exists(outside / "precious.txt"));
    }
    SECTION("a root that is itself a link is refused")
    {
        const fs::path real = t.base / "real_root";
        std::string    err;
        REQUIRE(ZipWorkDir::ensure_root(real, err));
        const fs::path s = make_session(real, "a_20200101-000000", now, 400);
        const fs::path link_root = t.base / "link_root";
        if (!make_dir_link(real, link_root)) {
            WARN("directory links cannot be created here; case skipped");
            return;
        }
        const auto r = ZipWorkCleaner::clean(options(link_root, now));
        CHECK_FALSE(r.root_ok);
        CHECK(fs::exists(s / "parts" / "cube.stl"));
        boost::system::error_code ec;
        fs::remove(link_root, ec);
    }
}

TEST_CASE("usage counts the bytes and session folders under the root", "[ZipClean]")
{
    TempRoot          t;
    const std::time_t now = 2000000000;
    make_session(t.root, "a_20261101-000000", now, 1);
    make_session(t.root, "b_20261101-000000", now, 1);
    const ZipWorkCleaner::Usage u = ZipWorkCleaner::usage(t.root);
    CHECK(u.sessions == 2);
    CHECK(u.bytes >= 2 * std::string("solid cube").size());
    const ZipWorkCleaner::Usage none = ZipWorkCleaner::usage(t.base / "missing");
    CHECK(none.bytes == 0);
    CHECK(none.sessions == 0);
}
