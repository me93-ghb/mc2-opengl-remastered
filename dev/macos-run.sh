#!/usr/bin/env bash
# macos-port: run MC2 with GL routed through Mesa Zink -> kosmickrisp -> Metal.
#
# Apple's OpenGL.framework caps at GL 4.1; MC2 needs a 4.3 core context (compute
# shaders + SSBO). This env stack turns Mesa's Zink into the GL implementation,
# translating to Vulkan, which kosmickrisp (Mesa's native Apple-Silicon Vulkan
# driver) turns into Metal. The mc2 binary is already linked against Mesa's
# libGL (see CMakeLists.txt "macos-port"), so the engine's GL calls and GLEW's
# proc lookups both land on Mesa.
#
# Requires: brew install mesa molten-vk vulkan-loader   (kosmickrisp ships in mesa)
#
# Usage:  dev/macos-run.sh [mc2 args...]
#   MC2_BIN=path        override the binary (default: build-mac/mc2)
#   MC2_MESA=prefix     override Mesa prefix (default: /opt/homebrew/opt/mesa)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESA="${MC2_MESA:-/opt/homebrew/opt/mesa}"
BREW_LIB="/opt/homebrew/lib"
ICD="${MESA}/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"
BIN="${MC2_BIN:-${REPO_ROOT}/build-mac/mc2}"

[ -f "$ICD" ] || ICD="/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"

# Mesa's libEGL/libGL (loaded by SDL and the binary) + the Vulkan loader.
export DYLD_LIBRARY_PATH="${MESA}/lib:${BREW_LIB}:${DYLD_LIBRARY_PATH:-}"

# Zink on kosmickrisp.
export VK_DRIVER_FILES="$ICD"
export MESA_LOADER_DRIVER_OVERRIDE=zink
export GALLIUM_DRIVER=zink

# kosmickrisp lacks EXT_custom_border_color, so Zink caps its own reported GL
# version low; force it back up. kosmickrisp's shader cache stores Metal PSO
# pointers that go stale across launches, so disable it.
export MESA_GL_VERSION_OVERRIDE="${MESA_GL_VERSION_OVERRIDE:-4.6}"
export MESA_GLSL_VERSION_OVERRIDE="${MESA_GLSL_VERSION_OVERRIDE:-460}"
export MESA_SHADER_CACHE_DISABLE=true

# SDL3's cocoa backend is CGL-only (Apple GL); its offscreen backend uses EGL,
# which is how we reach Mesa. Renders headless (no visible window yet).
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
export SDL_VIDEO_DRIVER="${SDL_VIDEODRIVER}"
export EGL_PLATFORM="${EGL_PLATFORM:-surfaceless}"

# SDL's own EGL/GL loader defaults to Linux .so names; point it at Mesa's
# dylibs explicitly (SDL3 names + sdl2-compat SDL2 names).
export SDL_EGL_LIBRARY="${MESA}/lib/libEGL.dylib"
export SDL_OPENGL_LIBRARY="${MESA}/lib/libGL.dylib"
export SDL_VIDEO_EGL_DRIVER="${MESA}/lib/libEGL.dylib"
export SDL_VIDEO_GL_DRIVER="${MESA}/lib/libGL.dylib"

# GLEW resolves GL entry points with dlsym(), which only sees Mesa libGL's flat
# exports; this interpose routes them through eglGetProcAddress (full ABI).
GLSHIM="$(dirname "$BIN")/libmc2_glshim.dylib"
if [ -f "$GLSHIM" ]; then
    export DYLD_INSERT_LIBRARIES="${GLSHIM}${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}"
else
    echo "[macos-run] WARNING: $GLSHIM not built; GLEW may miss GL entry points" >&2
fi

echo "[macos-run] MESA=$MESA  ICD=$(basename "$ICD")  SDL_VIDEODRIVER=$SDL_VIDEODRIVER" >&2
exec "$BIN" "$@"
