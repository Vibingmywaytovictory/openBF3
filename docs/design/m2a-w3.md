# M2a wave 3 contract (W3.0), as built

The integrator's contract commits for M2a wave 3 (docs/design/m2a.md §15 "Wave 3", §15.1 step 6), on branch `m2a/w3`
from `main` 5e296e7. Before the first commit the branch point was checked: `m2a/sv`, `m2a/cl`, `m2a/vm`, `m2a/fx2` and
`m2a/hud` are all ancestors of main (`git merge-base --is-ancestor`), so wave 2 is on main and the review's R1 does not
apply.

The commits declare every interface the wave-3 tracks (bt, tp, au3, fx3, gx) compile against, plus the seven
interfaces wave 2 did not build (G1-G7 of the readiness review). Bodies are written only where the tree must link,
where the body is the interface (G2, G5, G6, the presentation clocks, the case parser), or where it is a one-line
hook stub. Everything is additive except G1, which changes nt's v2 others' block and two `net.proto.unit` pins, as
one named change.

## 1. What the commits contain

| commit | what | m2a.md section it extends |
|---|---|---|
| 7f820a8 | G1: `OtherSoldier::team`, 2 bits after the burst seq in the v2 others' block; `set_other_team`; the server writes the life's team (`Gunplay::other_record`, one tagged line); `net.proto.unit` capacity pins 53 to 52; a new proto case (round trip in Snapshot and SnapshotOthers, team 3 refused) | §7.3, §9 |
| da47088 | G4: `ClientSession::send_kit_select(const net::KitSelect&)` (declaration after `take_own_test_samples`; a queue member next to `debug_out`; the drain after the debug-request send loop; the body at the end). Newest 16 kept, sent on the reliable channel by the client thread | §5.1 (K / T), §7.4 |
| c5a7513 | G3: `GameServerConfig::firefight`, the include and the `add_firefight_bots` call before `init` returns; new `firefight_brain.h` (FirefightConfig in full, FirefightObserver, FirefightStats, `firefight_seed`, FirefightLane, `firefight_layout`, FirefightBrain, `add_firefight_bots`) and a stub `firefight_brain.cpp` | §10.2, §4.1 |
| 2d13fb9 | G2 and G5 with bodies, in the new `bf3/host/client_wiring.h`: `present_other_weapon`, `present_other_weapons`, `make_client_gunplay_inputs` | §6.1, §8.3, §9, §4.2 |
| b9915b3 | named insertions in S3's `crowd_eval.h`: `CrowdSoldierInput` stance / weapon / fire / ADS / death fields, `CrowdWeapon` (with `weapon_joints`, `muzzle_joint`, `muzzle_offset`, `palette`), `CrowdTeam::weapons`, `kMaxCrowdWeapons`, `CrowdEvaluator::muzzle_model` (declared); an end block with `crowd_lanes` and `crowd_team_of_wire` | §9, §3, §16 Q11 |
| e1e17e0 | au3: `weapon_audio.h`, `impact_audio.h`, `game_audio_check.h`; `bf3_audio` links `bf3_host`. `PresentClock` / `present_clock` (body) in `client_wiring.h` | §12.3, §8.3, §4.4, E7 |
| 06fb23c | fx3: `bf3/fx/triggers.h`; `bf3_fx` links `bf3_host` | §12.2, §8.3, §4.4, E8 |
| 6af89d7 | Godot side: `m2a_game.h` (loader inputs, the per-frame bundle and order), `game_audio.h`, `game_fx.h`, `game_crowd.h`, `m2a_check.h` (Godot-free, with the case parser's body); G6 `viewmodel_muzzle.{h,cpp}` with its body; `m2a_w3_headers.cpp` compiles the Godot headers in the GDExtension | §12, §9, §11.1, §15 gx row |
| 414fa79 | G7: the member block in `level_session.h` (one named insertion) and the new `level_session_m2a.{h,cpp}` (empty `M2aSession`, no-op hooks) | §15 gx row, §3 |
| fbf4b25 | G7: one tagged call per hook in `level_session.cpp` (named insertions) | §15 gx row |
| 2043315 | `m2a.contract3` | §15.1 |
| 3cc624e | review fix: named insertion in `crowd_eval.h`'s `CrowdEvaluator`: `weapon_of(i)`, `weapon_palette(i)` (declared, tp's body); `kNoCrowdWeapon` in the end block | §9, R5 |
| f4cdb8b | review fix: tp's 3p weapon route. New `bf3/render/crowd_weapons.h` (`load_crowd_weapons` declared; `strip_team_weapon_parts`, `crowd_weapons_of`, `crowd_weapon_index` with bodies), the `# M2a w3` block in bf3_render's CMake, the route in `game_crowd.h`, `m2a.contract3` pins and a case | §9, R5, §13 |

