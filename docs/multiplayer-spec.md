# Multiplayer on ENet: spec

Written 2026-09-13 from the /spec interrogation; decisions recorded inline.
Status: MP-1 and MP-2 done 2026-09-13 (headless runner passes lobby + launch: both instances load WetWork with identical roster hash and seed and stay in mission); MP-4 runner covers lobby and launch; MP-3 in-mission sync next.

## Context

The macOS port carries the retail multiplayer UI and game hooks, but Microsoft shipped `MultiPlayer` with the bodies stripped: the original `multplyr.cpp` has 123 empty methods and 4 real ones, and the port's copy has 112 empty and 15 real (ENet transport, chat, commander-ID assignment).
The lobby, launch and in-mission screens all call into those stubs, so today two copies of the game can connect and chat and nothing else.
Goal: two humans play a full Elimination match on a stock multiplayer map, host-authoritative like retail, and a headless two-instance harness proves it on one machine.

## Current State (verified 2026-09-13, branch macos-port)

| Piece | State | Evidence |
|---|---|---|
| Transport | Works, tested game-free | `code/mptransport.{h,cpp}` (ENet, reliable ch0 / unreliable ch1), `tests/mp/mptransport_check.cpp`, target `mp_transport_check` |
| Session host/join/close | Works | `multplyr.cpp:489,549,586`; browser synthesises one "Direct" entry from `MC2_MP_CONNECT` |
| Commander-ID assignment | Works | `handlePlayerCID` `multplyr.cpp:1124`; host owns slot 0 |
| Chat | Works | `sendChat`/`handleChat` `multplyr.cpp:947,1109` |
| Message dispatch | 2 of 40 types handled | `processMessages` `multplyr.cpp:1308` switch: CHAT, PLAYER_CID, default logs "unhandled" |
| Wire structs | All defined, none sent | `MCMSG_*` classes `multplyr.h:589-1000`, ids `multplyr.h:160-201` |
| Lobby screen | Calls stubs | `mpparameterscreen.cpp` (2415 lines): `sendPlayerUpdate` x9, `redistributeRP` x5, `setInProgress` x3, reads `missionSettings` every frame |
| Launch flow | Calls stubs | `logistics.cpp:523-660`: `calcDropZones`, `sendMissionSetup`, `waitTillStartLoading`, `commandersToLoad` |
| Mission load MP | Exists, never run | `mission.cpp:4095-4130` quickstart remap; roster hooks `mission.cpp:1688,1748,1936,1974,2165` set `CONTROL_NET` on clients |
| In-mission hooks | Exist, land in stubs | `MPlayer->isServer` gates: mech.cpp 21, gvehicl.cpp 16, missiongui 8, turret 7; `sendPlayerOrder` x9, `addWeaponHitChunk` x9, `addMineChunk` x3, `sendReinforcement` x5 |
| Per-frame update | Only polls transport | `MultiPlayer::update` `multplyr.cpp:440` |
| Menu gate | Off by default | `mainmenu.cpp:489` `MC2_MP_ENABLE` |
| Two-instance test | Never run | TESTING_CHEATS.md "not yet run, build-verified only" |
| MP maps | 12 present, `multi.csv` lists 11 | all carry 12 quickstart parts tagged commander 0; `mc2_m01` WetWork = Elimination, 8 slots |

## Proposed Change

Fill the stubs in four slices, each a child issue, in dependency order.
Host-authoritative: the host simulates everything; clients send orders and apply host updates to `CONTROL_NET` movers.
Wire format: one `MCMSG_*` struct per ENet packet, byte 0 is the type, structs sent as raw bytes (both peers must be the same build; the lobby version stamp enforces it).
Reliable channel for everything except `MOVER_UPDATE` and `TURRET_UPDATE`, which go unreliable-sequenced.
Host random seed goes to clients in `MCMSG_MissionSetup.randomSeed` and feeds the existing `MC2_RNG_SEED` hook so both sides seed identically.

### Child Issues

