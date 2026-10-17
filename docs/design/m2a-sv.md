# M2a sv: server gunplay, as built

Track "M2a sv" of M2a wave 2 (docs/design/m2a.md §15, row sv), branch `m2a/sv` from `m2a/w2` f4bbc43 (W2.0 with nt
and lc merged). Scope: m2a.md §4 (server integration), §5 (weapons in the game: inputs, fire / ADS / reload / switch,
projectiles, health, the unmeasured switches, kits), the server side of §6 (lc's `LagHistory` / `RewindProvider` and
`posed_target` used as they are), §10.1 (stationary targets), §13 (hostile input on the server), §14.1 (the server
tick) and wp's R3 (exact aim). The README's "M2a sv" section repeats the gates and their commands.

Tags as in m2a.md: **[verified]** read from code, data or a measurement in this tree on 2026-10-17; **[inferred]** a
choice or estimate made here.

## What landed

| file | what |
|---|---|
| `src/bf3_host/src/gunplay.cpp`, `include/bf3/host/gunplay.h` | `Gunplay` (per-slot weapons, `ProjectileWorld`, `LagHistory` + `RewindProvider`, damage rules, deaths, the shot accumulator, the trace part), `GunplayLevel` and `load_gunplay_level` (the level's defs through `WeaponCatalog`, the mode's kits, the soldier, hit capsules, the capsule pose table from lc's builder on the retail clips), `make_match_table` / `match_weapon_hash` / `match_weapon_flags`, `load_mechanics_cfg`, `shot_frame`, `derive_match_seed`, `standin_capsule_table`, `GunplayObserver` (dev / tests) |
| `src/bf3_host/src/kits.cpp` | `make_kit_table`, `kit_table_hash`, `KitTable::loadout` (team and kit clamped) |
| `src/bf3_host/src/target_bots.cpp` | `TargetBotSource` (idle at a set pose and facing), `find_sky_lane` |
| `src/bf3_host/src/game_server.cpp` | eight tagged named insertions (`// M2a sv [init]`, `[recv]`, `[loop]`, `[rules]`, `[snap]`, `[ctrl]`, `[trace]`, `[stats]`) plus the `[impl]` declarations and one `// M2a sv` block at the end (`Impl::Sv`: the `ServerView`, the `CombatEventSink`, the fixed `RespawnPolicy`, per-slot kit / team / trace, the SnapshotOthers split, ShotEvents and CombatEvents sending; `place_soldier`, `view()`, `gunplay()`) |
| `include/bf3/host/game_server.h`, `level_load.h`, `src/level_load.cpp` | named insertions: `GameServerConfig::gunplay`, `GameServerStats` gunplay fields, `view()` / `gunplay()`; `SharedLevel::gunplay`, `SharedLoadOptions::gunplay`, `load_shared_gunplay` |
| `src/bf3_sim/src/weapons/weapon_sim.cpp`, `weapon_sim_def.cpp` (R3) | `WeaponTable::exact_aim`: each `ShotEvent::direction` = `aim_direction(cmd.yaw, cmd.pitch, {})`, `pellet_cone` 0; hashed after `infinite_magazine` |
| `src/bf3_sim/src/weapons/weapon_rules.cpp`, `mechanics_tuning.cpp`, `bf3-opensource/config/weapon_mechanics.cfg` | `step_weapons_m2a`'s two rules (zoom refused while deploying / switching / sprinting; the fire-mode counter taken at spawn), the cfg parser / formatter and the shipped file (§5.5) |
| `tests/host_gunplay_server_tests.cpp`, `tests/gunplay_fixture.h` (sv block) | `host.gunplay.server` (cases timing, drop, stk, reload, variants, hostile, install); `metro_gunplay`, `GunplayRecorder`, button and aim helpers |

## Tick order as built (extends m1.md §8.1, m2a.md §4.1)

1. receive: `CommandPacket` acks go to `LagHistory::ack`; a command newly buffered for its tick (or replacing one by a
   newer sequence) keeps the client's `AckState` at that moment (`Gunplay::note_command`, a 64-entry ring per slot keyed
   by tick).
2. commands, 3. command sources: unchanged (target bots are command sources).
4. motor step, then 4a `Gunplay::step_soldier` for every soldier alive before the step: `make_weapon_context` +
   `step_weapons_m2a`; each shot goes to `ProjectileWorld::spawn`, its view sample to `RewindProvider::note_shot` (or
   `note_refused`), and to the interval's shot list. A death across the motor step (fall damage) is noted and reported
   in step 5 as a `DeathEvent` with killer = victim and def `kNoWeapon`.
