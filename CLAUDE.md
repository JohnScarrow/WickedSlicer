# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

---

## Running a Single Test

Catch2 supports filtering by test name or tag:

```bash
# Run a single named test
cd build && ./tests/libslic3r/libslic3r_tests "[test name or tag]"

# Recommended flags for any manual test run
./tests/libslic3r/libslic3r_tests --order rand --warn NoAssertions

# Test suites available
# tests/libslic3r   — core geometry, config, mesh, G-code
# tests/fff_print   — FFF slicing logic
# tests/sla_print   — SLA slicing logic
# tests/libnest2d   — 2D nesting/packing
# tests/slic3rutils  — utility functions
```

Test data lives in `tests/data/` and is accessible inside tests via `TEST_DATA_DIR`.

---

## Architecture

### Two-Tier Design

**`src/libslic3r/`** — pure slicing engine with zero GUI dependencies. This is the heart of the application: geometry, mesh processing, configuration, slicing pipeline, G-code generation.

**`src/slic3r/GUI/`** — wxWidgets UI layer that wraps libslic3r. The GUI never does slicing work directly; it delegates to `BackgroundSlicingProcess` which runs the libslic3r pipeline on a worker thread.

The slicing pipeline is driven by `Print` / `PrintObject` (FFF) and `SLAPrint` (resin). Both expose a `process()` method that sequences steps via a `PrintStep` enum:

```
posSlice → posPerimeters → posInfill → posSupportMaterial → ... → posSimplifyPath → psGCodeExport
```

### Configuration / Preset System

All settings flow through a unified type hierarchy:

- `Config.hpp` — base key/value store (`DynamicConfig`, `StaticConfig`)
- `PrintConfig.hpp` — every print/printer/filament option, with metadata (min/max, tooltips, enum values)
- `PresetBundle` (`src/libslic3r/PresetBundle.cpp`) — loads and owns all preset collections (printer, filament, process). This is the class to touch for the "Reload Profiles" feature.
- `Preset` (`src/libslic3r/Preset.cpp`) — individual preset load/save/diff logic
- `AppConfig` (`src/slic3r/GUI/AppConfig.cpp`) — persistent UI/application settings (separate from print config)

Presets are loaded from:
- Bundled profiles: `resources/profiles/[manufacturer].json`
- User profiles: `<AppData>/OrcaSlicer/user/default/{filament,process,machine}/`

### GUI Layer Key Classes

| Class | File | Role |
|---|---|---|
| `MainFrame` | `GUI/MainFrame.cpp` | Top-level window, menu bar, toolbar |
| `Plater` | `GUI/Plater.cpp` | 3D viewport, model list, preset dropdowns |
| `Tab` subclasses | `GUI/Tab.cpp` | Printer / Filament / Process settings panels |
| `GLCanvas3D` | `GUI/GLCanvas3D.cpp` | OpenGL 3D rendering |
| `BackgroundSlicingProcess` | `GUI/Jobs/` | Worker thread for slicing |

Preset dropdowns in the `Plater` toolbar are the UI surface that must be refreshed after any preset reload.

### Key Subsystems

**Infill**: `src/libslic3r/Fill/` — each pattern is a `Fill` subclass (`FillGyroid`, `FillLightning`, `FillAdaptive`, etc.).

**Walls / Arachne**: `src/libslic3r/Arachne/` — variable-width wall toolpaths via `SkeletalTrapezoidation` + `BeadingStrategy`.

**Supports**: `src/libslic3r/Support/` (grid-based) and `TreeSupport.cpp` / `TreeSupport3D.cpp` (tree supports).

**G-code pipeline**: `src/libslic3r/GCode/` (~20 files) — includes `CoolingBuffer`, `WipeTower2`, `PostProcessor`, `ConflictChecker`, `AdaptivePA`.

**File formats**: `src/libslic3r/Format/` — `3mf.cpp` and `bbs_3mf.cpp` (Bambu extensions), `STL.cpp`, `AMF.cpp`, `STEP.cpp`.

**Device integration**: `src/slic3r/GUI/DeviceCore/` and `DeviceTab/` — Bambu Lab, Klipper, PrusaLink, OctoPrint network communication.

**G-code preview**: `src/libvgcode/` — standalone rendering module, no libslic3r dependency.

### Coding Conventions (from `.clang-format`)

- 4 spaces, no tabs; 140-character column limit
- Braces: wrap after class/function/struct bodies; do **not** wrap after namespace or control flow
- Omit braces on single-statement `if`/`for`/`while`
- Pointer declarator left-aligned: `int* ptr`
- No space before template angle brackets: `vector<int>`, not `vector< int >`
- `SortIncludes: false` — do not reorder `#include` blocks

---

## WickedSlicer — Custom Features Already Implemented

This is a fork called **WickedSlicer**. The following features are complete and committed.
See `CustomOrcaSlicer-Project.md` for full project context and future ideas.

### Live Preset Watcher (dirty-indicator mode)

`src/slic3r/GUI/MainFrame.hpp` / `MainFrame.cpp`

A `wxTimer` fires every 2 seconds and checks per-file `boost::filesystem::last_write_time()`
on all `.json` files across the watched preset directories. When a change is detected,
`reload_presets_from_disk(true)` is called:

