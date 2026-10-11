# 08 — Prior art, reusable work, legal and distribution constraints, scope

Written 2026-10-10 for the Godot 4.8 / C++ reimplementation of Battlefield 3 multiplayer that reads the user's retail
install as-is. Scope: what exists that we can learn from or reuse, what the law and EA's terms say about doing this, what the
user's install contains, and what to build in which order. Level loading/rendering is out of scope (see the handoff).

Tags: **[V]** verified by me in data, code, the API dump or a primary source I read today; **[R]** report claim (a
source I did not re-check, or a secondary/press source); **[I]** inferred (my reading). Every fact carries a citation:
URL, or absolute path with line numbers. **None of §3 is legal advice**; the user's jurisdiction is unknown, and where it
matters (EU vs US) both are given.

Notation: Frostbite type names are as the schema spells them. The VU schema (`C:\Users\olku\VU-Docs\types\fb\*.yaml`)
spells **fields lowerCamel** (`weaponTable`), while EBX stores them UpperCamel (`WeaponTable`, as the handoff writes
`InitialOwnerTeam`); the field names below follow the yaml unless quoted from the handoff.

---

## 0. Conclusions in one screen

1. **Nothing reusable exists as code we may legally copy for the core**: Rime, fun-bots, VU-Docs and the Venice-EBX
   dump have **no licence** (all rights reserved by default) [V GitHub API, §2.1, §1.5]; Frosty Toolsuite is CC BY-NC-ND
   (no derivatives) and has **no BF3 profile** at all [V §2.2]. The permissive sources are small and old:
   NicknineTheEagle/Frostbite-Scripts (MIT, contains Frankelstner's and NoFaTe's FB2 dumper/EBX code) and
   mitsuhiko/frostbite2-stuff (BSD-style, 2011) [V §2.3]. Our own Rust crates from IW4L are the real spec. Everything
   else is **knowledge only**: read it, never paste it.
2. **The task brief assumed fun-bots is GPL. It is not licensed at all** [V `gh api repos/Joe91/fun-bots` → license
   null; no LICENSE in the full history of `C:\Users\olku\unfun-bots`]. Porting fun-bots code or its 185 waypoint maps
   needs the authors' written permission; re-deriving the *ideas* (objective selection, PID flight, path-mode bits) is
   fine. The only GPL code in the VU ecosystem worth noting is BF3RM/MapEditor (GPL-3.0) [V].
3. **Venice Unleashed teaches the engine's model, not a route**: it runs the original `bf3.exe` with a hooked client,
   emulates EA's Blaze backend and links an Origin account for ownership [R/V §1.1]. What it documents for us: the
   superbundle mount order, the entity/blueprint/connection model, ~384 entity types with their events, the BF3 server
   event/hook inventory (a ready feature checklist), the 30 Hz server tick, and the kit/spawn/vehicle mechanics that
   fun-bots exercises [V §1.2–1.4].
4. **Bring-your-own-data (BYOD) projects survive when they ship no assets, contain no original code, and do not
   pretend to be the publisher**: OpenMW (Bethesda approved with conditions), OpenRA, OpenRCT2, OpenXcom, CorsixTH
   (an EA IP, alive since 2009), Julius/Augustus, ioquake3 (id's own GPL code). The two counter-examples are re3/reVC
   (decompiled Take-Two code: DMCA 2021, lawsuit, settled 2023) and EA's own 2017 request to the Revive Network
   (redistributed Battlefield clients under Battlefield branding) [V/R §3.1–3.3].
5. **EU law is the strongest footing**: ideas, functionality and **data file formats are not protected** (Art 1(2)
   Directive 2009/24/EC; CJEU *SAS v WPL* C-406/10); a lawful user may observe/study/test the program while running it
   (Art 5(3)); decompilation only for interoperability (Art 6); contract terms overriding 5(3)/6 are void (Art 8)
   [V §3.5]. EA's User Agreement forbids reverse engineering and extracting data "unless … permitted by law" [V §3.4].
   US: 17 U.S.C. §1201(f) interoperability exemption [V]. **Reading data formats is the safe part; disassembling
   `bf3.exe` to copy behaviour is the risky part — do not do it.**
6. **The user's install is complete**: Origin SKU, build 1147186, Patch (authoritative version 7) + Xpack1–5 (Back to
   Karkand, Close Quarters, Armored Kill, Aftermath, Endgame), 29 MP levels, English loc only, 34.0 GiB [V §4.2]. BF3 is
   still sold (Steam: Premium Edition only, 39,99 €, all five expansions) and PC Battlelog still answers [V §4.1].
7. **Scope**: MVP = 2–3 levels, TDM + Conquest Small, 4 kits with one primary each, infantry gunplay from BF3 data,
   two land vehicles, bots, ≤ 24 slots on a dedicated/listen server. v1 = all levels/modes, all weapons/gadgets/vehicles
   (air and sea), destruction, 64 slots with bot fill. Skip forever: Battlelog, Origin/EA login, Blaze emulation,
   progression/stats, PunkBuster [I §5]. **Hard risks in order: Ant animation decode, 64-player netcode with vehicles +
   destruction + bots on one server, vehicle feel, trademark/IP letters** [I §5.3].

---

## 1. Venice Unleashed (VU) — a knowledge source, never the route

### 1.1 What it is

| fact | tag | source |
|---|---|---|
| A modding and private-server platform that **runs the original BF3 client/server binaries** with its own scripting (VeniceEXT, Lua), WebUI and server browser; supports 30/60/120 Hz servers and spectating | R | https://veniceunleashed.net/ (fetched 2026-10-10); `C:\Users\olku\VU-Docs\content\hosting\config.md:85-99` [V] |
| Emulates BF3's **Blaze** online backend; announced by NoFaTe in 2013, beta sign-ups via PC Gamer, public release December 2020 | R | PC Gamer headline "Battlefield 3 server emulator accepting beta signups" (https://www.pcgamer.com/au/battlefield-3-server-emulator-accepting-beta-applications); shazoo.ru 2013-04-22; https://techraptor.net/gaming/news/battlefield-3-community-modding-platform-vu-released |
| Requires a licensed, up-to-date BF3 launched once; the user creates a VU account and **links an Origin account to verify ownership** | V | `C:\Users\olku\VU-Docs\content\general\playing.md:11-15`; https://veniceunleashed.net/about FAQ |
| VU's own legal position (paraphrased): it modifies/distributes no EA/DICE assets, runs separately from the vanilla client, is lawful under EU interoperability law, never works with pirated copies; footer disclaims affiliation with EA and DICE | V (that VU says so) | https://veniceunleashed.net/about |
| VU profiles are separate from Battlelog: no progression, everything unlocked | V | same FAQ |
| Before VU, BF3 PC servers could only be rented from EA-approved providers | R | PC Gamer via search summary (https://www.pcgamer.com/au/get-private-custom-battlefield-3-servers-with-a-new-mod/) |
| Actively maintained: client changeset 20878 dated 24/06/2026 | V | `C:\Users\olku\VU-Docs\content\general\changelog.md:8` |
| Same team runs **Project Rome**, an unofficial Bad Company 2 master server, after BC2's official shutdown (Dec 2023) | R | https://veniceunleashed.net/ ; Steam guide https://steamcommunity.com/sharedfiles/filedetails/?id=2734495434 |
| No public record of EA/DICE legal action against VU (searched 2013–2026) | I (absence of evidence) | searches logged in §6 |
| Rime is VU's own toolkit: VU's custom-content guide uses Rime (`mount_game … Frostbite2_0`, `dump_partition_json`, `build_sb`) to build **new** superbundles | V | `C:\Users\olku\VU-Docs\content\vext\guides\custom-content.md:14-80` |

Why it is not our route: it executes and hooks `bf3.exe`, depends on Origin/EA account linking and on VU's own
servers, and its mods patch EBX in memory or mount extra superbundles. The user's rule excludes all of that. What we
take from it is knowledge and, with the user's own VU/BF3 client, **reference observation** (screenshots, timings,
handling feel) — which EU Art 5(3) explicitly allows a lawful user to do (§3.5).

### 1.2 What VU-Docs teach about BF3 internals (all [V] unless noted)

* **Data hierarchy** (`content\vext\guides\data.md:12-90`): superbundle (`.sb`/`.toc`) → bundle (unique name, e.g.
  `Levels/MP_Subway/MP_Subway`, `Levels/MP_Subway/TeamDeathMatch`) → chunks / resources / partitions (EBX) →
  instances (GUID, one primary instance). Mount order to play Metro: `Chunks0/1/2`, `SpChunks`, `MpChunks`, `UI`,
  `Weapons`, `Loc\<lang>`, then `Levels\MP_Subway\MP_Subway.sb` (`data.md:14-24`). The handoff's loader already does
  this; the list is a cross-check for which superbundles a **multiplayer-only** build must mount (no SP/COOP levels).
* **Data layout** (`data.md:71-90`): `Characters/` (soldier parts, `Characters/Soldiers/MpSoldier`), `Gameplay/Kits`,
  `Vehicles/<name>`, `Vehicles/common/weapondata`, `Weapons/<name>`, `Weapons/Accessories`, `Weapons/Common/Bullets`,
  `Levels/<id>`. This is the map for the soldier/weapon/vehicle topics.
* **Type schema**: `types\fb\*.yaml` = 2,491 Frostbite types (classes, structs, enums with fields and inheritance)
  [V count]. Do **not** vendor it (no licence); our EBX reader takes types from the partitions' own descriptors, as the
  IW4L reader does (handoff §3). Use the yaml as documentation only.
* **Entity model** (`content\vext\guides\entities.md`): data (`EntityData`/`Blueprint`) → runtime entities; blueprints
  are prefabs spawned as an `EntityBus`; three connection kinds: **property** (value flows source→target), **link**
  (reference), **event** (event id on source fires event on target); ids are name hashes, e.g. `OnProjectileSpawned`
  → `Start` on an effect (`entities.md:356-374`). Some blueprints are **networked** (spawned on server, replicated to
  clients) when `needNetworkId` is set (`entities.md:332-354`). Entities accept named events (`Enable`, `Start`…) and
  a fixed set of property names (`Transform`, `Enabled`, `IntValue`, `Team`… `entities.md:223-248`).
