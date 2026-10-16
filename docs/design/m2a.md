# M2a design: infantry gunplay on Metro through the loopback server

Status: design for implementation, revision 2, 2026-10-16 (revision 1 reviewed; §19 lists what changed). Scope:
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
repeated here. Every criterion below runs **in the running game**: either headless through the real `ServerHost` and
`ClientSession` on Metro (`host.gunplay.*`), or windowed through Godot with the real input mapping
(`godot.m2a`, `godot.m2a_firefight`). Times are in `SimTime` units, 1/(kSimHz·10^6) s, so 75 ms = 4,500,000
[verified: sim_time.h].

**Measurement setup used by several rows.**
* **The range lane.** `find_range_lane` on Metro TDM gives the lane wp used: spawn 87, 96.9 m clear [verified:
  m2a-weapons.md §1 W3]. The check spawns stationary target bots on it with the dev request `SpawnTarget` (§10.1).
* **Exact aim.** A match option, not a runtime toggle: `GunplayConfig::exact_aim` (server flag `--exact-aim`, only
  with `dev_commands`) builds the weapon table with `WeaponTableOptions::exact_aim`, a new table field next to wp's
  `infinite_magazine` (§4.2). Inside `step_soldier_weapons` it sets each `ShotEvent`'s `direction` and `aim` to
  `aim_direction(cmd.yaw, cmd.pitch, {})` and `pellet_cone` to 0; the spread and recoil state still run. It is
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
  fall after the teleport does not matter. If `find_sky_lane` finds no lane, the expiry row falls back to wp's W2
  synthetic wall world (already passing) and the run reports it; the 100 / 300 m drop rows still need the sky lane.
