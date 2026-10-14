#include <catch2/catch.hpp>

#include "slic3r/Utils/VendorConnector.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Vendors;
using json = nlohmann::json;

// The Vendors page's CSV export: more rows than the chosen maximum are written as numbered files
// (src/slic3r/GUI/HomeVendors.cpp, export_csv). The splitting itself is Vendors::to_csv_parts.

namespace {

// A reader for what to_csv writes: RFC 4180 quoting (doubled quotes, commas and line breaks inside
// quotes), rows ending in CRLF, a leading byte order mark skipped. One vector of cells per record.
std::vector<std::vector<std::string>> parse_csv(const std::string& text)
{
    std::vector<std::vector<std::string>> rows;
    size_t                                at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    std::vector<std::string>              row;
    std::string                           cell;
    bool                                  quoted = false, any = false;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (quoted) {
            if (c == '"' && at + 1 < text.size() && text[at + 1] == '"') {
                cell += '"';
                ++at;
            } else if (c == '"')
                quoted = false;
            else
                cell += c;
        } else if (c == '"') {
            quoted = true;
            any    = true;
        } else if (c == ',') {
            row.push_back(cell);
            cell.clear();
            any = true;
        } else if (c == '\r' && at + 1 < text.size() && text[at + 1] == '\n') {
            row.push_back(cell);
            cell.clear();
            rows.push_back(row);
            row.clear();
            any = false;
            ++at;
        } else {
            cell += c;
            any = true;
        }
    }
    if (any) {
        row.push_back(cell);
        rows.push_back(row);
    }
    return rows;
}

std::vector<Item> numbered_items(int n, const std::string& name_prefix = "Model ")
{
    std::vector<Item> items;
    for (int k = 0; k < n; ++k) {
        Item i;
        i.id   = std::to_string(k);
        i.name = name_prefix + std::to_string(k);
        items.push_back(i);
    }
    return items;
}

std::vector<CsvGroup> one_group(std::vector<Item> items)
{
    return {CsvGroup{"CPL3D", "CPL3D", std::move(items)}};
}

constexpr size_t COLUMNS = 17;

} // namespace

TEST_CASE("csv split: 500 rows with a maximum of 200 make 200, 200 and 100", "[Vendors][CsvSplit]")
{
    const std::vector<CsvGroup>    groups = one_group(numbered_items(500));
    const std::vector<std::string> files  = to_csv_parts(groups, 200);
    REQUIRE(files.size() == 3);
    const size_t expected[] = {200, 200, 100};
    int          next       = 0;
    for (size_t f = 0; f < files.size(); ++f) {
        const auto rows = parse_csv(files[f]);
        REQUIRE(rows.size() == expected[f] + 1); // plus the header
        for (size_t r = 1; r < rows.size(); ++r) {
            REQUIRE(rows[r].size() == COLUMNS);
            CHECK(rows[r][2] == std::to_string(next)); // page order, no gaps, no repeats
            ++next;
        }
    }
    CHECK(next == 500);
    CHECK(csv_row_count(groups) == 500);
}

TEST_CASE("csv split: as many rows as the maximum is one file", "[Vendors][CsvSplit]")
{
    const std::vector<CsvGroup> groups = one_group(numbered_items(200));
    for (size_t max : {size_t(200), size_t(201), size_t(100000)}) {
        const std::vector<std::string> files = to_csv_parts(groups, max);
        REQUIRE(files.size() == 1);
        CHECK(files[0] == to_csv("CPL3D", "CPL3D", groups[0].items)); // the same as the file made before the option existed
    }
    CHECK(to_csv_parts(groups, 199).size() == 2);
    CHECK(to_csv_parts(groups, 1).size() == 200);
}

TEST_CASE("csv split: All (0) is one file however many rows", "[Vendors][CsvSplit]")
{
    const std::vector<CsvGroup>    groups = one_group(numbered_items(1234));
    const std::vector<std::string> files  = to_csv_parts(groups, 0);
    REQUIRE(files.size() == 1);
    CHECK(parse_csv(files[0]).size() == 1235);
    CHECK(files[0] == to_csv("CPL3D", "CPL3D", groups[0].items));
}

