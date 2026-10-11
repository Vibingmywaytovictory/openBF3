# 01 — BF3 infantry weapons: data, simulation, presentation, and a C++ design on Godot 4.8

Written 2026-10-10 for the Godot/C++ reimplementation. The game reads the retail install
(`C:\Program Files\EA Games\Battlefield 3`) in place and never writes to it; everything below is read from it at runtime
and converted data lives in `user://` keyed by content digest (handoff §0, HANDOFF.md:17-29).

Tags: **[V]** verified (checked here in retail data, code, the 4.8-dev7 API dump or a primary source that was read);
**[RC]** report claim (stated by a source not re-checked); **[I]** inferred (my reading). Frostbite type and field
names are spelled as the schema spells them (`C:\Users\olku\VU-Docs\types\fb\<Type>.yaml`, lower camel case); the Rime
JSON dumps use the same names in Pascal case.

## 0. How the numbers here were obtained (and how to reproduce them)

* **Retail EBX, patched install.** The IW4L Rime build (`C:\Users\olku\iw4L\context\rime-build\dist`, built from Rime
  HEAD b3145da, see `…\1-RESEARCH-PART\09-rime-tooling-and-live-bf3-dump.md`:12-14, 115-140) was copied to scratch and
  run read-only: `mount_game "…\Battlefield 3" Frostbite2_0 false` → `select_game 1` → `mount_sb
  win32/levels/mp_subway/mp_subway true` → `dump_partition_json <name> <dest> Indented`. 2,012 weapon partitions, the
  soldier, the global material container, the Metro material grid, 166 weapon-sound and 76 weapon-FX partitions were
  dumped to `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\weapons\`
  (`dumpall\`, `dump\`, `dump2\`, `dump3\`, `dumpfx\`; scripts `schema.py`, `resolve.py`, `table.py`, `grid.py`;
  the full roster `roster.tsv`) [V]. Nothing was written to the install.
* **Cross-check against the parallel probe.** `C:\Users\olku\BF3 reimplementation\research\probes\ebx-data-probe.md`
  (own extractor, 0 failed parses over 73,371 partitions) reports the same M16A3/AK-74M/M249/M98B/M9/870/RPG/SMAW/M320/
  M67 numbers (its §3, lines 91-114) and the same hit-zone multipliers (its §"Damage multipliers", lines 184-204) [V,
  two independent readers agree].
* **Community oracle.** Symthic's BF3 weapon chart ("up to date with the Aftermath patch", Dec 2012) and its mechanics
  tutorial were read from web.archive.org with curl (WebFetch is blocked there; the probe could not reach them):
  `https://web.archive.org/web/20130108185929/http://symthic.com:80/bf3-weapon-charts?recon=1&pdw=1&pistol=1&assault=1&engineer=1&support=1&sort=Class&adsc=DESC&attc=None`,
  tutorial thread 1434 (`…/web/20131009062619/http://forum.symthic.com/battlefield-3-technical-discussion/1434-in-depth-tutorial-for-charts-plots-recoil-and-spread-mechanism-etc/`),
  "state of knowledge" thread 2049 (`…/web/20130727134909/http://forum.symthic.com:80/battlefield-3-technical-discussion/2049-state-of-knowledge-on-weapon-handling-spread-and-recoil/`).
  An automated field-by-field comparison of all 62 Symthic rows against the install (ROF, v0, recoil up/left/right,
  recoil decrease, FSRM, ADS min, hip min, SIPS, empty reload) gives **50 of 62 identical on every field**; the 12
  differences are all explained in §2.8 [V].

Where weapon EBX lives [V]: `Win32/Weapons.sb` holds 105 `Win32/weapons/<w>/<w>_soldierweaponbundle` bundles with
**chunks only, no partitions, no res** (Rime `list_bundle_partitions`/`list_bundle_resources`; 40 chunks for M16A4) —
streamed 1p mesh/texture payloads. The weapon EBX is duplicated into every level's main bundle (Metro's
`Win32/levels/mp_subway/mp_subway` carries 3,497 `weapons/*` partitions incl. all XP1/XP2/XP4 weapons; the probe counts
`weapons/m16a4/m16a4` in 47 superbundles). So the loader reads weapon data from the level superbundle it is loading, or
from any one MP level once at startup, then caches it [I].

## 1. Data model

### 1.1 Partition map

| partition (Metro names; lower case as listed) | primary type / contents | shared? |
|---|---|---|
| `weapons/<folder>/u_<weapon>` | `SoldierWeaponUnlockAsset {weapon → SoldierWeaponBlueprint, extra → UnlockAssetBase, weaponIdentifier}` (+`UnlockAssetBase {unlockScore, identifier, debugUnlockId …}`) — one per selectable weapon variant (`u_m16a4`, `u_m16_burst`, `u_m4`, `u_m4a1`, `u_glock17`, `u_m9_silenced` …) [V] | per variant |
| `weapons/<folder>/u_<weapon>_<attachment>` | `UnlockAsset {linkedTo}` — attachment unlocks (`u_m16a4_acog`, `_heavybarrel`, `_m320_he`, `_12g_buck` …) [V] | per weapon |
| `weapons/<folder>/<name>` (e.g. `m16a4/m16a4`, `ak74m/ak74`, `model98b/model_98b`, `remington870/remington870mcs`) | `SoldierWeaponBlueprint` → `SoldierWeaponData`; all `WeaponFiringData`/`FiringFunctionData` variants (M16A4: 11 WFD, 10 `WeaponFiringDataModifier`), every `WeaponModifierData` entry and modifier object, `SocketData` + `WeaponRegularSocketObjectData`, `WeaponStateData`, `UnlockComponentData`, `EntityVoiceOverInfo` [V] | per weapon |
| `weapons/<folder>/<name>_gunsway` (sometimes inside `<name>`: `ak74m/ak74`, `aek971/aek971`, `model98b/model_98b`) | `GunSwayData` + `CameraRecoilData` [V] | per weapon |
| `weapons/<folder>/<name>_<attachment>` (`m16a4_foregrip`, `_bipod`, `_heavybarrel`, `_silencer`, `_flash_suppressor`, `_targetpointer`, `_burst`) | `GunSwayModifierData {unlockAsset, <stance><Zoom/NoZoom>Modifier, onlyInSupportedShooting, onlyOnWeaponLightEnabled}` [V] | per weapon |
| `weapons/<folder>/<name>_customization` | `VeniceSoldierWeaponCustomizationAsset → CustomizationTable.unlockParts[] → CustomizationUnlockParts {uiCategorySid, selectableUnlocks[]}`: M16A4 slots = primary accessory {HeavyBarrel, Flashlight, Sound_Suppressor, TargetPointer, FlashSuppressor, NoSecondaryAccessory}, optics {ACOG, RX01, EOTech, IRNV, Rifle_Scope, M145, PSO-1, Kobra, PK-AS, PKS-07, PKA, Ballistic_Scope, NoOptics}, secondary rail {Foregrip, Bipod, NoSecondaryRail}, camo [V] | per weapon |
| `weapons/<folder>/<name>_1p_mesh`, `_3p_mesh`, `kitpickup*` | `SkinnedMeshAsset` + MeshSet res; pickup mesh + Havok `_physics_0_win32` [V] | per weapon |
| `weapons/common/bullets/<cal>[_carbine|_pdw|_heavy|_cold|_semi|_bolt]` | `BulletEntityData` + `WeaponSuppressionData` (+`VeniceExplosionEntityData` for `12g_frag`) — 71 bullet partitions [V] | **shared across weapons** (all 5.56 rifles use `556x45mmnato`) |
| `weapons/common/zoomlevels/*`, `weapons/common/aimingcontrollers/*` | `ZoomLevelData` (18 partitions), `SoldierAimingSimulationData` (26 partitions) [V] | shared |
| `weapons/common/defaultfirstpersoncameradata`, `*soldieraimassistdata`, `launchersway`, `ai_unlock`, `aibullets/*`, `nooptics`, `noprimaryaccessory`, `nosecondaryrail` … | camera lag, aim assist, AI-only firing data, "none" unlocks [V] | shared |
| `weapons/gadgets/m320/40mm_{he,lvg,shg,smk}[_firing]`, `40mmgrenade`, `40mm_gunsway` | UGL firing data and projectiles used by the standalone M320 and by every rifle's `u_<rifle>_m320_*` [V] | shared |
| `weapons/rpg7/rpg7projectile`, `weapons/smaw/smaw_he_projectile`, `weapons/m67/m67_projectile` | `MissileEntityData`/`GrenadeEntityData` + explosions + suppression + Havok [V] | per launcher |
| `characters/soldiers/defaultsoldierbonecollision` | `SkeletonCollisionData` (13 hit capsules) [V] | global |
| `characters/soldiers/mpsoldier` | `SoldierBlueprint`, `SoldierEntityData`, `SoldierWeaponsComponentData`, `VeniceSoldierHealthModuleData`, `SoldierSuppressionComponentData`, `BreathControlData` … [V] | global |
| `materials/materialcontainer` (partition b50615c2) | `MaterialContainerAsset {materialNames[180], materialPairs[32,400]}` + `MaterialContainerPair {flagsAndIndex, physicsMaterialIndex, physicsPropertyIndex}` [V] | global |
| `levels/<L>/<L>/materialgrid_win32/grid` | `MaterialGridData {materialIndexMap[180], interactionGrid[102×102], …}` + 399 `MaterialRelationDamageData`, 165 `MaterialRelationPenetrationData`, sounds/effects/decals (Metro) [V] | per level |
| `fx/weapons/muzzleflashes/generic/*`, `fx/weapons/shells/*`, `fx/weapons/tracers/*`, `fx/weapons/trails/*` | `EffectBlueprint`s referenced by `FireEffectData.effect`, `MeshProjectileEntityData.trailEffect` [V] | shared |
| `sound/weapons/handheld/<w>/*`, `sound/weapons/shared_content/*` | per-weapon `SoundPatchAsset` + shared layer waves [V] | layers shared |

### 1.2 The record graph (M16A3 = `weapons/m16a4/*`, all [V])

```
SoldierWeaponUnlockAsset  weapons/m16a4/u_m16a4  (identifier 2580429694, weaponIdentifier 2851853268)
 └ weapon → SoldierWeaponBlueprint "Weapons/M16A4/M16A4"  .object →
   SoldierWeaponData (: WeaponEntityData : GameEntityData)
     weaponClass wc556x45mmNATO   animBaseSet wabsRif   damageGiverName "M16A4"   renderFov 55   zoomRenderFov 15
     weaponFiring → WeaponFiringData {deployTime 0.67, altDeployTime 0.5, disableZoomOnDeployTime 0.15,
                     supportDelayProne/Stand 0.3, reloadWholeMags false, weaponSway → GunSwayData (m16a4_gunsway),
                     primaryFire → FiringFunctionData}
     aimingController → SoldierAimingSimulationData (common/aimingcontrollers/aim_default_ironsight)
                         zoomLevels → [ZoomLevelData defaultbase (FOV 55), defaultironsights (FOV 40)]
     firstPersonCamera → FirstPersonCameraData (common/defaultfirstpersoncameradata)
     hud (HudData, inline)  weaponStates[] (WeaponStateData: mesh1p/meshZoom1p/mesh3p, weapon AntRef, scope filters)
     sockets[30] → SocketData {boneName "Wep_Root", unlockAsset, availableObjects → WeaponRegularSocketObjectData}
     weaponModifierData[] = {unlockAsset, modifiers[] → WeaponModifierBase subclasses}
     customization → VeniceSoldierWeaponCustomizationAsset (m16a4_customization)
     animationData → AntPackageAsset   aiData → AIWeaponData   components [UnlockComponentData]
```

`FiringFunctionData` (all fields, schema `FiringFunctionData.yaml`; values M16A3 [V]):

| sub-record (type) | fields | M16A3 |
|---|---|---|
| `dispersion` (`FiringDispersionData[]`) | `minAngle, maxAngle, increasePerShot, decreasePerSecond` | empty on every soldier weapon dumped |
| `weaponDispersion` (`SoldierWeaponDispersion`) | `standDispersion, crouchDispersion, proneDispersion` (FiringDispersionData), `jumpDispersionAngle, proneTransitionDispersionAngle, moveDispersionAngle, moveZoomedDispersionAngle, decreasePerSecond` | all 0 (dec 5) on rifles; **shotguns use it as the pellet cone**: 870 buckshot 3°/3°, flechette 2.5°, frag 2°, slugs 0 |
| `fireEffects1p/3p` (`FireEffectData[]`) | `rotation, offset, zoomRotation, zoomOffset, effect → EffectBlueprint, useZoomOffset, useZoomRotation, disableDuringZoom, updateTransform, stopLoopingEffects` | 1p: `fx_muzz_generic_rifle_1p` at (0, 0.028, 0.99), zoom (0, −0.05, 0.99); `fx_shellejection_m` at (−0.013, 0.026, 0.44); `fx_muzz_generic_rifle_fillout_1p`; 3p equivalents |
| `sound` → `SoundAsset` | | `Sound/Weapons/Handheld/M16A3/Weapon_AssaultRifle_M16A4_02` (SoundPatchAsset) |
| `shot` (`ShotConfigData`) | `initialPosition, initialDirection, initialSpeed Vec3, inheritWeaponSpeedAmount, muzzleExplosion, projectileData → ProjectileEntityData, secondaryProjectileData, projectile/secondaryProjectile → ProjectileBlueprint, alternateProjectiles[] (UnlockableProjectile), spawnDelay, numberOfBulletsPerShell, numberOfBulletsPerShot, numberOfBulletsPerBurst, relativeTargetAiming, forceSpawnToCamera, spawnVisualAtWeaponBone, activeForceSpawnToCamera` | `initialSpeed (0,0,650)`, `projectileData → weapons/common/bullets/556x45mmnato`, 1/1/3, `activeForceSpawnToCamera true` |
| `fireLogic` (`FireLogicData`) | `holdAndRelease (HoldAndReleaseData), boltAction (BoltActionData), recoil (RecoilData), fireInputAction, reloadInputAction, cycleFireModeInputAction, triggerPullWeight, rateOfFire, rateOfFireForBurst, clientFireRateMultiplier, reloadDelay, fireLogicTypeArray[], reloadThreshold, preFireDelay, reloadTime, reloadTimeBulletsLeft, fireLogicType, reloadLogic, automaticDelay, reloadType, holdOffReloadUntilZoomRelease, forceReloadActionOnFireTrigger, holdOffReloadUntilFireRelease, alwaysAutoReload` | `fltAutomaticFire` + `[fltSingleFire]`, RPM 800/800, `clientFireRateMultiplier 0.67`, reload 1.8 (bullets left) / 2.37 (empty), `reloadThreshold 0.85`, `rtMagazineWithPossibleShorterReload`, `RecoilData` all 0 |
| `ammo` (`AmmoConfigData`) | `magazineCapacity, numberOfMagazines, traceFrequency, ammoPickupMin/MaxAmount, autoReplenishDelay, ammoBagPickupAmount, ammoBagPickupDelayMultiplier, autoReplenishMagazine` | 31 × 5, `traceFrequency 1`, `autoReplenishDelay 5` |
| `overHeat` (`OverHeatData`) | `heatPerBullet, heatDropPerSecond, overHeatPenaltyTime, overHeatThreshold, overHeatEffect` | `heatPerBullet 0` (no infantry overheat) |
| scalars | `selfHealTimeWhenDeployed, ammoCrateReloadDelay, unlimitedAmmoForAI, usePrimaryAmmo` | |