* **Entity catalogue**: `content\vext\entities.md` lists **384** entity types with inheritance and supported events
  [V count of `## ` headings]. Use it to decide which logic-graph entities our runtime must interpret vs. replace with
  native rules (handoff 04 §1 already decided: native rules).
* **Server event/hook inventory** (`content\vext\ref\server\event\*.md`, `hook\*.md`): capture point captured/lost,
  combat-area deserting/returning, `LifeCounter:BaseDestroyed` (Rush), man-down/revive accepted/refused, kit pickup,
  resupply, suppressed enemy, spawn at vehicle / on player / on selected spawn point, squad change/leader, scoring stat
  event, round over/reset, vehicle damage/destroyed/disabled/enter/exit/spawn/unspawn; hooks `Soldier:Damage`,
  `BulletEntity:Collision`, `Player:FindBestSquad/Team`, `Server:SuppressEnemies`. This is a **checklist of BF3 MP
  mechanics** to reimplement (§5).
* **Server tick**: BF3's regular server frequency is **30 Hz** (`vu.FrequencyMode` returns `regular` for 30 Hz,
  `content\hosting\commands.md:73-75`); VU's changelog lists bugs where running at 60/120 Hz changed acceleration and
  damage-expiry timing (`content\general\changelog.md:134-135`) — BF3's simulation is tick-coupled in places. Lesson:
  fixed-step sim at a chosen rate, every timer in seconds.
* **Slots**: `-maxPlayers 64` example; spectator slots count against the cap (`content\general\args.md:242-248`).
* **Modes**: 16 mode ids and per-level availability for all 29 MP levels (`content\hosting\maps.md:8-96`), matching
  the handoff's 230 level×mode combinations.
* **Venice** is BF3's internal codename (VU, `VeniceSoldierCustomizationAsset`, the "Venice (BF3)" EBX dump) [V
  `types\fb\VeniceSoldierCustomizationAsset.yaml`; Venice-EBX description via GitHub API].

### 1.3 VU mods worth studying (knowledge only; licence decides what may be reused)

