# PT1 run: the running animation

Status: implemented on 2026-10-19, branch `pt1/run` (based on main 6a4061e). The finding comes from the first M2b
playtest: "Also the running animation is wrong and too fast."

Tags:
* **[verified]**: checked in this tree or on the install on 2026-10-19, by the command given.
* **[measured]**: a number from a run on this machine.
* **[inferred]**: not checked against BF3 itself (the Ant data gives the shape, not the runtime rule).
* **named default, measured = false**: a value the data does not give. It is a named constant, and this document
  says so.

---

## 1. What was checked

The finding covers three things:

* **3p bots running:** the crowd evaluator (`crowd_eval.cpp`, M2a path, drawn by `Bf3GameCrowd`).
* **The own 1p arms and weapon while running:** `ViewmodelAnimator` (`viewmodel_anim.cpp`), drawn by
  `Bf3Viewmodel`.
* **Ground speed:** the motor (`motor_tuning.cfg`, read from PhysicsMotion).

For each one this document gives:

* the clip chosen per direction, against BF3's Ant data;
* the playback rate: the clip's FPS and length against how fast our time advances;
* the blend parameters;
* foot sliding: the planted toes' stride against the ground distance per cycle;
* whether the motor's speeds match the data.

## 2. BF3's data [verified, `antdump objects --install s_basicassets <type>`]

### 2.1 3p move groups

BF3 drives 3p locomotion with three StreetMoveGroupControllerAssets in s_basicassets:

| Group | Stance |
|---|---|
| `MP_RunMoveGroup_PC` | stand |
| `MP_SprintMoveGroup` | sprint |
| `MP_SneakMoveGroup_PC` | crouch |

Prone has no move group.

A move group has two levels:

* **Outer BlendArrayAsset:** indexed by the movement angle X, in radians.
* **Inner BlendArrayAsset per direction:** indexed by speed, in metres per Ant tick. Each item is a
  StreetMoveGroupItemControllerAsset with ControllerAsset, ItemSpeed, ItemStartPhase, ItemAchievedAngle and Velocity.

The Ant clock is 60 ticks/s: FPS 30 × 1 / FPSScale 0.5. Rifle_RunFwd has NumTicks 36 and EndFrame 18.

The controllers are routed through choosers. An IndexChooser is indexed by WeaponTypeEnum (Rifle 0). A signal
chooser takes its first entry whose signals all match their BoolAsset Default. SwitchLoco_SIG is false, so it picks
Rifle_RunLeft1, Rifle_RunRight2, Rifle_SneakLeft2 and Rifle_SneakRight2.

`move_group.h` reads this. The data as read and the clips' measured root motion (`anim.pt1_run`, case "move groups"):

| Clip (moving item) | X | Velocity (data) | root × ItemSpeed (measured) | ratio | frames / s |
|---|---|---|---|---|---|
| Rifle_RunFwd | 0 | 4.23 m/s | 4.64 m/s | 0.912 | 18 / 0.600 |
| Rifle_RunRight2 | +π/2 | 2.56 m/s | 2.72 m/s | 0.939 | 15 / 0.500 |
| Rifle_RunLeft1 | −π/2 | 2.60 m/s | 2.60 m/s | 1.001 | 17 / 0.567 |
| Rifle_RunBwd | ±π | 2.96 m/s | 2.97 m/s | 0.997 | 17 / 0.567 |
| Rifle_Sprint | 0 | 5.76 m/s | 5.76 m/s | 0.999 | 18 / 0.600 |
| Rifle_SprintLeft45 / Right45 | ∓π/4 | 5.39 m/s | 5.20 m/s | 1.038 | 18 / 0.600 |
| Rifle_SneakFwd | 0 | 1.98 m/s | 1.98 m/s | 1.001 | 42 / 1.400 |
| Rifle_SneakLeft2 | −π/2 | 0.86 / 1.08 m/s | 0.86 / 1.08 m/s | 1.001 | 48 / 1.600 |
| Rifle_SneakRight2 | +π/2 | 0.87 / 1.01 m/s | 0.98 / 1.01 m/s | 0.889 / 1.000 | 40 / 1.333 |
| Rifle_SneakBwd | ±π | 0.83 m/s | 0.82 m/s | 1.001 | 42 / 1.400 |

