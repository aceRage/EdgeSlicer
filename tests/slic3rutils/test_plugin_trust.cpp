#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <libslic3r/Utils.hpp>
#include <libslic3r/Zipper.hpp>
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
const char* const k_seed2 = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
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

std::string tree_hash_of(const fs::path& root)
{
    std::string       error;
    const std::string hash = pt::sha256_plugin_tree(root, error);
    REQUIRE_FALSE(hash.empty());
    CHECK(error.empty());
    return hash;
}

pt::Signature sign_package(const fs::path& package, const PluginDescriptor& d, const std::string& seed, const std::string& publisher)
{
    pt::Signature sig;
    sig.hash = tree_hash_of(d.plugin_root.empty() ? package : fs::path(d.plugin_root));
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

const char* const k_plugin_source_with_helper = R"PY(# /// script
# requires-python = ">=3.12"
# requires-orca-api = "1.0.0"
#
# [tool.orcaslicer.plugin]
# name = "Trust Echo"
# version = "1.0.0"
# type = "script"
# ///
import helper_token
import orca

class Echo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Echo"
    def execute(self):
        return orca.ExecutionResult.success(helper_token.TOKEN)

@orca.plugin
class EchoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(Echo)
)PY";

void write_helper_wheel(const fs::path& wheel, const std::string& token)
{
    const std::string payload = "TOKEN = \"" + token + "\"\n";
    Zipper            zipper(wheel.string(), Zipper::NO_COMPRESSION);
    zipper.add_entry("helper_token.py", payload.data(), payload.size());
    zipper.finalize();
}