5. rules, first (`sv_rules`): `RewindProvider::begin_tick`, one `ProjectileWorld::step` for all projectiles,
   `end_tick`; damage sorted by (time, attacker, shot, pellet, target) and applied with `apply_damage`; Hit / Death /
   Detonation events to the `CombatEventSink`; a death asks the `RespawnPolicy` (M1's fixed timer); then
   `place_soldier` requests. Then M1's spawns (a spawn calls `on_spawn` with the slot's kit and team), the dev requests
   (`SpawnTarget`, `TargetInfo` added) and the sweep.
6. at snapshot ticks: `LagHistory::record_tick` with every live soldier's quantised record including the weapon fields
   (`Gunplay::other_record`).
7. per client: `own_weapons` = `weapons_wire_view`, the others replaced by the shared records, the overflow split into
   a `SnapshotOthers` of the same seq, `LagHistory::sent(seq, tick, main_mask, others_mask)`, the other shooters' shots
   in at most two `ShotEvents` datagrams (the second carries what the first left out), the interval's `CombatEvents`
   (reliable). After the last client: the interval starts over.

## §16 decisions taken (defaults, kept as named constants or config)

* **Q1** 350 ms = 21 ticks (lc's `kDefaultMaxRewindTicks`; `MatchInfo::max_rewind_ticks`).
* **Q2** HitReports deferred: the server never takes a client's hit claim.
* **Q3** the others that do not fit go in `SnapshotOthers` of the same seq, nearest first.
* **Q4** reload timing from the authored `ReloadTime` / `ReloadTimeBulletsLeft` (E5 pins both).
* **Q12** `measured = false` in `weapon_mechanics.cfg`; every row that depends on a switch says "unmeasured".
* **Q14** E3's / E6's thresholds as m2a.md §1 states them.
* **Q21** a refused zoom is refused for the whole tick (`step_weapons_m2a` clears `kZoomWasDown`).

## Decisions and deviations made here (each one a named constant, config field or documented rule)

* **More named insertions than the contract's four.** game_server.cpp needed eight insertion points to keep every
  existing line as it was (receive, motor loop, rules, spawn / Spawned, dev requests, snapshots, control messages,
  trace, stats); each is one commit naming its tag, and every inserted line carries `// M2a sv [tag]`. level_load.h /
  .cpp and game_server.h got field insertions inside their structs (a struct member cannot go in an end block).
* **`SharedLoadOptions::gunplay` is off by default**, so M1's load (E4 / E5 numbers, `host.session`) is unchanged
  until the integration turns it on. The load costs ≈ 1.2-1.4 s on Metro [verified: the fixture prints gunplay 1.24 s,
  capsule table 1.05 s, quiet machine], nothing is cached.
* **`weapon_table_hash` hashes `exact_aim` only when set** (a conditional line after `infinite_magazine`), so every
  existing table hash, and wp's W1-W11 outputs, stay bit-equal [verified: the outputs before and after R3 differ only
  in their "opened in" / "loaded in" timing lines].
* **`weapon_hash` = `weapon_table_hash` ^ the capsule table hash**; `kit_hash` = FNV-1a 64 of the kit table; both in
  `MatchInfo`, both recomputed by a client from the same level and cfg (`host.gunplay.server` checks the server's
  value against the table the test builds from the level, like a client).
* **The SnapshotOthers split encodes the Snapshot twice** (once to learn how many others fit, once to send). ≈ 0.1 ms
  per client at 64 soldiers [inferred]; a single-pass encoder that returns the cut is nt's to add.
* **Slots ≥ 64 are not rewindable**: lc's masks are 64 bits (`kMaxLagSlots`); a soldier in slot 64..127 is tested at
  the present. M2a runs ≤ 64 soldiers.
* **Friendly fire is off** (`GunplayConfig::friendly_fire = false`): damage between soldiers of one team (1 or 2) is
  dropped and counted (`team_damage_dropped`). A blast still hurts its own thrower.
