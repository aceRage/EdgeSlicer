#include <catch2/catch.hpp>

#include "libslic3r/ZipWorkDir.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct TempDir
{
    fs::path dir;
    TempDir()
    {
        dir = fs::temp_directory_path() / fs::unique_path("edgeslicer_zip_extract_%%%%%%%%");
        fs::create_directories(dir);
    }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// UTF-8 spelling of a path. The test binary does not imbue boost::filesystem with UTF-8 (the app
// does, in main), so on Windows path::string() is the ANSI code page and loses non-ASCII names.
std::string path_u8(const fs::path& p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

std::string read_binary_file(const fs::path& file)
{
    boost::nowide::ifstream in(path_u8(file), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_binary_file(const fs::path& file, const std::string& bytes)
{
    boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_zip_entries(const fs::path& zip_file, const std::vector<std::pair<std::string, std::string>>& entries)
{
    MZ_Archive zip;
    REQUIRE(zip.open_writer(zip_file.string()));
    for (const auto& entry : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip.arch, entry.first.c_str(), entry.second.data(), entry.second.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip.arch));
}

void write_zip_with_extra(const fs::path&    zip_file,
                          const std::string& legacy_name,
                          const std::string& data,
                          const std::string& central_extra,
                          mz_uint            flags = MZ_ZIP_FLAG_ASCII_FILENAME)
{
    MZ_Archive zip;
    REQUIRE(zip.open_writer(zip_file.string()));
    REQUIRE(mz_zip_writer_add_mem_ex_v2(&zip.arch, legacy_name.c_str(), data.data(), data.size(), nullptr, 0, flags, 0, 0, nullptr, nullptr,
                                        0, central_extra.empty() ? nullptr : central_extra.data(),
                                        static_cast<mz_uint>(central_extra.size())));
    REQUIRE(mz_zip_writer_finalize_archive(&zip.arch));
}

std::string unicode_path_field(const std::string& legacy_name, const std::string& utf8_name)
{
    const std::uint32_t crc = static_cast<std::uint32_t>(
        mz_crc32(0, reinterpret_cast<const unsigned char*>(legacy_name.data()), legacy_name.size()));
    const std::uint16_t len = static_cast<std::uint16_t>(5 + utf8_name.size());
    std::string         field;
    field.push_back(0x75);
    field.push_back(0x70);
    field.push_back(static_cast<char>(len & 0xFF));
    field.push_back(static_cast<char>(len >> 8));
    field.push_back(0x01);
    for (int i = 0; i < 4; ++i)
        field.push_back(static_cast<char>((crc >> (8 * i)) & 0xFF));
    field += utf8_name;
    return field;
}

std::string aes_extra_field()
{
    // WinZip AE-2 extra field (header id 0x9901, data size 7).
    std::string field;
    field.push_back(static_cast<char>(0x01));
    field.push_back(static_cast<char>(0x99));
    field.push_back(7);
    field.push_back(0);
    field.push_back(2); // version AE-2
    field.push_back(0);
    field.push_back('A');
    field.push_back('E');
    field.push_back(0x03); // AES-256
    field.push_back(8);    // stored compression method: deflate
    field.push_back(0);
    return field;
}

void rename_zip_entry(const fs::path& zip_file, const std::string& from, const std::string& to)
{
    REQUIRE(from.size() == to.size());
    std::string bytes = read_binary_file(zip_file);
    size_t      count = 0;
    for (size_t pos = bytes.find(from); pos != std::string::npos; pos = bytes.find(from, pos + to.size()), ++count)
        bytes.replace(pos, from.size(), to);
    REQUIRE(count == 2); // local header + central directory
    write_binary_file(zip_file, bytes);
}

void mark_last_zip_entry_symlink(const fs::path& zip_file)
{
    std::string bytes = read_binary_file(zip_file);
    const char  sig[] = {'P', 'K', 1, 2};
    size_t      last  = std::string::npos;
    for (size_t pos = 0; (pos = bytes.find(std::string(sig, 4), pos)) != std::string::npos; pos += 4)
        last = pos;
    REQUIRE(last != std::string::npos);
    REQUIRE(last + 42 <= bytes.size());
    const std::uint32_t attr = (0120777u << 16);
    bytes[last + 38]         = static_cast<char>(attr & 0xFF);
    bytes[last + 39]         = static_cast<char>((attr >> 8) & 0xFF);
    bytes[last + 40]         = static_cast<char>((attr >> 16) & 0xFF);
    bytes[last + 41]         = static_cast<char>((attr >> 24) & 0xFF);
    write_binary_file(zip_file, bytes);
}

void set_encryption_bit(const fs::path& zip_file, const std::string& entry)
{
    std::string bytes   = read_binary_file(zip_file);
    bool        patched = false;
    // Local header: signature PK\3\4, bit flag at +6.
    const std::string lsig("PK\x03\x04", 4);
    for (size_t pos = bytes.find(lsig); pos != std::string::npos; pos = bytes.find(lsig, pos + 4)) {
        if (pos + 30 + entry.size() > bytes.size())
            break;
        const size_t name_len = static_cast<unsigned char>(bytes[pos + 26]) | (static_cast<unsigned char>(bytes[pos + 27]) << 8);
        if (name_len == entry.size() && bytes.compare(pos + 30, name_len, entry) == 0) {
            bytes[pos + 6] = static_cast<char>(static_cast<unsigned char>(bytes[pos + 6]) | 0x01);
            patched        = true;
        }
    }
    // Central directory: signature PK\1\2, bit flag at +8.
    const std::string csig("PK\x01\x02", 4);
    for (size_t pos = bytes.find(csig); pos != std::string::npos; pos = bytes.find(csig, pos + 4)) {
        if (pos + 46 + entry.size() > bytes.size())
            break;
        const size_t name_len = static_cast<unsigned char>(bytes[pos + 28]) | (static_cast<unsigned char>(bytes[pos + 29]) << 8);
        if (name_len == entry.size() && bytes.compare(pos + 46, name_len, entry) == 0) {
            bytes[pos + 8] = static_cast<char>(static_cast<unsigned char>(bytes[pos + 8]) | 0x01);
            patched        = true;
        }
    }
    REQUIRE(patched);
    write_binary_file(zip_file, bytes);
}

void patch_declared_uncomp_size(const fs::path& zip_file, const std::string& entry, std::uint32_t declared)
{
    std::string       bytes   = read_binary_file(zip_file);
    bool              patched = false;
    const std::string csig("PK\x01\x02", 4);
    for (size_t pos = bytes.find(csig); pos != std::string::npos; pos = bytes.find(csig, pos + 4)) {
        if (pos + 46 > bytes.size())
            break;
        const size_t name_len = static_cast<unsigned char>(bytes[pos + 28]) | (static_cast<unsigned char>(bytes[pos + 29]) << 8);
        if (name_len != entry.size() || bytes.compare(pos + 46, name_len, entry) != 0)
            continue;
        for (int i = 0; i < 4; ++i)
            bytes[pos + 24 + i] = static_cast<char>((declared >> (8 * i)) & 0xFF);
        patched = true;
    }
    const std::string lsig("PK\x03\x04", 4);
    for (size_t pos = bytes.find(lsig); pos != std::string::npos; pos = bytes.find(lsig, pos + 4)) {
        if (pos + 30 > bytes.size())
            break;
        const size_t name_len = static_cast<unsigned char>(bytes[pos + 26]) | (static_cast<unsigned char>(bytes[pos + 27]) << 8);
        if (name_len != entry.size() || bytes.compare(pos + 30, name_len, entry) != 0)
            continue;
        for (int i = 0; i < 4; ++i)
            bytes[pos + 22 + i] = static_cast<char>((declared >> (8 * i)) & 0xFF);
        patched = true;
    }
    REQUIRE(patched);
    write_binary_file(zip_file, bytes);
}

void patch_crc(const fs::path& zip_file, const std::string& entry)
{
    std::string       bytes   = read_binary_file(zip_file);
    bool              patched = false;
    const std::string csig("PK\x01\x02", 4);
    for (size_t pos = bytes.find(csig); pos != std::string::npos; pos = bytes.find(csig, pos + 4)) {
        if (pos + 46 > bytes.size())
            break;
        const size_t name_len = static_cast<unsigned char>(bytes[pos + 28]) | (static_cast<unsigned char>(bytes[pos + 29]) << 8);
        if (name_len != entry.size() || bytes.compare(pos + 46, name_len, entry) != 0)
            continue;
        bytes[pos + 16] = static_cast<char>(static_cast<unsigned char>(bytes[pos + 16]) ^ 0xFF);
        patched         = true;
    }
    const std::string lsig("PK\x03\x04", 4);
    for (size_t pos = bytes.find(lsig); pos != std::string::npos; pos = bytes.find(lsig, pos + 4)) {
        if (pos + 30 > bytes.size())
            break;
        const size_t name_len = static_cast<unsigned char>(bytes[pos + 26]) | (static_cast<unsigned char>(bytes[pos + 27]) << 8);
        if (name_len != entry.size() || bytes.compare(pos + 30, name_len, entry) != 0)
            continue;
        bytes[pos + 14] = static_cast<char>(static_cast<unsigned char>(bytes[pos + 14]) ^ 0xFF);
        patched         = true;
    }
    REQUIRE(patched);
    write_binary_file(zip_file, bytes);
}

void mark_split_zip(const fs::path& zip_file)
{
    std::string       bytes = read_binary_file(zip_file);
    const std::string sig("PK\x05\x06", 4);
    const size_t      pos = bytes.rfind(sig);
    REQUIRE(pos != std::string::npos);
    REQUIRE(pos + 6 <= bytes.size());
    bytes[pos + 4] = 1; // number of this disk
    bytes[pos + 5] = 0;
    write_binary_file(zip_file, bytes);
}

std::vector<fs::path> list_regular_files(const fs::path& root)
{
    std::vector<fs::path>     out;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (fs::is_regular_file(it->path(), ec))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool extract_all(const fs::path& zip, const fs::path& dest, const ZipExtractCaps& caps, std::string& err)
{
    MZ_Archive archive;
    REQUIRE(archive.open_reader(zip.string()));
    const mz_uint        n = mz_zip_reader_get_num_files(&archive.arch);
    std::vector<mz_uint> indices;
    indices.reserve(n);
    for (mz_uint i = 0; i < n; ++i)
        indices.push_back(i);
    return extract_entries_confined(archive.arch, indices, dest, caps, err);
}

} // namespace

TEST_CASE("ZipExtractCaps defaults match the spec", "[ZipExtract]")
{
    const ZipExtractCaps caps;
    CHECK(caps.max_entries_scanned == 10000);
    CHECK(caps.max_entry_uncompressed == 2ull * 1024ull * 1024ull * 1024ull);
    CHECK(caps.max_total_uncompressed == 4ull * 1024ull * 1024ull * 1024ull);
    CHECK(caps.max_compression_ratio == 200);
    CHECK(caps.ratio_min_uncompressed == 1ull * 1024ull * 1024ull);
}

TEST_CASE("extract_entries_confined writes only the selected entries", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "bundle.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"keep.stl", "AAA"}, {"skip.stl", "BBB"}, {"models/part.obj", "CCC"}});

    std::string err;
    REQUIRE(extract_entries_confined(zip, {0, 2}, dest, ZipExtractCaps{}, err));
    CHECK(err.empty());
    CHECK(read_binary_file(dest / "keep.stl") == "AAA");
    CHECK(read_binary_file(dest / "models" / "part.obj") == "CCC");
    CHECK_FALSE(fs::exists(dest / "skip.stl"));
}

TEST_CASE("extract_entries_confined rejects traversal, absolute, drive, look-alike and backslash paths", "[ZipExtract][ZipSlip]")
{
    TempDir              tmp;
    const ZipExtractCaps caps;

    const std::vector<std::pair<std::string, std::string>> cases = {
        {"xx/evil.stl", "../evil.stl"},     // 11
        {"_tmp_evil.stl", "/tmp/evil.stl"}, // 13
        {"XXevil.stl", "C:evil.stl"},       // 10
        {"XXXXXXXXXXXXX.txt", "\xEF\xBC\x8E\xEF\xBC\x8E\xEF\xBC\x8F"
                              "evil.txt"}, // 17
        {"fwcolonx.stl", "C\xEF\xBC\x9A"
                         "evil.txt"},     // 12
        {"xx\\evil.stl", "..\\evil.stl"}, // 11
        {"evilXname.st", std::string("evil\0name.st", 12)},
    };

    for (const auto& c : cases) {
        DYNAMIC_SECTION("hostile name " << c.first)
        {
            const auto zip  = tmp.dir / (c.first + ".zip");
            const auto dest = tmp.dir / (c.first + ".out");
            fs::create_directories(dest);
            write_zip_entries(zip, {{"good.stl", "ok"}, {c.first, "evil"}});
            if (c.first != c.second)
                rename_zip_entry(zip, c.first, c.second);

            std::string err;
            CHECK_FALSE(extract_all(zip, dest, caps, err));
            CHECK_FALSE(err.empty());
            CHECK(list_regular_files(dest).empty());
            CHECK_FALSE(fs::exists(tmp.dir / "evil.stl"));
            CHECK_FALSE(fs::exists(tmp.dir / "evil.txt"));
        }
    }
}

TEST_CASE("extract_entries_confined rejects a symlink entry", "[ZipExtract][ZipSlip]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "link.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"good.stl", "ok"}, {"link.stl", "/etc/passwd"}});
    mark_last_zip_entry_symlink(zip);

    std::string err;
    CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(err.find("symlink") != std::string::npos);
    CHECK(list_regular_files(dest).empty());
}

TEST_CASE("extract_entries_confined refuses a lying uncompressed size", "[ZipExtract][ZipBomb]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "lie.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    const std::string payload(64 * 1024, 'A');
    write_zip_entries(zip, {{"good.stl", "ok"}, {"bomb.stl", payload}});
    patch_declared_uncomp_size(zip, "bomb.stl", 16);

    std::string err;
    CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK_FALSE(err.empty());
    CHECK(list_regular_files(dest).empty());
}

TEST_CASE("extract_entries_confined enforces the compression-ratio cap", "[ZipExtract][ZipBomb]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "ratio.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    // 1.5 MiB of zeros deflates to a few KiB (ratio >> 200).
    write_zip_entries(zip, {{"zeros.stl", std::string(3u * 512u * 1024u, '\0')}});

    ZipExtractCaps tight;
    tight.max_compression_ratio  = 10;
    tight.ratio_min_uncompressed = 1024 * 1024;
    std::string err;
    CHECK_FALSE(extract_all(zip, dest, tight, err));
    CHECK(err.find("ratio") != std::string::npos);
    CHECK(list_regular_files(dest).empty());

    ZipExtractCaps loose;
    loose.max_compression_ratio = 100000;
    REQUIRE(extract_all(zip, dest, loose, err));
    CHECK(fs::file_size(dest / "zeros.stl") == 3u * 512u * 1024u);
}

TEST_CASE("extract_entries_confined enforces the per-entry and total size caps", "[ZipExtract][ZipBomb]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "size.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"a.stl", std::string(200, 'a')}, {"b.stl", std::string(200, 'b')}});

    ZipExtractCaps per_entry;
    per_entry.max_entry_uncompressed = 100;
    std::string err;
    CHECK_FALSE(extract_all(zip, dest, per_entry, err));
    CHECK(err.find("larger") != std::string::npos);
    CHECK(list_regular_files(dest).empty());

    ZipExtractCaps total;
    total.max_total_uncompressed = 250;
    CHECK_FALSE(extract_all(zip, dest, total, err));
    CHECK(err.find("total") != std::string::npos);
    CHECK(list_regular_files(dest).empty());
}

