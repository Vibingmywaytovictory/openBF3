# M2a VFX1 design: effect compiler and particle simulator

Status: design for implementation, revision 1, 2026-10-15. Scope: slice **VFX1** of the plan
([research/PORTING-RESEARCH.md](../../research/PORTING-RESEARCH.md) §3.14, D19, R19, §6 M2a row, §7.10; slice table in
[research/topics/gap-r2-vfx-effects-system.md](../../research/topics/gap-r2-vfx-effects-system.md) §8.2, "gap VFX"
below). The interface headers named below are in the tree under `src/bf3_fx/include/bf3/fx/` and compile (ctest
`fx.contract`); their bodies are the implementation's work (§15).

Tags: **[verified]** = read from code, data or a measurement in this tree on the date above (bf3dump `types` / `dump`
/ `reshead` / `weapon` over the retail install plus Python over the dumps in scratch). **[plan]** = taken from the plan
or the gap report, with its section. **[inferred]** = a design choice or estimate made here; every inferred semantic
is pinned by a golden or listed in §13 / §14.

---

## 1. Goal and scope

VFX1 turns BF3's own effect data into something a renderer can draw, without Godot:

* **Compiler**: every `EffectBlueprint` and `EmitterDocument` of the install into a flat, owned `EffectDef` /
  `EmitterDef` (all 32 processor types, all 10 evaluator types, polynomials with clamp and scale, spawn and age
  rules, `MaxCount` / `MaxInstanceCount`, base and child emitters, tiers), cached by partition digest (§8).
* **Simulator**: a deterministic structure-of-arrays CPU simulation (`FxWorld`) with fixed or variable dt that
  outputs per-particle vertex records, point lights and sound / visual-environment events (§6.3–§6.8, §7.2).
* **Level data**: the per-level emitter atlas (`EmitterAtlas`, §6.9) and impact selection from the material grid
  (`ImpactTable`, §6.10).
* **Tools**: `bf3dump fx` commands (census, compile, simulate with stats and goldens, check, bench, atlas, impacts;
  §7.7).
* **Seams** for VFX2 (renderer), VFX3 (triggers, lights, decals), VFX4 (network rules) and the audio track (§7).

Not in VFX1: anything Godot-side (no `bf3_godot` change, no Godot check), pixel decode or upload of the atlas, the
light pool, decals, triggers, `EmitterParameter` inputs, camera proximity, visual environments (§12).

## 2. Exit gates

Each gate has one automated command or ctest and one threshold. "fx.install" exits 77 without an install (ctest
SKIP_RETURN_CODE 77), as every install test does. All timings are wall clock on a shared machine with other tracks
building, so they are noisy; the bench gate takes the median of 5 runs and runs RUN_SERIAL.

| id | gate | how measured | threshold |
|---|---|---|---|
| G1 | census: every blueprint and emitter compiles, no unknown type (plan §7.10 item 1) | ctest `fx.install` case "census" = `bf3dump fx census @all-mp` (`run_census`, 0 = all lanes) | blueprints 1,101/1,101 and EmitterDocuments 3,410/3,410 compiled; `unknown_types` and `unknown_enumerators` empty; 32/32 processor and 10/10 evaluator types seen; `max_work_units` ≤ kDefaultCompileWorkLimit / 16 (headroom) |
| G2 | polynomial golden (item 2) | ctest `fx.contract` (now, header-inline evaluator) and `fx.unit` (compiled evaluators) | f(0) = w and f(1) = x+y+z+w exact on synthetic coefficients; clamp-then-scale (rapidsmoke 0.1 at t = 0); 1p flash alpha 1 → 0.0051852 (rel 1e-4); colour interp endpoints |
| G3 | 1p muzzle golden `fx/weapons/common/muzzleflash/fx_muzz_generic_rifle_1p` (item 3) | ctest `fx.install` case "muzzle" = `bf3dump fx simulate fx_muzz_generic_rifle_1p --starts 10000 --seed 1 --seconds 1 --dt 0.0166667 --golden muzzle1p` | per-start component probabilities: `_1p_flash`, `_1p_light_01`, `_1p_light_02` 0.75 ± 0.02, `rapidsmoke_ls_01` 1.0 exact, `nozzleflames_ls_01` 0.25 ± 0.02; flash: exactly 1 particle per started component, particle life 0.05 s (± 1e-6), spawn size ∈ [0.32, 0.40] × P(0) with P(0) = 0.7980724, size(t)/size(0) = P(t)/P(0) (rel 1e-5 at every step), start frame value ∈ [1, 16) with min < 1.01 and max > 15.99 over the starts; lights: one per live light particle, radius 0.2 / 2.5 exact, colour (1000, 395.072, 65.754) / (5, 1.875, 0.255) × intensity(t), intensity = clamp(cubic(t)) (rel 1e-5); rapidsmoke live ≤ 6 every step; nothing live after 1 s |
| G4 | continuous steady state `em_amb_mp12_oilfire_l_fire_01` (item 4) | ctest `fx.install` case "oilfire" = `bf3dump fx simulate <its blueprint or the document via --emitter> --seconds 75 --dt 0.0166667` and again with `--variable` (seeded dt ∈ [1/240, 1/20]) | after a 15 s warm-up, live particles ∈ [79, 81] at every step over the next 60 s, in both runs; max live ≤ 81 (MaxCount) over the whole run; `spawns_dropped` = 0 |
| G5 | impact selection, Metro and Caspian (item 5) | ctest `fx.install` case "impacts" = `bf3dump fx impacts MP_Subway --projectile=bullet` and `... MP_007 ...` | exact strings of §5.3: Metro 102 rows, 223 effect / 110 decal relations, the 5.56 row's concrete / brick / body / chest / head / water / glass / metal / default cells; Caspian 118 rows, 299 / 123, concrete → `fx_impact_concrete_01_s_xp2` + `decal_concrete_s_01`, generic default on 63 material indices; 16 distinct 5.56 effects on each |
| G6 | Metro atlas (item 6) | ctest `fx.install` case "atlas" = `bf3dump fx atlas MP_Subway` | 52 entries, 42 base + 10 normal, 3 degenerate; base DXT5 (2) sRGB 4096 × 4096, 13 mips; normal NormalDXT5RGA (20) linear 1024 × 2048, 12 mips; every non-degenerate rect pixel exact; every texture and normal-map name of Metro's 1,053 EmitterDocuments resolves (0 missing) and every entry is used |
| G7 | every effect simulates sanely | ctest `fx.install` case "check" = `bf3dump fx check @all-mp --seconds 10` (each of the 1,101 effects started 3 times with distinct seeds, fixed dt 1/60, then once more with variable dt) | 0 failures, where a failure is: a non-finite value in any output, `nonfinite_kills` > 0, live particles of an instance > `EffectDef::max_particles` at any step, or an effect whose components all have a finite window (no continuous or repeating emitter) still live after window + max particle life + 1 s |
| G8 | determinism | ctest `fx.unit` (synthetic defs) and `fx.install` case "check" (retail) | byte-identical `particles()` / `lights()` / `outputs()` on 1, 2 and 8 lanes and with two different item partitions, for the same starts and dt sequence |
| G9 | simulator cost (plan §7.10 item 8: 24.7 ns/particle/frame from the research prototype) | ctest `tools.fx_bench` = `bf3dump fx bench` (retail muzzle + oilfire + impact mix, about 16k live particles, one lane, 600 steps of 1/60 s, median of 5 runs; RUN_SERIAL; 77 without an install; report only under asan) | ≤ 50 ns per live particle per step, reported next to the 24.7 ns reference (2× margin for the shared machine and for the full 32-processor path the prototype did not run) |
| G10 | hostile data | ctest `fx.fuzz` in the dev **and** asan presets (synthetic, no install) | every case returns an error or a bounded result; no crash, hang or unbounded allocation; `decode_effect_def` accepts only blobs that re-encode byte-identically |
| G11 | install unchanged | `fx.install` last case "install unchanged" (`take_install_snapshot` / `diff_snapshots`, as host.session) | layout.toc, cas.cat and every .sb / .toc identical before and after the run |

