# M2a weapons core: design and contract

Status: implemented (see "As built" below); design revision 2, 2026-10-15. Track "M2a weapons core" (branch `m2a/weapons`), the
first wave of milestone M2a "Infantry gunplay" ([research/PORTING-RESEARCH.md](../../research/PORTING-RESEARCH.md)
§6). The interface headers named here are in the tree and compile (ctest `m2a.weapons.contract`). Their bodies are
the implementation wave's work (§13).

Revision 2 answers the review of revision 1:
* W1's data row could not pass: a 31-round M16A3 cannot fire 134 shots in 10 s. There is now an oracle-only
  `WeaponTable::infinite_magazine`, and the one-magazine row is gated without it (§1, §5.5).
* The target seam could not rewind per shooter. `HitTargetProvider::targets_for(TargetQuery)` now carries the
  projectile's identity, and `refly_projectile` handles a late HitReport (§5.7, §7.1).
* Float projectile state could not meet W2's 1 mm max-range row. Flight is now double, closed form while unpowered
  (§5.8).
* Smaller fixes:
  * the kind partition corrected;
  * `ShotEvent::pellet_cone`;
  * timed phase events for W6;
  * the commit-time rounding;
  * the tracer, threshold and power bounds;
  * the fire-mode counter's modular step;
  * Tap pinned to the containing tick;
  * the synthetic world's extent and the 60 m fallback;
  * W7's hip-fire condition;
  * W11's aim, duty cycle, p99 rank and lock handling;
  * replayed shots discarded;
  * invisible-blocker counts.

Scope of this track: the Godot-free weapon core in `bf3_sim`: the per-soldier weapon state machine with sub-tick
shot times, the counter-based shot RNG, spread and recoil from data with the unmeasured mechanics as switches,
ballistics against the static world and zone-capsule hit targets, damage from the level material grid, explosive
projectiles for the M2a kits, the headless shooting range and its `bf3dump` commands. No GameServer change and no
Godot code in this wave: S1 is changing GameServer in parallel. §7 defines the seam the later M2a integration
uses.

Tags: **[verified]** = read from code, data or a measurement in this tree on the date above. **[plan]** = taken
from the plan or a research report, with its section. **[inferred]** = a design choice or estimate made here.
**[unmeasured]** = a mechanic whose semantics the data does not give. It is a switch in `mechanics.h` with the
plan's default until S5 Session 1 measures it (plan R5, §3.13).

### As built (implementation wave, 2026-10-15)

