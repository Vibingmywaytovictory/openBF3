# M2a bt: firefight bots, as built

Track "M2a bt" of M2a wave 3 (docs/design/m2a.md §15, row bt), branch `m2a/bt` from `m2a/w3` 3b3b991 (the W3.0
contract). Scope: m2a.md §10.2 (the firefight stub), E13 (the server tick with 64 firing bots), E15's headless half
(two firefights with one seed replay tick for tick), and the playable rule gx depends on: a human in the server is an
enemy like any bot. The README's "M2a bt" section repeats the gates and their commands.

Tags as in m2a.md: **[verified]** read from code, data or a measurement in this tree on 2026-10-18; **[inferred]** a
choice or estimate made here.

## What landed

| file | what |
|---|---|
| `src/bf3_host/src/firefight_brain.cpp` (replaces W3.0's stub), `include/bf3/host/firefight_brain.h` (bt's own header: `FirefightStats` counters added, the `firefight_layout` overload with a nav, `FirefightShared`, the shared-state constructor) | `add_firefight_bots` (the G3 hook body), `firefight_layout`, `FirefightBrain` |
| `src/bf3_host/include/bf3/host/bot_fill.h`, `src/bf3_host/src/bot_fill.cpp` (one `// M2a bt` block at the end of each) | `BotNav` / `make_bot_nav`: an own `NavQuery` + `PathService` over a `BotLevel`, `PathService::run` once per tick, the `WalkerView` as `BotDirector::command` builds it |
| `src/bf3_host/include/bf3/host/firefight_run.h`, `src/bf3_host/src/firefight_run.cpp` | `load_firefight_level` (database, shared level with gunplay, S1's bot level) and `run_firefight` (the headless run the tests and `bf3dump firefight` share; kills, displacement windows, geometry / ground / NaN checks, per-tick world hashes, tick-time percentiles), `firefight_report_text` |
| `tools/bf3dump/firefight_commands.{h,cpp}`; `bf3dump.cpp` (three named `// M2a bt` insertions: include, usage, dispatch); `tools/bf3dump/CMakeLists.txt` end block | `bf3dump firefight <LEVEL> <MODE> [--perf] ...` |
| `tests/host_gunplay_perf.cpp`, `tests/CMakeLists.txt` end block | `host.gunplay.firefight`, `host.gunplay.replay`, `host.gunplay.perf` |
| `tests/m2a_contract3_tests.cpp` | `kBtLanded = true` (G3: init with a Free config of 4 bots adds them) |

## How a brain works

One `FirefightShared` per `GameServer` holds what every brain of that server reads (the server, the config, the pose
eye heights and capsule from `SharedLevel::motor`, the `BotNav`, the lanes, the slot lives). A brain is a state machine
per life; it runs in step 3 of the tick (command sources, in slot order) and reads `ServerView` [verified].

* **Life.** A new `SoldierState::life` starts the life's stream (`splitmix64(firefight_seed(match_seed, slot) ^ life
  << 8)`), resets target, fire and strafe state. Pairs then calls `place_soldier` at its lane end. The slot was
  respawned by the `RespawnPolicy` at `choose_spawn` (the fixed 1 s timer), so placement moves a live soldier (life and
  weapons kept, protection cleared, sv's rule). Placing a dead slot would respawn it at once and bypass the policy, so
  brains never do it. Free bots spawn and respawn at the mode's spawns through the policy and are never placed.
* **Perceive.** Every `retarget_ticks` (30, staggered by slot), or at once when hit without a target: the nearest
  enemy (team 1 / 2, not ours, alive; remote peers included) with a clear eye-to-chest ray (`kProjectile`), at most 6
  rays per think, nearest first. A visible current target is kept unless another is 1.25 x nearer; it is re-checked
  every 10 ticks and dropped after 60 unseen.
* **Aim.** Chest = feet + 0.8 x the pose's eye height. The seeded error is σ 1.5° decaying to 0.5° over 1 s on target
  (§10.2), with pitch at 0.6 x yaw, redrawn every 6 ticks. The view turns at most 540°/s yaw and 360°/s pitch. The
  weapon's spread and recoil are the sim's: the command carries only the view.
* **Fire.** Fire happens when the view is within 4° of the aim, the target was seen at its last check, and the
  target's life is at least `kFreshTargetTicks` (60) old.
  - Automatic weapons fire bursts of 3-6 rounds with 0.3-0.6 s pauses. A weapon with an automatic mode is first
    switched to it through the fire-mode counter. Metro's kits needed no switch: 0 mode steps were measured.
  - Other weapons are tapped at 2 Hz.
  - Reload at an empty magazine, or below half without a target.
  - The sidearm is used when the primary has no round left.
* **Move.**
  - Free with walk: S1's `WalkerBrain` gets the nearest enemy (snapped to the main component) as its goal set
    (`objective_share` 0.8, so 1 in 5 goals stays S1's random points). Its move is re-expressed in the brain's view yaw.
    There is no sprint while aiming.
  - Within 25 m of the target, Free bots stop walking and strafe ±2 m legs (reversed at a missing floor). Pairs bots
    strafe while engaged.
* **Blind fire.** With no target for `no_target_s` (2 s), a bot fires one 1 s burst. Free bots fire along the
  longest clear of 8 headings. Pairs bots fire down the lane toward the partner's end. Counted as
  `no_target_fire`.

**Pairs layout.** The observer defaults to Metro's range lane (`find_range_lane`, spawn 87). Lane i of n is aimed at
the fan angle (±35°) at 15-60 m. A lane is 20-40 m long, across the view ray on even tries and along it on odd tries,
with up to `lane_retries` seeded retries.

An end is accepted when all of these hold:
- a downward ray finds a walkable floor (normal y ≥ 0.64) within 3 m of the observer's floor;
- the stand capsule fits;
- the nav (when given) has a main-component polygon within 1 m;
- both ends see each other eye to chest;
- the observer sees both chests (first half of the tries only).

With fewer lanes than pairs, a pair reuses a lane moved along the lane normal by ±k, ±(k+1) … × 1.2 m. The move is
used only where both ends re-foot and still see each other (`offset_lane`); otherwise the pair uses the lane as it is.

**Teams and kits.** Kit = slot % 4 (sv's rule). Team = 1 + ((i + i / 4) % 2), so a pair's ends are on opposite teams
and every team holds every kit. With `1 + i % 2`, team 1 held kits 0 / 2 and team 2 held kits 1 / 3, and team 1 shot
5,367 rounds against 3,164 in 60 s [verified].

## Constants (named in firefight_brain.cpp; [inferred] unless §10.2 gives them)

| constant | value | why |
|---|---|---|
| `kTurnRateDegS` / `kPitchTurnRateDegS` | 540 / 360 °/s | S1's walker yaw rate |
| `kFireConeDeg` | 4° | fire only roughly on target |
| `kTargetCheckTicks` / `kLoseTicks` | 10 / 60 | target ray cadence / unseen drop |
| `kMaxRaysPerThink` / `kMaxEngageMetres` / `kKeepTargetRatio` | 6 / 150 m / 1.25 | ray budget per think |
| `kErrorTicks` / `kErrorPitchShare` / `kChestShare` | 6 / 0.6 / 0.8 | aim error |
| `kModeDelayTicks` / `kModeWaitTicks` | 3 / 10 | fire-mode counter after spawn (§5.2's spawn rule) |
| `kEngageStopMetres` / `kStrafeHalfMetres` / `kStrafeAxis` / `kStrafe{Min,Max}Ticks` | 25 m / 2 m / 0.8 / 60-150 | strafe |
| `kBlindRayMetres` / `kBlindTicks` | 60 m / 60 | blind fire |
| `kHuntShare` / `kSprintConeDeg` | 0.8 / 20° | walking |
| `kFreshTargetTicks` | 60 (1 s) | `place_soldier` clears spawn protection. Without the hold, a recon bot on a lane died 78 times in 120 s without firing once [verified] |
| `kLayout*`, `kRangeLaneMinMetres`, `kSharedLaneOffset`, `kSharedLaneTries` | see the file | layout |

## Decisions and deviations

* **Hit targets stop at slot 63.** lc's `RewindProvider` (`lag_comp.cpp`, `kMaxLagSlots = 64`, 64-bit masks) builds
  hit targets for slots 0-63 only, so a soldier in slot 64 or above is never hit [verified].
  - The test that found it: an idle peer in slot 64, with 64 bots in 128 slots, was targeted and fired at for the
    whole minute (debug prints) and drew 0 impacts.
  - What changed here: `FirefightRunOptions::max_slots` defaults to 64, and the human-slot case runs 32 bots + 1 peer
    (gx's playable default is at most 32 bots). E13's observer sits in slot 64 of 65 slots and is never hit, and the
    report says so.
  - Not fixed here: `GameServer` accepts `max_slots` above 64 with gunplay on, which is lc's / sv's to decide (open
    issue).
* **"Below the terrain"** means no surface at all under the capsule. Five downward rays are cast from the feet + 0.5
  m, 200 m deep (the centre and four rim points at 0.9 r), against `kMovement`. A single centre ray missed 6-8 times
  per minute for soldiers standing `OnGround` across narrow gaps. Metro's subway lies legitimately under the street
  terrain, so "under the terrain mesh" cannot be the rule.
* **"Inside geometry"** means the pose's upright capsule (height at least 2r + 0.05) overlaps the static world
  (`kMovement`) deeper than 0.10 m. The check runs every 30 ticks (the firefight row) or every 60 ticks (replay).
* **Stuck** means a full 20 s window on one life whose farthest horizontal distance from the window's start is under
  1 m. Windows exist only for lives of 20 s or more, so the firefight row also requires a path length of at least
  10 m per bot over the 60 s.
* **Kills** are counted from `GunplayObserver::damage` (`applied` and `health_after <= 0`), which needs
  `dev_commands`. The run sets it and uses nothing else of the dev set.
* **World hash per tick** (FNV-1a 64, field by field, slot order) covers: every used slot's `soldier_state_hash`,
  `soldier_weapons_hash` and team; every live projectile; the tick's damage events with their outcome; impacts and
  detonations.
* **Replay runs** three servers (seed S, S, S + 1) on three threads over the one const level: equal results on
  parallel threads are part of the claim.
* **The run's own BotNav.** `GameServerConfig::bots` carries the `BotLevel` with count 0, so S1's director adds no
  walkers. A server with both S1 walkers and firefight bots would have two query sets with separate budgets (noted in
  bot_fill.h).
* **Paths are relative** to the project. `GunplayConfig::mechanics_cfg` defaults to `config/weapon_mechanics.cfg`.
  The tests pass the file in `bf3-opensource/config`. `bf3dump firefight` uses it when run from the repository root,
  else `--mechanics=`.

## Gates (measured 2026-10-18, dev preset; branch, unlocked, shared machine)

| id | threshold | measured | command |
|---|---|---|---|
| host.gunplay.firefight (64 bots) | 64 Free bots, 60 s: kills > 0 for both teams, every bot fired, 0 stuck windows, ≥ 10 m walked per bot, 0 inside geometry, 0 below ground, 0 non-finite, 0 motor / weapon precondition failures, 0 team kills | kills 117 / 100; 64 / 64 fired (63 hit an enemy, 56 killed one); 73 windows, 0 stuck; 7,249 checks with 0 / 0 / 0; 0 / 0 preconditions; 0 team kills | `ctest --preset dev -R "^host\.gunplay\.firefight$"` |
| host.gunplay.firefight (human slot) | 32 bots + 1 idle peer, 60 s: the peer is hit | hit 6 times, killed twice; exposed to an enemy bot in 29 of 116 checks | same row |
| host.gunplay.replay (E15) | two 3,600-tick runs with one seed: equal per-tick hashes; another seed: different | 0 of 3,600 ticks differ; the other seed differs on 3,600 of 3,600; run hashes 0x67c0c1af9d235ed1 twice, 0xca001becd743dc03 | `ctest --preset dev -R "^host\.gunplay\.replay$"` |
| host.gunplay.perf (E13) | tick p99 ≤ 8.0 ms, 64 Pairs bots + 1 observer, 10 s warm-up, 120 s | **indicative only** (branch, unlocked, shared machine): p99 1.909 ms. Weapons + projectiles + rewind mean 0.056 ms = 7.7 % of the 0.736 ms mean tick. 64 / 64 fired | `b\dev\tests\test_host_gunplay_perf.exe --test-suite=host-gunplay-perf` (merge pass: `ctest --preset dev-timing -R "^host\.gunplay\.perf$"` under the quiet lock) |
| install.unchanged | layout.toc, cas.cat, the .sb / .toc files unchanged | `install.unchanged: clean (256 files)` in each suite and in `bf3dump firefight` | the rows above |

Robustness, measured with `bf3dump firefight MP_Subway TeamDeathMatch0 --mode=free --count=64 --seconds=60 --seed=N`
for N = 2 … 9:
- every seed: both teams kill, 64 / 64 fire, 0 stuck, 0 inside geometry, 0 below ground;
- the shortest bot path was 90.7 m.

A 600 s soak (seed 3) gave:
- 36,000 ticks; 1,830 kills (976 / 854);
- 1,028 windows, 0 stuck;
- 36,573 checks with 0 violations;
- tick p99 1.917 ms.

Pairs at E13's settings:
- 27 lanes accepted of 32 wanted (147 rejected tries), 5 pairs on moved lanes; 1,887 placements;
- 1,836 deaths in 120 s.

The Pairs row does not gate stuck windows: 2 bots held still on their lane ends.

## Branch pass (2026-10-18, head of `m2a/bt`)

`tools\build.cmd dev --strict` ran 130 rows: 128 passed and 2 were skipped (`core.cache_data`, `core.res`, as on
W3.0). The phase took 408 s. That is far slower than W3.0's 176 s, because other worktrees' test runs shared the
machine at 100 % CPU.

`test_budget.cmake` reported one violation, on a row bt does not touch: `sim.terrain.unit` took 40.1 s in the phase
and has no COST. Alone it takes 0.85 s, so the 40 s came from the contended machine; the row was left unchanged.

The new rows read 58.1 s (`host.gunplay.firefight`) and 48.4 s (`host.gunplay.replay`) in the contended parallel
phase (warnings only). Alone they take 13.8 s / 30.9 s CPU and 11.4 s / 38.0 s CPU, which are their COST /
PROCESSORS. `m2a.contract3` passed in 0.32 s with `kBtLanded = true`.

## Open issues

* `GameServer` accepts more than 64 slots with gunplay on, but slots 64 and above are never hit (lag_comp.cpp
  `kMaxLagSlots`). This is lc's / sv's to decide: refuse at init, or widen the masks.
* Pairs finds 22-27 of 32 lanes around Metro's range lane, depending on the seed. The rest reuse moved lanes.
  `lane_retries` (8) is the contract's default and was left as it is.
