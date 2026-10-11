# 03 — Animation: BF3's Ant system and the routes to animated soldiers and weapons in Godot 4.8 (C++)

Written 2026-10-10 by the animation research agent (knowledge side; a separate probe agent is decoding real AssetBanks with
Rime at the same time). The question is how BF3's soldiers, 1p weapons, vehicle seat poses and collapses get animated in a C++
Godot 4.8 reimplementation that **reads the original BF3 files as they are** from the retail install and never modifies them.

Tags: **[verified]** = checked here in data, code, the 4.8 API dump or a primary source actually read; **[report claim]** = stated
by a source not re-checked; **[inferred]** = my reading. Paths are absolute; `RIME` = `C:\Users\olku\Rime`, `VU` =
`C:\Users\olku\VU-Docs\types\fb`, `CORPUS` = `C:\Users\olku\iw4L\context\bf3dump\ebx_corpus\mp_001` (raw EBX partitions the old
project extracted from the user's install), `API` = `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json`,
`SCRATCH` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\animation`.
EBX values below were read with a 150-line Python EBX reader written for this report (`SCRATCH\scripts\ebxdump.py`, format from
handoff `sections/01` §12); skeleton dumps are in `SCRATCH\skeletons.txt`.

---

## 0. Bottom line

1. **BF3 animation = EA "ANT" data in EA GenericData (GD) banks.** EBX `AntPackageAsset`s point either at an `AssetBank` res
   (`0x51A3C853`, packaging Static/Bundle) or at a chunk (`StreamingGuid`, packaging Chunk); collapses use `AntAnimationSetAsset`
   banks. The banks are **self-describing**: a `GD.REFL` section carries every type's layout (field names, offsets, types), then
   `GD.DATA` objects. Clips are `DctAnimationAsset` (8-frame DCT blocks, bit-packed int16 coefficients) plus Frame/Raw/Curve codecs.
   Everything else in a bank is the animation *graph* (state flows, blend spaces, choosers, layers, IK features) that gameplay
   drives through integer `AntRef.AssetId` signals. [verified in code + EBX; codec census unknown]
2. **The skeletons are plain EBX and fully readable now** [verified]: 1p `Venice1pSke01` 179 bones, 3p `VeniceAntSke01` 213 bones,
   `WeaponSke01` 25 bones, `ParachuteSke01` 42 bones, each with `BoneNames`, `Hierarchy`, `LocalPose`, `ModelPose`. Master skeletons
   splice the weapon skeleton into the body (1p bones 153–176, 3p 155–178). The 1p and 3p rigs carry explicit `IK_Joint_LeftHand`/
   `IK_Joint_RightHand` bones under `Wep_Root`: ideal hand-IK targets for Godot's `TwoBoneIK3D` or our own solver.
3. **Rime has readers, not an exporter.** It parses the bank framing, GD reflection and objects into 258 generated `ant` classes and
   has a DCT bit-unpacker that prints CSV. It does not name channels, apply clips to a skeleton, or export. Its one Ant utility
   calls methods that throw `NotImplementedException` as committed. **IceBloc** (marv7000, GPL-2.0, C#, BF3 + MoH Warfighter) is
   the only known end-to-end FB2 path: GD reader → DCT/Frame/Raw decode → channel names from `s_basicassets` → SMD/SEAnim. Its DCT
   code is the same algorithm as Rime's (shared lineage), so there is **one** independent decoding of the format, never checked
   against the game.
4. **Godot's own AnimationMixer is too slow for 64 BF3-sized soldiers** [verified, measured here]: an AnimationTree (5-clip
   BlendSpace2D + filtered additive layer) on 64 × 213-bone skeletons costs ~34 ms/frame on one Ryzen 7 5800X core; a plain C++
   pose pipeline doing the same work costs 0.46 ms. Pushing poses into `Skeleton3D` adds ~2 ms. **Build our own C++ animation
   core** (Godot-independent, also runs on the headless server for hit capsules) and use `Skeleton3D`/skins only as the render sink.
5. **The 1p viewmodel in the same HDR frame is a solved problem in 4.8** [verified]: `BaseMaterial3D.use_fov_override/fov_override`
   and `use_z_clip_scale/z_clip_scale` (PR #93142, 4.5), and for our own `ShaderMaterial`s the spatial-shader built-ins
   `PROJECTION_MATRIX` (inout), `Z_CLIP_SCALE` and `IN_SHADOW_PASS`. BF3 gives the numbers: `SoldierWeaponData.renderFov` 55,
   `zoomRenderFov` 15–28.
6. **Decision procedure (§6):** give Route A (decode Ant) one weekly usage window (≤ 5 working days) with hard visual and numeric
   exit criteria on one 1p reload clip; pass → clean-room C++ decoder in the `bf3` library; fail → Route B (procedural 1p from BF3's
   own weapon data + CC0 3p locomotion retargeted at runtime onto the BF3 skeleton), keeping Ant decoding as a background track.

---

## 1. What BF3's animation data is

### 1.1 Where it lives (MP_001 listing and EBX) [verified unless marked]

Source: `C:\Users\olku\iw4L\context\bf3dump\03_mp001_contents.out.txt` (Rime listing of the MP_001 superbundle) and the EBX corpus.

| asset | kind | count / size | what |
|---|---|---|---|
| `animations/antanimations` | EBX `AntProjectAsset` (`AntNativeProjectName` "Animations", `ProjectId` 0, `SceneOp.AssetId` 357073468) | 1 project, `PackageAssets` = 201 entries | the whole game's Ant project |
| `animations/antanimations/<weapon>` | EBX `AntPackageAsset`, `PackagingType` **Chunk** | **85** in MP_001; chunk sizes 18,136 B (ammobag) … 243,812 B (g3); M416 178,180 B; **8,005,736 B total** | 1p (and per-weapon) clip data, streamed per weapon |
| `animations/antanimations/s_basicassets` | `AntPackageAsset` **Static** + `AssetBank` res | 1 | the static graph + shared clips; IceBloc reads every clip's channel metadata from here (§1.5) |
| `b_basicassetsmp`, `b_vehicle_humvee/kornet/tow2/vodnik` | `AntPackageAsset` **Bundle** + `AssetBank` res | 5 | MP basics and per-vehicle seat animations |
| `animations/characters/mp/buildings/<bld>/<bld>_animset` | EBX `AntAnimationSetAsset` + `AssetBank` res (packaging AnimationSet) | 2 in MP_001 (mehouse01large, mehouse01mediumruin) | building collapses |
| `animations/characters/sp/shared/objects/cloths/...` | animsets + `AssetBank` | flags, curtains | cloth/flag motion |
| `animations/skeletons/*` | EBX `SkeletonAsset`, `MasterSkeletonAsset` | 4 skeletons + 3 masters + `weapon/weaponske01` | §1.2 |
| `animations/settings/ragdolls/ragdoll01` | EBX `RagdollAsset` + `RagdollResource` res (`0x319D8CD0`) | 1 | soldier ragdoll |
| `AnimTrackData` res (`0xD070EED1`) | Hermite keys (Rime template) | 27–45 per level | timeline/sequence curves (cameras, scripted objects) [report claim, handoff 02 §10] |

`AntPackageAsset` fields [verified `VU\AntPackageAsset.yaml`, values from `CORPUS\animations\antanimations\m416.ebx`]: `Name`
"Animations/AntAnimations/M416", `Win32FileName` "Animations/Package_Win32_Animations_M416", `XePs3FileName`, `PackagingType`
(`AntPackagingType`: 0 Static, 1 Chunk, 2 Bundle, 3 AnimationSet), `StreamingGuid` (195869e7-… for M416; zero for Static/Bundle,
whose data is the res named `lowercase(Name)`), `ChunkSize`. The copies of the 201 packages embedded in the project partition carry
zero `StreamingGuid`s; the per-package partitions carry the real ones [verified].

### 1.2 The EBX side: skeletons, bindings, weapon and soldier fields

**`SkeletonAsset`** [verified `VU\SkeletonAsset.yaml`; values `CORPUS\animations\skeletons\*.ebx`]: `boneNames string[]`,
`hierarchy int[]` (parent index, −1 root), `localPose LinearTransform[]`, `modelPose LinearTransform[]`, `weaponBoneName`,
`headBoneName`, `hipBoneName`, `cameraBoneName` (inherits `BaseSkeletonAsset : Asset`, no fields).

| skeleton | bones | special names | notable structure |
|---|---|---|---|
| `Animations/Skeletons/Venice1pSke01` (c69d9468/34256e97) | 179 | Wep_Root, Head, Hips, CameraJoint | 0 Reference → 1 AITrajectory → {Trajectory, CameraBase→CameraJoint (1.70 m), Hips (0.961 m), Connect}; full body incl. legs, 5-joint fingers, 30 face bones, Roll/Cloth/Phys helpers; **`Wep_Root` (153) is a child of `SpineX` (42)**; weapon bones 153–176 incl. `Wep_Trigger`, `Wep_Slide`, `Wep_Mag`, `Wep_Mag_Ammo`, `Wep_Grenade1/2`, `Wep_Belt1–5`, `Wep_Bipod1–3`, `Wep_Physic1–3`, `Wep_Extra1–3`, `Wep_Aim`, **`IK_Joint_LeftHand` (172) / `IK_Joint_RightHand` (173)**; `LeftWeapon`/`RightWeapon` under the hands |
| `Animations/Skeletons/VeniceAntSke01` (39b2c3ca/39b2c3cb) | 213 | same | 3p: ~60 face bones with `Offset_*` parents, `Throat`, cloth/phys helpers; `Wep_Root` (155) child of bone 7 (Spine2); IK joints 174/175; CameraBase/CameraJoint 209/210 |
| `Animations/Skeletons/Weapon/WeaponSke01` (2e912b2b/c6bdaf0f) | 25 | — | `exportAnimation` root → `Wep_Root` → the 23 weapon bones (same names as above) |
| `Animations/Skeletons/ParachuteSke01` | 42 | — | canopy rig (Spine…, Left/Right 1–3 Front/Back/Middle) |

Local transforms follow the DCC convention of bone-local X along the bone (e.g. `Spine` local (0.118, 0, 0), model (0, 1.078,
0.016)), so `LocalPose` rotations are non-identity and must be used as rest [verified].

**`MasterSkeletonAsset {masterSkeleton, subSkeletons[] SubSkeleton {skeleton, boneMap int[], transformMap LinearTransform[]}}`**
[verified schema + data]: `Venice1pMasterSkeleton` = Venice1pSke01 + WeaponSke01 with `BoneMap` [0, 153 … 176];
`Venice3pMasterSkeleton` = VeniceAntSke01 + WeaponSke01 with [0, 155 … 178]; `ParachuteMasterSkeleton` = ParachuteSke01 alone.
`TransformMap[i]` is identity rotation with translation = −(WeaponSke01 `ModelPose[i]`) (e.g. `Wep_Extra1` model (0, 0.146, 0.191)
→ T (0, −0.146, −0.191)) [verified], i.e. **the inverse bind of the weapon skeleton** [inferred]. Consequence: a weapon mesh is
skinned to the 25-bone weapon skeleton, mapped onto the body skeleton by `BoneMap`, bound with `TransformMap`; per-weapon bone
placement (where the magazine sits on an M416 vs an AK) comes from the clips, not from a per-weapon rest [inferred].

**Skinned meshes** [verified handoff `sections/02` §2]: `SkinnedMeshAsset : MeshAsset {boundingBoxPositionOffset,
boundingBoxSizeOffset}`; the MeshSet skinned LOD has `boneCount`, `u32[boneCount]` skeleton bone indices (`ptrA`) and `u32[boneCount]`
bone-name hashes (`ptrB`); each subset has `u16[boneCount]` bone slots; vertices carry UByte4 indices + UByte4N weights (4 per
vertex). The hash function for bone names is unverified (handoff §10).

**The soldier's animatables** [verified, `C:\Users\olku\iw4L\context\bf3dump\ebx\characters\soldiers\mpsoldier.json`, the
`characters/soldiers/mpsoldier` blueprint]. `SoldierBodyComponentData` holds four `AntAnimatableComponentData {realm, subRealm,
animationData AntAnimationHandlerData, mesh, autoActivate, animationControlledFromStart, forceDisableCulling}`:

| guid | realm / subRealm | master skeleton (`AntAnimatableData.masterSkeletonAsset`) | `antPackageData` | role [inferred] |
|---|---|---|---|---|
| 83cb9e70 | Client / LocalPlayer | 37b99e43/7b0e0d54 = **Venice1pMasterSkeleton** | → `animations/antanimations` (1bbbfb59) | 1p arms + weapon |
| 9159731e | Client / All | 40c4d593/6106f1be = **Venice3pMasterSkeleton** | — | 3p body seen by others |
| 0d3b9a0e | **Server** / All | Venice3pMasterSkeleton | → `animations/antanimations` | **server-side 3p pose (hit capsules)** |
| 122c3846 | ClientAndServer | ParachuteMasterSkeleton; `mesh` = `characters/parachute/parachute_mesh` | — | parachute canopy |

`AntAnimationHandlerData {animatable AntAnimatableData {actor, proceduralAwareness, right/leftHandEffectorDisableOverride,
masterSkeletonAsset}, rootController AntRef, lodBinding LodBinding {disableControllerUpdate, disablePoseUpdate, distanceFromCamera},
antPackageData AntPackageAsset[], bonesToMirror GameplayBone[], reportBackFromAnt, enableMasterSlaveCopy, isProp}` [verified]. The
1p handler mirrors `Wep_Aim`→AimBone, `AITrajectory`→RootBone, `Head`, `Connect`, `Hips`→RootMeshBone, `Wep_Root`→WeaponBone,
`Wep_Mag`→MagazineBone (`GameplayBones` enum) back to gameplay [verified].

**`AntRef {assetId int}` is the whole gameplay ↔ animation interface** [verified schema; 54 other types have `AntRef` fields]. Gameplay
writes values into Ant signal assets by id; −1 = unbound. The soldier's ids are 357,040,602 … 357,103,016 [verified]: a span of
62,414, just under the 62,766 objects Rime's history reports for `s_basicassets` (§1.5) — so ids are probably indices into the static
bank [inferred]. Bindings that matter (all `AntRef` fields, [verified schema], ids [verified data]):

| binding (owner) | signals |
|---|---|
| `AnimatedWeaponBinding` (`SoldierWeaponsComponentData`) | deploy, altDeploy, undeploy, quickSwitch, reload, reloadShotgun, fire, fireSingle, fireHoldAndRelease, fireSimple, firstShotSpawned, boltAction, pumpAction, meleeAttack, quickThrow, aimBody, oneHanded, oneHandedAiming, lowerGun, rfl/pstl/hgr/at/shg/lmg/bag/snpType, zoom, zoomParameter, zoomScaleFactor, dispersion, aimTargetPosBody, zoomIn/OutSpeed, deploy/unDeploySpeed, fireModeChanged, animType, numberOfBulletsLeftInGun, bulletsLeftInGun, abortVehicleDeploy, currentProjectileIndex, weaponActionESIG, kickBackInIronSight, weaponChooserSignal, weaponClassSignal, offsetX/Y/Z (+ unbound: isSprinting, gunDown, breathControl …) |
| `Animated1pOnlyWeaponBinding` / `Animated3pOnlyWeaponBinding` | undeployFinished, cameraFreeWeight, deploy1P, hideWeapon1p / deploy3P, hideWeapon3p |
| `SoldierHealthModuleBinding` (`VeniceSoldierHealthModuleData`) | headShot, deathAnimationTriggered, randomAnimationIndex, sprinting, hitLeft/Right/Front/Back, explosion, dead, revived, randomValue, rightSpeed, forwardSpeed |
| `RagdollBinding` (`RagdollComponentData`) | ragdollOnBack, ragdollBlend, ragdollForceBlendDisabled, ragdollFullyBlendedIn |
| `HitReactionComponentBinding` | hit, direction, firingDistance, boneType, impactType, immortal, randomAnimationIndex |
| `VehicleEntryListenerBinding` | inVehicle, inOpenEntry, vehicleEntryFire/Yaw/Pitch/Roll/Rotation, **vehicleEntryIndex (357078069)**, vehicleEntryIndexUpdated, vehicleOpenEntryIndex, inVehicleRetrigger, vehicleEntryChooserIndex |
| `SoldierParachuteBinding` | enabled, steerLeftRight (accelerateBrake/pitch/roll unbound) |
| `ClimbLadderBinding`, `SpecialMovesBinding`, `SupportedShootingBinding`, `WeaponLagBinding`, `MovementComponentBinding`, `SoldierEmoteBinding` (13 emotes), `CharacterMeleeBinding`/`CharacterMeleeCommonBinding` (knife takedown), `WarpAnimationBinding` | ladders, vaults/slide, bipod rest, procedural weapon lag (entityPitch/Yaw → pitch/yaw/rollResult), animation-controlled movement, emotes, takedowns, alignment warps |

**Weapons** [verified `VU\SoldierWeaponData.yaml`, `WeaponStateData.yaml`; values from `CORPUS\weapons\*\*.ebx`]:
`SoldierWeaponData {animBaseSet WeaponAnimBaseSetEnum, animationData AntPackageAsset, renderFov, zoomRenderFov, firstPersonCamera
FirstPersonCameraData, weaponStates[] WeaponStateData {mesh1p, meshZoom1p, mesh3p SkinnedMeshAsset, weapon AntRef,
animationConfiguration {shootModuleData, weaponOffsetModuleData WeaponOffsetData, weaponSpeedModuleData WeaponSpeedData},
animatedFireType, animatedAimingType, isOneHanded, skipFire/Deploy/ReloadAnimation, playDeployAfterFire, projectileBoneName,
boneFakePhysics[] …}, …}`. `WeaponAnimBaseSetEnum`: wabsRif 0, wabsPstl 1, wabsHgr 2, wabsAT 3, wabsShg 4, wabsLMG 5, wabsBag 6,
wabsSnp 7. `WeaponAnimType` (attachment variants, via `WeaponAnimTypeModifier`): NoAddon, Bipod, Foregrip, 40mm_GL, 40mm_GL_Fire,
Underslung_Shotgun, Underslung_Shotgun_Fire, Straight_Pull_Bolt, Extended_Mag.

| weapon | animBaseSet | animationData | renderFov / zoomRenderFov | fire type |
|---|---|---|---|---|
| M416 | wabsRif | M416 package (8117844e) | 55 / 20 | Automatic, TwoHanded; 17 `WeaponOffsetData` (per sight/attachment ADS offsets, e.g. zoomed (0, −0.045, −0.178)) |
| M9 | wabsPstl | M9 package | 55 / 20 | Single |
| M40A5 | wabsSnp | M40A5 package | 55 / 15 | Single |
| M249 | wabsLMG | package 6e563b43 | 55 / 28 | Automatic |
| Knife | wabsHgr | **null** (lives in the basic assets) | 55 / 10 | Single, `isOneHanded` |

So ADS alignment per optic is **data** (`WeaponOffsetData.weaponZoomedOffsetX/Y/Z`, sent to Ant through `offsetX/Y/Z`), not a clip
per sight [verified data, inferred use]. Timings that gameplay owns are also data: `FireLogicData.reloadTime`,
`reloadTimeBulletsLeft`, `rateOfFire`, `WeaponFiringData.deployTime`, `WeaponSpeedData.zoomIn/OutSpeed` [verified schema].

**`SoldierSpawnTemplateData {customizeSoldierData, sensing, behaviour, animationTemplate AnimationSpawnTemplate {runStyle
AntEnumeration}, voiceOverLabels}`** [verified schema]: the only animation field is a run-style enumeration (AI/scenario).

**Vehicle seats** [verified, `CORPUS\vehicles\*`]: `PlayerEntryComponentData {antEntryId AntEntryIdEnum, antEntryID string,
antEntryEnumeration AntEnumeration {antAsset AntRef, value int}, show1pSoldierInEntry, showSoldierInEntry, …}`. `AntEntryIdEnum`
has only 20 values (Humvee 0–3/11, F16 4, SU37 5, AH6 6, AH64 7–8, Vodnik 9–10/12–14, Vads 15, F18F 16–17, M1A2 18–19) and is stale:
BMP-2, LAV-25, M1A2 all say `Humvee_Driver` with `ShowSoldierInEntry false` (crew hidden, **no seat pose needed**). Open seats
select their pose through `AntEnumeration.value` written to `vehicleEntryIndex` (asset 357078069): Humvee driver 0, front passenger
11, rear 3; Vodnik 9/10/12/13; Kornet 47; TOW-2 46. `Show1pSoldierInEntry` is true for open-vehicle drivers (1p arms on the wheel,
driven by `AnimatedDriverEntityData {vehicleBoost, vehicleSpeed, vehicleMovementState, vehicleClutch, vehicleBrake, yaw/roll/pitch}`).
Seat clips are in `b_vehicle_<name>` banks.

**Collapses and props** [verified, `CORPUS\animations\characters\mp\buildings\mehouse01large\*`]: `ModelAnimationEntityData
{animationSet AntAnimationSetAsset, defaultAnimation "MEHouse01Large_anim", animationIndex 0, looping, autoStart false,
trajectoryBoneName "AITrajectory", connectBoneName "Connect", rootMeshBoneName "Base", modelAnimationTransformType World, …}`;
`AntAnimationSetAsset {skeletonAsset, actorAssetIndex 12, clipAssetIndices [18], loopingClipAssetIndices [19],
sceneOpMatrixAssetIndex 17, useTraj2Ref, allowAnimationCulling}` — indices into the set's own bank. The collapse skeleton has 13
bones (root, AITrajectory, `part1_joint` … `part10_joint`…): rigid parts. These small self-contained banks are the **easiest first
decode target**.

**Ragdoll and hit capsules** [verified]: `RagdollComponentData {skeletonAsset → VeniceAntSke01, ragdollAsset → ragdoll01,
skeletonCollisionData, binding, leftLegBoneName "LeftLeg", rightLegBoneName "RightLeg"}`; `RagdollAsset {materialPair null,
useServerRagdoll true}` — the joint/body definition is only in the `RagdollResource` res, never decoded. `SkeletonCollisionData`
`characters/soldiers/defaultsoldierbonecollision` has 13 `BoneCollisionData` capsules: hi-LOD RightArm/LeftArm r 0.08 len 0.28,
Spine r 0.23 len 0.10 and r 0.25, Head r 0.15, Right/LeftUpLeg r 0.11 len 0.40, Right/LeftLeg r 0.10 len 0.43 (hit-reaction types
HRT_*); lo-LOD Head + three capsules on AITrajectory. With the Realm_Server animatable this says **BF3's server poses the 3p skeleton
to place 9 hit capsules** [inferred from the two facts].

**Engine-wide animation LOD** [verified schema `VU\GameAnimationSettings.yaml`]: `temporalLodding{First…Sixth}Distance` and
`…DeltaTime` (six distance tiers with their own update intervals), `serverEnable`, `clientEnable`, `useAnimationDrivenCharacter`,
`antOnClientOnlyGamemodes`. `AntSettings` has `useHIK` (Autodesk HumanIK; `HIKData {reachT, reachR, pull, resist}` on
`IKComponentData`), `interpolatePoses`, `forceLodDistance`, `maxAnimatablesPerPoseJob`. Values were not read (engine settings).

### 1.3 The binary side: AssetBank and EA GenericData [verified in Rime code; layout itself never byte-checked here]

```
Frostbite wrapper (big-endian):  u32 AntPackagingType
  if != AnimationSet: PackageMeta { u32 metaBytes, packageType, assetCount, importCount, assetBytes, extraBytes;
        if extraBytes: u32 TotalNonStaticAssetCount, MaxNonStaticAssetCount, MaxNonStaticImportCount,
        StaticBundleImportBucketSize, StaticBundleImportBucketEntries, StaticBundleFlatImportCount,
        StaticBundleImportCountInSlots, StaticGuidToIntSize; skip to extra end; GuidToInt map (bytes, unparsed);
        static import map: StaticBundleImportCountInSlots × {u32 BE key, u32 LE ptr, i32 tid} ("probably a linear-probed hash map") }
EA::GD archive:  Blob = char[7] id ("GD.STRM" "GD.REFL" "GD.DATA" "REFPACK" "DAT2" "REF2" "INFO") + 1 char ('b' = big-endian)
                 + u32 size (incl. 12-byte header); RelocatableBlob adds u32 relocationTableOffset (16-byte header)
  GD.STRM { u32 sizeOfLargestBlock; GD.REFL; GD.DATA × N }          (REFPACK compression and REF2: not implemented)
  GD.REFL { u64 count; Ptr64[count] → LayoutHeader }
     LayoutHeader { i32 minSlot, i32 maxSlot, u32 dataSize, u32 alignment, u32 stringTableOffset, u32 stringTableSize,
                    bool recorded, bool native, pad2, u32 hash, EntryHeader[maxSlot-minSlot+1], string table (+1: type name) }
     EntryHeader  { u32 layoutHash (1..19 = primitive LayoutType, else struct layout), u32 elementSize, u32 offset, u32 nameOffset,
                    u16 count (>1 = fixed array), u16 flags (1 Array, 2/4/8 ForceAlign4/8/16), u16 elementAlign, i16 rle, u64 layoutOffset }
     slot −1 = base (Ptr64 to the base Data), −2 = __name (string container), −3 = __guid
  GD.DATA → LayoutData { u32 magic 0x030E6205 + pad, Ptr64 allocator, Ptr64 layout (layout hash in low 32 bits), i32 refCount,
                         u16 dataOffset, bool mutable } then the instance at dataOffset
  LayoutType: 1 Bool 2 Int8 3 UInt8 4 Int16 5 UInt16 6 Int32 7 UInt32 8 Int64 9 UInt64 10 Float 11 Vector2 12 Vector3 13 Vector4
              14 Quaternion 15 Matrix44 16 Guid (IdRef to another object by GUID) 17 String {u32 capacity, u32 count, u64 ptr}
              18 DataRef (Ptr64 to an embedded Data) 19 Double
  dynamic arrays: {u32 capacity, u32 count, u64 ptr}, element stride = align(dataSize, alignment)
```
Sources: `RIME\RimeLib.Animation.Frostbite2_0\Frostbite\AssetBank.cs:43-61,115-285`, `...\Frostbite\PackageMeta.cs:57-132`,
`RIME\RimeLib.Animation\EA\GenericData\{Blob.cs:86-113, RelocatableBlob.cs, Archive.cs:60-96, Reflection.cs, Data.cs:11,59-85}`,
`...\EA\Reflection\{LayoutHeader.cs, EntryHeader.cs, LayoutType.cs}`. IceBloc reads the same leading header differently
(`packageType, dataOffset, reflType, subDataCount, subDataCapacity, ptr, pad`, then seeks `dataOffset + 4` to `GD.STRM`;
`SCRATCH\ext\icebloc\IceBlocLib.Frostbite2\GenericData.cs:20-50`) — both land on `GD.STRM` [verified code].

**Design consequence:** the banks carry their own reflection, exactly like EBX partitions carry their descriptors. A C++ reader
must **read objects through the in-file layouts and look fields up by name**, never through compiled-in classes. Rime's 258
`ant\*.cs` classes (each `[AntBinding(layoutHash)]` + `[AntField(slot)]`) were generated by `RIME\Utils\AntLayoutDumper`
(a DLL injected into the running game that walks `Sdk::GD::Layout` registries, `dllmain.cpp:391`) — a field dictionary, nothing more.
The BF3 alpha needed alternative bindings for 8 drifted types (Rime commit d61622d4), which is exactly the fragility to avoid.

### 1.4 The object model inside a bank [verified field lists, `SCRATCH\ant_classes.txt` from `RIME\...\ant\*.cs`]

| group | classes (fields) |
|---|---|
| clips | `AnimationAsset {CodecType, AnimId, TrimOffset, EndFrame, Additive, ChannelToDofAsset}` ← `DctAnimationAsset` (0xb5a06c02) `{NumKeys, NumQuats, NumVec3, NumFloat, NumFloatVec, Cycle, QuantizeMultBlock, KeyTimes u16[], QuantizeMultSubblock u8, CatchAllBitCount u8, DofTableDescBytes u8[], DeltaBaseX/Y/Z/W i16[], BitsPerSubblock u16[], DataSize, Data u8[]}`; `FrameAnimationAsset {Data f32[], FloatCount, Vec3Count, QuatCount}`; `RawAnimationAsset {KeyTimes i16[], Data f32[], …, NumKeys, Cycle}`; `CurveAnimationAsset {FPS, NumRotations, NumVectors, NumFloats, Values, Keys, Consts, ConstOffsets, KeysBlocks, ChannelGroups, ChannelOffsets}`; `VbrAnimationAsset` (FB3-style VBR); codec options `DctCodecOptions {QuantizeMult_Block, QuantizeMult_Subblock_InTenths}`, `VbrCodecOptions` |
| channel mapping | `ClipControllerAsset` (0xfc431be4) `{Target LayoutHierarchyAsset, Anim AnimationAsset, NumTicks, FPS, FPSScale, DeltaTrajectory, Distance}`; `LayoutHierarchyAsset {LayoutAssets[], Children[]}`; `LayoutAsset {NoReorder, MinAlignment, Slots[] LayoutEntry {Name, Type}}`; `DeltaTrajLayoutAsset`; `ChannelToDofAsset {StorageType, IndexData u8[]}` |
| rig | Ant `SkeletonAsset` (0x931fe7fb) `{Joints[] JointInfo {JointName, ParentIndex, JointType}, TrajJointIndex}` — **no rest pose; the rest is in the EBX skeleton**; `RigAsset {FeatureCollection, Skeleton, DofSetLists, Default{Float,Int,Vector3,Vector4}Values, DeltaTrajDofSet}`; features: `PrimaryRigFeatureAsset`, `FirstPersonCameraFeatureAsset {Camera}`, `EffectorFeatureAsset`, `FootPlantFeatureAsset` (≈30 fields), `ParticleIKFeatureAsset`, `JointMappingFeature`, `IkRigToHIkRig` |
| graph | `StateFlowControllerAsset/NodeControllerAsset/TransitionAsset` (conditions on Bool assets, branch windows, Bézier blends), `LayersControllerAsset`/`LodLayersControllerAsset {LodDistancesNear/Far}`, `BlendSpace` (Voronoi), `SignalBlendSpaceControllerAsset`, `BlendArrayAsset`, `ChooserControllerAsset`, `LoopingControllerAsset`, `SequenceContainerAsset`/`ActorControllerAsset`/`SequenceAnimTrack`, `FpsLocoControllerAsset`, `LocoControllerAsset`, `StreetLocomotionControllerAsset`, `PhysicsMotion` (stand/crouch/prone velocities and gains, jump height — movement constants live in Ant), `WeaponLagSceneOpAsset` + `LagData` (PID pitch/yaw/roll), `WeaponAsset`, signals `BoolAsset/FloatAsset/IntegerAsset/…` |

**Channel naming chain** [report claim: IceBloc `Animations\Base\Animation.cs`, read here]: find the `ClipControllerAsset` whose
`Anim` is the clip (by GUID; "reverse lookup"), take `FPS` from it, walk `Target` → `LayoutHierarchyAsset.LayoutAssets` →
each `LayoutAsset.Slots` in order (names like `Hips.q`, `Hips.t`; `Type` 14 = rotation, 2049856663 = position, 2049856454 = scale;
a `DeltaTrajLayoutAsset` contributes 8 trajectory channels), then `ChannelToDofAsset.IndexData`: StorageType OVERWRITE = one byte per
DOF selecting a slot; APPEND = (target, slot) byte pairs. DOF order inside a DCT clip: quats first, then Vec3, then FloatVec. The
**clip controllers for all weapon chunks live in `s_basicassets`** (IceBloc loads it once as `BasicAssets` and resolves every
chunk's clips against it) [report claim]. Clip names seen in code: "ReloadClipEmpty Anim", "ReloadClipFull Anim" (M416/AK74 chunk;
`RIME\Utils\AntGenericDataInfo\Program.cs:456-457`) [verified literal].

### 1.5 DCT decompression as both decoders implement it [verified code: `RIME\RimeLib.Animation.Frostbite2_0\EA\Compression\DCT\Decompressor.cs:16-185`, `RIME\RimeLib.Animation\EA\Compression\DCT\DofTable.cs`, identical in IceBloc `DctDecompressor.cs`]

```
dofs = NumQuats + NumVec3 + NumFloatVec
per dof d: subBlocks = DofTableDescBytes[d] >> 4 (low nibble unused); deltaBase[d] = (DeltaBaseX[d], Y, Z, W)
           bits[d][s] = BitsPerSubblock[running index]; one u16 per sub-block = 4 nibbles (X<<12 | Y<<8 | Z<<4 | W); 0xF → CatchAllBitCount
bitstream = Data, MSB-first, big-endian, 64-bit reader; values are sign-extended ints (ReadIntHigh)
for block b in 0 .. ceil(NumKeys/8)-1:  for dof d:
    coeff[0..7] = vec4 int16
    if b == 0: coeff[0] = 0 and sub-block 0 is not in the stream (start reading at sub-block 1)
    read coeff[s] for each remaining sub-block s (X,Y,Z,W with their bit widths); missing sub-blocks (>= subBlocks) = 0
    coeff[0] += deltaBase[d]                                     <- constant base added to the DC term of every block
sample frame f (block f/8, row r = f % 8):
    value = Σ_{i=0..7} coeff[i] × C[r][i] × (QuantizeMultSubblock × 0.1 × i + 1) / QuantizeMultBlock
    C[r][i] = 0.5·cos((2r+1)·i·π/16) for i > 0 and 0.25 for i = 0 (an inverse DCT-II with halved DC; the table in the code
    matches this to 6 digits, e.g. C[0][1] = 0.490393, C[r][4] = ±0.353553)
quats: (x, y, z, w) normalised; Vec3: xyz of the vec4; KeyTimes[k] = frame index of key k; ClipController.FPS = frames/s
```
Unknowns that decide correctness (§7): whether `deltaBase` is really a constant per block or a running DC predictor; what
`KeyTimes` spacing means for sampling; float-channel use; quaternion sign continuity across blocks; additive clips' reference.
Note the attractive property: the DCT block is a **continuous** cosine series, so a runtime can sample any fractional frame inside a
block directly from the 8 coefficients (32 FMAs per vec4) — the compressed data itself is a good runtime format [inferred].

### 1.6 Exactly what Rime contains

| piece | file | status [verified by reading] |
|---|---|---|
| bank wrapper, PackageMeta | `RimeLib.Animation.Frostbite2_0\Frostbite\AssetBank.cs`, `PackageMeta.cs` | works through `AssetBank.Load(reader, loader)`; `AssetBank.Load(reader)` (line 524) and `ReflectionGeneralDataReader.Load(reader)` (line 454) **throw NotImplementedException** |
| GD archive/reflection/data | `RimeLib.Animation\EA\GenericData\*`, `EA\Reflection\*` | complete for FB2 `GD.*`; REFPACK/REF2 throw |
| object classes | `RimeLib.Animation.Frostbite2_0\ant\*.cs` (258) | generated dictionary, alpha-schema patches (d61622d4) |
| DCT | `...\EA\Compression\DCT\Decompressor.cs` + `RimeLib.Animation\EA\Compression\DCT\{DofTable,Header}.cs` | unpacks and evaluates; **prints `frame,dof,kind,vec4` CSV to the console**; no output object |
| AnimTrackData | `RimeLib.Animation.Frostbite2_0\{AnimTrackData,AnimKey}.cs`, `External\010templates\bree\fb2\AnimTrackData.bt` | 24-byte Hermite keys `{time, value, inTanX, inTanY, outTanX, outTanY}` over the whole res |
| `External\010templates\AssetBank.bt` | 38 lines | a stub: `PackageMeta` guess + `byte unknownidentical[12324]` — useless |
| `Utils\AntGenericDataInfo` | Program.cs (472 lines) | debug harness; active path calls the throwing `Load`; hard-coded output path `/home/txt/...` |
| `Utils\MeshExtractor` `DumpSkeleton` | Program.cs:242-321 | EBX `SkeletonAsset` → glTF nodes, then `throw new NotImplementedException()` (line 310) |
| front ends | `RimeLib.Cmd`, `Rime` (GUI) | do not reference the Ant libraries at all |

**Rime has no end-to-end exporter (clip → bone transforms).** It has readers. Its history records the most useful claim: keku645,
2026-07-03, d61622d4: "retail s_basicassets still parses fully (62766 objects); the alpha melee-takedown subgraph parses 249/249 blobs
end-to-end down to the DctAnimationAsset track data" [report claim; `git -C RIME show d61622d4`]. Rime has no licence file (local
clone and GitHub page): **reading it is fine, copying its code into the project is not licensed** [verified absence].

---

## 2. External decoders and exporters

| tool | engine / games | animation codecs | output | licence | state | notes |
|---|---|---|---|---|---|---|
| **IceBloc** (github.com/marv7000/IceBloc) | FB2: BF3, MoH Warfighter; FB2013 lib | Frame, Raw, **DCT**; Curve partial; Vbr stub | **SMD** and **SEAnim** animations (local bone pos/rot per frame, rest pose for unanimated bones), SMD skeletons from EBX `SkeletonAsset`, OBJ/SMD/SEModel meshes | **GPL-2.0** | commits "DctAnimation done" 2023-02-04, "SMD Anim export done" 2023-02-12, ".SEAnim export" 2023-02-20, "Animation fix" 2023-04-12; README 2023-07-03; 25 stars | the only known FB2 end-to-end path [verified code, cloned to `SCRATCH\ext\icebloc`]; correctness never published; DCT identical to Rime's; channel resolution against `s_basicassets`; additive flag passed through, no additive reconstruction; float channels dropped |
| **AssetBankPlugin** (github.com/marv7000/AssetBankPlugin) | Frosty plugin, FB3+: BF4 ✅, others WIP; **not BF3** | Raw, Frame, DCT; Curve/Vbr WIP | FBX (+ SMD/SEAnim exporters in code) | GPL-3.0 | 11 commits, 2023-04/07, "untested" disclaimer | same author and same DCT code [verified diff] |
| Frosty Toolsuite | FB3+ (BF4, BFH, BF1, BFV…) | — (via plugins) | — | — | BF3 absent from support lists [report claim, mintlify docs mirror] | not applicable to FB2 |
| FMT / FrostbiteModdingTool (FMT.Core) | FIFA/Madden-era FB | **VBR** encoder/decoder, FBX | — | — | [report claim, NuGet listing] | VBR ≠ BF3's DCT |
| id-daemon tools (Zenhax "Frostbite dump / model tools", 2017–2023) | BF3, BF4, BFH, BF1, BFV, MoHW, SWBF… | face poses only ("run the face tool where you have assetbanks and the skeleton") | SMD skeletons/meshes, XNALara ascii | closed | [report claim, zenhax thread] | no animation export documented |
| Noesis | — | — | — | — | no Frostbite AssetBank plugin found | — |
| Venice Unleashed | runs the original BF3 | — | — | — | knowledge only: `RagdollComponent` exposes per-bone Get/Set Local/World/Render transforms (`C:\Users\olku\VU-Docs\content\vext\ref\client\type\ragdollcomponent.md`); VU raised the engine's animation-operation batch from 64 to 512 (`content\general\changelog.md:481`) | not a route (runs bf3.exe) |
| DICE talks | GDC 2012 "Animation methodology for Battlefield 3" (Dahl, Hogstrom); "Modular Rigging in Battlefield 3" (Ramström: own deformation rig + ANT "used in FIFA" + extensive mocap) | — | — | — | abstracts only [report claim, gdcvault.com/play/1015831, /1015573] | no format detail |

No decoder exists in C or C++; none was validated against the running game.

---

## 3. What a BF3 soldier needs at runtime

### 3.1 Inventory, mapped to BF3's own signals

| layer | needs | BF3 source of truth |
|---|---|---|
| 1p weapon (Venice1pMasterSkeleton, arms mesh `characters/arms/arms1p_*` + weapon `mesh1p`) | deploy (first / normal / alt), undeploy, quick switch, idle, ADS in/out/idle (per-sight offset from data), fire (auto / single / hold-release / simple, last-round), reload full / empty (+ per-shell shotgun), bolt / pump cycle, fire-mode change, sprint in/loop/out, melee bash, quick-throw grenade, bipod / foregrip / GL / underslung shotgun variants, lower gun, camera bone motion | `AnimatedWeaponBinding` + 1p-only binding; `WeaponAnimType`; `WeaponOffsetData`; timing from `FireLogicData`/`WeaponFiringData`; procedural layer from `FirstPersonCameraData.weaponSpringEffect/cameraSpringEffect` (`WeaponLagSpringEffectData {offsetSprings, rotationSprings: WeaponLagEffectSpringVector {springX/Y/Z: {constant, damping}}, jumpForces, landForces, poseUpForces, poseDownForces, zoomForceModifier}`), `GunSwayData`, `RecoilData`, `CameraRecoilData {springConstant, springDamping, springMinThresholdAngle}`, Ant `WeaponLagSceneOpAsset` (PID) |
| 3p body (Venice3pMasterSkeleton, VeniceAntSke01 213 bones) | stand/crouch/prone × 8 directions × walk/run, sprint, turn in place, stance transitions, jump/fall/land, vault high/low/up, slide-to-prone, ladder, swim; upper body per `animBaseSet` (8 sets) with aim pitch/yaw offsets, fire, reload, deploy; hit reactions; emotes (13) | `PhysicsMotion` signals, `SpecialMovesBinding`, `ClimbLadderBinding`, `HitReactionComponentBinding`, `SoldierEmoteBinding`; hand IK onto `IK_Joint_*` (HumanIK in BF3) |
| vehicles | seat poses for open seats only (enclosed crews hidden), 1p arms on wheel for open-vehicle drivers, enter/exit | `PlayerEntryComponentData.antEntryEnumeration.value` → `vehicleEntryIndex`; `AnimatedDriverEntityData` |
| parachute | deploy, glide with steer, land; canopy rig (42 bones, ParachuteMasterSkeleton) | `SoldierParachuteBinding`; `SoldierParachuteComponentData.undeployTime` 1.0 |
| death / revive | directional deaths, headshot, explosion, then ragdoll blend; man-down; defib revive get-up | `SoldierHealthModuleBinding` (dead, revived, hitFront/Back/Left/Right…), `RagdollBinding`, `VeniceSoldierHealthModuleData {TimeForCorpse 15, ManDownStateTime 13, PostReviveHealth 20}` |
| knife takedown | paired attacker/victim (front/back; crouch/prone variants) with alignment warp | `MeleeEntityCommonData`, `CharacterMeleeBinding`, `WarpAnimationEntityData` (BoneToAlign ConnectBone) |
| server | 3p pose for 9 hit capsules (hi LOD) | Realm_Server animatable + `SkeletonCollisionData` |

### 3.2 Clip counts [inferred estimates; the probe's census replaces them]

Full BF3 parity: 85 1p packages × ~20–30 clips (core set + attachment variants) ≈ **2,000 1p clips**, plus the 3p graph in
`s_basicassets`/`b_basicassetsmp` (low thousands).

Minimal viable set for **6 weapons, 4 classes** (e.g. M416 assault, M4A1 engineer, M249 support, M40A5 recon, M9 pistol, knife+grenade
shared):

| block | clips |
|---|---|
| 1p per gun × 5 (deploy-first, deploy, undeploy, idle, ADS idle, fire, ADS fire, reload, reload-empty, sprint in/loop/out; bolt cycle for the M40A5) | 12 × 5 = **60** |
| 1p shared: knife slash + takedown, grenade pull + throw, 4 class gadgets (ammo/medic bag throw, defib ×2, repair tool ×3, C4/claymore place ×2) | **~14** |
| 3p lower body: stand idle, walk 8, run 8, sprint, crouch idle, crouch walk 8, prone idle, crawl 4, turn L/R, 4 stance transitions, jump start/loop/land, fall | **~44** |
| 3p upper body per set (rifle, pistol, LMG, sniper): aim up/mid/down, fire additive, reload, reload-empty, deploy, sprint carry | 8 × 4 = **32** |
| deaths (front/back/left/right, headshot) → ragdoll; revive get-up + defib use; takedown attacker/victim; grenade throw 3p | **~10** |
| seats (car driver, passenger, turret gunner, heli pilot, heli gunner, jet pilot, boat driver), parachute (deploy, glide), ladder (2), swim (2) | **~13** |
| **total** | **≈ 170–200** |

---

## 4. Godot 4.8 side (verified against `API`, Godot v4.8.dev7.official, single precision)

### 4.1 Skeleton and skin built at runtime [verified API]
`Skeleton3D`: `add_bone(name)`, `set_bone_parent`, `set_bone_rest(Transform3D)`, `set_bone_pose / _position / _rotation / _scale`,
`get_bone_global_pose`, `create_skin_from_rest_transforms()`, `register_skin(Skin) → SkinReference`, `localize_rests`,
`set_modifier_callback_mode_process(PHYSICS|IDLE|MANUAL)` + `advance(delta)`, `motion_scale`, `animate_physical_bones`. `Skin`:
`add_bind(bone, pose)`, `add_named_bind(name, pose)`, `set_bind_pose`. `ArrayMesh.add_surface_from_arrays` with `ARRAY_BONES`/
`ARRAY_WEIGHTS` (4 or 8 influences). Mapping: rest = EBX `LocalPose` as `Transform3D(Basis(right, up, forward), trans)` (handoff §5);
bind = inverse `ModelPose` for body meshes, `SubSkeleton.TransformMap` for weapon meshes; vertex bone index = LOD bone table entry of
`subset.boneSlots[slot]` mapped onto the master skeleton.
**Pitfall** [verified docs, class SkinReference]: "a Skeleton3D with multiple MeshInstance3D nodes which each have different
MeshInstance3D.skin objects may have multiple SkinReference instances (and hence, multiple skeleton RIDs)". A BF3 soldier is 4–6
skinned parts (head, upper body, lower body, arms, gear, weapon): build **one Skin per soldier skeleton covering all bones** and remap
every part's indices to it, so each soldier is one RenderingServer skeleton, not six. Node-less alternative: `RenderingServer.
skeleton_create`, `skeleton_allocate_data`, `skeleton_bone_set_transform`, `instance_attach_skeleton` [verified API]. GPU skinning in
the RD renderers is a compute pass (`servers/rendering/renderer_rd/shaders/skeleton.glsl`: 64-thread groups, up to 8 weights,
positions/normals/tangents to a destination buffer) [verified source], so depth/shadow passes reuse the deformed buffer [inferred].

### 4.2 Building `Animation`/`AnimationLibrary` in C++ [verified API]
`Animation.add_track(TYPE_POSITION_3D | TYPE_ROTATION_3D | TYPE_SCALE_3D | TYPE_BLEND_SHAPE | TYPE_VALUE | TYPE_BEZIER | …)`,
`track_set_path("Skeleton3D:Bone")`, `rotation_track_insert_key(t, Quaternion)`, `position_track_insert_key`, `set_length`,
`loop_mode`, interpolation NEAREST/LINEAR/CUBIC/LINEAR_ANGLE/CUBIC_ANGLE/MAKIMA, `optimize(velocity_err, angular_err, precision)`,
`compress(page_size, fps, split_tolerance)` (read-only afterwards), markers. `AnimationLibrary.add_animation`. `AnimTrackData`
Hermite keys map to `bezier_track_insert_key(t, value, in_handle, out_handle)` (handles = tangents/3) [inferred]. Everything is
reachable from godot-cpp because it is generated from this API. Only needed if Godot's mixer is used (§4.7 says: not for the crowd).

### 4.3 AnimationTree [verified API]
`AnimationTree.tree_root`; nodes `AnimationNodeBlendTree`, `StateMachine` (+ `StateMachinePlayback.travel`), `BlendSpace1D/2D`
(**`sync_mode` NONE / INDEPENDENT / CYCLIC_MUTABLE / CYCLIC_CONSTANT and `cyclic_length`** — phase-synced locomotion),
`OneShot` (`mix_mode` BLEND/ADD, fade curves), `Add2/3`, `Blend2/3`, `Sub2`, `TimeScale`, `TimeSeek`, `Transition`, `Sync`,
per-node bone filters (`set_filter_path`, `filter_enabled`, FilterAction PASS/STOP/BLEND), observers (`AnimationNodeObserverOneShot`
signals started/finished), and `AnimationNodeExtension._process_animation_node` for custom C++ nodes. `AnimationMixer.
callback_mode_process` PHYSICS/IDLE/**MANUAL** + `advance(delta)` (throttling), `root_motion_track`, `deterministic`.

### 4.4 SkeletonModifier3D stack [verified API; semantics from docs.godotengine.org/en/latest]
Modifiers run after the AnimationMixer, as children of the `Skeleton3D`; custom ones override `_process_modification_with_delta`.
Available: `TwoBoneIK3D` (root/middle/end bones, **target and pole are NodePaths**, needs a pole), `ChainIK3D` → `SplineIK3D`,
`IterateIK3D` → `FABRIK3D`/`CCDIK3D`/`JacobianIK3D` (IK returned in 4.6 per godotengine.org/article/inverse-kinematics-returns-to-godot-4-6),
`ModifierBoneTarget3D` ("intended to be used as another SkeletonModifier3D's target" — follows a bone inside the modifier cycle),
`LookAtModifier3D` (limits, damping, origin bone), `AimModifier3D`, `BoneConstraint3D` → `CopyTransformModifier3D` /
`ConvertTransformModifier3D`, `SpringBoneSimulator3D` (sling/belt/strap jiggle), `LimitAngularVelocityModifier3D`,
`BoneTwistDisperser3D`, `RetargetModifier3D`, `PhysicalBoneSimulator3D`; legacy `SkeletonIK3D`. BF3 fit: left hand →
`TwoBoneIK3D` (LeftArm/LeftForeArm/LeftHand) with target = `ModifierBoneTarget3D` on `IK_Joint_LeftHand`, pole from the elbow
direction; aim offsets → `LookAtModifier3D`/`AimModifier3D` on Spine1/Spine2 or additive clips; belts → `SpringBoneSimulator3D` on
`Wep_Belt1–5` (BF3 does it with `BoneFakePhysicsData`). With an own C++ animation core (§6) the same two-bone solve is ~40 lines
and runs on the server too; the Godot modifiers are optional conveniences on the client.

### 4.5 Retargeting [verified API + docs]
`SkeletonProfile` / `SkeletonProfileHumanoid` / `BoneMap` (import-time retarget in the editor) and `RetargetModifier3D`
("retrieves the pose … relative to the parent Skeleton's rest in model space and transfers it to the child Skeleton", per-transform
flags, `use_global_pose`). For Route B the authored clips live on a generic humanoid; at runtime we read the BF3 skeleton from the
install and transfer rest-relative rotations (own C++ bake, or `RetargetModifier3D` from a hidden source skeleton). No BF3 data is
written; the retarget result can be cached in `user://` keyed by the skeleton partition's digest.

### 4.6 Ragdolls [verified API; Jolt limits from docs]
`PhysicalBoneSimulator3D` (`physical_bones_start_simulation`, collision exceptions) over `PhysicalBone3D` (joint types
PIN/CONE/HINGE/SLIDER/6DOF, mass, damping, `apply_impulse`). Jolt (the project's physics server) ignores ConeTwist `bias/relaxation/
softness` and 6DOF soft-limit properties (docs "Using Jolt Physics") — set hard limits. Without `RagdollResource`, build the ragdoll
from `SkeletonCollisionData` capsules (9 hi-LOD bones) plus hand-set joint limits; blend from the animated pose using `RagdollBinding`
semantics (ragdollBlend in, fully-blended-in).

### 4.7 Performance of 64 animated soldiers [verified: measured on this machine]

Ryzen 7 5800X, Godot 4.8 dev7 console build, headless (no rendering, no skin uploads), synthetic clips animating every bone with
rotation + position tracks at 30 keys/s, 600 measured frames. Harness: `SCRATCH\bench\bench.gd` (scratch-only GDScript, not part of
the project; headless frames are floored at 6.9 ms by the engine, so values ≤ 6.9 mean "below the floor") and
`SCRATCH\bench\sampler_bench.cpp` (MSVC /O2 /arch:AVX2).

| path | 64 × 213 bones | 64 × 100 bones | 16 × 213 |
|---|---|---|---|
| skeletons only, no animation | ≤ 6.9 ms frame (process 0.7–1.2 ms) | — | — |
| `AnimationPlayer`, 1 clip each | **10.5 ms/frame** (~0.16 ms per soldier) | ≤ 6.9 | — |
| `AnimationTree`: BlendSpace2D of 5 clips + filtered additive OneShot | **34.6 ms/frame** (~0.5 ms per soldier) | 15.5 ms | ≤ 7.1 |
| setting all bone poses from script into `Skeleton3D` (27k calls) + its update | set calls 0.79 ms, process 3.1 ms vs 1.0 baseline → **~2 ms** | — | — |
| plain C++ pose pipeline (3-clip blend + additive half body + hierarchy + 3×4 skin matrices), 1 thread | **0.46 ms** | 0.20 ms | 0.10 ms |

Godot issues agree: per-skeleton skin updates become a bottleneck past ~100 skeletons and animation is single-threaded
(github.com/godotengine/godot/issues/99194, /93568, both open). Conclusions: (1) Godot's mixer is fine for one 1p viewmodel and props,
not for the crowd; (2) evaluate poses in our own C++ (parallel over soldiers with `WorkerThreadPool.add_group_task` [verified API]),
then push to `Skeleton3D` on the main thread; (3) add BF3-style temporal LOD (`GameAnimationSettings` six distance tiers): full rate
< 25 m, 30 Hz < 60 m, 15 Hz < 120 m, 7.5 Hz beyond, pose freeze off-screen (client) [tier numbers inferred, BF3's values unread];
(4) one shared Skin per soldier; (5) if the per-skeleton RS cost still bites at 64, fall back to a bone-matrix texture skinned in our own
vertex shader for far LODs. Budget target: ≤ 2 ms main-thread animation+skeleton work for 64 soldiers at 1440p; re-measure with
rendering on the RX 9070 XT.

### 4.8 First-person viewmodel in the same HDR frame (handoff decision 2) [verified]
* Built in since 4.5 (PR #93142, clayjohn, merged 2025-05-22, github.com/godotengine/godot/pull/93142): `BaseMaterial3D.
  use_fov_override` + `fov_override` (absolute FOV, behaves like `KEEP_HEIGHT`, perspective cameras only), `use_z_clip_scale` +
  `z_clip_scale` (scales the vertex toward the camera "without changing how the mesh is sorted or shaded"), flags
  `FLAG_USE_Z_CLIP_SCALE` 23 / `FLAG_USE_FOV_OVERRIDE` 24 [verified API + docs].
* For our generated `ShaderMaterial` families: spatial `vertex()` has `inout mat4 PROJECTION_MATRIX`, `out float Z_CLIP_SCALE`,
  global `IN_SHADOW_PASS` [verified docs, spatial_shader.html]. Generate a "viewmodel" variant of each material family that rebuilds the
  projection from `SoldierWeaponData.renderFov` (55) / `zoomRenderFov` (15–28, lerped by zoom) and writes `Z_CLIP_SCALE`, skipping both
  in the shadow pass.
* Same frame, same exposure, same lights, Enlighten probe SH as instance parameters — no SubViewport. PR caveats: screen-space effects
  (SSAO/SSR) misbehave on low z-clip-scale objects; translucent parts can mis-sort; multi-mesh weapons can be culled at high FOV
  (set `custom_aabb`); a project-wide FOV must be pushed to every material. Shadows: 1p meshes `cast_shadow` OFF, the local player's 3p
  body `SHADOW_CASTING_SETTING_SHADOWS_ONLY` [inferred, standard practice]. TAA/FSR motion vectors with an overridden projection are
  unverified (§7). `CameraJoint` motion in 1p clips should move the render camera (not the aim) [inferred from the 1p skeleton].

---

## 5. Fallback routes if Ant cannot be decoded in time

| route | what | cost | quality | legal |
|---|---|---|---|---|
| **A** decode Ant | runtime reader for GD banks + DCT/Frame/Raw codecs + channel mapping, clips sampled straight from the install | 1–2 weeks after a positive gate (§6); risk concentrated in the unknown DCT details | BF3-exact motion; graph logic still ours | reads the user's files at runtime, nothing redistributed. Implement clean-room from a written spec: Rime has no licence, IceBloc/AssetBankPlugin are GPL-2/3 — translating their code makes the project a derivative |
| **B1** authored 3p | CC0 humanoid clips retargeted at runtime onto `VeniceAntSke01`: Quaternius Universal Animation Library (CC0; 45 free of 120+, 8-direction locomotion, deaths, "combat and gun"; UAL2 adds 130+ incl. armed combos) [report claim quaternius.com, 80.lv]; CMU mocap (free for research and commercial use, citation requested; some re-uploads add NC licences) [report claim]; own mocap (any capture tool; the data is ours) | ~1–2 weeks for locomotion + aim + deaths on one rig; weapon-class upper bodies are the gap (few CC0 rifle sets) | generic, not BF3's style; fine at 3p distances | ship CC0/own files in the project. **Mixamo**: royalty-free in games, but raw files may not be redistributed and "easily found" files in game data are reportedly a breach [report claim, Adobe FAQ via licenseorg / community copies]; an open repository would redistribute them → avoid |
| **B2** procedural 1p | the weapon moves as a rigid body on `Wep_Root` with hand IK onto `IK_Joint_*`, all driven by BF3's own data: ADS = blend to `WeaponZoomedOffset` over `zoomIn/OutSpeed` with `ZoomLevelData.fieldOfView`; sway/lag = `WeaponLagSpringEffectData` springs (constant/damping per axis, jump/land/pose forces); recoil = `RecoilData` + `CameraRecoilData` springs + `GunSwayData` dispersion; reload = scripted `Wep_Mag` out/in with left-hand IK to the mag, timed by `reloadTime`/`reloadTimeBulletsLeft`; bolt/pump via `Wep_Slide`; deploy = swing in over `deployTime`; sprint = held offset + bob | 2–3 weeks once, then ~0 per weapon (data-driven over all 85 packages' meshes) | "indie/arcade" reloads; recoil/sway feel can be close to BF3 because the numbers are BF3's | nothing to license |
| **C** hybrid | B2 for 1p from day one, B1 for 3p, then replace clip by clip as Route A decodes (same clip interface) | B + incremental A | improves monotonically | as above |

The hybrid is the natural shape if the runtime consumes clips through one interface (`ClipSource`: Ant decode or project-authored
glTF → baked tracks).

---

## 6. Recommendation

### 6.1 Decision procedure (one weekly usage window per gate)

| gate | work | exit criterion (all must hold) | on fail |
|---|---|---|---|
| **G0 census** (≤ 1 day; uses the probe's Rime/IceBloc run) | parse `s_basicassets`, `b_basicassetsmp`, M416 chunk, one collapse animset; count objects by layout; count `AnimationAsset.CodecType` values; list the M416 clip names and lengths | 0 parse errors on the four banks; ≥ 95 % of clips are DCT/Frame/Raw; `ClipControllerAsset` found for every M416 clip | Curve/Vbr dominant → add a codec week or go C |
| **G1 one clip right** (≤ 5 working days) | decode M416 "ReloadClipEmpty Anim" (IceBloc SMD or Rime + our mapping), apply to `Venice1pMasterSkeleton` rest (EBX `LocalPose`) with arms + M416 1p meshes skinned in Godot | (a) raw quaternion norms within 1 ± 0.02 for ≥ 99 % of samples; (b) no pops at 8-frame block seams: per-bone angular step at seams < 3× the median step; (c) after the clip, hands are within 1 cm of `IK_Joint_*` (or the grip) without IK; (d) clip length × `FPS` within 10 % of `FireLogicData.reloadTimeBulletsLeft`/`reloadTime`; (e) side-by-side with a BF3/VU capture of the same reload looks the same to the user; (f) a collapse animset (mehouse01large) drops its 10 parts plausibly | **Route C**: B2 1p now, B1 3p, Ant continues as background research |
| **G2 port** (≤ 5 days) | clean-room C++ GD reader + codecs + channel map in the `bf3` library, from a written spec | float-for-float match (≤ 1e-5) with the C# decode on 20 clips; all 85 weapon chunks + basics parse; load time < 50 ms per weapon chunk | keep C# as an offline oracle only |
| **G3 3p** | locomotion and upper-body sets from `s_basicassets`; our own graph replicating the needed signals | 64 soldiers within the §4.7 budget; foot sliding acceptable to the user | B1 for 3p, A for 1p |

Do G0–G1 **before M4 (vehicles)**, as the handoff asks; G1's verdict sets the shape of M7.

### 6.2 C++ architecture (both routes)

```
bf3 (static lib, no Godot)                                   bf3_godot (GDExtension, godot-cpp pinned to the 4.8 build)
├─ ebx/        SkeletonAsset, MasterSkeletonAsset, Ant*Asset,  ├─ SoldierVisual (C++ class, no AnimationTree)
│              bindings (AntRef ids), weapon/soldier data      │    Skeleton3D (master skeleton, rest from EBX) + one shared Skin;
├─ gd/         EA GenericData reader: wrapper, REFL layouts,   │    MeshInstance3D parts (or RS instances + RS skeleton);
│              DATA objects read by name through layouts       │    receives anim::Pose each frame → set_bone_pose_* (main thread)
├─ ant/        ClipSet: ClipController→Layout→ChannelToDof map,├─ Viewmodel: 1p Skeleton3D + arms/weapon with viewmodel material
│              codecs: Dct (block unpack → int16 coeffs,       │    variants (PROJECTION_MATRIX from renderFov, Z_CLIP_SCALE),
│              sample any t), Frame, Raw, (Curve)              │    camera-bone delta → Camera3D
├─ anim/       Skeleton (from EBX), Pose (SoA quat+vec3),      ├─ Ragdoll: PhysicalBoneSimulator3D from capsules + limits
│              Sampler, Blend (lerp/nlerp, additive, bone      ├─ Cache: user://cache/anim/<sha1 of chunk or res>.bin (optional;
│              masks), TwoBoneIK, aim offsets, temporal LOD,   │    DCT sampling is cheap enough to skip it)
│              ClipSource interface (Ant | authored bake)      └─ Debug: optional AnimationNodeExtension wrapper to preview clips
├─ graph/      SoldierAnimGraph: our own state machine fed by        in the editor
│              BF3-named signals (deploy, reload, fire, zoom,
│              offsetX/Y/Z, pose, speeds, dead, revived, entry index…)
└─ server/     reduced-skeleton pose for the 9 hit capsules at tick rate; capsule history ring for lag compensation
```
Threading: pose evaluation per soldier in `WorkerThreadPool` group tasks; Godot calls stay on the main thread. Determinism: the
server and the owning client evaluate the same `anim/` code at the same tick for capsules. Data flow never writes to the install;
caches go to `user://` keyed by content digest, as the handoff requires.

### 6.3 First concrete tasks
1. Read EBX skeletons and master skeletons in C++ (done in Python here: 179/213/25/42 bones) and show `Venice1pMasterSkeleton` +
   arms + M416 1p mesh in bind pose in Godot with one shared Skin; verify bind = inverse `ModelPose` and the weapon `TransformMap`.
2. With the probe: run G0 and G1 using IceBloc's SMD output as the reference decode (it already maps channels).
3. Write the GD/DCT spec from Rime + IceBloc behaviour (this report §1.3–1.5) and implement from the spec.
4. In parallel, prototype B2's spring layer from `FirstPersonCameraData`/`GunSwayData`/`RecoilData` — it is needed in Route A too
   (BF3 layers procedural lag and recoil on top of the clips).

---

## 7. What could NOT be determined

* Whether the DCT decode is correct: `deltaBase` as constant vs running DC predictor, `KeyTimes` semantics, sub-block/bit-width
  ordering, quaternion sign handling, float channels, the additive reference pose — no decoded clip was compared with the game.
* The `CodecType` census and clip counts per bank (probe's job); how many 1p clips the M416 chunk holds; durations and FPS.
* Whether `AntRef.assetId` = index into `s_basicassets` (span 62,414 vs 62,766 objects fits, unproven) and how `PackageMeta`'s
  GuidToInt/static import maps resolve ids and cross-bank GUID references; which bundle carries `s_basicassets` for every level.
* The skinning bone-name hash function and whether MeshSet bone tables index the master skeleton or the sub-skeleton.
* `RagdollResource` layout (probably a Havok packfile; untested with the existing Havok reader); BF3's joint limits.
* BF3's `GameAnimationSettings` temporal-LOD values and `AntSettings` values (engine settings, not in level bundles).
* Whether BF3's `renderFov` is vertical (Godot's override is vertical/KEEP_HEIGHT); BF3's 1p near plane and depth handling.
* TAA/FSR motion-vector behaviour with an overridden projection in Godot 4.8; viewmodel + SSAO/SSR interplay at the chosen
  `Z_CLIP_SCALE`.
* Real Godot cost with rendering: skin compute, per-skeleton RS updates and shadow passes for 64 soldiers on the RX 9070 XT
  (measured here headless only).
* How BF3's graph semantics (state flows, Voronoi blend spaces, foot plant, HumanIK) behave in detail — only the field lists are known;
  the plan replaces the graph with our own.
* The exact current Adobe Mixamo terms (help page returned 403; relied on secondary copies).

## Sources

Local: `RIME\RimeLib.Animation.Frostbite2_0\*` and `RIME\RimeLib.Animation\*` (files cited inline), `RIME\Utils\AntGenericDataInfo\
Program.cs`, `RIME\Utils\MeshExtractor\Program.cs:192-321`, `RIME\Utils\AntLayoutDumper\dllmain.cpp`, `RIME\External\010templates\
{AssetBank.bt, bree\fb2\AnimTrackData.bt}`, Rime git log (commits d61622d4, fb482a8c, c287b705); `VU\*.yaml` (types cited inline);
`C:\Users\olku\VU-Docs\content\vext\ref\client\type\ragdollcomponent.md`, `content\general\changelog.md:481`; `CORPUS\animations\
**`, `CORPUS\weapons\{m416,m9,m40a5,m249,knife}\*.ebx`, `CORPUS\vehicles\**`, `CORPUS\characters\soldiers\defaultsoldierbonecollision.ebx`;
`C:\Users\olku\iw4L\context\bf3dump\ebx\characters\soldiers\mpsoldier.json`; `...\bf3dump\03_mp001_contents.out.txt`; handoff
`HANDOFF.md` and `sections\02-resources.md` §2/§10, `sections\04-gameplay-sim.md` §11; `API`; `SCRATCH\{scripts\ebxdump.py,
skeletons.txt, ant_classes.txt, bench\*}`; IceBloc and AssetBankPlugin clones in `SCRATCH\ext\`.
Web: github.com/marv7000/IceBloc; github.com/marv7000/AssetBankPlugin; github.com/ModdersLink/Rime;
github.com/godotengine/godot/pull/93142; github.com/godotengine/godot/issues/99194 and /93568;
raw.githubusercontent.com/godotengine/godot/master/servers/rendering/renderer_rd/shaders/skeleton.glsl;
docs.godotengine.org/en/latest/classes/{class_basematerial3d, class_skinreference, class_retargetmodifier3d, class_twoboneik3d,
class_modifierbonetarget3d, class_skeletonmodifier3d}.html; docs.godotengine.org/en/latest/tutorials/shaders/shader_reference/
spatial_shader.html; docs.godotengine.org/en/latest/tutorials/physics/using_jolt_physics.html;
godotengine.org/article/inverse-kinematics-returns-to-godot-4-6; gdcvault.com/play/1015831 and /1015573; zenhax.com/viewtopic.php@t=4864;
quaternius.com/packs/universalanimationlibrary.html; 80.lv (UAL article); www.licenseorg.com/guide/3d-assets/mixamo;
mintlify.com/CadeEvs/FrostyToolsuite (support lists); nuget.org/packages/FMT.Core.

---

## Independent verification (2026-10-10)

An independent verifier wrote this section. The body above was left unchanged.

**Method.** `VS` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\verify-animation`.

- **EBX values** were re-read with the EBX probe's Rust dumper, not with this report's `ebxdump.py`. The dumper is `scratchpad\probe-ebx\target\release\ebx-dump.exe` and links the IW4L `ebx_bf3` and `superbundle_bf3` crates. Outputs: `VS\out1`, `VS\out2`, `research\probes\ebx\animation\*.json`.
- **Ant package bytes** were written into `VS\raw` by a 30-line read-only C# dumper (`VS\rawdump`). It calls Rime's mounter, which opens every install file with `FileAccess.Read`.
- **Banks and DCT clips** were parsed and decoded by a new Python GenericData reader and DCT decoder (`VS\scripts\gd.py`, `dct.py`, `m416.py`, `ids.py`). They were written from this report's §1.3/§1.5 text and use no Rime, IceBloc or probe code.
- **Godot benchmarks** were re-run from a copy of the bench (`VS\bench`).
- **The API** was queried from `API` with `VS\scripts\api.py`.

Nothing was written to the install.

| claim id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| C1 | Ant banks: BE `AntPackagingType` + `PackageMeta`, then self-describing `GD.STRM`/`GD.REFL`/`GD.DATA`; read objects through the in-file layouts | **confirmed** | My own reader parsed `m9` (51,948 B), `m416` (178,180 B) and `s_basicassets` (22,481,708 B; 261 layouts, 62,766 objects) from these bytes. M9 header bytes: `00000001 00000018 00000002 0000000B`. `GD.STRM` sits at 0x1C and spans to EOF. The REFL layouts carry names, offsets and types, e.g. `DctAnimationAsset` 0xB5A06C02 with 18 slots, `NumKeys` u16 at +172 and `Data` u8[] at +16. LayoutHeader = 32 B, EntryHeader = 32 B. `LayoutData` magic 0x030E6205 with the layout hash at +16 and `dataOffset` at +28. Array/string pointers are relative to the `LayoutData` start. All of this matches §1.3. (`VS\scripts\gd.py`; `VS\raw\*.bin`) | On PC every GD blob is flagged **`'l'` (little-endian)**: STRM, REFL and DATA in all three packages. Only the package header is big-endian (see extra-7) |
| C2 | MP_001: 91 `AntPackageAsset` = 85 Chunk (18,136–243,812 B, Σ 8,005,736; M416 178,180) + 5 Bundle + 1 Static | **confirmed** | The Rust dumper's `scratchpad\probe-ebx\antpackages.tsv` gives MP_001 85 Chunk, Σ ChunkSize 8,005,736, min ammobag 18,136, max g3 243,812, M416 178,180 (GUID 195869e7…). The 5 Bundles are b_basicassetsmp and b_vehicle_humvee/kornet/tow2/vodnik. `s_basicassets` is `AntPackagingType_Static`. The corpus `ebx_corpus\mp_001\animations\antanimations` holds 91 files. The M416 chunk read from the install is exactly 178,180 B | — |
| C3 | EBX skeletons 179/213/25/42 bones; masters splice WeaponSke01 at 153–176 / 155–178; `TransformMap` = −weapon `ModelPose` translation | **confirmed** | Rust dumps: venice1pske01 179, veniceantske01 213, weaponske01 25 and parachuteske01 42 entries each in BoneNames, Hierarchy, LocalPose and ModelPose. `BoneMap` [0,153…176] and [0,155…178]; 24/24 names match WeaponSke01. Over all 25 bones, max \|TransformMap.trans + weapon ModelPose.trans\| = 1e-7. Every TransformMap rotation and every weapon ModelPose rotation is identity, so −T is exactly the inverse bind. `ParachuteMasterSkeleton` has 0 sub-skeletons (`VS\scripts\skel.py`, `sold.py`) | — (but see extra-2: the clips target the Ant rig skeletons, which have 181/215 joints) |
| C4 | `IK_Joint_Left/RightHand` under `Wep_Root`; `Wep_Root` parent SpineX (1p) / Spine2 (3p) | **confirmed** | 1p: Wep_Root 153 has parent 42 SpineX; IK joints 172/173 have parent 153. 3p: Wep_Root 155 has parent 7 Spine2; IK joints 174/175. WeaponSke01: IK joints 20/21 under Wep_Root (`VS\scripts\skel.py`) | — |
| C5 | Rime = readers only; 258 generated classes from an injected DLL; DCT prints CSV; AntGenericDataInfo calls the throwing `Load(reader)`; front ends do not reference Ant; no licence | **confirmed** | `AssetBank.cs:524-527` and `ReflectionGeneralDataReader.cs:454-457` throw `NotImplementedException`. `Utils\AntGenericDataInfo\Program.cs:195,232,378` call the one-argument `Load`. `Decompressor.cs:178-182` only does `Console.WriteLine`. `ant\` has 258 files headed "automatically generated". `AntLayoutDumper\dllmain.cpp:131,180,323-327` emits `[AntBinding]`/`[AntField]` .cs files from `Sdk::GD` layouts. Only `RimeLib.Animation.Frostbite2_0.csproj` and `AntGenericDataInfo.csproj` reference `RimeLib.Animation`. No LICENSE in the repo; `gh api repos/ModdersLink/Rime` returns licence `none` | Note: Rime's two-argument `AssetBank.Load(reader, loader)` works on all 322 packages (probe §1) |
| C6 | IceBloc (GPL-2.0) is the only known FB2 end-to-end exporter; same DCT as Rime; only one decoding exists, never validated | **corrected** | The IceBloc pipeline is as described (`ext\icebloc\IceBlocLib.Frostbite2\Animations\Base\Animation.cs:44-150`). Its DCT is the same algorithm and near-identical text as Rime's: same table, Skip(1) in block 0, `+= DeltaBase` every block. Licence GPL-2.0 (gh api). A web search found no other FB2 animation exporter | The algorithm **has now been validated against the install data** twice. My independent Python decoder and the probe's decoder agree (see C7). IceBloc's channel mapping is **wrong** for `StorageType` 2 (extra-1). IceBloc's version (2023-02-04) predates Rime's oldest commit of that file (2024-08-22), so Rime's DCT is probably IceBloc-derived (GPL) [inferred] |
| C7 | DCT decode rules; DeltaBase constant vs running predictor unknown | **corrected** | My decoder (`VS\scripts\dct.py`) uses: desc>>4 sub-blocks; nibbles X..W with 15 = `CatchAllBitCount`; MSB-first bits; block-0 DC not stored; `C0 += DeltaBase` every block; basis 0.25 / 0.5·cos; scale (1+0.1·QMS·i)/QMB. On all 5 M9 DCT clips and all 24 M416 DCT clips, bits consumed equal bits predicted (e.g. M9 ReloadFull 68,466/68,466; ReloadEmpty 77,510/77,510). Mean \|q\|−1 is 3.1–6.0e-5. Undeploy frame 0 vs the uncompressed `IdlePose` Frame clip (same `ChannelToDofAsset`): 0.070° and 0.4 mm. A **cumulative** DC predictor gives mean \|q\|−1 = 2.4e-2 (ReloadFull) and 2.9e-2 (ReloadEmpty) | **DeltaBase is a constant per-DOF base added to every block's DC, not a running predictor** [verified]. Caveats: `QuantizeMultSubblock` was 0 in all 29 clips checked, so the (1+0.1·QMS·i) term is untested. A block holds 8 **keys**, not 8 frames: `KeyTimes` can be sparse (M9 Undeploy: 9 keys over frames 0–10), so sample by KeyTimes. `DataSize` = 12+n+8n+2Σsub+len(Data) **+1** on all 5 M9 clips |
| C8 | Soldier AntRef ids 357,040,602–357,103,016; probably indices into `s_basicassets` | **corrected** | Span confirmed: 206 ids, 62,414 apart. All share the high 16 bits **0x1548**. Each id's low 16 bits equal the low 16 bits of an `s_BasicAssets` object's `__guid` index (BE u32, 0x80001–0x8F788): 206/206 resolve, and the names match the binding fields. Examples: 0x154801DA → `HideWeapon3P`, 0x1548F5A8 → `CurrentProjectileIndex`, `vehicleEntryIndex` 357,078,069 = 0x15489435 → `Game.Vehicle.EntryIndex`, `Zoom` → `Game.Character.Zoom` (`VS\scripts\ids.py`, `idmatch.py`) | `AntRef.assetId = 0x15480000 \| (s_BasicAssets object guid index & 0xFFFF)` [verified mapping; the meaning of 0x1548 is unknown]. These are not array positions: guid indices are sparse and the max soldier index, 62,888, exceeds the 62,766 objects. The span-vs-count argument was coincidental |
| C9 | 4 `AntAnimatableComponentData` (1p client, 3p client, 3p **server**, parachute); 9 hi-LOD capsules; BF3's server therefore poses soldiers for hit detection, and ours must too | **corrected** | Data confirmed: realms Client/LocalPlayer (Venice1pMaster, pkg antanimations), Client/All (Venice3pMaster), Server/All (Venice3pMaster, pkg antanimations) and ClientAndServer (Parachute, parachute_mesh). `defaultsoldierbonecollision` has 13 `BoneCollisionData`: 9 with `ValidInHiLod` and 4 with `ValidInLowLod` (Head + 3 on AITrajectory). `GameAnimationSettings.ServerEnable` true. **But** `default/settings_win32` (superbundle `Win32/default_settings_win32`) has `SyncedGameSettings {AllowClientSideDamageArbitration true, MaxAllowedLatency 0.85}`, and VU's server `DamageInfo` exposes `isClientDamage` (`C:\Users\olku\VU-Docs\content\vext\ref\server\type\damageinfo.md:33,107`) | BF3 ships with **client-side damage arbitration enabled**, so the shooter's client decides hits within a latency limit [verified value; semantics inferred from names]. The server animatable shows that the server animates; that it is used to place hit capsules is not shown. "Ours must too" is a design choice (server-authoritative lag compensation), not BF3 parity |
| C10 | Enclosed crews hidden; open seats via `AntEnumeration.value` → `vehicleEntryIndex` (Humvee 0/11/3, Vodnik 9/10/12/13, Kornet 47, TOW-2 46); `AntEntryIdEnum` stale | **confirmed** | BMP-2 (7 seats), LAV-25 (7) and M1 (3) all have `ShowSoldierInEntry` false and enum `Humvee_Driver`. Every enumeration writes `AssetId` 357078069. Values: Humvee 0, 11, 3 (+1 seat with none), Vodnik 13, 9, 10, 12, Kornet 47, TOW-2 46 (`VS\out2\vehicles__*.json`). Kornet and TOW say `Humvee_Driver` with `AntEntryID` 'VadsGunner' | Humvee: one passenger seat has no `AntEntryEnumeration` |
| C11 | renderFov 55; zoomRenderFov 20/15/28; ADS offsets are data; timings are data | **confirmed** | M416 55/20 wabsRif (pkg m416, 17 `WeaponOffsetData`, e.g. zoomed (0, −0.045, −0.178)); M9 55/20; M40A5 55/15; M249 55/28 (pkg `fnminimi`); Knife 55/10, null package. M9 `FireLogicData` ReloadTime 1.6, ReloadTimeBulletsLeft 1.35, ReloadThreshold 0.75 (`VS\out1\weapons__*.json`) | — |
| C12 | Godot 4.8 dev7 exposes every listed runtime API | **confirmed** | All 95 queried classes, methods, properties and enums are present in `API` (header 4.8.0 dev7, precision single). `TwoBoneIK3D.set_target_node/set_pole_node(int, NodePath)`; `BaseMaterial3D` flags 23 `FLAG_USE_Z_CLIP_SCALE` and 24 `FLAG_USE_FOV_OVERRIDE`; `WorkerThreadPool.add_group_task`; `AnimationNodeExtension._process_animation_node` virtual | Naming: the BlendSpace property is **`sync`** (enum `SyncMode`), not `sync_mode`. Enums also contain `INTERPOLATION_MAKIMA_ANGLE` and `FILTER_IGNORE` |
| C13 | 64×213 bones: AnimationTree ~34.6 ms, AnimationPlayer ~10.5 ms, C++ 0.46 ms, set-pose ~2 ms | **confirmed** (reproduced, ±15 %) | Re-run on the same Ryzen 7 5800X with `Godot_v4.8-dev7_win64_console.exe --headless`: tree 30.06 ms/frame (process 33.8 ms), player 9.79 ms, skeletons only 6.92 ms (the 6.9 ms headless floor; 145 fps), setpose set 0.80 ms with process 3.85 vs 1.26 ms (≈2.6 ms). `sampler_bench.exe`: 0.413 / 0.193 / 0.109 ms. Issues #99194 and #93568 are open (skeleton CPU cost, one busy thread) | Caveats: the clips are synthetic. The C++ path omits the inverse-bind multiply and samples the additive clip without interpolation. The issues involve 100–10,000 skeletons, not 64 |
| C14 | Different Skins mean multiple RS skeletons; share one Skin per soldier | **confirmed** | The docs' class_skinreference text says multiple MeshInstance3D nodes with different skins "may have multiple SkinReference instances (and hence, multiple skeleton RIDs)" (docs.godotengine.org/en/latest/classes/class_skinreference.html) | — |
| C15 | Viewmodel in the main frame: `use_fov_override`/`use_z_clip_scale` (PR #93142, merged 2025-05-22, 4.5); `PROJECTION_MATRIX` inout, `Z_CLIP_SCALE` out, `IN_SHADOW_PASS` | **confirmed** | PR #93142 (clayjohn): merged 2025-05-22, milestone 4.5. Thread caveats: depth-dependent and screen-space effects break at low clip scale; translucent sorting; culling at high FOV (use a custom AABB); perspective cameras only; per-material FOV only. Spatial shader docs: `inout mat4 PROJECTION_MATRIX`, `out float Z_CLIP_SCALE`, global `in bool IN_SHADOW_PASS`. Flags 23/24 are in `API` | — |
| C16 | Licences: UAL CC0 (45 free of 120+); CMU free with citation; Mixamo forbids raw redistribution; GPL/no-licence code means clean room | **corrected** (minor) | quaternius.com UAL page: "CC0 License", 120+ animations, free Standard tier "60-70%" of the pack; no "45 free" figure. Adobe community copies of the Mixamo FAQ: games allowed, raw-file redistribution not. IceBloc GPL-2.0, AssetBankPlugin GPL-3.0, Rime none (gh api) | The "45 free" figure is not on the primary page. CMU terms were not re-checked. The clean-room advice stands and is stronger given C6 (Rime's DCT is likely GPL-derived) |
| C17 | `GameAnimationSettings` six temporal-LOD tiers; values not read | **corrected** | Values are in data: `default/settings_win32` → `GameAnimationSettings`: First 2 m/0.02 s, Second 30/0.04, Third 60/0.06, Fourth 90/0.08, Fifth 120/0.10, Sixth 150/0.12, `TemporalLoddingFarDistance` 208; `ServerEnable` true; `ClientEnable` true; `UseAnimationDrivenCharacter` false (`VS\out2\default__settings_win32.json`). Schema: `VU\GameAnimationSettings.yaml`, `LodBinding.yaml`; Rime `LodLayersControllerAsset.cs:27,30` | Read the tiers from data. They replace §4.7's inferred tiers (<25 m full, 30 Hz <60 m …). The meaning (update interval beyond each distance) is inferred |
| extra-1 | §1.4: `ChannelToDofAsset` APPEND = (target, slot) byte pairs (IceBloc) | **refuted** | IceBloc reads byte pairs (`Animation.cs:127-141`). The probe maps 684,459/684,459 MP channels only with StorageType 1 = u8 per channel and **2 = u16 big-endian per channel**; the u16 rule fixed the last 108 clips (`research\probes\ant-animation-probe.md` §5, `ant\mapcheck-delta8.txt`) [report claim from probe, not re-run] | StorageType 2 is one u16 BE DOF index per channel |
| extra-2 | §1.4 "Ant SkeletonAsset: no rest pose; the rest is in the EBX skeleton"; §4.1 "rest = EBX LocalPose" | **corrected** | Probe `ant\skel-Venice1pAntSke01.txt`: the rig skeleton `Venice1pAntSke01_New` has 181 joints vs 179 in EBX. Rest rotation is within 0.5° for 168 joints. Root-level joints (AITrajectory, Trajectory, CameraBase, Hips) differ by 90°, worst 120°/1.70 m. 3p `RIGWep` has 215 joints vs 213 | Animate in the **Ant rig's** hierarchy and rest (`RigAsset.Default*Values`, "<Layout>.<Joint>.q/.t"). Use EBX `ModelPose` only for inverse binds |
| extra-3 | §1.2/§7: bone-name hash unverified; §4.1 vertex bone = LOD table entry of `subset.boneSlots[slot]` | **corrected** | I recomputed hashQuick(lowercase) (h=5381; h=h·33^c): wep_root F7A6EE7E, wep_trigger 1E355B60, wep_slide ECFF882F, wep_mag 741C8C53, hips 7C714E07, spine 0BA23F84, spine2 7FEA3036, leftupleg 424AC395. These equal the raw mesh LOD hashes in `research\probes\ant\meshbones.txt`; the case-sensitive form differs. The probe reports that subset slots hold skeleton bone indices directly (values up to 203 > 70 LOD entries) [report claim] | Hash = hashQuick(lowercase(name)). Weapon meshes index `WeaponSke01`; arms index `venice1pske01` |
| extra-4 | §7 "temporal-LOD … engine settings, not in level bundles" | **refuted** | See C17: the values are in `Win32/default_settings_win32` (`default/settings_win32`) | — |
| extra-5 | §6.1 G1 criterion (d): "clip length × FPS within 10 % of reloadTime" | **refuted** | Length × FPS gives frames, not seconds. M9 data: ReloadEmpty `EndFrame` 65 (2.17 s at the probe's 30 fps) vs `ReloadTime` 1.6; ReloadFull 57 (1.90 s) vs `ReloadTimeBulletsLeft` 1.35, i.e. 35–41 % off on a correct decode. 2.17×0.75 = 1.63 ≈ 1.6 (`ReloadThreshold` 0.75) | Compare `EndFrame/FPS × ReloadThreshold` with `ReloadTime` (relation inferred, ±10 %) |
| extra-6 | §0.1 lists DCT + Frame/Raw/Curve; §1.4 calls VBR "FB3-style" | **corrected** | `VbrAnimationAsset` (`'VBR '`) occurs in BF3 itself: 3 clips in the M9 chunk (my reader). Probe census: 2,225 install-wide and 146 MP clips (pistol/launcher zoom). IceBloc's `VbrAnimation` is empty | VBR is a live BF3 codec and is undecoded. Stand-in: the Frame `IdlePose`/`ZoomPose` clips |
| extra-7 | §1.3 GD blob = id + 'b' (big-endian) | **corrected** | All GD blobs in m9, m416 and s_basicassets are `'l'` (e.g. `GD.STRMl` at 0x1C, `GD.REFLl` at 0x2C) | GD payload is little-endian on PC; the package header is BE |
| extra-8 | §0.6/§6: Route A gets a ≤5-day gate; Route B/C as fallback | **corrected** | G0 is done and G1 is largely met by the probe: 0 parse errors on 322 packages; DCT+Raw+Frame = 95 % of MP clips; 3,778/3,778 DCT decode; the M9 reload renders correctly in Godot. My decoder reproduces the numeric criteria | Proceed with Route A (C++ port). Route B is needed only for VBR gaps (Frame stand-ins) and for twist/roll rig ops |
| extra-9 | Core principle (read as is, no modification) across all recommendations | **confirmed, with caveats** | All routes read at runtime and cache to `user://` by digest. No mod bundles; bf3.exe is not run for gameplay. IceBloc opens the install with `File.OpenRead` but writes a decrypted catalogue copy to `Cache\<game>\` **relative to its working directory** (`IceBlocLib.Frostbite2\IO.cs:72-73`) | Run IceBloc (§6.3 step 2) with its working directory and output in scratch, never in the install. G1(e)'s "BF3/VU capture" runs the original game and is acceptable only as a reference (knowledge use). Mixamo files must not enter the repository |
| extra-10 | §3.2 ≈2,000 1p clips over 85 packages | **confirmed** (order of magnitude) | M416 chunk: 38 clips (24 DCT, 10 Frame, 4 Raw); M9: 11 (5 DCT, 3 VBR, 3 Frame) (my reader). 8.0 MB ÷ ~4.7 KB/clip ≈ 1,700 [inferred] | — |

**Could NOT be determined in this pass.**

- Whether the (1+0.1·`QuantizeMultSubblock`·i) term is right: no checked clip had QMS ≠ 0.
- The u16-BE `ChannelToDofAsset` rule and the subset-slot rule were not re-run independently. Both rest on the probe.
- What 0x1548 in `AntRef.assetId` encodes, and whether all banks (not just the soldier's ids) use the same rule.
- What `AllowClientSideDamageArbitration` exactly does in BF3's netcode, and what the server-realm animatable is used for (ragdoll? melee? validation?).
- Exact `GameAnimationSettings` semantics (whether a tier's DeltaTime applies beyond or within its distance; what happens past 208 m), and whether server configs override `default/settings_win32`.
- `ClipControllerAsset.FPS`/`FPSScale` playback semantics. 30 fps is the probe's inference.
- CMU mocap terms and the current official Adobe Mixamo terms (only community copies were read).
- Godot animation cost with rendering on (skin compute, shadows) for 64 soldiers. Re-measured headless only.