The implementation wave is in the tree (branch `m2a/weapons`). Every gate below was measured on this machine on
2026-10-15 with the dev preset; W1–W10 run on simulated time and are exact, W11 is wall clock and noisy (the machine
is shared with the other tracks' builds). Each row has one command; the README's "M2a weapons core" section repeats
the thresholds.

| # | command | threshold | measured | met |
|---|---|---|---|---|
| W1 | `ctest -R sim.weapons` (`sim.weapons.unit` synthetic, `sim.weapons` MP_Subway); `bf3dump weapon-sim --check` | §1 W1 | M16A3: 31 shots, first at the press tick's start, all 30 gaps 4,500,000, empty reload at the last shot + 4,500,000; 134 shots in 10 s on the `infinite_magazine` table, every gap 4,500,000. M98B bolt cycle 78,000,000 (tap-to-tap 79,000,000 = 45.57 RPM, reported). JNG-90 / L96 / M40A5 / SV98 78,000,000 / 82,800,000 / 66,000,000 / 75,000,000. AN-94 14 presses, 2 shots 3,000,000 apart, burst to burst ≥ 6,000,000. M16A4 9 presses, 3 shots 4,500,000 apart. 870 shots 28,800,000 apart, 12 distinct pellets, widest 2.979° from the shot direction. M9 smallest gap 9,000,000 | yes |
| W2 | same | 1 cm; 1 mm | drop 0.1779 / 1.5981 m (M16A3 100 / 300 m), 1.0451 / 11.6098 m (M98B 300 / 1,000 m): 0.29–0.44 mm from the arithmetic (the chord sag of one tick). Expiry at 975.0000 m and 3,250.0000 m (0.000 mm). Walls at 0.99 × range hit, 1.01 × missed | yes |
| W3 | `sim.weapons` (arithmetic), `sim.weapons.range` (end to end) | §1 W3 | one-shot ranges .338 head every range, .338 chest 17.4762, M40A5 14.9998, SV98 19.9995, M39 head 50.7445, SKS head 16.2565 m. On Metro (TDM spawn 87, a 96.9 m lane), exact aim: chest 4 / head 2 hits kill at 5 m, 6 / 3 at 60 m | yes |
| W4 | `sim.weapons`; `bf3dump weapon-sim --all` | 119 rows, 0 errors | 172 variants, 119 qualifying, 119 rows, 0 errors | yes |
| W5 | `sim.weapons.range` | 1e-3 | first hit at 5 m on the stand-ins: M16A3 50.0000 / 25.0000 / 25.0000 / 22.7500 (head / chest / body / legs), M98B 223.2500 / 100.7000 / 86.4500 (head / chest / legs), 870 pellet head 18.0000 | yes |
| W6 | `sim.weapons.unit`, `sim.weapons` | exact | M16A3 tactical 108,000,000 (commit +91,800,000, magazine 31), empty 142,200,000 (commit +120,870,000, magazine 30); M9 81,000,000 / 96,000,000 (16 / 15); 870 5 shells at +109,200,000 + k × 40,200,000, done at +270,000,000; a switch before the commit cancels and keeps the old magazine, after it the new one stays; fire between shells interrupts | yes |
| W7 | `sim.weapons.unit`, `sim.weapons.range` | equal hashes; ≥ 99 % | per-tick `soldier_weapons_hash` equal over a 600-tick mixed run and after a mid-burst restore (tick 103, burst_left 2); two `CollisionWorld`s over one `StaticWorldShapes` give equal shot / impact list hashes (synthetic walls; Metro: M16A3 10 s hip fire, 870 taps, M67 throw). Seed 2 vs 1: 134 / 134 (synthetic) and 71 / 71 (Metro) hip-fire directions differ | yes |
| W8 | `sim.weapons.unit`, `sim.weapons.fuzz` (dev and asan) | no crash, every invariant | 43 hostile defs refused with the variant and field named; 17 synthetic defs × 600 random ticks, 0 violations; 2,000 retail mutants (M16A3 / 870 / M67 / RPG-7): 933 decoded, 683 accepted, 683 × 600 random ticks, 0 violations; 1,020 synthetic mutants, 0 violations | yes |
| W9 | `sim.weapons`, `sim.weapons.range` | §1 W9 | M67 spawn 10,200,000 after the release tick's start, detonation 216,000,000 after spawn, 0.003 mm off the closed form, launch (0, 3, 18); blast 112 / 112 / 81.45 / 40.73 / 0. M320 HE dud at a 10 m wall, explosion at 40 m; 112 / 0.35 / 5 and dud 56 / 2.1 / 5. RPG-7 / SMAW arm 30,000,000, dud at 10 m, explosion at 100 m, 70.21 m/s at 3 s, 100 m/s at 6 s. On Metro: stand-ins 1 / 3 / 5 / 8 m beside the M67's rest point take 112.000 / 85.521 / 44.796 / nothing; an RPG-7 at a stand-in 30 m away does 562.5 direct + 112 blast | yes |
| W10 | every install test and command | `install.unchanged` | yes (256 files: sizes and mtimes equal) in every case of `sim.weapons*`, `tools.weapon_sim`, `bf3dump weapon-sim` / `range` | yes |
| W11 | `sim.weapons.perf`; `bf3dump range weapons/m16a4/u_m16a4 --perf` | p99 ≤ 2.0 ms | 64 shooters, 600 ticks, 8,576 shots, peak 76 live, mean lane 24.6 m: mean 0.115 ms, p50 0.112 ms, p99 0.205 ms, max 0.407 ms (bf3dump run: p99 0.281 ms). Wall clock, noisy | yes |

Under the `asan` preset, `m2a.weapons.contract`, `sim.weapons.unit`, `sim.weapons`, `sim.weapons.range`,
`sim.weapons.fuzz` and `tools.weapon_sim` pass (no AddressSanitizer report); `sim.weapons.perf` skips there, since
W11 is a dev-preset measurement (an instrumented build measured p99 68.7 ms, which says nothing about the budget).

Rules the implementation fixed (this document is updated where they live):
* **Data times go through their shortest decimal** (§5.1). The data's floats are not the decimals the oracles quote
  (1.8f is 1.79999995 s = 107,999,997 units). `make_weapon_sim_def` converts every time, rate and fraction through
  the shortest decimal that reads back as the same float (`std::to_chars`, exactly specified, so every machine gets
  the same digits): 1.8 → 108,000,000, 1.3 → 78,000,000, 0.48 → 28,800,000, 3.6 → 216,000,000.
* **MaxHoldTime saturates** at 60 s instead of being refused (§9): the M67's 9999 means "no limit".
* **ReloadType Unknown** is refused only for a weapon with a magazine whose ammo is not unlimited (§9): the knife.
* **Sway modifiers are signed in data** (§9): the suppression DecreasePerSecondModifier of `xp2_hk417`, `xp2_l86` and
  `xp2_lsat` is −10 (found by W4). The bound is |m| ≤ 10,000; the effective MinAngle, MaxAngle and IncreasePerShot are
  clamped at 0, and a negative decrease grows the spread toward MaxAngle while the modifier holds [unmeasured].
* **Reload fields are checked for the reload type that uses them** (the 870's ReloadTimeBulletsLeft −1 is ignored).
* **870 interrupt:** fire between shells interrupts the reload only while no reload request is held; otherwise the
  two alternated for the rest of the tick (found by W8's random commands: 48 dropped timed events).
* **A burst completes after the script ends**: W1's burst rows run 30 settle ticks, so a press in the script's last
  tick is not counted as a short burst.
* **The direct hit and the blast both apply** to a missile's target (562.5 + 112 at the impact point) [inferred, H:W7].

Not wired yet (documented, no effect in this wave): `Mechanics::bullets_pass_soldiers` (a bullet stops at the first
soldier), the SoldierDef ExplosionDamageModifier (1 on Metro, read but not multiplied), spawn-protected bullet damage
(left to the server rules: `HitTarget::spawn_protected` is carried), the fire-mode counter's value at spawn (the
integration sets `mode_counter` from the player's current counter), the zoom block during deploy (not applied to the
sway table), and the stance-transition penalty (off).

Metro diagnostics (`sim.weapons.range`, `bf3dump range`): every Metro soup triangle's material value resolves to a
grid pair (1,114,171 triangles, 92 distinct values, 0 unresolved; terrain included). On the 96.9 m lane every one of
the 55 static hits of an M16A3 run passed one `kSoupInvisible` triangle first and then hit geometry behind it (property
34, Indestructible): R-W10's case of invisible geometry in front of a wall, and nothing was shot through (every pass
ended on a later static hit).
Collision-before-Raycast (R-W2): 0 in the M16A3 run, 2 in the 870 run.

New files beyond §13: `src/bf3_sim/include/bf3/sim/weapons/weapon_checks.h` + `src/weapons/weapon_checks.cpp` (the
oracle rows as one function, shared by the unit test, the install test and `bf3dump weapon-sim --check`),
`weapon_install.h` + `src/weapons/weapon_install.cpp` (the install side for tools and tests: catalog, grid pairs,
materials, soldier, tables, the W4 table), `tests/weapons_support.h` and `tests/weapons_install_support.h`.
Header additions: `slot_flags::kPressPending` / `kReleasePending`, `WeaponTickOutput::timed_dropped`,
`WeaponSimDef::hold_off_reload_until_fire_release`, `WeaponTable::sprint_recover_time` (hashed) with
`WeaponTableOptions::sprint_recover_seconds`, `SurfaceMaterialTable::has_grid`, `FireTimingResult` /
`run_fire_timing` in `shooting_range.h`.

---

## 1. Goal and exit gates

Goal (plan §6 M2a row, the weapons part): shooting stationary targets on Metro reproduces §7.1. That means
800 RPM = 75 ms, the 1.60 m drop at 300 m, the shots-to-kill table and the ×2.0 / ×2.35 headshots, plus the
reload timing against `ReloadTime`. Every number comes from the install through the M0 readers. Two §6 items are
not this wave's: the "disc/recovery variants chosen from S5 Session 1" (the switches exist, §6.9) and the
server-pose hit capsules (R14, §11).

The gates below run on **simulated time**. Shot and phase times are integers (§5.1), so these gates are exact and
deterministic, and they do not depend on how busy the machine is. Only W11 measures wall-clock cost, and it is noisy
on this shared machine (rule 8). Each gate has one ctest, and `bf3dump weapon-sim --check` (ctest
`tools.weapon_sim`) prints the data-backed rows as PASS/FAIL in one command (exit 0 / 1).

| # | criterion | measured by (ctest; command) | pass threshold |
|---|---|---|---|
| W1 | fire timing (§7.1) | `sim.weapons.unit` (synthetic defs with the §7.1 numbers) and `sim.weapons` (defs resolved from MP_Subway), from the per-tick `ShotEvent` times of `step_soldier_weapons`; `bf3dump weapon-sim --check` | **M16A3** (`u_m16a4`): (a) the real magazine: fire held from the press tick gives 31 shots, the first at the press tick's start, all 30 gaps exactly 4,500,000 units (75 ms); then the automatic empty reload; (b) 10 s of held fire on a table built with `infinite_magazine` (oracle only, §5.5), from the same data def and from the synthetic def: 134 shots, every gap exactly 4,500,000. **M98B** (`u_m98b`): the bolt cycle (`kBoltStarted` to `kBoltFinished`) is exactly 78,000,000 units (1.3 s, 46.15 RPM, §7.1 "46.2"); JNG-90 / L96 / M40A5 / SV98 give 1.3 / 1.38 / 1.1 / 1.25 s. **AN-94** (`u_an94`): each press fires 2 shots exactly 3,000,000 apart (1200 RPM). **870MCS** (`u_870`): each shot spawns 12 pellet projectiles with 12 distinct directions inside 3° + spread, and with the Tap script (pinned to the tick that contains the ready time, §10) shots are exactly 28,800,000 apart (0.48 s pump, 125 RPM). **M16A4** (`u_m16_burst`): each press fires 3 shots 4,500,000 apart; one burst per press. **M9**: single shots at least 9,000,000 apart (400 RPM) |
| W2 | ballistic drop, no drag; max range | `sim.weapons.unit` (synthetic) and `sim.weapons` (data defs): the `ProjectileWorld` flight (double, closed form, §5.8) against the synthetic wall world (`range_wall_triangles`: slab and walls out to 3,500 m, 150 m below the eye, so no gated flight meets the 200 m world margin, §10); `bf3dump weapon-sim <w>` drop table | v0 650, g 15 (M16A3 data): the impact height below the launch line at the 100 m and 300 m walls is 0.1775 / 1.5976 m **within 1 cm** (§7.1: 0.178 / 1.60). M98B (650, g 9.81): 1.0449 m at 300 m and 11.609 m at 1,000 m within 1 cm (§7.1 quotes 1.05 / 11.6; README already notes the 1.045). Max range: the horizontal distance at expiry is v0 × TTL within 1 mm (M16A3 975 m, M98B 3,250 m); a wall at 0.99 × range is hit, one at 1.01 × is not |
| W3 | shots to kill (§7.1 rows) | `sim.weapons` (`shots_to_kill`, `one_shot_range` on data defs) and `sim.weapons.range` (end to end through capsule hits on Metro, exact aim; the synthetic world when Metro has no 60 m lane, §10) | M16A3: chest/body 4 and head 2 at ≤ 8 m; 6 and 3 at ≥ 50 m. One-shot ranges within 0.01 m of the values computed from data (§7.1 rounds them): .338 head at every range (+∞); .338 chest 17.476 m; M40A5 chest 15.000 m (80 × 1.25 = 100 exactly kills); SV98 chest 20.000 m; DMR (M39 EBR) head 50.745 m; SKS head 16.257 m. In the range at 5 m and 60 m: 4 / 2 and 6 / 3 hits kill |
| W4 | generated table for every infantry weapon (report) | `sim.weapons`; `bf3dump weapon-sim --all` | one row per qualifying variant: a Bullet projectile, StartDamage > 0, not instant-hit, class not wcUgl / wcPowerTool. That is **119 of the 172** MP_Subway variants [verified 2026-10-15: `bf3dump weapons`]. Each row gives shots to kill per zone at 0 / 10 / 25 / 50 / 75 / 100 m and the one-shot ranges. Gate: 0 errors and the row count equals the qualifying count; the values are reported |
| W5 | body multipliers per material grid | `sim.weapons.range` (capsule hits at 5 m, exact aim) on top of the `core.gameplay.weapons` grid rows | damage per hit within 1e-3: M16A3 head 50.0 (×2.0), chest and body 25.0, legs 22.75 (×0.91); M98B head 223.25 (×2.35), chest 100.70 (×1.06), legs 86.45; 870 pellet head 18.0 (×1.0) |
| W6 | reload timing against ReloadTime | `sim.weapons.unit` and `sim.weapons`, from the exact times of the phase events (`WeaponTickOutput::timed`, `TimedWeaponEvent`; per-tick flags carry no time, and 2.37 s is 142.2 ticks) | M16A3: tactical `kReloadStarted` to `kReloadFinished` 108,000,000 units (1.8 s), empty 142,200,000 (2.37 s); `kReloadCommitted` at `scale_units(duration, 0.85)` = 91,800,000 / 120,870,000 units after the start [unmeasured rule]; a switch before the commit keeps the old magazine, after it the new one; the magazine after a tactical reload is 31, after an empty one 30 (`empty_reload_unchambered` [unmeasured]); the reserve drops by what went in. M9: 1.35 / 1.6 s. 870 (rtSingleBullet): 1.15 s + 0.67 s per shell [inferred composition], so 5 shells from empty take 4.5 s; fire between shells interrupts the reload |
| W7 | identical shot lists for identical seeds | `sim.weapons.unit` (synthetic world, two `CollisionWorld`s) and `sim.weapons.range` (Metro, two worlds over one `StaticWorldShapes`) | equal `shot_list_hash` and `impact_list_hash`, field for field, over 10 s of auto fire plus a 870 and an M67 run. A different match seed changes ≥ 99 % of the spread-drawn directions, measured in hip fire, where every shot's cone is wider than 0 (an ADS first shot with MinAngle 0 has nothing to draw). A run restored from a saved `SoldierWeaponsState` mid-burst equals the run it was saved from (`soldier_weapons_hash` every tick) |
| W8 | hostile data | `sim.weapons.unit` (bound rows) and `sim.weapons.fuzz` (mutated retail WeaponDef blobs; also under `asan`) | each bound of §9 refused with an error that names the field. 2,000 seeded mutations of the retail M16A3 / 870 / M67 / RPG-7 blobs (`decode_weapon_def`, then `make_weapon_sim_def`) never crash. Every accepted def runs 600 ticks of seeded random commands with every invariant held: magazine in [0, capacity], reserve ≥ 0, ≤ 8 shots per tick, live projectiles ≤ the limit, the step budget respected, no NaN in any state |
| W9 | explosive projectiles of the M2a kits | `sim.weapons` (data defs, synthetic world); `sim.weapons.range` (blasts on stand-ins on Metro) | **M67**: launch (0, 3, 18) in the aim frame after SpawnDelay 0.17 s; detonation exactly 216,000,000 units (3.6 s) after spawn, also while bouncing; free flight equals the closed form within 1 cm up to the first contact; blast on targets at 1.0 / 1.5 / 3 / 5 / 7 m = 112 / 112 / 81.45 / 40.73 / 0 within 0.01 hp (linear [unmeasured]). **M320 HE**: (0, 1.5, 45), g −7.2; a hit before 0.5 s gives the dud (56, inner 2.1, r 5), after it the explosion (112, 0.35, 5); direct hit 100 × 2.5. **RPG-7 / SMAW**: (0, 0, 40) from (−0.3, −0.1, 2), 10 m/s² thrust to 100 m/s (6.0 s) [inferred], g −1.5, TTL 10 s, armed after 0.5 s, direct hit 225 × 2.5 (reported, H:W7), blast 112 / 0.35 / 4 |
| W10 | install unchanged | every install test above and `bf3dump range` / `weapon-sim` | the `install.unchanged` row: sizes and modification times of the install's 256 index files equal before and after (`Install::snapshot`, `diff_snapshots`) |
| W11 | weapon cost per server tick (**wall clock, noisy**) | `sim.weapons.perf` (install, RUN_SERIAL) or `bf3dump range --perf`; both call `run_range_perf`. The runner honours the quiet lock before starting ctest, as for every run (rule 7). The test does not hard-code the session's lock path. If the environment variable `OPENBF3_QUIET_LOCK` names a file that exists, the test exits 77 (Skipped) instead of measuring on a busy machine | 64 shooters with M16A3 full auto at 64 Metro spawn points. Each aims level along its own longest clear heading, so bullets fly their lanes instead of dying in the nearest wall. The table is built with `infinite_magazine`: 100 % duty, the worst case, not the ~50 % a real magazine allows. Over 600 ticks, p99 of (64 × `step_soldier_weapons` + `ProjectileWorld::step`) ≤ **2.0 ms** on one thread, dev preset. p99 is the nearest rank, the 594th of 600 sorted ascending, so 6 ticks may exceed it. Mean, p50, max, the shots, the mean lane length and the peak live projectiles are reported. The threshold is a quarter of S1's 8 ms p99 tick budget (plan §6 S1). It is an estimate (§12 R-W4), so the implementation may revise it once with the measured values and its reasons |

Rows that depend on a `[unmeasured]` rule print `unmeasured` next to their verdict (`Mechanics::measured` false).

---

## 2. Module ownership

| library | this track adds | Godot? |
|---|---|---|
| `bf3_sim` | `include/bf3/sim/weapons/`: `sim_time.h`, `weapon_input.h`, `weapon_rng.h`, `mechanics.h`, `hit_target.h`, `surface_materials.h`, `weapon_sim_def.h`, `spread_recoil.h`, `weapon_sim.h`, `damage.h`, `ballistics.h`, `shooting_range.h`; bodies later in `src/weapons/*.cpp` | no |
| `bf3_core` | `gameplay/grid_pairs.h` (the grid's MaterialPairs in order, §4.3); in the implementation wave the WeaponDef additions of §4.5 (blob v2) | no |
| `bf3dump` | `weapon-sim` and `range` commands (`tools/bf3dump/weapon_sim_commands.{h,cpp}`, implementation wave) | no |
| tests | `weapons_contract_tests.cpp` now; `weapons_tests.cpp`, `weapons_install_tests.cpp`, `weapons_range_tests.cpp` later | no |

Plan §2.2 places weapons, damage and the lag-compensation history in `bf3_sim`, and the reduced server pose for hit
capsules in `bf3_anim`. This track keeps the core in `bf3_sim` behind `HitTargetProvider` (§5.7), so bf3_anim's
posed capsules plug in later without bf3_sim linking bf3_anim. No new link edge: bf3_sim already links bf3_core
PUBLIC [verified: src/bf3_sim/CMakeLists.txt], and Jolt stays PRIVATE (no weapons header names a Jolt type).

---

## 3. Threads

| thread | weapons work | rules |
|---|---|---|
| server (`ServerHost`'s thread; integration wave) | per live soldier `step_soldier_weapons` after its motor step, then one `ProjectileWorld::step` over the server's `CollisionWorld` and the present-tick targets | owner thread only, like `CollisionWorld` (m1.md §5.6); deterministic order: slots ascending, projectiles in spawn order |
| client prediction thread (`ClientSession`; integration wave) | the own soldier's `step_soldier_weapons` in prediction and replay, next to the motor | the same pure function and table as the server; `SoldierWeaponsState` goes into the prediction ring |
| loader (level load) | WeaponDefs from the cache (`weapon` blobs), `read_grid_pairs`, `SurfaceMaterialTable::make`, `make_weapon_table` | one-off, a few ms (§8); the table is immutable after it and shared through `shared_ptr<const>` |
| test / tool thread | the shooting range (own `CollisionWorld`, own `ProjectileWorld`) | single thread per run; `bf3dump weapon-sim --all` runs the per-variant arithmetic through `bf3::core::parallel_for` on all cores (`WeaponCatalog::resolve` is thread-safe [verified: weapon_catalog.h]) |

Jolt's narrow-phase queries are thread-safe on an unmodified system, but `CollisionWorld` keeps one temp allocator
and collector set per world [verified: collision_world.h]. So projectiles are queried on the owner's thread only.
M1's B2 note (scratch-taking overloads for worker queries) applies if S1 wants bullets on the job pool.

---

## 4. Data sources in the install (verified 2026-10-15)

All read through `bf3_core` (`WeaponCatalog` / `resolve_weapon`, `MaterialGrid`, `read_hit_capsules`,
`read_soldier_def`) or printed with `bf3dump` (`weapons`, `weapon`, `soldier`, `dump`), read-only. Dumps and TSVs
stayed in the scratch directory.

### 4.1 Weapons

* `bf3dump weapons` on MP_Subway: **172 variants, 172 resolved, 0 errors**, in 6.4 s while a build ran.
  * Projectile kinds (revision 1 misprinted them; recounted from the TSV, they sum to 172):
    * **147 Bullet:** 24 of them wcUgl, plus 2 knives, the repair tool, and the defibrillator with StartDamage 0;
    * **9 Grenade:** 8 × 40 mm LVG and the M67;
    * **5 Missile:** RPG-7, SMAW, Javelin, Stinger, Igla;
    * **11 Other:** ammo bag, C4, claymore, EOD bot, M15, M224, MAV, medic bag, radio beacon, SOFLAM, T-UGS.
  * Bullet variants with StartDamage > 0: 146. Excluding wcUgl (24), wcKnife (2) and wcPowerTool (1) leaves the
    **119** of W4.
* Chain [verified, weapon_resolver.h; topics/01 §1.2]: `SoldierWeaponUnlockAsset` → `SoldierWeaponBlueprint` →
  `SoldierWeaponData` → `WeaponFiringData` {DeployTime, DisableZoomOnDeployTime, WeaponSway → `GunSwayData`,
  PrimaryFire → `FiringFunctionData` {`ShotConfigData`, `FireLogicData` {`BoltActionData`, `HoldAndReleaseData`},
  `AmmoConfigData`, `SoldierWeaponDispersion`}} → `ProjectileData` → `BulletEntityData` / `MissileEntityData` /
  `GrenadeEntityData` + `VeniceExplosionEntityData` (explosion, dud) + `WeaponSuppressionData` +
  `MaterialContainerPair`.
* Values the gates use, from `bf3dump weapon <variant>` (MP_Subway):

| variant | modes, rate | bolt | reload (tac / empty / delay / threshold) | ammo | shot speed, offset | projectile | damage, falloff | g, TTL | pair → property | zones head / body / chest / legs |
|---|---|---|---|---|---|---|---|---|---|---|
| `m16a4/u_m16a4` | Auto + [Single], 800 | inert 0.5 + 1 | 1.8 / 2.37 / 0 / 0.85 | 31 × 5 | (0,0,650) | `556x45mmnato` | 25 → 18.4, 8–50 m | −15, 1.5 s | `5b6b84ac` → 77 BulletDamage | 2 / 1 / 1 / 0.91 |
| `m16a4/u_m16_burst` | Burst + [Single], 800, 3 @ 800 | inert | as M16A3 | 31 × 5 | (0,0,650) | as M16A3 | as M16A3 | as M16A3 | as M16A3 | as M16A3 |
| `model98b/u_m98b` | SingleWithBolt, 360 | 0 + 1.3 | 2.9 / 4.7 / 0 / 0.75 | 6 × 7 | (0,0,650) | `338magnum` | 95 → 59, 15–150 m | −9.81, 5 s | `4a899d88` → 166 Magnum | 2.35 / 1 / 1.06 / 0.91 |
| `an94/u_an94` | Burst + [Auto, Single], 600, 2 @ 1200 | inert | 2.45 / 3.3 / 0 / 0.75 | 31 × 5 | (0,0,600) | `545x45mmwp` | 25 → 18.4 | −15, 1.5 s | 77 | 2 / 1 / 1 / 0.91 |
| `remington870/u_870` | SingleWithBolt, 600 | 0 + 0.48 | −1 / 0.67 per shell / 1.15 / 1 (rtSingleBullet) | 5 × 7 | (0,0,300) × 12 pellets, cone 3° | `12g_buckshot` | 18 → 6, 8–25 m | −9.81, 1 s | `522396a0` → 88 Shotgun | 1 / 1 / 1 / 0.91 |
| `m9/u_m9` | Single, 400 | inert | 1.35 / 1.6 / 0 / 0.75 | 16 × 4 | (0,0,380) | `9x19mm_pistol` | 25 → 13.75, 12–50 m | −15, 1.5 s | 77 | 2 / 1 / 1 / 0.91 |
| `gadgets/m320/u_m320_he` | Single, 250 | inert | − / 2.9 (rtMagazine) | 1 × 4 | (0,1.5,45) at (−0.1,−0.3,0.86) | `40mmgrenade` (Bullet) | 100 → 100; arm 0.5 s | −7.2, **0 s** | `fd76a079` → 116 40mm | 2.5 all |
| `rpg7/u_rpg7`, `smaw/u_smaw` | Single, 100 / 350 | inert | − / 4.8, 5.8 | 1 × 4 | (0,0,40) at (−0.3,−0.1,2) | Missile `rpg7projectile`, `smaw_he_projectile` | direct 225 | −1.5, 10 s | `58cccf46` → 87 Missile | 2.5 all |
| `m67/u_m67` | HoldAndRelease, 300 | inert | − / 1.0 | 1 × 1 | (0,3,18), SpawnDelay 0.17 | Grenade `m67_projectile` | none (body 0) | −9.8, 3.6 s fuse | `b99f4d03` → 109 ThrowingWeapon | 0 all |
| `knife/u_knife` | Single, 175 | − | − | 1 × −1 | (0,0,1.8), SpawnDelay 0.3 | Bullet, InstantHit | 100 | 0, 1 s | → 113 MeleeDamage | 0.67 / 0.5 / 0.5 / 0.5 |

* `FireLogicData` fields the resolver does not carry yet, read from `bf3dump dump` of the blueprints:
  * M98B: `HoldBoltActionUntilFireRelease` true, `ForceBoltActionOnFireTrigger` true,
    `HoldOffReloadUntilFireRelease` true, `ForceReloadActionOnFireTrigger` true.
  * 870 buckshot: all four false; 12g frag pump 0.55 s; slug 0.48 s.
  * `AlwaysAutoReload` is true on every firing data dumped.
  * The M67 `HoldAndReleaseData`: Delay 0.5, MaxHoldTime 9999, Min/MaxPowerModifier 1 / 1, PowerIncreasePerSecond
    0.1, KilledHoldingPowerModifier 0.34, ForceFireWhenKilledHolding true.
  * Automatic rifles carry an inert BoltActionData (0.5 + 1.0, both hold flags true); it never applies outside
    SingleWithBolt.
* Projectile records (`bf3dump dump` of `rpg7projectile`, `m67_projectile`, `40mmgrenade`):
  * Missile: EngineStrength 10, MaxSpeed 100, EngineTimeToIgnition 0, EngineTimeToLive 40, Drag 0, TimeToArm 0.5,
    TimeToLive 10, MaxTurnAngle 90, ImpactImpulse 5000.
  * Grenade: CollisionSpeedMultiplier 0.3, MinBounceSpeed 0.5, DetonateOnTimeout true, TimeToLive 3.6,
    GrenadeType_Frag.
  * 40 mm: Bullet with TimeToLive 0, TimeToArmExplosion 0.5, DetonateOnTimeout false. Every record's own
    InitialSpeed is 350 and is overridden by the shot (topics/01 C3).

### 4.2 Explosions

`VeniceExplosionEntityData` (BlastDamage / InnerBlastRadius / BlastRadius / MaxOcclusionRaycastRadius; every one has
DisableOcclusion false and DisableStaticEntityOcclusion true):

| owner | explosion | dud | its pair → property |
|---|---|---|---|
| 40 mm HE | 112 / 0.35 / 5 / 0.25 | 56 / 2.1 / 5 / 0.5 | 79 BlastDamage, dud 139 Dud |
| RPG-7 (SMAW the same) | 112 / 0.35 / 4 / 0.25 | 56 / 2.1 / 4 / 0.25 | 79 BlastDamage, dud 139 Dud |
| M67 | 112 / 1.5 / 7 / 0.5 | — | 80 Grenade |

On the Metro grid every explosion pair's cell against Head / Body / Chest / Foot has DamageProtectionMultiplier 1.0
[verified: grid dump]. The thresholds are 15, or 10 for the M67 on head, body and chest. So blast damage is the
falloff alone (×1.0) until the WeaponDef carries the explosion's pair (§4.5).

### 4.3 Material grid and pairs

* `levels/mp_subway/mp_subway/materialgrid_win32/grid` (`MaterialGridData`) has MaterialIndexMap 180,
  InteractionGrid 102 × 102 and **MaterialPairs 32,400**. It holds 399 `MaterialRelationDamageData`, 165
  `…PenetrationData`, 223 `…EffectData`, 254 `…SoundData` and 110 `…DecalData`.
  `materials/materialcontainer` (`MaterialContainerAsset`) has 180 names and 32,400 pairs [verified: grid dump;
  `MaterialGrid::load` reports 32,400 referenced, 0 unresolved].
* **Index space of a static hit.** The soup's `MaterialFlagsAndIndices` low 24 bits index the grid's MaterialPairs
  [verified: collision_soup.h `kMaterialPairMask`]. On Metro the low 24 bits of every container pair's own
  FlagsAndIndex equal its position in the grid list: 32,400 of 32,400 [verified: grid dump]. `MaterialGrid` keeps
  pairs by GUID only, so `grid_pairs.h` adds the ordered list as a new file (the M0 reader is unchanged).
* Penetrable pairs (FlagsAndIndex bit 24): 1,800 of 32,400 [verified; topics/01 C9 / extra-3].

### 4.4 Soldier and capsules

* `characters/soldiers/defaultsoldierbonecollision`: 13 `BoneCollisionData`, 9 hi-LOD [verified:
  core.gameplay.weapons]. Head r 0.15, upper-spine sphere r 0.25 "Chest", lower spine r 0.23, arms r 0.08 len 0.28,
  thighs r 0.11 len 0.40 and shins r 0.10 len 0.43, the leg capsules with `DeactivateIfBehindWall`. Zone properties:
  Head 65, Body 66, Chest 158, Foot 91 (topics/01 §1.7).
* `bf3dump soldier` (MP_Subway):
  * MaxHealth 100, ExplosionDamageModifier 1, ImmortalTimeAfterSpawn 2.
  * Poses: stand 1.8 / eye 1.6, crouch 1.15 / 1.0, prone 0.3 / 0.3; prone footprint box_min (0.3, 0, 1.3),
    box_max (0.3, 0, 1.1).
  * SoldierSprintSettingsData RecoverTime 0.12.
  * Suppression: SuppressionSphereRadius 1.5, Low / High / UI thresholds 0.3 / 0.4 / 0.15, Decay 0.5, FallOffDelay 2.
* Level settings (topics/01 C10, verified there): `SyncedGameSettings.BulletDamageModifier` 1,
  `AllowClientSideDamageArbitration` true, `MaxAllowedLatency` 0.85, `MaxSimFps` 30.

### 4.5 What WeaponDef must gain (implementation wave, this track)

Three things the sim needs are not in `WeaponDef` today:

* `Explosion::material` (MaterialPairRef), for the blast zone cells;
* `FireFunction::hold_off_reload_until_fire_release`;
* `FireFunction::force_reload_action_on_fire_trigger` (`force_bolt_action_on_fire_trigger` is there).

Adding them changes the blob, so the implementation bumps `kWeaponBlobFormatVersion` and `kWeaponConverterVersion`
from 1 to 2. Every `weapon` key changes and the old blobs age out through LRU (README "Preparing the cache"). The
resolver, blob and format edits are additive, and the `bf3dump weapon` text gets the new lines at the end of their
blocks. The VFX and audio tracks may also need WeaponDef fields (FireEffects1p/3p, the sound patch). §16 asks the
orchestrator to route every WeaponDef change through this track, so the blob version moves once.

As built: the three fields are in `weapon_def.h` (at the ends of `FireFunction` and `Explosion`; `MaterialPairRef`
moved above `Explosion`), read by `weapon_firing.cpp` / `weapon_projectile.cpp`, written at the ends of the blob's
fire and explosion records, and printed at the ends of the `bf3dump weapon` reload and explosion lines. Blob format
and converter versions are 2.

---

## 5. Algorithms and key decisions

### 5.1 Sub-tick time (`sim_time.h`)

**Decision:** times are int64 units of 1 / (kSimHz × 10^6) s, so a tick is exactly 1,000,000 units. A shot,
phase end or spawn keeps its exact time inside a tick.

Plan §3.2 and topics/01 §5.2 proposed integer microseconds, but 1/60 s is 16,666.67 µs, not an integer. Tick-based
units make both the tick boundaries and the data's intervals exact: 75 ms = 4,500,000, 50 ms = 3,000,000,
1.3 s = 78,000,000, 0.48 s = 28,800,000, 2.37 s = 142,200,000 (static_asserted in the contract test). Rates that do
not divide (650 RPM = 5,538,461.5) are rounded once, when the def is made, and the rounded integer is in the def
hash. Server and clients then add the same integer forever: a 650 RPM weapon drifts 0.5 units (8 ns) per shot from
the ideal, and never from each other. A 30 Hz build (D4 fallback) only changes kSimHz: 75 ms is then 2,250,000.

**As built: data times go through their shortest decimal.** The data's floats are not the decimals the oracles
quote: 1.8f is 1.79999995 s, 107,999,997 units if converted as it stands. `make_weapon_sim_def` converts every
time, rate and fraction once through the shortest decimal that reads back as the same float (`std::to_chars`, an
exactly specified algorithm, so every machine gets the same digits), then rounds to the nearest unit: 1.8 →
108,000,000, 1.3 → 78,000,000, 0.48 → 28,800,000, 3.6 → 216,000,000. The integers are what the def stores and hashes.

Tick convention (M1's): the command for tick T drives [tick_start(T), tick_end(T)). A trigger held in command T
fires at tick_start(T) when the weapon is ready (`t = max(next_shot_time, tick start)`, plan §3.2).

### 5.2 Inputs and context (`weapon_input.h`, `WeaponContext`)

* `Command` already reserves kFire, kZoom and kReload [verified: command.h]. This track adds a 2-bit fire-mode
  toggle counter (bits 6–7) and a 4-bit wanted-slot field (bits 8–11, 0 = keep), inside the 24 wire bits.
* **Every weapon field is a level, never an edge.** `repeated_command` clears only `buttons::kEdgeMask` (kJump).
  A repeated or lost command must not cycle the fire mode twice or drop a switch, and with levels it cannot: a
  repeated "slot 2" is idempotent and an unchanged counter means no change. The mode cycles
  `fire_mode_steps(old, new)` = (new − old) mod 4 times, so a counter that moved by 2 across a lost command cycles
  twice, as pressed. Four or more presses between two received commands alias. That is a known limit of the 2-bit
  field, accepted because commands go out 3–4 times redundantly (plan §3.5). Single fire, burst starts and the
  bolt's "until fire release" compare the fire level with the previous command's, which the slot keeps
  (`kTriggerDown`). The contract test checks that a repeat keeps every weapon field, and it pins `fire_mode_steps`.
* `command.h` itself does not reserve bits 6–11 yet; only `weapon_input.h`'s static_asserts guard them, against
  kJump / kSprint / kUse. The integration wave adds a reserved-range comment and constants in `command.h`, so that
  another track adding a button cannot collide silently. Until then, §16 asks the other tracks to stay clear of
  bits 6–11.
* The soldier feeds the weapon (plan §3.1) through `make_weapon_context(motor, before, after, command)`:
  * eye at the tick's start and end: feet + the pose's eye height, blended through transitions by the motor's
    `eye_height`;
  * the command's view;
  * the sway context:
    * stance: the target pose during a transition [inferred];
    * zoom: kZoom held;
    * move: Sprinting when kSprinting; Jumping for Jumping, InAir and Falling (only stand has a jumping entry);
      Moving above 0.1 m/s horizontal [inferred]; else Base;
  * alive and sprinting.

  A sub-tick shot's origin is the eye lerped at the shot's fraction [inferred], so a strafing shooter's shots
  leave from where the eye was.
* Zoom state is instant for the spread table. The aiming controller's zoom transition (0.09–0.25 s, topics/01
  §1.4) is not resolved into WeaponDef. It belongs to the viewmodel / ADS wave (§11).

### 5.3 Fire modes (`step_soldier_weapons`)

Per mode (`FireModeDef`, from FireLogicType then FireLogicTypeArray in data order; the counter cycles them):

| logic | rule |
|---|---|
| Automatic | while fire is held, shots at `next_shot_time`, `next += interval` (60 / RateOfFire). Releasing never resets `next_shot_time`, so tapping cannot beat the rate |
| Single | one shot per press, and not before `next_shot_time` (interval). A press while not ready is remembered until the weapon is ready, if fire is still held [inferred] |
| Burst | a press starts `BulletsPerBurst` shots at the burst interval (60 / RateOfFireForBurst). The burst completes when released [inferred], and holding fires no second burst. The next burst may start `interval` after the burst's last shot [inferred]: AN-94 2 @ 50 ms, then ≥ 100 ms; M16A4 3 @ 75 ms, then ≥ 75 ms |
| SingleWithBoltAction | a single shot, then the bolt / pump cycle of BoltActionDelay + BoltActionTime (topics/01 C8). With `HoldBoltActionUntilFireRelease` (M98B) the cycle starts at the start of the first tick whose command releases fire; without it (870) it starts at the shot. The next shot is at max(interval end, cycle end). So a tapping M98B fires every 1.3 s + 1 tick (45.6 RPM; the release cannot come before the end of the pressing tick), while the cycle itself is exactly 1.3 s. W1 gates the cycle, as Symthic's 46.2 RPM counts it. The tap gap is reported. The 870 (no hold) with the Tap script presses in the tick that contains the ready time, so its shots are exactly the pump apart (§10) |
| HoldAndRelease (M67) | pressing enters Holding. Releasing after HoldDelay (0.5 s) throws: the shot is at the release tick's start and the projectile spawns SpawnDelay (0.17 s) later. Power = clamp(min + PowerIncreasePerSecond × held, min, max), 1.0 for the M67. A release before HoldDelay throws once the delay is reached [inferred]. Death while holding (ForceFireWhenKilledHolding) drops the grenade at KilledHoldingPowerModifier power, in the integration wave |
| instant hit (knife) | a shot whose projectile is one segment of speed × TTL (1.8 m) at its spawn time (SpawnDelay 0.3 s) |

* Sprint blocks fire, and it stays blocked for RecoverTime 0.12 s after the sprint ends
  (`Mechanics::sprint_blocks_fire` [inferred]). The Deploying and Reloading phases block fire too; a reload is
  interrupted by fire only for rtSingleBullet between shells.
* `PreFireDelay` is 0 on every dumped firing data; it is supported as an offset of the first shot.
* `ClientFireRateMultiplier` 0.67 (meaning unknown, H:W8) is not used.
* The cap is kMaxShotsPerTick = 8 shots per soldier per tick. The def builder refuses an interval below tick / 8
  (28,800 RPM), so the cap never cuts a valid def. The fastest retail rate is 1,000 RPM, 3.6 ticks per shot.

### 5.4 Seeds and determinism (`weapon_rng.h`)

`shot_seed(match_seed, player_slot, shot_index)` = SplitMix64 of SplitMix64(match_seed ^ slot << 32) ^ shot_index.
Every draw is `shot_random(seed, purpose, lane)`: spread radius and angle, per-pellet radius and angle, horizontal
recoil, fuse variation. `next_shot_index` counts every shot of a player slot over the match and is never reset, so
the slot plus the index name a shot uniquely across lives and weapons. That is the "hash(match seed, player slot,
shot index)" of the task, and it covers the plan's life/slot terms (plan §3.2).

No generator state exists. The server, the predicting shooter and a remote client holding the `ShotEvent` draw the
same numbers in any order. The values are pinned in `m2a.weapons.contract`. The hash is integer-only. The float
work on the replicated path avoids libm: the disc azimuth uses a polynomial sin/cos in `spread_recoil.cpp`, and the
exponential recovery factor is precomputed in the def (plan R4, as `make_motor_params` does for the gains,
m1.md §6.10).

### 5.5 Ammo, reload, switch, deploy

* Spawn: every slot full; magazine = MagazineCapacity (31); reserve = NumberOfMagazines × capacity [inferred]. A
  negative NumberOfMagazines means unlimited (the knife's 1 × −1) [inferred].
* **`WeaponTable::infinite_magazine` (oracle only).** Shots take no rounds, so a held trigger fires for as long as
  the script runs. W1's 10 s rows use it: a real 31-round M16A3 empties after 31 shots (2.25 s), then reloads for
  2.37 s. W11's full-duty load uses it too.
  * It is set through `WeaponTableOptions` by tests and `bf3dump` only; `GameConfig` never exposes it.
  * It is part of `weapon_table_hash`, so a table built with it cannot pass MatchInfo against a real one.
  * W1's one-magazine row and every W6 row run without it.
* Reload starts on a request (kReload held) when the magazine is below capacity and the reserve is not empty, or
  automatically with AlwaysAutoReload when the magazine is empty and the weapon is next ready. The M98B's
  HoldOffReloadUntilFireRelease holds it until fire is released (§4.5).
  * rtMagazineWithPossibleShorterReload: ReloadTimeBulletsLeft with rounds left (tactical), ReloadTime when empty.
  * rtMagazine: ReloadTime.
  * rtSingleBullet (870): ReloadDelay, then one shell per ReloadTime. Fire between shells ends the reload with the
    shells loaded so far [inferred].
* **ReloadThreshold [unmeasured, H:W9]:** the reload is committed (`kReloadCommitted`, the rounds move into the
  magazine) once threshold × duration has passed. A switch before that cancels the reload and keeps the old
  magazine (rlWeaponSwitchCancelsUnfinishedReload). After it, the new magazine stays even if the switch interrupts
  the rest. This reading follows plan §3.2 "Open" and Symthic ("unable to change weapons until the threshold").
  * The commit points are rounded once, in `make_weapon_sim_def`: `scale_units(duration, threshold)`, the nearest
    unit, half up. They are stored as `reload_commit_tactical` / `reload_commit_empty` and hashed, so the server
    and a client never round separately. M16A3: 91,800,000 and 120,870,000 units, static_asserted in the contract
    test.
  * Phase events carry their exact times (`WeaponTickOutput::timed`, at most 16 per tick). W6 and the viewmodel
    and audio cues read them.
* Rounds after an empty reload are capacity − 1 (30) under `empty_reload_unchambered` [unmeasured; Symthic 30 + 1].
* Switching: a wanted slot with a weapon starts its deploy (DeployTime); during the deploy the weapon cannot fire,
  and zoom is refused for DisableZoomOnDeployTime. There is no holster time (none in the data). AltDeployTime
  (2 s on pistols, unexplained) is read but unused [inferred]. Ammo, spread and recoil stay with each slot.

### 5.6 Spread and recoil (`spread_recoil.h`, `mechanics.h`)

* **Dispersion in effect** = `sway[stance][zoom].move[m]`. A missing move entry (crouch and prone have only Base and
  Moving) falls back to Base. It is multiplied by the suppression modifier while suppressed (`suppression ≥ 0.3`;
  MaxAngleModifier 0 = unset, topics/01 extra-7) and by every conditional (bipod / light) modifier whose gate holds.
  M2a has no supported-shooting or light input, so those gates are false. The multiplication is
  `gameplay::apply_sway_modifier`'s rule [verified: weapon_def.h].
* **Spread state** per slot: (spread, spread_time).
  * Decay is evaluated lazily at each shot time: s = max(min, s − DecreasePerSecond × Δt).
  * When the context's minimum rises (stand → moving), spread starts from max(s, new min) (MinSpreadRise::Immediate
    [unmeasured H:W2]).
  * After a shot: s = min(s + IncreasePerShot, max).
* **Direction:** the aim is the command view turned by the recoil offset. The shot direction is a draw in the cone
  of half-angle s: radius = s·√u (UniformArea, plan default) or s·u (UniformRadius) [unmeasured H:W1], azimuth
  2π·u. Each pellet then adds its own draw inside the stance's pellet cone (870: 3°) around the shot direction
  [inferred]. The cone's half-angle travels on the shot (`ShotEvent::pellet_cone`), so a remote client expands the
  pellets exactly as the server does without knowing the shooter's stance.
* **Recoil state** per slot: a view offset (pitch, yaw), with + yaw = left in the data's sign (topics/01 C7).
  * Per shot: pitch += RecoilAmplitudeIncPerShot, × FirstShotRecoilMultiplier when the first-shot flag is armed;
    yaw += lerp(HorizontalMin, HorizontalMax, u). Each is capped at its amplitude max.
  * Recovery runs once per whole tick at the tick's end: Linear (the decrease factor in degrees per second, the
    plan default) or Exponential (× the precomputed per-tick factor) [unmeasured H:W3]. While firing it is scaled
    by ShootingRecoilDecreaseScale (0.1) [inferred].
  * The first-shot flag re-arms on trigger release (FirstShotRearm::OnTriggerRelease [unmeasured H:W4]).
  * Horizontal draws are independent uniforms. Symthic's "not independent draws" stays open.
* **The recoil offset lives in the weapon state, not in the Command.** The command keeps the player's own view,
  and the camera shows view + offset from the predicted state. Server and client compute the offset identically
  from the same inputs and seeds. If the client folded recoil into its view instead, the offset would round
  through the view's quantisation every tick and could not be recomputed on the server. Plan §3.2's "view after
  recoil" in the fire command is therefore this offset, derivable on both ends; nothing extra goes on the wire.
* GunSwayStanceTransition penalties stay off (`stance_transition_penalty` false): how MaxPenaltyValue / CoolDown
  apply is unknown (topics/01 §6). The camera spring (CameraRecoilData 250 / 0.35) is presentation (viewmodel wave).

All the [unmeasured] rules are fields of one `Mechanics` struct. The defaults are the plan's no-measurement choice
(uniform-area disc + linear recovery, plan §3.13), and `measured` is false. The struct is part of
`weapon_table_hash`, so a server and a client that chose differently refuse each other at MatchInfo instead of
mispredicting.

### 5.7 Hit targets (`hit_target.h`)

* A `HitTarget` is up to 16 world-space `ZoneCapsule`s: segment a..b, radius, zone, BoneCollisionData index,
  property and the behind-wall flag, plus a bounding sphere.
* **The query carries the projectile's identity** (revision 2). `HitTargetProvider::targets_for(TargetQuery)`
  returns the set one segment is tested against. `TargetQuery` holds:
  * the segment's start and end times;
  * the shooter's player slot, the shot index, the pellet and the def;
  * why the provider is asked: Flight, Blast or Refly.

  Rewind in plan §3.2 / D7 is per shooter and per bullet. A human shooter's unreported targets are tested at that
  shooter's render time, which differs per client. Bots fire at the present. A reported target is tested at the
  report's (tick_a, tick_b, alpha). A provider that saw only a time could not give two bullets of two shooters in
  the same tick two different worlds. With the identity it can, so the integration needs no header change.
  `ProjectileWorld` asks once per projectile sub-step and once per blast. A rewinding provider caches its last
  answer keyed by (shooter, sample), so 64 shooters cost at most 64 rewinds per tick. The providers:
  * **this wave:** `StaticTargets` with stationary stand-ins; it ignores the identity;
  * **integration:** the server's soldiers. A bot's projectiles see the present tick. A human's see the lag-comp
    ring at that shooter's render time, which comes from the interpolation delay its commands acknowledge;
  * **integration / R14:** bf3_anim's posed capsules in the same ring.
* **Late HitReports** (the integration wave's rule, written down here because the seam must allow it). A report
  usually arrives after the server has flown that bullet live. The server keeps each human shooter's `ShotEvent`s
  for 250 ms plus the longest bullet flight. On a report it calls `refly_projectile(shot, pellet, until)`, which
  flies that single pellet again against the static world and a provider that answers the Refly query with the
  reported target posed at the reported sample. Flight is a pure function of (ShotEvent, def, static world), so the
  re-flown path is the live one; it costs a few segments.
  * The report is accepted when the re-flight hits that target, within the tolerance of plan §3.2.
  * Damage is applied at most once per (shot, pellet). If the live flight already damaged a target with that
    pellet, a report naming another target is refused and counted: the first resolution wins [inferred]. The
    integration may tune this rule, but it needs no change to these headers.
* **Stand-ins** (`standin_target`, [inferred] layout until R14). Radius, length, zone and pair come from the 9
  hi-LOD capsules. Placement uses a fixed table, in metres above the feet in the soldier's frame (x left, z
  forward), scaled to the SoldierDef pose:

| capsule | stand (h 1.8, eye 1.6) | crouch (h 1.15, eye 1.0) | prone (h 0.3; along +z inside the footprint) |
|---|---|---|---|
| head r 0.15 | centre y = eye + 0.05 = 1.65 | y 1.05, z +0.1 | y 0.25, z +0.95 |
| upper spine "Chest" r 0.25 | y 1.30 | y 0.80, z +0.05 | y 0.20, z +0.55 |
| lower spine r 0.23 len 0.10 | y 1.00–1.10 | y 0.55–0.65 | y 0.18, z +0.20..+0.30 |
| arms r 0.08 len 0.28 | x ±0.22, y 1.40 → forward 0.28 | x ±0.22, y 0.85 → forward | x ±0.20, y 0.12, z +0.65..+0.93 |
| thighs r 0.11 len 0.40 | x ±0.10, y 0.92 → 0.52 | x ±0.12, y 0.45, z 0 → +0.40 (bent) | x ±0.12, y 0.12, z −0.05..−0.45 |
| shins r 0.10 len 0.43 | x ±0.10, y 0.48 → 0.05 | x ±0.12, y 0.45 → 0.05 at z +0.40 | x ±0.12, y 0.10, z −0.50..−0.93 |

  The prone body lies inside the data footprint (x ±0.3, z −1.3..+1.1). The gates aim at capsule centres
  (`zone_aim_point`), so W3 and W5 do not depend on these inferred positions; only "what a random shot hits"
  does.
* `segment_capsule` is the exact segment-vs-capsule quadratic in double: earliest fraction, outward normal; a start
  inside hits at 0. `segment_target` tests the bound sphere first, then every capsule. The earliest wins and ties
  go to the lower capsule index, so the result is deterministic.
* `DeactivateIfBehindWall` (legs): a leg hit is skipped when a static ray from the shot's origin to the hit point is
  blocked [inferred, topics/01 §5.3].

### 5.8 Ballistics (`ballistics.h`)

* **Projectiles are queries, never bodies** (plan §3.2, D5).
* **Flight state is double, and unpowered flight is the closed form** (revision 2). A projectile keeps a base
  state: the time, position and velocity at its last discontinuity (spawn, bounce, engine phase change). Its
  position at time t is base_position + base_velocity·τ + ½·a·τ², with τ = units_to_seconds(t − base_time) and
  a = (0, g, 0), evaluated in double and never accumulated.
  * Why revision 1's float steps failed: float32 per-tick steps of p += v·dt + ½·a·dt² end 0.67 mm short at
    975 m (M16A3) and 5.86 mm short at 3,250 m (M98B), against W2's 1 mm max-range row. That is the review's
    figure, reproduced here. In double, closed form, the error is far below 1 µm, because the sample times are
    exact integers (§5.1).
  * Only the missile's thrust phase is stepped, in double, re-basing every sub-step.
  * The static ray itself is float (`CollisionWorld::ray`); its ulp at 3 km is 0.24 mm, inside every gate.
  * W2 compares the impact on the synthetic walls with the arithmetic.
* **No drag:** bullets have no drag field; the missile's Drag (0 on RPG and SMAW) is read as a = −drag·v [inferred,
  inactive].