Every shared CMake file got one `# M2a w3` block at its end (bf3_host, bf3_audio, bf3_fx, bf3_godot, bf3_render, tests).

### 1.1 Who writes which body

| track | declared in W3.0, body theirs | stubs W3.0 wrote (theirs to replace) |
|---|---|---|
| bt | `firefight_layout`, `FirefightBrain`, `add_firefight_bots` | `firefight_brain.cpp`: succeeds without a config or with `count` 0, otherwise fails with "W3.0 stub", so a run that asks for bots before bt lands fails loudly |
| tp | `CrowdEvaluator::muzzle_model`, `weapon_of`, `weapon_palette`, R5 in `crowd_eval.cpp`, `load_crowd_weapons` (its new `src/bf3_render/src/crowd_weapons.cpp`, listed in its own `# M2a tp` CMake block), `Bf3GameCrowd` | none |
| au3 | `SimMixerClock`, `load_weapon_audio`, `OneShotPool`, `WeaponAudioRouter`, `ImpactSoundCache`, `ImpactAudio`, `game_check_script`, `game_audition_items`, `GameAudioCheck`, `analyse_game_check`, `Bf3GameAudio` | none |
| fx3 | `load_fire_effects`, `FireEffectTable::blueprints`, `impact_sound_names`, `impact_effect_names`, `fallback_muzzle`, `FxTriggers`, `Bf3GameFx`; it owns `viewmodel_muzzle.{h,cpp}` from now on | none |
| gx | `M2aCheck`, every `m2a_*` hook body and `M2aSession`'s members | `level_session_m2a.cpp`: every hook does nothing |

Written by the integrator with bodies: `present_other_weapon(s)`, `make_client_gunplay_inputs`, `present_clock`,
`send_kit_select`, `set_other_team` and the G1 codec lines, `crowd_lanes`, `crowd_team_of_wire`, `sound_seed`,
`trigger_seed`, `firefight_seed`, `parse_m2a_cases` and the case-name helpers, `viewmodel_muzzle_transform`,
`strip_team_weapon_parts`, `crowd_weapons_of`, `crowd_weapon_index`.

## 2. The seven gaps (review §1.2), and what was decided

**G1, the others' team: 2 bits on the wire, not a Roster message.**
- The team belongs to a life. It switches with the generation at a respawn after a T-key change, and a per-sample
  field switches together with the pose and the weapon, by the same `u >= 1` rule.
- A Roster message would be reliable while the snapshots are not. Its order relative to the snapshots would be a
  race at every respawn. It would also need a client cache keyed by (slot, life), late-join replay, and its own
  fuzz rows.
- The cost is 2 bits per other: 180 bits = 22.5 B. 49 others still fit a snapshot with the own state (1,199 bytes),
  and 52 fit without it (nt v2: 53).
- The decoder refuses code 3, so a hostile datagram fails instead of being clamped. `set_other_team` stores what the
  receiver decodes: 1 and 2 are kept, anything else becomes 0 (unknown). That keeps the lag history's records equal
  to the wire's (nt's rule).
- The server writes the team in `Gunplay::other_record`, so the record and the lag history carry it.

**G2, the others' weapon fields at render time: a free function, not new `PresentedSoldier` fields.**
- `PresentedSoldier` is cl's, and `present_others` is cl's hot loop. `present_other_weapon(frame, slot,
  render_ticks)` and `present_other_weapons(frame, render_ticks, out)` read the same window with the same
  `net::select_sample` and the discrete-field rule of `sample_other`: the older sample's fields until u reaches 1,
  then the newer's.
