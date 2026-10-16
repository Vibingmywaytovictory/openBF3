# M2a vm: 1p viewmodel and ADS

Status: implemented on branch `m2a/vm` (wave 2), 2026-10-16. Scope: the vm row of [m2a.md](m2a.md) §15 (wave 2),
§11.1 (`viewmodel_set`, `viewmodel_anim`, `Bf3Viewmodel`), §11.2 (ADS camera and FOV), §5.2 as far as presentation
goes, and the defaults of §16 Q4, Q5, Q6 and Q21. The parts of other tracks that are not merged yet (cl's
`host::present_weapon` and the drained OwnShot / OwnTimed events; sv's zoom rule in `step_weapons_m2a`) are faked
test-locally in `namespace m2a_fake` and named below.

Tags: **[verified]** = read from the retail install through the repo's readers (`bf3dump vm clips`, `bf3dump dump`,
the vm check) on the date above. **[inferred]** = a design choice made here.

---

## 1. Exit gates and how each is measured

Every gate has one command. Timing rows are wall clock on a shared machine with other tracks building, so they are
noisy; the frame-exact rows (E19, E5) are not timing measurements: the check presents synthetic display frames at a
fixed dt (60 and 144 fps) and reads the Godot nodes back, so they are deterministic.

| id | gate (m2a.md) | command | threshold | measured (2026-10-16) |
|---|---|---|---|---|
| E19 unit | ADS, FOV, recoil, springs, refusal (Godot-free) | `ctest --preset dev -R anim.viewmodel.unit` | as E19, on the synthetic gate weapons at 30 / 60 / 144 / 240 fps | 9/9 cases pass; FOV reached within ±1 frame at every rate in both FOV-time modes; recoil worst 1.6e-15°; springs settle in sway 0.567 / bob 0.833 / kick 0.467 / camera 0.500 s against 0.75 / 1.0 / 0.6 / 0.6 configured |
| E19 fov.hip / fov.ads | world vertical FOV 55 hip, 40 iron sights | `ctest --preset dev -R godot.m2a_viewmodel` (= `Godot --headless --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --vm-check`), rows `e19.fov.hip`, `e19.fov.ads` | Camera3D `fov` 55 / 40 ± 1e-3 (KEEP_HEIGHT) at every hip / aimed mark | 128/128 hip marks 55, 64/64 aimed marks 40 |
| E19 vmfov.hip | viewmodel FOV = RenderFov 55 at hip | same, row `e19.vmfov.hip` | weapon and arms `fov_override` == the item's RenderFov at hip | 128/128; RenderFov is 55 on all 12 kit items |
| E19 fov.in_out | FOV reached in the FOV time ± 1 display frame after the press, back after the release | same, rows `e19.fov.in_out.specific`, `e19.fov.in_out.generic`, `e19.fov.generic_0.15` | \|reached − FOV time\| ≤ 1 display frame, 60 and 144 fps, both FOV-time modes (§2.3); `generic_0.15` also asserts that the FOV times in use are 0.15 s | 32 + 32 transitions, worst 1.00 frame in and out (the bias is −1..0 frames, §2.3); generic mode FOV times 0.1500..0.1500 s |
| E19 recoil | camera recoil offset == interpolated predicted recoil | same, row `e19.recoil` | ≤ 1e-4° every frame | 81,550 frames, worst 7.9e-6° (float transform round trip), largest recoil 2.26° |
| E19 springs | sway, bob, kick settle within their configured time | same, rows `e19.springs` and `e19.springs.sway` / `.bob` / `.kick` / `.camera` | `e19.springs`: every unsettled frame ≤ the largest settle_seconds (1.0 s) + 1 frame after the last spring input; at rest at mark + sway / bob settle time. Per spring: not at rest at most its own settle_seconds + 1 frame after its own last input (view rate; motion or sprint; shot; shot) | worst 0.736 s; 32/32 settle marks at rest. Per spring: sway 0.438 s (0.75), bob 0.800 s (1.0), kick 0.458 s (0.6), camera 0.493 s (0.6) |
| E19 zoom_refused | zoom refused while deploying or sprinting | same, row `e19.zoom_refused` (+ unit case) | zoom held in a deploying or sprinting frame presents no ADS and FOV 55 | 6,882 frames, 0 with ADS; the sim half is sv's: the check prints `PENDING (sv)` while the `step_weapons_m2a` stub still sets `kZoomWasDown` (§2.4) |
| E5 viewmodel row | clip start, fire after the finish, clip lengths | same, rows `e5.clip_start`, `e5.fire_after_finish`, `e5.clip_cut`; the table: `bf3dump vm clips` | clip on screen ≤ 1 display frame after `kReloadStarted`; first shot on screen ≤ 1 display frame after `kReloadFinished`; the shot takes the clip off within the 0.1 s crossfade + 1 frame | 96 reloads (tactical and empty, 12 kit weapons' 10 guns, 60 / 144 fps): worst 0.0143 s / 0.0143 s (both < 1 frame at 60 fps), cut within 0.104 s |
| E11 viewmodel | deploy, fire, reload clip or counted stand-in for every item of the 8 loadouts | same, rows `e11.coverage`, `e11.kits_match_data`; `bf3dump vm clips --team all` | 32/32 covered over the loadouts **read from the data** (§2.6); the data's loadouts equal the documented table | 32/32: 42 clips, 46 counted stand-ins, 8 not applicable (the knife's reload); data == documented table |
| look: alpha test | slots the level shader database alpha-tests are cut out | same, row `vm.alpha_test` | every drawn alpha-tested slot has an alpha-scissor material | 0 drawn by default (the only one is the knife's dog-tag chain, hidden, §2.7); 2 of 2 (US and RU knife) with `--vm-dogtags` |
| look: knife dog tags | the knife capture's stray bars (§2.7) | same, row `vm.knife_dog_tags` | the knife's dog-tag surfaces are left out and counted (drawn with `--vm-dogtags`) | 2 knives, 2 surfaces each hidden, 0 drawn |
| skinned geometry | no stretched triangles through deploy, fire and reload | `bf3dump vm skin [--team us\|ru\|all]` | no surface's longest skinned edge > 1.5 × its bind-pose edge (and > 5 cm); all weights sum to 1 | `VM SKIN: PASS` (`--team all`: 14 items, 95 surface rows, 0 stretched; 5.6 s warm; the knife's chain 0.397 m in the bind pose, 0.403 m posed: long by construction, not stretched) |
| bf3dump | `bf3dump vm clips` | `bf3dump vm clips [<unlock>...] [--team us\|ru\|all]` | prints roles, codecs, endpoints, E5 rows, E11 coverage | met (§4) |
| captures | hip, ADS, reload mid-clip for several kit weapons | `Godot --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --vm-shots=<openBF3>\b\shots\vm` (windowed) | PNGs for the user's judgement | 38 captures (§5) |
| asan | hostile data | `tools\build.cmd asan --no-test` then `ctest --preset asan -R anim.viewmodel.unit` | no crash, hang or unbounded allocation in the tuning parser, springs, animator, ADS camera; no AddressSanitizer report | 9/9 cases, 6,155 assertions, no AddressSanitizer report; the tuning fuzz made 3,000 mutations (792 parsed, 2,208 failed cleanly). Also under asan: `bf3dump vm skin --team all` (VM SKIN PASS, 14 items) and `bf3dump vm clips --team all` (E11 PASS 32/32), no report, install unchanged |
| install unchanged | layout.toc, cas.cat, .sb / .toc | the check's last line `[vm] install.unchanged` (also printed by `--vm-shots`, `bf3dump vm clips` and `bf3dump vm skin`) | identical sizes and mtimes of the 256 install files | yes (256 files) |
| existing tests | all existing tests keep passing | `ctest --preset dev -E "<TIMING regex of tools\build.cmd>"`, then `ctest --preset dev -R godot\.` with the quiet lock checked | 0 failures | dev with the timing rows excluded: 90/90 pass in 307 s (core.cache_data and core.res skipped as usual); the 12 godot.* rows one at a time, each after the lock check: all pass (godot.m1 346 s, godot.s3_crowd 545 s, godot.l1_look 239 s on the shared machine) |

