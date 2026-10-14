# Gap r2: Visual effects subsystem (EffectBlueprints, emitter graphs, material-pair impacts)

Date 2026-10-14. Round-2 gap report for `PORTING-RESEARCH.md` (no VFX decision, no slices; M6 "effects as
GPUParticles", M7 "FX"; 01-weapons §616-620 assumed an untested emitter→GPUParticles3D compiler). Handoff style.
Tags: **[verified]** = measured on the retail install / read in source this session; **[report claim]** = from a
document I did not re-derive; **[inferred]** = my reasoning. The task text named two output files; this is the one
named under OUTPUT (`gap-r2-vfx-effects-system.md`).

Read-as-is: the install was only opened read-only (copied IW4L readers). No BF3 data is under `openBF3`; every dump
is in `S = C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\vfx`.

## 0. Tools and data (all in scratch)

* `S\src-fxdump`: a copy of the probe dumper (`probe-ebx`) with three read-only commands added:
  `fxall <sb> <out.jsonl> <prefixes> <types>` (one JSON line per matching partition), `refsto <sb> <prefix>` (every
  external reference into `fx/`, with field paths), `texout <sb> <res> <out>` (DxTexture header + chunk). Built with
  `CARGO_TARGET_DIR=S\target`; exe `S\target\release\fx-dump.exe`.
* `S\fx-all.jsonl` (72 MB): 5,420 partitions over `@all-mp` (all `fx/` partitions, plus any partition that holds an
  EffectBlueprint, EmitterDocument, EmitterTemplateData, EmitterSystemAsset, EffectManagerSettings or
  WeaponFiringEffectsModifier). `S\refs-to-fx.tsv`: 39,946 references into `fx/`.
* Scripts `S\scripts\census1..4.py`, `fxtable.py`, `gridfx.py`, `show.py`, `bc3.py`, `normatlas.py`, `normrgn.py`.
  Outputs `S\census1.txt`, `S\census2.txt`, `S\emitters.json`, `S\effects.json`, `S\spawnrows.json`, `S\muzz1p.txt`.
* Atlas textures (Metro): `S\tex\subway_emittertexture{base,normal}_win32.{hdr,bin}`. Material grids: Caspian
  `S\..\probe-ebx\out2\levels__mp_007__...grid.json`, Metro `S\grid-subway\`.
* Benchmarks: Godot `S\bench\` (scratch project, GDScript, not shipped), C++ `S\cpusim\sim.cpp`.

## 1. Census over @all-mp (task 1)

### 1.1 Type counts in effect partitions [verified, `S\census1.txt`]
5,420 partitions: 3,410 EmitterTemplateData-primary (emitter documents), 1,101 EffectBlueprint, 29
EmitterSystemAsset (one per level), plus some weapon/settings partitions pulled in by type. 173 instance types. The
effect-relevant ones:

| group | type: instances |
|---|---|
| containers | EffectBlueprint 1,101 · EffectEntityData 1,101 · EmitterEntityData 4,353 · EmitterDocument 3,410 · EmitterTemplateData 3,410 · SoundEffectEntityData 319 · VisualEnvironmentEffectEntityData 88 · EffectReferenceObjectData 152 (nested placements inside `fx/` prefabs) |
| evaluators | PolynomialData 16,410 · RandomEvaluatorData 13,200 · RandomXYZEvaluatorData 2,313 · PolynomialColorInterpData 1,947 · BoxEvaluatorData 1,642 · SphereEvaluatorData 1,174 · RotateVectorData 1,156 · PolynomialOperatorData 107 (85 Multiplication, 22 Addition) · CameraProximityEvaluatorData 46 · SampleTextureData 30 |
| processors | see 1.2 (32 types) |

These numbers match the independent `types` census in `critique-r2\fx-types.tsv` (1,101 / 3,410 / 4,353)
[verified, second source].

**What an EffectBlueprint contains** [verified, `census3.py`]: `EffectEntityData.Components` holds only
**EmitterEntityData 4,353, SoundEffectEntityData 319, VisualEnvironmentEffectEntityData 88**. No PointLight,
SpotLight, Decal or camera-shake entity sits inside any EffectBlueprint. All 1,101 have `NeedNetworkId False`,
`InterfaceHasConnections False`, `AlwaysCreateEntityBus{Client,Server} False`. Nesting is flat: no
EffectBlueprint references another EffectBlueprint as a component.

Folders (`fx/<x>` for EffectBlueprints): ambient 179 + dlc ambient 237, destruction 142 + dlc 190, vehicles 106 + 21,
weapons 106 + 2, impacts 71 + 9, explosions 5, others ≤ 7 [verified].

### 1.2 Processor chains [verified, `S\census2.txt`]
Each EmitterDocument has `RootProcessor`; processors link through `NextProcessor`; each processor may have a `Pre`
evaluator (the evaluator tree is walked through `Pre`/inputs). Chain length min 1, median 21, max 26.

| processor (ProcessorType enum has 33 + count) | emitters using | % of 3,410 |
|---|---|---|
| SpawnRate, UpdateAge | 3,334 | 97.8 |
| SpawnSize, SpawnDirection, UpdateLinearVelocity, SpawnSpeed, UpdateSize | 3,297–3,315 | 96.7–97.2 |
| SpawnPosition 3,049 · SpawnRotation 2,983 · UpdateRotation 2,920 · SpawnRotationSpeed 2,899 · Gravity 2,841 · AirResistance 2,557 | | 75–89 |
| UpdateColor / UpdateTransparency / UpdateTextureCoords 2,419 each · UpdateOrientation 2,380 · UpdateClipScale 2,248 | | 66–71 |
| UpdateAlphaLevelScale 1,900 · SpawnAnimationFrame 1,872 · SpawnAnimation 1,858 · LocalForce 1,770 | | 52–56 |
| Turbulance 786 · UpdateAlphaLevelMin 743 · UpdateSizeX 654 · UpdateSizeY 585 · UpdateAlphaLevelMax 541 | | 16–23 |
| SpawnOrientation 285 · **EmitterData 174** · **BaseEmitterData 121** · UpdateCameraProximity 46 · UpdateSizeZ 16 | | 0.5–8.4 |

**Top-N coverage** (emitters whose whole processor set lies within the N most common types): N=22 → 35.2 %,
N=26 → 67.8 %, N=28 → 89.6 %, N=30 → 98.2 %, N=31 → 99.5 %, **N=32 → 100 %**. Evaluators: 7 → 94.7 %,
10 → 100 %. Unlike audio (37 of 46 for 98 %), the long tail is short: **implement all 32 processors and
10 evaluators**. Each is a few lines [inferred].

Two structural processors:
* `EmitterData.EmitterAssets[]` (174 emitters, 271 refs): child emitters, e.g. `burningpaper_mainparent` → child
  fire + parent mesh. Read as sub-emitters spawned per parent particle [inferred].
* `BaseEmitterData.EmitterAsset` (121; for 76 emitters it is the only reachable processor, `Enable False`): a derived
  emitter pointing at a base EmitterDocument. The partition still holds an unreachable full processor copy
  (76 orphan SpawnRate/UpdateAge/... sets) [verified]. Whether the runtime uses the base chain, the orphan copy or
  an override merge is not determined (§9).

### 1.3 Emitter template flags [verified, `census2.txt`]

| property | count of 3,410 |
|---|---|
| EmittableType | ScreenAlignedQuad 1,899 · ParticleMesh 931 · DirectionAlignedQuad 558 · Quad 18 · WorldAlignedQuad 4 · Trail 0 · Point 0 |
| `Mesh` set (mesh particles) | 937 (931 ParticleMesh + 6 SAQ with a mesh) |
| `LocalSpace` | 563 |
| `FollowSpawnSource` | 21 |
| `ActAsPointLight` (lit-emitting particles) | 367 |
| `Emissive` | 382 · `Opaque` 661 · `ForceFullRes` 344 (others render half-res) · `ForceNiceSorting` 26 |
| soft particles (`SoftParticlesFadeDistanceMultiplier>0`) | 180 |
| lit by scene (`LightMultiplier>0`) | 3,348; `VertexPixelLightingBlendFactor>0` 189; `LightWrapAroundFactor` set on 2,983 |
| `KillParticlesWithEmitter` 105 · `RepeatParticleSpawning` 556 · `TransparencySunShadowEnable` 154 · `ExclusionVolumeCullEnable` 139 |
| **collision** | **none**: the ProcessorType enum has no collision processor and no instance exists [verified, VU-Docs `ProcessorType.yaml`, census]. Mesh particles (shells, debris) fall and die by age. |

Quality tiers: `EmitterEntityData.HighEndPc False` on 616, `LowEndPc False` on 1,031 (the `_lowend_` twins), so a
tier filter is needed at instancing [verified].

### 1.4 Particle counts [verified, `effects.json`]
`MaxCount` per emitter: p50 13, p90 50, p99 200, max 1,051; Σ = 90,904. Per effect (Σ MaxCount of its emitters,
one instance): p50 46, p90 200, p99 894, max 3,000 (`fx_mp1_hurricanrain_cameraattached`); next are XP5 C130
health-critical (1,561) and XP4 building-aftermath sparks (1,550). Emitters per effect p50 2, p90 8, max 40.
`EffectEntityData.MaxInstanceCount`: 1 for 850 effects, 10 for 107, 20 for 80, 30 for 29, up to 100.

| class (name match) | n | Σ MaxCount p50 / max | light emitters |
|---|---|---|---|
| muzzle | 65 | 13 / 301 | 59 in 36 effects |
| impact | 85 | 55 / 894 | 87 in 51 |
| explosion | 29 | 121 / 825 | 58 in 23 |
| tracer | 12 | 5 / 5 | 0 |
| shell | 11 | 2 / 2 | 0 |
| destruction | 375 | 68 / 1,550 | 141 in 78 |

## 2. Semantics: spawn, age, processors, polynomials (task 2)

### 2.1 Time bases [verified data pattern; semantics inferred]
* `EmitterTemplateData.Lifetime` = **emitter (spawn-window) duration**: 0.1 s on 1,654 emitters (one-shot bursts),
  **0 on 468** (continuous: ambient, loops), 0.2 s 231, 5 s 167 …
* `UpdateAgeData.Lifetime` = **particle lifetime**: 2.0 s 449, 1.0 s 400, 3.0 s 393 … Normalised particle age
  `t = age / UpdateAge.Lifetime` ∈ [0,1], particle dies at 1. `EvaluatorInput` picks the evaluator input:
  `EfNormTime` (particle t), `EfEmitterNormTime` (emitter age / emitter lifetime), `EfOne` (constant 1), plus rare
  `EfVelocity` (2) [enum verified; mapping inferred].
* `SpawnRateData.SpawnRate` = **particles per second**, and `MaxCount` is an independent **hard cap** on live
  particles. Evidence: continuous emitters satisfy `MaxCount ≈ rate × particle life + 1`
  (`em_amb_mp12_oilfire_l_fire_01` 16/s × 5 s → 81; `clouds_jumpthrough_01` 2/s × 50 s → 101;
  `mp_015_camprox_debris` 50 × 1 → 51; `godrays_01` 4 × 1 → 5), while bursts are written as huge rates over a 0.1 s
  window, capped by MaxCount (`em_impact_gips_meshp_02_s` 10,000/s × 0.1 s, MaxCount 20;
  `em_grenade_frag_shrapnel_sprite_01` 100,000/s, MaxCount 4). For one-shot emitters, rate × window / MaxCount has a
  median of 5.0 (n 1,738), so the cap usually binds [verified stats, `census4.py`].
* At least one particle spawns on start even when rate × window < 1 (1p flash: 1/s × 0.1 s, MaxCount 1; light: 15/s ×
  0.05 s) → accumulator starts at 1 [inferred].
* `DistributeOverTime` (2,968 true) spreads spawns over the window; `False` (442) = burst at start [inferred];
  `DistributeOverDistance` (163) and `TrailSegmentLength` = spawn per metre of emitter travel (smoke trails)
  [inferred]. A `Pre` on SpawnRate (578 Polynomial, 18 SampleTexture, 11 Random), mostly on `EfEmitterNormTime`,
  scales the rate over the emitter's life [inferred].

### 2.2 Polynomials
* **`PolynomialData.Coefficients` (x,y,z,w) = (c3,c2,c1,c0): `f(t) = ((x·t + y)·t + z)·t + w`.** Evidence
  [verified statistic]: over 16,410 PolynomialData the non-zero patterns among x,y,z are only `z` (5,786),
  `yz` (3,474), `xyz` (5,554), constant (1,596). There is never `x` alone, `y` alone, `xy` or `xz`. That is the
  signature of fitted curves of degree 1, 2, 3, so z is linear, y quadratic, x cubic. Cross-check: fades read
  correctly only with w = c0. Examples: the 1p flash alpha `{w 1, y 0.953, z −1.95}` gives f(0)=1, f(1)=0; the light
  intensity `{w 0.971, x −1.712, y 3.967, z −3.264}` gives 0.971 → ≈0. The handoff's earlier "(c3,c2,c1,c0)"
  [inferred] agrees.
* Output: `clamp(f, MinClamp, MaxClamp) × ScaleValue` [inferred order]. The common triples (min,max,scale) are
  (0,1,1) 9,380; (0.1,1,1) 1,301; (0,1,2) 621. On 9,815 of the 16,410, an endpoint lies outside the clamp range,
  so the clamp is active, not decorative [verified count].
* Evaluator values multiply the processor's own scalar (e.g. `SpawnSize.Size 0.4 × U[0.8,1]`, `UpdateSize` =
  spawn size × P(t)); `DefaultValues` (Vec4) is the value when no `Pre` is set [inferred].
* **`PolynomialColorInterpData`: colour = lerp(Color1, Color0, clamp(f(t),0,1))**, with f in the same
  coefficient order. So f = 1 gives Color0. Evidence [verified statistic, semantics inferred]: across 846
  instances with a clear direction, this reading makes the colour go bright→dim (hot → cooling) in 689 cases. The
  opposite reading gives 157. Colours are HDR linear: muzzle 125–250, sparks 4,000, fire 500.

### 2.3 Check against `fx_muzz_generic_rifle_1p` (M16A4 `FireEffects1p`) [verified values, `S\muzz1p.txt`]
The M16A4's `FiringFunctionData.FireEffects1p` = {`fx_muzz_generic_rifle_1p`, `fx_shellejection_m`,
`fx_muzz_generic_rifle_fillout_1p`}. `FireEffects3p` = the `_3p` twins + the same shell. The bullet
`556x45mmnato.TrailEffect` = `fx_tracer_generic_parent`. EffectEntityData `MaxInstanceCount 7`,
`ResetInstanceWhenStarted`.

| emitter | p(spawn) | type | Max | emitter / particle life | rate | key curves |
|---|---|---|---|---|---|---|
| `_1p_flash` | 0.75 | SAQ | 1 | 0.1 / 0.05 s | 1 /s | size 0.4·U[0.8,1]·P[0.443,−0.881,0.636,0.798]; alpha P[0,0.953,−1.95,1]; colour lerp((48.7,12.0,0.74),(125,42.4,8.7), 0.794−41.2t); `muzzleflash_01` 16 frames / 4 cols, start frame U[1,16]; AlphaLevelScale exp 10 on P[0,0,0.98,0.005] clamp [0.9,1]; AirResistance 10; LocalForce (5,0.33,0) |
| `_1p_light_01` | 0.75 | SAQ, size 0 | 1 | 0.05 / 0.6 | 15 | **ActAsPointLight**, radius 0.2, colour (1000,395,66), intensity P[−1.712,3.967,−3.264,0.971] |
| `_1p_light_02` | 0.75 | SAQ, size 0 | 1 | 0.05 / 0.6 | 15 | ActAsPointLight, radius 2.5, colour (5,1.9,0.3), intensity P[0.075,−0.729,−0.366,1.012] |
| `_1p_rapidsmoke_ls_01` | 1.0 | SAQ local | 6 | 0.1 / 0.05 | 100 | `Muzz_Petals_4x8` 8 f |
| `_1p_nozzleflames_ls_01` | 0.25 | SAQ local | 2 | 0.1 / 0.05 | 100 | petals, size 0.1·U[0.1,1] |

`fillout_1p`: wpnsmoke (2 × 0.8 s, `Anim_TurbulentThickSmoke01_8x32` 32 frames) + smokeburst (3 × 0.6 s).
`fx_shellejection_m`: one ParticleMesh shell (0.75 s, opaque, CullDistance 3 m) + one smoke puff. Tracer: one
`ParticleMesh` tracer (Max 4, emitter life 0 = lives with the bullet, particle 5 s) + smoke at p 0.15. A 1p shot
therefore draws ≈ 10 particles and **two point lights (each p 0.75)**.

### 2.4 Check against the RPG / M67 blast [verified, `fxtable.py`, `gridfx.py`]
The blast is **two effects**:
1. `VeniceExplosionEntityData.DetonationEffect` = `fx/weapons/grenades/frag/fx_grenade_frag_01` for **M67, RPG-7
   and the M320 LVG** (RPG: blast 112 dmg, radius 4 m; M67 radius 7 m). Contents: smoke (10 SAQ, 1 s, size 4) and a
   sound `explosion_handgrenade`, plus shrapnel sprites (4, 0.5 s, size 15·U[0.5,1], **light radius 30 m**, colour
   (25,12.1,5.8)). `CameraShockwaveRadius 0` on both.
2. The **material-grid impact effect** of the explosion's own `MaterialPair` against the surface it hit (§3). On
   Caspian and Metro, M67 explosion material (`4897afc6`, matIdx 56) and RPG (`c0a9d7ef`, matIdx 63) → generic
   ground `fx_impact_generic_01_m`, gravel `fx_impact_gravel_m`, concrete `fx_impact_concrete_01_m` + decal
   `decal_concrete_l_01`, metal `fx_impact_metal_01_m`, vehicles `fx_impact_vehiclemetal_01_m`, soldiers
   `fx_impact_soldier_explosion_01_m`, most with `decal_scorchmark_m_01`. `fx_impact_generic_01_m` = ground
   shockwave (25 SAQ, 2 s), 2 invisible light carriers (4 particles, radius 10 m, (150,45,15), high/low tier
   twins), sparks (20 DAQ, 0.6 s, colour 4,000→100, **each a light, radius 0.6**), spike (15 DAQ, fire 500 →
   smoke 0.1), child debris, 5 mesh-particle emitters (stones, metal shard, paper, plastic; 10 s, prob 0.33) and one
   VisualEnvironmentEffect.

So BF3's blast look comes mainly from the surface material, not the weapon [verified wiring; that the explosion
spawns its grid effect at the hit surface is inferred from `ExplosionEntityData.materialPair` + the grid cells].

## 3. Impact selection and the D7 material grid (task 3) [verified]
* `MaterialGridData.InteractionGrid[row].Items[col]` is a `MaterialRelationPropertyPair` with two lists.
  **`PhysicsPropertyProperties`** hold Damage (3,423 cell refs on Caspian), Penetration (1,533) and
  TerrainDestruction (60). **`PhysicsMaterialProperties`** hold **Effect (1,947), Sound (1,976), Decal (814) and
  Vehicle (284)**. The per-material `MaterialProperties` hold MaterialPropertySound (29) and Terrain (1).
* So **damage (D7) indexes rows by `PhysicsPropertyIndex`**, and **effects, sounds and decals index by
  `PhysicsMaterialIndex`**, through the same `MaterialIndexMap[idx & 0xFF]`. With the property index, the 5.56 row
  holds zero effect cells. With the material index it gives ~15 distinct impact effects. Second source: IW4L
  `level_bf3/src/collision/material.rs:131` classifies surface materials by `PhysicsMaterialIndex` [verified].
  `materialcontainer` holds 32,400 MaterialContainerPairs = 180 material × 180 property indices.
* Lookup: `row = map[proj.PhysicsMaterialIndex & 0xFF]`, `col = map[surface.PhysicsMaterialIndex & 0xFF]` →
  `MaterialRelationEffectData {ImpactEffects[] (EffectBlueprint), ImpactDebris (ObjectBlueprint),
  ImpactEffectMaxSpreadAngle, MirrorImpactDirection}` + `MaterialRelationDecalData.Decal → DecalTemplateData
  {Size, RandomSize, Rotation, RandomRotation, ClipAngle, Projected, AtlasTile …}` + `MaterialRelationSoundData`
  (the 313 pairs D15/A5 already use). The surface material comes from the same per-triangle Havok material the
  bullet ray already returns for D7.
* 5.56 (`5b6b84ac`, matIdx 1) on Caspian: generic default `fx_impact_generic_01_s` (63 surface materials), concrete
  `fx_impact_concrete_01_s(_xp2)` + `decal_concrete_s_01` (spread 30°), metal `fx_impact_metal_01_s` + metal or
  metalarmor decal, cloth, grass, gravel, sand, wood, glass (spread 5°), tarmac, water `fx_impact_water_s`,
  soldier `fx_impact_soldier_body_s` (spread 90°, no decal). Metro gives the same set with different counts, plus
  brick → `decal_brick_s_01` [verified, both grids].
* Census: 8,059 MaterialRelationEffectData over all grids (Caspian 299, Metro 223); 7,639 `ImpactEffects[]`
  references [verified]. Vehicles use `MaterialRelationVehicleData {WheelEffect 525, GroundEffect 378, TrackEffect
  304, ChassiEffect 214}` from the same grid [verified refs].

## 4. What else effects contain; lights in a firefight (task 4)
* **Lights**: **effects carry no PointLight/SpotLight entities**. All effect lighting is `ActAsPointLight`
  particles. There are 367 such emitters: `PointLightRadius`, `PointLightColor` (HDR), `PointLightIntensity` =
  cubic over particle t, `PointLightRandomIntensityMin/Max`, `Min/MaxClamp`, `PointLightPivot`. **One light per live
  particle** [inferred from emitters whose MaxCount > 1, e.g. 20 lit sparks]. The 1,726 PointLightEntityData and 373
  SpotLightEntityData of the census are level/object lights (layer `*_lights`, objects, xp_raw), not effects
  [verified by partition prefix].
* **Decals**: none inside effects. Impact decals come from `MaterialRelationDecalData` in the grid (123 on Caspian,
  110 on Metro). The 1,604 DecalEntityData are level-placed static decals [verified]. `DecalSettings` exposes
  `staticBufferMaxVertexCount`/`ringBufferMaxVertexCount`, so dynamic decals are a vertex ring buffer [inferred].
* **Sounds**: 319 SoundEffectEntityData in 302 effects. They route to D15 (`SoundPatchAsset`).
* **Screen effects**: 88 VisualEnvironmentEffectEntityData (lifetime/cull-distance/cull-angle curves → a
  VisualEnvironment blueprint: colour grading, vignette, film grain …) [verified schema + counts].
* **Camera shake**: none in effects. `ExplosionEntityData.cameraShockwaveRadius` is 0 for M67/RPG/40 mm. The
  schema has `CameraShakeNodeData` as an **audio-graph node**, consistent with D15's "CameraShake/PadRumble
  nodes feed the camera" [verified schema; that this is BF3's explosion shake path is inferred].
* **Light count, typical firefight** [inferred arithmetic from verified data]. Assume 16 visible enemies, ~4
  rounds/s each, and the 3p rifle muzzle (2 lights per shot, p 1.0, particle life 0.6 s, intensity < 0.3 after
  ~0.18 s). That is 64 shots/s × 0.6 s ≈ 38 concurrent instances. If `MaxInstanceCount 30` is a global
  steal-oldest pool, the cap gives ≈ **60 lights** (radius 5 and 0.1 m). Own 1p fire adds ≤ 7 instances × 2 × 0.75 ≈
  **10**. About 80 visible bullet impacts/s on concrete add 80 × 0.1 × 5 × 0.2 s ≈ **8**. One grenade adds
  4 (r 30 m) + 8 (r 10 m) + 20 sparks ≈ **32**. Total ≈ **80–110 short-lived point lights**, most < 5 m. Merging
  particle lights per emitter instance (one light at the centroid, summed intensity) cuts this to ≈ 50–70.
  BF3 ran tile-based deferred lighting in DX11 compute shaders, built for "many more light sources" [report claim,
  Andersson GDC 2011, https://www.slideshare.net/DICEStudio/directx-11-rendering-in-battlefield-3].

## 5. Emitter texture atlas (task 5) [verified unless tagged]
* Per level, `levels/<l>/<l>/emittersystem_win32` (`EmitterSystemAsset`) has `TextureInfos[] {TextureName, MinUv,
  MaxUv}`, `BaseAtlasTexture` = `emittertexturebase_win32` (fmt 2 DXT5, flags 0x22 sRGB, **4096²**, 13 mips, group
  `AtlasTextureGro…`), `NormalAtlasTexture` = `emittertexturenormal_win32` (**fmt 20 NormalDXT5RGA, linear,
  1024×2048**, 12 mips). Metro has 52 entries.
* **Layout solved**: the `TextureInfos` list mixes two coordinate spaces. Diffuse entries (`*_d`, `*_c`, flipbooks)
  have UVs in the **base** atlas (3 px gutters: 0.0007 × 4096). Normal entries (`*_n`, `*_nr`, `*_nrm` and four
  `defaultnormalmap` aliases) have UVs in the **normal** atlas (gutter 0.0029 × 1024 ≈ 3 px). Example:
  `anim_turbulentthicksmoke01_8x32_nr` (0.0029,0.5015)–(0.9971,0.7485) = 1,018 × 506 px of the 1024×2048 atlas.
  `smokethick_01_5x5_n` fills the top half. The emitter picks the atlas rect by `TextureInfo1.TextureName` and
  `NormalmapName` separately. Three of the four default-normal aliases have inverted (degenerate) rects that point
  at the same ≈ 8×4 px flat patch.
* **NormalDXT5RGA channels**: in every normal region, **R** (std 0.06–0.08) and **A** (std 0.10–0.12) carry
  independent variation (corr ≈ −0.05). **G ≈ 0.84 and B ≈ 0.13** are nearly constant, correlate with z rebuilt
  from (R,A) (r 0.73–0.84), and are flat in the default-normal patch (R 0.49, G 0.84, B 0.14, A 0.50).
  Recommended decode: `x = 2R−1, y = 2A−1, z = sqrt(1−x²−y²)`, ignoring G/B [inferred; sign/handedness of y
  unknown]. This matters little: 1,571 + 211 + 19 + 13 of 2,479 UpdateTextureCoords use a default normal map. Only
  ≈ 650 use real normals (turbulent smoke 324, smokethick 287, smokearm 16).
* Flipbooks: `AnimationFrameCount` frames in `AnimationFrameColumnCount` columns inside the entry's rect;
  `SpawnAnimationFrame` (random start frame) and `SpawnAnimation` (speed) [verified fields; frame order row-major
  assumed].

## 6. Godot 4.8 dev7 options (task 6)

### 6.1 Expressiveness [verified API/source]
* RenderingServer has 49 `particles_*` calls, incl. `particles_set_process_material`, `particles_emit`,
  `particles_set_subemitter`, `particles_set_draw_order` (INDEX / LIFETIME / REVERSE_LIFETIME / **VIEW_DEPTH**),
  `particles_set_trails`, `particles_set_use_local_coordinates`. Custom particle shaders have `start()`/`process()`
  with TRANSFORM, VELOCITY, COLOR, CUSTOM and **6 USERDATA vec4s** (`particles_storage.h:333`), so every BF3
  processor and evaluator (cubic polynomials, randoms with per-particle seeds, colour lerp, box/sphere spawn,
  rotate-vector, camera proximity) can be **written** in a generated particle shader [inferred].
* `ParticleProcessMaterial` alone **cannot** express BF3: it couples emission rate = amount / lifetime and has
  curve textures, not per-emitter evaluator trees, `EfEmitterNormTime` inputs, PolynomialOperator, BF3's
  independent rate vs MaxCount cap, spawn probability, or alpha-level erosion [inferred from its property list].
* **Per-system cost is the issue**: `ParticlesStorage::update_particles()` walks every active particles instance
  and runs its own compute dispatch(es) (`particles_storage.cpp:1442-1624`, dispatches at `:1222-1350`). A BF3
  impact is 5–15 emitters, so mapping emitter instance → particles instance gives hundreds to thousands of
  dispatches per frame.
* Sorting: VIEW_DEPTH sorts inside one particles instance only (`:1248`). Across instances, Godot sorts transparent
  draws by instance. Soft particles: StandardMaterial3D `proximity_fade` (reads depth, Forward+). Lit particles:
  per-pixel or per-vertex spatial shading with clustered lights; custom `light()` allows BF3's wrap-around factor.
* Forward+ lights: `rendering/limits/cluster_builder/max_clustered_elements` default **512** (range 32–8192) for
  omni + spot + decal + reflection probe in view; beyond that, elements are dropped (`ProjectSettings.xml:3245`,
  `rendering_server.cpp:3830`, `cluster_builder_rd.h:237`).

### 6.2 Microbenchmark (Godot dev7, Vulkan Forward+, RX 9070 XT, 1920×1080, vsync off) [verified, `S\bench`]
Each phase: 3 s averaged after 1 s warm-up. `frame_ms` is wall time per frame; `rs_cpu` is RenderingServer CPU
time per frame; `gpu` is viewport GPU time; `script` is GDScript time in `_process`.

| scenario | frame ms | RS CPU ms | GPU ms | script ms |
|---|---|---|---|---|
| baseline (plane + sun) | 0.457 | 0.047 | 0.117 | 0.001 |
| 100 one-shot GPUParticles3D × 16, restarted on finish (impact-like) | 0.914 | 0.132 | 0.167 | 0.022 |
| 300 × 16 | 1.996 | 0.327 | 0.249 | 0.066 |
| **1,000 × 16** | **9.870** | 2.024 | 0.503 | 0.499 |
| 300 × 16 lit + soft + view-depth sorted | 2.701 | 0.778 | 0.620 | 0.070 |
| 1,000 × 16 lit + soft + sorted | 12.563 | 2.247 | 1.678 | 2.171 |
| one MultiMesh 4,800 quads, whole buffer uploaded per frame | 0.501 | 0.050 | 0.177 | 0.022 |
| **one MultiMesh 16,000 quads, uploaded per frame** | **0.567** | 0.052 | 0.246 | 0.067 |
| MultiMesh 16,000 lit + soft | 1.052 | 0.051 | 0.798 | 0.066 |
| + 64 OmniLight3D (range 3 m) | 1.730 | 0.083 | 1.463 | 0.066 |
| + 256 OmniLight3D | 4.079 | 0.173 | 3.797 | 0.069 |

C++ CPU simulation of a BF3-like chain (age, drag, gravity, local force, turbulence stand-in, 5 cubic evaluations,
alpha-level pow, colour lerp, rotation, 16-float output per particle), single thread, MSVC /O2 AVX2, 100k
particles: **24.7 ns/particle/frame** (`S\cpusim\sim.cpp`) [verified]. 16k particles ≈ 0.4 ms on one core, or
≈ 0.1 ms on 4 jobs.

Reading [inferred]: 16,000 particles in one CPU-fed MultiMesh cost ≈ 0.1 ms frame + 0.13 ms GPU over baseline.
The same count as 1,000 GPUParticles3D systems costs ≈ 9.4 ms frame and 2 ms RS CPU, from per-system
overhead. Part of that is node/GDScript cost (`script` 0.5–2.2 ms), but RS CPU alone is 2 ms. Lit transparent
quads × many omni lights is the real GPU risk (+0.67 ms for 64 lights, +3.0 ms for 256), so effect lights need a
pool.

## 7. Triggering, networking, late join (task 7)
Census of references into `fx/` by source (`S\refs-to-fx.tsv`, from non-fx partitions) [verified]:

| trigger | references | path in our design |
|---|---|---|
| weapon fire `FiringFunctionData.FireEffects1p/3p[]` + `WeaponFiringEffectsModifier` (attachments, e.g. suppressor) | 443/442 + 304/304 | ShotEvent (D7 seeded shot) → 1p on the predicted shot, 3p on the replicated `{weapon, shotSeq, t, origin, dir}` |
| projectile trails `BulletEntityData.TrailEffect` 113, `MissileEntityData.TrailEffect/EngineEffect` 28/24, `GrenadeEntityData.TrailEffect` 1 | | attached to the client's visual projectile (tracer converges over `visualConvergeDistance`, 01 §246) |
| impacts `MaterialRelationEffectData.ImpactEffects[]` | 7,639 | client resolves its own bullet re-fly (D7) → grid by material index → effect + decal + sound; server-confirmed player hits use the hit event |
| explosions `(Venice)ExplosionEntityData.DetonationEffect` | 48 + 27 | replicated detonation event (position, normal, surface material) → detonation + grid effect |
| vehicles: `EffectComponentData.Effect` (702 in vehicles), `MaterialRelationVehicleData` wheel/ground/track/chassis, `VehicleEntityData.CriticallyDamagedEffect/PreExplosionEffect` 18/14, overheat 29 | | derived from replicated vehicle state (health zone, speed, contact material) |
| destruction: `ReferenceObjectData.Blueprint → EffectBlueprint` in architecture/xp*/props/objects destruction prefabs (~6,000), `DebrisClusterData.Effect` 75, vegetation `StemRip/TreeFall/...` 69 | | from destruction slot transitions (`seed = hash(slot,state,server_tick)`, §3.9) |
| level logic / ambient: `EffectReferenceObjectData` in levels 5,714 (+ 48 architecture, 29 xp2, 17 gameplay) | | `AutoStart` placements + `Start/Stop` events from the **client realm** of the S4 interpreter (Metro park event fires ~20 `EffectReferenceObjectData.Start`, gap logic §5) |

* **Client-only and cosmetic**: no EffectBlueprint needs a network id, and all logic entities inside `fx/` prefabs are
  `Realm_Client` except 4 `Delay` and 5 `Locator` (`ClientAndServer`) [verified]. Effects never feed gameplay. So
  the **server instantiates no emitters**; a dedicated server skips the whole subsystem [inferred, consistent with
  D15's dummy server audio].
* **Determinism**: not needed bit-exact. Use `seed = hash(event id)` (shotSeq, detonation id, slot transition) so
  all clients see the same random choices (spawn probability, start frame) [inferred design].
* **Late join**: one-shot effects are not replayed. Looping and persistent effects are **re-derived from state**:
  level `AutoStart` ambients, destruction-state loops (burning wrecks, aftermath smoke), vehicle damage-state
  effects, logic-started loops from the logic ghost (SyncedBool → client interpreter). Dynamic decals are a local
  ring buffer and are not synced [inferred].
* **Do effects need the S4 interpreter?** No for the effect itself: an EffectBlueprint is a flat list of
  {emitter/sound/visual-env component, transform, SpawnProbability, StartDelay, quality flags, MaxInstanceCount,
  CullDistance}, with no internal event wiring (`InterfaceHasConnections False` on all 1,101) [verified]. Use a
  **separate client-only instancer**. The interpreter's client realm calls `start/stop` for level-placed
  `EffectReferenceObjectData` [inferred].

## 8. Recommendation

### 8.1 Decision row (format of PORTING-RESEARCH §4)

| # | decision | options considered | recommendation | evidence | conf. | how to validate early |
|---|---|---|---|---|---|---|
| D19 (new r2) | VFX | (a) GPUParticles3D per emitter with generated `ParticleProcessMaterial`; (b) GPUParticles3D with generated particle shaders, one pooled system per emitter definition fed by `particles_emit`; **(c) own C++ CPU emitter simulation (`bf3_fx`, Godot-free) feeding RenderingServer MultiMesh batches with custom spatial shaders**; effects through the S4 interpreter vs **a separate client-only instancer** | (c) + instancer: compile each EmitterDocument/EffectBlueprint from EBX into a flat EffectDef (32 processors, 10 evaluators, `(c3,c2,c1,c0)` polynomials, rate/s + MaxCount cap, child/base emitters, tiers), cached by digest; simulate on the job pool; draw per (shader class × blend) MultiMesh with CPU back-to-front sort, emitter atlas (BC3 base + RGA normal) read from the install; ActAsPointLight particles merged per emitter into a pooled ≤ 64-light set (RS omni, no shadows); impact decals from the grid as a pooled `Decal` set (later own mesh ring buffer); impacts selected by `PhysicsMaterialIndex` in the D7 grid; effects client-only, triggered by ShotEvent/projectile/impact/detonation/vehicle state/destruction transitions/client-realm logic, re-derived on late join; (b) kept as the fallback for > 1,000-particle ambient emitters | this report §1–§7: 3,410 emitters need all 32 processors (31 → 99.5 %); `ParticleProcessMaterial` cannot express rate ≠ amount/lifetime or the evaluator trees; benchmark: 1,000 GPUParticles3D systems 9.9 ms/frame (RS CPU 2.0 ms) vs 16k quads in one MultiMesh 0.57 ms; C++ sim 24.7 ns/particle; 64 vs 256 omni lights +0.67 / +3.0 ms GPU; effects carry no network id | M | VFX1 doctest goldens; VFX3 64-bot Metro firefight: FX ≤ 1 ms main thread, ≤ 2 ms GPU at 1440p, ≤ 64 effect lights |

### 8.2 Slices to insert (sizes in PORTING-RESEARCH slices) [inferred]

| slice | milestone | content | oracle (exit) |
|---|---|---|---|
| **VFX1** compiler + simulator | M2a, 1.5 | `bf3_fx` (Godot-free): EffectDef/EmitterDef compiler with digest cache; all 32 processors and 10 evaluators; spawn semantics §2.1; base/child emitters; local space; tiers; doctest | compiles **3,410/3,410 emitters and 1,101/1,101 effects, 0 unknown types** on @all-mp; golden 1p flash: 1 particle, 0.05 s, start frame ∈ [1,16], size 0.4·U[0.8,1]·P(t); `em_amb_mp12_oilfire_l_fire_01` steady state 80 ± 1 (cap 81); `PolynomialData` f(0)=w, f(1)=Σ |
| **VFX2** renderer | M2a, 1 | emitter atlas from the install (base sRGB BC3 resident; normal R/A decode); MultiMesh batches per shader class; spatial shaders (atlas rect + flipbook, HDR colour, alpha level min/max/scale, soft fade, wrap/vertex-pixel lighting, DAQ/SAQ/mesh); 1p effects in the viewmodel pass (`fov_override`); CPU sort | all 52 Metro `TextureInfos` resolve; M16A3 1p muzzle visible on each shot; 16k particles ≤ 0.3 ms GPU |
| **VFX3** weapon triggers, impacts, lights, decals | M2a, 1 | FireEffects1p/3p at the muzzle socket (predicted / ShotEvent); tracer on the visual bullet; impacts via grid (material index) + decal + A5 impact sound; light pool ≤ 64 with per-emitter merge; instance pools (MaxInstanceCount, ResetInstanceWhenStarted) | 5.56 on Metro: concrete → `fx_impact_concrete_01_s(_xp2)` + `decal_concrete_s_01`, brick → `decal_brick_s_01`, soldier → `fx_impact_soldier_body_s`; 64-bot firefight within the D19 budget |
| **VFX4** network rules | M3, 0.5 | seeds from event ids; late-join derivation; dedicated server excludes `bf3_fx` | two PCs see the same muzzle/tracer/impact choices for one burst; late joiner sees burning wrecks/ambient loops, no stale one-shots |
| **VFX5** explosions + vehicles | M4, 1 | detonation + grid effect by explosion material + scorch decal + VisualEnvironmentEffect (via D16/post) + audio-graph camera shake; vehicle wheel/track/ground/chassis, damage states, missile engine/trail | M67 on Caspian gravel → `fx_grenade_frag_01` + `fx_impact_gravel_m` + `decal_scorchmark_m_01`; RPG on concrete → `+ fx_impact_concrete_01_m` + `decal_concrete_l_01` |
| **VFX6** destruction FX | M6, 1 | destruction-prefab `ReferenceObjectData→EffectBlueprint` started on slot transitions; DebrisCluster effects; aftermath loops; vegetation effects | an RPG hole plays the same blueprint on two clients and a late joiner sees the aftermath loop |
| **VFX7** ambient + breadth | M7, 1 | 5,714 level `EffectReferenceObjectData` (AutoStart + client-realm logic), camera-proximity emitters (rain/snow), VisualEnvironment effects, quality tiers, (b) fallback for the largest emitters | all 29 levels instantiate their ambient effects with 0 unknown types; Kharg/Alborz weather within budget |

Totals [inferred]: **≈ 7 slices** (MVP M2a 3.5 + M3 0.5 + M4 1 = **5**; v1 +2), **≈ 9–12 k LOC** (compiler
1.5 k, simulator 3–4 k, renderer + shaders 2–2.5 k, instancer/manager 1.2 k, lights + decals 1.3 k, trigger glue
≈ 1 k). This is new work beyond 08's "Audio/FX 6–8 w", which audio already uses up.

## 9. Second-source re-check of the 5 most load-bearing claims
1. **Census sizes** (1,101 effects, 3,410 emitters, 4,353 emitter entities). My `fxall` dump matches the
   independent `types` run in `critique-r2\fx-types.tsv`. ✔
2. **Polynomial order (c3,c2,c1,c0) in (x,y,z,w).** Slot-pattern statistic (never x/y alone), endpoint fades on the
   muzzle flash, the muzzle light and frag curves, and the handoff's prior inference agree. No public formula exists
   (VU docs list only fields: https://docs.veniceunleashed.net/vext/ref/fb/polynomialdata). ✔ (semantics stay
   [inferred])
3. **Impacts index the grid by `PhysicsMaterialIndex`, damage by `PhysicsPropertyIndex`.** Reproduced on Caspian
   and Metro grids; IW4L `material.rs:131` uses `PhysicsMaterialIndex` for surface materials. ✔
4. **Godot per-system overhead.** Benchmark (1,000 systems 9.9 ms) and source: a per-instance loop with its own
   dispatches in `particles_storage.cpp:1442-1624`. ✔
5. **Forward+ light limit 512 clustered elements (configurable 32–8192).** `ProjectSettings.xml:3245` and
   `rendering_server.cpp:3830`, plus the drop rule in `cluster_builder_rd.h:237`. ✔ BF3's many-light tile-deferred
   renderer stays a [report claim] (GDC 2011 slides, link above).

## 10. What I could NOT determine
* `BaseEmitterData` runtime semantics (base chain vs. the orphan processor copy vs. merge) and the exact per-parent
  timing of `EmitterData` child emitters.
* `MaxInstanceCount` scope (global per blueprint vs per owner) and what `ResetInstanceWhenStarted` steals; this
  changes the light and particle caps in §4.
* Exact clamp/scale order in PolynomialData, `DefaultValues` × evaluator combination rules, `AlphaLevelMin/Max/Scale`
  math (alpha erosion with `Exponent`), `UpdateClipScale.Lookup` (int16 tables, probably per-frame quad clip
  polygons for overdraw; rendering full quads should be safe), `ZOcclusionLookup`, `DistributeOverDistance` /
  `TrailSegmentLength` and `Turbulance` noise. Oracles should be frame-matched against BF3 footage or the S5 VU
  harness (effects are observable there).
* Whether `PointLightRadius` scales with particle size (1p light_01 radius 0.2 with spawn `Size` 0), and BF3's
  real light intensity units.
* NormalDXT5RGA: the y sign/handedness and the purpose of G/B. The decode in §5 is a statistical inference, not
  a matched render.
* Emitter render passes BF3 uses (half-res vs full-res compositing, `ForceNiceSorting`) and BF3's actual particle
  and light counts at 64 players. Not measured; S5 could add a count.
* `Trail` and `Point` emittable types are unused in MP (0); the 18 `Quad` and 4 `WorldAlignedQuad` orientation
  conventions were not checked visually.
* The Godot benchmark used GDScript-driven nodes and a stock `ParticleProcessMaterial`. A C++-pooled option (b)
  with `particles_emit` was not measured.
