# M2b ru: TDM rules and the server flow (as built)

Track **ru** of M2b wave 1, branch `m2b/ru` from `m2b/w1` 01a26e8. Design: [m2b.md](m2b.md) §3.1-§3.6, §2.6 #1-#10 and
#19-#22, the R table of §11, E3-E5, the server half of E7, and E13. This file records what was built, where the code
differs from the design, the measured gates and one command per gate.

## 1. What landed

| piece | files | commit |
|---|---|---|
| `RoundNatives`: KillCounter, PreRound, HumanPlayer, AutoTeam, InputRestriction, LevelControl, RoundOver as natives of the LogicRuntime; `RoundNativeParams`; `choose_spawn_scored` | `bf3_sim` round_natives.{h,cpp}, the `M2b ru` blocks of spawn.{h,cpp}, the bf3_sim CMake block | b54a3f0 |
| R4: the binding rows of §2.6 #5 | logic_native.cpp | b0dada3 |
| #19 `[abort]` / `[abort hash]`: the spawn-immunity abort in the motor, hashed only when set | soldier_motor.cpp, motor_tuning.cpp | dcf946b |
| #22 R11 / R12: `MagazineCapacity == -1` is a gadget without a magazine | weapon_sim_def.cpp, weapons_tests.cpp | 1b4b365 |
| #9 the TDM kits: the defibrillator in assault slot `kKitGadgetSlot`, `KitTable::defib_def` (hashed only when set) | kits.{h,cpp}; gunplay.cpp `[kits]` | 46bef3a, 843d4b4 |
| #3 the TDM load: the logic graph, the natives' EBX fields, `tdm_tuning.cfg`, the data's scoring rows, the immunity abort | level_load.{h,cpp}, `bf3-opensource/config/tdm_tuning.cfg` | 4fa0948 |
| `TdmRules` (the `RespawnPolicy` and `CombatEventSink`), `TdmTuning` and its parser | tdm_rules.{h,cpp}, the bf3_host CMake block | 70ebddf |
| the GameServer side: `[sv] [init] [add] [cmd] [rules] [orders] [recv] [snap] [view]`, R1 R2 R3 R7 R8 R10 | game_server.{h,cpp} | 1ecaf73 |
| the fixture bodies (`load_tdm_level_once`, `ScriptedPeer`, `run_tdm`) | tests/tdm_fixture.h | f38a7f6 |
| `host.tdm.server`, `host.tdm.client`, `m2b.contract` ru switch | tests/host_tdm_*_tests.cpp, tests/CMakeLists.txt `# M2b ru` | 364524d |
| the defibrillator case in `sim.weapons.unit` | tests/weapons_tests.cpp `// M2b ru` | ae3c793 |

## 2. The TDM load (#3)

`SharedLoadOptions::tdm` builds the mode's `LogicWorld` the way `bf3dump logic` does (`expand_root_objects` with the
`LogicGraphOptions` caps, then `build_logic_graph`), so `SharedLevel::logic_digest` equals bf3dump's for the same mode
(Metro TDM: `6f6a0693b3924bde`, 0.02 s). The natives need EBX fields the graph does not carry, so the loader reads them
from the same `LogicWorld` into `SharedLevel::logic_natives` (`RoundNativeParams`): KillCounter MaxKillCount 100 /
EnemyWeight 1 / IgnoreAI false per team; PreRound RoundMinPlayerCount 4, RoundMaxPlayerCount 8,
RoundRestartCountdown 10; HumanPlayer PlayerKilledDelay 5; AutoTeam TamFullTeams, PlayerCountNeededToAutoBalance 8,
TeamDifferenceToAutoBalance 3, RotateTeamOnNewRound, AutoBalance; InputRestriction's booleans mapped onto
`net::restrict_bits` (allowed: Yaw, Pitch, Zoom, Jump, ChangePose, Reload, Prone; refused: Throttle, Strafe, Fire,
Sprint, MeleeAttack, ThrowGrenade). KillCounter TeamKillWeight is a struct the reader does not decode (Q15): the
cfg's `team_kill_weight = 0` stands in. A field the reader cannot find leaves the default and a note in
`SharedLevel::tdm_notes`.

The scoring rows (ID_SCORE_KILL 100, ID_SCORE_HEADSHOT 10, ID_SCORE_REVIVE 100) replace the cfg's values when the
data has them.

