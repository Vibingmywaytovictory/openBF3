# 02 — The BF3 soldier, classes/kits/gadgets, multiplayer mode rules, and their C++ rebuild on Godot 4.8

Research slice for the BF3-in-Godot reimplementation (MP only, bots, C++ GDExtension, original files read in place).
Written 2026-10-10. Tags: **[V]** verified here (in data read from the retail install, in code, in the Godot 4.8-dev7
API dump, or in a primary source actually read); **[R]** report claim (a source not re-checked); **[I]** inferred.

## 0. Method, sources, abbreviations

* **Reading the install.** Rime's `RimeREPL.exe` (built by IW4L slice 09 from an unmodified copy of `C:\Users\olku\Rime`)
  was **copied** to `…\scratchpad\soldier-kits-modes\rime\` and run with cwd and every output under
  `…\scratchpad\soldier-kits-modes\dump\` (full scratch root:
  `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\soldier-kits-modes`).
  Commands used: `mount_game … false`, `mount_sb`, `list_sb_bundles`, `list_bundle_partitions`,
  `dump_partition_json[_by_guid]` only; IW4L slice 09 audited these as read-only on the game directory
  (`C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\1-RESEARCH-PART\09-rime-tooling-and-live-bf3-dump.md` §1.1).
  Superbundles mounted: `win32/levels/mp_001/mp_001`, `xp2_palace`, `xp3_shield`, `xp4_quake`, `xp5_001` (base +
  `Update\Patch` + `Update\Xpack*` layering as Rime resolves it). 320 partitions dumped (incl. 132 Gun Master
  `gameplay/xp2/gm_*`), 3 requested names absent from those bundles, 0 parse failures [V].
  Scripts: `…\soldier-kits-modes\scripts\d1–d6.txt`; analysis helpers `…\tools\*.py`.
* **D** = those dumps, cited as `partition-name` + instance-GUID prefix (e.g. `characters/soldiers/defaultsoldierphysics`
  `a10ff2aa`). **D0** = IW4L's earlier dumps `C:\Users\olku\iw4L\context\bf3dump\ebx\**` (mpsoldier, conquest,
  deathmatch, capturepointprefab, full_teamdeathmatch, usassault, level sub-worlds of MP_001 and MP_Subway).
  **S** = schema `C:\Users\olku\VU-Docs\types\fb\<Type>.yaml`. **P** = the parallel EBX probe
  `C:\Users\olku\BF3 reimplementation\research\probes\ebx-data-probe.md` (its numbers agree with mine wherever both
  read the same instance; it read Caspian/Metro, I read Bazaar/XP levels). **API** = `research\godot-api\extension_api.json`
  (header: `4.8 dev7 official`, `precision single` [V]).
* Field-id hashes: interface/property ids are `djb2-xor` of the UpperCamel field name (`h=5381; h=(h*33)^b`), so
  connections can be resolved by hashing schema names (`tools\h.py`, `tools\conns.py`); e.g. `-206476404 =
  SprintMultiplier`, `264089340 = InitialTicketCount`, `-77167751 = AreaValue`, `-821768885 = SpawnDelay` [V].
  Designer-named ports stay unresolved hashes (listed where they matter).

---

## 1. The soldier

### 1.1 Entry chain [V]

`TeamEntityData.Team` → `TeamData` (`gameplay/teams/us` `94535eb8`: `Faction FactionUS`, `Soldier` → partition
`f256e142` = `characters/soldiers/mpsoldier`, `SoldierCustomization[4]` = the four kits, `VehicleCustomization[8]`) →
`SoldierBlueprint` `261e43bf` (`Characters/Soldiers/MpSoldier`) → `SoldierEntityData` `a9ffe6b4` (D0; S
`SoldierEntityData.yaml`, `TeamData.yaml`). One soldier blueprint serves both factions; faction differences are kits and
visual unlocks. fun-bots hard-codes the same GUID as `selectedKit`
(`C:\Users\olku\unfun-bots\ext\Server\BotSpawner.lua:935`, `:1545`) [V].

`SoldierEntityData` scalar fields (D0 `a9ffe6b4`): `MaxHealth 100`, `LowHealthThreshold 30`, `ExplosionDamageModifier 1.0`,
`RegenerationDelayModifier 1.0`, `UsePrediction true`, `DefaultViewMode PersonViewMode_ThirdPerson`,
`InteractiveManDownAllowed true`, `IsPushable false`, `ProximityCheck true`, `ShowNametag true`, `RuntimeComponentCount 45`;
references `HealthModule` → `VeniceSoldierHealthModuleData 705967ee`, `CharacterPhysics` → `defaultsoldierphysics a10ff2aa`,
`CollisionInfo` → `CollisionData 5917c5be` (fall damage), `SprintSettings` → `SoldierSprintSettingsData 296c4be6`,
`AimingConstraints` → `SoldierAimingConstraintsData a7c6c9fc`, `BreathControl[0]` → `e7cab698`, `MaterialPair 6609a12f`
(body), `HeadMaterialPair 8b81bca1`, `FootMaterialPair fda445e9`; `HeadCollision null`, `AutoAim null` [V].

Components under `SoldierBodyComponentData 1c721510` [V, D0]: `SoldierWeaponsComponentData` (weapon slots, ammo/grenade
multipliers), `SoldierCameraComponentData` (1p `VeniceFPSCameraData` + 2 chase cams, `AuthoritativeEyePosition true`),
`SoldierSuppressionComponentData`, `SpecialMovesComponentData` (vault/dive), `ClimbLadderComponentData`,
`EntityInteractionComponentData`, `BoneCollisionComponentData` → `defaultsoldierbonecollision`, `RagdollComponentData`,
`HitReactionComponentData`, `DropWeaponComponentData`, `PlayerEntryComponentData`, 4 × `AntAnimatableComponentData`,
`FlagComponentData`, `FaceposerComponentData`, `SoldierFootplantEffectComponentData`; plus on the entity:
`SpottingComponentData`, `SpottingTargetComponentData`, `SoldierParachuteComponentData`, 16 `UnlockComponentData`
(specializations, §2.3).

### 1.2 Health, regeneration, man-down, revive, spawn immunity [V unless noted]

`VeniceSoldierHealthModuleData 705967ee` (S lines 36–49):

| field | value | meaning |
|---|---|---|
| `RegenerationDelay` / `RegenerationRate` | 10 s / 5 | delay after damage; 5 hp/s [I] (0→100 in 20 s) |
| `SuppressionAbortsHealthRegeneration` (suppression comp.) | true | being suppressed restarts the delay [I] |
| `ImmortalTimeAfterSpawn` | 2 s | spawn protection; ended early by `AbortSpawnImmortalityInputs` = Jump, Throttle, Strafe, Fire, MeleeAttack, ThrowGrenade, Brake, Zoom |
| `ManDownStateTime` | 13 s | time a downed soldier stays revivable [I] |
| `ManDownStateHealthPoints` | 9999 | |
| `PostReviveHealth` | 20 | health after a defib revive [I] |
| `TimeForCorpse` | 15 s | corpse/dog-tag lifetime [I] |
| `InteractiveManDown` | false | no crawling man-down state in BF3 MP |
| `SprintDisabledWhenDamagedTime/Threshold` | 0 / 0 | sprint never disabled by damage |
| `CriticalFakeImmortalTime` | 0 | |

`LowHealthThreshold 30` (SoldierEntityData) drives the low-health HUD [I]. The community quotes a 16 s regen delay
(Battlefield Wiki "Regeneration", via P §3) — **conflicts with the data's 10 s** [R vs V]; read the data and measure once.
Hardcore presets run `vars.soldierHealth 60` and `vars.regenerateHealth false` [R]
([4netplayers preset table](https://www.4netplayers.com/en/wiki/battlefield-3/game-mode-presets/)).

### 1.3 Character physics (`characters/soldiers/defaultsoldierphysics` `a10ff2aa`) [V]

`Mass 100`, `PhysicalRadius 0.35`, `MaxAscendAngle 50`°, `WaterDepthLimit 1.4`, `InputAcceleration 0`,
`LadderAcceptAngle 90`, `LadderAcceptAnglePitch −10`, `JumpPenaltyTime 1.0`, `JumpPenaltyFactor 0.3`,
`PushableObjectWeight 100`, `RadiusToPredictCollisionOnCharacters 0`, `AllowPoseChangeDuringTransition true`,
`DefaultState OnGround`. `Sprint` → `CharacterSprintData fac7c821`: `AllowContinousSprinting true`,
`SprintPowerDecreasePerSecond 0` → **unlimited sprint** (no stamina).

Poses (`CharacterPoseData`; all `CharacterPoseCollisionType_Capsule`, `StepHeight 0.3`, `ThrottleModifierCurve`
(0,0)(0.1,0)(1,1) = 10 % stick dead zone then linear):

| pose | `Height` | `EyePosition.y` | `CollisionBoxMinExpand` / `MaxExpand` | look yaw / pitch | transition to |
|---|---|---|---|---|---|
| Stand `82d79307` | 1.80 | 1.60 | (0.2,0,0.1) / (0.1,0.6,0.2) | ±90 / ±45 | crouch 0.3 s, prone 0.7 s |
| Crouch `4496651d` | 1.15 | 1.00 | (0.1,0,0.6) / (0.2,0.1,0.4) | ±90 / ±45 | stand 0.3 s, prone 0.5 s |
| Prone `6c836a7a` | 0.30 | 0.30 | (0.3,0,1.3) / (0.3,0,1.1) | ±90 / −30…45 | stand 0.7 s, crouch 1.05 s |

`LookConstraints` are relative limits (head/body) rather than view limits [I]. Prone's 0.3 m height is below
2 × radius, and the expand boxes reach 1.3 m behind / 1.1 m ahead, so the prone proxy is a body-length box/footprint,
not a 0.3 m capsule [I]; prone aiming is clamped by `characters/soldiers/proneaimingconstraints` (yaw ±45, pitch ±45,
`ConstraintMoveSpeedMultiplier 2`) [V].

States (`CharacterStatePoseInfo` per pose; `SpeedModifier` = forward/back/left/right constants):

| state (data) | stand | crouch | prone | other fields |
|---|---|---|---|---|
| OnGround `18b95c2b` | **4.0 m/s**, back/strafe ×0.75, `AccelerationGain 0.35`, `DecelerationGain −15`, `SprintGain 0.25`, **`SprintMultiplier 1.625`** | **2.0**, ×0.8, sprint ×0 | **0.9**, back ×0.5, strafe ×0.7, sprint ×0 | `AllowedDistanceFromGround 0.5`, `GroundHugging true`, `JumpDelay 0`, `JumpStaminaPenalty 0`, `LimitDownwardVelocity false` |
| Jumping `3129bcfe` | 4.0, sprint ×1.2, gains 0 | – | – | **`JumpHeight 0.6` m**, `JumpEffectSize 0.12` |
| InAir `584d7b54` | 6.0, back/strafe ×0.5, accel 0.1, sprint ×1.5 | 6.0 | 6.0 | `FreeFallVelocity 16` |
| Falling `8de3e183` | 15, accel 0.05 | 15 | 15 | |
| Parachute `019535d8` | 9.0 | – | – | `DeployTime 1`, `TerminalVelocity 5`, `ForwardDragCoefficient 0.02`, `AngleOfAttack 10`, Bank/Throttle/Brake offsets 0.3/0.3/0.175, max roll/pitch/yaw rate 45/100/30 °/s |
| Swimming `8859f3d7` | 3.0, ×0.34, accel 0.05, decel 0.8 | – | – | `BodyUnderWater 1.4` |
| Climbing `8e757d6d` | 1.32 | – | – | `DownAngleLimit 35`, `DropOffAngle 30`, `AttractionVelocity 1`, `PushAwayVelocity 5`, climb-off 1.76 m up / 0.99 m across in 0.75 s each |
| Sliding `eaff72d9` | 4.0 (decel 0.8) | 2.0 | 0.9 | `HorizontalInputScale 0.3`, `GravityScale 1` |
| AnimationControlled `8b0ad88f` | 4.0 | 1.8 | 0.8 | `UpNormalTolerance 0` |

Derived [I]: sprint = 4.0 × 1.625 = **6.5 m/s** (×1.1 = 7.15 m/s with the Sprint specialization, §2.3); walk back/strafe
3.0 m/s; crouch-walk 2.0; prone crawl 0.9; jump take-off `v0 = √(2 g h)` = 3.43 m/s at g 9.81 for 0.6 m. BF4's
equivalent value `Velocity 4.0` and community "6.5 m/s" sprint agree with this
([Symthic forum, BF1/BF4 thread](http://forum.symthic.com/battlefield-1-general-discussion/battlefield-1-technical-discussion/11354-soldier-movement-speed)) [R].
`SprintMultiplier 0` on crouch/prone means sprint input stands the soldier up [I]. The gain semantics
(`AccelerationGain 0.35`, `DecelerationGain −15`, `SprintGain`) are not documented anywhere I found [I]; fit them
against a recording (time to full speed / to stop).

`SoldierSprintSettingsData 296c4be6`: `Fov 55`, `FovIn/OutTime 0.1`, `RecoverTime 0.12` s (sprint→fire delay [I]),
`SprintToProneRecoverTime 0.9`, `InterruptingActions` = Fire, Zoom, MeleeAttack, ThrowGrenade, Prone, ChangePose,
Clutch, CrawlSpeed [V]. `BreathControlData e7cab698`: hold 4 s, release 3 s, penalty ×3 for 3 s [V].
`SoldierAimingConstraintsData.SwimmingConstraints`: yaw ±180, pitch −30…80; parachute yaw ±45, pitch −85…70 [V].
`SupportedShootingCommonData` (bipod/rest): yaw ±45, pitch −30…15, probe rays 0.55/0.6 m above gun, 1 m long [V].

**Vaulting and dive exist** [V]: `SpecialMovesComponentData db892c47` holds `VaultOverHighState` (optimum obstacle
0.6 m high at 0.6 m; motion points to (0,1.1,0.8) at 0.4 s and (0,0,2.2) at 0.75 s; crouch pose override),
`VaultUpHighState` (obstacle 1.2 m; onto (0,1.25,1.0) in 0.4 s), `VaultOverLowState` (0.45 m; 3.3 m forward in
0.8 s), **`SprintToProneState`** (dive: 2.5 m in 0.4 s, 3.0 m at 0.6 s, ends prone), `DeathState`, indexed
variants with distance/height tolerance windows (`SpecialMoveVaultStateCriteria`), each with `MotionPoints` and
`GravityPoints` (gravity −9.81 switched on after 0.4 s) and `Min/MaxHeightScale`, `Min/MaxDistanceScale`. These are
authored root-motion curves: a data-driven vault is possible without Ant animation.

### 1.4 Fall and impact damage [V]

`CollisionData 5917c5be`: `DamageAtVerticalVelocity` and `DamageAtHorizVelocity` both
(0→0)(13.9→0)(14→20)(22→40)(30→100) (m/s → hp, piecewise linear [I]). At g 9.81 that is no damage below a
**9.9 m** drop, 20 hp at 10 m, death (100) at ≈ **46 m** [I]. `KillSoldierCollisionSpeedThreshold 5` sits on vehicles
(handoff 04 §4.1).

### 1.5 Hit zones and damage multipliers [V]

`characters/soldiers/defaultsoldierbonecollision` `SkeletonCollisionData` (skeleton `39b2c3ca`): per-bone capsules
(`BoneAxis 0`, `ValidInHiLod`):

| bone | capsule r / len / offset | `MaterialPair` (prop. index) | hit reaction |
|---|---|---|---|
| Head | 0.15 / 0.01 / (−0.033,0.05,0) | head `8b81bca1` (65) | HRT_Head |
| Spine (upper) | 0.25 sphere / (0,0.25,0) | upper torso `84454789` (158) | HRT_Body |
| Spine (lower) | 0.23 / 0.1 / (0,−0.1,0) | body `6609a12f` (66) | HRT_Body |
| Left/RightArm | 0.08 / 0.28 | body (66) | HRT_Left/RightArm |
| Left/RightUpLeg | 0.11 / 0.40 | legs `f44644a0` (91) | HRT_*Leg |
| Left/RightLeg | 0.10 / 0.43 | legs (91) | HRT_*Leg |
| `AITrajectory` ×3 | 0.30–0.35 (low LOD only) | | AI/low-LOD proxy |

Multipliers are **not** on the soldier or weapon: they are `MaterialRelationDamageData.DamageProtectionMultiplier` in
the level grid `levels/<lvl>/<lvl>/materialgrid_win32/grid` cell (projectile material, zone material), row =
`MaterialIndexMap[PhysicsPropertyIndex & 0xFF]` (Bazaar grid: 111 rows; head row 41, body 42, upper torso 106, legs 62)
[V, D + P §4]:

| projectile material | head | body/arms | upper torso | legs |
|---|---|---|---|---|
| 5.56 NATO (rifles, pistols, LMGs) | 2.0 | 1.0 | 1.0 | 0.91 |
| 7.62×51 bolt | 2.35 | 1.0 | 1.25 | 0.91 |
| .338 Magnum (P) | 2.35 | 1.0 | 1.06 | 0.91 |
| RPG/SMAW/Javelin/40 mm impact | 2.5 | 2.5 | 2.5 | 2.5 |
| knife `BulletEntityData` (100 dmg) | 0.67 | 0.5 | 0.5 | 0.5 |
| M67 / C4 / claymore / M15 body | 0 | 0 | 0 | 0 |

So a frontal knife hit is 50 hp [I] (takedowns from behind are a separate melee path in `MeleeEntityData`, game code).
Explosions damage soldiers through `ExplosionEntityData` (`BlastDamage` inside `InnerBlastRadius`, falloff to
`BlastRadius` [I]), scaled by `SoldierEntityData.ExplosionDamageModifier` (0.85 with Flak) and by cover via
`ExplosionCoverDamageModifier`/occlusion rays (`MaxOcclusionRaycastRadius`) [I].

### 1.6 Suppression [V]

Soldier side `SoldierSuppressionComponentData 5ecc8031`: `SuppressionSphereRadius 1.5` m, `SuppressionDecay 0.5`,
`FallOffDelay 2` s, `FallOffMultiplier 1`, reaction thresholds `SuppressionLow 0.3`, `High 0.4`, `UI 0.15`,
`SuppressionResistModifier 1` (0.5 with Cover), `SuppressionBoostModifier 1` (1.5 with Suppression),
`CheckLineOfSight false`. Projectile side `WeaponSuppressionData`: 5.56 bullet `MaxMultiplier 7 / MinMultiplier 7`,
`MinDistance 3`, `MaxDistance 500`; rockets/40 mm/mortar 50 / 50 (D `weapons/common/bullets/556x45mmnato`,
`rpg7projectile`, `m224_projectile`). Explosions carry `VeniceExplosionEntityData.IsCausingSuppression`. Effects on the
victim: weapon spread via the weapon's `GunSwayData.SuppressionModifierUnzoomed/Zoomed` (S `GunSwayData.yaml`) and
screen blur via `VeniceFPSCameraData.SuppressionBlurAmountMultiplier 0.8` [V]; regen restart [V flag, I semantics].
Model [I]: each bullet passing within 1.5 m of a soldier's eye adds `multiplier × k` to a 0..1 suppression value
(boost of shooter × resist of victim), which holds for 2 s then decays 0.5/s. Score: `SuppressionAssist` 50 when a
suppressed target dies within `TimeSinceSuppressionLimit 2` s with `SuppressionAmountLimit 0.5` (§3.11).

### 1.7 Spotting [V]

| carrier | `SpottingComponentData` (spotter) | `SpottingTargetComponentData` (target) |
|---|---|---|
| soldier | `SpottingFov 4`°, `SpottingDistance 1200`, `TeamOrderFov 15`, `PassiveSpottingTimeInterval 1`, `TimeRequiredToPassiveSpot 1`, cool-down 3 spots per 3 s; orders: need-ammo < 50 %, heal < 60 %, repair < 75 %, ranges 30/30/50/50 m | `ActiveSpottedTime 7` s, `PassiveSpottedTime 1.5` s, `SpotOnFireMultiplier 0.45`, target point y +1.7 m |
| MAV | Fov 10, 1200 m, passive 2 s | active 15, passive 5, center of bbox |
| SOFLAM / EOD bot | Fov 5 | active 30 / 15 |
| M224 mortar | Fov 3 | active 12, passive 3, on-fire ×2 |
| T-UGS / radio beacon | – | active 15 / 30, passive 3 |

`SpotType` enum: None, Active, Passive, Radar, Unspottable; `SpottingType` adds `STSquadUav` (S). Explosions:
`UnspottableTime 1`, `SpotsOnExplode`/`UnspotsOnExplode` flags. Server settings `vars.3dSpotting` and
`vars.miniMapSpotting` toggled them [R] (preset table above). Semantics [I]: active spot = Q-key cone of 4° within
1200 m with line of sight → 3D marker + minimap for 7 s to the spotter's team; passive = minimap-only exposure.

### 1.8 Interaction, pickups, death [V]

`EntityInteractionComponentData`: `PickupRadius 3`, `SoldierInteractRadius 3` (revive/heal targets),
`MaxLookAtAngle 20`°, `OnlyAllowInteractionWithManDownSoldiers true`, timers 0.5 s. `DropWeaponComponentData`:
drops `DeathPickup` (`gameplay/pickups/defaultkitpickup` `KitPickupEntityData`: `TimeToLive 30`, `InteractionRadius 2.5`,
`ReplaceAllContent`, `KeepAmmoState`) 0.2 s after death — the BF3 kit pick-up [V].

---

## 2. Classes, kits, gadgets

### 2.1 How a kit is defined [V]

A kit is a `VeniceSoldierCustomizationAsset` (`gameplay/kits/{us,ru}{assault,engineer,support,recon}`; XP variants
`_gm`, `_gm_xp4`, `_xp4_scv` in XP bundles) with three `CustomizationTable`s: `WeaponTable`, `SpecializationTable`,
`VisualTable`, each a list of `CustomizationUnlockParts {UICategorySid, SelectableUnlocks[] → UnlockAssetBase}`.
**The `UnlockParts` index is the `WeaponSlot`** (0 primary, 1 sidearm, 2 gadget-1 launchers/recon tools, 3 unused,
4 kit item medkit/ammo bag, 5 gadget-2, 6 grenade, 7 knife) — confirmed by fun-bots' slot assignment
(`BotSpawner.lua:2019,2036–2038,2055,2067,2081,2095`) [V]. A weapon entry is a `SoldierWeaponUnlockAsset`
(`u_<weapon>`, `Weapon` → `SoldierWeaponBlueprint`); attachments/variants are `UnlockAsset`s listed in
`UnlockWeaponAndSlot.UnlockAssets` and applied as `WeaponModifierData` (P §2). The spawned loadout is a
`CustomizeSoldierData {Weapons[UnlockWeaponAndSlot], ActiveSlot, Unlocks}` (S) — the same record Gun Master uses.

### 2.2 The kits as shipped (patched install, `gameplay/kits/*`) [V]

| kit | slot 0 primary | slot 2 (UI "Gadget 1") | slot 4 | slot 5 (UI "Gadget 2") | 6 / 7 |
|---|---|---|---|---|---|
| Assault | 28 (ARs + shared shotguns/PDWs) | M320 HE/SHG/SMK/LVG, M26 MASS (+frag/slug/flechette), XBOW, XBOW scoped | **medkit** | **defibrillator**, none | M67 / knife, ACB-90 razor |
| Engineer | 26 (carbines …) | US: SMAW, FIM-92 Stinger, FGM-148 Javelin; RU: RPG-7, SA-18 Igla, Javelin; + XBOW ×2 | – | **repair tool**, M15 AT mine, EOD bot | same |
| Support | 26 (LMGs …) | – | **ammo box** | C4, claymore, M224 mortar, XBOW ×2, none | same |
| Recon | 26 (snipers …) | T-UGS, SOFLAM, MAV, XBOW ×2, none | – | **radio beacon** | same |

Bold marks the items of the minimum viable set (§2.5), not a fresh soldier's start: by the persistence progression
tables (`KitDef::default_unlock`) the defibrillator is a scoring-bucket unlock, so assault slot 5 starts as
`weapons/common/nogadget2`, and assault slot 2 (M320/M26 score-gated, the rest ammo variants) has no default [V].

Slot 1: 18 sidearms. Medkit (slot 4) and the M320/M26 (slot 2) share the UI "Gadget 1" choice — the player carries one
or the other ([Battlefield Wiki, Assault (BF3)](https://battlefield.fandom.com/wiki/Assault/Battlefield_3)) [R]; the data
keeps them in separate slots. XBOW = `weapons/xp4_crossbow_prototype/u_crossbow_scoped_cobra`/`_riflescope`
(resolved by GUID) [V].

### 2.3 Squad specializations [V]

Each is a `ValueUnlockAsset` pair L1 (personal) / L2 (squad: `UnlockUserData` → `VeniceUnlockUserData
{CheckSquad true}` = `persistence/unlocks/soldiers/specializations/squadsharing`). The soldier blueprint wires each pair
through `UnlockComponentData` → `FloatHub/IntHub` → a component field (D0 mpsoldier property connections, hashes resolved):

| UI name | asset ids | value | target field |
|---|---|---|---|
| Sprint | `SprintBoostL1/L2` | 1.1 | `SoldierBodyComponentData.SprintMultiplier` |
| Ammo | `AmmoBoostL1/L2` | 1.5 | `SoldierWeaponsComponentData.AmmoClipIncreaseMultiplier` |
| Explosives | `ExplosiveBoostL1/L2` | 2.0 | `SoldierWeaponsComponentData.ExplosiveIncreaseMultiplier` |
| Grenades | `GrenadeBoostL1/L2` (+`UnderslungBoost` 3) | +1 (+3) | `SoldierWeaponsComponentData.GrenadeIncrease` (+`UnderslungGrenadeIncrease`) |
| Flak | `ExplosiveResistL1/L2` | 0.85 | `SoldierEntityData.ExplosionDamageModifier` |
| Cover | `SuppressionResistL1/L2` | 0.5 | `SoldierSuppressionComponentData.SuppressionResistModifier` |
| Suppression | `SuppressionBoostL1/L2` | 1.5 | `SoldierSuppressionComponentData.SuppressionBoostModifier` |
| (cut) | `HealSpeedBoostL1/L2` | 0.8 | `SoldierEntityData.RegenerationDelayModifier` — wired but **absent from every kit's `SpecializationTable`** (15 entries = 7 pairs + `weapons/common/nospecialization`); fun-bots marks it "Not used" (`BotSpawner.lua:1808`) |

Squad rule [I/R]: L2 applies to every squad member while the owner lives; values do not stack.

### 2.4 Gadget mechanics (data) [V]

| gadget | partition(s) | mechanics in data |
|---|---|---|
| Medkit | `medicbag/medicbag`, `mediccrate_projectile` | thrown (`InitialSpeed 5`, spawn 0.26 s), attaches (`MaxAttachableInclination 25`); `SupplySphereEntityData.Healing {Radius 3.5, SupplyIncSpeed 12}` → heals soldiers in 3.5 m at 12 hp/s [I]; health 50, destructible; weapon `HealthPointsPerBag 100`, auto-replenish 1 s after the bag is used (`MagazineCapacity 1`, `AutoReplenishDelay 1`) |
| Defibrillator | `defibrillator/defibrillator` | instant-hit `BulletEntityData` 1.75 m reach [I from `InitialSpeed (0,0,1.75)`], `AttackTime 0.25`, `RateOfFire 60`, `ReloadTime 3.6` (recharge); revived soldier `PostReviveHealth 20` |
| Ammo box | `ammobag/ammobag`, `ammobag_projectile` | `Ammo {Radius 3.5, SupplyIncSpeed 1, SupplyPointsCapacity 7}`; per-weapon refill pacing via `AmmoConfigData.AmmoBagPickupDelayMultiplier` (C4 2.5, claymore 5, mortar 4.5) |
| Repair tool | `repairtool/repairtool` | instant-hit 6 dmg/shot `BulletEntityData` + `PowerToolWeaponData` (repair vs damage by target team); overheat in `OverHeat` [I] |
| M15 AT mine | `m15/m15`, `m15projectile` | 3 carried, `MaxCount 6` placed; `HasVehicleDetonation`, `VehicleDetonationRadius 3`, arm 4 s, `MinSpeedForMineActivation` on vehicles; blast 500 / inner 2 / radius 5 |
| EOD bot | `eodbot/eodbot` (+`_pda`) | **a `VehicleBlueprint`** (250 kg tracked, `GravityModifier 0.8`, 100 hp/zone) remote-entered from the PDA; repair-torch weapon 8 dmg/shot |
| C4 | `c4/c4`, `c4_projectile` | 3 carried, `MaxCount 6`; `fltDetonatedFiring`, `DetonatedWeaponData.Range 1500`; blast 500 / inner 1.5 / radius 5, impulse 6000; `TimeToLiveOnPlayerDeath 5` s; health 15 |
| Claymore | `claymore/claymore`, `claymore_projectile` | 1 carried (2 placed max), `SoldierDetonationData {Radius 1.5, Angle 60, UseAngle, MinSpeedForActivation 3.8, ActivationDelay 3}`; blast 112 / inner 1 / radius 4.3 after 0.15 s; removed 1 s after owner death |
| M224 mortar | `m224/m224`, `m224_deployed`, `m224_projectile` | deploys a `VehicleBlueprint` (`DeployInsideNeutralCombatAreaOnly`, `CheckClearSky`); shell = `MissileEntityData` `Damage 400`, `Gravity −9.8`, blast 60 / inner 2 / radius 5, `NearTargetDetonation 10` m; RoF 46/min [V]; artillery weapon `ArtilleryStrikeWeaponData` |
| MAV | `mav/mav`, `mav_pda` | `VehicleBlueprint` (rotor `PropellerEngineConfigData`), 50 hp/zone, `RadarSweepComponentData` 30 m every 3 s for controllables faster than 2.05 m/s and mines; laser lock (`LockOnRadar`, lock 0.6 s, 400 m) |
| T-UGS | `t-ugs/t-ugs`, `t-ugs_vehicle` | `VehicleBlueprint`, 25 hp/zone; radar sweep **30 m every 2 s, only targets moving > 2.05 m/s** (crouch-walk at 2.0 m/s is invisible [I]) + mines |
| SOFLAM | `soflam/soflam_pda`, `soflam_projectile` | `VehicleBlueprint` with a laser-painter weapon (`ShowLaserPaintedVehicles`); spotting Fov 5 |
| Radio beacon | `radiobeacon/radio_beacon`, `radiobeacon_projectile` | `VehicleBlueprint` + `MobileSpawnComponentData {ParachuteSpawnRadiusMin 2, Max 10, ParachuteSpawnDistance 100}`; 50 hp/zone; squad spawn point [I] |
| M320 HE | `m320/m320`, `40mmgrenade` | `BulletEntityData` `Gravity −7.2`, impact `StartDamage 100`, `TimeToArmExplosion 0.5` s (dud blast 56 before arming), blast 112 / inner 0.35 / radius 5; muzzle speed comes from the firing record's `ShotConfigData.InitialSpeed` (not read here) |
| RPG-7 / SMAW | `rpg7projectile`, `smaw_he_projectile` | `MissileEntityData` `Damage 225`, `EngineStrength 10`, `MaxSpeed 100`, `Gravity −1.5`, arm 0.5 s; blast 112 / inner 0.35 / radius 4 |
| Javelin | `fgm148/fgm148`, `fgm148_projectile(_direct)` | `FireOnlyWhenLockedOn`; lock 2.25 s on radar (vehicles, 250 m) or **1.5 s on laser-painted (500 m)**; top-attack path (`UnguidedData StaticPosition (50,50)`), guidance after 1.75 s, blast 350 / radius 4.5 |
| Stinger / Igla | `fim92a/fim92a`, `sa18igla/sa18igla` | heat lock 1.25 s, 400 m, `AcceptanceAngle 10` (Igla 5); missile `Damage 400`, `MaxSpeed 200`, `MaxTurnAngle 155`, proximity 12.5 m |
| M67 | `m67/m67_projectile` | fuse 3.6 s (`TimeToLive`, `DetonateOnTimeout`), bounce 0.3, blast 112 / inner 1.5 / radius 7 |

Kill/damage numbers on vehicles go through vehicle health zones and the material grid (handoff 04 §4); not repeated.

### 2.5 Progression stance and minimum viable kit set (recommendation)

* **Everything unlocked, no persistence** [I]: BF3's ranks (`persistence/ranks/rankparams`: 146 `RankLevelData`,
  0 → 24,600,000 points [V]), `UnlockScore`, `ScoringBucketUnlockData` and Battlelog stats are an online-service layer
  the reimplementation does not have. Read the kit tables as the menu, offer every `SelectableUnlocks` entry, store the
  player's loadout in `user://loadout.json`. Keep score/rank only per round (scoreboard). This matches BF3 server
  `vars.unlockMode all`-style behaviour [R, b3 plugin docs; not re-checked].
* **Minimum viable set** (exercise every mechanic class once): per faction one primary per kit + one pistol + knife +
  M67; Assault medkit + defib; Engineer RPG/SMAW + repair tool; Support ammo box + C4; Recon radio beacon. Second wave:
  M320/M26 (underslung = weapon modifier), claymore, M15, Javelin + SOFLAM (lock chain), Stinger/Igla (heat lock),
  MAV/T-UGS/EOD/mortar (gadget vehicles), XBOW. Specializations are four float/int multipliers — ship them in wave one.

---

## 3. Game modes

### 3.1 What is data and what is logic [V]

Mode logic is an event/property graph in prefabs (`gameplay/gamemodes/conquest|rush|deathmatch|domination`,
`gameplay/level_setups/complete_setup/full_*`, components such as `rushcrate`, `base_rush`, `capturepointprefab*`). The
**entities' own fields are readable constants**, and each level's mode sub-world (`levels/<lvl>/<mode>` `SubWorldData`)
sets per-level values on its `InterfaceDescriptorData` which `PropertyConnections` push into the prefab interface and
on to entity fields. Resolved examples [V]:

| level sub-world | interface value → target |
|---|---|
| `levels/mp_001/conquest` (CQ Small) | `InitialTicketCount 250` → both `TicketCounterEntityData.InitialTicketCount`; `AreaValue 51` |
| `levels/mp_001/conquest_large` | `InitialTicketCount 400`, `AreaValue 34` |
| `levels/mp_subway/conquest_small` | `InitialTicketCount 350`, `AreaValue 55` |
| `levels/mp_subway/rush` | `BaseCount 4`, attacker lives `1037616691 = 100` → `LifeCounterEntityData.StartingLifeCount`, MCOM arm time `−1787899064 = 2.5` → `GameInteractionEntityData.HoldToInteractTime` |
| `levels/mp_001/rush` | `BaseCount 4` only (lives default 75, arm default) |
| `levels/mp_001/squad_rush`, `mp_subway/squadrush` | `BaseCount 2`, lives 20, arm 5.0 s |
| `levels/xp2_palace/domination` | `InitialTicketCount 150`, `AreaValue 60`, `SpawnAlwaysAllowed true`, flag letters A/B/C |
| `levels/xp3_shield/tanksuperiority` | `InitialTicketCount 200`, `AreaValue 150` (one flag) |
| `levels/xp3_shield/conquestsmall0` | `InitialTicketCount 200`, `SpawnDelay 60`, `InitialSpawnDelay 15` (vehicle spawners), `1371456564 = 40` |
| `levels/xp5_001/air_superiority` | `InitialTicketCount 250`, `AreaValue 55`, `SpawnDelay 5`, `InitialSpawnDelay 2` |
| `levels/xp5_001/ctf` | `−449530114 = 3` (captures to win [I]), `−1887631159 = 1200.0` (round time s [I]), `−1224113362 = 20.0`, `55907915 = true` |
| `levels/xp4_quake/scavenger` | Team1 `InitialTicketCount 300` (via `998329935`), `AreaValue 40`, `CaptureRadius 4` |

So: initial tickets, attacker lives, base count, arm time, capture times/radii/multipliers, area values, kill limits,
Gun Master ladders, spawn protection radii, vehicle spawn delays, round-start player counts and auto-balance
thresholds are **readable**. Bleed arithmetic, capture progress, MCOM fuse/defuse flow, base advance, squad spawns,
scoring triggers, demotion, flag carrying are **game code to reimplement** (the graph only routes events between
engine-implemented entities). Implement a resolver that walks interface → `PropertyConnection` chains (handoff 04 §2.1
link walker) to compute each entity's effective constants; do not build an event-graph VM for modes.

### 3.2 Shared round machinery [V]

| entity | values | meaning |
|---|---|---|
| `PreRoundEntityData` | `RoundMinPlayerCount 4`, `RoundMaxPlayerCount 8`, `RoundRestartCountdown 30` (CQ/Rush/Dom), 10 (DM) | round starts at 4 players [I]; bots must count |
| `AutoTeamEntityData` | `TeamDifferenceToAutoBalance 3`, `PlayerCountNeededToAutoBalance 8`, `AutoBalance`, `RotateTeamOnNewRound` true; `TeamAssignMode` `TamEqualTeams` (CQ, Rush, Dom, CTF) / `TamFullTeams` (TDM, SQDM, GM) | |
| `RoundOverEntityData {Team}` | fired by `CompareEntityData LessOrEqual` on `TicketCounter.Tickets` (CQ) / lives (Rush) / kills (DM) | |
| `CombatAreaTriggerEntityData` | `TimeToReturn 10` (CQ), 3 / 10 (DM), `StrikeRadius 5`, `SpawnHeight 50`, `MaxRandomSpawnHeight 100`, `InitalSpeed 100`, `IsTeamSpecific` (CQ) | deserters are killed by a strike projectile from 50–100 m up [I] |
| `killer_camera` prefab | `DelayEntityData 4` s gated by setting `BFServer.IsKillerCameraEnabled` | kill cam lasts 4 s, then spawn screen [I] |
| `SettingEntityData` | `Server.VehicleSpawnAllowed` | infantry-only toggle |

Server-side knobs BF3 exposed [R] (preset table, admin guides): `vars.gameModeCounter` (ticket/kill-limit scale %),
`vars.playerRespawnTime` (%), `vars.playerManDownTime` (%), `vars.killCam`, `vars.3pCam`, `vars.onlySquadLeaderSpawn`,
`vars.friendlyFire`, `vars.bulletDamage`, `vars.roundLockdownCountdown`
([Fragnet admin guide](https://www.clients.fragnet.net/knowledgebase/46/BF3-Admin-Guide.html),
[MyRCON R-33 limits](https://myrcon.net/topic/86-bf3-bf3-server-limitations-r-33/)). Mirror them as server config.

### 3.3 Conquest (Small, Large, Assault)

Data [V] (`gameplay/gamemodes/conquest` D0, two `TicketCounterEntityData` `56bd48`/`735521`):
`TicketLossPerMin 15`, `TicketLossStart 100`, `TicketPercentages []`, `DecreaseTickets TDTOnRespawned`,
`TicketLossWhenLostAll 999`, `TicketLossWhenLostAllControlsPoint 999` (Team1) / 9999 (Team2, interface-driven for
Assault), `HaltTicketLossOnEqualPointCount true`, `SetBestSquadSpawner true`, `ResetCapturePointsOnReset true`;
`TicketCounterEntity.controlledAreaValue` is a runtime property (VU-Docs
`content\vext\ref\server\type\ticketcounterentity.md:25`). Capture points (`capturepointprefabflagpole`
`CapturePointEntityData`): `TimeToGetControl 20`, `TimeToLoseControl 20`, `AddedMultiplierPerPlayer 0.34`,
`MaxCaptureMultiplier 4`, `MinNrToTakeControl 1`, `SeesawCapturing true`, `HoistFlag`, `StartAtBottom`,
`EnemyTicketLossWhenCaptured 1`, `CaptureRadius 0` (use the `Geometry` polygon, handoff 04 §2.3), `AreaValue` from the
level. HQ (`capturepointprefab_hq`): `CTUnableToChangeTeam`, `HideRadius 50`, spawner `SpawnProtectionRadius 20`.
Flag counts: Bazaar CQS 3, CQL 5, Metro CQS 3 [V].

**Bleed rule (inferred from data, matches every level read):** `AreaValue × flags` is tuned so that holding a
**majority** pushes the holder's `controlledAreaValue` over `TicketLossStart 100`: Bazaar CQS 2 × 51 = 102,
CQL 3 × 34 = 102, Metro 2 × 55 = 110, Domination 2 × 60 = 120, Air Superiority 2 × 55 = 110, Tank Superiority
1 × 150 = 150; one flag fewer never exceeds 100 [I]. Proposed rule:
`if controlled(enemy) > TicketLossStart && controlled(enemy) > controlled(team)` → `team.tickets −= TicketLossPerMin/60`
per second (15/min = 1 ticket per 4 s), held at equal counts; a team with no points and no living soldiers drains at
`TicketLossWhenLostAll` (999/min) [I]; each respawn costs 1 ticket (`TDTOnRespawned`), so a revive saves the ticket
[I, matches community "medics can revive to prevent ticket loss" — [Battlefield Wiki, Tickets](https://battlefield.fandom.com/wiki/Tickets) [R]];
`EnemyTicketLossWhenCaptured 1` costs the enemy one ticket per capture [I]. Whether BF3 scales the rate with the flag
lead is **not in the data I read** (community says bleed "increases linearly" with flags held, [R], same page): measure.

Capture model [I]: progress in [−1, 1] per point; attackers present and outnumbering defenders move it at
`1/TimeToLoseControl` (owned → neutral) then `1/TimeToGetControl` (neutral → owned) per second ×
`min(MaxCaptureMultiplier, 1 + AddedMultiplierPerPlayer × (attackers − defenders − 1))`; equal numbers freeze it
(seesaw); with nobody present an incomplete capture returns at `ReturnMultiplier 1`. Capture scoring is per player
inside (`CaptureCapturePoint 250`, assist 200, neutralize 200/150, defend 25; §3.11).

Conquest Assault (XP1): only the attackers' HQ has spawns (handoff 04 §2.2); the interface overrides
`TicketLossWhenLostAllControlsPoint` [V wiring, I meaning: attackers holding no flag bleed out].

### 3.4 Rush and Squad Rush [V data, I flow]

`gameplay/gamemodes/rush` `LifeCounterEntityData`: attackers (`Team1`, `Attacker true`) `StartingLifeCount 75`
(level override 100 Metro, 20 Squad Rush), `ReinforceTable [1]`, `SimpleReinforce true` (lives refill each base [I]),
`FallbackTime 20`, `DefensiveKillRadius 10`, UI thresholds 10/20, `FightHarderTreshold 20`; defenders 9999 lives,
`BaseCount 5` default (levels set 4 / 2), `FallbackTime 30`. `full_rush_level`: 10 `rushcrate` + 5 `base_rush`
references. `rushcrate` (MCOM):

* Arm: `GameInteractionEntityData` Team1, `UseWithinRadius 1.5`, `UseWithinAngle 360`, `TestIfOccluded`,
  `HoldToInteractTime` = level arm time (2.5 s Rush / 5 s Squad Rush), `IET_RushCrateArm`.
* Disarm: Team2, radius 1.5, hold **5 s** (interface `1576431478`), `IET_RushCrateDisarm`.
* Fuse: arming starts a `DelayTriggerEntityData` of **25 s**, or **45 s** when interface bool `2021056095`
  (fed by `full_rush_level` field `842789575`, unset on Metro/Bazaar) is true; expiry closes the disarm gate and fires
  `Destroy` on the crate object [V graph]. The crate is also destructible by damage (`OnDestroyed`).
* Scores: armed 100, disarmed 200, destroyed 500, attack/defend kills near MCOM 25 each.

Flow [I]: base N's two MCOMs active; when both are destroyed, attackers' lives refill, base N+1 spawners and combat
areas enable (handoff 04 §2.2: 8 spawners `Enabled=false` at load, one attacker + one defender combat area per base),
defenders get `FallbackTime` to leave; attackers win after the last base, lose at 0 lives.

### 3.5 Team Deathmatch, Squad Deathmatch [V]

`full_teamdeathmatch`: two `KillCounterEntityData {EnemyWeight 1, MaxKillCount 100}`; `full_squaddeathmatch`: four
(Team1–4) with `MaxKillCount 50`. Spawning: `gameplay/gamemodes/deathmatch` `FriendZoneEntityData {EnemyWeight −2,
FriendlyWeight 1, CorpsWeight −2, CorpsTimeout 15, InitialZoneRandomized}` scores spawn points; spawners
`SpawnProtectionRadius 10`, `SpawnProtectionCheckAllTeams`, `TryToSpawnOutOfSight`, friendly-kill blocking 3 kills /
30 s. One shared point pool for both teams (handoff 04 §2.2). SQDM ends at the first squad-team to 50 [I].

### 3.6 Gun Master (XP2, XP4) [V]

`full_gunmaster` `GunMasterKillCounterEntityData.WeaponsPreset[9]`, each a ladder of `GunMasterLevelInfo
{CustomizeData → gameplay/xp2/gm_*, KillsNeeded}`. Preset 0 (17 levels): MP443, M93R, .44, PP-19, P90, SPAS-12,
Jackhammer, ACW-R, MTAR, AUG, SCAR-L, LSAT, L86, M417, JNG-90 (2 kills each), **M320 (1 kill), knife (1 kill)**.
Preset 1 = preset 0 reversed (finale kept); presets 2–8: 17/17/11/15/12/12/12 levels (CQB, heavy, pistols, snipers
with XBOW bolt, US/RU/EU-style sets; finales M320-GL, C4, knife, SMAW, RPG). Per-level `LevelOverrides`
(`CustomizeSoldierData`, premium variants) exist. `KillCounterEntityData EnemyWeight 10000`; teams `TamFullTeams`.
`GunMasterNotificationType`: LevelUp, **Demoted**, ReachedLevel, TookLead, LostLead (S) → knife kills demote [I/R].
Round ends on the first knife-level kill [I].

### 3.7 Conquest Domination (XP2) [V]

`gameplay/gamemodes/domination`: same ticket counters as conquest (15/min, start 100, 999), two HQ points with
`InitialOwnerTeam Team1/Team2`, TDM-style spawning (`FriendZoneEntityData`, `LocationTextSid ID_H_MP_DM_RANDOMSPAWN`,
spawn height +20 m from a point [I]); flags `capturepointprefab_nospawner`: `TimeToGetControl 2`, `TimeToLoseControl 8`,
`AddedMultiplierPerPlayer 0.34`, `EnemyTicketLossWhenCaptured 1`; level: 150 tickets, `AreaValue 60`, 3 flags,
`SpawnAlwaysAllowed true`.

### 3.8 Tank Superiority (XP3), Air Superiority (XP5) [V]

TS: one flag (`capturepointprefab_nospawner_xp3`: get 10 s, lose 5 s, 0.34/player), `AreaValue 150`, 200 tickets,
16 `VehicleSpawnReferenceObjectData` + 16 spawn points in `layer0_ts_logic`. AS: 3 flags
(`capturepointprefab_nospawner_as`: 4 s / 4 s, **0.5 per extra player**), 250 tickets, `AreaValue 55`, jets spawn in the
air (`dynamicspawn_xp_f18_spawninair_as`, `_su35_spawninair_as`), vehicle `SpawnDelay 5`, `InitialSpawnDelay 2`;
`layer0_as_gameplay` holds 30 references and 4 `SphereData` [V]. Capture from the air needs `IsCapturedInUpperSphere`/
geometry height [I].

### 3.9 Capture the Flag (XP5) [V/I]

`full_ctf_prototype`: two `FlagCounterEntityData {ObjectiveFlagCount 3}`, `ObjectiveEntityData`×2, 38
`TeamFilterEntityData`, `TrackPlayerEntityData` (carrier tracking); flag prefab `ctf_flag_team{1,2}_progressreturn`:
`GameInteractionEntityData` `IET_CTFFlag`, radius 3 (enemy pick-up) / 2.5 (own team, progress return),
`DelayBetweenUses 1`; bases `ctf_us_base`/`ctf_ru_base` with `GeometryTriggerEntityData` + `OBBData`. Level: 3 captures
[I], 1200 s round [I]. Scoring handler `CTFScoringHandlerData`: assist/threat radii 10 m, suppression timeout 10 s;
capture 500, pick-up 100, return 100, carrier kill 100 (§3.11). DICE described the return as a timer sped up by
friendlies in the radius ([MP1st](https://mp1st.com/news/battlefield-3-end-capture-the-flag-game-mode-detailed)) [R].

### 3.10 Scavenger (XP4) [V]

Conquest-like with `capturepointprefab` variants, `AreaValue 40`, 300 tickets, `CaptureRadius 4`; kits `_xp4_scv`;
weapon pick-ups `scavengerweaponpickup_tier1..3` = `WeaponUnlockPickupEntityData {InteractionRadius 2.5,
ForceWeaponSlotSelection, Weapons[] with fixed ammo (e.g. 62/42)}`; ammo crates `AmmoCrateEntityData {RefillDelay 10,
Radius 1.7}`; 48 references in `layer0_scavenger_logic`. 3 × 40 = 120 means only holding **all three** flags crosses
100 under the §3.3 model [I] — verify.

### 3.11 Scoring (`persistence/scoring/scoring`, 71 `ScoringTypeData`) [V]

| event | pts | event | pts |
|---|---|---|---|
| Kill | 100 | Kill assist | ×100 of damage share (`AdditionalValueMultiplier 100`, `KillAssistLimit 0.1`, timeout 10 s) |
| Headshot bonus | 10 | Double / triple / multi kill | 40 / 60 / 100 (`MultiKillTimeLimit 0.8` s) |
| Revive (team / squad) | 100 / 110 | Heal (team / squad) | 20 / 30 |
| Resupply (team / squad) | 10 / 20 | Repair (team / squad) | 10 / 20 |
| Spot assist | 10 | Motion-sensor / MAV spot assist | 20 |
| Suppression assist | 50 | Savior / avenger kill | 20 / 10 (timeouts 3 s) |
| Capture / assist (CQ) | 250 / 200 | Neutralize / assist (CQ) | 200 / 150 |
| Capture / assist (Dom) | 125 / 100 | Neutralize / assist (Dom) | 100 / 75 |
| Flag defend | 25 | Squad spawn on you | 10 |
| MCOM armed / disarmed / destroyed | 100 / 200 / 500 | MCOM attack / defend kill | 25 / 25 |
| Vehicle destroyed | 50 (+ assist ×100 of damage) | Vehicle disabled | 100 |
| Equipment destroyed / explosives destroyed | 20 / 20 | Laser tag / tagged-damage assist | 10 / ×100 |
| Driver / passenger assist (team / squad) | 50 / 70; 20 / 40 | Roadkill bonus | 10 |
| Squad orders (attack/defend, followed) | 20 | Squad wipe | 10 |
| CTF capture / assist / pick-up / return | 500 / 50 / 100 / 100 | CTF carrier kill / heal / savior | 100 / 50 / 50 |
| Nemesis kill/payback, kill-streak stop, comeback | 10 × count (limits 5/5/4) | Marksman headshot | 1 per m over 50 m [I] |

`DamageScoringHandlerData`: `TeamDamageLimit 0.5`, `TeamHealingLimit 0.2`, `RepairVehicleLimit 0.1`,
`DamageTeamVehicleLimit 0.5`, `VehicleDestroyAssistLimit 0.1`, `HotVehicleTimeout 10`. `PointSystemParamsAsset`:
`KillStreakX 6`, `NemesisKillX 5`, `DnfTimeout 60`.

### 3.12 Squads and spawning

* Squad size **4** [R: VU changelog "Change default squad size to 4", `C:\Users\olku\VU-Docs\content\general\changelog.md:438`];
  `GameModeTeamSize {playerCount, squadSize}` exists in the schema [V] but no instance was read.
* Squad spawn on a living squad member [R/I]; disabled inside `SquadSpawnDisabling` volumes
  (`gameplay/level_setups/components/squadspawndisabling`: `GeometryTriggerEntityData Include ATPlayers`, tooltips
  `…SQUADSPAWN_OFF_INSIDE_AREA` / `…ON_OUTSIDE_AREA`) [V]; hardcore `onlySquadLeaderSpawn` [R].
* Radio beacon = squad spawn point (`MobileSpawnComponentData`, 2–10 m scatter) [V data, I use]; vehicles with
  `UseAsSpawnPoint`/`UseAsTeamSpawner` spawn into seats via `CharacterSpawnReferenceObjectData.VehicleEntryIndex` (handoff 04 §2.2, §4.1).
* Spawner fields [V] (`SpawnReferenceObjectData`): `SpawnProtectionRadius` 10 (DM) / 20 (HQ) / 25 (flags),
  `SpawnProtectionFriendlyKilledCount 3` within `SpawnProtectionFriendlyKilledTime 30` s, `SpawnProtectionCheckAllTeams`,
  `TryToSpawnOutOfSight`, `ClearBangersOnSpawn`, `LockedTeam`. Rule [I]: a point is skipped while an enemy is within
  its radius or after 3 friendly deaths near it in 30 s; prefer out-of-sight points; after spawn 2 s immortality.
* Death → 4 s kill cam → spawn screen (flags/HQ/squad/beacon/vehicles); revive window 13 s [I]; respawn delay is a
  server percentage (`vars.playerRespawnTime 100`) whose absolute base was not found in data.

### 3.13 Readable vs. reimplemented (summary)

| readable from data (resolve through interfaces) | reimplement in C++ |
|---|---|
| tickets, AreaValue, TicketLossPerMin/Start/LostAll, kill limits, attacker lives, BaseCount, arm/defuse hold times, 25/45 s fuse, capture times/multipliers/radii/polygons, HQ ownership, spawn points/spawners/protection, combat areas, Gun Master ladders, scoring table, kit tables, specialization values, vehicle spawn delays, pre-round counts, auto-balance thresholds | bleed formula, capture progress, round start/end, base advance, MCOM arm/defuse state machine, deserter strike, spawn selection and squad/beacon spawns, revive, kill cam, scoring triggers and assist accounting, demotion, CTF carry/drop/return, auto-balance moves, spotting visibility, suppression accumulation |

---

## 4. C++ / Godot 4.8 design

### 4.1 Layering [I]

1. `bf3` (Godot-free): EBX → typed POD structs for `SoldierTuning` (§1), `KitTable`, `ModeConstants` (§3.13), scoring
   table, material-grid damage lookup. Cached in `user://cache/<content-digest>/` (binary), never in the install.
2. `bf3_sim` (Godot-free, fixed tick): soldier motor, health/damage, suppression, spotting, mode rules, scoring, spawn
   selection. Talks to collision through an interface `ICollisionWorld { sweepCapsule, overlapCapsule, ray }`.
3. `bf3_godot` (GDExtension): implements `ICollisionWorld` over `PhysicsServer3D`, owns nodes/visuals, input, HUD,
   networking transport, bots' NavigationServer3D queries.

### 4.2 Soldier controller: `CharacterBody3D::move_and_slide` vs. a custom motor

Verified API [V, API]: `CharacterBody3D.move_and_slide() -> bool` takes no delta; docs: it "uses the physics step's
delta value automatically" and belongs in `_physics_process()`
([class docs](https://docs.godotengine.org/en/latest/classes/class_characterbody3d.html)) [V]. State it keeps across calls:
`is_on_floor/wall/ceiling`, floor/wall normals, `get_platform_velocity`, `get_last_motion`, `get_real_velocity` — the
floor flags are read-only outputs [V]. `PhysicsBody3D.move_and_collide(motion, test_only, safe_margin,
recovery_as_collision, max_collisions)` and `test_move(from, motion, …)` exist [V]. Server level:
`PhysicsServer3D.body_test_motion(body, PhysicsTestMotionParameters3D, PhysicsTestMotionResult3D) -> bool` with
`from`, `motion`, `margin`, `max_collisions`, `collide_separation_ray`, `exclude_bodies/objects`,
`recovery_as_collision`; results `travel`, `remainder`, safe/unsafe fraction, per-collision point/normal/depth/collider
[V]. `PhysicsDirectSpaceState3D`: `intersect_ray/point/shape`, `cast_motion`, `collide_shape`, `get_rest_info`, and
**allocation-free `*_into` variants** with result objects (`PhysicsCastMotionResult3D`, `PhysicsGetRestInfoResult3D`,
`PhysicsIntersectShapeResult3D`, `PhysicsCollideShapeResult3D`, `PhysicsIntersectRayResult3D`) [V]. No Jolt class is
exposed; Jolt is reached only through these server calls [V]. Physics servers are not thread-safe unless *Run on
Separate Thread* is enabled ([thread-safe APIs](https://docs.godotengine.org/en/latest/tutorials/performance/thread_safe_apis.html)) [V];
`Engine.physics_ticks_per_second`, `SceneTree.physics_interpolation`, `Node.reset_physics_interpolation`,
`Node3D.get_global_transform_interpolated` exist [V].

| criterion | `move_and_slide` | custom motor over `body_test_motion` / `cast_motion` |
|---|---|---|
| rollback restore | must restore hidden state (floor flag used for snapping, platform data) that has no setters → first replayed step can differ [I] | state is a POD struct; restore = copy |
| replay N inputs per frame | possible only inside `_physics_process` with the fixed physics delta | any count, any thread that owns the query |
| BF3 behaviour (step 0.3 m, ascend 50°, ground hugging 0.5 m, poses, vault curves, ladders, swim, parachute) | slopes/snap only; the rest bolted on | all explicit, data-driven |
| bots (64 soldiers on a server) | one Node each, main thread | plain structs, batched |
| effort | low | moderate (≈ 600–900 lines) |

**Recommendation:** custom `SoldierMotor` in `bf3_sim`, collide-and-slide built on `body_test_motion` (one kinematic
body RID per soldier shape set, excluded self) behind `ICollisionWorld`; keep `CharacterBody3D` only for a throwaway
M3 prototype. If profiling or determinism demands, swap `ICollisionWorld` for an own BVH over the same static triangles
and hulls without touching the motor.

Motor step (fixed `dt`, e.g. 1/60 s) [I]:
1. Pose request → `overlapCapsule` with the target pose shape; enter after `TransitionTimes`; block if no room.
2. Target horizontal velocity = `Velocity(state,pose)` × direction constant (`SpeedModifier`) × `ThrottleModifierCurve(|stick|)`
   × (sprint ? `SprintMultiplier` × body `SprintMultiplier` (spec 1.1) : 1); approach with tuned gains (§1.3).
3. Jump (OnGround, not prone): `v_y = √(2 g JumpHeight)`, scaled by `JumpPenaltyFactor 0.3` within `JumpPenaltyTime 1 s`
   of the previous jump [I]; vault if a forward probe matches a `SpecialMoveVaultStateCriteria` window → play the
   `MotionPoints`/`GravityPoints` curve as a kinematic move (AnimationControlled state).
4. Integrate gravity; collide-and-slide ≤ 4 iterations; step-up ≤ `StepHeight 0.3` (up, forward, down); walkable if
   normal angle ≤ `MaxAscendAngle 50`°; snap down ≤ `AllowedDistanceFromGround 0.5` m when `GroundHugging`.
5. State transitions: water depth > `WaterDepthLimit 1.4` → Swimming; ladder volume + `LadderAcceptAngle` → Climbing;
   vertical speed past `FreeFallVelocity 16` → Falling (parachute allowed) [I]; landing → fall damage from `CollisionData`.

Shapes per pose (Godot `CapsuleShape3D.height` is the total height) [I]: stand radius 0.35 / height 1.80 (centre 0.90),
crouch 0.35 / 1.15 (centre 0.575), prone = sphere r 0.35 at 0.35 for sweeping plus a 0.6 × 0.35 × 2.4 m box
(`CollisionBox*Expand`) checked with `overlap` before entering/while turning prone. Eye heights 1.6 / 1.0 / 0.3. Hit
detection uses the §1.5 bone capsules (posed by the animation system or a pose-table fallback), never the movement
capsule. Expect spawn settling (handoff 04 §2.2: BF3 spawn points sit closer to walls than the hull).

### 4.3 Damage pipeline [I on the formula, V on the inputs]

`hit(projectile, zoneCapsule)`: `dmg = lerp(StartDamage, EndDamage, falloff(distance))` (bullet fields, P §3) ×
`grid.DamageProtectionMultiplier(projMat, zoneMat)` (§1.5) × `vars.bulletDamage` → `health −= dmg`; explosions:
`BlastDamage`/`InnerBlastRadius`/`BlastRadius` with occlusion rays, × `ExplosionDamageModifier`; fall damage from
`CollisionData`; regen after `RegenerationDelay × RegenerationDelayModifier`, reset by damage and by suppression ≥ low
threshold [I]. Server-authoritative with lag compensation (rewind other soldiers' bone capsules to the shooter's view
time); BF3 itself is widely described as client-hit-detected, unconfirmed
([search summary of forum posts](https://slo-tech.com/forum/t452664/p3966850)) [R] — do not copy that.

### 4.4 Game-mode framework (server only) [I]

```cpp
struct ModeConstants { /* resolved from §3.1: tickets, area values, kill limits, lives, base count,
                          arm/defuse/fuse, capture params per point, spawner params, scoring table */ };