* **Sub-steps.** The chord between two samples sags g·h²/8 below the parabola: 0.52 mm for a whole tick at g 15,
  well inside W2's 1 cm. So bullets use one segment per tick. Missiles with the engine on use 4 per tick, with
  thrust along the velocity up to MaxSpeed. A shot spawns at its exact spawn time and flies the rest of that tick
  at once (plan §3.2, "advanced by tickEnd − t on spawn").
* **Segment tests**, earliest first:
  * Static world: `CollisionWorld::ray(query_mask::kProjectile)`, which sees the Collision and Raycast layers. The
    Character layer is soldier-only collision. This settles m1.md's "[inferred, M2a decides]" for the mask. Two
    kinds of hit are passed through by re-casting from 1 mm past the hit point, at most 8 re-casts per projectile
    per tick:
    * invisible blockers (`kSoupInvisible`, player clip: topics/01 §5.5 "bullets ignore"). The flag comes from an
      owner name containing "invisible" or "collisionmesh" (collision_soup.cpp `invisible_name`), and it is not a
      `Mechanics` switch. So every pass is counted (`ProjectileStepOutput::invisible_passes`), and the range
      reports the count on Metro. A wall whose only bullet geometry is a "collisionmesh" would show up there
      (§12 R-W10);
    * penetrable surfaces: the pair's bit 24 is set and the (projectile, surface) cell has no NeverPenetrate. The
      damage scales by the cell's DamagePenetrationMultiplier [inferred rule, topics/01 C9]: chicken wire,
      pass-through fences, BulletsOnly.
  * Targets: the provider's answer to the projectile's own `TargetQuery` (§5.7).