TEST_CASE("extract_entries_confined enforces the entry-count cap", "[ZipExtract][ZipBomb]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "many.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    std::vector<std::pair<std::string, std::string>> entries;
    for (int i = 0; i < 8; ++i)
        entries.push_back({"f" + std::to_string(i) + ".stl", "x"});
    write_zip_entries(zip, entries);

    ZipExtractCaps caps;
    caps.max_entries_scanned = 5;
    std::string err;
    CHECK_FALSE(extract_entries_confined(zip, {0}, dest, caps, err));
    CHECK(err.find("too many entries") != std::string::npos);
    CHECK(list_regular_files(dest).empty());
}

TEST_CASE("zip_entry_is_encrypted reports general-purpose bit 0 and AES extra field 0x9901", "[ZipExtract]")
{
    TempDir tmp;

    SECTION("traditional encryption bit")
    {
        const auto zip = tmp.dir / "trad.zip";
        write_zip_entries(zip, {{"plain.stl", "a"}, {"secret.stl", "b"}});
        set_encryption_bit(zip, "secret.stl");

        MZ_Archive archive;
        REQUIRE(archive.open_reader(zip.string()));
        ZipExtractCaps            caps;
        std::vector<ZipEntryInfo> infos;
        std::string               err;
        REQUIRE(inspect_zip_entries(archive.arch, caps, infos, err));
        REQUIRE(infos.size() == 2);
        CHECK_FALSE(infos[0].is_encrypted);
        CHECK(infos[1].is_encrypted);
        CHECK(infos[1].raw_name == "secret.stl");
    }

    SECTION("AES extra field without bit 0")
    {
        const auto zip = tmp.dir / "aes.zip";
        write_zip_with_extra(zip, "aes.stl", "payload", aes_extra_field());

        MZ_Archive archive;
        REQUIRE(archive.open_reader(zip.string()));
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&archive.arch, 0, &stat));
        CHECK(zip_entry_is_encrypted(&archive.arch, stat));
        CHECK_FALSE(stat.m_bit_flag & 0x1u);
    }
}

