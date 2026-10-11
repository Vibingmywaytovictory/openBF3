# 06 — Bots: navigation, behaviour, combat, vehicles and CPU budget for the BF3 reimplementation (Godot 4.8, C++)

Written 2026-10-10 for the Godot reimplementation that reads the retail BF3 install as-is. Scope: multiplayer bots (offline
and as server fill) for infantry, land vehicles, helicopters, jets and boats on the real BF3 levels. Tags: **[V]** verified
here (read in code/data/API dump, or measured by me), **[RC]** report claim (a source I did not re-check), **[I]** my
inference. Paths are absolute; `FB` = `C:\Users\olku\unfun-bots` (the user's fork of fun-bots), `RN` =
`C:\Users\olku\recastnavigation`, `API` = `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json`,
`G:` = Godot source at the local build's commit `c971f93e7` (https://github.com/godotengine/godot/blob/c971f93e7/…;
copies in `SCR\godot-src`), `SCR` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\bots`.
Measurements were taken on this machine (16 logical CPUs) with throwaway harnesses in `SCR` (C++ `navbench`, and two
GDScript files run headless in a scratch project purely as measurement probes; nothing of it ships).

**User principle honoured throughout:** our own code reads the original BF3 files in place; nothing is written to the
install; anything derived (navmeshes, road graphs, caches) lives under `user://` keyed by the content digest of what it was
built from. fun-bots and Venice Unleashed (VU) are studied as references only; neither is run, linked or shipped.

---

## 0. Answers in one page

1. **fun-bots** (Lua on VU, 36.8k lines) drives bots as real BF3 players by writing the same input levels a human produces
   (`EntryInput:SetLevel(EntryInputActionEnum.*)` + authoritative view angles) [V §1.3]. Its navigation is **185 hand-
   recorded trace files** (1,159,335 nodes, 9,857 paths; human players walked/drove them in-game) with path switching at
   link nodes; there is no navmesh, no dynamic replanning, no destruction awareness; air vehicles fly "pathless" to a
   point 110 m (helicopters) / 350 m (jets) above the current objective with PID controllers and no terrain avoidance [V].
   Line-of-sight checks for bot-vs-bot fights are **delegated to connected human clients**, so a server with no humans
   has no bot-vs-bot combat [V §1.4]. **fun-bots has no licence** (GitHub API `license: null`, no LICENSE in 3,672 commits)
   → port ideas, not code; never ship its map files; reading a user's own copy at runtime is an optional, user-provided
   input at most [V/I §1.13].
2. **Navigation:** Godot 4.8's NavigationServer3D works but does not fit 64 bots on 2 km maps: on Metro's real collision
   (744k triangles) its path queries cost **5–9 ms mean** (max 24 ms) against **0.06–0.11 ms** for Detour on the same
   geometry, it stores one polygon per detail triangle (55k vs 15k), caps searches at 4,096 polygons by default, has no
   area types, and with the default edge connections every region change rebuilds the map in **~6 s** (O(F²) over
   37,505 free edges; 48 ms with edge connections off) [V §2]. **Recommendation: link Recast/Detour (zlib) into the
   Godot-independent `bf3` library**: tiled soldier navmesh (64 m tiles) + one navmesh per vehicle hull class with BF3's
   own `RoadData` ribbons stamped as a cheap "road" area, off-mesh links for jumps/drops/ladders, per-tile async rebake on
   destruction (Metro: ≈ 30 ms per tile on average, 110–163 ms for the densest; 0.3 ms swap), cached in
   `user://nav/<digest>`; aircraft use a 2.5-D clearance grid,
   not a mesh [§2.9].
