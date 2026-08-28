#!/usr/bin/env bash
# macos-port: build MC2's game data from alariq/mc2srcdata and assemble a
# runnable game directory, so `dev/macos-run.sh` has real data to render.
#
# One command, idempotent: re-run it any time. It (1) builds the data tools,
# (2) clones mc2srcdata + pulls its LFS movies, (3) builds the .fst archives +
# fonts (fonts go through the same Zink path as the engine), (4) assembles
# run/ (gitignored) with symlinks to the built data + the remaster's shaders.
#
# Both mc2srcdata/ and run/ live in the repo but are gitignored, so nothing
# large is ever committed or pushed.
#
# Play after this finishes:   cd run && MC2_LOG=1 ../dev/macos-run.sh
#
#   MC2_SRCDATA=<dir>  override the mc2srcdata clone (default: <repo>/mc2srcdata)
#   MC2_RUNDIR=<dir>   override the assembled game dir (default: <repo>/run)
#   FORCE_DATA_BUILD=1 rebuild the .fst/fonts even if they already exist
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MESA="${MC2_MESA:-/opt/homebrew/opt/mesa}"
BUILD="$REPO/build-mac"
SD="${MC2_SRCDATA:-$REPO/mc2srcdata}"
RUN="${MC2_RUNDIR:-$REPO/run}"
BS="$SD/build_scripts"
GLSHIM="$BUILD/libmc2_glshim.dylib"
say() { printf '\033[1;36m[setup-data]\033[0m %s\n' "$*" >&2; }

# --- 1. data tools ---------------------------------------------------------
need_tools=
for t in makefst aseconv makersp pak; do [ -x "$BUILD/out/data_tools/$t" ] || need_tools=1; done
[ -x "$BUILD/out/text_tool/text_tool" ] || need_tools=1
if [ -n "$need_tools" ]; then
    say "building data tools + glshim"
    cmake --build "$BUILD" --target makefst aseconv makersp pak text_tool mc2_glshim -j8
fi
[ -f "$GLSHIM" ] || cmake --build "$BUILD" --target mc2_glshim -j8

# --- 2. mc2srcdata + LFS movies -------------------------------------------
if [ ! -d "$SD/.git" ]; then
    say "cloning alariq/mc2srcdata (~715MB) into $SD"
    git clone --depth 1 https://github.com/alariq/mc2srcdata.git "$SD"
fi
say "pulling LFS movies (~186MB, no-op if already present)"
git -C "$SD" lfs install --local >/dev/null 2>&1 || true
git -C "$SD" lfs pull

# --- 3. build .fst archives + fonts ---------------------------------------
for t in makefst aseconv makersp pak; do cp "$BUILD/out/data_tools/$t" "$BS/$t"; done
cp "$BUILD/out/text_tool/text_tool" "$BS/text_tool"
chmod +x "$BS"/{makefst,aseconv,makersp,pak,text_tool}

# The Zink env (mirrors dev/macos-run.sh) — text_tool creates a GL context to
# rasterize fonts, so it needs Mesa/Zink just like the engine does.
ICD="$MESA/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"
[ -f "$ICD" ] || ICD="/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"
export DYLD_INSERT_LIBRARIES="$GLSHIM"
export DYLD_LIBRARY_PATH="$MESA/lib:/opt/homebrew/lib"
export VK_DRIVER_FILES="$ICD" MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink
export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 MESA_SHADER_CACHE_DISABLE=true
export SDL_VIDEODRIVER=offscreen SDL_VIDEO_DRIVER=offscreen EGL_PLATFORM=surfaceless
export SDL_EGL_LIBRARY="$MESA/lib/libEGL.dylib" SDL_OPENGL_LIBRARY="$MESA/lib/libGL.dylib"