Present cost (info only, noisy): `Bf3Viewmodel::present` mean 12.7 µs, max 196 µs per frame over 11,988 frames at
60 fps (the check's info line).

## 2. Design

### 2.1 Modules

| piece | module | thread |
|---|---|---|
| `bf3/anim/viewmodel_anim.h` + cpp: `ViewmodelTuning` and its parser, springs, `ViewmodelAnimator`, `AdsCamera`, `ads_allowed`, `camera_view_angles` | `bf3_anim` (Godot-free) | main (per display frame); the parser anywhere |
| `bf3/render/viewmodel_set.h` + cpp: team sets of the 8 default loadouts, role clips, stand-ins, ADS data, default accessories | `bf3_render` | loader thread |
| `bf3-opensource/config/viewmodel_tuning.cfg` | data | read on the main thread (`FileAccess`), parsed with `parse_viewmodel_tuning` |
| `src/bf3_godot/src/viewmodel.{h,cpp}`: `Bf3Viewmodel` | `bf3_godot` | main |
| `fp_camera.{h,cpp}` M2a vm block: `ViewEffects`, `apply_view_effects`, `camera_view_angles_of` | `bf3_godot` | main |
| `vm_script.{h,cpp}`, `m2a_viewmodel_demo.{h,cpp}`, `m2a_viewmodel_check.cpp`, `scenes/m2a_viewmodel.tscn` | `bf3_godot` | loader thread (install, sets, weapon table, scripted runs), main (scene, check) |
| `tools/bf3dump/vm_commands.{h,cpp}` | bf3dump | main |

### 2.2 One display frame (`Bf3Viewmodel::present`)

1. **ADS**: `anim::ads_allowed(phase, sprinting)` (refused while deploying or sprinting, §16 Q21), then
   `AdsCamera::step(params, zoomed && allowed, dt)`: the FOV progress moves linearly in time over the FOV time of the
   direction, the world FOV is `hip + (zoom − hip) · smoothstep(progress)`, the viewmodel's ADS blend moves over the
   aiming controller's zoom times (0.09 / 0.15 s) [verified data], the look scale goes 1 → `LookSpeedMultiplier`
   (0.5) [verified].
2. **Animator** on the presented phase and timers, the frame's cues (OwnShot → Shot, OwnTimed → the event's cue; an
   empty magazine at `kReloadStarted` plays ReloadEmpty), the ADS blend, speed, sprint and view rates.
   Layers: base (Idle, ZoomIn / ZoomOut clip or IdlePose → ZoomPose blend, ZoomIdle, Sprint) and one action
   (Deploy cuts in; Fire / FireSingle weighted by 1 − ADS; Reload / ReloadEmpty; Bolt; Throw; Knife) with the 0.1 s
   crossfade. Procedural stand-ins (deploy from below, reload lower and raise, throw swing, knife stab, kick) are
   offsets, counted in E11.
