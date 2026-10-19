# M2b design: Metro TDM with bots

Status: design for implementation, revision 3 (after the second review), 2026-10-19. M2b is the plan's next row after M2a; M3
(ENet, many clients, the logic ghost) follows it and §3.8 lists what M3 takes from here. Scope: milestone M2b of the plan
([research/PORTING-RESEARCH.md](../../research/PORTING-RESEARCH.md) §6, row "M2b", 6–9 slices: 3p soldiers,
health / regen / death / respawn, kill feed, scoreboard, kill card, spawn screen; TDM through the S4 interpreter;
bots B2–B4; audio A3). The goal is a game a person can play: one human and 31 bots, 16 v 16 on Metro TDM, a round to
100 kills, a spawn screen, a kill feed, a scoreboard and a round end, with bots that look for, fight and revive.
It writes no code: the headers below are sketches, and each wave's first commit (the contract commit, §11) puts
them in the tree with a compile test.

**Starting point.** `main` at 47871fa (the M2a gate pass, docs/design/m2a-gate.md: every row passed). `tools\play.cmd`
already gives a playable firefight (24 Free-mode bots by default, `--bots` up to 32, `--respawn`, `--kit`, `--team`).

Tags: **[verified]** = read from the code at 47871fa or measured with `bf3dump` against the retail install on the date
above. **[plan]** = the plan or a merged design doc, with its section. **[inferred]** = a choice made here; every
mechanic whose formula needs a measurement ships a named default with `measured = false` (no VU work, the S5 VU
sessions are parked).