3. **Behaviour:** a Godot-independent `bf3_bots` C++ library: team **Director** (objective assignment per mode, fun-bots'
   4:1 attack:defend weighting as a start), **squads**, per-bot **utility/state brain** at 5–10 Hz, **perception** batched
   on the server job pool (rays against the server's own Jolt world), **aim controller** with reaction time, angular error
   that converges over time, recoil compensation read from the shared weapon state and lead/drop from EBX
   (`ShotConfigData.initialSpeed`, `BulletEntityData.gravity`), vehicle drivers (pure pursuit on the vehicle navmesh
   corridor), helicopter and jet pilots (PID like fun-bots plus terrain clearance) [§3]. Budget for 64 bots at the 60 Hz
   sim: ≈ 1–2 ms per tick single-threaded, < 1 ms wall with the job pool [I from measurements §3.9].
4. **Bots are server-side fake clients that emit the same `Command` as humans** (05-networking's "BotCommand = same
   Command type"): one movement/weapon/vehicle code path, spread/recoil/suppression/stamina apply to bots for free, bots
   replicate like any remote player; costs are closed-loop controllers and artificial reaction/turn limits [§4].
5. **Plan:** B0 offline nav bake + validation against fun-bots traces → B1 Metro TDM walkers → B2 Metro Conquest flag
   capture → B3 combat → B4 Rush/squads/classes → B5 Caspian infantry → B6 land vehicles → B7 helicopters → B8 jets and
   boats, each with an in-game pass criterion [§5].

---

## 1. fun-bots in depth (`FB`, upstream https://github.com/Joe91/fun-bots)

### 1.1 Provenance [V]

* `FB` is the user's fork (`origin` = `Vibingmywaytovictory/unfun-bots`, `upstream` = `Joe91/fun-bots`); 3,672 commits;
  upstream head in the clone `4ed81e38` (2026-02-01, release `V3.0.0-Release`); GitHub reports upstream `pushed_at`
  2026-10-08 but the newest commit is still `4ed81e38` (`gh api repos/Joe91/fun-bots/commits`).
* The fork adds 28 commits (`git diff --stat upstream/master master`): an in-game raycast navmesh baker
  (`FB\ext\Client\ClientNavmeshBaker.lua`, 1,458 lines; `FB\ext\Server\Navmesh.lua`, 430), a design note
  (`FB\docs\navmesh-design.md`), `FB\astar-lib\a-star.lua` (MIT, © 2012 RapidFire Studio, lines 1–23) and helper changes.
  The map files are unchanged from upstream. The design note states "The server cannot raycast"
  (`navmesh-design.md:62`) — outdated: VU 1.3.1 (2021-12-10) introduced a server `RaycastManager`
  (`C:\Users\olku\VU-Docs\content\vext\changelog.md:154-155`) and fun-bots itself casts on the server for revives
  (`FB\ext\Server\BotManager.lua:1572`).

### 1.2 Architecture [V unless tagged]

| file (`FB\ext\…`) | role |
|---|---|
| `Server\BotManager.lua` (1,726) | owns bots; creates the bot `Player` + `EntryInput` (`:868-879`); per-tick update tiers (`:55-62, 113-195`); bot↔bot LOS distribution to clients (`:1297-1478`); bot↔bot revive (`:1480-1592`); damage hook (`:308-350`) |
| `Server\Bot\Bot.lua` + `BotMovement`, `BotAiming`, `BotAttacking`, `BotActions`, `BotWeaponHandling`, `Vehicle*` | one bot: timers, PID controllers (`Bot.lua:187-199`), input write-out (`:519-544`), seat logic (`:301-384`) |
| `Server\BotStates\*.lua` (14) | state pattern: `Idle, Moving, Attacking, InVehicleMoving, InVehicleAttacking, InVehicleJetControl, InVehicleChopperControl, InVehicleStationaryAaControl, OnVehicleIdle, OnVehicleAttacking, StaticMovement, StaticAttacking` (`BotStates.lua`); each state has `UpdateVeryFast` (every tick), `UpdateFast`, `Update`, `UpdateSlow` |
| `Server\GameDirector.lua` (2,213) | team-level objective assignment every 1.5 s (`:157-468`), MCOM/flag events, air target point (`:1707-1749`), trace-objective ↔ capture-point name translation by proximity (`:2094-2145`) |
| `Server\PathSwitcher.lua` | at link nodes, choose a connected path by objective priority (`:20-47, 58-297`) |
| `Server\NodeCollection.lua` (1,872) | the waypoint store: load from SQLite (`:1256-1377`), save (`:1379-1631`), node decode (`:71-91`), link resolution (`:434-487`) |
| `Server\NodeEditor.lua` + `Client\ClientNodeEditor.lua` | in-game trace recorder/editor (§1.12) |
| `Server\BotSpawner.lua` (2,348) | team balance, kits/loadouts, spawn on squad mates/vehicles/beacons with probabilities (`Registry.lua:276-295`) |
| `Server\BotCreator.lua` | per-name persona: skill, reaction, accuracy, behaviour (`:55-106`) |
| `Server\Vehicles.lua`, `Shared\Constants\VehicleData.lua` | vehicle classification and **hand-tabulated** turret part ids, bullet speeds and drops per seat (`VehicleData.lua:1-15` explains they were copied from text EBX dumps) |
| `Shared\WeaponList.lua`, `WeaponClass.lua` | infantry weapons; bullet speed/drop read at runtime from EBX: `shot.initialSpeed.z` and `BulletEntityData.gravity` (`WeaponClass.lua:198-199`) |
| `Shared\Registry\Registry.lua`, `Shared\Config.lua` | constants (cited inline below) |
| `Client\ClientBotManager.lua` | client-side raycasts on behalf of the server (§1.4) |

**Update tiers** (time slicing, `BotManager.lua:55-62, 142-156`; `Registry.lua:210-214`): every server tick
`SingleStepEntry` + `UpdateVeryFast` (yaw slew); a batch of bots per tick so that each bot gets `UpdateFast` every 0.03 s
(aiming), `Update` every 0.13 s (movement, attack logic, input write), `UpdateSlow` every 0.66 s. Team director every
1.5 s (`Registry.lua:146`).

### 1.3 How it drives bots through VU [V]

* A bot is a real `Player` from `PlayerManager:CreatePlayer(name, team, squad)` with an `EntryInput` whose `flags =
  EntryInputFlags.AuthoritativeAiming` (`BotManager.lua:868-879`). Each tick `soldier:SingleStepEntry(entryId)` is called
  ("engine-requirement", `:153`) and the bot writes levels for input actions 0–36 with `input:SetLevel(i, v)`, resetting
  each one-shot input the next tick (`Bot.lua:519-544`). View direction is written to `input.authoritativeAimingYaw/Pitch`
  with a yaw slew limit of `MaximunYawPerSec = 550°/s` per tick (`Config.lua:171`; `BotMovement.lua:1117-1150`;
  `BotManager.lua:660-663`).
* Inputs used: `EIAThrottle, EIAStrafe, EIASprint, EIAJump, EIAQuicktimeJumpClimb, EIAFire, EIAZoom, EIAReload,
  EIASelectWeapon1..7, EIAMeleeAttack, EIAQuicktimeFastMelee, EIAInteract, EIAToggleParachute, EIAYaw, EIAPitch, EIARoll,
  EIABrake, EIAFireCountermeasure` (grep of `FB\ext\Server`). BF3's enum has 70 values (`VU-Docs\types\fb\
  EntryInputActionEnum.yaml`) — a ready-made list for our `Command` button set.
* **Not input-based (API shortcuts):** spawning (`Player:CreateSoldier` ≈ 3 ms, `SpawnSoldierAt` ≈ 17 ms per the
  comments, `BotManager.lua:901-914`); entering a seat with `Player:EnterVehicle(entity, seat)` once within 10 m, no
  interact animation (`BotActions.lua:322-363`; `Registry.lua` `MIN_DISTANCE_VEHICLE_ENTER = 10`); `soldier:SetPose`;
  **teleport when stuck** (`soldier:SetTransform`, probability 80 %, `BotMovement.lua:368-373`; `Registry.lua:218`);
  **suicide** when stuck > 15 s (`BotMovement.lua:388-390, 648-651`) or after 50 s on an inactive path
  (`GameDirector.lua:208-221`); bot damage multiplied by `DamageFactor* = 0.6` and self-kills cancelled in the damage hook
  (`BotManager.lua:340-344, 329-332`; `Config.lua:20-27`).

### 1.4 Perception and spotting [V]

* **No vision model on the server.** Bot→player detection is computed on **each human client** for its own player
  against nearby bots (`ClientBotManager.lua:201-423`, `DoRaycast :123-190`, `CollisionRaycast` up to 5 hits or async
  `Raycast`), and sent back as `ShootAtPlayer` results. **Bot→bot** checks are generated on the server (all enemy pairs in
  range, ≤ 30 checks per 0.05 s, `BotManager.lua:1361-1478`; `Registry.lua:79-83`) and **distributed to connected
  clients** (each client casts ≤ 3 rays per 0.03 s cycle, ≤ 2 of them for bot-bot pairs; `Registry.lua:71-77`;
  `BotManager.lua:652-654, 1297-1323`; `ClientBotManager.lua:213-222`). `if #self._ActivePlayers == 0 then
  return end` (`BotManager.lua:1363`): **with no human connected, bots never acquire each other.**
* Acceptance once LOS is reported (`Bot:ShootAt`, `BotActions.lua:65-266`): same-team and stationary-AA filter; range by
  weapon class (`MaxShootDistance 70 m`, sniper 150 m, missile-air 350 m, vehicles 375 m, gunship 1,000 m; `Config.lua:70-
  102`); FOV 160° × 90° on foot, 180° × 60° in vehicles, 360° × 160° in AA (`Config.lua:68-99`); an "edge" miss chance
  `p = 50 % × max(0.9·|yaw|/halfH, 0.33·|pitch|/halfV, 0.5·d/range)` (`BotGetters.lua:146-161`; `Registry.lua:208`);
  FOV ignored when the bot was hit ("shoot back", `BotManager.lua:308-327`) or a bot enemy made noise within 50/100 m
  (`BotActions.lua:152-164`).
* **No spotting** (no `EIAGiveOrder`/spot input, no team knowledge), **no suppression reaction, no cover use** (grep of
  `FB\ext`; the only "suppression" strings are unlock names, `BotSpawner.lua:1803-1807`).

### 1.5 Aiming [V `BotAiming.lua`, `BotGetters.lua`, `BotCreator.lua`]

* **Persona** per bot name: kits rotate by index; `skill = indexInKit / botsPerKit ∈ [0,1)`, `reaction = (0.5 − skill)
  mod 1`, `accuracy = skill`, `behaviour = indexInKit mod 7` (`BotCreator.lua:55-106`). Behaviours: `Default,
  DontShootBackBail, DontShootBackHide, AbortAttackFast, LongerAttacking, LovesExplosives, LovesPistols`
  (`Shared\Constants\BotBehavior.lua`).
* **First-shot delay** = `BotFirstShotDelay 0.8 + ReactionTime 0.9 × U(0.8,1.2) × reaction + 0.005·d·(1 + 0.4·(reaction −
  0.5))` → 0.8–1.9 s plus ~0.5 s per 100 m (`BotGetters.lua:127-137`; `Config.lua:19, 166`).
* **Target point**: the target soldier's position + camera offset (head optional), **perfect knowledge** of position and
  velocity read from the engine (`BotAiming.lua:180-217`); lead = velocity × time-of-flight (`d/v`, or the intercept
  quadratic when `USE_ADVANCED_AIMING`, default off, `:154-177`; `Registry.lua:242`); drop compensation `½·g·t²` (×0.25
  for rockets, "no idea why, but works", `:233-239`); grenades use a 50-entry distance→pitch table (`:6-106`).
* **Error**: uniform `±worsening/(0.95·d)` rad on yaw and pitch, re-rolled every 0.03 s aim update (≈ ±worsening metres
  at the target at any range), `worsening = base × (0.5 + accuracy)`, base `BotAimWorsening 0.9`, sniper 0.3, LMG 1.1
  (`:276-304`; `Config.lua:14-18`). Note: fun-bots' "accuracy" is a *worsening* factor (higher = worse) [I from the
  formula].
* **Recoil compensation**: reads the live weapon state `gunSway.currentRecoilDeviation + currentDispersionDeviation` and
  adds `(1 − 0.6·accuracy)` of it to the aim (`:112-146`) — i.e. the bot "knows" its recoil exactly.
* Fire: modes timers (`BotFireModeDuration 4.5 s`, min 0.7 s on one target, vehicle 9 s; `Config.lua:166-170`),
  stop-to-shoot 35 % (`Registry.lua`), sideways strafing cycles (`Config.lua:159-162`).

### 1.6 Objectives, paths, squads, classes [V]

* **Director** (`GameDirector.lua:157-468`): every 1.5 s, per team: `attackSlots = ceil(bots/(4·nAttack + 1·nDefend)·4)`,
  `defendSlots = ceil(…·1)` (`Registry.lua:154`), each unassigned bot takes the **closest** objective with a free slot;
  vehicle-enter objectives get exactly one bot; over-assigned bots are released; bots on inactive/base paths for 50 s are
  killed.
* **Path following**: node reached within `0.8 m` (×1.5 sprinting, ×0.7/0.5 crouch/prone) and `1.5 m` height
  (`BotMovement.lua:428-446`; `Config.lua:172`); random lateral **path offset** ±0.8–1.2 m per path, disabled on stairs
  (|dy| > 0.35 m) and near actions (`:16-125`); stuck → retry, skip 0–4 nodes or reverse, teleport (80 %) after 2 tries,
  reroute to the closest path after 6 s, suicide after 15 s (`:350-397, 626-651`).
* **Path switching** at nodes carrying `Links`: priority 4 = path whose single objective is mine, 3 = path listing mine
  among several, 2 = another single-objective path, 0 = no objectives; switches forced toward active/enabled objectives
  and away from base paths; same-priority switches at 20 % (`PathSwitcher.lua:20-297`; `Registry.lua` 
  `PROBABILITY_SWITCH_SAME_PRIO`).
* **Rush**: MCOM nodes carry an `mcom` action = hold `EIAInteract` 6 s facing a recorded yaw/pitch (`NodeEditor.lua:145-
  151`); defenders/attackers by team; air target point = the active MCOM pair's midpoint (`GameDirector.lua:1740-1746`).
* **Squads** only matter for spawning: spawn on a squad mate 20 %, in a squad mate's vehicle 70 %, closest flag 40 %,
  attacked flag 60 %, base 5 % (`Registry.lua:276-295`; `BotSpawner.lua:1316-1427`); the first bot of a squad is leader.
* **Classes**: assault revives (defib in slot 6, corpse ≤ 15 m with server LOS, 80 %/40 % if engaged; `BotManager.lua:
  1480-1592`; `Registry.lua:201`) and drops medkits, support drops ammo every `DeployCycle 90 s` or on request, engineer
  repairs a player's vehicle on request (`BotActions.lua:41-51`), rockets 20 % (85 % for `LovesExplosives`) vs vehicles,
  grenades 50 %/95 % at the end of an attack cycle (`Registry.lua`).

### 1.7 Vehicles [V]

* **Land/water**: bots walk a path to a `vehicle` action node (`EIAInteract`, 0.5 s, recorded yaw/pitch;
  `NodeEditor.lua:178-185`), enter the nearest vehicle within 10 m (`BotActions.lua:375-399`), then follow paths whose
  first node carries `"Vehicles":["land"]` (or `water`; amphibious takes both) (`PathSwitcher.lua:91-119`). Steering is a
  PID on yaw error (`Kp 5, Ki 0.05, Kd 0.2`, `Bot.lua:187`) into `EIAYaw`; throttle levels 0.25/0.5/0.8/1.0 from the node
  speed, −0.7 reversing (`VehicleMovement.lua:295-347`); nodes are "reached" within 4–24 m depending on speed
  (`:188-203`). **Bots stop driving while shooting** (`UpdateShootMovementVehicle`, `:288-290`); stop-to-shoot 40 %;
  turret aimed by PID (`Kp 10, Ki 2, Kd 2`) on `EIARoll/EIAPitch` (`:600-650`); passengers exit near objectives (50 m,
  `Bot.lua:389-436`); `exit` actions on traces; seat upgrades every 3 s.
* **Helicopters ("pathless")**: target = active objective + 110 m up, ±20 m z offset by team (`VehicleChopperControl.lua:
  28-40`; `Registry.lua:184`); PIDs on yaw (`EIAYaw`), altitude error → `EIAThrottle` (collective), pitch held at −0.35 rad
  (20° nose down) cruising, roll 0 or ±0.1 rad in hard turns (`:56-178`); fire when yaw and pitch errors < 0.15 rad; abort
  when target pitch > 0.5 rad. **No terrain or obstacle avoidance** — altitude is relative to the objective's height.
* **Jets ("pathless")**: target = objective + 350 m up, ±100 m by team (`VehicleJetControl.lua:42-52`; `Registry.lua:182`);
  take-off = 12 s of "70 m ahead and 70 m up" (`:54-69`; `Registry.lua:176`); controls: `roll = −3·Δyaw`, `pitch = 3·Δpitch`,
  `yaw = Δyaw` with errors in the body frame (`:91-115, 170-177`); throttle PID to 86.94 m/s (313 km/h) (`:180-189`); attack
  runs abort 120 m above / 75 m below the cruise altitude (`:128-134`), cooldown 6 s (4 s after a jet) (`Registry.lua`);
  air targets = closest enemy air vehicle with 40 %/25 % chance of 2nd/3rd closest (`AirTargets.lua:50-90`); flares/smoke at
  40–80 % of a missile's travel time (`BotManager.lua:403-467`). Changelog: "Kind of support for Air Superiority (jets still
  too passive)" (`FB\.github\CHANGELOG.md`).