TEST_CASE("csv split: nothing to write still gives a file with the header", "[Vendors][CsvSplit]")
{
    for (size_t max : {size_t(0), size_t(5)}) {
        const std::vector<std::string> files = to_csv_parts(one_group({}), max);
        REQUIRE(files.size() == 1);
        const auto rows = parse_csv(files[0]);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].size() == COLUMNS);
    }
}

TEST_CASE("csv split: every file starts with the byte order mark and the header row", "[Vendors][CsvSplit]")
{
    const std::vector<std::string> files = to_csv_parts(one_group(numbered_items(25)), 10);
    REQUIRE(files.size() == 3);
    const auto first = parse_csv(files[0]);
    REQUIRE(!first.empty());
    CHECK(first[0][0] == "connector");
    CHECK(first[0][2] == "item_id");
    for (const std::string& f : files) {
        CHECK(f.compare(0, 3, "\xEF\xBB\xBF") == 0);
        const auto rows = parse_csv(f);
        REQUIRE(!rows.empty());
        CHECK(rows[0] == first[0]);
        CHECK(f.find("\xEF\xBB\xBF", 3) == std::string::npos); // one mark per file
    }
}

TEST_CASE("csv split: a row with commas, quotes or line breaks is never cut", "[Vendors][CsvSplit]")
{
    std::vector<Item> items;
    const char* const names[] = {"plain", "a,b,c", "say \"hi\"", "two\nlines", "cr\r\nlf, and \"both\"", "x"};
    int               k       = 0;
    for (const char* n : names) {
        Item i;
        i.id       = std::to_string(k++);
        i.name     = n;
        i.designer = "Smith, J \"Jr\"\n";
        items.push_back(i);
    }
    // 1 row per file, 2 per file, 4 per file: every file still reads back whole, in order.
    for (size_t max : {size_t(1), size_t(2), size_t(4)}) {
        const std::vector<std::string> files = to_csv_parts(one_group(items), max);
        REQUIRE(files.size() == (items.size() + max - 1) / max);
        size_t next = 0;
        for (const std::string& f : files) {
            const auto rows = parse_csv(f);
            REQUIRE(rows.size() >= 2);
            CHECK(rows.size() - 1 <= max);
            for (size_t r = 1; r < rows.size(); ++r, ++next) {
                REQUIRE(rows[r].size() == COLUMNS);
                CHECK(rows[r][2] == std::to_string(next));
                CHECK(rows[r][3] == names[next]);
                CHECK(rows[r][4] == "Smith, J \"Jr\"\n");
            }
        }
        CHECK(next == items.size());
    }
}

TEST_CASE("csv split: the files of an item count as rows and each row names its connector", "[Vendors][CsvSplit]")
{
    Item a;
    a.id   = "1";
    a.name = "Three files";
    for (int n = 0; n < 3; ++n) {
        SubItem s;
        s.id   = "f" + std::to_string(n);
        s.name = "File " + std::to_string(n);
        a.subs.push_back(s);
    }
    Item b;
    b.id   = "2";
    b.name = "No files";
    Item c;
    c.id   = "3";
    c.name = "From the other connector";

    const std::vector<CsvGroup> groups = {CsvGroup{"CPL3D", "CPL3D", {a, b}}, CsvGroup{"Other", "Other Co", {c}}};
    CHECK(csv_row_count(groups) == 5);

    // Cut in the middle of the first item's files: the pieces keep the item id on every row.
    const std::vector<std::string> files = to_csv_parts(groups, 2);
    REQUIRE(files.size() == 3);
    const auto f1 = parse_csv(files[0]);
    const auto f2 = parse_csv(files[1]);
    const auto f3 = parse_csv(files[2]);
    REQUIRE(f1.size() == 3);
    REQUIRE(f2.size() == 3);
    REQUIRE(f3.size() == 2);
    CHECK(f1[1][0] == "CPL3D");
    CHECK(f1[1][2] == "1");
    CHECK(f1[1][10] == "f0");
    CHECK(f1[2][10] == "f1");
    CHECK(f2[1][2] == "1");
    CHECK(f2[1][10] == "f2");
    CHECK(f2[2][2] == "2");
    CHECK(f2[2][10] == "");
    CHECK(f3[1][0] == "Other");
    CHECK(f3[1][1] == "Other Co");
    CHECK(f3[1][2] == "3");

    // All in one file: the same rows as the connectors' own files put together under one header.
    const auto whole = parse_csv(to_csv_parts(groups, 0)[0]);
    REQUIRE(whole.size() == 6);
    CHECK(whole[5] == f3[1]);
}

