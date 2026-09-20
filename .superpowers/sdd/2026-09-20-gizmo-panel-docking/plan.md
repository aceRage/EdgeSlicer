# Gizmo Panel Docking + Opacity — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (inline). Steps use checkbox syntax.
> Spec: `.superpowers/sdd/2026-09-20-gizmo-panel-docking/design.md`. Branch: `feat/gizmo-panel-docking` (off `origin/main`).

**Goal:** Every windowed gizmo panel gains pin/collapse docking (like Cut/Assembly/Sculpt/Edit) plus a global opacity preference.

**Architecture:** Reuse the existing `GLGizmoBase` dock kit. Add a size-to-content docked mode and make the dock trio public; drive it from `GizmoObjectManipulation` for Move/Rotate/Scale; convert each remaining panel's window setup; apply per-window bg alpha in `GizmoImguiBegin` + the manipulation windows.

**Tech Stack:** C++17, ImGui (wrapped by `ImGuiWrapper`), wxWidgets, AppConfig.

## Global Constraints

- Build cmake: `C:\dev\tools\cmake-3.31.8-windows-x86_64\bin\cmake.exe` (not on PATH).
- Build dir: `C:\dev\SnapmakerOrca\build` (VS2022 generator, Release). Build detached: `Start-Process ... -RedirectStandardOutput "$env:TEMP\edgebuild.log" -WindowStyle Hidden`, then poll; kill orphan `cl`/`MSBuild` after.
- Do NOT modify the four existing opted-in panels (Cut, Assembly, Sculpt, Edit).
- Undocked behavior of every converted panel must be unchanged.
- No new translation-dependent doc references (Validate Documentation CI stays green).

---

### Task 1: GLGizmoBase — size-to-content, public facade, opacity

**Files:**
- Modify: `src/slic3r/GUI/Gizmos/GLGizmoBase.hpp` (dock section ~line 270-310; private members ~343-356)
- Modify: `src/slic3r/GUI/Gizmos/GLGizmoBase.cpp` (GizmoImguiBegin ~343; dock section 416-533)

**Interfaces:**
- Produces: `dock_setup_next_window(float &x, float &y, float bottom_limit, float window_width = 0.f, bool size_to_content = false)` (new last param); `dock_window_flags`, `dock_render_titlebar`, `get_dock_key`, `is_docked` become **public**; `static float gizmo_panel_opacity()` (new public).

- [ ] **Step 1:** In `GLGizmoBase.hpp`, move the three dock function declarations + `get_dock_key()` + `is_docked()` from `protected:` to `public:` (comment: "Public so non-gizmo panel renderers (GizmoObjectManipulation) can drive the same docking."). Add public declaration:

```cpp
    // Global panel opacity preference (AppConfig "gizmo_panel_opacity"),
    // clamped 0.3..1, default 1 (opaque). Applied per window in GizmoImguiBegin.
    static float gizmo_panel_opacity();
```

Add private member next to `m_docked`:

```cpp
    bool m_dock_size_to_content{ false }; // set by dock_setup_next_window each frame
```

- [ ] **Step 2:** In `GLGizmoBase.cpp`, change the signature and add the size-to-content branch:

```cpp
void GLGizmoBase::dock_setup_next_window(float &x, float &y, float bottom_limit, float window_width, bool size_to_content)
{
    load_dock_state();
    m_dock_size_to_content = size_to_content;

    if (!m_docked) {
        // ... existing undocked block unchanged ...
        return;
    }

    if (size_to_content) {
        // Small panel: keep AlwaysAutoResize, right-top anchored. Collapsed it
        // is just the title row, which auto-resize shrinks to by itself.
        const Size  cnv_size   = m_parent.get_canvas_size();
        const float collapse_h = m_parent.is_collapse_toolbar_on_left() ? 0.f
                                                                        : m_parent.get_collapse_toolbar_height();
        const float top = std::max(y, collapse_h + m_imgui->scaled(0.3f));
        m_imgui->set_next_window_pos((float) cnv_size.get_width(), top, ImGuiCond_Always, 1.0f, 0.0f);
        return;
    }
    // ... existing full-height docked block unchanged ...
}
```

- [ ] **Step 3:** In `dock_window_flags`, keep `AlwaysAutoResize` when size-to-content:

```cpp
int GLGizmoBase::dock_window_flags(int flags) const
{
    if (!m_docked)
        return flags;

    flags &= ~ImGuiWindowFlags_NoScrollbar;
    if (!m_dock_size_to_content)
        flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
    flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
             ImGuiWindowFlags_NoTitleBar;
    return flags;
}
```