**The immunity abort (§3.3, #19).** `SoldierDef::abort_spawn_immortality_inputs` holds raw `EntryInputActionEnum`
values {12, 0, 1, 7, 30, 31, 1, 2, 11}. The loader probes the soldier blueprint's type info for the enum's names; on
Metro the type info has **no enum names**, so the cfg's `immunity_abort_map` decides: 0 and 1 (Throttle, Strafe) end
the immunity by moving, 7 (Fire) by firing; 12, 30, 31, 2 and 11 stay unmapped. The values are [inferred],
`measured = false`, and the load says so in a note: "immunity abort: by tdm_tuning.cfg's immunity_abort_map (the type
info has no enum names); mapped {0 1 7 1}, unmapped {12 30 31 2 11}". The result goes into `SharedLevel::motor`
(`protect_abort_buttons = kFire`, `protect_abort_move = true`), so `MatchInfo::motor_hash` carries it to the client.

## 3. config/tdm_tuning.cfg

The numbers the data does not decide, each a named default [inferred] with `measured = false` (the file's comments
name the [verified] ones that repeat bf3dump): the revive wait of a bot near an assault teammate (20 m, 8 s), the
restart estimate (60 s), revive_takes_kill (Q2), the defibrillator's corpse sphere (centre 0.3 m up, radius 0.6 m,
range 1.8 m [verified]), the living sphere (0.6 m), the scoring fallbacks, the bot kit weights (40 / 20 / 20 / 20), the
spawn scoring (40 m safe distance, the friendly block 3 deaths in 30 s within 30 m, 16 LOS rays at 1.3 m), team kill
weight 0, team rotation on restart, no suppression input to regen, and the immunity abort map. `parse_tdm_tuning`
bounds the text (64 KiB), the abort map (64 entries) and every value's range, and names the line of an error.

## 4. TdmRules (as built)

* **Per step:** `natives.apply(effects)`, the round flow (`tick_round`), `runtime.step()`; the server-to-runtime tick
  base is set lazily. Phase changes stamp `MatchState::round_start_tick` / `round_end_tick`; `UnSpawnAllSoldiers`
  orders every player Unspawn and sets `restart_tick` = now + `restart_estimate_s`; the restart in place makes a fresh
  runtime and natives, rotates the teams, zeroes the counts and scores and orders Spawn.
* **Respawn policy (§3.4):** a deploy-screen human gets nullopt (it spawns on its SpawnRequest) and revive_until =
  death + man-down (13 s, 780 ticks); a bot or a non-deploy human respawns at death + 300 (PlayerKilledDelay 5 s); a
  bot with a living assault teammate within 20 m of its corpse waits min(8 s, man-down) = 480 ticks instead. RoundOver,
  Restarting and slots >= 64 get nullopt. The policy is idempotent per death; `death()` asks it itself when the server
  did not.
* **Scores:** a kill of an enemy in PreRound / InRound scores 100 (+10 for a headshot) and counts on the killer team's
  KillCounter; the victim's death counts. A revive scores the reviver 100, takes the victim's death back and (with
  revive_takes_kill) the killer team's kill (`RoundNatives::kill_undone`).