TEST_CASE("extract_entries_confined refuses encrypted selected entries and names them", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "enc.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"plain.stl", "a"}, {"secret.stl", "b"}});
    set_encryption_bit(zip, "secret.stl");

    std::string err;
    CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(err.find("encrypted") != std::string::npos);
    CHECK(err.find("secret.stl") != std::string::npos);
    CHECK(list_regular_files(dest).empty());
}

TEST_CASE("extract_entries_confined reports a split zip with a clear error", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "split.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"a.stl", "a"}});
    mark_split_zip(zip);

    std::string err;
    CHECK_FALSE(extract_entries_confined(zip, {0}, dest, ZipExtractCaps{}, err));
    CHECK(err.find("split") != std::string::npos);
    CHECK(list_regular_files(dest).empty());
}

TEST_CASE("extract_entries_confined decodes non-UTF-8 names (CP437, Shift-JIS + Unicode extra)", "[ZipExtract]")
{
    TempDir    tmp;
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);

    SECTION("CP437 without a Unicode extra field is escaped")
    {
        const auto        zip    = tmp.dir / "cp437.zip";
        const std::string legacy = "caf\x82.stl"; // é in CP437
        write_zip_with_extra(zip, legacy, "body", std::string());

        std::string err;
        REQUIRE(extract_entries_confined(zip, {0}, dest, ZipExtractCaps{}, err));
        const auto files = list_regular_files(dest);
        REQUIRE(files.size() == 1);
        CHECK(read_binary_file(files.front()) == "body");
#ifdef _WIN32
        CHECK(zip_name_is_valid_utf8(path_u8(files.front().filename())));
#else
        CHECK(files.front().filename().string() == escape_zip_entry_bytes(legacy));
#endif
    }

    SECTION("Shift-JIS name plus a matching Unicode extra field extracts as UTF-8")
    {
        const auto        zip  = tmp.dir / "sjis.zip";
        const std::string sjis = "\x82\xA0.stl";     // あ in Shift-JIS
        const std::string utf8 = "\xE3\x81\x82.stl"; // あ in UTF-8
        write_zip_with_extra(zip, sjis, "kana", unicode_path_field(sjis, utf8));

        std::string err;
        REQUIRE(extract_entries_confined(zip, {0}, dest, ZipExtractCaps{}, err));
        CHECK(read_binary_file(dest / utf8) == "kana");
    }
}