- [ ] **Step 4:** Opacity in `GizmoImguiBegin` + new static:

```cpp
bool GLGizmoBase::GizmoImguiBegin(const std::string &name, int flags)
{
    m_imgui->set_next_window_bg_alpha(gizmo_panel_opacity());
    return m_imgui->begin(name, flags);
}

float GLGizmoBase::gizmo_panel_opacity()
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return 1.f;
    const std::string v = cfg->get("gizmo_panel_opacity");
    if (v.empty())
        return 1.f;
    try {
        return std::clamp(std::stof(v), 0.3f, 1.f);
    } catch (...) {
        return 1.f;
    }
}
```

- [ ] **Step 5:** Commit: `git add -A && git commit -m "feat(gui): size-to-content docking, public dock facade, panel opacity hook"`.

---

### Task 2: Move/Rotate/Scale via GizmoObjectManipulation

**Files:**
- Modify: `src/slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp` — add member `const GLGizmoBase *m_dock_owner{ nullptr };` and inline `void set_dock_owner(const GLGizmoBase *owner) { m_dock_owner = owner; }` (header already sees GLGizmoBase via include; if not, forward-declare).
- Modify: `src/slic3r/GUI/Gizmos/GizmoObjectManipulation.cpp` — three functions with the same skeleton: `do_render_move_window` (~801), `do_render_rotate_window` (~1194), `do_render_scale_input_window` (~1380). Each: manual position block → `push_toolbar_style` + `PushStyleVar(ItemSpacing)` → `imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS)` → body → `imgui_wrapper->end(); ImGui::PopStyleVar(1); ImGuiWrapper::pop_toolbar_style();`

**Interfaces:**
- Consumes: Task 1 public `dock_setup_next_window(x,y,bottom_limit,0,true)`, `dock_window_flags`, `dock_render_titlebar`, `gizmo_panel_opacity()`.

- [ ] **Step 1:** In each of the three functions, make the manual position block conditional:

```cpp
    if (m_dock_owner) {
        m_dock_owner->dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    } else {
        // ... existing clamp + set_next_window_pos block unchanged ...
    }
```

- [ ] **Step 2:** In each of the three, replace the begin sequence:

```cpp
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    if (m_dock_owner) {
        imgui_wrapper->set_next_window_bg_alpha(GLGizmoBase::gizmo_panel_opacity());
        imgui_wrapper->begin(_L(name), m_dock_owner->dock_window_flags(ImGuiWrapper::TOOLBAR_WINDOW_FLAGS | ImGuiWindowFlags_NoTitleBar));
        if (!m_dock_owner->dock_render_titlebar(this->m_new_title_string)) {
            imgui_wrapper->end();
            ImGui::PopStyleVar(1);
            ImGuiWrapper::pop_toolbar_style();
            return;
        }
    } else {
        imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);
    }
```

- [ ] **Step 3:** Wire the owner. In the constructor or `on_init` of each gizmo where `m_object_manipulation` is created (`GLGizmoMove.cpp`, `GLGizmoRotate.cpp`, `GLGizmoScale.cpp`), add:

```cpp
    m_object_manipulation->set_dock_owner(this);
```

- [ ] **Step 4:** Commit: `"feat(gui): dock/collapse for Move/Rotate/Scale panels"`.

---

### Task 3: Paint palettes (Support/Seam/FuzzySkin/MMU/BrimEars)

**Files:** `GLGizmoFdmSupports.cpp` (~190), `GLGizmoSeam.cpp` (~148), `GLGizmoFuzzySkin.cpp` (~130), `GLGizmoMmuSegmentation.cpp` (~433), `GLGizmoBrimEars.cpp` (~636). All five use the identical pattern:

```cpp
    const float approx_height = m_imgui->scaled(XX);
    y = std::min(y, bottom_limit - approx_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always[, pivot]);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
```

(BrimEars additionally pushes two PushStyleVar after push_toolbar_style — keep; its `if (!mo) return;` precedes the position code, fine.)

- [ ] **Step 1 (per file):** Replace the position lines (`approx_height` + `min` + `GizmoImguiSetNextWIndowPos`) with:

```cpp
    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
```

change begin to:

```cpp
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar));
```

insert after begin:

```cpp
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }
```