* **Spawn protection ends when the soldier fires** (§4.3) without touching the predicted `SoldierState`: a soldier
  that has fired in its life is damaged with `Mechanics::spawn_protection_blocks` off. Blasts are tested by lc's
  provider with `HitTarget::spawn_protected`, so a protected soldier that fired is still spared by a blast until
  `protect_ticks` runs out (rare; `blast_protected` is reserved, 0). cl should mirror the rule in its HUD.
* **Kits**: server-side soldiers (bots, walkers, targets) take kit = slot % 4; a client takes
  `GunplayConfig::player_kit` until its `KitSelect` (the M1 client sends none). `KitSelect` team changes need
  `dev_commands` (else counted as `team_change_refused`); a malformed one is a bad datagram and `kit_select_refused`.
  Target bots are added before the walkers, so they take slots 0 .. N - 1.
* **The shot quantisation frame** is the static world's minimum floored to the metre minus 64 m
  (`kShotFrameMargin`), so shot origins and combat positions near the bounds travel unclamped.
* **Server-side shooters fire at the present** (view quality None, D7). A client shot whose view sample `shot_view`
  refuses, or whose command has no stored ack state, is tested at the present and counted (`view_refused`,
  `no_ack_state`).
* **`GameServer::view()` before `init`** is a programming error (it aborts with a message), not a data error.

## Measurement notes

* The M1 `ClientSession` carries the weapon buttons (24 wire bits of `held_buttons`) but no view sample, no
  `KitSelect` and no weapon prediction. The client halves of E1 (predicted = server), E2 (cosmetic flight), E5
  (predicted phase events, viewmodel), E6 (the mismatch disconnect, 0 weapon reconciliations) are cl's
  `host.gunplay.client`; this track measures the server halves.
* A semi-automatic tap is held ≈ 3 ticks here (the M1 client samples the held button once per client tick); the
  one-display-frame tap needs cl's fire press counter (§5.1).
* The hostile row and E6's shot-offset sample drive `GameServer` without sockets (the code ServerHost runs): the
  hostile fields cannot be produced by a `ClientSession`, and 2,000 offsets need 32 shooters.
* E2: the expiry distance is measured along the launch line (horizontal distance / |horizontal part of the
  direction|), which is v0 × TTL whatever the spread and the jump spread of a shooter in the air; the drop is the
  distance from the launch line (origin + direction) of the traced positions, linear between ticks.
* E3: the aim point is the zone capsule's centre raised by the bullet's drop over the distance (6.4 cm at 60 m for the
  M16A3; `exact_aim` removes recoil and spread, not gravity). Without it the 60 m chest shots land in the body zone
  (same multiplier, same count).
* Timings are wall clock on a shared machine and **noisy**. Only `host.scale` G1 is a timing gate here (run under the
  quiet lock); the hostile row prints tick times for reference.

## Gates (measured 2026-10-17, dev preset)

One command per gate (from the worktree; `ctest --preset dev-timing -R "^host.gunplay.server$"` runs them all; a timing row):