* **Collision versus Raycast bodies.** Both layers hold the same props: the hull for movement, the detailed shell
  for rays. A hull that lies outside the shell would catch bullets a few centimetres early. The range test reports,
  for every static hit on the Collision layer, whether a Raycast hit lies within 10 cm behind it, and the count goes
  to §12 R-W2. A per-object "ignore the hull where a shell exists" needs a placement id in the SurfaceTag, a T1
  change left to the integration wave if the count says it matters.
* **Explosive projectiles:**
  * Impact (static or target) at age ≥ time_to_arm: explosion. Before it: the dud explosion.
  * The direct damage applies on a target hit either way: 40 mm StartDamage, missile Damage, × the zone cell.
  * Grenades bounce: v' = reflect(v) × CollisionSpeedMultiplier [inferred, 0.3], and they rest below
    MinBounceSpeed (0.5 m/s). The grenade is a point [inferred: the Havok shape is not read].
  * The fuse (DetonateOnTimeout) fires at spawn + TimeToLive exactly, wherever the grenade is.
  * TimeToLive 0 (40 mm) means "until impact", capped at kMaxFlightTime (30 s).
  * A projectile more than 200 m outside the static world's bounds is removed.
* **Blasts**: tested against the provider's answer to a Blast query at the detonation time.
  * Distance = from the detonation point to the nearest point of the target's capsules [inferred].
  * Occlusion rays run from the blast to each capsule's centre against the Collision layer (`blast_occlusion`
    [unmeasured]). The ray's radius (MaxOcclusionRaycastRadius 0.25 / 0.5) is not modelled; a ray is a line.
  * Damage is applied once per target.
