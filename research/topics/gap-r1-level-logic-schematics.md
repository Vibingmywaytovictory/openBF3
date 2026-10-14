# Gap r1: per-level logic schematics, and D12 decided again

Date 2026-10-14. Gap slug `level-logic-schematics` (severity high), from
`C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\critic-r1.json`.
All install access was read-only. All data stays in the scratch folder
`...\scratchpad\level-logic` (abbreviated `LL\` below). Nothing was written under the install or into the repo except this
file.

Tags: **[verified]** means I read it from data or source in this session (or in the interrupted earlier run of this same
gap, whose outputs I re-read and spot-checked; see §10). **[report claim]** means a statement from another document.
**[inferred]** means my reasoning.

---

## 1. Answer

* **D12 as written ("no event-graph VM") does not hold for BF3's MP data.** The mode rules themselves are native C++
  entities in BF3 too (capture points, ticket, life and kill counters, spawners). But **which** spawners, vehicles,
  flags, combat areas, scripted events and destruction a level×mode turns on, and **when**, is decided by graph wiring.
  That wiring crosses up to four blueprint boundaries: shared prefab → level root → mode sub-world → shared prefab.
  It is keyed by designer-named ports that survive only as hashes (§3.5) [verified].
* **Scale.** Across the 230 level×mode rows, the **level-owned** server-side logic, after removing destruction
  coupling, constants and link lists, has a median of 56 edges per row (family medians 27–190, mean 110) and a maximum of 560 (xp3_valley RushLarge0).
  It sums to 25,304 edges over all rows. 73 rows have ≥ 100 such edges and 24 rows have ≥ 300 [verified, §3.4].
  Every one of the seven traced rows contains level-specific gameplay switching (§4).
* **The node vocabulary is small and closed.** 55 primitive logic types are reachable. 48 of them occur in a
  server-capable realm. 20 types cover 94.7 % of the server-capable unique instances and 36 types cover 99 %
  [verified, §3.3]. No `SyncedInt`/`SyncedFloat` occurs in MP level logic. `SyncedSequence` occurs almost only inside
  vehicle blueprints (§5.3).
* **Recommendation: revise D12 to "native gameplay entities + a deterministic, server-side, whitelisted logic
  interpreter + a replicated logic ghost"** (option A in §7). It is about 5–8 k LOC, against an estimated 15–45 k LOC
  plus per-level reverse engineering for hand-written rules (option B). Its main advantage is that **it needs no port
  names**: hashes are just ids. Option B has to work out the meaning of every designer-named hash, and 49–93 % of
  prefab and interface ports are unresolved (§3.5). Confidence M–H. Validate early with spike S4 (§9.2).

---

## 2. Method and tools

* Dumper: the existing probe was copied to `LL\ebx-dump` and gained a `logic` command (`LL\ebx-dump\src\logic.rs`), built
  into `LL\target\release\ebx-dump.exe`. Over `@all-mp` it writes, for every `Blueprint`-primary partition:
  * objects by type × `Realm`;
  * every `EventConnections` / `PropertyConnections` / `LinkConnections` row, grouped as
    (source type, realm, port, child blueprint, internal/external; target ditto; `TargetType`);
  * every `ReferenceObjectData`, with its child blueprint, `Excluded`, `AutoLoad` and `SubWorldInclusionSetting`;
  * the interface descriptor fields.

  Outputs are in `LL\census\{bp,nodes,edges,refs,iface}.tsv` (10,208 blueprints; 34,659 grouped edge rows) [verified].
* Closures: `LL\py\load.py` walks each level root through `WorldPartReferenceObjectData`, prefab references and
  `SubWorldReferenceObjectData`. Sub-worlds are filtered by the `GameMode` criterion of their inclusion settings, and
  each prefab is counted once per instantiation. This gives **230 rows over 29 levels** and 2,710 reachable unique
  partitions [verified: `LL\out\summary.txt`; the 230 matches handoff `sections\04-gameplay-sim.md` §1 (report claim)].
* Port names: djb2-xor hash `h=5381; h=h*33^byte` (u32 stored as i32). It is matched against every type name, field
  name and enum value in `C:\Users\olku\VU-Docs\types\fb\*.yaml`, plus the event names in
  `VU-Docs\content\vext\entities.md` (`LL\py\names.py`), plus a curated brute-force list (`LL\out\brute_curated.txt`;
  matches are shown with a `~` prefix).
* Classification: `LL\py\classes.py` sorts types into primitive/gameplay/presentation/structure. `LL\py\funcs.py` sorts
  edges by function (destruction, sound, fx, ui, spawn-links, constants, mode/gameplay, other). `LL\py\report.py` assigns
  the server/client side: E edges by `TargetType`, P and L edges by endpoint realm. Both classifications are heuristic
  [inferred].
* Traces: per-blueprint edge listings of the traced sub-worlds are in `LL\tr\*.txt` and `LL\tr2\*.txt` (with the raw
  JSON dumps beside them), compacted by `LL\py\trace.py` and `LL\py\filt.sh`.

---

## 3. Census

### 3.1 Where connections live

Connections are stored only in the sub-world roots and in prefab blueprints, never in layers [verified,
`LL\out\blueprints.tsv` / `bp.tsv`]. `WorldPartData` (layers) holds 0 edges. `SubWorldData` holds 31,046 of the
26,311 E + 20,334 P + 24,324 L edges in MP data, and the rest sit in prefabs. The layer named `*_schematic` holds the
**nodes**, while its parent sub-world (`levels/<l>/<mode>` or the level root) holds their **wires**.

Unique reachable partitions by origin [verified, `LL\out\blueprints.tsv` aggregated]:

| origin | partitions | primitive nodes | connections |
|---|---:|---:|---:|
| level root (`levels/x/x`) | 29 | 0 | 11,257 |
| mode sub-worlds | 227 | 0 | 30,925 |
| `*_schematic` layers | 284 | 1,334 | 0 |
| other layers | 1,245 | 482 | 0 |
| level-local prefabs | 225 | 283 | 2,052 |
| shared `gameplay/*` prefabs | 155 | 1,741 | 7,032 |
| other shared prefabs (props, fx, ui, sound) | 545 | 1,226 | 7,327 |

**Level-owned data holds 2,099 of 5,066 primitive nodes and 44,234 of 58,593 connections** (unique, not weighted)
[verified arithmetic]. The earlier reading of D12 rested on the shared `gameplay/*` row only.

### 3.2 Per level×mode totals (230 rows; instances counted per instantiation)

From `LL\out\combos.tsv` [verified]:

| per row | min | median | max | sum over 230 |
|---|---:|---:|---:|---:|
| primitive logic instances | 45 | 649 | 2,082 | 192,087 |
| of which server realm | 0 | 170 | 1,036 | 72,126 |
| `SyncedBool` instances | 0 | 17 | 259 | 13,102 |
| primitives in `*_schematic` layers | 3 | 19 | 75 | 5,269 |
| EventConnections | 270 | 2,117 | 9,223 | 661,293 |
| PropertyConnections | 115 | 1,150 | 2,745 | 273,450 |
| LinkConnections | 59 | 481 | 846 | 110,554 |
| edges classified server-side | 10 | 956 | 3,898 | 326,859 |
| edges classified client-only | 378 | 1,858 | 4,540 | 445,321 |

Per family (min/median/max per row) [verified, `LL\out\families.tsv`, `LL\out\funcs_family.tsv`]:

| family | rows | prim inst | server prim | SyncedBool | level server logic, no destruction | shared server logic |
|---|---:|---|---|---|---|---|
| Conquest (S/L) | 42 | 384/766/1788 | 133/268/701 | 8/45/136 | 24/100/538 | 826/1497/2924 |
| ConquestAssault | 12 | 448/643/885 | 168/228/340 | 4/10/33 | 20/64/193 | 842/1524/2218 |
| Rush | 29 | 1368/1644/2082 | 758/849/1036 | 132/150/259 | 10/97/560 | 3951/4610/5666 |
| SquadRush | 25 | 1323/1559/1929 | 751/770/904 | 132/136/227 | 4/52/502 | 3903/4142/5269 |
| TDM | 62 | 45/474/883 | 0/59/195 | 0/5/100 | 0/39/497 | 36/498/1704 |
| SQDM | 29 | 268/494/901 | 57/76/212 | 3/5/100 | 0/40/497 | 246/590/1781 |
| GunMaster | 8 | 250/354/555 | 37/38/62 | 3/3/7 | 4/39/162 | 198/459/1510 |
| Domination | 4 | 290/343/402 | 124/124/124 | 2/2/2 | 24/40/111 | 476/737/995 |
| Scavenger | 4 | 550/713/772 | 210/213/229 | 2/2/6 | 24/62/182 | 1313/1943/2562 |
| TankSuperiority | 4 | 468/553/845 | 91/98/111 | 3/3/87 | 13/190/509 | 527/610/894 |
| AirSuperiority | 4 | 752/858/1326 | 438/448/461 | 75/75/77 | 1/32/378 | 1420/1503/1553 |
| CaptureTheFlag | 4 | 605/714/1197 | 319/328/363 | 41/51/59 | 18/49/396 | 1309/1385/1552 |
| Tutorial | 3 | 130/130/130 | 15/15/15 | 6/6/6 | 27/27/27 | 253/253/253 |

The "level server logic, no destruction" column counts edges in level-owned partitions that run on the server, after
removing destruction coupling, spawn links, constants/ports and other link lists (`LL\py\funcs.py`).

Function totals over all 230 rows [verified, `LL\out\funcs.tsv`]:

| function | level-owned, server | shared, server |
|---|---:|---:|
| destruction coupling | 65,381 | 109,536 |
| spawn links | 31,856 | 2,757 |
| constants/ports | 13,112 | 19,388 |
| mode/gameplay | 9,462 | 217,920 |
| other logic | 11,246 | 24,188 |
| sound | 3,983 | 25,588 |
| UI | 2 | 52,675 |

Client-only presentation (level-owned): sound 51,648, destruction 33,807, fx/ambient 11,848, other 16,253.

### 3.3 Node types (union of the 230 closures)

There are 55 primitive types. Below are the server-capable unique instances, where realm is Server, ClientAndServer or
no `Realm` field ("N", which means networked or gameplay-owned). The cumulative share is of the 3,310 server-capable
unique primitives [verified, `LL\out\summary.txt`].

| # | type | server-capable unique | realms (S/CS/C/N) | weighted ×230 | cumulative |
|---:|---|---:|---|---:|---:|
| 1 | SyncedBoolEntityData | 579 | N 579 | 13,102 | 17.5 % |
| 2 | DelayEntityData | 392 | S 316, CS 76, C 137 | 27,910 | 29.3 % |
| 3 | AndEntityData | 337 | S 337, C 1 | 5,156 | 39.5 % |
| 4 | BoolEntityData | 319 | S 213, CS 106, C 52 | 4,879 | 49.2 % |
| 5 | TeamFilterEntityData | 243 | S 243, C 56 | 15,077 | 56.5 % |
| 6 | EventGateEntityData | 178 | S 172, CS 6, C 136 | 22,969 | 61.9 % |
| 7 | NotEntityData | 171 | S 168, CS 3 | 2,434 | 67.0 % |
| 8 | PlayerFilterEntityData | 155 | S 155, C 5 | 4,089 | 71.7 % |
| 9 | MessageEntityData (UI message) | 142 | N | 6,631 | 76.0 % |
| 10 | SequenceEntityData | 127 | S 49, CS 78, C 140 | 13,576 | 79.8 % |
| 11 | BoolToEventEntityData | 99 | S 99, C 18 | 3,099 | 82.8 % |
| 12 | RandomDelayEntityData | 88 | S 84, CS 4, C 86 | 1,980 | 85.5 % |
| 13 | CompareIntEntityData | 74 | S 74, C 43 | 4,078 | 87.7 % |
| 14 | EventSplitterEntityData | 70 | S 55, CS 15, C 49 | 9,627 | 89.8 % |
| 15 | CompareEntityData | 45 | S 45 | 1,566 | 91.2 % |
| 16 | IntEntityData | 29 | S 27, CS 2, C 12 | 666 | 92.1 % |
| 17 | ConsoleCommandTriggerEntityData | 25 | CS 23, S 2 | 355 | 92.8 % |
| 18 | OrEntityData | 23 | S 22, CS 1 | 512 | 93.5 % |
| 19 | PropertyGateEntityData | 19 | S 19, C 2 | 77 | 94.1 % |
| 20 | FloatEntityData | 19 | S 2, CS 17, C 359 | 21,021 | 94.7 % |

Types 21–48, up to 100 %: PrintDebugText, IntHub, BoolHub, CompareFloat, PropertyCast, MathOp (S 10 / C 410),
TransformHub, RandomMultiEvent, SyncedTransform (8), CompareBool, TransformMultiplier, Or4, MultipleTrigger,
TransformSplitter, DelayTrigger, VecSplitter, Vec3MathOp, TransformSelector, PlatformSplitter, EventSwitch,
SwitchPropertyString, Vec3, Transform, ToWorldSpaceTransform, TelemetryTrigger, SyncedSequence (2), PropertyDebug,
EventAndGate. Client-only types: RandomEvent, FloatHub, EventMemory, EulerTransform(Splitter), DebugText, CountDown.

Reachable **gameplay** entity types: 42 [verified]. Per-type schema fields, the events VU documents and the observed
in/out ports are in `LL\out\node_types.md` (679 lines). Examples:

* `SyncedBool`: in `SetTrue` 165 / `SetFalse` 84; out `OnTrue` 133 / `OnFalse` 35 / property `Out`.
* `Delay` (`delay`, `autoStart`, `runOnce`, `removeDuplicateEvents`): in `In` 416, `Restart` 11; out `Out` 1,066.
* `Sequence` (`events[]`, `propertyTracks[]`, `customSequenceTracks[]`, `looping`, `playInReverse`, `externalTime`):
  in `Start`/`Stop`/`Reset`/`PlayFirstFrame`/`Pause`; out `OnFinished`, `OnStopped` and designer-named track events.
* `TeamFilter`: in `In`; out `OnTriggeredByMatchingTeam` / `OnTriggerOnlyForSpecifiedTeam`.
* `CompareInt`: outs `A<B`, `A=B`, `A>B`, `A!=B` (named through the brute list).
* `VehicleSpawnReferenceObjectData`: P-in `Enabled` 506, `SpawnDelay` 83, `InitialSpawnDelay` 75, `MaxCount` 48; E-in
  `Spawn` 61, `Enable`/`Disable` 20 each; E-out `OnDestroyed` 41, `OnSpawned` 38, `OnPlayerEnter` 32.

### 3.4 Event routing (`EventConnection.TargetType`)

The enum is `Invalid 0`, `ClientAndServer 1`, `Client 2`, `Server 3`, `NetworkedClient 4`,
`NetworkedClientAndServer 5` [verified, `VU-Docs\types\fb\EventConnectionTargetType.yaml`]. `Realm` is `Client 0`,
`Server 1`, `ClientAndServer 2`, `None 3`, `Pipeline 4` [verified, `Realm.yaml`].

Unique reachable E edges: Client 5,565 · Server 5,522 · ClientAndServer 5,183 · **NetworkedClient 1,175 ·
NetworkedClientAndServer 464** [verified]. Weighted over 230 rows: server 221,662, client 240,233, both 139,206,
net-both 20,206, net-client 39,986 (`LL\out\edge_sides.tsv`). About 9 % of event edges are "Networked". I read
"Networked" as "fired on the server, delivered over the wire to clients" [inferred from the naming and from VU's
split runtime classes, §5].

### 3.5 Port-hash resolution (unique reachable edges)

[verified, `LL\out\summary.txt`; resolved = VU-Docs dictionary; "+brute" = curated list]

| kind | entity ports resolved | prefab/interface ports resolved |
|---|---|---|
| Event | 11,676 VU + 1,326 brute of 14,452 = **90 %** | 10,806 + 132 of 21,366 = **51 %** |
| Property | 13,366 + 1,376 of 16,911 = **87 %** | 5,325 of 16,571 = **32 %** |
| Link (source side) | 5,033 of 5,128 = **98 %** (target port 0 = the object itself in 92 % of rows) | 1,380 of 18,815 = **7 %** |

Interface ports carry designer names such as "Base1Spawn…", and `DataField` stores only `id`, `value`, `valueRef` and
`accessType`, with **no name** [verified, `VU-Docs\types\fb\DataField.yaml`]. The names are therefore not in the data.
Sequential hashes such as 1362204026…29 or −469454974…71 betray `…1`…`…4` digit suffixes [inferred]. An interpreter does
not care. A hand-written port does.

---

## 4. Traced examples (what each mode schematic switches)

Sources: `LL\tr\metro_rush_graph.txt`, `metro_rush_ep.txt`, `base_rush.txt` and `LL\tr2\{caspian_cql,kharg_rush,
bandar_cqs,neb,talah,palace}.txt`. Edge lines are [verified]. Meanings of unnamed hashes are [inferred].

**Metro (MP_Subway) RushLarge0.** Level `rush` sub-world: E 143 / P 92 / L 122.
* Layer `layer2_rush_logic` instantiates the shared `gameplay/level_setups/complete_setup/full_rush_level` (36
  primitives, 435 edges per instance) and feeds it about 28 `LinearTransform` interface constants (MCOM/crate
  positions), `BaseCount 4`, a float 2.5 (arm time) and an int 100 (lives).
* LinkConnections pass in, per base, the attacker and defender combat-area volumes (`VolumeVectorShapeData` in
  `layer4_base1_attacker_cz` … `layer17_base_4_defender_cz`) and 10 `UICombatAreaEntityData`.
* **8 `CharacterSpawnReferenceObjectData`** (Team1/Team2 × 4 bases, `Enabled=False`, linked through
  `AlternativeSpawnPoints` to 10–12 `AlternateSpawnEntityData` each) are driven by prefab outputs
  (#1362204026–29, #1378385658–61) → level `SyncedBool` (`layer39_full_rush_level_v2`) `.Out` → `.Enabled`.
  The raw JSON re-check is in §10.
* Level-specific scripted event: prefab output #963802567 (fired by the base progression) → `EventGate` → `Delay 10 s`
  → `EventGate` → `SyncedBool.SetTrue` (`layer35_parkconstructionsiteevent_schematic`) → `Delay 0.1` (ClientAndServer)
  → two more SyncedBools. Their effects:
  * `Predestruction.Enable` and `DamageAreaTrigger.DamageArea` (Server);
  * swapping `parkentrance_01_pre…` for `parkentrancedestroyed_02` through a `Not` on the property path;
  * about 20 `EffectReferenceObjectData.Start` (Client);
  * sounds via `NetworkedClient`.

  The gates are closed by prefab outputs #−570607766 / #−7291286.
* Client-only: cameras (`Sequence 30 s` → `CameraEntity.TakeControl`) and an ambient-warfare missile launcher. Its
  server `Delay 80 s` loop is gated by `EventGate`, and it plays client `Sequence`s that move `ReferenceObjectData`
  aircraft prefabs through `TransformMultiplier`.

**Caspian (MP_007) ConquestLarge0.** Sub-world: E 67 / P 73 / L 203.
* Layer `layer0_cq_l_logic` holds `gameplay/gamemodes/conquest`, capture-point prefabs, HQs and vehicle spawners.
* **Level schematic `layer1_conquest_full_4_flags2`**: capture prefab `OnCaptured` → `TeamFilter`(Team1/Team2) →
  `Bool` / `SyncedBool` Set{True,False}. Then:
  * `SyncedBool.Out` → `dynamicspawn_f18` / `dynamicspawn_su35` `.Enabled` (jets follow flag ownership);
  * `Bool.Value` → M1A2 / T-90 spawner `.Enabled`;
  * `SyncedBool`→`And`/`Not` → Growler / VDV-buggy `.Enabled`.

  The spawner's own output (#154856772) feeds back into `Not` [verified edges; "flag-dependent vehicle swap" inferred].
* Level root: `UnderFireTrigger` → `Delay 1/25 s` → `Disable`/`Enable` loops (Server) driving client effects. The
  conquest sub-worlds push two property constants into the radio-tower spawn prefab. There are 59 destruction-coupling
  edges.

**Kharg (MP_018) RushLarge0.** `rush` sub-world: E 64 / P 128 / L 145. This is the richest scripted logic of the seven.
* Prefab output #963802567 → `EventSplitter` → `khargmissilelauncher.Destroy` (Server) + `.Stop`
  (NetworkedClientAndServer), plus a mobile-radar prop.
* Prefab output #433417572 → `SyncedBool/Bool.SetFalse`.
* **Beach landing**: `SyncedBool` (`layer34_beachlanding`) `.OnTrue` → `Sequence 40 s` and `44 s` (ClientAndServer)
  that drive `prefab/lcac_w_humvee_armored` and `prefab/lcac_w_tank`, then `OnFinished` → `SyncedBool.SetTrue` →
  `TeamFilter(Team1)` → `Message`. In other words, the LCAC hovercraft delivers vehicles.
* **Airstrike**: `GeometryTrigger.OnEnter` (Delay 4, RunOnce) or `Delay 500 s` (AutoStart) → `SyncedBool` →
  `prefab/airstrike_mod2.Start` (Server) + `Sequence 10 s` + sound.
* Beach floodlights: `GeometryTrigger` → `SyncedBool` → `Delay 3/7`.
* Per-base vehicle spawners: `Bool` → `dynamicspawn_armoured…`, `dynamicspawn_m1abrams` `.Enabled`; `Bool` →
  `AlternateSpawn.Enabled`; `rib_prefab.Enabled`. `Or4` → `SyncedBool.In`.
* The level root pushes alternate-spawn properties for the Conquest sub-worlds and contains a `test_boat2` `Sequence`.

**Bandar Desert (XP3_Desert) ConquestSmall0.**
* The shared `conquest` sub-world drives `BoolHub.Out` → 9 capture prefabs `.Enabled` and `IntHub.Out` → `.AreaValue`.
  The CQS0/CQL0 sub-worlds select which flags are on through `.In2`/`.In4` and set `AreaValue`.
* **Gunship**: capture prefab F (artillery base) `OnCaptured` → level layer → **event into the CQS0 sub-world's
  interface** (#−2055381215, NetworkedClientAndServer) → `outpostspawn_xp_us_gunship_ru_gunship` #−1011855818
  (Server). Flag B → `outpostspawn_xp_2s25_sprutsd_m1128_nocamera`. Flag G → `Bool.SetTrue`.

  This one wire crosses four blueprints: shared capture prefab → level layer → mode sub-world → shared spawner prefab.
* Level root: gas-station destruction chains (`OnDestroyed` → `RandomDelay 0.1–2 s`, RunOnce → `Destroy` of the
  neighbouring pumps; client `MathOp` fade).

**Nebandan (XP5_002) CaptureTheFlag0 and AirSuperiority0.**
* CTF (E 18 / P 34 / L 16): the shared `complete_setup/full_ctf_prototype` (130 primitives) exchanges about 10 events
  and 4 properties each way with `xp5/gameplay/ctf_us_base` and `ctf_ru_base` (level-pack prefabs).
* AS (P 85 / L 5): interface constants (`InitialTicketCount` ×2, `AreaValue`, location SIDs) go into
  `gameplay/gamemodes/conquest` and three `capturepointprefab_nospawner_as*`.
* **12 + 12 `dynamicspawn_xp_{f18,su35}_spawninair_as`** get `SpawnDelay`/`InitialSpawnDelay` from the interface and
  their enable (#2088939021) from a `SyncedBool` in `layer1_as_gameplay_schematics`.
* Level root: 176 ClientAndServer event edges (mostly destruction/props).

**Talah (XP4_Rubble) Scavenger0.**
* `gameplay/gamemodes/domination` + `capturepointprefab_nospawner` × 5; `OnCaptured`/`OnLost` → `layer1_scavenger_logic_
  schematic` (game-mode voice-over and UI prefabs).
* 12 interface constants go into the domination prefab.
* Level root: `rubble_pillar_prefab` → `Delay 0.1` (RunOnce, ClientAndServer) → destroy a car prop.
* Scavenger weapon pickups are not in this sub-world's edges [not traced further].

**Palace (XP2_Palace) Domination0.**
* Smallest: E 12 / P 18 / L 11. `capturepointprefab_nospawner` × 3 → `layer0_dom_logic_schematic` (VO/UI prefabs).
* `domination` prefab constants.
* The `deathmatch` sub-world toggles spawner sets per mode with `Bool` constants into `SubWorldReference…{Domination,
  GunMaster, SQDM, TDM}`.
* Level root: client-only fx `Sequence`s for ambient warfare.

**What gets switched (union over the seven)** [verified edges]:
* `CharacterSpawnReferenceObjectData.Enabled`;
* dynamic vehicle-spawner prefab `.Enabled` / spawn events;
* `VehicleSpawnReferenceObjectData.Enabled`;
* `AlternateSpawnEntityData.Enabled`;
* capture-point prefab `.Enabled` / `.AreaValue` / `InitialOwnerTeam`;
* combat-area and UI-combat-area volumes (links);
* `Predestruction.Enable`, `DamageArea`, `ReferenceObject.Destroy/Disable`;
* effects `Start`, sounds, cameras, lights;
* VO/UI prefabs; `Message`.

`TicketCounter`/`LifeCounter` live inside the shared mode prefabs, and levels reach them only through interface
constants (Metro's `100` lives, AS `InitialTicketCount`).

---

## 5. Realms, synced types and runtime semantics

1. VU documents split runtime classes **`Server*` / `Client*`** for every synced and gameplay type:
   * `ServerSyncedBoolEntity` (events `SetTrue`, `SetFalse`, `Toggle`, `OnTrue`, `OnFalse`), `ClientSyncedBoolEntity`;
   * `Server/ClientSyncedInt/Float/Transform/SequenceEntity`;
   * `ServerCapturePointEntity`, `ServerVehicleSpawnEntity`, `ServerTicketCounterEntity`, `ServerCombatAreaTriggerEntity`,
     etc.

   Pure logic classes (`AndEntity`, `DelayEntity`, `SequenceEntity`, `EventGateEntity`, `MathOpEntity` …) have **no**
   realm prefix [verified, `VU-Docs\content\vext\entities.md`, 384 classes]. So pure logic is instantiated in whatever
   realm its `Realm` field names, while synced and gameplay entities exist as a server/client pair [inferred].
2. `SyncedBoolEntityData` has only `inValue: bool` and no `Realm` field [verified, `VU-Docs\types\fb\SyncedBoolEntityData.yaml`].
   It is the designers' tool for state that both sides, and late joiners, must agree on. In every traced scripted event
   the server first sets a SyncedBool, and the client presentation then hangs off `OnTrue` (Metro park, Kharg LCAC,
   airstrike, floodlights) [verified pattern; purpose inferred].
3. MP level logic has **no `SyncedInt`/`SyncedFloat`**. **`SyncedTransform`** has 8 unique instances (48 weighted).
   **`SyncedSequence`** has 66 instances in all MP data [verified, critic census `critique-r1\logic-types.tsv`]: 62 in
   vehicle blueprints (Su-35 16, F-18 16, Su-25 8, A-10 6, F-35 4, ASRAD 4…), probably landing gear and canopy, and the
   rest in mobile-radar props (2 unique reachable). `VehicleBlueprint`s carry **7,188** connections of their own
   [verified, `bp.tsv`], which is an in-vehicle graph outside this gap's scope (§12).
4. Event semantics and Networked delivery, late-join replay and ordering are **not** documented by VU's event tables.
   They are inferred (§7).

---

## 6. Server-authoritative vs client-only presentation

[verified counts, function labels inferred]

* **Server-side** (per row, median): primitives 170 (max 1,036); edges 956 (max 3,898).
  * Level-owned server logic without destruction: median 56, mean 110, max 560.
  * Server destruction coupling (`OnDestroyed`/`Disable`/`Destroy` chains, RandomDelay collapses): level 65,381 +
    shared 109,536 weighted edges. **The M6 destruction work needs the same Delay/RandomDelay/EventSplitter/Sequence
    semantics anyway** (Bandar gas station, Talah pillar, Metro park).
* **Client-only**: sound areas/reverb/mixers (level 51,648 weighted edges), ambient warfare and fx sequences (11,848),
  cameras, client-only destruction FX (33,807), UI (shared 57,126 client edges).
  * Realm=Client nodes: Sequence 140, MathOp 410, Float 359, EventGate 136, Delay 137, RandomDelay 86.
  * None of this changes the authoritative state. It can be skipped at first, or run later by the same interpreter in
    the client realm.
* **Mixed**: ClientAndServer nodes (Delay 76, Sequence 78, Bool 106) run on both sides from a shared trigger. They
  converge only if their inputs are synced. The data guarantees this through SyncedBool in the traced cases.

---

## 7. Options compared

All LOC and slice figures in this section are [inferred] estimates.

| | **A. Whitelisted logic interpreter + native gameplay entities** | **B. Hand-written C++ rules + per-level data** |
|---|---|---|
| Scope | Instantiate blueprints from EBX (reference objects, prefab interface forwarding by hash, sub-world inclusion); run 48 server-capable primitive types; gameplay entities stay C++ classes that expose ports by hash | C++ `GameMode` classes (as now) + per-level C++/data for every scripted behaviour and every designer-named port |
| Code | Runtime core (graph build, port maps, event queue, property propagation, link lists, realm filter) ≈ 1.5–2.5 k LOC. Primitives: ≈ 30 trivial types × 20–60 = 1–1.5 k; Delay/RandomDelay/DelayTrigger/MultipleTrigger ≈ 300; **Sequence** (event tracks, property tracks, loop, reverse, external time) ≈ 400–600; Synced* + ghost ≈ 300. Port adapters for 42 gameplay types ≈ 1–2 k (needed anyway in reduced form). **Total ≈ 5–8 k LOC** | Mode classes are already planned. Per level: 5–15 server-side scripted behaviours (from the seven traces: Metro 2, Caspian 2, Kharg 6, Bandar 3, Nebandan 2) × 29 levels ≈ 150–400 behaviours × 50–100 LOC ≈ **15–40 k LOC**. Plus mapping thousands of unresolved interface hashes (§3.5) by hand. Plus re-implementing the shared prefabs' graphs (`full_rush_level` 435 edges, `full_ctf_prototype` 336, `conquest` 182, capture prefabs) as C++ |
| Fidelity | Runs DICE's own wiring, so behaviour comes from the data, including unknown hashes | Only what we notice and transcribe; silent omissions (e.g. Caspian jets tied to flags) |
| Determinism | Server only, fixed tick: FIFO event queue drained to a fixed point at one point of the tick (cap e.g. 10 k events/tick); stable order = connection array order; Delay in ticks (`ceil(delay × hz)`); RandomDelay/RandomEvent from `hash(root_seed, entity guid, fire count)`; properties propagate on change only. Same inputs → same trace, which is testable | Same, but per hand-written behaviour |
| Tick integration | Server step: commands → bots → sim → destruction → rules/entities (emit events) → **logic drain** → apply `Enabled`/spawns next tick (1-tick latency, 16.7 ms) → ghost encode. Cost: ≤ 1,036 server primitives, ≤ 3,898 edges/row → well under 0.1 ms/tick | Same slot, negligible cost |
| Replication | **Logic ghost**: SyncedBool bitset (max 259 bits/row = 33 B), SyncedTransform (≤ 4/row), start tick of ClientAndServer sequences gated by synced state. Delta on change (reliable ch1), full set on join (ch2). Networked events (≈ 9 % of E edges) → ch1 reliable `{connection index}` messages | Ad-hoc fields per behaviour in the world ghost |
| Late join | Client applies the bitset, fires `OnTrue` locally for presentation (policy to confirm, §12), and starts running sequences at `now − start_tick`. Networked one-shot events are not replayed | Hand-coded per behaviour |
| Risk | Unknown exact semantics (re-entrancy, `removeDuplicateEvents`, Sequence track timing) and debugging data-driven graphs | Scale and silent omissions; the M7 "remaining modes/levels" estimate is wrong by the per-level work |

**Recommendation: A, with the gameplay entities native.** The interpreter is small because the vocabulary is closed and
simple. It removes the need to name hashes, and it also serves M6 destruction chains. B's real cost is not the mode
rules, which stay native in A too, but the **per-level wiring**: 25,304 weighted level-owned server edges, about 110
per row on average.

---

## 8. Revised D12 row

| ID | area | options | recommendation | evidence | confidence | early validation |
|---|---|---|---|---|---|---|
| D12 | Game modes and level logic | (a) event-graph VM for all; (b) resolved constants + C++ rules (old); **(c) native gameplay entities (CapturePoint, Ticket/Life/Kill counters, Character/Vehicle/AlternateSpawn, CombatArea, RoundOver, Damage/ClearArea, Interaction) + deterministic server-side interpreter for 48 whitelisted primitive types + logic ghost** | (c); client-realm presentation graphs later through the same interpreter; unknown types are logged and treated as no-ops | This report §3–§7: level-owned 2,099/5,066 primitives and 44,234/58,593 connections; 7 traced rows all switch spawners/vehicles/flags/events from level schematics; 20 types = 94.7 %, 36 = 99 %; interface ports 7–51 % nameable | M–H | **S4** (§9.2): Metro RushLarge0 + Caspian CQL on the headless server; spawner/vehicle `Enabled` timelines and a deterministic event trace hash across 2 runs |

---

## 9. Changes to the master plan

### 9.1 §3.5 replication

* Add a **logic ghost** to the world ghost:
  * `SyncedBool` bitset indexed by (blueprint instance path, entity guid) in sorted order, max 259 bits per row
    [verified max];
  * `SyncedTransform` (≤ 4 per row) quantised like other transforms;
  * the start ticks of ClientAndServer `Sequence`s that hang off synced state;
  * on change, send rows `(index u16, value)` reliably on ch1; on join, send the full bitset on ch2 with the destruction
    rows.
* Add **networked events** as a ch1 reliable message `{tick, connection index u32}` for EventConnections with
  `TargetType` NetworkedClient/NetworkedClientAndServer (unique 1,639 edges) [inferred design].
* The handshake `sim_digest` should include a digest of the instantiated logic graph (types + connections), so a
  mismatched install also fails here [inferred].

### 9.2 §6 milestones

* **New spike S4 "level logic interpreter" (1–2 slices), after M1 and before M4.** Exit criteria:
  * Metro RushLarge0 loads its full graph headless;
  * the 8 Rush spawners open in base order when MCOMs are forced destroyed;
  * the park event fires 10 s after base 1;
  * two runs give an identical event-trace hash;
  * unknown-type count = 0 for Metro/Caspian;
  * fail → fall back to (b) for those rows only.
* **M2b** TDM runs through the interpreter. TDM rows have median 59 server primitives.
* **M4** Conquest: capture/vehicle gating comes from the graph (Caspian jets/tanks).
* **M6** destruction chains reuse the interpreter.
* **M7** "remaining modes" shrinks from per-mode rules + per-level scripting toward entity classes + data. Estimate
  25–45 → **22–40 slices**, plus the S4 cost moved earlier [inferred].
* §3.7 "C++ design" changes: `GameMode` subclasses become thin. `ModeConstants` resolution stays (the interpreter
  pushes the same PropertyConnections at instantiation).

---

## 10. Second-source checks of the five most load-bearing claims

1. **Rush spawners start `Enabled=False` and are opened by SyncedBool wiring.**
   * Re-checked against the raw JSON dump (`LL\tr2\levels__mp_subway__rush.json`), independently of `trace.py`: 8 ×
     `SyncedBool.Out(193453899 = H("Out"))` → `CharacterSpawnReferenceObjectData.Enabled` with initial
     `Enabled=False`.
   * Agrees with handoff `sections\04-gameplay-sim.md:51` [report claim].
   * Confirmed.
2. **Caspian jets are gated by a level schematic.**
   * Raw JSON `LL\tr2\levels__mp_007__conquest_large.json`: 4 × `SyncedBool.Out` → `ReferenceObjectData.Enabled` for
     `gameplay/level_setups/dynamic_vehiclespawners/dynamicspawn_{f18,su35}`.
   * Confirmed.
3. **Hash function.**
   * `C:\Users\olku\Rime\RimeLibLite\Frostbite\Utils.cs:13-29` (`fb::hashQuick`, basis 0x1505, prime 0x21, xor) and
     `C:\Users\olku\iw4L\crates\ebx_bf3\src\lib.rs:16-20`.
   * Confirmed.
4. **Counts of SyncedBool (634 total / 579 reachable) and 156 `*_schematic` level partitions with 1,302 instances.**
   * Re-counted from `critique-r1\logic-types.tsv` (634; 1,302 rows in 156 partitions) and from my own census
     (579 reachable).
   * The two censuses differ only in scope.
   * Confirmed.
5. **The `TargetType` and `Realm` enums.**
   * `VU-Docs\types\fb\EventConnectionTargetType.yaml` and `Realm.yaml`, against the enum names decoded by the dumper in
     `edges.tsv`.
   * Confirmed. The *meaning* of "Networked" remains [inferred].

---

## 11. Web search (primary sources)

* No DICE/Frostbite talk on Schematics, event connections or realms was found.
* An EA job posting describes the Schematics team as owning Frostbite's "data-driven visual scripting engine"
  [verified page text via https://hitmarker.net/jobs/electronic-arts-software-engineer-frostbite-schematics-869288].
  The EA original at https://ea.gr8people.com/jobs/174282/software-engineer-frostbite-schematics returned 403.
* GDC 2015 "Frostbite: Implementing a Scripting Solution for Your Editor"
  (https://gdcvault.com/play/1021996/Frostbite-Implementing-a-Scripting-Solution) is about editor tooling (FBScript),
  not runtime graphs [report claim from search summary].
* The GDC 2007 rendering talk covers shader graphs only.
* Conclusion: runtime semantics must come from data and VU observation, not from DICE documents.

---

## 12. Could not determine

* Exact runtime semantics:
  * event re-entrancy and ordering;
  * `removeDuplicateEvents`;
  * whether property changes propagate the same tick;
  * Sequence event timing at track boundaries;
  * `PropertyGate`/`PropertyCast` edge cases;
  * `CompareEntity` with no edges.

  These need VU observation (knowledge only) or a careful reading of the data.
* Whether a late-joining BF3 client fires `OnTrue` for SyncedBools already true, and whether ClientAndServer Sequences
  resume mid-way.
* What "Networked" event delivery means exactly (reliability, ordering, whether the server-side target also fires).
* The names and meanings of designer-named interface hashes such as #963802567 (inferred "base fell"), #433417572,
  #−2055381215 and #2088939021. 49–93 % of prefab and interface ports are unresolved.
* The in-vehicle graphs (57 `VehicleBlueprint`s, 7,188 connections, SyncedSequence on jets) and soldier blueprints (389
  connections) were not analysed.
* Scavenger weapon-pickup logic and Gun Master progression wiring were not traced.
* The function classification (destruction/sound/etc.) and the server/client side of P/L edges are heuristics. The
  numbers in §3.2 and §6 are ±10–20 % on those splits; the raw counts are exact.
* The LOC and slice estimates in §7–§9 are unmeasured.