TEST_CASE("extract_entries_confined suffixes duplicate entry names", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "dup.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"model.stl", "first"}, {"model.stl", "second"}});

    std::string err;
    REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(read_binary_file(dest / "model.stl") == "first");
    CHECK(read_binary_file(dest / "model(1).stl") == "second");
}

TEST_CASE("extract_entries_confined suffixes case collisions on Windows and macOS only", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "case.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"Model.stl", "A"}, {"model.stl", "b"}});

    std::string err;
    REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
#if defined(_WIN32) || defined(__APPLE__)
    CHECK(fs::exists(dest / "Model.stl"));
    CHECK((fs::exists(dest / "Model(1).stl") || fs::exists(dest / "model(1).stl")));
#else
    CHECK(read_binary_file(dest / "Model.stl") == "A");
    CHECK(read_binary_file(dest / "model.stl") == "b");
    CHECK_FALSE(fs::exists(dest / "Model(1).stl"));
    CHECK_FALSE(fs::exists(dest / "model(1).stl"));
#endif
}

TEST_CASE("sanitize_zip_extract_path defuses reserved names and invalid Windows characters", "[ZipExtract]")
{
    CHECK(sanitize_zip_extract_path("CON.stl") == "_CON.stl");
    CHECK(sanitize_zip_extract_path("NUL") == "_NUL");
    CHECK(sanitize_zip_extract_path("models/COM1.obj") == "models/_COM1.obj");
    CHECK(sanitize_zip_extract_path("a<b>:c|d?.stl") == "a_b__c_d_.stl");
    CHECK(zip_name_is_valid_utf8("caf\xC3\xA9.stl"));
    CHECK_FALSE(zip_name_is_valid_utf8("caf\x82.stl"));
    CHECK(escape_zip_entry_bytes("caf\x82.stl") == "caf%82.stl");
}

