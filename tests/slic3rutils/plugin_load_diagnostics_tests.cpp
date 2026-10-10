// Why the network plug-in did not load: the classification of a failed LoadLibrary and the choice of
// message. Pure functions of (error code, NTSTATUS, file exists, file size); no DLL is loaded and
// no security policy is needed. The sources for the codes are listed in the PR that added them.

#include <catch2/catch.hpp>

#include "slic3r/Utils/PluginLoadDiagnostics.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/filesystem.hpp>

#include <fstream>

using namespace Slic3r;
namespace c = Slic3r::plugin_load_codes;

namespace {
PluginLoadFailureKind classify(unsigned long code, bool exists = true, std::uint64_t size = 1000, unsigned long nt = 0)
{
    return classify_plugin_load_error(code, nt, exists, size);
}
} // namespace

TEST_CASE("Code-integrity, policy and antivirus codes classify as blocked", "[PluginLoad]")
{
    CHECK(classify(c::kInvalidImageHash) == PluginLoadFailureKind::Blocked);         // 577 / 0x241
    CHECK(classify(c::kSystemIntegrityViolation) == PluginLoadFailureKind::Blocked); // 4551 / 0x11C7
    CHECK(classify(c::kAccessDisabledByPolicy) == PluginLoadFailureKind::Blocked);   // 1260
    CHECK(classify(c::kAccessDisabledNoSafer) == PluginLoadFailureKind::Blocked);    // 786
    CHECK(classify(c::kVirusInfected) == PluginLoadFailureKind::Blocked);            // 225
    CHECK(classify(c::kVirusDeleted) == PluginLoadFailureKind::Blocked);             // 226
    CHECK(classify(c::kAccessDenied) == PluginLoadFailureKind::Blocked);             // 5

    // The value the Bad Image dialog printed for the reporting user is an NTSTATUS, not a Win32
    // code; whichever code LoadLibrary left behind, that status alone says "blocked".
    CHECK(classify(0, true, 1000, 0xC0E90002UL) == PluginLoadFailureKind::Blocked);
    CHECK(classify(c::kBadExeFormat, true, 1000, 0xC0E90002UL) == PluginLoadFailureKind::Blocked);
    CHECK(classify(0, true, 1000, 0xC0000428UL) == PluginLoadFailureKind::Blocked); // STATUS_INVALID_IMAGE_HASH
    CHECK(classify(0, true, 1000, 0xC0000906UL) == PluginLoadFailureKind::Blocked); // STATUS_VIRUS_INFECTED
}

TEST_CASE("An absent or empty file is missing whatever the code says", "[PluginLoad]")
{
    CHECK(classify(c::kFileNotFound, false, 0) == PluginLoadFailureKind::Missing);
    CHECK(classify(c::kPathNotFound, false, 0) == PluginLoadFailureKind::Missing);
    CHECK(classify(c::kModNotFound, false, 0) == PluginLoadFailureKind::Missing);
    // An antivirus that empties the file instead of deleting it: the code is whatever the loader says.
    CHECK(classify(c::kBadExeFormat, true, 0) == PluginLoadFailureKind::Missing);
    CHECK(classify(c::kInvalidImageHash, true, 0) == PluginLoadFailureKind::Missing);
    CHECK(classify(c::kAccessDenied, false, 0) == PluginLoadFailureKind::Missing);
}

TEST_CASE("126 with the file present is a missing dependency, not a missing plug-in", "[PluginLoad]")
{
    CHECK(classify(c::kModNotFound, true, 123456) == PluginLoadFailureKind::MissingDependency);
    CHECK(classify(c::kProcNotFound, true, 123456) == PluginLoadFailureKind::MissingDependency);
    // "file not found" for a file we just saw on disk can only be about something it imports.
    CHECK(classify(c::kFileNotFound, true, 123456) == PluginLoadFailureKind::MissingDependency);
}

TEST_CASE("Invalid-image codes classify as bad image", "[PluginLoad]")
{
    CHECK(classify(c::kBadExeFormat) == PluginLoadFailureKind::BadImage);          // 193
    CHECK(classify(c::kBadFormat) == PluginLoadFailureKind::BadImage);             // 11
    CHECK(classify(c::kExeMachineTypeMismatch) == PluginLoadFailureKind::BadImage); // 216
    CHECK(classify(c::kImageMachineTypeMismatch) == PluginLoadFailureKind::BadImage); // 706
}

TEST_CASE("Anything else is other", "[PluginLoad]")
{
    CHECK(classify(1114) == PluginLoadFailureKind::Other); // ERROR_DLL_INIT_FAILED
    CHECK(classify(0) == PluginLoadFailureKind::Other);
    CHECK(classify(8) == PluginLoadFailureKind::Other);    // ERROR_NOT_ENOUGH_MEMORY
}

