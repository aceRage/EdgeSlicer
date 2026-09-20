# Design: Dockable/Collapsible Panels for All Gizmo Tools

Date: 2026-09-20
Status: approved by maintainer
Branch: `feat/gizmo-panel-docking`

## Goal

Give every windowed gizmo tool the same collapsible/dockable panel capability that
Cut, Assembly, Sculpt, and Edit already have: a title row with a **pin** (park the
panel against the right edge of the 3D view, remembered across sessions) and a
**chevron** (fold the body away, session-only).

Approved scope (option A): **all windowed tools** — Move, Rotate, Scale, Text,
Simplify, Measure, Brim Ears, Mesh Boolean, and the paint tools (Support paint,
Seam paint, Fuzzy skin, Color paint / MMU segmentation).

## Existing infrastructure (reused as-is)

`GLGizmoBase` already implements everything:

- `dock_setup_next_window(x, y, bottom_limit, window_width)` — positions the
  window either under its toolbar icon (undocked) or pinned to the right edge
  (docked). Persisted state is loaded lazily on first render.
- `dock_window_flags(flags)` — panel flags, plus resize/scroll flags a docked
  window needs.
- `dock_render_titlebar(title)` — draws title + pin + chevron; returns false when
  collapsed and the caller must skip the body.
- Persistence: `m_docked` is stored in AppConfig under `gizmo_dock_<key>`;
  `get_dock_key()` defaults to the gizmo's sprite id (= `GLGizmosManager::EType`),
  so each tool gets an independent remembered pin state for free.
  `m_collapsed` is deliberately session-only.

The four opted-in panels (Cut, Assembly, Sculpt, Edit) are the reference pattern;
they are **not** modified by this work.

## Base-class additions

Two small changes in `GLGizmoBase`:

1. **Size-to-content docked height (approved option A).**
   `dock_setup_next_window()` gains an opt-in height hint (new parameter, default
   = current full-height behavior). When docked *and* the hint is passed:
   - window height = content height, anchored at the top of the dock zone;
   - if the content exceeds the available height, fall back to full available
     height with scrollbar (exactly today's behavior).
   - When collapsed, the window is just the title row (existing behavior).
   Tall panels (the existing four) pass no hint and keep full height unchanged.

2. **Public dock facade for non-gizmo renderers.**
   Move/Rotate/Scale render through the shared `GizmoObjectManipulation` helper
   (`do_render_move_window` / `do_render_rotate_window` /
   `do_render_scale_input_window`), which is not a `GLGizmoBase` and cannot call
   the protected dock functions. The dock trio
   (`dock_setup_next_window` / `dock_window_flags` / `dock_render_titlebar`)
   becomes **public** with a comment marking them as the panel-renderer API.
   `GizmoObjectManipulation` receives the owning gizmo pointer (or a minimal
   struct of the three entry points) from Move/Rotate/Scale and drives the same
   machinery; window title continues to come from the existing
   `m_new_title_string + window_name` scheme, and `dock_render_titlebar` renders
   the human-readable title.

## Per-tool conversion

Each of the following swaps its window setup for the 3-call dock pattern:
`dock_setup_next_window(...)` → `push_toolbar_style` →
`GizmoImguiBegin(name, dock_window_flags(base_flags))` →
early-return through `GizmoImguiEnd`/`pop_toolbar_style` when
`!dock_render_titlebar(title)`. Per-panel quirks are preserved:

- extra-frame requests (`set_requires_extra_frame`) and active-item bookkeeping
  (e.g. Assembly's `m_last_active_item_imgui`) keep working when collapsed;
- undocked behavior is unchanged (`dock_setup_next_window` reproduces the old
  positioning exactly when `m_docked == false`, including draggable paint
  palettes).

Tools and their render paths:

| Tool | File(s) | Window rendered by |
|---|---|---|
| Move | `GLGizmoMove.cpp` → `GizmoObjectManipulation` | shared helper |
| Rotate | `GLGizmoRotate.cpp` → `GizmoObjectManipulation` | shared helper |
| Scale | `GLGizmoScale.cpp` → `GizmoObjectManipulation` | shared helper |
| Text | `GLGizmoText.cpp` | own `on_render_input_window` |
| Simplify | `GLGizmoSimplify.cpp` | own |
| Measure | `GLGizmoMeasure.cpp` | own |
| Brim Ears | `GLGizmoBrimEars.cpp` | own |
| Mesh Boolean | `GLGizmoMeshBoolean.cpp` | own |
| Support paint | `GLGizmoFdmSupports.cpp` | own |
| Seam paint | `GLGizmoSeam.cpp` (verify windowed) | own |
| Fuzzy skin | `GLGizmoFuzzySkin.cpp` | own |
| Color paint | `GLGizmoMmuSegmentation.cpp` | own |

Any tool in the list that turns out to render no input window is skipped.
Flatten is not in scope unless it has a window (verify during implementation).

## Non-goals

- No change to Cut/Assembly/Sculpt/Edit panels.
- No change to undocked panel behavior (position, dragging, styling).
- No new persistence keys beyond the existing `gizmo_dock_<key>` scheme.
- No translation-string changes (title/chevron/pin chrome is already translated
  by the existing four panels).

## Testing

There is no automated harness for ImGui gizmo panels; verification is:

1. Build `EdgeSlicer` (Release) plus compile-only confidence.
2. Manual smoke checklist in the maintainer's live-testing folder:
   - open each converted tool → panel shows title row with pin + chevron;
   - pin → panel parks at right edge; small panels size to content, tall ones
     keep full height;
   - collapse → body folds to title line; reopen;
   - undock → panel returns under its toolbar icon / previous floating behavior;
   - restart app → pinned tools stay pinned, unpinned stay unpinned;
   - paint-tool palettes remain draggable when unpinned;
   - Cut/Assembly/Sculpt/Edit unchanged.
3. Code-level checklist: every converted panel keeps its extra-frame and
   active-id bookkeeping intact on the collapsed early-return path.