| project | what | licence | lesson for us |
|---|---|---|---|
| **Joe91/fun-bots** (local fork `C:\Users\olku\unfun-bots`, origin `Vibingmywaytovictory/unfun-bots`, which is the user's GitHub account) | Bots as real BF3 players; 36.8k Lua lines; waypoint maps for 185 level/mode files; vehicle and air control | **none** (all rights reserved) [V API + `git log --all -- LICENSE*` empty] | §1.4. Ideas yes, code/maps only with permission |
| **BF3RM/MapEditor** | Real-time in-game level editor (Vue WebUI); saves → `LevelLoaderGen` → custom bundles | **GPL-3.0** [V] | Overlay edits on top of BF3 levels as JSON save files: a model for an optional "level overlay" feature later. Porting its code makes our code GPL |
| **BF3RM/NoHavokGen** | Generates a mod that replaces Havok-baked static objects with editable Frostbite objects | none [V] | Confirms BF3 static placements live inside Havok group resources (handoff 04 §3.1); our loader already expands them |
| BF3RM/BlueprintManager, CustomLevelLoader (archived), VEManager/VEEditor, BundleMounter, RMBundles | Spawn/unspawn blueprints with client sync; custom levels; visual-environment presets; bundle mounting | none [V] | Bundle dependency knowledge per map; VE presets show which `VisualEnvironment` fields matter visually |
| **Reality Mod** (BF3RM) | Realism overhaul on VU, released 2022-07-17 after ~7 years | site/landing MIT, mod mostly unlicensed [V org listing] | A large team needed years even *with* the original engine; plan scope accordingly [R https://mp1st.com/news/battlefield-3-reality-mod-release-locked-for-july-17-explosive-new-trailer-features-a-more-serious-affair] |
| thysw95/vu-progression | Progression mod | AGPL-3.0 [V] | not needed (we skip progression) |
| kiwidoggie/kPM, chrisguen/hidden-gamemode, HectorBailey/payload, Joe91/fun-bot-zombies, Maxinger15/VU-Killstreak | Game-mode mods (promod, Hidden, payload, zombies, killstreaks) | none [V] | Modes are scriptable on top of the same primitives (spawn, team, damage hooks) — design our mode layer as data + small C++ rule classes |
| VeniceUnleashed/Venice-EBX | Text dump of all BF3 partitions | none; it is EA data [V API] | Handy to *read*; never vendor; our tools can produce the same from the user's install |

### 1.4 How BF3 spawn, kit and vehicle logic is driven (what fun-bots shows) [V code; semantics R/I as marked]

* **Bots are real players**: `PlayerManager:CreatePlayer(name, team, squad)` then `player:CreateSoldier(kit,
  transform)` and `player:SpawnSoldierAt(soldier, transform, pose)` (`unfun-bots\ext\Server\BotManager.lua:842-913`);
  the authors' comments time these at ~3 ms and ~17 ms (`:905, :913`) — soldier creation is expensive in Frostbite;
  pool soldiers in ours [I]. Bots drive the soldier through the same input concepts as humans:
  `input:SetLevel(EntryInputActionEnum.*, value)` (`ext\Server\Bot\Bot.lua:523-527`; actions `EIAFire`, `EIAZoom`,
  `EIAStrafe`, `EIAJump`, `EIAMeleeAttack`, `EIASelectWeapon7`… `Bot\BotAttacking.lua:8-166`). **Design lesson**: make
  bots emit the same input-command struct as players so prediction, lag compensation and anti-cheat see one path [I].
* **Kits**: a kit is a `VeniceSoldierCustomizationAsset` at `Gameplay/Kits/<TEAM><Kit>[ _GM | _GM_XP4 | _XP4 | _XP4_SCV ]`
  (e.g. `Gameplay/Kits/USAssault`; Gun Master and XP4/Scavenger variants) (`ext\Server\BotSpawner.lua:2186-2208`).
  Schema: `VeniceSoldierCustomizationAsset` → `SoldierCustomizationAsset {weaponTable: CustomizationTable}` →
  `CharacterCustomizationAsset {labelSid, uiHudIcon, voiceOverLabels, visualTable, specializationTable}`;
  `CustomizationTable {unlockParts}` → `CustomizationUnlockParts {uiCategorySid, selectableUnlocks}`
  (`C:\Users\olku\VU-Docs\types\fb\SoldierCustomizationAsset.yaml:34-37`, `CharacterCustomizationAsset.yaml`,
  `CustomizationTable.yaml`, `CustomizationUnlockParts.yaml`). `TeamData {soldier, faction, soldierCustomization[],
  vehicleCustomization[]}` (`TeamData.yaml`) is where a team's kits hang.
* **Appearance** is a separate unlock: `Persistence/Unlocks/Soldiers/Visual/MP[_XP4]/<Us|RU>/MP_<US|RU>_<Kit>_Appearance01`
  or `_Appearance_<colour>` (`BotSpawner.lua:2214-2244`).
* **Loadout**: `CustomizeSoldierData {weapons[], activeSlot, deathPickup, removeAllExistingWeapons, disableDeathPickup}`
  with `UnlockWeaponAndSlot {weapon: SoldierWeaponUnlockAsset, slot: WeaponSlot, unlockAssets[]}` (attachments)
  (`types\fb\CustomizeSoldierData.yaml`, `UnlockWeaponAndSlot.yaml`). fun-bots' slot use: `WeaponSlot_0` primary,
  `_1` pistol, `_2` first gadget for Engineer/Recon, `_4` first gadget for Assault/Support, `_5` second gadget, `_6`
  grenade, `_7` knife (`BotSpawner.lua:1999-2120`). `SoldierWeaponUnlockAsset {weapon, extra, weaponIdentifier}` ←
  `UnlockAssetBase {unlockScore, …, autoAvailable, hiddenInProgression}`: progression data sits on every unlock; we
  ignore it and treat all as unlocked, like VU [I].
* **Spawning in Conquest** goes through the flag's own entities: iterate `ServerCapturePointEntity`, take one owned by
  the team, find the `ServerCharacterSpawnEntity` in its `bus.entities` whose `CharacterSpawnReferenceObjectData.team`
  is the team or 0, prefer flags being attacked (`flagLocation < 100`, `isControlled`), then fire a `Spawn`
  `ServerPlayerEvent` on that spawner (`BotSpawner.lua:1049-1157`). This matches the handoff's data reading (spawner
  `CharacterSpawnReferenceObjectData` → `AlternateSpawnEntityData` points, handoff 04 §2.2) and shows BF3's spawn
  choice is per spawner, the engine picking the point [R semantics]. `CharacterSpawnReferenceObjectData {playerType,
  vehicleEntryIndex, template, …}`, `SoldierSpawnTemplateData {customizeSoldierData, sensing, behaviour,
  animationTemplate, voiceOverLabels}` (yaml).
* **Squad / vehicle / beacon spawn** are handled by fun-bots' own `GameDirector:TrySpawnInVehicle/OnSoldier/OnBeacon`
  (`ext\Server\GameDirector.lua:1221-1297`): BF3 exposes them as player spawn events (`player_spawnatvehicle`,
  `player_spawnonplayer`, VU event list).
* **Vehicles**: identity is `VehicleEntityData.controllableType` (path tail, e.g. `M1Abrams`)
  (`ext\Server\Vehicles.lua:11-20`). fun-bots keeps a **hand-made table of 79 controllables** (10 tanks, 6 IFV, 8
  planes, 3+4+3 attack/scout/transport helicopters, 18 unarmoured, AA, artillery, gunship, stationary weapons, MAV,
  T-UGS…) with muzzle part indices, projectile speed and gravity copied by hand from the EBX dump
  (`ext\Shared\Constants\VehicleData.lua:1-15`, counts by `grep`). **We read these from EBX** (`WeaponFiringData`,
  projectile `initialSpeed`, `gravity`) instead of tables [I]. `VehicleSpawnReferenceObjectData` carries
  `botBailWhenHealthBelow`, `botBailOutDelay`, `keepAliveRadius`, `timeUntilAbandoned`, `wreckDuration`,
  `respawnRange` (yaml) — bot and respawn rules are partly data.
* **Bots fly** helicopters and jets with **PID controllers** on the input axes toward a target point above the
  objective (`ext\Server\PidController.lua`, `Bot\VehicleChopperControl.lua:13-60`, `Bot\VehicleJetControl.lua`). IW4L's
  bots never flew (handoff 04 §9); this is existence proof that simple PID flight is enough for bot fill [I].
* **Waypoint maps**: `mapfiles\<Level>_<Mode>.map`, 185 files, CSV `pathIndex;pointIndex;transX;transY;transZ;
  inputVar;data` in **BF3 world metres** (Metro CQL 3,844 rows); `inputVar = speed(4 bits: 0 wait,1 prone,2 crouch,
  3 walk,4 run) | extra<<4 | option<<8`; `data` JSON (`Objectives`, `Links`, vehicle tags)
  (`C:\Users\olku\unfun-bots\mapfiles\MP_Subway_ConquestLarge0.map:1-17`, `ext\Server\NodeCollection.lua:84-87`).
  Hand-recorded paths; the fork's own design doc admits its voxel bake links through walls
  (`unfun-bots\docs\navmesh-design.md:1-30`). For us: **bake our own navmesh** from collision (handoff 04 §9); the
  fun-bots maps could serve as a validation set (do bots reach every objective a human-authored path reaches?) *only*
  with the authors' permission.
* **Weapons**: fun-bots lists 174 weapon/attachment-set entries over ~86 base names (31 assault, 23 carbine, 25 LMG,
  20 PDW, 35 sniper, 23 shotgun, 19 pistol entries, plus gadgets) (`ext\Shared\WeaponList.lua`, counts by `grep`) —
  an order of magnitude for v1 scope [R].

### 1.5 Licences and lessons

* fun-bots, Rime, VU-Docs, Venice-EBX, BlueprintManager, NoHavokGen: **no licence** → copyright reserved; we may read
  and learn ideas (not protected) but not copy expression. Exception path: ask the authors (Joe91 et al.; OrfeasZ for
  Rime) for a permissive grant if a specific file is worth porting [I].
* MapEditor GPL-3.0 and vu-progression AGPL-3.0: copying any code forces our project to (A)GPL [V licences].
* The user's own commits in the unfun-bots fork are theirs, but the fork as a whole is a derivative of unlicensed
  upstream code: it cannot be relicensed without upstream consent [I].

---

## 2. Other reverse-engineering and reimplementation work

### 2.1 Rime (`C:\Users\olku\Rime`)

* Repo `ModdersLink/Rime`, public, created 2019-04-21, last push 2026-09-12, 4 stars, **license: none** [V GitHub API;
  no LICENSE file anywhere in `git ls-tree` or history]. Commits by Orfeas Zafeiris / OrfeasZ (NoFaTe, VU's author;
  the VU custom-content guide distributes Rime builds) [V `git log`; `custom-content.md:14-20`].
* 53 C# projects, ~469k lines incl. generated type classes; FB2 readers for containers, EBX, meshes, textures, Havok,
  terrain, Enlighten, shaders; `RimeLib.Animation.Frostbite2_0` has 265 `.cs` files incl. `ant\`, `EA\Compression\DCT`;
  010 templates `External\010templates\{AssetBank,FB2_Mesh,HavokPhysicsData,ShaderDB_Template}.bt` [V counts].
* Use: **run it locally as an oracle** (the old project matched 15,583/15,583 Rime nodes, handoff §9) and read it to
  understand formats. Do not translate its code line by line into the repo; write our own from the format knowledge
  (formats are not protected, *SAS v WPL*), or ask OrfeasZ for a licence for the AssetBank/DCT decoder, which is the
  one piece that could save months (§5.3) [I].

### 2.2 Frosty Toolsuite

* `CadeEvs/FrostyToolsuite`: README licence **CC BY-NC-ND 4.0** (non-commercial, **no derivatives**) [V
  https://github.com/CadeEvs/FrostyToolsuite]. Profiles in the 1.0.6.3 tree: Anthem, BF1, BF4, BFH, BFV, Dragon Age,
  FIFA17–20, Madden19/20, Mass Effect, Mirror's Edge, NFS14/16/17/Edge/Heat, PvZ1–3, SWS, Star Wars II — **no BF3** [V
  `api.github.com/repos/CadeEvs/FrostyToolsuite/git/trees/1.0.6.3`]. It is a mod manager that **repacks game data**,
  the opposite of our rule.
* What applies to FB2 conceptually: the FB3 toc/sb/cas family descends from FB2's, and EBX is the same idea with a
  newer layout; nothing in Frosty is needed — IW4L's readers already handle FB2 [I].

### 2.3 Format research: Frankelstner, Xentax/Zenhax, others

| source | licence | status | use |
|---|---|---|---|
| NicknineTheEagle/Frostbite-Scripts (`frostbite2/` = BF3): `dumper`, `ebxtotext`, `ebxtoasset` | **MIT**, "Portions Copyright 2012-2017 Frankelstner, 2016 NoFaTe" | active 2026-01 [V raw LICENSE/README] | The only permissively licensed FB2 code with EBX/DbObject parsing; may be ported with the MIT notice |
| mitsuhiko/frostbite2-stuff (`libfb2`): `.sb/.toc/.cas/.cat` readers, 2011 | BSD-style (redistribution with notice) | dormant since 2011-11 [V raw LICENSE] | historical cross-check |
| Venice-EBX dump | none | 2022 | made with a modified Frankelstner EBX→text script [R search summary] |
| XeNTaX forum | — | closed Nov 2023; successor forum Reshax; Wayback archive allowed by the owner | old FB threads readable via web.archive.org [R https://datahorde.org/?p=2923] |
| Zenhax | — | abandoned by owner (second-hand) | [R same] |
| GreyDynamics/Frostbite3_Editor | NOASSERTION | discontinued 2016 | FB3 only [V API] |

### 2.4 BF3 private-server emulators (existence and legal status only)

* **VU** emulates Blaze and requires verified ownership (§1.1). **BlazeServer**-style C# Blaze emulators and LAN forks
  exist on GitHub (status unverified) [R search summary]. Piracy-oriented emulators that bundle cracked clients also
  exist; not named or linked here.
* Legal status [I]: emulating a backend so owners can keep playing is argued under interoperability (EU Art 6, US
  1201(f)) and has survived in VU's case; distributing game clients or bypassing DRM is infringement/circumvention
  (re3 and Revive Network outcomes, §3.2). **Irrelevant to us**: we never talk to Blaze, Battlelog or Origin and never
  run `bf3.exe`.

### 2.5 Fan remakes / reimplementations of Battlefield (and an EA game in Godot)

| project | engine / lang | model | licence | state (2026-10) | lesson |
|---|---|---|---|---|---|
| **Moxnatiy/OpenBattlefield2** | own engine, C++20 | "clean-room" Refractor 2 reimplementation, **reads your own BF2 install, ships no content**; joins *original* dedicated servers, predicts soldier physics (343/344 states within 1 mm), uses BF2's own menus via Ruffle, own animation/ragdoll ports | MIT | created 2026-09-09, active, 35★ [V README via `gh api`] | Closest precedent to ours (BYOD, one-to-one behaviour). But it cites **addresses in the original binary** as sources (binary analysis, not strict clean-room) and uses "Battlefield" in its name — both are the risks we avoid |
| **Open-BFME/openbfme-godot** | **Godot 4.7 + godot-cpp 10.0.0-stable, C++ GDExtension** | BFME II recreation on the retail files; mount refuses unknown/mismatched archives (md5 list), hash cache in `user://` | GPL-3.0 (forced: translates EA's GPL Zero Hour source) | created 2026-07-25, active [V README/NOTICE] | Architecture twin: game logic in a C++ GDExtension, Godot as device layer, install paths in `user://`, NOTICE with non-affiliation text. Also uses decompilations (a risk we avoid) |
| SquarerFive/bf2redux | UE4 | remake with **new** assets | Apache-2.0 | dormant 2020, moved | asset re-creation stalls hobby remakes [V README] |
| rhysmcg/bf2vehicles | Godot | BF2 vehicle physics replica | none | 2026-10 | vehicle feel is its own project [V API] |
| DohmBoy64Bit/OpenBFVietnam | C | clean-room BF Vietnam engine | none | 2026-06, tiny | — [V API] |
| Project Reality: BF2 | Refractor 2 | mod → standalone (no BF2 install needed) in 2015 with own accounts | — | alive | whether EA consented is not documented [R https://en.wikipedia.org/wiki/Project_Reality] |
| Squad | UE4→UE5 | commercial successor of PR, 100 players | commercial | released 2020-09-23 | large-scale combined arms is feasible but took a funded studio years [V https://en.wikipedia.org/wiki/Squad_(video_game)] |
| BattleBit Remastered | Unity | 254 players (127v127), 3 developers | commercial | EA (early access) 2023-06-15 | huge player counts are possible with simple visuals and a custom stack [V https://en.wikipedia.org/wiki/BattleBit_Remastered] |
| Revive Network (BF2/2142/Heroes) | original binaries | revived online play, **distributed game files** under Battlefield branding | — | shut down 2017 after EA's legal team asked it to stop distributing EA IP | see §3.3 [V https://www.pcgamesn.com/battlefield-heroes/battlefield-heroes-revive-network-shutdown] |

No public, playable BF3 remake in Unreal/Unity/Godot was found (GitHub searches "battlefield 3 unreal", "battlefield
remake", "battlefield unity remake" returned nothing relevant) [V searches, §6].

### 2.6 Godot multiplayer FPS projects and libraries worth studying

| project | lang | licence | what to take |
|---|---|---|---|
| foxssake/netfox | GDScript | MIT | tick timing across machines, client prediction + server reconciliation, interpolation, rollback; a readable design reference [V README] |
| vantixorg/vantix | C#, Godot 4.6, D3D12, **Jolt at 128 Hz**, LiteNetLib UDP | NOASSERTION; assets not open | server authority, prediction, server-side rewind lag compensation, fog-of-war packet culling — and it **bypasses Godot's high-level multiplayer** for its own transport [V README] |
| unfa/liblast | GDScript | MIT | libre Godot FPS; stalled 2023 [V API] |
| devloglogan/MultiplayerFPSTutorial, Travh98/fpsdeathmatch (GodotCon 2025 workshop) | GDScript | MIT | minimal SceneMultiplayer patterns only [V API] |
| Godot's own `modules/multiplayer`, `modules/enet` | C++ | MIT | how SceneMultiplayer/ENet peers work; we sit below them |
| ValveSoftware/GameNetworkingSockets; mas-bandwidth/yojimbo + netcode | C++ | BSD-3-Clause | reliable/unreliable channels, encryption, connection tokens — usable via `MultiplayerPeerExtension` if ENet is not enough [V `gh api`] |
| GodotSteam; 3ddelano/epic-online-services-godot | C++ GDExtension | none / MIT | only if Steam/EOS transport is ever wanted (not planned) |

Godot facts for 64 players [V `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json`]:
`ENetMultiplayerPeer.create_server(port, max_clients = 32, …)` and `ENetConnection.create_host_bound(…, max_peers =
32, …)` default to **32** — pass 64+ explicitly; ENet's protocol allows peer ids up to `0xFFF`
(`ENET_PROTOCOL_MAXIMUM_PEER_ID`, https://raw.githubusercontent.com/lsalzman/enet/master/include/enet/protocol.h:19)
[V]. `MultiplayerPeerExtension`, `SceneMultiplayer`, `MultiplayerSynchronizer`, `MultiplayerSpawner`,
`WebRTCPeerConnection` exist; `CSharpScript` does not in this build [V]. `MultiplayerSynchronizer` sends configured
properties to all peers every network frame unless `replication_interval`/visibility filters say otherwise, and cannot
sync Object/Resource properties [R https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html].
**No shipped Godot shooter with 64-player authoritative servers was found** [V negative search]. Consequence [I]: own
snapshot/delta replication in C++ over ENet (or GNS) through `MultiplayerPeerExtension` or `ENetConnection`, not node
synchronizers; interest management and priority per client; server tick on its own thread (IW4L lesson, handoff §7).

Cross-topic note (godot-cpp): godot-cpp is now **versioned independently** (tags `10.0.0-rc1/rc2/stable`, besides
`godot-4.x-stable`) and v10 can target Godot 4.3+ via `api_version=…` or `custom_api_file=extension_api.json` dumped
from the exact build [V `git ls-remote`; README https://github.com/godotengine/godot-cpp]. "Pin godot-cpp to the exact
build" therefore means: godot-cpp v10 + `custom_api_file` = this build's dump (already in `research\godot-api\`).

---

## 3. Legal and distribution (not legal advice)

### 3.1 The bring-your-own-data precedent

| project | original (owner) | what it ships | code origin | licence | outcome | source |
|---|---|---|---|---|---|---|
| OpenMW | Morrowind (Bethesda) | engine only; you must own Morrowind | written from scratch; never uses the original exe | GPL-3.0 | alive (repo since 2009); Bethesda **approved continued development with a condition** (don't promote Morrowind on Android/mobile) | [V https://openmw.org/faq/; API] |
| OpenRA | C&C/RA (EA) | engine; downloads original assets or reads discs | own | GPL-3.0 | alive (repo since 2010); EA made RA freeware 2008 and released TD/RA source GPL in 2020 and more C&C source in Feb 2025; "EA has not endorsed" | [V https://www.openra.net/about/; https://en.wikipedia.org/wiki/Command_%26_Conquer:_Red_Alert; R https://gamefromscratch.com/ea-release-command-conquer-series-source-code/] |
| CorsixTH | Theme Hospital (**EA**) | engine; original data from CD/GOG/EA | own | MIT | alive since 2009; README points buyers to EA's store | [V README, API] |
| OpenRCT2 | RCT2 (Chris Sawyer/Atari) | engine; needs RCT2 files | incremental C++ "accurate clone" from 2014 | GPL-3.0 | alive | [V README; R https://en.wikipedia.org/wiki/OpenRCT2] |
| OpenXcom | UFO/TFTD (MicroProse/2K) | engine; needs vanilla data | own | GPL-3.0 | alive since 2011 | [V README] |
| Julius / Augustus | Caesar III (Sierra/Activision) | engine; "will not run without the original Caesar 3 files" | same logic as original (save-compatible) | AGPL-3.0 | alive | [V README] |
| ioquake3 | Quake III (id) | engine from **id's own GPL source release**; data must be bought | publisher-released | GPL-2.0 | alive | [V README] |
| OpenLara | Tomb Raider (Core/Eidos→Crystal) | engine | own | BSD-2-Clause | alive | [V API] |
| re3 / reVC | GTA III/VC (**Take-Two**) | engine needing original data | **decompiled** from the original binaries | — | DMCA Feb 2021, counter-notice restore, lawsuit Sept 2021 vs 4 named devs, settlement in principle Feb 2023, dismissed with prejudice Apr 2023 | [V https://torrentfreak.com/take-two-dismisses-claims-against-lead-defendants-in-gta-mods-lawsuit-230405/] |
| Revive Network | BF2/2142/Heroes (**EA**) | **game files** + revived services, Battlefield branding | original binaries | — | EA legal team asked it to stop distributing/using EA IP; shut down Nov 2017 | [V PCGamesN] |

### 3.2 What separates survivors from takedowns [I, from §3.1]

1. **No original assets distributed**, ever (installer, repo, CDN, release zip, cache downloads). Revive Network
   distributed files → stopped.
2. **No original code**: clean implementation from formats and observation, not decompiled source. re3/reVC were
   decompilations and were sued; OpenMW/OpenXcom/CorsixTH never were.
3. **No impersonation**: no publisher trademarks as the project's name, no fake official services; disclaimers.
   EA's 2017 letter was framed around IP and affiliation confusion [R KitGuru/CheatCC via search].
4. **Requires a legitimate copy** and says so (OpenMW, Julius, CorsixTH, VU); publishers' sales are not harmed.
5. **Not competing with a live product**: re3's lawsuit came as Take-Two launched the Definitive Edition (Nov 2021)
   [I timing]. BF3 is still sold (§4.1) — a free BF3 with bots would not replace a purchase because it *requires* one.
6. **Low commercial footprint**: no donations tied to the IP, no paid builds [I].

### 3.3 EA and BF3 specifically

* DICE never released BF3 mod tools: 2011 statements cite engine complexity, investment, and security/exploit
  concerns [R https://pcper.com/2011/07/battlefield-3-no-plans-for-mod-tools/, https://www.gamewatcher.com/2011-20-09-dice-no-bf3-mod-tools-for-a-while].
  Frankelstner's client-side colour mod was reportedly kicked by PunkBuster on ranked servers [R search summary of https://www.dsogaming.com/?p=36538 and https://guru3d.com/story/battlefield-3-mod-offers-natural-colors].
* EA vs fan projects: Revive Network (2017) stopped on request [V]; **no** record of action against VU in its
  public life since Dec 2020 (or its 2013 announcement) [I absence]; EA tolerates CorsixTH and released C&C source
  under GPL (2020, 2025) [V/R §3.1] — EA's pattern is to act on **distribution and branding**, not on engines that
  require the game.
* No EA statement about VU was found [V negative search].

### 3.4 EA User Agreement (version "Last Updated May 14, 2026") — paraphrased [V https://www.ea.com/legal/user-agreement]

| § | paraphrase | relevance |
|---|---|---|
| intro | "EA Services" include game software and its updates | BF3 is licensed under it |
| 2 | no access, copy, modification or distribution of EA Services/content, and **no reverse engineering or extraction of source code or other data**, *unless EA authorises it or the law permits it* | reading BF3 data is "extracting data"; our footing is the "permitted by law" carve-out (EU Art 5(3)/6/8, §3.5) and private use. Contract breach → the remedy EA reserves is licence/account termination (§7–8) |
| 2 | services are licensed, not sold | — |
| 5 | user-generated content licence to EA | n/a |
| 6 | no cheating, hacking, disrupting EA systems | we never touch EA systems |
| 7(C) | "Unauthorized Third-Party Program" includes add-ons and **mods** that modify the game; EA may investigate and terminate | applies to programs that interact with EA games/services; ours never runs with BF3 or online [I] |
| 11 | some services may let you play on servers EA does not own or control | — |

### 3.5 EU: Directive 2009/24/EC (computer programs) — paraphrased [V https://www.legislation.gov.uk/eudr/2009/24/article/5, /article/6, /article/8 (EU text as retained in the UK)]

* **Art 1(2)**: ideas and principles underlying a program, including those underlying its interfaces, are not protected.
* **Art 5(3)**: a person entitled to use a copy may, without authorisation, **observe, study or test** the program's
  functioning to determine its ideas and principles, while loading/displaying/running/transmitting/storing it.
* **Art 6(1)**: reproduction and translation of code (decompilation) without authorisation where **indispensable** to
  obtain information needed for **interoperability of an independently created program with other programs**, if
  (a) done by a licensee, (b) the information was not readily available, (c) confined to the necessary parts.
  **6(2)**: the information may not be used for other goals, given to others unless needed for interoperability, or
  used to make a program **substantially similar in its expression**, or for any infringing act. **6(3)**: no use
  that unreasonably prejudices the rightholder or conflicts with normal exploitation.
* **Art 8 (2nd para.)**: contract terms contrary to Art 6 or Art 5(2)/(3) are **null and void**.
* **CJEU C-406/10 *SAS Institute v World Programming*** (2 May 2012): functionality, programming language and **the
  format of data files** are not protected expression; a licensee may observe/study/test to learn ideas and
  principles [V https://en.wikipedia.org/wiki/SAS_Institute_Inc_v_World_Programming_Ltd].

How our activities map [I]:

| activity | footing | risk |
|---|---|---|
| implementing readers for toc/sb/cas/EBX/MeshSet/DxTexture/Havok/XAS from documented formats | formats unprotected (*SAS*); no copying of code | low |
| de-obfuscating `layout.toc`/`cas.cat` (XOR key stored in each file's own header; handoff 01 §2) | needed to read data the user owns; no secret key; nothing re-signed or written | low–medium (see §3.6 for the US view) |
| loading the user's assets into RAM at runtime | lawful user, private use; the data never leaves the machine | low |
| persistent converted cache in `user://` | a private reproduction of copyrighted assets; national private-copy rules differ; mitigated by never distributing it | low if never shared |
| observing real BF3/VU while running it (shots, timings, handling) | Art 5(3) | low |
| parsing compiled DXBC pixel shaders to recover material constants (handoff 03/07) | DXBC is code; extracting constants for rendering the data correctly is interoperability-shaped, but keep to parameters and write our own shaders | medium-low |
| disassembling `bf3.exe` to copy gameplay algorithms/constants | outside Art 6 (not interoperability; 6(2) bars "substantially similar" programs) | **high — don't** |
| porting unlicensed community code (fun-bots, Rime) | copyright of those authors | **high without permission** |
| emulating Blaze/Battlelog/Origin | interoperability argument exists (VU) but it impersonates EA services | not needed — don't |

### 3.6 US: 17 U.S.C. §1201(f) — paraphrased [V https://www.law.cornell.edu/uscode/text/17/1201]

(f)(1) someone lawfully using a program may circumvent an access control to identify and analyse the elements needed
for **interoperability of an independently created computer program**, if not readily available and not infringing;
(f)(2) may build tools for that; (f)(3) may share the information/tools solely for interoperability; (f)(4)
interoperability = the ability of programs to exchange and use information. [I] The BF3 toc/catalog XOR is arguably
not an access control at all (the key ships in the file and the content files are plain), and in any case (f) is the
relevant exemption; US fair-use precedents on intermediate copying for reverse engineering (*Sega v. Accolade*,
*Sony v. Connectix*) were not re-read here [R, unverified].

### 3.7 Trademark [I]

"Battlefield", "Battlefield 3", "BF3", "Frostbite", "DICE", "EA" and their logos are EA's marks. Do not use them in the
project name, executable/package names, logo, domain, store pages or server names; nominative use in docs ("reads data
from your legally owned copy of Battlefield 3") with a non-affiliation notice is the accepted pattern (VU footer; the
openbfme-godot NOTICE) [V those texts]. OpenBattlefield2 and the Revive Network show both ends: the first is new and
unknown, the second was stopped partly over branding.

### 3.8 Recommended practices (ordered)

1. **Zero BF3 bytes in the repo, releases, CI artefacts, issue attachments, test fixtures or screenshots committed to
   the repo.** Tests that need data run only where `BF3_INSTALL` is set; golden values are *numbers* (counts, hashes),
   not extracted content.
2. **Runtime read of the unmodified install** at a user-chosen path, opened read-only (`FILE_SHARE_READ`, no write
   handles); the cache lives in `user://cache/<content-digest>/…` and is never uploaded, synced or downloadable.
3. **Network play shares state, never content**: on join the server sends the level/mode id and a digest of the
   bundles it mounted (from the catalogs' SHA-1s); a client without matching local data cannot join. No asset
   streaming over the wire, no "download missing content".
4. **No EA code, no decompiled code**: behaviour comes from EBX data, public talks, and black-box observation of the
   user's own BF3/VU (Art 5(3)). Keep a `SOURCES.md` per subsystem naming where each rule came from (OpenBattlefield2's
   good habit, minus binary addresses).
5. **No impersonation of online services**: no Blaze/Battlelog/Origin/EA-account emulation or calls; own protocol on
   own port; LAN + direct connect first, an optional own server list later, under the project's own name.
6. **Name and branding** free of EA marks; NOTICE with non-affiliation and "requires a legitimately owned copy";
   don't use BF3 logos/UI art for the project's own icon or site.
7. **Own the install check, not ownership verification**: detect `version.json`/`package.mft` and known builds (warn on
   unknown builds; re-convert on digest change). Do not add DRM checks or Origin calls; do not support cracked
   installs in docs or issue triage (OpenMW's stance).
8. **Non-commercial**: no paid builds, no donations framed around BF3 content [I].
9. **Third-party code hygiene**: only MIT/BSD/zlib/Apache code enters the repo (Godot, godot-cpp, Jolt, ENet: MIT;
   Recast: zlib; GNS/yojimbo: BSD-3; Frostbite-Scripts: MIT with Frankelstner/NoFaTe notices). No GPL code unless the
   project chooses GPL; no unlicensed code ever.
10. **If a letter arrives**: comply on branding immediately, keep the engine, ask what IP is alleged; BYOD engines
    that ship nothing have usually been left alone [I].

### 3.9 Licence for our own code

* **Recommendation: MIT** (same as Godot, godot-cpp, Jolt, ENet; simplest for contributors; matches CorsixTH,
  OpenBattlefield2) [I]. Apache-2.0 if an explicit patent grant is wanted.
* **GPL-3.0** is the alternative if the user wants forks to stay open (OpenMW/OpenRA/OpenRCT2 chose it). It is
  *required* if any GPL code (e.g. MapEditor) is ever copied in. Godot (MIT) + GPL code may be distributed together
  under GPL-3.0 [I].
* **fun-bots cannot be "ported under GPL"**: it has no licence. Porting it under any licence needs the authors' grant;
  re-deriving behaviour from its ideas needs nothing [V licence state; I conclusion].
* Decide before the first outside contribution; use DCO sign-offs so the choice stays the user's [I].

---

## 4. BF3 in 2026 and the user's install

### 4.1 Status

| item | finding | tag / source |
|---|---|---|
| PC Battlelog server browser | responds; page shows "Found 1337 servers", the first five empty (0/16, 0/10) | V https://battlelog.battlefield.com/bf3/servers/ fetched 2026-10-10 (population not established) |
| Console shutdown | PS3/X360 delisted 2024-07-31; servers for BF3, BF4, Hardline on PS3/X360 off 2024-11-07; PC unaffected | R https://en.wikipedia.org/wiki/Battlefield_3; press 2024 |
| PC shutdown announced? | none found for 2025–2026 | V negative search; EA service-update page renders no list to the fetcher |
| Sold? | Steam: **Battlefield 3 Premium Edition** 39,99 € (base + all 5 expansions), EA account link required, Origin client named, PunkBuster, online features may be retired on 30 days' notice; also in EA Play | V https://store.steampowered.com/app/1238820/Battlefield_3/ fetched 2026-10-10 |
| Expansions | Back to Karkand (Dec 2011), Close Quarters (Jun 2012), Armored Kill (Sep 2012), Aftermath (Dec 2012), End Game (Mar 2013); Premium gave early access | R Wikipedia |

### 4.2 The user's install (listed read-only) [V]

`C:\Program Files\EA Games\Battlefield 3\version.json` = `{"sku":"origin","build":"1147186","region":"WW"}`; base
`__Installer\installerdata.xml`: gameVersion 1.6.0.0, content ids 70619, 71067, DGR01609244, DGR01609245. `pb\` and
`pbsvc.exe` (PunkBuster) present; `Data\Win32\Loc` holds **en** only.

| folder | `package.mft` | title (installerdata) | size | levels |
|---|---|---|---|---|
| `Data` | — | Battlefield 3 | 10.86 GiB (`cas_01..10.cas`, `cas.cat` 3.49 MB, `layout.toc`, `initfs_Win32`, `bom.fb2` 104 MB) | MP_001/003/007/011/012/013/017/018, MP_Subway; COOP_002/003/006/007/009/010; 12 SP_*; FrontEnd, Web_Loading |
| `Update\Patch` | `Name patch`, `AuthoritativeVersion 7` | — | 5.90 GiB (`cas_01/02.cas`, own `cas.cat`) | patched base + **XP1–XP4** levels and `Xp1–Xp4Chunks` (nothing for XP5) |
| `Update\Xpack1` | `Name xpack1`, `MountOrder 0`, `RequireVersion 2` | (no installerdata) = **Back to Karkand** | 2.95 GiB | XP1_001 Strike at Karkand, XP1_002 Gulf of Oman, XP1_003 Sharqi Peninsula, XP1_004 Wake Island |
| `Update\Xpack2` | `xpack2`, `RequireVersion 4` | **Close Quarters** | 2.76 GiB | XP2_Factory Scrapmetal, XP2_Office Operation 925, XP2_Palace Donya Fortress, XP2_Skybar Ziba Tower |
| `Update\Xpack3` | `xpack3`, `RequireVersion 5` | **Armored Kill** | 3.63 GiB | XP3_Alborz Alborz Mountains, XP3_Desert Bandar Desert, XP3_Shield Armored Shield, XP3_Valley Death Valley |
| `Update\Xpack4` | `xpack4`, `RequireVersion 6` | **Aftermath** | 3.81 GiB | XP4_FD Markaz Monolith, XP4_Parl Azadi Palace, XP4_Quake Epicenter, XP4_Rubble Talah Market |
| `Update\Xpack5` | `xpack5`, `RequireVersion 7` | **Endgame** | 4.11 GiB | XP5_001 Operation Riverside, XP5_002 Nebandan Flats, XP5_003 Kiasar Railroad, XP5_004 Sabalan Pipeline |

Total ≈ 34.0 GiB of game data. Xpack data is non-CAS (`XpNChunks.sb` + per-level `.sb/.toc` inside each Xpack) [V
listing]. Level names: `C:\Users\olku\VU-Docs\content\hosting\maps.md:8-40` [V].

Implications [I]: the user has everything a Premium owner has; the engine must still **work with any subset** (a
base-only owner has no `Update\XpackN`), listing only levels whose packages are present and whose `RequireVersion` is
met by the patch's `AuthoritativeVersion`. The data is frozen (last DLC 2013), so the digest-keyed cache will rarely
rebuild. A multiplayer-only build never mounts `SpChunks`-only content or SP/COOP levels except what MP bundles share
(`SpChunks.sb` is mounted for MP levels too, `data.md:19`).

---

## 5. Scope for "MP only, with bots, BF3 weapons and vehicles"

Effort is in focused developer-weeks (w) for the user plus agents, order of magnitude only [I]. Dependencies refer to
the handoff milestones M0–M7.

### 5.1 MVP — first playable BF3 match (target: one level loop end to end)

| feature | BF3 data that drives it | effort | depends on |
|---|---|---|---|
| Loaders in C++ (containers, EBX, MeshSet, DxTexture, Havok, terrain, XAS) | handoff 01/02 | 4–6 w | — (M0) |
| Static world for Metro, Bazaar, Caspian (look, terrain, scatter) | handoff 03 | 6–8 w | M0 (M1–M2) |
| Collision + soldier movement: stand/crouch/prone, sprint, jump, fall damage, swim stop | `CharacterPhysicsData`, `CharacterPoseData`, `CharacterSprintData` | 3–4 w | M1 |
| First-person arms + weapon, third-person soldiers (skinned BF3 meshes) **with the chosen animation route** | `SoldierEntityData`, skinned MeshSets, Ant/AssetBank | 4–12 w (the big unknown) | M0; Ant spike first |
| Infantry gunplay: fire modes/RoF, magazines/reload, ballistics, damage + falloff, hit zones, recoil/dispersion/sway, ADS zoom | `SoldierWeaponData`, `FiringFunctionData`, `BulletEntityData`, `WeaponSwayData`, `RecoilData`, `ZoomLevelData` | 4–6 w | movement |
| Health, regen, death, respawn, kill feed, scoreboard, round end | `SoldierEntityData` health fields, mode data | 1–2 w | gunplay |
| Modes **TDM** and **Conquest Small**: tickets, flags (polygon + capture timing), HQs, combat area | handoff 04 §1–2 | 2–3 w | world |
| Kits: 4 classes × 1 primary + pistol + knife + grenade, all unlocked; kit select screen | `VeniceSoldierCustomizationAsset`, `CustomizeSoldierData` slots (§1.4) | 1–2 w | gunplay |
| Netcode: dedicated (headless) + listen server, 30 Hz fixed tick on its own thread, snapshots + deltas, local soldier prediction, interpolation, server-side rewind for hits, ≤ 24 slots | own; Godot `ENetConnection` | 5–7 w | movement, gunplay |
| Bots: navmesh bake per agent class, objective choice, cover-less fight, aim model with reaction/spread | handoff 04 §9 | 3–5 w | world, gunplay |
| Two land vehicles (one wheeled, one tracked): seats, driver + gunner, damage, enter/exit, respawn | `VehicleEntityData`, spawners | 4–6 w | collision, netcode |
| Minimal HUD (crosshair, ammo, health, flags, static minimap), audio for shots/impacts/footsteps | UI/minimap assets, `MaterialRelationSoundData` | 2–3 w | — |

MVP total ≈ 40–65 w. The **animation route** (row 4) is the swing factor and must be decided by a 1–2 w spike before
gunplay work starts (handoff §8 says the same).

### 5.2 v1 — "BF3 multiplayer" as players remember it

| group | contents | effort |
|---|---|---|
| Classes and gadgets | Assault medkit, defibrillator/revive, M320/M26; Engineer repair tool, RPG/SMAW, Javelin, mines, EOD bot; Support ammo box, C4, claymore, mortar, bipod; Recon MAV, T-UGS, radio beacon, SOFLAM; man-down/revive flow | 6–10 w |
| Infantry systems | spotting (3D markers + minimap), **suppression**, squads (4) + squad spawn, attachments/sights/camo selection (all unlocked), death pickup | 4–6 w |
| All weapons | ~80 base weapons and gadgets data-driven from EBX (§1.4 counts) | 2–4 w once the system is data-driven |
| Vehicles | MBT, IFV, AA, jeeps/buggies/quads/dirt bikes, mobile artillery, **attack/scout/transport helicopters, jets, boats (RHIB, AAV)**, stationary AA/TOW; vehicle weapons, lock-ons, flares/ECM/IR smoke, disabled state, repair, ejection, parachute | 14–24 w |
| Destruction | slots/states/parts, wiring, collapse, debris, effects (handoff 04 §5–6) | 6–10 w (M5) |
| Modes | Conquest Large (64), Conquest Assault, Rush, Squad Rush, SQDM, TDM CQ, Domination, Tank Superiority, Air Superiority, CTF, Gun Master, Scavenger (16 ids, §1.2) | 4–6 w |
| Scale | 64 slots with bot fill to N, interest management, bandwidth/CPU budgets proven with 63 bots on Caspian CQL | 4–6 w |
| Bots in vehicles | land driving on vehicle navmeshes, gunners, PID flight for helicopters/jets (fun-bots proves it suffices) | 4–6 w |
| Audio/FX | sound patches for weapons/vehicles, footsteps by material, effects as GPUParticles | 6–8 w |
| Server ops | own server list (optional), map/mode rotation, admin commands, config | 2–3 w |

v1 total ≈ 50–85 w on top of the MVP.

### 5.3 Later / optional

Kill cam and spectator, demo recording (snapshots replay), commo rose and radio VO, VOIP, controller support, Linux
dedicated server (needs the BF3 data copied by the operator), overlay level edits (MapEditor-style JSON over BF3
levels), local stats, accessibility. Campaign/co-op: out of scope by goal.

### 5.4 Skip (never build)

Battlelog and its web launcher; Origin/EA app/EA account login or ownership calls; any Blaze/EA backend emulation;
rank/unlock progression, assignments, dog tags, service stars, stats and leaderboards (everything unlocked, as VU
does); Premium entitlement logic (the files present are the entitlement); PunkBuster and any kernel anti-cheat (server
authority only); EA telemetry; BF3's Gameface/Scaleform menus (own UI in Godot); SP/COOP levels.

### 5.5 Hard risks

| risk | why hard | mitigation | decide by |
|---|---|---|---|
| **Ant animation decode** (soldier and weapon animations in AssetBanks, DCT-compressed curves) | never decoded by IW4L; only Rime's unlicensed reader exists (handoff §1, 02 §10) | spike: run Rime on one soldier + one weapon AssetBank; if it decodes, write our own decoder from the format (or ask OrfeasZ for a licence); else IK + procedural + a small self-authored animation set | before MVP gunplay (week 1–2 of the soldier work) |
| **64-player netcode with vehicles, destruction and bots on one server** | per-tick CPU (bots' perception, physics, 64 soldiers + ~30 vehicles), bandwidth for vehicles (IW4L: ~180 B + 24 B/occupant per changed vehicle row), destruction state rows | own snapshot/delta system, priority + interest management, server tick off the render thread, BF3's 30 Hz, bot perception budgets, load test with 63 bots early | first netcode milestone |
| **Vehicle feel** | EBX gives masses, gears, wheels, rotor/wing params; the solver is not in the data (IW4L hand-wrote handling) | per-class models on Jolt (raycast wheels, track model, helicopter, jet aero), side-by-side tuning against the user's own BF3/VU footage (Art 5(3)) | v1 vehicles |
| **Legal** | trademark letter or an IP claim | §3.8; no branding, no assets, no EA code, no services emulation | before any public repo |
| **Engine churn** | Godot 4.8 dev→RC→stable; godot-cpp v10 | `custom_api_file` from the exact build; CI that loads the extension in the pinned editor binary | each Godot bump |
| **Scope** | 29 levels × 16 modes, ~80 infantry items, ~79 controllables (§1.4) | everything data-driven from EBX; no per-item hand tuning; MVP on 3 levels | continuous |
| **Server operators need the data too** | a dedicated server needs collision/gameplay from BF3 files | document: the operator must own BF3; server reads its own install; never ship extracted collision | v1 server ops |

---

## 6. What I could not determine

* Whether EA has ever contacted VU or any BF3 data-reading project (no public record either way); whether EA plans to
  retire BF3 PC online services (EA's service-update page did not render a list to the fetcher).
* The real BF3 PC population: Battlelog lists "1337 servers", but the first entries were empty and the count may be
  cached or a placeholder.
* Whether a typical non-Premium Origin/EA app install downloads `Update\XpackN` folders without the entitlement (I
  assumed "only when owned"); the Back to Karkand title string (Xpack1 has no `installerdata.xml`).
* That build 1147186 is BF3's final PC build (plausible from `AuthoritativeVersion 7` = Xpack5's `RequireVersion 7`,
  not confirmed by a primary source).
* Whether PR:BF2's 2015 standalone had EA's permission (claimed in community lore, not documented).
* EUR-Lex refused automated access; the Directive text was read from legislation.gov.uk's retained-EU copy (same
  articles). *Sega v. Accolade* / *Sony v. Connectix* and national private-copy rules were not re-read.
* fun-bots authors' willingness to license code or maps; Rime author's willingness to license the AssetBank/DCT code.
* Any shipped Godot title with 64-player authoritative FPS servers (none found; absence is not proof).
* Licence of `GodotSteam` (API reports none; its repo may carry a custom licence file) — irrelevant unless Steam
  transport is wanted.
* Whether OpenBattlefield2 or openbfme-godot have received publisher contact (both are weeks to months old).

## 7. Searches and fetches behind this report (2026-10-10)

GitHub API/`gh api`: Joe91/fun-bots, ModdersLink/Rime, VeniceUnleashed/VU-Docs, Venice-EBX, BF3RM org (all repos),
CadeEvs/FrostyToolsuite tree 1.0.6.3, NicknineTheEagle/Frostbite-Scripts, mitsuhiko/frostbite2-stuff,
Moxnatiy/OpenBattlefield2, Open-BFME/openbfme-godot (README, NOTICE), foxssake/netfox, vantixorg/vantix, GNS, yojimbo,
netcode, ENet, Jolt, Recast, Godot, godot-cpp (`git ls-remote` tags); repository searches "venice unleashed",
"veniceext", "godot multiplayer fps", "godot fps gdextension", "godot netcode rollback", "godot battlefield",
"battlefield remake", "battlefield 1942", "battlefield 2 engine", "refractor engine", "battlefield reimplementation",
"battlefield 3 unreal", "battlefield unity remake", "frostbite reverse engineering", "frostbite ebx".
Web: veniceunleashed.net (+ /about), EA User Agreement, legislation.gov.uk 2009/24 Arts 5/6/8, Cornell 17 USC 1201,
Wikipedia (Battlefield 3, SAS v WPL, OpenRCT2, Red Alert, BattleBit, Squad, Project Reality), Steam store 1238820,
Battlelog server list, openmw.org/faq, openra.net/about, TorrentFreak (re3), PCGamesN (Revive Network), plus the
search summaries cited inline as [R].

---

## Independent verification (2026-10-10)

Independent skeptical pass, same day. Method: `gh api` (repo metadata, READMEs, LICENSE/NOTICE files, trees),
`git ls-remote`, read-only `git log --all` over the local clones, a read-only listing of the install (`cat`, `ls`,
`du -sb`), `extension_api.json` queried with `python -I`, Godot `master` source via `gh api`, and primary pages via
WebFetch until the web quota ran out (veniceunleashed.net/about and Wikipedia could not be re-fetched in the first
pass; a resumed pass the same day fetched both, plus NoFaTe's blog and the dev7 Godot source at `c971f93e7`). Tags as in the
body: [V] checked by the verifier, [R] report claim, [I] inferred. Verdicts: confirmed / corrected / refuted /
unverifiable.

| claim id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| K1 | fun-bots has no licence, none in its history; no copying/porting without permission | confirmed | `gh api repos/Joe91/fun-bots`: license null (created 2020-12-26, pushed 2026-10-08) [V]; upstream root has no LICENSE/COPYING [V]; `git log --all --name-only` in `C:\Users\olku\unfun-bots` (origin + 15 `upstream/*` Joe91 branches + tags V0.1…V1.6.1.2, 3,713 commits) finds only `fun-bots-helper/dist/tk/license.terms`, the bundled Tk licence [V]; README.md and mod.json have no licence text [V]. Legal consequence = default copyright [I] | — |
| K2 | Rime (by OrfeasZ/NoFaTe) unlicensed: oracle + format knowledge only; ask OrfeasZ for DCT/AssetBank code | corrected | license null via API; no LICENSE/COPYING in `git ls-tree` or history of `C:\Users\olku\Rime` (only Frostbite type classes named `License*`) [V]. `git log --all` authors: kiwidoggie 188 (+18 as kiwipuppeh), Orfeas Zafeiris/OrfeasZ 196, FlashHit 110, txt231 58, Mats Bakken (Powback) 58, keku645 44 [V]. `RimeLib.Animation.Frostbite2_0/EA/Compression/DCT/Decompressor.cs` history: kiwidoggie 2024-08-22, Mats Bakken 2026-09-06; `EA/GeneralDataReader.cs`: kiwidoggie, txt231, FlashHit [V] | Rime is a multi-author ModdersLink project, not OrfeasZ's alone; a licence for the AssetBank/DCT code would need kiwidoggie, Powback, txt231, FlashHit and OrfeasZ (or the org). Moot: `research\probes\ant-animation-probe.md` §4, §9 re-derived a bit-exact DCT decoder from a written spec (3,778/3,778 clips), so no licence request is needed |
| K3 | Frosty is CC BY-NC-ND 4.0 and has no BF3 profile | confirmed | CadeEvs README §License: "Attribution-NonCommercial-NoDerivatives 4.0" (API license field null) [V]; `FrostySdk` profile DLLs in branches 1.0.6.3, 1.0.6.4, 1.0.7: Anthem, BF1, BF4, BFH, BFV, BF2042 (1.0.7), FIFA, Madden, ME, NFS, PvZ, SWS, StarWars, StarWarsII — no BF3/Venice/FB2 [V `gh api …/git/trees/<branch>?recursive=1`] | Add: successor repo FrostyToolsuite/FrostyToolsuite (created 2023-11-05) is **GPL-3.0**; its `FrostyCli/Profiles/*.json` also has no BF3 [V] |
| K4 | Frostbite-Scripts (MIT) is the only permissive FB2 parser; frostbite2-stuff (BSD-style) dormant | confirmed | LICENSE: "MIT License / Copyright (c) 2019 NicknineTheEagle / Portions … 2012-2017 Frankelstner / 2016 NoFaTe"; `frostbite2/` = dbo, dds, dumper, ebx, ebxtoasset, ebxtotext, noncas, res .py [V gh api]; frostbite2-stuff LICENSE BSD-style "Copyright (c) 2011 by the AUTHORS", last push 2011-11-01 [V]; `gh search repos` (frostbite, frostbite ebx, bf3 superbundle, venice ebx, bf3 tools …) found no other permissive FB2 parser [V negative, bounded]; resumed pass: GitLab topic "frostbite" lists only FrostyCON (Rust RCON library), and a web search for BF3 superbundle/EBX parsers surfaced only IceBloc [V negative, bounded] | "Only" is a bounded negative. The report omits **marv7000/IceBloc** (GPL-2.0, "Asset extractor for Frostbite 2 games", BF3: EBX dump, MeshSet, DxTexture, AssetBank Frame/Raw/Dct) [V gh api README]; copyleft, so the permissive statement stands |
| K5 | VU runs bf3.exe, emulates Blaze, links Origin; 30 Hz regular; 60/120 Hz timing bugs | corrected | Origin link for ownership: `VU-Docs\content\general\playing.md:11-15` [V]; `vu.FrequencyMode` regular = 30 Hz, high60, high120: `content\hosting\commands.md:73-75` [V]; changeset 20056 (25/09/2023) "quicker acceleration on high frequency", "faster damage expiration on high frequency": `content\general\changelog.md:134-135` [V]. "Blaze" occurs in VU-Docs only as Frostbite type pages (`BlazePresenceBackendData`, `BackendType`), not as a VU feature. Resumed pass: VU-Docs names VU's own backend **Zeus** (`content\general\args.md:30` "`-env [prod\|dev]` Specifies the Zeus environment"; `changelog.md:395, 416` drops/errors "when connection to Zeus was lost") [V]; NoFaTe's blog post "Venice Unleashed, The Delay, and Updates" (6 Sep 2013) says they "will no longer be emulating Blaze" and are stripping it from the client for their own backend services [V http://blog.nofate.me/?cat=7&paged=2]; veniceunleashed.net/about (re-fetched): VU "uses its own servers, and does not modify any game files", Origin link "so we can verify your ownership", "fully legal under EU interoperability laws", 60/120 Hz tickrate; no mention of Blaze [V]. Running the original executables: needs the BF3 install (`-gamepath`, `args.md:18`) and an EA-activated BF3 for the server (`hosting\setup-win.md:46`) [V]; that it executes bf3's own code is [I] (strong) | VU does **not** emulate Blaze today: Blaze emulation was the 2013 plan, dropped in Sept 2013 for VU's own backend (Zeus). Fix §0 item (line 31), §1.1 row 2 (line 64) and §2.4 (line 239). The "skip Blaze emulation" rules (§3.5 table, §3.8 item 5, §5.4) are unaffected |
| K6 | Kit = VeniceSoldierCustomizationAsset at Gameplay/Kits/…; slots 0/1/2-4-5/6/7; Conquest spawn = Spawn event on a flag's ServerCharacterSpawnEntity | confirmed | `unfun-bots\ext\Server\BotSpawner.lua:1049-1157` (`ServerPlayerEvent("Spawn", …)` fired on a `ServerCharacterSpawnEntity` from an owned `ServerCapturePointEntity`'s `bus.entities` whose `CharacterSpawnReferenceObjectData.team` is the team or 0; prefers `flagLocation < 100` and `isControlled`, else closest to the enemy target) [V]; slots `:2019` (0), `:2036/2038` (4 for Assault/Support, else 2), `:2055` (5), `:2067` (6), `:2081` (1), `:2095` (7) [V]; kit names `:2186-2208` [V]; `research\probes\ebx-data-probe.md:34` = 40 `gameplay/kits/*` assets (2 teams x 4 kits x 5 variants) [V]; yaml fields and `WeaponSlot` enum 0-9 match [V `types\fb\*.yaml`] | Nuance: this is fun-bots' bot spawn policy over BF3 entities, not BF3's own spawn-screen logic [I] |
| K7 | BYOD projects survive without assets/original code/branding; re3 decompiled and sued; EA stopped Revive 2017 | corrected | OpenMW FAQ: Bethesda "has given us approval to continue working on it" with conditions incl. not promoting Morrowind on Android/mobile [V openmw.org/faq]; CorsixTH LICENSE.txt "Copyright (C) 2009-2016 Peter "Corsix" Cawley", README points to GOG and EA's store [V]; OpenRA, OpenRCT2, OpenXcom, julius, openmw pushed 2026-09/10 [V API]; TorrentFreak: DMCA 2021, suit Sept 2021, settlement in principle 6 Feb 2023, named defendants dismissed **with prejudice** 3 Apr 2023, Does without prejudice; it says "reverse-engineered", not "decompiled" [V]; PCGamesN (updated 15 Nov 2017): EA asked Revive "to stop distributing and using their intellectual property", downloads disabled [V] | Facts hold; "decompiled" is the community/project description [R], and the survival rule (no assets, no original code, no branding) is an inference [I], not a verified fact |
| K8 | Directive 2009/24 Arts 1(2), 5(3), 6, 8; SAS v WPL | confirmed | legislation.gov.uk/eudr/2009/24 Art 1(2), 5(3), 6(1)-(3) incl. "substantially similar in its expression" (6(2)(c)), Art 8 second para "null and void" [V]; C-406/10 (2 May 2012): functionality, programming language and data-file format not protected [V en.wikipedia.org/wiki/SAS_Institute_Inc_v_World_Programming_Ltd] | Caveat [I]: the Directive covers programs; BF3's meshes, textures, animations and audio are other works (InfoSoc 2001/29), so Arts 5(3)/6/8 do not themselves cover the `user://` cache (the §3.5 table already treats it as private copy) |
| K9 | EA User Agreement (14 May 2026) §2 no RE/extraction unless authorised or permitted by law; §7(C) mods | confirmed | ea.com/legal/user-agreement: "Last Updated May 14, 2026"; §2 no reverse engineering or extracting source code or other data "unless expressly authorized by EA or permitted by law"; §7.C Unauthorized Third-Party Program incl. "add-on", "mod", "hack", "trainer", "cheat" [V] | — |
| K10 | 17 U.S.C. §1201(f) interoperability exemption | confirmed | law.cornell.edu/uscode/text/17/1201 (f)(1)-(4) [V] | — |
| K11 | Origin build 1147186, Patch AuthoritativeVersion 7, Xpack1-5, 29 MP levels, en only, 34.0 GiB; Patch updates XP1-4, not XP5 | corrected | `version.json` = `{"sku":"origin","build":"1147186","region":"WW"}` [V]; `Update\Patch\package.mft` = `Name patch` / `Authoritative` / `Version 7` [V]; Xpack1-5 `RequireVersion` 2/4/5/6/7 [V]; Xpack2-5 installerdata titles Close Quarters / Armored Kill / Aftermath / Endgame [V]; Patch holds `Xp1-Xp4Chunks` and XP1_*-XP4_* levels, nothing XP5 [V]; `Loc` = en only (base and Patch) [V]; 9 base MP + 4x5 XP = 29 [V]; `du -sb`: Data 11,663,884,035 B, Patch 6,332,304,778, Xpack1 3,163,743,568, Xpack2 2,962,781,084, Xpack3 3,902,848,296, Xpack4 4,089,045,716, Xpack5 4,409,578,624 → 34.02 GiB (whole folder 34.22 GiB) [V] | The manifest has two keys, `Authoritative` and `Version 7`, not one `AuthoritativeVersion 7`; the Xpack1 title (Back to Karkand) is inferred from its level ids, since it has no installerdata [I] |
| K12 | BF3 still sold (Steam Premium 39,99 €, EA link); Battlelog answers; console off 2024-11-07 | confirmed | Steam 1238820: Premium Edition 39,99€, "The game and all 5 expansion packs", must link EA and Steam accounts, online features retirable after 30 days' notice [V]; Battlelog "Found 1337 servers" [V]; console shutdown 2024-11-07, delisting 2024-07-31, PC unaffected [R mp1st.com/news/battlefield-3-bf4-hardline-on-ps3-and-xbox-360-servers-to-shut-down-on-nov-7-2024-games-to-be-delisted-on-july-31]; resumed pass: Wikipedia states PS3/X360 servers offline as of 7 Nov 2024 and delisting from those storefronts on 31 Jul 2024, and says nothing about a PC shutdown [V https://en.wikipedia.org/wiki/Battlefield_3, first 100k chars] | On re-fetch the first entries were populated (6/32 TDM, 1/48, 1/64); §4.1's "first five empty" was one snapshot |
| K13 | ENet defaults 32 peers (max 0xFFF); no 64-player Godot FPS; own C++ snapshot replication | confirmed | `extension_api.json`: `ENetMultiplayerPeer.create_server` max_clients "32", `ENetConnection.create_host_bound`/`create_host` max_peers "32" [V]; ENet `ENET_PROTOCOL_MAXIMUM_PEER_ID = 0xFFF` (upstream protocol.h and Godot `thirdparty/enet/enet/protocol.h:19`); Godot clamps 1..4095, `modules/enet/enet_connection.cpp:308` (master) [V]; no shipped 64-player Godot FPS found [V negative] | Add: `research\probes\godot-gdextension-probe.md` found Godot's ENet socket ignores RCVBUF (unreliable bursts above ~66 KB lose 39-94 %), so the snapshot sender must pace per tick [V probe]; `enet_multiplayer_peer.cpp:67` passes the channel count as `in_bandwidth` (max_channels 0) to `create_host_bound`, one more reason to drive `ENetConnection` directly. Resumed pass: the file at the dev7 commit `c971f93e7` (2026-09-29) has the same blob SHA as `master` (`baba00d5…`); line 67 there is `create_host_bound(bind_ip, p_port, p_max_clients, 0, p_max_channels > 0 ? p_max_channels + SYSCH_MAX : 0, p_out_bandwidth)`, and `enet_connection.h:112` at dev7 orders the parameters `(bind_address, port, max_peers, max_channels, in_bandwidth, out_bandwidth)`, matching `extension_api.json` [V gh api]. So with `max_channels = N > 0` the server gets max_channels 255 and an incoming-bandwidth limit of N+2 bytes/s, and `in_bandwidth` is ignored; with the default 0 the bug is invisible. No upstream issue/PR found (`gh search issues/prs`, bounded) [V negative]; runtime effect [I] |
| K14 | godot-cpp v10 independent; pin = v10 + custom_api_file of this build | corrected | Tags `10.0.0-rc1`, `10.0.0-rc2`, `10.0.0-stable`; branches up to `4.5` + `master` @ b051e694 [V `git ls-remote`]; README: "Starting with version 10.x, godot-cpp is versioned independently", "can target Godot 4.3 or later", `custom_api_file` [V] | Per `research\probes\godot-gdextension-probe.md:28-29, 282-284`, also pass `GODOTCPP_GDEXTENSION_DIR` holding the exe's `--dump-gdextension-interface-json` output (the custom API option alone keeps godot-cpp's bundled interface JSON). The probe used `master` b051e694 (10.0.0-stable + 8 commits incl. a hot-reload crash fix). The load gate checks only major.minor.patch, so a changed method hash fails at first call: re-dump both JSONs and rebuild on every dev→RC→stable bump |
| K15 | openbfme-godot (Godot 4.7 + godot-cpp 10, GPL-3.0 via Zero Hour); OpenBattlefield2 (MIT, binary addresses, Battlefield name) | confirmed | openbfme-godot README: "Godot 4.7", submodule "godot-cpp 10.0.0-stable", `user://install-paths.cfg`, md5 archive check; NOTICE: GPL v3 because parts are translated from EA's Zero Hour source, BFME behaviour follows the Open-BFME-1 **decompilation**, non-affiliation text [V gh api]; OpenBattlefield2 LICENSE "MIT License / Copyright (c) 2026 OpenBattlefield2 contributors", README "clean-room", ships no content, rules name "an address in the original" [V] | — (GitHub shows NOASSERTION for OB2; the file is MIT text) |
| K16 | Hard risks: Ant decode, 64-player netcode, vehicle feel, IP letters; 1-2 w Ant spike before gunplay | corrected | `research\probes\ant-animation-probe.md` §0, §4.3, §5, §9: spike done. DCT 3,778/3,778 bit-exact in 1.7 s; 5,592/5,592 MP clips mapped; M9 reload and 3P run rendered in Godot dev7; port ~1,000-1,500 C++ lines, 3-5 days. Open: VBR (146 MP clips, mostly pistol/launcher ADS, 2-5 days uncertain), 35 curve clips, rig-logic twist joints, and the Ant graph (5,729 `ActorControllerAsset`, state flows; not examined) [V probe] | Ant **decode** is no longer the top risk. Suggested order [I]: (1) 64-player netcode with vehicles, destruction and bots; (2) animation graph/clip selection and blending + VBR; (3) vehicle feel; (4) trademark/IP. §5.1 row 4 ("4-12 w, big unknown") should shrink accordingly |
| extra-1 | §5.5: "only Rime's unlicensed reader exists" for Ant | refuted | IceBloc (GPL-2.0) has `IceBlocLib.Frostbite2/Animations/DCT/DctDecompressor.cs` with the same basis; its VBR class is empty (`ant-animation-probe.md` §10; IceBloc README via gh api) [V] | Two prior decoders exist: Rime (unlicensed) and IceBloc (GPL-2.0); neither may be copied into an MIT repo; the probe's own spec replaces both |
| extra-2 | §2.1: Rime "commits by Orfeas Zafeiris / OrfeasZ"; "ask OrfeasZ for a licence" | corrected | see K2 [V git log] | Multi-author; the DCT file history starts with kiwidoggie |
| extra-3 | §5.1 gunplay row: data = `WeaponSwayData`, `RecoilData` | corrected | `GunSwayData` inherits `WeaponSwayData` (`VU-Docs\types\fb\GunSwayData.yaml:3`), reached via `WeaponFiringData.weaponSway` (`WeaponFiringData.yaml:42`); `ebx-data-probe.md`: spread/recoil values live in GunSwayData, `FiringFunctionData.WeaponDispersion` and `Recoil` (RecoilData) are all zero; body-part multipliers come from `MaterialGridData` [V] | Name `GunSwayData` (+ `GunSwayModifierData`) and `MaterialGridData` as the drivers; `RecoilData` is unused for infantry guns |
| extra-4 | §2.2 lists only CadeEvs Frosty | corrected | FrostyToolsuite/FrostyToolsuite: GPL-3.0, created 2023-11-05, no BF3 profile [V gh api] | Add it as GPL-3.0 knowledge-only reference (not needed) |
| extra-5 | §1.4: appearance path `…/Visual/MP[_XP4]/<Us/RU>/MP_<US/RU>_<Kit>_Appearance01` | corrected | `BotSpawner.lua:2214-2244`: default XP4 appearance = `…/Visual/MP/<Team>/MP_<TEAM>_<Kit>_Appearance01_XP4`; `MP_XP4/` is used only for colour variants `_Appearance_<colour>` [V] | As stated in the evidence |
| extra-6 | §1.4: "Metro CQL 3,844 rows"; fun-bots "36.8k Lua lines" | corrected | `MP_Subway_ConquestLarge0.map` has 3,844 lines incl. the CSV header = 3,843 waypoints; `git ls-files '*.lua' \| xargs cat \| wc -l` = 37,018 [V] | 3,843 waypoints; ~37.0k Lua lines (method-dependent) |
| extra-7 | Principle check: no recommendation modifies BF3 files | confirmed | Every recommendation reads the install as-is; cache in `user://` keyed by digest (§3.8.2); no repacking, mod bundles or bf3.exe hooking as a route (§1.1, §5.4) [V reading] | Watch items: (a) §5.3 Linux dedicated server "BF3 data copied by the operator" is fine only as a verbatim copy of the operator's own install, read as-is (never extracted/converted data shipped; §5.5 says so); (b) MapEditor-style overlays (§1.3, §5.3) must be data files in `user://` applied in memory — MapEditor's own route (LevelLoaderGen → custom bundles) is excluded; (c) observing BF3/VU is knowledge-only and writes nothing to the install; (d) handoff `HANDOFF.md:52` "If it decodes, port it" (Rime) conflicts with the licence finding; the report rightly says re-derive |

| extra-8 | §0 (line 31), §1.1 (line 64), §2.4 (line 239): "VU emulates EA's Blaze backend" | corrected | NoFaTe blog, 6 Sep 2013: "we will no longer be emulating Blaze", stripping it from the client for own backend services; VU-Docs `args.md:30` names the backend Zeus; /about says VU "uses its own servers" and never mentions Blaze [V, see K5] | VU started (2013) as a Blaze emulator but has used its own backend (Zeus) since late 2013; it still requires Origin-linked ownership. The press headlines cited in §1.1 describe the 2013 plan |

Resumed pass (same day) closed three of the earlier gaps: VU's own statements (veniceunleashed.net/about re-fetched;
Blaze corrected, see K5/extra-8), the Wikipedia console dates (K12, now [V]) and the dev7 ENet source (K13, identical
blob to `master`). Still not verified: any permissive FB2 parser outside GitHub/GitLab search (bounded negative); the
legal weight of the BYOD survival pattern (inference, not legal advice); whether VU's client process is bf3.exe itself
or a loader that maps BF3's code (VU docs only show it needs the install and an EA-activated BF3); the runtime effect of
the ENet `create_server` argument mix-up (not measured).
