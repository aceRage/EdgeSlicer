# Flashforge device tab: the missing bitmaps

Date: 2026-09-08  \
Branch: `fix/flashforge-missing-bitmaps`

## The bug

On a fresh Windows install the app put up a modal error dialog reading
`Could not load bitmap: device_dropdown` and the Device tab was unusable.

The ported Orca-Flashforge device tab (`src/slic3r/GUI/FlashForge/*.cpp`, mounted by
`MainFrame::show_flashforge_device()` when the selected printer preset's vendor is
`Flashforge` - see `src/slic3r/GUI/Plater.cpp`) was ported without its artwork. It asks for
bitmap names that did not exist in `resources/images`, and a miss was fatal:
`create_scaled_bitmap()` threw `Slic3r::RuntimeError`, which surfaced as a modal dialog.

Three things were wrong, and all three are fixed here.

## 1. The artwork is now in the tree

### Source and license

Upstream: **https://github.com/FlashForge/Orca-Flashforge**, the FlashForge org's own
OrcaSlicer derivative - the fork this device tab was ported from.

| | |
|---|---|
| Repo | `https://github.com/FlashForge/Orca-Flashforge` |
| Branch | `main` |
| Commit | `c45cc8216c184574c36873314ae7caea67c7a1ad` (tagged `1.7.9-beta`) |
| License | **AGPL-3.0**, declared in `LICENSE.txt` at the repo root |
| Source tree | `https://github.com/FlashForge/Orca-Flashforge/tree/c45cc8216c184574c36873314ae7caea67c7a1ad/resources/images` |

Files were fetched by pinned-sha raw URL, so the import is reproducible:

```
https://raw.githubusercontent.com/FlashForge/Orca-Flashforge/c45cc8216c184574c36873314ae7caea67c7a1ad/resources/images/<file>
```

EdgeSlicer is itself an OrcaSlicer/BambuStudio derivative under AGPL-3.0, so the copyleft
terms are compatible and no relicensing is required. Attribution is this document.

**Trademark caveat, deliberately noted:** the printer-model renders
(`adventurer_5m.png`, `guider4.png`, and the other model images) are FlashForge product
photography and carry FlashForge branding/trade dress. That is independent of the AGPL and
is not resolved by this import. They are shipped because the device tab exists to drive
FlashForge hardware, where naming and depicting the machine is the point - but if EdgeSlicer
is ever distributed commercially, or the Flashforge tab is repurposed, these specific files
should be reviewed. The vector UI chrome (arrows, checkmarks, slot and nozzle icons) is
generic and carries no such concern.

### How the list was derived - and why grep alone was not enough

This is the part worth reading, because the first pass got it wrong.

Static extraction over `src/slic3r/GUI/FlashForge/` found 56 names, from three sources:

1. String literals in `create_scaled_bitmap("...")`.
2. String literals in `ScalableBitmap(...)` / `ScalableButton(...)`.
3. **Names built at runtime** - `FFUtils::getBitmapFileName(pid)` returns
   `FFPrinterPreset::bmp_file_name` from a pid-keyed table in `src/slic3r/GUI/FFUtils.cpp`,
   feeding `create_scaled_bitmap(bitmap_name.ToStdString(), ...)` in `SingleDeviceState.cpp`
   and `DeviceListPanel.cpp`. These nine printer-model names appear in no grep for a quoted
   bitmap name.

All 56 were absent, and all 56 were imported. **Running the build then surfaced 59 more.**
The static pass had two blind spots:

- **Scope.** The Flashforge tab is not confined to `src/slic3r/GUI/FlashForge/`. It pulls in
  widgets that live in `src/slic3r/GUI/Widgets/` - `NewTempInput.cpp` alone accounts for the
  whole `push_button_*` family and the combobox arrows.
- **Call shape.** Names are also passed as plain constructor arguments, e.g.
  `FFPushButton(parent, id, "push_button_inc_normal", "push_button_inc_hover", ...)`.
  No amount of grepping for `create_scaled_bitmap("` finds those.

The authoritative list is therefore the **runtime** one: with the non-fatal fallback from
section 2 in place, every miss logs itself, so launching the app against a Flashforge preset
enumerates the real set. That is how the remaining 59 were found, and 63 files (some names
resolve to both a light and a `_dark` variant) were imported to cover them.

Five names that static analysis flagged were confirmed **dead** and deliberately not
imported - zero runtime hits, and every reference is commented out or a false positive:
`fan_pointer`, `mode_simple`, `organize`, `topbar_logo` (all inside `//` or `/* */` blocks),
and `hum_level`, which is not a filename at all but the stem of a concatenation
(`"hum_level" + std::to_string(level) + mode_string` in `AmsMappingPopup.cpp`); the real
`hum_level1_light.svg` and friends already ship.

### Imported from upstream (118 files)

Full list, all fetched from the pinned commit above:

