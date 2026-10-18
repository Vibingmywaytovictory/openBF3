# M2a design: infantry gunplay on Metro through the loopback server

Status: design for implementation, revision 3, 2026-10-16 (revisions 1 and 2 reviewed; §19 and §20 list what changed). Scope:
milestone M2a of the plan
([research/PORTING-RESEARCH.md](../../research/PORTING-RESEARCH.md) §6, row "M2a Infantry gunplay", 10.5–14.5
slices). This document joins the wave-1 parts into one playable loop and says how each exit criterion is measured in
the running game. It writes no code: the headers below are sketches, and each wave's first commit (the wave
contract, §15.1) puts them in the tree with a compile test.

**Starting point.** Waves 2 and 3 start from a `main` that has merged the four spikes (S1 bots and nav, S2
vehicles, S3 crowd animation, S4 level logic), the UI groundwork (ui1 SWF and fonts, ui2 HUD model, minimap and
icons), the L1 look and the three wave-1 tracks of M2a (wp weapon simulation, au audio A1/A2, fx VFX1). The facts
tagged [verified] were read on those branches (`m2a/weapons`, `m2a/audio`, `m2a/vfx`, `spike/s1`…`s4`,
`ui/swf-fonts`, `ui/minimap-icons`, `integ/spikes`) or measured with `bf3dump` against the retail install on the date
above.

Tags: **[verified]** = read from code, data or a measurement on the date above. **[plan]** = taken from the plan or
a design document of a merged track, with its section. **[inferred]** = a design choice or estimate made here.

Constraints that hold throughout: the retail install is read as-is (never written, never run or hooked); no BF3
data in git; screenshots and recordings go to `<repo>\b\shots\` (gitignored); caches live under the user cache
root only (§13). Clean room: no third-party BF3 tool code was opened for this design; IW4L crates (the user's own
Apache-2.0 code) were studied for the lag-compensation verdicts and the view-model springs.

---

## 0. What the merged parts give, and what M2a adds

| part | delivered (gate results) | what M2a still has to do |
|---|---|---|
| wp (`bf3_sim` weapons) | `step_soldier_weapons`, `ProjectileWorld` (double, closed form), `HitTargetProvider` with `TargetQuery` (Flight / Blast / Refly), material-grid damage, `standin_target` capsules, `Mechanics` switches. W1–W11 pass headless; W11 64 shooters p99 0.205 ms [verified: m2a-weapons.md §1] | put it on the GameServer tick, on the wire, in client prediction; capsules from a server pose (R14); kits; deaths |
| au (`bf3_core` audio data, `bf3_audio` runtime) | XAS decode, wave bank (Metro TDM 629 resident waves = 265.4 MiB ≤ 384 MiB), patch compiler and interpreter, `MixerCore`, `WeaponSoundDriver`, `Bf3AudioStream`. G0–G8 pass; G5 = plan §7.7 items 1–4 offline [verified: m2a-audio.md §1.1] | the mixer inside the level session; weapon and impact sounds driven by gameplay events; listener on the camera |
| fx (`bf3_fx`, client only) | `EffectLibrary`, `FxWorld` (begin_frame / begin_step / run_items / end_step), `EmitterAtlas`, `ImpactTable::select`. G1–G11 pass; G9 39.4 ns per particle [verified: m2a-vfx1.md §18] | VFX2 (renderer) and VFX3 (triggers, light pool, decals) |
| S1 | Detour nav, `WalkerBrain`, bot fill through `CommandSource` and `add_local_soldier`; `host.scale` G1 p99 1.51 ms (Metro) / 0.87 ms (Caspian) with 64 walkers, gate ≤ 8 ms; bot spawns only in the main nav component [verified: s1.md §19] | stationary target bots and a firefight stub brain (§10); kit by slot |
| S3 | `CrowdEvaluator` (backend B, one RS skeleton per soldier), pose lanes on `core::WorkerGroup`, additive aim (LocalPost, full pitch 78.1° up / 60.7° down), team reload clips; G1a 0.31 ms pass, G1b ≈ 2.35 ms and G2 p99 failed on the noisy machine [verified: s3.md §18] | stance, per-soldier weapon, firing and death inputs; feed from server state |
| ui1 / ui2 | SWF reader, atlases, TTF fonts, `Bf3UiAssets` (`get_font("$HUD_medium")`, `get_region`, `get_hud_placements`); `HudModel` in `bf3_core` (trivially copyable, `HudWeapon` 63 sources, `HudSoldier` 16; `kHudBindings` 235 rows), `ItemIconIndex` 172/172 `_small` icons [verified: ui-swf-fonts.md, ui-minimap-icons.md] | Controls for ammo, crosshair, hit indicator, weapon name and icon, fed from the game |
| M1 | 60 Hz server on `ServerHost`, `ClientSession` with prediction ring and `catch_up`, 30 Hz snapshots, `present_others` (Hermite, 75–150 ms delay), netsim, Metro static world, viewmodel demo (M9 reload, `RenderFov` 55) [verified: m1.md, integ README] | weapons in the ring, view sample in the command, events to the main thread |
| S2, S4 | vehicles; level-logic interpreter (headless only) | nothing in M2a. M2b runs TDM through the S4 interpreter, so M2a hard-wires neither scoring nor respawn: deaths leave the server only through `CombatEventSink` (§4.3), and the respawn decision goes through a `RespawnPolicy` that `GameServer` asks (M2a: M1's fixed `respawn_ticks`). M2b plugs the interpreter into both seams |

---

## 1. Exit criteria and how each one is measured

Plan §6, M2a row: "shooting stationary bot targets on Metro reproduces §7.1 …; 1p reload timing vs `ReloadTime`;
disc/recovery variants chosen from S5 Session 1; … M16A3 audio oracles §7.7 items 1–4; VFX oracles §7.10 items
1–8". The audio decode criteria of that row (frame counts, decode cost) are already met by au's G1–G3 and are not
repeated here; the M2a gate pass reruns them. One number differs from the plan row and is accepted with them: the
plan says "908 Metro waves", au's G1 pins **909** (this tree's `load_level` scope adds one wave; m2a-audio.md §1, its
open question 7). The acceptance list of the milestone carries this note, so the user accepts 909 knowingly. Every criterion below runs **in the running game**: either headless through the real `ServerHost` and
`ClientSession` on Metro (`host.gunplay.*`), or windowed through Godot with the real input mapping
(`godot.m2a`, `godot.m2a_firefight`). Times are in `SimTime` units, 1/(kSimHz·10^6) s, so 75 ms = 4,500,000
[verified: sim_time.h].

**Measurement setup used by several rows.**
* **The range lane.** `find_range_lane` on Metro TDM gives the lane wp used: spawn 87, 96.9 m clear [verified:
  m2a-weapons.md §1 W3]. The check spawns stationary target bots on it with the dev request `SpawnTarget` (§10.1).
* **Exact aim.** A match option, not a runtime toggle: `GunplayConfig::exact_aim` (server flag `--exact-aim`, only
  with `dev_commands`) builds the weapon table with `WeaponTableOptions::exact_aim`, a new table field next to wp's
  `infinite_magazine` (§4.2). Inside `step_soldier_weapons` it does exactly what wp's range post-process does
  (shooting_range.cpp, `RangeScenario::exact_aim`): each `ShotEvent`'s `direction` becomes `aim_direction(cmd.yaw,
  cmd.pitch, {})` and `pellet_cone` 0; `aim`, `origin` and every other field stay as the sim computed them (with
  recoil), and the spread and recoil state still run. So a range run and a table-flag run give equal `ShotEvent`s
  field for field (§4.2). It is
  hashed by `weapon_table_hash`, and the client learns it from MatchInfo's `weapon_flags` (§7.1), so it builds the
  same table and predicts the same directions. It is fixed for the match, so `weapon_hash` never changes mid-match.
  The runs of E3, E4 and E12 start their server with it; every other row runs without it. Aim points come from
  `zone_aim_point` on the server's own `HitTarget` (the dev `TargetInfo` reply).
* **The sky lane.** Metro has no 300 m lane. The dev request `Teleport` puts the shooter 40 m above the top of the
  static world's bounds at the bounds' minimum along the longest horizontal axis, heading along that axis
  (`find_sky_lane`, §10.1). wp's `ProjectileWorld` removes a projectile more than `world_margin` (200 m) outside the
  bounds (ballistics.h), so the lane must hold the whole flight: `find_sky_lane` accepts a lane only when a 1,000 m
  ray is clear **and** the in-bounds length (to the far bound + 200 m) is ≥ 1,050 m, and prints both. Metro's static
  world spans tile keys z −13..19 (64 m tiles, ≥ 1,984 m between the outer tiles' centroids) and x −12..5
  [verified: `bf3dump collision MP_Subway TeamDeathMatch0`, 170 tiles], so a lane along z fits the M16A3's 975 m
  with about 1,200 m to spare. The 975 m flight falls 16.9 m (½·15·1.5²), well inside the 200 m vertical margin.
  The drop is measured against the launch line of the server's `ShotEvent` (origin + direction), so the shooter's own
  fall after the teleport does not matter. The shooter is teleported again before each single shot and fires it in
  the third tick after the teleport (so the motor has settled into `InAir` and the shot origin is ≤ 0.1 m below the
  teleport point); the 40 m fall that follows may kill it with fall damage, which does not touch the flight: a
  projectile keeps flying after its shooter's death (§4.1 step 4a) and the Trace keeps recording it (the traced
  shooter is the slot, not the life). wp's out-of-world removal tests the whole box, every axis including above the
  top, with the 200 m margin [verified: ballistics.cpp `make_env`]; a flight 40 m above the top that falls 16.9 m
  never leaves it. If `find_sky_lane` finds no lane, the expiry row falls back to wp's W2
  synthetic wall world (already passing) and the run reports it; the 100 / 300 m drop rows still need the sky lane.
* **Server trace.** M1's `DebugKind::Trace` (per-tick server states, dev only) gains the tick's `ShotEvent`s,
  projectile positions of the traced shooter, `DamageEvent`s, `DetonationEvent`s and deaths (the wire is nt's, the
  server side sv's, §15). The additions are bounded per tick (≤ 8 shots, ≤ 8 projectile positions, ≤ 8 damage
  events, ≤ 2 detonations, ≤ 4 deaths; the excess is dropped and counted in the report), so a Trace of the full
  `kMaxTraceTicks` 1,200 stays ≤ 1,200 × ≈ 0.4 KiB ≈ 0.5 MiB on ch1 [inferred] next to M1's ≈ 92 KiB; the checks
  trace ≤ 3 s (drop, timing, reload) and print the report size.
* **Noise policy** for every wall-clock row (E13, E14, E9): run under the quiet lock; on a failure repeat once; if it
  still fails, report WARN with the level-alone baseline row next to it (the S3 precedent). The user decides
  whether a WARN blocks the milestone.
* **Test binaries** (one owner each, §15): `host.gunplay.server` (sv: cases timing, drop, stk, reload, variants,
  hostile), `host.gunplay.client` (cl: lagcomp, loopback, two clients), `host.gunplay.replay` and
  `host.gunplay.perf` (bt), `godot.m2a` / `godot.m2a_firefight` (gx). All headless ones run the real `ServerHost` and
  `ClientSession` on Metro.

| # | criterion | automated measurement | threshold | § |
|---|---|---|---|---|
| E1 | fire timing (§7.1: 800 RPM = 75 ms) | `host.gunplay.server` case "timing" (headless) and `godot.m2a` case "range" (`--m2a-check=range`, input through `Bf3InputMapper`): M16A3 (`u_m16a4`) fire held from a full magazine; server trace and the client's predicted shots | 31 shots; first at the press tick's start; all 30 gaps exactly 4,500,000; predicted `ShotEvent`s equal the server's field for field (time, shot index, seed, direction bits) 31/31; one 1p muzzle start per predicted shot (31). Every other kit firearm: gaps equal the table interval as W1 defines it (held automatic: every gap equal; single fire, tapped every 2 ticks so each tap is a new press: every gap ≥ the interval and the smallest within the 2-tick tap period). Semi-automatic taps (MK11, M9) one display frame long: one shot per tap (the fire press counter, §5.1) | §5, §8 |
| E2 | ballistic drop (§7.1: 1.60 m at 300 m) | `host.gunplay.server` case "drop": sky lane (§1 setup; in-bounds length printed), single shots; drop = distance of the projectile's traced position from the launch line where its horizontal distance crosses 100 / 300 m (linear between tick positions) | 0.1779 ± 0.01 m at 100 m; 1.5981 ± 0.01 m at 300 m; expiry (`ImpactKind::Expired`, not an out-of-world removal) at 975.0 ± 0.01 m, or wp's W2 synthetic-world row when no sky lane holds the flight (reported); the client's cosmetic flight of the same shot within 1 mm of the server's at 300 m | §5.3 |
| E3 | shots to kill (§7.1 table) | `host.gunplay.server` case "stk" and `godot.m2a` case "range": range lane, server started with `exact_aim`, target bots standing at 5 m and 60 m, single fire at the zone's aim point until the server's death event | M16A3: chest 4 / head 2 at 5 m, chest 6 / head 3 at 60 m (exact); damage per hit at 5 m 25.000 / 50.000 / 22.750 (chest / head / legs) ± 1e-3; one death event after the last hit; HUD hit indicator shown once per server hit | §5, §11.4 |
| E4 | headshot ×2.0 / ×2.35 | same harness: M16A3 and MK11 (`u_mk11`, multipliers head 2.35 / chest 1.25 / legs 0.91 [verified: `bf3dump weapons`]) | M16A3 head 50.000 (×2.0); MK11 head 117.500 at 5 m (×2.35, kill in 1); MK11 head at 60 m kills in 2 (one-shot range 50.74 m, W3); ± 1e-3 | §5 |
| E5 | 1p reload timing vs `ReloadTime` | `host.gunplay.server` case "reload" (server and predicted phase events) and `godot.m2a` case "reload" (viewmodel and HUD, per display frame) | server = predicted: M16A3 tactical 108,000,000 (1.8 s), commit at +91,800,000 → 31; empty 142,200,000 (2.37 s) → 30; M9 81,000,000 / 96,000,000; viewmodel reload clip starts within 1 display frame of the predicted `kReloadStarted`; the 1p weapon can fire again, and the HUD ammo changes, within 1 display frame of the predicted finish and commit (late; early by at most 1 tick, since a presented weapon's discrete fields come from the newer predicted tick); the authored clip length per kit weapon is printed next to its `ReloadTime` | §7.3, §11 |
| E6 | disc / recovery variants (S5 Session 1 not measured yet) | `host.gunplay.server` case "variants": four runs, `weapon_mechanics.cfg` set to each of disc {UniformArea, UniformRadius} × recovery {Linear, Exponential}; plus one run with the client's cfg differing from the server's. Both ends share one `SharedLevel` in the headless process, so the cfg is not part of it: `GunplayConfig::mechanics_cfg` (server) and the new `ClientSessionConfig::mechanics_cfg` (client, default the same path; cl) are each parsed by their own end when it builds its table, and the mismatch run sets the client's to a second file | four distinct `weapon_hash` values; the mismatched client disconnects on receiving MatchInfo with reason "weapon table mismatch" and `weapon_hash_mismatch` = 1 (it never sends Ready and never predicts); 0 weapon reconciliations over 600 ticks of mixed fire per matched run; 2,000 server shot offsets per run fit the variant's radial CDF (KS D ≤ 1.63/√2000 = 0.0364); the recoil offset after a 10-round burst follows the variant's closed form within 1e-4°. M2a ships UniformArea + Linear with `measured = false` (§5.5) | §5.5, §7.1 |
| E7 | audio §7.7 items 1–4 through the game mixer | `godot.m2a` case "audio": the game's own mixer (`Bf3GameAudio` in the level session, 48 kHz) records to WAV (`--audio-record`); a server bot fires M16A3 bursts while the free camera (the listener) holds at 5, 20, 100, 150, 280, 320 m; item 4 patches (M98B, RPG-7, M1A2) through the client-local audition (`Bf3GameAudio::audition(patch, position)`, driven by `m2a_check`; no server request) at their band distances; the recording is judged by au's `analyse_check` | item 1: every kit item's patch compiles strictly (12/12) and binds: for the 10 firearms `bind_weapon_sound` succeeds (start / stop / step inputs resolved); for the knife and the M67 the swing / throw / explosion one-shot patches start as `PatchInstance`s without a driver (the report lists which form each item uses). This narrows plan §7.7 item 1 to the kit items in the game; the census itself (all patches compile, node counts: 834 patches + 1 master, 32,117 node instances; the research's 833 / 32,113 came from `sound/` only) stays covered offline by au's G4 (`audio.census`) and G5, which the M2a gate pass reruns; items 2–4: au's §10.2 rows PASS on the in-game recording (M16A3 close layer cosine 1 → 0 over 0–20 m, far layer per au reading R; pitch within U[1.21, 1.262] × LFO[0.96, 1.0]; distant HPF 17..200 m → 0..310 Hz, 140.6 ± 1 Hz at 100 m; M98B, RPG-7, M1A2 bands); `short_blocks` = `late_blocks` = `late_commands` = 0. Audible correctness: the user (§17) | §12.3 |
| E8 | VFX §7.10 items 1–6 in the game, plus VFX3's cues | `godot.m2a` case "fx" plus the existing `fx.install` / `fx.unit` | (1) every effect the kits, the impact table and Metro reference compiles, 0 failures (and `fx.install` census passes); (2) `fx.unit` polynomial rows pass; (3) over 1,000 predicted M16A3 shots the `_1p_flash` emitter is present in [0.71, 0.79] of them (p 0.75, 3σ) and the renderer's drawn instance count equals `FxWorld::particles()` every frame; (4) the oilfire emitter started on Metro by the client-local `Bf3GameFx::start_effect` (driven by `m2a_check`; no server request) draws 80 ± 1 live particles after 15 s; (5) in-game 5.56 hits on Metro concrete, brick and a soldier start exactly the effects and decals of fx's G5 named cells; water: `m2a_check` first searches Metro's collision tags for a triangle of physics material Water (18) reachable from a spawn; if there is none, the water cell is checked through the same trigger code headless on Caspian (`fx.install` case "cells"; Caspian's grid has the same Water cell, fx G5) and the report says "Metro: no water"; (6) the atlas uploads 52/52 entries, 0 default-texture fallbacks; (7) cues: tracer starts = predicted shots with `tracer` set (from the table's `trace_frequency`), one shell eject per firearm shot, one impact one-shot sound per static `Impact` event within its cull distance, 0 double starts after replays | §12.1, §12.2 |
| E9 | VFX §7.10 items 7–8: firefight budget | `godot.m2a_firefight` (windowed, 2560×1440, vsync off, Metro TDM, 64 firefight bots in pairs in front of the observer, §10.2, 60 s after 10 s warm-up); `godot.fx_ref` (item 8, the Godot reference workloads re-run on this machine) | FX main-thread time (FxWorld begin/end + RenderPrep + buffer upload + light pool) p99 ≤ 1.0 ms; FX GPU time ≤ 2.0 ms (p50 of FX-on minus p50 of FX-off frames in alternating 5 s windows, the S3 G1b method); effect lights ≤ 64 in every frame; live particles reported (≤ 16k expected); item 8 reported, and VFX2's 16k-particle GPU cost ≤ 1.25 × the reference MultiMesh 16k lit + soft row on the same machine. **Deviation from the plan:** §7.10 item 7 says "16k particles ≤ 0.3 ms GPU"; the reference row itself measured 1.05 ms on this machine, so no MultiMesh path can meet an absolute 0.3 ms; the relative row replaces it (§16 Q17) | §12, §14 |
| E10 | minimal HUD (route A+) | `godot.m2a` case "hud" (per frame, plus shots to `b\shots\m2a\hud_*.png`) and `ui.hud.feed.unit` (Godot-free `fill_hud`) | ammo widget equals the predicted magazine / reserve every frame (0 mismatches), drawn in `$HUD_medium` at `hudmpscreen` Ammo_01 (1194, 669) scaled by min(w/1280, h/720); the crosshair gap equals (h/2)·tan(spread)/tan(vfov/2) ± 0.5 px every frame and hides in ADS; one hit indicator per server-confirmed hit, shown within 1 frame of the Hit event; the weapon name equals the localised string for all 12 kit items, with each team's kits spawned (`KitSelect` team, §5.6) (M16A3, M4A1, M27 IAR, MK11 MOD 0, AK-74M, AKS-74u, RPK-74M, SVD, M9, MP443, KNIFE [verified: `bf3dump text`], M67 GRENADE through its UI description [verified string 0x26436abd; link inferred]); the `_small` icon resolves for 12/12 | §11.4 |
| E11 | 4 kits × (primary + pistol + knife + M67) | `godot.m2a` case "kits": for each of the 8 loadouts (2 teams × 4 kits, §5.6) send `KitSelect {kit, team}` and respawn, cycle slots 0 → 1 → 7 → 6 → 0, fire each item once (the knife at a target 1.5 m away, the M67 thrown), reload each firearm | 8/8 loadouts spawn on the requested team with the kit's items in slots 0, 1, 6, 7 (the `Spawned` kit and team equal the request); every item deploys, fires and (firearms) reloads, with server events = predicted events; the viewmodel has a deploy, fire and reload clip or a counted stand-in for every item (report lists stand-ins); the knife hits; the M67's `DetonationEvent` time minus its `ShotEvent::spawn_time` (release + `SpawnDelay`) = 3.6 s exactly (wp's fuse is `spawn_time + time_to_live`), and the release-to-detonation time is printed | §5.6, §11.1 |
| E12 | server-validated hits with lag compensation | `host.gunplay.client` case "lagcomp": netsim "150/2", a target bot strafing ± 4 m/s over 8 m at 20 m, the test client aiming at the presented chest each tick (server started with `exact_aim`), 500 single shots; control run with `max_rewind_ticks` 0; stress run "lat=150-165,loss=2"; plus `sim.lagcomp.unit` (synthetic) | server hit for ≥ 99 % of the shots the client's own cosmetic test hit (both ends use the time rule of §6.4), same zone for ≥ 99 % of those; control run hit rate ≤ 50 % [inferred threshold: without rewind a 4 m/s strafe moves ≈ 1 m in 270 ms, wider than the chest; §16 Q14]; **sample equality**, two rows: (a) `sim.lagcomp.unit` (synthetic, two clients with different seqs, different Snapshot / `SnapshotOthers` splits, lost and reordered datagrams of both kinds, a respawn): the server's rebuild from `LagHistory` and the client's rebuild from `ReceivedRecords` give the same list and the same `OtherPose` bit for bit for every shot, with the pose cache on and off; (b) in the end-to-end run, for every shot whose target was sampled, the server's `OtherPose` (Trace) equals the client's cosmetic-test `OtherPose` bit for bit, except shots counted as `late_sample` (§6.3); the drawn-frame residuals (`generation_reorder`, `others_ahead`, `track_timeout`, `window_extrapolated`, §6.1, §6.3) reported; stress row and clamp counts reported | §6 |
| E13 | server tick budget with 64 firing bots | `host.gunplay.perf` (RUN_SERIAL; headless, Metro TDM, 64 firefight bots, 10 s warm-up, 120 s); `bf3dump lagcomp MP_Subway TeamDeathMatch0 --perf` (64 human shooters at the maximum lag: row "pairs" at 20–40 m, row "long" with every bullet flying its full 1.5 s above the world) | server tick p99 ≤ 8.0 ms (S1's gate); weapons + projectiles + rewind share printed; rewind provider p99 ≤ 0.5 ms per tick in both bench rows; queries, candidates and pose-cache hits per tick printed (§6.7) | §6.7, §14.1 |
| E14 | client frame budget in the firefight | `godot.m2a_firefight` (the E9 run) | frame p99 ≤ 16.7 ms at 2560×1440, vsync off; M2a main-thread additions (viewmodel, HUD, triggers, audio router, FX) p99 ≤ 3.0 ms, crowd step p99 ≤ 2.0 ms (S3 G1a); 0 audio underruns; 0 presentation-event overflows | §14.2 |
| E15 | determinism | `host.gunplay.replay` (two headless 3,600-tick firefights); `host.gunplay.client` cases "loopback" (10 min scripted play, no netsim) and "two clients" (the second `ClientSession` connects 30 s after the first, when every bot slot has fired) | equal per-tick world hashes (soldier states, weapon states, projectiles, combat events) for equal seeds, different for a different seed; 0 weapon and 0 motor reconciliations in loopback without netsim (health rebases, §8.2, counted separately and reported); both clients derive the same effect and sound seeds (§4.4) for every remote shot from the second client's join on (wave 2, cl: `bf3_host` does not link `bf3_fx`, so the row compares the seeds). After fx3 lands, `fx.triggers.unit` case "late join" feeds the two clients' recorded `PresentEvent` streams through two trigger instances and checks the same (effect, variant) choices per remote shot (wave 3) | §13.3 |
| E16 | load time and memory with weapons | `godot.m1_load` run on the M2a scene (`res://scenes/m2a_metro.tscn`; gx) | M1's E4 / E5 still hold: Metro cold ≤ 15 s, warm ≤ 5 s, with the weapon table, wave bank, effect library and atlas, kit viewmodels, 3p weapons and the capsule pose table loaded; peak working set and VRAM printed | §13.2 |
| E17 | hostile input; install unchanged | `net.proto.fuzz` (nt; dev and asan) over every new message; `host.gunplay.server` case "hostile" (sv); every install test's `install.unchanged` row | every malformed message refused and counted, no crash; a client sending wanted slot 15, `view_back` 65,535, a `view_seq` outside the history, fire-mode counter spam, `KitSelect` team 7 and 4× command rate changes no other soldier, clamps are counted, server tick unaffected; layout.toc, cas.cat and every .sb / .toc unchanged | §13.4 |
| E18 | hit capsules from the server pose (R14) | (a) wave 2, lc: `anim.server_pose` (install) and `bf3dump lagcomp --deviation`: `posed_target` from the table against a **reference evaluation** written in the test, not through the table builder: per query `sample_pose` of the stance idle (or the transition clip at its exact time), `apply_additive` of the aim clips at the exact pitch weight, `model_pose`, and the capsules placed from the bones' model transforms (bf3_anim's layer functions; no pitch buckets, no fraction steps, no LOD, no RS skeleton), at **off-grid** queries: the 32 mid-pitches between buckets and the 15 mid-fractions between transition steps; plus the stand rows against S3's `CrowdEvaluator` (backend B, an independent code path that exists in wave 2). (b) wave 3, tp: `anim.s3.unit` case "capsules" reruns the crouch and prone rows against `CrowdEvaluator` once tp has added the stance inputs | head and chest capsule centres within 3 cm of the reference in the steady rows (stand, crouch, prone at the 32 mid-pitches) and within 5 cm in the transition rows (6 transitions × 15 mid-fractions × 32 mid-pitches) [both inferred thresholds, §16 Q14]; the stand rows within 3 cm of `CrowdEvaluator` (a), the crouch and prone rows within 3 cm of it (b); legs and run (locomotion is not modelled) reported only; every row tagged clip-derived or stand-in (§6.5), 0 stand-in rows expected on the retail install; the table hash is part of `weapon_hash` | §6.5 |
| E19 | ADS, viewmodel FOV and springs | `anim.viewmodel.unit` (vm; Godot-free) and `godot.m2a` case "ads" (per display frame) | world vertical FOV 55 at hip and 40 at iron sights, reached the aiming controller's FOV time for that direction (its per-transition time where it lists one: iron sights 0 → 1 0.12 s and 1 → 0 0.15 s; else `fovTransitionTime` 0.15 s) ± 1 display frame after the presented ADS level changes, and back after the release (the press reaches the presented weapon through the predicted tick and the presentation delay: that press-to-presented latency is printed and gated ≤ 0.1 s; corrected at gx's verify fix, a frame-exact gate cannot include it); viewmodel FOV = `RenderFov` 55 at hip; the camera's recoil offset equals the interpolated predicted recoil every frame (± 1e-4°); sway, bob and kick springs come to rest within their configured settle time once the input stops; zoom refused while deploying or sprinting (no FOV change; the presented ADS level follows the predicted active slot's `kZoomWasDown`, which `step_weapons_m2a` clears in those ticks, §5.2) | §11.2 |

