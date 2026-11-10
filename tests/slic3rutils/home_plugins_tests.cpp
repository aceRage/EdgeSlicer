#include <catch2/catch.hpp>

#include "slic3r/Utils/HomePluginsLogic.hpp"

using namespace Slic3r;
using namespace Slic3r::HomeTab;
using json = nlohmann::json;

// wx-free rules for Home > Plugins: the page draws cards the host built, and may only name
// plugin_keys the host itself listed. GUI/HomePlugins.cpp follows these; the page is
// resources/web/home.

static PluginView sample_view()
{
    PluginView v;
    v.plugin_key   = "hello_script";
    v.name         = "Hello";
    v.version      = "1.2.0";
    v.author       = "Edge";
    v.description  = "A short description.";
    v.types        = { "script" };
    v.capabilities = { { "hello", "script", true } };
    v.source       = "local";
    v.loaded       = true;
    v.enabled      = true;
    return v;
}

TEST_CASE("home plugins: a card carries display fields only", "[HomePlugins]")
{
    const json card = plugin_card(sample_view());

    CHECK(card["plugin_key"] == "hello_script");
    CHECK(card["name"] == "Hello");
    CHECK(card["version"] == "1.2.0");
    CHECK(card["author"] == "Edge");
    CHECK(card["description"] == "A short description.");
    CHECK(card["source"] == "local");
    CHECK(card["loaded"] == true);
    CHECK(card["enabled"] == true);
    CHECK(card["loading"] == false);
    CHECK(card["can_run"] == true);
    CHECK(card["update_available"] == false);
    CHECK(card["trust"] == "");
    CHECK(card["in_process_pipeline"] == false);
    REQUIRE(card["types"].size() == 1);
    CHECK(card["types"][0] == "script");
    REQUIRE(card["capabilities"].size() == 1);
    CHECK(card["capabilities"][0]["name"] == "hello");
    CHECK(card["capabilities"][0]["type"] == "script");
    CHECK(card["capabilities"][0]["enabled"] == true);

    CHECK_FALSE(plugin_card_leaks_path(card));
    CHECK_FALSE(card.contains("entry_path"));
    CHECK_FALSE(card.contains("plugin_root"));
    CHECK_FALSE(card.contains("path"));
    CHECK_FALSE(card.contains("thumbnail_url"));
    CHECK_FALSE(card.contains("sharing_token"));
    CHECK(card["run_state"] == "");
    CHECK(card["run_message"] == "");
}

TEST_CASE("home plugins: run_state is only running, success, or failure", "[HomePlugins]")
{
    PluginView running = sample_view();
    running.run_state  = "running";
    json card          = plugin_card(running);
    CHECK(card["run_state"] == "running");
    CHECK(card["run_message"] == "");

    PluginView ok     = sample_view();
    ok.run_state      = "success";
    ok.run_message    = "sync-ok";
    card              = plugin_card(ok);
    CHECK(card["run_state"] == "success");
    CHECK(card["run_message"] == "sync-ok");

    PluginView fail  = sample_view();
    fail.run_state   = "failure";
    fail.run_message = "boom";
    card             = plugin_card(fail);
    CHECK(card["run_state"] == "failure");
    CHECK(card["run_message"] == "boom");

    PluginView junk = sample_view();
    junk.run_state  = "queued";
    junk.run_message = std::string(500, 'x');
    card            = plugin_card(junk);
    CHECK(card["run_state"] == "");
    CHECK(card["run_message"].get<std::string>().size() == 400);

    CHECK_FALSE(plugin_card_leaks_path(card));
}