TEST_CASE("extract_entries_confined writes a reserved name to its sanitized dest", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "reserved.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"CON.stl", "device"}});

    std::string err;
    REQUIRE(extract_entries_confined(zip, {0}, dest, ZipExtractCaps{}, err));
    CHECK(read_binary_file(dest / "_CON.stl") == "device");
    CHECK_FALSE(fs::exists(dest / "CON.stl"));
}

TEST_CASE("extract_entries_confined leaves no partial files after a failure", "[ZipExtract]")
{
    TempDir    tmp;
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);

    SECTION("good entry plus a traversal")
    {
        const auto zip = tmp.dir / "mix.zip";
        write_zip_entries(zip, {{"good.stl", "ok"}, {"xx/evil.stl", "evil"}});
        rename_zip_entry(zip, "xx/evil.stl", "../evil.stl");
        std::string err;
        CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
        CHECK(list_regular_files(dest).empty());
    }

    SECTION("CRC mismatch after a good sibling")
    {
        const auto zip = tmp.dir / "crc.zip";
        write_zip_entries(zip, {{"good.stl", "ok"}, {"bad.stl", "payload"}});
        patch_crc(zip, "bad.stl");
        std::string err;
        CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
        INFO(err);
        CHECK((err.find("CRC") != std::string::npos || err.find("extract failed") != std::string::npos ||
               err.find("inflat") != std::string::npos));
        CHECK(list_regular_files(dest).empty());
    }
}