* **Server trace.** M1's `DebugKind::Trace` (per-tick server states, dev only) gains the tick's `ShotEvent`s,
  projectile positions of the traced shooter, `DamageEvent`s, `DetonationEvent`s and deaths (the wire is nt's, the
  server side sv's, §15).
* **Noise policy** for every wall-clock row (E13, E14, E9): run under the quiet lock; on a failure repeat once; if it
  still fails, report WARN with the level-alone baseline row next to it (the S3 precedent). The user decides
  whether a WARN blocks the milestone.
* **Test binaries** (one owner each, §15): `host.gunplay.server` (sv: cases timing, drop, stk, reload, variants,
  hostile), `host.gunplay.client` (cl: lagcomp, loopback, two clients), `host.gunplay.replay` and
  `host.gunplay.perf` (bt), `godot.m2a` / `godot.m2a_firefight` (gx). All headless ones run the real `ServerHost` and
  `ClientSession` on Metro.

| # | criterion | automated measurement | threshold | § |
|---|---|---|---|---|
| E1 | fire timing (§7.1: 800 RPM = 75 ms) | `host.gunplay.server` case "timing" (headless) and `godot.m2a` case "range" (`--m2a-check=range`, input through `Bf3InputMapper`): M16A3 (`u_m16a4`) fire held from a full magazine; server trace and the client's predicted shots | 31 shots; first at the press tick's start; all 30 gaps exactly 4,500,000; predicted `ShotEvent`s equal the server's field for field (time, shot index, seed, direction bits) 31/31; one 1p muzzle start per predicted shot (31). Every other kit firearm: gaps equal the table interval as W1 defines it. Semi-automatic taps (MK11, M9) one display frame long: one shot per tap (the fire press counter, §5.1) | §5, §8 |
| E2 | ballistic drop (§7.1: 1.60 m at 300 m) | `host.gunplay.server` case "drop": sky lane (§1 setup; in-bounds length printed), single shots; drop = distance of the projectile's traced position from the launch line where its horizontal distance crosses 100 / 300 m (linear between tick positions) | 0.1779 ± 0.01 m at 100 m; 1.5981 ± 0.01 m at 300 m; expiry (`ImpactKind::Expired`, not an out-of-world removal) at 975.0 ± 0.01 m, or wp's W2 synthetic-world row when no sky lane holds the flight (reported); the client's cosmetic flight of the same shot within 1 mm of the server's at 300 m | §5.3 |
| E3 | shots to kill (§7.1 table) | `host.gunplay.server` case "stk" and `godot.m2a` case "range": range lane, server started with `exact_aim`, target bots standing at 5 m and 60 m, single fire at the zone's aim point until the server's death event | M16A3: chest 4 / head 2 at 5 m, chest 6 / head 3 at 60 m (exact); damage per hit at 5 m 25.000 / 50.000 / 22.750 (chest / head / legs) ± 1e-3; one death event after the last hit; HUD hit indicator shown once per server hit | §5, §11.4 |
| E4 | headshot ×2.0 / ×2.35 | same harness: M16A3 and MK11 (`u_mk11`, multipliers head 2.35 / chest 1.25 / legs 0.91 [verified: `bf3dump weapons`]) | M16A3 head 50.000 (×2.0); MK11 head 117.500 at 5 m (×2.35, kill in 1); MK11 head at 60 m kills in 2 (one-shot range 50.74 m, W3); ± 1e-3 | §5 |
| E5 | 1p reload timing vs `ReloadTime` | `host.gunplay.server` case "reload" (server and predicted phase events) and `godot.m2a` case "reload" (viewmodel and HUD, per display frame) | server = predicted: M16A3 tactical 108,000,000 (1.8 s), commit at +91,800,000 → 31; empty 142,200,000 (2.37 s) → 30; M9 81,000,000 / 96,000,000; viewmodel reload clip starts within 1 display frame of the predicted `kReloadStarted`; the 1p weapon can fire again, and the HUD ammo changes, within 1 display frame of the predicted finish and commit; the authored clip length per kit weapon is printed next to its `ReloadTime` | §7.3, §11 |
| E6 | disc / recovery variants (S5 Session 1 not measured yet) | `host.gunplay.server` case "variants": four runs, `weapon_mechanics.cfg` set to each of disc {UniformArea, UniformRadius} × recovery {Linear, Exponential}; plus one run with the client's cfg differing from the server's | four distinct `weapon_hash` values; the mismatched client disconnects on receiving MatchInfo with reason "weapon table mismatch" and `weapon_hash_mismatch` = 1 (it never sends Ready and never predicts); 0 weapon reconciliations over 600 ticks of mixed fire per matched run; 2,000 server shot offsets per run fit the variant's radial CDF (KS D ≤ 1.63/√2000 = 0.0364); the recoil offset after a 10-round burst follows the variant's closed form within 1e-4°. M2a ships UniformArea + Linear with `measured = false` (§5.5) | §5.5, §7.1 |
| E7 | audio §7.7 items 1–4 through the game mixer | `godot.m2a` case "audio": the game's own mixer (`Bf3GameAudio` in the level session, 48 kHz) records to WAV (`--audio-record`); a server bot fires M16A3 bursts while the free camera (the listener) holds at 5, 20, 100, 150, 280, 320 m; item 4 patches (M98B, RPG-7, M1A2) through the client-local audition (`Bf3GameAudio::audition(patch, position)`, driven by `m2a_check`; no server request) at their band distances; the recording is judged by au's `analyse_check` | item 1: every kit item's patch compiles strictly (12/12) and binds: for the 10 firearms `bind_weapon_sound` succeeds (start / stop / step inputs resolved); for the knife and the M67 the swing / throw / explosion one-shot patches start as `PatchInstance`s without a driver (the report lists which form each item uses); items 2–4: au's §10.2 rows PASS on the in-game recording (M16A3 close layer cosine 1 → 0 over 0–20 m, far layer per au reading R; pitch within U[1.21, 1.262] × LFO[0.96, 1.0]; distant HPF 17..200 m → 0..310 Hz, 140.6 ± 1 Hz at 100 m; M98B, RPG-7, M1A2 bands); `short_blocks` = `late_blocks` = `late_commands` = 0. Audible correctness: the user (§17) | §12.3 |
| E8 | VFX §7.10 items 1–6 in the game, plus VFX3's cues | `godot.m2a` case "fx" plus the existing `fx.install` / `fx.unit` | (1) every effect the kits, the impact table and Metro reference compiles, 0 failures (and `fx.install` census passes); (2) `fx.unit` polynomial rows pass; (3) over 1,000 predicted M16A3 shots the `_1p_flash` emitter is present in [0.71, 0.79] of them (p 0.75, 3σ) and the renderer's drawn instance count equals `FxWorld::particles()` every frame; (4) the oilfire emitter started on Metro by the client-local `Bf3GameFx::start_effect` (driven by `m2a_check`; no server request) draws 80 ± 1 live particles after 15 s; (5) in-game 5.56 hits on Metro concrete, brick and a soldier start exactly the effects and decals of fx's G5 named cells; water: `m2a_check` first searches Metro's collision tags for a triangle of physics material Water (18) reachable from a spawn; if there is none, the water cell is checked through the same trigger code headless on Caspian (`fx.install` case "cells"; Caspian's grid has the same Water cell, fx G5) and the report says "Metro: no water"; (6) the atlas uploads 52/52 entries, 0 default-texture fallbacks; (7) cues: tracer starts = predicted shots with `tracer` set (from the table's `trace_frequency`), one shell eject per firearm shot, one impact one-shot sound per static `Impact` event within its cull distance, 0 double starts after replays | §12.1, §12.2 |
| E9 | VFX §7.10 items 7–8: firefight budget | `godot.m2a_firefight` (windowed, 2560×1440, vsync off, Metro TDM, 64 firefight bots in pairs in front of the observer, §10.2, 60 s after 10 s warm-up); `godot.fx_ref` (item 8, the Godot reference workloads re-run on this machine) | FX main-thread time (FxWorld begin/end + RenderPrep + buffer upload + light pool) p99 ≤ 1.0 ms; FX GPU time ≤ 2.0 ms (p50 of FX-on minus p50 of FX-off frames in alternating 5 s windows, the S3 G1b method); effect lights ≤ 64 in every frame; live particles reported (≤ 16k expected); item 8 reported, and VFX2's 16k-particle GPU cost ≤ 1.25 × the reference MultiMesh 16k lit + soft row on the same machine. **Deviation from the plan:** §7.10 item 7 says "16k particles ≤ 0.3 ms GPU"; the reference row itself measured 1.05 ms on this machine, so no MultiMesh path can meet an absolute 0.3 ms; the relative row replaces it (§16 Q17) | §12, §14 |
| E10 | minimal HUD (route A+) | `godot.m2a` case "hud" (per frame, plus shots to `b\shots\m2a\hud_*.png`) and `ui.hud.feed.unit` (Godot-free `fill_hud`) | ammo widget equals the predicted magazine / reserve every frame (0 mismatches), drawn in `$HUD_medium` at `hudmpscreen` Ammo_01 (1194, 669) scaled by min(w/1280, h/720); the crosshair gap equals (h/2)·tan(spread)/tan(vfov/2) ± 0.5 px every frame and hides in ADS; one hit indicator per server-confirmed hit, shown within 1 frame of the Hit event; the weapon name equals the localised string for all 12 kit items, with each team's kits spawned (`KitSelect` team, §5.6) (M16A3, M4A1, M27 IAR, MK11 MOD 0, AK-74M, AKS-74u, RPK-74M, SVD, M9, MP443, KNIFE [verified: `bf3dump text`], M67 GRENADE through its UI description [verified string 0x26436abd; link inferred]); the `_small` icon resolves for 12/12 | §11.4 |
| E11 | 4 kits × (primary + pistol + knife + M67) | `godot.m2a` case "kits": for each of the 8 loadouts (2 teams × 4 kits, §5.6) send `KitSelect {kit, team}` and respawn, cycle slots 0 → 1 → 7 → 6 → 0, fire each item once (the knife at a target 1.5 m away, the M67 thrown), reload each firearm | 8/8 loadouts spawn on the requested team with the kit's items in slots 0, 1, 6, 7 (the `Spawned` kit and team equal the request); every item deploys, fires and (firearms) reloads, with server events = predicted events; the viewmodel has a deploy, fire and reload clip or a counted stand-in for every item (report lists stand-ins); the knife hits; the M67's `DetonationEvent` time minus its `ShotEvent::spawn_time` (release + `SpawnDelay`) = 3.6 s exactly (wp's fuse is `spawn_time + time_to_live`), and the release-to-detonation time is printed | §5.6, §11.1 |
| E12 | server-validated hits with lag compensation | `host.gunplay.client` case "lagcomp": netsim "150/2", a target bot strafing ± 4 m/s over 8 m at 20 m, the test client aiming at the presented chest each tick (server started with `exact_aim`), 500 single shots; control run with `max_rewind_ticks` 0; stress run "lat=150-165,loss=2"; plus `sim.lagcomp.unit` (synthetic) | server hit for ≥ 99 % of the shots the client's own cosmetic test hit (both ends use the time rule of §6.4), same zone for ≥ 99 % of those; control run hit rate ≤ 50 %; the server's rewound target sample equals the client's `sample_other` over the same received samples at the same time bit for bit (`sim.lagcomp.unit`, including lost and reordered snapshots); stress row, clamp counts and the `late_sample` residual (§6.3) reported | §6 |
| E13 | server tick budget with 64 firing bots | `host.gunplay.perf` (RUN_SERIAL; headless, Metro TDM, 64 firefight bots, 10 s warm-up, 120 s); `bf3dump lagcomp MP_Subway TeamDeathMatch0 --perf` (64 human shooters at the maximum lag: row "pairs" at 20–40 m, row "long" with every bullet flying its full 1.5 s above the world) | server tick p99 ≤ 8.0 ms (S1's gate); weapons + projectiles + rewind share printed; rewind provider p99 ≤ 0.5 ms per tick in both bench rows; queries, candidates and pose-cache hits per tick printed (§6.7) | §6.7, §14.1 |
| E14 | client frame budget in the firefight | `godot.m2a_firefight` (the E9 run) | frame p99 ≤ 16.7 ms at 2560×1440, vsync off; M2a main-thread additions (viewmodel, HUD, triggers, audio router, FX) p99 ≤ 3.0 ms, crowd step p99 ≤ 2.0 ms (S3 G1a); 0 audio underruns; 0 presentation-event overflows | §14.2 |
| E15 | determinism | `host.gunplay.replay` (two headless 3,600-tick firefights); `host.gunplay.client` cases "loopback" (10 min scripted play, no netsim) and "two clients" (the second `ClientSession` connects 30 s after the first, when every bot slot has fired) | equal per-tick world hashes (soldier states, weapon states, projectiles, combat events) for equal seeds, different for a different seed; 0 weapon and 0 motor reconciliations in loopback without netsim (health rebases, §8.2, counted separately and reported); both clients start the same effect variants (effect, seed) and the same sound seeds for every remote shot from the second client's join on | §13.3 |
| E16 | load time and memory with weapons | `godot.m1_load` run on the M2a scene (`res://scenes/m2a_metro.tscn`; gx) | M1's E4 / E5 still hold: Metro cold ≤ 15 s, warm ≤ 5 s, with the weapon table, wave bank, effect library and atlas, kit viewmodels, 3p weapons and the capsule pose table loaded; peak working set and VRAM printed | §13.2 |
| E17 | hostile input; install unchanged | `net.proto.fuzz` (nt; dev and asan) over every new message; `host.gunplay.server` case "hostile" (sv); every install test's `install.unchanged` row | every malformed message refused and counted, no crash; a client sending wanted slot 15, `view_back` 65,535, a `view_seq` outside the history, fire-mode counter spam, `KitSelect` team 7 and 4× command rate changes no other soldier, clamps are counted, server tick unaffected; layout.toc, cas.cat and every .sb / .toc unchanged | §13.4 |
| E18 | hit capsules from the server pose (R14) | `anim.server_pose` (install; lc): the capsule pose table against S3's full `CrowdEvaluator` pose of the same rig and clips; and `bf3dump lagcomp --deviation` | head and chest capsule centres within 3 cm of the full pose in the steady stance rows (stand, crouch, prone idle at all 33 pitch buckets); legs, run and transition fractions reported; every row tagged clip-derived or stand-in (§6.5), 0 stand-in rows expected on the retail install; the table hash is part of `weapon_hash` | §6.5 |
| E19 | ADS, viewmodel FOV and springs | `anim.viewmodel.unit` (vm; Godot-free) and `godot.m2a` case "ads" (per display frame) | world vertical FOV 55 at hip and 40 at iron sights, reached `fovTransitionTime` (0.15 s) ± 1 display frame after the ADS press, and back after the release; viewmodel FOV = `RenderFov` 55 at hip; the camera's recoil offset equals the interpolated predicted recoil every frame (± 1e-4°); sway, bob and kick springs come to rest within their configured settle time once the input stops; zoom refused while deploying (no FOV change) | §11.2 |

---

## 2. Module ownership

| library | M2a additions | rule |
|---|---|---|
| `bf3_core` | `gameplay/capsule_pose_table.h` (POD table of local capsule segments per stance, pitch and transition; hash) | Godot-free; no runtime state |
| `bf3_anim` | `server_pose.h`: builds the capsule pose table from the 3p rig, its stance clips and the additive aim (R14); `viewmodel_anim.h`: the 1p state machine, layers and springs; additive inputs on `crowd_eval.h` | Godot-free; the server links it (new `bf3_host → bf3_anim` link) |
| `bf3_sim` | `command.h` view-sample fields (the weapon button fields stay wp's `weapon_buttons::`); `weapons/posed_target.h` (capsule table + pose → `HitTarget`); `weapons/mechanics_tuning.h` (cfg parser); two additive edits to wp files: `WeaponTableOptions` / `WeaponTable::exact_aim` (hashed, applied in `step_soldier_weapons`; sv) and `TargetQuery`'s segment fields (filled by `ProjectileWorld`; lc); `prediction.h` steps weapons in `catch_up` | deterministic, no I/O except the cfg parser |
| `bf3_net` | protocol v2 messages (§7); `weapons_wire.h` (own-weapons wire view); `other_sample.h` (the Hermite sampler of one other soldier, shared by client and server) | the sampler is the one function both ends use for lag compensation |
| `bf3_host` | public headers in `include/bf3/host/`, sources in `src/`: `gunplay.h` (per-slot weapons, projectiles, damage, deaths, kits inside `GameServer`); `lag_comp.h` (history ring and `RewindProvider`); `kits.h`; `target_bots.h`, `firefight_brain.h`; `server_view.h` (read-only server world for brains); client side: `client_projectiles.h`, `presentation_events.h`, `presentation_weapon.h`, `hud_feed.h` | server parts are linked by the dedicated server too; nothing here links `bf3_fx` |
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
   acks (`snapshot_ack_bits`, the new `others_ack_bits`) into the slot's `ReceivedSet` (§6.3); `KitSelect` (kit and
   team) stored for the next spawn; dev requests queued.
2. Commands for this tick per slot (M1 §7.4).
3. Command sources write their command: target bots (§10.1), firefight brains (§10.2, reading `ServerView`), walkers.
4. Per live soldier, in slot order:
   * motor step (M1);
   * **4a** `ctx = make_weapon_context(motor, before, after, cmd)`, `step_soldier_weapons(table, weapons[slot], cmd,
     ctx, match_seed, out)`; each `out.shots[i]` → `projectiles.spawn`, and the command's view sample (lag and
     received set, §6.3) is stored in the shooter's shot-view ring under the shot index, so every shot of one command
     shares it; each shot is appended to every client's shot accumulator. A soldier dead before this tick steps no
     weapon; its projectiles already in flight keep flying.
5. **4b** `rewind.begin_tick(tick, present states)`, then `projectiles.step(tick, server_world, rewind, events)`
   once for all projectiles (wp §7.1).
6. **Rules**: `DamageEvent`s sorted by (time, shooter slot, shot index, pellet) and applied with `apply_damage`; a
   hit on a soldier already dead in this order is dropped; Hit, Death and Detonation events go to the
   `CombatEventSink` (§4.3); a death asks the `RespawnPolicy` when the slot respawns (M2a: M1's `respawn_ticks`); a
   (re)spawn calls `spawn_soldier_weapons` with the slot's kit and team and stamps the life; dev requests
   (`Teleport`, `SpawnAt`, `SpawnTarget`, `TargetInfo`) are applied here and stamped N. Effects and sounds are never
   server requests: the check's effect start and audio audition are client-local (§15, gx).
7. **Lag history**: at snapshot ticks, `history.record(seq, tick, records, part_of_slot)` for every live soldier,
   with which datagram (Snapshot or `SnapshotOthers`) carried each record. These are the same records the snapshots
   of this tick carry, so the client and the server hold identical samples.
8. Snapshots (§7.3), the shot datagram, the reliable combat events; ServerHost flushes ENet (M1 §7.5).

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
	bool exact_aim = false;           // ShotEvent direction = aim = view without recoil, pellet_cone 0 (E3, E4, E12)
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
`step_soldier_weapons` (weapon_sim.cpp), where it fills a `ShotEvent`, sets `direction = aim =
aim_direction(cmd.yaw, cmd.pitch, RecoilOffset{})` and `pellet_cone = 0` when the table has it. The spread and recoil
state still run, as in wp's range option. `m2a.contract` checks that the hash changes with the flag and that a W3
range run with the table flag gives the same directions as wp's `RangeScenario::exact_aim` post-process.

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
W6]. Two items that wp left for integration are done here:

* the zoom block during deploy and sprint (`Mechanics::sprint_blocks_fire` exists; zoom is refused while deploying)
  [inferred BF3 behaviour];
* `mode_counter` at spawn is set from the command's counter, so the first command after a spawn does not cycle the
  mode.

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
  (step 7 of §4.1). The client interpolates the same records.
* **Same function.** `net::sample_other(samples, render_time)` is the Hermite sampler that `present_others` uses
  today, moved into `bf3_net` so both ends call one function (positions Hermite with the sample velocities; yaw and
  pitch along the shortest arc; pose and mode switch at the newer sample; extrapolation ≤ 250 ms, then held). lc
  writes `other_sample.{h,cpp}` from `present_others`' body; cl replaces that body in client_session.cpp with the
  call. This is the one non-additive edit of a shared file in M2a, owned by cl and allowed explicitly (§15).
* **Same render time and same inputs.** Each command carries the render time of the frame the player saw when the
  input was sampled and the newest snapshot that frame used (§6.2). For a shot at time t from that command, the
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
	uint8_t view_seq8 = 0;      // low 8 bits of the newest snapshot seq the drawn frame's samples came from
};
}
```

The main thread puts the frame's render time (`render_ticks_at_publish` advanced to the frame, × 256) and the newest
snapshot seq of the `PresentationFrame` it drew into the `InputSample`; the client thread computes `view_back_q8`
when it samples the command. 26 bits per command on the wire. Bots send quality None (they fire at the present, D7).

### 6.3 How far back, what the client had, and what is clamped

Lag for a human ≈ RTT + command buffer + d [inferred]: loopback ≈ 0 + 33 + 75 ≈ 110 ms; "150/2" ≈ 150 + 33 + 85 ≈
270 ms; the one-way stress profile ≈ 420 ms. Plan D7 says "≤ 250 ms", which would clamp the gate profile. Default
`max_rewind_ticks` = 21 (350 ms) so "150/2" is fully compensated (§16 Q1). Beyond it the server tests at the
clamped time and counts `rewind_clamped`. A sample older than the history (24 snapshot samples = 800 ms) is never
needed under the clamp.

**Only the samples the client had.** After a lost snapshot (or a lost `SnapshotOthers`) the client interpolates
across the gap or extrapolates, while the server's history holds the missing record. If the server sampled from its
full history it would interpolate where the client extrapolated. So the server samples a shooter's targets only from
the records that client received: per client a `ReceivedSet` from the command packets' `snapshot_ack_bits`
(M1) and the new `others_ack_bits` (one bit per `SnapshotOthers` part, §7.2), limited to seqs ≤ the command's
`view_seq8` (resolved against the packet's `newest_snapshot` with `seq_newer`). The history knows which datagram
carried each record (§4.1 step 7). `sample_other` then runs on exactly the client's input. The one residual: a record
that arrived reordered after the frame was drawn but with a seq ≤ `view_seq8` counts as received although the frame
did not use it; the client counts such arrivals (`late_sample`) and E12 reports them.

Verdicts per shot, after IW4L's `ShotSampleProvenance` / history verdicts (studied, own code): `Present` (bot, or
quality None), `Rewound`, `Clamped`, `Refused` (malformed: lag < 0 after decode, `view_seq8` outside the history,
quality Held beyond 250 ms of extrapolation). Refused shots are tested at the present. All four are counted in
`LagCompStats`.

### 6.4 The target set at a rewound time

* **Time rule (both ends, one function).** A query for a projectile segment that starts at `time_start` tests the
  targets posed at `net::rewind_time_q8(time_start, view_back_q8)` = time_start in q8 ticks − view_back, clamped
  (§6.3) and rounded to 1/256 tick. One pose per segment (the segment is at most one tick; a target moves ≤ 11 cm in
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
  The provider tests the segment against each target's present bound sphere inflated by 6.5 m/s × lag + the
  segment radius (≈ 3 ns each, SoA) and poses only the candidates.
* **Pose cache shared across shooters.** Posed targets are cached per tick by (target slot, sample time q8) in an
  open-addressed table of 2,048 entries, cleared in `begin_tick` (when full, a pose is computed without being
  cached and counted; answers never depend on the cache); bots (lag 0) share one present set. The answer to
  a query is a scratch list of the candidates' `HitTarget`s in slot order, valid until the next call, as wp's
  contract requires; the old "last answer per (shooter, sample)" cache is dropped, because `ProjectileWorld` steps
  projectiles in spawn order with shooters interleaved and would thrash it.
* The shooter's view sample is stored per shot (ring of 128 (shot index → view_back, quality, received-set index)
  entries per slot), because a bullet flies for several ticks after the command that fired it.

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
  transitions are sampled at 8 fractions of their own length (the motor's transition fraction maps to the clip's
  normalised time).
* **Fallback, per row.** A row whose clip is missing, fails to bind or is not additive where required is built from
  wp's `standin_target` layout for that stance (transition rows: a lerp of the two stance rows by the fraction).
  Every row is tagged clip-derived or stand-in, and E18 prints the tags (0 stand-in rows expected).
* **One choice for both users.** tp (3p soldiers, wave 3) loads the same controller names through the same
  `ServerPoseClipNames` list (an additive extension of S3's `CrowdClipNames`) and uses the same stand-in rule, so a
  stance the server poses from a stand-in is also drawn from the stand clips, and the report says so.
* `bf3_anim::build_capsule_pose_table(rig, clips, capsules)` evaluates the 3p rig per stance row at its idle clip
  with its own additive aim (pitch weights from S3's 78.1° up / 60.7° down; the crouch and prone aim limits from the
  same LocalPost rule [inferred]) at 33 pitch buckets, and per transition at 8 fractions with the source stance's aim. For each
  entry it stores the 9 capsules' segments in the soldier's local frame (from `HitCapsule` bone, offset, length,
  axis, radius [verified: material_grid.h]; bones mapped to rig joints by name, as antdump `rigmap` does).
* Size: (3 × 33 + 6 × 8 × 33) entries × 9 capsules × 32 B ≈ 0.47 MiB; build ≈ 1,700 pose evaluations, a few tens of
  ms [inferred], cached (§13.1).
* `sim::weapons::posed_target(table, TargetPose)` lerps between the two nearest pitch buckets (and transition
  fractions), rotates by yaw and translates by the feet: O(1) per target, ≈ 0.3 µs for 9 capsules [inferred]. Both
  ends use it: the server for hits, the client for its cosmetic test (§8.4).
* The table's hash goes into `weapon_hash`, so a client with another table is refused.
* Not modelled: leg motion of locomotion (the legs stay in the idle stance), reload and fire upper-body motion.
  `anim.server_pose` (install, E18) reports the deviation of head and chest capsule centres from S3's full pose over
  idle, aim sweep and run; the gate is ≤ 3 cm for head and chest in the steady rows [inferred threshold], run, legs
  and transitions reported. Until this lands (§15), the server uses wp's `standin_target` layout.

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
struct LagCompConfig { uint32_t history_snapshots = 24; uint32_t max_rewind_ticks = 21; bool enabled = true; };
struct LagCompStats { uint64_t queries, candidates, pose_cache_hits, present, rewound, clamped, refused; double tick_ms_p99; };
struct ShotView { int32_t view_back_q8; uint8_t quality; uint16_t received_index; };   // per shot index
class LagHistory {
public:
	void record(uint16_t seq, uint32_t snapshot_tick, std::span<const net::OtherSoldier> records, std::span<const uint8_t> part_of_slot);
	void ack(uint8_t client_slot, uint16_t newest, uint32_t snapshot_ack_bits, uint32_t others_ack_bits);
	uint16_t received_set(uint8_t client_slot, uint8_t view_seq8);  // snapshot of the client's ReceivedSet
	// net::sample_other over the records of p_slot that the received set holds
	bool sample(uint8_t slot, uint8_t generation, int64_t time_q8, uint16_t received_index, net::OtherPose &r_pose) const;
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

Cost terms [inferred]: per query the broad phase over 63 present bounds ≈ 0.2 µs; per new (target, sample) pose
`sample_other` ≈ 40 ns + `posed_target` ≈ 0.3 µs; per candidate a copy into the answer (≈ 0.6 KB) ≈ 50 ns. Queries
per tick = live projectiles (one segment per projectile per tick, wp).

| bench row (64 human shooters at the maximum lag, M16A3 full auto, 13.3 rounds/s each) | live projectiles | candidates per query | per tick |
|---|---|---|---|
| "pairs": 32 opposing pairs 20–40 m apart (the firefight layout) | ≈ 64 × 13.3 × 0.05 s ≈ 45–100 | ≈ 1–2 (the target and a neighbour) | 100 × (0.2 + 2 × 0.4) µs ≈ 0.1 ms |
| "long": every bullet flies its full 1.5 s above the world | ≈ 64 × 20 = 1,280 | 0 | 1,280 × 0.2 µs ≈ 0.26 ms |

Distinct samples per tick are at most the candidates (each new (target, sample) is posed once); with lag varying per
command, sharing across shooters happens for bots and for shots of one command, not across humans. E13 gates both
rows at p99 ≤ 0.5 ms and prints queries, candidates and cache hits. Bots add nothing beyond the present set (posed
once in `begin_tick`, 64 × 0.35 µs ≈ 22 µs). Memory: history 64 × 24 × ≈ 48 B ≈ 72 KiB; shot-view rings 64 × 128 ×
8 B = 64 KiB; pose cache 2,048 × ≈ 0.6 KB ≈ 1.2 MiB; the present set ≈ 40 KiB.

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
inside the 24 button bits. `CommandPacket` gains `others_ack_bits` (u32: bit i = the `SnapshotOthers` of snapshot
`newest_snapshot − i` arrived), next to M1's `snapshot_ack_bits`. A 4-command packet grows from 53 B by 17 B
[inferred].

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
  drawn with 64 bots [verified: S1 run]. M2a adds `SnapshotOthers` (unreliable, same `seq`) carrying the remainder,
  nearest first in both. A lost `SnapshotOthers` makes its soldiers extrapolate (≤ 250 ms) like a lost snapshot, and
  its ack bit tells the lag compensation (§6.3). M3 replaces both with priorities and baselines (§16 Q3).

### 7.4 Events

| message | channel | content | size |
|---|---|---|---|
| `ShotEvents` | ch0, one datagram per snapshot when non-empty (two at most; further shots dropped farthest-first and counted) | server tick; per shot: shooter slot (7); shot index: the full 32 bits for a shooter's first shot in the datagram, then a 4-bit delta (escape to 32 bits), so a client that joins late has every index (the effect seeds need the full index, §4.4); def (8); time within the tick (16, 1/65,536 tick); origin (3 × 20 bits, 1/256 m from the level bounds' minimum); direction (octahedral 2 × 16); pellets (5); tracer (1); with tracer set, aim (octahedral 2 × 12: the tracer converges on it, §12.2); with pellets > 1, `pellet_cone` (12 bits, 1/64°, so remote ends expand pellets like the server; no M2a kit item has pellets, M3 / M7 shotguns use it); for a hold-and-release def, `power` (8 bits, 1/255). `spawn_time` is derived (time + the def's `SpawnDelay`); seeds are derived (§4.4), never sent | ≈ 18 B per shot, + 3 B with a tracer; ≈ 28 shots per snapshot in a 64-bot firefight ≈ 0.55 KiB |
| `CombatEvents` | ch1 | `HitEvent`, `DeathEvent` and `DetonationWire` (§4.3), quantised: damage and health in 1/100 hp, position as above, normal octahedral 16 | ≈ 22 B per hit, 8 B per death, 16 B per detonation |
| `KitSelect` | ch1, client → server | kit (u8 < 4) and team (u8: 0 keep, 1, 2; a change needs `dev_commands` in M2a) for the next spawn | 3 B |
| Spawned (M1) | ch1 | gains the kit index and the team | +2 B |
| `DebugRequest` / `DebugReport` (M1) | ch1 | new kinds `SpawnTarget {slot, position, yaw, pose}` and `TargetInfo {slot}` → report `TargetInfoRow {slot, pose, feet, yaw, HitTarget capsules}`; `Trace`'s report gains per tick the `ShotEvent`s, projectile positions of the traced shooter (≤ 64), `DamageEvent`s, `DetonationEvent`s and deaths; dev only, as M1 | Trace ≤ 20 s, fragments on ch1 |

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
* `sim::catch_up` steps the weapons after the motor with the same commands, in the same order as the server.
* On each snapshot the client compares `weapons_wire_view(predicted at server_tick)` with the decoded own block.
  A mismatch rebases the weapons from the wire view and replays (M1's rule). Expected 0 in loopback (E15).
* **Health is not a motor mismatch.** M1's `states_match` compares health (client_session.cpp), so every server hit
  on the own soldier would force a motor replay. cl splits the compare: when only health differs (and the life is
  the same), the client writes the server's health into the ring entries from the snapshot tick on, with no replay,
  and counts `health_rebases`; a death arrives as a motor state change and reconciles as in M1. E15 counts motor and
  weapon reconciliations without the health rebases, and reports those separately.
* **Only the first prediction of a tick emits.** Replays keep state but discard their `ShotEvent`s and timed events;
  the client tracks the highest emitted tick (wp §7.2). So effects and sounds never fire twice.
* On the own death (server state) the client stops emitting; on Spawned it calls `spawn_soldier_weapons` with the
  kit and team from the event.

### 8.3 Presentation

* `PresentationFrame` gains `own_weapons_prev/curr` (the wire views of the two latest predicted ticks, so spread,
  recoil and reload progress interpolate per display frame), the active def's static UI row index and the newest
  snapshot seq whose samples the frame holds (the main thread puts it in the `InputSample` as `view_seq8`, §6.2).
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
  Its target provider poses the others with `posed_target` from `net::sample_other` over the client's received
  samples at `net::rewind_time_q8(time_start, view_back_q8)` of the command that fired the shot: the server's time
  rule of §6.4, on the same samples (§6.3). So the client tests a target exactly where the server will.
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
	uint8_t weapon = 0;               // index into the team's weapon list (4 primaries, sidearm, knife, M67)
	uint8_t fire_seq = 0;             // changes per shot: upper-body fire kick
	bool ads = false, dead = false;
};
```

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
* **Pairs mode** (the budget scenario): 32 pairs of opposing bots placed with `SpawnAt` along clear lanes 20–40 m long
  in front of the observer camera, so every bot is in view and firing (worst case for FX and audio).
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
* Transition: the aiming controller's zoom-in time (iron 0.09 s) and zoom-out 0.15 s; FOV over `fovTransitionTime`
  0.15 s. Mouse sensitivity is scaled by `lookSpeedMul` (0.5 iron).
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
* **Clock**: `SimMixerClock` maps presentation time to mixer frames from the stream's played-frame counter; commands
  are scheduled `schedule_lead_blocks` (2 = 21.3 ms) ahead [verified: au].
