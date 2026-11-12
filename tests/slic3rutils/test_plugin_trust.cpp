#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/Utils/HomePluginsLogic.hpp>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace pt = Slic3r::plugin_trust;

namespace {

// RFC 8032 test vector 1 — TEST ONLY, see tests/data/plugins/trust/TEST_ONLY.md.
const char* const k_seed1 = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const char* const k_pub1  = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
// RFC 8032 test vector 2 — the wrong key for reject cases.
const char* const k_seed2 = "4ccd089b28ff96da9c80fc314fe0b7a126c472b8d0c6f5d53cc7dd6da61e0c8d";
const char* const k_pub2  = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";

const char* const k_plugin_source = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Trust Echo"
# version = "1.0.0"
# type = "script"
# ///
import orca

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        return orca.ExecutionResult.success("trust-ok")

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

std::string trim_newline(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::string read_text(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return trim_newline(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

fs::path fixture_dir() { return fs::path(TEST_DATA_DIR) / "plugins" / "trust"; }

void write_file(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << contents;
}

PluginDescriptor descriptor_for(const fs::path& package, const std::string& plugin_key)
{
    PluginDescriptor d;
    d.plugin_key        = plugin_key;
    d.name              = "Trust Echo";
    d.version           = "1.0.0";
    d.display_types     = {"script"};
    d.required_orca_api = "1.0.0";
    d.entry_path        = package.string();
    d.metadata_valid    = true;
    return d;
}

pt::Signature sign_package(const fs::path& package, const PluginDescriptor& d, const std::string& seed, const std::string& publisher)
{
    std::string   hash_error;
    pt::Signature sig;
    sig.hash = pt::sha256_file(package, hash_error);
    REQUIRE_FALSE(sig.hash.empty());
    sig.metadata = pt::metadata_from_descriptor(d);
    std::string pub_error;
    REQUIRE(pt::public_key_from_seed(seed, sig.public_key, pub_error));
    const std::string payload = pt::canonical_payload(sig.hash, sig.metadata);
    std::string       sign_error;
    REQUIRE(pt::sign(seed, payload, sig.signature, sign_error));
    sig.publisher = publisher;
    std::string write_error;
    REQUIRE(pt::write_signature_file(package, sig, write_error));
    return sig;
}

struct ScopedPluginManager
{
    bool initialized = false;
    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

bool load_and_wait(PluginManager& manager, const std::string& plugin_key, std::string& error)
{
    manager.load_plugin(plugin_key, /*skip_deps=*/true);
    return manager.wait_for_plugin_load(plugin_key, std::chrono::seconds(120), error);
}

ScriptCompletion wait_for_script(PluginManager& manager, const std::string& plugin_key, const std::string& cap_name)
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;

    auto launch = manager.launch_script_capability(plugin_key, cap_name, [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    });
    if (!launch.launched_async) {
        completion.error = launch.outcome.error.empty() ? std::string("not launched") : launch.outcome.error;
        return completion;
    }

    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(60), [&] { return done; }));
    return completion;
}

} // namespace

TEST_CASE("fixture keypair is marked test-only and matches RFC 8032", "[PluginTrust]")
{
    CHECK(read_text(fixture_dir() / "ed25519_seed.hex") == k_seed1);
    CHECK(read_text(fixture_dir() / "ed25519_public.hex") == k_pub1);
    CHECK(read_text(fixture_dir() / "ed25519_other_seed.hex") == k_seed2);
    CHECK(read_text(fixture_dir() / "ed25519_other_public.hex") == k_pub2);

    const std::string readme = read_text(fixture_dir() / "TEST_ONLY.md");
    CHECK(readme.find("TEST-ONLY") != std::string::npos);
    CHECK(readme.find("not Edge catalog") != std::string::npos);

    std::string pub;
    std::string error;
    REQUIRE(pt::public_key_from_seed(k_seed1, pub, error));
    CHECK(pub == k_pub1);
    REQUIRE(pt::public_key_from_seed(k_seed2, pub, error));
    CHECK(pub == k_pub2);
}

TEST_CASE("hash pin accept", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-pin");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");

    std::string       hash_error;
    const std::string hash = pt::sha256_file(package, hash_error);
    REQUIRE(hash.size() == 64);
    CHECK(hash_error.empty());

    REQUIRE(pt::local_install_needs_prompt("echo", hash));

