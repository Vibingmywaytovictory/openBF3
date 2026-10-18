# M2a fx3: VFX3 triggers, as built

Track fx3 of M2a wave 3, branch `m2a/fx3` from `m2a/w3` 3b3b991. Design: [m2a.md](m2a.md) §12.2, §8.3, §4.4, E8,
E15 and §15's fx3 row; the W3.0 contract: [m2a-w3.md](m2a-w3.md). This document records what was built, where it
differs from the contract, the measured results and one command per gate. Numbers marked "branch" were measured on
this branch on the shared machine without the quiet lock: they are indicative only.

## 1. Files

| file | what |
|---|---|
| `src/bf3_fx/include/bf3/fx/triggers.h`, `src/bf3_fx/src/triggers.cpp` | `load_fire_effects`, `FireEffectTable::blueprints`, `impact_sound_names` / `impact_effect_names` / `impact_decal_names`, `fallback_muzzle`, the effect-frame helpers, the decal-template reader, `effect_sound_cues`, `FxTriggers`, the BC1 decal un-keying (§5) |
| `src/bf3_fx/src/simulator.cpp`, `effect_def.h`, `m2a-vfx1.md` | the named replacement: BoxEvaluator pivot in half-extent units (commit 0ee8f74) |
| `src/bf3_godot/src/game_fx.{h,cpp}` | `Bf3GameFx` (§4) |
| `src/bf3_godot/src/fx_decals.{h,cpp}` | `FxDecalAssets` (loader thread) and `FxDecalPool` (≤ 256 Godot `Decal`s, §5) |
| `src/bf3_godot/src/viewmodel_muzzle.{h,cpp}` | G6 (fx3 owns it since W3.0): adds `muzzle_sample`, `to_godot_transform`, `to_fx_transform` |
| `src/bf3_godot/src/fx3_demo.{h,cpp}`, `bf3-opensource/scenes/fx3_demo.tscn` | `Bf3Fx3Demo`, the demo and capture check (§6) |
| `tests/fx_triggers_tests.cpp` | `fx.triggers.unit` (14 cases) and `fx.install.cells` (2 cases) |
| `tests/godot_fx3_demo.cmake` | `godot.fx3_demo`, the window row |
| `tests/m2a_contract3_tests.cpp` | `kFx3Landed = true` (the landing commit 0566938) |

Shared files carry one `# M2a fx3` / `// M2a fx3` block at their end: `src/bf3_fx/CMakeLists.txt`,
`src/bf3_godot/CMakeLists.txt`, `tests/CMakeLists.txt`, `register_types.cpp`, `README.md`.

## 2. Data facts [verified on the install, `fx.install.cells`]

* Metro TDM has 12 weapon defs: 10 firearms, all 10 with a tracer. They name 24 fire blueprints, and the impact
  table names 48 impact effects: 72 blueprints preload with 0 failures (E8 (1)), uncached in 4–6 ms (branch).
* The M16A3 (variant `weapons/m16a4/u_m16a4`) has 3 FireEffects1p entries. Entry 0 is
  `fx_muzz_generic_rifle_1p` at offset z 0.99, y 0.028, with UpdateTransform. Entry 1 is `fx_shellejection_m`
  (the shell, recognised by `/shells/` in the name) at z 0.44, 0.55 m behind the flash. Its FireEffects3p list starts with
  `fx_muzz_generic_rifle_3p` and also has a shell. Trace effect `fx_tracer_generic_parent`, trace frequency 1, bullet
  material 1, `VisualConvergeDistance` 30 m.
