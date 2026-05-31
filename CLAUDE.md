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

A `wxTimer` fires every 2 seconds and checks `boost::filesystem::last_write_time()` on
three user preset directories. When a change is detected:

1. Snapshot each Tab's `get_selected_preset().config`
2. Call `PresetBundle::load_presets()` — reloads everything from disk
3. Call `update_side_preset_ui()` — refreshes dropdowns
4. For each tab, compare `edited_config.keys()` vs the snapshot; for changed keys,
   restore `selected_config` to the old value via `set_key_value(key, old_opt->clone())`
5. Call `tab->update_dirty()` — dirty indicators appear on changed fields

The user then reviews proposed changes and clicks Save or Discard. `wxFileSystemWatcher`
was tried but inotify events don't reach wxWidgets in WSL2; timer polling is the fix.

**Watched directories** (Linux):
```
~/.config/OrcaSlicer/user/default/filament/base/
~/.config/OrcaSlicer/user/default/process/
~/.config/OrcaSlicer/user/default/machine/
```

**Key API facts learned:**
- `Tab::get_presets()` and `Tab::get_config()` are public
- `Tab::update_dirty()` is public; `Tab::load_key_value()` is protected
- `DynamicConfig::options` map is private — iterate via `keys()` + `option(key)`
- `DynamicConfig` deep-copies via `operator=` (clones all `ConfigOption*`)
- `set_key_value(key, raw_ptr)` takes ownership of the pointer

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
