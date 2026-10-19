# M2b cam: the death view and the 3p corpse, as built

Track cam of M2b wave 2 (docs/design/m2b.md §4, §2.6 #17, §12 J3; the cam row of §11 "Wave 2"; the contract:
docs/design/m2b-w2.md), on branch `m2b/cam` from the W2.0 head 5191059. cam owns the new
`src/bf3_godot/src/death_camera.{h,cpp}`, additive blocks and named insertions in `game_crowd.{h,cpp}` and
`crowd_eval.{h,cpp}` (#17), the defibrillator's slot-2 unlock in the viewmodel set (§4), a `revive` case in
`anim.s3.unit`, and (by the cam task) the `FxTriggers::sane` item that integ-w1 left open. Ragdolls stay out (J3): a
corpse is the M2a prone blend, held.

Where m2b.md or the W2.0 comments and the code disagree, the code was followed; §5 lists each difference. §6 is what gx
has to call; nothing of cam runs in a scene until gx's M2b session does.

## 1. Commits

| commit | what |
|---|---|
| 2372aba | named insertions `[corpse]` and `[revive]` in `crowd_eval.cpp`, `CrowdM2aStats::revives` (struct-member insertion) and a `// M2b cam` end block in `crowd_eval.h` (`kCrowdMaxCorpseSeconds`) |
| 71a3df8 | `anim.s3.unit` case `revive` (an additive `// M2b cam` block at the end of `tests/anim_s3_tests.cpp`) |
| b346e03 | named insertions `[corpse]` and `[revive]` in `game_crowd.cpp` and its `// M2b cam` end block |
| 2bdf835 | `death_camera.cpp` (the body), two additions to cam's own `death_camera.h`, the `# M2b cam` blocks of bf3_godot's and the tests' CMake, `kCamLanded = true` in `m2b_contract2_tests.cpp`, the new row `m2b.cam.unit` (`tests/m2b_cam_tests.cpp`) |
| 301ff37 | named replacement `[revive kind]` in `FxTriggers::sane` (triggers.cpp:1236) and an `fx.triggers.unit` case |
| db8d947 | named insertions `[defib]` in `viewmodel_set.{h,cpp}` (`ViewmodelKit::gadget`, `team_viewmodel_set`) and their `// M2b cam` blocks (`add_assault_gadget`) |
| (docs) | this file and the README section "M2b cam" |

## 2. The 3p corpse and the revive stand-up

**Corpse time.** `CrowdStanceSet::corpse_seconds` (W2.0, default `kCrowdDeathHideSeconds` = M2a's 3 s) reaches the
evaluator's hide test (`[corpse]`, the named replacement at the old crowd_eval.cpp:809). `CrowdEvaluator::make`
refuses a corpse time that is not finite or outside [0, 3600] s with an error. `Bf3GameCrowd::set_corpse_seconds`
(before `build()`) passes it through and replaces the crowd's own corpse test (`[corpse]` at the old game_crowd.cpp:299,
`anim::kCrowdDeathHideSeconds + kCorpseExtraSeconds` -> `corpse_seconds + kCorpseExtraSeconds`). It clamps a hostile
value to `kCrowdCorpseSeconds` (logged); after `build()` it is ignored (logged). M2a never calls it, so M2a's crowd is
unchanged.

**A corpse outlives its slot (the W2.0 open issue).** The crowd draws one soldier per crowd index, team x 128 + slot,
and a bot respawns 5-8 s after its death, before 15 s. With `set_corpse_seconds` on, every slot below 64
(`net::kMaxPlayerRows`; slots >= 64 are spectators in TDM and are not drawn by the M2b crowd, counted) owns two
indices, its two **lanes**: team x 128 + slot and + 64 (all 256 indices already exist in S3's uploader, so nothing is
rebuilt). When a new life of the slot shows (a new generation in `present_others`) while its corpse is still drawn,
the corpse moves to a `CamCorpse` record on the lane it died on and keeps its pose, place, team and time, and the new
life is drawn on the other lane. An older corpse on that lane (two deaths within 15 s) is cut short, counted. The
corpse records end at `corpse_seconds + kCorpseExtraSeconds`, then the index is hidden.

**The own corpse.** The own soldier is never in `present_others`, so the death camera would look at an empty floor.
With `set_corpse_seconds` on, the own Death event (aux = `M2aFrame::own_slot`) makes a corpse at once from the frame's
own state (`own_curr` + `visual_offset`, the yaw, the stance, `own_team`, the held weapon's 3p item), on the own slot's
first lane. The own Spawn event (a deploy, or the own revive: the 1p view takes over) removes it.

**The revive stand-up.** nt's `PresentKind::Revive` (slot = reviver, aux = revived) is applied when it is drained, not
on the render clock: the CombatEvents arrive with the server tick, the snapshots that show the revived life are drawn
an interpolation delay later. It marks the revived slot. The slot's next new generation then counts as the revive:
it stays on the corpse's lane, and its inputs carry `CrowdSoldierInput::revived`. In the evaluator (`[revive]`), `dead`
falling with `revived` set copies the last corpse pose and blends it to the alive pose over
`kCrowdReviveBlendSeconds` (0.5 s) at the same place; a plain respawn stands at once (M2a). A stand-up is due every
frame under temporal LOD, as a death is. A mark whose life does not show within 2 s (`kCamReviveWaitSeconds`
[inferred]) is dropped, counted. `CrowdM2aStats::revives` counts the stand-ups; `shutdown()` logs one line
(`[crowd] M2b corpses (15.0 s): ... kept on the second lane, ... cut short, ... own corpses, ...`).

## 3. Bf3DeathCamera (death_camera.cpp)

Godot-free, Frostbite axes (+Y up, the `fx::Transform` of `M2aFrame::camera`, `right = up x forward`, the
`m2a_frame_of` rule). Over the presentation clock t since `begin`:

| t | the view |
|---|---|
| [0, orbit 1.0 s) | from the 1p eye to the point `back_m` 2.5 m behind the corpse (against the death's view yaw) and `up_m` 1.6 m above its feet; the look point moves from 5 m straight ahead (where the 1p view looked) to the corpse (`eye_height_m` 0.4 m above the feet); smoothstep |
| [1.0, 1.75 s) | with `MatchState::killer_camera` and a killer drawn at least once: the camera swings to the point 2.5 m behind the corpse on the far side from the killer, 1.6 m up, and the look point moves to the killer (`killer_height_m` 1.2 m above its presented feet; a field cam added to `DeathCameraParams`); smoothstep |
| then | follows the killer's presented position; the last one seen when the killer is no longer drawn |
| >= 4.0 s | `finished` (= `kKillCardSeconds`); the view holds until `end()` |

Without a killer (a fall, the killer camera off, `killer_slot` 0xFF) it stays on the corpse. With a `CollisionWorld`
the camera is pulled in along the ray from the corpse look point (`query_mask::kProjectile`), `camera_radius_m` short of
the first hit and never closer than 0.3 m. Hostile inputs: a non-finite start (corpse, eye, yaw, clock) never begins, a
non-finite clock in `step` ends the view, a non-finite killer position is ignored (the last good one stays); each is
counted in `DeathCameraStats::bad_inputs`. Hostile params fall back to the named defaults one by one. The transform is
always finite and orthonormal (checked over whole views in `m2b.cam.unit`). All of these numbers are named defaults
[inferred, `measured = false`: no capture of BF3's killer camera].

## 4. The defibrillator in hand, and the FX

**The 1p mesh resolves** [verified 2026-10-19, `b\dev\tools\bf3dump\bf3dump.exe vm skin weapons/gadgets/defibrillator/u_defib --team all`]:
kind gun, mesh `weapons/gadgets/defibrillator/defibrillator_1p_mesh`, Ant package `defibrillator`, RenderFov 55,
DeployTime 0.40 s, the Deploy and IdlePose clips decoded, the Fire clip a `CurveAnimationAsset` (not decoded: the
procedural kick stands in, counted as one of its 2 stand-ins), weapon surface 1,399 vertices with bad weights 0,
`VM SKIN: PASS (1 items, 0 stretched surfaces)`, install unchanged.

So slot 2 joins the viewmodel set (`[defib]`): `ViewmodelKit` gains `gadget` (slot 2, "" = none), `team_viewmodel_set`
adds it after the kit's four slots, and `render::add_assault_gadget(kits, unlock)` sets it on the assault kits (a kit
name whose last component ends in "assault"). A TDM load calls it with `host::kTdmAssaultGadget` on
`read_viewmodel_kits`' result, so M2a's item set is unchanged and the kit table's def -> item map (gx's
`m2a_map_items`, which already reads every slot of the loadout) finds the defib. `ViewmodelKit::unlocks` keeps its
4 slots, so the documented table, `viewmodel_kits_difference`, bf3dump and the vm checks are untouched.

**`FxTriggers::sane`** (`[revive kind]`, triggers.cpp:1236): the kind test `<= PresentKind::Spawn` becomes
`<= PresentKind::Revive`, so a Revive event is no longer counted as rejected; it starts no effect (the dispatch's
default). A kind past Revive is still rejected.

## 5. Differences from m2b.md and the W2.0 notes

* **R13 is cam's here.** m2b-w2.md §1.1 assigns the `FxTriggers::sane` replacement to gx; the cam task carries it, so
  it is in this branch (301ff37). gx must not make it again: the same line edited on both branches conflicts.
* **The own corpse is new** (not in m2b.md): without it the death camera of §4 looks at the floor, because the own
  soldier is not among the others.
* **The two-lane corpse** is cam's answer to the W2.0 open issue (a bot's corpse ending at its respawn); m2b-w2.md
  allowed either. Its cost is that slots >= 64 are not drawn by the M2b crowd (TDM has none).
* **A Revive event is applied when drained**, not on the render clock (the other crowd events wait for it), because
  the revived life must not show before the mark.
* **`DeathCameraParams::killer_height_m`** and a private counter are added to the W2.0 header (cam's own file); the
  contract's struct checks are about trivial copyability only.
* **The defib goes in `ViewmodelKit::gadget`, not a fifth `unlocks` entry**, so nothing that indexes the four slots
  changes; the caller opts in with `add_assault_gadget`.

## 6. What gx calls (the M2b session)

1. `Bf3GameCrowd::set_corpse_seconds(anim::kCrowdCorpseSeconds)` (or `SoldierDef::time_for_corpse` when > 0) before
   `build()`. This turns on the corpse time, the lanes and the own corpse.
2. In the viewmodel loader (level_session_m2a.cpp, before `team_viewmodel_set`) of a TDM load:
   `render::add_assault_gadget(g->vm_kits, host::kTdmAssaultGadget)`. The input mapper's KEY_3 / slot 2 is gx's.
3. `Bf3DeathCamera`: `begin` on the drained own Death event (`present_flags::kOwnVictim`) with `corpse` = the own feet
   (`own_curr.position` + `visual_offset`), `eye` = the 1p camera position, `yaw_rad` = `sim::yaw_radians(own_curr.yaw)`,
   `killer_slot` = the event's `slot` (0xFF when it is the own slot), `killer_camera` = `frame.match.killer_camera`
   (true before the first MatchState); `step` every display frame while active with the killer's presented position
   (the `M2aFrame::others` entry of that slot, null when absent) and a client `CollisionWorld` if gx has one (null:
   no pull-in); `end()` on the own Spawn event. To a Godot `Camera3D`: `origin = trans`, basis columns
   `x = -right, y = up, z = -forward` (Godot looks down -Z; `m2a_frame_of` is the inverse).
4. Nothing for the revive: the crowd reads the Revive events from `M2aFrame::events` itself.

## 7. Gates, one command each

| gate | command | threshold | measured (2026-10-19, `m2b/cam`) |
|---|---|---|---|
| the branch pass | `tools\build.cmd dev` (with `OPENBF3_GODOT` set) | every functional row passes, no budget violation | 148 rows: 146 passed, 2 skipped as before (`core.cache_data`, `core.res`), 0 failed, 331.3 s wall, exit 0; only the existing budget warnings, none on a cam row; `godot.m2a_tp_crowd` (the headless M2a crowd through game_crowd.cpp) passed |
| the death camera (contract) | `ctest --test-dir b\dev -R "^m2b\.contract2$" --output-on-failure` | cam's case runs (no PENDING (cam)), passes | 4 cases, 95 assertions, passed, 0.05 s |
| the death camera (cam) | `ctest --test-dir b\dev -R "^m2b\.cam\.unit$" --output-on-failure` | passes | 3 cases, 701 assertions, passed, 0.01 s |
| corpse and revive | `ctest --test-dir b\dev -R "^anim\.s3\.unit$" --output-on-failure` | passes (case `revive`: 41 assertions) | passed, 0.44 s |
| Revive in the FX | `ctest --test-dir b\dev -R "^fx\.triggers\.unit$" --output-on-failure` | passes | passed, 0.20 s |
| asan (the four rows above) | `cmake --preset asan`, then `cmake --build --preset asan --target test_m2b_contract2 test_anim_s3 test_fx_triggers`, then `ctest --test-dir b\asan -R "^(m2b\.contract2|m2b\.cam\.unit|anim\.s3\.unit|fx\.triggers\.unit)$"` (from a vcvars64 prompt) | 0 asan reports | all 4 passed (2.02, 1.96, 0.06, 0.04 s), 0 reports. This also covers m2b.contract2's parser case (a W2.0 open item). game_crowd.cpp is Godot-side and is not under asan |
| defib 1p mesh | `b\dev\tools\bf3dump\bf3dump.exe vm skin weapons/gadgets/defibrillator/u_defib --team all` | `VM SKIN: PASS`, install unchanged | PASS, 0 stretched surfaces, install unchanged |

## 8. What the user must judge

The look of the death view (the pull-back, the swing to the killer, the 4 s hold), the 15 s corpses and the 0.5 s
stand-up after a revive, and the defibrillator in 1p. None of it can be captured on this branch: the only scene that
calls cam's parts is gx's M2b scene (not on this branch), so no screenshot exists under `b\shots\cam\`. The judgement
belongs to the wave-2 playtest (m2b.md §1.1 steps 4 and 6). The named defaults are the knobs: `DeathCameraParams`,
`kCrowdCorpseSeconds`, `kCrowdReviveBlendSeconds` (all `measured = false`).