The zero-speed item of each direction is a pose: Rifle_RunFwdToIdlePose (run and sprint) or Sneak_ZeroSpeed.

The other group fields:

| Group | OneToOneSpeed | MinSpeed |
|---|---|---|
| Run | 1 | 0.01 m/tick |
| Sneak | 1 | 0.01 m/tick |
| Sprint | 1 | 0 |

Every ItemStartPhase is 0.

The X sign that puts every moving clip's measured root heading on its X is −1. With that sign the worst direction
error is 0.0° in all three groups.

### 2.2 1p deltas

The 1p deltas come from `1P_MovementStateFlow`:

* **Running:**
  * `1P_RunLoopChooser[WeaponType]` → `1P_RifleNoiseLayer` → `Rifle_RunFwdLoopSeq (Looping)` → the sequence's
    actor → **Rifle_RunFwdDelta**.
  * Rifle_RunFwdDelta is additive and 110 frames long. The sequence plays StartInTime 28 to EndInTime 180 ticks,
    which is frames 14 to 90.
  * The rate is `1P_RunSpeed_ESIG` = OutMovementSpeed / 0.06 m/tick, so rate 1 is 3.6 m/s.
  * The moving threshold is OutMovementSpeed > 0.0015 m/tick (0.09 m/s).
* **Sprinting:**
  * `Wep_SprintDeltaChooser[WeaponType]` → `Rifle_SprintDeltaSeq (Looping)` → **Rifle_SprintDelta**.
  * Rifle_SprintDelta is 72 frames long, played whole at rate 1.

The other weapon types route to their own clips:

| Weapon | Run delta | Sprint delta |
|---|---|---|
| Fnminimi (LMG) | LMG_RunFwdDelta, frames 14 to 90 | Rifle_SprintDelta |
| Smaw (AT) | AT_RunFwdDelta, 133 frames | AT_SprintDelta (via `1P_AT_SprintDeltaChooser`) |
| Glock18 (Pistol) | Pistol_RunFwdDelta, 133 frames | Pistol_SprintDelta, 76 frames |
| M40a5 and M1014 | Rifle clips | Rifle clips |

A weapon's type comes from WeaponStates[0].Weapon (an AntRef) → WeaponAsset → WeaponClassEnum →
EnumerationValueAsset.Value. M16A2 is type 0.

### 2.3 Motor

The motor matches PhysicsMotion:

* stand 4.0 m/s, non-forward × 0.75;
* sprint 6.5 m/s;
* crouch 2.0 m/s, non-forward × 0.7.

**The ground speed is not a cause.**

## 3. Root causes (before the fix, with evidence)

The numbers below come from `ctest --preset dev -R "^anim\.pt1_run$"`, case "cycle rate and stride".

That case evaluates one soldier through the crowd evaluator for 5 s at 60 Hz, at the motor's speeds. It then
measures:

* **Cycle:** the time between successive plants of the left toe. A toe is planted while it is within 1 cm of its
  lowest point.
* **Stride/ground:** the planted toes' backward speed in model space, divided by the body's ground speed. 1 means
  no sliding.
* **Toe slide:** the planted toes' speed in world space.
* **Authored:** the same measure on the clip itself, at rate 1, moving at its own root speed. This is the floor
  that the clip's own data allows.

### RC1: the 3p sprint had no sprint clip

S3's run blend (`locomotion_weights`) chose from the four run cycles only. At 6.5 m/s it played Rifle_RunFwd at
1.40× its authored rate:

* cycle 0.427 s (281 steps/min);
* kSprinting was never read.

BF3 switches to MP_SprintMoveGroup. Rifle_Sprint is authored at 5.76 m/s and plays one to one with speed above that
(OneToOneSpeed 1): rate 1.25 and cycle 0.479 s (251 steps/min) at 6.5 m/s.

**This is the "too fast" for 3p bots.**

### RC2: a hand-made blend instead of the move group

S3 picks the playback rate as speed / blended root speed, clamped to 0.5–1.6. It uses Rifle_RunRight1.