fst_count=$(ls "$BS"/*.fst 2>/dev/null | wc -l | tr -d ' ')
font_count=$(ls "$BS"/assets/graphics/*.bmp 2>/dev/null | wc -l | tr -d ' ')
if [ "${FORCE_DATA_BUILD:-}" = 1 ] || [ "$fst_count" -lt 8 ] || [ "$font_count" -lt 12 ]; then
    say "building data (.fst + tgl + sound + objects); movies build to unused .mpg, ignore"
    ( cd "$BS" && make all BUILD_PLATFORM=linux -k ) || true   # -k: ride past flaky font/movie steps

    # Fonts: kosmickrisp is flaky under rapid context churn, so text_tool can
    # segfault intermittently. Generate the 12 fonts with retries.
    mkdir -p "$BS/assets/graphics"
    gen() { # ttf size out
        for _ in 1 2 3 4 5; do
            ( cd "$BS" && ./text_tool "../fonts/$1" "$2" "assets/graphics/$3" ) >/dev/null 2>&1 || true
            [ -f "$BS/assets/graphics/$3.bmp" ] && [ -f "$BS/assets/graphics/$3.glyph" ] && return 0
        done
        say "WARNING: font $3 failed after 5 tries"; return 0
    }
    for f in arial8 arial9game arial9chat arialnarrow8 arialnarrow9 arialblack9 agencyfb8 agencyfb11; do
        gen emulogic.ttf 8 "$f"; done
    for f in agencyfb14 agencyfb17 agencyfb20; do gen Mecha.ttf 14 "$f"; done
    gen 6px2bus.ttf 6 arialnarrow6
else
    say "data already built ($fst_count .fst, $font_count fonts) — skipping (FORCE_DATA_BUILD=1 to rebuild)"
fi

# --- 3b. retail .d3f fonts (crisp) ----------------------------------------
# text_tool's .bmp/.glyph fonts are rasterized from open TTFs and come out
# aliased/low-res. The retail .d3f fonts (from a licensed MC2 copy) embed an
# 8-bit-alpha, anti-aliased atlas, and gosFont::load prefers a .d3f over the
# same-basename .bmp/.glyph. Copy them in (lowercased to match) when the retail
# Graphics dir is present; MC2_RETAIL_GRAPHICS overrides the default location.
# Always runs (also upgrades an already-built data dir); harmless when absent.
RETAIL_GFX="${MC2_RETAIL_GRAPHICS:-$REPO/../MC2/FinalBuild/assets/Graphics}"
if [ -d "$RETAIL_GFX" ]; then
    mkdir -p "$BS/assets/graphics"; d3f_n=0
    for f in "$RETAIL_GFX"/*.d3f; do
        [ -e "$f" ] || continue
        lb=$(basename "$f" | tr '[:upper:]' '[:lower:]')
        cp -f "$f" "$BS/assets/graphics/$lb"; d3f_n=$((d3f_n + 1))
    done
    say "imported $d3f_n retail .d3f fonts (crisp anti-aliased) from $RETAIL_GFX"
else
    say "no retail .d3f fonts at $RETAIL_GFX — using built .bmp/.glyph (aliased); set MC2_RETAIL_GRAPHICS to enable crisp fonts"
fi

# --- 4. assemble run/ ------------------------------------------------------
say "assembling game dir: $RUN"
rm -rf "$RUN"; mkdir -p "$RUN/data"
for f in "$BS"/*.fst "$BS"/*.cfg "$BS"/testtxm.tga; do ln -s "$f" "$RUN/"; done
ln -s "$BS/assets" "$RUN/assets"
ln -s "$REPO/shaders" "$RUN/shaders"
# data/: built (objects/sound) + remaster overlays (materials/hdr/defs/...);
# movies point at the SOURCE .mp4 (the engine wants .mp4, not the .mpg build output)
for d in "$BS"/data/*; do [ "$(basename "$d")" = movies ] || ln -s "$d" "$RUN/data/$(basename "$d")"; done
ln -s "$SD/movies" "$RUN/data/movies"
for d in "$REPO"/data/*; do n=$(basename "$d"); [ -e "$RUN/data/$n" ] || ln -s "$d" "$RUN/data/$n"; done
# options.cfg (real copy) from minprefs; enable .mp4 movies (UseUpscaledVideos)
cp "$BS/minprefs.cfg" "$RUN/options.cfg"
grep -q 'UseUpscaledVideos' "$RUN/options.cfg" || \
    awk '1; /^\[MechCommander2\]/ && !d {print "l UseUpscaledVideos = 1"; d=1}' \
        "$RUN/options.cfg" > "$RUN/options.cfg.tmp" && mv "$RUN/options.cfg.tmp" "$RUN/options.cfg"

say "done."
printf '\n  Play:   cd %s && MC2_LOG=1 ../dev/macos-run.sh\n\n' "$RUN" >&2
