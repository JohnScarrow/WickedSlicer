#!/usr/bin/env bash
# Launch WickedSlicer (Release build) with correct WSL2/NVIDIA OpenGL settings
export GALLIUM_DRIVER=d3d12
export GDK_BACKEND=x11        # Use XWayland — native Wayland crashes in WSL2
export DISPLAY=${DISPLAY:-:0}
exec "$(dirname "$0")/build/src/Release/orca-slicer" "$@"