* **Outputs:**
  * `ImpactEvent`: time, position, normal, velocity, grid pair, material value, physics_material (the effects
    row), physics_property, surface class, layer, penetrated, target and zone. This is the impact descriptor the
    VFX and audio impact selection reads by PhysicsMaterialIndex (plan §3.2 r2, §3.14).
  * `DamageEvent` (§5.9) and `DetonationEvent`.

### 5.9 Damage (`damage.h`)

* **Bullet:** falloff × the (projectile property, capsule property) cell's DamageProtectionMultiplier ×
  BulletDamageModifier (1) × the penetration product.
  * Falloff is StartDamage to the falloff start, linear to EndDamage at the end, flat beyond (Symthic 1.7;
    `damage_at` [verified: weapon_def.h]).
  * The distance is the straight line from the shot origin to the hit (FalloffBasis::StraightLine [unmeasured
    H:W5]). It differs from the path length by under 1 mm at 300 m.
  * Which grid field scales the damage is [inferred]; DamageProtectionMultiplier reproduces the community numbers
    (topics/01 §1.7). DamageProtectionThreshold is not applied (`apply_protection_threshold` [unmeasured H:W6]).
* **Health:** SoldierDef MaxHealth (100). `apply_damage` writes the M1 `SoldierState` fields health, damage,
  kDamaged / kDied and MoveMode::Dead. Damage ≤ 0 health kills, so exactly 100 kills (80 × 1.25 at ≤ 15 m). It is
  refused while spawn-protected [inferred].