- So weapon, generation and pose always come from one sample. `m2a.contract3` checks this at eight render times
  against `present_others`' generation.
- `firing` means "fired since the previous snapshot tick" (sv). The 3p trigger, the fire kick and the burst seed are
  derived from `firing` plus `burst_seq` changes, as the header says.

**G3, the firefight hook.**
- `GameServerConfig::firefight` is a `shared_ptr<const FirefightConfig>`, null by default. `GameServer::init` calls
  `add_firefight_bots(*this, config)` after the target bots, the walkers and S1's bots, and before it returns.
- `FirefightConfig` holds every value of §10.2 with its default:
  - mode Pairs, count 64, observer;
  - aim error 1.5 to 0.5 degrees over 1 s;
  - bursts of 3-6 rounds with pauses of 0.3-0.6 s; semi-automatic at 2 Hz; reload below 0.5 of the magazine;
  - lane fan ±35 degrees, lanes 15-60 m out, pairs 20-40 m apart, 8 lane retries;
  - retarget every 30 ticks, a no-target burst after 2 s, walk on;
  - shared atomic stats.
- `firefight_seed(match_seed, slot)` is `match_seed ^ slot`.
- The header records the dead-slot rule for bt: `place_soldier` on a dead slot respawns it at once, so free mode
  waits for the RespawnPolicy.
- `walk` needs S1's BotLevel (`GameServerConfig::bots`). gx turns the bot level on for the firefight.

**G4, a runtime KitSelect.** `send_kit_select` queues the message (newest 16 kept). The client thread sends it on
the reliable channel next to the debug requests. Team changes still need `dev_commands`, which the level session
already sets.

**G5, the client's gunplay inputs.** `make_client_gunplay_inputs(const SharedLevel&)` wraps cl's existing
`client_gunplay_inputs`. A level loaded without `SharedLoadOptions::gunplay` gets an error that names the switch.
gx's `m2a_client_config` passes the result, so `restart_client` reuses one table.

**G6, the 1p muzzle.**
- The weapon skeleton has no muzzle joint [verified: `bf3dump dump Win32/Levels/MP_Subway/MP_Subway <scratch> 0 0
  animations/skeletons/weapon/weaponske01`].
- Its 25 bones are:
  - `exportAnimation`, `Wep_Root`, `Wep_Extra1-3`, `Wep_Trigger`, `Wep_Slide`;
  - `Wep_Grenade1-2`, `Wep_Mag`, `Wep_Mag_Ammo`;
  - `Wep_Physic1-3`, `Wep_Belt1-5`, `Wep_Bipod1-3`;
  - `IK_Joint_LeftHand`, `IK_Joint_RightHand`, `Wep_Aim`.
- There is no `Wep_Muzzle` and no `Wep_Barrel`; resdump's bone list had them only as hash candidates.
- A weapon's FireEffects1p offsets are relative to the weapon (fx_demo.cpp, m2a-fx2.md §6.1), and its frame on the
  rig is `Wep_Root`. So `viewmodel_muzzle_transform(const Bf3Viewmodel&[, offset])` returns:
  - `weapon`: the rig node's global transform × `model()[Wep_Root]`;
  - `muzzle`: `weapon × offset`, where fx3 passes the FireEffects1p entry's offset.
- Without a rig or an item in hand it falls back to fx2's camera-space point (0.10, -0.11, -0.70) on the rig's
  CameraJoint, flagged `CameraStandIn`.
- vm's files are untouched; the function uses only public accessors.