TEST_CASE("ZipWorkDir creates a session folder, marker, and collision names", "[ZipExtract][ZipWorkDir]")
{
    TempDir    tmp;
    const auto root = tmp.dir / "work";
    const auto zip  = tmp.dir / "My Model.zip";
    write_binary_file(zip, "not-a-zip"); // only the stem is used

    std::string err;
    fs::path    session;
    REQUIRE(ZipWorkDir::create_session(root, zip, session, err));
    CHECK(err.empty());
    CHECK(fs::is_directory(session));
    CHECK(fs::exists(root / ZipWorkDir::marker_name()));
    CHECK(session.parent_path() == root);
    CHECK(session.filename().string().find("My Model_") == 0);

    fs::path session2;
    REQUIRE(ZipWorkDir::create_session(root, zip, session2, err));
    CHECK(session2 != session);

    const fs::path first = ZipWorkDir::uniquify_entry_path(session, "models/part.stl", {});
    CHECK(first == session / "models" / "part.stl");
    const fs::path second = ZipWorkDir::uniquify_entry_path(session, "models/part.stl", {first});
    CHECK(second == session / "models" / "part(1).stl");

    fs::create_directories(first.parent_path());
    write_binary_file(first, "x");
    const fs::path third = ZipWorkDir::uniquify_entry_path(session, "models/part.stl", {});
    CHECK(third == session / "models" / "part(1).stl");
}

TEST_CASE("ZipWorkDir suffixes case collisions on Windows and macOS only", "[ZipExtract][ZipWorkDir]")
{
    TempDir    tmp;
    const auto session = tmp.dir / "session";
    fs::create_directories(session);
    const fs::path a = ZipWorkDir::uniquify_entry_path(session, "Model.stl", {});
#if defined(_WIN32) || defined(__APPLE__)
    const fs::path b = ZipWorkDir::uniquify_entry_path(session, "model.stl", {a});
    CHECK(b.filename().string().find("(1)") != std::string::npos);
#else
    const fs::path b = ZipWorkDir::uniquify_entry_path(session, "model.stl", {a});
    CHECK(b.filename().string() == "model.stl");
#endif
}

TEST_CASE("inspect_zip_entries reports encryption per entry and honours the scan cap", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip = tmp.dir / "scan.zip";
    write_zip_entries(zip, {{"a.stl", "a"}, {"b.stl", "b"}, {"c.stl", "c"}});
    set_encryption_bit(zip, "b.stl");

    MZ_Archive archive;
    REQUIRE(archive.open_reader(zip.string()));
    std::vector<ZipEntryInfo> infos;
    std::string               err;
    REQUIRE(inspect_zip_entries(archive.arch, ZipExtractCaps{}, infos, err));
    REQUIRE(infos.size() == 3);
    CHECK_FALSE(infos[0].is_encrypted);
    CHECK(infos[1].is_encrypted);
    CHECK_FALSE(infos[2].is_encrypted);
    CHECK(infos[0].is_safe);
    CHECK_FALSE(infos[1].is_safe);

    ZipExtractCaps caps;
    caps.max_entries_scanned = 2;
    CHECK_FALSE(inspect_zip_entries(archive.arch, caps, infos, err));
    CHECK(err.find("too many entries") != std::string::npos);
}