BF3 plays every clip at its ItemSpeed below the item's Velocity and uses Rifle_RunRight2 (§2.1). The results:

| | S3 (before) | BF3 data (after) |
|---|---|---|
| Run forward at 4.0 m/s | 0.693 s | 0.600 s |
| Strafe right at 3.0 m/s | 0.569 s (Right1) | 0.498 s (Right2) |

So at a plain run our 3p cycle was **slower** than BF3's. The new cadence is faster: 200 instead of 173 steps/min.

### RC3: the crouch rate clamp

tp's crouch cycle (Rifle_SneakFwd, authored at 1.035 m/s) had to play at 1.93× at 2.0 m/s. The 1.6 clamp held it
there:

* stride/ground 0.820;
* toe slide 0.36 m/s;
* cycle 0.878 s.

BF3's sneak group authors a second, faster SneakFwd item at 1.98 m/s.

### RC4: the 1p procedural bob instead of BF3's deltas

The animator bobbed the weapon in camera space:

* sideways at 1.8 Hz × min(speed / 4, 1.5);
* up and down at twice that.

At sprint that is 2.7 Hz sideways and 5.4 Hz up and down.

BF3's Rifle_SprintDelta sways once per 18 frames sideways (1.67 Hz) and once per 9 frames up and down (3.33 Hz).
Its run delta advances with speed / 3.6.

Measured on the 1p rig (`anim.pt1_run`, case "1p deltas"):

* **Before:** the bob's offset.
* **After:** Wep_Root in CameraJoint's frame.
* Frequency is counted by mean crossings over 10 s, after 1 s.

| | sideways | up and down |
|---|---|---|
| before, run 4.0 m/s | 1.80 Hz, 20.0 mm | 3.60 Hz, 10.0 mm |
| after, run 4.0 m/s | 1.70 Hz, 11.1 mm | 3.50 Hz, 7.6 mm |
| before, sprint 6.5 m/s | 2.70 Hz, 30.0 mm | 5.40 Hz, 15.0 mm |
| after, sprint 6.5 m/s | 1.60 Hz, 64.4 mm | 3.30 Hz, 11.9 mm |

**This is the "too fast" for the own weapon:** at sprint the bob was 1.7× BF3's frequency, with the wrong shape.

## 4. The fix

### 4.1 3p

**`move_group.h` / `move_group.cpp` (new, Godot-free):**

* `read_move_group` reads a StreetMoveGroupControllerAsset. Choosers, loops, layers and sequences are followed to
  one clip controller (`route_ant_controller`, at most 8 hops).
* `WorkBudget` is charged on every scan and hop.
* Arrays are capped at 16 directions, 8 entries and 32 clips. Non-finite numbers fail the read.