* Air paths were removed when flight became pathless ("No more air paths needed (only enter paths for choppers)",
  CHANGELOG; `FB\tools\remove_air-paths.py` deletes paths whose first node mentions `jet`/`air`).

### 1.8 What fun-bots cannot do [V/I]

No navmesh or replanning: bots only exist where humans recorded traces, and a destroyed wall or new crater is invisible to
them [V]. No bot-vs-bot combat without a connected human [V §1.4]. Perfect target knowledge with a random error instead of
perception [V]. No spotting, suppression response, cover, flanking or squad coordination [V]. Vehicles stop to shoot,
follow traces only, ballistics hand-tabulated per seat [V]. Aircraft have no terrain avoidance, fixed altitudes, no
formation/orbit logic; jets passive in Air Superiority [V/RC changelog]. Stuck handling by teleport/suicide [V]. Several
modes missing (§1.10). Map files must be authored per level×mode by humans in the running game [V §1.12].

### 1.9 The map file format (`FB\mapfiles\*.map`) [V]

Text, CRLF, `;`-separated, header `pathIndex;pointIndex;transX;transY;transZ;inputVar;data` (all 185 files), one row per
node, sorted by path then point; positions are **Frostbite world metres, Y up** (they land on our navmesh built from BF3
collision: §2.4).

| field | type | meaning |
|---|---|---|
| `pathIndex` | int ≥ 1 | path (trace) id; `0;0` = optional info row (14 files: `{"Authors":[…],"CompIndex":1,"Date":…}`) |
| `pointIndex` | int ≥ 1 | order within the path; consecutive nodes are implicitly linked (`Next`/`Previous`) |
| `transX/Y/Z` | float | node position |
| `inputVar` | int | `speed = v & 0xF` (0 wait, 1 prone, 2 crouch, 3 walk, 4 sprint, 5 slow), `extra = (v>>4) & 0xF` (1 = jump here), `opt = (v>>8) & 0xFF`: on a path's **first** node `0xFF` = not a loop (ping-pong at the ends), `0` = loop; on a wait node = wait seconds (`NodeCollection.lua:85-87, 522-555`; `BotGetters.lua:215-250`; `NodeEditor.lua:505-520`) |
| `data` | JSON or empty | `Objectives: [names]` (on first nodes: path belongs to these objectives; one name = the objective's own path), `Vehicles: ["land"\|"water"]` (first node: vehicle path), `LinkMode: 0, Links: [[path, point], …]` (switch candidates at this node), `Action: {type, inputs[], time, yaw, pitch, onlyPassengers}` with `type ∈ vehicle, mcom, exit, beacon, sniper, explore` |

Parsed examples (`FB\mapfiles\MP_007_ConquestLarge0.map`, Caspian CQL; `MP_Subway_RushLarge0.map`; `MP_001_RushLarge0.map`):

| row | decoded |
|---|---|
| `1;1;-78.491211;146.329910;678.092773;65283;{"Objectives":["b","base us"]}` | path 1 start; 65283 = 0xFF03 → walk, no jump, non-loop; path serves flag b and the US base |
| `38;1;-315.21;146.92;246.28;65283;{"LinkMode":0,"Links":[[34,148],[37,19],[49,132],[60,1]],"Objectives":["a","e"],"Vehicles":["land"]}` | a land-vehicle path between flags a and e, switchable to 4 other paths here |
| `28;1;-61.31;144.00;671.85;65283;{…"Objectives":["vehicle tankaa-us"]}` | an "enter vehicle" objective path (to the US AA tank spawn) |
| `28;23;-102.04;146.35;690.44;3;{"Action":{"inputs":[35],"pitch":-0.33,"time":0.5,"type":"vehicle","yaw":6.05}}` | walk; at this node press input 35 (`EIAInteract`) for 0.5 s facing yaw 6.05 rad → board |
| `23;36;-151.48;129.79;81.70;20;` | 20 = 0x14 → sprint + jump |
| `35;1;-85.95;69.04;-14.57;3;{"Action":{…"time":6,"type":"mcom"…},"Objectives":["mcom 1 interact"]}` (Metro Rush) | arm/disarm MCOM 1: hold interact 6 s |
| `55;50;-87.52;80.35;-45.70;512;` (Bazaar Rush) | 512 = 0x200 → wait 2 s here |
| `59;202;…;2;{"Action":{"inputs":[35],"time":0.5,"type":"exit"}}` (Caspian CQS) | crouch-speed node; dismount here |

**Totals over 185 files** (`SCR\scripts\mapstats.py`): 1,159,335 nodes, 9,857 paths, 25,174 link records (all
`LinkMode 0`), 8,244 `Objectives`, 2,244 actions (vehicle 1,112, mcom 559, exit 367, beacon 197, sniper 8, explore 1),
1,215 vehicle-path tags (land 1,177, **water 42** in 13 files, **air 0**); speeds: sprint 681,409, walk 414,290, crouch
60,839, prone 2,063, wait 732; 12,794 jump nodes. Mean node spacing 0.8–4.4 m per file. Per map (nodes / paths / links /
vehicle paths / objectives / extent): Metro CQL 3,843 / 51 / 208 / 0 / 9 / 154×478 m; Metro Rush 7,087 / 215 / 654 / 0 /
26; Metro TDM 2,012 / 8 / 0; Caspian CQL 8,569 / 64 / 470 / 11 land / 26 / 729×1,150 m; Caspian CQS 16,624 / 91 / 262 /
22 land; Bandar CQL 53,380 / 163 (the largest); Air Superiority files hold a single dummy node.

### 1.10 Coverage vs the MP level×mode list [V `SCR\scripts\coverage.py`]

IW4L's sweep lists **227** level×mode rows over 29 levels (`C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\
3-CONVERTER-PART\core-evidence\sweep.txt`; the handoff text says 230, I count 227 rows). fun-bots has 179 MP files
(+ 3 COOP, 3 SP_* that are irrelevant). Matching by `<Level>_<Mode>`: **160 direct**; **47 via fun-bots' own fallbacks**
(`NodeEditor.lua:1154-1186`: TDM-C/GunMaster/Scavenger → `TeamDeathMatch0`; Metro CQS → CQL; XP5_003 CQL → CQS;
XP4_Rubble Assault Large → Small; XP2's `TeamDeathMatchCQ0`/`CQTeamDeathMatch0` taken as VU's `TeamDeathMatch0` [I]);
**20 missing**: SquadRush0 on 12 levels (MP_012, MP_013, MP_018, XP1_001–004, XP5_001, XP5_003, XP3_Alborz, XP4_FD,
XP4_Parl), XP1 `Rush0` ×4, CaptureTheFlag0 on XP5_001/002/004, TankSuperiority0 on Alborz. fun-bots also has 12
`GunMaster0` files for base levels and XP2 `SquadRush0/TeamDeathMatch0` that the IW4L sweep does not list (mode-id naming
or patch layering; open question). Vehicle support per `FB\Supported-maps.md`: none on Metro; CQL/CQS on Caspian.

### 1.11 `mod.db` [V, opened read-only with `file:…?mode=ro&immutable=1`, `SCR\scripts\moddb.py`]

SQLite, 51.2 MB, 189 tables: 185 trace tables `<Level>_<Mode>_table (id INTEGER PRIMARY KEY AUTOINCREMENT, pathIndex
INTEGER, pointIndex INTEGER, transX FLOAT, transY FLOAT, transZ FLOAT, inputVar INTEGER, data TEXT)` with 1,159,349 rows
(= the map files' nodes + 14 info rows), `FB_Settings` (0 rows), `FB_Config_Trace` (0), `FB_Permissions` (1 row: a
player GUID/name — personal, not read further), `sqlite_sequence`. The runtime reads only `mod.db`
(`NodeCollection.lua:1256-1377`, `SELECT … ORDER BY pathIndex, pointIndex LIMIT 1024 OFFSET …`); the `.map` files are the
exchange format converted by `FB\fun-bots-helper\src\tools\import_traces.py`/`export_traces.py` (`addons\sets.py:324-380`).
No navmesh tables are present (the fork's baker would add `<map>_navmesh` tables, `navmesh-design.md`).

### 1.12 The editor [V]

In-game, F12 / commo-rose (`NodeEditor.lua`, `ClientNodeEditor.lua`, `UIPathMenu.lua`): a human **records a trace by
playing**: every ≥ 0.35 s and ≥ 0.35 m (`Config.TraceDelta`) a node is added with speed from pose/sprint/throttle and a jump
flag; switching to the secondary weapon turns the last node into a wait node counting seconds (`NodeEditor.lua:1295-1371`);
commands add `mcom`/`vehicle`/`exit`/custom actions with the player's current view angles (`:130-265`), vehicle tags
(`:269-318`), links, loop flag (`:505-520`), merge/split/teleport-to-edge. Saving rewrites the table (`NodeCollection.lua:
1565-1590`).

### 1.13 Licence and what it implies [V facts; implications I, not legal advice]

* fun-bots has **no licence**: `gh api repos/Joe91/fun-bots` → `"license": null`; `/license` → 404; no `LICENSE*` in the
  history of `FB` (`git log --all -- LICENSE*` empty). GitHub: without a licence "the default copyright laws apply" and
  "no one may reproduce, distribute, or create derivative works from your work"; other users may only "view and fork"
  under GitHub's ToS (https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository).
  Only `FB\astar-lib\a-star.lua` (fork addition) is MIT. Consistent with `research\topics\08-prior-art-legal.md` §1.
* (a) **Porting logic to C++**: translating its Lua (structure, constants, state machine) would be a derivative of the code
  → not without the authors' permission. Re-implementing the *ideas* documented here (time-sliced tiers, weighted objective
  slots, PID flight, recoil reading) from this description is ordinary engineering; keep our own structure and tuning [I].
* (b) **Loading `.map` files at runtime from the user's disk as optional data**: we would ship a reader for a published text
  format and nothing of theirs; the user supplies their own copy. Lower risk, still the authors' data — keep it opt-in,
  never required, never cached into anything we redistribute; good uses: validation oracle and hints (§2.6) [I].
* (c) **Shipping the map files** (or data derived from them, e.g. links baked into our navmesh files we distribute):
  not without written permission from the trace authors (the files record no per-path author; info rows exist in 14 files
  only) [I]. Asking Joe91 and contributors for permission or a licence is cheap and should be done if (b)/(c) become useful.

---

## 2. Navigation on Godot 4.8

### 2.1 What BF3 itself provides [V]

* **No navigation data in MP levels**: `LevelData.PathfindingBlobInfo {BlobId, BlobSize, ChunkSizes}` is all zero and
  `AISystem` null on MP_007 and MP_Subway (`C:\Users\olku\iw4L\context\bf3dump\level-structure\json_mp_007\levels\mp_007\
  mp_007.json`, `…\json\levels\mp_subway\…`; also `1-RESEARCH-PART\08-bf3-level-structure-vehicles.md:223`). The schema
  has BF3's SP/coop AI types (`PathfindingBlob`, `WaypointData`, `VehicleWaypointData`, `SensingSettings`,
  `AIDifficultyData`, `AIWeaponData`, `BFAISettingsData` in `VU-Docs\types\fb`), but MP ships no baked blob.