**G7, gx's hooks in the level session.**
- One member block in `level_session.h` with nine hooks and `m2a_` (an `M2aSession` with its deleter, defined in the
  new `level_session_m2a.{h,cpp}`). One tagged call per hook in `level_session.cpp`:
  - `m2a_parse_args` at the end of `parse_args`;
  - `m2a_load_options` before `load_shared_level` (gunplay on);
  - `m2a_load_assets` after the S1 bot level, before Phase D (viewmodel team sets, effects, wave bank, crowd assets;
    false fails the load);
  - `m2a_server_config(*game)` before the `ServerConfig` (gunplay, firefight, bots, `respawn_ticks`);
  - `m2a_client_config(cc)` before `client_config` returns (gunplay inputs, kit select);
  - `m2a_presented` after `set_input`, every frame from the session start, so the PresentationEvents ring is drained
    during the load as well (E14 gates its overflows at 0);
  - `m2a_frame` after the S1 calls; a true return skips M1's checks and shots for that frame;
  - `m2a_input` after the mapper;
  - `m2a_shutdown` after the loader has joined, before the client and server stop.
- W3.0's hook bodies do nothing, so M1, S1 and S3 run exactly as before.

## 3. Interfaces, by track

* **au3** (`weapon_audio.h`, `impact_audio.h`, `game_audio_check.h`, `game_audio.h`):
  - **SimMixerClock** is built on `MixerCore` (`clock()` and `earliest_frame()`) and the output latency measured on
    the stream. `frame_of(t, timeline)` = heard_now + (t - now(timeline)) × rate + bias, clamped to
    `earliest_frame()` and counted when late. Immediate events get `earliest_frame()`.
  - The two clocks come from `host::present_clock`:
    - Predicted = (own tick - 1 + `present_own_fraction`) × `kTimeUnitsPerTick`;
    - Render = `present_render_ticks` × `kTimeUnitsPerTick`.
  - **WeaponAudioRouter** has `on_events(span<const PresentEvent>, clock)`, `on_others(soldiers, weapons, muzzles,
    clock)`, `set_listener` (re-sends every live emitter: the mixer applies a listener only on the next SetEmitter)
    and `update(now_ns)`.
  - Voice limits: 32 weapon voices, 64 one-shots shared with `ImpactAudio` through one `OneShotPool`, and a mixer cap
    of 96 (§16 Q9).
  - `sound_seed(entity, burst_seq)` (§4.4).
  - `WeaponAudioForm`: Driver, OneShot (knife and M67) or None, with a reason (E7 item 1).
  - The impact patches compile in `Bf3GameAudio::load` (`ImpactSoundCache`) from `Bf3GameFx::impact_sound_names()`.
    The review's "who compiles the impact patches" goes to au3, with fx3 naming them.
  - **E7**: `GameAudioCheck` is a `MixTap` (the full mix) and a `WeaponAudioObserver` (the shooter's driver
    instances get the core and noise taps before Start). It records windows. `analyse_game_check` maps them onto a
    `CheckScript` and calls au's `analyse_check`, plus the in-game rows (no short or late blocks, no late commands,
    `mixer_refusals` 0).
  - `kGameCheckDistances` = 5 / 20 / 100 / 150 / 280 / 320 m. No au file is edited.
* **fx3** (`triggers.h`, `game_fx.h`, `viewmodel_muzzle.h`):
  - `FxTriggers::{on_muzzles, on_events(events, PresentClock, FxView)}`, then the frame's `decals()` and `sounds()`
    cues. bf3_fx never links bf3_audio; `Bf3GameAudio::step` takes `Bf3GameFx`'s cues.
  - Start age is now(timeline) - time. Events ahead of the drawn time wait in a pending list.
  - Duplicate starts are skipped and counted (`double_starts`, E8 (7)).
  - `trigger_seed(event seed, purpose, index)` gives every start inside an event its own seed.
  - `FireEffectTable` / `load_fire_effects` read FireEffects1p / FireEffects3p, the tracer, `trace_frequency` and
    the bullet material per def.
  - The soldier cells are the impact table's zone materials (Body 66, Chest 158, Head 65 [verified: fx_harness.cpp's
    Metro rows]). The Detonation effect is named by its short name `fx_grenade_frag_01`.
  - Every transform is an effect frame in fx2's convention (local +Y = barrel or normal).
  - Decal pool ≤ 256, faded after 30 s.
  - `Bf3GameFx::start_effect` is client-local (E8 (4)).