---

## 2. Module ownership

Path convention (as in the tree [verified: integ `src/*/include/bf3/*`]): a header written `bf3/<module>/x.h` in
this document lives at `src/bf3_<module>/include/bf3/<module>/x.h` and its source at `src/bf3_<module>/src/x.cpp`
(for example `bf3/host/lag_comp.h` → `src/bf3_host/include/bf3/host/lag_comp.h`); W2.0 and W3.0 create them there.

| library | M2a additions | rule |
|---|---|---|
| `bf3_core` | `gameplay/capsule_pose_table.h` (POD table of local capsule segments per stance, pitch and transition; hash) | Godot-free; no runtime state |
| `bf3_anim` | `server_pose.h`: builds the capsule pose table from the 3p rig, its stance clips and the additive aim (R14); `viewmodel_anim.h`: the 1p state machine, layers and springs; additive inputs on `crowd_eval.h` | Godot-free; the server links it (new `bf3_host → bf3_anim` link) |
| `bf3_sim` | `command.h` view-sample fields (the weapon button fields stay wp's `weapon_buttons::`); `weapons/posed_target.h` (capsule table + pose → `HitTarget`); `weapons/mechanics_tuning.h` (cfg parser); `weapons/weapon_rules.h` (`step_weapons_m2a`, the two integration rules of §5.2, sv); two additive edits to wp files: `WeaponTableOptions` / `WeaponTable::exact_aim` (hashed, applied in `step_soldier_weapons`; sv) and `TargetQuery`'s segment fields (filled by `ProjectileWorld`; lc); `prediction.h` steps weapons in `catch_up` through `step_weapons_m2a` | deterministic, no I/O except the cfg parser |
| `bf3_net` | protocol v2 messages (§7); `weapons_wire.h` (own-weapons wire view); `other_sample.h` (the Hermite sampler of one other soldier and the track rebuild, shared by client and server) | the sampler is the one function both ends use for lag compensation |
| `bf3_host` | public headers in `src/bf3_host/include/bf3/host/`, sources in `src/bf3_host/src/`: `gunplay.h` (per-slot weapons, projectiles, damage, deaths, kits inside `GameServer`); `lag_comp.h` (history ring and `RewindProvider`); `kits.h`; `target_bots.h`, `firefight_brain.h`; `server_view.h` (read-only server world for brains); client side: `client_projectiles.h`, `presentation_events.h`, `presentation_weapon.h`, `hud_feed.h` | server parts are linked by the dedicated server too; nothing here links `bf3_fx` |
| `bf3_audio` | `weapon_audio.h` (router: soldiers → `WeaponSoundDriver`s, sim time → mixer frames, voice limits), `impact_audio.h` | client only |
| `bf3_fx` | `render_prep.h` (sort, batch, light pool merge: Godot-free), `triggers.h` (shot / impact / hit / detonation → `FxWorld::start`) | client only; never linked by a server module (VFX4 rule, checked by a CMake test) |
| `bf3_render` | `viewmodel_set.h` (a kit's 1p meshes, arms, materials and clips), 3p weapon meshes for the crowd | loader threads only |
| `bf3_godot` | `Bf3Viewmodel`, `Bf3Hud` (+ widgets), `Bf3GameAudio`, `Bf3FxRenderer`, `Bf3GameFx`, `Bf3GameCrowd`; input mapping; `m2a_check`; scenes | only Godot glue; logic stays in the libraries above so headless tests reach it |

---

## 3. Threads

| thread | M2a work | rules |
|---|---|---|
| Godot main | input → `InputSample` (now with fire, ADS, reload, slot, fire-mode presses and the frame's render time); reads the newest `PresentationFrame` and drains the `PresentationEvents` ring; `present_others`; viewmodel pose; HUD; `Bf3GameCrowd::step` (S3); FX `begin_frame` / `begin_step` / `end_step`, `RenderPrep`, buffer upload, light pool; triggers; audio router posting `MixerCommand`s | budget §14.2; never runs prediction replays (M1 §3) |
| Godot `WorkerThreadPool` (max 4, M1 §3) | FX `run_items` (≤ 4 tasks per sub-step, disjoint item ranges) | the main thread waits for them inside the FX budget |
| client (`ClientSession` thread) | prediction with weapons; reconciliation; the cosmetic `ProjectileWorld` for own and remote shots; decoding shot and combat events; publishing frames and events | no Godot; per tick ≤ 1 ms [inferred] |
| server (`ServerHost` thread) | the GameServer tick with the steps of §4.1; bot brains (serial, as S1) | no Godot; p99 ≤ 8 ms (E13) |
| crowd pose lanes (S3 `core::WorkerGroup`) | 3p poses | **lane count changes at M2a**: clamp(hc − 8, 2, 4) instead of clamp(hc − 4, 2, 6) when a listen server runs in the process (§16 Q11) |
| Godot audio thread | `Bf3AudioPlayback` → `MixerCore` renders 512-frame blocks (au) | lock-free command ring from main; 0 underruns (E14) |
| loader (`Bf3LevelSession`'s thread) and lanes | M1 load phases plus: weapon table, kit viewmodels, 3p weapons, capsule pose table, effect library preload and atlas, wave bank and programs | read-only on the install and the cache |

**Thread count in game on 16 hardware threads** [inferred]: main 1, Godot pool 4 (`worker_pool/max_threads=4`
[verified: project.godot]), client 1, server 1, crowd lanes 4, audio 1 = 12, which leaves room for Godot's render
and driver threads. The previous S3 rule (6 lanes) plus the server would have oversubscribed the machine during a
firefight. Going from 6 to 4 lanes must not cost S3's accepted quiet-machine numbers (G1a p99 0.38 ms, G1b 1.82 ms,
frame p99 10.4 ms [verified: integ README]): gx reruns S3's `godot.s3_crowd` gate with 4 lanes and a listen server on the
quiet machine before the lane rule lands, and if G1a's p99 rises above 2.0 ms the rule falls back to 5 lanes (§16
Q11).

---

## 4. Server integration

### 4.1 Tick order (extends m1.md §8.1)

1. ENet service → commands (with the view sample, §6.2) into the jitter buffers, and each command packet's snapshot
   acks (`newest_snapshot`, `snapshot_ack_bits`, the new `others_ack_bits`) into `history.ack(client slot, …)`, which
   marks the parts that client received in its own seq space (§6.3); `KitSelect` (kit and team) stored for the next
   spawn; dev requests queued.
2. Commands for this tick per slot (M1 §7.4).
3. Command sources write their command: target bots (§10.1), firefight brains (§10.2, reading `ServerView`), walkers.
4. Per live soldier, in slot order:
   * motor step (M1);
   * **4a** `ctx = make_weapon_context(motor, before, after, cmd)`, `step_weapons_m2a(table, weapons[slot], cmd,
     ctx, match_seed, life.spawn_tick, out)` (§5.2: wp's `step_soldier_weapons` behind the two integration rules);
     each `out.shots[i]` → `projectiles.spawn`, and the command's view sample (lag and the received-set snapshot,
     §6.3) is stored in the shooter's shot-view ring under the shot index, so every shot of one command shares it;
     each shot is appended to every client's shot accumulator. A soldier dead before this tick steps no weapon; its
     projectiles already in flight keep flying. This call goes **inside** M1's per-soldier loop, right after the
     motor step, because it needs that soldier's before / after states; it is a named insertion (sv, §15), a single
     call `gunplay->step_soldier(slot, cmd, before, after)` and no other line of the loop changes.
5. **4b** `rewind.begin_tick(tick, present states)`, then `projectiles.step(tick, server_world, rewind, events)`
   once for all projectiles (wp §7.1).
6. **Rules**: `DamageEvent`s sorted by (time, shooter slot, shot index, pellet) and applied with `apply_damage`; a
   hit on a soldier already dead in this order is dropped; Hit, Death and Detonation events go to the
   `CombatEventSink` (§4.3); a death asks the `RespawnPolicy` when the slot respawns (M2a: M1's `respawn_ticks`); a
   (re)spawn calls `spawn_soldier_weapons` with the slot's kit and team and stamps the life; dev requests
   (`Teleport`, `SpawnAt`, `SpawnTarget`, `TargetInfo`) are applied here and stamped N. Effects and sounds are never
   server requests: the check's effect start and audio audition are client-local (§15, gx).
7. **Lag history, shared part**: at snapshot ticks, `history.record_tick(tick, records)` stores the quantised
   `OtherSoldier` of every live soldier once. `quantize_other(slot, state)` does not depend on the receiver
   [verified: protocol.cpp, game_server.cpp `send_snapshots`], and every client's snapshot of this tick is built in
   the same `send_snapshots(tick)` call, so these are bit for bit the records every client gets.
8. Snapshots (§7.3), the shot datagram, the reliable combat events; ServerHost flushes ENet (M1 §7.5). **Lag history,
   per-client part**: M1 numbers snapshots per client (`snap.seq = s.snapshot_seq++`, one counter per slot) and
   orders the others nearest-first from each receiver, so the seq of a tick and the split between Snapshot and
   `SnapshotOthers` differ per client. For each client, after encoding, `history.sent(client slot, seq, tick,
   main_mask, others_mask)` records which slots that client's Snapshot and `SnapshotOthers` of this seq carried
   (two 64-bit masks; `others_mask` 0 when no `SnapshotOthers` was sent).

Commands for a dead soldier are not applied (M1). A fire command for a tick after the shooter's death + 1 tick is
therefore never stepped, which is plan D7's discard rule.

### 4.2 `gunplay.h` (sketch)

```cpp
namespace bf3::host {
struct GunplayConfig {
	std::string mechanics_cfg = "config/weapon_mechanics.cfg"; // §5.5, parsed on both ends
	uint64_t match_seed = 0;          // 0: derived from GameServerConfig::seed
	LagCompConfig lag;                // §6
	// Match options, fixed at match start (dev_commands only), sent in MatchInfo::weapon_flags (§7.1) so the
	// client builds the identical table; both are WeaponTableOptions fields and therefore in weapon_hash.
	bool exact_aim = false;           // ShotEvent direction = view without recoil, pellet_cone 0, as the range (E3, E4, E12)
	bool infinite_magazine = false;   // wp's oracle option
};
struct GunplayStats { double weapons_ms, projectiles_ms, rewind_ms; uint64_t shots, hits, deaths, damage_dropped; };
class CombatEventSink { public: virtual ~CombatEventSink() = default;
	virtual void hit(const HitEvent &) = 0; virtual void death(const DeathEvent &) = 0; virtual void detonation(const DetonationWire &) = 0; };
class RespawnPolicy { public: virtual ~RespawnPolicy() = default;            // M2a: fixed respawn_ticks; M2b: S4 interpreter
	virtual std::optional<uint32_t> respawn_tick(uint8_t slot, uint32_t death_tick) = 0; };
class Gunplay {                     // owned by GameServer, server thread only
public:
	static core::Result<std::unique_ptr<Gunplay>> create(const SharedLevel &, const GunplayConfig &);
	void on_spawn(uint8_t slot, uint8_t kit, uint8_t team, uint32_t tick);
	void step_soldier(uint8_t slot, const sim::Command &, const sim::SoldierState &before, const sim::SoldierState &after);
	void step_projectiles(uint32_t tick, std::span<const sim::SoldierState> states);
	void apply_rules(uint32_t tick, std::span<sim::SoldierState> states, CombatEventSink &);
	const sim::weapons::SoldierWeaponsState &weapons(uint8_t slot) const;
	uint64_t weapon_hash() const;     // weapon_table_hash(table) ^ capsule table hash, into MatchInfo
	const GunplayStats &stats() const;
};
}
```

`GameServerConfig` gains `GunplayConfig gunplay` (additive). `GameServer` calls the four entry points at steps 4a,
5, 6 and spawn. `SharedLevel` (level_load.h) gains the level's weapon defs, kit list and capsule pose table
(additive fields, sv); both ends build their `WeaponTable` from them with `make_weapon_table(defs, materials,
{mechanics from the cfg, exact_aim, infinite_magazine})`, the client only after MatchInfo has given the flags
(the build is a few milliseconds for 12 defs [inferred]).

**The `exact_aim` edit to wp's files** (sv owns it; W2.0 declares it): `WeaponTableOptions` and `WeaponTable` gain
`bool exact_aim = false` after `infinite_magazine`; `weapon_table_hash` hashes it after `infinite_magazine`;
`step_soldier_weapons` (weapon_sim.cpp), where it fills a `ShotEvent`, sets `direction =
aim_direction(cmd.yaw, cmd.pitch, RecoilOffset{})` and `pellet_cone = 0` when the table has it, and nothing else:
`aim` (which the tracer converges on) and `origin` keep the recoil-affected values, exactly as wp's range
post-process leaves them (shooting_range.cpp overrides only `direction` and `pellet_cone` [verified]). The spread and
recoil state still run. `m2a.contract` checks that the hash changes with the flag and that a W3 range run with the
table flag (and `RangeScenario::exact_aim` off) gives `ShotEvent`s equal **field for field** to the same run with the
post-process (table flag off), and so the same projectiles and hits. The default (flag off) path is untouched, so
W1–W11 stay bit-equal.

**Server view for brains.** S1's `CommandSource::command(tick, own state)` sees only its own soldier. sv adds
`server_view.h`: `class ServerView { std::span<const sim::SoldierState> states() const; uint8_t team(uint8_t) const;
bool alive(uint8_t) const; const sim::weapons::SoldierWeaponsState &weapons(uint8_t) const; const
sim::CollisionWorld &world() const; }`, valid during step 3; `GameServer::view()` returns it and brains keep a
pointer from their construction. The `CommandSource` interface is unchanged.

### 4.3 Damage, health and death

* Health lives in the `SoldierState` (M1's fall damage already uses it). `apply_damage` (wp) subtracts and clamps.
  Spawn protection (`spawn_protection_blocks`, `ImmortalTimeAfterSpawn` from data) refuses damage until it ends or
  the soldier fires; the dev `SpawnTarget` clears it, as M1's `Teleport` does.
* Deaths: the motor state goes to its dead mode (M1), the weapons state is cleared, and the `RespawnPolicy` names the
  respawn tick (M2a: M1's fixed `respawn_ticks`; the death path itself knows no rule of the mode). Health
  regeneration, kill feed, scoring and revive are M2b [plan].
* Events produced per tick: `HitEvent {victim, shooter, shot_index, pellet, zone, damage, health_after, position,
  normal}`, `DeathEvent {victim, killer, def, zone, shot_index, headshot}`, `DetonationWire {shooter, shot_index,
  def, time, position, normal, dud}` (from wp's `DetonationEvent`, so every client places a grenade's explosion
  where the server's flew, whatever its own cosmetic flight did). They go reliably to every client (§7.4) through
  `CombatEventSink`; no scoring or kill feed reads them in M2a.

### 4.4 Deterministic seeds

* `match_seed` (u64) = splitmix64(GameServerConfig::seed ^ level hash), sent in MatchInfo.
* Shots: `shot_seed(match_seed, slot, shot_index)` (wp §5.4). Pellets follow from it.
* Bots: brain seeds splitmix64(match_seed ^ (slot << 8 | life)).
* Cosmetics (client): effects `splitmix64(match_seed ^ (slot << 32 | shot_index))`; impacts mix in the pellet and
  the impact ordinal; sounds `splitmix64(entity id << 32 | burst seq)` (au §7.2). All inputs are replicated, so
  every client picks the same variants (E15).

---

## 5. Weapons in the game

### 5.1 Inputs

`Command` already reserves `kFire` (bit 2), `kZoom` (3), `kReload` (4) and `kUse` (5); wp uses bits 6–7 for a
fire-mode counter and bits 8–11 for the wanted slot, as levels, never edges [verified: command.h,
weapon_input.h]. M2a uses wp's names (`weapon_buttons::kFireModeShift / kFireModeMask / kSlotShift / kSlotMask /
kAll`, weapon_input.h) everywhere and adds no second set; wp's own static_asserts already keep them inside the 24
wire bits and off the motor's buttons. `m2a.contract` adds one more: no M2a bit outside `weapon_buttons::kAll` and
the motor's buttons is set by `with_weapon_input`.

**Taps.** `InputSample::held_buttons` is a level and `set_input` is newest-wins, so a fire press released inside one
client tick would be lost (semi-automatic weapons, the knife). `InputSample` gains `fire_presses`, a monotonically
increasing counter like M1's `jump_presses`: when the client thread samples a command and the counter moved since
the previous command, it sets `kFire` in that command even though the level is already up again; the next command
then carries the release, which wp's single-fire logic reads as the trigger transition. One tap = one shot (E1).

Key mapping (`Bf3InputMapper`, additive): fire = left mouse, ADS = right mouse (hold), reload = R, slots: 1
primary, 2 sidearm, G grenade (slot 6), F knife (slot 7), mouse wheel cycles the filled slots, B fire mode, K
cycles the kit and T the team for the next spawn (both send `KitSelect`); `--kit=` and `--team=` set them at start.

### 5.2 Fire, ADS, reload, switch

All of it is wp's `step_soldier_weapons`: fire modes, sub-tick shot times, burst counts, bolt, reload with the
commit at `ReloadThreshold`, deploy (0.67 s for the M16A3), switching with cancel-before-commit [verified: wp W1,
W6]. Two rules that wp left for integration change predicted weapon state, so they must run in the same `bf3_sim`
code on both ends. They are **not** edits of wp's files: sv adds a new file pair `bf3/sim/weapons/weapon_rules.h`
+ `src/bf3_sim/src/weapons/weapon_rules.cpp` (declared in W2.0) with one wrapper, and both `GameServer` (§4.1 step
4a) and cl's `catch_up` call the wrapper, never `step_soldier_weapons` directly. wp's W1–W11 do not call it, so they
stay bit-equal by construction.

* **Zoom refused while deploying or sprinting** [inferred BF3 behaviour]. wp sets `sway.zoom` straight from `kZoom`
  in `make_weapon_context` (weapon_sim.cpp) and has no deploy test. The wrapper, before stepping, clears `kZoom` from
  its copy of the command and sets `ctx.sway.zoom = NoZoom` when the active slot's phase is `Deploying` with
  `phase_end` after the tick's start, or the command switches to another valid slot (a deploy starts this tick), or `ctx.sprinting`. The
  whole tick is refused (a deploy that ends mid-tick zooms from the next tick). Spread, recoil and the slot's
  `kZoomWasDown` flag then see no zoom.
* **The fire-mode counter at spawn.** `spawn_soldier_weapons` zeroes the state, so every slot's `mode_counter` is 0,
  and `step_soldier_weapons` counts `fire_mode_steps(s.mode_counter, cmd counter)` (weapon_sim.cpp): the first
  command after a spawn would cycle the mode by the client's current counter. The wrapper, when `cmd.tick ==
  spawn_tick + 1`, first writes the command's counter into every slot's `mode_counter`, so that command makes 0
  steps. `spawn_tick` is the life's spawn tick, which both ends know (the server's life stamp; the client's
  `Spawned` event, kept in the ring entry).

```cpp
namespace bf3::sim::weapons {
// The M2a integration rules (§5.2) around step_soldier_weapons; the server and the client's catch_up call only this.
void step_weapons_m2a(const WeaponTable &, SoldierWeaponsState &r_state, const Command &, WeaponContext ctx,
		uint64_t match_seed, Tick spawn_tick, WeaponTickOutput &r_out);
}
```

`m2a.contract` rows for the wrapper: a zoom held from spawn through the deploy gives `kZoomWasDown` false and hip
spread until the first tick after `DeployFinished`; a zoom held while sprinting likewise; a counter of 2 in the first
command after spawn leaves the mode unchanged and a later change of 1 steps once.

ADS for the spread table is instant (wp §5.2). The visual transition (zoom-in 0.09 s iron, zoom-out 0.15 s, FOV
transition 0.15 s [verified data: topics/01 §1.4]) is presentation only (§11.2).

### 5.3 Projectiles and the world

The server's `ProjectileWorld` flies every shot against the server's `CollisionWorld` and the `RewindProvider`'s
targets (§6). Bullets stop at the first soldier (`bullets_pass_soldiers` false). The client flies the same shots
cosmetically (§8.4). The closed-form flight makes both agree to well under a millimetre (E2).

### 5.4 Health numbers that the criteria pin

From `bf3dump weapons` on the install [verified 2026-10-16]: M16A3 25 → 18.4 over 8–50 m, head 2.0, chest 1.0,
legs 0.91; MK11 50 → 37.5 over 15–75 m, head 2.35, chest 1.25, legs 0.91 (the same row for the SVD). Soldier
health 100. So M16A3 chest 4 / head 2 at ≤ 8 m and 6 / 3 at ≥ 50 m; MK11 head 117.5 at 5 m, and at 60 m 40.625 ×
2.35 = 95.47, two hits.

### 5.5 The unmeasured switches: what M2a ships and how they change

S5 Session 1 has not run. M2a ships wp's defaults [verified: mechanics.h]:

| switch | shipped | the alternative the switch selects |
|---|---|---|
| `disc` | UniformArea | UniformRadius |
| `recovery` | Linear | Exponential |
| `first_shot_rearm` | OnTriggerRelease | WhenRecovered |
| `min_spread_rise` | Immediate | AtDecreaseRate |
| `falloff_basis` | StraightLine | PathLength |
| `apply_protection_threshold`, `bullets_pass_soldiers`, `stance_transition_penalty` | false | true |
| `measured` | false | true once S5 decides |

**The switch.** A new tuning file `bf3-opensource/config/weapon_mechanics.cfg` (key = value, the motor_tuning.cfg
format) is parsed by `parse_mechanics_tuning` into `Mechanics` at level load on both ends. `Mechanics` is part of
`weapon_table_hash`, which goes into MatchInfo as `weapon_hash`. Hello carries no hash, and M1 only counts a
`motor_hash` mismatch; M2a makes the weapon check a refusal on the client side: on receiving MatchInfo the client
builds its table (with MatchInfo's `weapon_flags`), and if its hash differs it disconnects with the reason "weapon
table mismatch" (`ClientSessionStats::weapon_hash_mismatch`, state `Refused`) before Ready, so prediction can never
run on another variant than the server (cl owns it; E6 measures exactly this path). A server-side kick is M3. When S5
Session 1 decides, the change is one line per switch plus `measured = true`; no code changes, and E1–E6 rerun.
Every report and the dev overlay print "unmeasured" while `measured` is false (wp rule).

### 5.6 Kits

Default TDM loadouts on Metro [verified: `bf3dump kits MP_Subway TeamDeathMatch0`]:

| kit | US primary | RU primary | both teams |
|---|---|---|---|
| assault | `u_m16a4` (M16A3) | `u_ak74m` | sidearm (US `u_m9`, RU `u_mp443`) in slot 1, `u_m67` in slot 6, `u_knife` in slot 7 |
| engineer | `u_m4a1` | `u_aks74u` | same |
| support | `u_m27iar` | `u_rpk-74m` | same |
| recon | `u_mk11` | `u_svd` | same |

The kit data also lists gadgets (SMAW / RPG-7, repair tool, ammo bag, medkit, radio beacon) in slots 2, 4 and 5.
M2a leaves those slots empty (plan: "1 primary + pistol + knife + M67"); `kits.h` resolves only slots 0, 1, 6, 7
and reports the skipped ones. The local player picks a kit with `--kit=` or K and a team with `--team=` or T (sent
as `KitSelect {kit, team}`, applied at the next spawn); bots take kit `slot % 4` and S1's alternating teams 1 / 2. 12
distinct items in all. M1 puts every client on team 1 (game_server.cpp) with no way to change it; in M2a a team change is accepted only
with `dev_commands` (team balance and the team-switch rules come with M2b's TDM), which is what E10 and E11 need to
spawn the RU kits.

---

## 6. Lag compensation and hit capsules

### 6.1 What the server rewinds to

The client draws the others at a render time behind the server (M1 §9.4: received server time − d, d = 75–150 ms).
To hit what the player saw, the server tests a human's shot against the targets **as the client drew them**:

* **Same samples.** The server's history holds, per soldier and snapshot tick, the quantised `OtherSoldier` it sent
  (step 7 of §4.1; receiver-independent). The client interpolates the same records.
* **Same function.** `net::sample_other` is the Hermite sampler that `present_others` uses today, moved into
  `bf3_net` so both ends call one function. Its inputs are everything `present_others` reads today, passed in:

  ```cpp
  namespace bf3::net {
  struct TimedOther { uint32_t server_tick; OtherSoldier soldier; };          // = host::OtherSample, moved here
  struct OtherPose { uint8_t slot, generation, pose, target_pose, transition, mode, flags; bool extrapolated, held;
  	sim::Vec3 position, velocity; float yaw, pitch, eye_height; uint32_t tick_a, tick_b; uint8_t kind; };
  // p_samples: one soldier's samples, oldest first, distinct ticks (the client's track after its rules, §6.3).
  OtherPose sample_other(std::span<const TimedOther> p_samples, double p_render_ticks,
  		std::span<const float, sim::kPoseCount> p_pose_eye_heights, double p_max_extrapolate_ticks);
  int64_t rewind_time_q8(sim::SimTime p_segment_start, int32_t p_view_back_q8, uint32_t p_max_rewind_ticks);
  // The client's track rules (§6.3) over one target's candidate records; both ends call it.
  size_t rebuild_track(std::span<const TimedOther> p_candidates, uint32_t p_view_tick, std::span<TimedOther, 8> r_list);
  }
  ```

  The selection rule is today's, unchanged: the bracketing pair is the newest sample at or before the render time
  and the next one; before the oldest sample, hold it; past the newest, extrapolate along its velocity for at most
  `p_max_extrapolate_ticks` (250 ms), then hold. Positions are Hermite with the sample velocities, velocity lerped,
  yaw along the shortest arc, pitch and eye height lerped. **Discrete fields** (`pose`, `target_pose`,
  `transition`, `mode`, `flags`, `generation`) all switch together at the newer sample (u ≥ 1), as `pose` and `mode`
  do today; `PresentedSoldier` gains `target_pose` and `transition` (additive), which the capsules (`TargetPose`)
  and tp's stance blend read. `tick_a` / `tick_b` / `kind` (interpolate, extrapolate, hold) say which samples were
  used: the pose cache key (§6.4) and the E12 trace rows use them. The client draws at its double render time; lag
  compensation passes `double(time_q8) / 256`, exact in a double, on both ends. lc writes `other_sample.{h,cpp}`
  from `present_others`' body; cl replaces that body with the call (named exception 1 of §15; the others are
  listed there).
* **Same list on both ends.** `present_others` sees the 3-sample window that `publish` copies from the client's
  track (from the newest sample at or before the publish render time). The render time moves less than one client
  tick (16.7 ms) between publishes and the samples are 2 ticks apart, so the bracketing pair picked in the window is
  always the pair the full track gives; lc's unit test checks the two agree. (Only a main-thread stall longer than
  one sample interval can carry the render time past the window's newest sample while the track has a newer one;
  `present_others` then counts `window_extrapolated`, and E12 reports it.) So the function's list is defined as
  **the client's track after its rules** (§6.3), on both ends.
* **Same render time and same inputs.** Each command carries the render time of the frame the player saw when the
  input was sampled and the newest snapshot seq the client had received when that frame was published (§6.2). For
  a shot at time t from that command, the
  targets are evaluated at t − lag (lag = command tick − render time, §6.4's time rule), from only the samples
  that client had received (§6.3).

This is plan D7's "server-validated hits, tested at the sample the client drew", without a per-hit report: the
command's view sample says which sample to test, and the server re-flies the seeded shot itself. Because the server
restricts itself to the samples the client acknowledged, a lost snapshot no longer makes the two ends disagree. What
remains for M3's `HitReport` and `refly_projectile` is the reordering residual (§6.3) and the clamp beyond 350 ms
(§16 Q2); E12 measures what M2a gets without them.

### 6.2 The view sample in the command

```cpp
namespace bf3::sim {
enum class ViewQuality : uint8_t { None = 0, Interpolated = 1, Extrapolated = 2, Held = 3 };
struct Command {
	// ... M1 fields ...
	uint16_t view_back_q8 = 0;  // (tick << 8) - render_time_q8, server ticks × 256, saturated at 65,535 (4.27 s)
	uint8_t view_quality = 0;   // ViewQuality, from present_others' counters at that frame
	uint8_t view_seq8 = 0;      // low 8 bits of the newest Snapshot seq (this client's seq space) received before
	                            // the drawn frame was published
};
}
```

The main thread puts the frame's render time (`render_ticks_at_publish` advanced to the frame, × 256) and the
`PresentationFrame`'s `newest_seq` (new field: the newest Snapshot seq received when it was published, M1's
`newest_snapshot` at that moment; §7.2 says why not a newer `SnapshotOthers`) into the `InputSample`; the client
thread computes `view_back_q8` when it samples the command. 26 bits per command on the wire. Bots send quality None (they fire at the present, D7). The server resolves `view_seq8` to a
full seq against the command packet's `newest_snapshot` (the newest seq ≤ it whose low byte matches, within 32
seqs; otherwise Refused). `sim::Command` grows by these fields, so M1 tests that pin `Command`'s size, layout or a
command-stream hash change in the same sv commit that adds them (sv; `m2a.contract` pins the new size).

### 6.3 How far back, what the client had, and what is clamped

Lag for a human ≈ RTT + command buffer + d [inferred]: loopback ≈ 0 + 33 + 75 ≈ 110 ms; "150/2" ≈ 150 + 33 + 85 ≈
270 ms; the one-way stress profile ≈ 420 ms. Plan D7 says "≤ 250 ms", which would clamp the gate profile. Default
`max_rewind_ticks` = 21 (350 ms) so "150/2" is fully compensated (§16 Q1). Beyond it the server tests at the
clamped time and counts `rewind_clamped`. A sample older than the history (48 snapshot samples = 1.6 s; corrected
by lc from 24: see m2a-lc.md "history size") is never needed under the clamp.

**Only the samples the client had.** After a lost snapshot (or a lost `SnapshotOthers`) the client interpolates
across the gap or extrapolates, while the server's history holds the missing record. If the server sampled from its
full history it would interpolate where the client extrapolated. So the server rebuilds, per shot, the list the
client's track held, from three stores of `LagHistory` (§6.6):

1. **Records per tick, shared** (§4.1 step 7): `ticks[tick % 24][slot]` = the quantised `OtherSoldier`, plus a
   live mask. Receiver-independent, so stored once.
2. **Sent ring per client** (§4.1 step 8): 64 entries indexed by the client's seq: `{seq, tick, main_mask,
   others_mask}`. This maps that client's seq space to ticks and says which datagram carried which slot for that
   client (the nearest-first split is per receiver).
3. **Acks per client** (§4.1 step 1): two bits per sent-ring entry, `main_received` and `others_received`, set from
   every command packet's `newest_snapshot` / `snapshot_ack_bits` / `others_ack_bits` (bit conventions in §7.2) and
   never cleared while the entry lives. An ack for a seq the ring does not hold, or an others bit for a seq that sent
   no `SnapshotOthers`, is ignored and counted.

**The received set of a shot, the same on both ends.** When a command first arrives (the first packet that carries
it; commands are sent redundantly), the server stores with it the slot's cumulative ack state at that moment; the
client stores with the command its own received state when it first sent that command, which is exactly what that
packet acked. When the command fires, both ends put the same snapshot into the shot's view: the full `view_seq`
(resolved from `view_seq8`, §6.2) and two 32-bit masks, bit i = the Snapshot / `SnapshotOthers` of seq `view_seq − i`
had arrived (`ShotView`, 16 B, §6.6). The only way they differ is a datagram with a seq ≤ `view_seq` that arrived
between the client's first send of the command and the packet the server first got it in (the first packet was
lost, or packets were reordered, and a datagram arrived in between); the client counts every arrival with a seq ≤
the `view_seq` of a firing command it has already sent (`late_sample`, an upper bound of the mismatches).

**One rebuild function.** lc writes `net::rebuild_track(candidates, view_tick, r_list)` in `other_sample.h`, and both
ends call it. The candidates of target T are, for i = 0..31, T's record of seq `view_seq − i` when the mask bit of
the datagram that carried T to this client is set. The server gets them from `LagHistory` (store 1 by the seq's
tick, store 2 for which datagram carried T); the client from its new `ReceivedRecords` ring (cl: the records of the
last 32 seqs exactly as they arrived, per slot, ≈ 96 KiB), filtered by the shot's masks. `rebuild_track` then
applies **the client's track rules** (client_session.cpp `add_others` and `publish` [verified: integ]):

* sorted by tick, duplicates once (`add_others` inserts reordered samples in tick order and skips a duplicate);
* only the newest `kTrackSamples` = 8 (`pop_front` after the insert keeps the 8 newest ticks whatever the arrival
  order);
* only the generation of the newest record in the set (`add_others` resets the track on a generation change);
* empty if the newest record is more than 60 ticks (1 s) older than `view_seq`'s tick (the client drops a track
  after `kTrackTimeoutNs` = 1 s without a sample).

`sample_other` then runs on that list at the shot's rewound time, on both ends: the server for the hit, the client
for its cosmetic test (§8.4). So the two agree bit for bit by construction, except `late_sample`. **What the frame
drew** can differ from that list (these do not break the server = client-test equality, but they are where the
player saw something else), each counted on the client and reported by E12: a reordered old-generation record that
reset the drawn track (`generation_reorder`); a `SnapshotOthers` newer than the newest arrived Snapshot, drawn but
outside `view_seq` (`others_ahead`, §7.2); the wall-clock track timeout against the 60-tick rule when snapshots stall
for about 1 s (`track_timeout`); the 3-sample window after a main-thread stall (`window_extrapolated`, §6.1).
`sim.lagcomp.unit` checks the server's and the client's rebuilds against each other and against a model of the
drawn track over generated arrival orders, for two clients with different seqs and splits at the same ticks.

Verdicts per shot, after IW4L's `ShotSampleProvenance` / history verdicts (studied, own code): `Present` (bot, or
quality None), `Rewound`, `Clamped`, `Refused` (malformed: lag < 0 after decode, `view_seq8` outside the history,
quality Held beyond 250 ms of extrapolation, `view_seq` not in the client's sent ring). Refused shots are tested at
the present. All four are counted in `LagCompStats`.

### 6.4 The target set at a rewound time

* **Time rule (both ends, one function).** A query for a projectile segment that starts at `time_start` tests the
  targets posed at `net::rewind_time_q8(time_start, view_back_q8)` = time_start in q8 ticks − (view_back − 256),
  the lag clamped to 0 .. max_rewind_ticks × 256 (§6.3) and rounded to 1/256 tick (time_start in q8 ticks: the state
  after server tick S sits at S × 256, so the step of command tick T covers [(T − 1) × 256, T × 256); a shot at the
  start of command tick T then tests at exactly the drawn render time R = (T << 8) − view_back; corrected by lc,
  see m2a-lc.md "time rule": "− view_back" alone tested one tick before R). One pose per segment (the segment is at most one tick; a target moves ≤ 11 cm in
  it at sprint speed). The view sample is the one stored for the projectile's shot index, so every shot of one
  command, and every tick of one bullet's flight, uses the same lag; the target time advances with the bullet. A
  projectile spawned mid-tick starts its segment at its spawn time. The client's cosmetic test (§8.4) calls the same
  function with its own command's view sample, so E12 compares like with like.
* Only targets alive at both the sample and now, and with the same life generation (`OtherSoldier::generation`): a
  respawned soldier is never hit at its old body.
* The shooter is never its own target.
* **Broad phase on the segment.** wp's `TargetQuery` carries the segment's times and the projectile's identity but
  not its geometry, so a provider could not cull. lc adds, additively at the end of `TargetQuery` (hit_target.h),
  `Vec3 from{}, to{}; float radius = 0; bool has_segment = false;`, and `ProjectileWorld` fills them at its query
  sites (ballistics.cpp: the flight segment; a blast's centre and `BlastRadius`; refly). `StaticTargets` and the
  present-tick provider ignore them, and wp's W1–W11 rerun unchanged (the contract test checks bit-equal results).
  The provider tests the segment against each target's **swept bound over the history window**: an AABB of the
  target's recorded feet positions over the last `max_rewind_ticks` + 1 snapshot ticks and its present position,
  grown by the capsule table's `bound_radius` and the 250 ms extrapolation at the largest recorded speed, plus the
  segment radius (≈ 3 ns each, SoA; maintained in `record_tick`, so it costs nothing per query). A falling,
  jumping or blast-thrown soldier is therefore never culled where the client could have drawn it; a constant
  speed bound would miss those. Only the candidates are posed.
* **Pose cache shared across shooters, keyed by the sample inputs.** Two queries at the same time with different
  received sets can need different poses (one interpolates, the other extrapolates across a datagram it lost). So
  the key is everything `sample_other`'s answer depends on: (target slot, generation, time q8, `tick_a`, `tick_b`,
  `kind`), where the pair and the kind come from running the selection rule on the shooter's rebuilt list (§6.3;
  ≈ 50 ns, done before the lookup). The records of a tick are shared by all clients (§6.3 store 1), so equal keys
  mean equal inputs and equal answers. Open-addressed, 2,048 entries, cleared in `begin_tick`; when full, a pose is
  computed without being cached and counted. The answers are therefore independent of the cache, and
  `sim.lagcomp.unit` checks that (cache on vs off, bit-equal). Bots (lag 0) share one present set. The answer to a
  query is a scratch list of the candidates' `HitTarget`s in slot order, valid until the next call, as wp's contract
  requires; the old "last answer per (shooter, sample)" cache is dropped, because `ProjectileWorld` steps
  projectiles in spawn order with shooters interleaved and would thrash it.
* The shooter's view sample is stored per shot (ring of 128 entries per slot: shot index → `ShotView` {view_back_q8,
  quality, view_seq, main bits, others bits}, 16 B), because a bullet flies for several ticks after the command that
  fired it.

### 6.5 Hit capsules from the server pose (R14)

R14's mitigation is a reduced skeleton for the 9 hi-LOD capsules, recomputed from root, stance, aim and phase
[plan R14]. M2a builds that once per level as a table:

* **Clip source** [verified: `antdump list --install b_basicassetsmp`, 2026-10-16; every name occurs exactly once,
  as S3's loader requires, all on rig VeniceAntSke01]:

  | role | controller | asset, frames |
  |---|---|---|
  | stand idle / aim up / aim down | `Rifle_StandIdle_Pose` / `Rifle_StandAimUp` / `Rifle_StandAimDwn` (S3's) | Raw 1 / Raw 1 / Raw 1 |
  | crouch idle / aim up / aim down | `Rifle_CrouchIdle_Pose` / `Rifle_CrouchAimUp` / `Rifle_CrouchAimDwn` | Frame 0 / Dct 1 / Dct 1 |
  | prone idle / aim up / aim down | `Rifle_ProneIdle_Pose` / `Rifle_ProneAimUpPose` / `Rifle_ProneAimDwnPose` | Frame 0 / Dct 1 / Raw 1 |
  | transitions | `Rifle_T_StandToCrouch`, `_StandToProne`, `_CrouchToStand`, `_CrouchToProne`, `_ProneToStand`, `_ProneToCrouch` | Dct, end frames 30 / 32 / 28 / 42 / 39 / 44 |

  The crouch and prone aim clips have the stand aim clips' layout (41 rotation, 12 vector channels, target
  `FullBodyWeaponRootNoFingers`); whether they decode as additive is checked at build (`DecodedClip::additive`). The
  transitions are sampled at 16 fractions of their own length (the motor's transition fraction maps to the clip's
  normalised time).
* **Fallback, per row.** A row whose clip is missing, fails to bind or is not additive where required is built from
  wp's `standin_target` layout for that stance (transition rows: a lerp of the two stance rows by the fraction).
  Every row is tagged clip-derived or stand-in, and E18 prints the tags (0 stand-in rows expected).
* **One choice for both users.** tp (3p soldiers, wave 3) loads the same controller names through the same
  `ServerPoseClipNames` list (an additive extension of S3's `CrowdClipNames`) and uses the same stand-in rule, so a
  stance the server poses from a stand-in is also drawn from the stand clips, and the report says so.
* `bf3_anim::build_capsule_pose_table(rig, clips, capsules)` evaluates the 3p rig per stance row at its idle clip
  with its own additive aim (pitch weights from S3's 78.1° up / 60.7° down; the crouch and prone aim limits from the
  same LocalPost rule [inferred]) at 33 pitch buckets, and per transition at 16 fractions with the source stance's aim. For each
  entry it stores the 9 capsules' segments in the soldier's local frame (from `HitCapsule` bone, offset, length,
  axis, radius [verified: material_grid.h]; bones mapped to rig joints by name, as antdump `rigmap` does).
* Size: (3 × 33 + 6 × 16 × 33) entries × 9 capsules × 32 B ≈ 0.90 MiB; build ≈ 3,300 pose evaluations, a few tens of
  ms [inferred], cached (§13.1).
* `sim::weapons::posed_target(table, TargetPose)` lerps between the two nearest pitch buckets (and transition
  fractions), rotates by yaw and translates by the feet: O(1) per target, ≈ 0.3 µs for 9 capsules [inferred]. Both
  ends use it: the server for hits, the client for its cosmetic test (§8.4).
* The table's hash goes into `weapon_hash`, so a client with another table is refused.
* Not modelled: leg motion of locomotion (the legs stay in the idle stance), reload and fire upper-body motion.
  `anim.server_pose` (install, E18) compares the table with a reference evaluation that does not use the table
  builder (bf3_anim's `sample_pose` → `apply_additive` at the exact pitch weight → `model_pose`, capsules placed
  from the bone transforms) at off-grid pitches and transition fractions, so the row measures the bucket and
  fraction interpolation and the builder's capsule placement, not the builder against itself; the stand rows are
  also checked against S3's `CrowdEvaluator` (independent RS-skeleton code). Gates: head and chest ≤ 3 cm in the
  steady rows, ≤ 5 cm in the transition rows [inferred thresholds]; run and legs reported. The crouch and prone rows
  against `CrowdEvaluator` need tp's stance inputs and rerun in wave 3 (tp, E18 (b)). Until lc lands (§15), the
  server uses wp's `standin_target` layout.

```cpp
namespace bf3::core::gameplay {
struct CapsuleLocal { float a[3], b[3], radius; uint8_t zone, capsule, property, flags; };      // 32 B
struct CapsulePoseTable {
	uint32_t version = 0; uint64_t hash = 0; uint8_t capsule_count = 0;                          // 9
	std::array<float, 33> pitch_deg{};                                                            // -60.7 .. +78.1
	std::vector<CapsuleLocal> steady;      // [stance 3][pitch 33][capsule]
	std::vector<CapsuleLocal> transition;  // [transition 6][fraction 8][pitch 33][capsule]
	std::array<uint8_t, 9> row_source{};   // per stance (3) and transition (6): 0 clip-derived, 1 stand-in
	float bound_radius = 0;
};
}
namespace bf3::anim {
struct ServerPoseClipNames { /* the controllers of the table above; package b_basicassetsmp */ };
core::Result<core::gameplay::CapsulePoseTable> build_capsule_pose_table(const Skeleton &rig, const ServerPoseClips &clips,
		std::span<const core::gameplay::HitCapsule> capsules, const CrowdConfig &aim);
}
namespace bf3::sim::weapons {
struct TargetPose { Vec3 feet; float yaw, pitch; Pose pose, target_pose; float transition; uint32_t id, team; bool spawn_protected; };
HitTarget posed_target(const core::gameplay::CapsulePoseTable &, const TargetPose &);
}
```

### 6.6 `lag_comp.h` (sketch, bf3_host, server thread)

```cpp
namespace bf3::host {
struct LagCompConfig { uint32_t history_snapshots = 48; uint32_t max_rewind_ticks = 21; bool enabled = true; };
struct LagCompStats { uint64_t queries, candidates, pose_cache_hits, present, rewound, clamped, refused; double tick_ms_p99; };
struct AckState { uint16_t newest = 0; uint64_t main_bits = 0, others_bits = 0; };   // 64 seqs back from newest
struct ShotView {                       // per shot index, 16 B; a snapshot of the shooter's received set (§6.3)
	int32_t view_back_q8 = 0; uint8_t quality = 0; uint8_t pad = 0; uint16_t view_seq = 0;
	uint32_t main_bits = 0;              // bit i: the Snapshot of seq view_seq - i was acked (bit 0 = view_seq itself)
	uint32_t others_bits = 0;            // bit i: the SnapshotOthers of seq view_seq - i was acked
};
class LagHistory {
public:
	// Shared, once per snapshot tick (§4.1 step 7): every live soldier's record; receiver-independent.
	void record_tick(uint32_t snapshot_tick, std::span<const net::OtherSoldier> records);
	// Per client, after its datagrams of this tick are encoded (§4.1 step 8): that client's seq and split.
	void sent(uint8_t client_slot, uint16_t seq, uint32_t snapshot_tick, uint64_t main_mask, uint64_t others_mask);
	// Per command packet (§4.1 step 1); conventions of §7.2; unknown seqs / parts ignored and counted.
	void ack(uint8_t client_slot, uint16_t newest, uint32_t snapshot_ack_bits, uint32_t others_ack_bits);
	// At a command's first arrival: the slot's cumulative acks, kept with the buffered command (§6.3).
	AckState ack_state(uint8_t client_slot) const;
	// For a firing command: resolve view_seq8 against the stored acks and fill the shot view; false = Refused.
	bool shot_view(uint8_t client_slot, const AckState &at_arrival, uint8_t view_seq8, int32_t view_back_q8, uint8_t quality,
			ShotView &r_view) const;
	// The candidates of p_target under the shot view, then net::rebuild_track (oldest first, <= 8).
	size_t track(uint8_t client_slot, const ShotView &, uint8_t p_target, std::span<net::TimedOther, 8> r_list) const;
	net::OtherPose sample(std::span<const net::TimedOther> list, int64_t time_q8) const;   // net::sample_other
	void client_reset(uint8_t client_slot);   // connect / disconnect: the sent ring and acks of that slot
};
class RewindProvider final : public sim::weapons::HitTargetProvider {
public:
	RewindProvider(const LagHistory &, const core::gameplay::CapsulePoseTable &, const LagCompConfig &);
	void begin_tick(uint32_t tick, std::span<const sim::SoldierState> present);  // present set, bounds SoA, cache clear
	void note_shot(uint8_t slot, uint32_t shot_index, const ShotView &);
	std::span<const sim::weapons::HitTarget> targets_for(const sim::weapons::TargetQuery &) const override;
	const LagCompStats &stats() const;
};
}
```

### 6.7 Rewind cost for 64 soldiers

Cost terms [inferred]: per query the broad phase over 63 swept bounds ≈ 0.2 µs; per candidate the track rebuild
and pair selection (§6.3, ≤ 32 seq lookups) ≈ 50 ns; per new cache key `sample_other` ≈ 40 ns + `posed_target` ≈
0.3 µs; per candidate a copy into the answer (≈ 0.6 KB) ≈ 50 ns. Queries
per tick = live projectiles (one segment per projectile per tick, wp).

| bench row (64 human shooters at the maximum lag, M16A3 full auto, 13.3 rounds/s each) | live projectiles | candidates per query | per tick |
|---|---|---|---|
| "pairs": 32 opposing pairs 20–40 m apart (the firefight layout) | ≈ 64 × 13.3 × 0.05 s ≈ 45–100 | ≈ 1–2 (the target and a neighbour) | 100 × (0.2 + 2 × 0.45) µs ≈ 0.11 ms |
| "long": every bullet flies its full 1.5 s above the world | ≈ 64 × 20 = 1,280 | 0 | 1,280 × 0.2 µs ≈ 0.26 ms |

Distinct poses per tick are at most the candidates (each new key is posed once); with lag and received sets varying
per command, sharing happens for bots, for the shots of one command and for a bullet's own later segments only
when the key repeats, rarely across humans. E13 gates both rows at p99 ≤ 0.5 ms and prints queries, candidates and
cache hits. Bots add nothing beyond the present set (posed once in `begin_tick`, 64 × 0.35 µs ≈ 22 µs). Memory:
shared records 48 snapshots × 64 × ≈ 48 B ≈ 144 KiB plus the swept bounds (64 × 24 B); sent rings and acks 64 clients ×
64 seqs × 24 B = 96 KiB; shot-view rings 64 × 128 × 16 B = 128 KiB; pose cache 2,048 × ≈ 0.6 KB ≈ 1.2 MiB; the
present set ≈ 40 KiB.

---
## 7. Protocol additions (bf3_net v2; the path to M3)

`kProtocolVersion` 1 → 2. Channels unchanged: ch0 unreliable unsequenced, ch1 reliable, ch2 bulk; datagrams ≤ 1,200 B
[verified: protocol.h, m1.md §7.1]. Message numbers below are proposals; nt picks free values. nt owns every wire
format below, including the dev messages; the server's handling of them is sv's, the client's cl's (§15).

### 7.1 MatchInfo

Gains `match_seed` (u64), `weapon_hash` (u64: table + mechanics + `exact_aim` / `infinite_magazine` + capsule
table), `weapon_flags` (u8: bit 0 `exact_aim`, bit 1 `infinite_magazine`; the client builds its table with them),
`kit_hash` (u64), `max_rewind_ticks` (u8). M1 only counts a `motor_hash` mismatch ("M3 kicks"); for `weapon_hash`
and `kit_hash` the client disconnects with a reason on receiving MatchInfo, before Ready (§5.5; cl). Hello is
unchanged.

### 7.2 Commands

Each command gains `view_back_q8` (16 bits), `view_quality` (2) and `view_seq8` (8); the weapon fields already ride
inside the 24 button bits. `CommandPacket` gains `others_ack_bits` (u32) next to M1's `snapshot_ack_bits`. The two
conventions differ by one on purpose, and both are pinned by `m2a.contract` and the `net.proto` tests:

| field | bit i means | the newest seq itself |
|---|---|---|
| `snapshot_ack_bits` (M1, protocol.h) | the Snapshot of seq `newest_snapshot − 1 − i` arrived | implied: `newest_snapshot` is a Snapshot that arrived |
| `others_ack_bits` (new) | the `SnapshotOthers` of seq `newest_snapshot − i` arrived | bit 0 (the main Snapshot's arrival does not imply its `SnapshotOthers`) |

`newest_snapshot` keeps M1's meaning (the newest Snapshot that arrived), so M1's server code that reads it is
unchanged. A `SnapshotOthers` whose seq is newer than the newest arrived Snapshot (that Snapshot was lost) cannot be
acked yet; the frame's `newest_seq` and therefore `view_seq8` (§6.2) are also the newest *Snapshot* seq, so the
server leaves such records out, and the client counts each frame whose track holds one as `others_ahead` (an E12
residual, §6.3). A 4-command packet grows from 53 B by 17 B [inferred].

### 7.3 Snapshot

* **Own weapons** (`weapons_wire.h`): a compact wire view of `SoldierWeaponsState` (≈ 600 B in memory) at full
  precision, ≈ 110 B: `next_shot_index`, `active`, `pending`, `events`, `sprint_recover_until`, per non-empty slot
  (def, magazine, reserve), and for the active and the pending slot every timer as an i32 offset from the snapshot
  tick's start (saturated at ± 35 s), spread, spread time, recoil, hold start, phase, mode, counter, burst, flags.
  `weapons_wire_view(state)` is what both ends compare (§8.2); the contract test round-trips every state of wp's
  W7 600-tick run and checks that stepping the decoded state gives the same states as stepping the original.
* **Others** gain 19 bits each: weapon (u8 index into the match's def list, 255 none), phase (3), firing (1: trigger
  down and a shot this interval), ADS (1), reload sequence (2: counts reload starts, so a lost snapshot cannot hide
  one), burst sequence (4: the audio seed, au §7.2). ≈ 178 bits per other.
* **Everyone drawn.** With the own block the first datagram holds ≈ 49 others; S1 measured only 57 of 66
  drawn with 64 bots [verified: S1 run]. M2a adds `SnapshotOthers` (unreliable, same `seq` and the same
  `server_tick` as its Snapshot, both carried in its header, so its records are usable when the Snapshot of that seq
  is lost) carrying the remainder, nearest first in both. A lost `SnapshotOthers` makes its soldiers extrapolate
  (≤ 250 ms) like a lost snapshot, and its ack bit tells the lag compensation (§6.3). M3 replaces both with
  priorities and baselines (§16 Q3).

### 7.4 Events

| message | channel | content | size |
|---|---|---|---|
| `ShotEvents` | ch0, one datagram per snapshot when non-empty (two at most; further shots dropped farthest-first and counted) | server tick; per shot: shooter slot (7); shot index: the full 32 bits for a shooter's first shot in the datagram, then a 4-bit delta (escape to 32 bits), so a client that joins late has every index (the effect seeds need the full index, §4.4); def (8); time within the tick (16, 1/65,536 tick); origin (3 × 20 bits, 1/256 m from the level bounds' minimum: a 4,096 m span per axis; Metro's static tiles span 1,152 × 2,112 m and Caspian's 1,792 × 1,024 m [verified: `bf3dump collision` tile keys, 64 m tiles], so both fit; heights of aircraft far above the bounds are an M3 / M4 input, like the bandwidth); direction (octahedral 2 × 16); pellets (5); tracer (1); with tracer set, aim (octahedral 2 × 12: the tracer converges on it, §12.2); with pellets > 1, `pellet_cone` (12 bits, 1/64°, so remote ends expand pellets like the server; no M2a kit item has pellets, M3 / M7 shotguns use it); for a hold-and-release def, `power` (8 bits, 1/255). `spawn_time` is derived (time + the def's `SpawnDelay`); seeds are derived (§4.4), never sent | ≈ 18 B per shot, + 3 B with a tracer; ≈ 28 shots per snapshot in a 64-bot firefight ≈ 0.55 KiB |
| `CombatEvents` | ch1 | `HitEvent`, `DeathEvent` and `DetonationWire` (§4.3), quantised: damage and health in 1/100 hp, position as above, normal octahedral 16 | ≈ 22 B per hit, 8 B per death, 16 B per detonation |
| `KitSelect` | ch1, client → server | kit (u8 < 4) and team (u8: 0 keep, 1, 2; a change needs `dev_commands` in M2a) for the next spawn | 3 B |
| Spawned (M1) | ch1 | gains the kit index and the team | +2 B |
| `DebugRequest` / `DebugReport` (M1) | ch1 | new kinds `SpawnTarget {slot, position, yaw, pose}` and `TargetInfo {slot}` → report `TargetInfoRow {slot, pose, feet, yaw, HitTarget capsules}`; `Trace`'s report gains per tick the `ShotEvent`s, projectile positions of the traced shooter (≤ 64), `DamageEvent`s, `DetonationEvent`s and deaths; dev only, as M1; the per-tick additions are bounded (§1) | Trace ≤ 20 s and ≤ ≈ 0.5 MiB, fragments on ch1 |

The owner does not receive its own shots (it predicted them). 3p reloads are derived from the reload sequence in the
others' records; nothing else is needed for M2a. Effects and sounds are never on the wire: the checks start them
client-locally (§15, gx).

**Bandwidth** in a 64-bot firefight per client [inferred]: snapshots ≈ 1.6 KiB × 30 = 48 KiB/s, shots ≈ 16 KiB/s,
combat ≈ 2 KiB/s, ≈ 66 KiB/s in all. Fine for loopback and LAN, but well above M3's exit of ≤ 36 KB/s per client:
**an M3 input**. The levers are delta compression and baselines for the others, relevance culling of shots
(farthest-first already exists), and fewer remote shot fields (origin relative to the shooter's replicated eye).

**Path to M3.** The same messages are what M3 replicates to remote clients: shots (cosmetic flight on each client),
hits, damage, deaths and detonations (reliable, authoritative), reloads (state). M3 adds HitReports with refly,
delta compression and relevance; none of the M2a message meanings change.

---
## 8. Client: prediction of the own weapon, server authority

### 8.1 What is predicted and what is not

| predicted on the client (same bf3_sim code) | authoritative from the server only |
|---|---|
| fire timing, shot times, seeds, directions; magazine and reserve; reload, deploy, switch, fire mode; spread and recoil offset (the camera applies the recoil) | hits on soldiers, damage, health (except M1's predicted fall damage), deaths, kills; own death; spawn, kit and team |

### 8.2 Ring, replay, reconciliation (extends m1.md §9.2)

* The prediction ring entry gains `SoldierWeaponsState` (+ ≈ 600 B per entry; M1's default ring of 128 entries →
  ≈ 77 KiB more).
* `sim::catch_up` steps the weapons after the motor with the same commands, in the same order as the server, through
  the same `step_weapons_m2a` wrapper (§5.2), with the life's spawn tick from the ring entry.
* On each snapshot the client compares `weapons_wire_view(predicted at server_tick)` with the decoded own block.
  A mismatch rebases the weapons from the wire view and replays (M1's rule). Expected 0 in loopback (E15).
* **Health is not a motor mismatch.** M1's `states_match` compares health (client_session.cpp), so every server hit
  on the own soldier would force a motor replay. cl splits the compare: when only what a hit writes differs (and the
  life is the same), the client writes the server's health into the ring entries from the snapshot tick on, with no
  replay, and counts `health_rebases`; a death arrives as a motor state change and reconciles as in M1. What a hit
  writes is wp's `apply_damage`: the health, the tick's `damage` and the `kDamaged` event (rule fixed by cl against
  sv's server: "only health" reconciled every hit, because `damage` and `kDamaged` differ in the hit's tick too; the
  motor's own fall damage stays covered, it follows from `kLanded`, `impact_speed` and `protect_ticks`). E15 counts
  motor and weapon reconciliations without the health rebases, and reports those separately.
* **Only the first prediction of a tick emits.** Replays keep state but discard their `ShotEvent`s and timed events;
  the client tracks the highest emitted tick (wp §7.2). So effects and sounds never fire twice.
* On the own death (server state) the client stops emitting; on Spawned it calls `spawn_soldier_weapons` with the
  kit and team from the event.

### 8.3 Presentation

* `PresentationFrame` gains `own_weapons_prev/curr` (the wire views of the two latest predicted ticks, so spread,
  recoil and reload progress interpolate per display frame), the active def's static UI row index and `newest_seq`,
  the newest Snapshot seq received at publish (the main thread puts it in the `InputSample` as `view_seq8`, §6.2).
* **Events go through a ring, not the frame.** The frame is a latest-value copy (M1 §9.5); events must not be lost
  between two reads. `PresentationEvents` is a single-producer single-consumer ring (2,048 entries × 64 B), client
  thread → main thread, with an overflow counter (gated 0, E14). Entry kinds: OwnShot, OwnTimed (deploy, reload
  start / commit / finish, bolt, mode, empty click), RemoteShot, Impact, SoldierContact, Hit, Death, Detonation,
  Spawn. Each carries its sim time and its **timeline**, so the main thread schedules effects (start age) and
  sounds (mixer frame) exactly:
  * **Predicted timeline**: the own soldier's events (OwnShot, OwnTimed, own-shot Impacts and SoldierContacts),
    placed at the predicted time the own viewmodel and camera show.
  * **Render timeline**: everything about other soldiers (RemoteShot and its impacts, Hit and Death of another
    soldier, Detonation), placed at render time = server time − d, the time `present_others` draws them at. Without
    this, a remote muzzle flash, tracer or death would lead the drawn shooter by 75–150 ms.
  * The two server events about the own player go at once on arrival: a Hit whose victim is the own soldier (health,
    damage direction) and a Hit whose shooter is the own soldier (the hit indicator, §11.4).

```cpp
namespace bf3::host {
enum class PresentKind : uint8_t { OwnShot, OwnTimed, RemoteShot, Impact, SoldierContact, Hit, Death, Detonation, Spawn };
enum class PresentTimeline : uint8_t { Predicted, Render, Immediate };
struct PresentEvent { PresentKind kind; PresentTimeline timeline; uint8_t slot, aux; uint16_t def; uint32_t id; int64_t time;
	float pos[3], dir[3]; uint32_t material; };   // 64 B
class PresentationEvents {   // SPSC
public:
	bool push(const PresentEvent &);            // client thread; false and counted when full
	size_t drain(std::span<PresentEvent> out);  // main thread
	uint64_t overflows() const;
};
}
```

### 8.4 The client's cosmetic projectiles

`ClientProjectiles` (client thread) owns two `ProjectileWorld`s against the client's `CollisionWorld`:

* **Own world, predicted timeline**: every own predicted shot, stepped with the prediction (first prediction only).
  Its target provider poses the others with `posed_target` from `net::sample_other` over the list that
  `net::rebuild_track` builds from the client's `ReceivedRecords` under the shot's received set (§6.3, the same
  function and inputs as the server's) at `net::rewind_time_q8(time_start,
  view_back_q8)` of the command that fired the shot: the server's time rule of §6.4, on the same list. So the client
  tests a target exactly where the server will, up to the counted `late_sample` shots of §6.3. Each tested sample's
  (`tick_a`, `tick_b`, `kind`) and pose go into the client's E12 log, which the test compares with the server Trace.
* **Remote world, render timeline**: every remote `ShotEvent` (pellets from the derived seed, `pellet_cone` and
  `power` from the message), spawned when the render time reaches its time and stepped to the render time each
  client tick, against the others at the render time. Its impacts are therefore drawn in step with the drawn
  shooters. A remote grenade's own cosmetic detonation is suppressed; the server's `DetonationWire` places it.

A static contact gives an `Impact` event (the effect, decal and impact sound, §12). A soldier contact gives
`SoldierContact`: for own shots the blood effect plays at once (cosmetic; the hit indicator waits for the server),
for remote shots nothing plays until the server's Hit event, which carries the position. E12 counts the agreement
between own `SoldierContact`s and the server's hits.

---
## 9. 3p soldiers from server state

S3 deferred driving the crowd from the server (`present_others` → `CrowdSoldierInput`, s3.md §13); M2a does it.
`CrowdSoldierInput` gains, additively [inferred field set]:

```cpp
struct CrowdSoldierInput {            // existing: position, velocity, yaw, pitch, camera_distance, team, flags
	uint8_t pose = 0, target_pose = 0; float transition = 0;  // stance and its blend
	uint8_t weapon = 0;               // index into CrowdTeam::weapons (4 primaries, sidearm, knife, M67)
	uint8_t fire_seq = 0;             // changes per shot: upper-body fire kick
	bool ads = false, dead = false;
};
struct CrowdWeapon {                  // new: one per item a team can hold
	const SkinBinds *binds = nullptr;     // the 3p weapon mesh only (body, head, helmet stay in CrowdTeam::binds)
	const CrowdClipData *reload = nullptr; float reload_rate = 1.0f;   // S3's per-team reload, now per weapon
	std::span<const Xform> weapon_local;  // weaponske01 LocalPose of this item (S3 step 3)
	uint8_t palette = 0;                  // material palette index of the 3p mesh
};
```

**This is a structural extension of S3's `make()` inputs, not only new input fields** (W3.0 declares it): today
`CrowdTeam` holds one `SkinBinds` list (body, head, helmet and the weapon together), one reload clip and one
`weapon_local` per team [verified: crowd_eval.h]. tp splits the weapon out of the team's binds into
`CrowdTeam::weapons` (`std::span<const CrowdWeapon>`, ≤ 8), keeps the existing fields as the team's body and the
default weapon (index 0, so S3's own scenes and tests run unchanged), and the evaluator picks the reload clip,
`weapon_local` and weapon mesh per soldier from `CrowdSoldierInput::weapon`. The skinning step draws the body from
the team binds and the weapon from the soldier's `CrowdWeapon`. The S3 tests gain a case with two weapons per team
(`anim.s3.unit`).

* **Locomotion**: S3's run blend; stance from the pose fields. The stance idles, aims and transitions are the
  controllers of §6.5's table, loaded through the same `ServerPoseClipNames` with the same per-stance stand-in rule,
  so the drawn soldier and the server's capsules come from one source. Moving in a stance uses
  `Rifle_SneakFwd` / `_SneakLeft1` / `_SneakRight1` (crouch) and `Rifle_CrawlFwd` / `_CrawlBwd` / `_CrawlLeft` /
  `_CrawlRight` (prone), each unique in `b_basicassetsmp` [verified: antdump list]; `Rifle_SneakBwd` occurs twice, so
  the crouched backward move uses `Rifle_SneakBwdLeft45` / `_SneakBwdRight45` blended, or the stand clip (reported).
  A missing clip falls back to the stand clips, counted per stance.
* **Upper body**: additive aim (S3), per-soldier weapon (S3's `weapon_choice_index` reload clips per weapon, e.g.
  M16A4 41, AK-74M 33 [verified: s3.md]); firing: `Rifle_CrouchFire` (21 frames) and the `Rifle_Crawl*Fire` clips
  exist for crouch and prone, but no unique stand rifle fire controller [verified: antdump list], so standing fire
  is a 50 ms procedural kick on the spine and weapon joints [inferred] (tp may find the stand fire through a chooser,
  as S3 did for the reload); reload start from the reload sequence.
* **Death**: blend to the prone pose over 0.5 s, hide after 3 s; ragdolls are M2b [inferred].
* **3p weapon meshes** per kit item, posed by the rig's weapon joints (S3). The 3p muzzle transform per soldier is
  published to the FX triggers (§12.2) after `wait_and_publish`.
* Gate: S3 G1a (≤ 2.0 ms p50 and p99) still holds with these inputs, measured inside E14.

---

## 10. Bots: stationary targets and a firefight stub

### 10.1 Stationary targets (the M2a criterion's "bot targets")

`TargetBotSource` (a `CommandSource`) writes an idle command with a fixed pose (stand, crouch or prone) and facing.
Bots come from S1's bot fill (`add_local_soldier`), spawn in the main nav component (`spawn_in_main`) and stand
still. The checks place them where they need them with the dev request `SpawnTarget {slot, position, yaw, pose}`
(applied in the rules step, clears spawn protection) and read their server capsules with `TargetInfo` (wire: nt;
handling: sv). Helpers (server side, dev only): `find_range_lane` (wp), and `find_sky_lane(world, flight_m,
margin_m)` (new, sv): along the bounds' longest horizontal axis, start at the bounds' minimum on that axis + 10 m,
40 m above the bounds' top, heading along the axis; try lateral offsets across the bounds in 32 m steps until a ray
of 1,000 m is clear; accept only when the in-bounds length (to the far bound + `margin_m` = `world_margin`) is ≥
`flight_m` + 75 m (M16A3: 975 + 75 = 1,050 m). Returns the lane, its clear length and in-bounds length, or a
failure that makes E2's expiry row use wp's W2 world (§1).

The target bots and the firefight brains are stubs of M2b's bot behaviour only to the extent the rows need: no
navigation choice beyond S1's walkers, no cover, no objectives, no team play. M2b replaces both behind the same
`CommandSource` seam.

### 10.2 Firefight stub (only for E9, E13, E14; M2b replaces it)

`FirefightBrain` (a `CommandSource`, serial on the server thread like S1's brains; it reads enemy positions, its
own magazine and weapon through sv's read-only `ServerView`, §4.2):
* **Pairs mode** (the budget scenario): 32 pairs of opposing bots along clear lanes 20–40 m long in front of an
  **observer pose**, so every bot is in view and firing (worst case for FX and audio). M1's `DebugKind::SpawnAt` is a
  spawn-point index only and comes from clients, so it cannot place server-side bots: sv adds a server-side
  placement call, `GameServer::place_soldier(slot, position, yaw, pose)` (the same code that handles `SpawnTarget`;
  applied in the rules step, clears spawn protection), and the brains use it at (re)spawn. The observer pose is
  `FirefightConfig::observer {position, yaw}`, given at server start: gx's `m2a_check --m2a-firefight` passes the
  fixed camera spot of `m2a_metro.tscn` (the client's free camera is placed there too); headless runs
  (`host.gunplay.perf`, `bf3dump firefight`) default to the start of Metro's range lane (spawn 87, §1). The layout is
  computed once from the observer pose: 32 lanes in a fan of ±35° around the view direction, 15–60 m out, each lane's
  ends checked for a clear eye-to-chest ray and a nav-mesh foothold, rejected lanes retried with a seeded offset.
* **Free mode**: S1's bot fill at TDM spawns; a target is the nearest enemy with a clear eye-to-chest ray (checked
  every 30 ticks, staggered by slot); with none for 2 s, the bot fires along its longest clear heading, so muzzle,
  tracers, impacts and decals still happen.
* Aim: the direction to the target's chest plus a seeded error (σ 1.5° decaying to 0.5° over 1 s on target)
  [inferred]; bursts of 3–6 rounds with 0.3–0.6 s pauses, semi-automatic weapons at 2 Hz; reload at an empty
  magazine or with no target below half; respawn at its pair position after death.
* Everything seeded from `match_seed` and the slot (§4.4), so the firefight is reproducible (E15).

---

## 11. Client presentation: viewmodel, ADS, HUD

### 11.1 1p viewmodel (`bf3_render::viewmodel_set.h`, `bf3_anim::viewmodel_anim.h`, `Bf3Viewmodel`)

* **Loading**: M9's `load_viewmodel` generalised to the local kit: the team's 1p arms (US
  `arms1p_bareglove03_mesh` today, RU arms from the RU appearance [inferred]), each item's 1p mesh on
  `weaponske01`, materials and textures, and the item's Ant clips: Deploy, Idle, Fire / FireSingle, Reload and
  ReloadEmpty, Bolt / Pump, ZoomIn / ZoomOut / ZoomIdle, Throw (M67), Knife. Clip names are resolved per package by
  `bf3dump vm clips <item>` (vm adds it). VBR clips (146 MP clips, mostly pistol and launcher zoom [verified:
  research §3.3]) use the Frame `IdlePose` → `ZoomPose` stand-in over the zoom time; a missing clip gets a
  procedural stand-in (deploy: raise from below over `DeployTime`; fire: kick). Every stand-in is counted (E11).
  Kit changes load on the loader thread before the next spawn.
* **Animator** (Godot-free): inputs per display frame are the predicted weapon phase and timers (prev / curr +
  fraction), the drained OwnTimed and OwnShot events, the ADS level, horizontal speed, grounded and sprint. Layers:
  base (idle, sprint), action (deploy, fire, reload, bolt, throw, knife) with 0.1 s crossfades, ADS blend.
  Output: local pose of the 1p skeleton (one evaluation per frame, ≈ 20 µs [inferred]).
* **Reload timing** (E5): the clip starts at the predicted `kReloadStarted` time and plays at its authored rate;
  the weapon is ready at `ReloadTime` from the sim, and a shot after that cuts the clip with a 0.1 s blend (the
  M9's empty reload clip is 2.167 s long against a 1.60 s `ReloadTime` [verified: integ README], so clip end ≠
  ready; §16 Q4).
* **Springs** (presentation only, `viewmodel_tuning.cfg`): sway (view-rate lag), bob (move speed), and the recoil
  kick (BF3 `CameraRecoilData` spring on the camera plus a weapon-local kick). IW4L's `weapon_iw4` view_bob, sway
  and kick were studied for the spring forms; the constants are tuned by the user's feel (§17).
* **Weapon switch**: the wanted slot is a command level (§5.1); the viewmodel plays the new item's deploy at
  `kDeployStarted`.

### 11.2 ADS camera and FOV

* World FOV: `ZoomLevelData.fieldOfView` hip 55, iron sights 40 (fast iron 40, 2× 32, 3.4× 20, 4× 17.2 …)
  [verified data: topics/01 §1.4], treated as vertical (Godot KEEP_HEIGHT) like `RenderFov` [inferred, §16 Q5].
* Viewmodel FOV: `RenderFov` 55 at hip (M9 viewmodel); optics' `zoomRenderFov` when zoomed.
* Transition: the aiming controller's zoom-in time (iron 0.09 s) and zoom-out 0.15 s; FOV over the controller's
  per-transition FOV time where it lists one (iron 0 → 1 0.12 s, 1 → 0 0.15 s [verified data, m2a-vm.md §2.3]), else
  `fovTransitionTime` 0.15 s. Mouse sensitivity is scaled by `lookSpeedMul` (0.5 iron).
* Recon default optics: the kit's default aiming controller zoom level with a procedural vignette stand-in; scope
  filters are M7 (§16 Q6).
* The camera adds the predicted recoil offset (interpolated) and the presentation kick.
* Measured by E19: the FOV curve per display frame, the recoil offset against the predicted one, the springs'
  settling, and the zoom block during deploy. vm's `--vm-check` scene drives the same rows on a scripted weapon
  timeline before the game wiring exists.

### 11.3 Weapon and impact cues for the HUD

The HUD reads only `HudModel` (ui2). Nothing in the HUD reads the sim directly.

### 11.4 HUD (`bf3_host::fill_hud`, `Bf3Hud`)

```cpp
namespace bf3::host {
struct HudStatic { /* per def: name_sid_hash, item_id, clip_capacity, fire_modes, crosshair kind */ };
void fill_hud(const PresentationFrame &, double own_fraction, std::span<const PresentEvent> drained,
		const HudStatic &, core::ui::HudModel &r_model);   // pure, unit-tested (ui.hud.feed.unit)
}
```

* **Fed fields** (ui2's M2a-tagged sources [verified: hud_model.h]): `active_weapon_ammo`, `_clips_left`,
  `_clip_capacity`, `_is_reloading`, `_reload_percent`, `_critical_ammo`, `active_weapon_name`,
  `active_weapon_data` (item id → `_small` icon through `ItemIconIndex`), `dispersion` (degrees from the
  interpolated predicted spread), `zoomed`, `firing_mode`, `show_crosshair`, `show_hitindicator` /
  `hitindicator_level`, soldier `pose` and health.
* **Controls** on the ui1/ui2 groundwork (route A+): `Bf3HudAmmo` (digits in `$HUD_medium` at Ammo_01, scaled by
  min(w/1280, h/720)), `Bf3Crosshair` (procedural; gap = (h/2)·tan(spread)/tan(vfov/2); hidden in ADS),
  `Bf3HitIndicator` (Hitindicator_01 at the centre; shown 0.3 s per server hit, level 1 [inferred, §16 Q7]),
  `Bf3WeaponName` (localised through `Bf3Translation`, icon from the SWF atlas region), health number at Health_01.
  The damage-direction indicator (DamageIndicator_01) is reported only.
* Shots of the HUD go to `b\shots\m2a\` for the user's judgement.

---

## 12. Effects and audio

### 12.1 VFX2: renderer (`bf3_fx::RenderPrep`, `Bf3FxRenderer`)

* **Atlas**: Metro's `emittersystem_win32` base 4096² DXT5 sRGB 13 mips and normal 1024 × 2048 NormalDXT5RGA → BC5,
  uploaded with `render_bridge::make_texture` from data prepared on the loader (≈ 21 MiB + 3 MiB VRAM [inferred]).
* **Batches**: `RenderPrep` turns `FxWorld::outputs()` and `particles()` into one instance stream per (shader class
  SAQ / DAQ / WAQ / mesh × blend alpha / additive / premultiplied × 1p or world pass); one MultiMesh each (≤ 24),
  instance layout 80 B (transform 12 floats, colour 4, custom 4: frame, alpha levels, soft fade), uploaded with
  `multimesh_set_buffer` (16k particles ≈ 1.25 MiB per frame).
* **Sorting**: transparent batches back-to-front by emitter depth; particles keep simulation order inside an emitter
  [inferred, §16 Q10]; additive needs no sort.
* **Shaders**: flipbook from `EmitterAtlas::frame_rect`, alpha levels and exponent, soft fade from the depth texture,
  clip scale; lighting blends per fx §12. Half-res stays deferred (reported).
* **Light pool**: ≤ 64 `OmniLight3D` RIDs, no shadows; the lights of one `EmitterOutput` merge into one (summed
  intensity, intensity-weighted centre, largest radius); priority by intensity over distance²; the 65th is dropped
  and counted (E9: ≤ 64).
* **1p pass**: 1p effects (muzzle) draw with the viewmodel's FOV override and z-clip scale, so they sit on the
  weapon.
* `FxWorld` stepping: `run_items` on the Godot pool (§3).

### 12.2 VFX3: triggers (`bf3_fx::triggers.h`, `Bf3GameFx`)

| event | effect |
|---|---|
| OwnShot | the item's `FireEffects1p` at the 1p muzzle joint, in the 1p pass; shell eject (one per firearm shot, E8 (7)); tracer when the shot's `tracer` flag is set (converging from the muzzle onto `aim` over 30 m, wp §7.3) |
| RemoteShot (render timeline, §8.3) | `FireEffects3p` at the 3p muzzle transform from the crowd (§9); tracer from the message's `aim` |
| Impact (static) | `ImpactTable::surface_material(triangle material)` → `select(bullet material, surface)` → `ImpactCell`: one of `effects` by seed, spread and mirror, `decal`, `impact_sound` (§12.3) |
| SoldierContact (own) / Hit | the soldier cell (`fx_impact_soldier_body_s`, spread 90°, no decal [verified: fx G5]) |
| Detonation (the server's `DetonationWire`; own and remote) | `fx_grenade_frag_01` at the server's position + the surface's impact + scorch decal (item 9, reported) |

* **Decals**: a pool of ≤ 256 Godot `Decal`s, oldest replaced, fade after 30 s [inferred].
* **Budgets**: `MaxInstanceCount` per def (fx §6.7), effect `CullDistance`, and the world's admission limit; an
  impact further than its cull distance is not started.
* Seeds from event ids (§4.4); `FxEvent` SoundStart / SoundStop are forwarded to the audio router.

### 12.3 Audio in the game (`bf3_audio::WeaponAudioRouter`, `Bf3GameAudio`)

* `Bf3GameAudio` owns one `MixerCore`, one `Bf3AudioStream` and one `AudioStreamPlayer` (au §7.2); the wave bank and
  the kit items' programs load on the loader thread inside the level load (E16 re-measures the load).
  `project.godot` has `audio/driver/mix_rate` 48000 (au).
* **Weapons**: one `WeaponSoundDriver` per soldier on its active weapon's patch: the own soldier with
  `IsFirstPerson` 1 at the camera, every other soldier with 0 at its 3p muzzle. Trigger down, rounds and trigger up
  come from the own predicted OwnShot / trigger events (exact sub-tick round times) and, for the others, from the
  firing bit and burst sequence of their records (state, so a lost shot datagram does not cut a burst's sound).
  **Rule fixed by au3** (docs/design/m2a-au3.md "Bursts"): sv's `burst_count` rises once per tick with shots after a
  tick without, i.e. about once per shot for an automatic weapon whose interval spans more than one tick, so "a
  burst_seq change = a new burst" does not hold. As built, an other's RemoteShot (it arrives about the render delay
  early) starts the burst on its exact frame and places each round by its shot index; the records' firing bit and
  burst_seq only keep the burst alive and start it when every RemoteShot of it was lost; a burst_seq change never
  starts a burst.
* **Clock**: `SimMixerClock` maps presentation time to mixer frames from the stream's played-frame counter; commands
  are scheduled `schedule_lead_blocks` (2 = 21.3 ms) ahead [verified: au].
* **Impacts**: `ImpactCell::impact_sound` and effect sound components (`FxEvent`) start one-shot `PatchInstance`s
  seeded from the event (au §7.2: no driver needed).
* **Voice limits until A3** (M2b brings HDR, scopes and the 128-voice cap): at most 32 weapon instances (nearest
  first; a culled one stops at its next trigger up) and 64 one-shots (oldest stolen) [inferred, §16 Q9]. au's
  `MixerConfig::max_instances` defaults to 64 [verified: mixer_core.h], below 32 + 64, so `Bf3GameAudio` creates its
  mixer with `max_instances = 96` (a config value; no au code changes) and the router enforces the two sub-limits
  before the mixer's own cap is reached, so the mixer never refuses an instance (counted, gated 0 in E7). The mix
  cost of 96 instances on the audio thread is covered by E14's 0-underrun row. AreaType stays 0 (A4, M3).
* **Knife and M67**: no `WeaponSoundDriver` loop; their swing, throw and explosion patches start as one-shot
  `PatchInstance`s from OwnShot / RemoteShot / Detonation (E7 item 1 lists the form per item).
* **Client-local audition** for E7 item 4: `Bf3GameAudio::audition(patch, position)` starts a one-shot of any
  compiled patch at a world position; only `m2a_check` calls it (never the server, never the wire). **Rule fixed by
  au3:** the M1A2 patch is not on Metro's mount, so the load step compiles an E7 audition patch the level lacks from
  `audio::kAuditionSuperbundles` (MP_001, which holds it and its waves) with a bank of its own; an audition step
  that still cannot run is skipped through `GameAudioCheck::skip_step` and fails E7 (never a silent pass).
* Listener = the active camera (1p or free).

---

## 13. Caching, memory, determinism, hostile input

### 13.1 What is cached (user cache root only; nothing written to the install)

| item | cache | key |
|---|---|---|
| weapon table | wp §8 | as wp |
| capsule pose table | new `capsules/` entry | rig, clip and capsule list hashes + `kCapsuleTableVersion` |
| kit viewmodel meshes and textures, 3p weapon meshes | existing mesh / texture cache (`load_mesh_cached`) | existing keys |
| FX atlas prepared pixels | texture cache | atlas partition hash + converter version |
| compiled effects | fx effect cache | as fx |
| sound programs, wave index | au audio cache | as au |

### 13.2 Memory (resident additions, [inferred] unless marked)

| item | size |
|---|---|
| wave bank, Metro TDM | 265.4 MiB [verified: au G6], budget 384 MiB |
| FX atlas (VRAM) | ≈ 24 MiB |
| particles (CPU instance buffers, double) | ≈ 2.5 MiB |
| kit viewmodel set (7 items + arms) | ≈ 40–80 MiB, reported by E16 |
| 3p weapon meshes (14) | ≈ 30 MiB, reported |
| capsule pose table | 0.47 MiB (both ends) |
| server: rewind pose cache (2,048 posed targets) | ≈ 1.2 MiB |
| server: weapon states 64 × 600 B, lag history (shared records 72 KiB, per-client sent rings and acks 96 KiB), shot-view rings 128 KiB | < 0.4 MiB |
| client: prediction ring with weapons (M1's 128 entries) | ≈ 77 KiB more |
| client: `ReceivedRecords` (32 seqs × 64 slots × ≈ 48 B, §6.3) | ≈ 96 KiB |

### 13.3 Determinism

* The server tick is deterministic for a given seed and command stream: slot order everywhere, damage sorted by
  (time, shooter, shot, pellet), the provider's tie rule (earliest hit, lower index), seeded bots (E15).
* Prediction uses the same `bf3_sim` code, the same cfg-built `Mechanics` (hash-checked) and the same capsule table
  (hash-checked).
* Lag compensation uses one sampler on identical quantised samples (E12's bit-equality row).
* Cosmetics agree across clients by seed, not by bits (fx §13).

### 13.4 Hostile input (the server never trusts a client)

| input | rule |
|---|---|
| wanted slot 9–15 or an empty slot | ignored, counted |
| fire-mode counter | at most 3 steps per command (mod-4 aliasing, wp §5.2) |
| `view_back_q8` beyond `max_rewind_ticks` | clamped, counted; negative after decode → Refused, tested at the present |
| `view_seq8` that resolves to no seq in the client's sent ring (newer than the newest sent, or more than 32 back) | Refused, tested at the present, counted |
| `snapshot_ack_bits` / `others_ack_bits` claiming a seq or part never sent to that client | the bit is ignored and counted (the per-client sent ring knows what was sent) |
| view quality Held or Extrapolated beyond 250 ms | Refused, present |
| fire while dead, sprinting or deploying | the sim refuses it (wp); counted |
| `KitSelect` kit ≥ 4 or team > 2, or a team change without `dev_commands` | refused, counted; a valid one applies at the next spawn only |
| `fire_presses` | never on the wire: a client sends levels only (the counter is resolved on the client thread, §5.1) |
| command rate | M1's jitter-buffer rules |
| dev requests (`SpawnTarget`, `TargetInfo`, `Trace`, …) and the `--exact-aim` match option | only with `dev_commands`; effect starts and audio auditions are client-local and never reach the server |
| server → client decoding | counts bounded (shots ≤ 128 per datagram, others ≤ 127), defs < table size, finite floats; a malformed message is refused and counted |

---

## 14. Budgets

### 14.1 Server tick (60 Hz, 16.7 ms)

| step | budget | basis |
|---|---|---|
| M1 motor + S1 brains, 64 bots | ≈ 1.5 ms p99 | S1 G1 1.51 ms [verified] |
| weapons, 64 shooters | ≤ 0.3 ms | W11 p99 0.205 ms [verified] |
| projectiles + rewind | ≤ 0.5 ms | §6.7 (rewind alone ≤ 0.26 ms in the worst bench row) |
| rules, history, encode | ≤ 0.5 ms | [inferred] |
| **gate** | p99 ≤ 8.0 ms (E13) | S1's gate |

### 14.2 Client frame (main thread), the firefight at 2560 × 1440

| work | p99 budget |
|---|---|
| crowd step (S3 G1a) | ≤ 2.0 ms |
| FX (E9) | ≤ 1.0 ms |
| viewmodel, HUD, triggers, audio router | ≤ 1.0 ms together (inside the 3.0 ms of E14 with FX) |
| frame | ≤ 16.7 ms (E14) |
| GPU: FX | ≤ 2.0 ms (E9) |

S3's G2 already failed on the noisy machine with the level alone (p99 14.9–18.4 ms). The levers if E14 misses on a
quiet machine: crowd shadows off, S3 backend C, temporal LOD on (the gate run keeps it off), half-res particles,
fewer decals.

---

## 15. Implementation waves and tracks

Two waves of parallel git worktrees. Wave 2 starts on a `main` with the spikes, the UI groundwork and wave 1
merged. Each wave starts with a **contract commit** by the integrator (W2.0, W3.0): every header named in that
wave's interface column, with declarations only, plus `m2a.contract` (wave 2) or `m2a.contract3` (wave 3), a ctest
that compiles them together and checks POD sizes, enum values and hashes. Tracks then fill in the bodies. W2.0 also
carries the declarations of the two additive edits to wp files (`WeaponTableOptions::exact_aim` /
`WeaponTable::exact_aim`; `TargetQuery`'s segment fields with their defaults), the new `bf3/sim/weapons/weapon_rules.h`
(`step_weapons_m2a`, §5.2), `bf3/net/other_sample.h` (`TimedOther`, `OtherPose`, `sample_other`, `rewind_time_q8`,
`rebuild_track`, §6.1), the additive fields of M1 structs (`PresentedSoldier::target_pose` / `transition`,
`PresentationFrame::newest_seq`, `ClientSessionConfig::mechanics_cfg`, the `Command` view sample) and
`GameServer::place_soldier`, so every track compiles against them from the start; the bodies are the owners'.
`m2a.contract` pins: the sizes of `Command`, `ShotView`, `PresentEvent`, `OtherPose`; the two ack-bit conventions of
§7.2 (a table of (newest, bits) → acked seqs, run against nt's encoder once it lands); `weapon_buttons::kAll` inside
the wire mask; the `exact_aim` hash change and its field-for-field equality with the range post-process (§4.2); the
three `step_weapons_m2a` rows (§5.2).

**Shared files are edited additively only.** Each track appends one block headed `# M2a <key>` (CMake) or
`// M2a <key>` (C++) at the end of: each library's `CMakeLists.txt`, `tests/CMakeLists.txt`,
`tools/bf3dump/CMakeLists.txt` and `bf3dump.cpp` (dispatcher), `src/bf3_godot/src/register_types.cpp`,
`bf3-opensource/project.godot`, `README.md`. Files owned by one track below are edited only by that track; where a
track adds to a file another track owns (named in its row), it adds a block at the end and never rewrites lines.
**Named insertions** (a call into the track's own new code is inserted at a named place inside an existing
function of a shared file; no existing line changes): sv in `game_server.cpp` at four places: the command receive
(`history.ack`, `ack_state` stored with a newly buffered command, §6.3), M1's per-soldier loop right after the motor
step (`gunplay->step_soldier`, §4.1 4a), the rules step (rules, dev kinds, `place_soldier`), and `send_snapshots`
(`history.record_tick` / `history.sent`, the own weapons block, `SnapshotOthers`, the shot and combat datagrams); cl in
`prediction.cpp` inside `catch_up`'s per-tick loop right after the motor step (`step_weapons_m2a`, §8.2), and in
`client_session.cpp` in `handle_datagram` (`SnapshotOthers`, `ReceivedRecords`, shot and combat decode) and
`publish` (weapon views, `newest_seq`). Corrected by cl (the list proved incomplete; m2a-cl.md "insertions"): the
client also needs one-line calls in `authoritative` (the spawn's weapons, the ring-miss and reconcile rebase from the
wire view, the weapons compare and R2's health rebase in the match branch), in `client_tick` (the view sample,
`fire_presses` and the received state after `sample_command`, the weapons after the motor step, `others_ack_bits`)
and in `start` (Ready leaves `start` for MatchInfo, so a refused client never sends it, E6), plus one member block
inside `ClientSession::Impl` and `PredictionRing::attach_weapons` (the lane `catch_up` replays). Corrected at the
wave-2 integration (sv's four places proved too few; m2a-sv.md "Decisions and deviations"): `game_server.cpp` takes
eight tagged insertions, each line marked `// M2a sv [tag]` (`[init]`, `[recv]`, `[loop]`, `[rules]`, `[snap]`,
`[ctrl]`, `[trace]`, `[stats]`), plus one-line `[impl]` declarations inside `GameServer::Impl`, and `game_server.h` /
`level_load.{h,cpp}` take field insertions inside their structs (a member cannot go in an end block); all additive,
no existing line changed. cl's dev-only `GunplayObserver::wants_rewind_rows` / `rewind_rows` (the E12 (b) server
source, commit 7ea1331) are lines marked `// M2a cl` inside sv's `gunplay.{h,cpp}`, accepted at the integration.

**Named replacements** (lines are replaced, by the owner named, and nowhere else):

| # | owner | file | what |
|---|---|---|---|
| R1 | cl | client_session.cpp | `present_others`' Hermite body → the call to lc's `net::sample_other` (§6.1) |
| R2 | cl | client_session.cpp | the health split of `states_match` (§8.2) |
| R3 | sv | weapon_sim.cpp, weapon_sim_def.cpp | the `exact_aim` branch where `step_soldier_weapons` fills a `ShotEvent`, and the hash line in `weapon_table_hash` (§4.2) |
| R4 | lc | ballistics.cpp | filling the new `TargetQuery` fields at `ProjectileWorld`'s query sites (§6.4) |
| R5 | tp (wave 3) | crowd_eval.cpp | `make()` and the skinning step pick the weapon binds, reload clip and `weapon_local` per soldier from `CrowdTeam::weapons` (§9; index 0 = today's per-team data) |
| R6 | gx (wave 4) | fx3's game_fx.cpp | `resolve()`: a partition holding an `EmitterDocument` counts as one (an emitter partition's primary instance is its `EmitterTemplateData`; E8 (4)'s oilfire was refused), commit b7652bc |
| R7 | gx (wave 4) | au3's tests/audio_router_tests.cpp | the test-local `m2a_fake::impact_sound_names` deleted for fx3's real `bf3::fx::impact_sound_names` (fx3 merged), commit 589a3b0 |

R6 and R7 were added at gx's verify fix (wave 4): the rule (R1–R5, decided before wave 2) proved too narrow for the
end-to-end step, whose in-game checks are the first to run the merged tracks together and find their seams; gx
named and committed each one separately in the other track's file, nothing else of fx3's or au3's changed.
Each insertion and replacement is a separate commit whose message names it. wp's W1–W11 must stay bit-equal after
R3 and R4 (they do not take the new branches: `exact_aim` is off and the `TargetQuery` fields are only read by the
new provider), and S3's G0 and `anim.s3.unit` rows must stay equal after R5 (one weapon per team). The two §5.2
rules are no edit of wp's files at all (new `weapon_rules.cpp`).

**Test files, one owner each**: `host_gunplay_server_tests.cpp` (sv: `host.gunplay.server`),
`host_gunplay_client_tests.cpp` (cl: `host.gunplay.client`), `host_gunplay_perf.cpp` (bt: `host.gunplay.perf`,
`host.gunplay.replay`). A shared fixture header `tests/gunplay_fixture.h` (headless `ServerHost` + `ClientSession`
on Metro, dev request helpers) is created in W2.0 and extended additively.

### Wave 2

| key | title | owns (creates / edits) | interfaces (headers) | depends on |
|---|---|---|---|---|
| sv | server gunplay | new `src/bf3_host/include/bf3/host/gunplay.h`, `kits.h`, `target_bots.h`, `server_view.h` with sources in `src/bf3_host/src/`; additive blocks and the four named insertions in `game_server.{h,cpp}` (config, the steps of §4.1, the `LagHistory` feeds (acks, `record_tick`, `sent`), `CombatEventSink`, `RespawnPolicy` (M1's fixed timer), `ServerView`, stats, handling of the dev kinds `SpawnTarget` / `TargetInfo` and the Trace additions, `KitSelect` kit and team, MatchInfo's `match_seed` / `weapon_hash` / `weapon_flags` / `kit_hash`) and `level_load.{h,cpp}` (`SharedLevel` weapon defs, kits, capsule table load); `command.h` view-sample fields; the `exact_aim` edit to wp's `weapon_sim_def.{h,cpp}` and `weapon_sim.cpp` (R3); `src/bf3_sim/src/weapons/mechanics_tuning.cpp`, `bf3-opensource/config/weapon_mechanics.cfg`; new `bf3/sim/weapons/weapon_rules.h` + `src/bf3_sim/src/weapons/weapon_rules.cpp` (`step_weapons_m2a`, §5.2); `GameServer::place_soldier`; the M1 tests that pin `Command`'s size or a command hash (updated in the commit that adds the view sample); `find_sky_lane`; tests `host_gunplay_server_tests.cpp` (cases timing, drop, stk, reload, variants, hostile: E1–E6 server side, E17 server side) | `bf3/host/gunplay.h`, `kits.h`, `target_bots.h`, `server_view.h`, `bf3/sim/command.h` (additions), `bf3/sim/weapons/mechanics_tuning.h`, `weapon_rules.h`, `weapon_sim_def.h` (`exact_aim`) | nt (encoders); lc for the rewind provider and capsules (until it lands: present-tick targets from wp's `standin_target`, a real symbol) |
| lc | lag compensation and hit capsules | `bf3/core/gameplay/capsule_pose_table.h` + cpp; `bf3/anim/server_pose.h` (with `ServerPoseClipNames`, §6.5) + `src/bf3_anim/src/server_pose.cpp`; `bf3/sim/weapons/posed_target.h` + cpp; the `TargetQuery` segment fields in wp's `hit_target.h` and their filling in `ballistics.cpp` (R4); `bf3/net/other_sample.h` + cpp (`sample_other`, `rewind_time_q8`, `rebuild_track`; written from present_others' body and the track rules of `add_others` / `publish`, cl switches the call); `bf3/host/lag_comp.h` + cpp (`LagHistory`: shared records, per-client sent rings and acks, shot views; `RewindProvider` with the swept-bound broad phase and the input-keyed pose cache); `bf3dump lagcomp` (`--perf` rows pairs / long, `--deviation`); tests `sim_lagcomp_tests.cpp` (`sim.lagcomp.unit`: two clients with different seqs and splits, lost and reordered datagrams of both kinds, server vs client rebuild, cache on vs off, window vs track), `anim_server_pose_tests.cpp` (`anim.server_pose`, E18 (a), with the off-grid reference evaluation) | the five headers; `hit_target.h` (additions) | — (S3 rig on main; the extra stance clips of §6.5 are in the retail package, verified) |
| nt | protocol v2 | `protocol.h` / `protocol.cpp` (MatchInfo fields, command view sample, `others_ack_bits`, others' weapon fields, `SnapshotOthers`, `ShotEvents` with full shot indices, aim / pellet_cone / power, `CombatEvents` with `DetonationWire`, `KitSelect` kit + team, Spawned kit + team, the `DebugKind`s `SpawnTarget` / `TargetInfo` and `DebugReport`'s `TargetInfoRow` and Trace additions), new `weapons_wire.{h,cpp}`; tests: block in `net_proto_tests.cpp`, new `net_proto_fuzz.cpp` (`net.proto.fuzz`, dev and asan; E17 wire side) | `bf3/net/protocol.h` (v2), `bf3/net/weapons_wire.h` | — |
| cl | client prediction and events | additive blocks in `client_session.{h,cpp}` (ring weapons, reconciliation by wire view, table build and refusal on MatchInfo, first-prediction emission, view sample and `fire_presses` from `InputSample`, `SnapshotOthers` merge and its ack bits, shot / combat decode, `late_sample` count) plus R1 and R2 and the named insertions (§15), `ReceivedRecords`, the per-command received state at first send, `ClientSessionConfig::mechanics_cfg`, the counters `late_sample` / `generation_reorder` / `others_ahead` / `track_timeout`, `PresentedSoldier::target_pose` / `transition` and the `window_extrapolated` counter in `present_others`; `prediction.{h,cpp}` (`catch_up` steps weapons through `step_weapons_m2a`, named insertion); new `client_projectiles.{h,cpp}` (own and remote worlds, §8.4), `presentation_events.{h,cpp}`, `presentation_weapon.h`; tests: block in `host_session_tests.cpp`, `host_gunplay_client_tests.cpp` (cases lagcomp, loopback, two clients: E12, E15) | `bf3/host/presentation_events.h`, `presentation_weapon.h`, `client_projectiles.h`, `client_session.h` (additions) | nt; lc (`sample_other`, `rewind_time_q8`, `posed_target`); sv phase 1 for its end-to-end cases (until then: a test-local fake server in `m2a_fake`) |
| vm | 1p viewmodel and ADS | `bf3/render/viewmodel_set.h` + cpp (generalises `load_viewmodel`; `viewmodel_assets.*` gains an additive overload), `bf3/anim/viewmodel_anim.h` + cpp, `src/bf3_godot/src/viewmodel.{h,cpp}` (`Bf3Viewmodel`), additive block in `fp_camera.{h,cpp}` (ADS FOV, recoil, kick); `bf3-opensource/config/viewmodel_tuning.cfg`; demo `scenes/m2a_viewmodel.tscn` (`--vm-check`: drives a scripted weapon timeline); `bf3dump vm clips`; tests `anim_viewmodel_tests.cpp` (`anim.viewmodel.unit`, E19 unit rows), `godot.m2a_viewmodel` | `viewmodel_set.h`, `viewmodel_anim.h`; reads `presentation_weapon.h` (W2.0) | — (codes against the contract types; cl provides real frames in wave 3) |
| fx2 | VFX2 renderer | `bf3/fx/render_prep.h` + cpp, `bf3/fx/atlas_pixels.h` + cpp (prepared atlas, cached), `src/bf3_godot/src/fx_renderer.{h,cpp}` (`Bf3FxRenderer`: MultiMeshes, light pool, 1p pass), `bf3-opensource/shaders/fx_*.gdshader`; demo `scenes/fx_demo.tscn` (synthetic starts on Metro); tests `fx_render_tests.cpp` (`fx.render.unit`: batching, sort, light merge), `godot.fx_demo`, `godot.fx_ref` (E9 item 8) | `render_prep.h`, `atlas_pixels.h` | — |
| hud | minimal HUD | `bf3/host/hud_feed.h` + cpp (`fill_hud`, `HudStatic` built from the weapon table and UI descriptions), `src/bf3_godot/src/hud_*.{h,cpp}` (`Bf3Hud`, ammo, crosshair, hit indicator, weapon name and icon, health); demo `scenes/m2a_hud.tscn` (`--hud-check`, scripted model); tests `ui_hud_feed_tests.cpp` (`ui.hud.feed.unit`), `godot.m2a_hud` | `hud_feed.h` | — (ui1, ui2 on main; contract types) |

### Wave 3 (starts when wave 2 is on main)

| key | title | owns (creates / edits) | interfaces (headers) | depends on |
|---|---|---|---|---|
| au3 | game audio | `bf3/audio/weapon_audio.h` + cpp (`WeaponAudioRouter`, `SimMixerClock`, voice limits, the two timelines of §8.3), `impact_audio.h` + cpp; `src/bf3_godot/src/game_audio.{h,cpp}` (`Bf3GameAudio`: mixer, stream, player, bank load step, client-local `audition`); new `bf3/audio/game_audio_check.h` + cpp (the in-game check: drives the recording schedule and calls au's public `analyse_check` from `audio_check.h`; no au file is edited); `MixerConfig::max_instances` = 96 in `Bf3GameAudio`; new `bf3/audio/kit_audio.h` + cpp and `bf3/audio/game_audio_frame.h` + cpp (added by au3, docs/design/m2a-au3.md: the kit items' sources and the Godot-free halves of `Bf3GameAudio` load / step / audition the offline rows run); tests `audio_router_tests.cpp` (`audio.router.unit`, `audio.game`, `audio.game.crowd`, `audio.game.underruns`) | `weapon_audio.h`, `impact_audio.h`, `game_audio.h` | cl (events, presented others) |
| fx3 | VFX3 triggers | `bf3/fx/triggers.h` + cpp (shot, impact, hit, detonation; seeds; cull; the two timelines), `src/bf3_godot/src/game_fx.{h,cpp}` (`Bf3GameFx`: FxWorld + renderer + triggers + decal pool, client-local `start_effect`), `fx_decals.{h,cpp}`; tests `fx_triggers_tests.cpp` (`fx.triggers.unit`, incl. case "late join" = E15's variant row on cl's recorded event streams) and the `fx.install` case "cells" (in-game cell selection, incl. the Caspian water fallback of E8 (5)) | `triggers.h`, `game_fx.h` | fx2, cl; tp for 3p muzzles (until then: muzzle at eye height + the weapon offset, a test-local fake) |
| tp | 3p soldiers | additive blocks in `crowd_eval.{h,cpp}`, `crowd_clips.{h,cpp}` (stance via lc's `ServerPoseClipNames`, sneak / crawl moves, `CrowdWeapon` and `CrowdTeam::weapons` (W3.0 declares them), fire, death, muzzle out) and R5; new `src/bf3_godot/src/game_crowd.{h,cpp}` (`Bf3GameCrowd`: present_others → inputs, 3p weapon meshes); tests: block in the S3 tests (`anim.s3.unit`: two weapons per team, and case "capsules" = E18 (b), the crouch and prone rows against `CrowdEvaluator`) | `crowd_eval.h` (additions), `game_crowd.h` | nt, cl, lc (clip names, the E18 reference) |
| bt | bots | new `src/bf3_host/include/bf3/host/firefight_brain.h` + `src/bf3_host/src/firefight_brain.cpp` (reads `ServerView`, places with sv's `place_soldier`, `FirefightConfig::observer`, §10.2); additive block in S1's bot fill (kit by slot, pairs layout); `bf3dump firefight` (headless run, `--perf`); tests `host_gunplay_perf.cpp` (`host.gunplay.perf`, RUN_SERIAL, E13; `host.gunplay.replay`, E15) | `firefight_brain.h` | sv |
| gx | Godot integration and gate runs | additive blocks in `level_session.{h,cpp}` (adds and feeds `Bf3Viewmodel`, `Bf3Hud`, `Bf3GameAudio`, `Bf3GameFx`, `Bf3GameCrowd`; loader steps; crowd lane rule of §3 after its re-measure), `input_mapper.{h,cpp}` (§5.1 keys incl. K / T, `fire_presses`, render time and `view_seq8` into `InputSample`); new `m2a_check.{h,cpp}` (`--m2a-check=range,reload,kits,hud,ads,audio,fx`, `--m2a-firefight`; the client-local effect start and audio audition); `scenes/m2a_metro.tscn`; `tests/godot_m2a.cmake`, `tests/godot_m2a_firefight.cmake`, the `godot.m1_load` run on `m2a_metro.tscn` (E16); README "M2a" | `m2a_check.h` | all wave-2 tracks; au3, fx3, tp, bt for their cases (each case is skipped with a reason until its track lands) |

### 15.1 Integration order and stubs

1. **W2.0** contract commit (integrator): the wave-2 headers and declarations listed at the top of §15,
   `tests/gunplay_fixture.h`, `m2a.contract` (the rows listed there).
2. **nt and lc** merge first: pure libraries with unit tests and no dependency on the other tracks.
3. **sv phase 1**: weapons on the server tick, kits and teams, deaths, the own-weapons block and others' weapon fields
   on the wire, stand-in capsules, `exact_aim`. Then **cl** lands prediction and events against it.
4. **sv phase 2**: lc's `RewindProvider` and `posed_target` replace the stand-ins; dev range requests; E1–E6, E12,
   E17 headless.
5. **vm, fx2, hud** merge whenever ready; they only touch their own files and demo scenes.
6. **W3.0** contract commit (`CrowdWeapon` / `CrowdTeam::weapons`, the new `CrowdSoldierInput` fields,
   `FirefightConfig`, `game_audio_check.h`, `triggers.h`, `game_fx.h`, `game_crowd.h`, `m2a_check.h`;
   `m2a.contract3`); **bt and tp** first (independent), then **au3 and fx3**, then **gx**, which wires
   everything into `m2a_metro.tscn` and owns the windowed gate runs (E1, E3–E5, E7–E11, E14, E16, E19 in Godot).
7. Gate pass on main by the integrator: every E row, with the noise policy of §1.

**Stubs.** As in M1 §15.1: a track that needs a part not yet merged uses a test-local fake in its own namespace
(`namespace m2a_fake`), never a definition of a real symbol, and deletes it when rebasing onto the real part. The
only production fallback is sv's use of wp's `standin_target` until lc lands, which is a real, already merged
function.

---
## 16. Open questions (recommended default in bold)

1. **Max rewind.** Plan D7 says samples ≤ 250 ms; "150/2" needs ≈ 270 ms. **350 ms (21 ticks), clamp and count
   beyond.**
2. **HitReports and refly.** **Defer to M3**; M2a's view-sample rewind on the client's received samples (§6.3)
   covers loopback, LAN and lost snapshots, and E12 measures it. Both Q1 and Q2 deviate from plan D7 and are the
   user's decisions before W2.0.
3. **Drawing all 64 others.** **A second `SnapshotOthers` datagram in M2a**; M3 replaces it with priorities and
   baselines. Alternative: cull to ≈ 49 nearest.
4. **1p reload clip vs `ReloadTime`.** **Authored rate; the weapon is ready at `ReloadTime`; firing cuts the clip with
   a 0.1 s blend.** Alternative: scale the clip to `ReloadTime`.
5. **FOV convention.** `ZoomLevelData` 55 / 40: **vertical (KEEP_HEIGHT), like `RenderFov`**; the user judges by feel.
6. **Recon optics.** **Default aiming controller zoom with a procedural vignette; scope filters in M7.**
7. **Hit indicator.** **Shown 0.3 s per server-confirmed hit, level 1; no client-predicted markers.**
8. **Own blood effect before confirmation.** **Yes, cosmetic, from the client's own test**; the HUD marker waits.
9. **Voices before A3.** **32 weapon instances (nearest) and 64 one-shots (oldest stolen).**
10. **FX sort.** **Emitter back-to-front; no per-particle sort** unless the user sees errors.
11. **Thread budget.** **Crowd lanes clamp(hc − 8, 2, 4) with a listen server**, so the server thread keeps a core;
    gx re-measures S3's quiet-machine gate with 4 lanes first, and if G1a's p99 exceeds 2.0 ms the rule is 5 lanes (§3).
12. **Mechanics until S5 Session 1.** **wp's defaults, `measured = false`, "unmeasured" in every report**; is M2a
    allowed to exit with them, the S5 rerun of E1–E6 counting as a follow-up?
13. **3p death.** **Blend to prone, hide after 3 s; ragdolls in M2b.**
14. **Inferred thresholds of E12 and E18.** **E18: head and chest ≤ 3 cm in the steady rows and ≤ 5 cm in the
    transition rows against the off-grid reference; legs and run reported only. E12: the control run without rewind
    ≤ 50 % hits** (a 4 m/s strafe moves ≈ 1 m in the ≈ 270 ms lag, wider than the chest). All three are design
    choices, not data; the user confirms them before W2.0.
15. **Budget scenario.** **Pairs in front of the observer** (worst case for FX and audio) rather than free TDM play.
16. **Gadget slots 2, 4, 5.** **Empty in M2a**, as the plan says; the kit data's gadgets come with M2b / M7.
17. **Plan §7.10 item 7's absolute GPU row.** "16k particles ≤ 0.3 ms GPU" is below the reference MultiMesh cost on
    this machine (1.05 ms). **Replace it by ≤ 1.25 × the reference row (E9)**, a deviation from the plan.
18. **Team change.** **Only with `dev_commands` in M2a** (E10, E11); M2b's TDM brings team balance and the real
    switch rules.
19. **Rewind time resolution.** **One target pose per projectile segment at its start time, 1/256 tick**, on both
    ends (§6.4). Alternative: pose at the segment's mid time (halves the ≤ 11 cm in-tick motion), at no extra cost.
20. **Remote shots on the render timeline.** **Yes** (§8.3): remote muzzle, tracer, impacts and deaths are delayed
    by the interpolation delay so they line up with the drawn soldiers; the own hit indicator and own damage are
    shown on arrival.
21. **Zoom while deploying or sprinting.** **Refused for the whole tick** (§5.2, `step_weapons_m2a`); a deploy that
    ends mid-tick zooms from the next tick. Alternative: accept the zoom from the deploy's end time inside the tick
    (needs an edit inside wp's step, which this design avoids).
22. **A `SnapshotOthers` newer than the newest Snapshot.** **Left out of the rewind set** (§7.2: `newest_snapshot`
    keeps M1's meaning; `others_ahead` counted). Alternative: a separate `newest_others` seq in the command packet
    (2 B more per packet) so such records count too.
23. **Swept bounds for the broad phase.** **AABB of the recorded positions over the rewind window** (§6.4).
    Alternative: a constant speed bound (cheaper to explain, misses falling and blast-thrown soldiers).

The recommended defaults (in bold above) were taken by the orchestrator on 2026-10-16 while the user was away; the
user may revise them.

---

## 17. What the user must judge

* **By ear** (audio has oracles, not taste): the M16A3 near / mid / far bands and the distant high-pass through the
  game mixer, the 1p vs 3p balance, the M98B, RPG-7 and M1A2 auditions, impact sounds, and the overall loudness of a
  64-bot firefight without HDR (A3 is M2b).
* **Feel**: recoil kick and recovery springs, sway and bob, the ADS transition and both FOVs, the crosshair and hit
  indicator, reload clip vs `ReloadTime`, deploy and switch timing, key mapping, and how shots feel under "150/2"
  (the delay of the hit indicator).
* **Look**: muzzle flashes, tracers, impacts and decals against footage; the 3p firing, reload, stance and death
  presentation; viewmodel placement per kit item; HUD screenshots.
* **Decisions**: the open questions of §16 (Q1, Q2, Q3, Q4, Q12 and Q17 change behaviour or the plan most; Q14 holds
  the inferred thresholds of E12's control row and E18's 3 cm / 5 cm gates); accepting 909 Metro waves against the
  plan's 908 (§1); whether a WARN on a noisy-machine budget row (E9, E13, E14) blocks the milestone; the mechanics
  variant once S5 Session 1 is measured.

---

## 18. Facts checked for this design

* Kits: `bf3dump kits MP_Subway TeamDeathMatch0` (wp build), 2026-10-16: the loadouts of §5.6, slots 0 / 1 / 2 / 4 /
  5 / 6 / 7.
* Weapon rows: `bf3dump weapons` (wp build): M16A4 800 RPM, 31 × 5, reload 1.8 / 2.37, v0 650, 25 → 18.4 over 8–50,
  g −15, deploy 0.67, head 2.0; MK11 260 RPM, 50 → 37.5 over 15–75, head 2.35 / chest 1.25 / legs 0.91; SVD the same
  multipliers; M9 400 RPM, 16 rounds, reload 1.35 / 1.6; M67 fuse 3.6 s, throw 18; knife v0 1.8, TTL 1.
* Names: `bf3dump text` (integ build): the 11 `ID_P_WNAME_*` strings of E10; `ID_P_WNAME_M67` absent; "M67 GRENADE"
  is string 0x26436abd.
* Headers: `command.h`, `protocol.h`, `game_server.h`, `client_session.h` (integ); `weapon_sim.h`, `hit_target.h`,
  `mechanics.h`, `ballistics.h`, `damage.h` (wp); `weapon_sound.h` (au); fx headers; `crowd_eval.h` (S3);
  `hud_model.h` (ui2); `viewmodel_assets.h` (integ).
* Gate tables: wp W1–W11, au G0–G8, fx G1–G11, S1 G1, S3 G1a / G1b / G2 (their design documents, results sections);
  S3's quiet-machine acceptance G1a p99 0.38 ms, G1b 1.82 ms, frame p99 10.4 ms (integ README).
* 3p stance clips: `antdump list --install b_basicassetsmp` (integ build), 2026-10-16: 2,444 objects; the controllers
  of §6.5 and §9 each occur once (`Rifle_SneakBwd` twice), all on rig VeniceAntSke01; transitions Dct with end
  frames 30 / 32 / 28 / 42 / 39 / 44; no unique stand rifle fire controller.
* Metro's static extent: `bf3dump collision MP_Subway TeamDeathMatch0` (wp build): 1,114,171 triangles in 170 tiles
  of 64 m, tile keys x −12..5, z −13..19.
* wp seams: `Mechanics` has no aim switch (exact aim is `RangeScenario::exact_aim`, a post-process in
  shooting_range.cpp); `WeaponTable::infinite_magazine` is hashed; `TargetQuery` has times and identity, no segment;
  `BallisticsLimits::world_margin` 200 m; the fuse is `spawn_time + time_to_live`; `next_shot_index` is never reset;
  `ShotEvent` carries `aim`, `pellet_cone`, `power`, `spawn_time`.
* M1 seams: MatchInfo counts a `motor_hash` mismatch only; Hello has no hash; every client is team 1; `states_match`
  compares health; the command packet carries `newest_snapshot` + `snapshot_ack_bits` (bit i = `newest_snapshot − 1
  − i`); `InputSample` has `jump_presses` and level `held_buttons`; the prediction ring defaults to 128;
  `SharedLevel` is the loaded level.
* M1 snapshots (integ `game_server.cpp` `send_snapshots`, `protocol.cpp`), checked for revision 3: `snapshot_seq` is
  a per-slot counter (`snap.seq = s.snapshot_seq++`); all clients' snapshots of a tick are built in one call with the
  same `server_tick`; the others are sorted nearest-first from each receiver and `encode_snapshot` keeps as many as
  fit in that order; `quantize_other(slot, state)` takes no receiver.
* M1 client track (integ `client_session.cpp`): `kTrackSamples` 8, `kTrackTimeoutNs` 1 s, a track reset on a
  generation change, reordered samples inserted by tick and duplicates skipped, `publish` copies a 3-sample window
  from the newest sample at or before the publish render time; `present_others` takes the bracketing pair, holds
  before the oldest, extrapolates ≤ `extrapolate_max_ms` past the newest, switches `pose` / `mode` / `flags` /
  `generation` at the newer sample; `PresentedSoldier` has no `target_pose` or `transition`.
* wp step (m2a/weapons `weapon_sim.cpp`): `make_weapon_context` sets `sway.zoom` from `kZoom` alone;
  `spawn_soldier_weapons` resets the state (every `mode_counter` 0) and takes no command; `step_soldier_weapons`
  counts `fire_mode_steps(s.mode_counter, counter)` and then writes the counter into every slot; the range's
  `exact_aim` post-process overrides `direction` and `pellet_cone` only; `make_env` removes projectiles outside the
  world box grown by `world_margin` on every axis.
* S3 (integ `crowd_eval.h`): `CrowdTeam` holds one `SkinBinds` list (body, head, helmet, weapon), one reload clip and
  one `weapon_local`; `CrowdSoldierInput` has position, velocity, yaw, pitch, camera distance, team and flags only.
* au (`mixer_core.h`): `MixerConfig::max_instances` 64; `audio_check.h` exposes `analyse_check`; G1 pins 909 waves,
  G4 the census (834 + 1 patches, 32,117 node instances).
* Caspian's static extent: `bf3dump collision MP_012 ConquestLarge0` (wp build): 1,357,710 triangles in 253 tiles,
  tile keys x −16..11, z −9..6 (64 m tiles).

---

## 19. Changes in revision 2 (after review of revision 1)

| finding | change |
|---|---|
| `exact_aim` had no home in `Mechanics`, no owner and no path to the client | it is a `WeaponTableOptions` / `WeaponTable` field next to `infinite_magazine`, hashed, applied inside `step_soldier_weapons`; sv owns the wp edit; the client builds its table from MatchInfo's `weapon_flags`; a match option, never toggled at runtime (§1, §4.2, §7.1) |
| E2's sky lane ignored `world_margin` | `find_sky_lane` starts at the bounds' edge along the longest axis and requires the whole flight inside bounds + 200 m; Metro's z extent (≥ 1,984 m, verified) fits it; fallback to wp's W2 world for the expiry row (§1, §10.1) |
| the capsule table needed stance clips S3 does not load | the crouch, prone and 6 transition controllers are named and verified unique in `b_basicassetsmp`; a per-row stand-in rule with tags; tp shares the names and the rule; new E18 (§6.5, §9) |
| E11 needed the player on both teams | `KitSelect {kit, team}`, `Spawned` kit + team, `--team=` / T; team change with `dev_commands` only (§5.6, §7.4) |
| the rewind cost model did not match `TargetQuery` | `TargetQuery` gains the segment (lc edits wp, named exception); broad phase on the segment; pose cache per (target, sample) shared across shooters; one time rule for both ends; estimate redone per bench row (§6.4, §6.6, §6.7, E13) |
| dev requests and trace additions had no owner; FxStart / Audition on the server | wire kinds are nt's, server handling sv's; effect start and audio audition are client-local in gx's `m2a_check` (§4.1, §7.4, §15) |
| hash refusal "at Hello" | the client refuses on MatchInfo with a reason, before Ready (§5.5, §7.1, E6) |
| 12-bit shot index broke late join | full index per shooter, then deltas; the "two clients" case joins late (§7.4, E15) |
| `ShotEvents` dropped `aim`, `pellet_cone`, `power` | carried when they matter; spawn time derived; the server's `DetonationWire` places grenades (§4.3, §7.4) |
| E12's client time rule unspecified; extrapolation mismatch | `net::rewind_time_q8` on both ends; the server samples only the client's acknowledged records (`view_seq8`, `others_ack_bits`); the reordering residual is counted (§6.3, §6.4, §8.4) |
| remote effects led the drawn shooter | render timeline for everything about other soldiers (§8.3, §8.4) |
| `sample_other` move broke "additive only" | named exceptions with owners (§15) |
| duplicate button constants | wp's `weapon_buttons::` reused (§5.1) |
| plan items without an E row | E18 (capsules), E19 (ADS, FOV, springs), E8 (7) (tracers, shells, impact sounds) |
| water on Metro unverified | `m2a_check` searches for it; Caspian fallback (E8 (5)) |
| E9 deviation implicit | stated in E9 and §16 Q17 |
| TDM seam for M2b | `CombatEventSink` and `RespawnPolicy` (§0, §4.2) |
| bandwidth above M3's exit | listed as an M3 input (§7.4) |
| E11 M67 timing basis; E7 "bind" for knife and M67 | from `ShotEvent::spawn_time`; one-shot patches for knife and M67 (E7, E11, §12.3) |
| one test file with three writers; E16 / E17 owners | split per track; E16 gx, E17 nt + sv (§1, §15) |
| health in `states_match`; brains without a world view; lost fire taps | health rebases without replay (§8.2); `ServerView` (§4.2); `fire_presses` (§5.1) |
| name drift; thread rule vs quiet numbers | `SharedLevel`, `include/bf3/host/`, ring 128 (≈ 77 KiB); re-measure S3's quiet gate with 4 lanes before the rule lands (§3, §13.2) |

---

## 20. Changes in revision 3 (after review of revision 2)

| finding | change |
|---|---|
| `LagHistory` keyed by one seq and one split, but M1 numbers snapshots per client and splits per receiver | records stored once per tick (receiver-independent); per client a sent ring {seq → tick, which slots in Snapshot / `SnapshotOthers`} and acks; `record_tick` / `sent` / `ack` take the client slot where it matters (§4.1 steps 1, 7, 8; §6.3; §6.6) |
| pose cache key ignored the received set | the key is (target, generation, time q8, `tick_a`, `tick_b`, `kind`), all inputs of `sample_other`; answers checked cache on vs off (§6.4) |
| server and client sample lists could differ | one `net::rebuild_track` on both ends over the same candidates: the server's from `LagHistory`, the client's from `ReceivedRecords`, both under the ack state of the command's first send / first arrival; the client's track rules (8 samples, generation, timeout, reorder) mirrored; the remaining differences named and counted (§6.1, §6.3, §8.4, E12) |
| the two §5.2 rules had no owner and touched wp | new `weapon_rules.{h,cpp}` (sv) with `step_weapons_m2a`; the server and `catch_up` call it; wp's files untouched, W1–W11 unaffected by construction; contract rows (§5.2, §15) |
| E18's reference did not exist in wave 2 | an off-grid reference evaluation through bf3_anim's layer functions in lc's test (all rows, wave 2) and the stand rows against `CrowdEvaluator`; crouch and prone against `CrowdEvaluator` rerun by tp in wave 3; transition rows gated at 5 cm (E18, §6.5) |
| ack conventions | both pinned in a table and in `m2a.contract`; `newest_snapshot` keeps M1's meaning; `SnapshotOthers` carries seq and `server_tick` (§7.2, §7.3) |
| "one non-additive edit" vs the exception list | §15 lists named insertions and replacements R1–R5 (adds sv's per-soldier-loop insertion, cl's `catch_up` insertion and tp's `CrowdTeam` weapon split); §6.1 refers to it |
| `sample_other` signature and discrete fields | takes the samples, render time, eye heights and the extrapolation limit; `target_pose` / `transition` switch with `pose`; `PresentedSoldier` gains them; the window vs track equivalence stated and counted (§6.1) |
| `exact_aim` set `aim` while the range does not | direction and pellet cone only; origin and aim as the sim computes; contract checks field-for-field equality with the range (§1, §4.2) |
| sky lane: shooter falls, flights after death, top margin | re-teleport per shot, fire in the third tick; flights and Trace continue after the shooter's death; the world box covers every axis (§1) |
| constant-speed broad phase | swept AABB over the recorded window (§6.4) |
| origin span; 908 / 909; census | Caspian's extent verified to fit 20-bit origins; 909 carried to the acceptance list; au G4 / G5 cover the census (§1, §7.4, E7) |
| header paths | the tree's `src/bf3_<module>/include/bf3/<module>/` convention (§2, §15) |
| inferred thresholds | E12's control and E18's gates shown in §16 Q14 and §17 |
| per-soldier 3p weapons | `CrowdWeapon` and `CrowdTeam::weapons` as a structural extension declared in W3.0, R5 (§9) |
| voice limits above `max_instances` 64 | `Bf3GameAudio` sets 96; the router enforces the sub-limits first (§12.3) |
| E15 variants in wave 2 | wave 2 compares seeds; fx3's `fx.triggers.unit` "late join" compares variants (E15) |
| E6 mismatch in one process | `ClientSessionConfig::mechanics_cfg` (E6) |
| firefight placement | sv's `GameServer::place_soldier`; `FirefightConfig::observer` from gx or the range lane (§10.2) |
| Trace size | per-tick additions bounded, ≤ ≈ 0.5 MiB (§1, §7.4) |
| au3 edited au's check | new `game_audio_check` calling `analyse_check` (§15) |
| `Command` grows | M1 tests that pin its size or hash change in sv's commit; the contract pins the new size (§6.2) |