| # | Title | Priority | Effort (human / CC) | Depends on |
|---|---|---|---|---|
| MP-1 | Lobby sync: players, settings, ready, version | Critical | ~3 d / ~1 h | none |
| MP-2 | Launch from lobby: mission setup, load handshake, start | Critical | ~3 d / ~1.5 h | MP-1 |
| MP-3 | In-mission sync: orders, mover/weapon/world updates, end mission | Critical | ~8 d / ~4 h | MP-2 |
| MP-4 | Headless two-instance harness + enable MP button by default | High | ~2 d / ~1 h | MP-2 (chat/launch), full value after MP-3 |
| MP-5 | Browser host:port entry field | Low | ~0.5 d / ~15 min | MP-1 |

### Dependency Graph

```
MP-1 lobby ──> MP-2 launch ──> MP-3 in-mission sync ──> MP-4 harness (full)
                  └──────────> MP-4 harness (host/join/launch only)
MP-5 host:port field (independent, after MP-1)
```

Sequencing: nothing in-mission is testable until two clients load the same mission with the same rosters (MP-2), and MP-2 is meaningless until both sides agree on settings and slots (MP-1).
MP-4 is built as soon as MP-2 lands so MP-3 develops against the harness instead of two windows.

### MP-1 Lobby sync

Implement in `code/multplyr.cpp`, dispatch cases in `processMessages`:

- `sendPlayerUpdate(stage, subType, cid)` / `handlePlayerUpdate`: payload `MCMSG_PlayerUpdate` (`MC2Player info`, `versionStamp[15]` = first 12 chars of build SHA from `BUILD_FINGERPRINT`, `sessionIPAddress`). Client sends on join and on any change to name, color, team, ready. Host merges into `playerInfo[cid]`, then rebroadcasts the full `playerInfo` row to all. Host refuses (boots) a peer whose stamp differs; `getVersionStatus` returns BAD on mismatch so the screen's existing dialog fires.
- `sendMissionSettingsUpdate` / `handleMissionSettingsUpdate`: host sends `MCMSG_MissionSettingsUpdate` to all after every settings edit and on every new join; clients overwrite `missionSettings`.
- `sendPlayerInfo` / `handlePlayerInfo`, `sendPlayerCheckIn` / `handlePlayerCheckIn`, `allPlayersCheckedIn`, `allPlayersReady`, `getPlayers`: real implementations over `playerInfo[]`.
- `setPlayerTeam`, `setPlayerBaseColor`, `setNextFreeColor`, `setLocked`, `setInProgress`, `redistributeRP`: local state plus a `sendPlayerUpdate`/`sendMissionSettingsUpdate` where the host is authoritative.
- `sendLeaveSession` / `handleLeaveSession` / `handlePlayerLeftSession`: clean slot release; host disconnect sets `hostLeft` so the existing dialog path in `logistics.cpp:440` runs.

Acceptance:
1.
Two headless instances: client shows host's session name, both player names and the host's settings within 1 s of joining (log line `[MP] settings applied` on client).
2.
Host changes any of drop weight, C-bills, time limit, RP, or a toggle; client's `missionSettings` matches within 1 s (harness compares log dumps).
3.
Client toggles ready; host log shows `[MP] player 1 ready=1`.
4.
A client built from a different SHA is refused with `VERSION_STATUS_BAD` and the lobby dialog text.
5.
Client leaves: host frees the slot and a new join gets it.
Host leaves: client returns to main menu via the host-left dialog.

### MP-2 Launch from lobby

- `sendMissionSetup(0,0,NULL)` (host, `logistics.cpp:590`) sends `MCMSG_MissionSetup` with `randomSeed`, `commandersToLoad[8][3]`, quickstart flag; `handleMissionSetup` stores them and sets `startLoading`.
- `waitTillStartLoading`, `waitTillMissionLoaded`, `waitTillMissionSetup`, `playersReadyToLoad`: real handshakes over `MCMSG_PlayerCheckIn` stages (`readyToLoad[]`, `missionDataLoaded[]`, `missionFullySetup[]`); host waits for all, then `sendStartMission` (`MCMSG_StartMission`); `handleStartMission` sets `startMission`.
- Seed: `Mission::init` uses `MPlayer->randomSeed` when `MPlayer` is set, else the existing hash/env path.
- Roster: `addToPlayerMoverRoster`, `addToLocalMovers`, `addToTurretRoster`, `buildMoverRosterRLE`, `removeFromMoverRoster`: fill `moverRoster[]`, `playerMoverRoster[cid][]`, `localMovers[]`; both sides log `[MP] roster hash=<crc of watchIDs in roster order>`.
- Validate the quickstart remap in `mission.cpp:4095-4130` with 2 commanders on `mc2_m01` (12 template parts, `numMoversPerCommander` from settings).