TEST_CASE("extract_entries_confined keeps a pre-existing dest file through a rollback", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "keep.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_binary_file(dest / "model.stl", "original");
    write_zip_entries(zip, {{"model.stl", "new"}, {"bad.stl", "payload"}});
    patch_crc(zip, "bad.stl");

    std::string err;
    CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(read_binary_file(dest / "model.stl") == "original");
    const auto files = list_regular_files(dest);
    REQUIRE(files.size() == 1);
    CHECK(files.front().filename() == "model.stl");
}

#ifndef _WIN32
TEST_CASE("extract_entries_confined refuses to write through a pre-existing dir symlink that leaves dest", "[ZipExtract][ZipSlip]")
{
    TempDir    tmp;
    const auto zip     = tmp.dir / "link.zip";
    const auto dest    = tmp.dir / "out";
    const auto outside = tmp.dir / "outside";
    fs::create_directories(dest);
    fs::create_directories(outside);
    fs::create_directory_symlink(outside, dest / "link");
    write_zip_entries(zip, {{"link/evil.stl", "evil"}});

    std::string err;
    CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(list_regular_files(outside).empty());
    CHECK(fs::is_symlink(fs::symlink_status(dest / "link")));
}

TEST_CASE("extract_entries_confined follows a pre-existing dir symlink that stays inside dest", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "inlink.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest / "real");
    fs::create_directory_symlink(dest / "real", dest / "alias");
    write_zip_entries(zip, {{"alias/part.stl", "ok"}});

    std::string err;
    REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(read_binary_file(dest / "real" / "part.stl") == "ok");
}
#endif

TEST_CASE("extract_entries_confined renames a file that clashes with a folder in the same archive", "[ZipExtract]")
{
    TempDir    tmp;
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);

    SECTION("file first")
    {
        const auto zip = tmp.dir / "clash1.zip";
        write_zip_entries(zip, {{"a", "file"}, {"a/b.stl", "inner"}});
        std::string err;
        REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
        CHECK(fs::is_directory(dest / "a"));
        CHECK(read_binary_file(dest / "a" / "b.stl") == "inner");
        CHECK(read_binary_file(dest / "a(1)") == "file");
    }
    SECTION("folder first")
    {
        const auto zip = tmp.dir / "clash2.zip";
        write_zip_entries(zip, {{"a/b.stl", "inner"}, {"a", "file"}});
        std::string err;
        REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
        CHECK(read_binary_file(dest / "a" / "b.stl") == "inner");
        CHECK(read_binary_file(dest / "a(1)") == "file");
    }
    SECTION("a file already in dest blocks the folder with a clear error")
    {
        const auto zip = tmp.dir / "clash3.zip";
        write_binary_file(dest / "a", "existing");
        write_zip_entries(zip, {{"ok.stl", "ok"}, {"a/b.stl", "inner"}});
        std::string err;
        CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
        INFO(err);
        CHECK(err.find("already exists") != std::string::npos);
        CHECK(read_binary_file(dest / "a") == "existing");
        CHECK(list_regular_files(dest).size() == 1);
    }
}

TEST_CASE("extract_entries_confined ignores duplicate indices", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "dupidx.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"one.stl", "1"}, {"two.stl", "2"}});

    std::string err;
    REQUIRE(extract_entries_confined(zip, {0, 1, 0, 0, 1}, dest, ZipExtractCaps{}, err));
    const auto files = list_regular_files(dest);
    REQUIRE(files.size() == 2);
    CHECK(read_binary_file(dest / "one.stl") == "1");
    CHECK(read_binary_file(dest / "two.stl") == "2");
    CHECK_FALSE(fs::exists(dest / "one(1).stl"));
}