* **Regen (§3.3):** 600 ticks after the last damage (a hit, or the motor's fall damage), +regeneration_rate / 60 per
  tick up to max health (Metro: 10 s, 5 hp/s, from the SoldierDef).
* **The defibrillator (§3.6):** every defib `ShotEvent` once (a per-shooter shot_index cursor), from a living shooter:
  the segment origin + direction x range (1.8 m) against each corpse's sphere, with a clear static ray to its centre.
  A downed teammate inside its window gets a Revive order (post_revive_health 20, no immunity); an enemy corpse, an
  expired one or a living soldier on the segment counts as refused, anything else as a miss.
* **SpawnRequest refusal order:** Spectator, RateLimited (twice in a tick), Phase (no deploy screen, RoundOver /
  Restarting, faulted rules), Alive (alive or a request pending), BadKit (kit >= 4 or spawn_choice != 0), TooEarly. An
  accepted request becomes a Spawn order in the next step and forfeits the revive.
* **Bot kits:** drawn per life from splitmix64(match seed ^ slot ^ life << 32) by the weights.
* **Rows:** MatchState and the player rows are rebuilt in every step; the send decision compares them with the copies
  last sent (`mark_state_sent`, `mark_players_sent`).

Additions to the W1.0 declarations (all in tdm_rules.h, which ru owns): `RulesOrder::reviver`; RulesStats fields
appended after `rounds` (`reserved0`, `spawns_scored`, `spawns_enemy_within_10m`, `runtime_faults`);
`player_list(tick, full)`, `faulted()`, `natives()`, `mark_state_sent()`, `mark_players_sent()`; `TdmTuning`,
`ImmunityAbortEntry`, `parse_tdm_tuning`, `load_tdm_tuning`, `kTdmTuningPath`, `kMaxTdmTuningBytes`,
`kMaxImmunityAbortEntries`. level_load.h: `SharedLevel::logic_natives`, `tdm_tuning`, `tdm_notes` and
`SharedLoadOptions::tdm_tuning_cfg`.

## 5. The GameServer side

Each insertion is a named one (the tag on every line). The bodies that need the M2a `Sv` struct are out of line
(`sv_*` helpers in the `M2b ru [sv]` block at the end of game_server.cpp), because Impl's inline members cannot see
the incomplete `Sv`.

* `[init]`: rules without `SharedLevel::logic` or gunplay is an init error; the defib def goes into
  `GunplayConfig::defib_def`; `TdmRules::create` runs after the match seed and before the target bots, so every
  server-side soldier gets AutoTeam's team (`[add]`; team 0 = no soldier).
* R2 / `[add]`: AutoTeam's team for peers and server-side soldiers; `on_disconnect` leaves the rules.
* `[cmd]`: `apply_input_restriction` before the motor step.
* R3 / R8: the fall death and the damage death ask `sv_policy()` (the rules, else M2a's FixedRespawn); R8 then pushes
  a copy of the DeathEvent stamped with `earliest_respawn`, `revive_until` and the killer's health (0 for a fall), then
  calls `rules->death`.
* R1: the rules' scored chooser over the list `spawn()` selected, the M2a chooser on SIZE_MAX (counted).
* R7: the rules' team and kit (or `kKeepKit`) and `on_spawned` at every spawn.
* R10: a deploy-screen slot (or a spectator) is not spawned by Ready.
* `[rules]` / `[orders]`: `TdmRules::step` between `apply_rules` and the copy-back; its orders after the placements:
  Spawn (when not alive), Revive (`sv_revive`: a new life at the corpse with no immunity, the post-revive health,
  `Spawned{revived = true}` to the slot's peer, then `ReviveEvent` through the sink into CombatEvents and the rules'
  score), Unspawn.
* `[recv]`: SpawnRequest -> `request_spawn(slot, kit, choice, tick + 1)`; an undecodable one counts in bad_datagrams.
* `[snap]`: MatchInfo carries phase, logic_digest and rules_flags; a joining peer gets MatchState and a full
  PlayerList; MatchState goes on change, changed rows at most every 15 ticks. With the W1.0 codec stubs the encoders
  return empty and nothing is sent.
* `[view]`: `downed`, `phase`, `input_restricted`, `kit` from the rules (the defaults without them).

**Where it differs from the design.**
* A revive's score and the kill taken back show in the rows and MatchState one tick after the revive: the rows are
  the rules' step's, and the Revive order is applied after that step (the runtime's events go into the next step
  anyway). host.tdm.server checks them two ticks later.
* The DeathEvent stamps (respawn_tick, revive_until, killer_health) are in `CombatEvents::deaths` on the server, but
  the v2 combat codec does not carry them; they travel once nt's v3 codec lands.

## 6. Tests

`test_host_tdm` (tests/CMakeLists.txt `# M2b ru`), one Metro TDM load per process (`load_tdm_level_once`: 8.3 s, of
which the shared level 3.2 s, gunplay 2.1 s, logic 0.02 s, the bot nav cold on a per-process scratch cache 5.1 s).

* **host.tdm.server** (functional, no label, parallel-safe; exits 77 without an install): the GameServer stepped back
  to back on the test thread with `ScriptedPeer`s and scripted server-side soldiers; no sockets, no wall-clock wait.
  Cases `tuning`, `health*` (E3), `respawn*` (E4), `revive*` (E5), `hostile*` (E7, server half), `install unchanged`.
* **host.tdm.client** (label `timing`, RUN_SERIAL): E13 through ServerHost + ClientSession. Written against the W1.0
  declarations; it exits 77 with PENDING until `net::kProtocolVersion` is 3 (nt), and needs bb's `add_tdm_bots`. Not
  run on this branch (a timing row, docs/testing.md).
* `sim.weapons.unit`: the defibrillator case (a synthetic u_defib with the data's numbers).
* `m2b.contract`: `kRuLanded = true` (the `[abort hash]` and defib hash rows, `[init]` refusing rules without logic).

`tests/tdm_fixture.h` bodies, as built: `ScriptedPeer` connects in its constructor, sends nothing for a SpawnRequest
while the encoder is the W1.0 stub, stamps commands for `current_tick() + 1`, and keeps every decoded message;
`run_tdm` runs 64 slots with dev commands, a `BotFillConfig` with count 0 (the BotLevel for walking), `tdm_bots` only
when `bots > 0`, the peer's SpawnRequest every 30 ticks while dead (deploy screen), and a per-tick world hash over the
soldier states, the weapons states, the teams and the MatchState bytes. `TdmRunConfig::keep_trace_records` is not
wired (MatchRulesConfig has no trace option); `sight_violations`, `stale_aim_shots` are bb's and stay 0 here.

## 7. Gates (measured 2026-10-19, dev preset, m2b/ru)

Every host.tdm.server number is a tick count on the server (no wall clock), so it is the same on every run. One
command per gate (the dev build directory; `--test-case` takes the case name's prefix):

| gate | threshold | measured | met | command |
|---|---|---|---|---|
| E3 health | no regen before T + 600, then 1/12 hp per tick ± 1 tick; a new hit restarts the delay | first regen at T + 600 after both drops (79.98 hp left each), 61 steps of 0.08334 hp; SoldierDef 10 s / 5 hp/s | yes | `b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=health*` |
| E3 immunity | 120 ticks from the spawn; Fire ends it, moving ends it; the server equals a local `SoldierMotor` over the client's MotorParams | idle +120, Fire at +30 -> +30, move at +30 -> +30; 0 per-tick protect_ticks mismatches (the abort map is [inferred], §2) | yes | same as E3 health |
| E4 respawn (server) | DeathEvent respawn_tick = death + 300; bots respawn by themselves with the rules' kit; a deploy-screen Ready spawns nothing | +300 (respawned exactly at earliest_respawn), +480 next to a living assault teammate (revive wait), +300 for its enemy; gunplay kit == rules kit; R10: 0 spawns in 120 ticks | yes | `b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=respawn*` |
| E4 M2a hashes | a load without the TDM kits gives M2a's kit_table_hash, weapon hash, motor_params_hash | M2a kit `31b45dc7cc3f4271`, weapon `b8fd23d8ccfc4c63`, motor `6d75dd5f51d95090` (TDM: `838d62104f6fb483`, `ffccf2d398c49167`, `d251e770c41372ef`); recorded from this branch's M2a path, **not** from a main 47871fa build (§8) | partly | same as E4 respawn |
| E4 SpawnRequest (wire) | refused before respawn_tick, accepted at it, Spawned within 1 tick with the kit | PENDING (nt): the W1.0 encoder is empty; the refusal order itself is E7's | no (nt) | same as E4 respawn |
| E5 revive (server) | a teammate's defib inside the window: a new life at the corpse with 20 hp, enemy count -1, reviver +100; a living target and an enemy medic do nothing; no stale respawn | bot and peer victims: revived +78 / +72 ticks after the death, life 1 -> 2, 20.00 hp, 0.000 m from the corpse, no immunity, team 2 kills 1 -> 0, medic score 0 -> 100; living target 3 shots refused; enemy medic refused; life unchanged past the old respawn tick | yes | `b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=revive*` |
| E5 wire and deploy | Spawned.revived = 1 at the peer; after the window and after the victim deployed nothing happens | PENDING (nt): the v2 Spawned codec has no revived bit; the expired and deployed cases need a deploy-screen peer (SpawnRequest) | no (nt) | same as E5 |
| E7 (server half) | SpawnRequests counted by refusal: alive, too early, bad kit, twice in a tick, RoundOver, spectator; hostile v3 datagrams counted, nothing crashes | refusals on TdmRules: 10 requests, accepted 2, alive 2, too early 1, bad kit 2, phase 1, rate 1, spectator 1; 300 hostile datagrams, 300 counted bad, 0 runtime faults, 0 precondition failures; the RoundOver refusal is not exercised (§8); `parse_tdm_tuning`: 16 hostile lines refused with the line named, 3,000 mutations (1,261 still parse), no crash | yes (RoundOver: no) | `b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=hostile*` and `--test-case=tuning*` |
| E7 asan | 0 asan reports in ru's rows | `host.tdm.server` 127.1 s (all 6 cases), `sim.weapons.unit` 1.5 s, `m2b.contract` 0.1 s under the asan preset: 3 / 3 passed, no sanitizer report | yes | `cmake --preset asan`, `cmake --build b\asan --target test_host_tdm test_weapons test_m2b_contract`, `ctest --test-dir b\asan -R "^(host\.tdm\.server|sim\.weapons\.unit|m2b\.contract)$"` |
| E13 | the real client path (deploy screen, freeze, death stamps, RoundOver) <= 30 s | not run: a timing row (the merge pass), and it needs nt's v3 client and bb's brains; it exits 77 with PENDING until `kProtocolVersion` is 3 | no (nt, bb) | `ctest --preset dev-timing -R "^host\.tdm\.client$"` |
| defib weapon (#22) | u_defib builds with capacity -1; held 3 s: 3 shots, no reload, 0 damage | 3 shots in 180 ticks, magazine 1 -> 1, no reload event, 1 s apart | yes | `b\dev\tests\test_weapons.exe --test-suite=weapons-unit --test-case=*defibrillator*` |
| m2b.contract (ru rows) | `[abort hash]` and the defib hash change the hashes; rules without logic refused | pass (5 cases, 101 assertions; nt / bb / au PENDING) | yes | `ctest --test-dir b\dev -R "^m2b\.contract$"` |
| install.unchanged | the install unchanged after the TDM load and every case | `install.unchanged: clean (256 files)` | yes | `b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=install*` |
| branch pass | every functional row passes | 139 rows: 137 passed, 2 skipped (`core.cache_data`, `core.res`, as on m2b/w1), 0 failed; 193.4 s ctest wall; no budget violation (warnings as before); `host.tdm.server` 8.6 s (install-row budget 15 s; indicative only) | yes | `tools\build.cmd dev` |

## 8. Open issues and handoff

* **nt (wire).** These cases wait for nt's v3 codecs and are marked PENDING (nt) in host.tdm.server: the
  ScriptedPeer decoding Spawned.revived, MatchState and PlayerList; a SpawnRequest through the server's `[recv]`
  (accepted at respawn_tick with Spawned within 1 tick, refused before); the DeathEvent stamps (respawn_tick,
  revive_until, killer_health) and ReviveEvent in CombatEvents on the wire; kProtocolVersion 3 with a v2 client
  refused by Kick; E5's "after the window" and "after the victim deployed" (they need a deploy-screen peer). After nt
  merges, the guarded branches in `respawn*`, `revive*` and `hostile*` run without edits; `host.tdm.client` (E13)
  starts running once `kProtocolVersion` is 3.
* **bb.** E13 needs `add_tdm_bots` (7 bots). `run_tdm` sets `tdm_bots` only for `bots > 0`; `sight_violations`,
  `stale_aim_shots` are bb's to count.
* **E13's revive half** (a scripted assault bot reviving the client) is not in host.tdm.client: a bot's kit is the
  rules' seeded draw, so the row would need a seed search or a dev kit override. The server side is E5's.
* **The M2a hash constants** are recorded from this branch's M2a path (the 3-argument `load_gunplay_level`, the motor
  without the abort fields), not from a build of main 47871fa. They equal main's as long as those paths are main's;
  the merge pass's M2a rows check it end to end.
* **The RoundOver SpawnRequest refusal** is implemented (Phase) but no case reaches RoundOver with a deploy peer; it
  comes with E1's round rows (`host.tdm.round`, bb / integration) or once nt's SpawnRequest travels.
* **[inferred] mechanics** (`measured = false`): the immunity abort map (Metro's type info has no enum names), the
  revive wait 20 m / 8 s, the corpse and living spheres, the bot kit weights, the spawn scoring constants, team kill
  weight 0, the one-tick lag of a revive's score in the rows.
* The full asan tier (docs/testing.md §8: ru touches src/bf3_sim) is the acceptance step's; only ru's three rows ran
  under asan here.