TEST_CASE("home plugins: sanitize strips host paths", "[HomePlugins]")
{
    json dirty = plugin_card(sample_view());
    dirty["entry_path"]     = "/home/me/orca_plugins/hello/hello.py";
    dirty["plugin_root"]    = "C:\\Users\\me\\orca_plugins\\hello";
    dirty["path"]           = "/etc/passwd";
    dirty["thumbnail_url"]  = "https://example.invalid/x.png";
    dirty["sharing_token"]  = "tok";
    REQUIRE(plugin_card_leaks_path(dirty));

    const json clean = sanitize_plugin_card(dirty);
    CHECK_FALSE(plugin_card_leaks_path(clean));
    CHECK(clean["plugin_key"] == "hello_script");
    CHECK_FALSE(clean.contains("entry_path"));
    CHECK_FALSE(clean.contains("plugin_root"));
    CHECK_FALSE(clean.contains("path"));
    CHECK_FALSE(clean.contains("thumbnail_url"));
    CHECK_FALSE(clean.contains("sharing_token"));
}

TEST_CASE("home plugins: host envelope shape", "[HomePlugins]")
{
    PluginView err = sample_view();
    err.plugin_key = "broken";
    err.error      = "manifest is invalid";
    err.loaded     = false;
    err.enabled    = false;
    err.types      = { "slicing-pipeline" };
    err.in_process_pipeline = true;

    const json msg = plugins_message({ sample_view(), err }, false, "scan failed");
    CHECK(msg["type"] == "plugins");
    CHECK(msg["loading"] == false);
    CHECK(msg["error"] == "scan failed");
    CHECK(msg["catalog"] == false); // Phase 11 placeholder
    REQUIRE(msg["items"].size() == 2);
    CHECK_FALSE(plugin_card_leaks_path(msg["items"][0]));
    CHECK_FALSE(plugin_card_leaks_path(msg["items"][1]));
    CHECK(msg["items"][1]["in_process_pipeline"] == true);
    CHECK(msg["items"][1]["error"] == "manifest is invalid");

    const json loading = plugins_message({}, true, "");
    CHECK(loading["loading"] == true);
    CHECK(loading["items"].empty());
    CHECK(loading["catalog"] == false);

    PluginView nameless;
    nameless.plugin_key.clear();
    const json skipped = plugins_message({ nameless }, false, "");
    CHECK(skipped["items"].empty());

    PluginView script_only;
    script_only.plugin_key = "script_only";
    script_only.types      = { "script" };
    const json run = plugin_card(script_only);
    CHECK(run["can_run"] == true);
    CHECK(run["trust"] == "");
    CHECK_FALSE(run.contains("catalog"));
}

TEST_CASE("home plugins: the page may only name keys the host listed", "[HomePlugins]")
{
    const std::vector<std::string> listed{ "hello_script", "other" };
    CHECK(is_listed_plugin_key("hello_script", listed));
    CHECK(is_listed_plugin_key("other", listed));
    CHECK_FALSE(is_listed_plugin_key("", listed));
    CHECK_FALSE(is_listed_plugin_key("Hello_script", listed));
    CHECK_FALSE(is_listed_plugin_key("missing", listed));
    CHECK_FALSE(is_listed_plugin_key("hello_script", {}));
    CHECK_FALSE(is_listed_plugin_key("/home/me/orca_plugins/hello", listed));
    CHECK_FALSE(is_listed_plugin_key("C:\\Users\\me\\orca_plugins\\hello", listed));
    CHECK_FALSE(is_listed_plugin_key("../plugins/ultranet", listed));
    CHECK_FALSE(is_listed_plugin_key("hello/../../plugins", listed));
    // A path-shaped key is refused even if the host list were wrong.
    CHECK_FALSE(is_listed_plugin_key("a/b", { "a/b" }));
}

TEST_CASE("home plugins: plugin_run only accepts a plugin_key the host listed", "[HomePlugins]")
{
    // GUI/HomePlugins.cpp ignores plugin_run unless listed(key), which is is_listed_plugin_key.
    const std::vector<std::string> listed{ "hello_script" };
    CHECK(is_listed_plugin_key("hello_script", listed));
    CHECK_FALSE(is_listed_plugin_key("not_listed", listed));
    CHECK_FALSE(is_listed_plugin_key("{data_dir}/plugins/hello", listed));
    CHECK_FALSE(is_listed_plugin_key("C:\\Users\\me\\plugins\\hello", listed));
}