Enums [V]: `FireLogicType {fltSingleFire 0, fltSingleFireWithBoltAction 1, fltAutomaticFire 2, fltBurstFire 3,
fltHoldAndRelease 4, fltDetonatedFiring 5}`; `ReloadType {rtSingleBullet 0, rtMagazine 1,
rtMagazineWithPossibleShorterReload 2}`; `ReloadLogic {rlWeaponSwitchCancelsUnfinishedReload,
rlReloadUnaffectedByWeaponSwitch}`; `WeaponClassEnum` (calibre classes `wc556x45mmNATO`… plus `wcAssault … wcUGS`);
`WeaponAnimBaseSetEnum {wabsRif, wabsPstl, wabsHgr, wabsAT, wabsShg, wabsLMG, wabsBag, wabsSnp}`.
`fireLogicType` is the default mode and `fireLogicTypeArray` the extra modes the fire-mode key cycles through
(AEK-971 `A+[S,B]`, AN-94 `B+[A,S]`, M16A3 `A+[S]`) [V data; cycling semantics I, matches in-game mode lists].
`FireLogicData.recoil` (`RecoilData` max/min angle X/Y/Z, recoilFov) is **zero on soldier weapons**; soldier recoil is
in `GunSwayData` [V].

### 1.3 `GunSwayData` — BF3's deviation (spread) and recoil model

Layout [V schema + data]:

```
GunSwayData : WeaponSwayData
  stand  GunSwayStandData      {noZoom, zoom : GunSwayBaseMoveJumpData}
  crouch GunSwayCrouchProneData{noZoom, zoom : GunSwayBaseMoveData}
  prone  GunSwayCrouchProneData
    GunSwayBaseMoveJumpData = {baseValue, moving, jumping, sprinting, vaultingSmallObject, vaultingMediumObject
                               : GunSwayDispersionData{minAngle, maxAngle, increasePerShot, decreasePerSecond},
                               recoil : GunSwayRecoilData, gunSwayLag : GunSwayLagData}
    GunSwayBaseMoveData     = {baseValue, moving, recoil, gunSwayLag}
    GunSwayRecoilData = {recoilAmplitudeMax, recoilAmplitudeIncPerShot, horizontalRecoilAmplitudeIncPerShotMin,
                         horizontalRecoilAmplitudeIncPerShotMax, horizontalRecoilAmplitudeMax, recoilAmplitudeDecreaseFactor}
  proneToCrouch … standToCrouch : GunSwayStanceTransition {maxPenaltyValue : GunSwayDispersionData, coolDown}
  suppressionModifierUnzoomed / suppressionModifierZoomed : GunSwayStanceZoomModifierData
       {dispersionMod : GunSwayDispersionModData{minAngleModifier, maxAngleModifier, increasePerShotModifier,
        decreasePerSecondModifier}, recoilMagnitudeMod, recoilAngleMod, lagYawMod, lagPitchMod}
  modifiers[] → GunSwayModifierData (per attachment, gated by unlockAsset)
  deviationScaleFactorZoom/NoZoom, gameplayDeviationScaleFactorZoom/NoZoom (1.0 on all dumped weapons)
  shootingRecoilDecreaseScale (0.1), firstShotRecoilMultiplier, cameraRecoil → CameraRecoilData
       {springConstant 250, springDamping 0.35, springMinThresholdAngle 0} (M16A3)
```

M16A3 values [V]: stand noZoom base 2.5–7 +0.1/shot, −15/s; moving 3–7; jumping 7–7; sprinting 5–7; crouch 2.0,
prone 1.5; zoom base **0.2**–3 +0.1, moving 1.0; recoil (all states) up `0.26`/shot, horizontal `−0.4..0.1`, amplitude
caps 10/10, decrease factor 18; `firstShotRecoilMultiplier 2.5`; `shootingRecoilDecreaseScale 0.1`; stance
transitions: X→stand/crouch from prone `maxPenaltyValue.maxAngle 5.5, increasePerShot 1.8`, to prone `minAngle 7,
coolDown 1`; suppression (both zooms) `minAngleModifier 1.5, increasePerShotModifier 0.1 (sic), recoilAngleMod 1.5`.
There is **no first-shot spread multiplier** in BF3's schema (only `firstShotRecoilMultiplier`) [V schema].

### 1.4 Aiming and zoom

`SoldierAimingSimulationData {zoomLevels[], zoomTransitionTime, aimAssist, stand/crouch/pronePose AimingPoseData
{minimumPitch −70, maximumPitch 85, targetingFov, aimSteadiness, speedMultiplier, recoilMultiplier},
zoomTransitionTimeArray[] ZoomLevelSpecificTransitionTime{from, to, zoomTransitionTime, fovTransitionTime},
fovTransitionTime 0.15, fovDelayTime, modifiers[], aimingRange, lockAimToTargetSpeed, returnToZoomAfterReload}` [V].
Zoom-in time: iron 0.09 s, EOTech 0.13, 3.4× 0.08, 4× 0.14, 6×/8×/10× 0.2–0.25; zoom-out 0.15 everywhere; UGL
controllers 9999 (no ADS) [V].

`ZoomLevelData` (shared) [V]:

| level | fieldOfView | lookSpeedMul | moveSpeedMul | sway pitch/yaw mul | recoilFovMul |
|---|---|---|---|---|---|
| defaultbase (hip) | 55 | 1 | 1 | 0/0 | 1 |
| defaultironsights / defaultatsights / 1xenvg | 40 | 0.5 | 0.5 | 0.0009375/0.001875 (envg 0.0075/0.015) | 1 |
| fastironsights / fast_2_0xzoom | 40 / 32 | 0.5 / 0.32 | 0.8 | 0.0009375/0.001875 | 1 |
| 2_0x / 3_4x / 4_0x | 32 / 20 / 17.2 | 0.32 / 0.36 / 0.31 | 0.5 | 0.0009375/0.001875 | 1 |
| 6_0x / 7_0x / 8_0x / 10_0x / 12_0x / 20x | 11.6 / 9.9 / 8.7 / 7 / 5.8 / 3.5 | 0.21 / 0.21 / 0.16 / 0.13 / 0.11 / 0.08 | 0.5 | 0.00375/0.0075 | −2 |

Every level has `dispersionMultiplier 1`, `recoilMultiplier 1`; idle sway frequency `timePitchMultiplier 0.15`,
`timeYawMultiplier 0.05` on zoom levels. Optics swap the aiming controller and the 1p render FOV through modifiers:
ACOG `aim_default_4x` + `zoomRenderFov 15`; RX01 `zoomRenderFov 17`; Kobra 18; EOTech/IRNV 20; rifle scope `aim_slow_6x`
+ `enableBreathControl`; ballistic `aim_slow_10x` [V].

### 1.5 Modifiers, unlocks and variants (the resolution rule)

`SoldierWeaponData.weaponModifierData[] = WeaponModifierData {unlockAsset, modifiers[] → WeaponModifierBase}`;
modifier classes [V schema]: `WeaponFiringDataModifier {weaponFiring}` (replaces the whole WeaponFiringData),
`WeaponShotModifier {initialSpeed, numberOfBulletsPerShell (−1 = keep)}`, `WeaponProjectileModifier {projectileData}`,
`WeaponFiringEffectsModifier {fireEffects1p/3p}`, `WeaponSoundModifier {sound}`, `WeaponAimingSimulationModifier
{aimingController}`, `WeaponZoomModifier {zoomRenderFov}`, `WeaponMiscModifier {enableBreathControl,
canBeInSupportedShooting, unZoomOnBoltAction, holdBoltActionUntilZoomRelease, isSilenced}`, `WeaponMagazineModifier
{magazineCapacity, numberOfMagazines}`, `WeaponDeployTimeModifier`, `WeaponAnimTypeModifier {weaponAnimType}`,
`WeaponAnimationConfigurationModifier`, `WeaponAnimatedFireTypeModifier`, `WeaponClassModifier {weaponClass}`. Spread
and recoil changes are separate: `GunSwayData.modifiers[] → GunSwayModifierData` keyed by the same unlock.

