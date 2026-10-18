# M2a tp: 3p soldiers from server state, as built

Track "tp" of M2a wave 3 (docs/design/m2a.md §15, row tp). Branch `m2a/tp` from the W3.0 contract (`m2a/w3`
3b3b991). Scope: m2a.md §9, R5, E18 (b), §16 Q13 and S3's rules (s3.md). The README's "M2a tp" section repeats the
gates.

Tags as in m2a.md: **[verified]** means read from code, data or a measurement in this tree on 2026-10-18;
**[inferred]** means a choice or estimate made here. Time numbers come from a branch run on an unlocked, shared
machine, so they are indicative only (rule 7). Timing gates (S3 G1a inside E14) are not run on this branch.

## 1. What landed

| file | what |
|---|---|
| `src/bf3_anim/include/bf3/anim/crowd_eval.h` | named insertion: the `CrowdStanceSet` / `CrowdM2aStats` forward declarations, `make()` with a stance set, `drawn()`, `m2a_stats()`; the `# M2a tp` end block: `CrowdDeathStyle`, the Q13 constants, `CrowdStanceSet`, `CrowdM2aStats` and the step list |
| `src/bf3_anim/src/crowd_eval.cpp` | named insertions (the M2a impl members, the per-soldier hooks), **R5** in its own commit (ff8c664), and the `# M2a tp` block: `evaluate_m2a` (stance, transitions, the crouch and prone moves, fire, death, the weapon palette, the muzzle) |
| `src/bf3_anim/{include/bf3/anim,src}/crowd_clips.{h,cpp}` | `# M2a tp` blocks: `CrowdMoveClip` (13 controllers), `CrowdMoveClipNames`, `CrowdMoveClips`, `load_crowd_move_clips` (binds on the S3 package and rig and measures each move's root motion) |
| `src/bf3_render/src/crowd_weapons.cpp` (+ `# M2a tp` CMake block) | `load_crowd_weapons` and the other bodies of W3.0's `bf3/render/crowd_weapons.h`: one skinned 3p mesh per kit item, its binds, the reload clip and rate, `weapon_local`, the 3p muzzle, `strip_team_weapon_parts` and `crowd_weapon_index` |
| `src/bf3_godot/src/game_crowd.cpp`, `game_crowd_items.h` | `Bf3GameCrowd` (W3.0's `game_crowd.h`): `present_others` → `CrowdSoldierInput`, events, per-soldier 3p weapon instances, the muzzles for fx3; `crowd_item_specs` builds the per-team item lists from the level's kits |
| `src/bf3_godot/src/tp_crowd_check.{h,cpp}`, `bf3-opensource/scenes/m2a_tp_crowd.tscn` | `Bf3TpCrowdCheck`: the crowd's runtime check in the level until gx wires it (§5) |
| `src/bf3_godot/src/register_types.cpp` | named insertion (a62c2ef): the `openbf3_register_m2a_tp()` call |
| `tests/anim_s3_tests.cpp` | `# M2a tp` block: three `anim-s3-unit` cases (R5 with one weapon per team, R5 with two, the stance set) and the `anim-s3` case "capsules" (E18 (b)) |
| `tests/render_crowd_weapons_tests.cpp` | `render.crowd_weapons` (install) |
| `tests/godot_m2a_tp_crowd.cmake`, `tests/CMakeLists.txt` block | `godot.m2a_tp_crowd` (headless) |
| `tests/m2a_contract3_tests.cpp` | named replacement: `kTpLanded = true` |

## 2. The evaluator (crowd_eval)

### 2.1 R5: weapons per soldier

* `CrowdTeam::weapons` (≤ `kMaxCrowdWeapons` = 8) holds one `CrowdWeapon` per kit item. When a team has items, the
  soldier's `CrowdSoldierInput::weapon` picks three things from `weapons[weapon]`:
  - the weapon layout (S3 step 3, `weapon_local`);
  - the reload clip and rate (step 4);
  - the weapon binds (step 6, written to a separate weapon palette).
* The team's own `binds` become the body (body, head and helmet).
* An index past the end draws item 0 and is counted in `bad_weapon`.
* A weapon change cancels a reload that is playing (`reloads_cancelled`).
* Index 0 is today's data. A team without items draws as before, and with one item equal to S3's weapon the
  evaluator is S3's bit for bit (gate R5, §4).
* **Binds init fix.** Each soldier's buffers are sized from the largest team's binds and the largest
  `CrowdWeapon::binds`. A soldier that was never evaluated has an empty palette. S3 sized every soldier from team 0's
  binds, so a team-1 soldier with more binds would have read past its buffer.
* `make()` without a stance set and without items builds S3's plain evaluator. Nothing in S3's scenes changes.

### 2.2 The stance set

`make(p_clips, CrowdStanceSet{ pose, moves, death }, ...)` takes two clip sources:

* `pose`: lc's `ServerPoseClips` (`load_server_pose_clips` with `ServerPoseClipNames`, the stance idles, aims and the
  six `Rifle_T_*` transitions). The drawn soldier and the server's capsule table therefore come from one source
  (m2a.md §6.5).
* `moves`: tp's `CrowdMoveClips`.

The per-soldier rule, in order (crowd_eval.h end block):

1. **Dead** → the death pose (§2.4).
2. **`pose != target_pose`** → the transition clip at `transition` × its end frame, with no locomotion. This is the
   capsule table's rule: from != to is a transition at the fraction, even at 0 and 1. A transition aims with its
   source stance, as the table does.
3. **Steady stance:**
   - stand is S3's run blend, bit for bit;
   - crouch and prone use lc's idle (`Rifle_CrouchIdle_Pose`, `Rifle_ProneIdle_Pose`) and tp's move cycles, blended
     by S3's run blend on each stance's own `LocoClipInfo` table.
4. **Aim:** the stance's AimUp / AimDown additive, at weight min(1, |pitch| / full). For stand, full is S3's config
   values. For crouch and prone it is lc's `server_pose_stance_aim`, the same weights as the capsule table.
5. **Fire, reload, palettes, muzzle** (§2.3, §2.5).

A missing stance idle draws from the stand idle. Incomplete moves make the stance move with the stand cycles. Both
are counted in `stance_fallbacks`; the retail install has 0 (§4). Hostile inputs are clamped and counted
(`bad_stance`):
* pose or target_pose > 2;
* a non-finite transition, which becomes 0.

### 2.3 Moves, fire

The 13 controllers of `CrowdMoveClipNames` were each found exactly once in `b_basicassetsmp` [verified, antdump list
2026-10-18].

| clip | controller | frames | measured root speed |
|---|---|---|---|
| crouch fwd | `Rifle_SneakFwd` | 42 | 1.04 m/s |
| crouch bwd | `Rifle_SneakBwdLeft45` + `Rifle_SneakBwdRight45` 50/50 at one phase (`Rifle_SneakBwd` occurs twice) | 32 / 32 | 0.90 m/s |
| crouch left / right | `Rifle_SneakLeft1` / `Rifle_SneakRight1` | 45 / 45 | 0.99 / 0.92 m/s |
| prone fwd / bwd | `Rifle_CrawlFwd` / `Rifle_CrawlBwd` | 35 / 65 | 0.64 / 0.36 m/s |
| prone left / right | `Rifle_CrawlLeft` / `Rifle_CrawlRight` | 51 / 45 | 0.31 / 0.28 m/s |
| crouch fire | `Rifle_CrouchFire` (FullBodyWeaponRootNoFingers) | 21 | |
| deaths | `Death_StandChest`, `Death_Crouch1`, `Death_Prone1` | 35, 36, 15 | |

* **Fire.** A change of `fire_seq` starts the fire.
  - Crouch plays `Rifle_CrouchFire` from its start. It plays additive when the clip decodes additive; otherwise it is
    a masked override ramped like the reload.
  - Stand and prone have no unique rifle fire controller (m2a.md §9). They add `kCrowdFireKickDeg` = 3° of aim pitch
    as a triangle over `kCrowdFireKickSeconds` = 50 ms [inferred].
  - The first `fire_seq` an evaluator sees never fires.
  - A dead soldier neither fires nor aims.
* **Reload.** This is S3's reload start (`crowd_flags::kReloadStart`) on the soldier's weapon's clip at its rate.

### 2.4 Death (§16 Q13)

* `CrowdDeathStyle::BlendToProne` is the default. The pose blends from the alive pose to the prone idle over 0.5 s.
* `CrowdDeathStyle::Clip` plays the stance's death clip and holds its last frame. It falls back to BlendToProne when
  the clip is missing.
* Either way, the soldier is hidden 3 s after `dead` rose: `drawn()` is false and `weapon_of` is `kNoCrowdWeapon`.
* When `dead` falls back to false (a respawn), the soldier restarts alive.
* Death times run on the input clock (set_inputs' dt), so temporal LOD skips do not stretch them. A dying soldier is
  always due.

### 2.5 The 3p muzzle

`muzzle_model(i, r_xform)` gives the muzzle in the soldier's model space: the rig's muzzle joint pose × the item's
`muzzle_offset`. Bf3GameCrowd multiplies it by the root. weaponske01 has no muzzle joint (as for 1p, W3.0 G6), so
`load_crowd_weapons` builds the transform as follows:

* `muzzle_joint` = the rig's `Wep_Root`.
* `muzzle_offset` = inverse(weaponske01 model pose[`Wep_Root`]) × T(FireEffects3p entry Offset) × R(entry Euler, order
  Y·X·Z [inferred]) × Rx(+90°).
* The Rx(+90°) term makes the effect's +Y run along the barrel. The 3p meshes' barrel is +Z: dir.z = 1.00 on every
  long gun [verified, `render.crowd_weapons`].
* FireEffects3p is read through `WeaponDef::firing_data` → `WeaponFiringData.PrimaryFire` → `FireEffects3p`. The first
  entry whose Effect contains "muzz" is used, otherwise entry 0. The M67 and the knife have no entry with an offset,
  so they have no muzzle.

## 3. 3p weapon meshes (crowd_weapons.cpp)

* `load_crowd_weapons(install, spec, items)` loads each item's 3p mesh (`<weapon>_3p_mesh`) as its own skinned mesh,
  on worker threads. It produces, per item:
  - the binds against the crowd rig;
  - the reload clip through S3's chooser (`weapon_choice_index`);
  - `weapon_local`;
  - the palette and the muzzle (§2.5).
  Extra textures decode with `parallel_for`.
* `strip_team_weapon_parts` removes the weapon S3 baked into each team's mesh before upload, so no drawn vertex reads
  the team's default-weapon binds.
* **Item lists** (`crowd_item_specs`, game_crowd_items.h): item 0 is the SoldierSpec weapon (S3's), followed by the
  level's kit defs (kit slots 0, 1, 6 and 7), unique, up to 8. `crowd_weapon_index(weapons, team, def)` maps a
  `WeaponDef` index to the team's item.
* **Reload rate** = the clip's duration / `WeaponDef` fire.reload_time, clamped to [0.5, 2] [inferred; s3.md Q3 played
  the clip at rate 1].
  - US: M16A4 1.336, M9 1.354, M67 2.0 (clamped), M4A1 1.277, M27 IAR 1.160, MK11 1.319.
  - RU: AK-74M 1.161, MP443 1.667, AKS-74u 1.200, RPK-74M 1.208, SVD 1.091.
  - The knife has no reload time and plays at rate 1.
* **Failures:** item 0 failing fails the load. Any other item that fails is dropped with a note, and the soldier
  holding it draws item 0 (`unknown_weapons` counts that frame). This is a deviation from failing the whole load; Metro
  TDM drops none.

Metro TDM [verified, `render.crowd_weapons` and `godot.m2a_tp_crowd`]:

| team | items | 3p mesh binds |
|---|---|---|
| US | m16a4, m9, m67, knife, m4a1, m27iar, mk11 | 5, 5, 2, 10, 5, 6, 5 |
| RU | ak74m, mp443, m67, knife, aks74u, rpk-74m, svd | 5, 6, 2, 10, 5, 5, 5 |

* 10 of the 14 items have a muzzle.
* Item 0 matches S3's baked binds: 5 of 5 per team.

## 4. Bf3GameCrowd (game_crowd.cpp)

* **Load** (worker thread, from `M2aLoadInputs`), in this order:
  - S3's soldier assets;
  - the item lists and `load_crowd_weapons`;
  - lc's server pose clips and tp's move clips on the same package;
  - `strip_team_weapon_parts`, `measure_aim` and the two `CrowdTeam`s with their `weapons`.
* **Build** (main thread):
  - an evaluator for 256 soldier indices on `crowd_lanes(hc, true)` lanes;
  - S3's uploader (RS skeletons) and the 3p weapon meshes.
* **Soldier index** = crowd team × 128 + wire slot. `net::kMaxSlots` is 128, and 256 = `kMaxCrowdSoldiers`. The team
  is `crowd_team_of_wire(PresentedOtherWeapon::team)`: wire 1/2 → crowd 0/1. A slot that changes team hides its old
  index.
* **Inputs per frame** (`step`): position, velocity, yaw, pitch, pose, target_pose, transition, camera distance from
  `present_others`, plus three fields from `present_other_weapon`:
  - def → `crowd_weapon_index`;
  - `reload_seq` changes → `kReloadStart`;
  - ads.
  Fire comes from RemoteShot events (slot = shooter), or, when the stream carries none, from `firing` on a new
  `sample_tick`.
* **Events** (`PresentEvent`) are applied when their timeline's clock reaches them, so the Render timeline matches the
  presented positions:
  - Death (aux = victim) starts the death;
  - Spawn ends it;
  - a new `generation` is a new life.
  A corpse keeps its last pose until it is hidden.
* **Step order:** build_inputs → `wait_and_publish` → upload (body palettes) → draw_weapons → `set_inputs` → kick.
  draw_weapons covers the weapon instances, their skeletons and the muzzles.
  - Each drawn soldier has one RS instance and skeleton for its weapon, rebased when the item changes.
  - A kick failure counts in `frozen_frames`.
* **Muzzles** for fx3 (`fx::MuzzleSample`) are published after `wait_and_publish`:
  - slot = wire slot, `first_person` false;
  - transform = root × `muzzle_model`, in Godot space (= sim space);
  - velocity = the soldier's.
* **Headless:** the dummy RenderingServer refuses instances and skeletons. Bf3GameCrowd then poses without drawing
  (`draw` false) and still publishes the muzzles, so the functional row runs headless.

## 5. Bf3TpCrowdCheck and what gx still wires

gx owns `level_session_m2a.cpp`'s wave-3 hooks and has not wired `Bf3GameCrowd` into them yet. The session also loads
gunplay only with `SharedLoadOptions::gunplay`. Until then, `res://scenes/m2a_tp_crowd.tscn` (Session + TpCrowd) is the
crowd's runtime proof. Its setup:

* `Bf3TpCrowdCheck` loads the crowd as the session would (`M2aLoadInputs`: the install, the SharedLevel with its
  GunplayLevel, the cache), builds it under itself and drives it with a scripted `M2aFrame`, one 1/60 s step per
  display frame.
* The script has 12 presented soldiers on S3's validated Metro stage: 2 rows of 6, slots 10 to 21, both teams.
* Every kit item is held at least once.
* The script covers stand, crouch and prone, a stand→crouch→prone cycle, a moving crouch, fire events and a reload.
* Soldier 9 dies at 2.0 s and respawns at 7.0 s.
* At 6.0 s one soldier switches from the M16A4 to the M9.

With `--tp-shots=<dir>` (windowed) it saves screenshots at 1.5, 2.25, 2.6 and 6.5 s.

## 6. Gates and measured results (2026-10-18)

| gate | threshold | measured | met | command |
|---|---|---|---|---|
| S3 G0 / anim.s3.unit after R5 | the S3 rows bit-equal; every existing case passes | case "M2a tp R5: one weapon per team…": an evaluator with `CrowdTeam::weapons` = { S3's weapon } vs S3's, 30,720 soldier-frames bit-equal with temporal LOD off and 30,720 with it on (body palette, weapon palette, aim, reload); stand with the stance set bit-equal to S3 too. Every pre-existing S3 case passes unchanged: `s3.contract`, `anim.s3.unit`, `anim.s3`, `render.s3`, `anim.server_pose`, `m2a.contract3` | yes | `ctest --preset dev -R "^(s3\.contract|anim\.s3\.unit|anim\.s3|render\.s3|anim\.server_pose|m2a\.contract3)$"` |
| R5 two weapons per team | per-soldier binds, reload, weapon_local; teams with different bind counts; refusals | case "M2a tp R5: two weapons per team…": each soldier's weapon palette from its item, its own reload clip and layout, team B with 3 body binds (the binds-init fix), out-of-range index → item 0 counted, reload cancelled on switch, muzzle per item; 697,485 assertions in the three new unit cases; anim.s3.unit 0.65 s | yes | `ctest --preset dev -R "^anim\.s3\.unit$" -V` |
| E18 (b) "capsules" | crouch and prone, 32 mid-pitches, head and chest vs `CrowdEvaluator` ≤ 3 cm | max over 32 mid-pitches, head / chest: stand 0.013 / 0.002 cm, **crouch 0.077 / 0.023 cm, prone 0.179 / 0.044 cm**; stance_fallbacks 0; 0 stand-in rows. Reported: transitions vs the table at its steps (the table is lerped between 16 fractions, the evaluator is exact), max head / chest S→C 1.01 / 0.63, S→P 2.45 / 3.09, C→S 0.79 / 0.46, C→P 1.69 / 0.99, P→S 1.56 / 0.86, P→C 2.47 / 1.20 cm; transitions vs the E18 reference evaluation 0.000; moving crouch at 2 m/s about 33 cm from the steady row and prone at 0.8 m/s 9.7 / 21 cm (locomotion poses, not drift: the hips track the idle) | yes | `ctest --preset dev -R "^anim\.s3$" -V` (case "capsules", install) |
| 3p weapon meshes | every kit item loads; item 0 = S3's binds; muzzle along the barrel | 7 + 7 items, 0 dropped; item 0 5 / 5 binds equal per team; 10 muzzles, long guns: muzzle 5 to 13 cm short of the mesh tip, lateral < 0.05 m, dir.z 1.00; 14 weapon palettes from the evaluator; install unchanged; 2.9 s (assets 1.32 s, gunplay 1.37 s, weapons 0.34 s, indicative) | yes | `ctest --preset dev -R "^render\.crowd_weapons$" -V` |
| playable: right weapon, stance, fire / reload / death at the presented positions | the check's rows | `M2A TP CROWD CHECK: PASS`: 12 drawn, 12 weapon instances, 10 muzzles, 0 unknown weapons, soldier 0's muzzle +Y along the view (cos 1.000), 1.05 m ahead at 1.40 m; the dying soldier drawn at 2.4 s, hidden at 5.5 s (11 drawn, 9 muzzles), respawned at 8.0 s (12, 10); the weapon switch rebases 1 instance; 0 frozen frames of 540; crowd load 3.44 s alone (indicative); 9.4 s alone, 39 s inside the 16-job pass | yes | `ctest --preset dev -R "^godot\.m2a_tp_crowd$" -V` (OPENBF3_GODOT set) |
| playable, pixels | screenshots of the scripted moments | windowed run, `M2A TP CROWD CHECK: PASS` with the same rows as headless (0 frozen frames of 540, crowd load 3.52 s). 4 shots `tp_crowd_0..3.png` in `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\tp\` (gitignored). They show the stand, crouch and prone soldiers holding their weapons, the dying soldier lying prone at 2.6 s and gone at 6.5 s. The lower half of each frame is the background below the stage's floor edge: that is how the scripted stage is framed, not a crowd defect | yes | `"%OPENBF3_GODOT%" --path bf3-opensource res://scenes/m2a_tp_crowd.tscn -- --tp-check --tp-shots=<openBF3>\b\shots\tp --walkers=0 --no-hud --bf3-cache=b\dev\godot-cache\godot.m2a_tp_crowd` (windowed) |
| godot.s3_crowd functional | `S3 CROWD CHECK` functional rows pass | Both runs pass the functional rows: G0-stage, G0-config, G0-view (64 of 64), G0-blend (two-cycle 44.4 %, idle mix 4.1 %, 1.49 clips per soldier-frame), G3 (16 of 16 shots) and install unchanged. G1a passes at step p50 0.278 ms, p99 0.532 ms (≤ 2.0). Two timing rows fail, so the row exits non-zero: G1b (2.43 and 2.02 ms vs 2.0) and G2 (frame p99 23 to 33 ms vs 16.7). The machine was shared with other tracks' builds at about 60 % CPU; R0, the level alone without the crowd, was already at p99 13.3 ms. Branch, unlocked, shared machine: indicative only (rule 7). godot.s3_crowd uses the plain S3 evaluator, which R5 keeps bit-equal | functional yes; timing not judged here | `set OPENBF3_GODOT_WINDOWED=1 && ctest --test-dir b\dev -R "^godot\.s3_crowd$" -V` (OPENBF3_GODOT and OPENBF3_USER_ACTIVE_FLAG set; the dev preset leaves window rows out) |
| branch pass | every row passes, no budget violation | **Run 1:** 130 rows, 128 passed, 2 skipped (`core.cache_data` and `core.res`, as on W3.0), 0 failed, 372 s wall. Strict found 2 violations: `render.crowd_weapons` (34 s) and `godot.m2a_tp_crowd` (39 s) ran over 30 s without COST. Fixed with COST (9bba82d). **Run 2:** 128 passed, 2 skipped, 0 failed, 455 s wall. Strict found 4 violations on rows that take 2 to 5 s alone, which tp does not own or only adds a case to: `anim` 34.0 s, `render` 37.2 s, `anim.s3` 54.9 s, `render.s3` 63.8 s. Run alone right after, they took 1.8, 2.3, 4.5 and 4.2 s. Other worktrees were loading the machine (W3.0 measured the same pass at 178 s) | rows yes; strict no on a loaded machine (§7) | `tools\build.cmd dev --strict` (OPENBF3_GODOT set) |
| asan | the new code clean under ASan | 7 of 7 passed under asan with no AddressSanitizer report: `s3.contract` 0.14 s, `anim.s3.unit` 3.5 s, `anim.server_pose` 36.9 s, `m2a.contract3` 0.17 s, `anim.s3` 58.6 s (incl. "capsules"), `render.s3` 72.4 s, `render.crowd_weapons` 81.8 s (-j 4) | yes | `tools\build.cmd asan --no-test` then `ctest --preset asan -R "^(anim\.s3\.unit|anim\.s3|render\.crowd_weapons|m2a\.contract3|s3\.contract|anim\.server_pose|render\.s3)$" -j 4` |
| install.unchanged | layout.toc, cas.cat, .sb/.toc unchanged | unchanged in `render.crowd_weapons`, `anim.s3` and `godot.m2a_tp_crowd` | yes | the rows above |
| S3 G1a inside E14 (timing) | ≤ 2.0 ms p50 and p99 | not run (rule 7: timing runs on main after the merge) | n/a | gx's E14 run on main |

## 7. Deviations and open issues

* **Case "capsules" is in `anim.s3`, not `anim.s3.unit`.** It needs the install (the server pose clips and the capsule
  table from bf3_sim), and `anim.s3.unit` is the data-free row. `test_anim_s3` now links `bf3_sim`.
* **Non-zero kit items that fail are dropped, not fatal** (§3). Item 0 still fails the load.
* **Reload rate rule** (§3) is new [inferred]. S3 played its one reload at rate 1.
* **Handgun muzzle.** The M9's and MP443's FireEffects3p offset is (0, 0.05, 0) from `Wep_Root`. That puts the flash
  at the grip, while the 3p mesh reaches about 0.5 m. The long-gun check covers rifles only. Either BF3 positions the
  handgun 3p effect another way (a different joint or the 3p mesh's own socket), or that entry is not the muzzle flash.
  This stays open for fx3 / a later track.
* **Transition rows vs the capsule table** are up to 3.09 cm (S→P chest). The evaluator samples the clip exactly at
  the fraction, while the table lerps between its 16 fractions. Against the E18 reference evaluation the transition
  error is 0.000. E18's transition gate (5 cm) is lc's table vs the reference and is not affected.
* **gx wiring.** Bf3GameCrowd is not called by the level session yet (§5). gx wires it into `level_session_m2a.cpp`'s
  hooks, sets `SharedLoadOptions::gunplay`, and passes the muzzles to `Bf3GameFx`.
* **E14 cost.** Each drawn soldier has a second RS instance and skeleton (the weapon, 2 to 10 bones), so 64 soldiers
  mean 64 more instances. The extra pose work is the weapon palette and the muzzle. S3 G1a with these inputs is
  measured in E14 on main (rule 7) and has not been measured here. The crowd lane rule's re-measure (m2a.md §3) is
  gx's.
* **Stand fire** is the 3° kick. There is no unique stand rifle fire controller, and none was found through a chooser.
