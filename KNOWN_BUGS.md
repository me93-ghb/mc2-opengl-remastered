# Known bugs (macos-port)

Open issues on the `macos-port` branch. One line each; fix and delete the line when done.

## UI / render
- Thin vertical line down the far-right edge of the mission screen. Pre-existing, cause unknown.
- Mission-results banner: a few-pixel gap between the black box and the hazard-border frame (left/top). Cosmetic, ~matches retail. See MACOS-PORT-16.
- Promotion screen intermittent static-prop pop-in. gos_static_prop_registry.cpp. (May share the MACOS-PORT-27 cause — verify on next promotion screen.)
- Fast camera scrolls dip framerate a bit since MACOS-PORT-27's whole-map object admission (every prop feeds update/touch + GPU cull each frame). Upgrade path: frustum-true block test in the OBJ-CULL-BYPASS site (mclib/terrain.cpp).

## Terrain / assets (data gaps, not code bugs)
- No PBR normal detail on terrain/buildings: mat0_normal.tga..mat8_normal.tga are not built by the macOS data pipeline. Flat lighting only.
- Night lighting leftovers (epic core landed in MACOS-PORT-29; user: "not finished"):
  craters/footprint decals (decal.frag) not night-dimmed; GV (vehicle) search-light beam
  cones not restored (mech + building cones are — same RenderSpotlightChildren treatment,
  gvactor.cpp); spotlight buildings render retail-CPU at night and read brighter than GPU
  neighbours because the static-prop shader skips point-light contributions (add a
  point/spot term to static_prop.vert from its ObjectLights rows to close the gap
  upward); light-grid per-cell cap is 15 strongest (seam possible only if a cell has 15+
  significant lights); pool look tunable live via MC2_NIGHT_TUNE=1 + run/night_tune.txt
  (gain gamma albedo_floor glow); camera-move lag on night maps needs a clean measure
  (user had Claude+ChatGPT running). Shader hot-reload (MC2_SHADER_HOT_RELOAD) does NOT
  cover terrain_lod_chunk / terrain_overlay / static_prop programs (only
  gosRenderMaterial materialList_) — shader edits there need a relaunch.

## Non-fatal / to verify
- INTERMITTENT: Razorback (chassis 'werewolf') vanished from purchase lists (missions 2 AND 5) within one long session — all other chassis fine — and a fresh boot with the same save healed it. Data and code verified clean (purchase02/05 both list werewolf; fresh-campaign harness and fresh save-load both show RAZORBACK; MC2_LOG_LOGISTICS=1 baseline: purchase05 newMechAvailableCount=6). Suspect in-memory variant/availability state corrupted by session history (multiple save loads / mission cycles / mech lab). Note: LogisticsData::loadVariant creates custom variants as `new LogisticsVariant(pChassis, 0)` — possible ID collision with the stock variant 0. If it recurs: MC2_LOG_LOGISTICS=1 and grab updateAvailability + [MECHLIST] lines before restarting.
- Some GL call raises GL_INVALID_OPERATION during mission frames; source unknown (was misblamed on shoreline `screenSize` — glsl_program::apply inherited the queued error; foam/screenSize were always correct). Hunt with MC2_GL_DEBUG=1.
- All-pilots-dead on the final objective reportedly shows mission-success. Unverified.