    std::string pin_error;
    REQUIRE(pt::pin_hash("echo", hash, pin_error));
    CHECK(pin_error.empty());

    CHECK_FALSE(pt::local_install_needs_prompt("echo", hash));
    const pt::TrustStore store = pt::load_trust_store();
    CHECK(pt::is_hash_pinned(store, "echo", hash));
    CHECK_FALSE(store.user_file_corrupt);

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::ApprovedLocally);
    CHECK(state.allowed());
    CHECK(state.sha256 == hash);
    CHECK(state.label == "Approved locally");
}

TEST_CASE("signature accept", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-sig-ok");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");
    sign_package(package, d, k_seed1, "Edge Test");

    std::string add_error;
    REQUIRE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Signed);
    CHECK(state.allowed());
    CHECK(state.publisher == "Edge Test");
    CHECK(state.label == "Signed by Edge Test");
}

TEST_CASE("signature reject: wrong key", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-wrong-key");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");
    sign_package(package, d, k_seed2, "Other");

    std::string add_error;
    REQUIRE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
    CHECK(state.label == "Untrusted");
}

TEST_CASE("signature reject: tampered file", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-tamper-file");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");
    sign_package(package, d, k_seed1, "Edge Test");
    std::string add_error;
    REQUIRE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));

    write_file(package, std::string(k_plugin_source) + "\n# tampered\n");

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("signature reject: tampered metadata", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-tamper-meta");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d   = descriptor_for(package, "echo");
    pt::Signature          sig = sign_package(package, d, k_seed1, "Edge Test");
    std::string            add_error;
    REQUIRE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));

    sig.metadata.name = "Not Trust Echo";
    std::string write_error;
    REQUIRE(pt::write_signature_file(package, sig, write_error));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("signature reject: truncated sig", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-trunc");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d   = descriptor_for(package, "echo");
    pt::Signature          sig = sign_package(package, d, k_seed1, "Edge Test");
    std::string            add_error;
    REQUIRE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));

    REQUIRE(sig.signature.size() == 128);
    sig.signature.resize(64);
    std::string write_error;
    REQUIRE(pt::write_signature_file(package, sig, write_error));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("different hash of same plugin_key prompts again", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-reprompt");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    std::string       hash_error;
    const std::string hash1 = pt::sha256_file(package, hash_error);
    REQUIRE(hash1.size() == 64);

    std::string pin_error;
    REQUIRE(pt::pin_hash("echo", hash1, pin_error));
    CHECK_FALSE(pt::local_install_needs_prompt("echo", hash1));

    write_file(package, std::string(k_plugin_source) + "\n# v2\n");
    const std::string hash2 = pt::sha256_file(package, hash_error);
    REQUIRE(hash2.size() == 64);
    REQUIRE(hash1 != hash2);

    const pt::TrustStore store = pt::load_trust_store();
    CHECK(pt::plugin_key_has_different_pin(store, "echo", hash2));
    CHECK(pt::local_install_needs_prompt("echo", hash2));
    CHECK_FALSE(pt::is_hash_pinned(store, "echo", hash2));
}

TEST_CASE("corrupt trust.json fails closed", "[PluginTrust]")
{
    ScopedDataDir data("plugin-trust-corrupt");
    fs::create_directories(data.plugins_dir());
    write_file(data.plugins_dir() / "trust.json", "{ this is not json ]]");

    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");

    std::string       hash_error;
    const std::string hash = pt::sha256_file(package, hash_error);
    std::string       pin_error;
    // A pin written after a corrupt read must not crash; evaluate of the corrupt file itself
    // reports empty-trust before any rewrite.
    const pt::TrustStore store = pt::load_trust_store();
    CHECK(store.user_file_corrupt);
    CHECK(store.pins.empty());
    CHECK(store.user_publishers.empty());
    CHECK_FALSE(pt::is_hash_pinned(store, "echo", hash));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
    CHECK(state.label == "Untrusted");

    REQUIRE_NOTHROW(pt::load_trust_store());
}

