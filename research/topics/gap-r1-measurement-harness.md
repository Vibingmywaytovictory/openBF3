# Gap R1: the measurement pass behind R3/R5, designed as a VU instrumentation harness (knowledge only)

**Date** 2026-10-10. **Status** designed and feasibility-checked, **not run**: I did not start VU, BF3, the EA app or
any server. I wrote nothing into `C:\Program Files\EA Games\Battlefield 3`. The only commands that touched it were
read-only metadata listings and SHA-256 reads (§4.2). **Scope** closes the completeness-review gap "the measurement pass
behind R3/R5 is unplanned and unchecked". It covers `PORTING-RESEARCH.md` §9 items 1–6 (lines 960–969), the "Open"
lines of §3.1–§3.7 (318–320, 390–396, 474–476, 547–550, 605–607, 661–663, 709–711), R3 (783), R5 (785) and the
§6 measurement row (820). This file is the topic the task brief calls `13-measurement-harness`; it was written under the
output name the brief asked for.

**The principle, applied here.** Venice Unleashed (VU) runs the original BF3 binaries, so it is a knowledge source and
never the product route (`08-prior-art-legal.md` §1.1). The harness is a private VU server mod (Lua) in the user's VU
server-instance folder. It observes the running game and writes **numbers only** into its own log. An extractor
outside VU copies those rows to `research\measurements`. Nothing in the harness, its logs or VU itself enters the
product. The product stays C++ reading the install as-is. The harness's Lua is research tooling, like `probes/tools`,
and is never shipped.

