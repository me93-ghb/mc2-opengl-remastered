# Known bugs (macos-port)

Open issues on the `macos-port` branch. One line each; fix and delete the line when done.

## UI / render
- Thin vertical line down the far-right edge of the mission screen. Pre-existing, cause unknown.
- Mission-results banner: a few-pixel gap between the black box and the hazard-border frame (left/top). Cosmetic, ~matches retail. See MACOS-PORT-16.
- Promotion screen intermittent static-prop pop-in. gos_static_prop_registry.cpp. (May share the MACOS-PORT-27 cause — verify on next promotion screen.)
- Fast camera scrolls dip framerate a bit since MACOS-PORT-27's whole-map object admission (every prop feeds update/touch + GPU cull each frame). Upgrade path: frustum-true block test in the OBJ-CULL-BYPASS site (mclib/terrain.cpp).

## Terrain / assets (data gaps, not code bugs)
- No PBR normal detail on terrain/buildings: mat0_normal.tga..mat8_normal.tga are not built by the macOS data pipeline. Flat lighting only.
- NIGHT LIGHTING EPIC: cement pads, roads/decal overlays, and static props (buildings/walls) all render at daylight brightness — none of those paths take the mission's night lighting. A cement-only colormap burn-in was tried and REVERTED (the colormap is not a reliable tint source: day pads darkened wrong, roads/overlays untouched, patchwork). Right shape: one mission-light term (ambient/sun colour uniform) applied consistently across terrain_lod_chunk cement, terrain_overlay, and the static-prop shader.
- Spotlight illumination fully invisible (user: "I see nothing"): the retail beam cone was DELIBERATELY suppressed upstream (T3.1, unconditional skip of SpotLight_ children in gos_static_prop_batcher submitMultiShape; flag bit 2 recycled to alphaTest) in favour of "real" TG_Light pool lights (BldgAppearance::update ~3260) — but those are classic world lights, and the terrain LOD-chunk shader has NO dynamic world-light term, so the ground pool has nowhere to render. Part of the night-lighting epic: either add a world-light term to the terrain shader, or restore the beam cone under a new flag bit (night-gated, additive). [SPOT_DIAG v1] first_register lines verify the registration side fires.
- Spotlight buildings don't animate (no sweep, no light-cone effect) and turrets (autocannon etc.) don't rotate. Suspected: animated buildings frozen into the static-prop registration (pose baked at registerStatic; markVisible replays the baked matrix). Check isStaticEligible() coverage for animated/spinMe/activity types; also verify whether MACOS-PORT-27's whole-map admission changed this or it predates.

## Non-fatal / to verify
- INTERMITTENT: Razorback (chassis 'werewolf') vanished from purchase lists (missions 2 AND 5) within one long session — all other chassis fine — and a fresh boot with the same save healed it. Data and code verified clean (purchase02/05 both list werewolf; fresh-campaign harness and fresh save-load both show RAZORBACK; MC2_LOG_LOGISTICS=1 baseline: purchase05 newMechAvailableCount=6). Suspect in-memory variant/availability state corrupted by session history (multiple save loads / mission cycles / mech lab). Note: LogisticsData::loadVariant creates custom variants as `new LogisticsVariant(pChassis, 0)` — possible ID collision with the stock variant 0. If it recurs: MC2_LOG_LOGISTICS=1 and grab updateAvailability + [MECHLIST] lines before restarting.
- Some GL call raises GL_INVALID_OPERATION during mission frames; source unknown (was misblamed on shoreline `screenSize` — glsl_program::apply inherited the queued error; foam/screenSize were always correct). Hunt with MC2_GL_DEBUG=1.
- All-pilots-dead on the final objective reportedly shows mission-success. Unverified.