3. **Camera**: `apply_view_effects`: the eye, the local view + the interpolated predicted recoil + the presentation
   kick (`ViewmodelOutput::camera_recoil_*`, the camera spring), KEEP_HEIGHT, the world FOV.
4. **Rig**: skeleton = camera · weapon offset (sway + bob + kick springs) · Ry(π) · inverse(model[CameraJoint]); the
   clip's camera motion therefore moves the viewmodel against the camera, not the world camera [inferred: the world
   camera stays on the predicted view so E19's recoil row holds exactly].
5. **Viewmodel FOV**: every weapon, accessory and arms material's `fov_override` = RenderFov at hip, ZoomRenderFov
   aimed (blended with the ADS blend).
6. **Accessories**: the default customization's rigid meshes (§2.5) on their bone, Asset1p under half ADS blend,
   Asset1pzoom from there.
7. **Vignette** (§16 Q6): a procedural CanvasLayer vignette when the aimed zoom level is an optic (FOV ≤ 30); the
   default kits are all iron sights, so it stays off (the check prints 0.00 at full ADS for every item).

### 2.3 FOV transition time: a rule that proved wrong

m2a.md E19 named `fovTransitionTime` (0.15 s) for both directions. The data has more: the iron-sights aiming
controllers (`weapons/common/aimingcontrollers/aim_default_ironsight` and others) carry per-transition times, 0 → 1
FOV 0.12 s / zoom 0.09 s and 1 → 0 FOV 0.15 s / zoom 0.15 s, next to the generic `FovTransitionTime` 0.15 [verified:
`bf3dump dump` of the controllers, `bf3dump vm clips`]. M16A4, M4A1, M9, AK74M, AKS74U and MP443 use 0.12 s in;
M27, MK11, RPK and SVD 0.15 s.

The shipped default (`fov_transition_specific = true` in `viewmodel_tuning.cfg`) uses the per-transition times; false
uses the generic 0.15 s for both directions. The check measures both modes and has a row for the literal E19 number
(`e19.fov.generic_0.15`). m2a.md E19 and §11.2 are corrected in the commit "m2a.md: E19 FOV time follows the aiming
controller's per-transition time (rule fix)".