1. Snapshot each Tab's `get_selected_preset().config`
2. Call `PresetBundle::load_presets()` — reloads everything from disk
3. Call `update_side_preset_ui()` — refreshes dropdowns
4. For each tab, compare `edited_config.keys()` vs the snapshot; for changed keys,
   restore `selected_config` to the old value via `set_key_value(key, old_opt->clone())`
5. Call `tab->update_dirty()` + `tab->reload_config()` — dirty indicators appear

The user then reviews proposed changes and clicks Save or Discard.

**Implementation note:** Per-file mtime tracking (`m_preset_file_mtimes: map<string, time_t>`)
is used instead of directory mtime because Linux only updates directory mtime on file
creation/deletion, not on content changes. `wxFileSystemWatcher` was tried but inotify
events don't reach wxWidgets in WSL2; timer polling is the fix.

**Watched directories** (Linux):
```
~/.config/OrcaSlicer/user/default/filament/base/
~/.config/OrcaSlicer/user/default/process/
~/.config/OrcaSlicer/user/default/machine/
~/.config/OrcaSlicer/system/Custom/{filament,process,machine}/
```

**`reload_presets_from_disk(bool mark_dirty = true)`** is a public helper on `MainFrame`:
- `mark_dirty=true` (default, used by watcher) — restores old selected values so dirty
  indicators appear; user reviews and saves/discards
- `mark_dirty=false` — accepts new values immediately with no dirty indicator (reserved
  for future use cases where no review is needed)

### AI Assistant Panel

`src/slic3r/GUI/AIAssistantPanel.hpp` / `AIAssistantPanel.cpp`

A Claude-powered chat panel docked to the right side of the Plater. Toggle via
**View → AI Assistant**. Uses `claude --print` via `popen()` in a `boost::thread` —
no API key required; works with an existing Claude Pro subscription.

**Architecture:**

- `wxTextCtrl` (read-only) for conversation display, `wxTextCtrl` for input, Send / Clear buttons
- Background thread reads `claude --print` output line-by-line, posting `wxEVT_AI_TOKEN`
  events to the main thread via `wxQueueEvent`
- `wxEVT_AI_DONE` / `wxEVT_AI_ERROR` signal completion or failure
- Prompt is written to `/tmp/wicked_ai_prompt_<pid>.txt`; deleted after `pclose()`

**Preset context injection:**

Every prompt includes a `build_context_block()` snapshot of the currently active presets:
- Printer name, file, nozzle diameter, bed type
- Process name, file, and ~10 key settings (layer height, infill, walls, speeds, etc.)
- Full raw process preset JSON (so Claude knows exact key names for `WICKED_CHANGE`)
- Filament name, material, temperatures (up to 4 filaments)
- Active project name

**WICKED_CHANGE protocol — in-memory preset editing:**

When the user asks Claude to change a setting, Claude emits a structured marker:
```
WICKED_CHANGE:{"file":"/absolute/path.json","key":"exact_key_name","value":new_value}
```
`on_ai_done()` scans `m_current_response` for these markers via `parse_change_requests()`.
For each one, a confirmation dialog is shown. On approval:
1. `json_to_serialize_str(value)` converts the JSON value to the string format
   `DynamicConfig::set_deserialize_strict` expects (bool→"1"/"0", array→comma-joined, etc.)
2. `preset_type_from_path(file)` maps the file path to `Preset::TYPE_PRINT / FILAMENT / PRINTER`
3. `wxGetApp().get_tab(type)->get_config()->set_deserialize_strict(key, str)` — in-memory only
4. `tab->update_dirty()` — dirty indicator (pencil) appears on the preset dropdown
5. `tab->reload_config()` — UI widgets refresh to show the new value

**No file is written.** The user reviews the dirty preset and saves (or discards) via the
normal preset Save button. This is identical to manually editing a field in the Tab UI.

**Key guard:** `apply_preset_change_in_memory()` reads the on-disk JSON first and rejects
any key not already present, returning a list of valid keys so Claude can self-correct.

**Conversation persistence:**

History is stored in `AppConfig` under section `ai_chat_history`, key `default`.
Loaded on startup and rendered via `render_history()`. Cleared by the Clear button
(also erases from AppConfig). Max 40 exchanges (80 messages) kept in memory.

**Key API facts:**
- `Tab::get_config()` returns `DynamicPrintConfig*` of the currently edited preset
- `Tab::update_dirty()` and `Tab::reload_config()` are public
- `Tab::load_key_value()` is protected — use `set_deserialize_strict` + `update_dirty` instead
- `DynamicConfig::set_deserialize_strict(key, str)` throws `std::exception` on bad values

---

### Branding

- Title bar: `update_title()` in `MainFrame.cpp` calls `SetTitle("WickedSlicer")`
- Splash: `resources/images/splash_logo.svg` and `splash_logo_dark.svg` replaced with
  vectorized WickedSlicer logo (480x480, generated with `vtracer` Python package).
  Code path in `GUI_App.cpp` is **identical to upstream** — only the SVG assets differ.

### WSL2 Launch Script

`run.sh` — sets `GALLIUM_DRIVER=d3d12` + `GDK_BACKEND=x11`, then exec's the binary.
Required for NVIDIA RTX on WSL2 (Mesa D3D12 backend + force XWayland over Wayland).

## Build & Run (WSL2 / Linux)

```bash
./build_linux.sh          # always use this, not raw cmake
./run.sh                  # launch with correct WSL2 env vars
```

Binary is at `build/src/Release/orca-slicer`.
