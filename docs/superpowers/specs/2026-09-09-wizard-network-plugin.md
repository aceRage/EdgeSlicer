# The setup wizard leaves a Bambu Lab user with no network plug-in

Branch `fix/wizard-network-plugin`, from `feat/ultra-preferences` at `52a929e27e`.

A first-time user picks a Bambu Lab printer in the setup wizard, finishes it, opens
**Account > Login**, clicks Google sign-in - and gets a 404 on our loopback callback. There is no
Bambu network plug-in on disk to exchange the sign-in ticket with, and nothing in the flow they just
walked through ever offered to fetch one.

## Cause

Two facts that are individually harmless and together produce a dead end.

1. **`installed_networking` has no default.** It is written by exactly two things: a successful
   `GUI_App::install_plugin()` (`src/slic3r/GUI/GUI_App.cpp:1774`) and the Ultra Net self-heal at
   `GUI_App.cpp:3071`, which only fires when a plug-in binary is *already* in
   `data_dir()/plugins`. On a machine that has never had one, the key is simply absent, so
   `app_config->get_bool("installed_networking")` reads **false**, and
   `GUI_App::load_networking_module()` (`GUI_App.cpp:3432`) logs
   `"Don't load plugin as installed_networking is false"` and skips the load.

2. **The download dialog is only reachable from places a new user has no reason to visit.**
   `GUI_App::ShowDownNetPluginDlg()` (`GUI_App.cpp:4203`) is wired to the home-page banner, a
   Device-tab link, and the Preferences checkbox. A user who has just finished the wizard and gone
   straight to Account > Login touches none of them.

So the wizard - the one screen that actually knows a Bambu Lab printer was chosen - said nothing.

## Change

### 1. The decision, as a pure function

`src/slic3r/GUI/WizardNetworking.{hpp,cpp}` - no wx, no config, no filesystem:

```cpp
bool wizard_should_enable_networking(bool key_present, bool key_value, bool any_bbl_selected);
```

The load-bearing part is that `key_present` and `key_value` are **separate** inputs. "Never set"
and "set to false" both read as false through `get_bool()`, but they mean opposite things: the first
is a user who has no opinion, the second is a user who went into Preferences and turned the plug-in
off. Only the first is ours to change. `AppConfig::has()` (`src/libslic3r/AppConfig.hpp:203`) is
what tells them apart.

### 2. The wizard calls it

At the end of `ConfigWizard::priv::apply_config()` (`src/slic3r/GUI/ConfigWizard.cpp`), after
`preset_bundle->export_selections(*app_config)`:

* `any_bbl_selected` comes from `enabled_vendors.find("BBL")` - the same `appconfig_new.vendors()`
  map the rest of `apply_config()` uses to decide what to install - and is true when the BBL vendor
  has at least one model with a non-empty variant set.
* When the helper says yes, `installed_networking` is set to true.
* If additionally no plug-in binary exists in `data_dir()/plugins` (`bambu_networking.dll` /
  `libbambu_networking.so` / `libbambu_networking.dylib` - the same three names the Ultra Net
  self-heal checks), `ShowDownNetPluginDlg()` is scheduled with a single `CallAfter`. It has to be
  deferred: the wizard is still modal at that point and the download dialog is modal too.

The call sits on the success path only, after the last `return false`, so a wizard the user backed
out of changes nothing.

### 3. Preferences - already correct, left alone

`Preferences.cpp:1023` already offers the download when the checkbox is switched on:

```cpp
if (param == "installed_networking") {
    bool pbool = app_config->get_bool("installed_networking");
    if (pbool) {
        GUI::wxGetApp().CallAfter([] { GUI::wxGetApp().ShowDownNetPluginDlg(); });
    }
```

`ShowDownNetPluginDlg()` is itself a no-op when the plug-in is present and current, so no guard was
added. No change to `Preferences.cpp` was needed and none was made.

## Proofs

* **`tests/slic3rutils/wizard_networking_tests.cpp`** (Catch2, tag `[WizardNetworking]`, three
  cases). The one that matters is *"An explicit preference is never overridden"*: `key_present=true,
  key_value=false, any_bbl=true` must answer false. It is the difference between fixing a first-run
  dead end and silently re-enabling a plug-in the user disabled on purpose - and it is invisible to
  any test that only feeds the helper a single already-collapsed bool.
* **Release build** of the worktree: `Snapmaker_Orca`, `Snapmaker_Orca_app_gui` and
  `slic3rutils_tests`, `BUILD_EXIT=0`.
* **Scratch instance** `inst_wizardnet`, data dir `dd_wizardnet` copied from `dd_ctl`: the built
  binary starts clean and shuts down on its own PID. Nothing in `%APPDATA%\EdgeSlicer` and no
  printer was touched.

## Owner test (a click test, not automated)

The wizard itself needs a person, because it is modal and the interesting state is "a data dir that
has never seen a plug-in".

1. Start the build with a **fresh** data dir (`--datadir` at a path that does not exist yet), so the
   wizard runs as `RR_DATA_EMPTY` and `installed_networking` is genuinely unset.
2. In the wizard, select any **Bambu Lab** printer (an X1 or P1 model) and click Finish.
3. Expect: the **"Downloading Bambu Network Plug-in"** dialog appears by itself once the wizard
   closes. Preferences > "Enable network plugin" is now checked.
4. Then the actual complaint: **Account > Login > Google sign-in** completes instead of 404-ing on
   the loopback callback.

Negative pass, on the same fresh data dir:

5. Select a **Snapmaker-only** set of printers. Expect: no dialog, and "Enable network plugin"
   still unchecked.
6. And the regression this is most likely to cause: with a data dir where the checkbox was
   explicitly **unchecked**, re-run the wizard (Help > Configuration Wizard) and pick a Bambu
   printer. Expect: the checkbox stays unchecked and no dialog appears.