Acceptance:
1.
Host presses Launch; both instances reach `phase=mission_ready` on `mc2_m01` within 30 s of each other.
2.
Roster hash log line identical on both sides.
3.
Each side owns exactly its own lance: `[MP] local movers=N` equals the quickstart lance size on both, and client movers are `CONTROL_NET`.
4.
Both sides log the same `randomSeed`.

### MP-3 In-mission sync

Client to host, reliable: `sendPlayerOrder` (`MCMSG_PlayerOrder`, tac order chunk already produced by `missiongui.cpp`), `sendHoldPosition`, `sendPlayerMoverGroup`, `sendPlayerArtillery`, `sendReinforcement`.
Host applies them through the existing `TacticalOrder` path for the owning commander.

Host to clients, unreliable-sequenced at 10 Hz (`initUpdateFrequencies`): `sendMoverUpdate` (`MCMSG_MoverUpdate`: RLE over `moverRoster`, per mover cell/heading/velocity/damage summary, `updateId` monotonic; clients drop stale ids), `sendTurretUpdate`.

Host to clients, reliable: `sendMoverWeaponFireUpdate`, `sendWeaponHitUpdate` (drains `addWeaponHitChunk` queue), `sendMoverCriticalUpdate`, `sendWorldUpdate` (drains `addWorldChunk`, `addMineChunk`, `addCaptureBuildingChunk`, `addArtilleryChunk`, `addLightOnFireChunk`), `sendEndMission` (`MCMSG_EndMission` with team and player scores; `calcMissionStatus` decides Elimination win = all enemy movers destroyed, mirrors `allUnitsDestroyed[]`).

Clients: `handleMoverUpdate` moves `CONTROL_NET` movers toward the host position (snap if > 2 cells off, else lerp over the update interval), applies damage; weapon-fire and hit handlers play the existing effects through the same code the host uses locally; `handleEndMission` sets scores and triggers the results screen.

`MultiPlayer::update` gains the send scheduler: host drains queues every frame and sends mover updates on the 10 Hz tick; client sends orders as they are issued.

Acceptance:
1.
Two headless instances play `mc2_m01` Elimination to completion with scripted orders (harness issues attack-move orders every 20 s); both sides show the results screen with the same winner and same kill counts.
2.
Position metric: each side logs `[MP_POS] t cid roster_idx cellRow cellCol` once per second; ≥95% of paired samples within one cell.
Logged and reported from day one, blocking only after MP-3 is complete.
3.
A client dropping mid-match: host continues, dropped lance idles under host AI, host reaches results.
4.
Host dropping: client returns to main menu through the host-left dialog within 5 s.
5.
No `[MP] unhandled msg type` lines in either log during a full match.

### MP-4 Headless harness + default on

- Env hooks in the style of `MC2_BOOT_TO_BAY` (`mainmenu.cpp:434`): `MC2_MP_AUTOHOST=1` presses Multiplayer, Host, then fills `MPlayer->missionSettings.map` from `MC2_MP_MAP` (default `mc2_m01`); `MC2_MP_AUTOJOIN=host:port` presses Multiplayer, opens the browser, joins the Direct entry and sets ready; `MC2_MP_AUTOLAUNCH=<n>` makes the host press Launch once n players are ready. `MC2_MP_SCRIPT_ORDERS=1` issues an attack-move toward the enemy drop zone every 20 s.
- `tests/mp/run_mp_smoke.py`: starts host then client via `dev/macos-run.sh` (no `MC2_MACOS_WINDOW`, `MC2_LOG=1`, distinct `MC2_MP_PORT`), waits for `[MP] hosting`, `[MP] peer ... joined`, `[MP] settings applied`, `mission_ready` on both, runs `--duration` seconds or until results, kills both, parses the logs for the MP-1 to MP-3 acceptance lines and the position metric, exits 0 or 1 with a one-line verdict.
- When the runner passes end to end, flip `mainmenu.cpp:489` so the Multiplayer button is enabled by default; `MC2_MP_ENABLE=0` disables.