TEST_CASE("The message follows the failure; restart only when nothing failed", "[PluginLoad]")
{
    using K = PluginLoadFailureKind;
    using M = PluginLoadMessage;

    // Nothing failed: the genuine "copied but not loaded until restart" case.
    CHECK(plugin_load_message(K::None, true, 1000) == M::Restart);
    CHECK(plugin_load_message(K::None, false, 0) == M::Restart);

    CHECK(plugin_load_message(K::Blocked, true, 1000) == M::Blocked);
    CHECK(plugin_load_message(K::MissingDependency, true, 1000) == M::MissingDependency);
    CHECK(plugin_load_message(K::BadImage, true, 1000) == M::BadImage);
    CHECK(plugin_load_message(K::Incompatible, true, 1000) == M::Incompatible);
    CHECK(plugin_load_message(K::Other, true, 1000) == M::Other);

    // Missing at start-up: if the file is there now (the first-run copy came after the load point)
    // a restart is the fix; if it is still absent or empty, reinstalling is.
    CHECK(plugin_load_message(K::Missing, true, 1000) == M::Restart);
    CHECK(plugin_load_message(K::Missing, false, 0) == M::MissingFile);
    CHECK(plugin_load_message(K::Missing, true, 0) == M::MissingFile);

    // A blocked plug-in stays blocked even though its file exists: a restart would not change that.
    CHECK(plugin_load_message(K::Blocked, true, 1000) != M::Restart);
}

TEST_CASE("The error text carries the code in hex, and the NTSTATUS when it differs", "[PluginLoad]")
{
    PluginLoadFailure f;
    f.code = 4551;
    CHECK(plugin_load_error_text(f) == "0x11C7");
    f.nt_status = 0xC0E90002UL;
    CHECK(plugin_load_error_text(f) == "0x11C7, status 0xC0E90002");
    f.code = 577;
    f.nt_status = 0;
    CHECK(plugin_load_error_text(f) == "0x241");
}

TEST_CASE("The last failure is kept until cleared", "[PluginLoad]")
{
    clear_plugin_load_failure();
    CHECK(last_plugin_load_failure().kind == PluginLoadFailureKind::None);

    PluginLoadFailure f;
    f.kind    = PluginLoadFailureKind::Blocked;
    f.code    = 577;
    f.library = "x.dll";
    record_plugin_load_failure(f);
    CHECK(last_plugin_load_failure().kind == PluginLoadFailureKind::Blocked);
    CHECK(last_plugin_load_failure().code == 577);

    clear_plugin_load_failure();
    CHECK(last_plugin_load_failure().kind == PluginLoadFailureKind::None);
}

TEST_CASE("File facts: absent, empty and non-empty", "[PluginLoad]")
{
    namespace fs = boost::filesystem;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("plugdiag_facts_%%%%%%%%");
    fs::create_directories(dir);

    bool exists = true;
    std::uint64_t size = 99;
    plugin_file_facts((dir / "absent.dll").string(), exists, size);
    CHECK_FALSE(exists);
    CHECK(size == 0);

    { std::ofstream((dir / "empty.dll").string(), std::ios::binary); }
    plugin_file_facts((dir / "empty.dll").string(), exists, size);
    CHECK(exists);
    CHECK(size == 0);

    { std::ofstream f((dir / "full.dll").string(), std::ios::binary); f << "MZ1234"; }
    plugin_file_facts((dir / "full.dll").string(), exists, size);
    CHECK(exists);
    CHECK(size == 6);

    plugin_file_facts(dir.string(), exists, size); // a directory is not a plug-in file
    CHECK_FALSE(exists);

    boost::system::error_code ec;
    fs::remove(dir / "empty.dll", ec);
    fs::remove(dir / "full.dll", ec);
    fs::remove(dir, ec);
}

TEST_CASE("The legacy_networking flag does not change the version the plug-in must report", "[PluginLoad]")
{
    // The reported bug: legacy_networking=true made the check expect the old 01.10 ABI, so the
    // plug-in we ship ("02.01.01.xx") was rejected on every start and no restart could help.
    const bool saved = NetworkAgent::use_legacy_network;
    for (bool legacy : {false, true}) {
        NetworkAgent::use_legacy_network = legacy;
        INFO("use_legacy_network = " << legacy);
        CHECK(NetworkAgent::expected_version() == std::string(BAMBU_NETWORK_AGENT_VERSION));
        CHECK(NetworkAgent::is_compatible_version("02.01.01.53")); // our plug-in: same MM.mm.pp, other build number
        CHECK(NetworkAgent::is_compatible_version(BAMBU_NETWORK_AGENT_VERSION));
        CHECK_FALSE(NetworkAgent::is_compatible_version(BAMBU_NETWORK_AGENT_VERSION_LEGACY)); // never accepted any more
        CHECK_FALSE(NetworkAgent::is_compatible_version("00.00.00.00")); // inconsistent build / no get_version export
        CHECK_FALSE(NetworkAgent::is_compatible_version("02.01.0"));      // too short to compare
        CHECK_FALSE(NetworkAgent::is_compatible_version(""));
    }
    NetworkAgent::use_legacy_network = saved;
    CHECK_FALSE(NetworkAgent::use_legacy_network); // the default: nothing sets it any more
}