```
Orca-FlashforgeTitle.ico                ad5x.png                                adventurer_5m.png
adventurer_5m_pro.png                   adventurer_a5.png                       ams_tutorial_icon.svg
arrow_down.png                          arrow_up.png                            camera_button_offline.png
color_close_btn.png                     device_back.svg                         device_back_hover.svg
device_back_press.svg                   device_bottom_temperature.png           device_bottom_temperature.svg
device_cancel_print.png                 device_cancel_print.svg                 device_cooling_fan.svg
device_dropdown.svg                     device_file_info.png                    device_file_offline.svg
device_fill_rate.svg                    device_filter.png                       device_filter_offline.svg
device_idle_file_info.svg               device_initial_speed.svg                device_lamp_control.png
device_lamp_offline.svg                 device_layer.svg                        device_left_nozzle_fan.svg
device_material.svg                     device_material_weight.svg              device_mid_temperature.png
device_mid_temperature.svg              device_more.svg                         device_more_hover.svg
device_more_press.svg                   device_nozzle_fan.svg                   device_pause_print.png
device_pause_print.svg                  device_right_nozzle_fan.svg             device_speed.svg
device_top_temperature.png              device_top_temperature.svg              device_z_axis.svg
edit_black_btn.png                      edit_hover_btn.png                      edit_press_btn.png
edit_white_btn.png                      empty_slot.png                          empty_u1_mat.png
empty_u1_nozzle.png                     ff_check_off.svg                        ff_check_on.svg
ff_print_time.svg                       ff_print_weight.svg                     ff_warning.svg
file_list_refresh.svg                   four_color_disabled.svg                 four_color_select.svg
four_color_unselect.svg                 guider4.png                             guider4_pro.png
guider_3_ultra.png                      local_file_list.svg                     login-eye-off.svg
login-eye-on.svg                        login-lock.svg                          login-usr.svg
login-verify.svg                        modify_material.png                     monitor_device_empty.svg
nozzle_selected.svg                     nozzle_with_wire.png                    plug_slot_switch_btn_disabled.svg
plug_slot_switch_btn_select.svg         plug_slot_switch_btn_unselect.svg       print_check.svg
progress_num_1.svg                      progress_num_2.svg                      progress_num_3.svg
progress_num_4.svg                      push_button_arrow_dec_disable.svg       push_button_arrow_dec_hover.svg
push_button_arrow_dec_normal.svg        push_button_arrow_dec_press.svg         push_button_arrow_inc_disable.svg
push_button_arrow_inc_hover.svg         push_button_arrow_inc_normal.svg        push_button_arrow_inc_press.svg
push_button_confirm_hover.svg           push_button_confirm_normal.svg          push_button_confirm_press.svg
push_button_dec_disable.svg             push_button_dec_hover.svg               push_button_dec_normal.svg
push_button_dec_press.svg               push_button_inc_disable.svg             push_button_inc_hover.svg
push_button_inc_normal.svg              push_button_inc_press.svg               selected_slot.png
success_btn.svg                         supply_wire.png                         time_lapse_video.svg
title_close.png                         title_closeHover.png                    title_closePress.png
unbind_select.svg                       unknow_slot.png                         unknown_name.svg
unknown_u1_mat.png                      unprogress_num_1.svg                    unprogress_num_2.svg
unprogress_num_3.svg                    unprogress_num_4.svg                    video_download_error.png
withdrawn_wire.png
```

Two of these deserve a note:

- `guider4.png` - upstream ships it as `Guider4.png`. The code asks for `guider4`
  (`FFUtils::printer_preset_map`), which resolves on Windows' case-insensitive filesystem
  but would miss on Linux and macOS, so it is imported under the lowercase name the code
  actually requests.
- `Orca-FlashforgeTitle.ico` - window icon loaded by `PrintDevLocalFileDlg.cpp` through
  `resources_dir()` rather than the bitmap cache, and also absent.

### Created as placeholders (4 files)

These have **no counterpart anywhere in upstream's 1058-file `resources/images`**:

- `creator_5.svg`, `creator_5_pro.svg` - referenced by `FFUtils::printer_preset_map`, but
  the Creator 5 models are newer than the artwork in that tree. Neutral generic
  enclosed-printer line art on the same 1000x1000 canvas as the real model renders, in the
  same flat single-stroke style, with no FlashForge branding.
- `combobox_arrow_down.svg`, `combobox_arrow_up.svg` - used live by `NewTempInput.cpp` and
  hit at runtime. Solid triangles matching the existing `device_dropdown.svg`.

Replace the two printer renders with the real artwork when upstream ships it.

### Other resources checked

The FlashForge sources also reach for non-image resources through `resources_dir()`. All
were verified present except the `.ico` noted above:

- `resources/web/orca/missing_connection.html` (`PrinterCameraPanel.cpp`) - present.
- `resources/profiles/<vendor>/<model>_cover.png` (`PrinterModelPanel.cpp`) - present for
  every Flashforge model in `resources/profiles/Flashforge/`.
- `time_lapse_video.html` (`TimeLapseVideoPanel.cpp`) is written at runtime into `data_dir()`,
  not shipped - nothing to add.