* **tp** (`crowd_eval.h` additions, `game_crowd.h`):
  - `CrowdWeapon` per team: binds, reload clip and rate, weapon joints and their local pose, muzzle joint and offset,
    palette. `CrowdTeam::weapons` (empty: S3's single weapon, so S3 is unchanged).
  - `CrowdSoldierInput` gains `pose`, `target_pose`, `transition`, `weapon` (an index into `weapons`), `fire_seq`,
    `ads`, `dead`. Their defaults are S3's soldier.
  - `crowd_team_of_wire` maps the wire's 1 / 2 to the crowd's 0 / 1.
  - `crowd_lanes(hc, listen)` = clamp(hc - 8, 2, 4) with a listen server, else S3's clamp(hc - 4, 2, 6) (§16 Q11).
  - `CrowdEvaluator::muzzle_model(i, out)` publishes the muzzles. `Bf3GameCrowd::muzzles()` gives them to fx3 and au3
    as `fx::MuzzleSample`.
  - The review's watch item (`crowd_eval.cpp` sizes every soldier's binds from team 0) is part of tp's R5: size the
    body buffers from the largest team's binds and the weapon buffers from the largest `CrowdWeapon::binds`.
  - **tp's 3p weapon route** (closes review §1.1 row 1, the W3.0 review's blocking item). S3 bakes one weapon into
    each team: it is the last `SoldierPart` of `SoldierTeam`, its binds are in the team's one `SkinBinds` list,
    `SoldierSpec` names one weapon per team, and `SkinUploader::build` draws one combined mesh per team from
    `SoldierTeam::parts`. tp owns neither `soldier_assets.*` nor `skin_upload.*`, so the route edits neither. This
    is option (a) of the review. Option (b), named insertions in S3's loader and uploader, was not taken: each of
    the uploader's backends would need a second mesh path, and S3's G0 / G1 rows would have to be re-proven.
    1. Load: `load_soldier_assets` as S3 does, then `render::load_crowd_weapons` (new `crowd_weapons.h`, tp's body)
       for every kit item of each team. Each item gets:
       - its own `SkinBinds` (weaponske01 bound into the rig, with the `bind_lod` S3 uses for its weapon part);
       - its mesh through `load_mesh_cached` and its materials through `mesh_materials`;
       - its reload clip through `load_chosen_clip`, by its chooser index;
       - the step-3 weapon layout;
       - the 3p muzzle: Wep_Root and the FireEffects3p offset, as G6 does for 1p.
       Item 0 of each team is the team's `SoldierSpec` weapon, so index 0 keeps S3's meaning. Every primitive the
       loader needs is public (`mesh_cache.h`, `material_lookup.h`, `skinning.h`, `crowd_clips.h`).
    2. `render::strip_team_weapon_parts(assets)` (body in W3.0) removes the weapon `SoldierPart` before
       `SkinUploader::build`. The team meshes then hold body, head and helmet only. The team binds, and so
       `palette(i)`, keep their layout. The default weapon's few binds are still posed, but no drawn vertex reads
       them.
    3. `render::crowd_weapons_of(set, team)` gives the `CrowdTeam::weapons` views for `CrowdEvaluator::make`.
       `render::crowd_weapon_index(set, team, def)` is the def to `CrowdSoldierInput::weapon` table. It returns -1
       for an unknown def; the soldier then draws item 0 and `GameCrowdStats::unknown_weapons` counts it.
    4. The evaluator output (named insertion 3cc624e, tp's body):
       - `weapon_of(i)`: the item behind the front palette, or `kNoCrowdWeapon` without one;
       - `weapon_palette(i)`: one `Mat34` per bind of that item's `binds`, model space, in the same per-soldier
         double buffer and under the same `changed(i)` flag as `palette(i)`.
    5. Draw: tp's own code in `game_crowd.cpp`. It makes one RS mesh per item, and per soldier one instance and one
       RS skeleton sized from its item's binds. When `weapon_of(i)` changes, the instance is re-based and the
       skeleton re-allocated (`GameCrowdStats::weapon_rebases`). The bones come from `weapon_palette(i)` when
       `changed(i)`, as the RS backend does for the body.
    The m2a.md §13 budget line "3p weapon meshes (14) ≈ 30 MiB" covers these items. The weapon skeletons add about
    5 to 8 bone calls per drawn soldier to the RS backend's 107-113 [inferred from s3.md's bind counts]. tp
    reports the measured cost inside E14.
* **bt** (`firefight_brain.h`): see G3. The functional 64-bot row is registered as its own ctest,
  `host.gunplay.firefight`, outside build.cmd's TIMING regex (review R6).
* **gx** (`m2a_check.h`, `m2a_game.h`, the G7 hooks):
  - Cases: range, reload, kits, hud, ads, audio, fx, **play** (the scripted 5-minute play run) and firefight
    (`--m2a-firefight`).
  - `parse_m2a_cases` refuses an empty list, an empty item, an unknown or mistyped name, a repeat, and "all" mixed
    with names. "all" is the seven functional cases.
  - `m2a_case_track` names the track a case waits for; until then the case is skipped with a reason.
  - `m2a_game.h` fixes the per-frame order:
    1. drain the events once;
    2. `present_others` and `present_other_weapons`;
    3. crowd step, then the own 1p muzzle followed by the crowd's muzzles;
    4. FX step, then its sound cues;
    5. audio step.
  - `m2a_game.h` also fixes the load order: FX, then audio with FX's impact sound names, then crowd.

`Bf3GameAudio`, `Bf3GameFx` and `Bf3GameCrowd` are plain classes owned by the level session, not registered Godot
classes. They have no editor surface, the session drives every call, and an unregistered class needs no
`_bind_methods` before its body exists. A track may register a node later in its own `register_types.cpp` block.

## 4. The review's other items (review.md §1.1, §1.3, §2, §3)

| item | status |
|---|---|
| R1: the starting point | closed: wave 2 is on main (ancestor check above); m2a/w3 is from main 5e296e7 |
| R2: parallel tracks need G1-G7 | closed by G1-G7 |
| `CrowdWeapon` without weapon joints or a muzzle joint | closed: `weapon_joints`, `weapon_local`, `muzzle_joint`, `muzzle_offset` |
| the wire's team 1 / 2 against the crowd's 0 / 1 | closed: `crowd_team_of_wire` (m2a.contract3 pins it) |
| `CrowdWeapon` / `CrowdTeam::weapons`: the weapon is baked into each team's combined mesh, and tp owns neither `soldier_assets.*` nor `skin_upload.*` (row 1; the W3.0 review's blocking item) | closed: tp's 3p weapon route (§3): `crowd_weapons.h` (`load_crowd_weapons` is tp's; `strip_team_weapon_parts`, `crowd_weapons_of`), `CrowdEvaluator::weapon_of` / `weapon_palette` (named insertion), and tp's own weapon draw in `game_crowd.cpp`; no S3 file is edited |
| def id to the crowd weapon index | closed: `render::crowd_weapon_index` (body in W3.0); Bf3GameCrowd (tp) fills the specs' defs |
| 3p muzzle output | closed: `CrowdEvaluator::muzzle_model`, `Bf3GameCrowd::muzzles`, `fx::MuzzleSample`, `audio::MuzzlePoint` |
| `FirefightConfig` in full, the server hook, the dead-slot rule | closed (G3) |
| a dead bot's source is still called every tick | documented for bt: the brain checks the slot is alive |
| the `MixerCore::clock()` meaning, the listener pitfall | closed in `weapon_audio.h` (SimMixerClock, `set_listener`) |
| the impact patch compile owner | closed: au3 compiles them (ImpactSoundCache), fx3 names them |
| `game_audio_check` needs its own script, taps and a MixTap | closed: `GameAudioCheck` (MixTap + WeaponAudioObserver), `analyse_game_check` |
| the lane rule local to `s3_crowd.cpp` | closed: `anim::crowd_lanes` (S3's file untouched; S3 can adopt it later) |
| `--m2a-check=play` not in m2a.md | closed: `M2aCase::Play` declared (gx) |
| the effect-frame convention (+Y = barrel or normal) | closed: stated in `triggers.h` |
| light pool and 1p pass already done (fx2) | stated in `game_fx.h` |
| VFX1's BoxEvaluator pivot read as metres (1p flash 2 m along the barrel, m2a-fx2.md §9) | fx3, as a named replacement in its own commit |
| `godot_m1_load.cmake` hard-codes m1_metro.tscn (E16) | gx: a `-DSCENE` parameter as a named replacement, or a new `godot_m2a_load.cmake` |
| the s3_crowd 4-lane re-measure (a timing row) | the integrator, on main after the merges (timing rows run on main only); the rule lands defaulted to 4 lanes with a listen server |
| `host.gunplay.firefight` naming (R6) | bt, outside the TIMING regex |
| sv's `kMaxLagSlots` 64: with 64 bots the player lands in slot 64 and is not rewindable | open for gx: the playable default stays at ≤ 32 bots (the spec's 16-32), and a firefight run with 64 bots keeps the player out of the server |
| walker seeds use `GameServerConfig::seed`, not `match_seed` (§4.4) | open, not W3.0's: sv / S1 decide; the firefight brains use `firefight_seed(match_seed, slot)` |
| stance moves (Sneak, Crawl) not in `ServerPoseClipNames` | tp loads them separately |
| E9's reference row (m2a.md 1.05 ms against fx2's 0.758 ms) | the integrator, at the gate pass |
| playability (review §2: mouse capture, own death, kill counter, friend / foe, respawn time, kit / team, def to item, launch, free camera, capsules first) | gx's spec; W3.0 provides the team (G1), runtime KitSelect (G4), `respawn_ticks` through `m2a_server_config`, the per-frame hooks and the own-alive flag in `M2aFrame` |
| R3-R5, R7, R9-R11 | script-level (merge order, filters, reviewers); not W3.0's |
| no CMake test enforces VFX4 (no server module links bf3_fx) | still true; bf3_fx and bf3_audio now link bf3_host, never the reverse |
| the W3.0 review's non-blocking notes | recorded in §4.1 |