* **Impacts**: `ImpactCell::impact_sound` and effect sound components (`FxEvent`) start one-shot `PatchInstance`s
  seeded from the event (au §7.2: no driver needed).
* **Voice limits until A3** (M2b brings HDR, scopes and the 128-voice cap): at most 32 weapon instances (nearest
  first; a culled one stops at its next trigger up) and 64 one-shots (oldest stolen) [inferred, §16 Q9]. AreaType
  stays 0 (A4, M3).
* **Knife and M67**: no `WeaponSoundDriver` loop; their swing, throw and explosion patches start as one-shot
  `PatchInstance`s from OwnShot / RemoteShot / Detonation (E7 item 1 lists the form per item).
* **Client-local audition** for E7 item 4: `Bf3GameAudio::audition(patch, position)` starts a one-shot of any
  compiled patch at a world position; only `m2a_check` calls it (never the server, never the wire).
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
| server: weapon states 64 × 600 B, lag history 72 KiB, shot-view rings 64 KiB | < 0.3 MiB |
| client: prediction ring with weapons (M1's 128 entries) | ≈ 77 KiB more |

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
| `view_seq8` newer than the newest sent snapshot or older than the history | Refused, tested at the present, counted |
| `others_ack_bits` claiming a part never sent | the bit is ignored (the history knows which parts exist) |
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
`WeaponTable::exact_aim`; `TargetQuery`'s segment fields with their defaults), so every track compiles against them
from the start; the bodies are sv's and lc's.

**Shared files are edited additively only.** Each track appends one block headed `# M2a <key>` (CMake) or
`// M2a <key>` (C++) at the end of: each library's `CMakeLists.txt`, `tests/CMakeLists.txt`,
`tools/bf3dump/CMakeLists.txt` and `bf3dump.cpp` (dispatcher), `src/bf3_godot/src/register_types.cpp`,
`bf3-opensource/project.godot`, `README.md`. Files owned by one track below are edited only by that track; where a
track adds to a file another track owns (named in its row), it adds a block at the end and never rewrites lines.
**Named exceptions** (lines are replaced, by the owner named, and nowhere else): cl replaces `present_others`' Hermite
body in client_session.cpp with the call to lc's `net::sample_other`, and splits health out of `states_match`
(§8.2); sv inserts the `exact_aim` branch where `step_soldier_weapons` fills a `ShotEvent` (weapon_sim.cpp) and the
hash line in `weapon_table_hash`; lc fills the new `TargetQuery` fields at `ProjectileWorld`'s query sites
(ballistics.cpp). Each exception is a separate commit whose message names it, and wp's W1–W11 must stay bit-equal
after the two wp edits.

**Test files, one owner each**: `host_gunplay_server_tests.cpp` (sv: `host.gunplay.server`),
`host_gunplay_client_tests.cpp` (cl: `host.gunplay.client`), `host_gunplay_perf.cpp` (bt: `host.gunplay.perf`,
`host.gunplay.replay`). A shared fixture header `tests/gunplay_fixture.h` (headless `ServerHost` + `ClientSession`
on Metro, dev request helpers) is created in W2.0 and extended additively.

### Wave 2

| key | title | owns (creates / edits) | interfaces (headers) | depends on |
|---|---|---|---|---|
| sv | server gunplay | new `include/bf3/host/gunplay.h`, `kits.h`, `target_bots.h`, `server_view.h` with sources in `src/bf3_host/src/`; additive blocks in `game_server.{h,cpp}` (config, the steps of §4.1, `ReceivedSet` feed from packet acks, `CombatEventSink`, `RespawnPolicy` (M1's fixed timer), `ServerView`, stats, handling of the dev kinds `SpawnTarget` / `TargetInfo` and the Trace additions, `KitSelect` kit and team, MatchInfo's `match_seed` / `weapon_hash` / `weapon_flags` / `kit_hash`) and `level_load.{h,cpp}` (`SharedLevel` weapon defs, kits, capsule table load); `command.h` view-sample fields; the `exact_aim` edit to wp's `weapon_sim_def.{h,cpp}` and `weapon_sim.cpp` (named exception); `src/bf3_sim/src/weapons/mechanics_tuning.cpp`, `bf3-opensource/config/weapon_mechanics.cfg`; `find_sky_lane`; tests `host_gunplay_server_tests.cpp` (cases timing, drop, stk, reload, variants, hostile: E1–E6 server side, E17 server side) | `bf3/host/gunplay.h`, `kits.h`, `target_bots.h`, `server_view.h`, `bf3/sim/command.h` (additions), `bf3/sim/weapons/mechanics_tuning.h`, `weapon_sim_def.h` (`exact_aim`) | nt (encoders); lc for the rewind provider and capsules (until it lands: present-tick targets from wp's `standin_target`, a real symbol) |
| lc | lag compensation and hit capsules | `bf3/core/gameplay/capsule_pose_table.h` + cpp; `bf3/anim/server_pose.h` (with `ServerPoseClipNames`, §6.5) + `src/bf3_anim/src/server_pose.cpp`; `bf3/sim/weapons/posed_target.h` + cpp; the `TargetQuery` segment fields in wp's `hit_target.h` and their filling in `ballistics.cpp` (named exception); `bf3/net/other_sample.h` + cpp (`sample_other`, `rewind_time_q8`; written from present_others' body, cl switches the call); `bf3/host/lag_comp.h` + cpp (`LagHistory` with received sets, `RewindProvider` with segment broad phase and pose cache); `bf3dump lagcomp` (`--perf` rows pairs / long, `--deviation`); tests `sim_lagcomp_tests.cpp` (`sim.lagcomp.unit`, incl. lost and reordered snapshots), `anim_server_pose_tests.cpp` (`anim.server_pose`, E18) | the five headers; `hit_target.h` (additions) | — (S3 rig on main; the extra stance clips of §6.5 are in the retail package, verified) |
| nt | protocol v2 | `protocol.h` / `protocol.cpp` (MatchInfo fields, command view sample, `others_ack_bits`, others' weapon fields, `SnapshotOthers`, `ShotEvents` with full shot indices, aim / pellet_cone / power, `CombatEvents` with `DetonationWire`, `KitSelect` kit + team, Spawned kit + team, the `DebugKind`s `SpawnTarget` / `TargetInfo` and `DebugReport`'s `TargetInfoRow` and Trace additions), new `weapons_wire.{h,cpp}`; tests: block in `net_proto_tests.cpp`, new `net_proto_fuzz.cpp` (`net.proto.fuzz`, dev and asan; E17 wire side) | `bf3/net/protocol.h` (v2), `bf3/net/weapons_wire.h` | — |
| cl | client prediction and events | additive blocks in `client_session.{h,cpp}` (ring weapons, reconciliation by wire view, table build and refusal on MatchInfo, first-prediction emission, view sample and `fire_presses` from `InputSample`, `SnapshotOthers` merge and its ack bits, shot / combat decode, `late_sample` count) plus its two named exceptions (`present_others` → `net::sample_other`; health split of `states_match`), `prediction.{h,cpp}` (`catch_up` steps weapons); new `client_projectiles.{h,cpp}` (own and remote worlds, §8.4), `presentation_events.{h,cpp}`, `presentation_weapon.h`; tests: block in `host_session_tests.cpp`, `host_gunplay_client_tests.cpp` (cases lagcomp, loopback, two clients: E12, E15) | `bf3/host/presentation_events.h`, `presentation_weapon.h`, `client_projectiles.h`, `client_session.h` (additions) | nt; lc (`sample_other`, `rewind_time_q8`, `posed_target`); sv phase 1 for its end-to-end cases (until then: a test-local fake server in `m2a_fake`) |
| vm | 1p viewmodel and ADS | `bf3/render/viewmodel_set.h` + cpp (generalises `load_viewmodel`; `viewmodel_assets.*` gains an additive overload), `bf3/anim/viewmodel_anim.h` + cpp, `src/bf3_godot/src/viewmodel.{h,cpp}` (`Bf3Viewmodel`), additive block in `fp_camera.{h,cpp}` (ADS FOV, recoil, kick); `bf3-opensource/config/viewmodel_tuning.cfg`; demo `scenes/m2a_viewmodel.tscn` (`--vm-check`: drives a scripted weapon timeline); `bf3dump vm clips`; tests `anim_viewmodel_tests.cpp` (`anim.viewmodel.unit`, E19 unit rows), `godot.m2a_viewmodel` | `viewmodel_set.h`, `viewmodel_anim.h`; reads `presentation_weapon.h` (W2.0) | — (codes against the contract types; cl provides real frames in wave 3) |
| fx2 | VFX2 renderer | `bf3/fx/render_prep.h` + cpp, `bf3/fx/atlas_pixels.h` + cpp (prepared atlas, cached), `src/bf3_godot/src/fx_renderer.{h,cpp}` (`Bf3FxRenderer`: MultiMeshes, light pool, 1p pass), `bf3-opensource/shaders/fx_*.gdshader`; demo `scenes/fx_demo.tscn` (synthetic starts on Metro); tests `fx_render_tests.cpp` (`fx.render.unit`: batching, sort, light merge), `godot.fx_demo`, `godot.fx_ref` (E9 item 8) | `render_prep.h`, `atlas_pixels.h` | — |
| hud | minimal HUD | `bf3/host/hud_feed.h` + cpp (`fill_hud`, `HudStatic` built from the weapon table and UI descriptions), `src/bf3_godot/src/hud_*.{h,cpp}` (`Bf3Hud`, ammo, crosshair, hit indicator, weapon name and icon, health); demo `scenes/m2a_hud.tscn` (`--hud-check`, scripted model); tests `ui_hud_feed_tests.cpp` (`ui.hud.feed.unit`), `godot.m2a_hud` | `hud_feed.h` | — (ui1, ui2 on main; contract types) |