* **Roads exist as data**: `RoadData : RibbonData : VisualVectorShapeData : VectorShapeData {points[], normals[], tension,
  isClosed}`, `RibbonData.ribbonPoints[] {left, right}` half-widths (`VU-Docs\types\fb\RoadData.yaml`, `RibbonData.yaml`,
  `RibbonPointData.yaml`). Caspian `layer4_roads` has **134 `RoadData`**: 12 wide ribbons (median 13.9 m, 8.2 km), 18 at
  7.2 m (4.3 km), 4 unshaded at 7.2 m (5.3 km) and 100 narrow markings (0.3 m, 4.4 km); tension 0.5
  (`…\json_mp_007\levels\mp_007\mp_007\layer4_roads.json`). Metro has 75 (handoff 03 §114).
* **AI tuning on vehicle weapons**: MP vehicles carry `AIWeaponData` (BMP-2 remote HMG: `StartAccuracyPercentage 0.5`,
  `FinalAccuracyPercentage 0.5`, `MinBurstCoolDownTime 1`, `MaxBurstCoolDownTime 2`, `AimTransitionFireDelay 0.35`,
  `Inner/OuterInaccurateDistance 5/10`, `CanSuppress true`) and `SensingComponentData`
  (`C:\Users\olku\iw4L\context\bf3dump\ebx\vehicles\bmp2\bmp2.json`; 5 `AIWeaponData` there). BF3's own AI parameters, usable
  as defaults; the AI code that used them is not in the data.

### 2.2 Godot 4.8 dev7 API — what exists [V `API`]

| need | exposed (exact names) |
|---|---|
| maps, regions | `NavigationServer3D.map_create/map_set_active/map_set_cell_size/map_set_cell_height/map_set_use_edge_connections/map_set_edge_connection_margin/map_set_use_async_iterations/map_force_update/map_get_iteration_id`, `region_create/region_set_map/region_set_navigation_mesh/region_set_transform/region_set_navigation_layers/region_set_enter_cost/region_set_travel_cost/region_set_use_async_iterations/region_set_use_edge_connections` |
| bake from our arrays | `NavigationMeshSourceGeometryData3D.append_arrays(PackedFloat32Array, PackedInt32Array)`, `add_faces`, `add_mesh_array`, `add_projected_obstruction(vertices, elevation, height, carve)`, `merge`; `NavigationServer3D.bake_from_source_geometry_data(_async)(mesh, data, callback)`, `is_baking_navigation_mesh` |
| bake parameters | `NavigationMesh.cell_size, cell_height, border_size, agent_height, agent_radius, agent_max_climb, agent_max_slope, region_min_size, region_merge_size, edge_max_length, edge_max_error, vertices_per_polygon, detail_sample_distance, detail_sample_max_error, filter_low_hanging_obstacles, filter_ledge_spans, filter_walkable_low_height_spans, filter_baking_aabb, filter_baking_aabb_offset, sample_partition_type` — **no tile size, no per-triangle area type** |
| queries | `query_path(NavigationPathQueryParameters3D, NavigationPathQueryResult3D, Callable)`; parameters `map, start/target_position, navigation_layers, path_postprocessing (CORRIDORFUNNEL/EDGECENTERED/NONE), simplify_path, simplify_epsilon, included/excluded_regions, path_return_max_length/radius, path_search_max_polygons, path_search_max_distance`; `map_get_path`, `map_get_closest_point(_normal/_owner/_to_segment)`, `map_get_random_point`, `simplify_path` |
| links, obstacles, agents | `link_create/link_set_start_position/end_position/bidirectional/enter_cost/travel_cost`; `NavigationLink3D`; `obstacle_create/obstacle_set_vertices/radius/height/velocity`; `NavigationObstacle3D.affect_navigation_mesh, carve_navigation_mesh`; RVO `agent_*` |
| other | `AStar3D` (graph A*; per-point `open_pass/closed_pass` state → one search at a time per instance, `G:core/math/a_star.h:58-59, 109`); `WorkerThreadPool.add_task/add_group_task` (Callable only); **no `NavigationServer3DExtension`** (a custom server cannot be written in a GDExtension; `NavigationServer3DManager.register_server` exists) |

### 2.3 Godot internals that decide the question [V `G:` source at c971f93e7]

* Godot bundles **Recast 1.6.0 only** (no Detour, DetourCrowd or TileCache) (`G:thirdparty/README.md:957-965`); its own
  map/A* code replaces Detour.
* The bake (`G:modules/navigation_3d/3d/nav_mesh_generator_3d.cpp`) is a **single Recast solo bake per NavigationMesh**:
  `filter_baking_aabb` sets the heightfield bounds (`:398-406`), `border_size` → `cfg.borderSize` (`:348-349`), refused
  above **30,000,000 columns** unless crash checks are disabled (`:412-414`; at cs 0.25 m that is ≈ 1.37 km square), every
  triangle gets area 0/walkable from `rcMarkWalkableTriangles` only (`:431-439`) → **no area costs**; the result keeps
  **every Recast detail triangle as one navigation polygon** (`:556-578`).
* `append_arrays` keeps the caller's index order (`G:scene/resources/3d/navigation_mesh_source_geometry_data_3d.cpp:59-70`)
  while `add_mesh_array/add_faces` swap indices 2↔3 to turn Godot's clockwise front faces into Recast's order (`:177-179`).
  BF3 triangles are counter-clockwise-front (handoff §5) → feed them through `append_arrays` unchanged [V, walkable
  output in §2.4].
* Map build: tiles join when their border edges coincide (edge merge via hashed vertex keys); "edge connections" then
  compare **every free edge with every other free edge** (`G:modules/navigation_3d/3d/nav_map_builder_3d.cpp:190-232`),
  only when `use_edge_connections` is on (`:180-184`; default true, `G:servers/navigation_3d/navigation_server_3d.cpp:256`).
* Path queries take a slot from `navigation/pathfinding/max_threads` slots behind a semaphore and read-lock the current map
  iteration (`G:…/nav_map_3d.cpp:147-181, 860-870`) → concurrent queries are supported; the map builds asynchronously when
  `navigation/world/map_use_async_iterations` is on (`:395-396, 884`). Default search cap `path_search_max_polygons = 4096`
  (`G:servers/navigation_3d/navigation_constants_3d.h:75`). Docs: "NavigationServer2D and NavigationServer3D are both
  thread-safe and thread-friendly" (https://docs.godotengine.org/en/latest/tutorials/performance/thread_safe_apis.html).

### 2.4 Measurements on real BF3 collision [V; raw outputs in `SCR\navbench\*.txt`, `SCR\godotbench\*.txt`]

Input: IW4L's exported collision soups (`C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\3-CONVERTER-PART\
collision-evidence\mp_subway_conquestlarge0_collision.obj`, `mp_001_conquestlarge0_collision.obj`; IW inches Z-up,
converted back with `fb = (iw.y, iw.z, iw.x)/39.37008`; SOLID + GLASS + PLAYERCLIP groups; WATER left out). Agent: radius
0.5 m, height 1.8 m, climb 0.5 m, slope 45°. Recast/Detour = `RN` (v1.6.0-367-g9f4ce64) copied to `SCR\navbench\rn` and
built with MSVC 18 /O2; harness `SCR\navbench\navbench.cpp` (tiled bake like `Sample_TileMesh`, BV-tree, 65,535-node query
pool, 4,096-poly corridor).

