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

# Zink on kosmickrisp (shared by both modes).
export VK_DRIVER_FILES="$ICD"
export MESA_LOADER_DRIVER_OVERRIDE=zink
export GALLIUM_DRIVER=zink

# kosmickrisp lacks EXT_custom_border_color, so Zink caps its own reported GL
# version low; force it back up. kosmickrisp's shader cache stores Metal PSO
# pointers that go stale across launches, so disable it.
export MESA_GL_VERSION_OVERRIDE="${MESA_GL_VERSION_OVERRIDE:-4.6}"
export MESA_GLSL_VERSION_OVERRIDE="${MESA_GLSL_VERSION_OVERRIDE:-460}"
export MESA_SHADER_CACHE_DISABLE=true
export EGL_PLATFORM="${EGL_PLATFORM:-surfaceless}"

if [ -n "${MC2_MACOS_WINDOW:-}" ]; then
    # macos-port: VISIBLE WINDOW mode. Run SDL's *cocoa* driver so there is a
    # real window with mouse+keyboard; gos_render.cpp then renders on a manual
    # Mesa EGL context (Zink/kosmickrisp) and presents each frame through a Metal
    # SDL_Renderer. CRITICAL: Mesa's libGL/libEGL are keg-linked into
    # ${BREW_LIB}, and SDL's cocoa view does dlopen("libGL"); if that resolves
    # to Mesa's Zink libGL, +[NSOpenGLContext currentContext] SIGBUSes. So the
    # only thing on DYLD_LIBRARY_PATH is a curated dir holding *just* the Vulkan
    # loader (@rpath/libvulkan.1.dylib) -- Mesa's own libs load by their absolute
    # install-names, out of SDL's dlopen leaf-search path.
    VKONLY="${REPO_ROOT}/build-mac/macos-vk-only"
    mkdir -p "$VKONLY"
    [ -e "$VKONLY/libvulkan.1.dylib" ] || ln -s "${BREW_LIB}/libvulkan.1.dylib" "$VKONLY/libvulkan.1.dylib"
    export DYLD_LIBRARY_PATH="${VKONLY}:${DYLD_LIBRARY_PATH:-}"
    export SDL_VIDEODRIVER=cocoa
    export SDL_VIDEO_DRIVER=cocoa
    export SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-metal}"
    # The 16:9 UI-aspect canvas insets the 4:3-era menu and leaves the starfield
    # background bleeding into the letterbox margins on a non-16:9 display. Full-
    # stretch fills the screen the way the retail menu does; override to re-enable.
    export MC2_UI_ASPECT_ANCHOR="${MC2_UI_ASPECT_ANCHOR:-0}"
    # Do NOT point SDL at Mesa's libGL/libEGL here -- its cocoa view must use
    # Apple's OpenGL.framework, not Zink.
else
    # Headless/offscreen mode (smoke, framedump). SDL's offscreen backend uses
    # EGL to reach Mesa; no visible window. Mesa's libEGL/libGL + Vulkan loader
    # all on DYLD; no cocoa view exists so libGL leaf-exposure is harmless here.
    export DYLD_LIBRARY_PATH="${MESA}/lib:${BREW_LIB}:${DYLD_LIBRARY_PATH:-}"
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"
    export SDL_VIDEO_DRIVER="${SDL_VIDEODRIVER}"
    # SDL's own EGL/GL loader defaults to Linux .so names; point it at Mesa's
    # dylibs explicitly (SDL3 names + sdl2-compat SDL2 names).
    export SDL_EGL_LIBRARY="${MESA}/lib/libEGL.dylib"
    export SDL_OPENGL_LIBRARY="${MESA}/lib/libGL.dylib"
    export SDL_VIDEO_EGL_DRIVER="${MESA}/lib/libEGL.dylib"
    export SDL_VIDEO_GL_DRIVER="${MESA}/lib/libGL.dylib"
fi

# GLEW resolves GL entry points with dlsym(), which only sees Mesa libGL's flat
# exports; this interpose routes them through eglGetProcAddress (full ABI).
GLSHIM="$(dirname "$BIN")/libmc2_glshim.dylib"
if [ -f "$GLSHIM" ]; then
    export DYLD_INSERT_LIBRARIES="${GLSHIM}${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}"
else
    echo "[macos-run] WARNING: $GLSHIM not built; GLEW may miss GL entry points" >&2
fi

echo "[macos-run] MESA=$MESA  ICD=$(basename "$ICD")  SDL_VIDEODRIVER=$SDL_VIDEODRIVER  mode=${MC2_MACOS_WINDOW:+window}${MC2_MACOS_WINDOW:-headless}" >&2
exec "$BIN" "$@"