* The M67's projectile names `DetonationEffect` `fx_grenade_frag_01`. The knife has no muzzle and no shell.
* Metro's collision triangles by `PhysicsMaterialIndex`, through the sim's `SurfaceMaterialTable::static_pair` (what
  cl's Impact event carries): Default 34,042, Brick 48,528, Metal 127,036, Light 38,310, Concrete 103,580, Plastic
  20,496, Glass 58,738, Gravel 2,289, Sand 4,828, Wood 82,714, Stone 259,630, Grass 372, Forest 612, Concrete_XP2 132,
  and others (the full census is in the row's output). There is no Water (18) triangle.
* The named cells (E8 (5)): Concrete → `fx_impact_concrete_01_s_xp2` + `decal_concrete_s_01`; Brick →
  `fx_impact_concrete_01_s` + `decal_brick_s_01`; Glass → `fx_impact_glass_01_s` + `decal_glass_s_01`; Metal →
  `fx_impact_metal_01_s` + `decal_metalarmor_s_01`. These are fx G5's cells. The soldier zones (Body 66, Chest 158,
  Head 65) give `fx_impact_soldier_body_s` with no decal. Caspian (MP_007) Water → `fx_impact_water_s` and the sound
  `bullet_impact_water`.
* The 32 impact sounds and 16 decal templates the cells name. 14 of the decals have a diffuse texture beside them.
  `decal_cloth_s_01` and `decal_plastic_s_01` have none on Metro.
* Decal textures: the concrete, glass and scorch ones are BC3 with a real alpha. The metal, brick, stone, wood and
  generic `decal_s_01` ones are BC1 with no alpha. Four of those (metal, stone, wood, `decal_s_01`) are keyed on
  magenta: decompressed, the background is pure (255, 0, 255) and the hole's halo blends towards it, with mean RGB
  (0.90, 0.01, 0.90); their key share is 61-95 %. **Brick is not keyed** (corrected after verification; the first
  notes listed it as keyed): `decal_brick_s_01_d` is a red (255, 0, 0) square around a yellow-to-green blob, mean
  RGB (0.87, 0.44, 0.00), 0 % key and 0 % see-through. It is a channel mask, not a colour texture, and drawn as an
  albedo it was an opaque red square on every Brick hit. It is now refused (§5). The template names no texture
  (its ShaderGraph binds them), so `pick_decal_texture` infers it [inferred].

## 3. Contract corrections

* **Impact material.** W3.0 wrote "`surface_material(material)`". In fact `PresentEvent::material` of an Impact
  already is the surface's PhysicsMaterialIndex: cl's `ClientProjectiles` copies `ImpactEvent::physics_material`,
  which the sim resolved with `SurfaceMaterialTable::static_pair`. The triggers select with it directly. Applying
  `surface_material` again would misread the pair index.
* **MuzzleSample reference point.** A sample sits at the person's reference entry, the def's first FireEffects1p (1p)
  or FireEffects3p (3p) entry (the flash at 0.99 m on the M16A3). The other entries are placed relative to it in the
  weapon frame (`weapon_of_effect_frame` / `effect_frame_of_weapon`). G6 is called with entry 0's offset
  (`Bf3GameFx::own_muzzle`).
* **1p list only at a 1p muzzle.** An OwnShot plays FireEffects1p in the 1p pass only when this frame has the own 1p
  muzzle. Otherwise it plays FireEffects3p in the world: free camera, own death (Bf3GameFx drops the 1p sample),
  no viewmodel. The own soldier's crowd muzzle is used if there is one, else the aim fallback.
* **Seeds for events cl leaves at 0.** SoldierContact, Hit and Detonation carry no seed, so the triggers derive them
  from the match seed:
  - `soldier_seed = impact_seed(effect_seed(match, shooter, shot), pellet, 0xFFFFFFFF)`;
  - `detonation_seed = trigger_seed(effect_seed(...), Detonation, 0xFFFFFFFF)`.
* **Soldier cells.** The own predicted SoldierContact plays the zone's cell. A later Hit of the same (shooter, shot,
  pellet) counts as `hits_confirmed`, not as a second start. A remote shooter's contact plays nothing; its Hit plays
  the cell. A Hit on the own soldier (`kOwnVictim`) plays nothing. A non-finite contact zone reads as Body. Legs map
  to Body [inferred], because the grid has no leg cell.
* **BoxEvaluator pivot (named replacement, 0ee8f74).** VFX1 read `BoxEvaluatorData.Pivot` in metres, which put the
  1p rifle flash 2 m along the barrel (m2a-fx2.md §9). The box is now `(0.5 pivot + u - 0.5) x dimensions` per axis,
  so the flash sits 0.25 m ahead of the muzzle. `fx.unit`, `fx.fuzz`, `fx.install` and `fx.render.unit` still pass.