| run | result |
|---|---|
| **Metro CQL**, 743,835 tris, 728 × 998 m, Detour, cs 0.25 / ch 0.1, 64 m tiles | 192 tiles (127 non-empty), 14,797 polys (49,882 detail tris), **2.6 MiB**; bake **3.7 s on 1 thread** (peak heap 12 MiB), 1.0 s on 4, 0.70 s on 8, 0.5–0.67 s on 16 (peak 81–86 MiB); busiest tile (47,172 tris) rebake 110–163 ms, swap into dtNavMesh **0.3 ms**; 400 queries in the largest component: mean **0.10–0.11 ms**, p95 0.29–0.32 ms, max 0.44–0.57 ms for paths of mean 288 m / max 830 m |
| Metro, cs 0.3 / ch 0.3, 48 m tiles, 1 thread | 2.45 s, 5.4 MiB peak — but `walkableClimb = floor(0.5/0.3) = 1 voxel` (0.3 m): stairs break, fun-bots nodes spread over 45 components. **Choose ch so that climb/ch is whole (ch 0.1 or 0.25)** |
| **Bazaar CQL**, 811,976 tris, 514 × 799 m, cs 0.25 / ch 0.25 | 101 tiles, 10,599 polys, 1.7 MiB; 0.46–0.48 s on 16 threads; queries mean 0.06 ms, max 0.37 ms |
| **Synthetic 2,048 × 2,048 m terrain** (2 m grid, 2.1 M tris, rolling ±35 m), cs 0.3 / ch 0.1, 64 m tiles | 1,089 tiles, 1,980 polys (235,049 detail tris), **3.1 MiB**, one component; **9.7 s wall on 16 threads, 26.6 s on 4 (≈ 107–155 s CPU)**, peak 8–21 MiB; queries over mean 1.3 km / max 2.9 km paths: mean 0.07 ms, max 0.47 ms |
| same terrain, cs 0.5, 128 m tiles | 256 tiles, 3.1 MiB, **2.5 s wall / 39 s CPU** |
| **Godot 4.8 dev7**, Metro, same agent, cs 0.25 / ch 0.1 (`SCR\godotbench\bench.gd`) | whole map as one NavigationMesh: **5.3–6.4 s**, 53,520 polygons (triangles); 64 m tiles with `border_size` 2 m and `filter_baking_aabb` (each tile reads the whole source): 6.4–11.2 s sync on the main thread, **0.76–1.5 s async**; 127 regions → 55,319 polygons, 62,590 merged edges |
| Godot map build | edge connections on (default): map usable **6.2–8.1 s** after adding the regions (37,505 free edges), **one region swap → 6.0 s** until the next iteration; edge connections off: **53 ms** and **48 ms**, same reachability (tiles join by edge merge) |
| Godot `query_path`, 300 random fun-bots node pairs | default cap 4,096: mean **5.1–5.9 ms**, max 9.5 ms, 135/300 reached; cap off: mean 5.8–8.7 ms, max 16–24 ms (endpoints are trace nodes in any component, Detour's are poly centroids in the largest; the ~50× gap is the order of magnitude, not an exact ratio) |
| fun-bots traces vs Detour navmesh (Metro CQL) | **97.1–97.8 %** of 3,843 nodes lie within 1 m of the navmesh; 58.9 % in the largest component, 29.9 % in the second; 34 component changes between consecutive trace nodes, mostly on flat floor (|dy| < 0.1 m) through a 2.4 m SOLID+GLASS object at (−172, 59.3, −259) that the in-game trace walks through. Bazaar: 98.4 % on mesh, 91.6 % in one component |

Reading the Metro split [I]: the soup contains something the soldier passes in BF3 (an open gate, a destructible already
gone, or a "Raycast"-body shell that BF3 characters do not collide with — IW4L used the Raycast mesh as SOLID whenever an
object had one, handoff 04 §3.2). The navmesh must be baked from **exactly the set the character controller collides
with** (and the fun-bots traces, where the user has them, make a cheap oracle for that: §2.6).

Perception rays (`SCR\godotbench\rays.gd`, Jolt inside Godot): Metro soup as one `ConcavePolygonShape3D` (built in
0.78–0.86 s); 20,000 eye-height rays between trace nodes < 150 m apart: **3.7 µs/ray** on the main thread including
GDScript call overhead, **0.75 µs/ray wall** through `WorkerThreadPool.add_group_task` on 16 threads; 90.8 % blocked.
`PhysicsServer3D.space_get_direct_state` must be called on the main thread (error at `physics_server_3d_wrap_mt.h:122` when
called from a worker), after which the state object worked from workers; Jolt refuses queries while the space steps
(`G:modules/jolt_physics/spaces/jolt_physics_direct_space_state_3d.cpp:452`) [V]. Since 07-godot-engine §4.3 and
05-networking §4.3 put the authoritative sim on its own thread with **our own Jolt world**, bot rays go to that world's
`NarrowPhaseQuery` between steps; the Godot number is an upper bound for the cost [I].

### 2.5 Per-tile rebake on destruction [V numbers; I design]

Destruction swaps collision pieces (handoff 04 §5.3). Keep a tile → collision-piece index; when a destruction state
changes, mark the tiles its AABB (+ agent radius + border) touches dirty, rebuild them on the job pool from the new piece
set (Metro's worst tile 110–163 ms; typical tiles are a fraction of that), then swap `removeTile/addTile` at a tick boundary
(0.3 ms). Bots whose corridor crosses a swapped tile re-plan (`dtPathCorridor::isValid`/`fixPathStart`). Debris that
blocks temporarily (wrecks, parked vehicles) is better handled by `dtTileCache` obstacles (`addObstacle`,
`addBoxObstacle`, `removeObstacle`, `update`; `RN\DetourTileCache\Include\DetourTileCache.h:137-156`) or local steering —
TileCache obstacles can only remove walkable area, so openings created by destruction still need a rebake. In Godot the
same swap costs 48 ms (edge connections off) or ~6 s (on) of stale navigation, and per-tile bakes re-read the whole source
unless we pass per-tile geometry.

### 2.6 Links, obstacles and what to bake [I unless tagged]

* Bake source for soldiers: static collision the character collides with + terrain heightfield (+holes) + PLAYERCLIP;
  never foliage/scatter (no collision in BF3, handoff 04 §3.4); WATER marked as a separate area (shallow water walkable,
  deep water excluded by depth test).
* **Off-mesh links**: (1) automatic drop-downs (ledges ≤ ~4 m with a clear landing; IW4L baked drops ≤ 192 in = 4.9 m),
  (2) jumps over gaps/low walls found by sampling border edges, (3) ladders if BF3 data has ladder entities (not checked,
  open question), (4) optional hints from the user's fun-bots traces: consecutive nodes that change navmesh component, or
  carry the jump flag, propose candidate links to validate by a capsule sweep. Godot equivalents would be
  `NavigationLink3D`; Detour uses `offMeshCon*` in `dtNavMeshCreateParams` (per tile).
* **Areas** (Detour only): ROAD (cheap for vehicles), WATER, INDOOR (prefer for cover), OBJECTIVE zone; costs via
  `dtQueryFilter::setAreaCost`, plus flags for "vehicle-passable".
* **Per agent class**: soldier (r 0.5, h 1.8, climb 0.5, slope 45°); jeep/LAV class (half width + 0.3 m, climb 0.4–0.6 m,
  slope ~30°), tank class (wider, same climb), boat (water surface mesh). Vehicle meshes include VEHICLECLIP, exclude
  PLAYERCLIP (handoff 04 §9). One `dtNavMesh` per class; Godot would need one map per class because the radius is baked in.
* **Aircraft**: a 2.5-D max-height grid (terrain + tallest static collision per 8–16 m cell, built at load from the same
  soup) gives "minimum safe altitude" along any segment; no mesh.

### 2.7 Options compared

| | Godot NavigationServer3D | Recast/Detour linked in `bf3` | IW4L walk graph | fun-bots traces |
|---|---|---|---|---|
| bake from BF3 collision | yes, own arrays [V] | yes [V] | yes (48 in grid, coarse lattice) | no (human-recorded in-game) |
| 2 km maps | must tile manually (30 M column guard), each tile re-reads all input unless pre-split [V] | native tiles [V] | 1 M-node cap, coarse 192 in lattice, 4 s Caspian [RC handoff] | per mode file, e.g. Caspian CQS 16,624 nodes [V] |
| query cost (Metro) | 5–9 ms mean [V] | 0.06–0.11 ms [V] | A* quota 2,048 expansions/tick [RC] | trivial (follow a list) |
| long paths | 4,096-polygon default cap [V] | node pool sized by us; sliced search [V API] | quota-limited | path switching only |
| area costs / road preference | no (region costs only) [V] | per-poly areas + filter [V] | no | vehicle paths only |
| dynamic change | region swap → map rebuild (48 ms / 6 s) [V] | tile swap 0.3 ms; TileCache obstacles [V] | never re-baked [RC] | none |
| threading | thread-safe queries, query slots [V] | one `dtNavMeshQuery` per thread (instance node pool, `DetourNavMeshQuery.h:573-577`) [V]; mesh edits between ticks [I] | own | n/a |
| fits server-thread architecture (07/05) | callable off-main (thread-safe) but engine-coupled [I] | engine-independent, unit-testable [I] | n/a | n/a |
| licence | MIT (engine) | zlib [V `RN\License.txt`] | ours | none (§1.13) |

### 2.8 Godot pitfalls if the NavigationServer is used anyway (e.g. for debug or tools) [V/I]

Set `map_set_use_edge_connections(false)` and `border_size` per tile (edge merge then joins tiles); match
`map_set_cell_size/height` to the meshes; raise `path_search_max_polygons` for long paths; wait for
`map_get_iteration_id` (queries before the first sync fail: "made before first map synchronization", seen in the run);
feed BF3 triangles through `append_arrays` (no winding swap).

### 2.9 Recommendation (hybrid)

1. **Infantry**: Recast/Detour vendored into the Godot-independent `bf3` library (namespace or hidden visibility to avoid
   clashing with Godot's own Recast symbols on Linux [I]); tiled navmesh 64 m, cs 0.25, ch 0.1, built per level×mode inside
   the mode's combat area + margin, on the job pool at load, cached as `user://nav/<collision digest>-<params hash>.bin`
   (Metro 2.6 MiB; a 2 km terrain map ≈ 3 MiB + structures).
2. **Vehicles**: one Detour navmesh per hull class with ROAD areas stamped from BF3's own `RoadData` ribbons (centre spline +
   left/right widths) and terrain road decals → routes prefer roads without a separate graph; corridor + pure pursuit for
   steering (§3.7).
3. **Aircraft**: 2.5-D clearance grid + objective/target points (§3.8).
4. **fun-bots traces** (only if the user has them, opt-in path in settings): validation oracle (coverage % per map, as in
   §2.4) and link/position hints (sniper, beacon, explore nodes). Not a runtime dependency, not shipped.
5. **Godot NavigationServer3D**: not in the bot path; optional only for editor-style debug drawing.

---

## 3. Behaviour design in C++

### 3.1 Structure [I]

`bf3_bots` (static, Godot-independent, unit-tested headless) consumed by the server thread (05-networking §4.3):

| part | rate (60 Hz sim) | input → output |
|---|---|---|
| `Director` per team | 1–2 Hz | objective states, team roster → objective slots, squad orders |
| `Squad` | 2 Hz | leader goal, members' positions → member goals (follow/hold/attack point) |
| `Brain` per bot (utility selection over tasks: capture, defend, roam, fight, revive, resupply, repair, board vehicle, drive, fly, flee) | 5–10 Hz, staggered | blackboard → task + nav goal |
| `Perception` per bot | 10 Hz candidates, rays batched on the job pool | snapshot → contacts (seen / heard / spotted / remembered) |
| `Motor` | every tick | corridor, contact → `Command` (move axes, buttons, view yaw/pitch) |
| `AimController`, `VehicleDriver`, `HeliPilot`, `JetPilot`, `GunnerController` | every tick | target state → axes/buttons |

The server sim is read through an `ISimView` (immutable per-tick snapshot) and bots write only `Command`s; world queries
go through an `IWorldQuery` (rays, ground height, navmesh) so tests can run on recorded worlds.

### 3.2 Objectives per mode [I; numbers from fun-bots/IW4L as starting points]

* **Conquest (CQL/CQS/Assault)**: capture points from `CapturePointEntityData` (`captureRadius`, `timeToGetControl`,
  `timeToLoseControl`, `onlyTakeableByTeam`, `initialOwnerTeam`; schema `VU-Docs\types\fb\CapturePointEntityData.yaml`). Slot
  assignment like fun-bots (attack weight 4, defend 1, ceil, min 1, every 1–2 s), scored by travel time (navmesh distance /
  speed, or vehicle trip time — IW4L: walk 150 in/s then ride at ½·min(750, 0.8·top) in/s, board only if faster), threat
  (recent deaths/contacts there) and ticket state (bleeding team attacks more). Inside the radius: spread to sampled nav
  points inside it, prefer cover/indoor polygons, face likely approach directions (corridor entry points).
* **Rush**: attackers → current MCOM pair (arm = hold interact; time from the MCOM entity data, fun-bots used 6 s),
  defenders → armed MCOM first (disarm), else hold; respawn waves follow the stage.
* **TDM/SQDM/GunMaster/Scavenger**: roam to sampled nav points biased toward recent contact heat; SQDM = squads as teams.
* **Domination (XP2)**: conquest rules on small maps. **CTF (XP5)**: carrier → own base by shortest path, escorts follow,
  defenders intercept the enemy carrier. **Tank/Air Superiority**: vehicle-only conquest/dogfight.

### 3.3 Squads [I]

BF3 squads of up to 4. Leader chooses the squad's objective (Director suggestion), members keep within ~25 m (IW4L group
radius 300 in ≈ 7.6 m was for moving together), spawn on the leader/beacon as BF3 allows, mirror the leader's orders;
human squad leaders' commo-rose orders (spot/attack/defend) re-target bot members.

### 3.4 Perception and spotting [I; data fields V]

* Candidates: enemies within the weapon-class range and a FOV cone (fun-bots 160°×90° on foot is a sane start), plus anyone
  who damaged the bot, made sound nearby, or is **spotted** for the team. For each candidate, cast 2 rays (head, chest)
  from the eye; keep `lastSeen`, `lastKnownPos`, `confidence`; re-test a committed target every 2–3 ticks, others at 5–10 Hz.
  Detection is not instant: accumulate "seen time" scaled by distance, stance, motion and FOV eccentricity (fun-bots'
  edge-miss formula is a crude version) before a target becomes engageable.
* **Spotting**: bots press the spot input when they acquire a target, rate-limited by `SpottingComponentData.
  coolDownHistoryTime / coolDownAllowedSpotsWithinHistory`, with `spottingFov`, `spottingDistance`; targets carry
  `SpottingTargetComponentData.activeSpottedTime/passiveSpottedTime` (schema fields V; BF3 values in the soldier blueprint
  not yet read). Spotted enemies enter every teammate bot's knowledge as positions (as on a human's minimap) but still need
  LOS to shoot.