Acceptance:
1.
`python3 tests/mp/run_mp_smoke.py --duration 120` exits 0 on this Mac with no window.
2.
The runner exits 1 with the failing check named when any acceptance line is missing (mutation test: comment out `handleStartMission`).
3.
Multiplayer button enabled by default after the flip; `MC2_MP_ENABLE=0` hides it.

### MP-5 Browser host:port field

Text edit in `mpgamebrowser.cpp` next to the session list; value overrides `MC2_MP_CONNECT`.
Acceptance: typing `127.0.0.1:27500` and Join reaches `joinSession` without the env var.

## Testing Plan

| Layer | What | Count |
|---|---|---|
| Unit (game-free) | `tests/mp/mpmsg_check.cpp`: round-trip every `MCMSG_*` struct through the transport on loopback, sizes and type bytes intact; RLE encode/decode of a 24-mover roster | +2 |
| Integration | `run_mp_smoke.py` stages: join+settings (MP-1), launch+roster hash (MP-2), full match (MP-3) | +3 |
| E2E | Two windowed instances on this Mac played by hand once per slice | +1 per slice |

## Rollback Plan

Every slice is behind `MC2_MP_ENABLE`; the default-on flip is the last change in MP-4 and is one line.
Reverting a slice's commit restores the previous stub behavior since no single-player path is touched except the seed hook in `Mission::init`, which is guarded by `MPlayer != NULL`.

## Effort Estimate

MP-1 ~3 d (settings 1, players/ready 1, leave/version 1).
MP-2 ~3 d (setup/handshake 1.5, roster/seed 1, quickstart validation 0.5).
MP-3 ~8 d (orders 1.5, mover update+RLE+client apply 3, weapons/world 2, end mission 1, dropouts 0.5).
MP-4 ~2 d (hooks 1, runner 1).
MP-5 ~0.5 d.
CC estimates in the table.

## Files Reference

| File | Change |
|---|---|
| `code/multplyr.cpp` | fill ~60 of the 112 stubs listed per slice; dispatch cases in `processMessages`; scheduler in `update` |
| `code/multplyr.h` | no wire changes; add `randomSeed` accessor, update-frequency fields if missing |
| `code/mptransport.{h,cpp}` | `send` gains unreliable-sequenced; no other change |
| `code/mission.cpp:2338` | use `MPlayer->randomSeed` when in multiplayer |
| `code/mission.cpp:4095-4130` | validate/fix quickstart remap for 2 commanders |
| `code/logistics.cpp:523-660` | unchanged unless handshake needs a wait state |
| `code/mainmenu.cpp:434,489` | auto-host/join/launch hooks; default-on flip |
| `code/mpgamebrowser.cpp` | auto-join; MP-5 text field |
| `code/mpparameterscreen.cpp` | auto-ready/launch hooks only |
| `tests/mp/mpmsg_check.cpp`, `tests/mp/run_mp_smoke.py` | new |
| `CMakeLists.txt` | `mp_msg_check` target next to `mp_transport_check` |
| `TESTING_CHEATS.md` | replace "not yet run" section with the runner command |

## Out of Scope

- Retail and GameRanger cross-play (DirectPlay 8 wire protocol): separate epic, door kept open by sending retail structs unchanged.
- NAT punch-through, relay, master server, LAN discovery: host forwards UDP 27500, joiner uses direct IP.
- Lobby mech purchasing and insignia transfer (`sendPlayerInsignia`, `MCMSG_DeployForce`): follow-up after MP-3.
- Host migration (`sendNewServer`, `switchServers`).
- Game modes other than Elimination; more than 2 players tested (code is written for N).

## Definition of Done

1.
`run_mp_smoke.py --duration 120` exits 0 on macOS headless: join, settings, launch, full match, same winner both sides, position metric ≥95%.
2.
Two windowed copies played by hand through one full match on WetWork.
3.
Multiplayer button on by default; `MC2_MP_ENABLE=0` hides it.
4.
No `[MP] unhandled msg type` in either log.
