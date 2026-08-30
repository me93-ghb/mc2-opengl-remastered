# Known bugs (macos-port)

Open issues on the `macos-port` branch. One line each; fix and delete the line when done.

## UI / render
- Thin vertical line down the far-right edge of the mission screen. Pre-existing, cause unknown.
- Mission-results banner: a few-pixel gap between the black box and the hazard-border frame (left/top). Cosmetic, ~matches retail. See MACOS-PORT-16.
- Promotion screen intermittent static-prop pop-in. gos_static_prop_registry.cpp. (May share the MACOS-PORT-27 cause — verify on next promotion screen.)
- Fast camera scrolls dip framerate a bit since MACOS-PORT-27's whole-map object admission (every prop feeds update/touch + GPU cull each frame). Upgrade path: frustum-true block test in the OBJ-CULL-BYPASS site (mclib/terrain.cpp).

## Terrain / assets (data gaps, not code bugs)
- No PBR normal detail on terrain/buildings: mat0_normal.tga..mat8_normal.tga are not built by the macOS data pipeline. Flat lighting only.
- HDRI sky fails to load: [HDRI_SKY] exr_load_failed.

## Non-fatal / to verify
- Some GL call raises GL_INVALID_OPERATION during mission frames; source unknown (was misblamed on shoreline `screenSize` — glsl_program::apply inherited the queued error; foam/screenSize were always correct). Hunt with MC2_GL_DEBUG=1.
- All-pilots-dead on the final objective reportedly shows mission-success. Unverified.