* **`FxTriggerStats` additions:**
  - counts: `own_shots`, `remote_shots`, `firearm_shots`, `tracer_shots`, `hits_confirmed`, `duds`, `rejected`,
    `unknown_defs`;
  - load: `tracer_steals`, `attached_updates`, `pending_now`;
  - `culled`: starts of any purpose beyond their CullDistance. A remote 3p shell 6.7 m from the camera is one, and
    `impacts_culled` counts only the impacts.
* **The effect-sound accessor** agreed with au3: `Bf3GameFx::effect_sound_cues()` returns `fx::EffectSoundCue`, which
  matches `audio::EffectSoundRequest` field for field (`patch`, `effect` = handle slot << 32 | generation,
  `component`, `position`, `start`). gx converts it in its frame hook.

## 4. `Bf3GameFx` (`game_fx.h`)

* **`load`** (loader thread, no Godot API) runs these steps in order:
  1. Opens its own `EbxLibrary` over the level superbundle (`open_weapon_sources`; `load_all`, 12,524 partitions in
     0.10–0.14 s). The database's world is Phase B's and is dropped after the load.
  2. Builds the EffectBlueprint and EmitterDocument name catalogue.
  3. Loads the emitter atlas and its pixels. The cache is used for this step only: `M2aLoadInputs::cache` dies with
     the load. Warm, this takes 0.03 s; converted, 0.12–0.14 s.
  4. Loads the impact table of the level grid.
  5. Builds an uncached `EffectLibrary`.
  6. Runs `load_fire_effects` over `SharedLevel::gunplay->defs`. With no gunplay it uses an empty table and writes a
     note.
  7. Preloads `fire->blueprints()` ∪ `impact_effect_names` on `M2aLoadInputs::threads`.
  8. Loads the decal assets (§5).
  9. Creates the FxWorld and the triggers.

  `M2aLoadInputs::stop` is checked between the phases. On Metro the whole load takes 0.23–0.43 s (branch).
* **`build`** (main thread):
  1. Creates fx2's `Bf3FxRenderer` under the parent.
  2. Uploads the atlas and counts the E8 (6) entries (52, with 0 outside the upload).
  3. Frees the atlas pixels.
  4. Builds the decal pool under the renderer node.
* **`step`** runs once per display frame, in the order `m2a_game.h` gives:
  1. Sets `own_slot` and `match_seed` from the frame.
  2. Drops the 1p muzzle in free camera or when the own soldier is dead (`muzzles_dropped`).
  3. Calls `on_muzzles` and then `on_events` with the frame's clock and camera.
  4. Places the frame's decal cues and ages the pool.
  5. Calls `step_world` and `draw` on the renderer.
  6. Collects the frame's effect sound cues from `FxWorld::events()`.
  7. Updates the stats: drawn instances, lights, texture fallbacks and decal counters.

  `sound_cues()` and `effect_sound_cues()` stay valid until the next step.
* **`own_muzzle(viewmodel, def, slot, velocity)`** is G6 at the def's first FireEffects1p entry, converted with
  `muzzle_sample`. For a WeaponRoot muzzle the basis is the Frostbite weapon frame: column 2 is the barrel and column
  1 is up. For the camera stand-in, the camera's -Z is the barrel.
* **`start_effect`** is client-local and never sent on the wire. It resolves the name in this order:
  1. a full name in the level;
  2. a short name in the level (`find_partition`);
  3. a full name the level lacks, through a lazily opened `@all-mp` library. This is check-only; MP_012's oilfire is
     such a name.

  An EffectBlueprint of the level goes through the library. Any other effect, and every EmitterDocument, goes through
  `compile_effect` / `compile_emitter_effect`, kept for at most 256 names. Failures are counted.
* **`shutdown`** frees the renderer and the decal root by ObjectID, so a parent that freed them first is safe. It is
  idempotent. A `load` after a `shutdown` (or after a failed load) starts from nothing: the catalogue, notes, stats
  and tables are reset on the loader thread (no Godot node is touched there).