**Evidence tags.** **[verified]** = I checked it in VU-Docs, local files or logs, code, or a primary source I read.
**[report claim]** = stated by a source I did not re-check. **[inferred]** = my reading or design. Paths without a drive
are relative to `C:\Users\olku\VU-Docs\content\vext\ref\` unless noted. VU's Lua API spells fields in lowerCamel, as in
`gunSway.dispersionAngle`. Frostbite data fields are written PascalCase, as in `GunSwayData.FirstShotRecoilMultiplier`,
matching the EBX descriptors.

---

## 0. Conclusions in one screen

1. **Feasible now, on this PC.** VU client and server build 20682 (VeniceEXT 1.15.0) are installed at
   `%LOCALAPPDATA%\VeniceUnleashed\client`. A server instance exists at
   `C:\Users\olku\Documents\Battlefield 3\Server` with `server.key` (64 B; contents not read), `Admin\`, `Mods\` and
   `logs\`. The 2026-06-28 server log shows a full start: Zeus connection, authentication, game registration. A client
   on the same PC (LAN IP 192.168.10.88) then joined. Start-menu shortcuts for 30/60/120 Hz servers and "Activate BF3"
   exist [verified: `...\Server\logs\vu_2026-06-28_15-19-10_0767.log`:89–110,
   `%LOCALAPPDATA%\VeniceUnleashed\vu_2026-06-28_15-23-30_0510.log`:3–23, shortcut targets read via WScript.Shell].
   The open question was whether the user can run it at all. That is settled: they already have.
2. **VU did not write into the install.** All 2,101 items under the BF3 root have a newest LastWriteTime of
   2026-04-25T17:22:36 and a newest CreationTime of 2026-04-25T17:20:47 (the EA-app install). The VU sessions of
   2026-06-27/28 left no trace. Zero items were written after 2026-04-26 [verified, metadata census 2026-10-10]. This
   agrees with VU's FAQ: VU "does not modify any game files" [report claim, https://veniceunleashed.net/about]. §4.2
   gives a tested before/after script for every future session.
3. **VU exposes what R5 needs.** The `GunSway` fields are per-update, on server and client: dispersion, recoil, random
   angle/radius, seed, first-shot index. The other observables are `Soldier:Damage` (pre/post health),
   `BulletEntity:Collision`, `TicketCounterEntity.ticketCount/controlledAreaValue`, a **float**
   `TicketManager:GetTicketCount`, and `CapturePointEntity.flagLocation/flagVelocity/playersInside`. For vehicles there
   are velocity, angular velocity and per-part transforms (turrets), plus `Vehicle:Damage`. Server-side test bots
   driven through `EntryInput` give exact, repeatable input profiles [verified docs; existence also confirmed by
   independent use in fun-bots code, §8].
4. **What VU does not expose:** engine rpm, gear and clutch state (no runtime property in any VU type; grep negative),
   the 1p viewmodel `RenderFov`, BF3's force laws as such (only their effects), Ant graph and rig-op internals, and
   navigation. §1.2 lists them with their oracle.
5. **Contamination risk.** The user's current instance loads `unfun-bots`, `SelectableUnlocks` and `VU-Noclip-master`
   and sets `vu.SuppressionMultiplier` [verified `Admin\ModList.txt`, `Startup.txt` keys]. unfun-bots rewrites game
   data in memory: `SyncedGameSettings.allowClientSideDamageArbitration` around bot bullets,
   `ServerSettings.isRenderDamageEvents`, the stationary-AA `FiringFunctionData` dispersion/heat, and
   `AutoTeamEntityData` [verified `C:\Users\olku\unfun-bots\ext\Server\__init__.lua`:595–614, 708–752]. Measurements
   therefore need a **separate, clean instance** with only the harness loaded and a neutral `Startup.txt` (§3.1).
6. **Design.** Observe-only (no data edits, hooks pass through). Own test bots through VU's documented API, never
   fun-bots code. 30 Hz (BF3's own rate) for everything, with a 60 Hz repeat of the weapon and movement items to expose
   tick coupling. One JSON row per tick or event, printed to the instance log, extracted into `research\measurements`.
7. **Cost.** About 0.6–0.8 agent slice (harness about 700–900 Lua lines, extractor and analysis about 600 Python
   lines) plus about 4–5 h of the user's attended time and 5–6 h unattended, over three sessions. The §6 plan said
   "(user, ~1)" with no agent share and no session split (§7).
8. **Order change.** The pass does not depend on any of our code. Run **Session 1 (weapons, soldier, modes) during
   M0**, not "before M2 tuning". The soldier-gain and gravity results feed M1's motor. Run Session 2 (ground vehicles)
   before M4 and Session 3 (air) before M5 (§7).
9. **If VU becomes unavailable** (Zeus down, activation lost), no single fallback replaces it. The archived Symthic
   threads say spread was assumed "a random place in that circle" and recoil decrease was "odd" and "under
   investigation" (2012). They do not settle the disc or the recovery law. Video frame counting can time bleed, capture
   and vehicle speeds but not spread internals. Retail BF3 adds no observables (§6).

---

## 1. Open items and what observes each

### 1.1 Mapping table

Priority: **P0** = decides a swappable function or oracle before the milestone that needs it; **P1** = an oracle for a
later milestone; **P2/P3** = cheap or low-impact confirmation. Scenario ids refer to §3.5.

| id | open item (source line in PORTING-RESEARCH.md) | VU observable(s) [verified existence; semantics tagged in §2] | scen. | settles | prio |
|---|---|---|---|---|---|
| W1 | spread disc distribution (§9.1 l.961; §3.2 l.391; R5) | `GunSway.randomAngle`, `randomRadius`, `seed`, `dispersionAngle`, `currentDispersionDeviation{pitch,yaw}` per shot (`shared/type/gunsway.md`:16–27); impact points via server hook `BulletEntity:Collision` → `RayCastHit.position` (`server/hook/bulletentity_collision.md`:5, `shared/type/raycasthit.md`:11) | W-A | D7 "disc sampling switchable until measured" (l.367); R5 | P0 |
| W2 | spread growth/decay, minimum rise on state change, `GunSwayStanceTransition` application; `DecreasePerSecond` 15 (data) vs 8 (community) (C14 l.934; §3.2 l.391–392) | `GunSway.dispersionAngle`, `minDispersionAngle`, `isMoving`, `isJumping`, `jumpingTimer`, `suppressionMinDispersionAngleFactor` per tick; soldier `pose`/`pendingPose` (`server/type/soldierentity.md`:41–42) | W-A, S-B | D7 spread state machine | P0 |
| W3 | recoil recovery: `RecoilAmplitudeDecreaseFactor` as a rate or exponential; `ShootingRecoilDecreaseScale 0.1`; horizontal recoil shot-to-shot correlation (§9.1; §3.2 l.390) | `GunSway.currentRecoilDeviation{pitch,yaw,roll,transY}` per tick (`gunsway.md`:14, `gunswaydeviation.md`:17–20); `cameraRecoilTransform` (:13); `WeaponFiring.recoilTimer`, `recoilAngleX/Y/Z` (`weaponfiring.md`:21–24) | W-A | D7 recoil law | P0 |
| W4 | when `FirstShotRecoilMultiplier` re-arms (§9.1) | recoil increment of shot *k* vs idle gap; `GunSway.initialFireShot`, `timeSinceLastShot`, `fireShot` (`gunsway.md`:17, 28–29) | W-A | D7 first-shot flag | P0 |
| W5 | falloff shape and distance basis; M98B end 150 (data) vs 120 m (community) (§9.2 l.963; §3.2 l.393; C14) | hook `Soldier:Damage(soldier, info, giverInfo)` (`server/hook/soldier_damage.md`:5) with `DamageInfo.damage`, `position`, `origin`, `direction`, `boneIndex`, `isBulletDamage`, `isClientDamage` (`server/type/damageinfo.md`:17–33); health before/after `hookCtx:Call()`; flight time from projectile creation to collision | W-B | R5; §7.1 shots-to-kill oracle | P0 |
| W6 | `DamageProtectionThreshold`, `DamagePenetrationMultiplier`; whether bullets pass through soldiers (§3.2 l.392–393) | as W5, with two targets in line and targets behind penetrable cover; `RayCastHit.material` (`raycasthit.md`:14) | W-B | penetration rule in D7 | P1 |
| W7 | RPG direct `MissileEntityData.Damage 225` vs community 100; blast falloff (§9.4 l.965; §3.2 l.395) | `Soldier:Damage` with `isExplosionDamage`, `damage`; projectile tracking via hook `EntityFactory:Create` → `ServerMissileEntity`/`ServerGrenadeEntity` (`server/hook/entityfactory_create.md`:5; `..\entities.md`:2124, 2313) | W-C | explosion model, §7.1 RPG row | P1 |
| W8 | `ClientFireRateMultiplier 0.67` (§3.2 l.394) | per-shot timestamps, client (`predictedWeaponFiring`/`correctedWeaponFiring`, `client/type/soldierweapon.md`:23–24) vs server (bullet creation) | W-D | fire-sim timing in D7 | P2 |
| W9 | `ReloadThreshold` semantics (§3.2 l.394) | `WeaponFiring.weaponState` transitions (`shared/type/weaponstate.md`:9–24), `SoldierWeapon.primaryAmmo` (`server/type/soldierweapon.md`:25), event `Player:Reload` (`server/event/player_reload.md`:5) | W-D | reload state machine, 1p clip sync | P2 |
| W10 | ADS zoom FOV, and whether `FieldOfView`/`ZoomRenderFov` are vertical (§9.6 l.969; §3.2 l.396) | client `ClientUtils:WorldToScreen`, `GetCameraTransform`, `GetWindowSize` (`client/library/clientutils.md`:11–14); `AimingSimulation.fovMultiplier`, `zoomLevel` (`client/type/aimingsimulation.md`:19, 24). The viewmodel `RenderFov 55` is **not** observable (§1.2) | W-D | D13 camera FOV | P2 |
| S1 | regen delay 10 s (data) vs 16 s (community); rate; `SuppressionAbortsHealthRegeneration` (§3.1 l.319; §9.3; C14) | `SoldierEntity.health` per tick (`soldierentity.md`:39); `Soldier:HealthAction` (`server/event/soldier_healthaction.md`:5) | S-A | §7.2 oracle; C14 | P0 |
| S2 | `ImmortalTimeAfterSpawn 2` and the inputs that abort it | `Soldier:Damage` pre/post at known times after `Player:Respawn` | S-A | spawn rules | P2 |
| S3 | `AccelerationGain 0.35`, `DecelerationGain −15`, `SprintGain`; soldier gravity; `FreeFallVelocity` and the Falling velocity; fall damage vs speed (§3.1 l.318–319) | soldier velocity (`PhysicsEntity.velocity`, `server/type/physicsentity.md`:23; the soldier inherits it, `soldierentity.md`:5), `SpatialEntity.transform`; `DamageInfo.collisionSpeed` (`damageinfo.md`:26) | S-B | D6 motor (M1 exit criteria) | P0 |
| S4 | respawn delay base behind `vars.playerRespawnTime` (§3.1 l.320; §3.7 l.711) | `Player:Killed` → `Player.isAllowedToSpawn` flip and `Player:Respawn` (`server/type/player.md`:26; `server/event/player_respawn.md`:5) | S-A | spawn director | P3 |
| M1 | bleed scaling; `TicketLossStart` and `controlledAreaValue` semantics (§3.7 l.709; §9.3 l.964; D12 bleed rule l.679–682) | `TicketCounterEntity.ticketCount`, `controlledAreaValue` (`server/type/ticketcounterentity.md`:23–25); `TicketManager:GetTicketCount(team)` → **float** (`server/library/ticketmanager.md`:12); entity type `ServerTicketCounterEntity` (`..\entities.md`:2748) | M-A | D12 bleed function; R5 | P0 |
| M2 | capture timing with 1/2/4 attackers; is a flip lose + get; seesaw, return (§3.7 l.710; §9.3) | `CapturePointEntity.flagLocation`, `flagVelocity`, `team`, `attackingTeam`, `isControlled`, `isAttacked`, `playersInside`, `cooldownTimer`, `team1/2CooldownPenalty` (`server/type/capturepointentity.md`:22–38); events `CapturePoint:Captured/Lost`, `Player:Entered/ExitedCapturePoint` | M-B | D12 capture function; R5 | P0 |
| M3 | interface fields with no value: Bazaar Rush arm time, disarm override (§3.7 l.710–711) | MCOM entity event callbacks (`EntityBusPeer:RegisterEventCallback`, `shared/type/entitybuspeer.md`:29); `LifeCounterEntity` fields (`server/type/lifecounterentity.md`:22–30) | M-C | ModeConstants resolver defaults | P3 |
| M4 | Gun Master demotion; squad-spawn conditions (§3.7 l.711) | `Player:Score`, `ScoringSystem:StatEvent`, `Player:SpawnOnPlayer` events | M-C | GM and spawn rules | P3 |
| V1 | wheeled: 0–60, top speed, braking, full-lock circle; `ForwardGearSpeeds` and `MaxVelocity` units (§9.5 l.966; §3.4 l.547; R3) | vehicle `PhysicsEntity.velocity`, `PhysicsEntityBase.linearVelocity/angularVelocity` (`shared/type/physicsentitybase.md`:23–24), transform; driver-bot `EntryInput` levels (`shared/type/entryinput.md`:17–32). Gear changes only as acceleration kinks (no gear property) | V-A | R3; M4 oracle replaces the IW4L report claim | P0 |
| V2 | tracked: T-90 vs M1A2 top speed, pivot rate, `TrackedTurnSpeedLimit` (§9.5) | as V1 | V-A | R3 | P0 |
| V3 | turret traverse and elevation, controller law (`AngularMomentum*`), `WorldSpaceLockEfficiency` (§9.5 l.968; §3.4 l.548) | `PhysicsEntityBase:GetPartTransform(partId)` → `QuatTransform` per part (`physicsentitybase.md`:43), `partCount` (:28) | V-B | turret model | P0 |
| V4 | damage-angle function, `DamageProtectionThreshold`, splash across zones, reactive armour; `VelocityDamageThreshold` units (§9.4; §3.4 l.547–549; C20) | event `Vehicle:Damage(vehicle, damage, info)` (`server/event/vehicle_damage.md`:5); `PhysicsEntity.internalHealth` (`physicsentity.md`:22); `RegisterDamageCallback` / `RegisterCollisionCallback` → `CollisionInfo.speed` (`physicsentity.md`:37–42, `server/type/collisioninfo.md`:22); `Vehicle:Disabled` | V-C | vehicle damage formula; R5 (reactive armour) | P1 |
| V5 | helicopter hover sink at idle collective, climb, speed at 25°/36° nose-down, rotor spool (§9.5; §3.4 l.548) | as V1 plus attitude from the transform; pilot inputs | A-A | R3 (M5) | P1 |
| V6 | jet turn rate vs speed (speed-axis units), stall, `PowerFadeOutRange` (§9.5 l.967; §3.4 l.547) | as V5 | A-B | R3; flap-curve axis units | P1 |
| V7 | boat planing speed and turn; amphibious transitions | as V1 | A-C | R3 | P2 |
| V8 | AC-130 orbit logic (§3.4 l.549–550) | AC-130 transform over time (radius, altitude, period) | A-C | gadget-vehicle model | P3 |
| N1 | effective state rate; BF3 interpolation delay (§3.5 l.605) | client-side transform cadence of a remote bot vs the server's per-tick log; step-move cross-correlation | N-A | knowledge only (D4/D10 are decided) | P3 |
| T1 | tick coupling of any of the above (D4; C5 l.925; VU changelog 20056) | the W-A/S-B rows repeated on a `-high60` server; `SharedUtils:GetTickrate()` (`shared/library/sharedutils.md`:21) | W-A60, S-B60 | whether "every rate per second" reproduces BF3 | P0 |

### 1.2 Not observable through VU, and the oracle instead

| item | why not | oracle |
|---|---|---|
| engine rpm, gear, clutch (vehicle audio, `ForwardGearSpeeds` directly) | no such property in any VU runtime type: grep for rpm/gear/engine over `server\`, `client\`, `shared\` finds only unrelated pages [verified] | acceleration kinks in V-A; optional discovery probe: `RegisterPropertyChangedCallback` on every peer of the vehicle bus (`entitybuspeer.md`:38) to log property names while driving [inferred, may yield nothing] |
| 1p viewmodel `RenderFov 55` | the viewmodel camera has no VU accessor; `WorldToScreen` uses the world camera [inferred] | user screenshots at fixed ADS/hip poses vs our render |
| Ant graph, rig ops, VBR, `TrimOffset` (§3.3 open) | animation internals are not exposed; `RagdollComponent` (client) only | the Ant probe's data route |
| nav items (§3.6 open) | BF3 MP has no nav data; VU adds none | Detour work (topic 06) |
| weapon sound-graph environment selection | not exposed | listening tests |
| real bandwidth | not a script quantity | OS counters on a two-PC LAN, optional |
| §3.5 Linux symbol clash, `CharacterVirtual` replay | our engine, not BF3 | S0/S2 spikes |

---

## 2. VU runtime surface used by the harness (inventory)

"RO" = marked read-only in VU-Docs. The `prop` shortcode's second argument renders "Read-only property"
(`C:\Users\olku\VU-Docs\layouts\shortcodes\prop.html`) [verified]. Field semantics are **[inferred] from names**: the
yaml sources (`C:\Users\olku\VU-Docs\types\shared\type\GunSway.yaml`) carry types only, no descriptions [verified].

| realm | API | gives | doc |
|---|---|---|---|
| shared | events `GunSway:Update`, `GunSway:UpdateRecoil` `(gunSway, weapon, weaponFiring, deltaTime)` | per-update hook into every weapon's sway state, on server and client | `shared/event/gunsway_update.md`:5; `gunsway_updaterecoil.md`:5 |
| shared | `GunSway`: `dispersionAngle`, `minDispersionAngle`, `currentRecoilDeviation`, `currentDispersionDeviation`, `currentLagDeviation`, `cameraRecoilTransform`, `timeSinceLastShot`, `randomAngle`, `randomRadius`, `seed`, `initialFireShot`, `fireShot`, `isFiring`, `isMoving`, `isJumping`, `jumpingTimer`, `crossHairDispersionFactor`, `currentGameplay/VisualDeviationScaleFactor`, `suppressionMinDispersionAngleFactor`; `isSupportedShooting` RO | current cone angle (deg [inferred]), accumulated recoil and dispersion offsets, the last random polar sample (units unknown) and RNG seed | `shared/type/gunsway.md`:11–34 |
| shared | `WeaponFiring`: `gunSway` RO, `weaponState`/`last`/`next`, `timeToWait`, `recoilTimer`, `recoilAngleX/Y/Z`, `recoilFovAngle`, `reloadTimeMultiplier`, `primaryAmmoToFill`, `instanceId` RO | fire-logic state machine (16 `WeaponState` values incl. `PrimaryAutomaticFire`, `Reload`, `PostReload`, `BoltAction`) | `weaponfiring.md`:11–30; `weaponstate.md`:9–24 |
| shared | events `WeaponFiring:Create/Update/UpdateAmmo`, `Engine:Update(dt, simDt)`, `UpdateManager:Update(dt, pass)` with `UpdatePass_PostSim` | per-tick sampling point; firing lifecycle | `shared/event/*.md`:5; `fb/updatepass.md` |
| shared | `PhysicsEntityBase`: `linearVelocity`, `angularVelocity`, `position` RO, `mass` RO, `partCount` RO, `GetPart`, `GetPartTransform(partId)` | rigid-body state, per-part pose (turret ring, gun, rotor) | `shared/type/physicsentitybase.md`:21–53 |
| shared | `EntryInput`: `authoritativeAimingYaw/Pitch`, `flags`, `zoomLevel`, `GetLevel/SetLevel(EntryInputActionEnum)` | exact inputs: read for humans, scripted for test bots (`EIAThrottle` 0, `EIABrake` 2, `EIAYaw` 4, `EIAPitch` 5, `EIARoll` 6, `EIAFire` 7, `EIAZoom` 11, `EIAJump` 12, `EIASprint` 32, `EIAGearUp/Down` 40/41, `EIAClutch` 42, `EIAHandBrake` 43) | `shared/type/entryinput.md`:15–32; `fb/entryinputactionenum.md` |
| shared | `SharedUtils:GetTimeMS/GetTimeNS/GetTickrate/GetLevelName/GetCurrentGameMode`; `EntityManager:GetIterator(type)`, `TraverseAllEntities`; `json.encode` (Vec3, LinearTransform, Quat, tables) | clocks, census, encoding. `io` is not in the sandbox (allowed: `table string math os coroutine package utf8`) | `shared/library/sharedutils.md`:11–22; `entitymanager.md`:11–17; `..\_index.md`:14–80 |
| server | hook `Soldier:Damage(soldier RO, info, giverInfo RO)`; `DamageInfo` fields; `DamageGiverInfo.giver`, `weaponFiring`, `weaponUnlock`, `damageType` | damage before application; `hookCtx:Call()` runs the engine, after which health shows what was applied | `server/hook/soldier_damage.md`:5; `damageinfo.md`:17–33; `damagegiverinfo.md`:17–25; `..\guides\hooks.md`:54–65 |
| server | hook `BulletEntity:Collision(entity RO, hit, giverInfo RO)` | each server bullet's collision: position, normal, material, part, bone | `server/hook/bulletentity_collision.md`:5 |
| server | hook `EntityFactory:Create` | every entity creation, incl. `ServerBulletEntity`, `ServerMissileEntity`, `ServerGrenadeEntity` (spawn transform = shot origin and direction [inferred]) | `server/hook/entityfactory_create.md`:5; `..\entities.md`:1750, 2124, 2313 |
| server | `SoldierEntity`: `health`, `maxHealth`, `pose`, `pendingPose`, `isFiring` RO, `isReloading` RO, `weaponsComponent` RO; `SetPosition`, `SetPose`, `SetTransform`, `Kill` | soldier state; test fixtures | `server/type/soldierentity.md`:24–54 |
| server | `SoldierWeaponsComponent.currentWeapon` RO, `isZooming` RO; `SoldierWeapon.weaponFiring` RO, `primaryAmmo`, `name` RO | the weapon whose `GunSway` is sampled | `soldierweaponscomponent.md`:13–16; `soldierweapon.md`:22–27 |
| server | `PhysicsEntity.velocity`, `internalHealth`; `RegisterCollisionCallback`/`RegisterDamageCallback`; `ControllableEntity.GetPlayerInEntry`, `GetDataForEntry`, `Get/SetExternalInput` | vehicle and soldier dynamics, damage, collisions, seat data | `physicsentity.md`:22–48; `controllableentity.md`:23–34 |
| server | `TicketCounterEntity`: `team` RO, `ticketCount`, `initialTicketCount` RO, `controlledAreaValue`; `TicketManager:GetTicketCount` (float) | bleed time series | `ticketcounterentity.md`:22–25; `ticketmanager.md`:11–14 |
| server | `CapturePointEntity`: `name`, `playersInside`, `isInPreRound`, `isAttacked`, `previousOwner`, `attackingTeam`, `isFlagMoving` (all RO); `team`, `flagVelocity`, `flagLocation`, `flagHome`, `isControlled`, `isCaptureEnabled`, `cooldownTimer`, `team1/2CooldownPenalty` | capture time series (`flagLocation` 0–100 [inferred from fun-bots testing `< 100.0`, `C:\Users\olku\unfun-bots\ext\Server\BotSpawner.lua`:1087]) | `capturepointentity.md`:22–38 |
| server | `PlayerManager:CreatePlayer(name, team, squad)`; `Player:CreateSoldier`, `SpawnSoldierAt`, `SelectWeapon`, `SelectUnlockAssets`, `EnterVehicle`, `ExitVehicle`; `Player.input` (writable), `isAllowedToSpawn` | test bots (targets, shooters, drivers, capture crowds) | `server/library/playermanager.md`:21; `server/type/player.md`:26, 32, 60–71 |
| server | events `Vehicle:Damage/Disabled/Destroyed/Enter/Exit`, `Player:Killed/Respawn/Reload/Chat`, `CapturePoint:Captured/Lost`, `Player:Entered/ExitedCapturePoint`, `Soldier:HealthAction/ManDown`, `Server:RoundOver`, `Player:Score`, `ScoringSystem:StatEvent` | event rows; chat commands to steer scenarios | `server/event/*.md`:5 |
| server | `RCON:SendCommand` | reads `vu.FrequencyMode` and `vars.*` into the meta row | `server/library/rcon.md`:11–12 |
| server | `RaycastManager:DetailedRaycast` | finds the wall plane for W-A and the ground under placed bots | `server/library/raycastmanager.md`:11 |
| client | `SoldierWeapon.correctedWeaponFiring` / `predictedWeaponFiring` (RO), `aimingSimulation`; `ClientUtils:GetCameraTransform/WorldToScreen/GetWindowSize`; `PlayerManager:GetLocalPlayer`; `Console:Register`; `NetEvents` | client prediction vs server correction; FOV probe; console commands | `client/type/soldierweapon.md`:22–29; `client/library/clientutils.md`:11–14; `client/library/playermanager.md`:19; `client/library/console.md`; `..\guides\netevents.md` |

---

## 3. Harness specification

### 3.1 Placement and instance (user runs; nothing goes near the install)

```
C:\Users\olku\Documents\Battlefield 3\ServerMeasure\     <- new instance; the existing ...\Server stays untouched
    server.key                    <- copy of the existing key (sequential use) or a second key from the VU key portal
    Admin\ModList.txt             <- exactly one line: mharness
    Admin\MapList.txt             <- per session (e.g. "MP_Subway ConquestSmall0 1", "XP3_Desert ConquestLarge0 1")
    Admin\Startup.txt             <- neutral, below
    Admin\Mods\mharness\mod.json  <- {"Name","Version","HasVeniceEXT":true,"Dependencies":{"veniceext":"^1.15.0"}}
    Admin\Mods\mharness\ext\shared\__init__.lua
    Admin\Mods\mharness\ext\server\__init__.lua   (+ server\scenarios\*.lua)
    Admin\Mods\mharness\ext\client\__init__.lua
    logs\vu_*.log                 <- VU writes here; mod prints land here [verified: unfun-bots prints in ...\Server\logs]
```

* Instance layout, `mod.json` fields and `ModList.txt` loading: `hosting/config.md`:10–35, `hosting/mods.md`:10–41,
  `modding/structure.md`:10–20, `modding/your-first-mod.md`:27–60 [verified]. A separate instance per server is the
  documented way to run several (`config.md`:101–111). Reusing one key for two instances that never run at the same
  time is untested [inferred OK].
* **Neutral `Startup.txt`**, using BF3 R25 RCON variables (`vars.*` semantics from the BF3 PC Server Remote
  Administration Protocol R25, pdftotext of
  `https://raw.githubusercontent.com/dcodeIO/BattleCon/master/eadocs/BF3/BF3%20PC%20Server%20Remote%20Administration%20Protocol.pdf`,
  lines 484–548, 1303–1344 [verified]):
  `vars.gamePassword <private>`, `vars.regenerateHealth true`, `vars.soldierHealth 100`, `vars.bulletDamage 100`,
  `vars.playerRespawnTime 100`, `vars.gameModeCounter 100`, `vars.vehicleSpawnAllowed true`,
  `vars.vehicleSpawnDelay 100`, `vars.friendlyFire false`, `vars.autoBalance false`, `vars.idleTimeout 0` ("0 to
  disable idle kick"), `vu.SuppressionMultiplier 100` (VU default, `hosting/commands.md`:107–111),
  `vu.TimeScale 1.0` (:119–123). **Do not** set `vu.DisablePreRound` or lower `vars.roundStartPlayerCount` unless a
  scenario requires it. Test bots count as players and can meet the round-start count [inferred]. Whatever is used goes
  into the meta row.
* **Launch** (manual equivalents of the user's shortcuts, which are `vu.exe -server -dedicated [-high60|-high120]`
  [verified shortcut arguments]):
  `"%LOCALAPPDATA%\VeniceUnleashed\client\vu.exe" -serverInstancePath "C:\Users\olku\Documents\Battlefield 3\ServerMeasure" -server -dedicated -unlisted`
  for 30 Hz, adding `-high60` for the T1 repeat (`general/args.md`:158–178, 218–220). Then `VU Client` and
  `vu://join/<server-guid>/<password>` (args.md:110–116). `-unlisted` and a password keep the server private.

### 3.2 Observe-only rules (what keeps the measurements BF3's own)

1. No data edits: no `MakeWritable`, no `ResourceManager` callbacks that change instances, no bundle mounts. This is
   exactly what unfun-bots does and must not be done here (§0.5).
2. Hooks pass through. A handler may call `hookCtx:Call()` to read the result. It never calls `Pass` with altered
   arguments or `Return` with a different value (`..\guides\hooks.md`:32–78) [verified semantics].
3. **Allowed fixtures**, each logged as an `fx` row: create, spawn, place (`SetPosition`/`SpawnSoldierAt`), pose, kill
   and remove *test bots*; set *test-bot* inputs and aim; seat test bots in vehicles. Respawning a target counts as a
   fixture. Writing a weapon's `primaryAmmo` or a flag's `team` does **not**: respawn the bot instead, capture
   naturally.
4. No other mods loaded. Vanilla kits and attachments only, recorded per run (weapon name and unlock list in `meta`).
5. No personal data: rows carry the harness's own small integer ids, never `ip`, `accountGuid`, `onlineId` or names.

### 3.3 Script set (outline, written from VU-Docs, no fun-bots or VU code copied)

**`shared/__init__.lua` (about 120 lines).** Row builder `row(e, fields)` adds the envelope (§3.4) and prints
`MH1 <json>`. `json.encode` handles Vec3, Quat and LinearTransform natively (`..\_index.md`:57–76). It also holds the
run-id state, the tick counter (incremented on `UpdateManager:Update` with `UpdatePass_PostSim`), the simulated time
(sum of `deltaTime` there) and `SharedUtils:GetTimeMS()` for wall time. Floats are rounded to 6 significant digits.

**`server/__init__.lua` plus `server/scenarios/*.lua` (about 600–700 lines).**
* *Meta*: on `Level:Loaded`, emit level, mode, round, `GetTickrate()`, `RCON:SendCommand('vu.FrequencyMode')`, the
  `vars.*` above, harness version and a census (`TraverseAllEntities`: count per `typeInfo.name`). The census confirms
  the iterator type names (`ServerTicketCounterEntity`, `ServerCapturePointEntity`, `ServerVehicleEntity`, verified in
  `..\entities.md`:1772, 2748, 2801) before any scenario relies on them.
* *Samplers on PostSim* (active only for watched ids): `hp` (health, pose, position, velocity), `tk` (both teams'
  float tickets, `ticketCount`, `controlledAreaValue`), `cp` (every flag's fields and per-team inside counts), `veh`
  (transform as position + quaternion, velocity, angular velocity, `internalHealth`, selected part transforms, driver
  input levels).
* *Weapon sampler*: subscribe to `GunSway:Update`. When the weapon belongs to a watched soldier (matched by
  `instanceId` against `soldier.weaponsComponent.currentWeapon.weaponFiring`), emit `gs` every update with `fireShot`
  and every tick otherwise. Emit `wf` on every `weaponState` change.
* *Projectile and hit hooks*: `EntityFactory:Create` → after `Call()`, if the entity `:Is('ServerBulletEntity')`,
  `ServerMissileEntity` or `ServerGrenadeEntity`, emit `shot` (spawn transform) and track missiles and grenades per
  tick. `BulletEntity:Collision` → `hit`. `Soldier:Damage` → `dmg` before `Call()` (info as given) and after it (health
  after). `Vehicle:Damage` → `vdmg`, followed by `internalHealth` on the next tick.
* *Event rows*: kill, respawn, reload, captured/lost, entered/exited, health action, man-down, vehicle
  enter/exit/disabled, round over, score.
* *Command parser*: `Player:Chat` messages beginning `!mh` (`server/event/player_chat.md`:5). Examples:
  `!mh wall` (record the user's eye and view; raycast to the wall plane), `!mh shooter M16A4 stand ads`,
  `!mh target 40` (spawn a target bot 40 m along the user's view, snapped to the ground by raycast, facing the user),
  `!mh ring 0.5,1,2,3,4,5,6,7`, `!mh run W-A`, `!mh drive P1`, `!mh seat gunner`, `!mh stop`, `!mh mark <label>`.
* *Fixtures*: `Bot.new(team, kit, primary)` = `CreatePlayer` → `SelectUnlockAssets`/`SelectWeapon` →
  `CreateSoldier` → `SpawnSoldierAt`. `Bot.aim(yaw, pitch)` and `Bot.hold(action, level, ticks)` write
  `player.input` each tick. `Bot.enter(vehicle, entryId)` picks the driver entry from `GetDataForEntry` seat data,
  never assuming entry 0. `Bot.respawn()`. Scenario modules are small state machines `step(k, t)` that emit `mark`
  rows at phase changes.

**`client/__init__.lua` (about 120 lines).**
* `GunSway:Update` for the local player's current weapon: emit `gs` with `r:"C"` on `fireShot` and every 4th frame,
  from both `correctedWeaponFiring.gunSway` and `predictedWeaponFiring.gunSway`.
* FOV probe on zoom change: take the camera transform, project two points at ±5° about the camera's right axis and two
  about its up axis at 100 m with `WorldToScreen`, and solve `fov = 2·atan(tan 5° · (size/2) / offset_px)`. The
  forward sign (+forward or −forward) is chosen as whichever lands at screen centre.
* `Console:Register('mh', …)` forwards commands to the server by `NetEvents`. Rows print to the client log
  (`%LOCALAPPDATA%\VeniceUnleashed\vu_*.log`, where client mod prints already land [verified, client log :11–37]).

**Extractor `research\probes\tools\vu-harness\extract.py` (about 150 lines, `python -I`).** Reads only the two log
folders and filters lines matching `\[VeniceEXT\] \[mharness\] MH1 (\{.*\})$`. It validates the envelope and writes
`research\measurements\<run>\raw.jsonl` (gzip if over 20 MB), `meta.json` and `manifest-before/after.csv` (§4.2). It
never opens anything under `C:\Program Files`. **Analysis** `analyze_<scenario>.py` writes `summary.json` and one row
per finding into `research\measurements\SOURCES.md` (value, run id, VU build, Hz, n, fit residual).

### 3.4 Output format

One JSON object per line. Envelope, then payload.

| key | meaning |
|---|---|
| `v` | schema version (1) |
| `run` | `YYYYMMDD-<scenario>-<hz>-<n>` |
| `sc` | scenario id (§3.5) |
| `r` | realm `S`/`C` |
| `k` | sim tick index since run start (PostSim count) |
| `t` | simulated seconds since run start (sum of PostSim `deltaTime`) |
| `ms` | `GetTimeMS()` wall clock |
| `e` | row type below |

| `e` | payload (units: m, m/s, deg unless the API's unit is unknown, then raw) |
|---|---|
| `meta` | level, mode, round, tickrate, freq mode, vars, harness ver, census, weapon and unlock names, vehicle data names |
| `fx` | fixture action and arguments (spawn, place, kill, input profile start/stop, seat) |
| `mark` | scenario phase label |
| `gs` | `id`, `wid`, `da`, `dmin`, `rec{p,y,r,ty}`, `disp{p,y}`, `ra`, `rr`, `seed`, `ifs`, `fs`, `fir`, `tsls`, `mov`, `jmp`, `jt`, `gsf`, `vsf`, `smf`, `chf`, `pose`, `zoom`, `src` (`srv`/`pred`/`corr`) |
| `wf` | `wid`, `ws`, `ws_last`, `ws_next`, `ttw`, `rt`, `ra{x,y,z}`, `ammo`, `ammo2` |
| `shot` | `id`, `wid`, `kind` (bullet/missile/grenade), `pos`, `dir`, `uid` (entity uniqueId) |
| `hit` | `uid`, `pos`, `n`, `bone`, `part`, `mat` (material instance GUID), `body` (rigidBody type name), `shooter` |
| `dmg` | `victim`, `phase` (pre/post), `dmg`, `bone`, `pos`, `origin`, `dir`, `bul`, `exp`, `cli`, `cspd`, `dot`, `hp`, `giver`, `wfire` (GUID), `dist` |
| `hp` | `id`, `hp`, `pose`, `pos`, `vel`, `alive`, `mandown` |
| `tk` | per team: `tf` (float tickets), `tc` (ticketCount), `cav` (controlledAreaValue) |
| `cp` | `name`, `team`, `att`, `loc`, `vel`, `ctrl`, `atk`, `n1`, `n2`, `cd`, `pen1`, `pen2` |
| `veh` | `id`, `pos`, `q`, `vel`, `w`, `ih`, `parts[{i,q,t}]`, `in{thr,brk,yaw,pit,rol,fire}` |
| `vdmg` | `veh`, `dmg`, `giver`, `ih_before`, `ih_after` |
| `fov` | `zoom`, `fovmul`, `vfov`, `hfov`, `w`, `h` |
| `ev` | `name` plus that event's numeric arguments |

Logs keep **numbers and identifiers only** (instance GUIDs, data-container names as ids), never asset content. That
satisfies "numbers only, stored under `research\measurements`".

### 3.5 Scenarios and the prioritized measurement table

"Who": **B** = unattended (test bots only; the user's client stays connected, see §4.4); **U** = the user plays a
part. Durations are wall time at 30 Hz [inferred estimates].

| # | prio | scen. | items | needed before | who | procedure | duration |
|---|---|---|---|---|---|---|---|
| 1 | P0 | **W-A fixed wall** | W1–W4, T1 | M2a (run in M0) | B | User faces a flat wall about 25 m away, `!mh wall`, steps aside. A shooter bot takes the recorded eye pose and fixed authoritative aim. Per config: (a) 200 single shots 1.5 s apart (pure minimum disc, first-shot recoil); (b) 100 five-round bursts, 1.5 s rests (growth, recoil); (c) shot pairs with gaps g ∈ {0.075, 0.1, 0.15, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5} s × 20 (first-shot re-arm, spread decay); (d) 10 full magazines (recoil saturation, horizontal autocorrelation); (e) 3 s idle per tick after each burst (recovery curves). Bot respawned for ammo. Configs: M16A3 stand/crouch/prone × hip/ADS; AK-74M, M416, M249, M98B at stand-hip, stand-ADS, prone-ADS | ≈ 4.2 h B |
| 2 | P0 | **W-A60** | T1 | M2a | B | configs M16A3 stand hip and ADS repeated on a `-high60` server | ≈ 0.5 h B |
| 3 | P0 | **S-B movement** | S3, T1 | M1 | B | Bot from rest, `EIAThrottle` 1 for 3 s then 0, for stand/crouch/prone, sprint, strafe, back. Jumps on flat ground. Drops by `SetPosition` at 5, 8, 10, 12, 15, 20, 30, 50 m above flat ground (fall-damage thresholds, terminal speed). Pose changes during idle and firing (stance-transition spread). Repeat on 60 Hz | ≈ 0.5 h B |
| 4 | P0 | **W-B falloff** | W5, W6 | M2a | U (+B if bot damage works, §4.4) | `!mh target d` at d ∈ {5, 8, 10, 15, 20, 25, 30, 40, 50, 60, 75, 100, 125, 150, 175} m. Per d: 3 chest and 3 head ADS single shots, target respawned after each hit. Weapons M16A3, AK-74M, M98B, M9, 870MCS (pellets at 5–30 m). Extras: two targets 1 m apart in line at 20 m (pass-through); a target behind wood/glass cover (penetration) | ≈ 1.3 h U |
| 5 | P0 | **S-A health** | S1, S2, S4 | M2b (run in M0) | B/U | One 25-damage hit on a target, health logged for 40 s × 5. The same with a shooter bot spraying 2 m beside it (suppression abort) × 3. Shots at 0.5/1.0/1.5/2.5 s after spawn × 3. Kill the user once: log `isAllowedToSpawn` flip time | ≈ 0.4 h |
| 6 | P0 | **M-A bleed** | M1 | M4 (run in M0) | B | `MP_Subway ConquestSmall0` and `MP_001 ConquestLarge0`. Bots capture flags naturally (placed inside the polygon until the flag flips), then hold in HQ with no deaths. States held 150 s each: 0–0, 1–0, 2–0 (Metro 2 × 55 = 110 > 100), 3–0, 2–1, 1–1, and for CQL 3–0 (3 × 34 = 102). `tk` and `cp` per tick | ≈ 0.8 h B |
| 7 | P0 | **M-B capture** | M2 | M4 (run in M0) | B | Neutral flag with n ∈ {1, 2, 3, 4, 5} attackers × 3. Enemy-owned flag flip with n ∈ {1, 2, 4} × 3. Contested 1v1, 2v1. Attackers leave at 50 % (return or decay rate) | ≈ 0.5 h B |
| 8 | P0 | **V-A ground** | V1, V2 | M4 | U positions, B drives | User drives the vehicle to the start of a straight 400 m flat track (e.g. `XP3_Desert`), exits, `!mh drive`. Driver-bot profiles: P1 full throttle 15 s; P2 full brake from top speed; P3 coast; P4 full lock at full throttle (circle) and at 30 km/h; P5 pivot (yaw only) 10 s; P6 reverse. Humvee, Vodnik or LAV-25, M1A2, T-90, quad | ≈ 0.6 h |
| 9 | P0 | **V-B turrets** | V3 | M4 | B (+U to park) | Gunner bot in the MBT main gun and in an HMG seat: yaw input ±1 for 4 s, pitch ±1. Hull pivot with zero turret input (world-lock check). All parts logged for 2 s first to identify the turret part | ≈ 0.2 h |
| 10 | P1 | **W-C explosives** | W7 | M2a/M4 | U | RPG-7 and SMAW direct hits at 30 m × 3; M320 HE direct and offset. M67 into a ring of target bots at radii 0.5–7 m (one throw gives the whole falloff curve) | ≈ 0.3 h U |
| 11 | P1 | **V-C vehicle damage** | V4 | M4 | U | MBT shell into a parked MBT front/side/rear × 3; RPG × 3 per side; small-arms burst; HMG burst; IFV 25 mm. A reactive-armour variant if the unlock can be selected for the target vehicle (§9). Humvee driver bot into a wall at 10–50 km/h (`CollisionInfo.speed` vs damage) | ≈ 0.5 h U |
| 12 | P1 | **A-A helicopter** | V5 | M5 | U (+B steps) | User hovers an AH-1Z at 50 m over flat ground and hands off: idle collective 10 s (sink rate); collective step to 1 (climb); pitch held 25° and 36° nose-down by a P-controller on `EIAPitch` (steady speed); rotor spool from engine start | ≈ 0.5 h U |
| 13 | P1 | **A-B jet** | V6 | M5 | U | F/A-18 at 500 m: speed holds at 4 throttle settings, full-pitch loops at each (turn rate vs speed); slow-flight stall; full-afterburner climb to ceiling (`PowerFadeOutRange`) | ≈ 0.5 h U |
| 14 | P2 | **W-D client** | W8, W9, W10 | M2a | U | User fires full-auto magazines (client predicted vs corrected vs server shot times), reloads at 0/50/90 % magazine, ADS with every scope (FOV probe) | ≈ 0.3 h U |
| 15 | P2 | **A-C boats/AC-130** | V7, V8 | M5/M7 | U | RHIB top speed and turn; LAV water entry and exit on Noshahr; AC-130 transform for 3 orbits | ≈ 0.3 h U |
| 16 | P3 | **M-C mode details** | M3, M4 | M7 | U | Bazaar Rush MCOM arm/disarm/fuse (event callbacks on MCOM entities); Gun Master knife demotion; squad spawn conditions | ≈ 0.3 h U |
| 17 | P3 | **N-A replication** | N1 | — | U | A bot walks a square; the client logs its transform each frame, the server each tick. A teleport step gives the delay by cross-correlation | ≈ 0.1 h |

### 3.6 Decision rules (what each result changes in our code)

| item | analysis | if A | if B |
|---|---|---|---|
| W1 | Two-sample KS on normalised impact radius ρ = r/(d·tan(dispersionAngle)) and on `randomRadius`, testing uniform-area (CDF ρ²) against uniform-radius (CDF ρ); n ≥ 200 per config separates the medians 0.707 and 0.5 easily [inferred] | `disc = uniform_area` | `uniform_radius`; else store the empirical CDF as a 16-knot table |
| W2 | `dispersionAngle(t)` after the last shot: slope gives `DecreasePerSecond` (15 vs 8); jump per shot gives `IncreasePerShot` | data value confirmed (C14 closes) | record the factor between data and behaviour |
| W3 | fit `rec.p(t)` after the last shot: linear-to-zero vs exponential; compare RMS; ratio of decay during vs after firing gives `ShootingRecoilDecreaseScale`; lag-1 autocorrelation of horizontal increments (Symthic suspected "not independent random draws") | recovery law = better fit, parameter mapped to `RecoilAmplitudeDecreaseFactor` | horizontal recoil from a correlated process, noted for D7 |
| W4 | recoil increment of shot 2 vs gap g; re-arm time = smallest g with increment ≈ `FirstShotRecoilMultiplier` × base | first-shot flag re-arms on a timer | re-arms on `dispersionAngle` reaching min, or on `initialFireShot` |
| W5 | piecewise fit with knots at `DamageFalloffStartDistance/EndDistance`; residual < 0.1 hp | linear (Symthic claim confirmed) | store curve shape |
| S1 | first tick of health increase after the last damage; slope | 10 s and 5 hp/s (data) | 16 s community value explained by a second timer (e.g. suppression) |
| S3 | v(t) under constant input: first-order lag (time constant τ) vs constant acceleration; map τ to `AccelerationGain`; vertical acceleration in free flight gives gravity | motor uses the fitted law | — |
| M1 | ticket rate per state vs Σ enemy `AreaValue` and `controlledAreaValue` | rule A: 15/min whenever enemy > 100 and > own (current [inferred] rule) | rule B: rate scales with the lead; fit the function |
| M2 | time to capture T(n). Model under test: T = 20 / min(1 + 0.34 (n−1), 4) → 20.0, 14.9, 9.9 s for n = 1, 2, 4 [inferred arithmetic]; flip = lose + get = 40 s for n = 1 | model holds | fit per-player multiplier and flip rule |
| V1–V6 | time series become oracles in §7.4, replacing the IW4L [report claim] rows (68 km/h at 6 s, pivot 65° in 2 s, 159 km/h at 25°) | — | — |
| T1 | each W/S quantity per second at 30 vs 60 Hz | equal: per-second rates reproduce BF3 (C5 holds) | tick-coupled: note which; our 60 Hz sim must scale those to the 30 Hz feel |

---

## 4. Feasibility

### 4.1 Requirements and their state on this PC

| requirement (source) | state | tag |
|---|---|---|
| VU account and linked Origin/EA account (`hosting/prereq.md`:8–10; `general/playing.md`:13–15) | done: the client logged in and joined on 2026-06-28 | verified (client log :3–23) |
| server key from the VU key portal as `server.key` (`prereq.md`:13–14; `hosting/setup-win.md`:16) | present, 64 B, created 2026-05-01; server authenticated with Zeus | verified (listing; server log :99–100) |
| BF3 activation (`setup-win.md`:44–60; `args.md`:52–66) | done: an "Activate BF3" shortcut (`vu.exe -console -wait -activate -lsx`) exists; sessions ran. Re-activation may be needed "after major Windows Updates or hardware changes" (`setup-win.md`:60) | verified / report claim |
| BF3 installed, full and up to date in the EA app (`prereq.md`:16–18), "launched at least once via Battlelog" (`playing.md`:11) | Premium install of 2026-04-25 (`08-prior-art-legal.md` §4.2); VU ran on it | verified |
| EA app running during play | not needed at that client start: "LSX encountered an error … actively refused it" and the client still joined | verified for one session (client log :5–23) |
| VU dedicated server specs (`prereq.md`:20–28) | well within (RX 9070 XT class PC) | inferred |
| disk for logs | 736 GB free on C:; about 5 MB per hour at 30 Hz [inferred] | verified (free space) |

### 4.2 Does VU or the EA app write into the BF3 install?

* **Docs.** "VU functions separately from vanilla BF3 and does not modify any game files" (`troubleshooting\_index.md`:63;
  https://veniceunleashed.net/about) [report claim, VU's statement]. VU keeps its own files in
  `%LOCALAPPDATA%\VeniceUnleashed` (`setup-win.md`:10), logs and mods in the instance folder (`args.md`:176–178), and
  the profile in `Documents\Battlefield 3\settings` [verified listing].
* **Retrospective evidence.** Metadata census of the install on 2026-10-10: 2,101 items (1,918 files, 182 directories
  and the root). Max LastWriteTime 2026-04-25T17:22:36, max CreationTime 2026-04-25T17:20:47, **0 items written on or
  after 2026-04-26**. `Data\layout.toc` 2,282 B / 17:14:04, `Data\cas.cat` 3,494,716 B / 17:08:29,
  `Update\Patch\Data\layout.toc` 3,470 B, `Update\Patch\Data\cas.cat` 993,852 B, all on 2026-04-25 [verified]. VU was
  installed 2026-05-01 and ran on 2026-06-27/28. A file created, deleted or renamed in any directory would have bumped
  that directory's mtime, and none did [inferred from NTFS semantics]. Limitation: a writer could restore timestamps;
  the hash check below covers the index files.
* **Before/after check for every session** (tested 2026-10-10 against the install, read-only: 2,101 items, 243 files
  hashed, 112 MiB, under 1 s warm; a negative control with an altered `cas.cat` timestamp is detected, exit code 1)
  [verified]. Scripts: `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\measurement\install-manifest.ps1`
  and `install-compare.ps1`. Their essence:

```powershell
# install-manifest.ps1 -Out <csv OUTSIDE the install>   (refuses an -Out inside the install)
$items = @(Get-Item -LiteralPath $Root -Force) + @(Get-ChildItem -LiteralPath $Root -Recurse -Force)
# per item: Path, Dir, Length, LastWriteUtc, CreationUtc, and SHA-256 for .toc .cat .mft .json .xml .exe .dll and initfs_Win32
# install-compare.ps1 -Before a.csv -After b.csv  -> "install unchanged: N items" (exit 0) or the differing rows (exit 1)
```

  Procedure: run the manifest into `research\measurements\<run>\manifest-before.csv`, run the session, run it again
  into `manifest-after.csv`, then compare. A non-zero exit stops the campaign until explained.
* **EA app.** Its update/repair can rewrite install files. That is a property of the EA app, not of VU. BF3's data has
  been frozen since 2013 (`08` §4.2), so an update is unlikely [inferred]. The before/after check would show any write.
  Keep EA-app auto-update for BF3 off if the setting exists [inferred].

### 4.3 Does a local or LAN server need internet?

Yes, at least at start. The server connects to and authenticates with the Zeus backend and registers the game *before*
"The server is now accepting connections" (server log :96–108) [verified]. Community: "I don't think it's possible to
play a game through VU completely without internet … you need to connect to Zeus Backend" (24-7-NightHawk,
2021-04-21, https://community.veniceunleashed.net/t/suggestion-on-bots-mod/2965) [report claim]. The docs list no
offline flag; `-env [prod|dev]` only picks the Zeus environment (`args.md`:28–30) [verified]. The local/LAN join
itself worked with the client and server on one PC (192.168.10.88) [verified]. Port forwarding is not needed for the
host (2021 report claim; consistent with the log). An older changeset fixed connected players being dropped when the
Zeus connection was lost (`general/changelog.md`:395) [verified text], so a mid-session Zeus loss should no longer end
a run [inferred]. **Risk:** a Zeus outage at start blocks a session; nothing in the harness depends on it otherwise.

### 4.4 Are bots needed, and can fun-bots be used?

* **Needed.** Targets (W-B, S-A, W-C), crowds of 1–5 for capture (M-B), flag holders (M-A), and drivers and gunners
  with exact inputs (V-A, V-B). A second human would need a second VU account with its own linked Origin account
  (`troubleshooting\_index.md`:109–111) [verified].
* **Own test bots, not fun-bots.** VU documents `PlayerManager:CreatePlayer`, `Player:CreateSoldier`, `SpawnSoldierAt`,
  `EnterVehicle` and writable `Player.input` (§2) [verified]. fun-bots uses exactly these calls
  (`C:\Users\olku\unfun-bots\ext\Server\BotManager.lua`:868–913) [verified], which shows they work. fun-bots stays
  **unloaded** during measurements because of the data edits in §0.5. It may run in the user's *normal* instance as a
  reference for behaviour (the user already does: `unfun-bots` with 8,568 waypoints on Caspian, server log :10, :112)
  [verified]. Its code is unlicensed, so ideas only (`08` §1.3).
* **Open (Phase 0 of Session 1).** fun-bots switches `allowClientSideDamageArbitration` off around bot bullet
  collisions "so the server's own hit applies" (`__init__.lua`:595–614; `Registry.lua`:22–23 "Use worse hitboxes")
  [verified code]. That suggests bot bullets do not damage soldiers under the shipped setting [inferred]. Phase 0 tests
  it in 2 minutes: a bot shoots a target bot. If no `dmg` row appears, every damage scenario uses the user as shooter.
  That is the canonical client-arbitrated path anyway, and wall statistics (W-A) need only `hit` rows. Second open
  point: whether rounds and bot physics advance with no human connected. The plan keeps the user's client connected
  during "B" runs.

### 4.5 Tick rate

`vu.FrequencyMode` returns `regular` for 30 Hz (`hosting/commands.md`:73–75), which is BF3's own
`GameTimeSettings.MaxSimFps 30` (`05-networking.md` K1) [verified]. VU changeset 20056 (2023-09-25) fixed "quicker
acceleration on high frequency" and "faster damage expiration on high frequency" (`general/changelog.md`:132–136)
[verified text]. So some BF3 behaviour is tick-coupled and high-frequency modes needed corrections. All oracles come
from **30 Hz**. The 60 Hz repeat (T1) is a probe for our D4 choice, not an oracle.

### 4.6 Risks to the measurement itself

| risk | mitigation |
|---|---|
| VU auto-updates mid-campaign | record the build from the log header (`Initializing Venice Unleashed Server (Build 20682)`) in `meta`; `-noUpdate` exists (`args.md`:236–238) but clients must stay compatible [inferred] |
| `randomAngle`/`randomRadius` units unknown | W1 also uses impact points, which need no units |
| `GetPartTransform` frame (world or vehicle-local) unknown | log the hull transform alongside; with zero turret input and hull yaw, a world-locked ring stays constant in world |
| log line length or print rate limits unknown | keep rows under 1 KB; per-tick sampling only for watched ids |
| client-arbitrated hits differ from server hits | `isClientDamage` is logged; damage magnitudes are compared per path |

---

## 5. Legal and principle check (not legal advice)

* **EU Directive 2009/24/EC Art 5(3).** A person entitled to use a copy may, without authorisation, observe, study or
  test the program's functioning to determine its underlying ideas and principles while loading, running or storing it
  (`08-prior-art-legal.md` §3.5) [verified by 08 from legislation.gov.uk]. The harness observes the running game's
  behaviour and records numbers. It does not decompile, disassemble or copy code. Art 1(2): ideas and principles (for
  example "spread is uniform over the disc area", "bleed is 15/min above 100 area value") are unprotected; numeric
  behaviour parameters are facts about them [inferred]. Art 8 voids contract terms contrary to Art 5(3) [verified by
  08].
* **Running BF3 through VU.** 08 §3.5 rates "observing real BF3/VU while running it" as low risk under Art 5(3). The
  practical exposure is contractual: EA User Agreement §7(C) "Unauthorized Third-Party Program" (mods) with account
  termination as the remedy (`08` §3.4) [verified by 08]. VU states its use is lawful under EU interoperability law and
  bans nobody (FAQ) [report claim]. The harness adds nothing beyond an ordinary VU server mod on a private, passworded,
  unlisted server [inferred]. VU's own terms of service were not read (§9).
* **The user's principle.** Nothing is written into the install (§4.2 check every session). VU is used for knowledge
  only. The harness lives in the VU instance folder (deployed copy) and `research\probes\tools\vu-harness` (master
  copy, research only). Logs hold numbers and identifiers under `research\measurements`. **Nothing from VU (code,
  docs text, type yaml, logs) is copied into the product.** Product code cites the measured value through
  `SOURCES.md` (`08` §3.8 item 4) [inferred].
* **Hygiene.** No screenshots or video of BF3 go into any public repo (`08` §3.8 item 1). Logs carry no IPs, account
  GUIDs or names (§3.2 rule 5).

---

## 6. Fallbacks if VU is unavailable, with cost

| fallback | what it settles | what it cannot | cost | tag |
|---|---|---|---|---|
| **Symthic archives** (web.archive.org; the live symthic.com refused connections on 2026-10-10) | weapon table values (already matched 50/62 from data, `01` C6). The community model: spread "defines the size of a circle … the bullet will be in a random place in that circle" (tutorial 1434). Recoil decrease "is only apparent when you are not firing … could be not completely right … under investigation" (1434). "Recoil from shot to shot is not independent random draws" (thread 2049, 2012-06-29); "Recoil decrease is odd" (Rezal, 2049) | the disc distribution (never specified), the recovery law (open in 2012), first-shot re-arm, bleed, capture, vehicles | about 0.05 slice to read; results stay [report claim] | verified that the threads say this (curl of the archive copies, scratch `dl-symthic\`) |
| **Frame-counted published videos** (full-round uploads with the HUD ticket counter; vehicle runs with the aircraft HUD speed) | bleed rate per flag state (ticket counter over minutes), capture-bar times, top speeds and turn times, at ±1 frame (33 ms at 30 fps) | spread or recoil internals, damage values (except hit-marker counts), anything off-HUD | about 1–3 agent hours per item to find and frame-count (ffmpeg); 0.3–0.5 slice for the P0 set | inferred |
| **Retail BF3 on remaining official servers** (Battlelog lists servers; population not established, `08` §4.1) | feel and screen-measured spread on an empty server, if one allows it | no scripting, no bots, PunkBuster forbids memory tools, no control over flags | high and uncertain: finding an empty, controllable server; user time | inferred |
| user feel judgement against our build | final tuning sign-off | numbers | per tuning pass | inferred |

**Plan impact without VU [inferred]:** R5 stays at H likelihood. D7 ships with the switchable disc defaulted to
uniform-area and a linear recovery law, both marked "unmeasured". D12 bleed rule A ships with rule B behind the same
swappable function, tuned to video ticket timings. R3's M4/M5 oracles fall back to IW4L's [report claim] numbers plus
video timing. M2a and M4 tuning steps become "tune to video oracles plus user feel", about +0.5 slice each, at L
confidence.

---

## 7. Expected cost and proposed plan changes

| part | who | cost [inferred] |
|---|---|---|
| harness mod (shared/server/client, scenarios W-A…N-A), extractor, install manifest wrapper | agent | 0.3–0.4 slice |
| Phase 0 shakedown with the user (census, bot-damage test, chat commands, log extraction) | user + agent | 30–45 min user |
| Session 1, weapons/soldier/modes (W-A, W-A60, S-B, S-A, W-B, M-A, M-B, W-C, W-D) | user | about 2.5 h attended + about 6 h unattended |
| Session 2, ground vehicles (V-A, V-B, V-C) | user | about 1.3 h attended |
| Session 3, air/sea/details (A-A, A-B, A-C, M-C, N-A) | user | about 1.5 h attended |
| analysis, fits, `SOURCES.md`, updates to §7 oracles | agent | 0.2–0.3 slice |
| **total** | | **0.6–0.8 agent slice + about 4–5 h attended and about 6 h unattended user time** |

Proposed edits to `PORTING-RESEARCH.md` [inferred]:
* §6 l.820: replace "*measurement pass* | (user, ~1)" with "**S4 Measurement pass** | 0.6–0.8 + user 3 sessions |
  `topics/gap-r1-measurement-harness.md` §3.5 | exit: every P0 item has a run in `research\measurements` and a
  `SOURCES.md` row; the D7 disc/recovery/first-shot switches, the D12 bleed/capture functions and the soldier-motor
  gains have their chosen variant; install manifest unchanged on every run". Schedule Session 1 **during M0**,
  Session 2 before M4, Session 3 before M5.
* R5 (l.785) early test: "S4 Session 1 (W-A, W-B, S-A, M-A, M-B), run during M0". R3 (l.783): "S4 Sessions 2–3
  (V-A, V-B, A-A, A-B)".
* §9 items 1–6: point to the scenario ids here. Add T1 (30 vs 60 Hz) as a D4 check.
* §7.4: the "IW4L feel targets" row becomes an S4 output.

---

## 8. Second-source re-check of the five most load-bearing claims

| # | claim | first source | independent second source | result |
|---|---|---|---|---|
| 1 | VU exposes `GunSway` dispersion and recoil state at runtime, server side included | `shared/type/gunsway.md`:11–34 | fun-bots reads `gunSway.currentRecoilDeviation` and `currentDispersionDeviation` from a bot's `weaponFiring` on the server (`unfun-bots\ext\Server\Bot\BotAiming.lua`:118–136) | **confirmed** (existence and server-side use; semantics of `randomAngle`/`randomRadius`/`seed` still unconfirmed) |
| 2 | VU does not write into the BF3 install | VU FAQ / `troubleshooting\_index.md`:63 | my metadata census: newest write 2026-04-25 across 2,101 items despite VU sessions on 2026-06-27/28 (server and client logs) | **confirmed** |
| 3 | A VU server needs internet (Zeus) even locally | user's server log :96–108 | community thread 2021-04-21 (NightHawk); no offline flag in `args.md` | **confirmed** for build 20682 |
| 4 | Test bots can be created, placed and driven with documented API calls | `server/library/playermanager.md`:21; `server/type/player.md`:32, 60–71; `shared/type/entryinput.md`:31–32 | fun-bots `BotManager.lua`:868–913 uses the same calls; server log "BotAttributes of 379 Bots created" (:10) | **confirmed** |
| 5 | Ticket and capture state is observable per tick | `ticketcounterentity.md`:22–25; `capturepointentity.md`:22–38; `ticketmanager.md`:12 | fun-bots reads `TicketManager:GetTicketCount` (`BotSpawner.lua`:372) and `flagLocation`/`isControlled`/`team` (`BotSpawner.lua`:1087, 1132; `GameDirector.lua`:146) | **confirmed** for tickets and flags; `controlledAreaValue` has the docs only |
| (6) | 30 Hz is BF3's own rate; VU high-frequency modes change behaviour | `hosting/commands.md`:73–75; `changelog.md`:132–136 | BF3's own `MaxSimFps 30` read from the install (`05-networking.md` K1) | confirmed |

---

## 9. What I could not determine

* The units and meaning of `GunSway.randomAngle`, `randomRadius`, `seed`, `initialFireShot`, `cameraRecoilTransform`:
  VU-Docs give names and types only. W1 is designed to work from impact points regardless.
* Whether `GunSway:Update` fires once per sim tick on the server (vs per frame), and whether the server-side `GunSway`
  of a human player mirrors the client's predicted one.
* Whether bot bullets damage soldiers with `allowClientSideDamageArbitration true` (fun-bots implies not; untested).
* Whether a VU server advances rounds and simulates bots with no human client connected.
* Whether `RayCastHit.position` in `BulletEntity:Collision` is the exact impact point; whether the `EntityFactory:Create`
  spawn transform of a `ServerBulletEntity` carries the post-dispersion direction.
* Whether `PhysicsEntityBase:GetPartTransform` is world- or vehicle-local.
* Whether bot `EntryInput` drives vehicles and aircraft exactly like a human's input (fun-bots drives vehicles, so
  plausible).
* Whether rpm or gear can be recovered through property-changed callbacks on vehicle bus peers.
* Whether two instances may share one `server.key` when never run concurrently.
* Whether the reactive-armour unlock can be selected for a target vehicle through VU without data edits.
* Log line length or print-rate limits in VU.
* VU's own terms of service (not read); whether EA or VU account terms address private data collection.
* Whether the EA app will ever write into the install (only the past state was observed); whether it has a per-game
  auto-update switch in its current version.
* The live state of symthic.com (connection refused 2026-10-10); the Battlelog population for retail tests.
* How long the Zeus backend and EA activation remain available.

---

## 10. Sources

Local (read-only):
* `C:\Users\olku\VU-Docs\content\vext\ref\**` (types, events, hooks, libraries as cited); `content\vext\_index.md`,
  `guides\hooks.md`, `guides\netevents.md`, `guides\events.md`, `entities.md`; `content\hosting\setup-win.md`,
  `prereq.md`, `config.md`, `mods.md`, `commands.md`; `content\general\args.md`, `playing.md`, `changelog.md`;
  `content\troubleshooting\_index.md`; `content\modding\structure.md`, `your-first-mod.md`;
  `layouts\shortcodes\prop.html`; `types\shared\type\GunSway.yaml`; `types\fb\GunSwayData.yaml`,
  `GunSwayRecoilData.yaml`, `GunSwayDispersionData.yaml`, `MaterialRelationDamageData.yaml`,
  `TicketCounterEntityData.yaml`, `CapturePointEntityData.yaml`, `VeniceSoldierHealthModuleData.yaml`,
  `ChildRotationBodyData.yaml`. Repo commit `f3cb9f19` (2026-06-25).
* `C:\Users\olku\Documents\Battlefield 3\Server\logs\vu_2026-06-28_15-19-10_0767.log`;
  `C:\Users\olku\AppData\Local\VeniceUnleashed\vu_2026-06-28_15-23-30_0510.log`; `...\Server\Admin\ModList.txt`,
  `Maplist.txt`, `Startup.txt` (keys only); start-menu shortcuts for VU.
* `C:\Users\olku\unfun-bots\ext\Server\__init__.lua`:590–616, 700–752; `Bot\BotAiming.lua`:118–136, 292–306;
  `BotManager.lua`:868–913; `BotSpawner.lua`:372, 1072–1180; `GameDirector.lua`:109–146, 1903;
  `ext\Shared\Registry\Registry.lua`:15–30.
* `C:\Program Files\EA Games\Battlefield 3` (metadata and SHA-256 reads only, 2026-10-10).
* `C:\Users\olku\BF3 reimplementation\research\PORTING-RESEARCH.md`; `topics\01-weapons.md`:555–575;
  `topics\08-prior-art-legal.md` §1, §3, §4.2; `C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\15-GODOT-HANDOFF\HANDOFF.md`.

Web (fetched 2026-10-10):
* https://veniceunleashed.net/about (FAQ: no game files modified; legal position; ownership).
* https://community.veniceunleashed.net/t/suggestion-on-bots-mod/2965 (2021: no fully offline play; Zeus).
* https://forums.veniceunleashed.net/viewtopic.php?p=48738 (2015 setup guide: activation "because VU does not bypass
  the game's DRM"; Zeus registration).
* https://raw.githubusercontent.com/dcodeIO/BattleCon/master/eadocs/BF3/BF3%20PC%20Server%20Remote%20Administration%20Protocol.pdf
  (BF3 R25 RCON variables; text via pdftotext in scratch `dl-rconpdf\`).
* https://web.archive.org/web/20130727134909/http://forum.symthic.com:80/battlefield-3-technical-discussion/2049-state-of-knowledge-on-weapon-handling-spread-and-recoil/
  and https://web.archive.org/web/20131009062619/http://forum.symthic.com/battlefield-3-technical-discussion/1434-in-depth-tutorial-for-charts-plots-recoil-and-spread-mechanism-etc/
  (curl, scratch `dl-symthic\`).
* https://symthic.com/bf3-weapon-info?w=M16A3 (connection refused).