### 4.1 Notes from the W3.0 review (non-blocking)

* **gx, `input_mapper.{h,cpp}`.** Several of gx's input changes alter lines inside existing functions:
  - the fire / ADS / reload / slot / K / T actions;
  - the click that captures the mouse, which must not fire (`input_mapper.cpp`'s left click);
  - `fire_presses` in `InputMapper::sample()`.

  Each is a gx named insertion in its own commit, named in the message. m2a.md §15 already gives gx the file. If a
  change cannot be made as an insertion, there is a fallback: the `m2a_presented` hook runs after `set_input`, so gx
  can call `set_input` again with an augmented sample.
* **Effect sounds (fx3 and au3).** `ImpactAudio::on_effect_sound` takes FxEvent SoundStart / SoundStop (m2a.md
  §12.3). But `Bf3GameFx` exposes only `sound_cues()`, and `Bf3GameAudio::step` takes only impact cues. Both
  headers belong to their owners:
  - fx3 adds an accessor for the frame's effect-sound events to `game_fx.h`;
  - au3 adds a matching `step` parameter to `game_audio.h`;
  - they agree on it before au3's merge, and gx passes it in its frame hook.
* **G4 timing (gx).** `send_kit_select` drains on every client loop, including before the server has assigned the
  slot. `sv_kit_select` then counts the message in `bad_datagrams` and drops it. So gx sends runtime KitSelects
  only after `Spawned`; the kit at start still goes with Ready. That keeps the E14 / E17 `bad_datagrams` gates
  clean. m2a.contract3's G4 row checks only that queueing before start neither crashes nor sends.
* **Capsules (gx).** M1's `draw_others` capsules keep drawing after tp lands. gx hides that node in `m2a_frame`
  once `Bf3GameCrowd::built()` is true (stated in `game_crowd.h`).
* **Commit granularity.** b9915b3 put three named insertions into `crowd_eval.h` in one commit: the
  `CrowdSoldierInput` fields, `CrowdTeam::weapons` and `CrowdEvaluator::muzzle_model`. Its message names each one.
  The review fix 3cc624e is one commit for one insertion.
* **`FirefightConfig::count` = 64** (m2a.md §10.2). With an observer client, the player lands in slot 64, outside
  sv's `kMaxLagSlots`. See the gx row in §4: the playable default stays at 32 bots or fewer.
* **G1 capacity.** A SnapshotOthers holds 52 others, and a Snapshot about 42 with the own state and a kit. That is
  ample for 64 slots; it would only matter at 128.

## 5. m2a.contract3

`ctest --preset dev -R "^m2a\.contract3$"` runs `tests/m2a_contract3_tests.cpp` (suite `m2a-contract3`, target
`test_m2a_contract3`). It needs no install and runs no session thread.

* Compiled together: every Godot-free wave-3 header, plus `m2a_game.h` and `m2a_check.h` from `src/bf3_godot/src`.
  The Godot-side headers compile together in `m2a_w3_headers.cpp`.
* Pinned with `static_assert`:
  - the shapes: `PresentEvent` 64 B; trivially copyable `PresentedOtherWeapon`, `PresentClock`, `MuzzleSample`,
    `MuzzlePoint`, the stats and `CrowdSoldierInput`; the base classes;
  - the enum values: FirefightMode, WeaponAudioForm, TriggerPurpose 0-7, M2aCase 0-8 with names and tracks,
    PresentTimeline;
  - the limits: 32 / 64 / 96, 256 decals, 30 s fade, 30 m tracer, the six E7 distances, 8 crowd weapons, 4 listen
    lanes;
  - the lane rule at 0, 2, 4, 8, 10, 11, 12 and 16 cores, and the team map;
  - `kNoCrowdWeapon` 0xFF and the member types of `CrowdEvaluator::weapon_of`, `weapon_palette` and `muzzle_model`;
  - the seed hashes: `sound_seed(7, 5)` = 0xf90639b22b02a930, `sound_seed(0, 0)` = 0xe220a8397b1dcdaf, four
    `trigger_seed` values, `firefight_seed`, and cl's `effect_seed(0x5EED5EED12345678, 3, 42)` = 0xb5be2a90e372ab24.
* Functional rows:
  - every §10.2 / §16 default;
  - G1: 127 others encode with 75 dropped, 52 decoded with their teams;
  - G2: eight render times against `present_others`, including hold, exact tick, u = 1 and extrapolation;
  - the presentation clocks;
  - G3: init without a config, with count 0, and with 4 free-mode bots (the stub refuses with "W3.0 stub" until bt);
  - G4: 40 `send_kit_select` calls before start, then stop;
  - G5: a synthetic level without gunplay is refused with the switch's name;
  - the case parser with 10 hostile command lines;
  - tp's route:
    - `strip_team_weapon_parts` on a synthetic two-team asset set: two parts removed, then none (idempotent), and
      the vertex and triangle totals;
    - `crowd_weapons_of` on ten items: it stops at 8, with the reload, weapon-joint and muzzle views;
    - `crowd_weapon_index`: hits, an item past 8, an unknown team.
* Pending switches: `kBtLanded`, `kTpLanded`, `kAu3Landed`, `kFx3Landed`. Each owner sets its switch in the commit
  that lands the body. au3's row then checks SimMixerClock on the two timelines; fx3's checks the triggers on an empty
  frame; bt's checks that init adds the bots.

## 6. Gates, one command each

* `tools\build.cmd dev`: the branch pass (no timing, realtime or window rows).
* `ctest --preset dev -R "^m2a\.contract3$"` and `ctest --preset asan -R "^m2a\.contract3$"` (after
  `tools\build.cmd asan --no-test`).
* `ctest --preset dev -R "^net\.proto"`: G1's pins and the new proto case.

Measured results: README "M2a wave 3 contract (W3.0)".

## 7. Track worktrees

From `m2a/w3` at the last W3.0 commit:
- bt, tp, au3, fx3 and gx, each on branch `m2a/<name>` at `C:\Users\olku\BF3 reimplementation\wt\<name>`;
- submodules initialised with `--reference` to the main checkout's modules;
- `bf3-opensource/.godot/extension_list.cfg` written as in the other worktrees.