class GameMode {                       // one per mode, pure C++, ticks at sim rate
public:
  virtual void on_round_start(World&) = 0;
  virtual void tick(World&, float dt) = 0;            // bleed, capture, timers
  virtual void on_kill(World&, const KillEvent&) = 0; // tickets/kills/GM ladder/scoring
  virtual void on_respawn(World&, PlayerId) = 0;      // TDTOnRespawned ticket cost
  virtual void on_interact(World&, PlayerId, InteractableId, InteractPhase) = 0; // MCOM, CTF flags
  virtual std::optional<TeamId> winner(const World&) const = 0;
};
// Conquest(+Small/Large/Assault/Domination/TankSup/AirSup/Scavenger share CapturePointSet + TicketCounter),
// Rush(SquadRush), Deathmatch(TDM/SQDM), GunMaster, CaptureTheFlag
```

Shared parts: `CapturePointSet` (polygon + height volume test per tick, §3.3 progress), `TicketCounter` (§3.3 rule
behind one function so the bleed variant is swappable after measurement), `LifeCounter`, `KillCounter`,
`SpawnDirector`, `Scoreboard` (event → `ScoringTypeData` lookup), `RoundFlow` (pre-round 4-player gate, lockdown,
end-of-round `EOR_Camera`, rotate teams), `AutoBalance` (diff ≥ 3 when ≥ 8 players; move dead players first [I]).

### 4.5 Spawn system [I]

Inputs: active spawners for the team (flags owned, HQ, Rush base N), squad members, beacons, vehicles with free seats.
For automatic choice (bots, "spawn on best"), score points: −∞ if an enemy is inside `SpawnProtectionRadius` or
3 friendly deaths in 30 s; + out-of-sight bonus (rays to known enemies); DM uses `FriendZone` weights (+1 per friend,
−2 per enemy, −2 per corpse younger than 15 s). After placement: capsule settle (rings, lift), 2 s immortality cancelled
by the listed inputs.

### 4.6 Spotting and suppression [I]

Spotting: server raycasts from the spotter's eye inside a 4° cone (soldier) to targets within 1200 m; on success the
target gets `spottedUntil[team] = now + ActiveSpottedTime` (7 s infantry, per-target data for gadgets/vehicles);
cool-down 3 per 3 s; passive exposure on firing unsuppressed for `PassiveSpottedTime × SpotOnFireMultiplier` [I].
T-UGS/MAV sweeps every 2/3 s mark controllables faster than 2.05 m/s within 30 m. Replicate spotted bits per team;
minimap markers are client-side from them.
Suppression: server tracks bullet near-misses within 1.5 m of a soldier's eye segment; per victim
`s = clamp(s + mult × boost × resist × k)`, decay 0.5/s after 2 s; send `s` quantised to the victim only (drives blur,
sway); score assist.

### 4.7 What is replicated [I]

| object | to whom | fields |
|---|---|---|
| soldier (others) | all in relevance range | pos (quantised mm), yaw/pitch (16 bit), velocity (for interp), pose + transition t, state (ground/air/swim/ladder/parachute/vault id+t), sprint/zoom/fire flags, active weapon slot + id, health bucket (for nametag), team/squad, `spottedByTeam` bits, man-down flag |
| soldier (own) | owner | authoritative pos/vel/state for reconciliation with last processed input sequence, exact health, ammo per slot, suppression, regen timer |
| inputs | client → server | sequence, move stick (2×i8), view (2×i16), buttons bitmask, target interactable id |
| mode state | all | tickets/lives/kills per team, flag owner + progress (i8) per point, MCOM state (idle/armed/fuse remaining), active base, CTF flag carrier/pos, GM level per player, round phase/timer |
| events | all / relevant | kill feed (killer, victim, weapon, headshot), score events (owner), revive, spot pings |

Godot transport: ENet via `ENetConnection`/`ENetPacketPeer` or a `MultiplayerPeerExtension` [V classes exist]; do not
use `MultiplayerSynchronizer` for soldiers (no prediction/rollback hooks; property-based) [I].

### 4.8 Bots hooks (naming parity with fun-bots) [V]

fun-bots: `BotKits {Assault 1, Engineer 2, Support 3, Recon 4}` (`ext\Shared\Constants\BotKits.lua:4–7`), weapon lists
per kit and faction (`ext\Shared\WeaponLists\CustomWeapons*{Us,Ru}.lua`), gadgets by unlock path (`Weapons/Gadgets/
Medicbag/U_Medkit`, `…/Ammobag/U_Ammobag`, `…/T-UGS/U_UGS`, `…/RadioBeacon/U_RadioBeacon`, `ext\Shared\WeaponList.lua:323–331`),
L2 specializations only (`BotSpawner.lua:1801–1808`), 185 waypoint maps in `mapfiles\`. Keep the same unlock paths as
loadout keys so bot loadout tables port directly.

---

## 5. Could not determine

* **Ticket bleed rate scaling**: data gives one `TicketLossPerMin 15` and empty `TicketPercentages`; whether BF3 scales
  with flag lead, and the exact role of `TicketLossStart`/`controlledAreaValue`, are inferred (§3.3). Needs a
  measurement (two observers, timed bleed at 2-of-3 and 3-of-3 flags).
* **Capture timing semantics**: whether `TimeToLoseControl` + `TimeToGetControl` add for a flip, the exact
  per-player multiplier formula, and contested behaviour.
* **Regen delay**: data 10 s vs community 16 s.
* Movement **gain semantics** (`AccelerationGain`, `DecelerationGain −15`, `SprintGain`), soldier gravity value, the
  meaning of `FreeFallVelocity 16` and `Falling` velocity 15, and the prone collision proxy.
* **Respawn delay base** (seconds behind `vars.playerRespawnTime 100`), revive window semantics (`ManDownStateTime 13`),
  charged-defib health, kill-cam/spawn-screen timing beyond the 4 s delay.
* Names of several interface hashes: `842789575` (45 s fuse switch), `1576431478` (disarm time), CTF
  `−449530114/−1887631159/−1224113362/55907915`, XP3 `1371456564`; values are known, meanings inferred.
* Squad-spawn conditions in BF3 (in-combat restriction or not), squad size per mode (`GameModeTeamSize` not read).
* Domination/TS/AS/CTF/Scavenger **level-specific** overrides beyond the one level read per mode; Conquest Assault
  values; XP1 maps not read.
* Gun Master demotion and round-end rules (only notification enums and ladders are in data); presets' official names.
* Blast falloff curve and cover/occlusion formula for explosions; how `DamageProtectionThreshold` and
  `DamagePenetrationMultiplier` apply.
* Suppression accumulation formula (how `WeaponSuppressionData` multipliers convert to the 0..1 value).
* BF3's own netcode facts (tick rate, hit registration) — only forum claims found.
* Community movement-speed tables for BF3 specifically (Symthic was unreachable from this machine, P §3); BF3 numbers
  above are from data only.

---

## Independent verification (2026-10-10)

**Method.** Values were re-read with a second, independent EBX reader: the EBX probe's `ebx-dump`, which uses the
IW4L `ebx_bf3`/`superbundle_bf3` readers rather than Rime and only reads the install. Its JSON dumps are in
`C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\verify-02-soldier-kits-modes\`
(**VD** below: subfolders `d0\`–`d3\`; helper scripts `q.py`, `conns.py`, `ev.py`, `grid.py`, `kits.py`). Field-id hashes
were resolved with the same djb2-xor rule against every VU-Docs schema field name. Godot claims were checked against
`research\godot-api\extension_api.json`, the engine source at the exact build
(`…\scratchpad\godot-engine\godot-src`, commit `c971f93e7`) and the docs.

| claim id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| C1 | One `SoldierBlueprint` 261e43bf for US and RU | confirmed | VD `d0\gameplay__teams__us.json` `TeamData 94535eb8` (`FactionUS`) and `…teams__ru.json` `da5d89a8` (`FactionRUS`) both have `Soldier` → `characters/soldiers/mpsoldier:261e43bf`. Their `SoldierCustomization` lists different kit assets (`gameplay/kits/us*` vs `ru*`). fun-bots `ext\Server\BotSpawner.lua:935` uses GUID `261E43BF-259B-41D2-BF3B-9AE4DDA96AD2` [V] | – |
| C2 | Movement constants (r 0.35; heights 1.8/1.15/0.3; eye 1.6/1.0/0.3; step 0.3; 50°; 4.0/2.0/0.9; ×0.75; 1.625; jump 0.6; unlimited sprint) | confirmed | VD `d0\characters__soldiers__defaultsoldierphysics.json`: `CharacterPhysicsData a10ff2aa` (`PhysicalRadius 0.35`, `MaxAscendAngle 50`, `Sprint` → `CharacterSprintData fac7c821` with `SprintPowerDecreasePerSecond 0` and `AllowContinousSprinting true`); `CharacterPoseData` 82d79307/4496651d/6c836a7a; OnGround pose infos 69a866a2 (4.0; back/left/right 0.75; `SprintMultiplier 1.625`), 79c79dfb (2.0) and 173494fc (0.9); `JumpStateData.JumpHeight 0.6` [V] | – |
| C3 | Health model: 100 hp; 10 s delay; rate 5; 2 s spawn immunity; man-down 13 s; post-revive 20; suppression aborts regen | confirmed (values) | VD `d0\characters__soldiers__mpsoldier.json`: `SoldierEntityData a9ffe6b4 MaxHealth 100`. `VeniceSoldierHealthModuleData 705967ee`: `RegenerationDelay 10`, `RegenerationRate 5`, `ImmortalTimeAfterSpawn 2`, `ManDownStateTime 13`, `PostReviveHealth 20`, `AbortSpawnImmortalityInputs [12,0,1,7,30,31,1,2,11]`. Per `VU-Docs\types\fb\EntryInputActionEnum.yaml` those inputs are Jump, Throttle, Strafe, Fire, MeleeAttack, ThrowGrenade, Brake and Zoom. `SoldierSuppressionComponentData.SuppressionAbortsHealthRegeneration true` [V]. In the schema `VeniceSoldierHealthModuleData.yaml` the fields span lines 36–51 (`regenerationRate` is line 51) | The meanings stay [I]: "5 hp/s" and "suppression restarts the delay". The schema citation should read lines 36–51 |
| C4 | Body-part multipliers live in `MaterialGridData`: 5.56 gives 2.0/1.0/0.91; 7.62 bolt gives head 2.35 and upper torso 1.25; rockets give 2.5 | confirmed | `grid.py` over VD `d0\levels__mp_001__mp_001__materialgrid_win32__grid.json` (111 rows; head 41, body 42, upper torso 106, legs 62) and the Metro grid (102 rows). Head/body/upper torso/legs: 5.56 2.0/1.0/1.0/0.91; 7.62 bolt 2.35/1.0/1.25/0.91; .338 2.35/1.0/1.06/0.91; RPG-7 2.5 in every zone; knife 0.67/0.5/0.5/0.5 (the knife `BulletEntityData` has `StartDamage 100`). The grid is symmetric. Zone materials come from `defaultsoldierbonecollision`: arms and lower Spine 6609a12f (body), upper Spine sphere 84454789, Head 8b81bca1, legs f44644a0 [V] | – |
| C5 | Kit `UnlockParts` index = `WeaponSlot` (0, 1, 2, 4, 5, 6, 7) | confirmed | `kits.py` over VD us{assault,engineer,support,recon} and ruengineer shows 8 `UnlockParts`: [3] always empty; [4] `u_medkit` (assault) or `u_ammobag` (support); [2] M320/M26/XBOW, SMAW/FIM92/FGM148 (US) or RPG7/SA18/FGM148 (RU), or UGS/SOFLAM/MAV; [5] defib, repair/M15/EOD, C4/claymore/M224, or radio beacon; [6] `u_m67`; [7] knives. fun-bots `BotSpawner.lua:2034–2038` puts the Assault/Support gadget 1 in `WeaponSlot_4` and every other kit's in `WeaponSlot_2` [V] | – |
| C6 | Specializations: 1.1/1.5/2.0/+1(+3)/0.85/0.5/1.5, their wiring, HealSpeedBoost unused | confirmed | VD `d0\persistence__unlocks__soldiers__specializations__*.json`, `UnlockedValue`: Sprint 1.1, Ammo 1.5, ExplosiveBoost 2.0, Grenade 1 plus Underslung 3, ExplosiveResist 0.85, SuppressionResist 0.5, SuppressionBoost 1.5, HealSpeed 0.8. `sprintboostl2.UnlockUserData` → `…/squadsharing`. `conns.py` on mpsoldier shows `FloatHub/IntHub.Out` → `SoldierBodyComponentData.SprintMultiplier`, `AmmoClipIncreaseMultiplier`, `ExplosiveIncreaseMultiplier`, `GrenadeIncrease`, `UnderslungGrenadeIncrease`, `SoldierEntityData.ExplosionDamageModifier`, `RegenerationDelayModifier`, `SuppressionBoost/ResistModifier`. healspeedboost is absent from every kit's 15-entry `SpecializationTable` [V] | – |
| C7 | MAV, T-UGS, SOFLAM, beacon, EOD bot and M224 are `VehicleBlueprint`s spawned by `VehicleProjectileEntityData` | confirmed | VD `d1\weapons__gadgets__*`. These primaries are `VehicleBlueprint`: mav/mav 2ee940b5, t-ugs_vehicle bdabc122, soflam_projectile 2a644984, radiobeacon_projectile aa301a25, eodbot 6ca1f97d, m224_deployed e5f87c27. Each is referenced by a `VehicleProjectileEntityData` in mav_pda, t-ugs, soflam_pda, radio_beacon, eodbot_pda or m224 [V] | – |
| C8 | Per-level mode constants sit on sub-world interfaces and reach entities through `PropertyConnections`; the logic is a graph | confirmed | `ifv.py`/`conns.py` on VD. mp_001 conquest 250/51 (3 `AreaValue` targets); conquest_large 400/34 (5); subway conquest_small 350/55 (3); subway rush `BaseCount 4`, `1037616691=100`, `−1787899064=2.5`; squad rush 2/20/5; palace domination 150/60; tank superiority 200/150; air superiority 250/55 with `SpawnDelay 5`; ctf `−449530114=3` and `−1887631159=1200`. In every conquest-family mode, `InitialTicketCount` → mode reference fields `998329935` and `998329932` [V] | Scavenger: the level's single field `998329935=300` feeds both `998329935` and `998329932` on the mode reference, so both teams start with 300, not just Team1 (extra-2) |
| C9 | Conquest ticket-counter and capture-point constants | confirmed | VD `d0\gameplay__gamemodes__conquest.json`, `TicketCounterEntityData` 56bd4848 (Team1) and 7355218d (Team2): `TicketLossPerMin 15`, `TicketLossStart 100`, `TDTOnRespawned`, `TicketLossWhenLostAll 999`, `…ControlsPoint 999/9999`, `HaltTicketLossOnEqualPointCount true`, `TicketPercentages []`. `capturepointprefabflagpole` `CapturePointEntityData 237dadc8`: 20/20, 0.34, 4, `MinNrToTakeControl 1`, `SeesawCapturing true`, `ReturnMultiplier 1` [V] | – |
| C10 | Bleed starts when the enemy's controlled area value exceeds 100, i.e. it holds the majority of flags; rate scaling unknown | confirmed (as inference) | Arithmetic re-derived from VD, counting flags as `AreaValue` targets: 2×51, 3×34, 2×55, 2×60, 1×150 and AS 2×55 all exceed 100, and one flag fewer never does. Scavenger fits too once its real flag count is used: 5 flags at 40, so 3×40 = 120 and 2×40 = 80 (extra-1). `controlledAreaValue` exists: `VU-Docs\content\vext\ref\server\type\ticketcounterentity.md:25` [V]. Community [R], via search summaries: bleed starts once a team holds more than half the flags, and Epicenter CQ at 2–2 drains nothing (Prima eGuide); the rate "increases linearly the more flags held" ([Battlefield Wiki, Tickets](https://battlefield.fandom.com/wiki/Tickets); the page itself returned HTTP 402) | The mechanism stays [I]. The "rate scaling unknown" item should cite the community claim that the rate scales with flags held, and keep the measurement plan |
| C11 | MCOM: arm 2.5 / 5 s, disarm 5 s, radius 1.5 m, fuse 25 / 45 s chosen by a bool; attacker lives 75 / 100 / 20; `SimpleReinforce` | confirmed | VD `d0\…components__rushcrate.json`: `GameInteractionEntityData 1768d76f` (Team1, `IET_RushCrateArm`, `HoldToInteractTime 2.5`, `UseWithinRadius 1.5`) is wired to interface `−1787899064`; `2cea23b6` (Team2, `IET_RushCrateDisarm`, 5.0) is wired to `1576431478`. `ev.py`: interface bool `2021056095` → `SyncedBoolEntityData 63074604`; OnTrue opens gate 349ac0d8 → `DelayTriggerEntityData 53793564` (45 s), OnFalse opens gate a6530b12 → `7abfa3a2` (25 s). `full_rush_level` field `842789575` → `2021056095` on the crate references. `gameplay/gamemodes/rush` `LifeCounterEntityData 7435dba1`: `Attacker true`, 75, `ReinforceTable [1.0]`, `SimpleReinforce true`; interface `1037616691` → its `StartingLifeCount` [V] | "Lives refill per base" stays [I]. Defender lives are effectively 999 (extra-3) |
| C12 | TDM 100; SQDM 50 per team ×4; Gun Master 9 presets, standard ladder 17 levels (15×2, then M320×1, knife×1) | confirmed | VD `full_teamdeathmatch`: `KillCounterEntityData` e671c2df/a3d6510c `MaxKillCount 100`. `full_squaddeathmatch`: 4 counters (Team1–4) at 50. `d2\…full_gunmaster.json`: `GunMasterKillCounterEntityData f0d72ae2` with 9 presets of 17/17/17/17/11/15/12/12/12 levels; preset 0 runs gm_mp443 … gm_jng90 at 2 kills each, then gm_m320 1, gm_knife 1. GM `KillCounterEntityData EnemyWeight 10000` [V] | – |
| C13 | 71 `ScoringTypeData`: kill 100, revive 100/110, CQ capture 250, MCOM 500, CTF 500, spot 10, suppression 50 | confirmed | VD `d0\persistence__scoring__scoring.json`: 71 `ScoringTypeData`, including `ID_SCORE_KILL 100`, `REVIVE 100`, `REVIVE_SQUAD 110`, `CAPTURE_FLAG 250` (125 in Dom), `CRATE_DESTROY 500`, `ID_XP5_SCORE_CAPTURE_FLAG 500`, `SPOT 10`, `SUPPRESSION_ASSIST 50`. `SuppressionScoringHandlerData` 0.5 / 2 s; `DamageScoringHandlerData` limits match §3.11 [V] | §3.11 lists "CTF … savior 50", but no CTF savior type exists. The CTF 50-point types are `FLAG_CARRIER_HEAL`, `FLAG_PROTECTOR(_ASSIST)` and `FLAG_CARRIER_KILL_ASSIST`; `DOGTAG_SAVIOR 50` is a generic type |
| C14 | `body_test_motion` with its params/result; space-state queries plus `*_into`; no Jolt classes; `move_and_slide()` takes no delta; floor state read-only | confirmed | `extension_api.json` (header 4.8 dev7, single precision): `PhysicsServer3D.body_test_motion(body, PhysicsTestMotionParameters3D, PhysicsTestMotionResult3D) → bool`. `PhysicsDirectSpaceState3D` has `intersect_ray/point/shape`, `cast_motion`, `collide_shape`, `get_rest_info` and all six `*_into` variants; all five result classes exist; no class name contains "jolt". `CharacterBody3D.move_and_slide()` has no arguments, and the only floor/platform calls are getters plus `apply_floor_snap`. [Class docs](https://docs.godotengine.org/en/latest/classes/class_characterbody3d.html): "uses the physics step's delta value automatically" [V] | – |
| C15 | A custom fixed-tick motor over `body_test_motion` beats `CharacterBody3D` for rollback and for 64-soldier servers | corrected | The rollback half holds: `character_body_3d.cpp:95` (`bool was_on_floor = collision_state.floor;`) drives snapping and has no setter, and line 45 picks the physics or process delta by `is_in_physics_frame()` [V source]. The threading/performance half is unsupported. Jolt refuses every query, `body_test_motion` included, while the space steps, and each query calls `flush_pending_objects()`, which mutates the space (`modules\jolt_physics\spaces\jolt_physics_direct_space_state_3d.cpp:452–454, 867`). The probe measured `body_test_motion` at 24–30 µs per call (`research\probes\godot-gdextension-probe.md:125`, 138–146). `move_and_slide` itself calls `body_test_motion` (`physics_body_3d.cpp:113`), so neither route has a cost advantage [V] | Keep the custom motor, for rollback and the BF3-specific states. Drop "any thread" and "plain structs, batched": motor queries must run on the physics thread, or in a worker window the game opens while no step runs. Budget about 64 soldiers × ~5 sweeps × 60 Hz ≈ 19k calls/s, which is up to ~0.5 core at the probe's long-sweep cost. Measure short sweeps before assuming headroom for 64 soldiers |
| C16 | Vault and dive are data-authored root-motion curves | confirmed | VD mpsoldier `SpecialMovesComponentData db892c47` has fields `VaultOverHighState`, `VaultUpHighState`, `VaultOverLowState` and `SprintToProneState` (type `SpecialMoveStateData`), plus 2 indexed variants of each vault. Motion and gravity curves match §1.3; for example the dive reaches (0,0,2.5) at 0.4 s and (0,0,3.0) at 0.6 s [V] | §1.3 says gravity switches on "after 0.4 s", but the onset differs per move: dive 0.3 s, VaultOverHigh 0.41 s, VaultUpHigh 0.5 s, VaultOverLow ramping 0.4→0.8 s |
| C17 | Regen delay: 10 s in the data vs 16 s on the wiki | confirmed | Data value 10 s [V, C3]. [Battlefield Wiki "Regeneration"](https://battlefield.fandom.com/wiki/Regeneration): 16 s, then 5 hp/s; 12 s after leaving medkit range. Read from a search summary, since the page itself returned HTTP 402 [R] | The community rate of 5 hp/s agrees with `RegenerationRate 5` |
| extra-1 | §3.10 Scavenger: "3 × 40 = 120 means only holding all three flags crosses 100"; "Conquest-like with `capturepointprefab` variants" | corrected | VD `d3\levels__xp4_quake__scavenger__layer0_scavenger_logic.json`: `AreaValue` and `CaptureRadius` go to **5** `ReferenceObjectData` → `capturepointprefab_nospawner` (`CapturePointEntityData ccbdc747`: get 2 s, lose 8 s, 0.34, the Domination flag), and the mode reference 6e5f7ef1 → `gameplay/gamemodes/domination` [V]. Community [R], via search summary of Prima/MP1st: Scavenger has five flags, and holding three starts the drain | Scavenger runs Domination logic with 5 Domination-type flags. Holding the 3-flag majority gives 3 × 40 = 120 > 100 and 2 flags give 80, so it fits the §3.3 majority rule like every other mode |
| extra-2 | §3.1 row `xp4_quake/scavenger`: "Team1 `InitialTicketCount 300` (via `998329935`)" | corrected | `conns.py` on VD `d2\levels__xp4_quake__scavenger.json`: interface `998329935` → mode reference fields `998329935` **and** `998329932`, the same pair that conquest's `InitialTicketCount` drives [V] | Both teams start with 300 |
| extra-3 | §3.4 "defenders 9999 lives" | corrected | VD `full_rush_level`: interface `1037616688` = `Int32 999` → `LogicReferenceObjectData 1580b7f7` (`gameplay/gamemodes/rush`) field `1037616688` → `LifeCounterEntityData 8326cbce` (`Attacker false`) `StartingLifeCount` [V] | Defenders' effective `StartingLifeCount` is 999; the entity default of 9999 is overridden. Either way it is effectively unlimited |
| extra-4 | §3.12 / §5: squad size only [R]; "`GameModeTeamSize` … no instance was read" | resolved | VD `d3\levels__mp_subway__mp_subway__settings_win32.json`: `GameModeSettings` → one `GameModeInformation` per platform. Win32 `GameModeSize.PlayerCount`: CQS 32, RL 32 (`RoundsPerMap 2`), CQL 64, TDM 32, SDM 16 (4 teams × 4), SR 8, CQAS 32, CQAL 64, DOM 16, GM 16, TDMC 16, TS 24, SCV 32, CTF 32, AS 24. Every `GameModeTeamSize.SquadSize` is 4; PS3/Xenon entries with 24-player sizes are also present [V] | Squad size is 4 in every mode [V]. Per-mode player counts can be read from `levels/<lvl>/<lvl>/settings_win32`; only Metro's was read |
| extra-5 | §4.2 table: the custom motor replays "any count, any thread that owns the query" | corrected | Same evidence as C15: the Jolt stepping guard plus `flush_pending_objects`, and the probe's race test, in which 74–77 queries were refused over the run [V] | "Any count" holds; "any thread" does not. Queries are safe only inside a no-step window the game controls |
| extra-6 | Core principle: BF3 files are read as-is | no violation | The recommendations read EBX in place and cache in `user://cache/<content-digest>/` (§4.1); the loadout goes to `user://loadout.json` (§2.5); Rime ran from a scratch copy (§0). The "measure in BF3" items (§1.2, §5) are observations of the retail game for reference, which the principle allows. Nothing patches, repacks or hooks the files, and nothing runs `bf3.exe` as the delivery route [V, report text] | – |

**Could not determine (verifier).**
* What `TicketLossStart` and `controlledAreaValue` mean, and whether bleed scales with the flag lead. No primary source was found: the fandom pages return HTTP 402 to the fetcher and web.archive.org is blocked from this machine. A timed in-game measurement is still needed.
* Whether an interface field with no value pushes a default. This decides two things: whether Bazaar Rush uses the prefab's 2.5 s arm time, and whether the disarm field `1576431478` ever overrides 5 s. In `full_rush_level` that field is wired to only 2 of the 10 crate references and has no value.
* What `body_test_motion` costs for short per-tick sweeps against real level collision. The probe's 24–30 µs was a 10 m drop onto a 100k-triangle mesh. Whether a custom motor is cheaper than `move_and_slide` was not tested.
* Whether the `GameModeSettings` in `settings_win32` differ per level. Only Metro's was read.
* Why the data's 10 s regen delay differs from the community's 16 s: possibly "delay" vs visible regen onset, or a patch change. Not measured.