TEST_CASE("csv split: file names", "[Vendors][CsvSplit]")
{
    CHECK(csv_part_path("vendor-models.csv", 1, 1) == "vendor-models.csv"); // one file: no suffix
    CHECK(csv_part_path("vendor-models", 1, 1) == "vendor-models");
    CHECK(csv_part_path("vendor-models.csv", 1, 3) == "vendor-models_1.csv");
    CHECK(csv_part_path("vendor-models.csv", 3, 3) == "vendor-models_3.csv");
    CHECK(csv_part_path("vendor-models.csv", 12, 12) == "vendor-models_12.csv");
    CHECK(csv_part_path("vendor-models", 2, 3) == "vendor-models_2.csv"); // no extension: .csv is added
    CHECK(csv_part_path("C:\\Users\\me\\My Files\\list.csv", 2, 3) == "C:\\Users\\me\\My Files\\list_2.csv");
    CHECK(csv_part_path("/home/me/list.v2.csv", 1, 2) == "/home/me/list.v2_1.csv"); // only the last dot is the extension
    CHECK(csv_part_path("C:\\dir.d\\list", 1, 2) == "C:\\dir.d\\list_1.csv");        // a dot in a folder is not an extension
    CHECK(csv_part_path("/tmp/.hidden", 1, 2) == "/tmp/.hidden_1.csv");
    CHECK(csv_part_path("list.", 1, 2) == "list._1.csv");
    CHECK(csv_part_path("list.CSV", 2, 2) == "list_2.CSV");
}

TEST_CASE("csv split: a connector's own maximum is part of its template", "[Vendors][CsvSplit]")
{
    json base = json::parse(R"({"name": "X", "vendor": "X", "base_url": "https://x.example/api/"})");

    SECTION("absent: none, and the template does not grow a csv key")
    {
        const Spec s = spec_from_json(base);
        CHECK(s.csv_max_records == 0);
        CHECK(!spec_to_json(s).contains("csv"));
    }
    SECTION("set: read, written and kept through an export and an import")
    {
        json j = base;
        j["csv"] = {{"max_records", 200}};
        const Spec s = spec_from_json(j);
        CHECK(s.csv_max_records == 200);
        const json out = spec_to_json(s);
        REQUIRE(out.contains("csv"));
        CHECK(out["csv"]["max_records"] == 200);
        CHECK(out["version"] == 1); // an optional key: still template version 1
        CHECK(spec_from_json(out).csv_max_records == 200);
        const ImportResult r = import_check(out.dump(2));
        REQUIRE(r.ok);
        CHECK(spec_from_json(r.json).csv_max_records == 200);
    }
    SECTION("a number in text is read like the other numbers")
    {
        json j = base;
        j["csv"] = {{"max_records", "500"}};
        CHECK(spec_from_json(j).csv_max_records == 500);
    }
    SECTION("the largest allowed")
    {
        json j = base;
        j["csv"] = {{"max_records", CSV_MAX_RECORDS_CAP}};
        CHECK(spec_from_json(j).csv_max_records == CSV_MAX_RECORDS_CAP);
    }
    SECTION("bad values are refused")
    {
        for (const json& bad : {json(0), json(-5), json(CSV_MAX_RECORDS_CAP + 1), json(2.5), json("many"), json(true), json("-1")}) {
            json j = base;
            j["csv"] = {{"max_records", bad}};
            INFO("max_records = " << bad.dump());
            CHECK_THROWS_AS(spec_from_json(j), std::runtime_error);
        }
    }
    SECTION("a csv key that is not an object is ignored, as any unknown shape")
    {
        json j = base;
        j["csv"] = 7;
        CHECK(spec_from_json(j).csv_max_records == 0);
    }
}