Constraints that hold throughout: the retail install is read as-is (never written, never run or hooked); no BF3 data
in git; caches only under the user cache root; never build in the main checkout. Clean room: no third-party BF3 tool
or bot code was opened for this design; the IW4L crates (the user's own Apache-2.0 code) may be ported with
attribution (none is needed here).

**Testing is lean (the user's direction).** Each track runs its own functional rows once on its branch. A wave's
merges are batched: the merge workflow runs **one** merge pass after the wave's last merge, not one per track. The
wave-1 pass runs the `timing` phase only (E8, E11, E13; wave 1 is the only wave that changes timed code) and no
window phase. The wave-2 pass runs the `window` phase only (E9, E12; window rows "never on branches", docs/testing.md
§1) and no timing phase, because wave 2 changes only Godot client code. Then one verifier reads one log per kind once,
then the user's playtest (§11.1). There is no separate gate pass, no acceptance step that re-runs what the verifier
read, and no timing row measured twice. Branch work never runs or waits on a timing row.

---

## 1. Exit criteria and how each one is measured

Plan §6, M2b row: "a TDM round to 100 kills against 16 bots on Metro; a bots-only server produces kills; team K/D ≈ 1;
64 bots firing keeps the audio thread under 10 % of a core with no xruns." The rows below run **the real game code**
on Metro TDM. The headless `host.tdm.*` rows step the real `GameServer` back to back on the test thread, the way M2a's
`run_firefight` does (firefight_run.h:9–13: no ServerHost, no sockets, no wall-clock wait), with a **datagram-level
scripted peer** (`tests/tdm_fixture.h`, #21) that sends Hello, Ready, SpawnRequest and CommandPackets through
`GameServer::on_receive` and decodes the server's `OutDatagram`s (MatchState, PlayerList, Spawned, CombatEvents,
snapshots). That makes them deterministic and fast. The real threaded `ServerHost` + `ClientSession` path runs in one
short `timing` row (E13; ClientSession owns ENet, a client thread and a wall-clock time sync, client_session.h:318–330,
so it cannot be stepped). The Godot rows use the real input mapping (`godot.m2b*`). Labels follow docs/testing.md
§1: none = functional, every branch build at `-j16`; `timing` and `window` = the merge pass on main only.

| id | criterion | threshold | measurement (row, label) |
|---|---|---|---|
| E1 | A TDM round to 100 kills through the interpreter, 1 scripted peer + 31 bots (16 v 16) | `RoundOver` fires once with the team whose `KillCounter.Kills` (the runtime's output port) reached 100; the `MatchState` the peer decodes shows `RoundOver`, the same winner and the same two counts; the runtime never faults; every native delivery to a TDM type of §3.1 (KillCounter, PreRound, RoundOver, HumanPlayer, AutoTeam, InputRestriction, LevelControl, CharacterSpawnReference, AlternateSpawn) is handled by `RoundNatives`, 0 of them `Recorded` (`RoundNatives::coverage()`, §3.1); the round ends within 21,600 ticks (6 simulated minutes, a hard stop); after the 60 s round end the round restarts in place with both counts 0 | `host.tdm.round` (none; `--preround=0`; stepped; COST ≈ 25 s, see the estimate below the table) |
| E2 | Bots-only server produces kills, team K/D ≈ 1, no seeing through walls, no stuck bots | 32 bots, seeds {1, 2, 3}, kill limit 50, one round each, the three in one process: every round reaches `RoundOver`; **pooled** team K/D (team-1 kills / team-2 kills over the 3 rounds) in [0.75, 1.33] (the per-seed values are printed, not gated); 0 sight detections of a target with no clear eye-to-head or eye-to-chest ray at that tick, and 0 aimed shots at a target whose last clear ray is more than 30 ticks old (blind fire at a last-known position is excluded and counted apart); 0 stuck bots (M2a's firefight metric: moved < 1 m in a 20 s window; freeze, regen-wait, revive-wait and revive ticks left out); 0 scored spawns (§3.5, the M2a fallback excluded and counted apart) with an enemy within 10 m; revives ≥ 1 over the 3 rounds | `host.tdm.bots` (none; `--preround=0`; stepped; the shared level loaded once, the 3 seeds in `parallel_map`, PROCESSORS 3, COST ≈ 15 s) |
| E3 | Health, regen and spawn immunity | Damage at tick T: no regen before T + 600 (10 s), then +5 hp/s (1/12 hp per tick) to 100, ± 1 tick; a hit during regen restarts the delay; spawn immunity 120 ticks (2 s) from `Spawned`; any input of `SoldierDef::abort_spawn_immortality_inputs` that §3.3 maps onto a command bit ends it (the case presses Fire in one run and moves in another; both end it on the server, and a `sim::SoldierMotor` over the client's `MotorParams` given the same commands gives the same `protect_ticks`, which is what the client predicts with) | `host.tdm.server`, cases `health*` (none; scripted damage through the peer's dev `Debug` requests) |
| E4 | Death and respawn flow | `DeathEvent.respawn_tick` = death + 300 (PlayerKilledDelay 5 s); a deploy-screen peer is **not** spawned by its Ready (R10) and spawns only after an accepted `SpawnRequest`; a `SpawnRequest` before `respawn_tick` is refused, at or after it is accepted and `Spawned` follows within 1 tick with the requested kit; bots respawn by themselves with the rules' kit; with rules off (`mode = None`, no `SharedLoadOptions::tdm`) the M2a rows pass unchanged in the same dev pass, and the case checks that such a load gives M2a's `kit_table_hash`, `weapon_table_hash` and `motor_params_hash` (constants recorded in the case from main 47871fa) | `host.tdm.server`, cases `respawn*` (none) |
| E5 | Revive | A downed teammate whose corpse a teammate's defibrillator shot reaches (§3.6) inside its revive window comes back at the corpse with 20 hp as a new life, `Spawned.revived = 1`; this holds for a bot and for the **peer's** slot (a test-local scripted assault teammate fires the defib at the corpse; the peer decodes the `Spawned`); the enemy team's count drops by 1 (§3.6, default); the reviver scores 100; after the window, after the victim deployed, from an enemy, or at a living soldier, nothing happens; a revived bot is not respawned again at its old `respawn_at` | `host.tdm.server`, cases `revive*` (none) |
| E6 | Determinism | Two stepped runs of the same seed, 32 bots + the scripted peer, kill limit 20, to `RoundOver` + 600 ticks: logic trace hash equal, per-tick world hash equal (run_firefight's hash: every slot's `soldier_state_hash` and `soldier_weapons_hash`, the projectiles, the tick's damage and detonations; firefight_run.h:24–26) and the decoded MatchState / PlayerList bytes equal, 0 differing ticks; a third run with another seed differs. Snapshot bytes are not compared: their `echo_hold_100us` is wall-clock (game_server.cpp:551) | `host.tdm.replay` (none; its own 3 short runs in `parallel_map`, COST ≈ 10 s) |
| E7 | Hostile input | The protocol fuzz extended to the v3 messages: 0 crashes, and 0 asan reports in the asan run of ru's branch acceptance (it runs because ru touches src/bf3_sim, docs/testing.md §8; ru is accepted after nt is on main, so the new fuzz cases are in it); the server ignores and counts: a SpawnRequest while alive, before `respawn_tick`, with a kit out of range, twice in a tick, in `RoundOver`, for a spectator slot ≥ 64; a v2 client is refused with `Kick` | `net.proto.fuzz` (existing row, new cases), `host.tdm.server` cases `hostile*` (none) |
| E8 | Perception and bot budget | 64 bots TDM (with `RoundNativeConfig::max_players = 64`; the default 32 is Metro TDM's cap, and AutoTeam's TamFullTeams would leave half the bots out), 120 s simulated: perception ≤ 0.2 ms/tick p99; all bot brains ≤ 1.5 ms/tick p99; the server tick ≤ 4 ms p99 | `host.tdm.perf` (`timing`, the wave-1 merge pass) |
| E9 | Client screens fed from the game | Scripted Godot play, 90 s, `--m2b-check=play --kill-limit=15 --preround=0` (the player stands in the open, so bots find it): the spawn screen appears at join and after each own death (≥ 1), and Deploy spawns the chosen kit; kill-feed lines = deaths received; the scoreboard's kills per team = `MatchState`; the kill card names the killer and the weapon; the round-end screen appears on `RoundOver`; 0 missing font, icon or string lookups; 0 ERROR lines | `godot.m2b.play` (`window`, the wave-2 merge pass; COST ≈ 110 s with the load) |
| E10 | HDR window oracle (plan §7.7) | Only a 71 dB footstep playing: window top 80, bottom 35 (± 0.5 dB); a 110 dB shot starts: the top reaches 110 within 0.05 s (± one 512-frame block), the bottom 65, and a 63 dB source is suspended; after the shot the top releases on the setting's spline; the scope rows of §6.2 (Bullet_Impact keeps the closest 5 within 1 m / 0.4 s; Death_Scream at most 4) | `audio.hdr.unit` (none, offline, synthetic sources through `MixerCore`) |
| E11 | 64 bots firing: audio thread | M2a au3's "64 shooters for 60 s" render (audio_router_tests.cpp:1990: Metro's kit patches, the real router, impacts) through the A3 mixer: mix CPU per block / block duration ≤ 0.10 at p99 (the `MixerStats` profiler), 0 late blocks (the existing `OPENBF3_AU3_GATE_LATE` gate), live voices ≤ 128 every block | `audio.game.underruns` (existing `timing` row; au adds the A3 checks; the wave-1 merge pass) |
| E12 | Load | Warm load of `m2b_metro.tscn` ≤ 5 s (the M2a E16 limit; the logic graph adds a measured 0.15 s load + 0.008 s build) | `godot.m2b_load` (`window`, the wave-2 merge pass; the `godot_m1_load.cmake` script on the new scene) |
| E13 | The real client path | Real `ServerHost` + `ClientSession` on Metro TDM, rules on, deploy screen, 7 bots, kill limit 5, pre-round 3 s: no `Spawned` before the first `send_spawn_request`; during the freeze the client predicts the restriction (0 corrections above `teleport_above_m`, 0 predicted shots while restricted); `Spawned` carries the requested kit; after a scripted death the frame's `own_respawn_tick` / `own_revive_until` equal the DeathEvent's; a revive by a scripted assault bot gives the client a new life (`Spawned.revived`); `MatchState` reaches `RoundOver`; ≤ 30 s | `host.tdm.client` (`timing`: real time and sockets, docs/testing.md §1; the wave-1 merge pass) |

**Row cost estimates** (the basis of the COSTs; each track replaces them with its measured COST). Every
functional `host.tdm.*` row is stepped (the GameServer ticks back to back, as `run_firefight` does), so wall time is
CPU time, not simulated time. M2a measured
64 Free bots at 1.909 ms p99 per server tick and E13's 64 Pairs bots at 7,200 ticks in 4.59 s wall (0.64 ms per tick);
32 soldiers with perception: ≈ 0.5 ms per tick [inferred]. M2a's 64 Free bots made 3.0–3.6 kills/s; 32 bots on the same
map ≈ 1–1.5 kills/s [inferred], so a round to 100 (≈ 190 kills with K/D ≈ 1, plus the kills revives undo) takes
≈ 130–200 simulated seconds = 8,000–12,000 ticks ≈ 4–6 s wall, plus 3,600 ticks of round end (≈ 2 s), plus the shared
level load (≈ 5–10 s with the gunplay tables). E1 ≈ 25 s; its 21,600-tick hard stop bounds it at ≈ 11 s of
simulation, so it cannot cross the 60 s cap even at 2× under `-j16`. E2's three rounds to 50 run in parallel (≈ 3 s
each). A round that hits the hard stop fails the row as a bot behaviour bug (bots that do not fight), not as a budget
issue. `host.tdm.server` (E3–E5, E7's server half) loads the shared level once per process and runs its short cases on
fresh GameServers over it (docs/testing.md §4): ≈ 10 s load + ≈ 5 s of cases, COST ≈ 15 s. E13 is the only row on
the real-time path; it is short by construction (kill limit 5, 7 bots, a 3 s pre-round) and bounded at 30 s.

Bot behaviour targets of research/topics/06-bots.md §5 that are not rows above (the TTK-vs-skill table, ≤ 1 redeploy
per bot-hour) are not built in M2b; the playtest judges the bots.

### 1.1 Playtest script for the user (after the verifier, ≈ 15 minutes)

1. `tools\play.cmd` (new default: Metro TDM, 31 bots, spawn screen on). The level loads to the spawn screen with the
   pre-round countdown (10 s) behind it.
2. Pick a kit on the spawn screen (assault, engineer, support, recon) and press Deploy. You spawn at a random TDM point
   with no enemy in sight; you cannot move or fire until the countdown ends.
3. Fight. Check: bots turn towards gunfire, do not see through walls, miss more at range and on the first shots, strafe
   and push towards fights; your health regenerates 10 s after the last hit.
4. Die once. Check: the kill card (killer, weapon, distance, the killer's health) while the camera looks from your
   body towards the killer for about 4 s, then the spawn screen with a timer (5 s) and "revive possible" while a medic can still reach you. Deploy again
   with another kit.
5. Hold Tab: the scoreboard (two teams, kills, deaths, score, the bots marked), the team counts at the top.
6. Play assault: the defibrillator is on key 3 (weapon slot 2, a new binding). Revive a downed teammate (aim at the body within 1.8 m and
   fire); watch a bot revive one, or get revived yourself while the spawn screen says "revive possible".
7. Let the round end (or start with `tools\play.cmd --kill-limit=20`): the round-end screen with the winner, then the
   round restarts in place after 60 s.
8. The pre-round countdown runs before every round (J2); start with `--preround=0` if it gets in the way.
9. Report anything that feels wrong; the named defaults of §13 are the knobs (`config/tdm_tuning.cfg`, `config/bot_tuning.cfg`).

---

## 2. The as-is: seams M2b builds on

### 2.1 Server (bf3_host)

| seam | where | what M2b does with it |
|---|---|---|
| `CommandSource` | src/bf3_host/include/bf3/host/game_server.h:75–79 | the TDM bot brain is one, as FirefightBrain is |
| `GameServerConfig` | game_server.h:81–103; `respawn_ticks = 60` at :88; `bots` (BotFillConfig, `spawn_in_main`) :96; `firefight` :102 | gains `rules` and `tdm_bots` (§2.6 #1); `respawn_ticks` stays M2a's fallback |
| `GameServer::init` order | game_server.cpp:768–813 (`init` ends at :813): `sv_init()` at :792, S1's bots :799–810, `add_firefight_bots(*this, p_config)` at :811 | `[tdm bots]` after :811: `add_tdm_bots(*this, p_config)` when `config.tdm_bots` is set; it runs after `sv_init` created the rules, so `[add]`'s `on_connect` has them. ServerHost exposes no GameServer after init (server_host.h:23–35, the `game` config at :35), so a config member is the only way the Godot sessions can add TDM bots |
| `GameServer::on_connect` / `on_receive` / `tick(out)` | game_server.h:176–181 | the stepped fixture (#21) drives these directly, as `run_firefight` does (firefight_run.cpp:305, :369) |
| `GameServer::add_local_soldier` / `place_soldier` / `view()` / `gunplay()` | game_server.h:185, :196, :199, :201 | TDM bots are added through `add_local_soldier` (team 0 = AutoTeam's choice, `[add]`); `place_soldier` is **not** used for revive: `sv_place` refuses a client's slot (game_server.cpp:1167–1168; `place_soldier` itself at :1520) and never sends `Spawned` |
| no hook for a custom `RespawnPolicy` or `CombatEventSink` | game_server.h (absent) | the missing interface #1 |
| `Impl::Slot` (team, want_spawn, respawn_at, spawn_index, state, source) | src/bf3_host/src/game_server.cpp:88–122 | per-slot rules state lives in the rules, keyed by slot, not in `Slot` |
| `add_local` (sets `s.team = p_team`, then `spawn` at once) | game_server.cpp:721–736 | `[add]`: with rules, the team comes from `TdmRules::on_connect` before the spawn |
| `spawn()` with `sim::choose_spawn`; sends `net::Spawned` to a remote slot | game_server.cpp:224–270: the point list is BotLevel's main-component list for a bot slot with `spawn_in_main`, else `config.level->spawns` (:227–230); the call at :236; the send at :258–263 | R1 passes **that same list** to the rules' chooser, so the returned index always refers to the list `spawn()` uses; the revive path copies the send |
| Ready handler | game_server.cpp:859–861: `if (!s.spawned) s.want_spawn = true;` | R10: with rules and `deploy_screen`, Ready does not set `want_spawn` (the human waits on the spawn screen) |
| `handle_hello` sets `s.team = 1` | game_server.cpp:300, :330 | AutoTeam picks the team (R2) |
| `run_tick` | game_server.cpp:588–700: sources at step 3, the motor loop with `sv_step_soldier` (:661), fall death `respawn_at = n + config.respawn_ticks` (:662–665), `sv_rules(n)` (:672), respawn when `want_spawn && n >= respawn_at` (:673–677), debug, sweep, snapshots | the rules step goes in `sv_rules` (below); the fall-death lines ask the policy (R3) |
| KitSelect dispatch | game_server.cpp:893; `sv_kit_select` :1433 sets `ps.kit` (:1451), team change refused without `dev_commands` :1452–1457 | kit choice moves to `SpawnRequest` with the deploy screen; KitSelect stays for M2a rows and for `--deploy-screen=0` |
| `FixedRespawn`; `Sv::respawn` is a `std::unique_ptr<RespawnPolicy>` | game_server.cpp:978–982, :1052, made in `sv_init` :1071–1073 | stays the default policy; R3 and R8 ask `sv_policy()` (#20: the rules when present, else `*respawn`), so no unique_ptr is aliased |
| `Sink` | game_server.cpp:984–1003: `death` pushes the `DeathEvent` into `combat.deaths` **first** (:988), then asks `sv->respawn->respawn_tick(victim, tick + 1)` and sets `want_spawn` / `respawn_at` (:994–999); it runs inside `apply_rules`, which works on `sv->tick_states` (:1143–1144) | R8 reorders it: the policy first, then the stamped event is pushed (§3.4); `killer_health` is read from `sv->tick_states[killer]`, the current tick's copy, not from the slots |
| `View`, PerSlot, `sv_init` creating FixedRespawn | game_server.cpp:1004–1033, :1065, :1072 | `sv_init` creates the rules when `config.rules.mode == Tdm` |
| `sv_rules`: `tick_states` built (:1136–1141), `step_projectiles` / `apply_rules` (:1143–1144), copied back into the slots (:1145–1149), then the placements (:1152–1158) | game_server.cpp:1130–1159 | `[rules]` between `apply_rules` and the copy-back (the only writable copy of every state: regen writes health there); `[orders]` after the placements |
| `sv_place`, `sv_on_spawn` (KitSelect's `next_team` :1202–1205, `ps.kit = slot % kKitCount` for local soldiers :1206–1208, then `gunplay->on_spawn(slot, kit, team)` :1209–1210), `sv_spawned` | game_server.cpp:1162, :1196, :1214 | R7: with rules, `s.team = rules->team_of(slot)`, the kit from `rules->kit_of(slot)` when it has one (a remote slot without the deploy screen keeps its KitSelect kit), and `rules->on_spawned(slot, tick)` |
| snapshot fields | game_server.cpp:551: `echo_hold_100us` from `net::monotonic_ns()` | wall-clock, so E6 hashes the world, not the snapshot bytes |
| `sv_match_info` | game_server.cpp:1410 | adds the logic digest and rules flags |
| `CombatEventSink` (hit, death, detonation) | src/bf3_host/include/bf3/host/gunplay.h:141–147 | gains `revive` (default empty body) |
| `RespawnPolicy::respawn_tick(slot, death_tick)` | gunplay.h:150–154 | the rules implement it |
| `GunplayConfig` (friendly_fire false, target_bots, player kit / team, observer) | gunplay.h:95–113 | gains `revive` |
| `GunplayLevel` (defs, materials, `SoldierDef soldier`, capsules, kits, max_health 100) | gunplay.h:158–171 | the rules read `soldier` for every health number |
| `Gunplay` (on_spawn, step_soldier, step_projectiles, apply_rules, clear, `interval_shots()` :243, `kit(slot)` :233) | gunplay.h:202–259; `clear` keeps `next_shot_index` src/bf3_host/src/gunplay.cpp:401–410; `apply_rules` drops damage to a dead victim and to a teammate (gunplay.cpp:621–628); lag_comp.cpp:505 leaves every `MoveMode::Dead` soldier out of the hit targets | the defib's shots are read from `interval_shots()` by the rules; no change in Gunplay or lag_comp (§3.6) |
| kits | src/bf3_host/include/bf3/host/kits.h: kKitCount 4 (:23), `kKitSlots {0,1,6,7}` (:25, gadget slots 2/4/5 skipped), `make_kit_table(teams, kits, defs)` (:50, no options), `kit_table_hash` (:53); built once in `load_gunplay_level` (gunplay.cpp:245–262), which resolves only the kKitSlots unlocks into `level->defs`; the client compares `MatchInfo::kit_hash` (client_session.cpp:1547) | an options overload and a load option (#9): the defib only with rules on |
| `ServerView` | src/bf3_host/include/bf3/host/server_view.h:17–27 (states, team, alive, weapons, world, tick; all pure virtual, read-only); one implementer, `View` game_server.cpp:1004–1033 | gains `downed()`, `phase()`, `input_restricted()`, `kit()` with default bodies (#10) |
| `SharedLevel`, `SharedLoadOptions`, `load_shared_level` | src/bf3_host/include/bf3/host/level_load.h:106–123, :125–137 (`gunplay` :137), :140 | gain the logic graph (#3) and the assault gadget option (#9) |
| FirefightBrain | src/bf3_host/include/bf3/host/firefight_brain.h: `FirefightConfig` :74–102, seeds `match_seed ^ slot` :105, `FirefightBrain` :134–147, `add_firefight_bots` :151; as-built in docs/design/m2a-bt.md | the TDM brain starts from its code (perception, burst fire, hunt) |
| BotSpawns / BotLevel / BotNav | src/bf3_host/include/bf3/host/bot_fill.h:74–81, :83–98 | TDM bots keep S1's main-component spawn list (`spawn_in_main`, the default, bot_fill.h:157); `TdmRules::create` takes `GameServerConfig::bots->level` for the nav |
| `PresentKind`, `PresentEvent` (64 bytes; Death: slot = killer, aux = victim, def, flags kHeadshot, material = zone) | src/bf3_host/include/bf3/host/presentation_events.h:43, :54–71 | appends `Revive` |
| `ClientSessionConfig`, `PresentationFrame`, `ClientSession` (`send_kit_select` :361) | src/bf3_host/include/bf3/host/client_session.h:103, :254–306, :318–369 | gains match state, player rows, `send_spawn_request` |
| `fill_hud` | src/bf3_host/include/bf3/host/hud_feed.h:95 (HudFeedInput), :104, :126 | fills the kill feed, players, spawn and round-end parts |

### 2.2 Net (bf3_net)

| seam | where | M2b |
|---|---|---|
| `kProtocolVersion = 2` | src/bf3_net/include/bf3/net/protocol.h:35 | 3 |
| `MessageType` (… CombatEvents 23, KitSelect 24, Debug 30, DebugReport 31) | protocol.h:54–75 | MatchState 25, PlayerList 26, SpawnRequest 27 |
| `Hello` {protocol, name} | protocol.h:80–83 | name feeds the player row |
| `MatchPhase {Loading, PreRound, InRound, RoundOver}` | protocol.h:85 | used as is |
| `MatchInfo` | protocol.h:87–106 | `logic_digest`, `rules_flags` |
| `Spawned` (slot, spawn_index, state, kit, team) | protocol.h:123–132 | `revived` flag |
| `DeathEvent` {victim, killer, def, zone, shot_index, headshot, time} | protocol.h:437–445 | `respawn_tick`, `revive_until` |
| `CombatEvents` | protocol.h:457 | `revives` array |
| `KitSelect` {kit, team} | protocol.h:473 | unchanged |

### 2.3 Sim and core

| seam | where | M2b |
|---|---|---|
| `SoldierState` (`reserved0`, `reserved1`, health, damage, protect_ticks), MoveMode::Dead 8, kSpawnProtected, kDamaged / kDied / kSpawned | src/bf3_sim/include/bf3/sim/soldier_state.h:53–81 | no layout change: regen is server-side and the client rebases health (as M2a) |
| spawn immunity in the motor: `protect_ticks` counts down (soldier_motor.cpp:262–263), blocks fall damage (:500), is set at spawn (:564–565); `MotorParams::immortal_ticks = 120` (soldier_motor.h:181); no input aborts it today; the client compares `protect_ticks` in `states_match` (client_session.cpp:69) | src/bf3_sim | the abort runs **in the motor** so both ends predict it (#19); `SoldierDef::abort_spawn_immortality_inputs` holds raw EntryInputActionEnum ints (soldier_def.h:108, read at soldier_def.cpp:333–338), no names decoded |
| `choose_spawn(points, team, live, radius, seed)`, comment "TryToSpawnOutOfSight and FriendZone are M2b" | src/bf3_sim/include/bf3/sim/spawn.h:101; `SpawnPoint` :33 | a new scored chooser beside it (#6) |
| `LogicRuntime` (create, watch, start, force_event, force_property, step, effects, networked, output, ghost, trace, `unhandled()` = primitive ports without a handler only, :194–201, faulted) | src/bf3_sim/include/bf3/sim/logic/logic_runtime.h:204–242; options :62; `LogicContext {player, team}` :76; `LogicEffect` :87; `LogicNetworkedEvent` :100; `LogicGhost` :173 | owned by the rules on the server thread |
| `LogicNativeStubs`, `NativePortMeaning` (already has `OnPlayerEnter`, `RoundStarted`, `OnRoundOver`, `Activate`, :55–58), `NativeTimelineEntry` (:85–90) | src/bf3_sim/include/bf3/sim/logic/logic_native.h:43–60, :97–124 ("M2b/M4/M6 replace the stub bodies with the real systems behind the same bindings"); bindings src/bf3_sim/src/logic/logic_native.cpp:70–80 | the TDM natives (#4, #5) |
| `build_logic_graph(resolver, root, options)` | src/bf3_core/include/bf3/core/gameplay/logic_graph.h:301; `LogicGraph` :217, `nodes_of_type` :246; bf3dump's call tools/bf3dump/logic_commands.cpp:160 | called in `load_shared_level` |
| `LogicTypeId` for CharacterSpawn, AlternateSpawn, CombatAreaTrigger, KillCounter, RoundOver, PreRound, AutoTeam, HumanPlayer, FriendZone, LevelControl, InputRestriction | src/bf3_core/include/bf3/core/gameplay/logic_types.h:150–174 | the native layer's types |
| `SoldierDef` health block (max_health … interactive_man_down) | src/bf3_core/include/bf3/core/gameplay/soldier_def.h:93–113 | every health number |
| `KillCounterDef`, `ModeDef`, `read_mode_def`, `ScoringTable` / `read_scoring` | src/bf3_core/include/bf3/core/gameplay/mode_def.h:75, :126, :164, :173–180 | kill limit check, scores |
| WalkerBrain, PathService, BotGoal, WalkerParams, WalkerView | src/bf3_sim/include/bf3/sim/bots/walker_brain.h:43, :52, :76, :107, :119 | bot movement |
| `instant_hit` | src/bf3_sim/include/bf3/sim/weapons/weapon_sim_def.h:83; src/bf3_sim/src/weapons/ballistics.cpp:445 (the knife) | the defibrillator uses it |
| ammunition in `make_weapon_sim_def` | src/bf3_sim/src/weapons/weapon_sim_def.cpp:486–494: `MagazineCapacity < 0` fails the def (:487–489), capacity clamped to ≥ 0 (:490), `unlimited_ammo = NumberOfMagazines < 0` (:494); one failed def fails `make_weapon_table` (:617–620); `has_ammo` (src/bf3_sim/src/weapons/weapon_sim.cpp:51) needs `unlimited_ammo` or a round in the magazine; the hostile-def case expects capacity −1 to fail (tests/weapons_tests.cpp:295) | the defib is `ammo -1 x 1` (MagazineCapacity −1, NumberOfMagazines 1), so **on main it cannot be built** [verified: `bf3dump weapon-sim weapons/gadgets/defibrillator/u_defib` → "MagazineCapacity = -1 is outside [0, 10000]"]; R11 / R12 (§3.6) |
| `motor_params_hash` | src/bf3_sim/src/motor_tuning.cpp:558 (every field hashed), declared soldier_motor.h:195; compared through `MatchInfo` (protocol.h:97) | `[abort hash]`: the #19 fields are hashed only when not default, so M2a's hash is unchanged |
| `HudModel` (kHudMaxPlayers 64, kHudKillFeedSize 8; HudPlayer :59, HudKill :101, HudKillFeed :112, HudSoldier :240 with is_alive / health / show_medic_revive / time_until_revive / is_beeing_revived, HudKillCounter :287, HudMessages :309, HudSpawnPoints :332, HudSpawnLogic :339, HudTeam :359, HudModel :382) | src/bf3_core/include/bf3/core/ui/hud_model.h | no kill-card, end-of-round or spawn-screen struct yet (#16) |

### 2.4 Audio

| seam | where | M2b |
|---|---|---|
| `MixerConfig` (max_instances 64), `MixerStats` (no CPU or HDR counters), `MixerCore` | src/bf3_audio/include/bf3/audio/mixer_core.h:56, :92–108, :127 | HDR, cap, profiler |
| router limits kMaxWeaponVoices 32, kMaxOneShotVoices 64, kGameMixerInstances 96 | src/bf3_audio/include/bf3/audio/weapon_audio.h:55–58 | one 128-voice cap in the mixer |
| fixed window `window_top_db 115`, `window_headroom_db 6` | src/bf3_audio/include/bf3/audio/audio_config.h:31–32; the gain src/bf3_audio/src/patch_instance.cpp:369 | replaced by the HDR window (R5) |
| kFlagEnableHdr, `loudness_db`, `ai_loudness` | src/bf3_core/include/bf3/core/audio/sound_program.h:107, :231, :235 | no HdrSetting or scope reader yet (#14) |
| `Bf3GameAudio` | src/bf3_godot/src/game_audio.h | the profiler readout |

### 2.5 Godot client

| seam | where | M2b |
|---|---|---|
| `M2aLoadInputs`, `M2aFrame` | src/bf3_godot/src/m2a_game.h | the M2b game copies the shape |
| `M2aGame` (client-side score counting from Death events, death label, K / T keys, respawn_s 5) | src/bf3_godot/src/level_session_m2a.h; playable scene src/bf3_godot/src/level_session_m2a.cpp:300–610 (Free FirefightBrains, kMaxPlayableBots 32, `respawn_ticks` from `--respawn`) | replaced in the M2b scene by server truth (`MatchState`, player rows) |
| HUD widgets (Ammo, Crosshair, HitIndicator, WeaponName, Health) | src/bf3_godot/src/hud_controls.h | new widgets beside them |
| 3p death: `dead` input, prone blend 0.5 s, hidden after 3 s | src/bf3_godot/src/game_crowd.cpp:13–15, :142–143, :185–191, :296–305; `kCrowdDeathHideSeconds = 3.0f` src/bf3_anim/include/bf3/anim/crowd_eval.h:284 | corpse stays 15 s (time_for_corpse); a revive stands it up |
| Godot rows | tests/CMakeLists.txt:1333–1343 (`godot.m2a*`, `openbf3_godot_row`, COST) | new rows in the same form |
| `tools/play.cmd` (`--bots=24`, res://scenes/m2a_metro.tscn) | tools/play.cmd | the new scene and defaults |

### 2.6 Interfaces M2b needs that do not exist (declared in the contract commits)

"W1.0" and "W2.0" are the contract commits of §11, made by the integrator; the owner fills the bodies.

| # | interface | kind | declared in | owner |
|---|---|---|---|---|
| 1 | `GameServerConfig::rules` (`MatchRulesConfig`: `mode` None / Tdm, `deploy_screen`, `revive`, `kill_limit_override`, `preround_override_s`, `max_players`; §3.2) and `GameServerConfig::tdm_bots` (`std::shared_ptr<const TdmBotConfig>`, null = none); the `[tdm bots]` line in `GameServer::init` after :811 that calls `add_tdm_bots(*this, p_config)` | struct-member insertions, game_server.h; one tagged line, game_server.cpp | W1.0 | ru (the line), bb (`add_tdm_bots`) |
| 2 | `bf3/host/tdm_rules.h`: `TdmRules : RespawnPolicy, CombatEventSink`, `RulesOrder`, `SpawnRefusal`, `RulesStats` (§3.2) | new header | W1.0 | ru |
| 3 | `SharedLoadOptions::tdm` (bool, one switch for the three TDM load parts: the logic graph, the assault gadget of #9, the motor's immunity abort of #19); `SharedLevel::logic` (`shared_ptr<const LogicGraph>`), `SharedLevel::logic_digest` (u64), `SharedLevel::seconds_logic`; the overload `load_shared_gunplay(const LevelDatabase &, SharedLevel &, const SharedLoadOptions &)` (the 2-argument form at level_load.h:150 stays and passes default options) | struct-member insertions + an overload, level_load.h | W1.0 | ru |
| 4 | `bf3/sim/logic/round_natives.h`: `RoundNativeConfig`, `RoundPhase`, `RoundNatives`, `NativeCoverage` (§3.1) | new header | W1.0 | ru |
| 5 | `NativePortMeaning` values appended (the ones not already at logic_native.h:55–58): `Kills`, `KillLimit`, `TeamWins`, `StopTime`, `UnSpawnAllSoldiers`, `OnPlayerSpawned`, `OnPlayerDied`, `OnSpectatorEnter`, `PlayerEnter`, `DisableAllowedToSpawnOn`, `Deactivate`, `RestartLevel`, `StartRound`, `EnablePlayerMovement`; the matching type-scoped rows in logic_native.cpp's binding table (R4; `InputRestriction.Activate` is scoped by `native_port(type, …)`, so GameInteraction's `Activate` row is untouched) | enum append + table rows | W1.0 | ru (the stubs' `switch (meaning)` in logic_native.cpp has a `default:` that records, so every new meaning stays `Recorded` there and S4's rows stay bit-equal) |
| 6 | `bf3/sim/spawn.h`: `SpawnQuery`, `FriendlyDeath`, `choose_spawn_scored(const SpawnQuery &)` (§3.5) | new declarations, additive block | W1.0 | ru |
| 7 | `CombatEventSink::revive(const net::ReviveEvent &)` with an empty default body | virtual-member insertion, gunplay.h | W1.0 | ru |
| 8 | `GunplayConfig::defib_def` (`uint16_t`, `kNoWeapon` = none; set by the loader when #9 resolves the defib) | member insertion, gunplay.h | W1.0 | ru |
| 9 | `bf3/host/kits.h`: `struct KitTableOptions { std::string assault_gadget; }` (empty = M2a), a 4-argument `make_kit_table(teams, kits, defs, const KitTableOptions &)` overload (the 3-argument one at kits.h:50 stays and calls it with empty options), `kKitGadgetSlot = 2`, `KitTable::defib_def`; in gunplay.h `struct GunplayLoadOptions { bool tdm_kits = false; }` and a 4-argument `load_gunplay_level(install, level, mode, const GunplayLoadOptions &)` (the 3-argument form at gunplay.h:181 stays, so fx3_demo.cpp:230, tp_crowd_check.cpp:147 and the audio_router, fx_triggers and render_crowd_weapons tests keep calling it unchanged); the `[kits]` insertion resolves the gadget into `level->defs` and fills assault slot 2 only with `tdm_kits`. `kKitSlots` is not changed; with the option off the table, its hash and every caller are unchanged | new structs + overloads + constant + member insertion, kits.h / kits.cpp / gunplay.h / gunplay.cpp `[kits]` | W1.0 | ru |
| 10 | `ServerView::downed()` (`std::span<const DownedSoldier>`), `phase()` (`net::MatchPhase`), `input_restricted()`, `kit(slot)`, **with default bodies** (empty, InRound, false, 0) so test fakes stay cheap; `struct DownedSoldier { uint8_t slot, team; uint32_t until_tick; sim::Vec3 corpse; uint16_t yaw; }` is declared **in server_view.h** (it needs only sim types; server_view.h adds `bf3/net/protocol.h` for `net::MatchPhase`, which bf3_host already links, and never includes tdm_rules.h or gunplay.h) | virtual-member insertions + struct, server_view.h; `View` overrides in game_server.cpp `[view]` | W1.0 | ru (bb reads) |
| 11 | protocol v3: `kProtocolVersion = 3`; `MessageType::MatchState = 25`, `PlayerList = 26`, `SpawnRequest = 27`; structs `MatchState` (with `restrict_until_tick`, `restrict_mask`), `PlayerRow`, `PlayerList`, `SpawnRequest` (`kit`, and `spawn_choice` u8, 0 = random, reserved for M4's flag spawns), `ReviveEvent`; `DeathEvent::respawn_tick`, `revive_until`, `killer_health`; `CombatEvents::revives`; `MatchInfo::logic_digest`, `rules_flags`; `Spawned::revived`; their encode / decode declarations | constant change, enum values, new structs, member insertions, protocol.h | W1.0 | nt |
| 12 | `ClientSession::send_spawn_request(uint8_t kit)`; `PresentationFrame::match` (`net::MatchState`), `players` (`std::array<net::PlayerRow, 64>` + `player_count`), `own_respawn_tick`, `own_revive_until`, `own_unspawned`; `PresentKind::Revive` appended; the client applies `MatchState`'s restriction to each predicted command whose tick is below `restrict_until_tick` (the same rule as `[cmd]`) | member insertions, client_session.h, presentation_events.h | W1.0 | nt |
| 13 | `bf3/host/tdm_brain.h` (`TdmBotConfig`, `TdmBrain`, `add_tdm_bots(GameServer &, const GameServerConfig &)`), `bf3/sim/bots/perception.h` (`PerceptionParams`, `Perception`), `bf3/sim/bots/aim_model.h` (`AimParams`, `aim_error`, `lead_point`) | new headers | W1.0 | bb |
| 14 | `bf3/core/audio/hdr_setting.h` (`HdrSetting`, `read_hdr_setting`), `bf3/core/audio/sound_scope.h` (`SoundScope`, `ScopeTable`, `read_scopes`, patch → scope index) | new headers | W1.0 | au |
| 15 | `bf3/audio/hdr_window.h` (`HdrWindow`), `bf3/audio/voice_cull.h` (`cull_voices`); `MixerConfig::hdr`, `MixerConfig::max_voices` (128), `MixerConfig::scopes`; `MixerStats::mix_ns_last`, `mix_ns_max`, `mix_ns_hist[16]`, `blocks`, `live_voices_max`, `culled_precull`, `culled_scope`, `culled_cap`, `suspended`, `window_top_db`, `window_bottom_db` | new headers + member insertions, mixer_core.h | W1.0 | au |
| 16 | `HudKillCard`, `HudEndOfRound`, `HudSpawnScreen`, `HudPreRound` structs and their `HudModel` members (`kill_card`, `end_of_round`, `spawn_screen`, `pre_round`); `HudFeedInput::match`, `players`, `kill_card_source` | new structs + member insertions, hud_model.h, hud_feed.h | W2.0 | ui |
| 17 | `src/bf3_godot/src/death_camera.h` (`Bf3DeathCamera`), `CrowdSoldierInput::revived`, `kCrowdCorpseSeconds` (replaces the 3 s hide for the M2b scene; the M2a constant stays) | new header + member insertion, crowd_eval.h | W2.0 | cam |
| 18 | `src/bf3_godot/src/m2b_game.h` (`M2bLoadInputs`, `M2bFrame`), `level_session_m2b.h` (`M2bGame`), `m2b_check.h` | new headers | W2.0 | gx |
| 19 | `sim::MotorParams::protect_abort_buttons` (u32, a `buttons::` mask) and `protect_abort_move` (bool), both default 0 / false (M2a unchanged); the motor clears `protect_ticks` when the tick's command presses a masked button or moves (`move_forward` / `move_right` ≠ 0) while protected. Set in `SharedLevel::motor` by the loader with `SharedLoadOptions::tdm` (§3.3), so server and client predict the same `protect_ticks`. `motor_params_hash` (motor_tuning.cpp:558) hashes the two fields **only when they are not the default**, so M2a's hash stays the constant E4 checks | member insertions, soldier_motor.h; insertions in soldier_motor.cpp `[abort]` and motor_tuning.cpp `[abort hash]` | W1.0 | ru |
| 20 | GameServer internals (not public API, but other tracks' rows depend on them): `Sv::rules` (`std::unique_ptr<TdmRules>`); `Impl::sv_policy()` (`RespawnPolicy &`: `*rules` when present, else `*respawn`); `Impl::sv_revive(slot, position, yaw, health, tick)` (any slot, remote ones included: a new life at the corpse, `want_spawn = false` and `respawn_at = 0` so the respawn loop at :673–677 does not respawn it again, `sv_on_spawn`, and for a remote slot `net::Spawned{revived = 1}` as `spawn()` sends it at :258–263); `Impl::sv_unspawn(slot)` (spawned = false, want_spawn = false, `gunplay->clear`, no DeathEvent); `Impl::sv_orders(tick, std::span<const RulesOrder>)` | member insertions, game_server.cpp `[sv]` | W1.0 | ru |
| 21 | `tests/tdm_fixture.h`: `load_tdm_level_once()` (the shared Metro TDM level with gunplay, the logic graph and the BotLevel, one per process), `ScriptedPeer` (a datagram-level peer on `GameServer::on_connect` / `on_receive`: Hello, Ready, SpawnRequest, KitSelect, Debug and CommandPackets stamped for the tick about to run; it decodes MatchState, PlayerList, Spawned, CombatEvents and its snapshots from the tick's `OutDatagram`s), `TdmRunConfig`, `TdmRunResult` (kills per team, K/D, revives, stuck bots, spawn checks, per-tick world hash, trace hash, timings) and `run_tdm(const TdmRunConfig &)` (init, then `tick` back to back on the caller's thread, as firefight_run.h:9–13) | new test header | W1.0 | ru (the body; ru's cases need it first), bb (additive counters) |
| 22 | The gadget-ammunition rule in `make_weapon_sim_def`: `MagazineCapacity == -1` means a gadget without a magazine: `unlimited_ammo = true`, `magazine_capacity = 1` (so `has_ammo` holds, a shot takes no round, weapon_sim.cpp:166, and `can_reload` is false); any other negative value still fails | named replacement R11 in weapon_sim_def.cpp:487–494 and R12 in weapons_tests.cpp:295 (the hostile case becomes −2); a new `sim.weapons.unit` case | W1.0 (the rule is stated in the contract; ru writes it) | ru |

---

## 3. Server

### 3.1 TDM through the S4 interpreter

The Metro TDM graph [verified: `bf3dump logic stats MP_Subway TeamDeathMatch0`]: 5,047 nodes, 383 primitive, 1,698
native, 783 event edges, 384 property, 385 link, 41 networked-only, 7 synced bools; `unknown_nodes` 0 (the 231
`nodes.unknown` are all unconnected); digest 6f6a0693b3924bde; build 0.008 s, load 0.145–0.165 s over three runs. The TDM part of it [verified, tdm edges]:

* `KillCounter` 3626 (TeamId Team1) and 3627 (Team2): MaxKillCount 100, EnemyWeight 1, IgnoreAI false;
  NeutralTeamWeight and TeamKillWeight are not decoded (`bf3dump logic fields` prints `(16)`), so team kills count 0
  [inferred, `measured = false`; ru may decode the two fields and use them]. No edge feeds them: the native counts
  kills itself. `Kills` and `KillLimit` (property outputs) → `Compare` (GreaterOrEqual) →
  `BoolToEvent.OnTrue` → `RoundOver(own team).TeamWins` and `RoundOver(other).StopTime`.
* `PreRound` 3558: RoundMinPlayerCount 4, RoundMaxPlayerCount 8, RoundRestartCountdown 10. `StartRound` →
  `InputRestriction.Deactivate`, both `RoundOver.RoundStarted`, `EventGate b7f13498.Close`; `EnablePlayerMovement` →
  the same gate, `InputRestriction.Deactivate`, `Delay 15` → FriendZone; an unnamed output (#769591088) →
  `LevelControl.RestartLevel`.
* `RoundOver` 3619 / 3620: `OnRoundOver` → `EventGate d7a51a33.Close` and `Delay 0.5` → `Delay` 3557 (1.75 s, a
  MathOp sum of 0.5 + 0.75 + 0.5 [inferred]) → `HumanPlayer.UnSpawnAllSoldiers`, `SyncedBool eca8ced6.SetTrue` →
  `Delay 60` → `LevelControl #34800493`. `OnRoundOver` also goes to the client's `UIGraph` (NetworkedClient).
* `HumanPlayer` 3628: PlayerKilledDelay 5.0. Outputs `OnPlayerEnter` (→ `AutoTeam.PlayerEnter`,
  `AutoTeam.DisableAllowedToSpawnOn`, PlayerFilters), `OnPlayerSpawned`, `OnPlayerDied`, `OnSpectatorEnter`.
* `AutoTeam` 3630: TamFullTeams, PlayerCountNeededToAutoBalance 8, TeamDifferenceToAutoBalance 3,
  RotateTeamOnNewRound true, AutoBalance true.
* `InputRestriction` 3674: allowed Yaw / Pitch / Jump / Prone / Zoom / Reload, refused Throttle / Strafe / Fire /
  Sprint / Melee / ThrowGrenade (the pre-round freeze [inferred]); `EventGate b7f13498.Out` → `Activate`.
* `CharacterSpawnReferenceObjectData` logic nodes 3532 (Team2) and 3533 (Team1), Enabled, each linked to all 102
  `AlternateSpawnEntityData` nodes (3459 …) [verified: `bf3dump logic instantiate`]. These are graph node indices: the
  `object` column of `bf3dump spawns` (where points 0 and 1 also show 3532 / 3533) is the EBX object index, a
  different index space. The 102 spawn points are the AlternateSpawns; the two references are not among them.
* `LevelControl` 3623, `FriendZone` 3559, `CombatAreaTrigger` 3554 / 3575, `Setting` 3677
  "BFServer.IsKillerCameraEnabled".
* KillCounter → MathOp → CompareFloat (B = 10 / 5 / 1) → NetworkedClient Sound: the "kills remaining" voice-over.

**`RoundNatives` (new, `bf3_sim`, #4).** The real native entities of the TDM types behind S4's bindings, the way
`LogicNativeStubs` is (logic_native.h:97–124): single-threaded, owned beside the `LogicRuntime` it feeds, `apply(tick,
effects, runtime)` before each `step`. Types it does not implement it hands to an embedded `LogicNativeStubs` (they
stay Recorded), so one object covers the graph.

```cpp
namespace bf3::sim::logic {
struct RoundNativeConfig { uint8_t max_players = 32; int kill_limit_override = 0; float preround_override_s = -1.0f; };
enum class RoundPhase : uint8_t { PreRound, InRound, RoundOver, Restarting };
class RoundNatives {
public:
	static core::Result<RoundNatives> create(std::shared_ptr<const core::gameplay::LogicGraph>, const RoundNativeConfig &);
	core::Status apply(Tick, std::span<const LogicEffect>, LogicRuntime &);
	// Game -> graph (each forces the native's outputs into the runtime's next step)
	core::Status player_enter(uint8_t slot, bool bot, LogicRuntime &);   // HumanPlayer.OnPlayerEnter -> AutoTeam
	core::Status player_leave(uint8_t slot, LogicRuntime &);
	core::Status player_spawned(uint8_t slot, uint8_t team, LogicRuntime &);
	core::Status player_died(uint8_t slot, uint8_t team, int killer, uint8_t killer_team, LogicRuntime &);
	core::Status kill_undone(uint8_t killer_team, LogicRuntime &);      // a revive (§3.6)
	core::Status tick_round(Tick, LogicRuntime &);                      // PreRound countdown, player counts
	// Graph -> game
	RoundPhase phase() const; uint8_t winner() const;                   // 0 none
	int kills(uint8_t team) const; int kill_limit() const;
	bool input_restricted() const; uint32_t restriction_mask() const;   // InputRestriction allowed-bits
	uint8_t team_of(uint8_t slot) const;                                // AutoTeam's choice
	bool take_unspawn_all(); bool take_restart();                        // edge-triggered requests
	float player_killed_delay_s() const;                                 // HumanPlayer (5.0)
	std::span<const NativeTimelineEntry> timeline() const;
	// Per native type: deliveries this object handled vs. handed to the embedded stubs as Recorded (E1).
	struct NativeCoverage { core::gameplay::LogicTypeId type; uint32_t handled = 0, recorded = 0; };
	std::span<const NativeCoverage> coverage() const;
};
}
```

What each native does [inferred where the data does not say; every timing is a named field of `config/tdm_tuning.cfg`
read once at load]:

| native | behaviour |
|---|---|
| KillCounter | +EnemyWeight per kill of an enemy by a member of its team (team kills and suicides count 0; IgnoreAI false, so bots count); publishes `Kills` on change and `KillLimit` once at start (the `kill_limit_override` dev option replaces 100) |
| PreRound | counts players in the game (bots included). At ≥ RoundMinPlayerCount: a RoundRestartCountdown (10 s) countdown, then fires `StartRound` and `EnablePlayerMovement` in that tick. `preround_override_s = 0` skips the countdown (tests, `--preround=0`) |
| RoundOver | `RoundStarted` arms it; `TeamWins` sets the winner to its team (first one wins in a tick, lower node index on a tie) and fires `OnRoundOver`; `StopTime` recorded |
| HumanPlayer | `OnPlayerEnter` / `OnPlayerSpawned` / `OnPlayerDied` / `OnSpectatorEnter` forced with the player's `LogicContext`; `UnSpawnAllSoldiers` → `take_unspawn_all()` (the rules kill nobody: they remove every soldier without a death, §3.4) |
| AutoTeam | `PlayerEnter`: the smaller team, team 1 on a tie; with AutoBalance and ≥ 8 players, a difference ≥ 3 moves the most recently joined bot of the larger team at its next death; a human never moves while bots can; RotateTeamOnNewRound swaps every player's team at the restart (§13 Q4) |
| InputRestriction | `Activate` / `Deactivate`; the allowed-bit mask from the entity's fields. The server applies it to every command before the motor (a refused bit reads as released) |
| LevelControl | `RestartLevel` and the 60 s post-round output → `take_restart()`: the round restarts in place (counts 0, every slot back to the spawn screen, PreRound again). No rotation before M3 |
| CharacterSpawn / AlternateSpawn | the point sets per team (both teams get all 102, TDM's random spawns); `Enabled` tracked |
| FriendZone, CombatAreaTrigger, Setting | Recorded (FriendZone has no M2b effect; Metro TDM's combat area is not enforced in M2b, §13 Q7); `IsKillerCameraEnabled` read once as a bool (default true) |

**The E1 coverage check.** `LogicRuntime::unhandled()` (logic_runtime.h:194–201) lists only primitive ports with no
handler, and a native port with no real handler lands in the stubs as `Recorded` (logic_native.cpp:220–221), so a
missing native never shows in `unhandled()`. E1 therefore reads `RoundNatives::coverage()`: every TDM type of the
table above has `recorded == 0` after a round; `Recorded` is allowed only for FriendZone, CombatAreaTrigger and
Setting. Types RoundNatives does not implement (TicketCounter and the other Conquest / Rush types) stay `Recorded` in
the embedded stubs, so M4 adds them without changing an S4 or M2b row.

### 3.2 `TdmRules` (bf3_host, server thread, #2)

```cpp
namespace bf3::host {
struct MatchRulesConfig { enum class Mode : uint8_t { None, Tdm } mode = Mode::None; bool deploy_screen = false;
	bool revive = true; int kill_limit_override = 0; float preround_override_s = -1.0f; uint8_t max_players = 32; };
// What the rules ask the GameServer to do this tick; executed by Impl::sv_orders in [orders] (#20), in vector order.
struct RulesOrder {
	enum class Kind : uint8_t { Spawn, Revive, Unspawn } kind = Kind::Spawn;
	uint8_t slot = 0;
	sim::Vec3 position{}; uint16_t yaw = 0; float health = 0;   // Revive only
};
enum class SpawnRefusal : uint8_t { None, Alive, TooEarly, BadKit, Phase, RateLimited, Spectator };
inline constexpr uint8_t kKeepKit = 0xFF;   // kit_of: keep the slot's KitSelect kit (a remote slot without the deploy screen)
class TdmRules final : public RespawnPolicy, public CombatEventSink {
public:
	// p_bots: GameServerConfig::bots->level (may be null; the bots' nav and their spawn list).
	static core::Result<std::unique_ptr<TdmRules>> create(const SharedLevel &, std::shared_ptr<const BotLevel> p_bots,
			const MatchRulesConfig &, uint64_t match_seed, uint32_t max_slots);
	// RespawnPolicy (asked through sv_policy() by Sink::death, R8, and the fall death, R3): bots and humans without the
	// deploy screen get death + PlayerKilledDelay, or the longer revive wait of §3.4 for a bot near an assault
	// teammate; a deploy-screen human and every death in RoundOver / Restarting get nullopt (the spawn then comes only
	// as a RulesOrder::Spawn).
	std::optional<uint32_t> respawn_tick(uint8_t slot, uint32_t death_tick) override;
	uint32_t earliest_respawn(uint8_t slot) const;   // what R8 stamps into DeathEvent::respawn_tick (also for nullopt)
	uint32_t revive_until(uint8_t slot) const;       // what R8 stamps into DeathEvent::revive_until (0: not revivable);
	                                                 // for an auto-respawning slot min(man-down end, its respawn tick)
	void hit(const net::HitEvent &) override; void death(const net::DeathEvent &) override;
	void detonation(const net::DetonationWire &) override; void revive(const net::ReviveEvent &) override;
	uint8_t on_connect(uint8_t slot, bool bot, std::string_view name);   // returns AutoTeam's team (1 / 2)
	void on_disconnect(uint8_t slot);
	void on_spawned(uint8_t slot, uint32_t tick);   // every spawn (R7): leaves the downed list, HumanPlayer.OnPlayerSpawned
	SpawnRefusal request_spawn(uint8_t slot, uint8_t kit, uint32_t tick);
	// [rules], inside sv_rules after apply_rules and before the copy-back: logic drain and natives, regen (writes
	// r_states[slot].health), man-down expiry, the defib test over gunplay.interval_shots(), the round flow.
	core::Status step(uint32_t tick, const ServerView &, const Gunplay &, std::span<sim::SoldierState> r_states,
			std::vector<RulesOrder> &r_orders);
	// §3.5. p_points is the list spawn() chose for the slot (game_server.cpp:227-230); the result indexes it.
	// SIZE_MAX = use the M2a chooser.
	size_t choose_spawn(uint8_t slot, std::span<const sim::SpawnPoint> p_points, const ServerView &, uint32_t tick) const;
	// [cmd]: a refused bit or axis reads as released for a command whose tick is below restrict_until_tick.
	uint32_t apply_input_restriction(uint32_t command_tick, uint32_t buttons, int8_t &r_forward, int8_t &r_right) const;
	std::span<const DownedSoldier> downed() const; net::MatchPhase phase() const; bool input_restricted() const;
	const net::MatchState &match_state() const; std::span<const net::PlayerRow> players() const;
	bool match_state_changed() const; bool players_changed() const;              // send on change (§3.7)
	uint8_t team_of(uint8_t slot) const;   // AutoTeam's team, including a balance move or the round's rotation
	uint8_t kit_of(uint8_t slot) const;    // SpawnRequest's kit, the bot draw, or kKeepKit
	const sim::logic::LogicTrace &trace() const;                                // E6 (logic_runtime.h:138)
	const RulesStats &stats() const;                                            // refusals, fallbacks, revives (E2, E7)
};
}
```

**Kits.** A human's kit is the one of its accepted `SpawnRequest` (deploy screen). A remote slot without the deploy
screen (M2a rows, `--deploy-screen=0`) keeps its `KitSelect` kit: `kit_of` returns `kKeepKit` for it and R7 leaves
`ps.kit` as `sv_kit_select` set it (game_server.cpp:1451). A bot's kit is drawn by the rules at each death from
`splitmix64(match_seed ^ slot ^ life)` with the weights of `tdm_tuning.cfg` (assault 40 %, engineer, support, recon
20 % each); the brain reads it through `ServerView::kit(slot)` (#10), so no interface carries a kit from bb to ru.

**Teams.** `team_of(slot)` is AutoTeam's current team: the join choice, a balance move (applied at the moved bot's
next death) and RotateTeamOnNewRound's swap at the restart. R7 writes it into `s.team` before `gunplay->on_spawn`
(game_server.cpp:1209–1210, which sets gunplay's friendly-fire teams), so `Spawned.team` and `sv_spawned` follow
it; with rules on, KitSelect's `next_team` (:1202–1205) is ignored.

**Slots ≥ 64 with rules on (the `kMaxLagSlots` limit).** lag_comp.cpp's RewindProvider answers hit targets for slots
0–63 only (lag_comp.cpp:19, firefight_run.h:79). With rules on, a slot ≥ 64 is a **spectator**: `add_tdm_bots` never
fills it, `request_spawn` refuses it (`Spectator`, counted), and the rules never order its spawn. `init` is not
changed. Without rules nothing changes: M2a's 64-bot runs keep their observer peer in slot 64
(host_gunplay_perf.cpp:313–314, level_session_m2a.cpp:593–607), and `host.gunplay.perf` stays as it is.

**Wiring (named insertions into game_server.{h,cpp}, each line marked `// M2b ru [tag]`):**

| tag | where | what |
|---|---|---|
| `[cfg]` | `GameServerConfig` (game_server.h:81–103), after `firefight` (:102) | `MatchRulesConfig rules;` and `std::shared_ptr<const struct TdmBotConfig> tdm_bots;` (#1) |
| `[sv]` | the `Sv` struct and the Impl declarations (:740–752) | `std::unique_ptr<TdmRules> rules`; `sv_policy`, `sv_revive`, `sv_unspawn`, `sv_orders` (#20) |
| `[init]` | `sv_init` (:1066–1073) | with `config.rules.mode == Tdm`: fail unless `config.level->logic` and gunplay are set; create the rules (passing `config.bots ? config.bots->level : nullptr`); `sv->respawn` stays the FixedRespawn, and `sv_policy()` returns the rules while they exist |
| `[tdm bots]` | `GameServer::init`, after `add_firefight_bots` (:811) | `if (p_config.tdm_bots) BF3_TRY(add_tdm_bots(*this, p_config));` (after `sv_init` at :792, so the rules exist) |
| `[add]` | `add_local` (:721–736), before the spawn | with rules: `s.team = rules->on_connect(at, bot, "")`; bots pass team 0 |
| R2 | `handle_hello`, `s.team = 1` (:330) | `s.team = rules ? rules->on_connect(slot, false, hello.name) : 1` |
| R10 | the Ready handler, `if (!s.spawned) s.want_spawn = true;` (:859–861) | `if (!s.spawned && !(rules && config.rules.deploy_screen)) s.want_spawn = true;` — with the deploy screen the slot only becomes ready (its player row exists from `on_connect`) and spawns on its first accepted SpawnRequest |
| R8 | `Sink::death` (:986–1001) | reordered: ask `sv_policy()` first, set `want_spawn` / `respawn_at` as today, then push a copy of the event with `respawn_tick = rules->earliest_respawn(victim)`, `revive_until = rules->revive_until(victim)` and `killer_health = sv->tick_states[killer].health` (the current tick's copy apply_rules is writing; 0 for a fall) stamped (0 / 0 / 0 with rules off, so the M2a order and content are unchanged), then `rules->death(event)` |
| `[cmd]` | the motor loop before `motor.step` (:659) | `s.last.buttons = rules->apply_input_restriction(command tick, …)` (a refused bit or axis reads as released) |
| R3 | the fall death (:662–665) | `respawn_at` from `sv_policy().respawn_tick` (want_spawn only when it has a value) |
| `[rules]` | `sv_rules` between `apply_rules` (:1144) and the copy-back (:1145) | `rules->step(p_tick, view, *gunplay, tick_states, orders)` |
| `[orders]` | `sv_rules` after the placements (:1158) | `sv_orders(p_tick, orders)`: Spawn → `spawn(slot, SIZE_MAX, tick)` (R1 picks the point); Revive → `sv_revive`; Unspawn → `sv_unspawn` |
| R1 | `spawn()`, the `choose_spawn` call (:236) | `rules ? rules->choose_spawn(p_slot, points, view, p_tick)` over the `points` list :227–230 selected, falling back to `sim::choose_spawn` on SIZE_MAX (counted) |
| R7 | `sv_on_spawn`, `ps.kit = slot % kKitCount` (:1206–1208) | `if (rules) { s.team = rules->team_of(slot); if (const uint8_t k = rules->kit_of(slot); k != kKeepKit) ps.kit = k; rules->on_spawned(slot, tick); } else if (!s.remote) ps.kit = …` (the old line) |
| `[recv]` | the datagram dispatch next to KitSelect (:893) | `SpawnRequest` → `request_spawn` |
| `[snap]` | the send step | `MatchState` and `PlayerList` on change, the full pair to a client after its `MatchInfo` |
| `[view]` | `View` (:1004–1033) | the #10 overrides read `sv->rules` (defaults without rules) |

Without `rules` every M2a path is unchanged (E4's hash checks).

**Load.** `load_shared_level` (level_load.h:140), on the loader thread, with `SharedLoadOptions::tdm`: build the graph
the way bf3dump does (`build_logic_graph(db, root, options)`, logic_commands.cpp:160) and store it with its digest
(s4.md:122–134); resolve the assault gadget (#9) and set the motor's immunity abort (#19). 0.16 s on Metro [verified];
not cached (§9.4).

### 3.3 Health and regen

Numbers from the soldier def [verified: `bf3dump soldier`]: max_health 100, low_health 30, regeneration delay 10 s,
rate 5 /s, immortal 2 s, man_down_time 13 s, man_down_health_points 9999, post_revive_health 20, time_for_corpse 15 s,
interactive_man_down 0, SuppressionAbortsHealthRegeneration 1, regeneration_delay_modifier 1 (0.8 only in the
HealSpeedBoost specialization), abort_spawn_immortality_inputs = {12, 0, 1, 7, 30, 31, 1, 2, 11}.

* **Regen** is server-side, per slot in `TdmRules`: `last_damage_tick` (from `hit` events, and fall damage seen as
  `kDamaged` in the states). At `tick ≥ last_damage + delay·60` the rules add `rate/60` hp per tick up to max_health,
  writing `r_states[slot].health` in `[rules]` (the only writable copy; ServerView and Gunplay have no health
  setter). The snapshot carries it and the client moves its ring's health as for a hit (M2a R2: `states_match`
  leaves health out, client_session.cpp:57–74), so regen is not a misprediction; it raises the `health_rebases`
  counter, which no row gates with rules on. `SoldierState` does not change.
* RegenerationDelayModifier is a specialization effect (squad perks are not in M2b): not applied, `measured = false`.
* Suppression is not simulated in M2b; `SuppressionAbortsHealthRegeneration` has no input (named default
  `suppression_resets_regen = false`, `measured = false`).
* **Spawn immunity** stays the motor's `protect_ticks` = 120. The data's abort list holds 9 raw EntryInputActionEnum
  values; no enum names are decoded in the project (soldier_def.h:108). ru maps them through a named table
  `immunity_abort_map` in `tdm_tuning.cfg` (value → a `buttons::` bit or `move`) [inferred, `measured = false`]; the
  default maps only what it can name with confidence, Fire and the two movement axes, and lists the unmapped values in
  the load notes. The mask goes into `SharedLevel::motor` (#19), so the server's motor and the client's prediction
  end the immunity on the same tick (`protect_ticks` is compared in `states_match`, client_session.cpp:69).
  Before falling back to the hand-made table, ru checks whether the EBX type info of EntryInputActionEnum carries
  member names the reader can print; if it does, the map is built from those names (Fire, Throttle, Strafe, ...) and
  only the name → command-bit step is [inferred]. Until then E3 tests the mapping as configured, which is a named
  default, not a measurement (§13 Q13).

### 3.4 Death and the respawn flow

0. **Join.** A deploy-screen human's Ready does not spawn it (R10, game_server.cpp:859–861); it sits on the spawn
   screen (E9's "at join") until its first accepted `SpawnRequest`. Bots and humans without the deploy screen spawn
   at Ready / `add_local` as in M2a.
1. A death (gunplay's `apply_rules` or a fall) reaches `Sink::death`. R8 asks `sv_policy()`, then pushes the event
   stamped with `respawn_tick = death + round(PlayerKilledDelay·60)` (+300), `revive_until` (below; 0 when revive is
   off) and `killer_health` (from `sv->tick_states`, 0 for a fall), then calls `TdmRules::death`: `player_died` to the
   natives, killer +100 (+10 headshot) from `read_scoring` (mode_def.h:173–180), victim deaths +1; the victim joins the
   downed list until `revive_until`.
2. **Bots** respawn at their respawn tick through the M2a loop (want_spawn / respawn_at, game_server.cpp:673–677), R1
   picks the point and R7 the kit and team. The respawn tick is death + 300, except that a bot that dies with a living
   assault teammate within `revive_wait_radius_m` (20 m) of its corpse waits up to `revive_wait_s` (8 s, inside the 13 s
   man-down) for a revive [inferred, named defaults in `tdm_tuning.cfg`, `measured = false`]. Without the wait a bot
   would leave the downed list after 5 s, too soon for a teammate 20–30 m away. For every slot that respawns by
   itself, `revive_until = min(death + man_down_time·60, respawn tick)`, so the downed list, `ServerView::downed()` and
   the bots' revive test (§5.6) all use the real window. Whatever spawns a slot (the loop, an order, a revive) ends in
   R7's `on_spawned`, which takes it off the downed list, so a living soldier is never "revived".
3. **Humans with `deploy_screen`** get nullopt from the policy (`revive_until = death + 780`) and wait for a
   `SpawnRequest {kit}`. The server refuses it while alive, before `respawn_tick`, in `RoundOver` / `Restarting`,
   with a kit ≥ kKitCount, a second one in a tick, or for a spectator slot; a refusal is counted (`RulesStats`) and
   otherwise silent: the spawn screen shows the timer from `respawn_tick` itself. An accepted request becomes a
   `RulesOrder::Spawn` in the next `step`; deploying while still revivable forfeits the revive.
4. **Humans without `deploy_screen`** (M2a rows, `--deploy-screen=0`): the M2a automatic respawn at `respawn_tick`,
   with their KitSelect kit (`kKeepKit`, §3.2).
5. `UnSpawnAllSoldiers` (1.75 s after `RoundOver`): one `RulesOrder::Unspawn` per spawned slot. `sv_unspawn` sets
   spawned = false and want_spawn = false and clears the weapons (`gunplay->clear`), with no DeathEvent and no score;
   unspawned slots drop out of the snapshots (the send loop skips `!s.spawned`, :683–686), so clients stop drawing
   them; `MatchState::unspawn_tick` tells the own client to stop predicting (`PresentationFrame::own_unspawned`, nt).
   At restart (`take_restart()`): counts 0, the downed list cleared, PreRound again; bots get a Spawn order each,
   humans see the spawn screen.
6. Spawned → `on_spawned` → `player_spawned` to the natives; `protect_ticks` set by the motor as today.

**Respawn timer mapping:** PlayerKilledDelay → the earliest respawn is [inferred], `measured = false`.

### 3.5 Spawn selection

Metro TDM has 102 points, all team 0, `LocationTextSid` ID_H_MP_DM_RANDOMSPAWN, SpawnProtectionRadius 10,
SpawnProtectionFriendlyKilledCount 3 within 30 s, TryToSpawnOutOfSight true [verified: `bf3dump spawns`]. 92 of 102
are in the main nav component [verified: `bf3dump nav components --points`].

**Which list.** R1 hands the rules the list `spawn()` already selected (game_server.cpp:227–230): for a bot slot with
`BotFillConfig::spawn_in_main` (the default, bot_fill.h:157), BotLevel's re-indexed main-component list, so bots never
start where the nav cannot take them; for every other slot, `config.level->spawns` (all 102). The returned index
always refers to that list, so no index crosses between the two lists. There is no enclosed-spawn filter for humans
in M2b: the user said the spawn room is not important now (§13 Q11); the scored rule below already prefers open
points out of sight.

```cpp
namespace bf3::sim {
struct FriendlyDeath { Vec3 position; uint32_t tick = 0; uint8_t team = 0; };
struct SpawnQuery {
	std::span<const SpawnPoint> points; uint8_t team = 0;                      // points with a team != 0 and != team are skipped
	std::span<const Vec3> enemies; std::span<const Vec3> enemy_eyes;
	std::span<const FriendlyDeath> recent_deaths;                              // the rules' ring of the last 30 s
	float protection_radius = 10.0f; float friendly_block_radius = 30.0f; int friendly_block_count = 3;
	uint32_t friendly_block_ticks = 1800; uint32_t tick = 0; uint64_t seed = 0; int max_los_rays = 16;
	const CollisionWorld *world = nullptr;                                     // null: no line-of-sight term
};
size_t choose_spawn_scored(const SpawnQuery &);   // SIZE_MAX when nothing survives (1)-(3)
}
```

The rule [inferred, named defaults in `tdm_tuning.cfg`]: (1) the points of the slot's team (all of Metro TDM's are
team 0, both teams use every point); (2) drop points with an enemy within the protection radius; (3) drop points
where `friendly_block_count` (3, data) teammates died within `friendly_block_radius` inside `friendly_block_ticks`
(30 s, data); the 30 m radius is [inferred]: the spawner data gives only the count and the time; (4) for
TryToSpawnOutOfSight, rank the survivors by the distance to the nearest enemy and test eye-to-chest rays from the 4
nearest enemies to the best 4 (≤ 16 rays); points seen by none rank first; (5) take the best, ties by the seeded draw
`splitmix64(match_seed ^ slot ^ tick)`. The query is stateless (the deaths ring lives in the rules), so it works the
same over either list. If nothing survives (1)–(3), the M2a chooser picks (counted as a fallback). The 2 s immunity
covers the rest.

### 3.6 Revive

* **Man-down.** A dead soldier is revivable until its `revive_until` (§3.4: 13 s for a deploy-screen human, the
  shorter auto-respawn window for the others), unless it deployed. interactive_man_down 0: no crawling.
* **The defibrillator.** `weapons/gadgets/defibrillator/u_defib` [verified: `bf3dump weapon defibrillator`]: damage
  giver "Defib", instant hit, max range 1.8 m, damage 0, 60 rpm, deploy 0.4 s, sound defibrillator_fire, ammo
  `-1 x 1` (the knife is `1 x -1`). The assault kit's gadget1 defaults to none in the data (10 options, the defib among
  them, scoring-gated) [verified: `bf3dump kits`]; with rules on M2b puts it in assault slot 2 (#9, §12 J1).
* **Making the weapon sim build it (R11, R12, #22).** On main the defib **cannot be built** [verified:
  `bf3dump weapon-sim weapons/gadgets/defibrillator/u_defib` fails with "MagazineCapacity = -1 is outside [0,
  10000]"]: weapon_sim_def.cpp:487–489 fails a negative capacity, one failed def fails `make_weapon_table`
  (:617–620), and the table is built at gunplay.cpp:322, so every `SharedLoadOptions::tdm` load would fail. Even with
  the check relaxed, the capacity clamps to 0, `unlimited_ammo` is false (`NumberOfMagazines` 1 ≥ 0, :494) and
  `has_ammo` (weapon_sim.cpp:51) would never fire. R11 makes `MagazineCapacity == -1` a gadget without a magazine:
  `unlimited_ammo = true`, `magazine_capacity = 1`; `has_ammo` then holds, a shot takes no round (weapon_sim.cpp:166),
  and `can_reload` is false (the data's 5 s auto-replenish has nothing to refill). Any other negative capacity still
  fails, so R12 changes the hostile case at weapons_tests.cpp:295 from −1 to −2. No table on main contains a def
  with capacity −1 (each would have failed), so `weapon_table_hash` changes only for tables that now contain the
  defib, i.e. only with `SharedLoadOptions::tdm`. ru adds a `sim.weapons.unit` case: u_defib's def builds and, held
  for 3 s, fires 3 instant-hit shots (60 rpm) of range 1.8 m with 0 damage and no reload (`bf3dump weapon-sim` on u_defib then
  builds too). The defib then fires through the weapon sim like the knife (an
  instant-hit projectile, weapon_sim_def.h:83, ballistics.cpp:445), so the server and the client see a normal
  `ShotEvent` with its def.
* **The test (in `TdmRules::step`, not in gunplay or lag compensation).** A corpse is not a hit target: lag_comp.cpp:505
  leaves `MoveMode::Dead` soldiers out, and gunplay drops damage to dead victims and teammates (gunplay.cpp:621–628),
  so the shot itself does nothing. The rules read `gunplay.interval_shots()` (gunplay.h:243), keep a per-slot cursor on
  `shot_index` (never reset, weapon_sim.h:115) so each shot is seen once, and for every shot with `def ==
  KitTable::defib_def` from a living shooter: the nearest **downed teammate** whose corpse sphere (centre at the
  corpse position + 0.3 m, radius 0.6 m, named defaults) the segment origin → origin + direction × 1.8 m enters, with a
  clear static-world ray from the origin to the centre. An enemy shooter, a living target or an expired man-down:
  nothing (counted).
* **The revive.** A `RulesOrder::Revive {slot, corpse, yaw, post_revive_health}` → `sv_revive` (#20): a new life at the
  corpse (`life + 1`, standing, no spawn immunity, health 20, the same kit), `want_spawn = false` and `respawn_at = 0`
  (so the respawn loop at :673–677 does not respawn the revived bot at its old tick), `sv_on_spawn`, and for a remote
  slot `net::Spawned{revived = 1}` sent exactly as `spawn()` sends it, so the human's client takes the new life like
  any spawn. A `ReviveEvent {reviver, revived, tick}` goes into `CombatEvents` through the sink's `revive`.
* **Score.** Reviver +100 (the scoring table's revive row; the 110 variant unused [plan §7.3]); the revived player's
  death −1; the killer keeps the personal kill; the killer team's `KillCounter` −1 (`kill_undone`) [inferred: BF3 TDM
  revives take kills back, default true, `measured = false`, §13 Q2].
* **Bots** revive (§5.6).

### 3.7 Protocol v3 (bf3_net)

The protocol names three channels (protocol.h:36–39): ch0 unreliable (snapshots, commands, ShotEvents), ch1
reliable-ordered (handshake, Spawned, CombatEvents at protocol.h:72, KitSelect) and ch2 bulk. Every M2b addition is
on ch1; revision 1 wrongly put the events on ch2.

| message | dir / channel | content | sent |
|---|---|---|---|
| `MatchState` (25) | S→C, ch1 | phase, round start tick, countdown end tick, kills[2], kill_limit, winner, round_end_tick, unspawn_tick, restart_tick, `restrict_until_tick` (UINT32_MAX while restricted with no known end, 0 = none), `restrict_mask` (the InputRestriction allowed bits), killer_camera bool | on change, and to a new client |
| `PlayerList` (26) | S→C, ch1 | ≤ 64 `PlayerRow` {slot, team, kit, bot, alive, kills, deaths, score (i32), ping, name ≤ 16 UTF-8 bytes} | full list on join; changed rows at most every 15 ticks |
| `SpawnRequest` (27) | C→S, ch1 | kit (u8), `spawn_choice` (u8, 0 = random; M2b refuses any other value as `BadKit`; reserved so M4's Conquest spawn screen needs no protocol bump) | on Deploy |
| `DeathEvent` + `respawn_tick`, `revive_until`, `killer_health` | ch1, in the existing CombatEvents | — | per death |
| `ReviveEvent` in `CombatEvents::revives` | ch1 | reviver, revived, tick | per revive |
| `MatchInfo` + `logic_digest`, `rules_flags` (tdm, deploy_screen, revive) | ch1 | — | handshake |
| `Spawned` + `revived` | ch1 | — | per spawn and per revive |

**The pre-round freeze on the client.** The server rewrites the buttons in `[cmd]` before the motor; a client that did
not do the same would predict movement and shots the server drops (snap-backs and refused shots during the 10 s
countdown, playtest step 2). So the restriction is a pure function of (command tick, `restrict_until_tick`,
`restrict_mask`): the server applies it to the received command, and nt's client session applies the same function to
each predicted command (#12). The countdown's end tick is known 10 s ahead, so both ends switch on the same tick.

The kill feed is the DeathEvent stream (weapon def, headshot, zone). `kProtocolVersion = 3`; a v2 client gets `Kick`.
A client whose own load disagrees with `rules_flags` (no TDM load while the server has one) is refused as a kit-hash
mismatch is today. **One M2a gate issue is fixed here** (m2a-gate.md:108): shot ids reused across a respawn give double
impact cues; the server keeps `next_shot_index` over lives (gunplay.cpp:407, weapon_sim.cpp:467–470), so the reuse is on
the client path that seeds a new life before its `Spawned` (client_session.cpp:1801–1817); nt finds it and adds a row.
The other gate note, `kMaxLagSlots` (m2a-gate.md:113), is handled by the spectator rule of §3.2 in rules mode and left
as M2a reported it otherwise.

### 3.8 What M3 needs from this

* The v3 messages are transport-neutral: M3's ENet keeps ch1 reliable-ordered for MatchState, PlayerList,
  SpawnRequest and the CombatEvents that carry deaths and revives; nothing in them assumes loopback.
* `MatchInfo::logic_digest` is the graph digest M3's digest handshake compares (both sides build the same graph), and
  `rules_flags` tells a remote client which `SharedLoadOptions::tdm` load to make.
* M2b does not run a client-side logic graph. `MatchState` is a server summary; M3 adds the logic ghost (7 synced
  bools, `LogicRuntime::ghost()`) and the 41 networked events (the UIGraph round-over cue, the kills-remaining VO)
  through `LogicRuntime::networked()`, which the rules already collect and drop (counted).
* M4's Conquest spawn screen (flags, squad) uses `SpawnRequest::spawn_choice`, which v3 already carries.
* AutoTeam and the player rows already handle N humans (join, leave, names from `Hello`); a late joiner gets the full
  MatchState and PlayerList; M3 only adds sockets.
* `LevelControl` restarts in place; M3's rotation replaces `take_restart()`'s handler. `MatchRulesConfig::Mode` gains
  Conquest in M4, and the Conquest natives fill the types RoundNatives leaves `Recorded`.

---

## 4. Client (route A+: the ui1 / ui2 groundwork)

Everything below is on the main thread, fed by `fill_hud` from the `PresentationFrame` (client_session.h:254–306),
drawn with ui1's assets: fonts `get_font("$HUD_medium")`, icons `get_region("ui/assets/iconsingame", …)`, kill-feed
weapon icons from `ItemIconIndex`'s `_small` DxTextures, strings through the localisation table
(docs/design/ui-swf-fonts.md §12.3). The flows the SWF data names are `spawnscreenpc`, `killscreen`,
`scoreboardtwoteamshud*screen`; the UI comp data M2b reads (docs/design/ui-minimap-icons.md): UIKillCardCompData
(27 fields), EndOfRound (52), PreRound, Nametag, DamageIndicator, Spectator. Route A+ means: the layouts come from
those comp data and SWF placements where they exist (`get_hud_placements`), drawn with Godot Controls, not a SWF
player.

| screen | data | behaviour |
|---|---|---|
| Kill feed | `HudKillFeed` (8 lines) from Death events: killer name, `_small` weapon icon, headshot icon, victim; own kills highlighted (LocalKillMessage) | lines fade after the comp data's time (default 5 s) |
| Scoreboard | `HudPlayer` rows from `PlayerList`, two columns (`scoreboardtwoteamshud`), sorted by score; bots marked; team counts and kill limit from `MatchState` | hold Tab |
| Spawn screen | new `HudSpawnScreen`: kit bar (4 kits; assault shows the defibrillator in slot 2, no toggle: it replaces nothing), Deploy (enabled at `own_respawn_tick`), the timer, "revive possible" until `own_revive_until`; no point choice (TDM spawns are random) | after each death and at join; Enter or click Deploy |
| Kill card | new `HudKillCard`: killer name, kit, weapon icon and name, distance, killer health left (`DeathEvent::killer_health`), headshot | 4 s after the own death, with the death camera |
| Death camera | `Bf3DeathCamera`: orbit the corpse, then turn to the killer's presented position for 4 s when `IsKillerCameraEnabled` | then the spawn screen |
| Pre-round | new `HudPreRound`: "round starts in N" from `MatchState`, the freeze shown | until StartRound |
| Round end | new `HudEndOfRound`: winner, team scores, top 3 players, "next round in N" (60 s from the graph) | on `RoundOver` |
| HUD soldier | `HudSoldier` M2b fields: is_alive, health, show_medic_revive, time_until_revive / death, is_beeing_revived | existing widgets |

3p soldiers: the crowd keeps a corpse for time_for_corpse (15 s) instead of hiding after 3 s (`kCrowdCorpseSeconds`
for the M2b scene; game_crowd.cpp:299 already adds `kCorpseExtraSeconds`); a revive brings the corpse back to stand
(prone → stand blend, 0.5 s) at the same place. Ragdolls are out of M2b (§12 J3). Nametags over teammates (the
Nametag comp data) and score popups are deferred (not needed to play). The defibrillator in hand: the viewmodel set
resolves slots 0, 1, 6 and 7 (viewmodel_set.h:274–281); cam adds slot 2's unlock to it when the defib's 1p mesh
resolves like the others, otherwise the defib is drawn as no viewmodel (the HUD weapon name still shows), accepted for
M2b.

---

## 5. Bots B2–B4

M2a's `FirefightBrain` Free mode is the starting code [verified, m2a-bt.md]: nearest enemy with a clear eye-to-chest
ray every 30 ticks (≤ 6 rays), σ 1.5° → 0.5° over 1 s, turn 540 / 360 °/s, fire cone 4°, bursts 3–6, sidearm when
empty, WalkerBrain hunt (kHuntShare 0.8), strafe within 25 m, blind fire; teams 1 + ((i + i/4) % 2); measured 64 bots,
60 s, 117 / 100 kills, 0 stuck, replay 0 / 3,600 differing ticks, p99 1.909 ms, 600 s soak 1,830 kills (976 / 854).
`TdmBrain` (#13) replaces Free mode for TDM; FirefightBrain stays for the M2a rows. B3 (Conquest capture) is M4.

### 5.1 Perception (`bf3/sim/bots/perception.h`, pure)

Per bot, per candidate enemy, every perception tick (each bot every 6 ticks, staggered by slot, so 1/6 of the bots per
tick): in the FOV (160° × 90°, research 06-bots.md §3.4) and within 120 m → 2 rays (eye → head, eye → chest) against the
static world, at most 4 candidates per bot (nearest first), ≤ 8 rays per bot-update. A visible candidate accumulates
detection `+dt · (1 + moving + firing) / detection_time(distance)` (detection_time 0.2 s at 10 m to 1.5 s at 100 m,
linear [inferred]); detected at 1.0, forgotten 3 s after the last sight. **Hearing:** a shot within
`hearing_radius_m` (default 60 m; AILoudness is −1 in the weapon patches [verified], so it is a named default,
`measured = false`) gives a heading and a position estimate with 5 m error; no rays. Perception never reads hidden
state other than positions it could see or hear.

### 5.2 Target choice and reaction

Score per detected enemy: `w_d / distance + w_aim · (enemy aims at me) + w_dmg · (damaged me in the last 3 s) +
w_cur · (current target)`. Switch only when a new score beats the current by 25 %. Reaction delay before the first
shot: uniform 0.25–0.6 s by skill (§3.5 of the research), drawn from the bot's seed.

### 5.3 Aim error

Error cone σ(t) = e∞ + (e0 − e∞)·exp(−t / τ) from the moment the target was acquired (e0 3°, e∞ 0.5° at skill 0.5;
τ 0.6 s), plus `+1.5°` while the bot moves and `+0.5°/(m/s)` of target crossing speed. **Lead** from the weapon's
`InitialSpeed`, **drop** from its gravity (the same ballistic constants the weapon sim uses). Recoil: the bot
compensates `skill · 100 %` of the weapon's kick per shot. Never a damage multiplier (research §3.6). Skill per bot
from its seed: uniform 0.3–0.8.

### 5.4 Movement (B2)

WalkerBrain on the Detour nav (S1). Goals: (1) the last known enemy or heard shot, (2) a "contact heat" grid (16 m
cells, decayed 30 s) of recent deaths and shots, picked with weight, (3) a random main-component point. While engaged:
strafe 2–6 m, crouch at > 40 m for skill > 0.5, keep the target in the fire cone; retreat to break line of sight below
30 hp and wait for regen. Pre-round: no movement (input restriction applies to bots too). Stuck detection from S1
(no progress 3 s → repath, 10 s → a new goal); a stuck bot is never teleported.

### 5.5 Fighting

M2a's fire logic: bursts by range (3–6 rounds, single shots beyond 60 m), reload behind cover when < 30 % and no
target, sidearm when empty, grenade (M67) at a target out of sight behind cover within 15–35 m, at most once per 20 s.
Knife at < 2 m from behind. The kit is the rules' draw (§3.2: assault 40 %, the others 20 % each), read through
`ServerView::kit(slot)`; the brain never chooses it.

### 5.6 Revive

An assault bot not engaged for 2 s sees a downed teammate (via `ServerView::downed()`, #10) within 30 m and
`until_tick` − now > travel time + 1 s, where `until_tick` is the real window of §3.4 (13 s for a waiting human, up
to the 8 s revive wait for a bot that died near an assault teammate): it paths there, switches to slot 2, faces the corpse and fires once within
1.8 m at the corpse centre the rules test (§3.6); it gives up when shot at or when the man-down ends. Pre-round and
round-end: the brain idles while `ServerView::input_restricted()` or `phase()` says so (the server would drop the
inputs anyway, `[cmd]`).

### 5.7 Budgets and seeds

* 16 v 16 (32 bots): brains ≤ 0.8 ms/tick p99; 64 bots: ≤ 1.5 ms/tick p99, perception ≤ 0.2 ms/tick p99 (research
  §3.9: 0.4–1 ms for 64). Measured once in `host.tdm.perf` (E8, timing).
* Seeds as M2a §4.4: bot seed = `match_seed ^ slot`, each life draws its kit, skill and goal stream from
  `splitmix64(bot_seed ^ life_index)`. No wall clock, no unordered iteration: E6 replays bit-equal.

---

## 6. Audio A3

### 6.1 HDR window (`bf3/audio/hdr_window.h`, audio thread, inside `MixerCore`)

The HdrSetting [verified: gap-r1-audio-system.md §6.3]: WindowSize 45, WindowMinTop 80, WindowMinBottom 35, top
attack 0.05 s, top release 0.035–0.85 s on a spline, bottom attack 0, bottom release 0.025, Headroom 5,
AllowedOvershoot 3. Per mix block:

* top target = max(WindowMinTop, the loudest active voice's loudness_db (sound_program.h:231)); the top moves towards
  it with the attack / release;
* bottom = max(WindowMinBottom, top − WindowSize);
* each voice's gain = `loudness − top − Headroom` dB (the headroom applies to the amplitudes, not to the window top:
  gap-r1-audio-system.md §6.3, and today's path already subtracts it the same way, `loudness − window_top_db −
  window_headroom_db`, patch_instance.cpp:369);
* voices below `bottom` are suspended (silent, still advancing, the plan's "suspended").

Check against E10 (plan §7.7): a 71 dB footstep alone → top max(80, 71) = 80, bottom max(35, 35) = 35; a 110 dB shot
→ top 110 after the 0.05 s attack, bottom 65, so a 63 dB source is suspended. Revision 1 added the headroom to the top
target (115 / 70), which would have failed E10's 110. This replaces the fixed `window_top_db` gain
(audio_config.h:31–32, patch_instance.cpp:369, R5); with kFlagEnableHdr off a patch keeps today's path. CompressFactor 0.5, DischargeFactor 3.35 and MaxAllowedEnergy 110 have
unknown formulas: not applied, `measured = false`.

### 6.2 Scopes and culling (`bf3/core/audio/sound_scope.h`, `bf3/audio/voice_cull.h`)

33 scopes [verified]: e.g. Bullet_Impact DuplicateSpawn 1 m / 0.4 s, keep closest 5; Explosion closest 10;
Death_Scream max 4. 811 of 1,295 patches have no scope, including every handheld weapon. The order (§11.3 of the
research): (1) pre-cull a new one-shot below the previous block's window bottom − 3 dB; (2) scopes (duplicates within
the radius and time, then the closest N, then the max count); (3) over the voice cap, drop the lowest loudness ×
priority; loops become pending (resume when a voice frees), one-shots are dropped. The router's three limits
(weapon_audio.h:55–58) become one `MixerConfig::max_voices = 128`; the router keeps its per-source bookkeeping.

### 6.3 Profiler

`MixerStats` (#15) gains the mix CPU time per block (`steady_clock` around the mix call,
nanoseconds), a 16-bucket histogram, the most live voices in a block, the culled counts per step, suspended voices
and the window edges. `Bf3GameAudio` shows them in the dev overlay (F3). E11 reads them in M2a au3's 64-shooter
render (`audio.game.underruns`, a timing row, so the CPU share is read on a quiet machine): % of a core = mix_ns /
block duration (512 frames at 48 kHz = 10.67 ms).

**How the M2a budgets carry over.** M2a's E14 gated the client frame in the 64-bot firefight scene (frame p99
≤ 16.7 ms, M2a main-thread additions ≤ 3.0 ms, 0 underruns; measured 6.06 ms / 0.906 ms / 0). E11 measures the audio
thread's own share (≤ 10 % of a core), 0 late blocks and the 128-voice cap offline with the same 64-shooter load the
au3 row already renders, so it needs no window. `godot.m2a_firefight` stays in the default window set unchanged; the
wave-2 window phase runs it anyway, so E14's 0-underrun gate also sees the A3 mixer in the real stream at no extra
cost. M2b re-measures none of E14's frame gates.

---

## 7. Threads

| thread | M2b work |
|---|---|
| loader | `build_logic_graph` in `load_shared_level`; HdrSetting and scope tables with the audio bank |
| server (60 Hz) | `TdmRules::step` (logic drain, natives, regen, man-down, the defib test over the tick's shots, orders) in `sv_rules`; spawn LOS rays (≤ 16 per spawn); `TdmBrain` sources at step 3, perception rays inline (≤ 64 bots × 8 rays / 6 ticks ≈ 86 rays per tick) |
| main | `fill_hud` M2b parts, the new Controls, death camera, crowd corpse |
| audio | HDR window, culling, profiler counters (written by the audio thread, read by main as relaxed atomics) |

No new thread and no new lock. The logic runtime is single-threaded beside the rules (logic_native.h:95).

## 8. Budgets

| budget | value |
|---|---|
| server tick, 64 bots TDM | ≤ 4 ms p99 (E8); rules step ≤ 0.1 ms; logic drain (TDM: a few deliveries a tick) ≤ 0.05 ms |
| bots | 32: ≤ 0.8 ms; 64: ≤ 1.5 ms; perception ≤ 0.2 ms (E8) |
| audio | ≤ 10 % of a core p99 with 64 bots firing, 0 xruns, ≤ 128 voices (E11) |
| client frame | the M2a firefight frame + ≤ 0.3 ms for the new Controls (kill feed, scoreboard drawn only while held) |
| load | ≤ 5 s warm (E12) |

## 9. Memory, determinism, hostile input, caching

### 9.1 Memory [inferred]

Logic graph + runtime ≈ 2 MiB (5,047 nodes); `RoundNatives` < 64 KiB; per-slot rules state 64 × 128 B; bot detection
matrix 64 × 64 × 8 B = 32 KiB; contact heat 64 × 64 cells × 4 B; HDR and scope tables < 64 KiB; 128 voices of mixer
state (the instance size × 128). The UI: no new atlases (ui1's are resident).

### 9.2 Determinism

The logic runtime draws from `LogicRuntimeOptions::root_seed = match_seed`; the natives iterate slots and nodes in index
order; AutoTeam and spawn choices are pure functions of (state, match_seed, tick, slot); bots as §5.7. E6 checks the
logic trace hash and the per-tick world hash over the stepped run; it does not compare snapshot bytes, because the
snapshot's `echo_hold_100us` comes from the wall clock (game_server.cpp:551) and the jitter buffer stamps arrivals
with `net::monotonic_ns()` (:870). The stepped fixture delivers each command before the tick it is for, so the
buffer's contents, and with them the simulation, do not depend on the wall clock. The audio and UI are client-only
and not part of it.

### 9.3 Hostile input

The server never trusts a client: SpawnRequest validation (§3.4, counted refusals, a spectator slot ≥ 64 included, a `spawn_choice` other than 0);
the kit index range; a defib shot is tested by the server against its own downed list and corpse positions, so a
client cannot revive an enemy, a living soldier or an expired corpse; the input restriction applies server-side to the received buttons;
`PlayerList` names are server-truncated to 16 bytes of valid UTF-8 (a hostile `Hello` name cannot overflow a row);
the decoders bound every count (≤ 64 rows, ≤ the v2 event limits) and reject trailing bytes. `net.proto.fuzz` covers
the new messages (E7).

### 9.4 Caching

Nothing new is cached. The graph build is 0.008 s and the load 0.145–0.165 s [verified]; HdrSetting and scopes are a few
hundred instances read with the bank. Nav caches (S1), audio caches (au), UI atlases (ui1) stay as they are; nothing is
written to the install.

---

## 10. Track scope summary

| key | wave | one line |
|---|---|---|
| ru | 1 | TDM rules: `RoundNatives`, `TdmRules`, health / regen, immunity abort, respawn policy, spawn selection, revive, the defib kit option and the weapon-sim gadget rule, the GameServer wiring, the stepped fixture, E13 |
| nt | 1 | protocol v3 codecs and the client session: messages, `send_spawn_request`, frame fields, the client-side restriction, unspawn, the shot-id fix, fuzz |
| bb | 1 | `TdmBrain`: perception, target choice, aim model, movement, fighting, revive; the end-to-end stepped rows; the optional `play.cmd --tdm` preview |
| au | 1 | audio A3: HdrSetting and scope readers, HDR window, culling, 128-voice cap, profiler, the A3 checks in `audio.game.underruns` |
| ui | 2 | HUD model additions and the screens: kill feed, scoreboard, spawn screen, kill card, pre-round, round end |
| cam | 2 | death view (corpse to killer), corpse time, revive stand-up in the crowd, the defib in hand |
| gx | 2 | the M2b Godot scene, input, `play.cmd`, the Godot checks (E9, E12), README; the one integration |

Cut to keep the tracks small (none is in the plan row): the `m2b_hud.tscn` demo scene, the `bf3dump tdm` commands
(`host.tdm.bots` prints the E2 table; the TTK report), the `bf3dump audio hdr / scopes` commands (`audio.hdr.unit`
prints what it reads), score popups, nametags, spawn line-of-sight with 64 rays (16 now), and the enclosed-spawn
filter for humans with its E2 report (the user said the spawn room is not important now). cam stays a track of its
own because the orchestration keeps 3–5 tracks per wave and it owns disjoint files (crowd, anim, camera); it runs in
parallel with ui, so it adds no wall time. If the budget is tight, folding it into ui is the first saving (§13 Q17).

---

## 11. Implementation waves and tracks

Two waves of parallel git worktrees. Each wave starts with a **contract commit** by the integrator (W1.0, W2.0): every
header in that wave's interface column with declarations only, the struct-member and enum insertions of §2.6, and a
compile-only ctest (`m2b.contract`, `m2b.contract2`) that pins the sizes of `PresentEvent` (64), `HudModel` (its new
size), `net::PlayerRow`, and the enum values of §2.6 #5 and #11. Tracks fill in the bodies. W1.0 also retires the
three over-budget M2a window rows from the default window set (§13 Q8): it adds the label `retired` to `godot.m2a`,
`godot.m2a.kits` and `godot.m2a.play` (tests/CMakeLists.txt:1333–1336) and `"exclude": { "label": "^retired$" }` to
the `dev-window` preset's filter (CMakePresets.json:83); the rows stay runnable by name (`ctest -R ^godot\.m2a$`),
and no merge pass of M2b runs them.

**The one production stub.** So that ru, bb and nt link on parallel branches, W1.0 also defines the new v3 encode
functions as stubs returning an empty byte vector and the decoders as returning an error "v3 codec: nt", each line
marked `// M2b W1.0 stub`; a sender skips an empty vector. nt replaces those bodies (R9). No other real symbol gets a
stub: a track that needs a part not yet merged uses a test-local fake in `namespace m2b_fake` and deletes it when
rebasing.

**Shared files are edited additively** (the M2a §15 rules): each track appends a block headed `# M2b <key>` /
`// M2b <key>` at the end of each library's `CMakeLists.txt`, `tests/CMakeLists.txt`, `register_types.cpp`,
`project.godot`, `README.md`. Named insertions are lines marked `// M2b <key> [tag]` (§3.2 lists ru's); no existing
line changes except the named replacements:

| # | owner | file | what |
|---|---|---|---|
| R1 | ru | game_server.cpp:236 | the spawn choice call → the rules' chooser over the list :227–230 selected, when rules are active |
| R2 | ru | game_server.cpp:330 | `s.team = 1` → AutoTeam's team when rules are active |
| R3 | ru | game_server.cpp:662–665 | the fall-death respawn lines → `sv_policy()` |
| R4 | ru | logic_native.cpp:70–80 | the binding table gains the rows of §2.6 #5 (the old rows unchanged) |
| R5 | au | patch_instance.cpp:369 | the fixed-window gain → the HDR window's gain for HDR patches |
| R6 | au | weapon_audio.h:55–58 and its router | the three voice limits → the mixer's cap |
| R7 | ru | game_server.cpp:1206–1208 | `ps.kit = slot % kKitCount` → the rules' team, kit (or `kKeepKit`) and `on_spawned` when rules are active |
| R8 | ru | game_server.cpp:986–1001 (`Sink::death`) | reordered: `sv_policy()` first, then the stamped copy pushed, then `rules->death` |
| R9 | nt | protocol.cpp, the W1.0 stub bodies | the real v3 codecs |
| R10 | ru | game_server.cpp:859–861 (Ready) | no `want_spawn` for a deploy-screen slot when rules are active |
| R11 | ru | src/bf3_sim/src/weapons/weapon_sim_def.cpp:487–494 | `MagazineCapacity == -1` → a gadget without a magazine (#22) |
| R12 | ru | tests/weapons_tests.cpp:295 | the hostile `MagazineCapacity` case −1 → −2 |

**Test files, one owner each:** `host_tdm_server_tests.cpp` (ru: `host.tdm.server` with the case groups `health*`,
`respawn*`, `revive*`, `hostile*`, and `host.tdm.client`), `host_tdm_round_tests.cpp` (bb: `host.tdm.round`, `.bots`,
`.replay`; `host.tdm.perf`), both in one executable `test_host_tdm` so each row loads the level once and the rows split
with `openbf3_case_row` (docs/testing.md §6); the defib case in `weapons_tests.cpp` (ru, one additive case); the v3
block in `net_proto_tests.cpp` and `net_proto_fuzz.cpp` (nt); `audio_hdr_tests.cpp` (au: `audio.hdr.unit`) and an
additive A3 block in `audio_router_tests.cpp`'s 64-shooter case (au: E11); `ui_hud_tdm_tests.cpp` (ui:
`ui.hud.tdm.unit` and `ui.hud.tdm.install`); `tests/godot_m2b*.cmake` (gx). `tests/tdm_fixture.h` (#21) is declared in
W1.0; ru writes its body, bb adds counters to it additively.

### Wave 1

| key | title | owns (creates / edits) | interfaces | depends on |
|---|---|---|---|---|
| ru | TDM rules and server flow | new `bf3/sim/logic/round_natives.h` + cpp; R4; additive block in `spawn.{h,cpp}` (`choose_spawn_scored`); `soldier_motor.{h,cpp}` (#19, `[abort]`) and `motor_tuning.cpp` (`[abort hash]`); R11 in `weapon_sim_def.cpp`, R12 and the defib case in `weapons_tests.cpp`; new `bf3/host/tdm_rules.h` + cpp; game_server.{h,cpp}: the tags of §3.2 (`[cfg]` `[sv]` `[init]` `[tdm bots]` `[add]` `[cmd]` `[rules]` `[orders]` `[recv]` `[snap]` `[view]`) and R1–R3, R7, R8, R10; `level_load.{h,cpp}` (#3, the `tdm` load); `kits.{h,cpp}`, `gunplay.h` (#7, #8, the #9 overload) and the `[kits]` insertion in `gunplay.cpp`; `server_view.h` (#10); `config/tdm_tuning.cfg`; the body of `tests/tdm_fixture.h`; `host_tdm_server_tests.cpp` (E3, E4, E5, the server half of E7; E13) | #1–#10, #19–#22 | nothing to start (the W1.0 codec stubs let it link); its wire-level cases (the peer decoding Spawned / MatchState) run after nt is on main, before ru merges |
| nt | protocol v3 and client session | R9: `protocol.{h,cpp}` v3 codecs (#11); additive blocks in `client_session.{h,cpp}` (decode MatchState / PlayerList / ReviveEvent into the frame, `send_spawn_request`, the restriction applied to predicted commands, the `revived` spawn, stop predicting at `unspawn_tick`, `rules_flags` check); `presentation_events.{h,cpp}` (`Revive`); the shot-id reuse fix on the client path with a row; `net_proto_tests.cpp` block, `net_proto_fuzz.cpp` cases (the wire half of E7). **No game_server edit** (the slot limit is ru's spectator rule) | #11, #12 | none; merges first |
| bb | TDM bots | new `bf3/sim/bots/perception.h` + cpp, `aim_model.h` + cpp; new `bf3/host/tdm_brain.h` + cpp (`add_tdm_bots`; reads `ServerView`, incl. `kit()` and `downed()`); `config/bot_tuning.cfg`; additive counters in `tdm_fixture.h`; `host_tdm_round_tests.cpp` (E1, E2, E6 functional; E8 timing, not run on the branch); an additive `--tdm` block in `level_session_m2a.cpp` (below) | #13 | perception and aim units alone from the start; the brain against `ServerView` fakes; E1, E2, E6 after ru and nt are on main (bb merges last in wave 1) |
| au | audio A3 | new `bf3/core/audio/hdr_setting.h` + cpp, `sound_scope.h` + cpp; new `bf3/audio/hdr_window.h` + cpp, `voice_cull.h` + cpp; additive blocks in `mixer_core.{h,cpp}` (#15), R5, R6; the profiler readout in `game_audio.{h,cpp}` (one block); `audio_hdr_tests.cpp` (E10); the A3 block in `audio_router_tests.cpp` (E11's checks) | #14, #15 | none |

**bb's `--tdm` preview (optional, untested).** `tools\play.cmd --tdm` on the M2a scene turns the rules on (no deploy
screen: the M2a automatic respawn, the KitSelect kit), sets `SharedLoadOptions::tdm` for the server's and the
client's load (or the client is refused by the `rules_flags` check) and sets `GameServerConfig::tdm_bots` instead of
the FirefightBrains. The M2a scene has no TDM HUD, so the block prints one console line on every `MatchState` change
(phase, the two counts, the kill limit, the winner). It is compiled, not a row: it is a preview for the user between
the waves, labelled as such in `play.cmd --help`, and nothing gates on it. If it does not work, the user loses
nothing: the wave-2 scene is the real path.

### Wave 2 (starts when wave 1 is on main)

| key | title | owns (creates / edits) | interfaces | depends on |
|---|---|---|---|---|
| ui | TDM HUD screens | `hud_model.h` additions (#16); additive block in `hud_feed.{h,cpp}` (kill feed, players, spawn screen, kill card, pre-round, round end from the frame); new `src/bf3_godot/src/hud_killfeed.{h,cpp}`, `hud_scoreboard`, `spawn_screen`, `kill_card`, `round_end`, `pre_round`; `ui_hud_tdm_tests.cpp`: `ui.hud.tdm.unit` (data-free: the HUD model from recorded frames, ≤ 2 s) and `ui.hud.tdm.install` (every string, font and icon lookup resolved against ui1 / ui2 on the install, ≤ 15 s, with install.unchanged; the split of M2a's `ui.hud.feed.unit` / `.install`) | #16 | nt (frame fields, on main) |
| cam | death view and 3p corpse | new `src/bf3_godot/src/death_camera.{h,cpp}`; additive blocks in `game_crowd.{h,cpp}` (corpse 15 s, revive stand-up) and `crowd_eval.h` (#17); the defib's slot-2 unlock in the viewmodel set if it resolves (§4); a "revive" case in `anim.s3.unit` | #17 | nt (Revive event, own death data, on main) |
| gx | Godot integration | new `m2b_game.{h,cpp}`, `level_session_m2b.{h,cpp}` (rules on, deploy screen, `tdm_bots`, feeds ui, cam, audio), `m2b_check.{h,cpp}` (`--m2b-check=play`, `--kill-limit`, `--preround`); additive blocks in `input_mapper.{h,cpp}` (Tab, Enter / click Deploy, the kit keys, KEY_3 → slot 2 and slot 2 in the wheel order at input_mapper.cpp:279); `scenes/m2b_metro.tscn`; `tools/play.cmd` (new defaults; `--m2a` for the old scene); `tests/godot_m2b.cmake`, `godot.m2b_load`; README "M2b" | #18 | all of wave 1; ui and cam (their parts of the scene are skipped with a reason until they land) |

### 11.1 Integration order and the lean test plan

1. **W1.0** contract commit (integrator): the wave-1 headers and insertions of §2.6 #1–#15, #19–#22, the codec stubs,
   the `tests/tdm_fixture.h` declarations, `m2b.contract`, and the `retired` label of Q8.
2. **nt** merges first; **au** and **ru** merge when done (ru after its wire-level cases ran on top of nt, and after its
   acceptance asan run, which also covers nt's fuzz cases, E7); **bb** last, after it ran E1, E2 and E6 on top of ru
   and nt. Weekly-usage planning: nt, au and ru run in parallel; bb's units run in parallel too, but its end-to-end
   rows wait for ru.
3. **Wave-1 merge pass** on main, **once**, after bb's merge (the merge workflow batches the wave's four merges and
   does not run a pass per merge): the `timing` phase only. It measures E8 (`host.tdm.perf`), E11
   (`audio.game.underruns` with au's A3 checks) and E13 (`host.tdm.client`), together with the existing timing rows.
   No window phase: wave 1 changes no Godot scene a window row would see except au's mixer, and E11 measures that
   offline. After it, `tools\play.cmd --tdm` is the optional preview above.
4. **W2.0** contract commit: #16–#18, `m2b.contract2`. **ui** and **cam** in parallel, then **gx**. gx is the one
   integration: on its branch it runs only functional rows (its cmake registration, the headless parts of the M2b
   check, `ui.hud.tdm.*` and `anim.s3.unit` as part of the dev pass); it never runs a window row.
5. **Wave-2 merge pass** on main, **once**, after gx's merge: the `window` phase only, user away: E9
   `godot.m2b.play`, E12 `godot.m2b_load`, and the rest of the default window set (the three retired rows excluded;
   `godot.m2a_firefight` included, so E14's 0-underrun gate sees the A3 mixer). No timing phase: by design wave 2 adds only Godot-side code, HUD model structs and a
   new crowd constant, which no timing row's measured path runs. The rule the merge workflow applies: the timing phase
   runs in the wave-2 pass only if the wave-2 diff changed a source file that a timing row links and executes; it
   then runs only those rows.
6. **One verifier** then reads one log of each kind once: main's dev pass after the wave-2 merge (or, when the §7
   ledger rule skipped it, gx's accepted branch dev pass of the same tree), which holds every functional row (E1–E7,
   E10, the units); ru's acceptance asan log (E7's asan half); the wave-1 timing log (E8, E11, E13); the wave-2
   window log (E9, E12). It checks each row against its threshold (no reruns except a row that failed for a stated
   environmental reason, per the noise policy) and writes `docs/design/m2b-results.md`. There is no gate pass and no
   acceptance step after it.
7. **The user's playtest** (§1.1).

**Without rules** the GameServer keeps M2a's `FixedRespawn`, `choose_spawn`, team 1, Ready spawning and kit `slot % 4`,
and the load without `SharedLoadOptions::tdm` makes the M2a kit table, weapon table, motor and hashes, so every M2a row
stays as it is.

**Test plan (lean).** Each track's rows are listed in its table row and run once on its branch (functional rows only;
a branch never runs a `timing` or `window` row). New rows: `host.tdm.server` (≈ 15 s, one load for E3–E5 and E7's
server cases), three stepped dev rows with stated COSTs below the 60 s cap (`host.tdm.round` ≈ 25 s, `.bots` ≈ 15 s
with PROCESSORS 3, `.replay` ≈ 10 s; §1's estimate), `audio.hdr.unit`, `ui.hud.tdm.unit`, `ui.hud.tdm.install`, two
timing rows (`host.tdm.perf`, `host.tdm.client`), two window rows (`godot.m2b.play` ≈ 110 s, `godot.m2b_load`), new
cases in `sim.weapons.unit`, `net.proto*` and `anim.s3.unit`, and new checks in one existing timing row
(`audio.game.underruns`). Each timing row is measured once (the wave-1 pass), each window row once (the wave-2 pass).

---

## 12. What the user must judge

| # | judgement | recommended |
|---|---|---|
| J1 | The defibrillator in every assault loadout when TDM rules are on (the data defaults gadget1 to none and gates the defib behind scoring) | **yes**: revive is part of the M2b goal; it sits in the empty gadget slot 2 and replaces nothing, so there is no toggle |
| J2 | Pre-round countdown of 10 s (RoundRestartCountdown) with the input freeze | **yes** in play; `--preround=0` for quick tests |
| J3 | Ragdolls deferred (m2a.md §16 Q13 said "ragdolls in M2b"): the corpse stays prone for 15 s instead | **defer** to M5 (physics); it is not needed to play |
| J4 | Revives take a kill back from the enemy team's count | **yes** (BF3 TDM behaviour as remembered, `measured = false`) |
| J5 | Bot difficulty: skill 0.3–0.8, e0 3°, e∞ 0.5°, reaction 0.25–0.6 s | **as stated**; tune after the playtest in `bot_tuning.cfg` |
| J6 | 31 bots as the playable default (16 v 16 with the player) | **yes**; `--bots` keeps working |
| J7 | Does the game feel playable (the playtest of §1.1) | the user's call; it is the milestone's real exit |

## 13. Open questions (recommended default in bold)

| # | question | default |
|---|---|---|
| Q1 | PlayerKilledDelay 5 s = the earliest respawn? | **yes**, `measured = false` |
| Q2 | Does a revive undo the enemy's TDM kill? | **yes**, killer keeps the personal kill (J4) |
| Q3 | Pre-round: does StartRound wait for the countdown even with bots filling the server? | **yes**, 10 s once ≥ 4 players |
| Q4 | RotateTeamOnNewRound: swap every player's team at the restart? | **yes** (the data says true); dev flag to keep teams |
| Q5 | AutoTeam with one human: may the balance move the human? | **no**, it moves bots first |
| Q6 | Hearing radius without AILoudness (−1 in weapon patches) | **60 m**, `measured = false` |
| Q7 | Enforce the TDM combat area (CombatAreaTrigger) in M2b? | **no**; the nav and the static world bound Metro; M4 adds out-of-bounds |
| Q8 | Retire `godot.m2a.play`, `godot.m2a` (391 s) and `godot.m2a.kits` (159 s; both over the 120 s window budget) from the default merge window set? | **yes, in W1.0** (the `retired` label, §11), so no M2b merge pass runs them; keep them runnable by name; `godot.m2a_firefight` stays (E14) |
| Q9 | HDR CompressFactor / DischargeFactor / MaxAllowedEnergy | **not applied**, `measured = false` |
| Q10 | Suppression aborting regen | **off** until a suppression system exists |
| Q11 | Enclosed spawns: filter them for humans? | **no, deferred**: the user said the spawn room is not important now; bots keep S1's main-component list (`spawn_in_main`), humans use all 102 points with the scored rule of §3.5 |
| Q12 | Kill-limit override for tests and play | **`--kill-limit=N`**, dev only, not in MatchInfo's digest |
| Q13 | The EntryInputActionEnum values of the immunity abort list beyond Fire and movement (§3.3) | **unmapped** until a name source exists; listed in the load notes, `measured = false` |
| Q14 | E11 offline on au3's 64-shooter render instead of in a windowed scene | **yes**: the same 64-shooter load, a timing row measured once in the wave-1 pass, no window; `godot.m2a_firefight` (64 bots in front of the observer) still runs in the wave-2 window phase as E14 |
| Q15 | KillCounter's TeamKillWeight / NeutralTeamWeight are not decoded | **team kills count 0**, `measured = false`; ru decodes them if cheap |
| Q16 | A bot that dies near a living assault teammate waits for a revive (§3.4) | **yes**: 20 m, up to 8 s, `measured = false`; without it bot revives are rare (the window would be 5 s) |
| Q17 | Fold cam into ui to save one track's start-up | **no** while the 3–5-tracks-per-wave rule holds; yes if the weekly budget is tight (cam's work is small and on disjoint files) |

## M2b bb: contract additions (wave 1, for the integrator to confirm)

<!-- # M2b bb -->
Two named insertions of track bb into ru's files, made after the w1 merge (as built: docs/design/m2b-bb.md §2). Each
is in its own commit, every line marked `// M2b bb [tag]`; no existing line changed.

| tag | file | what | why |
|---|---|---|---|
| `[bot spawn]` | game_server.h (after `add_local_soldier`), game_server.cpp (an `// M2b bb` end block) | `int GameServer::add_local_bot(std::unique_ptr<CommandSource>, int team)`: `add_local` with the bot mark; `add_tdm_bots` uses it instead of `add_local_soldier` (§2.6 table above) | §3.5 and Q11 want the TDM bots on S1's main-component list (`spawn_in_main`), which `spawn()` selects only for a slot with the bot mark; `add_local_soldier` sets none. Without it 1-3 bots per team spawned on nav islands on Metro and E2's K/D left its band |
| `[spawn transform]` | round_natives.cpp, `RoundNatives::apply`, the `default:` case of the meaning switch | a CharacterSpawn / AlternateSpawn `BlueprintTransform` property input is handled (no effect) | the two CharacterSpawnReference HQ nodes receive it at tick 1; Recorded, it failed E1's coverage check (§3.1) |

## M2b integ-w1: integrator decisions (wave 1)

<!-- # M2b integ-w1 -->
Made on `integ/m2b-w1` (README "Sixth integration").

* **Confirmed: bb's `[bot spawn]` and `[spawn transform]`** (the block above). `[bot spawn]` puts the TDM bots on S1's
  main-component list, as §3.5 and Q11 intend. `[spawn transform]` handles a property input that has no effect. Both are
  additive and marked.
* **Confirmed: au's cap counts busy sampler voices, not patch instances** (m2b-au.md "The cap counts sampler voices").
  A voice is one playing sound, and an instance-level cap of 128 made the 64-shooter mix 3-5 times slower than M2a's.
* **E13 as built:** 31 TDM bots instead of 7. The TDM bots make about 0.02 kills per bot per second, so 7 bots reached
  only 3 / 1 of the kill limit 5 in 30 s. The scripted death is a 46 m drop onto `sim::find_drop_spot`. The row's other
  thresholds are unchanged.
* **Named insertion `[ray any]`** (E8): `CollisionWorld::ray_any` (collision_world.h / .cpp), and perception.cpp's two
  line-of-sight rays call it. Every line is marked `// integ/m2b-w1 [ray any]`, and the results are identical to `ray()`.
* **Named insertions `[xas groups]`, `[slot fast]`, `[lazy zero]`, `[biquad pair]`** (E11). The wave-1 merge pass
  failed E11 on the mix share: p99 in the < 0.20 bucket, p50 < 0.15, max block 2.54 ms. These four insertions fix it
  without changing a sample:
  - `[xas groups]`: `decode_xas_frame` (sound_stream.cpp) runs the four group predictors together, in SSE2 lanes on
    x86-64; `StreamDecoder::read` copies mono and stereo with a fixed stride.
  - `[slot fast]`: `render_slot`'s resampler loop (patch_sampler.cpp) keeps its history and phase in registers, with
    the two channels in two SSE2 lanes, while the slot has no fade and its input is buffered.
  - `[lazy zero]`: `PatchInstance::render` zeroes a voice's buffer only when something writes or reads it, so the
    silent voices of a weapon patch cost neither the fill nor au's `[quiet]` scan.
  - `[biquad pair]`: one loop runs both channels' biquads.

  The 64-shooter mix is bit-identical: an FNV-1a hash over every mixed block was 4a27abc862d46310 before and after.
  The audio rows pass, in dev and under asan. Re-measured under the quiet lock: wall 4.05 s (was 8.64 s), max block
  1.505 ms, p50 and p99 < 0.08, 0 late blocks, 128 live voices at most. E11 passes.
* **Open:** in wave 2, `fx::FxTriggers::sane` should accept `PresentKind::Revive` (m2b-nt.md).