* **Shots to kill:** `shots_to_kill(def, distance, health)` = ceil(health / (per-shot damage × pellets)) per zone,
  0 for a zero multiplier. `one_shot_range` bisects to 1 mm over [0, max range]. Both are the arithmetic W3 and W4
  report; the range reproduces them through capsule hits.
* **Explosions:** the blast falloff (Linear [unmeasured]) × the explosion pair's zone cell (1.0 on Metro, §4.2) ×
  ExplosionDamageModifier (1). Penetration does not apply.
* Bullets stop at the first soldier (`bullets_pass_soldiers` false [unmeasured]): the soldier pairs are flagged
  penetrable with ×0.8 in their cells, but whether BF3 lets bullets through is not known.

### 5.10 Suppression carried on the shot

Every `ProjectileSimDef` carries the WeaponSuppressionData values: 7 on 5.56, 10 on `_heavy`, 4 on `_cold`, 20 on
7.62 semi, 35 on bolt / .338 / slug, 50 on RPG and 40 mm [verified via topics/01 C16; bf3dump: M16A3 7..7 over
3–500 m, M98B 35, 870 2..4 over 7–40 m]. The near-miss accumulation on victims (the 1.5 m sphere, decay 0.5 after
2 s) is M7's "suppression" (plan §6). Until then `WeaponContext::suppression` stays 0 and the suppressed spread
modifier never applies.

---

## 6. Interfaces (headers in the tree)

| header | holds |
|---|---|
| `bf3/sim/weapons/sim_time.h` | `SimTime`, `kTimeUnitsPerTick` / `PerSecond`, `tick_start` / `tick_end` / `tick_containing` / `tick_fraction`, `seconds_to_units`, `scale_units` (the commit rounding), `rpm_to_interval` (constexpr) |
| `weapon_input.h` | `weapon_buttons::` (fire, zoom, reload, fire-mode counter, wanted slot), `WeaponInput`, `weapon_input(Command)`, `fire_mode_steps`, `with_weapon_input` (constexpr), `kMaxWeaponSlots` 8 |
| `weapon_rng.h` | `splitmix64`, `shot_seed`, `RngPurpose` (append-only), `shot_random`, `unit_float` (constexpr, pinned) |
| `mechanics.h` | `Mechanics`: every [unmeasured] switch (§6.9) |
| `hit_target.h` | `HitZone`, `ZoneCapsule`, `HitTarget`, `TargetHit`, `TargetQueryKind`, `TargetQuery`, `HitTargetProvider::targets_for`, `StaticTargets`, `segment_capsule`, `segment_target`, `standin_target`, `zone_aim_point` |
| `surface_materials.h` | `SurfacePair`, `SurfaceMaterialTable` (static pair by material value, a pair's view, grid cells, zone properties) |
| `weapon_sim_def.h` | `WeaponSimDef` (with the rounded reload commit points), `FireModeDef`, `ProjectileSimDef`, `ExplosionSimDef`, `WeaponTable` (with the oracle-only `infinite_magazine`), `WeaponTableOptions`, the k* bounds, `make_weapon_sim_def`, `make_weapon_table`, `weapon_table_hash`, `sim_shot_interval_seconds`, `sim_effective_rpm` |
| `spread_recoil.h` | `SwayContext`, `effective_dispersion` / `effective_recoil`, `decay_spread` / `enter_spread` / `spread_after_shot`, `sample_cone`, `RecoilOffset`, `recoil_after_shot`, `recover_recoil_tick`, `aim_direction` |
| `weapon_sim.h` | `WeaponPhase`, `WeaponSlotState`, `SoldierWeaponsState` (POD), `weapon_events::`, `WeaponContext`, `ShotEvent` (with `pellet_cone`), `TimedWeaponEvent`, `WeaponTickOutput` (with `timed`), `spawn_soldier_weapons`, `step_soldier_weapons`, `soldier_weapons_hash`, `make_weapon_context` |
| `damage.h` | `DamageKind`, `DamageEvent`, `falloff_damage`, `zone_damage`, `blast_damage`, `apply_damage`, `shots_to_kill`, `one_shot_range` |
| `ballistics.h` | `DVec3`, `Projectile` (double base state), `ImpactEvent`, `DetonationEvent`, `BallisticsLimits`, `ProjectileStepOutput` (with the static diagnostics), `ProjectileWorld` (`spawn`, `step`), `ballistic_position`, `fly_free`, `refly_projectile` |
| `shooting_range.h` | `RangeLane`, `find_range_lane`, `range_wall_triangles`, `FireScript`, `RangeTargetSpec`, `RangeScenario`, `RangeTargetResult`, `RangeResult`, `run_range`, `RangePerfResult`, `run_range_perf`, `shot_list_hash`, `impact_list_hash` |

Hashes (`weapon_table_hash`, `soldier_weapons_hash`, `shot_list_hash`, `impact_list_hash`) are FNV-1a 64 field by
field in declaration order, with floats by their bit pattern. They never memcpy a struct:
`SoldierWeaponsState` and `WeaponSlotState` have implicit padding before their SimTime fields.
| `bf3/core/gameplay/grid_pairs.h` | `read_grid_pairs(db, grid, MaterialGrid)` |

### 6.9 The unmeasured switches (`Mechanics`)

| field | default | harness item | what decides it |
|---|---|---|---|
| `disc` | UniformArea | H:W1 | S5 W-A: KS test of the 2,000-shot wall pattern |
| `min_spread_rise` | Immediate | H:W2 | W-A growth / decay runs |
| `recovery` | Linear | H:W3 | W-A recovery fit |
| `first_shot_rearm` | OnTriggerRelease | H:W4 | W-A first-shot re-arm |
| `falloff`, `falloff_basis` | Linear, StraightLine | H:W5 | W-B piecewise fit |
| `apply_protection_threshold`, `bullets_pass_soldiers` | false, false | H:W6 | threshold / penetration runs |
| `blast_falloff`, `blast_occlusion` | Linear, on | W-C | explosives runs (P1) |
| `stance_transition_penalty` | off | (W-A extension) | stance-change bursts |
| `empty_reload_unchambered` | true | H:W9 | reload counts |
| `sprint_blocks_fire`, `spawn_protection_blocks` | true, true | S-A | observation |
| `suppression_threshold` | 0.3 | (M7) | suppression model |

---

## 7. Seams for the later M2a integration

### 7.1 Server (GameServer, after S1 lands)

The tick order of m1.md §8.1 gains these steps, all additive and behind a `GameConfig` weapons table:

* **4a.** After each soldier's motor step, in slot order:
  * `ctx = make_weapon_context(motor, before, after, cmd)`;
  * `step_soldier_weapons(table, weapons[slot], cmd, ctx, match_seed, out)`;
  * every `out.shots[i]` goes to `projectiles.spawn`.
* **4b.** `projectiles.step(tick, server_world, provider, events)`. The provider answers each projectile's
  `TargetQuery`:
  * a bot's projectiles get the present tick's capsules;
  * a human's get the lag-comp ring at that shooter's render time (§5.7).

  The capsules are stand-ins posed at the state's pose and yaw until R14.
* **5.** The rules apply each `DamageEvent` with `apply_damage`. Deaths go to the existing respawn path, and
  `spawn_soldier_weapons` runs on spawn.
* **6.** Lag-compensation history and HitReports.
  * The history is a ring of the present `HitTarget`s per tick: 64 × 16 capsules × 32 B ≈ 32 KiB per tick, and
    250 ms = 15 ticks ≈ 0.5 MiB.
  * A HitReport's (tick_a, tick_b, alpha) is answered by a provider that lerps two ring entries (plan §3.2).
  * A report that arrives after the live flight goes through `refly_projectile` with a Refly query, and is applied
    at most once per (shot, pellet) (§5.7).
  * The server retains each human shooter's ShotEvents for the def's time to live plus 250 ms, at most 5.25 s for
    the M98B; a ShotEvent is about 100 B. A report whose shot is gone, or whose sample is older than 250 ms, is refused.
  * R14's reduced bf3_anim pose replaces the stand-in capsules in the same ring.
* **Command bits.** `command.h` gains the reserved range of bits 6–11 with named constants, which
  `weapon_input.h` then uses. That edit is integration-wave work, because S1 is editing `bf3_sim` in parallel.
* **7.** Snapshots:
  * the owner gets its `SoldierWeaponsState` (full precision, like its `SoldierState`);
  * the others get the active slot's def id, phase and a fire flag;
  * remote clients get `ShotEvent`s (shooter, def, shot index, time, origin, quantised direction: plan §3.2
    "Replicated") as events.

  That is a bf3_net protocol addition for the integration wave; T3's message layout has room.
* `MatchInfo` gains `weapon_hash = weapon_table_hash(table)`, beside `motor_hash`.

### 7.2 Client (ClientSession)

* The prediction ring entry gains `SoldierWeaponsState`.
* `sim::catch_up` steps the weapons with the same commands, after the motor.
* A reconciliation compares `soldier_weapons_hash` as well as the motor state.
* Predicted `ShotEvent`s feed the 1p muzzle flash, tracer and sound at once. The server's ShotEvents for the own
  shots are not replayed.
* **Only a tick's first prediction emits.** `catch_up` and reconciliation replays step the weapons again for ticks
  already predicted. The ShotEvents and timed events those replays produce are discarded; only their state is
  kept. So the 1p effects and sounds never fire twice. The client tracks the highest tick it has emitted for.

### 7.3 Presentation, VFX, audio, HUD (other tracks)

* **Viewmodel:** `weapon_events::` map onto the Ant signals of topics/01 §1.8: Deploy, Reload (and ReloadShotgun
  per shell), Fire / FireSingle, BoltAction / PumpAction, FireModeChanged, NumberOfBulletsLeftInGun from
  `magazine`. The camera adds `recoil` to the view; the CameraRecoilData spring is a presentation-side kick.
* **VFX / audio:**
  * `ShotEvent` gives the muzzle and shell effects (FireEffects1p/3p, a WeaponDef addition, §4.5), the tracer
    (`tracer` flag, from the muzzle converging over VisualConvergeDistance 30 m) and the fire sound
    (`WeaponDef::sound`).
  * `ImpactEvent` gives the impact effect, sound and decal by `physics_material` against the projectile's row.
  * `DetonationEvent` gives the explosion effect.
  * These are plain data; the fx and au tracks read them without linking anything new.
* **HUD:** ammo (`magazine`, `reserve`), the crosshair from `spread`, hit markers from `DamageEvent`s.

---

## 8. Caching

No new Cache kind:

* **WeaponDefs** are already cached per variant (kinds `weapon` / `weapon-deps`, `OBF3WDEF` v1, converter 1,
  README "Preparing the cache"). The §4.5 fields bump both versions to 2: every key changes once and the old
  entries age out.
* **WeaponSimDef / WeaponTable** are derived at level load. The cost is tens of µs per weapon, and a level's kits
  use about 30 variants (172 at most). Not caching them avoids a second format for the same content. The table's
  hash goes to MatchInfo.
* **The grid pair table** (`read_grid_pairs`, 32,400 entries) and `SurfaceMaterialTable` are built from the
  level's EbxDb at load. If the implementation measures more than 20 ms for them on Metro, they get a kind
  `grid-pairs`. Its key would be the grid partition's and `materials/materialcontainer`'s manifest SHA-1s, a
  converter version and the engine id (the `make_cache_key` scheme). Until then they are rebuilt.

---

## 9. Hostile-input bounds

* **`make_weapon_sim_def`** refuses, naming the variant and the field (bounds in `weapon_sim_def.h`; retail maxima
  measured on the 172 MP_Subway variants):
  * any non-finite number;
  * negative times, speeds, damages or radii;
  * RateOfFire / RateOfFireForBurst ≤ 0 or an interval below tick / 8;
  * bullets per burst outside 1..32;
  * pellets outside 1..64;
  * magazine above 10,000 or magazines above 1,000;
  * speeds above 5,000 m/s;
  * |gravity| above 100;
  * damages above 100,000;
  * radii above 100 m;
  * angles above 90°;
  * rates above 10,000 / s;
  * times above 60 s, except HoldAndReleaseData.MaxHoldTime, which saturates at 60 s (the M67's 9999 = no limit;
    as built);
  * more than 4 fire modes or an Unknown logic;
  * ReloadThreshold outside [0, 1];
  * hold-and-release powers outside [0, 10], or min above max; PowerIncreasePerSecond outside [0, 10,000];
  * TraceFrequency above 1,000. A value ≤ 0 is stored as 0, meaning no tracers: the knife's is 0 in data, and the
    tracer rule never divides by it.

  ReloadType Unknown is refused for weapons with a magazine whose ammo is not unlimited (as built: the knife has a
  1-round magazine and unlimited ammo). Reload fields are checked only for the reload type that uses them (the
  870's ReloadTimeBulletsLeft −1). Sway modifiers are signed in data (the XP2 LMGs' suppression
  DecreasePerSecondModifier −10, found by W4), so their bound is |m| ≤ 10,000; the effective cone angles and growth
  are clamped at 0 (as built, §5.6).
* **Per tick and soldier:** at most 8 shots, and every loop is bounded by them. Phase changes per tick are bounded
  by the phases a tick can end (≤ 4: deploy, bolt, reload, shell). At most 16 timed events
  (`kMaxTimedEventsPerTick`); a 17th is a bug that the W8 invariants catch.
* **ProjectileWorld:**
  * at most 8,192 live projectiles; a spawn beyond is refused and counted;
  * per projectile per tick at most 8 re-casts, 4 bounces and 4 sub-steps;
  * one `WorkBudget` per step (2^22 units) charged before each segment, each re-cast and each target capsule
    test. When it runs out, the remaining projectiles keep their state and fly next tick, and the output says so.
    Work per tick is therefore at most linear in the budget, whatever the data or the crowd;
  * the provider's span is read, never stored.
* **`read_grid_pairs`:** more than 2^24 entries is an error, and the EBX reads are charged as in the other gameplay
  readers.
* **Commands:** any 32-bit buttons value is valid; an out-of-range slot means keep.
* All of this is tested by W8 (`sim.weapons.unit`, `sim.weapons.fuzz`) and runs under the `asan` preset.

---

## 10. The shooting range and bf3dump

**Harness** (`shooting_range.h`). The static world is Metro TDM through `load_level_collision` and
`build_static_world`, as `sim.motor` loads it [verified: sim_motor_tests.cpp], or a synthetic world.

* `find_range_lane` picks the longest straight, level line of sight at stand eye height from the TDM spawn points
  (64 headings each).
* The scripted shooter stands at the lane's start: a fixed context, Commands from a `FireScript` (Hold, Tap,
  TapEvery, ReloadAt, Throw).
