# M2b bb: TDM bots (B2-B4), as built

Track bb of M2b wave 1, on branch `m2b/bb` from the W1.0 contract head 01a26e8 of `m2b/w1`. Design:
[m2b.md](m2b.md) §5 (perception, target choice and reaction, aim error, movement, fighting, revive, budgets and
seeds), §2.6 #13, gates E1, E2, E6, E8 and the `--tdm` paragraph of §11. This file says what was built, the named
constants, where the code differs from the design, and the gates with one command each.

No VU session measured BF3's bots (the S5 sessions are parked), so every bot number is a named default with
measured = false. The playtest tunes them through `config/bot_tuning.cfg`.

## 1. What the commits contain

| file | what |
|---|---|
| `src/bf3_sim/src/bots/perception.cpp` (+ additions to `perception.h`) | §5.1 perception: the staggered update, FOV + range candidates, head / chest rays, distance-scaled detection, hearing as an estimate only, forgetting |
| `src/bf3_sim/src/bots/aim_model.cpp` | §5.3 `aim_error` (the skill-scaled cone) and `lead_point` (lead and drop) |
| `src/bf3_host/src/tdm_brain.cpp` (+ additions to `tdm_brain.h`) | the TDM brain (replaces the W1.0 stub), `TdmShared`, `add_tdm_bots`, `parse_bot_tuning` / `load_bot_tuning`, the `TdmBotObserver` hooks |
| `bf3-opensource/config/bot_tuning.cfg` | every tuning default, documented; the shipped file equals the compiled-in defaults (a unit case pins it) |
| `tests/host_tdm_round_tests.cpp` | `host.tdm.bots.unit`, `host.tdm.round` (E1), `host.tdm.bots` (E2), `host.tdm.replay` (E6), `host.tdm.perf` (E8) |
| `tests/tdm_fixture.h` `// M2b bb` block | `BotChecks` (the E2 sight, stale-aim, blind-fire and stuck checks as a `TdmBotObserver`; after the merge also the stuck reports, bots per team, the gunplay counters, the player rows and the spawn / death centroids the E rows print), `attach_bot_checks`, `apply_bot_checks` |
| `game_server.{h,cpp}` named insertion `[bot spawn]` (after the w1 merge, §2) | `GameServer::add_local_bot`: `add_local` with the bot mark (the main-component spawns) |
| `tests/CMakeLists.txt` `# M2b bb` block | the `test_host_tdm` executable (created if ru has not), the five rows, labels, COSTs, timeouts, asan settings |
| `src/bf3_sim/CMakeLists.txt` `# M2b bb` block | the two new sources |
| `tests/m2b_contract_tests.cpp` | `kBbLanded = true` (the owner's switch) |
| `src/bf3_godot/src/level_session_m2a.cpp` `// M2b bb` block + named insertions `[tdm]` | the `--tdm` preview (§11), compiled, not a row |
| `tools/play.cmd` named insertion `[help]` | `--help`, documenting `--tdm`, `--kill-limit`, `--preround` |

### 1.1 Perception (§5.1)

* A bot perceives on `(tick + slot) % update_ticks == 0` (6: one sixth of the bots per tick).
* Candidates: every live enemy inside the 160° x 90° view cone within 120 m, nearest first (already-detected ones
  first among equals, then by slot). Only the first `max_candidates` (4) get rays: eye to chest, and eye to head only
  when the chest ray is blocked (after the w1 merge; it was head first). `PerceivedEnemy::chest_visible` records which
  one was clear, so the brain aims at a point it can see. The rays use `query_mask::kProjectile`.
* Head and chest points: the feet plus `head_height_m` / `chest_height_m` scaled by the pose's eye height over the
  standing eye height (`perception_head`, `perception_chest`), so a crouched or prone enemy is aimed at lower.
* Detection: `detection += dt (1 + moving + firing) / detection_time(d)`, detected at 1. `detection_time` runs
  linearly from 0.2 s at 10 m to 1.5 s at 100 m, clamped outside. "Moving" is above 0.5 m/s horizontal; "firing" is
  a shot within the last 30 ticks.
* Hearing: an enemy shot within 60 m of the eye in the update window makes the shooter known (heard) at the shot
  origin plus a seeded error of at most 5 m. Hearing never detects (no aimed fire from a heard position).
* Forgetting: a detected or heard entry is dropped `forget_s` (3 s) after the last sight or heard shot, a partial
  detection after `forget_s / 3`. Dead enemies and enemies that left are dropped at once.
* Hostile input: non-finite positions are skipped, every span is bounded, at most one entry per slot.

### 1.2 Aim (§5.3)

* `sigma = k(skill) (e_inf + (e0 - e_inf) exp(-t / tau)) + moving_penalty + crossing_deg_per_mps x crossing speed`,
  `k(skill) = max(0.25, 1 + 2 skill_scale (0.5 - skill))` (1 at 0.5, 1.4 at 0.3, 0.4 at 0.8), clamped to [0, 45]°.
* The brain draws a gaussian error with that sigma every 6 ticks (pitch at 0.6 of yaw) and aims at the chest point
  (only the head ray clear: 0.1 m below the head point), led by `lead_point` (three fixed-point iterations, at most 5 s of flight, the drop `-0.5 g t²`) unless the weapon
  is instant hit.
* The command subtracts `skill x` the weapon's current recoil offset (better bots compensate more).
* The view turns at most 540°/s in yaw (S1's walker rate) and 360°/s in pitch.

### 1.3 Target choice and reaction (§5.2)

* Score of a detected enemy with a clear ray in the last 12 ticks: `w_distance / d + w_aim (it aims at me, within
  10°) + w_damage (it shot at me within 3 s) + w_current`. A new target must score `switch_margin` (0.25) better.
* A new target starts the reaction delay (0.25-0.6 s, the faster end for the better skill) and the aim error's clock;
  a switch during a fight reacts in half the time.
* A shot passing within 1.5 m of the bot's chest marks it "shot at" and the shooter; with no target in sight the bot
  turns towards the shot's origin.

### 1.4 Activities (§5.4-§5.6)

Priority, highest first: Revive > Grenade > Retreat / RegenWait > Fight > blind fire > Hunt / Roam.

* **Restricted.** `input_restricted()` or a phase other than InRound: an idle command.
* **Fight.** Aim and fire (§1.2), aiming down the sights (`kZoom`) beyond 6 m. Within 25 m it strafes legs of 2-6 m
  (checking there is floor ahead); beyond it pushes in (strafing when there is no path); beyond 40 m a bot with skill
  above 0.5 crouches and holds for at most 4 s per target, then pushes in. The knife within 2 m from behind (more
  than 100° off the target's view).
* **Fire.** Within 4° of the aim, with the last clear ray at most 12 ticks old and the reaction elapsed. Bursts of 6
  rounds close falling to 3 at 60 m with 0.15-0.35 s between bursts, single shots beyond 60 m, taps for
  semi-automatic weapons; no shot while a living teammate's chest is within 0.8 m of the line of fire; the fire mode is
  switched to automatic when the weapon has one. Reload at an empty magazine, or below 30 % with nothing in sight. The
  sidearm when the primary is empty for good.
* **Blind fire.** When a target is lost, one burst at its last-known position with probability `blind_fire_share`
  (0.5), counted apart (E2 excludes it).
* **Grenade.** A lost target behind cover 15-35 m away, at most every 20 s: switch to slot 6, hold, release towards
  the last-known position.
* **Hunt.** Walk to the last seen or heard position of a known enemy; arriving there forgets what was known before.
  A hunt that finds nobody in 20 s is given up: the bot forgets it and roams for 10 s (`hunts_given_up`).
* **Off the main component.** Every 30 ticks the bot checks that its feet are on the nav's main component. Off it,
  it walks straight at the nearest main-component nav point found on rings of 2-32 m around it (else the nearest
  main-component spawn), jumping when it makes no progress for 45 ticks (`off_main_ticks`). With the bots on the
  main-component spawns (§1.6) the E rows measure 0 such ticks.
* **Roam.** Every 10 s at most, a goal drawn by weight from the contact heat grid (64 x 64 cells of 16 m, centred on
  the mean spawn point; shots add 0.05, deaths 1, decay over 30 s) within 160 m, else S1's random nav points.
  `hunt_share` (0.85) of the walker's legs go to the brain's goal.
* **Look around (as built, not in §5).** Without a walker (a level with no nav, the synthetic floor) a roaming bot
  holds its spawn heading for 1.5-3 s, then turns 60-150° to a random side; it holds the heading while an enemy in
  sight is being detected. Without this, bots whose spawns face away from each other never meet.
* **Retreat / RegenWait.** Below 30 HP: move 14 m away from the threat (at most 6 s), then wait out of sight for
  regen until 90 HP or 15 s.
* **Revive.** An assault bot (kit 0) not engaged for 2 s goes to a downed teammate within 30 m when the man-down
  window outlasts the travel estimate (straight distance x 1.3 / 4.5 m/s) plus 1 s. It steers straight within 2.5 m,
  crouches within 2 m, selects slot 2 (the defibrillator, `kKitGadgetSlot`) and taps fire within 1.7 m of the corpse
  centre (the defib reaches 1.8 m), retrying every 66 ticks. Being shot at, or 900 ticks, aborts.

### 1.5 Seeds and determinism (§5.7)

* The bot seed is `match_seed ^ slot`. Each life draws its skill (uniform in [0.3, 0.8]) and its streams from
  `splitmix64(seed ^ life << 8)`.
* No wall clock reaches a decision; the per-tick nanoseconds are only measured and published
  (`last_perception_ns`, `last_brains_ns`). No unordered container. A brain reads only `ServerView` (states, team,
  alive, weapons, world, tick, `downed()`, `phase()`, `input_restricted()`, `kit()`) and the gunplay's interval shots.

### 1.6 add_tdm_bots

* Loads `TdmBotConfig::tuning_cfg` when it is set (a load failure fails `GameServer::init`); the caller's `count`
  wins over the file's.
* Caps the count at the free slots below 64 (lag compensation covers slots 0-63); more bots than slots are capped,
  not refused.
* With the rules on, team 0 (AutoTeam decides) and the kit from `ServerView::kit()`; without rules, the M2a teams
  `1 + ((i + i / 4) % 2)` and the gunplay's kit.
* The bots join through `GameServer::add_local_bot` (the named insertion `[bot spawn]`, §2): the slot carries the
  bot mark, so every spawn of it chooses among `BotLevel::spawns` (the main nav component, S1's list) when
  `BotFillConfig::spawn_in_main` (the default), with the rules' scored chooser over that list.

### 1.7 Tuning file

`parse_bot_tuning` reads `key = value` lines with `#` comments, at most 64 KiB. An unknown key, a missing `=`, a
trailing character, a non-finite number or a value outside its range fails, and leaves the config unchanged (the
parse writes into a copy). Integer keys: `count`, `burst_min_rounds`, `burst_max_rounds`, `perception.update_ticks`,
`perception.max_candidates`; the rest are floats.

## 2. Differences from m2b.md

1. **Look around without a walker** (§1.4): not in §5; added because the unit case on the synthetic floor (spawns on
   a circle facing outwards) never produced a detection without it.
2. **The bots join through `add_local_bot`, not `add_local_soldier`** (a named insertion outside the m2b.md contract:
   `[bot spawn]`, the declaration in `game_server.h` and the body in an `// M2b bb` end block of `game_server.cpp`,
   commit eac1472). The first E2 run on Metro after the merge had 1-3 bots per team that never killed nor died in a
   whole round: the level's TDM spawns include points on nav islands at y = 64.6-65 (around (-213, -479), (-219,
   -524), (-178, -537), (-230, -490)), enclosed areas the walker has no path out of (walking straight out failed too:
   0.00 m in 20 s windows). Team 2 had more of them, so the pooled K/D was 1.31-1.55 for team 1. With the bot mark the
   TDM bots spawn among the main-component spawns; the next E2 run had 0 off-main ticks and a pooled K/D of 1.137.
   **A human peer can still spawn on those points** (the rules' chooser runs over the level's spawns for peers): an
   open issue for ru's spawn choice.
3. **Stuck bots.** E2's stuck count is `BotChecks`' own, which leaves out Dead, Restricted, RegenWait and Revive ticks
   as §E2 says; the M2a-style count from `run_tdm` is kept and printed beside it.
4. **E1's coverage and the peer's MatchState** (after the w1 merge). `run_tdm` returns neither the server nor the
   peer, so the E1 case captures both in `TdmRunConfig::on_tick`: a copy of `TdmRules::natives().coverage()` and every
   `MatchState` the peer decoded. It checks 0 `Recorded` deliveries for the nine TDM types of §3.1
   (`CharacterSpawnReference` is `LogicTypeId::CharacterSpawn`), the peer's RoundOver winner and counts against the
   rules', and 0 / 0 after the restart in place. Recorded deliveries to other native types are printed, not checked.
   **It fails** on ru's `RoundNatives`: the two `CharacterSpawnReferenceObjectData` nodes 3532 / 3533 (the TDM HQ
   spawns, Team = team2 / team1) get the property input `BlueprintTransform` (port id -409930682) at tick 1, which
   `RoundNatives::apply` hands to the stubs as Recorded (no `native_port` binding for it in `logic_native.cpp`'s
   spawner rows). The fix is ru's (bind or accept it); the check stays strict.
5. **The fixes after the merge (the first Metro runs, §1.1-§1.4):** aim at the visible point (chest first), hold
   fire for a teammate in the line of fire (team damage dropped 308 -> 16-47 per round), aim down the sights, the 4 s
   cap on the crouched hold (two bots had held a 62 m duel for 65 s and counted as stuck), shorter burst pauses, the
   hunt give-up and the island walk-out. All [inferred] named constants in `tdm_brain.cpp` (`kHeadAimDrop`,
   `kFriendlyLineMetres`, `kZoomBeyondMetres`, `kHoldTicks`, `kBurstPause*S`, `kHuntGiveUpTicks`, `kHuntBlockTicks`,
   `kIsland*`); m2b.md §5 does not give them. The §5.3 aim numbers are unchanged.

## 3. Rows

| row | suite | label | COST | what |
|---|---|---|---|---|
| `host.tdm.bots.unit` | host-tdm-bots-unit | none | (well under 30 s) | perception, aim model, tuning file, a brain on a fake ServerView, `add_tdm_bots` on the synthetic floor; no install |
| `host.tdm.round` | host-tdm-round | none | 1 25 | E1 |
| `host.tdm.bots` | host-tdm-bots | none | 3 15 | E2, seeds 1-3 in `parallel_map` |
| `host.tdm.replay` | host-tdm-replay | none | 3 10 | E6, seeds 11 / 11 / 12 |
| `host.tdm.perf` | host-tdm-perf | timing (RUN_SERIAL) | 1 20 | E8, 64 bots, never run on the branch |

Each install case prints and checks `install.unchanged`. Since m2b/w1 (nt, au, ru) was merged into `m2b/bb`,
`kRuNtLanded` in `host_tdm_round_tests.cpp` is true: the install rows run ru's `run_tdm` / `load_tdm_level_once` and
skip (exit 77) only without an install.

## 4. Gates, one command each

* Branch pass: `tools\build.cmd dev`.
* Unit row: `ctest --test-dir b\dev -R "^host\.tdm\.bots\.unit$" --output-on-failure`.
* Contract: `ctest --test-dir b\dev -R "^m2b\.contract$"`.
* Asan (the new parser `parse_bot_tuning`, perception and aim on hostile values): `cmake --preset asan`, then
  `cmake --build --preset asan --target test_host_tdm`, then
  `ctest --test-dir b\asan -R "^host\.tdm\.bots\.unit$" --output-on-failure`.
* E1: `ctest --test-dir b\dev -R "^host\.tdm\.round$" --output-on-failure`.
* E2: `ctest --test-dir b\dev -R "^host\.tdm\.bots$" --output-on-failure`.
* E6: `ctest --test-dir b\dev -R "^host\.tdm\.replay$" --output-on-failure`.
* E8 (the wave-1 merge pass on main, timing): `ctest --preset dev-timing -R "^host\.tdm\.perf$"`.
* Preview (not a row): `tools\play.cmd --tdm --bots=31` (`tools\play.cmd --help` lists it).

Measured results: README "M2b bb: TDM bots".

Still open after the w1 merge:
* E1 fails on the `BlueprintTransform` deliveries (§2 item 4, ru's `RoundNatives`); everything else in E1 passes.
* E8 `host.tdm.perf` (timing) runs in the merge pass on main.
* Human peers can spawn on the nav-island spawns (§2 item 2, ru's spawn choice).