The ± 1 frame tolerance is used on one side only: the progress counts a whole dt in the first frame that sees the
press, so the FOV lands up to one frame early (−1..0 frames; e.g. 0.1333 s for 0.15 s at 60 fps when the press is on a
frame boundary). Crediting only the time since the press would move the error to 0..+1 frame; both are within E19.

### 2.4 Zoom refusal (§5.2, §16 Q21) and what is faked

The presentation refuses ADS in every deploying or sprinting frame (`anim::ads_allowed`), which is what the user
sees. The sim rule (sv: `step_weapons_m2a` clears `kZoomWasDown` in those ticks) is not merged; the stub forwards to
`step_soldier_weapons`, which follows the input. The check counts the frames where the sim flag is still set and
prints `PENDING (sv)`; once sv lands the rule the line reads "the sim refuses too" with no change here.

Faked in `namespace m2a_fake` (`vm_script.h`): `present_weapon` (cl's `host::present_weapon`: continuous fields
lerp between the two predicted ticks around the display time, discrete fields from the later one) and the OwnShot /
OwnTimed `PresentEvent`s (the run's ShotEvents and TimedWeaponEvents with their sim times). Display times are
rounded to sim units before comparing with event and tick times, so a frame on a tick boundary sees that tick.

### 2.5 Default accessories (iron sights of flat-top rifles)

The first captures showed the M16A4 and M4A1 without sights: their iron sights are not in the 1p mesh but on a
socket. `SoldierWeaponData.Sockets` lists `SocketData` (BoneName, Transform, BoneRigidTransform, UnlockAsset,
AvailableObjects of `WeaponRegularSocketObjectData` with Asset1p, Asset1pzoom and Transform) [verified: `bf3dump
dump weapons/m16a4/m16a4`]. The kits' default customization is "no attachment" in each slot, so the loader attaches
the sockets whose UnlockAsset is a `weapons/common/no…` unlock (the M16A4's `nooptics` socket: `m16_ironsight_1p_mesh`
and `m16_sight_1p_mesh` on `Wep_Root`) [inferred: the default customization; the BF3 UI shows iron sights for a
weapon without an optic]. Loaded: M16A4 and M4A1 (m16_ironsight, m16_sight), M27 (m416_sight), MK11
(general_front_sight, general_back_sight). The composition is BoneRigidTransform · SocketData.Transform · object
Transform [inferred: both socket transforms are identity on these weapons, so the order is not tested]. At most 16
accessories per item; unknown bones and skinned socket objects are skipped with a note.

### 2.6 The 8 loadouts come from the data

`render::read_viewmodel_kits(ebx, level, mode = TeamDeathMatch0)` reads the mode's teams 1 (US) and 2 (RU) with
`read_mode_def`, the progression tables with `read_unlock_progression`, each listed kit with `read_kit_def`, and takes
`KitDef::default_unlock` of slots 0, 1, 6 and 7: the same resolution as sv's `make_kit_table` (bf3_host kits.h). It
fails when a team is missing, lists other than 4 kits, or a kit has no default for one of the four slots. The demo,
`--vm-check` (E11, the E5 runs, the captures) and `bf3dump vm clips` / `vm skin` iterate over what it returns; the
former hard-coded table is kept only as `documented_viewmodel_kits()`, the m2a.md §5.6 list, and is compared with the
data (row `e11.kits_match_data`; `bf3dump vm clips` prints the comparison and fails on Metro when they differ). The
read takes about 0.3 s on MP_Subway [verified: `bf3dump kits MP_Subway TeamDeathMatch0`, 18,053 work units].

### 2.7 Material looks and the knife's dog tags

The first knife captures showed long thin bars from the hand across the view. `bf3dump vm skin` (below) showed that
nothing is stretched: every knife surface keeps its bind-pose edge lengths. The bars are the dog-tag **ball chain**
of `knife_1p_mesh` (subset `M_Dogtag`, shader `objects/shaders/proppreset_alphatest`, Diffuse
`weapons/knife/dogtag_d` with alpha), a strip of long thin quads (0.397 m edges in the bind pose) that the shader cuts
out by the Diffuse alpha; the viewmodel drew every slot opaque. The tags themselves are subset `dogtag2`
(`proppreset_diffuseonly`, `dogtag_d2`) [verified: `bf3dump vm skin weapons/knife/u_knife`, `bf3dump looks MP_Subway
TeamDeathMatch0 --shader objects/shaders/proppreset_alphatest`: surface type 1 = OpaqueAlphaTest].

Two changes:

* **Alpha test from the shader database** (structural, all items): the loader resolves every material slot of the
  arms, weapons and accessories through L1's `LookResolver` (the level shader database's surface type; M1's name rule
  for a shader it lacks) into `ViewmodelSlotLook` (alpha_test, double_sided); `Bf3Viewmodel` gives those materials an
  alpha scissor at 0.5 (`kViewmodelAlphaScissor`, as L1's level materials) and no culling when double-sided. Row
  `vm.alpha_test`. On the 12 kit items the only alpha-tested slot is the knife's chain.
* **Dog tags left out** (a counted stand-in): even cut out, the chain is a 0.4 m loop that the static
  `Knife_IdlePose` holds upright from the tags below the hand up past it to the screen centre (camera-space box
  y −0.465..+0.026 m, the hand at about −0.15 m). BF3 shows the player's chosen dog tags on the knife (a persistence
  customization M2a does not have) and lets the chain hang [inferred]; the knife's `BoneFakePhysics` list is empty,
  so how BF3 hangs it is not in the weapon data. M2a hides every knife surface whose Diffuse texture names `/dogtag`
  (`kDogTagTexture`), counts them (`ViewmodelItem::hidden_surfaces`, 2 per knife) and lists them in the notes; the
  demo's `--vm-dogtags` draws them (comparison captures in `b\shots\vm\dogtags\`). Row `vm.knife_dog_tags`.