What the M16A3 attachments do in data [V; multiplier semantics confirmed by matching Symthic's derived "M16A4" row, §2.8]:

| unlock | WeaponModifierData effect | GunSwayModifierData (× factors) |
|---|---|---|
| `U_M16A4_Sound_Suppressor` | projectile → `556x45mmnato_cold` (falloff 4→35 m instead of 8→50; suppression 4 % instead of 7 %), `initialSpeed z 380`, 1p effect `fx_muzz_generic_silencer*`, silenced sound, `isSilenced` | ADS min ×0.75, hip min ×1.4, vertical recoil ×0.9 |
| `U_M16A4_HeavyBarrel` | projectile → `556x45mmnato_heavy` (falloff end 65 m; suppression 10 %) | ADS min ×0.25, hip min ×1.2, vertical recoil ×1.15 |
| `U_M16A4_FlashSuppressor` | muzzle effect swap | hip min ×1.2, vertical recoil ×0.8 |
| `U_M16A4_Foregrip` | `WeaponAnimType_Foregrip` | ADS min ×1.5, horizontal recoil ×0.75 |
| `U_M16A4_Bipod` | `canBeInSupportedShooting`, `WeaponAnimType_Bipod` | only when supported: ADS min ×0.25 (hip ×0.123/0.138/0.158 by stance), SIPS ×0.5, vertical ×0.25, horizontal ×0.5 |
| `U_M16A4_TargetPointer` (laser) | socket objects | hip min ×0.5, only while the light/laser is on (`onlyOnWeaponLightEnabled`) |
| `U_M16A4_Burst` | `WeaponFiringDataModifier` → FFD with `fltBurstFire`+`[fltSingleFire]`, 3/burst | ADS min ×0.5, horizontal ×0.75 |
| `U_M16A4_M320_*`, `_M26_*` | `WeaponFiringDataModifier` → UGL/underslung shotgun firing data, `WeaponClassModifier wcUgl`, own aiming controller, `zoomRenderFov 35` | — |
| optics | aiming controller + `zoomRenderFov` (+ breath control on 6×+) | — |

Variants are unlock assets that switch modifiers on: `u_m16_burst` is a `SoldierWeaponUnlockAsset` on the **same**
blueprint whose `extra` → `U_M16A4_Burst` activates the burst modifiers (in-game "M16A4") [V; probe report :85-89
agrees]. `u_glock17` likewise points at the Glock18 blueprint (base data automatic 900 RPM; the semi-auto Glock 17
comes from a modifier) [V blueprint link; modifier I]. Resolution rule for the port [I, consistent with all data seen]:
`final = blueprint base`; for each active unlock U in {variant.extra, selected attachments, kit specialisation}: apply
every `WeaponModifierData` whose `unlockAsset == U` in array order (WeaponFiringDataModifier first replaces the base
WFD, then shot/projectile/effects/sound/zoom/misc/magazine overrides), then multiply the per-(stance, zoom)
dispersion/recoil by every `GunSwayModifierData` whose `unlockAsset` is active and whose gate holds.

### 1.6 Projectiles and explosions

Hierarchy [V schema]: `ProjectileEntityData : GamePhysicsEntityData {visualConvergeDistance, initialSpeed, timeToLive,
maxCount, initMeshHideTime, materialPair, hitReactionWeaponType, explosion → ExplosionEntityData, suppressionData →
WeaponSuppressionData {maxMultiplier, minMultiplier, minDistance, maxDistance}, ammunitionType,
serverProjectileDisabled, detonateOnTimeout, suppressOwnerVehicleDamage}` → `MeshProjectileEntityData
{initialAngularVelocity, trailEffect, mesh, maxAttachableInclination, extraDamping, isAttachable}` →
`BulletEntityData {stamina, flyBySound, dudExplosion, gravity, impactImpulse, detonationTimeVariation,
vehicleDetonationRadius, vehicleDetonationActivationDelay, flyBySoundRadius, flyBySoundSpeed, firstFrameTravelDistance,
distributeDamageOverTime, startDamage, endDamage, damageFalloffStartDistance, damageFalloffEndDistance,
timeToArmExplosion, hasVehicleDetonation, instantHit, stopTrailEffectOnUnspawn}`; siblings `GrenadeEntityData
{collisionSpeedMultiplier, gravity, grenadeType, minBounceSpeed}`, `MissileEntityData {gravity, engineEffect,
dudExplosion, engineStrength, maxSpeed, engineTimeToIgnition, engineTimeToLive, timeToActivateGuidingSystem, timeToArm,
maxTurnAngle, drag, nearTargetDetonation, damage, unguidedData, lockableInfo, lockingController, …}`,
`ExplosionPackEntityData` (C4, claymore, M15), `VehicleProjectileEntityData` (MAV, EOD bot, SOFLAM, T-UGS, beacon,
M224). **BulletEntityData has no drag field**; only `MissileEntityData.drag` exists (0 on RPG/SMAW) [V].

Facts that matter for the sim [V unless tagged]:

* **Muzzle velocity is `ShotConfigData.initialSpeed`** (local Vec3, z forward, y up); every bullet record carries
  `initialSpeed 350` that is overridden (fun-bots reads `shot.initialSpeed.z` as bullet speed:
  `C:\Users\olku\unfun-bots\ext\Shared\WeaponClass.lua`:198; the 44magnum record's own 460 equals the Taurus shot speed)
  [V data; override I].
* Bullets: `instantHit false` and `firstFrameTravelDistance 0` on all 71 shared bullet records; `distributeDamageOverTime 0`;
  `stamina 0`; `visualConvergeDistance 30`; `flyBySoundRadius 15` (buckshot 5); `traceFrequency 1`.
* Gravity (m/s², along world −Y via the sign): automatic bullets −15; semi/bolt/.338/crossbow −9.81; slugs −12;
  buckshot/flechette −9.81; frag rounds −15; 40 mm −7.2; RPG/SMAW −1.5; M67 −9.8.
* `timeToLive` (max range = v0 × TTL, Symthic's "max distance" column reproduces it, e.g. M16A3 975 m, M98B 3,250 m):
  1.5 s rifles/PDW/LMG/pistols, 2.0 s 9×39, 5 s semi/bolt/.338/crossbow/slug, 1 s buckshot/flechette, 3 s frag, 0 s
  40 mm (presumably "until impact" [I]), 10 s missiles.
* Shotgun pellets: `numberOfBulletsPerShell` 870 12, SPAS-12 11, M1014 10, DAO-12/Saiga 9, Jackhammer 8, USAS-12 7,
  M26 12; pellet cone from `weaponDispersion` (870 buckshot 3°).
* Explosions (`VeniceExplosionEntityData : ExplosionEntityData {blastDamage, blastRadius, innerBlastRadius,
  blastImpulse, shockwaveDamage, shockwaveRadius, shockwaveImpulse, shockwaveTime, maxOcclusionRaycastRadius,
  disableOcclusion, disableStaticEntityOcclusion, materialPair, detonationEffect, …, isCausingSuppression}`):
  RPG-7/SMAW 112 dmg, inner 0.35 m, radius 4 m, impulse 2000, shockwave 1 dmg r 10; their `dudExplosion` 56 dmg
  inner 2.1/r 4 (the "weaker second explosion" the probe could not place); 40 mm HE 112, 0.35/5 m, `timeToArmExplosion
  0.5`; M67 112, 1.5/7 m; all `disableStaticEntityOcclusion true`, `maxOcclusionRaycastRadius 0.25`.
* RPG-7/SMAW missile: shot `initialSpeed (0,0,40)` spawned at `initialPosition (−0.3,−0.1,2.0)`; `engineStrength 10`,
  `maxSpeed 100`, `engineTimeToLive 40`, `timeToArm 0.5`, `damage 225` (direct hit), `impactImpulse 5000`. M320 HE shot
  `(0,1.5,45)` at `(−0.1,−0.3,0.86)` (LVG 35 m/s). M67: shot `(0,3,18)`, `spawnDelay 0.17`, fuse = `timeToLive 3.6`,
  `detonateOnTimeout`, `collisionSpeedMultiplier 0.3`, `minBounceSpeed 0.5`, `holdAndRelease {delay 0.5,
  killedHoldingPowerModifier 0.34, forceFireWhenKilledHolding}`.
* `ShotConfigData.activeForceSpawnToCamera true` on rifles, launchers and grenades: projectiles start at the camera,
  the visual tracer at the muzzle converging over `visualConvergeDistance` 30 m [V flags; semantics I].
* Suppression per bullet (`WeaponSuppressionData` min=max, distance 3–500 m): 7 (assault/PDW/LMG 5.56/5.45/9 mm …),
  10 (7.62 auto, `_heavy` 5.56), 13 (7.62 `_heavy`), 4 (`_cold` = suppressed, `_pdw`), 20 (semi DMR), 35 (bolt,
  .338, slug, crossbow), 25 (frag), shotgun pellets 2–4 within 7–40 m, RPG 50. Symthic prints these as "suppression %"
  (7.0 %, 10.0 %, 35.0 %) [V both].

### 1.7 Hit zones and damage multipliers (where BF3 defines them)

Hit zones are **capsules on the animated skeleton**, `SoldierEntityData` → `BoneCollisionComponentData
.skeletonCollisionData` → `SkeletonCollisionData {skeletonAsset, boneCollisionData[] : BoneCollisionData {boneName,
capsuleOffset, capsuleLength, capsuleRadius, boneAxis, animationHitReactionType, materialPair, minPitch, maxPitch,
validInHiLod, validInLowLod, usePhysicsRotation, deactivateIfBehindWall}}` (`characters/soldiers/
defaultsoldierbonecollision`) [V]:

| set | bone | r / len m | offset | material pair (pmi / ppi) | notes |
|---|---|---|---|---|---|
| hi LOD | RightArm, LeftArm | 0.08 / 0.28 | 0 | 6609a12f Body (66/66) | HRT_Right/LeftArm |
| hi LOD | Spine | 0.23 / 0.10 | (0,−0.10,0) | Body (66/66) | lower torso |
| hi LOD | Spine | 0.25 / 0.00 (sphere) | (0,0.25,0) | 84454789 Body (66/**158 "Chest"**) | upper chest |
| hi LOD | Head | 0.15 / 0.01 | (−0.03,0.05,0) | 8b81bca1 Head (65/65) | HRT_Head |
| hi LOD | Right/LeftUpLeg | 0.11 / 0.40 | (0.02/0,0,0.02) | f44644a0 Body (66/**91 "Foot"**) | `deactivateIfBehindWall` |
| hi LOD | Right/LeftLeg | 0.10 / 0.43 | 0 | Foot (66/91) | `deactivateIfBehindWall` |
| lo LOD | Head + 3 × AITrajectory (r 0.35/0.30/0.35, boneAxis 1, pitch-dependent, `usePhysicsRotation`) | | | Head / Body / Chest / Foot | coarse set |

The multiplier is **not** on the soldier or the bullet: it is the level grid's relation between the projectile's
property and the capsule's property. Lookup [V by reproduction; also probe :186-204]: `pair = MaterialContainerPair`
(of `ProjectileEntityData.materialPair` and of the capsule); `row = MaterialGridData.materialIndexMap
[pair.physicsPropertyIndex & 0xFF]`; relation = `interactionGrid[rowA].items[rowB].physicsPropertyProperties[]` →
`MaterialRelationDamageData` (symmetric for soldier cells). `physicsMaterialIndex` rows carry the sound/effect/decal
relations in `physicsMaterialProperties`. Projectile properties [V]: 77 "BulletDamage" (all automatic bullets,
pistols 9 mm/.45), 85 "Sniper" (semi DMR, bolt 7.62, .357/.44, slugs, crossbow), 88 "Shotgun" (buckshot),
164 "Flechette", 166 "Magnum" (.338), 113 "MeleeDamage", 87 Missile, 116 40mm, 109 ThrowingWeapon.

`MaterialRelationDamageData.damageProtectionMultiplier` per cell (Metro [V]; the probe reports Caspian identical [RC]):

| projectile property | Head | Body (arms, lower spine) | Chest (upper-spine sphere) | Foot (legs) |
|---|---|---|---|---|
| BulletDamage (77) | **2.0** | 1.0 | 1.0 | **0.91** |
| Sniper (85) | **2.35** | 1.0 | **1.25** (threshold 2) | 0.91 |
| Magnum .338 (166) | 2.35 | 1.0 | **1.06** (threshold 2) | 0.91 |
| Shotgun buckshot (88) | **1.0** | 1.0 | 1.0 | 0.91 |
| Flechette (164) | 1.0 | 1.0 | 1.0 | 1.0 |
| MeleeDamage (113) | 0.67 | 0.5 | 0.5 | 0.5 |

Other fields in these cells: `damagePenetrationMultiplier` 0.8 on body/foot/chest for bullets (1.0 head),
`damageProtectionThreshold` 0 (2 on chest for Sniper/Magnum, 2 on melee cells), `collisionDamageMultiplier 1`,
`collisionDamageThreshold 30`, `explosionCoverDamageModifier 1`, `inflictsDemolitionDamage true`; buckshot and melee
cells also carry `MaterialRelationPenetrationData {neverPenetrate true}` [V]. Which field scales hit damage is [I]:
`damageProtectionMultiplier` reproduces the community's 2.0×/2.35× headshots and the "1.06 chest" .338 rule (probe
:138, GameFAQs [RC]); `damageProtectionThreshold` semantics unknown.

Bullets against the world (row 43 BulletDamage vs every row) [V]: multiplier 0 (threshold 2) against concrete, brick,
metal, wood, plastic, tarmac, stone, grass, sand, mud, foliage, "Destructible", "Indestructible", JeepDamage, Armored,
DamageClass_3–5, Painted …; multiplier 1 against `DamageClass_1/2` (penetration mult 0.75 on class 2), `Passthrough`,
`BulletsOnly` (0.75), `AllProjectiles`, `ChickenWire`, `CollResponseCombined` (0.75), `Light` (124),
`StationaryDamage` (demolition on). This is the "bullets only break light props" rule handoff 04 §5.4 had to guess.

Penetration [V data; rule I]: `MaterialContainerPair.flagsAndIndex` top byte bit `0x01` = Penetrable (handoff 04 §3.3,
`04-gameplay-sim.md`:131); it is set on pairs whose property is Head/Body/Chest/Foot, `Passthrough`, `ChickenWire` and
`AllProjectiles` (flag 9 = penetrable + see-through), `BulletsOnly`, `DmgClass3`, `DamageClass_0` (1,520 of 32,400
pairs). `neverPenetrate true` appears in 1,317 cells, all for explosive/melee/rocket/grenade/buckshot properties; 10
cells say `false`. So a bullet passes through chicken-wire, pass-through fences and similar, never through walls; whether
it continues through a soldier (soldier pairs are flagged penetrable, `damagePenetrationMultiplier 0.8`) is not
determined [I].

### 1.8 Soldier-side numbers that weapons interact with (`characters/soldiers/mpsoldier`) [V]

`SoldierEntityData.maxHealth 100`, `lowHealthThreshold 30`, `explosionDamageModifier 1`; `VeniceSoldierHealthModuleData
{regenerationDelay 10, regenerationRate 5, immortalTimeAfterSpawn 2, manDownStateTime 13, postReviveHealth 20,
timeForCorpse 15}`; `SoldierSuppressionComponentData {reactionToSuppression {suppressionHighThreshold 0.4,
suppressionLowThreshold 0.3, suppressionUIThreshold 0.15}, suppressionDecay 0.5, suppressionSphereRadius 1.5,
fallOffDelay 2, fallOffMultiplier 1, suppressionAbortsHealthRegeneration true, checkLineOfSight false}`;
`BreathControlData {breathControlTime 4, releaseTime 3, penaltyMultiplier 3, penaltyTimeout 3}`;
`SoldierWeaponsComponentData.animatedWeaponBinding` = the Ant signal set the weapon drives (Deploy, AltDeploy,
Undeploy, QuickSwitch, Reload, ReloadShotgun, Fire, FireSingle, FireHoldAndRelease, FirstShotSpawned, BoltAction,
PumpAction, MeleeAttack, QuickThrow, Zoom, ZoomParameter, Dispersion, FireModeChanged, NumberOfBulletsLeftInGun,
KickBackInIronSight, WeaponClassSignal, OffsetX/Y/Z …); `SyncedGameSettings` (level `settings_win32`)
`allowClientSideDamageArbitration true`, `maxAllowedLatency 0.85` (networking topic `05-networking.md`:65-66).

### 1.9 Weapon roster (base data of each blueprint before unlock modifiers)

Source: `scratchpad\weapons\roster.tsv` built from the Metro dump (one row per `SoldierWeaponBlueprint`, its shortest
`U_` asset; 90 blueprints, 172 `SoldierWeaponUnlockAsset`s) [V]; grouping by kit/class from fun-bots
(`C:\Users\olku\unfun-bots\ext\Shared\WeaponList.lua`:33-332) [V file; fun-bots' classing]. "pack" = folder prefix
(`XP1_` Back to Karkand, `XP2_` Close Quarters, `XP4_` Aftermath); **no `XP3_`/`XP5_` weapon folder exists** — Armored
Kill and End Game added no infantry weapon partitions [V for folder names; DLC attribution of base-folder weapons such as
DAO-12/M93R/G18 pre-order items not determined]. Columns: modes `A` auto, `S` single, `B` burst, `+[…]` extra modes;
damage `max>min @ falloff start–end m`; spread/recoil = stand, Zoom (ADS) row except "hip min" = stand NoZoom;
horizontal recoil `Min..Max` in data sign (§2.2: Max = left, −Min = right). Rows show the base firing data: the
Glock17 row is the Glock18 blueprint (semi-auto via modifier), M4 = M4A1 blueprint.

**Assault (Assault kit)**

|weapon (U_ asset)|pack|WeaponClass|modes|RPM|v0 m/s|mag|reload tac/empty s|bullet, damage|g|ADS min / hip min +SIPS|recoil up / horiz|FSRM|rec. dec.|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|AEK971|base|545x45mmWP|A+S/B|900|580|31|2.55/3.68|545x45mmwp 25>18.4 @8-50|-15|0.3/2.5 +0.1|0.2 / -0.3..0.5|3|18|
|AK74M|base|545x45mmWP|A+S|650|600|31|2.1/3.1|545x45mmwp 25>18.4 @8-50|-15|0.2/2.5 +0.1|0.28 / -0.3..0.2|1.5|18|
|AN94|base|545x45mmWP|B(2 @1200)+A/S|600|600|31|2.45/3.3|545x45mmwp 25>18.4 @8-50|-15|0.2/2.5 +0.1|0.3 / -0.3..0.2|1.5|18|
|F2000|base|556x45mmNATO|A+S|850|600|31|2.5/3.5|556x45mmnato 25>18.4 @8-50|-15|0.4/2 +0.1|0.26 / -0.4..0.5|3|18|
|FAMAS|B2K|556x45mmNATO|A+S/B|1000|630|26|3.05/3.9|556x45mmnato 25>18.4 @8-50|-15|0.4/2.5 +0.1|0.35 / -0.55..0.35|2.6|18|
|KH2002|base|556x45mmNATO|B+S|800|650|31|2.9/3.4|556x45mmnato 25>18.4 @8-50|-15|0.2/2 +0.1|0.2 / -0.4..0.4|1.5|18|
|L85A2|B2K|556x45mmNATO|A+S|650|600|31|2.8/3.35|556x45mmnato 25>18.4 @8-50|-15|0.2/2 +0.1|0.2 / -0.28..0.28|2.5|18|
|M16A4 (=M16A3)|base|556x45mmNATO|A+S|800|650|31|1.8/2.37|556x45mmnato 25>18.4 @8-50|-15|0.2/2.5 +0.1|0.26 / -0.4..0.1|2.5|18|
|M416|base|556x45mmNATO|A+S|750|600|31|1.75/2.5|556x45mmnato 25>18.4 @8-50|-15|0.2/2.5 +0.1|0.26 / -0.3..0.1|2|18|
|SCAR-L|CQ|556x45mmNATO|A+S|620|580|31|2.35/3|556x45mmnato 25>18.4 @8-50|-15|0.2/2.5 +0.1|0.2 / -0.225..0.225|2.75|18|
|SteyrAug|CQ|556x45mmNATO|A+S|700|670|31|2.5/2.9|556x45mmnato 25>18.4 @8-50|-15|0.2/2 +0.1|0.2 / -0.4..0.3|2.6|18|
|G3A3|base|762x51mmNATO|A+S|550|500|21|2/3.3|762x51mmnato 34>22 @8-60|-15|0.1/3.5 +0.12|0.45 / -0.2..0.2|1.4|15|
|M4 (M4A1 blueprint)|base|556x45mmNATO|A+S|800|580|31|1.85/2.48|556x45mmnato_carbine 25>14.3 @8-50|-15|0.4/2 +0.1|0.26 / -0.4..0.1|2.8|18|

**Carbines (Engineer)**

|weapon|pack|WeaponClass|modes|RPM|v0|mag|reload|bullet, damage|g|ADS / hip +SIPS|recoil|FSRM|dec|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|AKS74u|base|545x45mmWP|A+S|650|440|31|1.9/3|545x45mmwp_carbine 25>14.3 @8-50|-15|0.4/2 +0.1|0.28 / -0.3..0.2|1.5|18|
|A91|base|556x45mmNATO|A+S|800|420|31|2.62/3.3|556x45mmnato_carbine 25>14.3 @8-50|-15|0.4/1.5 +0.1|0.2 / -0.5..0.5|3|18|
|ACR|CQ|556x45mmNATO|A+S|850|500|27|1.85/2.5|65x39mm_grendel_carbine 20>16.7 @8-50|-15|0.3/2.25 +0.1|0.2 / -0.3..0.1|2.5|18|
|G36C|base|556x45mmNATO|A+S/B|750|500|31|1.9/2.85|556x45mmnato_carbine 25>14.3 @8-50|-15|0.4/2 +0.1|0.28 / -0.3..0.3|1.8|18|
|HK53 (G53)|B2K|556x45mmNATO|A+S/B|750|450|31|2.3/3.2|556x45mmnato_carbine 25>14.3 @8-50|-15|0.4/2 +0.1|0.24 / -0.35..0.2|2.5|18|
|MTAR|CQ|556x45mmNATO|A+S|900|570|31|3/3.3|556x45mmnato_carbine 25>14.3 @8-50|-15|0.5/1.5 +0.1|0.3 / -0.5..0.25|2.5|18|
|SG553LB|base|556x45mmNATO|A+S/B|700|430|31|2.1/2.65|556x45mmnato_carbine 25>14.3 @8-50|-15|0.4/2 +0.1|0.25 / -0.4..0.2|2.2|18|
|QBZ-95B|B2K|58x42mm|A+S/B|650|490|31|2.3/2.9|58x42mmdap87_carbine 25>14.3 @8-50|-15|0.4/1.5 +0.1|0.2 / -0.3..0.3|2.5|18|
|SCAR-H|base|762x51mmNATO|A+S|600|420|21|2.1/2.75|762x51mmnato_carbine 30>20 @8-50|-15|0.3/2.5 +0.12|0.5 / -0.2..0.2|1.35|13|

**LMG (Support)**

|weapon|pack|WeaponClass|modes|RPM|v0|mag|reload|bullet, damage|g|ADS / hip +SIPS|recoil|FSRM|dec|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|M249|base|556x45mmNATO|A|800|620|100|-/6.2|556x45mmnato 25>18.4 @8-50|-15|0.5/5 +0.1|0.4 / -0.4..0.4|2.2|12|
|LSAT|CQ|556x45mmNATO|A|650|620|100|-/5.6|556x45mmnato 25>18.4 @8-50|-15|0.4/4 +0.1|0.4 / -0.35..0.35|2.1|15|
|MG36|B2K|556x45mmNATO|A+S/B|750|550|51|2.8/4.6|556x45mmnato 25>18.4 @8-50|-15|0.4/3.5 +0.1|0.4 / -0.3..0.1|1.8|13|
|QBB-95|B2K|58x42mm|A+S/B|650|670|76|3.8/4.5|58x42mmdap87 25>18.4 @8-50|-15|0.4/3 +0.1|0.37 / -0.2..0.3|2|13|
|Type88|base|58x42mm|A|650|600|100|3.6/7.4|58x42mmdap87 25>18.4 @8-50|-15|0.5/5 +0.1|0.36 / -0.3..0.3|1.5|12|
|RPK-74M|base|545x45mmWP|A+S|700|600|46|3/4|545x45mmwp 25>18.4 @8-50|-15|0.4/3.5 +0.1|0.3 / -0.2..0.2|1.8|13|
|L86|CQ|556x45mmNATO|A+S|750|640|46|2.8/3.3|556x45mmnato 25>18.4 @8-50|-15|0.4/3 +0.1|0.32 / -0.25..0.15|2|13|
|M27IAR|base|556x45mmNATO|A+S|750|650|46|1.9/2.5|556x45mmnato 25>18.4 @8-50|-15|0.4/3.5 +0.1|0.35 / -0.3..0.1|2.2|13|
|M60|base|762x51mmNATO|A|580|560|100|-/7.2|762x51mmnato 34>22 @8-60|-15|0.5/5 +0.12|0.6 / -0.2..0.2|1.5|12|
|M240|base|762x51mmNATO|A|650|610|100|-/6.2|762x51mmnato 34>22 @8-60|-15|0.5/5 +0.12|0.7 / -0.5..0.5|1.7|12|
|Pecheneg (PKP)|base|762x54mmR|A|600|560|100|-/5.38|762x54mmr 34>22 @8-60|-15|0.5/5 +0.12|0.65 / -0.4..0.4|1.5|13|

**Sniper / DMR (Recon)** — bolt-action RPM is the cap; the real interval is `boltActionTime` (M98B 1.3 s ⇒ 46.2 RPM,
Symthic's figure) [V].

|weapon|pack|WeaponClass|modes|RPM|v0|mag|reload|bullet, damage|g|ADS / hip +SIPS|recoil|FSRM|dec|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|QBU-88|B2K|58x42mm|S|260|550|11|2.7/3.6|58x42mmdap88_semi 50>37.5 @15-75|-9.81|0.025/3 +0.8|1.5 / -0.3..0.3|1|6|
|SKS|base|762x39mmWP|S|333|440|21|2.2/3.4|762x39mmwp_semi 43>27 @15-60|-15|0.1/2.5 +0.3|0.55 / -0.4..0.3|1|9|
|HK417 (M417)|CQ|762x51mmNATO|S|295|450|21|1.8/2.4|762x51mmnato_semi 50>37.5 @15-75|-9.81|0.05/3 +0.55|1.15 / -0.25..-0.05|1|10|
|M39EBR|base|762x51mmNATO|S|300|570|11|2.15/3.4|762x51mmnato_semi 50>37.5 @15-75|-9.81|0.0167/3.5 +0.8|1.2 / -0.2..0.2|1|6|
|MK11|base|762x51mmNATO|S|260|550|11|1.85/2.4|762x51mmnato_semi 50>37.5 @15-75|-9.81|0.0167/3.5 +0.8|1.5 / -0.3..-0.1|1|6|
|SVD|base|762x54mmR|S|260|530|11|2.15/3.3|762x54mmr_semi 50>37.5 @15-75|-9.81|0.0167/3.5 +0.8|1.5 / -0.3..-0.1|1|6|
|JNG90|CQ|338Magnum|Bolt|360|560|11|2.5/4.6|762x51mmnato_bolt 80>59 @15-100|-9.81|0/5 +0|0.5 / 0..0|1.5|6|
|M98B|base|338Magnum|Bolt|360|650|6|2.9/4.7|338magnum 95>59 @15-150|-9.81|0/5 +0|0.5 / 0..0|1|6|
|L96|B2K|762x51mmNATO|Bolt|360|540|11|2.6/4.8|762x51mmnato_bolt 80>59 @15-100|-9.81|0/5 +0|0.5 / 0..0|1|6|
|M40A5|base|762x51mmNATO|Bolt|360|490|11|2.7/4|762x51mmnato_bolt 80>59 @15-100|-9.81|0/4 +0|0.5 / 0..0|1|6|
|SV98|base|762x54mmR|Bolt|360|520|11|2.4/4.8|762x54mmr_bolt 80>50 @20-80|-9.81|0/3.5 +0|0.5 / 0..0|1|6|

**PDW (all kits; crossbow Aftermath)**

|weapon|pack|WeaponClass|modes|RPM|v0|mag|reload|bullet, damage|g|ADS / hip +SIPS|recoil|FSRM|dec|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|UMP45|base|45cal|A+S/B|600|320|26|1.85/2.7|45cal 34>12.5 @8-40|-15|0.5/1 +0.1|0.25 / -0.2..0.2|2.75|20|
|MP7|base|46x30mm|A+S|950|390|21|2.4/2.6|46x30mm 20>11.2 @12-46|-15|0.5/1 +0.06|0.12 / -0.45..0.45|2.2|20|
|MagpulPDR|base|556x45mmNATO|A+S/B|750|430|21|2.5/3.4|556x45mmnato_carbine 25>14.3 @8-50|-15|0.4/1.5 +0.1|0.2 / -0.4..0.2|2|18|
|P90|base|57x28mm|A+S|900|420|51|2.5/3|57x28mm 20>11.2 @12-46|-15|0.5/1 +0.06|0.18 / -0.4..0.4|2|20|
|MP5K|CQ|9x19mm|A+S/B|900|380|21|2.2/3|9x19mm 25>13.75 @12-50|-15|0.6/1.2 +0.1|0.25 / -0.48..0.48|1.55|30|
|PP-19|B2K|9x19mm|A+S|900|320|55|2.4/3.8|9x18mm_makarov 16.7>12.5 @12-40|-15|0.5/1 +0.06|0.2 / -0.3..0.3|1.5|20|
|PP2000|base|9x19mm|A+S|650|360|21|2.45/3.45|9x19mm 25>13.75 @12-50|-15|0.5/1 +0.06|0.18 / -0.2..0.3|2.5|20|
|ASVal|base|9x39mm|A+S|900|333|21|1.7/2.85|9x39mmsp6 20>18.4 @6-40|-15|0/2.5 +0.1|0.4 / -0.3..0.2|0.5|18|
|Crossbow (Scoped Cobra)|AM|None|Bolt|360|100|1|2/3.7|crossbow_bolt 100>60 @20-50 (also _ba, _he, _scan bolts)|-9.81|0/1 +0|0.2 / -0.335..0.335|1|18|

**Shotguns (all kits)** — v0 shows `×pellets`; 870/SPAS/DAO/M1014 reload per shell (`rtSingleBullet`).

|weapon|pack|WeaponClass|modes|RPM|v0|mag|reload|bullet, damage|g|ADS / hip +SIPS|recoil|FSRM|dec|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|870|base|12gauge|Bolt (pump 0.48 s)|600|300x12|5|-/0.67 per shell (+1.15 delay)|12g_buckshot 18>6 @8-25|-9.81|0.1/1.5 +0.6|1 / -0.3..-0.1|1|10|
|SPAS12|CQ|12gauge|Bolt|600|300x11|5|-/0.67|12g_buckshot 18>6 @8-25|-9.81|0.1/1.5 +0.6|1 / -0.3..-0.1|1|10|
|DAO-12|base|12gauge|S|220|300x9|8|-/0.69|12g_buckshot 18>6 @8-25|-9.81|0.4/1.5 +0.6|1.2 / -0.3..-0.1|1|15|
|Jackhammer|B2K|12gauge|A|255|300x8|8|3.45/4.1|12g_buckshot 18>6 @8-25|-9.81|0.6/1.5 +0.6|1.5 / -0.3..-0.1|1.5|6|
|M1014|base|12gauge|S|220|300x10|5|-/0.55|12g_buckshot 18>6 @8-25|-9.81|0.4/1.5 +0.6|1.2 / -0.3..-0.1|1|15|
|SAIGA_20K|base|12gauge|S|220|300x9|7|2.2/3.5|12g_buckshot 18>6 @8-25|-9.81|0.4/1.5 +0.6|0.8 / -0.2..0.2|1|15|
|USAS-12|base|12gauge|A+S|275|300x7|7|2.5/3.45|12g_buckshot 18>6 @8-25|-9.81|0.6/1.5 +0.6|1.5 / -0.4..0.4|2|6|

Ammo variants [V]: flechette 12.5>8.4 @14-30 (v 450 on 870), slug 75>37.5 @15-50 g −12 (1 pellet), frag 20>5 @8-40
with explosion (v 150–200).

**Pistols (all kits)**

|weapon|pack|WeaponClass|modes|RPM|v0|mag|reload|bullet, damage|g|ADS / hip +SIPS|recoil|FSRM|dec|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|M9|base|9x19mm|S|400|380|16|1.35/1.6|9x19mm_pistol 25>13.75 @12-50|-15|0.5/1 +0.2|0.7 / -0.1..0.1|1|20|
|MP443|base|9x19mm|S|400|320|18|1.35/1.6|9x19mm_pistol 25>13.75 @12-50|-15|0.5/1 +0.2|0.7 / -0.1..0.1|1|20|
|M1911|base|45cal|S|333|300|9|1.75/2.05|45cal_pistol 34>14.3 @12-50|-15|0.5/1 +0.2|0.7 / -0.1..0.1|1|20|
|Glock18 (and G17 via modifier)|base|9x19mm|A+S|900|375|20|1.35/1.6|9x19mm_autopistol 20>12.5 @8-40|-15|0.8/1.5 +0.5|0.7 / -0.4..0.4|2.4|30|
|M93R|base|9x19mm|B+S|900|380|21|1.35/1.6|9x19mm_autopistol 20>12.5 @8-40|-15|0.8/1.5 +0.5|0.5 / -0.2..0.2|1.5|30|
|MP412Rex|base|357Magnum|S|255|370|6|-/3.4|357magnum 50>28 @15-37|-15|0.3/1.25 +0.3|1.2 / -0.2..0.2|1|10|
|Taurus44|base|44Magnum|S|160|460|6|-/3.9|44magnum 60>30 @12-50|-15|0.3/1 +0.3|2 / -0.2..0.2|1|4|

**Gadgets, launchers, grenades, melee** (`WeaponClass` / fire logic / projectile type) [V]: RPG-7 and SMAW (`wcAt`,
missiles above), FGM-148 Javelin and FIM-92 Stinger / SA-18 Igla (`wcAt`, `MissileEntityData`, locking), M320 HE/LVG/
SHG/SMK (`wcUgl`) and GP-30 (`weapons/gadgets/gp-30`), M26 MASS (`wc12gauge`, 6 rounds, 12 pellets), M67 (`wcHgr`,
`fltHoldAndRelease`), knife and XP2 razor knife (`wcKnife`, `BulletEntityData` 100 dmg, `instantHit true`, shot speed 1.8; melee cells
×0.5 body), C4 (`wcC4`, `fltDetonatedFiring`, `ExplosionPackEntityData`), claymore and M15 AT mine (`wcATMine`), medic
bag and ammo bag (`SupplySphereEntityData`), defibrillator, repair tool (`wcPowerTool`, 6 dmg bullet), MAV, EOD bot,
SOFLAM, T-UGS, radio beacon, M224 mortar (`VehicleProjectileEntityData`). Their partitions are under
`weapons/gadgets/<name>/` and `weapons/{rpg7,smaw,fgm148,fim92a,sa18igla,m67,knife,xp2_knife_razorblade}/`.

## 2. Behaviour (with test oracles)

### 2.1 Spread (deviation)

Model (Symthic tutorial 3.2 [RC], structure [V]): the bullet leaves in a random direction inside a cone of half-angle
`spread` around the (recoil-displaced) aim direction. `spread` starts at `minAngle` of the current state
(stance × zoom × {base, moving, jumping, sprinting, vaulting}), grows by `increasePerShot` after each shot up to
`maxAngle`, and decays at `decreasePerSecond` (15°/s on every rifle) back to `minAngle`. Symthic confirms
"spread has been confirmed to be generated as we assume" and "spread decrease seems to work about as we thought"
(thread 2049, post 2) [RC]. Examples (stand, ADS min → after n shots): M16A3 0.2° → 0.2+0.1·n capped 3°; M249 0.5 →
+0.1; M98B 0 → +0 (no ADS bloom; hip 5° +1.2/shot); 870 ADS 0.1 +0.6 and the pellet cone 3° added per pellet [V data].
Moving ADS min is 1.0 on M16A3 (5× the stationary 0.2), "moving spread is never really reached" (tutorial note
linking a test) [RC]. Suppression multiplies min spread by `minAngleModifier` and horizontal recoil by `recoilAngleMod`:
1.5/1.5 on the rifles, carbines, LMGs and PDWs tabulated, 2.0/1.5 on bolt-actions, 0.5/1.5 on the 870 and USAS-12,
0/1 on the M9 (0 probably "unset") [V data; trigger threshold I, likely `suppressionLowThreshold`].
Stance changes add `GunSwayStanceTransition.maxPenaltyValue` (to stand/crouch from prone: up to 5.5°, +1.8 per shot;
cool-down 0–1 s) [V data; exact application I]. Jumping 7°, sprinting 4–5° (ADS) [V].

### 2.2 Recoil

Per shot [V data; Symthic 3.1 RC]: pitch kick `recoilAmplitudeIncPerShot` (deg, deterministic) × `firstShotRecoilMultiplier`
on the first shot; yaw kick uniform in `[horizontalRecoilAmplitudeIncPerShotMin, …Max]`; accumulations capped at
`recoilAmplitudeMax`/`horizontalRecoilAmplitudeMax` (10°/10°). **Sign**: Symthic's "left" equals data `Max` and its
"right" equals `−Min` on all 50 matching weapons (M16A3: data −0.4..0.1 ↔ Symthic left 0.1 / right 0.4; AEK-971 −0.3..0.5
↔ left 0.5 / right 0.3) [V]. Frostbite +X is the soldier's left (handoff §5: `iw = (fb.z, fb.x, fb.y)` maps fb +X to
IW +Y = left), so a positive value is a right-hand-rule yaw about +Y = turn left [I, consistent]. Recovery
(`recoilAmplitudeDecreaseFactor` 18 on rifles, 6 on bolt-action ADS, 12–13 LMG, 20–30 pistols/PDW): Symthic models
it as "how much recoil is decreased per second … only apparent when you are not firing … could be not completely
right" [RC]; `shootingRecoilDecreaseScale 0.1` suggests recovery continues at 10 % while firing [I]. Thread 2049:
horizontal recoil "is not independent random draws" and symmetric recoil handles worse than asymmetric — unresolved
by the community [RC]. Visual camera kick is a spring (`CameraRecoilData` k 250, damping 0.35) separate from aim
recoil [V data; I].

### 2.3 Ballistics

Projectile with travel time, constant gravity, **no drag** (no drag field on bullets; Symthic "range only limited by
max range", max distance = v0 × TTL) [V schema/data, RC]. Drop oracle (v0 650 m/s, g 15, no drag): 100 m → t 0.154 s,
drop 0.178 m; 300 m → 0.462 s, 1.60 m; M98B (650, g 9.81) 300 m → 1.05 m, 1,000 m → 11.6 m [I arithmetic].
fun-bots compensates with `0.5·t²·(−gravity)` (`ext\Server\Bot\BotAiming.lua`:221-235) [V code].

### 2.4 Damage

`damage(d) = startDamage` for `d ≤ damageFalloffStartDistance`, linear to `endDamage` at `damageFalloffEndDistance`,
flat beyond (Symthic 1.7: "flat at the maximum 25 until 8 meters, then a linear dropping off until 50 meters where it
reach its minimum value 18.4") [RC; data V]. × hit-zone multiplier (§1.7) × friendly-fire/game settings
(`SyncedGameSettings.bulletDamageModifier`, `ServerSettings.humanHealthMultiplier` exist) [V schema]. Shots to kill
(100 hp): M16A3 4 body / 2 head ≤ 8 m, 6 body / 3 head ≥ 50 m (18.4×6 = 110); .338 headshot 223 (OHK at any range);
.338 upper chest 95×1.06 = 100.7 ⇒ OHK only while damage ≥ 94.4, i.e. ≤ 17.5 m; 7.62 bolt 80×1.25 = 100 ⇒ upper-chest
OHK ≤ 15 m (SV98 ≤ 20 m); DMR head 50×2.35 = 117.5 (OHK ≤ 15 m, 37.5×2.35 = 88 beyond 75 m); legs ×0.91 [I from V
data]. Explosions: `blastDamage` inside `innerBlastRadius`, falling to 0 at `blastRadius` (shape not in data, linear
assumed) [I]; occlusion rays (`maxOcclusionRaycastRadius 0.25`) [V field].

### 2.5 Rate of fire, bursts, bolt, reload

Interval = 60/`rateOfFire`; burst = `numberOfBulletsPerBurst` at `rateOfFireForBurst` (AN-94 2 @ 1200, Symthic "600 RPM
(Burst 1200)") [V]; bolt/pump: next shot after `boltActionDelay + boltActionTime` (data: delay 0 and time M98B/JNG-90 1.3,
M40A5 1.1, L96 1.38, SV98 1.25 s ⇒ 46.2/54.5/43.5/48 RPM, exactly Symthic's figures; 870 pump 0.48 s ⇒ 125 RPM)
[V cross-check]; `unZoomOnBoltAction`,
`returnToZoomAfterBoltAction`, `holdBoltActionUntilFireRelease/ZoomRelease` gate the cycle [V fields].
`clientFireRateMultiplier 0.67` on every weapon and `ClientSettings.ignoreClientFireRateMultiplier` exist; meaning
not determined (server tolerance for client fire timing is my best guess) [V fields; I]. Reload: `rtMagazine…Shorter
Reload` uses `reloadTimeBulletsLeft` when rounds remain, `reloadTime` when empty; `reloadThreshold` (0.85) = point after
which the reload is committed (Symthic: "unable to change weapons until the threshold") [RC]; `rtSingleBullet`
(870) = `reloadDelay` 1.15 s then `reloadTime` 0.67 s per shell [V fields; timing composition I].
`magazineCapacity 31` = 30 + chambered round (Symthic 1.9) [RC]; whether an empty reload yields 30 is not in data.

### 2.6 Suppression

A passing bullet adds its `WeaponSuppressionData` multiplier (7/10/20/35/50, read by Symthic as percent) when it
passes within `suppressionSphereRadius 1.5 m` and its distance from the shooter is within `minDistance..maxDistance`;
the level decays at `suppressionDecay 0.5`/s after `fallOffDelay 2 s`; `suppressionLowThreshold 0.3`/`High 0.4`
drive the reaction (blur, sway) and `suppressionAbortsHealthRegeneration` [V fields; accumulation formula I]. Effect on
the victim's weapon: §2.1.

### 2.7 Numbers table for the requested weapons

Sources: **D** = retail data (this dump; probe agrees), **S** = Symthic Aftermath chart (archived), = means S equals D.

| weapon | RPM | dmg max>min | falloff m | v0 m/s | g | mag/reload | ADS min (+SIPS) / hip min | recoil up, L/R, FSRM, dec | src |
|---|---|---|---|---|---|---|---|---|---|
| M16A3 | 800 (A/S) | 25>18.4 | 8–50 | 650 | −15 | 31, 1.8/2.37 | 0.2 (+0.1) / 2.5 | 0.26, 0.1/0.4, 2.5, 18 | D, S= |
| AK-74M | 650 | 25>18.4 | 8–50 | 600 | −15 | 31, 2.1/3.1 | 0.2 / 2.5 | 0.28, 0.2/0.3, 1.5, 18 | D, S= |
| M4A1 | 800 | 25>14.3 | 8–50 | 580 | −15 | 31, 1.85/2.48 | 0.4 / 2.0 | 0.26, 0.1/0.4, 2.8, 18 | D, S= |
| AEK-971 | 900 | 25>18.4 | 8–50 | 580 | −15 | 31, 2.55/3.68 | 0.3 / 2.5 | 0.2, 0.5/0.3, 3.0, 18 | D, S= |
| M249 | 800 | 25>18.4 | 8–50 | 620 | −15 | 100, —/6.2 | 0.5 / 5.0 | 0.4, 0.4/0.4, 2.2, 12 | D, S= |
| PKP | 600 | 34>22 | 8–60 | 560 | −15 | 100, —/5.38 | 0.5 (+0.12) / 5.0 | 0.65, 0.4/0.4, 1.5, 13 | D, S= |
| M98B | 46.2 eff. (bolt 1.3 s) | 95>59 | 15–150 | 650 | −9.81 | 6, 2.9/4.7 | 0 / 5.0 (+1.2 hip) | ADS 0.5 / hip 2.0, 0/0, 1.0, 6 (hip 15) | D; S shows hip recoil |
| SV98 | 48 eff. | 80>50 | 20–80 | 520 | −9.81 | 11, 2.4/4.8 | 0 / 3.5 | as M98B | D; S= (hip values) |
| M9 | 400 | 25>13.75 | 12–50 | 380 | −15 | 16, 1.35/1.6 | 0.5 (+0.2) / 1.0 | 0.7, 0.1/0.1, 1.0, 20 | D, S= |
| 870MCS | 600 cap, pump 0.48 s | 12 × 18>6 | 8–25 | 300 | −9.81 | 5, 1.15 + 0.67/shell | 0.1 (+0.6), pellet cone 3° | 1.0, −0.3..−0.1 (both right), 1.0, 10 | D (not in S chart) |
| UMP-45 (PDW) | 600 | 34>12.5 | 8–40 | 320 | −15 | 26, 1.85/2.7 | 0.5 (+0.1; S 0.06) / 1.0 | 0.25, 0.2/0.2, 2.75, 20 | D; S differs on SIPS |
| MP7 / P90 (PDW) | 950 / 900 | 20>11.2 | 12–46 | 390 / 420 | −15 | 21 / 51 | 0.5 (+0.06) / 1.0 | 0.12 / 0.18, 0.45 / 0.4 sym., 2.2 / 2.0, 20 | D, S= |
| RPG-7 | 100 (1 rd) | direct 225, blast 112 | inner 0.35, r 4 | 40 → 100 (engine 10) | −1.5 | 1+4, 4.8 | — | 2.5 up, −1..−0.5 | D |
| SMAW | 350 (1 rd) | as RPG-7 | as RPG | 40 → 100 | −1.5 | 1+4, 5.8 | — | as RPG | D |
| M320 HE | 250 | impact 100, blast 112 | inner 0.35, r 5; arm 0.5 s | 45 (+1.5 up) | −7.2 | 1+4, 2.9 | — | 2.5 | D |
| M67 | hold & release | blast 112 | inner 1.5, r 7 | 18 (+3 up), fuse 3.6 s | −9.8 | 1+1 | — | — | D |

### 2.8 Where Symthic (Dec 2012) and the install differ (12 of 62 rows)

G17 (our row is the Glock18 blueprint base, modifier not applied); five bolt-actions (Symthic prints hip recoil 2.0/
dec 15/SIPS 1.2 = the `NoZoom` block, and effective RPM); SKS dec 15 (= NoZoom); M4 and "M16A4" burst (our base rows
omit the burst/variant `GunSwayModifierData`: ×0.5 ADS min and ×0.75 horizontal reproduce Symthic's 0.1 and 0.075/0.3
exactly, which verifies the modifier semantics); LSAT ADS 0.5 vs 0.4, UMP-45 SIPS 0.06 vs 0.1, 93R SIPS 0.2 vs 0.5
(changes after Aftermath or chart errors; the install is the final patch) [V comparison].

## 3. Hit registration

What BF3 did:

* Client-side damage arbitration: `SyncedGameSettings.allowClientSideDamageArbitration = true` in the shipped level
  settings, `maxAllowedLatency 0.85` s (networking topic `05-networking.md`:65-66, reading `settings_win32`) [V via that
  report]; VU's server `DamageInfo` carries `isClientDamage` and `boneIndex`, `damagedMaterial`, `collidingMaterial`
  (`C:\Users\olku\VU-Docs\types\server\type\DamageInfo.yaml`) [V doc].
* The server still simulates a `ServerBulletEntity` per shot (VU server hook `BulletEntity:Collision(entity, hit,
  giverInfo)`, `VU-Docs\content\vext\ref\server\hook\bulletentity_collision.md`) [V doc]. fun-bots' server-side bots
  have no client, so it flips `allowClientSideDamageArbitration` off around a bot's bullet collision so the server's own
  hit applies (`unfun-bots\ext\Server\__init__.lua`:595-614, 717-724); turning it off globally is labelled "Use worse
  hitboxes" (`ext\Shared\Registry\Registry.lua`:22-23) [V code]. Together with the hi/lo-LOD capsule sets (§1.7) and
  `ServerSettings.isSoldierDetailedCollisionEnabled` / `GameSettings.useSpeedBasedDetailedCollision`, this reads as:
  clients hit the detailed hi-LOD capsules, the server uses coarse lo-LOD ones unless detailed collision is on [I].
* Server tick 30 Hz (`GameTimeSettings.MaxSimFps 30`, `05-networking.md`:51; VU "For 30Hz it returns `regular`",
  `VU-Docs\content\hosting\commands.md`:75; VU adds 60/120 Hz modes, `content\hosting\config.md`:85-95) [V doc].
* Consequences the community reported: "shot behind cover", trades (two players killing each other "at the same time")
  [RC: overclock.net thread 1182370]. BF4 later: December 2013 PC patch "The time window where a bullet could cause
  damage from an already dead player is now calculated as intended" (patch notes reproduced by MP1st/Destructoid) [RC];
  DICE "Addressing Netcode in Battlefield 4" explains client-predicted vs server-confirmed health on kill cards and
  fixes to "latency compensation" (https://battlelog.battlefield.com/bf4/news/view/addressing-netcode-in-bf4/, read)
  [V page]; the 2014 High Frequency update raised per-player update rates in a "bubble" around the player
  (https://battlelog.battlefield.com/bf4/news/view/new-netcode-update-rolling-out/, read) [V page], and DICE's tick
  PDF warns "Weapon spread/recoil is not 1:1" above 30 Hz (`05-networking.md`:88-89) [RC via that report].

Recommendation (aligned with `05-networking.md` §5.4): **server-authoritative, lag-compensated hits with client hints**.

1. The server owns every shot: it receives `FireCmd {tick, sub-tick µs, slot, shot_seq, view yaw/pitch after recoil,
   interp target time}` and runs the same fire sim (§5.2) to produce the same directions (seeded RNG).
2. It traces projectiles against the world in the present and against **soldier capsules rewound to the shooter's
   interpolation time** (cap 200–250 ms, not BF3's 0.85 s); the client may attach `(victim, capsule, shot_seq)` hints,
   used only to choose which history samples to test, never as proof.
3. Bots are server-side shooters: no rewind (they see present state), same fire sim, same damage path; fun-bots' 0.6
   damage factors (`ext\Shared\Config.lua`:20-33) show BF3 bots need nerfing through aim error, not damage edits.
4. Trades: a shot is valid if its client fire time ≤ the shooter's server-side death time + one tick; anything later is
   discarded (BF4's fix, made explicit).
5. Cheat surface: aim/trigger bots remain possible in any shooter; hit forging and wall hits are closed because the
   server traces; no-spread is possible because clients must know the RNG to predict tracers (accepted trade-off, as in
   CS-style games).

## 4. Presentation

* **Meshes and states** [V]: `WeaponStateData {mesh1p, meshZoom1p, mesh3p : SkinnedMeshAsset, weapon AntRef,
  zoomMeshTransitionFactor, zoomScaleFactor, zoomedScopeFilter/nonZoomedScopeFilter : ScopeFilterData,
  animatedFireType, animatedAimingType, projectileBoneName, isOneHanded, alwaysAimHead …}`; M16A3 has one state
  (`AnimatedFireAutomatic`, `AnimatedAimingTwoHanded`, `keepAimingTime 3`). Skeletons (probe :279-284 [RC]):
  `animations/skeletons/venice1pske01` (1p, 179 bones incl. `Wep_Root`, `Wep_Trigger`, `Wep_Slide`, `Wep_Mag`),
  `veniceantske01` (3p, 213), `weapon/weaponske01` (25).
* **Sockets/attachments** [V]: 30 `SocketData` on the M16A3, most on bone `Wep_Root`, gated by `unlockAsset`; each lists
  `WeaponRegularSocketObjectData {asset1p, asset1pzoom, asset3p, transform, mesh3pTransforms}` (e.g. ACOG zoom mesh at
  (0, 0.052, 0.836)); meshes in `weapons/accessories/<item>/<item>_{1p,3p,zoom_1p}_mesh`, reticles as meshes
  (`kobra_reticule_1p_mesh`, `eotech_reticule_small_1p_mesh`, `rx01_reticule_1p_mesh`, `pkas_reticule_1p_mesh`).
  Laser/light: `weapons/accessories/anpeq2/*`, `flashlight/*` (beam meshes, projection texture, lens flares).
* **Render FOV**: `renderFov 55` (hip) and `zoomRenderFov` 15–35 per optic draw the 1p weapon with its own FOV [V data].
  Godot 4.8 has this natively: `BaseMaterial3D.use_fov_override/fov_override` and `use_z_clip_scale/z_clip_scale` exist
  in the API dump [V `extension_api.json`]; spatial shaders get `Z_CLIP_SCALE` (and `PERSPECTIVE_SCALE`) built-ins from
  PR #93142 [RC]. Custom BF3 material shaders need the projection override in `vertex()`; copy the code Godot generates
  for a `StandardMaterial3D` with `fov_override` set [I]. This keeps one camera and one HDR frame (handoff decision 2).
* **Fire effects** [V]: per weapon 3 × 1p + 3 × 3p `FireEffectData` (muzzle flash, "fillout" flash, shell ejection
  `fx_shellejection_{s,m,762,45cal,46mm,shotgun,m_russian…}`, belt links `fx_beltlinkejection_m`) with weapon-space
  offsets and zoom offsets; tracer `fx_tracer_generic_parent(_rus)`, `fx_tracer_mg`, `fx_tracer_sniper` via
  `trailEffect`; overheat `fx_wpn_overheat_generic`. They are ordinary `EffectBlueprint`s → the handoff's
  emitter→GPUParticles3D compiler (04 §6.2).
* **Weapon sound** [V]: `SoundPatchAsset "Weapon_AssaultRifle_M16A4_02" {graph → SoundGraphData, outputNodes[5],
  loudness 110, radius 0.8, isLooping true, defaultStart/StopEvent}` with 163 nodes: 13 `SamplerNodeData`, 8
  `WaveSwitcherNodeData`, 12 `MultiCrossfaderGroup` (distance fades in m: 0–20, 1–5, 1–15, 1–17, 1–19, 3–15, 5–35,
  20–50, 250–300, cosine/linear), 4 `LfoNodeData` (pitch/gain wobble 0.6–1.4), 2 `AdsrNodeData`, IIR high-pass/peaking
  filters, `PhysicsNodeData`, `ConeOutputNodeData` "Cone" (minDistance 10, HF damping 0.8, reverb −17 dB) and
  "Handling" (−29 dB), events/parameters by name hash. Layers referenced: core-bass close/distant
  (`shared_content/corebassclose_lmg_08`, `corebassdistant_lmg_04`), hi-fi mechanics (`hifi_lmg_m4`,
  `hifi_rattle_composite`), LFE, **noise layers close/distant/far × {forest, indoor_garage, indoor_small, openfield,
  urban, urban_alley}**, reflections close/far × environment (+desert), shell casings indoor/outdoor/tarmac, handling
  (fire-mode switch, deploy, light switch, trigger click, iron-sight zoom), 1p reload long/short. Waves are EA-XAS v1
  (handoff 02 §11) [RC via handoff]. How the environment class is chosen per shot (sound scopes/areas) was not traced
  [not determined].
* **Scopes and HUD** [V]: `ScopeFilterData` subclasses `SniperLensScopeFilterData` (chromatic aberration, blur,
  tint) and `ColorTintScopeFilterData` → a full-screen pass (`CompositorEffect` or a quad); `ZoomLevelData` fade-to-black
  fields for scope transitions. `SoldierWeaponData.hud` (`HudData {crosshairScaleMin 0, crosshairScaleMax 5,
  crosshairOpacityMin 1, crosshairOpacityMax 0.1, lowAmmoWarning 0.3, reloadPrompt 0.3, hideCrosshairWhenAimOnFriend}`);
  crosshair size presumably follows current spread [I].
* **Animation**: `SoldierWeaponData.animationData → AntPackageAsset`, `WeaponStateData.weapon` AntRef, and the binding
  list in §1.8. Whatever plays the animations (Ant decode or authored), the weapon state machine must emit these
  signals (deploy, reload variants, fire variants, bolt/pump, zoom parameter, dispersion, bullets left) [V data; I].

## 5. C++ design on Godot 4.8

### 5.1 Layering and data

Follow `07-godot-engine.md` §4.3/§7: `bf3_core` (Godot-free EBX reader) → `bf3_sim` (Godot-free; weapons live here) →
`bf3_godot` (presentation). Weapon definitions are built once per install digest from the level superbundle and cached
as a flat blob in `user://cache/weapons/<digest>.bin` [I].

```cpp
struct Dispersion { float min, max, incPerShot, decPerSec; };              // GunSwayDispersionData
struct Recoil { float up, hMin, hMax, ampMax, hAmpMax, decFactor; };        // GunSwayRecoilData
enum class Move : uint8_t { Base, Moving, Jumping, Sprinting, VaultSmall, VaultMedium };
struct SwayState { Dispersion d[6]; Recoil r; };                            // crouch/prone fill Base/Moving only
struct SwayMod { float minMul, maxMul, incMul, decMul, recoilMagMul, recoilAngleMul; };
struct GunSway {
  SwayState s[3][2];                         // [Stand|Crouch|Prone][NoZoom|Zoom]
  struct Transition { Dispersion penalty; float coolDown; } t[6];
  SwayMod suppression[2];                    // [NoZoom|Zoom]
  struct Gated { uint32_t unlock; bool supportedOnly, lightOnly; SwayMod m[3][2]; };
  std::vector<Gated> mods;
  float firstShotRecoilMul, shootingRecoilDecreaseScale, camSpringK, camSpringDamp;
};
enum class FireLogic : uint8_t { Single, SingleBolt, Auto, Burst, HoldRelease, Detonated };
struct FireDef {
  FireLogic primary; small_vector<FireLogic,3> extra; float rpm, rpmBurst; int perBurst, pellets, perShot;
  float boltDelay, boltTime; bool unzoomOnBolt, holdBoltUntilFire, holdBoltUntilZoom;
  ReloadType reloadType; float reloadEmpty, reloadPartial, reloadDelay, reloadThreshold; int mag, spareMags;
  float preFireDelay, spawnDelay, deployTime; Vec3 shotPos, shotVel; float pelletConeMin, pelletConeMax;
  HoldRelease hold;
};
struct ProjectileDef {
  Kind kind; float gravity, ttl, dmgStart, dmgEnd, falloffStart, falloffEnd, impactImpulse, armTime;
  uint16_t materialPair;                       // index into MaterialContainerAsset.materialPairs
  int32_t explosion = -1, dud = -1;            // ExplosionDef indices
  float supMul, supMin, supMax;                // WeaponSuppressionData
  MissileDef missile;                          // engineStrength, maxSpeed, engineTtl, drag
  uint32_t trailFx, flyBySound;
};
struct WeaponDef { uint32_t id; FireDef fire; int projectile; GunSway sway; AimDef aim; Presentation look; };
WeaponDef resolve(const Blueprint&, std::span<const uint32_t> activeUnlocks);   // §1.5 rule
```

### 5.2 Fixed-tick fire simulation (server and predicting client run the same code)

* Tick = the sim tick (60 Hz recommended by the networking topic; BF3 ran 30). Times are integer microseconds; every
  rate is per second (DICE's "spread/recoil not 1:1" warning).
* Sub-tick firing: while the trigger is held and `nextFire ≤ tickEnd`, fire at `t = max(nextFire, tickStart)`,
  `nextFire = t + 60e6/rpm` (burst uses `rpmBurst`, then waits for re-press; bolt adds `boltDelay + boltTime`); the
  projectile is advanced by `tickEnd − t` on spawn. ROF is then exact at any tick rate.
* State per weapon instance: `spread`, `recoilPitch`, `recoilYaw`, `shotSeq`, `mag`, `spare`, `nextFire`, `burstLeft`,
  `reload{phase, until}`, `bolt{until}`, `firstShot` flag, `heat`.
* Per tick: pick `Dispersion D = sway.s[stance][zoom].d[move]` × active `SwayMod`s × suppression mod;
  `spread = clamp(spread − D.decPerSec·dt, D.min, D.max)` (raise to `D.min` when the state changes; how fast BF3 raises
  it is not known, §6); recoil recovery `recoil *= exp(−decFactor·scale·dt)` placeholder until measured.
* Per shot: `dir = rotate(aim(view + recoil), cone(spread, rng(seq, pellet)))`; per pellet add the pellet cone;
  then `spread = min(spread + D.incPerShot, D.max)`; `recoilPitch += up·(firstShot ? FSRM : 1)·recoilMagMul`,
  `recoilYaw += lerp(hMin, hMax, rng(seq, 'h'))·recoilAngleMul` (positive = left), clamp to amplitude max.
* RNG: counter-based, `h = splitmix64(matchSeed ^ playerId<<40 ^ lifeId<<32 ^ slot<<24 ^ shotSeq<<8 ^ pellet<<2 ^
  purpose)`; disc sampling `r = spread·sqrt(u1)` (uniform area) or `spread·u1` (centre-biased) as a switch until a
  BF3 capture decides (§6).

### 5.3 Projectile simulation and queries

* Bullets are queries, never bodies (Jolt's `max_linear_velocity` is 500 m/s, below rifle v0; `07-godot-engine.md`:193).
  Per tick: `p1 = p0 + v·dt + ½·(0, g, 0)·dt²`, `v += (0, g, 0)·dt`; missiles add engine thrust to `maxSpeed` and
  `drag`; grenades bounce with `collisionSpeedMultiplier`.
* Segment `p0→p1` is tested against (a) the static world + vehicles (Jolt `NarrowPhaseQuery::CastRay` in `bf3_sim`'s own
  `PhysicsSystem`, per sub-shape user data = BF3 material pair, so hit → `flagsAndIndex`, physics-material row and
  property row), (b) soldier hit capsules analytically (13 capsules from the posed skeleton; history ring of capsule
  endpoints per tick for rewind: 13 × 24 B ≈ 312 B/soldier/tick), (c) destructible pieces. Earliest hit wins; legs are
  skipped when `deactivateIfBehindWall` and a wall lies between shooter and capsule [I].
* If Godot's physics is used (client cosmetics, prototypes), the 4.8 API is [V `extension_api.json`]:
  `PhysicsDirectSpaceState3D::intersect_ray(PhysicsRayQueryParameters3D) → Dictionary` and the new allocation-free
  `bool intersect_ray_into(PhysicsRayQueryParameters3D, PhysicsIntersectRayResult3D)` (result: `position, normal,
  collider_rid, collider_id, collider, collider_shape, collider_face_index`); parameters `from, to, collision_mask,
  exclude (RID[]), collide_with_bodies, collide_with_areas, hit_from_inside, hit_back_faces`;
  `PhysicsServer3D::space_get_direct_state(RID)`. Jolt reports `face_index` only with
  `physics/jolt_physics_3d/queries/enable_ray_cast_face_index` (+~25 % ConcavePolygonShape3D memory; Godot Jolt docs,
  read) [V page]. Threading [V source, godot master 2026-10]: `space_get_direct_state` fails with "Space state is
  inaccessible right now" when `(on_separate_thread && !doing_sync) || space->is_stepping()`
  (`modules/jolt_physics/jolt_physics_server_3d.cpp`:231-237; Godot Physics equivalent
  `godot_physics_server_3d.cpp`:191-197); Jolt's `intersect_ray` errors if the space is stepping and calls
  `flush_pending_objects()`, which locks a mutex "because it's called when performing queries, which aren't covered by
  `PhysicsServer3DWrapMT`" (`spaces/jolt_physics_direct_space_state_3d.cpp`:452-455, `spaces/jolt_space_3d.cpp`:442-449).
  So parallel queries from `WorkerThreadPool` tasks between steps are what the Jolt module expects, never during a
  step or while bodies are added/moved [I from source]; the docs call physics "not thread-safe by default" and Jolt +
  separate thread "experimental" [V pages]. This is why weapons belong in `bf3_sim`'s own Jolt world on the server
  thread (`07-godot-engine.md` §4.3).

### 5.4 Damage pipeline

```
d      = |hit − spawn|                            // distance travelled (falloff basis) [I]
base   = falloff(dmgStart, dmgEnd, falloffStart, falloffEnd, d)
row_p  = materialIndexMap[projectile.pair.physicsPropertyIndex & 0xFF]
row_t  = materialIndexMap[target.pair.physicsPropertyIndex & 0xFF]   // capsule / surface pair
rel    = interactionGrid[row_p].items[row_t].physicsPropertyProperties  // MaterialRelationDamageData, PenetrationData
dmg    = base × rel.damageProtectionMultiplier × settings.bulletDamageModifier
continue through if (target.flags & Penetrable) && !rel.neverPenetrate  (damage × rel.damagePenetrationMultiplier) [I]
soldier: health −= dmg; regen after 10 s at 5/s; suppression added to soldiers within 1.5 m of the segment
props:  destruction health −= dmg if rel multiplier > 0 (this replaces handoff 04 §5.4's heuristics)
```

Explosions: per target within `blastRadius`, occlusion rays, `blastDamage` inside `innerBlastRadius` falling to 0,
`explosionCoverDamageModifier` from the relation, soldier `explosionDamageModifier`; `dudExplosion` when the projectile
hits before `timeToArm`/`timeToArmExplosion` [I].

### 5.5 Collision layers (sim ObjectLayers; mirror in Godot layers for cosmetics)

| layer | content (handoff 04 §3.1-3.4) | bullets | soldiers | vehicles |
|---|---|---|---|---|
| WORLD_RAYCAST | Havok Raycast bodies (detailed meshes), terrain | hit | — | — |
| WORLD_COLLISION | Collision bodies (hulls) | hit only where no Raycast body | block | block |
| PLAYER_CLIP / VEHICLE_CLIP | invisible blockers by name | **ignore** | block / — | — / block |
| GLASS / PASSTHROUGH | SeeThrough/glass pairs, chicken-wire, pass-through fences | hit, penetrate per §1.7 | block | block |
| DESTRUCTIBLE | piece-tagged parts in current state | hit | block | block |
| VEHICLE | hulls + part meshes | hit | block | block |
| WATER | volume | splash only | swim | float |
| SOLDIER_CAPSULES | analytic, not a physics layer | hit | — | — |

### 5.6 Client prediction vs server authority

Client predicts: fire timing, ammo/reload/bolt timers, spread and recoil (same code and RNG), 1p animation signals,
muzzle flash, shell ejection, sound, tracer (from the muzzle socket converging onto the camera ray over 30 m), camera
spring kick, world impact effects from its own trace, and an unconfirmed hit marker. Server decides: projectile
outcome, damage, kills, ammo truth, explosions, destruction, suppression. Remote clients get `ShotEvent {shooter,
weapon, shotSeq, t, origin, quantised dir}` and play 3p effects/tracers deterministically.

### 5.7 Build order (each step testable against the oracles above)

1. `bf3_core`: read `SoldierWeaponUnlockAsset → … → GunSwayData/BulletEntityData` from a level superbundle; resolve
   variants and attachments with the §1.5 rule; golden test = the §1.9 roster rows and the Symthic match (50/62 base rows,
   plus the burst-modifier reproduction of "M16A4" 0.1 / 0.075 / 0.3).
2. Material container + per-level grid reader; golden test = the §1.7 multiplier table (2.0/2.35/1.25/1.06/0.91/1.0/0.5).
3. `bf3_sim` fire sim with sub-tick timing and seeded RNG; unit tests for ROF (800 RPM → 75 ms), AN-94 2 @ 1200, bolt
   1.3 s, 870 pump 0.48 s, reload tactical/empty, burst re-press.
4. Projectile stepping against the sim's own Jolt world + analytic capsules; ballistic oracle (650 m/s, g 15 → 1.60 m
   drop at 300 m), max range = v0·TTL, falloff and shots-to-kill table.
5. Server-authoritative hits with rewind (§3), shot events to remote clients, client prediction of effects.
6. Presentation: 1p meshes with `fov_override`/`z_clip_scale` equivalents, sockets/attachments, fire effects through the
   effect compiler, layered weapon audio (start with per-distance crossfades and environment noise layers; full patch
   graph later).
7. Measurement pass in BF3/VU (fixed wall, mouse untouched) to settle §6's spread distribution and recoil recovery,
   then lock the constants.

## 6. What I could NOT determine

* Exact semantics of `recoilAmplitudeDecreaseFactor` (rate vs exponential), `shootingRecoilDecreaseScale`, when the
  first-shot multiplier re-arms, and the pattern Symthic saw in horizontal recoil ("not independent random draws").
* Spread disc distribution (uniform area vs uniform radius), how fast the minimum rises on state changes ("moving spread
  never reached"), and how `GunSwayStanceTransition.maxPenaltyValue/coolDown` apply. Measure in BF3/VU with a fixed-wall
  shot harness (Symthic-style 2,000-burst plots) before tuning.
* Which field scales hit damage (`damageProtectionMultiplier` reproduces the community numbers; `damageProtectionThreshold`
  and `damagePenetrationMultiplier` unknown), whether bullets continue through soldiers, falloff distance basis
  (travelled vs shooter–target), and the explosion falloff curve.
* `clientFireRateMultiplier 0.67`, `reloadThreshold` units, chamber-round rule on empty reloads, `ReloadWholeMags`.
* The server's choice of hi/lo-LOD capsules and how `isSoldierDetailedCollisionEnabled` interacts with client arbitration;
  BF3's actual validation checks on client hits beyond `maxAllowedLatency`.
* Suppression accumulation formula (multiplier units vs thresholds 0.3/0.4) and its visual reaction parameters.
* FOV conventions: whether `fieldOfView`/`renderFov` are vertical, and how 55/40/20/17.2… map to the 3.4×/4× labels.
* The weapon sound graph's environment selection and the named meaning of hashed events/parameters; patch-graph
  evaluation in general (only node inventory and fade ranges were read).
* Kit membership and DLC attribution of base-folder weapons (taken from fun-bots and folder prefixes, not from kit
  assets); `AIWeaponData` values for bots (fields only).
* Primary DICE statements specific to BF3 hit registration (only settings, VU behaviour and BF4-era posts were found).

## Independent verification (2026-10-10)

Method. A separate verifier re-derived each load-bearing claim without the Rime dumps where possible. EBX was re-read with
the EBX probe's own extractor (`ebx-dump`, built from the IW4L crates, not Rime), run read-only against the install, with
output only in scratch. `VW` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\verify-weapons`
(fresh dumps `d1`–`d4`, scripts `scripts\*.py`, own web fetches `web\`). `PE` = `…\scratchpad\probe-ebx` (the probe's
`all-entries.tsv` listing and its Metro grid/material-container dump `out7`). The Symthic Aftermath chart was fetched
again from web.archive.org and parsed (62 rows, `VW\web\symthic.json`). Godot was checked against `extension_api.json`, the
dev7 source at commit c971f93e7 (`…\scratchpad\godot-engine\godot-src`) and the godot-docs master. Verdicts: confirmed /
corrected (partly wrong; corrected statement given) / refuted / unverifiable.

| claim id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| C1 | Weapon EBX is duplicated into every level's main bundle; Metro carries 3,497 `weapons/*` partitions; `Win32/Weapons.sb` has 105 chunk-only bundles | corrected | The probe listing `PE\all-entries.tsv` (ebx-dump `list`, not Rime) shows `Win32/Weapons` = 105 bundles, all `*_soldierweaponbundle`, with only `chunks` rows (M16A4: 40). `weapons/m16a4/m16a4` is in 47 superbundles, including the main bundle of all 29 MP/XP levels (MP_001 … XP5_004). The Metro superbundle holds 2,795 unique `weapons/*` EBX: 2,778 in bundle `win32/levels/mp_subway/mp_subway`, plus 17 texture EBX in mode bundles. It also holds 702 `weapons/*` res. The report's `scratchpad\weapons\metro_weapon_parts.txt` (3,497 lines) = 2,795 EBX + 401 MeshSet + 280 DxTexture + 21 HavokPhysicsData. Weapon folders exist only for XP1/XP2/XP4 [verified]. | "3,497 partitions" is really 3,497 entries: 2,795 EBX partitions plus 702 res. 2,778 of the EBX are in Metro's main bundle. The rest stands. |
| C2 | Spread and recoil are in `GunSwayData`; `FiringFunctionData.dispersion` is empty and `FireLogicData.recoil` is zero on soldier weapons; no first-shot spread multiplier | confirmed | VU-Docs `types\fb\GunSwayData.yaml`, `GunSwayStandData.yaml`, `GunSwayCrouchProneData.yaml`, `GunSwayBaseMoveJumpData.yaml`, `GunSwayBaseMoveData.yaml`, `GunSwayRecoilData.yaml`. Across all 2,491 schema types the only `firstShot*` fields are `GunSwayData.firstShotRecoilMultiplier` and `AnimatedWeaponBinding.firstShotSpawned`. Probe dump `research\probes\ebx\weapons\weapons__m16a4__m16a4.json` (`VW\scripts\sway.py`): stand NoZoom 2.5–7 +0.1 −15/s; Zoom 0.2–3; recoil 0.26, −0.4..0.1, max 10/10, dec 18; FSRM 2.5. All 157 `FiringFunctionData` in the 2,012 dumped weapon partitions (`VW\scripts\scanall.py`): `FireLogic.Recoil` is all zero, and `Dispersion[]` is empty except on the MAV jammer gadget. | Clarification: only stand has jumping/sprinting/vaulting (`GunSwayBaseMoveJumpData`); crouch and prone have base and moving only (`GunSwayBaseMoveData`). §1.3 says this correctly. See extra-6 for the AI firing data inside the blueprints. |
| C3 | v0 = `ShotConfigData.initialSpeed`; every bullet record has 350; no drag; g −15 automatic / −9.81 semi and bolt; max range = v0 × TTL | corrected | Of 83 `BulletEntityData`, 81 have `initialSpeed` 350; `44magnum` has 460 and the defibrillator 2000. The schema chain `BulletEntityData → MeshProjectileEntityData → ProjectileEntityData → GamePhysicsEntityData` has no drag field (only `MissileEntityData.drag`). The inherited `MeshProjectileEntityData.extraDamping` is a bool, false on all 93 projectile records. Gravity is −15 on every automatic bullet and also on `762x39mmwp_semi` (SKS), `357magnum`, `44magnum` and the pistol rounds. It is −9.81 on `_semi` 7.62/5.8, `_bolt`, `338magnum`, buckshot, flechette and `crossbow_bolt`; −12 on slugs; −2 on `crossbow_bolt_ba`; −11 on `crossbow_bolt_he`. In the re-parsed Symthic chart, "max distance" = v0 × bullet TTL on 62/62 rows, and Symthic's v0 always equals the shot speed, never 350. fun-bots reads `shot.initialSpeed.z` (`unfun-bots\ext\Shared\WeaponClass.lua`:198). | Every bullet record has 350 except `44magnum` (460) and the defibrillator (2000). Semi-auto is not uniformly −9.81: the SKS round is −15, crossbow BA −2, HE −11, slugs −12. Whether the engine adds hidden drag remains [inferred]. |
| C4 | 13 `BoneCollisionData` capsules; multipliers from the level `MaterialGridData` | confirmed | Fresh dump `VW\d1\characters__soldiers__defaultsoldierbonecollision.json` has 13 capsules. Nine are hi-LOD: arms r 0.08; spine 0.23; spine sphere 0.25 (pair 84454789 "Chest"); head 0.15; 4 legs 0.11/0.10, all with `DeactivateIfBehindWall`. Four are lo-LOD: head plus 3 `AITrajectory` (r 0.35/0.30/0.35). My lookup `VW\scripts\hz.py` over `PE\out7` (Metro) uses row = `MaterialIndexMap[PhysicsPropertyIndex & 0xFF]`. BulletDamage(77): head 2.0, body 1.0, chest 1.0, foot 0.91. Sniper(85): head 2.35, chest 1.25 (threshold 2), foot 0.91. Magnum .338 (166): head 2.35, chest 1.06. Buckshot(88): head 1.0, with `neverPenetrate`. Flechette: 1.0 everywhere. MeleeDamage: head 0.67, all other zones 0.5. Bullet pairs: 5.56 and 9×19 pistol → 5b6b84ac; 7.62 semi/bolt, slug and .44 → 7783b944 (Sniper); .338 → 4a899d88. | — (which field scales the damage stays [inferred]) |
| C5 | Unlock-keyed `WeaponModifierData`; `GunSwayModifierData` factors; `u_m16_burst` reproduces Symthic's "M16A4" | confirmed | Schema: `WeaponModifierData {unlockAsset, modifiers}`; `WeaponShotModifier {initialSpeed, numberOfBulletsPerShell}`; `WeaponZoomModifier {zoomRenderFov}`; `WeaponFiringDataModifier {weaponFiring}`; `GunSwayModifierData {unlockAsset, stand/crouch/prone × Zoom/NoZoom modifiers, onlyInSupportedShooting, onlyOnWeaponLightEnabled}`; `GunSwayStanceZoomModifierData {dispersionMod, recoilMagnitudeMod, recoilAngleMod, lagYawMod, lagPitchMod}`. Fresh dumps `VW\d1\weapons__m16a4__m16a4_{burst,foregrip,heavybarrel,silencer,flash_suppressor}.json`: foregrip Zoom min ×1.5, angle 0.75. Heavy barrel Zoom ×0.25, NoZoom ×1.2, magnitude 1.15. Silencer Zoom ×0.75, NoZoom ×1.4, magnitude 0.9. Flash suppressor NoZoom ×1.2, magnitude 0.8. Burst Zoom ×0.5, angle 0.75. The suppressor's `WeaponModifierData` sets projectile `556x45mmnato_cold`, `WeaponShotModifier` z 380, a silenced `SoundPatchAsset` and `isSilenced`. `u_m16_burst` is a `SoldierWeaponUnlockAsset` with `weapon` → `weapons/m16a4/m16a4` and `extra` → `u_m16a4_burst`. Symthic's M16A4 row (ADS 0.1, ADS moving 0.5, left 0.075, right 0.3, all else equal to the M16A3) is exactly the M16A3 × burst modifier, with angle = horizontal and magnitude = vertical. The same holds for Symthic "M4" vs M4A1. | — |
| C6 | Install matches Symthic on every field for 50 of 62 weapons; the 12 differences are explained | confirmed | My own comparison `VW\scripts\cmp.py` covers 16 fields: RPM, v0, the damage 4-tuple, recoil up/left/right/dec, FSRM, ADS/hip min, SIPS, empty reload and magazine. Result: 50/62 identical. The differences are exactly G17, 93R (SIPS), UMP-45 (SIPS), the five bolt-actions (NoZoom block and effective RPM), SKS (dec: Zoom 9 vs NoZoom 15, checked in `weapons~sks~sks.json`), LSAT (ADS), M4 and M16A4. Ten rows were re-read from fresh ebx-dump output. | — |
| C7 | Symthic "left" = `…IncPerShotMax`, "right" = −`…Min`; positive = yaw left | confirmed | Chart vs data: M16A3 −0.4..0.1 ↔ left 0.1 / right 0.4; AEK-971 −0.3..0.5 ↔ 0.5 / 0.3; M417 −0.25..−0.05 ↔ left −0.05 / right 0.25. This holds for all 50 matching rows. Handoff `sections\02-resources.md`:340-342 says the fb→IW mapping is a pure rotation, verified there by mesh handedness; a right-handed, Y-up, Z-forward frame has +X = left [report claim]. | The sign mapping is verified; "positive = yaw left" stays [inferred]. |
| C8 | Bolt interval = `boltActionDelay + boltActionTime`; AN-94 2-round bursts at 1200 | confirmed | Fresh dumps: JNG-90 0 + 1.3, L96 0 + 1.38, M40A5 0 + 1.1, SV98 0 + 1.25, M98B 0 + 1.3, all with `rateOfFire` 360. 60/t gives 46.2 / 43.5 / 54.5 / 48 / 46.2 RPM, exactly Symthic's figures. AN-94: `fltBurstFire`, `fireLogicTypeArray [2,0]`, `numberOfBulletsPerBurst 2`, `rateOfFireForBurst 1200`, `rateOfFire 600`. Automatic rifles carry an inert default `BoltAction` of 0.5 + 1.0. | The interval composition matches Symthic only; it was not measured in game. |
| C9 | Bullets do 0 damage to world materials and 1.0 to a listed few; penetration only through Penetrable pairs without `neverPenetrate` | corrected | `VW\scripts\world.py`, `world2.py` over the Metro grid, BulletDamage row 43. Multiplier 0 (threshold 2) on 37 rows: Concrete, Brick, Metal, Wood, Destructible, Indestructible, Armored, DamageClass_0/3/4/5, `Chickenwire` (property 95), StationaryDamage, and `Light` properties 4 and 37. Multiplier 1.0 on Passthrough, DamageClass_1, AllProjectiles, `Light` (124) and `ChickenWire` (150). Multiplier 1.0 with penetration multiplier 0.75 on DamageClass_2, BulletsOnly and CollResponseCombined. Top-byte bit 0x01 is set on 1,800 of 32,400 `MaterialContainerPair`s (flag byte 1 ×1,232, 9 ×388, 3 ×176, 11 ×4). The flagged properties are Head, Body, Chest, Foot, Passthrough, ChickenWire, AllProjectiles, BulletsOnly, DmgClass3 and DamageClass_0. `neverPenetrate` is true in 1,317 cells and false in 10. | "1.0 to … ChickenWire and Light" holds only for properties 150 and 124; `Chickenwire` (95) and the other `Light` properties (4, 37) get 0. The penetrable set also includes BulletsOnly, AllProjectiles, DmgClass3 and DamageClass_0. The penetration rule stays [inferred]. |
| C10 | Client-side damage arbitration (latency 0.85 s), 30 Hz server, server-side `BulletEntity`; fun-bots toggles arbitration | confirmed | Fresh dump `VW\d3\levels__mp_subway__mp_subway__settings_win32.json`. SyncedGameSettings: `AllowClientSideDamageArbitration true`, `MaxAllowedLatency 0.85`, `BulletDamageModifier 1`. GameTimeSettings: `MaxSimFps 30`. ServerSettings: `IsSoldierDetailedCollisionEnabled false`. GameSettings: `UseSpeedBasedDetailedCollision false`. VU documents the server hook `BulletEntity:Collision(entity, hit, giverInfo)` (`VU-Docs\content\vext\ref\server\hook\bulletentity_collision.md`:5) and `DamageInfo.isClientDamage` (`VU-Docs\types\server\type\DamageInfo.yaml`:42). fun-bots sets arbitration false around `p_HookCtx:Call()` when `giver.onlineId == 0`, then back to true (`unfun-bots\ext\Server\__init__.lua`:595-614). VU-Docs `content\hosting\commands.md`:75: 30 Hz is "regular". | The data values are now [verified] (the report had them as [report claim]). They are level-data defaults; a live server's runtime config is not in the install. |
| C11 | 4.8-dev7 exposes `intersect_ray` and `intersect_ray_into` with `collider_shape` and `collider_face_index`; Jolt needs `enable_ray_cast_face_index` | confirmed | `extension_api.json` (v4.8.dev7.official, single precision): `PhysicsDirectSpaceState3D.intersect_ray(PhysicsRayQueryParameters3D) → Dictionary` and `intersect_ray_into(PhysicsRayQueryParameters3D, PhysicsIntersectRayResult3D) → bool`. `PhysicsIntersectRayResult3D` (RefCounted) has position, normal, collider_rid, collider_id, collider, collider_shape and collider_face_index. godot-docs `tutorials/physics/using_jolt_physics.rst` (master, fetched): with Jolt, face_index is −1 unless `physics/jolt_physics_3d/queries/enable_ray_cast_face_index` is set, which costs about 25 % more ConcavePolygonShape3D memory. | — |
| C12 | `space_get_direct_state` fails while stepping; Jolt queries error during a step and lock in `flush_pending_objects`; parallel queries between steps | confirmed | dev7 source c971f93e7: `modules/jolt_physics/jolt_physics_server_3d.cpp`:231-237 (identical to master). `spaces/jolt_physics_direct_space_state_3d.cpp`:451-454 (master 452-455): `ERR_FAIL_COND_V_MSG(space->is_stepping(), …)`, then `flush_pending_objects()`. `spaces/jolt_space_3d.cpp`:445-451 (master 442-449) carries the `PhysicsServer3DWrapMT` mutex comment verbatim. Godot Physics: `godot_physics_server_3d.cpp`:191-197. godot-docs `thread_safe_apis.rst`: physics "is *not* thread-safe by default". GDExtension probe: worker-thread rays scale to 4 threads while the main thread blocks, and about one query per step is refused during stepping. | dev7 line numbers differ from master by 1–3. The stepping guard is a check, not a lock, so the game itself must fence worker query batches outside the step. |
| C13 | `BaseMaterial3D` `fov_override` and `z_clip_scale` cover the 1p weapon's render FOV without a second viewport | confirmed | API dump: `use_fov_override`, `fov_override`, `use_z_clip_scale`, `z_clip_scale`. dev7 `doc/classes/BaseMaterial3D.xml`:236-238: `fov_override` acts like a Camera3D with `KEEP_HEIGHT`, i.e. a vertical FOV. `scene/resources/material.cpp`:1447-1462 sets `Z_CLIP_SCALE` and rewrites `PROJECTION_MATRIX` in `vertex()` outside the shadow pass. PR #93142 was merged on 2025-05-22. | Godot's `fov_override` is vertical. Whether BF3's `renderFov` 55 / `zoomRenderFov` are vertical is still open (§6). |
| C14 | M16A3 `SoundPatchAsset` graph has 163 nodes; layer list and crossfade ranges | corrected | Fresh dump `VW\d4\sound__weapons__handheld__m16a3__weapon_assaultrifle_m16a4_02.json`. The partition has 163 instances, but `SoundGraphData.nodes` has 107 entries. There are 12 `MultiCrossfaderGroup`s with fades 0–20, 1–5, 1–15, 1–17, 1–19, 3–15, 5–35, 20–50, 250–300 (×3) and 300–301 m, plus 2 `ConeOutputNodeData`. The patch references 51 sound partitions, which match the stated layers: core-bass close/distant, hi-fi, LFE, noise close/distant/far × 6 environments, reflections, `shellcasings_762` indoor/outdoor/tarmac, and handling. | "163 nodes" → 163 instances in the partition, of which 107 are graph nodes. The 12 crossfader groups are fade groups, not nodes. |
| C15 | Falloff is linear from start to end damage and flat outside that range | unverifiable | Symthic tutorial 1434 §1.7 (my own fetch, `VW\web\tut1434.html`): flat at 25 to 8 m, "a linear dropping off until 50 meters", then flat at the minimum. The data values 25/18.4/8/50 are verified. No DICE or engine source was found; a web search returned only forum restatements. | Keep as [report claim]; the §5.7 measurement pass should settle it. |
| C16 | maxHealth 100; regen 5/s after 10 s; suppression decay, sphere and thresholds; per-bullet multipliers 7/10/20/35/50 | confirmed | Fresh dump `VW\d1\characters__soldiers__mpsoldier.json`. `SoldierEntityData.MaxHealth` 100. `VeniceSoldierHealthModuleData`: RegenerationDelay 10, RegenerationRate 5, ImmortalTimeAfterSpawn 2. `SoldierSuppressionComponentData`: SuppressionDecay 0.5, FallOffDelay 2, SphereRadius 1.5, thresholds High 0.4 / Low 0.3 / UI 0.15. `WeaponSuppressionData`: 556 7, `_heavy` 10, `_cold` 4, 7.62 semi 20, bolt/.338/slug/crossbow 35, 40 mm 50. Symthic prints 7.0 / 10.0 / 20.0 / 35.0 %. | (The SKS round is 10, not 20; see extra-2.) |
| extra-1 | §2.4: "DMR head 50×2.35 = 117.5 (OHK ≤ 15 m …)" | refuted | The report's own data and model give 50 → 37.5 damage over 15–75 m. One shot to the head kills while damage ≥ 100/2.35 = 42.55, which holds out to 15 + 60·(50−42.55)/12.5 ≈ 50.8 m [inferred arithmetic]. | DMR (7.62/5.8 `_semi`) headshot one-shot kill holds to ≈ 50.8 m. For the SKS (43 → 27 over 15–60 m) it holds only to ≈ 16.3 m. |
| extra-2 | §1.6: gravity "semi/bolt/.338/crossbow −9.81"; suppression "20 (semi DMR)" | corrected | `scratchpad\weapons\dumpall\weapons~common~bullets~*.json`: `762x39mmwp_semi` has gravity −15 and suppression 10 (Symthic SKS 10.0 %). `crossbow_bolt_ba` is −2 and `crossbow_bolt_he` −11. The §1.9 SKS row already shows −15. | Gravity −9.81 applies to 7.62×51/×54R and 5.8 `_semi`, `_bolt`, .338, `crossbow_bolt` and buckshot/flechette. The SKS round is −15 with suppression 10. |
| extra-3 | §1.7: "multiplier 1 against … StationaryDamage (demolition on)"; "1,520 of 32,400 pairs" are penetrable | corrected | `VW\scripts\world2.py`: StationaryDamage (property 48, row 25) has `DamageProtectionMultiplier` 0 with `InflictsDemolitionDamage` true. The penetrable-bit count is 1,800. | StationaryDamage takes 0 bullet damage, though the demolition flag is on. 1,800 pairs are flagged penetrable. |
| extra-4 | §5.3: "Jolt's `max_linear_velocity` is 500 m/s, below rifle v0" | corrected | dev7 `modules/jolt_physics/jolt_project_settings.cpp`:69: `physics/jolt_physics_3d/limits/max_linear_velocity` has default 500 and range hint `or_greater`. `ProjectSettings.xml`:2772-2775 says it caps RigidBody3D. | It is a configurable default, not a hard limit. The conclusion (bullets as queries) is unaffected. |
| extra-5 | §4: spatial shaders get `Z_CLIP_SCALE` "(and `PERSPECTIVE_SCALE`)" built-ins from PR #93142 | corrected | dev7 `servers/rendering/shader_types.cpp`:100 registers only `Z_CLIP_SCALE`; `PERSPECTIVE_SCALE` appears nowhere in `servers/` or `scene/`. The PR page says `PERSPECTIVE_SCALE` was replaced by `fov_override` before the merge, and that the PR added `IN_SHADOW_PASS`. | The built-ins are `Z_CLIP_SCALE` and `IN_SHADOW_PASS`. A custom shader overrides the FOV by rewriting `PROJECTION_MATRIX` in `vertex()`, as the generated StandardMaterial3D code does. |
| extra-6 | §1.2: `weaponDispersion` "all 0 (dec 5) on rifles" | corrected | Probe dumps of the M16A4, AK-74M and M249 blueprints each also hold an AI `FiringFunctionData`. It uses projectile `weapons/common/aibullets/ai_western_tracer` or `ai_russian_tracer`, with v0 250, `weaponDispersion` stand 1–3°, and bullet TTL 0.5 s at gravity 0. The player firing data is 0 as stated. | The loader must take the player `WeaponFiringData` (from `SoldierWeaponData.weaponFiring` and the unlock modifiers), never the AI copy that sits in the same partition. |
| extra-7 | §1.3: suppression modifier "minAngleModifier 1.5, increasePerShotModifier 0.1, recoilAngleMod 1.5" | corrected | Probe dump of the M16A4 `SuppressionModifierZoomed`: it also has `MaxAngleModifier 0.0`, `DecreasePerSecondModifier 1.0`, `RecoilMagnitudeMod 1.0` and `LagYawMod`/`LagPitchMod 0.0`. Every attachment `GunSwayModifierData` dumped also carries `Lag*Mod 0.0`. | Multiplying by every field literally would set `maxAngle` to 0 under suppression. The port must treat 0 as "unset" for `MaxAngleModifier` and `Lag*Mod`. Whether `IncreasePerShotModifier 0.1` is a multiplier is equally unclear [inferred]; measure it. |
| extra-8 | Core principle (BF3 files read as-is) | confirmed | The report reads the install in place and caches to `user://cache/weapons/<digest>.bin` (§5.1). It uses VU and fun-bots only as references. The §5.7 step-7 measurement in BF3/VU gathers knowledge offline and is not a runtime route. No recommendation writes to, patches, repacks or hooks the install. | Keep the measurement pass offline and run by the user, never a shipped dependency. |

Not determined by this verification: the in-game falloff shape, the bolt-interval composition, the spread-disc
distribution and recoil recovery (all need the measurement pass); whether the engine really overrides
`BulletEntityData.initialSpeed` with the shot speed (Symthic and fun-bots agree, but no engine source exists); which
`MaterialRelationDamageData` field scales damage and what `damageProtectionThreshold` means; the Caspian grid (only Metro
was re-read); live retail server settings beyond the level defaults.