| gate | command | threshold | measured | met |
|---|---|---|---|---|
| E1 (server) | `b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=timing` | M16A3 held from a full magazine: 31 shots, first at the press tick's start, 30 gaps of exactly 4,500,000; the trace's shots = the server's field for field; every other kit firearm: gaps = the table interval; semi-automatics one shot per tap | M16A3 31 shots, 30 / 30 gaps 4,500,000, first at tick_start(56); trace 200 ticks, 31 / 31 shots bit-equal, 20,579 bytes, 0 dropped. M4A1 4,500,000, M27 IAR 4,800,000, AK-74M and AKS-74u 5,538,462, RPK-74M 5,142,857 (8 shots each, first at the press tick); MK11, SVD, M9, MP443 5 / 5 taps one shot each, gaps ≥ interval | yes |
| E2 (server) | `... --test-case=drop` | drop 0.1779 ± 0.01 m at 100 m, 1.5981 ± 0.01 m at 300 m; expiry (`Expired`) at 975.0 ± 0.01 m; sky lane in-bounds length printed | sky lane along z, clear 1,000 m, in-bounds 2,197.5 m; three shots, each 3 ticks after the teleport, origin 5.4 mm below the teleport eye: 100 m 0.1780 / 0.1785 / 0.1786 m, 300 m 1.5993 / 1.6030 / 1.6039 m, expiry 975.0000 m ×3 | yes |
| E3 (server) | `... --test-case=stk` | M16A3 chest 4 / head 2 at 5 m, chest 6 / head 3 at 60 m; per hit at 5 m 25.000 / 50.000 / 22.750 ± 1e-3; one death event after the last hit | range lane spawn 87, 96.9 m; 4 / 2 at 5 m, 6 / 3 at 60 m; 25.0000 / 50.0000 / 22.7500 (legs: 5 hits to kill); 1 death each, 0 misses, 0 other zone | yes |
| E4 (server) | `... --test-case=stk` | M16A3 head 50.000; MK11 head 117.500 at 5 m (kill in 1), head at 60 m kills in 2 | 50.0000; MK11 117.5000 kill in 1; 60 m 95.59 per hit, kill in 2 | yes |
| E5 (server) | `... --test-case=reload` | M16A3 tactical 108,000,000, commit +91,800,000 → 31; empty 142,200,000 → 30; M9 81,000,000 / 96,000,000 | 108,000,000, +91,800,000, 26 → 31; 142,200,000 (+120,870,000), 0 → 30; M9 81,000,000 (14 → 16) / 96,000,000 (0 → 15); every kit weapon's ReloadTime printed | yes |
| E6 (server) | `... --test-case=variants` | four distinct `weapon_hash`; 2,000 server shot offsets per variant fit its radial CDF (KS D ≤ 0.0364); recoil after a 10-round burst = the variant's recurrence within 1e-4°; a mismatched cfg gives another hash | 4 distinct hashes, client-built = server for each; KS D 0.0158 for each variant (0.25-0.26 against the other disc); recoil pitch 0.230000° (linear) / 0.397755° (exponential), diff ≤ 4.5e-9°; 600 ticks of mixed fire, 0 precondition failures; mismatch hash differs | yes |
| E17 (server) | `... --test-case=hostile` | wanted slot 15, view_back 65,535 with a view_seq outside the history, fire-mode spam, KitSelect team 7, 4× command rate change no other soldier; clamps counted; tick unaffected | 0 / 900 ticks with another soldier's state or weapon hash different from the control run (3,600 soldier-ticks); slot field clamped 900; 84 of 86 shots refused, the rest clamped by the provider; 899 fire-mode changes (≤ 3 per command); 90 / 90 KitSelects refused; 0 precondition failures; tick p99 0.120 ms (control 0.126; noisy) | yes |
| E17 install | `... --test-case=install` (last) | layout.toc, cas.cat, every .sb / .toc unchanged | `install.unchanged: clean (256 files)` | yes |
| G1 (M1/S1, regression) | `ctest --preset dev -R "^host\.scale$" -V` (under the quiet lock) | tick p99 ≤ 8 ms | p99 1.212 / 0.783 / 1.454 ms (the three G1 rows; 05:13 run under the sv lock, noisy) | yes |
| M1/S1 regression | `ctest --preset dev -E "<timing rows>"`, then the timing rows serially under the quiet lock | every row passes | 94 / 94 (core.cache_data and core.res skip as on the base); timing rows 16 / 16, among them host.session, host.session.unit, host.scale, godot.m1 | yes |
| asan | `ctest --preset asan -R "^(m2a\.contract\|net\.proto\.unit\|net\.proto\.fuzz\|sim\.lagcomp\.unit\|sim\.weapons\.fuzz\|host\.gunplay\.server)$"` | the new messages, parsers and the hostile row clean under /fsanitize=address | 6 / 6 pass, no sanitizer report (host.gunplay.server all 7 cases, 218 s; install clean) | yes |
| R3 | `bf3dump weapon-sim --check`; `ctest -R "^sim\.weapons" -E perf` | wp W1-W11 bit-equal after R3 | outputs before / after R3 equal bar the timing lines (1,346 lines) | yes |

## Open limits

* The client side of every row (cl), the firefight brains (bt) and the HUD (hud) are other tracks.
* `blast_protected` is reserved (0): wp's step does not separate a blast spared by protection from a miss.
* The others' `firing` field (`other_record`) means "fired since the previous snapshot tick" (the flag is cleared
  when the interval ends), so a 30 Hz receiver sees every burst; how long the 3p muzzle shows is cl's.
* Windowed Godot rows (`godot.m1_load`, `godot.s3_crowd`, `godot.l1_look`, `godot.ui2`, `godot.ui_fonts_shots`) were
  not run on this branch (they open a window; the user may be at the machine). sv touches no Godot code.