`bf3dump vm skin [<unlock>...] [--team us|ru|all]` loads the meshes and plays every item through the animator
(deploy, fire, reload at 30 fps). Per surface of the weapon and the arms it prints the longest skinned triangle edge
against the bind pose's, the weights that do not sum to 1, the camera-space extent and bones at idle, the material
slots (shader, textures, alpha test) and any Scale DOF of the role clips (none on the kit items: the knife and M67
idles have 96 quaternion and 39 vec3 channels, all rotation or translation). `VM SKIN: PASS` when no surface is
stretched (`viewmodel_surface_skin`, `kSkinStretchFactor` 1.5, `kSkinStretchFloor` 5 cm).

### 2.8 Contract notes for wave 3

* §15 names "viewmodel_assets.* gains an additive overload". vm added the overload to `viewmodel_set.h` instead
  (`load_viewmodel_set(ViewmodelSources &, ...)`, sharing the Ant library and level superbundle between the team
  sets); `viewmodel_assets.*` is untouched.
* `VmClip` (viewmodel_anim.h) and `ViewmodelClipRole` (viewmodel_set.h) gained `Sprint = 12` before `Count`
  (static_asserts tie the two enums), and `ViewmodelSet::library` is a `shared_ptr` (the team sets share it). cl and
  gx should build against these.
* `ViewmodelKit` holds strings now and comes from `read_viewmodel_kits`; `team_viewmodel_set(kits, team)` takes the
  kits. cl's own-soldier kit change should pass the match's `KitTable` loadout unlocks the same way.

## 3. Tuning (`bf3-opensource/config/viewmodel_tuning.cfg`)

| spring | stiffness / damping | settle_seconds | other keys |
|---|---|---|---|
| sway | 256 / 32 | 0.75 | scale 0.01, max 0.05 |
| bob | 64 / 16 | 1.0 | amplitude 0.008, frequency 1.8, ads_scale 0.2 |
| kick | 900 / 60 | 0.6 | back 0.02, pitch 1.0, ads_scale 0.4 |
| camera_recoil | 625 / 50 | 0.6 | camera_kick_pitch_deg 0.3 |

Other keys: `ads_in 0.09`, `ads_out 0.15` (used when the controller has no zoom times), `fov_transition_specific
true`, `sprint_blend 0.2`, `deploy_drop 0.25`, `reload_drop 0.12`. Springs use the closed-form damped solution (a
semi-implicit Euler step peaked about 12 % low at 30 fps), dt clamped to 0.25 s, NaN states reset. The parser is a
key table with ranges (motor_tuning style); unknown keys, bad values and out-of-range values are errors with the
line number, and the built-in defaults are used (the demo prints which).

## 4. Data findings [verified: `bf3dump vm clips --team all`]