## 5. Decals

* `load_decal_template` reads DecalTemplateData's Size, RandomSize, Rotation, RandomRotation, NormalOffset, ClipAngle
  and AtlasTile. It applies bounded reads; a Size outside (0, 100] and a non-finite RandomSize, Rotation,
  RandomRotation, NormalOffset or ClipAngle are errors (a NaN passed `std::clamp` before). A non-finite FireEffectData
  Offset or Rotation fails that def's fire lists with a note, like any other read error. `decal_placement` draws a size of
  Size + U x RandomSize and a rotation of Rotation ± RandomRotation from the cue's seed
  (`trigger_seed(.., Decal, 0)`).
* `FxDecalAssets::load` runs on the loader thread. It loads each named template, picks its texture, then loads it
  with `res::load_texture` and prepares it with `prepare_texture(Color)`.
* **The decal-texture rule** (`fx::prepare_decal_texture`, Godot-free, the one call that decides every decal texture):
  - Level 0 is decoded: BC1 (`decode_bc1`), BC3 (`decode_bc3`: 4-colour block, BC4-style alpha), RGBA8 as is. Any
    other format (BC2, BC4, BC5, L8, RG8) is `Unsupported` and refused.
  - A texture whose texels are at least 20 % key (R, B ≥ 230 and G ≤ 25) is un-keyed: each texel is read as
    a × grey + (1 - a) × magenta, so 1 - a = min(R, B) - G and the grey is G / a [inferred]. The grey goes into all
    three channels: R and B carry the key, and what they hold beyond the grey is off the model (the first version
    kept it as hue, and stone's cracks drew saturated blue and metal's hole blue-violet). Un-keyed textures get an
    alpha-weighted RGBA8 mip chain (`rgba_mip_chain`).
  - The texture is drawn only when at least 5 % of level 0 is see-through (alpha < 128, after un-keying;
    `kMinDecalTransparentShare`). A decal that covers its whole square is a mask or a mistake, never a bullet mark.
    Anything else is `Opaque` and refused: the decal is untextured (skipped and counted; `FxDecalAssets::refused`, a
    load note "opaque (BC1, 0 % see-through): not drawn").
  - On Metro: 9 textures load, 4 un-keyed (see-through: `decal_s_01_d` 94 %, metal 97 %, stone 99 %, wood 75 %),
    4 with a real alpha (concrete_s 55 %, concrete_l 74 %, glass 98 %, scorch 89 %), brick refused (0 %); 5.2 MiB in
    0.03 s. fx.install.cells runs the rule over every Metro decal texture and checks brick refused, the named cells'
    other decals and the scorch mark drawn, and every drawn one see-through on 5-99.9 % of its texels.
  - The decoders are hostile-safe: a zero or > 4096 size and short input are errors (unit cases "decal keys" and
    "decal texture rule", also under asan).
  - Brick hits therefore leave no mark on Metro. A brick-coloured mark would need the mask's meaning (BF3's decal
    shader), which is not known.
