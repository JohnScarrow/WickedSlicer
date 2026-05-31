# WickedSlicer Project Brief

## What This Is

A personal fork of OrcaSlicer (AGPL-3.0) called **WickedSlicer**. The primary goal is
making AI-assisted preset management seamless — external edits to JSON preset files are
detected automatically and surfaced as reviewable dirty changes in the UI.

## Current State (as of commit e13c3832bb)

All core features are **implemented and working**:

### 1. Live Preset Watcher (dirty-indicator mode)
- A `wxTimer` polls the user preset directories every 2 seconds
- When a file change is detected, `load_presets()` reloads the bundle from disk
- The watcher then restores the *selected* preset's config to its pre-change values,
  leaving the *edited* preset config at the new disk values
- This causes OrcaSlicer's built-in dirty indicator (the revert dot) to appear on
  every field the AI changed — the user reviews and clicks Save or Discard
- No restart required; no menu button needed

### 2. WickedSlicer Branding
- Title bar shows "WickedSlicer" via `update_title()` in `MainFrame.cpp`
- Splash screen uses a custom vectorized logo (`resources/images/splash_logo.svg` and
  `splash_logo_dark.svg`) — identical code path to upstream, only the SVG assets differ
- SVGs were generated from `WickedSlicer_Icon.png` using vtracer (480x480)

### 3. WSL2 Launch Script
- `run.sh` in repo root sets `GALLIUM_DRIVER=d3d12` and `GDK_BACKEND=x11` for
  NVIDIA RTX on WSL2, then launches `build/src/Release/orca-slicer`

## Build Environment (Linux / WSL2)

- **Build script**: `./build_linux.sh` (do NOT use raw cmake commands)
- **Binary**: `build/src/Release/orca-slicer`
- **Run**: `./run.sh`
- WSL2 requires `GALLIUM_DRIVER=d3d12` + `GDK_BACKEND=x11` (already in `run.sh`)
- ccache is configured for fast incremental builds

## Key Files Changed From Upstream

| File | What changed |
|---|---|
| `src/slic3r/GUI/MainFrame.hpp` | Added `m_preset_poll_timer`, `m_preset_dirs`, `m_preset_dir_mtimes`, `on_preset_poll_timer()` |
| `src/slic3r/GUI/MainFrame.cpp` | Timer setup in constructor, `on_preset_poll_timer()` handler, `update_title()` |
| `src/slic3r/GUI/GUI_App.cpp` | No changes from upstream (SVG splash loading is stock) |
| `resources/images/splash_logo.svg` | Replaced with WickedSlicer vectorized logo |
| `resources/images/splash_logo_dark.svg` | Same |
| `run.sh` | New — WSL2 launch script |

## Preset Directory Structure (Linux)

```
~/.config/OrcaSlicer/user/default/
    filament/base/    ← filament presets (watcher target)
    process/          ← process presets (watcher target)
    machine/          ← printer presets (watcher target)
```

## How the Watcher Works (technical)

In `MainFrame.cpp::on_preset_poll_timer()`:
1. Check directory mtimes via `boost::filesystem::last_write_time()`
2. If any changed, snapshot each Tab's `get_selected_preset().config`
3. Call `PresetBundle::load_presets()` — sets both selected and edited to new disk values
4. Call `update_side_preset_ui()` — refreshes dropdowns
5. For each tab, iterate `edited_config.keys()`:
   - If `edited_config.option(key) != old_selected.option(key)`: restore `selected_config` to old value
6. Call `tab->update_dirty()` — triggers dirty indicator on changed fields

## Known Behaviour

- If user discards a proposed change, the watcher won't re-propose until the file is
  modified again (directory mtime only changes on file write, not on UI discard)
- `wxFileSystemWatcher` was tried first but inotify events don't reach wxWidgets in WSL2;
  timer polling is the working solution

## Source Code

- GitHub upstream: `https://github.com/SoftFever/OrcaSlicer`
- Language: C++17, wxWidgets GUI, CMake + Ninja build
- Build platforms: Windows, macOS, Linux

## Ideas for Next Features

- **Per-field change highlight**: flash or tint the field background when the watcher
  first proposes a change, so it's obvious which fields updated
- **Watcher notification toast**: small non-blocking popup saying "X fields updated by
  external edit" with Save All / Discard All buttons
- **About dialog branding**: update `src/slic3r/GUI/AboutDialog.cpp` to show WickedSlicer
  name and version
- **PR to upstream**: the watcher logic in `MainFrame.hpp/.cpp` is the only upstream-worthy
  change; branding (SVGs, title, run.sh) stays in the fork

## Portfolio Notes

- Demonstrates C++17, wxWidgets GUI, filesystem event handling, and preset system internals
- The dirty-indicator workflow is a novel UX pattern for AI-assisted configuration tools
- WSL2 + NVIDIA OpenGL troubleshooting shows systems debugging ability