* Stand-in targets face it at the scenario's distances. Each keeps its own health and records hits per zone, the
  killing shot and the time.
* `exact_aim` zeroes spread and recoil for the direction (their state still runs) so the W3 / W5 rows hit their
  capsule every time.
* **FireScript::Tap is pinned to the containing tick.** The script presses in command T when the ready time lies
  before tick_end(T), and releases in command T + 1. The shot then goes at max(ready, tick_start(T)), which is the
  exact ready time: the 870's shots are 28,800,000 apart, not 29,000,000. A bolt that waits for the release (M98B)
  therefore taps every cycle + 1 tick (§5.3).
* **The synthetic wall world** is `range_wall_triangles` through `build_static_world(triangles)`:
  * a level slab spanning x and z from −100 m to 3,500 m, 150 m below the eye;
  * vertical walls 40 m wide at the gated distances.

  `ProjectileWorld` removes projectiles 200 m outside the world's bounds. That extent keeps every gated flight
  inside: the M98B flies 3,250 m and falls 122.6 m in its 5 s, the M16A3 975 m and 16.9 m.
* **Metro's longest lane** is unknown until the implementation measures it; the TDM spawns span about
  194 × 146 m (the reviewer's figure, not re-measured here), so a 60 m lane is likely.
  * Rows longer than the lane (the 300 m and 1,000 m drop) run on the synthetic wall world, where W2 measures
    anyway.
  * If no Metro lane reaches 60 m, W3's 60 m rows fall back in order: the spawns of the other Metro modes
    (Conquest, Rush), then the synthetic world. The test prints which world it used and why. The gate stays
    measurable either way.
  * The range report prints the lane length, the static hits, the invisible passes (R-W10), the penetrations and
    the Collision-before-Raycast count (R-W2).
* **W11's load** (`run_range_perf`): see the W11 row. Each shooter aims along its own longest clear heading, with an
  `infinite_magazine` table and no targets.

**bf3dump** (implementation wave, `tools/bf3dump/weapon_sim_commands.{h,cpp}`, additive dispatch). Output is BF3
data and stays out of the repo.

| command | output |
|---|---|
| `weapon-sim <partition> [--unlock <u>]... [--level <sb>] [--seed N] [--seconds S]` | the def's sim numbers; fire timing: the first 20 shot times in units and ms, the gaps, shots in S s, burst / bolt / pump pattern; reload timing (tactical, empty, commit, per shell); drop table at 10 / 25 / 50 / 100 / 200 / 300 / 500 / 1000 m up to max range (time of flight, drop, damage); shots to kill per zone at 0 / 10 / 25 / 50 / 75 / 100 m; one-shot ranges; `unmeasured` marks |
| `weapon-sim --all [--level <sb>] [--threads N]` | W4's TSV: one row per infantry variant (119 on MP_Subway) |
| `weapon-sim --check [--level <sb>]` | the W1–W6 and W9 data rows as `PASS` / `FAIL` lines, `WEAPON-SIM CHECK: PASS (n/n)`, exit 0 / 1 (ctest `tools.weapon_sim`) |
| `range <partition> [--level MP_Subway --mode TeamDeathMatch0] [--distances 10,25,50] [--zone head\|chest\|body\|legs] [--script hold\|tap\|throw] [--seconds S] [--seed N] [--exact] [--perf]` | the lane (spawn, heading, length), per target hits per zone, damage, kill shot and time, the shot and impact list hashes, impact materials; `--perf` runs W11's 64-shooter load and prints mean / p50 / p99 / max per tick; `install.unchanged` row (exit 1 on a change) |

---

## 11. Deferred, and to which wave

| item | to |
|---|---|
| GameServer integration (§7.1), ShotEvent / weapon state on the wire, MatchInfo `weapon_hash`, ClientSession prediction of weapons, HitReport validation and the lag-compensation ring | M2a integration wave (after S1) |
| hit capsules from the server pose (bf3_anim reduced skeleton, R14); the lo-LOD AITrajectory set | M2a integration / M2b (with S3's crowd animation) |
| 1p viewmodel signals, ADS / zoom transition timing (aiming controller), CameraRecoilData spring, scope filters, breath control | M2a viewmodel wave |
| muzzle, shell, tracer and impact effects; weapon and impact sounds | M2a VFX (VFX3) and audio (A1 / A2) waves, from the §7.3 events |
| HUD ammo, crosshair, hit markers | M2a HUD wave |
| health regeneration, death, respawn, kill feed | M2b |
| suppression accumulation on victims and its reaction (blur, sway) | M7 (values already carried, §5.10) |
| bipod / supported shooting and light / laser gates, overheat, ammo pickup and auto-replenish, quick throw and quick knife, ForceFireWhenKilledHolding | M7 breadth (the gates and fields exist) |
| missile guidance (locking launchers: Javelin, Stinger, Igla), vehicle detonation, explosion impulse on physics, destruction damage to props | M4 / M5 / M6 |
| a placement id in SurfaceTag to ignore hulls behind shells (§5.8) | integration wave, if R-W2's count says so |

Nothing of the task's list is deferred: the M67, M320 HE and RPG-7 / SMAW are all in this track (W9).

---

## 12. Risks

| # | risk | mitigation |
|---|---|---|
| R-W1 (plan R5) | the [unmeasured] rules are wrong: disc, recovery, first-shot re-arm, falloff basis, thresholds, penetration through soldiers, blast falloff and occlusion, reload commit, the chambered round | every rule is one `Mechanics` field with the plan's default; S5 Session 1 picks them without a code change; reports mark dependent rows |
| R-W2 | Collision hulls in front of Raycast shells catch bullets early (§5.8) | the range reports how often; a placement id in the tag is the fix |
| R-W3 (plan R14) | stand-in capsules are not BF3's pose, so random-shot hit rates on moving soldiers will differ | gates aim at capsule centres; the provider seam takes bf3_anim's capsules unchanged |
| R-W4 (plan R1) | 64 soldiers firing and thousands of bullets in flight cost too much on the server thread | W11 measures it; bullets are one ray per tick; the budget caps a tick; worker-thread queries (M1 B2 note) if needed |
| R-W5 (plan R4) | cross-machine float differences in the replicated path (sin/cos, exp) desync the prediction | integer time and RNG; polynomial trig; exp precomputed in the def and hashed; the hash refuses a mismatch |
| R-W6 | sub-tick rounding: a non-dividing RPM drifts 8 ns per shot from the ideal | the drift is the same on every machine (the integer is in the def); far below any observable |
| R-W7 | the burst, bolt-release and hold-and-release rules are inferred (§5.3) | W1 gates what the data fixes (rates, the bolt cycle, burst counts); the inferred parts are named in reports |
| R-W8 | Metro has no 300 m lane, so the plan's "1.60 m drop at 300 m on Metro" cannot be shot on Metro | W2 measures it on the synthetic wall world through the same code; the range reports Metro's lane length |
| R-W9 | the RPG direct hit (225 × 2.5) contradicts the community's "100" | reported, not gated; H:W7 decides |
| R-W10 | `kSoupInvisible` is a name heuristic ("invisible" / "collisionmesh"), and bullets always pass it: a wall whose only bullet geometry is a collision mesh would be shot through | the range counts the invisible passes on Metro and reports them; a count on a lane wall is investigated before W3 is signed off |
| R-W11 | merge friction with the parallel tracks: s1, s2, s4 and wp all append blocks at the end of `src/bf3_sim/CMakeLists.txt` (and likely `tests/CMakeLists.txt`); `weapon_def.h` / the weapon blob may be wanted by au and fx too | the blocks are additive and headed by track, so conflicts are textual and trivial. s1's `CollisionWorld::query_count` does not touch `ray` / `kProjectile`. §16 Q1 settles WeaponDef ownership before the implementation wave, so there is one blob version bump and no parallel edit of `weapon_def.h` |
| R-W12 | late HitReports: the per-(shot, pellet) "first resolution wins" rule may refuse valid reports when the live flight at render time and the client disagree | the integration wave measures the refusal rate in loopback with latency; the rule lives in the server's report handling, not in these headers |

---

## 13. Files this track creates or modifies

**This wave (in the tree now):**
* new: `docs/design/m2a-weapons.md`; `src/bf3_sim/include/bf3/sim/weapons/{sim_time, weapon_input, weapon_rng,
  mechanics, hit_target, surface_materials, weapon_sim_def, spread_recoil, weapon_sim, damage, ballistics,
  shooting_range}.h`; `src/bf3_core/include/bf3/core/gameplay/grid_pairs.h`; `tests/weapons_contract_tests.cpp`.
* additive blocks headed `# M2a weapons`: `src/bf3_sim/CMakeLists.txt`, `src/bf3_core/CMakeLists.txt`,
  `tests/CMakeLists.txt`; README: a section "M2a weapons core" and one line under the ctest paragraph.

**Implementation wave:**
* new: `src/bf3_sim/src/weapons/{weapon_sim_def, surface_materials, spread_recoil, weapon_sim, hit_target, damage,
  ballistics, shooting_range, weapon_checks, weapon_install}.cpp` and `weapons_internal.h`;
  `src/bf3_sim/include/bf3/sim/weapons/{weapon_checks, weapon_install}.h` (as built);
  `tests/weapons_support.h`, `tests/weapons_install_support.h` (as built); `src/bf3_core/src/gameplay/grid_pairs.cpp`;
  `tools/bf3dump/weapon_sim_commands.{h,cpp}`; `tests/weapons_tests.cpp`, `weapons_install_tests.cpp`,
  `weapons_range_tests.cpp`.
* additive: the CMake blocks above, the bf3dump dispatch and usage text (`# M2a weapons` block),
  `tools/bf3dump/CMakeLists.txt`, README.
* edits in place (this track's M0 files): `weapon_def.h` (§4.5 fields), `weapon_firing.cpp`,
  `weapon_projectile.cpp`, `weapon_blob.{h,cpp}` (v2), `weapon_format.cpp` (new lines at block ends).

No file of another wave track is touched: GameServer, ClientSession, bf3_net, bf3_anim and bf3_godot are not edited
in this wave.

---

## 14. Test plan

Each data test exits 77 (Skipped) without an install and prints `install.unchanged`. The no-data tests also run
under `asan`.

| ctest | data? | content |
|---|---|---|
| `m2a.weapons.contract` (now) | no | every weapons header compiles with the M1 headers; POD traits; the exact §7.1 intervals in units; the commit rounding (`scale_units`); `fire_mode_steps`; 134 shots in 10 s at 800 RPM; pinned SplitMix64 / shot seeds; weapon buttons are levels that survive `repeated_command`; a provider answers per shooter through `TargetQuery`; double projectile state; `infinite_magazine` off by default; `Mechanics` defaults |
| `sim.weapons.unit` | no | synthetic defs with the §7.1 numbers: W1 every mode (auto, single, burst re-press, bolt with / without hold-until-release, pump, hold-and-release, instant hit), sub-tick shot origins, the shot cap; W6 reloads, commit and cancel, shells; spread decay / rise / kick, the disc distributions (moment checks over 100 k draws), recoil kick, caps and both recoveries; W2 on synthetic walls; W7 on two worlds and a mid-burst restore; capsule geometry (inside starts, grazing, ties); damage arithmetic, blasts, `shots_to_kill`, `one_shot_range`; explosive timing (arm, dud, fuse, bounce); W8 bound rows and the random-command invariants |
| `sim.weapons` | install | `WeaponCatalog` on MP_Subway + grid + capsules + soldier: W1, W2, W3 (arithmetic), W4 (the 119-row table), W6, W9 on data defs; `read_grid_pairs` 32,400 entries resolved and index = low 24 bits; `SurfaceMaterialTable` zone properties 65 / 66 / 158 / 91; every Metro soup material value resolves to a pair |
| `sim.weapons.range` | install | Metro static world: the lane found and printed; W3 end to end (5 m, 60 m), W5, W7 (two worlds), W9 blasts on stand-ins; every static impact carries a resolved pair and physics material; the Collision-before-Raycast count (R-W2) printed |
| `sim.weapons.fuzz` | install (synthetic part without) | W8: 2,000 mutated retail blobs through `make_weapon_sim_def` and 600 random ticks each; asan |
| `sim.weapons.perf` | install, RUN_SERIAL | W11 (`run_range_perf`, `infinite_magazine`; exits 77 when `OPENBF3_QUIET_LOCK` names an existing file) |
| `tools.weapon_sim` | install | `bf3dump weapon-sim --check` exits 0 |

Godot checks: none in this wave (no Godot code). The existing suite, including the `godot.*` checks with
`OPENBF3_GODOT`, must keep passing (the run is recorded in the commit and the track report).

---

## 15. Facts checked for this design

| fact | value | how |
|---|---|---|
| variants | 172, all resolve, 0 errors; kinds 147 bullet / 9 grenade / 5 missile / 11 other; W4 set 119 | `bf3dump weapons` (MP_Subway), 2026-10-15; kinds recounted from its TSV in revision 2 |
| TraceFrequency | 1 on every gate weapon, 0 on the knife (hence "≤ 0 = no tracers") | `bf3dump weapon` per variant |
| `kSoupInvisible` source | owner name contains "invisible" or "collisionmesh" | collision_soup.cpp `invisible_name` |
| float vs double flight | float32 per-tick steps: x −0.67 mm at 975 m, −5.86 mm at 3,250 m (y −0.29 mm); double steps 1e-11 m; double closed form 0 | the review's figure, reproduced with a scratch NumPy simulation (60 Hz, v0 650, g 15 / 9.81) in revision 2; §5.8 |
| gate weapons' numbers | the §4.1 table | `bf3dump weapon` per variant |
| bolt / hold / reload flags | M98B hold-until-release, force flags; 870 none; M67 hold data | `bf3dump dump` of the blueprints (FiringFunctionData.FireLogic) |
| missile, grenade, 40 mm records | §4.1 | `bf3dump dump` of the projectile partitions |
| explosion pairs and cells | §4.2: all 1.0 | grid + container dump, cell lookup by property |
| grid sizes, pair index space | 180 / 102² / 32,400; index = low 24 bits on all 32,400 | the same dump |
| capsules | 13, 9 hi-LOD | core.gameplay.weapons (in tree) |
| soldier | §4.4 | `bf3dump soldier Win32/Levels/MP_Subway/MP_Subway` |
| CollisionWorld API | `ray(origin, delta, mask, StaticHit*)`, `StaticHit::tag.material / flags`, `query_mask::kProjectile` | collision_world.h, static_world.h |
| tick convention, Command buttons | kFire / kZoom / kReload reserved; kEdgeMask = kJump; 24 wire bits | command.h, soldier_state.h |
| Metro load path for tests | `load_level_collision` + `build_static_world` + `read_soldier_def(level->db())` | sim_motor_tests.cpp |
| WeaponDef has no explosion pair, no HoldOffReloadUntilFireRelease | fields absent | weapon_def.h |
| contract helpers | 5 cases, 159 assertions pass (revision 1: 4 / 154) | `test_weapons_contract` (dev), 2026-10-15 |

---

## 16. Open questions

For the orchestrator:

1. **WeaponDef ownership (§4.5).** The VFX and audio tracks may need WeaponDef fields too (FireEffects1p/3p, the
   sound patch). The proposal is that this track owns `weapon_def.h` / `weapon_blob` changes in the implementation
   wave and bumps the blob version once for all three tracks' fields.
2. **Recoil as weapon state (§5.6).** Plan §3.2 has the client send the "view after recoil". This design keeps
   the Command's view as the player's own and derives the same recoil offset on both ends. Is that reading
   accepted? It changes nothing on the wire.
3. **The bolt tap interval (§5.3).** W1 gates the M98B's bolt cycle (1.3 s exact, Symthic's 46.2 RPM). Because of
   HoldBoltActionUntilFireRelease, the fastest tap-to-tap interval is 1.3 s + 1 tick at 60 Hz (45.6 RPM). Is that
   reading of §7.1 accepted?
4. **"On Metro" (R-W8).** If Metro has no 300 m lane, the 300 m drop row is measured on the synthetic world
   through the same code. The same holds for W3's 60 m rows, if no Metro lane reaches 60 m (§10). Is that
   acceptable for the M2a exit?
5. **Command bits 6–11 (§5.2).** Until the integration wave reserves them in `command.h`, the other tracks should
   not add buttons in bits 6–11.
6. **Late HitReports (§5.7, R-W12).** The proposal is to re-fly with `refly_projectile`, with damage once per
   (shot, pellet) and the first resolution winning. Is that the server rule the integration should build?

For the implementation (answered by measurement or S5): the [unmeasured] switches (§6.9); Metro's longest lane;
the R-W2 hull count; the grid table's load time (§8); W11's measured cost.

Answered by the implementation wave (2026-10-15): Metro TDM's longest lane is 96.9 m (spawn 87), so W3's 60 m rows
run on Metro and no fallback was needed; R-W2's Collision-before-Raycast count is 0 (M16A3 run) and 2 (870 run);
the install side (catalog, grid pairs, materials, soldier) opens in about 0.2–0.5 s with a warm file cache; W11's
p99 is 0.205 ms (threshold 2.0 ms, unchanged). The [unmeasured] switches stay at the plan's defaults.