TEST_CASE("zip_name_is_valid_utf8 handles end-of-name multibyte, overlongs and surrogates", "[ZipExtract]")
{
    // Valid multibyte as the very last character (2, 3 and 4 bytes).
    CHECK(zip_name_is_valid_utf8("caf\xC3\xA9"));
    CHECK(zip_name_is_valid_utf8("x\xE2\x82\xAC"));
    CHECK(zip_name_is_valid_utf8("x\xF0\x9F\x98\x80"));
    CHECK(zip_name_is_valid_utf8("\xED\x9F\xBF"));     // U+D7FF, just below the surrogates
    CHECK(zip_name_is_valid_utf8("\xF4\x8F\xBF\xBF")); // U+10FFFF
    // Truncated at the end.
    CHECK_FALSE(zip_name_is_valid_utf8("caf\xC3"));
    CHECK_FALSE(zip_name_is_valid_utf8("x\xE2\x82"));
    CHECK_FALSE(zip_name_is_valid_utf8("x\xF0\x9F\x98"));
    // Overlongs.
    CHECK_FALSE(zip_name_is_valid_utf8("\xC0\xAF"));
    CHECK_FALSE(zip_name_is_valid_utf8("\xE0\x80\xAF"));
    CHECK_FALSE(zip_name_is_valid_utf8("\xF0\x80\x80\xAF"));
    // Surrogates and out of range.
    CHECK_FALSE(zip_name_is_valid_utf8("\xED\xA0\x80"));
    CHECK_FALSE(zip_name_is_valid_utf8("\xED\xBF\xBF"));
    CHECK_FALSE(zip_name_is_valid_utf8("\xF4\x90\x80\x80"));
    CHECK_FALSE(zip_name_is_valid_utf8("\xF5\x80\x80\x80"));
}

TEST_CASE("extract_entries_confined keeps a multibyte last character in the name", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "mb.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    const std::string name = "caf\xC3\xA9";
    write_zip_entries(zip, {{name, "x"}});

    std::string err;
    REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
    const auto files = list_regular_files(dest);
    REQUIRE(files.size() == 1);
    CHECK(files.front().filename().string().find('%') == std::string::npos);
}

TEST_CASE("sanitize_zip_extract_path treats superscript and padded device names as reserved", "[ZipExtract]")
{
    CHECK(sanitize_zip_extract_path("COM\xC2\xB9.stl") == "_COM\xC2\xB9.stl");
    CHECK(sanitize_zip_extract_path("com\xC2\xB2") == "_com\xC2\xB2");
    CHECK(sanitize_zip_extract_path("LPT\xC2\xB3.txt") == "_LPT\xC2\xB3.txt");
    CHECK(sanitize_zip_extract_path("lpt\xC2\xB9") == "_lpt\xC2\xB9");
    CHECK(sanitize_zip_extract_path("CON .txt") == "_CON .txt");
    CHECK(sanitize_zip_extract_path("nul. .stl") == "_nul. .stl");
    CHECK(sanitize_zip_extract_path("COM\xC2\xB9" "0.stl") == "COM\xC2\xB9" "0.stl");
    CHECK(sanitize_zip_extract_path("CONSOLE.txt") == "CONSOLE.txt");
}

TEST_CASE("extract_entries_confined never lets Unicode case or normalization variants overwrite each other", "[ZipExtract]")
{
    TempDir    tmp;
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);

    SECTION("upper and lower case E-acute")
    {
        const auto zip = tmp.dir / "ucase.zip";
        write_zip_entries(zip, {{"\xC3\x89.stl", "upper"}, {"\xC3\xA9.stl", "lower"}});
        std::string err;
        REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
        const auto files = list_regular_files(dest);
        REQUIRE(files.size() == 2);
        std::vector<std::string> contents;
        for (const auto& f : files)
            contents.push_back(read_binary_file(f));
        std::sort(contents.begin(), contents.end());
        CHECK(contents == std::vector<std::string>{"lower", "upper"});
    }
    SECTION("NFC and NFD spellings")
    {
        const auto zip = tmp.dir / "nfd.zip";
        write_zip_entries(zip, {{"caf\xC3\xA9.stl", "nfc"}, {"cafe\xCC\x81.stl", "nfd"}});
        std::string err;
        REQUIRE(extract_all(zip, dest, ZipExtractCaps{}, err));
        const auto files = list_regular_files(dest);
        REQUIRE(files.size() == 2);
        std::vector<std::string> contents;
        for (const auto& f : files)
            contents.push_back(read_binary_file(f));
        std::sort(contents.begin(), contents.end());
        CHECK(contents == std::vector<std::string>{"nfc", "nfd"});
    }
}

TEST_CASE("extract_entries_confined rejects a zero-size entry with a non-zero CRC", "[ZipExtract]")
{
    TempDir    tmp;
    const auto zip  = tmp.dir / "empty.zip";
    const auto dest = tmp.dir / "out";
    fs::create_directories(dest);
    write_zip_entries(zip, {{"empty.stl", ""}});
    patch_crc(zip, "empty.stl");

    std::string err;
    CHECK_FALSE(extract_all(zip, dest, ZipExtractCaps{}, err));
    CHECK(list_regular_files(dest).empty());
}