For **BrimEars**, the collapsed early return must pop its two extra style vars first: `ImGui::PopStyleVar(2);` before `ImGuiWrapper::pop_toolbar_style();` (match the function's normal exit pop count).

- [ ] **Step 2:** Commit: `"feat(gui): dock/collapse for paint-tool palettes"`.

---

### Task 4: Utility panels (Measure/MeshBoolean/Text/Simplify)

- [ ] **Step 1 — Measure** (`GLGizmoMeasure.cpp` ~2189-2205): replace `GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);` (line ~2195) with `dock_setup_next_window(x, y, bottom_limit, 0.f, true);` (keep the `static last_y/last_h` extra-frame bookkeeping and `m_current_active_imgui_id` capture before it). Wrap begin flags with `dock_window_flags(...)`. After begin insert (Assembly-style active-item advance):

```cpp
    if (!dock_render_titlebar(get_name())) {
        m_last_active_item_imgui = m_current_active_imgui_id;
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }
```

- [ ] **Step 2 — MeshBoolean** (`GLGizmoMeshBoolean.cpp` ~198-201): replace `GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);` with `dock_setup_next_window(x, y, bottom_limit, 0.f, true);` (keep `static last_y/last_w` bookkeeping). Begin keeps literal name `"MeshBoolean"`; wrap flags; titlebar check:

```cpp
    if (!dock_render_titlebar("MeshBoolean")) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }
```

- [ ] **Step 3 — Text** (`GLGizmoText.cpp` ~780-797): tall panel → **full-height** docking: replace the position call before begin with `dock_setup_next_window(x, y, bottom_limit);` (no 5th arg). Wrap flags with `dock_window_flags(...)`; titlebar check with literal `"Text"`; collapsed early return pops the two extra style vars: `ImGui::PopStyleVar(2);` before `ImGuiWrapper::pop_toolbar_style();` (match the function's normal exit).

- [ ] **Step 4 — Simplify** (`GLGizmoSimplify.cpp` ~225-238): uses `push_common_window_style` + native title bar. Replace the position call preceding `push_common_window_style` with `dock_setup_next_window(x, y, bottom_limit, 0.f, true);`; add `m_imgui->set_next_window_bg_alpha(GLGizmoBase::gizmo_panel_opacity());` before begin; change begin flags to `dock_window_flags(ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)`; after begin:

```cpp
    if (!dock_render_titlebar(on_get_name())) {
        m_imgui->end();
        // match the function's normal style pops exactly (read its exit path)
        return;
    }
```

- [ ] **Step 5:** Commit: `"feat(gui): dock/collapse for Measure/MeshBoolean/Text/Simplify panels"`.

---

### Task 5: Preferences opacity slider

**Files:** Modify `src/slic3r/GUI/Preferences.cpp` (GUI options section).

- [ ] **Step 1:** Find the appconfig-backed options pattern: `grep -n "appconfig" src/slic3r/GUI/Preferences.cpp`. Copy the nearest float appconfig option verbatim, changing: label `L("Gizmo panel opacity")`, tooltip `L("Background opacity of the tool panels on the 3D view. Lower values let you see the model behind a docked panel.")`, key `"gizmo_panel_opacity"`, default `1.0`, min `0.3`, max `1`.
- [ ] **Step 2:** Confirm `src/libslic3r/AppConfig.hpp` `set(const std::string &key, const std::string &value)` is generic (no allow-list).
- [ ] **Step 3:** Commit: `"feat(gui): gizmo panel opacity preference"`.

---

### Task 6: Build, smoke-test, install, PR

- [ ] **Step 1:** Full Release build (detached + poll + orphan cleanup). Expect only touched TUs to recompile.
- [ ] **Step 2:** Stop EdgeSlicer if running; copy `build\src\Release\EdgeSlicer.exe` + `EdgeSlicer.dll` into `build\Snapmaker_Orca`.
- [ ] **Step 3:** Maintainer smoke checklist in the testing folder:
  1. Move/Rotate/Scale: pin → top-right content-sized; chevron folds; unpin returns under icon; restart keeps per-tool pin state.
  2. Text: pin → full-height dock with scrollbar.
  3. Paint palettes unpinned behave as before; pin/collapse works.
  4. Simplify/Measure/MeshBoolean/BrimEars same.
  5. Cut/Assembly/Sculpt/Edit unchanged.
  6. Preferences slider 50% → all panels translucent, readable; persists; 100% = normal.
- [ ] **Step 4:** Push branch; open draft PR to `main`.

---

## Self-review notes (done)
- Spec coverage: dock/collapse all windowed tools (T2-T4), size-to-content (T1/T3/T4), opacity global (T1/T5), persistence (existing scheme), undocked unchanged (each step keeps the old path in `else`), smoke checklist (T6).
- Naming consistent everywhere: `gizmo_panel_opacity` (AppConfig + static), `set_dock_owner` / `m_dock_owner`, `m_dock_size_to_content`.
- Out of scope per spec: SVG/Emboss/SLA/Hollow/FaceDetector/AdvancedCut panels untouched.