* Budget: 64 bots × ≤ 6 candidates × 2 rays × 10 Hz ≈ 7,700 rays/s ≈ 128 rays per 60 Hz tick → ≈ 0.5 ms at the measured
  3.7 µs/ray single-threaded, ≈ 0.1 ms on the job pool (§2.4).

### 3.5 Combat [I; data fields V]

* **Target choice**: threat (aiming at me, recently hit me), distance, weapon effectiveness (`AIWeaponData.Strengths
  {Infantry, LightArmour, HeavyArmour, Helicopter}` where present), objective relevance, switching hysteresis.
* **Reaction**: first-shot delay = base(skill) + distance term (fun-bots: 0.8–1.9 s + 0.5 s/100 m is lenient; human-like is
  0.25–0.6 s at close range), plus a re-acquire delay after losing LOS.
* **Aim model**: aim point = target hitbox point (BF3 head/chest from the soldier data) + lead (`t` from the intercept of
  the target's velocity with the projectile speed `ShotConfigData.initialSpeed`) + drop (`BulletEntityData.gravity`, both
  the fields fun-bots reads, `WeaponClass.lua:198-199`). Error = angular offset that starts at `e0(skill, distance,
  target speed, own movement, suppression)` and decays toward `e∞(skill)` over the time on target — exactly the shape of
  BF3's `AIWeaponData.StartAccuracyPercentage → FinalAccuracyPercentage` with `AIDifficultyData.accuracyIncreaseTimeModifier`
  (schema V). Drive the view through a rate-limited, critically damped controller (max turn rate by skill; fun-bots 550°/s
  is superhuman for rifles).
* **Recoil/spread**: the shared weapon sim applies spread, recoil, suppression and stance; the bot compensates a fraction
  of the recoil it reads from its own weapon state (skill), and bursts per weapon class (AIWeaponData burst limits/cool
  downs where present; else per-class defaults).
* **Suppression response**: BF3's suppression already degrades the bot's weapon through the shared sim
  (`SoldierSuppressionComponentData` schema); the brain adds behaviour: crouch/prone, prefer cover polygons, delay peeks.
* **Cover (optional, later)**: sample nav polygons within 10–20 m, keep those with a blocked chest ray to the threat and an
  open eye ray when standing (peek), cache per area.