void plant_unchecked_pyc(const fs::path& py_path, const std::string& tampered_source)
{
    const fs::path tmp = py_path.parent_path() / (py_path.stem().string() + ".planted.py");
    write_file(tmp, tampered_source);

    PythonGILState gil;
    REQUIRE(gil);

    PyObjectPtr util(PyImport_ImportModule("importlib.util"));
    REQUIRE(util);
    PyObjectPtr source(PyUnicode_DecodeFSDefault(py_path.string().c_str()));
    REQUIRE(source);
    PyObjectPtr cached(PyObject_CallMethod(util.get(), "cache_from_source", "O", source.get()));
    REQUIRE(cached);
    const char* cached_utf8 = PyUnicode_AsUTF8(cached.get());
    REQUIRE(cached_utf8);
    const fs::path cache_path(cached_utf8);
    boost::system::error_code mk_ec;
    fs::create_directories(cache_path.parent_path(), mk_ec);

    PyObjectPtr compile_mod(PyImport_ImportModule("py_compile"));
    REQUIRE(compile_mod);
    PyObjectPtr mode_enum(PyObject_GetAttrString(compile_mod.get(), "PycInvalidationMode"));
    REQUIRE(mode_enum);
    PyObjectPtr unchecked(PyObject_GetAttrString(mode_enum.get(), "UNCHECKED_HASH"));
    REQUIRE(unchecked);
    PyObjectPtr compile_fn(PyObject_GetAttrString(compile_mod.get(), "compile"));
    REQUIRE(compile_fn);

    PyObjectPtr tmp_s(PyUnicode_DecodeFSDefault(tmp.string().c_str()));
    PyObjectPtr cache_s(PyUnicode_DecodeFSDefault(cache_path.string().c_str()));
    REQUIRE(tmp_s);
    REQUIRE(cache_s);

    PyObjectPtr kwargs(PyDict_New());
    REQUIRE(kwargs);
    REQUIRE(PyDict_SetItemString(kwargs.get(), "cfile", cache_s.get()) == 0);
    REQUIRE(PyDict_SetItemString(kwargs.get(), "dfile", source.get()) == 0);
    REQUIRE(PyDict_SetItemString(kwargs.get(), "invalidation_mode", unchecked.get()) == 0);

    PyObjectPtr args(PyTuple_Pack(1, tmp_s.get()));
    REQUIRE(args);
    const fs::path conventional = py_path.parent_path() / "__pycache__" / cache_path.filename();
    fs::create_directories(conventional.parent_path());

    auto compile_to = [&](const fs::path& dest) {
        PyObjectPtr dest_s(PyUnicode_DecodeFSDefault(dest.string().c_str()));
        REQUIRE(dest_s);
        REQUIRE(PyDict_SetItemString(kwargs.get(), "cfile", dest_s.get()) == 0);
        PyObjectPtr compiled(PyObject_Call(compile_fn.get(), args.get(), kwargs.get()));
        if (!compiled)
            PyErr_Print();
        REQUIRE(compiled);
        REQUIRE(fs::exists(dest));
    };

    compile_to(conventional);
    if (!mk_ec)
        compile_to(cache_path);

    boost::system::error_code ec;
    fs::remove(tmp, ec);
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

    const std::string hash = tree_hash_of(package);
    REQUIRE(hash.size() == 64);

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
    const std::string hash1 = tree_hash_of(package);
    REQUIRE(hash1.size() == 64);

    std::string pin_error;
    REQUIRE(pt::pin_hash("echo", hash1, pin_error));
    CHECK_FALSE(pt::local_install_needs_prompt("echo", hash1));

    write_file(package, std::string(k_plugin_source) + "\n# v2\n");
    const std::string hash2 = tree_hash_of(package);
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

    const std::string    hash = tree_hash_of(package);
    std::string          pin_error;
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
    CHECK_FALSE(pt::pin_hash("echo", hash, pin_error));
    CHECK_FALSE(pin_error.empty());
    CHECK(read_text(data.plugins_dir() / "trust.json").find("this is not json") != std::string::npos);
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
    const std::string hash = tree_hash_of(d.plugin_root.empty() ? fs::path(d.entry_path) : fs::path(d.plugin_root));
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

TEST_CASE("pin then sibling wheel is not Approved and load refuses", "[PluginTrust][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data("plugin-trust-sibling-whl");
    const fs::path plugin_dir = data.plugins_dir() / "Trust_Echo";
    write_file(plugin_dir / "Trust_Echo.py", k_plugin_source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor d;
    REQUIRE(manager.try_get_plugin_descriptor("Trust_Echo", d));
    const fs::path    root = d.plugin_root.empty() ? fs::path(d.entry_path) : fs::path(d.plugin_root);
    const std::string hash = tree_hash_of(root);
    std::string       pin_error;
    REQUIRE(pt::pin_hash("Trust_Echo", hash, pin_error));
    CHECK(pt::evaluate(d).kind == pt::TrustKind::ApprovedLocally);

    write_file(root / "extra.whl", "PK\x03\x04 sibling-not-in-pin");

    const pt::TrustState after = pt::evaluate(d);
    CHECK(after.kind != pt::TrustKind::ApprovedLocally);
    CHECK(after.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(after.allowed());

    std::string error;
    CHECK_FALSE(load_and_wait(manager, "Trust_Echo", error));
    CHECK(error.find("untrusted") != std::string::npos);
}

TEST_CASE("valid signature under a different plugin_key is not Signed", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-key-mismatch");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor signed_for = descriptor_for(package, "echo");
    sign_package(package, signed_for, k_seed1, "Attacker");

    std::string add_error;
    REQUIRE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));

    PluginDescriptor other     = signed_for;
    other.plugin_key           = "other";
    const pt::TrustState state = pt::evaluate(other);
    CHECK(state.kind != pt::TrustKind::Signed);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("trust.json v99 with a valid pin is Untrusted", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-v99");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d    = descriptor_for(package, "echo");
    const std::string      hash = tree_hash_of(package);

    fs::create_directories(data.plugins_dir());
    write_file(data.plugins_dir() / "trust.json",
               std::string("{\"version\":99,\"publishers\":[],\"pins\":[{\"sha256\":\"") + hash + "\",\"plugin_key\":\"echo\"}]}\n");

    const pt::TrustStore store = pt::load_trust_store();
    CHECK(store.user_file_corrupt);
    CHECK(store.pins.empty());
    CHECK_FALSE(pt::is_hash_pinned(store, "echo", hash));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("empty trust.json fails closed", "[PluginTrust]")
{
    ScopedDataDir data("plugin-trust-empty");
    fs::create_directories(data.plugins_dir());
    write_file(data.plugins_dir() / "trust.json", "");

    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");

    const pt::TrustStore store = pt::load_trust_store();
    CHECK(store.user_file_corrupt);
    CHECK(store.pins.empty());
    CHECK(store.user_publishers.empty());
    CHECK(pt::evaluate(d).kind == pt::TrustKind::Untrusted);
}

TEST_CASE("unknown version without pins is corrupt", "[PluginTrust]")
{
    ScopedDataDir data("plugin-trust-unknown-ver");
    fs::create_directories(data.plugins_dir());
    write_file(data.plugins_dir() / "trust.json", "{\"version\":2,\"publishers\":[],\"pins\":[]}\n");

    const pt::TrustStore store = pt::load_trust_store();
    CHECK(store.user_file_corrupt);
    CHECK(store.pins.empty());
}

TEST_CASE("symlink replace of a pinned file is not Approved", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-symlink");
    const fs::path plugin_dir = data.dir / "plug";
    const fs::path entry      = plugin_dir / "echo.py";
    write_file(entry, k_plugin_source);
    write_file(plugin_dir / "other.py", std::string(k_plugin_source) + "\n# other\n");

    PluginDescriptor d = descriptor_for(entry, "echo");
    d.plugin_root      = plugin_dir.string();

    const std::string hash = tree_hash_of(plugin_dir);
    std::string       pin_error;
    REQUIRE(pt::pin_hash("echo", hash, pin_error));
    CHECK(pt::evaluate(d).kind == pt::TrustKind::ApprovedLocally);

    fs::remove(entry);
    fs::create_symlink(plugin_dir / "other.py", entry);

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind != pt::TrustKind::ApprovedLocally);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("pin_hash and add_user_publisher refuse to save while trust.json is corrupt", "[PluginTrust]")
{
    ScopedDataDir data("plugin-trust-no-wipe");
    fs::create_directories(data.plugins_dir());
    const fs::path sidecar = data.plugins_dir() / "trust.json";
    write_file(sidecar, "{ this is not json ]]");

    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const std::string hash = tree_hash_of(package);

    std::string pin_error;
    CHECK_FALSE(pt::pin_hash("echo", hash, pin_error));
    CHECK(pin_error.find("corrupt") != std::string::npos);

    std::string add_error;
    CHECK_FALSE(pt::add_user_publisher({"Edge Test", k_pub1, "user"}, add_error));
    CHECK(add_error.find("corrupt") != std::string::npos);

    CHECK(read_text(sidecar).find("this is not json") != std::string::npos);
    CHECK(pt::load_trust_store().user_file_corrupt);

    std::string reset_error;
    REQUIRE(pt::reset_corrupt_trust_store(reset_error));
    CHECK(reset_error.empty());
    CHECK_FALSE(pt::load_trust_store().user_file_corrupt);

    REQUIRE(pt::pin_hash("echo", hash, pin_error));
    CHECK(pt::is_hash_pinned(pt::load_trust_store(), "echo", hash));
}

TEST_CASE("empty plugin_key pin is rejected", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-empty-key");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const std::string hash = tree_hash_of(package);

    std::string pin_error;
    CHECK_FALSE(pt::pin_hash("", hash, pin_error));
    CHECK(pin_error.find("plugin_key") != std::string::npos);
    CHECK(pt::load_trust_store().pins.empty());
}

TEST_CASE("signed label uses the pinned key name not the sidecar publisher", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-label-keyfile");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d = descriptor_for(package, "echo");
    sign_package(package, d, k_seed1, "Attacker");

    std::string add_error;
    REQUIRE(pt::add_user_publisher({"", k_pub1, "user"}, add_error));

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind == pt::TrustKind::Signed);
    CHECK(state.publisher != "Attacker");
    CHECK(state.publisher.empty());
    CHECK(state.label == "Signed by unknown publisher");
}

TEST_CASE("canonical payload does not bind empty permissions_digest", "[PluginTrust]")
{
    pt::SignedMetadata meta;
    meta.name                 = "Trust Echo";
    meta.version              = "1.0.0";
    meta.types                = {"script"};
    meta.min_orca_version     = "1.0.0";
    meta.plugin_key           = "echo";
    const std::string payload = pt::canonical_payload(std::string(64, 'a'), meta);
    CHECK(payload.find("permissions_digest") == std::string::npos);
    CHECK(payload.find("plugin_key=echo") != std::string::npos);
}

TEST_CASE("escaping symlink is Untrusted", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-escape-symlink");
    const fs::path plugin_dir = data.dir / "plug";
    const fs::path entry      = plugin_dir / "echo.py";
    write_file(entry, k_plugin_source);
    const fs::path outside = data.dir / "outside.txt";
    write_file(outside, "not-in-plugin\n");

    PluginDescriptor d = descriptor_for(entry, "echo");
    d.plugin_root      = plugin_dir.string();
    REQUIRE(pt::evaluate(d).kind == pt::TrustKind::Untrusted);

    const std::string hash = tree_hash_of(plugin_dir);
    std::string       pin_error;
    REQUIRE(pt::pin_hash("echo", hash, pin_error));
    CHECK(pt::evaluate(d).kind == pt::TrustKind::ApprovedLocally);

    boost::system::error_code ec;
    fs::create_symlink(fs::path("..") / "outside.txt", plugin_dir / "escape.py", ec);
    if (ec)
        SKIP("Cannot create a file symlink here: " + ec.message());
    if (!fs::is_symlink(fs::symlink_status(plugin_dir / "escape.py")))
        SKIP("create_symlink did not produce a symlink");

    std::string       hash_error;
    const std::string escaped = pt::sha256_plugin_tree(plugin_dir, hash_error);
    INFO("hash_error=" << hash_error);
    INFO("target=" << fs::read_symlink(plugin_dir / "escape.py"));
    CHECK(escaped.empty());
    CHECK(hash_error.find("escapes") != std::string::npos);

    const pt::TrustState state = pt::evaluate(d);
    CHECK(state.kind != pt::TrustKind::ApprovedLocally);
    CHECK(state.kind == pt::TrustKind::Untrusted);
    CHECK_FALSE(state.allowed());
}

TEST_CASE("trust.json missing version key is corrupt", "[PluginTrust]")
{
    ScopedDataDir  data("plugin-trust-missing-version");
    const fs::path package = data.dir / "echo.py";
    write_file(package, k_plugin_source);
    const PluginDescriptor d    = descriptor_for(package, "echo");
    const std::string      hash = tree_hash_of(package);

    fs::create_directories(data.plugins_dir());
    write_file(data.plugins_dir() / "trust.json",
               std::string("{\"publishers\":[],\"pins\":[{\"sha256\":\"") + hash + "\",\"plugin_key\":\"echo\"}]}\n");

    const pt::TrustStore store = pt::load_trust_store();
    CHECK(store.user_file_corrupt);
    CHECK(store.pins.empty());
    CHECK_FALSE(pt::is_hash_pinned(store, "echo", hash));
    CHECK(pt::evaluate(d).kind == pt::TrustKind::Untrusted);
}

TEST_CASE("reload after extract-dir tamper still imports hashed wheel bytes", "[PluginTrust][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data("plugin-trust-extract-tamper");
    const fs::path plugin_dir = data.plugins_dir() / "Trust_Echo";
    write_file(plugin_dir / "Trust_Echo.py", k_plugin_source_with_helper);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor d;
    REQUIRE(manager.try_get_plugin_descriptor("Trust_Echo", d));
    const fs::path root = d.plugin_root.empty() ? fs::path(d.entry_path).parent_path() : fs::path(d.plugin_root);
    write_helper_wheel(root / "helper_token-1.0-py3-none-any.whl", "from-wheel");
    const std::string hash = tree_hash_of(root);
    std::string       pin_error;
    REQUIRE(pt::pin_hash("Trust_Echo", hash, pin_error));
    CHECK(pt::evaluate(d).kind == pt::TrustKind::ApprovedLocally);

    std::string error;
    REQUIRE(load_and_wait(manager, "Trust_Echo", error));
    REQUIRE(manager.is_plugin_loaded("Trust_Echo"));

    const ScriptCompletion first = wait_for_script(manager, "Trust_Echo", "Echo");
    CHECK(first.error.empty());
    CHECK(first.result.message == "from-wheel");

    const fs::path extracted = root / "__whl_extracted__" / "helper_token-1.0-py3-none-any" / "helper_token.py";
    REQUIRE(fs::exists(extracted));
    write_file(extracted, "TOKEN = \"tampered\"\n");

    manager.unload_plugin("Trust_Echo");
    REQUIRE(load_and_wait(manager, "Trust_Echo", error));
    REQUIRE(manager.is_plugin_loaded("Trust_Echo"));

    const ScriptCompletion second = wait_for_script(manager, "Trust_Echo", "Echo");
    CHECK(second.error.empty());
    CHECK(second.result.message == "from-wheel");
    CHECK(second.result.message != "tampered");
    CHECK(read_text(extracted) == "TOKEN = \"from-wheel\"");
}

TEST_CASE("planted pyc is not used on reload", "[PluginTrust][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());

    ScopedDataDir  data("plugin-trust-planted-pyc");
    const fs::path plugin_dir = data.plugins_dir() / "Trust_Echo";
    const fs::path entry      = plugin_dir / "Trust_Echo.py";
    write_file(entry, k_plugin_source);

    PluginManager& manager = PluginManager::instance();
    manager.discover_plugins(/*async=*/false, /*clear=*/true);

    PluginDescriptor d;
    REQUIRE(manager.try_get_plugin_descriptor("Trust_Echo", d));
    const std::string hash = tree_hash_of(d.plugin_root.empty() ? fs::path(d.entry_path) : fs::path(d.plugin_root));
    std::string       pin_error;
    REQUIRE(pt::pin_hash("Trust_Echo", hash, pin_error));

    std::string error;
    REQUIRE(load_and_wait(manager, "Trust_Echo", error));
    REQUIRE(manager.is_plugin_loaded("Trust_Echo"));

    const ScriptCompletion first = wait_for_script(manager, "Trust_Echo", "Echo");
    CHECK(first.error.empty());
    CHECK(first.result.message == "trust-ok");

    std::string tampered = k_plugin_source;
    const auto  needle   = tampered.find("trust-ok");
    REQUIRE(needle != std::string::npos);
    tampered.replace(needle, std::string("trust-ok").size(), "planted-ok");
    plant_unchecked_pyc(entry, tampered);
    CHECK(read_text(entry).find("trust-ok") != std::string::npos);
    CHECK(read_text(entry).find("planted-ok") == std::string::npos);

    manager.unload_plugin("Trust_Echo");
    REQUIRE(load_and_wait(manager, "Trust_Echo", error));
    REQUIRE(manager.is_plugin_loaded("Trust_Echo"));

    const ScriptCompletion second = wait_for_script(manager, "Trust_Echo", "Echo");
    CHECK(second.error.empty());
    CHECK(second.result.message == "trust-ok");
    CHECK(second.result.message != "planted-ok");
}