TEST_CASE("path handling never resolves into UltraNet plugins", "[PluginTrust]")
{
    ScopedDataDir data("plugin-trust-ultranet");
    fs::create_directories(data.plugins_dir());

    const fs::path trust    = pt::trust_sidecar_path();
    const fs::path ultranet = data.dir / "plugins";
    CHECK(trust == data.plugins_dir() / "trust.json");
    CHECK(trust.filename() == "trust.json");
    CHECK(trust.parent_path().filename() == "orca_plugins");
    CHECK(trust.parent_path().filename() != "plugins");

    PluginAuditManager& audit = PluginAuditManager::instance();
    CHECK(audit.is_ultranet_plugins_path(ultranet));
    CHECK(audit.is_ultranet_plugins_path(ultranet / "trust.json"));
    CHECK(audit.is_ultranet_plugins_path(data.dir / "Plugins" / "trust.json"));
    CHECK(audit.is_ultranet_plugins_path(data.dir / "PLUGINS" / "lib.so"));
    CHECK(audit.is_ultranet_plugins_path(data.dir / "pLuGiNs" / "libbambu_networking.so"));
    CHECK_FALSE(audit.is_ultranet_plugins_path(trust));
    CHECK_FALSE(pt::path_is_ultranet(trust));
    CHECK(pt::path_is_ultranet(ultranet / "trust.json"));
    CHECK(pt::path_is_ultranet(data.dir / "Plugins"));

    std::string pin_error;
    REQUIRE(pt::pin_hash("echo", std::string(64, 'a'), pin_error));
    CHECK(fs::exists(trust));
    CHECK_FALSE(fs::exists(ultranet));
    CHECK_FALSE(fs::exists(data.dir / "Plugins"));
    CHECK_FALSE(fs::exists(data.dir / "PLUGINS"));
}

TEST_CASE("refusal to enable when untrusted", "[PluginTrust][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data("plugin-trust-enable-deny");
    const fs::path plugin_dir = data.plugins_dir() / "Trust_Echo";
    write_file(plugin_dir / "Trust_Echo.py", k_plugin_source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "Trust_Echo", error));
    CHECK_FALSE(manager.is_plugin_loaded("Trust_Echo"));
    CHECK(error.find("untrusted") != std::string::npos);
}

TEST_CASE("enable and run succeed when pinned; removing the pin refuses run", "[PluginTrust][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data("plugin-trust-run");
    const fs::path plugin_dir = data.plugins_dir() / "Trust_Echo";
    write_file(plugin_dir / "Trust_Echo.py", k_plugin_source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor d;
    REQUIRE(manager.try_get_plugin_descriptor("Trust_Echo", d));
    std::string       hash_error;
    const std::string hash = pt::sha256_file(d.entry_path, hash_error);
    std::string       pin_error;
    REQUIRE(pt::pin_hash("Trust_Echo", hash, pin_error));

    std::string error;
    REQUIRE(load_and_wait(manager, "Trust_Echo", error));
    REQUIRE(manager.is_plugin_loaded("Trust_Echo"));

    const ScriptCompletion ok = wait_for_script(manager, "Trust_Echo", "Echo");
    CHECK(ok.error.empty());
    CHECK(ok.result.message == "trust-ok");

    REQUIRE(pt::unpin_plugin("Trust_Echo", pin_error));
    CHECK(pt::evaluate(d).kind == pt::TrustKind::Untrusted);

    auto launch = manager.launch_script_capability("Trust_Echo", "Echo");
    CHECK_FALSE(launch.launched_async);
    CHECK(launch.outcome.error.find("untrusted") != std::string::npos);

    manager.unload_plugin("Trust_Echo");
    CHECK_FALSE(load_and_wait(manager, "Trust_Echo", error));
    CHECK(error.find("untrusted") != std::string::npos);
}

TEST_CASE("home plugins cards pass through trust labels", "[HomePlugins][PluginTrust]")
{
    HomeTab::PluginView signed_view;
    signed_view.plugin_key = "echo";
    signed_view.name       = "Echo";
    signed_view.trust      = "Signed by Edge Test";
    CHECK(HomeTab::plugin_card(signed_view)["trust"] == "Signed by Edge Test");

    HomeTab::PluginView local;
    local.plugin_key = "echo";
    local.trust      = "Approved locally";
    CHECK(HomeTab::plugin_card(local)["trust"] == "Approved locally");

    HomeTab::PluginView untrusted;
    untrusted.plugin_key = "echo";
    untrusted.trust      = "Untrusted";
    CHECK(HomeTab::plugin_card(untrusted)["trust"] == "Untrusted");
}