* **Damage**: no damage multipliers (fun-bots' 0.6) — difficulty comes from perception, reaction and aim only.

### 3.6 Class behaviours [I]

Assault: revive corpses within ~15–30 m with LOS (probability lower when engaged, fun-bots 80/40 %), drop medkits near
hurt teammates. Support: ammo bags near teammates low on ammo; LMG suppressive fire at last-known positions. Engineer:
repair friendly vehicles (walk to them, hold the tool), rockets vs vehicles (priority by threat), mines on roads near
objectives. Recon: high-ground/sniper positions found offline (nav polygons with long sightlines over objectives; or the
user's fun-bots `sniper` nodes as hints), radio beacon behind the front, spot a lot. All of it through the same inputs
(select gadget, aim, fire/hold).

### 3.7 Land vehicles [I; IW4L/fun-bots numbers RC/V]

Board: path to the vehicle's entry point, press interact within the entry radius (BF3 data, handoff 04 §4.1), choose seat
by role. Drive: plan on the hull-class navmesh with ROAD areas cheap; follow the corridor with pure pursuit (IW4L:
lookahead 220 in + 0.6·v ≈ 5.6 m + 0.6·v), speed from curvature and slope, brake for corners; box sweep ahead for
obstacles; stuck → reverse, re-plan, shared per-match memory of impassable spots (IW4L had 16–18 stuck events per 2 min on
Bazaar before its clearance filter). Tanks pivot in place when the heading error is large (IW4L < 150 in/s); gunners lay
the turret at a capped rate (IW4L 35 % of view speed) and fire when on target; drivers may keep moving while gunners shoot
(fun-bots stops). Bail at `VehicleSpawnReferenceObjectData.botBailWhenHealthBelow` after `botBailOutDelay` (schema fields
V; values per spawner unread); dismount near the objective (fun-bots 50 m). Boats: water-surface mesh/graph.

### 3.8 Helicopters and jets [I; fun-bots V]

* **Helicopter pilot**: goal = objective/target + altitude, but altitude **above the clearance grid along the next 300 m**
  (fun-bots used objective + 110 m with no terrain check). Cascaded control: altitude error → collective (PID), horizontal
  velocity error → pitch/roll attitude targets (PID), attitude → cyclic inputs, heading → pedals; attack runs = point the
  nose (pilot gun/rockets) or orbit for the gunner; break off and climb under AA lock (flares, fun-bots' 40–80 % of missile
  ETA). Validate against our helicopter sim (04-vehicles); gains depend on it.
* **Jet pilot**: cruise circuits at ~300–400 m AGL around the combat area (fun-bots 350 m above the objective), throttle
  PID to a cruise speed (fun-bots 86.9 m/s; use the jet's data), attack runs on air targets (pursuit with lead) and ground
  targets (dive with pull-up altitude from the clearance grid), abort/re-attack logic, flares, take-off from the runway
  heading (fun-bots: 12 s straight climb).
* Both: never pathfind on a mesh; use the clearance grid for "minimum altitude on segment", and a small set of precomputed
  loiter points per level (from objectives and combat-area outline).

### 3.9 CPU budget for 64 bots [I from V measurements]

| item | per second | per 60 Hz tick |
|---|---|---|
| perception rays (§3.4) | ~7,700 rays | ~0.5 ms single-thread / ~0.1 ms on the job pool |
| path queries (re-plan every ~4 s + events ≈ 20/s, Detour 0.1–0.5 ms) | 2–10 ms | 0.03–0.17 ms (sliced, `initSlicedFindPath/updateSlicedFindPath`, `DetourNavMeshQuery.h:215-236`) |
| brains (64 × ~10 µs at 10 Hz) | ~6 ms | ~0.1 ms |
| motors/controllers (64 × ~1–2 µs every tick) | — | ~0.1 ms |
| director/squads | negligible | — |
| **total** | | **≈ 0.4–1 ms single-thread; less with the pool** |

IW4L measured 0.2–2 ms per 20 Hz tick for 16 bots (perception + routing) and 1–2 ms bot thinking cut to 0.2 ms with cheaper
perception [RC handoff 04 §9-10]; fun-bots slices its 3 tiers over ticks for the same reason [V]. Rule: brains and
perception run on the server job pool between sim steps; nav-mesh tile swaps and the commit of `Command`s happen on the
server thread at the tick boundary.

---

## 4. Bots as server-side fake clients emitting player `Command`s

[V for fun-bots' practice; I for our design] fun-bots proves the concept on BF3 itself: bots are `Player`s whose
`EntryInput` levels are written like a human's, so BF3's own soldier, weapon and vehicle code runs unchanged (§1.3).
05-networking already specifies `bots → BotCommand = same Command type` and a server pipeline `commands → bots → sim`.
Implications:

* **One sim path**: movement, stamina/sprint, spread/recoil/suppression, vehicle physics, seat rules and damage are
  identical for bots and humans; bugs reproduce with either; no "bot physics".
* **Replication for free**: a bot is a server-owned player slot; clients interpolate it like a remote human; scoreboard,
  kill feed, spotting all work. Bots occupy slots (64 total).
* **No latency, so add it**: server bots see the true state each tick; reaction time, perception delay and turn-rate
  limits must be explicit (§3.5). Lag compensation does not apply to bot shots (they fire at the present).
* **Closed-loop control is mandatory**: bots must turn desired motion into stick/axis values — trivial on foot, PID/pure
  pursuit for vehicles, cascaded control for aircraft; tuning depends on the vehicle sims.
* **No shortcuts**: board with interact, no teleports or seat warps, redeploy (suicide) only as a last resort after long
  stuck, logged. Keep a debug-only "teleport bot" command.
* **Determinism and testing**: per-bot RNG seeded from match seed + slot; bots' commands are recorded with everyone else's,
  so demos and soak tests replay exactly; a headless bots-only server (Godot console build, `--headless`) can run soak tests
  with metrics.
* **Command content**: the button set can mirror BF3's `EntryInputActionEnum` (70 values; fun-bots uses 0–36 as levels):
  move axes, view yaw/pitch, vehicle throttle/brake/yaw/pitch/roll, fire, zoom, reload, weapon/gadget slot, interact,
  spot/order, change seat/entry, parachute, countermeasure.
* **Cost**: a bot `Command` is produced one tick ahead from the previous snapshot; the brain never touches sim state
  directly.

---

## 5. Implementation plan (ordered; each step ends verified in the running game)

Prerequisites from other topics: M0 loaders and collision assembly (handoff §8), the server thread and `Command`
(05-networking), soldier movement with BF3 numbers (02/M3), weapons for B3 (placeholder hitscan before M7).

| step | build | in-game verification |
|---|---|---|
| **B0 Nav bake (offline + cache)** | Recast/Detour in `bf3`; bake from the character-collision set; `bf3nav` CLI: bake, stats, component report, and (if a fun-bots folder is configured) trace coverage; cache under `user://nav/` keyed by digest | Metro CQL: ≥ 97 % of trace nodes on mesh within 1 m and **all flags, HQs and spawns in one component** after links/collision fixes (today 58.9 %+29.9 % split: find and fix the passable objects); bake ≤ 5 s on 1 thread, cache load < 100 ms; deleting the cache or changing the install digest re-bakes; nothing written outside `user://` |
| **B1 Debug view** | draw navmesh, links, corridors, bot intents (RS immediate geometry) | toggle overlay on Metro; corridors hug walls by ≥ agent radius |
| **B2 Walkers on Metro TDM** | `BotManager`, `Command` emission, Motor (corridor following, stuck recovery), roam goals | 32 bots for 10 min: every bot moves ≥ 80 % of the time, ≤ 1 redeploy per bot-hour, no bot outside the combat area, CPU ≤ 0.5 ms/tick |
| **B3 Metro Conquest capture** | Director slots, capture logic, defend spread, spawn choice | bots-only CQL round ends by tickets; each flag changes hands ≥ 2 times; bots distributed ≈ by slot weights (log) |
| **B4 Combat** | perception (rays on the job pool), aim controller, firing through the weapon sim, reaction/skill, revive/ammo/med, grenades | bots-only server: kills happen (unlike fun-bots without humans); team K/D ≈ 1; no shots through walls (ray agreement test); human-vs-bot TTK vs skill setting in a table; perception ≤ 0.2 ms/tick wall |
| **B5 Rush, squads, spotting** | MCOM arm/disarm, squad follow/spawn, spot input + team knowledge | Metro Rush: attackers arm and destroy ≥ 1 MCOM pair per round on default skill; spotted markers appear for humans when bots spot |
| **B6 Caspian infantry** | tiled bake for 2 km, sliced queries, vehicle trip-time rule disabled | bake ≤ 15 s first time on 16 threads, cached after; p95 query < 1 ms; bots reach every flag; long walks visible (vehicles next) |
| **B7 Land vehicles** | hull-class navmeshes, ROAD areas from `RoadData`, boarding by interact, driver pure pursuit, gunners, bail, dismount | Caspian CQL: ≥ 3 of 4 jeeps and the tanks taken within 30 s of spawn; ≥ 1 km driven per vehicle per 5 min; ≤ 2 stuck events per vehicle per 10 min; tank kills on armour |
| **B8 Helicopters** | clearance grid, HeliPilot, gunner roles, flares | AH-1Z/Mi-28 and transport helis take off, patrol, attack, land/bail; 0 terrain crashes in 10 min bots-only; AA bots shoot at them |
| **B9 Jets, boats, AA** | JetPilot circuits/attack runs, boat driver, stationary AA | jets take off, fight each other, no ground impacts in 10 min; boats on Kharg/Oman reach shore objectives |
| **B10 Scale and soak** | 64 bots, all levels in rotation | 64 bots at 60 Hz: bot total ≤ 2 ms/tick p99 on the server thread; 2-hour rotation without stalls; per-level nav coverage report clean |

---

## 6. What I could not determine

* Why Metro's navmesh splits where human traces walk through (gate open in game, destructible removed, or BF3 characters
  not colliding with that object's Raycast body). Needs the collision set rebuilt from Collision/Character bodies only and
  a re-run of the coverage probe; the IW4L OBJ has no body type per triangle.
* BF3 soldier capsule (radius/height per pose), step height, max slope (`CharacterPhysicsData`) — the bake used 0.5/1.8/
  0.5 m/45° placeholders. Ladder/vault entities in MP data, if any.
* Real Caspian bake time and memory (only Metro/Bazaar soups exist as exports; Caspian was estimated with a synthetic
  2 km terrain). Whether mixing cell sizes per tile (coarse on open terrain) keeps Detour's tile links intact.
* `RoadData` junction topology (endpoints must be snapped into a graph), the spline type behind `Tension 0.5`, and whether
  `RoadData` without a shader are drivable roads.
* BF3 values of `SpottingComponentData`, `SpottingTargetComponentData`, `SoldierSuppressionComponentData` and the MCOM arm
  time in the soldier/gameplay blueprints (fields known, values not read); whether infantry weapons carry `AIWeaponData`
  like the vehicles.
* fun-bots' mode-id differences from the IW4L sweep (GunMaster0 on base maps, XP2 TDM ids, XP1 `Rush0` vs `RushLarge0`)
  and the handoff's 230 vs the sweep's 227 rows.
* Godot query cost under its own `max_threads` slots with many concurrent queries, and Godot's numbers on a Caspian-size map
  (not measured; the Metro numbers already rule it out for the bot path).
* Whether the fun-bots authors would grant permission to redistribute the traces (not asked).
* Exact cost of ray queries against our own Jolt world from C++ (measured only through Godot's Jolt with GDScript overhead).

---

## Independent verification (2026-10-10)

Done by a separate verifier that tried to refute each claim. It re-read the code and data, re-fetched Godot sources at `c971f93e7` from GitHub (byte-identical to `SCR\godot-src`), and re-ran the measurements with its own probes. `VER` =
`C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\verify-06-bots`:
* `VER\gproj\vbench.gd` is an independent GDScript NavigationServer3D probe.
* `VER\nb2\navbench_v.cpp` is a copy of the harness with three additions: trace-pair queries, a two-sided-triangle switch and agent radius/slope switches.
* `VER\mapcount.py` and `VER\cov.py` are independent parsers.

Nothing was written to the BF3 install or to any read-only folder. Tags: [V] checked here, [RC] not re-checked, [I] inference.

| id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| C1 | fun-bots has no licence; only `astar-lib/a-star.lua` is MIT | corrected | [V] `gh api repos/Joe91/fun-bots` → `"license": null`; `/license` → 404. No `LICENSE*`/`COPYING` on master or any of the 13 upstream branches (`git ls-tree -r`). `git log --all -- LICENSE*` is empty. The GitHub docs wording was re-read. `FB\astar-lib\a-star.lua:1-23` is MIT. **But** upstream `FB\WebUI\package.json:17` declares `"license": "MIT"` (`"author": "Joe91"`, `"private": true`; file history from 2025-04-16). Commits: master 3,672, upstream/master 3,644, all refs 3,713 | A second MIT declaration exists: the upstream WebUI npm package metadata, scoped to `WebUI/`. The bot logic (`ext/`) and `mapfiles/` carry no licence, so the §1.13 conclusions stand |
| C2 | bots are Players driven through EntryInput; SetLevel 0–36 every tick; SingleStepEntry each tick; yaw 550°/s; API shortcuts | corrected | [V] Confirmed: `BotManager.lua:876-878` (`EntryInput()`, `EntryInputFlags.AuthoritativeAiming`); `:152` SingleStepEntry in the every-tick loop; `Config.lua:171` 550; `Registry.lua:164` (10 m); `Registry.lua:218` (80 %). Wrong part: `Bot:_UpdateInputs` (`Bot.lua:519-544`, the 0–36 SetLevel loop) is called only from each state's `Update` (`BotStates\StateMoving.lua:42`, `StateAttacking.lua:64`, …). That is the 0.13 s batched tier (`Registry.lua:212`, `BotManager.lua:55-62`) | Input levels are rewritten about every 0.13 s (≈ 7.7 Hz, staggered across bots). Only SingleStepEntry and the yaw slew run every tick. §1.3 contradicts §1.2 on this point |
| C3 | LOS is computed on human clients; with no human, bots never acquire each other | corrected | [V] Confirmed: `BotManager.lua:1363-1364` returns when `#self._ActivePlayers == 0`; `:1297-1323` sends `CheckBotBotAttack` to active players (`RegisterActivePlayer`, `:551-560`). Not covered by the claim: jet pilots and stationary-AA bots pick targets on the server with no ray, via `AirTargets:GetTarget` (`StateInVehicleJetControl.lua:60`, `StateInVehicleStationaryAAControl.lua:67`). That list holds every player in an air vehicle, bots included (`AirTargets.lua:24-26, 95-102`). A damaged bot also shoots back at the damage giver (`BotManager.lua:284-293, 326`) | Ground and infantry bot-vs-bot acquisition needs a connected human. Jet and AA-vs-aircraft combat and shoot-back-on-damage work without one. Also applies to §0.1, §1.8 and the B4 wording |
| C4 | 185 recorded trace files; 1,159,335 nodes; 9,857 paths; row format; inputVar bits; land 1,177, water 42, no air | confirmed | [V] `VER\mapcount.py` over `FB\mapfiles`: 185 files, the same header in all, 1,159,335 nodes, 9,857 paths, 14 info rows, 0 bad rows. Tags: land 1,177 (73 files), water 42 (13 files), no row mentions air or jet. Actions: 1,112 / 559 / 367 / 197 / 8 / 1. Decode at `NodeCollection.lua:85-87`. Recording by playing at `NodeEditor.lua:1295-1330` | Speed codes 5 and 6 also occur, once each |
| C5 | 227 rows: 160 direct, 47 via fallbacks, 20 missing | corrected | [V] `VER\cov.py`: sweep.txt has **230** rows over 29 levels. 160 match directly and 39 match through fun-bots' own rules (`NodeEditor.lua:1154-1186`). 31 do not match: the report's 20, plus 3 `MP_012 TutorialMP*` rows, plus 8 XP2 `CQTeamDeathMatch0`/`TeamDeathMatchCQ0` rows. The TDM fallback fires only for `TeamDeathMatchC0`/`TeamDeathMatch0` (`Server\__init__.lua:894-898`). VU lists only `TeamDeathMatchC0` for XP2 (`VU-Docs\content\hosting\maps.md:81-82`), and that mode does fall back | 227 = 230 minus the 3 tutorial rows. The "47" is 39 by fun-bots' code plus 8 XP2 id aliases that VU never runs [I]. The list of 20 missing rows is right. XP1 `Rush0` is not on VU's mode list either (`maps.md:77-80`) |
| C6 | MP levels have no baked nav (PathfindingBlobInfo zero, AISystem null) | confirmed | [V] `…\json_mp_007\levels\mp_007\mp_007.json:7272-7277` and `…\bf3dump\ebx\levels\mp_subway\mp_subway.json:28876-28881`: zero BlobId, BlobSize 0, empty ChunkSizes, `AISystem: null`. Independent check: the EBX probe found pathfinding partitions only in COOP/SP levels and no navmesh res type (`research\probes\ebx-data-probe.md:245-247`) | — |
| C7 | Godot query_path 5.1–8.7 ms (max 24) vs Detour 0.06–0.11 ms (max 0.57); 55,319 vs 14,797 polygons | confirmed | [V] `VER\gproj\vbench.gd`, 4 runs on the same soup (64 m async tiles, 400 random trace-node pairs). Results: 55,319 polygons; mean 4.0–7.0 ms with the 4,096 cap and 7.2–8.7 ms uncapped; max 8.2–30.7 ms. Pairs only 1–10 nodes apart already cost 2.6–6.1 ms. The cause is a brute-force start/end search over every polygon of every region (`G:modules/navigation_3d/3d/nav_mesh_queries_3d.cpp:237-277`); `map_get_closest_point` alone takes 1.7–4.0 ms. Detour rerun: 14,797 polygons (`VER\nb_metro_1t.txt`). Detour on Godot's exact workload (random trace-node pairs in any component, plus two findNearestPoly calls): mean 0.16–0.17 ms, p95 0.49–0.51 ms, max 0.69–0.95 ms (`VER\nb2\metro_1t_v.txt`, `metro_2s.txt`) | Refinement: on an identical workload the gap is ≈ 25–50×. Detour's 0.06–0.11 ms is for centroid pairs inside one component. The conclusion is unchanged |
| C8 | default edge connections compare every free edge with every other; ~6 s stale after a swap; 48–53 ms with them off | confirmed | [V] Source: `G:…/3d/nav_map_builder_3d.cpp:180-184` collects free edges only when edge connections are on; `:211-232` is a nested loop over all free edges. The default is true (`navigation_server_3d.cpp:256`). Own reruns, edge connections on vs off: map usable after 5.8–6.6 s vs 71–96 ms; one region swap 6.0–6.4 s vs 28–56 ms. Reachability is identical: 176/400 capped, 180/400 uncapped (`VER\vbench_*.txt`) | — |
| C9 | Recast 1.6.0 only; one solo bake per mesh; 30M-column guard; no area types; cap 4,096; append_arrays keeps winding | confirmed | [V] Source, byte-identical to GitHub at c971f93e7: `G:thirdparty/README.md:957-965` (Recast/ and License only); `nav_mesh_generator_3d.cpp:348-349, 398-414, 431-439, 556-578`; `navigation_constants_3d.h:75`; `navigation_mesh_source_geometry_data_3d.cpp:59-70` vs `:176-179`. The API has no tile or area property and no per-triangle area input. The handoff verifies CCW front faces for render meshes (`sections/02-resources.md:87`) and states the collision contract (`04-gameplay-sim.md:115`). The soup also has `*_two_sided` groups, which Recast reads one-sided. Duplicating all 84,149 of them with reversed winding changed nothing on Metro (`VER\nb2\metro_2s.txt`) | Caveat: Havok two-sided shells carry no reliable winding. Feed them both ways, or flip them normal-up |
| C10 | Detour tiled bake numbers (Metro 3.7 s/12 MiB/2.6 MiB; 2 km in 9.7 s; tile 110–163 ms, swap 0.3 ms; < 0.5 ms queries) | confirmed | [V] Harness code read: per-tile Recast, custom rc/dt allocators for peak heap, `steady_clock`. Reruns: Metro on 1 thread 3.69 s and 4.20 s, peak 12.0 MiB, 2.6 MiB, 127 tiles. Synthetic 2 km on 16 threads: 8.3 s, 3.1 MiB, one component, max query 0.21 ms over 2.9 km paths. Busiest Metro tile: rebake 113–139 ms, swap 0.29–0.56 ms | Swap 0.3–0.6 ms (still sub-ms). Synthetic peak heap 21–28 MiB at 16 threads |
| C11 | 97.1–97.8 % of trace nodes within 1 m; 58.9 % + 29.9 % split; cause unknown; bake from the character-collision set | confirmed | [V] Reruns give 3,757 of 3,843 nodes (97.8 %) and 2,265 + 1,148 nodes in the two largest components. "Within 1 m" means horizontal distance (`dtVdist2D`) inside a ±2 m vertical search box (findNearestPoly ext {1,2,1}). New results: (a) the two-sided winding cause is ruled out (the split is unchanged with duplicated two-sided triangles). (b) SOLID objects 2–4.6 m tall stand on the floor at the split points (`probe_obj.py` at (−171.5, −258.9), (−131.7, −235.3), (−150.2, −273.2)). (c) BF3's own capsule (r 0.35, StepHeight 0.3, slope 50°, `ebx-data-probe.md:170-171`) makes it **worse**: 95.7 % on the mesh, the largest node component holds only 43.4 %, 37 components. Step 0.3 causes this (`VER\nb2\metro_bf3capsule.txt`); r 0.35 with climb 0.5 reproduces the original split exactly | Strengthens the conclusion. The IW4L soup has steps a 0.3 m soldier cannot climb in Recast terms. Bake from the character-collision set, with the BF3 capsule rather than the 0.5/0.5/45 placeholders |
| C12 | RoadData road splines; Caspian has 134 (≈ 18 km of 7–14 m ribbons plus 100 narrow markings) | corrected | [V] Own parse of `layer4_roads.json`: 134 `RoadData`. Ribbons ≥ 7 m wide: 29, 17.1 km of control-point polyline; plus 5 at 5.0–5.1 m (0.5 km). Ribbons ≤ 1.3 m: 101 (99 at 0.3 m). `Tension` is 0.5 on 129 and 0.0 on 5. `RibbonPoints[].Right` is stored negative (Left 0.16, Right −0.16) | Left and Right are signed lateral offsets, so width = Left − Right. They are not two positive half-widths as §2.1 says. Tension is not uniform. The totals agree |
| C13 | rays 3.7 µs on the main thread and 0.75 µs wall on 16 threads; direct state is main-thread only; Jolt refuses queries while stepping | confirmed | [V] `SCR\godotbench\metro_rays.txt`: 3.68 µs, 0.75 µs, 18,153 of 20,000 blocked. `G:servers/physics_3d/physics_server_3d_wrap_mt.h:122` (`!Thread::is_main_thread()`). `jolt_physics_direct_space_state_3d.cpp:452` (the same guard on all 8 query kinds); each query calls `flush_pending_objects`. GDExtension probe in C++: `intersect_ray` 1.5–2.2 µs, `intersect_ray_into` 0.53–0.99 µs; 4 threads give 2.2×, and 8 are slower than 2 | Rays cost 2–7× less from C++. Multi-thread scaling is weak, so the 16-thread figure is optimistic |
| C14 | fun-bots flies pathless (+110 m helicopters, +350 m jets), no terrain avoidance; land vehicles stop to shoot | confirmed | [V] `Registry.lua:182, 184`; `VehicleChopperControl.lua:29`; `VehicleJetControl.lua:46`; no raycast or terrain query in either control file; `VehicleMovement.lua:288-290` sets NoMovement while attacking | — |
| C15 | MP vehicle weapons carry AIWeaponData gunnery parameters; vehicles carry SensingComponentData | corrected | [V] `bmp2.json` has 5 `AIWeaponData` and 3 `SensingComponentData`. All five weapons have identical values: Start = FinalAccuracyPercentage 0.5, burst cooldown 1/2 s, AimTransitionFireDelay 0.35, inaccurate distances 5/10 m, CanSuppress true, all `Strengths` 0, MinRange/MaxRange 0 | The fields are present but hold generic template values: no accuracy convergence and zero strengths and ranges. They are weak as tuning data |
| C16 | bots as server-side fake clients sending the same `Command` | confirmed (design inference; premises verified) | [V] `05-networking.md:570` ("BotCommand = same Command type") and `:220` (commands → bots → sim); fun-bots drives BF3's own soldier through EntryInput (C2). [I] The design is sound and respects the read-as-is principle | fun-bots itself also uses API shortcuts and writes inputs at ≈ 7.7 Hz; do not copy either (§4 already says so) |
| C17 | 64 bots at 60 Hz ≈ 0.4–1 ms per tick single-threaded | corrected | [V arithmetic] The §3.9 rows sum to 0.47 + 0.03–0.17 + 0.11 + 0.06–0.13 = **0.67–0.87 ms**. §0.3 says 1–2 ms single-threaded and `05-networking.md:598` says 1–2 ms wall on the pool. Using the probe's C++ ray cost (0.53–2.2 µs), perception is 0.07–0.28 ms and the total ≈ 0.3–0.7 ms. The brain (10 µs) and motor (1–2 µs) costs per bot are unmeasured. The IW4L figure matches `handoff sections/04-gameplay-sim.md:458, 467` | ≈ 0.3–0.9 ms per tick single-threaded (an estimate dominated by rays). Rays must run in the window between Jolt steps. §0 and 05-networking need reconciling |
| extra-1 | §0.3: "≈ 1–2 ms per tick single-threaded, < 1 ms wall with the job pool" | corrected | Contradicts §3.9 (0.4–1 ms) and its own table (C17) | Use one figure, ≈ 0.3–0.9 ms single-threaded |
| extra-2 | §1.1: VU 1.3.1 "(2021-12-10)" introduced the server RaycastManager | corrected | [V] The changelog uses DD/MM/YYYY ("18/10/2023", "29/05/2022"); 1.3.1 is "12/10/2021" (`VU-Docs\content\vext\changelog.md:152-155`) | Released 2021-10-12 |
| extra-3 | §2.4, §2.6, §6: agent 0.5/1.8/0.5/45 are placeholders; capsule, step and slope unknown | corrected | [V in probe] The EBX probe read CharacterPhysicsData: `PhysicalRadius 0.35`, `MaxAscendAngle 50`, `LadderAcceptAngle 90`. CharacterPoseData: heights 1.8/1.15/0.3, `StepHeight 0.3` (`ebx-data-probe.md:170-171`). Effect on the bake: C11(c) | Use r 0.35, climb 0.3, slope 50. ch 0.1 keeps climb/ch whole; ch 0.25 does not |
| extra-4 | §6: spotting, suppression and spawner bail values not read | corrected | [V in probe] `SpottingDistance 1200`, `SpottingFov 4`, passive spot 1.0 s. Suppression: `SuppressionSphereRadius 1.5`, `SuppressionDecay 0.5`, `FallOffDelay 2.0`, thresholds 0.3/0.4. `BotBailOutDelay 0.5` on the Caspian M1 spawner. Humvee driver AI: `WantedSpeed 22`, driver settings `LookAheadTime 1.2` (`ebx-data-probe.md:179-181, 226-255`) | Use these BF3 values as the defaults |
| extra-5 | §1.10, §6: handoff's 230 vs the sweep's 227 rows is an open question | corrected | [V] The sweep has 230 rows. The 3 extra are `MP_012` `TutorialMP`, `TutorialMP_Sandbox` and `TutorialMP_ShootHouse` (subworlds 0) | Resolved |
| extra-6 | Principle check over every recommendation | confirmed | [V] Navmeshes and road data are built from BF3 data read in place and cached as `user://nav/<digest>` (§2.9, §5 B0). fun-bots and VU are references only. fun-bots traces are optional, user-supplied, and never shipped or cached for redistribution. The IW4L OBJ served only as a measurement input outside the install | No violation found |

**Not determined by this verification:**
* Godot's whole-map bake time (5.3–6.4 s) and the 37,505 free-edge count were not re-run.
* What the SOLID objects at Metro's split points are in game (gates, kiosks, Raycast-only shells), and whether BF3's Character bodies give stairs ramps that a 0.3 m step can climb. Both need the collision rebuilt per body type.
* IW4L's 0.2–2 ms per tick for 16 bots stays [RC].
* Per-bot brain and motor costs are unmeasured.
* The Linux symbol clash between a vendored Recast and Godot's own Recast was not checked.
* The legal effect of the WebUI `package.json` MIT field (it only labels the UI package) is not legal advice.