* `FxDecalPool` places Godot `Decal` nodes. The basis is the cue's effect frame (+Y = normal), turned about +Y by
  the placement rotation. Godot decals project along -Y. The box is size × 0.12 × size, NormalOffset above the
  surface.
  - Albedo mix is 1. The normal fade is 0.5 cos(ClipAngle) + 0.5 [inferred from Godot's shader], and the upper and
    lower fades are 0.3.
  - Past 256 decals the oldest node is reused. A decal fades out over 5 s once it is 30 s old.
  - A cue without a template or texture is skipped and counted. Cloth and plastic (no texture) and brick (refused)
    are the untextured ones on Metro.

## 6. Demo (`res://scenes/fx3_demo.tscn`, `--fx3-check`)

`Bf3Fx3Demo` runs the game path end to end beside a `Bf3LevelSession`.

**Load.** The loader thread loads the gunplay level next to the session's level, as gx's `M2aLoadInputs` would
carry it, and calls `Bf3GameFx::load`. It also gathers dirt candidates: up-facing collision triangles of Sand, Grass,
Forest and Gravel, through the sim's pair table.

**Frames.** The main thread builds one `M2aFrame` per display frame. Each frame carries:
* PresentEvents shaped the way cl publishes them, with a valid clock and age 0:
  - OwnShot: the M16A3 def at 800 rpm, every third round a tracer.
  - Impact: 6 per surface capture, within 0.18 m.
  - Hit: chest zone, on a remote victim.
  - RemoteShot: 3 tracers. There is no crowd here, so they use the aim fallback.
* G6's camera stand-in as the own 1p muzzle, as `ViewmodelMuzzle{CameraStandIn}` through `muzzle_sample`.

**Surfaces** are found through the collision world and the sim's `static_pair`:
* Concrete and metal: a ray fan from the stage camera and every spawn point's eye.
* Dirt: Sand first, from the collision triangles, needing 3 m clear along the normal.

Metro's Gravel lies only in planters under hedges, and its Sand is the sandbags. So the "dirt" capture is a Sand
impact (`fx_impact_sandbright_01_s`) on a sandbag.

**Hit target.** A grey capsule stands in for the soldier, 4 m in front of the main pose. The main pose is spawn
point 0's eye along its facing, with 12 m clear.

**Report.** The run prints load, draw, shots, impacts, decals, hit, the FX time (indicative), the captures and
install unchanged, then `FX3 DEMO: PASS` or `FAIL`. Diagnostics: the decal textures' formats and coverage are logged,
and `--fx3-dump-decals=<dir>` writes them as PNGs (refused inside the install; scratch only, never committed).

## 7. Captures (`C:\Users\olku\BF3 reimplementation\openBF3\b\shots\fx3\`, gitignored; the look is the user's call)

| capture | what it shows |
|---|---|
| `fx3_muzzle_1p.png` | The 1p M16A3 flash from the 1p pass: a bright yellow-white fireball right of centre and below it, with the orange muzzle light on the station floor. The capsule stand-in is 4 m ahead. The flash is at fx2's nominal camera point (no viewmodel mesh is loaded), so it floats in front of the camera with no weapon behind it. Shell stand-in boxes show at the bottom. |
| `fx3_impact_concrete.png` | A top-down view of a Concrete floor 1.5 m away, 0.1 s after 6 impacts. A dark dust burst with streaks fills the centre, debris specks surround it, and the decals are under the dust. |
| `fx3_decals_concrete.png` | The same view 5.5 s later. The dust has gone, and the 6 overlapping `decal_concrete_s_01` marks remain as a grey-brown chipped patch. |
| `fx3_impact_metal.png` | A Metal panel (orange, weathered). A bright spark glow with fine sparks, and 6 small round bullet-hole decals (`decal_metal_s_01_d` un-keyed). Before the un-keying fix these drew as magenta squares; since the grey rule (§5) they are faint grey rings with no blue core (re-captured 2026-10-18). |
| `fx3_impact_dirt.png` | A top-down view of a Sand-material sandbag. A pale blue-grey sand puff, and 6 small dark holes on the bag (generic `decal_s_01`). |
| `fx3_hit_soldier.png` | The grey capsule 2.2 m away, 0.08 s after a chest Hit: dark red-black spray particles of `fx_impact_soldier_body_s` on its chest. |
| `fx3_tracers.png` | The main pose 0.03 s after 3 remote shots from 6 m to the right. A thin tracer streak crosses the left half, the 3p muzzle smoke shows near the shooter, and the own tracer glow lights the capsule. |

## 8. Open issues

* **DisableDuringZoom** is carried but not applied: M2a gives the triggers no zoom state.
* **Detonations** play the detonation effect and the scorch decal only. The surface's impact needs the surface
  material, which the Detonation event does not carry.
* **Metro has no Water** triangle. The water cell is checked on Caspian in `fx.install.cells` (E8 (5)).
* **Tiled decals** (AtlasTile > 1) would draw their whole texture. None are tiled on Metro (all 1 × 1).
* **Untextured decals.** Cloth and plastic have no texture on Metro, and brick's texture is an opaque channel mask
  that the decal-texture rule refuses (§5), so they are skipped and counted. Brick hits show their impact effect and
  sound but no mark.
* **Inferred decal textures.** The texture choice and the magenta-key meaning (a grey over magenta) are inferred.
  The un-keyed stone decal now reads as dark grey cracks and metal as a grey hole with a pale rim; the look is the
  user's call.
* **Late join replays no recording.** fx.triggers.unit case "late join" builds the RemoteShot events itself the way
  `client_session.cpp` emits them (`effect_seed(match, slot, shot)`, the `kTracer` flag; checked equal to its lines
  1678-1686 on 2026-10-18) and takes the Impact events from cl's real `ClientProjectiles`. A later change to
  ClientSession's RemoteShot emission would not be caught there; a <= 2 s unit row cannot run a session.
* **E8 (5)'s oracle is a copy.** fx.install.cells lists fx G5's named Metro cells as literals; G5's table is local to
  `fx_harness.cpp` (fx's file) and not exported. The values equal G5's today and can drift.
* **Debris.** Debris mesh components draw as fx2's mesh stand-ins (boxes).
* **The demo's 1p muzzle** is G6's camera stand-in, not a weapon joint. Loading a viewmodel in the demo would add
  seconds; gx's scene has it.
* **Integration (gx).** gx wires `Bf3GameFx` into the playable scene: `load` in `m2a_load_assets`, `build` and `step`
  in `m2a_frame`, `own_muzzle` first in `M2aFrame::muzzles`, `sound_cues` and `effect_sound_cues` to
  `Bf3GameAudio`. Until then the playable scene draws no VFX3 effects. The demo and its captures show the path.
* **3p muzzles** come from tp's crowd (`Bf3GameCrowd::muzzles`). tp landed on m2a/w3, so the test-local
  `m2a_fake::crowd_muzzle` is gone: `fx.triggers.unit` feeds a crowd-style sample (wire slot, 3p, off the eye
  fallback) and checks the trigger takes it; a soldier without a crowd muzzle still gets the aim fallback (counted
  `muzzle_fallbacks`). Until gx feeds `Bf3GameCrowd::muzzles()` to `Bf3GameFx` in the playable scene, every remote
  shot there uses the fallback.

## 9. Gates, measured (2026-10-18; branch, unlocked, shared machine: indicative only)

| gate | threshold | measured | command |
|---|---|---|---|
| fx.triggers.unit | all cases pass, ≤ 2 s | 15 / 15 cases, 4,530 assertions, 0.21-0.35 s (after the decal-texture fix; 14 cases and 4,496 assertions before). Synthetic flash 766 / 1000; concrete choices 103 / 97 over 200 seeds, widest spread 29.8° (limit 30); late join: 87 starts over 25 remote shots, 25 impacts and 2 variants, identical between the two clients | `ctest --preset dev -R "^fx\.triggers\.unit$"` |
| fx.install.cells | both cases pass, ≤ 15 s | 2 / 2 cases, 194 assertions, 2.9-4.3 s alone (11.1 s in the strict pass); the decal-texture rule over Metro's 9 decal textures: brick refused, 8 drawn, 0.03 s | `ctest --preset dev -R "^fx\.install\.cells$"` |
| E8 (1) | every blueprint of the kits and the impact table compiles | 72 / 72, 0 failures | same (fx.install.cells) |
| E8 (3), Godot-free half | `_1p_flash` in [710, 790] of 1,000 M16A3 OwnShots | 759 / 1000 | same |
| E8 (5) | the in-game cells start fx G5's named effects and decals; water: Metro's tags, else Caspian | Concrete, Brick, Glass, Metal as G5; soldier zones; "Metro: no water"; Caspian water → `fx_impact_water_s` | same |
| E8 (7), Godot-free | tracers = flagged shots, one shell per firearm shot, one sound per impact in its cull distance, 0 double starts | 1000 / 1000 tracers, 1000 shells; the unit cases check sounds, culling and duplicates | same, and fx.triggers.unit |
| E15 variant row | the same (effect, variant) per remote shot on both clients | equal | fx.triggers.unit case "late join" |
| m2a.contract3 | fx3 rows run (no PENDING) | 10 / 10 cases, 336 assertions | `ctest --preset dev -R "^m2a\.contract3$"` |
| asan | the new decoder, the template reader and the triggers under ASan | see §10 | `ctest --preset asan -R "^fx\.(triggers\.unit|install\.cells)$"` |
| godot.fx3_demo (window) | `FX3 DEMO: PASS`: 0 preload failures; drawn == particles every frame; one shell per own shot; 0 double starts; impacts on concrete, metal and dirt; decals; a hit; 7 captures; install unchanged | PASS (re-run after the decal fix): 2,595 / 2,595 frames; 180 own + 3 remote shots, 366 muzzle starts, 180 shells, 63 tracers, 19 impacts, 18 decals, 1 soldier cell; decals: 8 textures, 4 un-keyed, 1 refused (brick); FX main thread p50 0.09 / p99 0.16 ms; 24.7 s | `ctest --preset dev-window -R "^godot\.fx3_demo$"` with `OPENBF3_GODOT`, `OPENBF3_GODOT_WINDOWED=1` and `OPENBF3_USER_ACTIVE_FLAG` (a window row) |
| install unchanged | layout.toc, cas.cat, every .sb/.toc | unchanged | fx.install.cells case "install unchanged", and godot.fx3_demo |

## 10. Branch pass and asan (2026-10-18; branch, unlocked, shared machine: indicative only)

* **asan** (`toolsuild.cmd asan --no-test`, then
  `ctest --preset asan -R "^(fx\.triggers\.unit|fx\.install\.cells|fx\.unit|fx\.render\.unit|m2a\.contract3)$"`):
  5 / 5 rows passed with no AddressSanitizer report. That covers `decode_bc1`, `unkey_magenta`, `rgba_mip_chain`,
  the decal-template reader, `load_fire_effects` and the triggers. Under asan, fx.triggers.unit took 3.4 s and
  fx.install.cells 60.9 s (the asan preset has no time budget).
* **`toolsuild.cmd dev --strict`**, run twice while other sessions were building and testing on the machine (one
  of them was running ctest with host.gunplay.perf):
  - **Pass 1:** 130 rows, 128 passed and 2 skipped (`core.cache_data` and `core.res`, as on m2a/w3), with 430 s of
    test time (m2a/w3's own pass took 178 s). `test_budget` STRICT found 2 violations, both from the contention:
    fx.unit at 36.5 s and fx.install.cells at 32.2 s, both without COST.
  - **Pass 2:** 702 s of test time, with sim.terrain alone at 439 s. 129 of 130 rows passed. `core.ui.font` failed
    its own wall-clock check (cold 1.64 s against a 1.0 s limit; not an fx3 row). STRICT found 5 violations, none on
    an fx3 row: core.gameplay.tables.unit, sim.motor.unit, host.registry, sim.terrain.unit and anim.server_pose.
  - **The fx rows alone, right after:** fx.triggers.unit 0.26 s, fx.unit 0.49 s, fx.install.cells 3.06 s, all within
    their budgets (2 s unit and 15 s install). core.ui.font passed alone in 0.48 s.

  So the strict gate was not seen passing in those two runs. Both failures followed the machine's load and moved
  between rows.
* **After the verifier's fixes** (decal-texture rule, COST 5 on fx.install.cells): `toolsuild.cmd dev --strict`
  exit 0: 130 rows, 128 passed and 2 skipped (`core.cache_data`, `core.res`), 0 failed, **0 STRICT violations**
  (budget warnings only, none on an fx3 row); phase wall 193 s. fx.unit 1.6 s, fx.install.cells 11.1 s,
  fx.triggers.unit 0.35 s. asan after the fixes: the same 5 rows passed with no AddressSanitizer report
  (fx.triggers.unit 2.2 s, fx.install.cells 49.4 s), now also covering `decode_bc3`, `transparent_share` and
  `prepare_decal_texture`.
