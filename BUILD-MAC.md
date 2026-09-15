Building and playing on macOS (Apple Silicon)
=============================================

The `macos-port` branch runs the remaster natively on Apple Silicon.
Apple's own OpenGL stops at 4.1 and the engine needs a 4.3 core context, so GL is routed through Mesa's Zink (GL to Vulkan) onto kosmickrisp (Mesa's Vulkan to Metal driver for Apple GPUs).
There is no prebuilt macOS release yet; you build from source, and the game data is built from alariq's `mc2srcdata` by a script.
Verified on macOS 26 with an M2; any Apple Silicon Mac (M1 and later) should work.
Intel Macs are not supported (kosmickrisp is arm64 only).

## 1. Prerequisites

Xcode Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh), then:

```bash
brew install cmake git-lfs mesa molten-vk vulkan-loader sdl2 sdl2_mixer sdl2_ttf glew ffmpeg
```

`mesa` ships kosmickrisp and Zink.
`git-lfs` is needed to pull the movies from `mc2srcdata`.
You do not need `3rdparty.zip` on macOS; all dependencies come from Homebrew.

## 2. Build the engine

```bash
git clone --branch macos-port <this repo> mc2-opengl-remastered
cd mc2-opengl-remastered
cmake -S . -B build-mac -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-mac --target mc2 mc2_glshim -j8
```

Output: `build-mac/mc2` plus `build-mac/libmc2_glshim.dylib` (a `dlsym` interpose so GLEW finds every GL entry point through Mesa; the run script injects it).
Use `RelWithDebInfo`, same as Windows.

## 3. Build the game data

```bash
dev/macos-setup-data.sh
```

One idempotent command.
It builds the data tools, clones `alariq/mc2srcdata` (~715 MB plus ~186 MB of LFS movies) into `mc2srcdata/`, builds the `.fst` archives and fonts, and assembles a runnable `run/` directory that symlinks the built data and this repo's `shaders/` and `data/` overlays.
Both `mc2srcdata/` and `run/` are gitignored.
Re-run it any time; set `FORCE_DATA_BUILD=1` to rebuild the archives.

If you own a retail copy, point `MC2_RETAIL_GRAPHICS` at its `assets/Graphics` folder before running the script and it copies the anti-aliased `.d3f` fonts in.
Without them the fonts are rasterized from open TTFs and look aliased.

## 4. Play

```bash
cd run
MC2_MACOS_WINDOW=1 ../dev/macos-run.sh
MC2_MACOS_WINDOW=1 ../dev/macos-run.sh -mission mc2_01   # skip menus, load a mission
```

`dev/macos-run.sh` sets up the Mesa/Zink/kosmickrisp environment and execs `build-mac/mc2` with any arguments you pass.
`MC2_MACOS_WINDOW=1` gives you a real Cocoa window with mouse and keyboard.
Without it the game runs headless through SDL's offscreen driver, which is what the smoke tests and the multiplayer harness use.

Useful knobs:

| Variable | Effect |
|---|---|
| `MC2_LOG=1` | Keep the engine log on the terminal (without it stdout and stderr are silenced) |
| `MC2_BIN=path` | Run a different binary (default `build-mac/mc2`) |
| `MC2_MESA=prefix` | Mesa prefix if not `/opt/homebrew/opt/mesa` |
| `MC2_SKIP_INTRO=1` | Skip the intro movie |

The startup line `WARNING: ... doesn't support base Zink requirements: have_EXT_custom_border_color` is expected; the script overrides the reported GL version to 4.6 to get past it.

`dev/mc2app.env` is the env file for an optional `~/Applications/MC2.app` wrapper used by desktop-automation tooling.
You do not need it to play.

## 5. What is not on macOS

- The Mission Editor, UI Editor, asset viewer and `mc2-launcher` are Windows desktop tools and are skipped by CMake on Apple. Campaign and mod import through the launcher is therefore not available; the game runs the stock campaign from `mc2srcdata`.
- Terrain and building normal maps (`mat*_normal.tga`) are not produced by the macOS data pipeline, so terrain lighting is flat.
- Open issues are tracked one per line in [KNOWN_BUGS.md](KNOWN_BUGS.md).