## 3. Modules, ownership and threads

| piece | module | owner thread(s) |
|---|---|---|
| `fx_types.h` (vocabulary, bounds, polynomials, RNG), `effect_def.h` | `bf3_fx` (new, static, links `bf3_core` only) | any (plain data, header-inline helpers) |
| compiler `effect_compiler.h`, cache `effect_cache.h` | `bf3_fx` | load-time workers (`bf3::core::parallel_for`); thread-safe across items over a thread-safe resolver (EbxLibrary, read-only EbxDb) |
| `EffectLibrary` (`effect_library.h`) | `bf3_fx` | any; internally one mutex over the name map, compiles outside the lock with a per-name once flag |
| `EmitterAtlas`, `ImpactTable` | `bf3_fx` | built once per level on a load worker; immutable afterwards, read from any thread |
| `FxWorld` (`simulator.h`) | `bf3_fx` | owner thread (in VFX2 the Godot main thread) calls start / stop / set_* / begin_step / end_step; `run_items` on any threads over disjoint ranges; owns no thread |
| `bf3dump fx ...` | `tools/bf3dump` (`fx_commands.h/.cpp`, the spawns_commands pattern) | the tool's main thread plus `parallel_for` lanes |
| tests | `tests/fx_*` | doctest executables |

`bf3_fx` is client only: never linked into `bf3_sim`, `bf3_net` or `bf3_host` (plan §2.6, D19 "effects
client-only"); VFX4 turns this into a checked rule (§12). It is added by an appended block at the end of the top-level
CMakeLists.txt (§15) so the shared file's existing lines stay untouched.

## 4. Verified data sources

All counts over `@all-mp` (the EbxLibrary over every MP superbundle, 5,420 effect-related partitions) unless a level
is named [verified 2026-10-15; agree with plan §3.14 and gap VFX §1].

| type | count | fields VFX1 reads |
|---|---|---|
| `EffectBlueprint` | 1,101 | `Object` → `EffectEntityData` |
| `EffectEntityData` | 1,101 | `Components[]`, `MaxInstanceCount` (1:850, 2:1, 5:4, 7:6, 10:107, 15:2, 20:80, 30:29, 50:18, 100:4), `ResetInstanceWhenStarted` (true on all 1,101), `CullDistance`, `StartDelay`, `HighEndPc/MediumPc/LowEndPc`; components 1..41 per effect |
| `EmitterEntityData` | 4,353 (all inside effects, all internal) | `Emitter` (external → EmitterDocument; 3,187 distinct documents, 223 documents referenced by no blueprint), `Transform`, `SpawnProbability` (≠ 1 on ≈ 256), `StartDelay` (> 0 on 211), `CullDistance` (> 0 on 13), tier flags |
| `SoundEffectEntityData` | 1,716 (319 inside effects) | `Sound` (SoundPatchAsset), `Transform` |
| `VisualEnvironmentEffectEntityData` | 88 | carried by name (VFX5) |
| `EmitterDocument` / `EmitterTemplateData` | 3,410 / 3,410 | `RootProcessor`, `TemplateData` |
| `EmitterSystemAsset` | 29 (one per MP level) | `BaseAtlasTexture`, `NormalAtlasTexture`, `TextureInfos[] {TextureName, MinUv, MaxUv}` |
| processors | 32 types | `NextProcessor`, `Pre`, `EvaluatorInput`, `DefaultValues`, `Enable` + per type (§6.4) |
| evaluators | 10 types | §6.6 |

Chain and graph facts [verified]:

* Chains: length min 1 / median 21 / max 26; no cycles; no type twice in one reachable chain; `Pre` names one
  evaluator leaf; evaluator depth 1 (no evaluator references another; `PolynomialOperatorData` holds two inline
  `PolynomialTempData` operands); `NextProcessor` and `Pre` are always internal references.
* Per-emitter type usage (top and tail): SpawnRate and UpdateAge 3,334, SpawnSize 3,315, ..., EmitterData 174,
  BaseEmitterData 121, UpdateCameraProximity 46, UpdateSizeZ 16. Disabled (`Enable` false): BaseEmitterData 121 (all),
  Turbulance 7 (779 enabled), UpdateRotation 5.
* Order varies between documents: SpawnDirection after SpawnSpeed in 443; UpdateAge after SpawnRate in 56;
  AirResistance before Gravity 2,243 vs after 102; Gravity and LocalForce always before UpdateLinearVelocity; every
  emitter with SpawnSpeed or Gravity also has UpdateLinearVelocity (3,311).
* Enumerators: EvaluatorInput EfNormTime 36,806, EfOne 24,755, EfEmitterNormTime 2,136, EfVelocity 2; EmitterParameter
  None 34,482, Distance 1,703, Vec 722, Param1 122, Param2 29, VecAverage 20, Param3 8; EmittableType SAQ 1,899,
  ParticleMesh 931, DAQ 558, Quad 18, WAQ 4 (Trail, Point unused).
* Template (lifetime == 0, RepeatParticleSpawning): (F,F) 2,399, (F,T) 543, (T,F) 455, (T,T) 13;
  KillParticlesWithEmitter 105; TimeScale ≠ 1 on ≈ 355 (0.5 on 111), range 0.05..4; LifetimeFrameCount ≠ 0 on 8.
* Ranges: particle life 0.05..10,000 s; SpawnRate 0.005..1e6; emitter Lifetime 0..720 s; no non-finite float; largest
  |coefficient| ≈ 7.6e6 (one polynomial); MinClamp > MaxClamp never, == 137 times; Random Min > Max 126 times.
* Base emitters (`BaseEmitterData`, 121, no nested bases): in 76 documents BaseEmitterData is the only reachable
  processor, and the partition's unreachable "orphan" processor copy equals the base document's chain instance for
  instance (76/76) while the template differs from the base's; in the other 45 the disabled BaseEmitterData sits at
  position 0 followed by a full own chain (42 with the base's type set, 3 supersets).
* Child emitters (`EmitterData.EmitterAssets[]`): 271 child references, at most 4 per emitter, no grandchildren;
  largest parent MaxCount × child MaxCount = 2,300 (c130 flares). Example: the burningpaper `mainparent` emits
  invisible carrier particles (size 0, debug texture) whose children are the fire (LocalSpace) and a mesh.
* `DefaultValues`: UpdateColor `DefaultValues.xyz == Color` on 2,479/2,479; SpawnDirection (0,1,0,0); SpawnPosition
  (0,0,0,1); UpdateRotation and UpdateTextureCoords (1,0,0,0); SpawnAnimation (0,0,0,0) on 1,858/1,858; UpdateSizeX/Y
  varied without Pre (30, 2, ...); everything else (1,1,1,1).
* Animation: SpawnAnimationFrame has Pre on 1,871 of 1,872, nearly all `Random [0 or 1, N]` with N the texture's
  `AnimationFrameCount`, and `AnimationFrame` 0 with Pre `Random [1, 32]` on 86; SpawnAnimation `AnimationSpeed` > 0
  without Pre on 1,055, with Pre `Random [0.75, 1.5]`-style multipliers on 273; UpdateTextureCoords Pre (572) is a
  NormTime polynomial starting near 1 and mostly falling to 0.2–0.8.
* Gravity sign: positive on 1,713, negative on 965; debris median +10 (196 of 200 positive), sparks +9, shells +8,
  fire −1 (192 of 233 negative), smoke −0.5: **positive Gravity pulls down**.

## 5. Golden data

### 5.1 1p muzzle `fx_muzz_generic_rifle_1p` [verified, `bf3dump dump`; plan §7.10 item 3, gap VFX §2.3]

`EffectEntityData` MaxInstanceCount 7, ResetInstanceWhenStarted true, 5 emitter components:

| component | p | key data |
|---|---|---|
| `_1p_flash` | 0.75 | SAQ, MaxCount 1, emitter Lifetime 0.1, UpdateAge 0.05, SpawnRate 1 (DistributeOverTime true); SpawnSize 0.4 × Random U[0.8, 1]; UpdateSize poly {w 0.7980724, x 0.44260055, y −0.88148373, z 0.6358111}; UpdateTransparency poly {w 1, y 0.95294344, z −1.9477582} (f(1) = 0.00518524); UpdateAlphaLevelScale Exponent 10, poly {w 0.0050001144, z 0.9804876} clamp [0.9, 1]; UpdateColor interp {w 0.794, z −41.20004}, Color0 (125, 42.417, 8.716), Color1 (48.719, 12.026, 0.739); SpawnAnimationFrame AnimationFrame 1, Pre Random U[1, 16]; texture `fx/textures/muzzleflash/muzzleflash_01` 16 frames, 4 columns; AirResistance 10; LocalForce (5, 0.33, 0); SpawnSpeed 0.1 × U[0.6, 1]; SpawnPosition box pivot y 2, dimensions (0, 0.25, 0); template light radius 20 but ActAsPointLight false |
| `_1p_light_01` | 0.75 | ActAsPointLight, radius 0.2, colour (1000, 395.072, 65.754), intensity poly {w 0.97097605, x −1.7119899, y 3.966709, z −3.2640686}, random 1..1; emitter Lifetime 0.05, particle life 0.6, SpawnRate 15, DistributeOverTime false (one particle), Size 0, texture debug_arrow_up |
| `_1p_light_02` | 0.75 | as `_01`, radius 2.5, colour (5, 1.875, 0.255), intensity {w 1.0118451, x 0.07549087, y −0.72858727, z −0.3657508} |
| `rapidsmoke_ls_01` | 1.0 | MaxCount 6, SpawnRate 100, window 0.1, particle life 0.05, LocalSpace; UpdateTransparency {w 1.1179048, z −1.117318} scale 0.1 (clamp then scale: 0.1 at t = 0) |
| `nozzleflames_ls_01` | 0.25 | MaxCount 2, LowEndPc false |

### 5.2 Oilfire `em_amb_mp12_oilfire_l_fire_01` [verified; plan §7.10 item 4]

SAQ, emitter Lifetime 0 (continuous), MaxCount 81, TimeScale 0.5, UpdateAge 5.0, SpawnRate 16 DistributeOverTime true,
Emissive; no EmitterParameter evaluator. Steady state rate × life = 80 live, invariant under TimeScale (both the
spawn clock and the age clock scale): with TimeScale 0.5 a particle lives 10 s of real time and 8 spawn per real
second. G4's 15 s warm-up covers the 10 s fill.

### 5.3 Impacts [verified, `bf3dump weapon` / `dump`; plan §7.10 item 5, gap VFX §3]

The 5.56 bullet (`weapons/common/bullets/556x45mmnato`) material pair 5b6b84ac has PhysicsMaterialIndex 1 ("Bullet"),
property 77; M67 material 90, RPG-7 26. `MaterialContainerAsset.MaterialNames` first indices: Default 0, Bullet 1, Brick
2, Metal 3, Concrete 8, Glass 10, Gravel 11, Wood 13, Water 18, Head 65, Body 66, Chest 158 (names repeat).

* Metro `levels/mp_subway/mp_subway/materialgrid_win32/grid`: 102 rows, MaterialIndexMap 180, 32,400 pairs, 223 effect
  and 110 decal relations, 114 surface material indices with an effect for 5.56, 16 distinct 5.56 effects. 5.56 row:
  Concrete → `fx_impact_concrete_01_s_xp2` + `decal_concrete_s_01`, spread 30; Brick → `fx_impact_concrete_01_s` +
  `decal_brick_s_01`, spread 30; Body / Chest → `fx_impact_soldier_body_s`, spread 90, no decal; Head →
  `fx_impact_soldier_body_s`, spread 0; Water → `fx_impact_water_s`, no decal; Glass → `fx_impact_glass_01_s` +
  `decal_glass_s_01`, spread 5; Metal → `fx_impact_metal_01_s` + `decal_metalarmor_s_01`, spread 45; Default (79
  material indices) → `fx_impact_generic_01_s`; 65 property rows empty.
* Caspian `levels/mp_007/mp_007/materialgrid_win32/grid`: 118 rows, 299 / 123 relations, generic default on 63 material
  indices, 16 distinct 5.56 effects (tarmac present, steampipe absent), Concrete → `_s_xp2` + `decal_concrete_s_01`.

### 5.4 Atlas [verified, `bf3dump dump` / `reshead`; plan §7.10 item 6, gap VFX §5]

Metro `levels/mp_subway/mp_subway/emittersystem_win32`: 52 TextureInfos (other levels 48..53). Base
`emittertexturebase_win32`: format 2 (DXT5), flags 0x22 (sRGB), 4096 × 4096, 13 mips. Normal
`emittertexturenormal_win32`: format 20 (NormalDXT5RGA), flags 0x20, 1024 × 2048, 12 mips. Name rule (`_n`, `_nr`,
`_nrm` suffix or `normalmap` substring → normal atlas) gives 42 base + 10 normal, 3 degenerate `defaultnormalmap`
aliases (min > max); all non-degenerate rects pixel exact with the min corner ≡ 3 (mod 4) px (3 px gutter). Other
levels' normal atlases have other sizes, so pixel rects use the header sizes. Metro's superbundle has 280
EffectBlueprints and 1,053 EmitterDocuments using 42 diffuse + 10 normal names, 0 missing, all entries used.

## 6. Algorithms and decisions

### 6.1 Vocabulary and the census

`fx_types.h` fixes the 32 `ProcessorType` and 10 `EvaluatorType` values with their exact EBX type names
(`fx_types.cpp`, round-trip tested in `fx.contract`), plus the EvaluatorInput, EmitterParameter, PolynomialOperation
and EmittableType enumerator names. A type or enumerator outside the tables fails the item and is recorded in
`CompileContext::unknown_*`; G1 requires both lists empty. EBX type names compare case-sensitively.

### 6.2 Chain resolution

1. Walk `RootProcessor → NextProcessor` (internal references only), at most kMaxChainLength links, with a visited set:
   a cycle, an external link, a second instance of a type, or a non-processor instance is an error.
2. Skip `Enable == false` processors (counted), except that a `BaseEmitterData` is always read for its asset.
3. **BaseEmitterData rule** [verified census decision, §4]: if the document's reachable chain holds nothing but the
   BaseEmitterData, the processors come from the base document `EmitterAsset` (resolved recursively up to
   kMaxBaseDepth; retail depth 1); otherwise the document's own chain is used and the base is provenance only. The
   template always comes from the document. The emitter is flagged `kEmitterDerived` in the first case. The orphan
   copy is never read (it equals the base chain in all 76 retail cases).
4. **EmitterData**: each `EmitterAssets[]` document compiles into the same EffectDef as a child (flag
   `kEmitterChild`, index in the parent's `children`), up to kMaxChildrenPerEmitter and kMaxChildDepth (retail 4 and
   1).
5. Each present slot stores `EvaluatorInput`, `DefaultValues` and at most one evaluator (`Pre`) in
   `EmitterDef::evaluators`.

### 6.3 Canonical processor order

The simulator runs present slots in `ProcessorType` order (the enum is the order), stage by stage: Structural (base,
children) → Emitter (SpawnRate) → Spawn (position, direction, speed, size, rotation, rotation speed, orientation,
animation frame, animation speed) → Update (age, camera proximity, drag, gravity, local force, turbulence, velocity
integration, rotation, sizes, colour, transparency, alpha levels, texture coords, orientation, clip scale). Data order
varies (§4), but no processor reads another's output except through the particle state, and the only order-sensitive
pairs are resolved by physics: drag, then forces (each `v += a·dt`), then `x += v·dt` (semi-implicit Euler), so
SpawnSpeed's magnitude applies to SpawnDirection's direction whatever the data order. Drag is `v *= exp(−k·dt)` so a
sub-step split does not change it [inferred].

### 6.4 Processor semantics

`v = Pre ? evaluate(Pre, input) : DefaultValues` (a Vec4; scalar evaluators broadcast). `t` = particle age / life, `te`
= emitter normalised time. Particle-state fields named as in `ParticleVertex`.

| processor | rule | basis |
|---|---|---|
| BaseEmitter, Emitter | structural (§6.2, §6.5 children) | [verified] |
| SpawnRate | emitter stage: rate = SpawnRate × v.x (§6.5) | [plan §3.14] |
| SpawnPosition | local position = v.xyz (Box / Sphere / CameraProximity evaluators; default origin) | [inferred] |
| SpawnDirection | dir = normalize(v.xyz) (RotateVector cone around DefaultValues.xyz; default +Y); blended toward normalize(position) by DirectionFromEmitterOrigin; InheritSpeedAndDirectionFromEmitter adds the source velocity | [inferred] |
| SpawnSpeed | velocity = dir × Speed × v.x | [plan] |
| SpawnSize | size.xyz = Size × v.xyz | [plan, golden G3] |
| SpawnRotation | rotation = Rotation × v.x × π rad (Rotation 1.0 on 2,353, 180/360 on 33) | [inferred unit, R19] |
| SpawnRotationSpeed | rotation speed = RotationSpeed × v.x × π rad/s | [inferred unit] |
| SpawnOrientation | axis = v.xyz Euler degrees (RandomXYZ ±180/360; ParticleMesh only, 255) | [inferred] |
| SpawnAnimationFrame | frame = Pre ? v.x : AnimationFrame (0-based flipbook position; Random [0, N] dominant, AnimationFrame 0 with Random [1, 32] rules out a product) | [verified pattern, inferred meaning] |
| SpawnAnimation | frame rate = AnimationSpeed × (Pre ? v.x : 1) frames/s; the (0,0,0,0) DefaultValues are ignored (1,055 animated emitters have no Pre) | [verified pattern] |
| UpdateAge | life = Lifetime × v.x (spawn time); age += dt_e; t = age / life; dies at t ≥ 1 | [plan] |
| UpdateCameraProximity | compiled and carried; no-op in VFX1 (VFX7) | deferred |
| AirResistance | velocity *= exp(−DragFactor × v.x × dt_e) | [inferred form] |
| Gravity | velocity.y −= Gravity × v.x × dt_e (positive pulls down, §4) | [verified sign] |
| LocalForce | velocity += R_emitter · (LocalForce ⊙ v.xyz) × dt_e | [inferred] |
| Turbulance | velocity += Intensity × v.x × n(serial, age) × dt_e, n a unit vector from value noise keyed by (seed, serial) with knots every 1/30 s of age, linearly interpolated (dt-partition independent) | [inferred, R19] |
| UpdateLinearVelocity | position += velocity × dt_e (absent: the particle does not move) | [inferred; present on every moving emitter] |
| UpdateRotation | rotation += rotation speed × v.x × dt_e | [inferred] |
| UpdateSize, UpdateSizeX/Y/Z | size = spawn size ⊙ (UpdateSize v) ⊙ (v_x, v_y, v_z of the axis processors); Pivot carried | [golden G3] |
| UpdateColor | rgb = Color ⊙ (Pre ? eval.rgb : 1) (DefaultValues.xyz == Color on all 2,479, so this equals the general rule without Pre) | [verified] |
| UpdateTransparency | a = v.x; CullThreshold carried | [golden G2/G3] |
| UpdateAlphaLevelMin / Max / Scale | alpha_min = MinLevel × v.x, alpha_max = MaxLevel × v.x, alpha_scale = v.x; Exponent carried per emitter (renderer erosion) | [inferred, R19] |
| UpdateTextureCoords | frame += frame rate × v.x × dt_e (Pre: a playback-rate curve, 1 → 0.2–0.8); TextureInfo1 → `TextureRef` | [verified pattern] |
| UpdateOrientation | renderer marker (on every quad emitter), no state | [verified] |
| UpdateClipScale | Lookup (132 int16) carried for VFX2 | deferred |

Point lights (`ActAsPointLight`): one `ParticleLight` per live particle: radius = PointLightRadius, intensity =
clamp(cubic(t), PointLightMinClamp, PointLightMaxClamp) × U[RandomIntensityMin, Max] (drawn once per particle),
colour = PointLightColor × intensity, position = particle + PointLightPivot [plan §3.14; merge is VFX3].

### 6.5 Spawn, age and emitter lifetime

* Emitter clock: dt_e = dt × TimeScale; window L = template Lifetime (0 = continuous until stopped).
* **Rate mode** (DistributeOverTime true): an accumulator starts at **1** (at least one particle spawns, plan §3.14);
  each sub-step adds rate × dt_e and spawns floor(acc), keeping the fraction; spawned particles get sub-step age
  offsets so a fast emitter does not clump [inferred].
* **Burst mode** (DistributeOverTime false): floor(1 + rate × L) particles at the window start (`_1p_light_*`: 1).
  A continuous emitter in burst mode behaves as rate mode [inferred, counted by the census].
* DistributeOverDistance: the accumulator adds rate × metres travelled by the source instead [inferred].
* **Cap**: a spawn with `MaxCount` particles live is discarded and counted (`spawns_capped`); no queue. A spawn past
  the world's particle budget is dropped (`spawns_dropped`).
* Window: spawning stops at e_age ≥ L. RepeatParticleSpawning wraps e_age and re-arms the accumulator / burst; such an
  emitter ends only on stop. An emitter is done when its window is over (or it was stopped) and its particles are
  dead; KillParticlesWithEmitter drops them at stop.
* No UpdateAge slot (none remain after §6.2 in retail): particles die when the emitter ends [inferred].
* EfEmitterNormTime: te = L > 0 ? min(e_age / L, 1) : 0 [inferred]. EfOne = 1. EfVelocity = |velocity| [inferred,
  2 uses].
* Children: at a parent particle's spawn one instance of each child emitter starts at the particle, follows its
  position each step (orientation from its velocity) and is stopped when the particle dies [inferred; gap VFX §10
  "child timing" open]; the child pool per parent instance is the parent's MaxCount.
* Effect: each component starts after effect + component StartDelay if its tier matches and its SpawnProbability draw
  succeeds (one draw per component per start). An effect instance is done when every component is done; sound and
  visual-environment components emit `FxEvent`s.

### 6.6 Evaluators

Polynomial, PolynomialColorInterp: `fx_types.h` (header-inline, G2). PolynomialOperator: clamp(op(first(t),
second(t)), MinClampResult, MaxClampResult), op ∈ {×, +}. Random / RandomXYZ: lerp(Min, Max, u) per particle and slot
(Min > Max legal). Box: Pivot + (u − ½) ⊙ Dimensions [inferred full extents]. Sphere: Pivot + Scale ⊙ (uniform in a
ball of Radius) [inferred]. RotateVector: a direction in a cone of Angle degrees around the slot's DefaultValues.xyz,
uniform in solid angle; RotateWithinPlane fixes the azimuth [inferred]. SampleTexture: a 1-D gradient
(`GradientData`) sampled at t, lerp(IntensityMin, IntensityMax, sample) [inferred]. CameraProximity: compiled, VFX7.
**EmitterParameter** (2,604 non-None uses): carried in `EvaluatorDef::parameter` and `EffectStart::params`, evaluated
as None in VFX1 (open Q3); `kEmitterHasParameterisedEvaluator` marks affected emitters. None of the gate effects uses
one [verified].

### 6.7 Pools

* `MaxCount`: per emitter instance (§6.5).
* `MaxInstanceCount`: one pool per `EffectDef` per `FxWorld` (global, not per owner) [plan §3.14 "start with a global
  steal-oldest pool"]. When full, ResetInstanceWhenStarted (true on all 1,101) kills the oldest instance and counts
  `stolen_instances`; otherwise the start is refused (`refused_starts`).
* World bounds `FxWorldConfig` (effects 4,096, emitter instances 16,384, particles 65,536): a start that does not fit
  is refused; storage grows geometrically up to these caps and is never freed during a session.

### 6.8 Time steps and determinism

* dt is clamped to `max_dt` (2 s, counted) and split into ceil(dt / `max_substep`) equal sub-steps (≤ 0.25 s), so a
  long frame cannot tunnel a burst window or blow up drag. Fixed and variable dt use the same path.
* Random numbers are counter-based (`fx_key`: splitmix64 over (start seed, component, particle serial, slot)); spawn
  serials count per emitter instance; nothing depends on draw order, thread count or item partition (G8).
* `run_items` writes only its own emitter's output range (sized in `begin_step` from MaxCount); `end_step` compacts in
  a fixed order (instance slot, component, emitter), particles in serial order.
* NaN guard: after the update stage any non-finite position, velocity, size, colour or frame kills the particle
  (`nonfinite_kills`); the renderer never sees a NaN.

### 6.9 Emitter atlas

`EmitterAtlas::load` reads the level's `EmitterSystemAsset`, the two textures' 128-byte DxTexture headers
(`read_raw_res`; no pixel chunk) and classifies entries by the name rule (§5.4). Names are compared lowercase with `\`
→ `/`. Pixel rects come from header sizes; `pixel_exact` flags a corner > 0.01 px off. Flipbooks: frame f of
frame_count frames in frame_columns columns, row-major [assumed], inside the entry rect (`frame_rect`).
`EffectLibrary::bindings` maps each emitter's texture and normal map to entries (nullptr → VFX2 draws
`systems/emitter/defaulttexture` and counts it).

### 6.10 Impact table

`ImpactTable::load` reads the level grid (`MaterialGrid::grid_of_level`), its MaterialIndexMap, InteractionGrid and
MaterialPairs, and the container's MaterialNames; cells are deduplicated. `select(projectile, surface)` takes two
PhysicsMaterialIndex bytes; `surface_material(triangle material)` maps a collision-soup material value (& 0xFFFFFF)
through MaterialPairs. An empty cell selects nothing; no DefaultMaterialIndex fallback in VFX1 (the default material's
own row already carries `fx_impact_generic_01_s`). Choosing among several `ImpactEffects[]`, spreading the direction and
placing the decal are VFX3.

## 7. Interfaces and seams

### 7.1 EffectLibrary (`effect_library.h`)

One per level session: `get(blueprint)` compiles once (cache first) and returns the same `shared_ptr<const
EffectDef>`; failures are remembered. `preload(list, threads)` at load. `bindings(def)` gives atlas entries per
emitter; `impacts()` the level's table.

### 7.2 FxWorld (`simulator.h`)

`start(def, EffectStart{transform, velocity, seed, tier, first_person, params}) → EffectHandle` (slot + generation);
`stop(handle, Stop|Kill)`, `set_transform`, `set_params`, `alive`. Per frame: `begin_step(dt, view) → n` (owner) →
`run_items(first, count)` (any threads, disjoint) → `end_step()` (owner), or `step(dt, view, lanes)`. Outputs valid
until the next `begin_step`: `outputs()` (`EmitterOutput` per live emitter instance with def, emitter index,
component, first_person, particle and light ranges, emitter transform), `particles()` (`ParticleVertex`, 80 B,
world space), `lights()` (`ParticleLight`, 32 B), `events()` (`FxEvent`), `stats()`.

### 7.3 VFX2 renderer seam

VFX2 reads `outputs()` / `particles()` per frame, takes the emittable type and render fields from
`EmitterDef::tmpl`, the texture from `EmitterDef::texture` + `EffectLibrary::bindings`, frame rects from
`EmitterAtlas::frame_rect`, and pixels from the atlas partitions named in `AtlasTextureInfo`. Sorting, batching,
shaders, half-res, soft fade and the 1p viewmodel pass are VFX2's. `FxWorld` stepping moves onto Godot's
WorkerThreadPool through `run_items`.

### 7.4 VFX3 trigger seam

Triggers call `EffectLibrary::get(name)` and `FxWorld::start` with a world transform and `seed = hash(event id)`
(shotSeq, impact id). Impacts: `ImpactTable::surface_material(triangle material)` then `select(bullet material,
surface)` → `ImpactCell {effects, debris, max_spread_degrees, mirror_impact_direction, decal, impact_sound}`. The light
pool merges `lights()` per `EmitterOutput` light range. Decals use `ImpactCell::decal`.

### 7.5 VFX4 network seam

No network ids: `EffectStart::seed` is the only cross-client input; `bf3_fx` stays out of server modules.

### 7.6 Audio seam

`FxEvent {SoundStart|SoundStop, effect, component, def, transform}` with `EffectComponentDef::asset` = the
SoundPatchAsset partition (319 sound components), and `ImpactCell::impact_sound` (MaterialRelationSoundData) for A5.
The audio track owns voices; VFX1 only emits events in a fixed order.

### 7.7 bf3dump fx

| command | output |
|---|---|
| `fx census [<superbundle or @all-mp>] [--threads N]` | counts, per-type emitter counts, unknown lists, first errors, max work units, seconds; exit 1 unless G1 holds when run on `@all-mp` |
| `fx compile <partition>...` | `format_effect_def` / `format_emitter_def` canonical text |
| `fx simulate <effect> [--emitter <doc>] [--seconds S] [--dt D \| --variable] [--seed N] [--starts K] [--golden muzzle1p\|oilfire]` | per-step live counts, per-component start fractions, stats; with `--golden`, the G3 / G4 checks and exit code |
| `fx check [<sb>] [--seconds S]` | G7 / G8 over every effect |
| `fx bench` | G9 ns per particle per step (median of 5) |
| `fx atlas <LEVEL>` | G6 entries and texture headers |
| `fx impacts <LEVEL> [--projectile=bullet\|<pair partition>]` | G5 rows |

All commands take the usual `--cache <root>` and open the install read-only through the repo's readers.

## 8. Caching

Kind `fx-deps` (key: blueprint SHA1, `kFxConverterVersion`, `cache_engine_id()`, `kFxDepsFormatVersion`, params
`effect=<name>`) → the compile's source list; kind `fx-effect` (key: the SHA1s of those partitions in list order, zero
digest for an absent one, `kFxConverterVersion`, engine, `kFxBlobFormatVersion`, params `effect=<name>;deps=<SHA-256 of
the list>`) → blob `OBF3FXEF` v1 (`BlobWriter`). This is the WeaponDef two-entry pattern (`gameplay/weapon_blob.h`):
a warm lookup parses no EBX, a patched emitter document changes the effect key, a patched blueprint both. Through
`cached_convert` into the session's Cache root (`user://` in the game, `--cache` in tools); never next to the install.
`kFxConverterVersion` bumps on any §6 semantic change. The atlas and impact table are not cached in VFX1 (header reads
and one grid walk; `fx.install` reports their load time; cache them in VFX2 if > 50 ms per level).

## 9. Performance

Prototype 24.7 ns/particle/frame (plan §7.10 item 8) with a BF3-like chain. Design: SoA per emitter instance
(position, velocity, size, colour, age, life, frame, rotation, serial), present-slot dispatch per emitter (a switch
over a short array of present processors, no virtual calls), header-inline polynomials. 16k particles × 50 ns = 0.8 ms
on one lane; spread over 4–8 lanes in VFX2. Compile cost target: census over `@all-mp` < 30 s on all lanes, a warm
`get` < 50 µs (reported, not gated).

## 10. Hostile-input bounds

* Compile: `CompileContext::budget` (`gameplay::WorkBudget`, 2^20 units; one per instance read, array element, and
  reference followed) ends any graph; plus the structural caps in `fx_types.h`: chain 64 (retail 26), base depth 4
  (1), child depth 2 (1), children per emitter 16 (4), MaxCount 16,384 (1,051), components 256 (41), emitters per
  effect 512 (46), Σ particles 2^17 (3,000), MaxInstanceCount 1,024 (100), gradient samples 4,096 (512), clip lookup
  4,096 (132), z-occlusion lookup 4,096 (33), |float| ≤ 1e7 (7.6e6) and finite, names ≤ 1,024. Unknown enumerators,
  wrong instance types where a processor / evaluator is expected, external chain links and cycles are errors.
* Blob decode: counts checked against remaining bytes and the same caps before allocating; every enum, index and
  float validated; trailing bytes refused; re-encode identical.
* Atlas: TextureInfos ≤ 4,096, UV finite; header sizes ≤ 16,384 and mips ≤ 15. Impact table: rows ≤ 256, map ≤
  4,096, ImpactEffects per cell ≤ 64.
* Simulation: world caps (§6.7), dt clamp and sub-steps (§6.8), the NaN guard, MaxCount per instance; a def whose
  max_particles exceeds the world budget is refused at start.
* `fx.fuzz` runs in the dev and asan presets: mutated retail-shaped and synthetic EBX graphs through an in-memory
  resolver, truncated / bit-flipped / count-inflated blobs, NaN / huge parameters and extreme dt sequences.

## 11. Test plan

| ctest | executable / command | content | install |
|---|---|---|---|
| `fx.contract` (now) | `test_fx_contract` | header compile, record sizes 80 / 32, 32 / 10 vocabularies, stage order, name tables round-trip, polynomial / colour / RNG goldens, default values | no |
| `fx.unit` | `test_fx` `--test-suite=fx-unit` | synthetic defs: spawn accumulator, burst, window, repeat, cap, TimeScale, sub-steps, drag / gravity sign, size / colour product rules, frame rules, child start / stop, pools (steal, refuse), determinism across lanes and partitions (G8), blob round-trip, cache warm / cold through a temp Cache root | no |
| `fx.install` | `test_fx` `--test-suite=fx-install` | G1, G3, G4, G5, G6, G7, cold vs warm `format_effect_def` equality, G11; exits 77 without an install | yes |
| `fx.fuzz` | `test_fx` `--test-suite=fx-fuzz` (also in the asan preset with the PATH modification of `m1.contract`) | G10 | no |
| `tools.fx_bench` | `bf3dump fx bench` | G9, RUN_SERIAL, 77 without install, report only under asan | yes |

No Godot check in VFX1 (nothing links `bf3_fx` into `bf3_godot` yet).

## 12. Deferred items

| item | wave |
|---|---|
| Atlas pixel upload, normal decode (x = 2R−1, y = 2A−1), MultiMesh batches, shaders (SAQ/DAQ/WAQ/mesh, flipbook blend, alpha levels and Exponent, soft fade, clip scale, lighting blends), sorting, half-res, 1p viewmodel pass, default-texture fallback, atlas / impact caching if slow | VFX2 (M2a) |
| Weapon / tracer / impact triggers, choice among `ImpactEffects[]`, spread and mirror, decals, light pool ≤ 64 with per-emitter merge, instancer policy beyond the global pool, CullDistance culling, 64-bot firefight budget | VFX3 (M2a) |
| Seeds from event ids, late-join derivation, link-time exclusion of `bf3_fx` from server modules | VFX4 (M3) |
| Explosions, vehicle effects, VisualEnvironment effects, camera shake | VFX5 (M4) |
| Destruction effects | VFX6 (M6) |
| `EmitterParameter` inputs, UpdateCameraProximity / CameraProximity weather, level `EffectReferenceObjectData`, quality-tier UI, GPUParticles fallback for the largest emitters | VFX7 (M7) |
| Sound components' playback | audio track (A5) via `FxEvent` |

## 13. Risks

* **Inferred semantics** (R19): base-emitter rule (decided by census), rotation unit, drag form, turbulence noise,
  child timing, alpha-level meaning, Box / Sphere / RotateVector shapes, frame 0- vs 1-based. Mitigation: each is one
  function with a unit test; `kFxConverterVersion` bumps on change; VFX2 compares against footage.
* **Cost**: the full 32-processor path may exceed 50 ns/particle; mitigations: per-emitter specialised update loops
  for the common chains, lanes.
* **Determinism vs float**: byte-identical only for the same binary and dt sequence; cross-client agreement is by seed,
  not bits (plan VFX4).
* **Census drift**: `@all-mp` may hold documents no level uses (223 unreferenced); they must still compile (G1).
* **Shared machine timing**: G9 and the census time are noisy; G9 uses a median and a 2× margin.

## 14. Open questions

1. Should `ImpactTable::select` fall back to `DefaultMaterialIndex` when a cell is empty? (VFX1: no; 65 Metro 5.56
   cells are empty.)
2. `MaxInstanceCount` scope: global per def (chosen) vs per owner (weapon, vehicle).
3. `EmitterParameter` semantics (Distance 1,703, Vec 722, ...): what sets them and how they combine with `t`.
4. Child emitter timing: does a child stop or live out when its parent particle dies?
5. Does PointLightRadius scale with particle size? (VFX1: no.)
6. Flipbook order (row-major assumed) and whether frames blend (UpdateTextureCoords TextureBlendValue).

## 15. Files

Created now (this commit):

* `docs/design/m2a-vfx1.md` (this file)
* `src/bf3_fx/CMakeLists.txt`
* `src/bf3_fx/include/bf3/fx/fx_types.h`, `effect_def.h`, `effect_compiler.h`, `effect_cache.h`, `effect_library.h`,
  `emitter_atlas.h`, `impact_table.h`, `simulator.h`
* `src/bf3_fx/src/fx_types.cpp` (the name tables; the only stub the tree needs)
* `tests/fx_contract_tests.cpp`

Modified (additive "M2a VFX1" blocks only): top-level `CMakeLists.txt` (appended `add_subdirectory(src/bf3_fx)`),
`tests/CMakeLists.txt` (appended `fx.contract`), `README.md` (one line under the ctest list and an "Effects (M2a
VFX1)" section at the end).

Created by the implementation: `src/bf3_fx/src/{effect_compiler,effect_cache,effect_library,emitter_atlas,
impact_table,simulator,census}.cpp`, `tools/bf3dump/fx_commands.{h,cpp}`, `tests/fx_unit_tests.cpp`,
`tests/fx_install_tests.cpp`, `tests/fx_fuzz_tests.cpp`. Modified additively: `src/bf3_fx/CMakeLists.txt`,
`tools/bf3dump/CMakeLists.txt` (link `bf3_fx`), `tools/bf3dump/bf3dump.cpp` (usage line and dispatch of `fx`),
`tests/CMakeLists.txt` (`fx.unit`, `fx.install`, `fx.fuzz`, `tools.fx_bench`), `README.md` (its "Effects (M2a VFX1)"
section and ctest line grow with the commands and tests).

## 16. Baseline at design time

Full ctest on m2a/vfx before any VFX1 code (dev preset, OPENBF3_GODOT set, shared machine): all rows pass except
`godot.m1` (E1 teleport: "the soldier did not arrive at the course start within 10 s") and `godot.m1_load` (E4 cold
19.0 s > 15 s, E5 warm 6.2 s > 5 s), both Godot load-time / timing rows that VFX1 cannot affect (no Godot-side change);
the rerun after this commit is in the commit message.