`move_group_weights` is the model [inferred from the data's shape]:

* X = x_sign × atan2(x, z), wrapped into [Min, Min + 2π).
* The two directions that bracket X get linear weights.
* In each direction, the two entries that bracket the speed get linear weights. Each plays at its ItemSpeed.
* Above the last entry, the rate is ItemSpeed × (1 + OneToOneSpeed × (v / V − 1)).
* One normalised phase is shared by all terms. It advances at the weighted mean of rate / seconds of the moving
  terms.

**Named default `kMoveGroupVelocityFromRoot = true` (measured = false):** an entry's velocity is its clip's measured
root speed × ItemSpeed, not the data's Velocity. The two differ by up to 9 % (Rifle_RunFwd 0.912). With the data's
Velocity the worst stride error is 14.3 %; with the root speed it is 2.4e-7 (`anim.pt1_run.unit`).

**`crowd_clips` PT1 block, `load_crowd_move_groups`:**

* loads the three groups' clips on the crowd rig;
* measures each clip's root motion;
* derives the X sign.

**`crowd_eval` (`[movegroup]`, `[path]`, PT1 block):**

* Stand and crouch draw from the run and sneak groups. The sprint group blends in by the soldier's kSprinting flag
  over `kCrowdSprintBlendSeconds = 0.2` (named default, measured = false).
* Below MinSpeed the soldier fades to the stance idle over `kCrowdMoveBlendSeconds = 0.2` (named default,
  measured = false).
* Prone, and any group that failed to load, keep the old cycles.

**`game_crowd.cpp` (`[load]`, `[stance]`, `[sprint]`):**

* loads the groups;
* hands them to the evaluator;
* passes kSprinting.

### 4.2 1p

**`viewmodel_set` (`[decl]`, `[load]`, PT1 block):** each item gets its run and sprint deltas:

* weapon type from its WeaponStates[0].Weapon;
* both choosers routed;
* the sequence window converted to clip frames (ticks × FPSScale).

A missing or non-additive clip leaves the item on the bob, with a note.

**`viewmodel_anim` (`[api]`, `[loco]`, PT1 block):**

* The deltas are applied as additive layers after the action layer (LocalPost).
* **Run weight:** moving × (1 − sprint weight) × (1 − ADS). The moving weight ramps over sprint_blend_seconds. Its
  frame advances at fps × speed / 3.6 within the window.
* **Sprint weight:** the base layer's sprint weight, frame at rate 1.
* With deltas the bob's target is 0.
* Taking the run delta out while aimed is a named default (measured = false). BF3's aimed moves are other state
  nodes, not read here.

**`viewmodel.cpp` `[loco]`:** select_item hands the item's deltas to the animator.

### 4.3 Captures

* **`--pt1-off` (`[ab]`):** keeps the old cycles and the bob, so before and after come from one build.
* **`tp_crowd_check --pt1-run` and `m2a_viewmodel --pt1-run-shots` (`[shots]`):** the capture sequences in §5.3.

## 5. Before and after

### 5.1 3p

`anim.pt1_run`, case "cycle rate and stride" [measured]:

| Gait | Clip (after) | Before: cycle, stride/ground, toe slide | After: cycle, stride/ground, toe slide | Authored stride/ground |
|---|---|---|---|---|
| Run forward, 4.0 m/s | Rifle_RunFwd | 0.693 s (173/min), 0.988, 0.07 m/s | 0.600 s (200/min), 0.978, 0.19 m/s | 0.988 |
| Run left, 3.0 m/s | Rifle_RunLeft1 | 0.490 s, 0.990, 0.25 m/s | 0.490 s, 0.990, 0.25 m/s | 0.988 |
| Run right, 3.0 m/s | Rifle_RunRight2 | 0.569 s, 0.989, 0.08 m/s | 0.498 s, 1.016, 0.15 m/s | 1.018 |
| Run back, 3.0 m/s | Rifle_RunBwd | 0.561 s, 0.988, 0.09 m/s | 0.561 s, 0.988, 0.09 m/s | 0.988 |
| Sprint, 6.5 m/s | Rifle_Sprint | 0.427 s (281/min), 0.988, 0.10 m/s | 0.479 s (251/min), 0.928, 0.49 m/s | 0.928 |
| Crouch forward, 2.0 m/s | Rifle_SneakFwd | 0.878 s, 0.820, 0.36 m/s | 0.723 s, 0.990, 0.04 m/s | 0.987 |
| Crouch left, 1.4 m/s | Rifle_SneakLeft2 | 1.058 s, 0.993, 0.03 m/s | 0.989 s, 0.974, 0.06 m/s | 0.976 |

The checks:

* After the fix, stride/ground is within 3 % of the clip as authored.
* The cycle is within 4 % of the data's 1 / cycles_per_second.

At sprint the planted toes move at 0.928 of the root's speed **in the clip as authored**. With BF3's one-to-one rate,
the toe slide rises from 0.10 to 0.49 m/s. The before value was lower only because the run clip, played fast, slid
less. It is still the wrong clip at the wrong cadence. §7 lists this for the user to judge.

### 5.2 1p

The table is in §3, RC4. The rows check:

* **Sprint:** sideways 1.67 Hz ± 8 % and up and down 3.33 Hz ± 8 %, and the bob was more than 1.5× faster.
* **Run:** sideways 1.67 Hz ± 8 %. This is one sway per stride pair, like the 3p run at 4 m/s [inferred
  correspondence].

### 5.3 Captures

The captures are in `openBF3\b\shots\pt1-run\` (gitignored). They are windowed and come from one build.

**3p** (`3p_before\`, `3p_after\`): `m2a_tp_crowd.tscn -- --pt1-run [--pt1-off] --tp-shots=<dir>`. This is a 0.6 s
sequence at 0.05 s steps of 12 soldiers in place:

* 1 runs at 4.0 m/s;
* 2 sprints at 6.5 m/s;
* 10 strafes left at 3.0 m/s;
* 11 crouches at 2.0 m/s.

`*_strip.png` shows every second frame.

Pixel measure: the mean |frame k − frame 0| per 40 px column, over y 230–430. The strongest moving column (x 840):

| | Motion | Best repeat | Distance there | Mean distance |
|---|---|---|---|---|
| Before | 10.9 | none clear | 20.4 | 21.0 |
| After | 17.8 | 0.35 s | 6.6 | 19.6 |

The after value is one step of a two-step cycle, which matches the crouch's 0.723 s / 2 [inferred: which soldier
that column holds was not separated].

**1p** (`1p_before\`, `1p_after\`): `m2a_viewmodel.tscn -- --pt1-run-shots [--pt1-off] --vm-shots=<dir>`. This is
the M16A4 at hip, running (hip mark + 0.2 s) and sprinting (sprint mark + 0.2 s), 8 frames at 0.05 s each.
`*_grid.png` shows them.

Weapon top edge (y) and centroid of the dark pixels (x), in pixels at 1152 × 648:

| | Run: top y range | Run: x range | Sprint: top y range | Sprint: x range |
|---|---|---|---|---|
| Before | 21 px, minima 0.25–0.3 s apart (the 3.6 Hz bob) | 37 px | 28 px, minima every 0.2 s (5 Hz) | 33 px |
| After | 11 px | 13 px | 35 px, one slow swing (lowest at 0.25 s) | 112 px over 0.35 s (half of a 1.67 Hz sway) |

## 6. One command per check

| Check | Command | Result (2026-10-19) |
|---|---|---|
| Clip choice, cycle rate per speed, stride identity, hostile input, crowd evaluator, 1p animator (data-free) | `ctest --preset dev -R "^anim\.pt1_run\.unit$"` (= `b\dev\tests\test_pt1_run.exe --test-suite=pt1-run-unit`) | 7 cases, 359 assertions, pass, 0.36 s |
| Move group data, chosen clips, cycle and stride per gait before / after, 1p routes and cadence, install unchanged | `ctest --preset dev -R "^anim\.pt1_run$"` (= `b\dev\tests\test_pt1_run.exe --test-suite=pt1-run`) | 4 cases, pass, about 1.6 s |
| The data | `b\dev\tools\antdump\antdump.exe objects --install s_basicassets StreetMoveGroupControllerAsset` | §2 |
| Asan (the new readers and the delta layer) | `ctest --preset asan -R "^anim\.pt1_run"` | both pass: 0.06 s and 18.0 s |
| Captures | the two commands in §5.3 | §5.3 |

## 7. For the user to judge

* **The 3p cadence at a plain run is now faster** (200 instead of 173 steps/min at 4 m/s). It follows BF3's data:
  Rifle_RunFwd at rate 1 below its item velocity.
* **3p sprint toe slide** is 0.49 m/s, as the clip is authored (§5.1).
* **1p sprint sway** is 64 mm peak to peak sideways, against a bob of 30 mm.
* **The look in play.** No M2b play was run on the branch (windowed rows run in the merge pass).

## 8. Rows the merge pass should watch

* **`godot.m2a_tp_crowd`:** the crowd now draws BF3's move groups. Its own check passed windowed, with
  `--pt1-run`, on the branch.
* **`godot.s3_crowd`:** the S3 path is unchanged without move groups.
* **`godot.m2b.play` and `godot.m2b_load`:** the move groups load in `load_body`, adding about 0.03 s to the load
  [measured, in-test].
* **`godot.m2a_viewmodel`:** the deltas replace the bob, so the bob spring rests sooner. e19's springs check
  "at most its settle time".
* **`anim.s3` / `anim.s3.unit`:** the synthetic sets are unchanged.

The branch pass (`toolsuild.cmd dev`) had 153 rows: 151 passed, 2 skipped as before, 0 failed, exit 0. The README
section gives the details.