* Reload (Q4: authored rate; the weapon is ready at ReloadTime, a shot cuts the clip):

  | weapon | tactical ReloadTimeBulletsLeft (clip) | empty ReloadTime (clip) |
  |---|---|---|
  | M16A4 | 1.80 (2.333) | 2.37 (2.900) |
  | M4A1 | 1.85 (2.267) | 2.48 (2.867) |
  | M27 IAR | 1.90 (2.333) | 2.50 (2.900) |
  | MK11 | 1.85 (2.267) | 2.40 (2.867) |
  | M9 | 1.35 (1.900) | 1.60 (2.167) |
  | AK74M | 2.10 (2.667) | 3.10 (3.600) |
  | AKS74U | 1.90 (2.333) | 3.00 (3.333) |
  | RPK-74M | 3.00 (3.600) | 4.00 (4.467) |
  | SVD | 2.15 (2.967) | 3.30 (3.867) |
  | MP443 | 1.35 (1.900) | 1.60 (2.167) |

* Zoom clips: on most rifles the ZoomIn / ZoomOut end frames miss IdlePose / ZoomPose by more than 2° on a driven
  bone, so the animator blends IdlePose → ZoomPose (counted as a stand-in); the clip drives the transition only on the
  M16A4, M27 and AKS74U ZoomIn. The pistols' zoom clips are VBR and use the same blend.
* Fire clips exist only for the MK11, SVD and the pistols; the others use the procedural kick.
* The knife and the M67 have no Ant package of their own: the static `Knife_IdlePose` / `HandGrenade_IdlePose`
  controllers give the idle; deploy, throw, stab and the next-grenade raise are procedural stand-ins.
* RenderFov is 55 on every kit item; ZoomRenderFov is 15 (M16A4), 20 (most), 22 (AKS74U), 25 (MK11), 10 (knife,
  M67, unused: no zoom level).

## 5. Captures (`openBF3\b\shots\vm\`, gitignored)

38 PNGs at 1152 × 648 from `--vm-shots` (retaken on the final tree, 2026-10-16 evening; 10 more with
`--kits=0 --vm-dogtags` in `dogtags\`): hip, ADS and reload mid-clip of all 10 guns of the 8 kits, plus hip, throw
and stab of the M67 and knife (US and RU assault). What they show:

* **Hip**: arms and weapon in the lower right as in BF3, textures and gloves correct for US and RU; with the
  accessories the M16A4 / M4A1 show the carry-handle rear sight and the front sight post.
* **ADS** (iron sights): the sight line is on the screen centre for every gun (M16A4 / M4A1 rear aperture with the
  front post inside it, AK74M / AKS74U / RPK / SVD notch and post, M9 / MP443 three-dot, MK11 aperture). ZoomRenderFov
  15 on the M16A4 makes the rear sight large (it fills about the centre third of the screen height); 20-25 on the
  others looks closer to BF3. The user should judge the M16A4 look (§6).
* **Reload mid-clip**: the authored clips (magazine out, hand at the well, rifle rolled) for every gun; nothing
  procedural.
* **M67**: hip idle low right; the throw shows both arms raised wide (the procedural swing over the static idle).
* **Knife**: the hand holds the knife in the static idle, blade forward; no stray geometry since the dog tags are
  left out (§2.7). With `--vm-dogtags` (`b\shots\vm\dogtags\`) the cut-out chain shows as a thin loop standing up
  from the hand: the user should judge whether BF3 shows the tags there and how they hang.

## 6. Open issues

* sv's zoom rule in `step_weapons_m2a` (E19 sim half): `PENDING (sv)`.
* cl's `present_weapon` and event drain (wave 3) replace `m2a_fake`; `Bf3Viewmodel` takes `ViewmodelFrame` from
  `viewmodel_frame_from(PresentedWeapon, ...)` and `viewmodel_cues_from(...)` unchanged.
* m2a.md names the Godot E19 rows `godot.m2a` case "ads" (integ's scene, wave 3); this track's rows are
  `godot.m2a_viewmodel`.
* For the user's judgement: the M16A4's ZoomRenderFov 15; the spring constants; the procedural throw and stab; the
  knife's dog tags (hidden stand-in, §2.7).
* The FOV transition lands −1..0 frames early (the first frame after the flip credits a whole dt, and the stand-in
  `present_weapon` takes the discrete zoom flag from the later tick), so the worst case sits at exactly 1.00 frame,
  inside E19's ± 1 frame only through the 1e-6 slack. cl's real `present_weapon` decides the flag's timing in wave 3;
  crediting only the part of the frame after the flip would centre the error.
