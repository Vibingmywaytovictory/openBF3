# EBX data probe: soldiers, weapons, kits, vehicle AI (read from the retail install)

> **Repo note (2026-10-14):** the dumps this report cites under `research\probes\ebx\` are BF3-derived data. They were
> removed from the git repo and its history (decision D14: no BF3 bytes in the repo). A local copy is in
> `C:\Users\olku\BF3 reimplementation\research-data\probes\ebx\`, outside the repo. Regenerate them with
> `research\probes\tools\ebx-dump` (README) against your own install.

Probe run 2026-10-10 against `C:\Program Files\EA Games\Battlefield 3` (base + `Update\Patch` + `Update\Xpack1..5`),
**read-only**: the tool opens files for reading and writes only under the scratch folder and `research\probes`.
Tags: **[V]** verified here (in data read by the tool, or a primary source read), **[R]** report claim (a source not
re-checked), **[I]** inferred. EBX citations are `partition` + instance GUID (8-hex prefix where unambiguous); the
curated dumps under `research\probes\ebx\` hold every field of every cited instance.

## 0. Method and tool

* Tool: `research\probes\tools\ebx-dump` (Rust). It links byte-identical copies of the IW4L crates `superbundle_bf3`
  and `ebx_bf3` and a copy of `asset_transport\src\bf3` with three accessors added (README there). Built in scratch
  with `CARGO_TARGET_DIR` in scratch; nothing was built inside `C:\Users\olku\iw4L` [V]. Rime was not needed.
* The IW4L readers open every superbundle of the install, not only MP levels, once `LevelMount::mount` is public:
  all 76 superbundles in `layout.toc` mounted (Globals, UI, default_settings, Loc, 47 level superbundles,
  chunk superbundles) with **0 failed EBX parses** across 73,371 unique partitions [V]. `Win32/Weapons` has 0 bundles
  (chunks only) [V].
* Speed (user machine): full bundle listing of the install 3.3 s; one level superbundle's ~12.5 k partitions parsed
  2.2 s; all MP/XP + global superbundles (50,486 unique partitions) 32 s [V].
* Each weapon's data closure was dumped from Metro (`Win32/Levels/MP_Subway/MP_Subway`); soldier, kits, material grid,
  vehicles and spawns from Caspian (`Win32/Levels/MP_007/MP_007`) [V].

## 1. Partition census (task 1)

Full list: scratch `probe-ebx\all-entries.tsv` (1,094,723 rows: every ebx/res entry of every bundle) and
`probe-ebx\ebx-partitions.tsv` (73,371 unique partitions with first superbundle/bundle and copy counts). The
gameplay subset (8,141 partitions: `weapons/ characters/ gameplay/ animations/ ai/ persistence/unlocks/soldiers
input/` and vehicle AI) is `research\probes\ebx\partitions-gameplay-subset.tsv` [V].

| prefix | unique partitions (all) | in MP/XP+global superbundles | representative names |
|---|---|---|---|
| `weapons/` | 2,910 | 2,829 | `weapons/m16a4/m16a4` (SoldierWeaponBlueprint), `.../m16a4_gunsway` (GunSwayData), `.../u_m16a4_acog` (UnlockAsset), `weapons/common/bullets/556x45mmnato` (BulletEntityData), `weapons/common/zoomlevels/*`, `weapons/common/aimingcontrollers/*`, `weapons/accessories/*`, `weapons/gadgets/*` (194) |
| `characters/` | 2,236 | 1,318 | `characters/soldiers/mpsoldier` (SoldierBlueprint), `characters/soldiers/defaultsoldierphysics` (CharacterPhysicsData), `characters/soldiers/defaultsoldierbonecollision`, upperbody/lowerbody/headgear/heads meshes |
| `gameplay/kits/` | 40 | 40 | `gameplay/kits/usassault` … `rurecon`, `_gm`, `_xp4`, `_xp4_scv`, `_gm_xp4` variants (VeniceSoldierCustomizationAsset) |
| `gameplay/teams/` | 14 | 14 | `gameplay/teams/us`, `ru`, `us_large`, `neutral`, `_gm`, `_xp4`, `_xp4_scv` (TeamData) |
| `gameplay/` (all) | 548 | 354 | `level_setups` 143, `prefabs` 127, `xp2` 133 (gun master), `vehicles/*customization` 8, `weapons/invisible*` 7 |
| `animations/` | 760 | 224 | `animations/antanimations` (AntProjectAsset, 362 packages), `animations/antanimations/m16a2` (AntPackageAsset), `animations/skeletons/veniceantske01`, `venice1pske01`, `weapon/weaponske01`, `animations/settings/ragdolls/ragdoll01` |
| `ai/` | 244 | 45 | `ai/entries/jeep_driver_ai_entry` (AIEntryData), `ai/driversettings/car`/`tank` (DriverSettings), `ai/defaultsensing` (SensingSettings), `ai/weapons/handheld_rif_ai_weapon` (AIWeaponData), `ai/behaviours` (AIVehicleTypes) |
| `vehicles/` | 1,696 | 1,173 | 57 VehicleBlueprint in MP (`vehicles/m1a2/m1abrams`, `vehicles/humveearmored/humveearmored`, `vehicles/ah1z/ah1z`, …) |
| `persistence/unlocks/` | 1,433 | 1,232 | soldier specializations (`.../specializations/sprintboostl1`, ValueUnlockAsset) |

Type census over all MP/XP+global superbundles [V]: 96 SoldierWeaponBlueprint, 57 VehicleBlueprint,
2 SoldierBlueprint, 40 VeniceSoldierCustomizationAsset, 478 AntPackageAsset instances (362 inside the project asset),
72 SkeletonAsset, 4 MasterSkeletonAsset (`research\probes` scratch `types-all.tsv`).

Where things live [V]:
* Weapon partitions are duplicated into **each level's main bundle** (e.g. `weapons/m16a4/m16a4` is in 47
  superbundles, bundle `Win32/levels/<lvl>/<lvl>`); the M98B is `weapons/model98b/model_98b`.
* `characters/soldiers/mpsoldier` sits in the **mode** bundles (`Win32/levels/mp_007/conquest_large`, …) except
  Metro (main bundle). Kits and teams: mode bundles (`Win32/levels/mp_subway/teamdeathmatch`, `.../teams_large`).
* `animations/antanimations/s_basicassets` (AssetBank) is in every level's `*_gameconfiglight_win32` bundle and in
  `Win32/Globals` bundle `Win32/gameconfigurations/game`; `b_basicassetsmp` in every MP mode bundle.
* The per-level material grid is `levels/<lvl>/<lvl>/materialgrid_win32/grid` (MaterialGridData).

## 2. How BF3 organises a soldier weapon (structure, all [V] from `weapons/m16a4/m16a4`)

`SoldierWeaponBlueprint` → `SoldierWeaponData` (chain `WeaponEntityData ← GameEntityData …`) with: `WeaponFiring` →
`WeaponFiringData` (`DeployTime`, `AltDeployTime`, `WeaponSway` → GunSwayData in `<weapon>_gunsway`, `PrimaryFire` →
`FiringFunctionData`); `AimingController` → `SoldierAimingSimulationData` (in `weapons/common/aimingcontrollers/*`,
holding `ZoomLevels` → `ZoomLevelData` and `AimAssist`); `AIData` → `AIWeaponData` (`ai/weapons/*`);
`AnimationData` → `AntPackageAsset`; `AnimBaseSet` (`wabsRif`); `FirstPersonCamera`; `Customization`;
`RenderFov 55`; `WeaponModifierData[]` = `{UnlockAsset, Modifiers[]}`.

`FiringFunctionData` holds `Shot` (ShotConfigData: `InitialSpeed` Vec3, `ProjectileData` → BulletEntityData /
MissileEntityData / GrenadeEntityData, `NumberOfBulletsPerShell`, `NumberOfBulletsPerBurst`), `FireLogic`
(FireLogicData: `FireLogicType`, `FireLogicTypeArray`, `RateOfFire`, `RateOfFireForBurst`, `ReloadTime`,
`ReloadTimeBulletsLeft`, `ReloadType`, `BoltAction`{`BoltActionTime`,`BoltActionDelay`}, `HoldAndRelease`,
`Recoil` (all zero for the M16A4), `ClientFireRateMultiplier 0.67`), `Ammo` (AmmoConfigData), `OverHeat`,
`WeaponDispersion` (SoldierWeaponDispersion, all zero), `FireEffects1p/3p`, `Sound`.

**Spread and recoil are not in FiringFunctionData** but in `GunSwayData`: per stance (`Stand`, `Crouch`, `Prone`)
× (`NoZoom`, `Zoom`): `BaseValue`/`Moving`/`Jumping`/`Sprinting`/`Vaulting*` (GunSwayDispersionData `MinAngle`,
`MaxAngle`, `IncreasePerShot`, `DecreasePerSecond`), `Recoil` (GunSwayRecoilData `RecoilAmplitudeIncPerShot`,
`RecoilAmplitudeMax`, `RecoilAmplitudeDecreaseFactor`, `HorizontalRecoilAmplitudeIncPerShotMin/Max`), `GunSwayLag`,
plus `FirstShotRecoilMultiplier`, `ShootingRecoilDecreaseScale`, stance-transition penalties and `Modifiers` →
GunSwayModifierData per attachment (`m16a4_foregrip`, `_bipod`, …) [V].

Attachments and underslung launchers are modifiers keyed by unlock: e.g. `u_m16a4_m320_he` →
WeaponFiringDataModifier (a whole alternate WeaponFiringData in the same partition) + WeaponShotModifier;
`u_m16a4_sound_suppressor` → WeaponProjectileModifier (bullet `556x45mmnato_cold`) + WeaponShotModifier
(`InitialSpeed.z 380`); `u_m16a4_heavybarrel` → WeaponProjectileModifier (`556x45mmnato_heavy`). One M16A4 partition holds 11 WeaponFiringData, 3
FiringFunctionData, 10 WeaponFiringDataModifier [V]. AI soldiers get their own FiringFunctionData via
`weapons/common/ai_unlock` (`ai_western_tracer` bullet, 62-round mag) [V].

**Naming trap [V+I]:** `weapons/m16a4/m16a4` (DamageGiverName "M16A4") is full auto (`fltAutomaticFire`,
`FireLogicTypeArray [0]` = add single, enum from `VU-Docs\types\fb\FireLogicType.yaml`). The burst rifle is the
separate unlock `weapons/m16a4/u_m16_burst` (SoldierWeaponUnlockAsset, `Extra` → `u_m16a4_burst` →
WeaponFiringDataModifier → FFD `b8195790` `fltBurstFire`, 800 RPM, 3/burst). The US assault kit lists both
`u_m16a4` and `u_m16_burst` [V]; the in-game names are "M16A3" and "M16A4" [I, matches community naming].

## 3. Key weapon numbers (task 2/3)

All [V] from `research\probes\ebx\weapons\_summary.json` (generated by `scripts\summarize.py` from the dumps).
Angles in degrees, speeds m/s, gravity m/s², times s. "hip"/"ADS" = `Stand.NoZoom`/`Stand.Zoom` `BaseValue`
(min–max, +per shot); recoil = `Stand.Zoom.Recoil`: up = `RecoilAmplitudeIncPerShot`, horiz =
`HorizontalRecoilAmplitudeIncPerShotMin..Max`, dec = `RecoilAmplitudeDecreaseFactor`; FSRM =
`FirstShotRecoilMultiplier`.

| weapon (partition) | fire logic, RPM | mag × spare | reload tac / empty | v0 (Shot) | damage (bullet) | falloff | g | hip | ADS | recoil up / horiz / dec, FSRM | deploy |
|---|---|---|---|---|---|---|---|---|---|---|---|
| M16A3 auto (`m16a4/m16a4`) | Automatic+[Single], 800 | 31×5 | 1.8 / 2.37 | 650 | 25 → 18.4 (`556x45mmnato`) | 8 → 50 | −15 | 2.5–7 +0.1 | 0.2–3 +0.1 | 0.26 / −0.4..0.1 / 18, 2.5 | 0.67 |
| AK-74M (`ak74m/ak74`) | Automatic+[Single], 650 | 31×5 | 2.1 / 3.1 | 600 | 25 → 18.4 (`545x45mmwp`) | 8 → 50 | −15 | 2.5–7 +0.1 | 0.2–3 +0.1 | 0.28 / −0.3..0.2 / 18, 1.5 | 0.67 |
| M249 (`m249/m249`) | Automatic, 800 | 100×3 | — / 6.2 | 620 | 25 → 18.4 (`556x45mmnato`) | 8 → 50 | −15 | 5–7 +0.1 | 0.5–3 +0.1 | 0.4 / −0.4..0.4 / 12, 2.2 | 1.3 |
| M98B (`model98b/model_98b`) | SingleFireWithBoltAction, 360; bolt 1.3 | 6×7 | 2.9 / 4.7 | 650 | 95 → 59 (`338magnum`) | 15 → 150 | −9.81 | 5–7 +1.2 | 0–2 +0 | 0.5 / 0..0 / 6, 1.0 | 0.95 |
| M9 (`m9/m9`) | SingleFire, 400 | 16×4 | 1.35 / 1.6 | 380 | 25 → 13.75 (`9x19mm_pistol`) | 12 → 50 | −15 | 1–7 +0.2 | 0.5–3 +0.2 | 0.7 / −0.1..0.1 / 20, 1.0 | 0.54 |
| 870MCS (`remington870/remington870mcs`) | SingleFireWithBoltAction, 600; pump 0.48 | 5×7 | per shell 0.67 | 300 | 18 → 6 ×12 pellets (`12g_buckshot`) | 8 → 25 | −9.81 | 1.5–7 +0.6 | 0.1–2 +0.6 | 1.0 / −0.3..−0.1 / 10, 1.0 | 0.72 |
| RPG-7 (`rpg7/rpg7`) | SingleFire, 100 | 1×4 | — / 4.8 | 40 → MaxSpeed 100 (`EngineStrength 10`) | Missile `Damage 225`; blast 112, r 0.35–4 | — | −1.5 | 2–5.5 +1.8 | 0–3 +1.8 | 2.5 / −1..−0.5 / 15 | 1.0 |
| SMAW (`smaw/smaw`) | SingleFire, 350 | 1×4 | — / 5.8 | 40 → 100 | Missile `Damage 225`; blast 112, r 0.35–4 | — | −1.5 | as RPG | as RPG | as RPG | 1.0 |
| M320 HE (`gadgets/m320/m320`) | SingleFire, 250 | 1×4 | — / 2.9 | 45 | impact bullet 100 → 100 (falloff 100–200); blast 112, r 0.35–5 | — | −7.2 | 2–5.5 +1.8 | 0–3 +1.8 | 2.5 / −1..−0.5 / 15 | 1.0 |
| M67 (`m67/m67`) | HoldAndRelease | 1×1 | 1.0 | 18 | blast 112, r 1.5–7; fuse `TimeToLive 3.6`, `DetonateOnTimeout` | — | −9.8 | — | — | — | 0.2 |

Other verified details: missiles `TimeToArm 0.5`, `TimeToLive 10`; RPG/SMAW explosion `BlastImpulse 2000`,
`ShockwaveRadius 10`, `ShockwaveDamage 1`; a second, weaker VeniceExplosionEntityData (56 dmg, r 2.1–4) also sits
in `rpg7projectile` (role not determined). Bullet `TimeToLive` 1.5 (rifles), 5.0 (.338), 1.0 (buckshot).
Bullets' own `InitialSpeed 350` is overridden by `ShotConfigData.InitialSpeed`. `ReloadThreshold 0.85` and
`ReloadType rtMagazineWithPossibleShorterReload` on the rifles. Iron-sight `ZoomLevelData`
(`weapons/common/zoomlevels/defaultironsights`): `FieldOfView 40`, `MoveSpeedMultiplier 0.5`,
`LookSpeedMultiplier 0.5`; aiming controller `FovTransitionTime 0.15`, `ZoomTransitionTime 0.2`.

### Comparison with community values

Symthic (`symthic.com`, ECONNREFUSED) and its forum (`forum.symthic.com` NXDOMAIN, `forum.sym.gg` TLS error) and
web.archive.org were unreachable from this machine; fandom pages returned HTTP 402 to fetch. The values below are
from search-engine summaries of the Battlefield Wiki (which credits Symthic) and fan posts, so all are **[R]**.

| item | data [V] | community [R] | verdict |
|---|---|---|---|
| M16A3 damage / v0 / drop | 25→18.4, 650, −15 | 25→18.4, 650 m/s, drop 15 (BF wiki; Steam guide "until 8 m") | agree |
| M16A3 falloff | 8 → 50 m | 8 m (Steam guide) vs "14 m … 44 m" (gazettegamerreviews blog) | agrees with one source; blog differs |
| AK-74M recoil | up 0.28, horiz −0.3..0.2, dec 18, FSRM 1.5 | 0.28 up, 0.2 left, 0.3 right, FSRM 1.5, dec 18 (BF wiki) | agree; shows **sign: Max = left, −Min = right** [I] |
| AK-74M spread | hip stand 2.5/3.0, crouch 2.0/2.5, prone 1.5/2.0 (base/moving); ADS 0.2/1.0 | 2.5/3, 2/2.5, 1.5/2.5; 0.2/1.0 | agree except prone-moving 2.0 vs 2.5 |
| M249 | up 0.4, horiz ±0.4, dec 12, FSRM 2.2; hip 5/6; ADS 0.5/1.5, crouch ADS 0.4, prone 0.2 | 0.4/0.4/0.4, FSRM 2.2, dec 12; 5/6; 0.5/1.5, 0.4, 0.2 | agree |
| M98B RPM | `RateOfFire 360`, `BoltActionTime 1.3` | 46 RPM | agree if cycle = BoltActionTime (60/1.3 = 46.2) [I] |
| M98B damage / v0 / drop | 95→59, 650, −9.81 | 95/59, 650, 9.81, 1.3 s between shots (Battlelog posts) | agree |
| M98B falloff end | 150 m | "minimum past 120 m" (BF wiki) | **differs** (patch level?) |
| M98B spread decrease | `DecreasePerSecond 15` (hip and ADS) | 8.0/s (BF wiki) | **differs** |
| M98B magazine | `MagazineCapacity 6` | "5 + 1" | agree [I] |
| .338 chest multiplier | upper-spine 1.06 (grid) | "chest multiplier 1.06" (GameFAQs) | agree |
| M9 | 25→13.75, 380 | 25.0–13.75, 380 m/s | agree |
| 870MCS | 18→6 ×12, `BoltActionTime 0.48` | 18–6 ×12, 125 RPM | agree (60/0.48 = 125) [I] |
| RPG-7/SMAW | v 40→100, blast 112, r 0.35–4 | 40–100 m/s, explosion 112, 0.35–4 m | agree |
| RPG direct hit | `MissileEntityData.Damage 225` | "100 direct hit" | **differs** (225 may be the vehicle-facing value; not resolved) |
| health regen | `RegenerationDelay 10`, `RegenerationRate 5` | 16 s delay, 5 hp/s (BF wiki "Regeneration") | rate agrees, **delay differs** |

Sources: [Steam guide M16A3](https://steamcommunity.com/sharedfiles/filedetails/?id=204235598),
[gazettegamerreviews M16A3](https://gazettegamerreviews.blogspot.com/2013/12/battlefield-3-weapon-guides-m16a3.html),
BF wiki pages [AK-74M](https://battlefield.fandom.com/wiki/AK-74/Battlefield_3),
[M249](https://battlefield.fandom.com/wiki/M249_SAW/Battlefield_3),
[M98B](https://battlefield.fandom.com/wiki/M98B/Battlefield_3), [M9](https://battlefield.fandom.com/wiki/M9/Battlefield_3),
[870MCS](https://battlefield.fandom.com/wiki/Model_870/Battlefield_3), [RPG-7V2](https://battlefield.fandom.com/wiki/RPG-7/Battlefield_3),
[SMAW](https://battlefield.fandom.com/wiki/SMAW), [Regeneration](https://battlefield.fandom.com/wiki/Regeneration),
[GameFAQs L96 vs M98B](https://gamefaqs.gamespot.com/boards/621026-battlefield-3/62613854),
[Battlelog M98B platoon](http://battlelog.battlefield.com/bf3/platoon/2832655241391215372/).

## 4. Soldier (task 2)

Chain [V]: `gameplay/teams/us` TeamData (`Faction FactionUS`, `Soldier` → `characters/soldiers/mpsoldier`
SoldierBlueprint `261e43bf`, `SoldierCustomization` → 4 kits, `VehicleCustomization` → 8
VeniceVehicleCustomizationAsset) → SoldierBlueprint → `SoldierEntityData a9ffe6b4`. The blueprint partition holds
~290 instances incl. 45 runtime components (`RuntimeComponentCount 45`).

SoldierEntityData [V]: `MaxHealth 100`, `LowHealthThreshold 30`, `ExplosionDamageModifier 1.0`,
`RegenerationDelayModifier 1.0`, `HealthModule` → VeniceSoldierHealthModuleData `705967ee`, `CharacterPhysics` →
`characters/soldiers/defaultsoldierphysics`, `CollisionInfo` → CollisionData, `MaterialPair` (body) `6609a12f`,
`HeadMaterialPair 8b81bca1`, `FootMaterialPair fda445e9`, `SprintSettings` → SoldierSprintSettingsData,
`BreathControl` → BreathControlData, `DefaultViewMode PersonViewMode_ThirdPerson`.

| group | values [V] |
|---|---|
| CharacterPhysicsData | `Mass 100`, `PhysicalRadius 0.35`, `MaxAscendAngle 50`, `JumpPenaltyFactor 0.3`, `JumpPenaltyTime 1.0`, `WaterDepthLimit 1.4`, `LadderAcceptAngle 90`, `DefaultState OnGround`, 9 states |
| CharacterPoseData stand / crouch / prone | `Height` 1.8 / 1.15 / 0.3; `EyePosition.y` 1.6 / 1.0 / 0.3; `StepHeight 0.3`; capsule; transition stand→crouch 0.3, stand→prone 0.7, crouch→stand 0.3, crouch→prone 0.5, prone→stand 0.7, prone→crouch 1.05; look pitch ±45 (prone −30..45) |
| OnGround speeds (`CharacterStatePoseInfo.Velocity`) | stand 4.0 (back/strafe ×0.75), `SprintMultiplier 1.625` (→ 6.5 m/s [I]), `AccelerationGain 0.35`, `DecelerationGain −15`; crouch 2.0 (×0.8 back/strafe), prone 0.9 (back ×0.5, strafe ×0.7) |
| Jump / air | `JumpStateData.JumpHeight 0.6`; jump pose velocity 4.0, sprint ×1.2; InAir 6.0 (accel gain 0.1); `InAirStateData.FreeFallVelocity 16`; falling velocity 15 |
| other states | Sliding (`GravityScale 1`, `HorizontalInputScale 0.3`); Swimming 3.0 (`BodyUnderWater 1.4`); Climbing 1.32 (ClimbOff 0.99 m / 1.76 m in 0.75 s); Parachute 9.0, `TerminalVelocity 5`, `DeployTime 1.0`; AnimationControlled 4.0/1.8/0.8 |
| CharacterSprintData | `AllowContinousSprinting true`, `SprintPowerDecreasePerSecond 0` (unlimited sprint) |
| SoldierSprintSettingsData | `Fov 55`, `FovInTime/FovOutTime 0.1`, `RecoverTime 0.12`, `SprintToProneRecoverTime 0.9` |
| health module | `RegenerationDelay 10`, `RegenerationRate 5`, `ImmortalTimeAfterSpawn 2.0`, `ManDownStateTime 13`, `PostReviveHealth 20`, `TimeForCorpse 15`, `InteractiveManDown false` |
| fall damage (CollisionData) | `DamageAtVerticalVelocity` and `...HorizVelocity`: 0 up to 13.9 m/s, 20 at 14, 40 at 22, 100 at 30 |
| suppression (SoldierSuppressionComponentData) | `SuppressionSphereRadius 1.5`, `SuppressionDecay 0.5`, `FallOffDelay 2.0`, thresholds low 0.3 / high 0.4 / UI 0.15, `SuppressionAbortsHealthRegeneration true` |
| breath (BreathControlData) | hold 4.0 s, release 3.0, penalty ×3 for 3.0 s |
| spotting (SpottingComponentData) | `SpottingDistance 1200`, `SpottingFov 4`, passive spot 1.0 s, order distances 30/50 |
| hit capsules (`defaultsoldierbonecollision` SkeletonCollisionData) | per bone `BoneName`, `CapsuleLength`, `CapsuleRadius`, `CapsuleOffset`, `MaterialPair`, `AnimationHitReactionType`: Head r 0.15 (head material), Spine r 0.23 + upper Spine r 0.25 (upper-spine material `84454789`), Arms r 0.08 len 0.28 (body), UpLeg r 0.11 len 0.4, Leg r 0.10 len 0.43 (leg material `f44644a0`, `DeactivateIfBehindWall true`) … (full list in the dump) |

### Damage multipliers (material grid)

Body-part multipliers are **not** on the soldier or the bullet: they come from the level's MaterialGridData cell for
(projectile MaterialPair, hit-capsule MaterialPair). Lookup [V]: `row = MaterialIndexMap[PhysicsPropertyIndex & 0xFF]`
for each pair, `InteractionGrid[row].Items[col].PhysicsPropertyProperties` → MaterialRelationDamageData. The soldier
cells are symmetric (row/column order irrelevant there) [V]. Caspian and Metro grids give identical values for the
cells checked [V]. Full table: `research\probes\ebx\soldier\damage-multipliers-mp007.json`.

| projectile material (example) | head | body / arms | upper spine | legs / foot |
|---|---|---|---|---|
| rifle/pistol/LMG (`5b6b84ac`: 5.56, 5.45, 9×19) | 2.0 | 1.0 | 1.0 | 0.91 |
| .338 Magnum (`4a899d88`) | 2.35 | 1.0 | 1.06 | 0.91 |
| 12g slug (`7783b944`) | 2.35 | 1.0 | 1.25 | 0.91 |
| 12g buckshot (`522396a0`) | 1.0 | 1.0 | 1.0 | 0.91 |
| 12g/40mm flechette (`bae0c7e7`) | 1.0 | 1.0 | 1.0 | 1.0 |
| 40mm HE impact / RPG-SMAW missile | 2.5 | 2.5 | 2.5 | 2.5 |
| M67 / 40mm LVG grenade body | 0.0 | 0.0 | 0.0 | 0.0 |

(`DamageProtectionMultiplier`; `DamagePenetrationMultiplier` is 0.8 on body/legs for most bullets; several cells add a
`NeverPenetrate` property.) Which field multiplies hit damage is [I] (`DamageProtectionMultiplier` reproduces the
community's 2.0/2.35 headshot and 1.06 chest values).

## 5. Kits (task 2)

Kits are `VeniceSoldierCustomizationAsset` (`gameplay/kits/usassault` etc.) [V]: `WeaponTable`,
`SpecializationTable`, `VisualTable` (CustomizationTable) → `UnlockParts[]` (CustomizationUnlockParts) →
`SelectableUnlocks[]` (SoldierWeaponUnlockAsset / UnlockAsset). Slot list sizes [V]:

| kit | primary (count, first entries) | sidearm | gadget 1 | gadget 2 | grenade | knife | specs | camo |
|---|---|---|---|---|---|---|---|---|
| US Assault | 28: u_m16a4, u_m416, u_aek971, u_m16_burst, … + shotguns + PDWs | 18 (u_m9 …) | 10 (M320 HE/SHG/SMK/LVG, M26 variants, crossbow) | u_medkit; u_defib / nogadget2 | u_m67 | 2 | 15 | 18 |
| US Engineer | 26: u_m4a1, u_scar-h, u_m4 … | 18 | 5: u_smaw, u_fim92, u_fgm148 … | u_repairtool, u_m15, u_eodbot | u_m67 | 2 | 15 | 18 |
| US Support | 26: u_m27iar, u_m249, u_pecheneg … | 18 | — | u_ammobag; u_c4, u_claymore, u_m224 … | u_m67 | 2 | 15 | 18 |
| US Recon | 26: u_mk11, u_sv98, u_sks … | 18 | 6: u_ugs, u_soflam, u_mav … | u_radiobeacon | u_m67 | 2 | 15 | 18 |
| RU Assault | 28: u_ak74m, u_m416, u_aek971 … | 18 (u_mp443 …) | as US | as US | u_m67 | 2 | 15 | 18 |

A SoldierWeaponUnlockAsset carries `Weapon` → SoldierWeaponBlueprint, `Identifier`, `WeaponIdentifier`,
`UnlockUserData` → SoldierWeaponMultiUnlock (pairs that combine e.g. rifle + `u_m320_he` → `u_m16a4_m320_he`) [V].

## 6. Vehicle AI and spawning (task 2)

Driver-seat AI on `vehicles/humveearmored/humveearmored` (Caspian) [V]:
* 4 PlayerEntryComponentData: seat 0 `AntEntryIdEnum_Humvee_Driver`, `EST_Driver`, `EntryRadius 3.0`, `AIData` →
  `ai/entries/jeep_driver_ai_entry` (AIEntryData `StrengthType LightArmour`, `ReuseTime 1.0`, `Mobility` →
  MobilityData `MaxSpeed 20`, `EquipmentType` → `ai/behaviours` AIVehicleBehaviourData); seat 1 gunner →
  `tank_gunner_ai_entry` (ArmamentData, TurretData, AIAimingConstraintsData); seats 2–3 passengers →
  `jeep_passenger_ai_entry` (`InterludeOnly true`).
* `DriverComponentData` `WantedSpeed 22`, `Settings` → `ai/driversettings/car` (DriverSettings: `LookAheadTime 1.2`,
  `MaxAcc 8`, `MaxDec 25`, `StoppingDeceleration 10`, `MinRadius 3.5`, `RadiusGrowthSpeed 0.3`,
  `SteeringSensitivity 25`, `AngleDiffForNoSlowdown 40`, `AngleDiffForFullSlowdown 70`, `VelocityAtFullSlowdown 5`,
  PID `P 0.1 I 0.01 D 0.01` with `UsePIDRegulator false`, throttle/accel curve breakpoints).
* `PathFollowingComponentData` `PreferredPathfindingIndex 1`, `AlternatePathfindingIndices [0]`,
  `UpdatePathAtDistancePercent 25`, `MovementCorridorRadius 0`.
* 5 `SensingComponentData` → `ai/defaultsensing` (SensingSettings: `MaxRays 3`, `LineOfSightTestTime 1.0`,
  `ImpactSenseDistance 12`, `SmokeSphereRadius 10` for 12 s, vision boxes; `DefaultTemplate` →
  `oldobjects/soldiers/templates/ai/defaultsoldiersensing` whose limits are all 0, `DebugText "FAIL No Template"`).
* `AnimatedDriverEntityData` (yaw ±30, velocity 2) and an `AIWeaponData` for the HMG (`vehicle_hmg_ai_weapon`:
  `MaxRange 200`, bursts 3–6 near / 3–4 far, cooldown 1–2.5 s, strengths Infantry 10, LightArmour 5).
* Infantry weapons carry AIWeaponData too (`handheld_rif_ai_weapon`: `MaxRange 100`, bursts 3–5 / 1–4, cooldown
  0.25–0.45 s, `StartAccuracyPercentage 0.5`, `ReloadBehavior RbInCover`).

This is DICE's single-player/co-op AI data riding along in MP vehicles. **No MP/XP level contains AI pathfinding
layers** (`*pathfinding*` partitions exist only in COOP/SP levels, e.g. `levels/coop_002/coop_002/layer23_general_pathfinding`),
and the install has no navmesh res type (all 20 res types seen are named; none is navigation) [V]. Bot navigation
must therefore be built by the reimplementation (fun-bots waypoints or a Recast bake), and these numbers serve as
tuning seeds for a driving controller [I].

VehicleSpawnReferenceObjectData example [V] (`levels/mp_007/conquest_large/layer0_cq_l_logic`, `f6ca88c0`):
`Blueprint` → `vehicles/m1a2/m1abrams`, `Team Team1`, `BlueprintTransform.trans (−17.60, 141.00, 751.45)`,
`SpawnDelay 30`, `InitialSpawnDelay 0`, `AutoSpawn true`, `UseAsSpawnPoint true`, `MaxCountSimultaneously 1`,
`TimeUntilAbandoned 10`, `TimeUntilAbandonedIsDestroyed 10`, `KeepAliveRadius 20`, `SpawnAreaRadius 2.5`,
`SpawnProtectionRadius 20`, `BotBailOutDelay 0.5`, `ReturnControlOnIdle true`. The partition holds 33 such
references (TOW2 ×6, Growler ×6, VDV buggy ×6, Kornet ×5, T-90 ×3, M1 ×3, Mi-28, AH-1Z, LAV-AD, Tunguska); 91 on
Caspian overall incl. `gameplay/level_setups/dynamic_vehiclespawners/*` for the jets [V].
File: `research\probes\ebx\vehicles\spawnref_mp007_cql_layer0.json`.

## 7. Animation resources (task 4)

* Res types present in the whole install: AssetBank 1,744 entries (397 names), AnimTrackData 5,005 entries (1,485
  names, mostly level/vehicle prop tracks), RagdollResource 48 entries (1 name, `animations/settings/ragdolls/ragdoll01`,
  41,744 B) [V]. MP/XP+Globals use 49 AssetBank names. List: `research\probes\ebx\animation\ant-resources.tsv`.
* Soldier banks [V]: `animations/antanimations/s_basicassets` (AntPackagingType_Static; **four size variants**
  22,223,108 / 22,226,764 / 22,356,568 / 22,481,708 B in different superbundles; Globals' is 22,481,708),
  `b_basicassetsmp` (Bundle, 15,929,912 B, every MP mode bundle), `b_basicassetsai` (8,110,112 B),
  `b_shared1p` (171,208 B, SP only), vehicle banks `b_vehicle_humvee` (199,460 B) … `b_vehicle_su-35` (10,552 B).
* **First-person weapon animations are chunks, not res** [V]: 86 standalone AntPackageAsset partitions
  (`animations/antanimations/m16a2`, `ak74`, `fnminimi`, `rpg7`, `smaw`, `m320`, `m9`, `remington870`, …) are
  `AntPackagingType_Chunk` with `StreamingGuid` = chunk id and `ChunkSize`. All 86 chunks resolved from the MP
  level mounts with decoded size exactly `ChunkSize` (e.g. M16A2 `54620b35-bdb6-a4f1-c73a-44712121a7a1`, 179,792 B)
  [V]. 29 more packages are `AntPackagingType_Bundle`. Table: `research\probes\ebx\animation\ant-packages.tsv`.
  The copies inside the `animations/antanimations` AntProjectAsset have zero StreamingGuid; only the standalone
  partitions carry the chunk id [V].
* Payload format [V, header bytes only]: weapon chunks and AssetBanks are big-endian with `GD.STRMl` / `GD.REFLl`
  section tags (Rime's `EA\GeneralDataReader` / `ReflectionGeneralDataReader` family) — e.g. M16A2 chunk starts
  `00 00 00 01 00 00 00 18 00 00 00 02 … 47 44 2e 53 54 52 4d 6c`. Decoding was not attempted here.
* Skeletons are plain EBX with bind poses [V]: `animations/skeletons/veniceantske01` (3p, 213 bones),
  `venice1pske01` (1p, 179 bones, includes `Wep_Root`, `Wep_Trigger`, `Wep_Slide`, `Wep_Mag`, `Wep_Grenade1/2`),
  `weapon/weaponske01` (25 bones), each with `BoneNames`, `Hierarchy` (parent index), `LocalPose`, `ModelPose`
  (LinearTransform), `HipBoneName Hips`, `HeadBoneName Head`, `CameraBoneName`, `WeaponBoneName Wep_Root`.
  MasterSkeletonAsset `venice3pmasterskeleton` = veniceantske01 + SubSkeleton weaponske01 with a 25-entry
  `BoneMap`. This closes part of the handoff's "bind pose for skinned meshes" gap.
* Soldier/weapon components reference Ant signals by `AntRef.AssetId` (e.g. health module `Dead 357048259`,
  weapons `Fire 357051578`) [V]; these are ids inside the Ant project, needed if Ant graphs are ever interpreted.

## 8. Consequences for the Godot design (all [I])

1. Weapon behaviour is fully data-driven and readable as-is: a C++ `WeaponDef` built at load from SoldierWeaponData +
   active unlock modifiers (WeaponFiringDataModifier swaps the whole firing data; Shot/Projectile/Magazine modifiers
   patch fields). Model fire-mode cycling from `FireLogicType` + `FireLogicTypeArray`, bolt/pump cycle from
   `BoltActionTime`.
2. Spread/recoil simulation needs GunSwayData semantics (per stance/zoom/move state; increase per shot, decay per
   second; vertical/horizontal recoil amplitude with decrease factor; first-shot multiplier). The numbers above agree
   with community-measured behaviour, so community descriptions of the algorithm can be used to fit the formula.
3. Damage = bullet `StartDamage→EndDamage` lerp over `DamageFalloffStart/EndDistance` × material-grid multiplier of
   the hit capsule; hit capsules come from `SkeletonCollisionData` on the animated skeleton (Godot: per-bone
   `CapsuleShape3D` Area3D/raycast targets driven by `Skeleton3D`). Read the grid per level.
4. Movement: a C++ character controller on Jolt with BF3 pose heights/eye heights and the speed table; the
   `CharacterPhysicsData` numbers are enough for M3.
5. Cache keys: identical partition names exist with different bytes across superbundles (`s_basicassets` four
   sizes); key converted data by content SHA1 (the bundle manifests carry it), not by name.

## 9. Artifacts

| path | content |
|---|---|
| `research\probes\ebx\weapons\*.json` | 10 weapon closures (root partition + bullets, gunsway, aiming, zoom, AI weapon, projectiles/explosions), minified |
| `research\probes\ebx\weapons\_summary.json` | per-weapon key numbers (input to §3) |
| `research\probes\ebx\soldier\characters__soldiers__mpsoldier.json` | SoldierBlueprint closure incl. physics, bone collision, sockets |
| `research\probes\ebx\soldier\gameplay__kits__*.json`, `gameplay__teams__us/ru.json` | kits and TeamData |
| `research\probes\ebx\soldier\damage-multipliers-mp007.json` | projectile × body-part MaterialRelationDamageData |
| `research\probes\ebx\vehicles\vehicles__humveearmored__humveearmored.json`, `m1abrams`, `ah1z` | vehicle blueprints + `ai/` partitions |
| `research\probes\ebx\vehicles\spawnref_mp007_cql_layer0.json` | one full VehicleSpawnReferenceObjectData + 33 summarised |
| `research\probes\ebx\animation\*` | skeletons, Ant project/packages, `ant-packages.tsv`, `ant-resources.tsv` |
| `research\probes\ebx\partitions-gameplay-subset.tsv` | 8,141 gameplay-relevant partitions with first superbundle/bundle |
| `research\probes\tools\ebx-dump\` | tool source + scripts + README |
| scratch `...\scratchpad\probe-ebx\all-entries.tsv`, `ebx-partitions.tsv`, `types-all.tsv` | full listings (too big for research) |

## 10. Could not determine

* Symthic primary pages: unreachable (connection refused; forum domains dead; archive.org blocked for this tool), so
  every community value is a search-summary [R]. Exact Symthic spread/recoil tables were not compared field by field.
* The runtime **formulas** for GunSway (how `RecoilAmplitudeDecreaseFactor`, `ShootingRecoilDecreaseScale`,
  `DeviationScaleFactor*`, `GunSwayLag` combine), and the sign convention of horizontal recoil (inferred from one
  AK-74M comparison).
* Whether `DamageProtectionMultiplier` or `DamagePenetrationMultiplier` (or both) scales hit damage; the role of the
  second weaker explosion in `rpg7projectile`; why `MissileEntityData.Damage` is 225 when the community quotes 100.
* Why data says `RegenerationDelay 10` while the community quotes 16 s (another component, a game-mode setting, or
  patch level), and the M98B falloff-end (150 vs 120 m) and spread-decrease (15 vs 8) differences.
* Which `FireLogicTypeArray` entry is the default mode, and how the client applies `ClientFireRateMultiplier 0.67`.
* `PreferredPathfindingIndex`/`AlternatePathfindingIndices` meaning (no MP pathfinding data exists to index).
* Ant payloads (chunks/AssetBanks) were only identified and sized, not decoded; the meaning of `AntRef.AssetId`
  values; whether the four `s_basicassets` variants differ in content or only in patch level.
* Health/gadget numbers for kits' gadgets (medkit, ammo bag, defib), and the `_gm`/`_xp4_scv` kit variants were not
  dumped. Jets, boats and helicopter driver AI were not dumped (AH-1Z and M1 closures are saved but not analysed).
* Patch level of the install (`Update\Patch` package `version 7` per its manifest [V]) versus the community data's
  patch level.