## 2. A missing bitmap is no longer fatal

Importing the artwork fixes today's crash but not the class of bug - and, as section 1 shows,
it would not even have fixed today's crash completely, because 59 of the missing names were
invisible to static analysis. Both `create_scaled_bitmap()` overloads in
`src/slic3r/GUI/wxExtensions.cpp` ended a failed lookup with:

```cpp
throw Slic3r::RuntimeError("Could not load bitmap: " + bmp_name);
```

A single absent icon could therefore take down a whole tab. That is the wrong severity for a
missing decoration. Both sites now call a shared `missing_bitmap_placeholder()` which:

- logs `BOOST_LOG_TRIVIAL(warning)` **once per bitmap name** (a name that misses tends to
  miss on every repaint; unthrottled this would flood the log),
- keeps an `assert()` so a missing asset still fails loudly in debug builds, and
- returns a **fully transparent bitmap at the requested size** rather than an empty one.

The size matters. Callers across the FlashForge tab do `SetBitmap(...)` and `DrawBitmap(...)`
without an `IsOk()` check, and an invalid `wxBitmap` makes wxWidgets assert or draw nothing at
all; a correctly-sized transparent bitmap lets the surrounding sizer lay out exactly as it
would with real art, leaving a blank space instead of a broken window. Callers therefore need
no changes - the returned bitmap is always `IsOk()`.

Release builds no longer raise a dialog for a missing icon under any circumstances.

This change also paid for itself immediately: the once-per-name warning **is** the instrument
that produced the authoritative missing-bitmap list.

## 3. Why a fresh Bambu install landed on a Flashforge preset

This turned out to be a genuine defect, in `ConfigWizard::priv::apply_config()`
(`src/slic3r/GUI/ConfigWizard.cpp`), and not in the Flashforge code at all.

Two lines asked for the preferred printer with a vendor **key** that did not match the
**bundle** passed beside it:

```cpp
get_preferred_printer_technology("BBL", bundles.sm_bundle());
get_preferred_printer_model("BBL", bundles.sm_bundle(), preferred_variant);
```

`bundles.sm_bundle()` is the **Snapmaker** bundle (`PresetBundle::SM_BUNDLE == "Snapmaker"`).
Both helpers look the enabled-model set up in `app_config` under the *key*, then intersect it
with `bundle.vendor_profile->models`. Bambu model ids (`Bambu Lab X1 Carbon`, ...) and
Snapmaker ones (`Snapmaker U1`, ...) are disjoint namespaces, so the intersection was
**always empty**. The entire preferred-printer fast path was dead code on every wizard run.
Git blame puts the slip in the Snapmaker rebrand (`e89263e51a`), which rewrote the bundle
argument but left the `"BBL"` literal behind.

Control then fell into the fallback scan, which takes the first enabled vendor it meets while
iterating `BundleMap` - and `BundleMap` derives from `std::unordered_map`. The winner was
therefore decided by **hash-bucket order** over ~40 vendors. Flashforge winning on this
user's machine is not special; the same install could pick a different vendor on a different
STL version or with a different set of vendor JSONs present. The non-determinism is the bug.

Fixed at both sites:

- the key now matches the bundle (`PresetBundle::SM_BUNDLE`), restoring the intended
  in-house-first preference, and
- both fallback loops iterate vendor ids in **sorted order** via a new `sorted_bundle_ids()`
  helper, so when the fast path legitimately finds nothing the choice is at least
  reproducible rather than hash-dependent.

Note this only makes the selection deterministic and honours the in-house preference. A fresh
Bambu-only run enables no Snapmaker model, so it still reaches the fallback scan - it now just
does so predictably. If the user's config had *only* Flashforge profiles installed,
`PresetBundle::load_selections()`'s `unique_visible_printer()` fallback would select a
Flashforge preset regardless of the wizard, which is correct behaviour and is left alone.

## 4. Packaging

No packaging change is needed, and this was verified rather than assumed. The Windows install
rule in the top-level `CMakeLists.txt` copies the resources tree wholesale:

```cmake
install(DIRECTORY "${SLIC3R_RESOURCES_DIR}/" DESTINATION "./resources")
```

There is no per-file manifest or glob of `resources/images` anywhere in the build, and the
build tree reaches the same directory through a junction created by `src/CMakeLists.txt`.
New files under `resources/images/` are therefore picked up by both the portable tree
(`cmake --install`) and the NSIS installer (`cpack -G NSIS`) with no edit to either.

## Verification

A scratch instance was run against a data dir whose selected printer preset is
`Flashforge Creator 5 0.4 nozzle`, so `show_flashforge_device()` mounts the ported tab.

- Before the artwork import, with the non-fatal fallback in place: the tab mounted, the
  process stayed alive for the full run, and the log carried 59 `[warning]` lines and **zero**
  errors or `RuntimeError`s - the exact behaviour this change is meant to guarantee, and the
  measurement that produced the missing-name list.
- After importing the artwork: no `Could not load bitmap` line remains.