### Wave 3 (starts when wave 2 is on main)

| key | title | owns (creates / edits) | interfaces (headers) | depends on |
|---|---|---|---|---|
| au3 | game audio | `bf3/audio/weapon_audio.h` + cpp (`WeaponAudioRouter`, `SimMixerClock`, voice limits, the two timelines of §8.3), `impact_audio.h` + cpp; `src/bf3_godot/src/game_audio.{h,cpp}` (`Bf3GameAudio`: mixer, stream, player, bank load step, client-local `audition`); the in-game check script (extends au's `audio_check`); tests `audio_router_tests.cpp` (`audio.router.unit`) | `weapon_audio.h`, `impact_audio.h`, `game_audio.h` | cl (events, presented others) |
| fx3 | VFX3 triggers | `bf3/fx/triggers.h` + cpp (shot, impact, hit, detonation; seeds; cull; the two timelines), `src/bf3_godot/src/game_fx.{h,cpp}` (`Bf3GameFx`: FxWorld + renderer + triggers + decal pool, client-local `start_effect`), `fx_decals.{h,cpp}`; tests `fx_triggers_tests.cpp` (`fx.triggers.unit`) and the `fx.install` case "cells" (in-game cell selection, incl. the Caspian water fallback of E8 (5)) | `triggers.h`, `game_fx.h` | fx2, cl; tp for 3p muzzles (until then: muzzle at eye height + the weapon offset, a test-local fake) |
| tp | 3p soldiers | additive blocks in `crowd_eval.{h,cpp}`, `crowd_clips.{h,cpp}` (stance via lc's `ServerPoseClipNames`, sneak / crawl moves, per-soldier weapon, fire, death, muzzle out); new `src/bf3_godot/src/game_crowd.{h,cpp}` (`Bf3GameCrowd`: present_others → inputs, 3p weapon meshes); tests: block in the S3 tests (`anim.s3.unit`) | `crowd_eval.h` (additions), `game_crowd.h` | nt, cl, lc (clip names) |
| bt | bots | new `include/bf3/host/firefight_brain.h` + `src/bf3_host/src/firefight_brain.cpp` (reads `ServerView`); additive block in S1's bot fill (kit by slot, pairs placement); `bf3dump firefight` (headless run, `--perf`); tests `host_gunplay_perf.cpp` (`host.gunplay.perf`, RUN_SERIAL, E13; `host.gunplay.replay`, E15) | `firefight_brain.h` | sv |
| gx | Godot integration and gate runs | additive blocks in `level_session.{h,cpp}` (adds and feeds `Bf3Viewmodel`, `Bf3Hud`, `Bf3GameAudio`, `Bf3GameFx`, `Bf3GameCrowd`; loader steps; crowd lane rule of §3 after its re-measure), `input_mapper.{h,cpp}` (§5.1 keys incl. K / T, `fire_presses`, render time and `view_seq8` into `InputSample`); new `m2a_check.{h,cpp}` (`--m2a-check=range,reload,kits,hud,ads,audio,fx`, `--m2a-firefight`; the client-local effect start and audio audition); `scenes/m2a_metro.tscn`; `tests/godot_m2a.cmake`, `tests/godot_m2a_firefight.cmake`, the `godot.m1_load` run on `m2a_metro.tscn` (E16); README "M2a" | `m2a_check.h` | all wave-2 tracks; au3, fx3, tp, bt for their cases (each case is skipped with a reason until its track lands) |

### 15.1 Integration order and stubs

1. **W2.0** contract commit (integrator): the wave-2 headers, the declarations of the two wp edits,
   `tests/gunplay_fixture.h`, `m2a.contract` (sizes, enum values, `weapon_buttons::kAll` inside the wire mask, the
   `exact_aim` hash change).
2. **nt and lc** merge first: pure libraries with unit tests and no dependency on the other tracks.
3. **sv phase 1**: weapons on the server tick, kits and teams, deaths, the own-weapons block and others' weapon fields
   on the wire, stand-in capsules, `exact_aim`. Then **cl** lands prediction and events against it.
4. **sv phase 2**: lc's `RewindProvider` and `posed_target` replace the stand-ins; dev range requests; E1–E6, E12,
   E17 headless.
5. **vm, fx2, hud** merge whenever ready; they only touch their own files and demo scenes.
6. **W3.0** contract commit; **bt and tp** first (independent), then **au3 and fx3**, then **gx**, which wires
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
14. **Capsule gate.** **≤ 3 cm for head and chest in idle and aim**; legs and run reported only.
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
* **Decisions**: the open questions of §16 (Q1, Q2, Q3, Q4, Q12 and Q17 change behaviour or the plan most); whether a
  WARN on a noisy-machine budget row (E9, E13, E14) blocks the milestone; the mechanics variant once S5 Session 1 is
  measured.

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
  compares health; the command packet carries `newest_snapshot` + `snapshot_ack_bits`; `InputSample` has
  `jump_presses` and level `held_buttons`; the prediction ring defaults to 128; `SharedLevel` is the loaded level.

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
