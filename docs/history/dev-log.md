# openBF3 development log

The milestone-by-milestone record that used to be the second half of README.md: what each step (M1, the S1-S4 and L1
spikes, ui1/ui2, M2a and its waves, M2b, Playtest 1) built, how it was measured and what was merged, in the order it
happened. It is history, kept for the numbers and the decisions it records; the system as it stands is described in
[docs/architecture.md](../architecture.md), and the per-track designs are in [docs/design/](../design/).
`tools/play.cmd` and the design documents that cite a README section by one of the headings below mean this file.

## M1 static world (T1)

The collision soup (above) becomes Jolt shapes: one `MeshShape` per 64 m tile and layer (Collision, Raycast,
Character), the triangle's `SurfaceTag` (material, surface, body, flags) in its per-triangle user data. The shapes
are built once per process (`StaticWorldShapes`, immutable, shared) and every `CollisionWorld` (server, client)
puts one static body per shape into its own `PhysicsSystem`. Design: docs/design/m1.md §5.

```
bf3dump shapes MP_Subway TeamDeathMatch0 [--cache=<dir> | --no-cache] [--threads=N] [--no-raycast] [--tiles]
bf3dump prepare MP_Subway TeamDeathMatch0 --shapes      # meshes + weapons, then the shapes, into the same root
```

Both print the cache result, the per-phase times, the shape counts and bytes, the peak working set and an
`install.unchanged` row (exit 1 if the install changed). `--tiles` lists every tile (x, z, triangles per layer,
tags, content digest).

Cache entries (in the derived-data cache root, never in the repo):
* `jolt-tile`: one tile's tag table and the `Shape::SaveBinaryState` stream of each layer's MeshShape, keyed by a
  SHA-256 of the tile's input triangles, so modes that share a tile share the entry (Metro ConquestLarge0 after
  TeamDeathMatch0: 160 of 170 tiles hit). Every stream's Jolt tree is walked and checked before Jolt restores it.
* `jolt-world`: the tile list of a level × mode, keyed by the source digest (SHA-256 over the SHA1s of every
  manifest entry in the bundles the mode mounts) and `kCollisionSoupVersion`. A warm load reads it and restores the
  tiles without building the soup.

Measured on Metro TDM (dev build, 16 threads, 2026-10-15; timings noisy, other builds ran in parallel):

| | |
|---|---|
| shapes | 170 tiles, 291 MeshShapes (= bodies per CollisionWorld), 1,903 tags; 969,031 collision, 142,908 raycast, 2,232 character triangles |
| resident shapes | 22.55 MiB (21.2 B per triangle; the design estimated ≈ 14 MiB) |
| cache entries | 22.6 MiB in 171 entries; soup (cold only, freed) 51.0 MiB |
| cold | 0.66–0.70 s: soup 0.37 s, cook + cache writes 0.26–0.30 s (0.12 s with `--no-cache`) |
| warm | 0.035–0.038 s: source digest ≈ 25 ms (4 bundles, 21,030 entries), restore 0.011 s, no soup |
| `prepare --shapes` peak (E6) | cold 332–338 MiB (≤ 512 MiB), warm 199.6 MiB with 1,134 of 1,134 hits |
| soup sources outside the digest | 0 (155 EBX, 121 res, 10 chunks) |

Tests: `sim.world.unit` (synthetic worlds; tile and world blob round trips, hostile blobs and 2,000 mutated Jolt
trees, cache keys, cold / warm / damaged / closed-cache builds), `sim.world` (Metro from the install into a scratch
root: cold versus warm byte-identical and answering 1,000 rays alike, the source coverage, rays, capsule and box
overlaps and capsule sweeps against a brute-force double-precision reference over the soup triangles,
`install.unchanged`), `tools.m1_prepare` (E6 and an all-hits second run). The parity case on Metro: 1,728 ray hits
and 272 misses with the reference, the tag equal on all 1,523 distinct hit triangles; capsule overlaps 785 / 214;
box overlaps 366 / 104; sweeps 322 hits, 78 clear, the time of impact inside 3 mm; 0 disagreements. Samples too close
to call are counted as ambiguous and not judged: a capsule within 2 mm of touching (1), a box within 4 cm (30;
Jolt rounds the box's corners by its convex radius), a ray with several triangles in reach.

## M1 soldier motor (T2)

<!-- M1 T2 -->
`bf3_sim` steps one soldier per call: `SoldierMotor::step(state, command)` on a Jolt `CharacterVirtual` that is reset
from the POD `SoldierState` every tick, so a step depends only on (state, command, static world, params) and a
client replay equals the server's step bit for bit (docs/design/m1.md §6). The numbers are Metro's soldier
(`read_soldier_def` + `make_motor_params`); the gain semantics nobody has measured yet live in
`bf3-opensource/config/motor_tuning.cfg` (19 keys, all `[inferred]` defaults; `parse_motor_tuning` rejects
unknown keys, duplicates and out-of-range values and names the line). Also in this track: `spawn.h`
(`settle_spawn` in three pieces for the server's sweep, `stamp_spawn`, `choose_spawn`, and the deterministic test
courses `find_clear_heading` / `find_drop_spot`), `prediction.h` (`PredictionRing`, `sim::catch_up`, the one rule
ClientSession uses for reconciliation and the base fill), `soldier_state_hash`, and `bf3_core`'s
`read_level_spawns` (AlternateSpawn points and CharacterSpawnReference spawners with PropertyConnection values).

`bf3dump spawns <LEVEL> <MODE>` prints the spawn points (index, object, position, the yaw the server spawns with,
team, enabled, priority, entity) and the spawners as TSV. Metro TDM: 102 neutral points at y 59.8-73.4 and the two
spawners #6 (Team2) and #7 (Team1), radius 10, 3 friendly kills in 30 s, in 0.18 s.

Tests: `sim.motor.unit` (no data) and `sim.motor` (install; skipped without one).

* `sim.motor.unit`, synthetic worlds: E1a walk 4.0000 / sprint 6.5 / crouch 2.0 / prone 0.9 m/s within 1 % per tick
  (the default gains reach 99 % by tick 22, sprint by 33), direction factors and diagonals, deceleration; E1b apex
  0.599999 m (the penalised second jump 0.1797 m); E1c 18 / 42 / 18 / 30 / 42 / 63 ticks; E1d 9.8 m → 13.86636 m/s,
  0 hp; 10.1 m → 14.07700 m/s, 20.192 hp; 46 m → 30.04195 m/s, dead; ramps 49° / 51°, a 10° ramp at full speed,
  step limit 0.42 m, wall pushes without jitter, ledges (0.4 m hugged, 0.6 m InAir), a 55° slide without damage, a
  ceiling jump, stand-blocked, the prone footprint, floor cracks; determinism over 10,000 busy steps on two worlds
  and in a child process (chain `6e8ede9353e499c8`), save / restore, 8 interleaved soldiers, hostile states;
  `catch_up` from a mismatch, forward and backward resets, a 3 s stall and a spawn against the server's own
  stepping; settle, `choose_spawn`, the course searches; the shipped cfg, its round trip and a parser fuzz.
* `sim.motor`, Metro's real static world (soup → `build_static_world`): `make_motor_params` from the level's
  soldier has the same `motor_params_hash` as the unit tests' synthetic soldier; `settle_spawn` 102 / 102 ok (worst
  lift −0.18 m, shift 0.4 m, gap ≤ 4.5 mm, 0.05 s); E1a-E1c along `find_clear_heading` courses from 6 spawn points
  (window means within 0.5 %, apex 0.599998 m, transition ticks exact); E1d at `find_drop_spot`
  (−526.3, 62.5, −758.5): the same three impacts as above, fall = h within 1e-5 m; the game's path, T1's
  `load_static_world_cached` cold (170 tiles cooked) and warm (170 restored) on a scratch cache, gives the motor the
  direct build's answers bit for bit (all 102 settles, every course heading, the drop spot, 9 sprint traces);
  install unchanged. About 2 s in `dev`, 70 s under `asan`.

Known limit (docs §6.1 "As built in T2"): where two floor meshes meet unwelded, Jolt's character padding meets the
far mesh's edge and lifts a walker 0.5-14 mm for a tick, costing up to 3.5 % of the speed for about half a second.
Metro's E1a therefore holds the window mean to ±1 % and each tick to ≥ 95 %.

## M1 session (T4)

<!-- M1 T4 -->
`bf3_host` runs the M1 game: `GameServer` inside `ServerHost`'s 60 Hz thread (`ServerConfig::game`; the S0 world is
untouched without it) and `ClientSession` on its own thread, both Godot-free (docs/design/m1.md §7-§9, §12). The
server keeps a 32-tick jitter buffer per slot (a newer sequence replaces a buffered tick, the buffer report follows
the highest sequence), holds a new slot on the spawn's hold command, applies Teleport / SpawnAt / spawns in the rules
step stamped at the tick, records `Trace` reports, runs `SpawnSweep` in batches of the free slots, and sends and
flushes the tick's datagrams in the tick. The client runs the command clock (target buffer, slew, reset with a
hold-off, forced forward), predicts every tick from the ring's entry at C - 1, reconciles and fills gaps through
`sim::catch_up`, smooths corrections (IW4L's 100 ms decay), interpolates the others on the received server time and
publishes a fixed-size `PresentationFrame`; `present_others` / `present_own_fraction` evaluate it per display frame.
`judge_rest` (rest_check.h) is the one E2 judge. `level_load.h` loads the level database and the `SharedLevel`
(spawns, soldier, static world through the cache).

Tests: `host.session.unit` (no data, about 70 s; its 4 socket-free cases run on every branch as
`host.session.unit.pure`) and `host.session` (install; skipped without one), split by case into `host.session.gate`
(load and E2 150/2), `host.session.stress` and `host.session.e3` (about 96 / 96 / 4 s), labelled `realtime`: they run
side by side in the merge pass's real-time lane (docs/testing.md). `host.session.unit` is labelled `timing` and runs
alone (beside the lane's shards it needed reruns).

* `host.session.unit`: `judge_rest` on synthetic recordings; `present_others` brackets, extrapolates 250 ms and
  holds; `GameServer` without sockets (hold command, late / repeated / replaced counts, the buffer report, Teleport
  and SpawnAt stamped at the rules tick, Trace, refused dev commands, a 20,000-datagram fuzz) and its SpawnSweep in
  3 batches equal to headless rows; then real ENet loopback on a synthetic floor: spawn with 0 reconciliations, the
  spawn view, 238 / 238 predicted states equal the server's Trace bit for bit, odd-tick events and a superseded
  snapshot's events reach `server_events`, RTT 1.25 ms (bound: 1 tick + 2 ms), clock target 2; clock disturbances
  of -20 / +20 ticks reset exactly once each (+20: 5-6 commands replaced on the server, the forward jump filled at
  once), a 3 s stall and a start 30 ticks behind recover with 0 precondition failures; a 0.08 m Teleport is
  reconciled once and smoothed to 0 in 350 ms; E2 under "150/2" (shortened: 12 s idle, 2 cycles) 3 / 3 rest periods
  ok, RTT 156 ms against 150 ms netsim, extrapolated 0.07 % of frames.
* `host.session`, Metro TDM through `load_shared_level` and the shipped tuning: cold 0.89 s / warm 0.22 s on a
  scratch cache; E2 under "150/2" (the gate: 30 s idle + 4 walk-stop cycles along a `find_clear_heading` course):
  5 / 5 rest periods ok, 0 reconciliations, RTT 157 ms; "lat=150-165,loss=2" (stress row, reported): 5 / 5 ok, RTT
  322 ms; `SpawnSweep` with 128 slots 102 / 102 ok and equal to in-process `settle_spawn` rows; install unchanged.

## M1 game (T6)

<!-- M1 T6 -->
`res://scenes/m1_metro.tscn` is M1: Metro TDM walkable through the loopback server. Its `Bf3LevelSession` node
(bf3_godot, docs/design/m1.md §11, §12) loads the level on a loader thread (mount, scene plan, `SceneLanes`, the
`SharedLevel` through the cache), starts a `ServerHost` with the `GameServer` (2 scripted walkers) and a
`ClientSession` as soon as the shapes are in, and uploads the scene behind a loading screen: RenderingServer meshes
and textures on Godot worker tasks (at most 3 in flight; the pool is capped at 4 threads in `project.godot`),
materials and 10,506 RS instances on the main thread in 12 ms slices with vsync off. The first-person camera blends
the client's presentation at the display frame's time; the mapper uses BF3's PC keys and adopts each life's spawn
view. Headless (`--headless` or `--m1-check`) the session skips the scene and loads only what the server needs.

```bat
:: play (WASD move, Shift sprint, Space jump, Ctrl crouch toggle, Z prone toggle, mouse look,
:: F6 free camera with Q/E down/up and the wheel for speed, Esc releases the mouse)
"%GODOT%" --path bf3-opensource res://scenes/m1_metro.tscn [-- --netsim=150/2 --walkers=2 --bf3-cache=<dir>]
:: E1 / E2 / E3 headless: PASS/FAIL table, exit 0 or 1 (about 6 minutes)
"%GODOT%" --headless --path bf3-opensource res://scenes/m1_metro.tscn -- --m1-check [--bf3-cache=<scratch dir>]
:: E4 / E5 windowed: run cold on an empty cache root, then warm on the same root
"%GODOT%" --path bf3-opensource res://scenes/m1_metro.tscn -- --m1-load-check=cold --bf3-cache=<scratch dir>
"%GODOT%" --path bf3-opensource res://scenes/m1_metro.tscn -- --m1-load-check=warm --bf3-cache=<scratch dir>
:: screenshots after the load: spawn points (stand / walk / crouch / prone) and free-camera views
"%GODOT%" --path bf3-opensource res://scenes/m1_metro.tscn -- --shot-spawns=22:prone,13:walk --shot-dir=C:\temp\m1 --shots=C:\temp\m1\over.png@-150,260,-800,0,-40 --quit-after=1
```

Tests: the headless `--m1-check` as three shards (`--m1-phase=e1|e2_gate|e2_stress`): `godot.m1.e1`,
`godot.m1.e2_gate` and `godot.m1.e2_stress` (about 165 / 95 / 95 s side by side in the real-time lane, instead of one
6-minute `godot.m1`), and `godot.m1_load` (windowed `--m1-load-check` cold then warm on a fresh root under `%TEMP%`,
8 spawn shots each under `b\<preset>\shots\m1_load-<tag>`; exit 77 = skipped without a window; label `window`,
`RUN_SERIAL`). All need `OPENBF3_GODOT` and an install.

Measured on Metro TDM (dev build, Radeon RX 9070 XT, D3D12; other tracks were building on the machine, so the
timings are noisy):

| criterion | result |
|---|---|
| E1a speeds (6 courses through the server's Trace) | walk 3.9577–4.0000 m/s, sprint 6.2727–6.5000, crouch 2.0000, prone 0.9000; worst window mean error 0.45 % (sprint) |
| E1b jump apex | 0.599998 m on 6 / 6 courses |
| E1c pose transitions | S→C 18, C→P 30, P→S 42, S→P 42, P→C 63, C→S 18 ticks on 6 / 6 courses |
| E1d drops | 9.8 m: 0 damage; 10.1 m: 20.19 damage; 46 m: dead; fall within 1 mm, impact speed within 1e-5 m/s of √(2gh) |
| E1 snapshots | 3,985 / 3,985 own states in the snapshots equal the server's Trace bit for bit |
| E2 "150/2" (gate) | 5 / 5 rest periods ok, 0 reconciliations, eye jitter 0 m, RTT 157 ms, 12,960 samples |
| E2 "lat=150-165,loss=2" (stress, reported) | 5 / 5 ok, 0 reconciliations, RTT 322 ms, 0.03 % of the others' frames extrapolated |
| input keys (device path, `input.keys`) | 48 / 48: Space / Ctrl / Z / F6 fire with no modifier, Shift, Ctrl and Shift+Ctrl held (sprint-jump, sprint-crouch); echoes and releases do nothing |
| E3 spawns (`SpawnSweep`, 128 slots) | 102 / 102 ok and equal to in-process `settle_spawn`, 1.0 s |
| E4 cold (process start → first controllable frame) | 2.26–2.33 s (`_ready` → Done 1.61–1.69 s); gate 15 s |
| E5 warm | 1.49 s (`_ready` → Done 0.84–0.85 s); gate 5 s |
| E6 converter peak (`tools.m1_prepare`, T1) | 332–338 MiB cold; gate 512 MiB |
| game peak working set (reported, not gated) | 851–862 MiB cold, 947–959 MiB warm (windowed, D3D12); headless check 426–435 MiB |
| frame time after Done (5 s at the spawn) | p50 5.56 ms, p99 8.33 ms |
| install | unchanged after every run (256 files, sizes and mtimes) |

Load phases (cold / warm): mount 0.16 / 0.16 s, shapes 1.20 / 0.11 s, scene lanes wall 0.90 / 0.26 s (604 mesh
blobs converted / hit), C1 198 / 103 worker tasks with 0.82 / 0.65 s of work (largest task 27 ms), C2 0.036 /
0.030 s of main-thread work, 670 / 605 loading frames at 2.4 / 1.4 ms mean. The very first cold run after a build
measured 4.8 s (the OS file cache was colder).

Known gaps of the minimal look: about half of Metro's materials name no texture and draw flat (T5 notes); the
terrain is one flat colour; the plan's depth fog is off by default (`--fog` draws it) because the park's fog colour
floods the frame; shadowed and indoor surfaces take a strong blue cast from the sky-only ambient.

## H hygiene follow-ups

<!-- H hygiene -->
Four follow-ups from M0/M1. Measured 2026-10-15 on the Ryzen 7 5800X while five other tracks built and tested in
parallel, so every timing here is noisy; the memory figures varied by under 1 %.

**stop_all registries (`bf3_host`).** `ServerHost::stop_all`, `LoopbackClient::stop_all` and `ClientSession::stop_all`
(the extension deinit's safety net, register_types.cpp) copied the pointer list under the registry mutex and called
`stop()` through the copies after releasing it. An owner destroying its instance in between left `stop_all` with a
dangling pointer: ASan reports a heap use after free in `stop()`'s lifecycle mutex. `InstanceRegistry<T>`
(`src/bf3_host/src/instance_registry.h`) now pins every instance a pass takes, under the same mutex. The destructor
calls `retire()` first, which waits until no pass holds a pin. A pass skips an instance that was retired or stopped
before it got there. `host.registry` (no data, about 5 s, also under `asan`) checks the rule on `InstanceRegistry`
itself (a destructor waits while `stop_all` is inside its instance; a retired instance is skipped, not called), then
churns 160 ServerHosts, 160 LoopbackClients and 96 connected ClientSessions against a thread calling `stop_all` in a
loop. Under `asan` the old code fails it: ServerHost 3 of 3 runs, LoopbackClient 2 of 2, ClientSession 1 of 2 (at 48
sessions). The fix passes 3 of 3 runs.

**EbxLibrary memory (`bf3_core` ebx).** `bf3dump ebxload <sb>` now prints where the memory goes (`partition_heap`:
vector capacities, from the new `EbxPartition::memory_use()`) and the working set after the mount and catalog, after
`load_all` and at its peak. Two causes stood out:
* The tables derived from the descriptors alone (field and type descriptors, lookup tables, enumerators) took
  173 MiB, but `@all-mp`'s 50,486 partitions have only 2,533 distinct descriptor layouts.
* The import table, 2.38 M imports, was copied out of bytes the partition already keeps (72.6 MiB).

The changes:
* The descriptor-derived tables are interned process-wide (`EbxPartition::TypeTables`, `EbxTypeIntern` in ebx.cpp).
  The key is the three counts that place the tables plus the bytes from the name table to the end of the type
  descriptors. Keys are hashed with the per-process seed and compared in full. A partition whose layout is already
  loaded adopts that copy and skips building it. The last partition of a layout frees it. `ebx_live_type_layouts()`
  counts the live layouts.
* Imports are read in place: an `EbxImport` is two 16-byte raw GUIDs, the file's own records.
* The instance list is reserved (bounded by the payload) and the string NUL index is shrunk.

The public API only gained `memory_use()` and `ebx_live_type_layouts()`; `field_descs()`, `complex_descs()`,
`imports()` and `lookup()` return the same types.

| `bf3dump ebxload` | before | after |
|---|---|---|
| `@all-mp` peak working set (3 runs) | 903-908 MiB | **678-680 MiB** (the Rust oracle: about 662 MiB) |
| Metro (`Win32/Levels/MP_Subway/MP_Subway`) peak (3 runs) | 166-168 MiB | **127-128 MiB** |
| `@all-mp` after the mount and catalog (LevelMounts, not changed here) | 261-264 MiB | 261-263 MiB |
| `@all-mp` partition heap | bytes 306.4, descriptors + imports 168.5, lookups 77.4, instances 36.5, strings 2.3 (slack 16.2 of these), objects 14.3 MiB; 439,017 blocks | bytes 306.4, type tables 39.8 (2,533 layouts, 15.9 of it the keys), instances 33.4, strings 1.9, objects 13.5 MiB; 232,153 blocks |
| `@all-mp` parse (all cores) | 0.31-0.33 s | 0.29-0.34 s |
| `ebxload @all-mp --lookups`: `get()` by name of all 4,936,637 top-level fields, one thread, best of 3 | 0.447-0.449 s | 0.408-0.414 s |
| Metro, the same | 0.111-0.113 s | 0.106-0.110 s |

Gate: `ctest -R tools.ebx_memory` (or `cmake -DBF3DUMP=b\dev\tools\bf3dump\bf3dump.exe -P tests\ebx_memory.cmake`).
It passes when every partition parses and the peak working set is at most **760 MiB for `@all-mp`** and **145 MiB for
Metro**. It is RUN_SERIAL, report-only under `asan`, and 77 without an install. The old binary fails it (907.8 and
166.3 MiB).

Lookups are not gated (noisy timing). Rerun `bf3dump ebxload @all-mp --lookups` on a quiet machine; it should stay
within a few percent of 0.41 s.

Parity: the old and new `bf3dump` binaries print byte-identical output for:
* `find @all-mp m16a4 shield`
* `types @all-mp SoldierWeaponUnlockAsset MeshVariationDatabase LevelData`
* `dump @all-mp <dir> 3 weapons,characters` of `u_m16_burst`, `mpsoldier` and the Metro level (32 MB of JSON)
* `level MP_Subway TeamDeathMatch0` and `level MP_007 ConquestSmall0`
* `weapons`
* `kits MP_001 ConquestLarge0 --all`
* `text --census @all-mp`

`core.formats` adds two cases:
* partitions of one layout share one copy and free it with the last one, a layout error is still found while a good
  look-alike is alive, and a moved partition keeps its views;
* 8 threads parse two layouts at once and agree on one copy each.

`core.formats`, `core.install`, `core.fuzz` (also at 2,000 mutants per input), `core.level.unit`, `core.ui.text` and
`core.gameplay.inspect` pass under `asan`.

**Text database corrections (research/).** Checked with `bf3dump text --census`: the research set's 1,271 SIDs give
1,247 resolved, `missing_marker=0`, `absent=24`. Of the 24, 18 are vehicle-weapon names `ID_P_VWNAME_*`; the others
are `ID_H_COMBAT_AREA_`, three `ID_M_MOD_*_DESC`, `ID_P_VDESC_BARSUK` and `ID_WEAPON_CATEGORYGADGET1`. None of the
15,515 entries holds `<MISSING>`; the only texts containing "MISSING" are two "THIS SECTION OF THE LEVEL IS MISSING"
lines. `bf3dump text ID_P_WNAME_M16` gives `85119825  M16A4`, while `ID_P_VNAME_M1A2` and the `ID_P_MAP_*` guesses
are absent. `research/PORTING-RESEARCH.md` §3.8 and `topics/gap-r1-ui-hud-minimap-localisation.md` are corrected; this
README's "Text database" already said so.

**Caspian §9 counts.** The handoff count is reproduced exactly and attributed; see "Level traversal". The oracle is
`level-oracle static-models MP_007 ConquestSmall0`.

## S4 level logic (spike)

Design: [docs/design/s4.md](docs/design/s4.md). The level's own EBX wiring (blueprints, prefab interfaces flattened
by port hash, sub-worlds by GameMode) becomes an immutable `LogicGraph` (`bf3_core`, `gameplay/logic_graph.h`),
run server-side by a deterministic FIFO interpreter (`bf3_sim`, `sim/logic/logic_runtime.h`) over the 48
server-capable primitive types, with gameplay entities kept native behind port bindings (`sim/logic/logic_native.h`).
The type table (`gameplay/logic_types.h`) classifies all 130 entity types found on the connections of the 230
level x mode rows: 54 primitive (34 in the S4 slice), 36 native (25 in S4), 35 passive, 5 data, plus 4 structural.

Gates (each one ctest, run as `ctest --preset dev -R "^logic\.metro$"` and so on; install tests exit 77 = skipped
without an install, found as in "Install location" above; timing is wall clock on a shared machine, so only
percentiles and generous maxima are gated):

| gate | ctest | pass threshold | measured 2026-10-15 |
|---|---|---|---|
| G1 Metro RushLarge0 full graph | `logic.metro` | builds; 549 scopes, 7,299 objects, 145 SyncedBool, 0 unknown / unimplemented connected nodes, 0 unbound event edges, work <= 1 M | met: 549 / 7,299 / 145, 0 / 0, 0 unbound event edges, 4,841 params equal `LogicWorld::effective`, work 95,181, build 13 ms |
| G2 Rush spawners in base order | `logic.metro` | 8 spawners open as 4 Team1+Team2 groups, group g between base g-1's and base g's fall | met: bases fall at 3,661 / 9,067 / 14,473 / 19,879, groups open at 62 / 3,668 / 9,074 / 14,480 and close 7 ticks after their base's fall (3,668 / 9,074 / 14,480 / 19,886; the judge's bound is fall + 1,802) |
| G3 park event | `logic.metro` | park SyncedBool SetTrue exactly 600 ticks after base 1 falls | met: SetTrue at 4,261 = 3,661 + 600 |
| G4 determinism | `logic.metro` | 2 in-process runs and 1 `bf3dump logic trace` run: equal trace hashes | met: 843761c3205e74bb, 11,386 records, all three |
| G5 0 unknown types | `logic.types` | 14 Metro + Caspian rows: 0 unknown, 0 unimplemented connected server nodes (unconnected ones are info) | met on all 14 rows |
| G6 Caspian CQL timeline | `logic.caspian` | the 10 spawners of docs/design/s4.md §9.2 (6 capture-gated, 4 constant jets) have the table's Enabled values at ticks 1/601/1,201/1,801/2,401 and no other change | met: 10 of 10, 0 stray changes |
| G7 cost | `logic.perf` | p99 <= 100 us over all ticks, p99 <= 500 us over busy ticks, max <= 20 ms incl. tick 0, < 10,000 deliveries and no carry-over per tick | met (noisy, shared machine): p99 0.3 us, busy p99 12.1 us, max 194 us (tick 0), 418 deliveries max, 0 carry-over |
| I1 census (info) | `logic.census` | all 230 rows listed | 230 of 230 build and run; 0 connected unknown types anywhere; unimplemented connected types on 96 rows (25 types) |

One command per gate: `ctest --preset dev -R "^logic\.metro$"` (G1-G4), `-R "^logic\.types$"` (G5),
`-R "^logic\.caspian$"` (G6), `-R "^logic\.perf$"` (G7), `-R "^logic\.census$"` (I1); the same numbers from
`bf3dump logic stats MP_Subway RushLarge0`, `bf3dump logic run MP_Subway RushLarge0 --script rush` (G2/G3 verdicts),
`bf3dump logic trace MP_Subway RushLarge0 --script rush` (G4 hash), `bf3dump logic run MP_007 ConquestLarge0
--script conquest` (G6) and `bf3dump logic census [LEVEL...] [--out FILE]` (I1/G5 TSV). `logic.unit` has a Builder-graph case
with exact ticks for every primitive row of docs/design/s4.md §6.2; `logic.fuzz` is meant for `ctest --preset asan`. Full results, the as-built semantics (round start forced at tick 1, silent init settle) and
the fallback rule are in docs/design/s4.md §1.1, §7, §9.1 and §14.

## S3 crowd (spike)

<!-- S3 crowd -->
Spike S3 (docs/design/s3.md): bf3_anim drives 64 third-person soldiers (32 US, 32 RU) on Metro with decoded
`b_basicassetsmp` clips. A run blend over idle and four direction cycles carries a masked upper-body reload (each
weapon's clip through `3P_Wep_Reload_NoAddon_Stand_Chooser`) and an additive aim. Temporal LOD tiers come from
`GameAnimationSettings`; poses are evaluated on persistent worker lanes (`bf3/core/worker_group.h`) and the skin
palettes are uploaded on the main thread. Status: implemented (scene `res://scenes/s3_crowd.tscn`, node
`Bf3Crowd`); the measured results are below and in docs/design/s3.md §18.

Gates (one command; a 2560x1440 primary screen, windowed; the machine is shared, so timings are noisy and a failed
timing gate is repeated once with both runs reported; ctest TIMEOUT 2000 s covers both runs):

```bat
set OPENBF3_GODOT=C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe
ctest --preset dev -R godot.s3_crowd -V
:: = "%OPENBF3_GODOT%" --path bf3-opensource res://scenes/s3_crowd.tscn --
::     --s3-check --walkers=0 --no-hud --bf3-cache=<fresh scratch root> --quit-after=600   (process timeout 900 s)
```

| id | threshold |
|---|---|
| G0 | validity, else the check FAILs without measuring: the fixed Metro stage is clear (floor, no overlap, unoccluded rays); 64 of 64 soldiers inside the camera frustum on every sampled frame; walkers 0, HUD off, thread model Safe, cycle-timer self-test; run blend exercised (two-cycle >= 25 %, idle mix >= 1 %, >= 1.25 locomotion clips per soldier-frame) |
| G1a | step wall time (wait, upload, inputs, kick) p50 and p99 <= 2.0 ms, 64 soldiers in view, temporal LOD off, vsync off, >= 30 s |
| G1b | p50(G1a) + hidden main-thread CPU (animated minus frozen windows interleaved in one run, step's own CPU excluded, `QueryThreadCycleTime`) <= 2.0 ms |
| G2 | frame time p50 and p99 <= 16.7 ms, vsync off and vsync on, >= 30 s each |
| G3 | 16 shoulder close-ups (US / RU x idle / run / reload / aim x 2 views) in `b\shots\s3\`, each with the shoulder joint on screen and unoccluded, plus pinch and hand-IK metrics and one stage overview; the user judges the pinch |

Screenshots of BF3 content go to `b\shots\s3\` (gitignored) and are never committed. The wrapper writes them to
`%OPENBF3_S3_SHOT_DIR%` when set, else `<checkout>\b\shots\s3`. Interactive run (no gates):
`"%OPENBF3_GODOT%" --path bf3-opensource res://scenes/s3_crowd.tscn -- --walkers=0 --no-hud [--s3-backend=rs|skeleton3d]
[--s3-lod=on|off]`; `--s3-check --s3-diag` adds a roots-only run that splits the hidden cost.

Results (2026-10-15, `ctest --preset dev -R godot.s3_crowd -V`; the machine was shared with other tracks at 50-100 %
CPU, so all timings are noisy; the repeat run decides):

| id | measured (first run / repeat) | result |
|---|---|---|
| G0 | stage clear, 64 / 64 in view, config true, blend 43.7 % / 4.1 % / 1.48 | PASS |
| G1a | step p50 0.315 / 0.306 ms, p99 0.605 / 0.453 ms (backend B, RS skeleton) | PASS |
| G1b | 0.315 + 1.597 = 1.912 ms / 0.306 + 2.048 = 2.354 ms | FAIL (on the threshold within the noise) |
| G2-off | frame p50 12.48 / 9.74 ms, p99 35.47 / 20.07 ms | FAIL (p99) |
| G2-on | frame p50 11.69 / 10.58 ms, p99 28.73 / 21.23 ms | FAIL (p99) |
| G3 | 16 / 16 shots with pinch (min ratio 0.35-0.65, at most 13 of 107 vertices < 0.7) and hand-IK (<= 9.9 mm) metrics | PASS (evidence; the user judges) |
| install | unchanged | PASS |

The pose job takes 0.27 ms on 6 worker lanes (1.02 ms on one thread) and never blocks the main thread. Backend B
(one RS skeleton per soldier) beats A (Skeleton3D) in every bench (2.70 against 4.90 ms in the deciding run). About
2 ms per frame of Godot's own work for 64 changed skeletons (0.22 ms when only the roots move) is what misses G1b. The
level alone (R0) already has a frame p99 of 14.9-18.4 ms here; the crowd adds 3-6 ms p50. The next levers (shadows
off for the crowd, fewer surfaces, backend C, temporal LOD on, a quiet-machine rerun) are the user's call (s3.md §16,
§18). Rig ops are not needed for twist (the roll joints are keyed); see s3.md §9.

Accepted on a quiet machine (medians of 3): every gate PASS, backend B (G1a p99 0.38 ms, G1b 1.82 ms, frame p99
10.4 ms); the integrated rerun is in "Integration of S1-S4 and L1".

## L1 look (Metro materials and lighting)

<!-- L1 look -->
L1 is the look `res://scenes/m1_metro.tscn` draws by default: every drawn material gets the colour and normal map
BF3's shader graph samples, and the level is lit from its VisualEnvironment EBX. `-- --look=m1` draws the M1 look
(for before / after shots and the frame-time A/B). Code: `bf3_render` `shader_database.h` (the level shader
database reader), `look_materials.h` (which textures a material draws, and how), `look_environment.h` (the
environment read, per-camera blend, fog fit, grade LUTs), `level_scene.cpp` (plans L1 unless
`ScenePlanOptions::look` says M1); `bf3_godot` `level_upload.cpp` (six L1 material shaders) and `look_bridge.cpp`
(sun, sky, ambient, fog, exposure, LUT and the opt-in glow, updated each frame from the camera position). The
selection and material rules are ported from IW4L `level_bf3` / `resource_bf3` (the user's own Apache-2.0 code).

**Why about half of Metro's materials drew flat.** The MeshVariationDatabase entry (and the MeshMaterial) of those
slots lists no TextureParameters at all: their textures are constants of the surface shader graph, which only the
level's compiled shader database names (`levels/mp_subway/mp_subway/shaderdb`, res type `0x36F3F2C0`, 28.8 MB, 640
surface shaders on the first render path). The L1 resolver ranks candidates per role in three tiers: the slot's
parameters (by name, sampled on the UV set and tiling the graph gives that parameter), the graph's own textures and
the textures its solutions' pixel constants bind (by name suffix: `_d` / `_c` colour, `_n` normal, `_m` masks last),
then, for a shader the database lacks, textures named like the mesh in its folder. Each texture is sampled with its
own UV set (uv0, uv1; the overlay set uv2 is drawn from uv0 and a world-projected graph texture by the dominant axis
of the world normal) and tiling. A normal map pairs with the colour's UV set and tiling, else it is the best one on a
mesh UV set. Blend and sides come from the database's surface type and solutions. Glass keeps its colour map as a
tint and draws blended at a fixed opacity of 0.3, because BF3 computes glass opacity in compiled shader code.

**Material census** (`bf3dump looks MP_Subway TeamDeathMatch0`, about 5 s; it prints every drawn shader with its
instance slots, parameters, M1 and L1 albedo and normal coverage, glass and the colour source, then the totals and
the gate; `--shader NAME` dumps one graph from the database):

| drawn instance slots, Metro TDM | M1 | L1 |
|---|---|---|
| with a base colour texture (**gate: L1 >= 95 %**) | 48.95 % (8,221 of 16,796) | **98.01 %** (16,451 of 16,785) |
| with a colour where BF3's graph names one | | 100.00 % (16,451 of 16,451) |
| with a normal map where BF3 has one | 46.54 % (5,960 of 12,807) | 99.73 % (12,773 of 12,807) |
| instances with every drawn slot coloured | 41.10 % | 97.29 % (of 10,505) |
| plan materials / textures | 624 / 378 | 631 / 647 (3 of them the sky) |

The 334 slots L1 leaves without a colour texture have none in their graph: their colour is compiled into the
shader (for example `ss_metrosign_frame` 44, `ss_sub_tickethall_shopouterwall` 32, `gravel_01` 26,
`black_shader` 12), or they are glass with no colour map (100 slots, drawn as a grey tint). L1 also drops 11 slots
that are transparent overlays drawn by their mask alone. The largest shaders M1 drew flat and L1 now textures:
`proppreset_glass` (521 slots), `parkwall_01_mp_subway` (399), `fenceparc` (384), `ss_pipesystem_02` (383),
`subwaytrain_interior_base` (289), `debrispile_02_objectsshader` (243), and `mp_subway_asphalt_02` (144, world
projected at 0.5).

**Environment selection.** A level places VisualEnvironmentReferenceObjectData. The reference's Priority orders them;
the entity's own Priority is 1 on every Metro VE. A VE whose Visibility a PropertyConnection drives from an
AreaProximityEntityData is an *area*: the proximity's LinkConnection names its shape (VolumeVectorShapeData polygon
swept up by Height, or OBBData box), and its ProximityDistance is the fade. Every other VE that is not Excluded is
always on. Per component (OutdoorLight, Enlighten, Sky, Fog, Tonemap, ColorCorrection), the base state comes from the
always-on VE with the highest Priority. Areas then blend over it, lowest Priority first, by their weight at the
camera: 1 inside the shape, falling linearly to 0 at the fade distance [inferred]. Metro uses:

| VE | role | Priority | shape | what it changes |
|---|---|---|---|---|
| `ve_mp_subway_park_01` | base | 2750 | always on | sun (4, 3.02, 1.88) at elevation 35°, sky, fog 25..1382 m, MiddleGray 0.3, exposure 0.1..5, LUT 0 |
| `ve_mp_subway_city_01` | area | 3500 | box, centre (-194.8, 76.4, -538.0), half extents (159, 20, 114.6), fade 1 m | street: sun peak 6, sky ×5, fog density 0.061, exposure 1..7, LUT 1 |
| `ve_mp_subway_subway_01` | area | 4000 | 6-point polygon, floor 43.48 m, height 26 m, fade 2 m | station: sun peak 10, sky ×4, a fog curve that fits to density 0 (no fog), MiddleGray 0.15, exposure 0.25..30, LUT 2 |
| `ve_mp_subway_subway_02` | area | 4500 | polygon, floor 36.41 m, height 100 m, fade 5 m | deeper tunnels: sun off |

`cameras_01` is Excluded. `motionblur` has no component the look reads. The dominant state's LUT is the one drawn
(an area's LUT from weight 0.5). `bf3dump looks` prints the read environment, the LUT grey ramps and the blended
values at the four shot views.

**Mapping to Godot** (`look_bridge.cpp`; one DirectionalLight3D, one WorldEnvironment, CameraAttributesPractical):

* Sun: the light's colour is SunColor divided by its peak, and its energy is the peak (park 4, street 6)
  [inferred]. Energy = peak / π, which treats SunColor as irradiance, drew every view far too dark at any exposure
  within Min..MaxExposure. The direction comes from SunRotationX (azimuth from +Z toward +X) and SunRotationY
  (elevation). Shadows reach 250 m.
* Ambient: a sky shader whose radiance pass draws three bands:
  * up = Enlighten SkyBoxSkyColor + 0.25 × bounce;
  * horizon = the mean of sky and ground + 0.5 × bounce;
  * down = SkyBoxGroundColor + bounce;
  * bounce = SunColor × sin(elevation) × 0.25 (kBounceAlbedo) × BounceScale [inferred].
  Enlighten is precomputed radiosity, stored in a res format not read here. Enlighten SunScale is not applied:
  with it, the street's shade (SunScale 0.25) drew near black.
* Sky: the panorama covers the upper hemisphere (u from PanoramicRotation, v over PanoramicUVMinY..MaxY), blended by
  the red channel of PanoramicAlphaTexture over SkyGradientTexture, all × BrightnessScale. The sky parameters are
  re-sent only when they change by more than 1 % (each change re-renders the 64² radiance map).
* Fog: on by default. Godot depth fog, density × smoothstep(begin, end, d)^curve [inferred], is least-squares fitted
  to the Fog Curve cubic over the first 1500 m past Start; the park gives 25..1382 m, density 0.156, curve 0.57. The
  fog colour is the panorama's horizon (its bottom 15 % of rows, linear) × BrightnessScale, tinted toward FogColor by
  the peak of FogColorCurve. The station area's fog curve fits to density 0, so it draws no fog.
* Exposure: BF3 exposes each frame to MiddleGray / average luminance, clamped to Min..MaxExposure. Godot's auto
  exposure does the same, with scale = MiddleGray and its sensitivity clamp set from the exposure limits:
  min_sensitivity = MiddleGray / MaxExposure / k and max_sensitivity = MiddleGray / MinExposure / k. Speed is
  1 / ExposureAdjustTime. k = 0.00125 scene luminance per ISO was **measured**:
  * With both sensitivities at 24 ISO, the park drew like a fixed exposure of about 9.6.
  * The street clamp (34..240 ISO) then bounded the views between fixed exposures 7 and 1.
  * The 12.5 / 100 that the API docs suggest overexposed every view about 100×.
* Tonemap and bloom: filmic with white 4 [inferred] stands in for BF3's FilmicNeutral. Bloom is off by default:
  Godot's glow (additive, HDR threshold 0, intensity = the peak BloomScale, about 0.07 on Metro) cost 0.31 ms of
  GPU time per frame (2.08 -> 1.76 ms over the four views) and changed the frames by under 0.5 / 255 on average,
  because auto exposure takes the added light back. `--look-bloom` draws it.
* Colour correction: Contrast (about 0.5), Brightness and Saturation (about Rec.601 luma) per channel, then the
  ColorGradingTexture (a 32³ volume, red fastest, corner-aligned). They are baked into one 32³ RGBA8 LUT per state,
  sampled at texel centres, and drawn through Environment adjustment_color_correction (an ImageTexture3D). A probe
  run confirmed that Godot feeds the LUT display-encoded (sRGB) values, as BF3 grades them.

**The blue cast in shade.** M1 normalised the Enlighten sky colour to a peak of 0.7 and drew it as the only ambient,
at energy 0.6, against a sun of energy 1.0 (SunColor × 0.25). Shade therefore got 42 % of the sun's level, in pure
sky blue. BF3's numbers are a sky radiance of (0.06, 0.116, 0.157) against a SunColor of 4, plus warm bounce light.
L1 keeps the sky at its radiance and the sun at SunColor, and adds the bounce estimate: the park's ambient is
up (0.20, 0.22, 0.22) and down (0.74, 0.57, 0.33). The teal that remains in deep shade is BF3's own grade: park LUT 0
maps display grey 0.27 to about (0.07, 0.20, 0.22), and `--look-off=lut` removes it.

**Inferred constants** (each marked [inferred] in the code):

| constant | value | where |
|---|---|---|
| area fade | linear, 1 inside the shape to 0 at ProximityDistance | `look_area_weight` |
| sun energy | the SunColor peak, no 1/π | `look_frame_of` |
| bounce albedo | 0.25 (kBounceAlbedo) | `look_environment.h` |
| ambient bands | up = sky + ¼ bounce, horizon = ½ (sky + ground) + ½ bounce, down = ground + bounce | `look_frame_of` |
| fog shape | Godot depth fog as density × smoothstep^curve, fitted over 1500 m | `fog_fit` |
| exposure unit | 0.00125 luminance per ISO (measured) | `look_bridge.cpp` kLuminancePerIso |
| static exposure (`--look-off=exposure` only) | reference albedo 0.3, sunlit share 0.1 | `look_environment.h` |
| filmic white | 4 | `look_bridge.cpp` |
| glass | opacity 0.3, untextured tint 0.25, roughness 0.1 | `level_upload.cpp` |
| overlay UV (TEXCOORD2) | drawn from uv0 at scale 1 | `look_materials.h` |

Diagnosis switches (after `--`): `--look=m1`, `--look-bloom`, `--look-off=exposure,fog,bloom,lut,blend` (any
subset; blend draws glass opaque),
`--look-exposure=<e>` (a fixed exposure), `--shot-settle=<s>` (free shots wait s seconds for auto exposure; default
0), and `--shot-measure=<n>` (time n frames at each free shot, both as frame intervals and as the viewport's GPU
render time, with per-view medians).

**Gates** (dev build, Radeon RX 9070 XT, D3D12, 2560×1440 fullscreen, vsync off; other tracks were building and
testing on the machine, so every timing here is noisy and the acceptance rerun on a quiet machine decides):

| gate | threshold | measured | command |
|---|---|---|---|
| L1 albedo coverage | >= 95 % of drawn instance slots | 98.01 % (PASS) | `bf3dump looks MP_Subway TeamDeathMatch0` |
| frame time, L1 against M1 | median L1 frame interval <= 1.25 × M1's **and** median L1 p99 frame interval <= 16.7 ms (since the integration, below; was <= 1.10 × M1's) | 7.41 against 6.88 ms, 1.076 (PASS); full ctest run 3.75 against 3.91 ms, 0.961 (PASS); after the verifier fixes 5.91 against 4.97 ms, 1.187 (FAIL), then 4.62 against 4.78 ms, 0.966 (PASS), CPU at 34-93 % from other tracks | `ctest --preset dev -R godot.l1_look` |
| E4 cold load | 15 s | 2.59 s and 2.86 s in two full ctest runs, 2.63 s after the verifier fixes (PASS; `_ready` -> Done 1.98 / 2.05 / 1.95 s) | `ctest --preset dev -R godot.m1_load` |
| E5 warm load | 5 s | 1.98 s and 2.15 s, 2.08 s after the verifier fixes (PASS; `_ready` -> Done 1.30 / 1.43 / 1.41 s) | same |

`godot.l1_look` runs `--look=m1` and `--look=l1` alternately for 5 rounds (`-DROUNDS=n` in script mode); a run
whose viewport is not 2560x1440 (`--fullscreen` takes the monitor's mode) fails. Each run
holds 4 free views (street at TDM spawn 13, station at spawn 22, park, overview) and times 300 frames per view after
a 3 s settle. A run's frame time is the mean over its views of the per-view median frame interval, and the medians
over the rounds are compared. GPU render time (same statistic) is reported, not gated. The interval swings with the
load: within one 5-round run M1 alone ranged 3.2-7.6 ms, and a 3-round A/B of M1, L1, L1 with glass opaque and L1
also without auto exposure gave medians of 4.28, 4.16, 3.54 and 4.58 ms, an order no part of L1 explains, while the
GPU medians stayed in order (1.50, 1.68, 1.59, 1.58 ms). The quiet-machine rerun decides the interval gate.

<!-- integration (integ/spikes) -->
**Frame-time gate changed at the integration (orchestrator decision, 2026-10-16).** The quiet-machine acceptance run
measured the L1 / M1 frame-interval ratio at a median 1.103 against the 1.10 limit: about 2.7 ms per frame at 1440p,
GPU +14 %, all of it from drawing glass and transparency, the fog and the LUT, which M1 did not draw at all. A
relative limit of 10 % penalised drawing content the baseline skipped rather than measuring a budget. `godot.l1_look`
now passes when the median over the rounds of L1's frame interval is **<= 1.25 × M1's** and the median over the rounds
of L1's **p99 frame interval** (every timed frame of a run, all four views; `[look] frame time p99` in the run's
output) is **<= 16.7 ms** at 2560x1440, vsync off. Glass, fog and the LUT stay on by default; the ratio, both p99s and
the GPU times are printed.

**Early stop (test speed, 2026-10-17).** With the default 5 rounds the row stops after round 3 when the decision can
no longer change: the median of 5 values lies between the smallest and largest of any 3 of them, so when the largest
L1 interval of rounds 1-3 is <= 1.25 × the smallest M1 interval of rounds 1-3 (the same integer limit) and the largest
L1 p99 of rounds 1-3 is <= 16.7 ms, every outcome of rounds 4 and 5 passes the gate. It prints `early stop after 3
rounds: decision fixed` and the medians of the 3 rounds; otherwise all 5 rounds run and decide as before. A failed
run in rounds 1-3 (a short value list), `-DROUNDS=n` or `-DEARLY_STOP=OFF` runs every round. What an early stop does
not check is rounds 4-5 themselves: 4 fewer Godot runs of the same binary and scene are checked for a crash, the
viewport size and the frame-time lines; the frame-time decision is identical. Replayed on the 19 recorded 5-round
runs (scratch harness): 11 stop after round 3, all of which pass with 5 rounds; the 3 failing runs and the 4 older
logs without p99 lines never stop early.

The measured frame intervals (M1 6.87-7.62 ms per round) are about 2.5 × what the same runs give on an idle
machine (M1 about 2.7 ms), because the other tracks kept every core busy: the frames are CPU bound, and the
interval is mostly scheduling. An earlier gate run on a lighter load gave M1 2.67 against L1 2.94 ms (mean
statistic, 3 rounds). GPU render time is stable under that load (M1 1.498-1.504 ms per round): L1 draws in
1.764-1.777 ms, **+17.6 %** (0.27 ms) over M1 (the final full ctest run: 1.675 against 1.426 ms, +17.4 %). Parts, measured with `--look-off` (four views, GPU medians, ms):

| run | GPU | against M1 |
|---|---|---|
| M1 | 1.506 | |
| L1 with bloom (before it went off by default) | 2.076 | +0.57 |
| L1 without bloom (the default) | 1.761 | +0.26 |
| L1 without exposure | 2.024 | exposure 0.05 |
| L1 without fog | 2.068 | fog 0.01 |
| L1 without LUT | 2.057 | LUT 0.02 |
| L1 with glass opaque | 1.981 | the transparent pass 0.10 |
| L1 without exposure, fog, bloom, LUT and blending | 1.616 | the textures and normal maps 0.11 |

Set
`OPENBF3_LOOK_SHOTS=<dir>` to keep the last round's shots, `m1_<view>.png` and `l1_<view>.png` (for example
`b\shots\l1`, which is gitignored; never commit them).

Tests: `render.look.unit` (no data) covers the shader database reader on a synthetic res: fields, every truncation,
4 × 863 single-byte mutations, and a res whose 20,000 solution uses exceed the work budget. It also covers the
resolver's tiers, normal pairing, glass, alpha test and undrawn rules on a synthetic lookup, plus `fog_fit`, the
area weights (box, polygon, fade, NaN and infinite positions), the `look_frame` blend and `look_grade`. Two cases
hold the resolver's work near-linear on hostile data: 200,000 shaders repeating 100,000 name hashes across two
databases merge first-wins through a set (the scan of seen hashes was O(n²)), and a graph of 20,000 constants plus
20,000 sampled names resolves for 2,000 slots within a work bound (0.18 s). Each graph's colour and normal
candidates, its streamed/resident split and its parameter index are built once per graph and capped at 64
(`kLookMaxGraphCandidates`; Metro's widest graph has 7, so none are dropped), and each folder's neighbour candidates
are indexed once by class and file name, so a slot finds the longest shared prefix by binary search. A third case
checks those neighbour picks. `bf3dump looks` prints the resolver work and the candidate maximum; Metro's resolved
materials (`--materials`) did not change.
`render.look` (install) parses Metro's shader database: 2 render paths, 1,280 shaders, 51,602 work units of a
budget of 116 M. It also opens the resolver with 0 failures, reads the environment (park base, 3 areas, 3 LUTs) and
checks install.unchanged. Both pass under `--preset asan`, as do `render.scene` and `bf3dump looks`. `render.scene`
now accepts an albedo that is not one of the slot's textures when the L1 resolver chose it from the graph.

Open issues:
* No local lights (BF3's point and spot lights are not read) and no Enlighten radiosity: indoor and station light is
  the area's sun and ambient only.
* The terrain still draws one flat colour.
* The 334 constant-colour slots draw untextured grey. Their colours are in compiled shader code, which is not read.
* Glass opacity is a fixed guess.
* Overlay uv2 is drawn from uv0.
* World-projected graph textures use a triplanar-style dominant-axis mapping, not BF3's exact projection.
* The sky, fog, bounce and LUT mappings are inferred and have not been compared against BF3 frames.
* GPU render time rises by 17.6 % (0.27 ms at 2560x1440): textures and normal maps on twice as many slots, the
  glass pass, auto exposure, fog and the LUT. The frame interval, which is CPU bound on this machine, stays within
  the gate. A GPU-bound budget would have to give up the glass pass (0.10 ms) or auto exposure (0.05 ms) first.
* The plan default is now L1, so `godot.m1` and `godot.m1_load` run with L1 materials.

<!-- S1 server scale (spike) -->
## S1 server scale (spike)

Spike S1 (docs/design/s1.md) asks whether a 60 Hz server steps 64 navmesh walker bots within 8 ms per tick, and
whether the Detour navmesh baked from exactly the motor's collision set (Collision + Character bodies, terrain with
holes, the level's combat-area tiles as terrain cover) connects every flag, HQ, MCOM and spawn point. Interfaces:
`bf3/core/level/mode_areas.h`, `bf3/sim/nav.h`, `bf3/sim/nav_query.h`, `bf3/sim/bots/walker_brain.h`,
`bf3/host/bot_fill.h`, `bf3/host/scale_bench.h` (counters in `bf3/sim/bots/bot_stats.h`, `bf3/host/bot_fill_stats.h`).
Recast/Detour is built with 64-bit poly refs.

Each gate is one ctest (the machine is shared while the tracks run, so timings are noisy; acceptance reruns them on
a quiet machine):

| gate | ctest | threshold |
|---|---|---|
| G1 tick | `host.scale` (Metro and Caspian ConquestLarge0, 64 walkers + 1 observer, 10 s warm-up + 120 s sim time ticked back to back) | p99 `GameServer::tick` <= 8.0 ms on each level; >= 90 % of bots walk >= 100 m, abandoned goals <= 10 % (bots spawn in the main nav component, so G1 does not depend on G2; a paced 60 Hz row, the thread's CPU share, the bots' p99 vs 2 ms and the server working set vs 0.4 GiB are printed, not gated) |
| G2 connectivity | `sim.nav` (Metro and Caspian, all 7 MP modes) | every flag, HQ, MCOM and spawn on the mesh and in the main strongly connected component (each row also prints its weak component) |
| G3 sweep cost | `host.scale` | motor steps, queries per step, queries per second, us per step, direct us per sweep and per overlap printed and non-zero |
| G4 bake | `sim.nav.bake` (Caspian and Kharg ConquestLarge0, uncached, 16 threads, drop links sweep-checked on per-lane worlds) | Caspian <= 6.0 s, Kharg <= 13.0 s (tile inputs + lanes incl. link checks + assembly; not like-for-like with the research's region, tile counts printed) |

The same numbers for a human: `bf3dump bots <LEVEL> <MODE>`, `bf3dump nav bake|stats|components|path|probe`. In the
game, `res://scenes/m1_metro.tscn -- --bots=64 --nav-overlay` adds the walkers to the M1 server and draws the navmesh
by component (F7 toggles); screenshots go to `b\shots\s1\`.

Results, 2026-10-15, on the shared machine, so the timings are noisy (docs/design/s1.md §19):

* **G1 passes.** p99 1.51 ms on Metro and 0.87 ms on Caspian, both runs valid.
* **G2 fails.** Every Conquest flag and HQ is in the main component, but not every row is:

  | level | rows in main |
  |---|---|
  | Metro | 485 / 566 |
  | Caspian | 661 / 709 |

  * The Rush stage geometry is closed in a bake over all stages: `ParkEntranceUndestroyed` cuts the park off the station.
  * Crates of stages a mode does not play are parked off the map.
  * Some spawns are on platforms cut off by props and steps the motor cannot climb.
  * `sim.nav` reports the all-rows gate as a WARN and CHECKs what holds.
  * Integration decision (2026-10-16): G2 stays a reported WARN. The fixes (per-stage Rush nav or Detour tile-cache
    obstacles driven by the Rush logic, scoring only played points, the dynamic-prop and step / platform-edge spawn
    cases) move to M2b (bots); the 24 Caspian RL spawns about 30 m under the terrain move to the terrain-collision fix
    that follows this integration (docs/design/s1.md §19.5).
* **G3 is measured.** Metro: 3.44 queries per step, 4.4 µs per sweep. Caspian: 4.21 queries per step, 1.8 µs per sweep.
* **G4 passes.** Caspian 2.8 s, Kharg 3.8 s on 16 threads.

The as-built nav uses:

* cs 0.2 with the radius rounded up (0.4 m);
* a detail error of 2 ch;
* the water rule, under which Kharg's sea bed tiles lose their terrain.

The run with bots: `"%GODOT%" --path bf3-opensource res://scenes/m1_metro.tscn -- --mode=ConquestLarge0 --bots=64 --nav-overlay [--shot-delay=12 --shots=...]`.

## S2 vehicle replay (spike)

<!-- S2 vehicle -->
Spike S2 (docs/design/s2.md): the Humvee on Jolt's `VehicleConstraint` + `WheeledVehicleController` in a server world
and a prediction world over one shared static world (MP_007 RushLarge0), saved and restored at full precision, driven
by recorded command logs on 7 found courses. The interfaces are in the tree (`bf3_core` `gameplay/vehicle_drive.h`,
`level/vehicle_spawns.h`; `bf3_sim` `vehicle_types.h`, `vehicle_ghost.h`, `vehicle_model.h`, `vehicle_world.h`,
`vehicle_course.h`, `vehicle_replay.h`); what the data does not say is `bf3-opensource/config/vehicle_tuning.cfg`
(30 keys, all `[inferred]` placeholders: IW4L's tuned jeep and Jolt defaults, for S5 Session 2 to replace).

Gates, each one command (thresholds in docs/design/s2.md §1; timings printed by the runs are noisy on a shared machine
and are not gates):

| gate | threshold | command |
|---|---|---|
| C courses found | all 7 kinds found on MP_007 (a finder result, kept apart from G1 / G2) | `ctest --preset dev -R "^sim\.vehicle$" --output-on-failure` |
| D courses drove | every found course meets its kind's activity criteria (distance, speed, turns, impacts; driving kinds never asleep); G1 and G2 fail if any found course fails D | same |
| G1 free driving bit-identical | 0 of 3,600 ticks with a differing state hash, every found course | same |
| G1r replay from any tick | 0 mismatching ticks over 15 restore ticks per found course | same |
| G1m several vehicles | 3 touching Humvees on synthetic ground: G1 and G1r with 0 mismatching ticks, >= 30 contact ticks | `ctest --preset dev -R "^sim\.vehicle\.unit$" --output-on-failure` |
| G2x corrections with the full-precision owner state (the gate since the integration) | `ExactPod`: the server's `VehicleState` restored unquantised onto the client's own diverged history, max < 0.02 m per found course at 100 and 150 ms replays (and >= 1 diverged base, >= 30 static-collision samples on the collision course) | `ctest --preset dev -R "^sim\.vehicle$" --output-on-failure` |
| G2 corrections with quantised 30 Hz ghosts | report only since the integration: max (and p99) per found course at 100 and 150 ms replays against 0.02 m, printed as `G2 (report only, quantised ghost)` | same |
| X same hashes from a second binary | report only: the per-tick server hashes of a second build equal the first | `b\dev\tools\bf3dump\bf3dump.exe vehicle-replay MP_007 RushLarge0 --no-g1 --no-g2 --hash-out=dev.txt`, the same with `b\asan\...` (or `b\release\...`) into `other.txt`, then `fc dev.txt other.txt` |
| H handling numbers | report only (unverified placeholders) | `ctest --preset dev -R "^sim\.vehicle$"`, or `bf3dump vehicle-replay MP_007 RushLarge0 --handling` |
| S spawns settle | report only: < 1 cm drift over the last 60 settle ticks per Humvee spawner | `ctest --preset dev -R "^sim\.vehicle$"` |
| install unchanged | sizes and mtimes equal | `ctest --preset dev -R "^sim\.vehicle$"` |
| contract / units | pass | `ctest --preset dev -R "^(s2\.contract\|sim\.vehicle\.unit)$"` |

S2 passes only when C, D, G1 (with G1r), G1m, G2x, install unchanged and contract / units all pass. Under `ctest
--preset asan` (Jolt runs ~27x slower there), `sim.vehicle` runs the `OwnHistory` and `ExactPod` modes only and the
synthetic-course case of `sim.vehicle.unit` runs `OwnHistory` + `ExactPod` + `Full` (the other ablations are `dev` /
`release`). Both suites print steps
and seconds per course; the TIMEOUTs are 3x the measured runs (docs/design/s2.md §15.1).

Measured (2026-10-15, docs/design/s2.md §15.1): **since the integration the quantised G2 is report-only and G2x
(`ExactPod`) is the gate, which passes**; before it, S2 failed on G2 alone. C, D, G1, G1r, G1m, install unchanged and
the units pass: Jolt's vehicle is bit-identical on every MP_007 and synthetic course (3,600 ticks; 30 restore runs
each, into the prediction world and as an in-place rollback; three touching Humvees), and the dev, release and asan
builds produce the same per-tick hashes. G2 with the quantised 440-bit ghost fails on slalom, bumps and collision
(max 2.2-6.5 cm against 2 cm; p99 <= 1.3 cm everywhere), while restoring the full-precision `VehicleState`
(`ExactPod`) onto the diverged client gives at most 1.9 mm (0 without static chassis contact; the contact cache need
not be sent) and the same without the lambdas (`PodOwnLambdas`) fails at 2.8 cm: M3 sends the driver's own vehicle
that state unquantised, lambdas included, and the ghost (376 bits: the suspension codes carry nothing) to everyone
else. H (unverified placeholders): 0-60 km/h 7.4 s,
9.1 m/s^2 braking, 18.4 m turning circle, 75 km/h top speed. S: 3 of 6 Humvee spawners settle within 1 cm.

<!-- integration (integ/spikes) -->
**Integration decision (2026-10-16, docs/design/s2.md §15.2).** The pre-registered fail branch (s2.md §1, plan R4)
was taken: M4's driven vehicle stays on Jolt, M3 sends the driving client its full-precision `VehicleState` (about
0.5-0.6 KB per update) and the quantised ghost only for vehicles the client does not drive. G2 measured the ghost on
the driver, a configuration M3 will not use, so it is report-only now (its numbers keep being printed); the gate is
**G2x**: `ExactPod` max < 2 cm on every found course, which is what M3 ships. `sim.vehicle` and `sim.vehicle.unit`
pass with it (accepted run: `ExactPod` <= 1.91 mm on MP_007, 0.75 mm synthetic).

Tools: `bf3dump vehicledrive <LEVEL> <MODE> [<blueprint>]` prints the drive def as read and the model built from it;
`bf3dump vehicle-replay <LEVEL> <MODE>` runs the course search and the gates with the test's report (`--synthetic`
the synthetic courses with the level's model, `--courses=a,b`, `--modes=<hex>`, `--handling`, `--sweep` the ghost
width rows, `--trace=N` a per-tick server trace, `--hash-out=`, `--log-out=<dir>` / `--log-in=<file>` command logs,
scratch only).

## UI groundwork 1 (ui1)

Design: [docs/design/ui-swf-fonts.md](docs/design/ui-swf-fonts.md). BF3's SwfMovie res (434 movies) are read at run
time as symbol tables (no SWF renderer, no ActionScript): timelines, labels, placements, exports and imports, the
Scaleform external-image atlases and their sub-images, edit texts, scaling grids and `DefineFont3` fonts. The atlas
cutter maps labels and symbols to atlas rectangles (Godot `AtlasTexture`); the fonts are converted to in-memory
TrueType for `FontFile.set_data` and cached (cache kind `ui-font`, converter version in the key);
`UIFontMappingCollection` resolves `$MENU_*` / `$HUD_*`; the HUD screens' widget positions are read from their
1280 x 720 stages.

Implemented (2026-10-16). Readers in `src/bf3_core/src/ui/` (headers in `src/bf3_core/include/bf3/core/ui/`:
`swf_movie.h`, `swf_library.h`, `swf_census.h`, `swf_atlas.h`, `swf_font.h`, `font_ttf.h`, `ui_fonts.h`,
`swf_layout.h`); Godot side `src/bf3_godot/src/ui_assets.{h,cpp}` (`Bf3UiAssets`: fonts as `FontFile`s, atlas
regions as `AtlasTexture`s, HUD rows and layout trees, loaded on its own thread after `Bf3Localisation`) and
`ui_font_check.{h,cpp}` (`Bf3UiFontCheck`, scene `res://scenes/ui_fonts.tscn`). Every parser charges a
`WorkBudget` and has hard caps (16 MiB body, 2^22 tags, 2^16 frames, 2^18 display events, 4,096 contours /
16,384 points per glyph, 2^20 atlas pieces); hostile input fails with an error.

**Commands.** `bf3dump swf census [--tsv <out.tsv>]` (exit 1 on any error), `swf movie <name>`, `swf labels <name>`,
`swf layout <name>`, `swf hud`, `swf fonts [--cache=<dir> | --no-cache]` (the fonts' stats and SHA-256 and the
name-resolution table; it writes no font file). Their output is BF3 data: keep it out of the repo. In Godot:
`<godot> --path bf3-opensource res://scenes/ui_fonts.tscn` shows the font page (`-- --page=1` the coverage table,
`--page=2` the iconsingame labels); `-- --ui-check` prints the rows and `UI1 CHECK: PASS (n/n)`; `--shot-dir=<dir>`
also writes `ui1_fonts.png`, `ui1_coverage.png`, `ui1_icons.png`.

**Gates** (measured 2026-10-16 on this machine; 16 threads shared with other tracks' builds and tests, so every time
is noisy and each test prints its own; commands from the worktree root after `tools\build.cmd dev`, the Godot tests
with `OPENBF3_GODOT` set; details in the design's §21):

| gate | measured | command |
|---|---|---|
| G1 census | 434 movies, 434 parsed, 0 errors (CFX 371, GFX 63); 0 names with two SHA-1s; every tag total equals the design's §4.2 table and no other code occurs; AS2 1,638,265 bytes; 2,441 labels (0 after the last ShowFrame); 135 external images, 0 textures missing; 820 sub-images, 0 outside; 1,268 imported symbols, 1 missing (deckofcards imports `iconsShared.swf:IconsShared_Check`, not exported); fonts 63 outlines / 21 glyphless / 2 codes-only | `ctest --preset dev -R "^core\.ui\.swf$" -V` (also `bf3dump swf census`) |
| G2 iconsingame | `iconsingame_i1.tga` 4096 x 128, its DxTexture DXT5 of that size; 162 sub-images, 0 outside; 427 labels on 6 sprites: 135 with art (110 one piece, 25 two), 292 empty; 95 sub-images from labels, 96 with symbols, 162 from frames; the 10 spot rectangles of §6.3 exact | `ctest --preset dev -R "^core\.ui\.swf$" -V` (also `bf3dump swf labels ui/assets/iconsingame`) |
| G3 fonts | TrueType 444 / 444 / 34 / 34 glyphs, 442 / 442 / 32 / 32 codes (Purista EA Medium / Semibold, Venice-Medium / SemiBold), U+00C9 in both Purista, 0 contours reversed, every point within 0.5 unit of the source, advances exact, kerning 335 / 324 / 0 / 0; cold 4 converted, warm 4 hits, a damaged entry dropped and reconverted, then a hit | `ctest --preset dev -R "^core\.ui\.font$" -V` |
| G4 Godot | `UI1 CHECK: PASS (25/25)`; glyph advances within 0.0156 px of the source at 64 px; shaped "0123456789" 366.500 px (source 366.519), "OPERATION MÉTRO" 547.031 px (547.019); "AV" 3.844 px narrower than "A" + "V" (source kerning 3.841 px) | `ctest --preset dev -R "^godot\.ui_fonts$" -V` |
| G5 shots | the windowed check passes 25/25 and writes the three PNGs (findings below) | `ctest --preset dev -R "^godot\.ui_fonts_shots$" -V` |
| G6 mapping | Exact 5, CaseFolded 2, Family 4, Device 5, Unresolved 0; `G6 family-inferred: $FONT_TITLE $HUD $HUD_light $Menu_light`; the other font libraries' 59 names all Exact under their own collections | `ctest --preset dev -R "^core\.ui\.swf$" -V` (also `bf3dump swf fonts`) |
| G7 HUD table | 86 placements (14, 8, 9, 7, 8, 9, 10, 7, 0, 1, 4, 8, 1, 0), every stage 1280 x 720, the §8.2 spot values within 0.05 px | `ctest --preset dev -R "^core\.ui\.swf$" -V` (also `bf3dump swf hud`) |
| G8 asan | `ui1.contract`, `core.ui.swf.unit`, `core.ui.font.unit`, `core.ui.swf`, `core.ui.swf.fuzz`, `core.ui.font` pass under ASan with no report; in dev every hostile case ends in under 1 s (slowest over two runs: 2^18 + 1 display events 0.14 to 0.55 s, 2^22 + 1 tags 0.11 to 0.48 s; noisy) | `ctest --preset asan -R "^core\.ui\.(swf\|font)"` |
| G9 install | 256 index files unchanged in `core.ui.swf`, `core.ui.swf.fuzz`, `core.ui.font` and `godot.ui_fonts` | the `install.unchanged` lines of those tests |
| T1 census time | 0.28 s (library open 0.25 s + census 0.037 s; limit 15 s) | `ctest --preset dev -R "^core\.ui\.swf$" -V` |
| T2 font load | cold 0.031 s (limit 1.0 s), warm 0.003 s (limit 0.25 s) | `ctest --preset dev -R "^core\.ui\.font$" -V` |

**What the implementation changed in the design** (each fixed in the design in the same commit): G6 counts Exact 5,
not 4 (the en font library's own edit texts name all four long names; revision 2 missed Venice-SemiBold); two §8.2
values were rounded (`HudBackgroundWidget_01` in hudtdmscreen is at y 486.95, both `Compass_01`s at x 85.05 with
scale 0.99985); the G4 string widths are the sum of the shaped glyph advances, because `Font.get_string_size()` and
`TextServer.shaped_text_get_width()` return the width ceil()ed to whole pixels on Godot 4.8 dev7 (367.000 px for a
366.519 px line) while the shaped advances are exact.

**Screenshots** (G5; `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui1\`, gitignored, not in the repo),
inspected 2026-10-16:
* `ui1_fonts.png`: both Purista weights and both Venice weights at 64 / 32 / 16 px. The counters of O, A, R, P, B,
  0, 4, 6, 8, 9 and % are open (no inverted contour); every line sits on its red baseline; spacing is even with no
  overlaps or gaps at any size; "OPERATION MÉTRO" shows its acute accent; "AV" visibly kerns. The red
  baseline under the 32 px lines is fainter than the others (it falls on a half pixel); the text is right.
* `ui1_coverage.png`: the 442 codes of each Purista weight (Latin-1, Latin Extended-A, Cyrillic, punctuation up
  to U+2588) draw their glyphs, except five cells that draw nothing: U+0020, U+00A0 and U+00AD (expected) and
  U+2012 and U+2588 (no visible outline at that size; the round trip shows the TrueType glyphs equal the source, so
  this is BF3's data, not the converter). Venice draws only `#`, `%`, `-`,
  `.`, `/`, `0`-`9` and `:`: the other 16 of its 32 codes (`!"$&'()*+,;<=>?`) are empty glyphs in BF3's own font
  library (the source outlines have no contours for them; the converter keeps that), so `has_char` is true and
  Godot's system fallback does not replace them. HUD text in `$HUD_*` with those characters draws nothing for them,
  as Venice would in BF3 if those characters were ever shown; the HUD wave should use `$MENU_*` for such text.
* `ui1_icons.png`: the 135 iconsingame labels with art, each from its `AtlasTexture` pieces: kit, vehicle, objective,
  flag and squad icons in their friendly / enemy colours; the 25 two-piece labels (e.g. `KitPickupAssault`, the CTF
  flag "picked" states) show the icon composed over its background; no piece is cut off or bleeds into a neighbour.

Converted fonts exist only in memory and in the derived-data cache; no command writes them anywhere else (design
revision 2, §20).

## UI minimap, HUD icons and HUD model (ui2)

Design and measured results: [docs/design/ui-minimap-icons.md](docs/design/ui-minimap-icons.md) (plan §3.8, D16,
§7.8; results in its §18).

**In the game.** `res://scenes/m1_metro.tscn` has a `MinimapHud` node (`Bf3MinimapHud`) beside the session: after
the level is controllable it loads, on its own thread at below-normal priority with 2 parse lanes, the level's
`UIMinimapData` (from the LevelData partition), the mode's combat-area distance fields, the capture points and the
hud3dicons atlas, then draws BF3's minimap bottom-left (the L8 satellite tinted with `DetailTextureTint`, outside the
combat area shaded with `CombatAreaColor`, flags and HQs from the atlas, the player marker, the walkers) and the
flag / HQ icons over the 3D view with their distance. Nothing is cached; the loader takes about 0.5 s on Metro.
Keys: M toggles north-up / rotating; the minimap hides with the free camera (F6). User args after `--`:
`--no-minimap`, `--minimap-mode=<mode>` (objectives and areas of another mode: the M1 game runs TDM, which has no
flags; e.g. `--minimap-mode=ConquestSmall0`), `--minimap-size=<px>`, `--minimap-north-up`, `--minimap-check`,
`--minimap-shots=<dir>`. The node is inert under `--headless`, `--m1-check`, `--m1-load-check`, `--shot-spawns`,
`--shots` and `--no-hud`, so other tracks' measurements and reference shots are unchanged.

**Libraries and tools.** `bf3_core` `ui/minimap.h` (`read_minimap_data`, `read_combat_area`, `load_minimap_level`,
`load_minimap_images`, `l8_mip_chain`, `combat_area_min_field`, `minimap_census`; the mapping
`u = 0.5 - (x - Cx)/S`, `v = 0.5 - (z - Cz)/S` and the on-screen view inline), `ui/hud_icons.h` (the atlas, mirrored
frames as `flip_x` / `flip_y`, `_small` item icons by unlock identifier, vehicle HUD icons), `ui/hud_sources.h` (the
HUD DataSources table, generated from the install at run time and never stored), `ui/hud_model.h` (`HudModel` and
the 235 binding rows, `check_hud_bindings`). bf3dump:

```
bf3dump minimap MP_Subway ConquestSmall0     :: UIMinimapData, masks, objectives, image sizes, timings
bf3dump minimap --census                     :: every combat-area asset and distance field of @all-mp (G3)
bf3dump hud-icons                            :: the hud3dicons atlas (G6)
bf3dump item-icons MP_Subway                 :: the catalog variants' _small icons by identifier (G7)
bf3dump hud-sources [--unbound]              :: the DataSources table and the binding check (G8)
```

**Tests** (one command per gate; the godot one needs `OPENBF3_GODOT` and a desktop session, the install ones exit 77
without an install):

| gate | command | measured 2026-10-15 (dev build, shared machine, times noisy) |
|---|---|---|
| G1–G5 minimap data, masks, objectives, orientation | `ctest --preset dev -R "^ui\.minimap$"` | 29 / 29 levels; 455 / 455 masks, 498 / 498 fields, 562 / 562 distance-field assets; Metro CQS 5 / 5, Caspian CQS 6 / 6, CQL 7 / 7; TDM mask vs gameplay polygon IoU 1.000 (others 0.000) |
| G6, G7 atlas and weapon icons | `ctest --preset dev -R "^ui\.hud\.icons$"` | 161 entries / 159 types / 93 with states, 8 mirrored frames; 172 / 172 catalog variants find a DXT5 64 × 16 icon |
| G8 DataSources | `ctest --preset dev -R "^ui\.hud\.sources$"` | 84 types, 1215 sources, 58 state components; 235 / 235 binding rows, 0 missing, 0 extra |
| G9–G11, G14 in game | `set OPENBF3_UI2_SHOTS=<dir>& ctest --preset dev -R "^godot\.ui2$"` | 10 / 10 rows: read-back byte-exact; map UV error ≤ 0.25 satellite texel in both views (mirrors ≥ 850 texels); marker ≤ 0.76 canvas unit; icons ≤ 0.22 texel, world icons 0.008 units; 6 shots (inspected by eye); loader 0.43–0.69 s |
| G12 asan | `ctest --preset asan -R "^ui(2\.contract|\.minimap|\.hud)"` | 8 / 8, 0 reports |
| G13 install unchanged | the `install.unchanged` row of each install test above | unchanged |

`ui.minimap.unit` and `ui.hud.unit` need no data; `ui.minimap.fuzz` and `ui.hud.fuzz` feed 250 seeded mutants per
input (`OPENBF3_FUZZ_ITERS`) through the readers.

## M2a weapons core

<!-- # M2a weapons -->
Design, contract and as-built results: [docs/design/m2a-weapons.md](docs/design/m2a-weapons.md). The Godot-free weapon
core is in `bf3_sim` (`src/bf3_sim/include/bf3/sim/weapons/`, `src/bf3_sim/src/weapons/`): the per-soldier weapon state
machine with sub-tick shot times (`SimTime`, 1,000,000 units per tick: 800 RPM = 4,500,000 units = 75 ms), the
counter-based shot RNG, spread and recoil from GunSwayData with the unmeasured mechanics as `Mechanics` switches,
double closed-form ballistics against `CollisionWorld` and zone-capsule hit targets (`HitTargetProvider`, stand-in
capsules), damage from the level material grid (`read_grid_pairs`, `SurfaceMaterialTable`), the M67 / M320 HE /
RPG-7 / SMAW explosives, and the headless shooting range. `weapon_checks.h` holds the §7.1 oracle rows as one
function (the unit test, the install test and `bf3dump weapon-sim --check` run the same rows); `weapon_install.h` is
the install side for tools and tests. The data's float times are converted through their shortest decimal, so 1.8 s
is exactly 108,000,000 units.

Commands (read-only; output is BF3 data, keep it out of the repo; each ends with `install.unchanged`):

```bat
b\dev\tools\bf3dump\bf3dump.exe weapon-sim weapons/m16a4/u_m16a4      :: sim numbers, fire / reload timing, drop table, shots to kill
b\dev\tools\bf3dump\bf3dump.exe weapon-sim --check                    :: the data rows, "WEAPON-SIM CHECK: PASS (38/38)", exit 0 / 1
b\dev\tools\bf3dump\bf3dump.exe weapon-sim --all > %TEMP%\w4.tsv      :: W4: 119 infantry rows on MP_Subway
b\dev\tools\bf3dump\bf3dump.exe range weapons/m16a4/u_m16a4 --exact   :: Metro TDM lane (96.9 m), targets at 10 / 25 / 50 m
b\dev\tools\bf3dump\bf3dump.exe range weapons/m16a4/u_m16a4 --perf    :: W11's 64-shooter load
```

Gates (one ctest each; W1–W10 run on simulated time and are exact; W11 is wall clock and **noisy** on this shared
machine). Measured 2026-10-15, dev preset:

| gate | ctest / command | threshold | measured |
|---|---|---|---|
| W1 fire timing | `sim.weapons.unit`, `sim.weapons`; `tools.weapon_sim` | M16A3 one magazine: 31 shots, every gap 4,500,000 units; `infinite_magazine` table: 134 shots in 10 s; M98B bolt cycle 78,000,000; AN-94 2 @ 3,000,000; 870 12 pellets, 28,800,000 pump; M16A4 3 per press; M9 ≥ 9,000,000 | all exact, on synthetic and MP_Subway defs (38/38 rows) |
| W2 drop, range | `sim.weapons.unit`, `sim.weapons` | 0.1775 / 1.5976 m at 100 / 300 m, M98B 1.0449 / 11.609 m at 300 / 1,000 m within 1 cm; range v0 × TTL within 1 mm | 0.1779 / 1.5981 / 1.0451 / 11.6098 m; 975.0000 / 3,250.0000 m |
| W3 shots to kill | `sim.weapons`, `sim.weapons.range` | M16A3 4 / 2 at ≤ 8 m, 6 / 3 at ≥ 50 m; one-shot ranges within 0.01 m | as required; Metro 5 m and 60 m end to end; 17.4762 / 14.9998 / 19.9995 / 50.7445 / 16.2565 m |
| W4 table | `sim.weapons`; `bf3dump weapon-sim --all` | 119 rows on MP_Subway, 0 errors | 119 rows, 0 errors |
| W5 zone multipliers | `sim.weapons.range` | M16A3 head 50.0 (×2.0), chest / body 25.0, legs 22.75; M98B 223.25 / 100.70 / 86.45; 870 pellet 18.0; within 1e-3 | exact to 1e-4 |
| W6 reload | `sim.weapons.unit`, `sim.weapons` | M16A3 108,000,000 / 142,200,000 units; commit 91,800,000 / 120,870,000; 870 270,000,000 | exact |
| W7 determinism | `sim.weapons.unit`, `sim.weapons.range` | equal shot and impact list hashes; ≥ 99 % of hip-fire directions change with the seed | equal; 100 % |
| W8 hostile data | `sim.weapons.unit`, `sim.weapons.fuzz` (dev and `asan`) | 2,000 mutated blobs: no crash, invariants hold | 0 violations (683 accepted mutants × 600 random ticks) |
| W9 explosives | `sim.weapons`, `sim.weapons.range` | M67 fuse 216,000,000 units; blast 112 / 81.45 / 40.73 / 0 at 1.5 / 3 / 5 / 7 m; M320 / RPG arm 0.5 s | exact; Metro stand-ins 112 / 85.52 / 44.80 / 0 |
| W10 install unchanged | every install test and command | `install.unchanged` row | yes (256 files) |
| W11 cost (wall clock, noisy) | `sim.weapons.perf` (RUN_SERIAL; exits 77 while `OPENBF3_QUIET_LOCK` names an existing file); `bf3dump range --perf` | 64 M16A3 shooters on full auto, 600 ticks: p99 (nearest rank) ≤ 2.0 ms per tick | p99 0.205 ms (mean 0.115, max 0.407) |

## Effects (M2a VFX1)

`bf3_fx` (`src/bf3_fx`, Godot-free, client only) is BF3's visual-effects core: the compiler from `EffectBlueprint` /
`EmitterDocument` EBX into flat, digest-cached `EffectDef`s (all 32 processor and 10 evaluator types), the
deterministic CPU particle simulator `FxWorld`, the per-level emitter atlas and the impact table from the material
grid. The design, exit gates (census 1,101/1,101 effects and 3,410/3,410 emitters with 0 unknown types, the 1p muzzle
and oilfire goldens, Metro / Caspian impacts, Metro's 52 atlas entries, cost per particle) and the seams for the
renderer (VFX2), triggers (VFX3) and audio are in [docs/design/m2a-vfx1.md](docs/design/m2a-vfx1.md).
`ctest -R fx.contract` checks the interface headers without data; `fx.unit` and `fx.fuzz` need no data; `fx.install`
and `tools.fx_bench` read the install (Skipped without one).

`bf3dump fx` (`fx census | compile | simulate | check | bench | atlas | impacts`, design §7.7) prints the compiled
defs and runs the gates; every gate has one command (the design's §18 has the measured values):

| gate | command | threshold | measured (2026-10-16, shared machine) |
|---|---|---|---|
| G1 census | `bf3dump fx census @all-mp` | 1,101/1,101 effects, 3,410/3,410 emitters, 0 unknown types, 32/32 + 10/10 types | met |
| G2 polynomials | `ctest -R "fx.contract\|fx.unit"` | exact f(0), f(1), clamp then scale | met |
| G3 1p muzzle | `bf3dump fx simulate fx_muzz_generic_rifle_1p --starts 10000 --seed 1 --seconds 1 --dt 0.0166667 --golden muzzle1p` | fractions 0.75 / 1 / 0.25 ± 0.02, flash life 0.05 s, lights exact | met |
| G4 oilfire | `bf3dump fx simulate --emitter fx/ambient/levelspecific/mp_12/emitters_oilfire/em_amb_mp12_oilfire_l_fire_01 --seconds 75 --dt 0.0166667 --seed 1 --golden oilfire` (and `--variable`) | 79..81 live after 15 s | 80..80, met |
| G5 impacts | `bf3dump fx impacts MP_Subway` / `MP_007` | design §5.3 counts and cells | met |
| G6 atlas | `bf3dump fx atlas MP_Subway` | 52 entries, 42 + 10, 0 missing | met |
| G7 every effect | `bf3dump fx check @all-mp --seconds 10 --seed 1` | 0 failures | 0 of 1,101, met |
| G8 determinism | `ctest -R fx.unit` and the `fx check` above | identical on 1 / 2 / 8 lanes | 0 mismatches, met |
| G9 cost | `ctest -R tools.fx_bench` (= `bf3dump fx bench --seed 1`) | median <= 50 ns per particle per step | 39.4 ns (prototype 24.7), met; noisy |
| G10 hostile data | `ctest --preset asan -R fx.fuzz` | no crash / hang / unbounded allocation / superlinear work (compiler, blob, simulator, impact table, atlas) | met |
| G11 install unchanged | `ctest -R fx.install` | layout.toc, cas.cat, .sb/.toc unchanged | met |

The cost gate runs RUN_SERIAL and is report-only under asan; the figure is wall clock on a shared machine, so it is
noisy (the same build measured 39-42 ns across runs).

## M2a audio

Design: [docs/design/m2a-audio.md](docs/design/m2a-audio.md) (slices A1 XAS voices + wave bank + mixer, A2 weapon
patch interpreter; section 1.1 has the measured results, section 16 the implementation notes). Implemented:
* `bf3_core` `audio/`: the EA-XAS / PCM16 stream codec (a port of IW4L `sound.rs`, Apache-2.0, attributed in
  `sound_stream.cpp`), the `SoundWaveAsset` reader, the resident `WaveBank` (budget 384 MiB), the node type table and
  the `SoundPatchAsset` compiler with its `OBF3SNDP` program blob, validator and cache entries.
* `bf3_audio` (Godot-free): `PatchInstance`, the A2 interpreter for the 32 node types the weapon patches use
  (AreaType fixed at 0), the Sampler model, DSP (biquads, one-pole, gain ramps, pan, resampler), `MixerCore` (voices
  summed on a 512-frame grid, lock-free command / return / feedback rings from the game thread), WAV I/O, analysis and
  the shared M16A3 check (`audio_check.h`).
* `bf3_godot`: `Bf3AudioStream`/`Bf3AudioPlayback` (an `AudioStreamPlayback` whose `_mix` pulls from `MixerCore`) and
  `Bf3AudioDemo` in `res://scenes/audio_m16.tscn`. The project runs the driver at 48 kHz (`[audio]` in
  `project.godot`). Tuning that the data does not state lives in `bf3-opensource/config/audio_tuning.cfg`.
* `bf3dump audio waves|loc|census|bank|check|render|wave` (see `bf3dump` with no arguments for the usage).

The gates, one command each (from the repo root after `tools\build.cmd dev`; `bf3dump` is
`b\dev\tools\bf3dump\bf3dump.exe`; each ctest wraps exactly that command and checks the threshold):

| gate | ctest | command | pass threshold |
|---|---|---|---|
| G1 | `audio.waves` | `bf3dump audio waves MP_Subway TeamDeathMatch0 --decode` | 909 waves, decoded frames == header frames: 0 mismatches, 0 errors |
| G2 | `audio.loc` | `bf3dump audio loc en --decode` | 4,290 chunks, 15,890 streams, 0 mismatches, 0 errors |
| G3 (timing) | `audio.decode.perf` | `bf3dump audio waves MP_Subway TeamDeathMatch0 --bench 3` | `best_cpu_ms_per_channel_second` ≤ 0.2 |
| G4 | `audio.census` | `bf3dump audio census @all-mp` | 834 patches + 1 master, 32,117 nodes / 46 types, 7,323 voices, 1,879 waves, 164 handheld strict on 32 types |
| G5 | `audio.oracles` | `b\dev\tests\test_audio_install.exe --test-suite=audio-oracles` | plan §7.7 items 1–4 (design §10.2) |
| G6 (timing) | `audio.bank` | `bf3dump audio bank MP_Subway TeamDeathMatch0 --loads 2` | 629 resident waves, 265.4 MiB ≤ 384 MiB, 0 starved, `warm_load_ms` ≤ 500 |
| G7 | `audio.m16.offline` | `bf3dump audio check --g7 --out-dir b\shots\audio` | 38 rows, 0 failed |
| G8 (timing) | `godot.audio` | `"%OPENBF3_GODOT%" --path bf3-opensource res://scenes/audio_m16.tscn -- --audio-check --audio-record=b\shots\audio\godot_m16.wav --quit-after=90` | `AUDIO CHECK: PASS`: rows (a)–(d), ≥ 30.0 s recorded, short/late blocks, late commands, ring overflows, starved voices all 0, offline replay equal sample for sample |

`audio.contract` checks the headers, the node type table, the seam hashes and FourCCs and the tuning file;
`audio.xas.unit`, `audio.patch.unit`, `audio.dsp.unit` and `audio.mixer.unit` need no data; `audio.cache` checks the
cold / warm / patched program cache and `audio.fuzz` feeds mutated chunks, the M16A3 partition and its program blob
(run it under `ctest --preset asan`). Under `asan` the bf3dump gates report their counts and timings without gating
them, but still fail on a non-zero exit.

The offline M16A3 render (G7) and the Godot run (G8) share one check script and analysis (`bf3/audio/audio_check.h`,
design section 10.4): onsets at 75 ms on the stepped corebass tap, levels near > mid > far and the distance high-pass
on the full mix and on the voice-14 tap, and in G8 a sample-for-sample comparison with an offline replay. G8 measures
the mixer's compute time and ring health, not device xruns. Recordings (`m16_offline*.wav`, `godot_m16*.wav`) go to
`b/shots/audio/` (gitignored, never committed); `OPENBF3_AUDIO_SHOTS`, set when CMake configures, overrides the directory the ctests use. The
timing gates (G3 on thread CPU time, G6 warm load, G8) need a quiet machine; timings taken while other work runs are
noisy.

## Integration of S1-S4 and L1 (`integ/spikes`)

<!-- integration (integ/spikes) -->
`integ/spikes` merges, from `main` 111f937 (M1 plus the hygiene track): `spike/s4`, `spike/s3`, `look/l1`,
`spike/s1`, `spike/s2`, in that order, each with its own `--no-ff` merge commit. Every shared file kept both sides
under the comment naming its track. The `dev` build and the touched suites passed after every merge. Then the
orchestrator decisions were applied:

* S2: G2 is report-only and the gate is G2x (see "S2 vehicle replay" and docs/design/s2.md §15.2).
* L1: `godot.l1_look` gates the L1 / M1 ratio at <= 1.25 and the L1 p99 frame interval at <= 16.7 ms (see "L1 look").
* S1: G2 stays a WARN, and its fixes move to M2b and to the terrain-collision fix (docs/design/s1.md §19.5).
* `host.session.unit` was already RUN_SERIAL. It passed in the integrated runs (dev under the quiet lock, and asan),
  so nothing changed.

Integrated tree, 2026-10-16 (Ryzen 7 5800X, RX 9070 XT, 2560x1440). The timing tests ran under the quiet lock while
the other worktrees paused; the rest ran with `-j8`. The accepted numbers are medians of 3 on a quiet machine. Each
integrated number is one run.

| track gate | accepted | integrated |
|---|---|---|
| S4 `logic.metro` G1-G4 | PASS | 549 scopes / 7,299 objects / 145 SyncedBool, 0 unknown / unimplemented, work 95,181; G2 and G3 PASS; G4 hash 843761c3205e74bb, 11,386 records, all three runs equal |
| S4 `logic.types` G5, `logic.caspian` G6, `logic.perf` G7, `logic.census` I1 | PASS, I1 230 / 230 | 14 rows 0 / 0; G6 PASS; G7 p99 0.4 us, busy p99 10 us, max 158 us; I1 PASS |
| S1 `host.scale` G1 p99 tick (gate 8 ms) | 1.27 ms Metro / 0.67 ms Caspian | **1.17 / 0.64 ms**, both runs valid; G3 3.44 / 4.21 queries per step |
| S1 `sim.nav.bake` G4 | Caspian 2.70 s / Kharg 3.70 s | **2.53 / 3.47 s** |
| S1 `sim.nav` G2 (WARN) | Metro 485 / 566, Caspian 661 / 709 rows | the same rows |
| S2 `sim.vehicle` G1 / G1r | 3,600 / 3,600 bit-identical, 0 replay mismatches | the same, on all 7 MP_007 and all 7 synthetic courses |
| S2 G2x (`ExactPod` max, gate 2 cm) | <= 1.91 mm | **1.91 mm** MP_007, 0.75 mm synthetic (PASS) |
| S2 G2 (report only) | 2.25-6.49 cm | the same (slalom, bumps, collision over 2 cm) |
| S3 `godot.s3_crowd` | every gate PASS; G1a p99 0.38 ms, G1b 1.82 ms, frame p99 10.4 ms | every gate PASS; G1a p50 0.229 / p99 0.325 ms, G1b 1.769 ms, frame p99 10.88 ms vsync off / 10.31 ms on, G3 16 / 16 |
| L1 `godot.l1_look` | ratio 1.103 (FAIL against the old 1.10 gate) | ratio **1.073** (2.671 against 2.487 ms), L1 p99 **5.41 ms** (limit 16.7), GPU 1.645 against 1.453 ms (+13 %): PASS |
| L1 `bf3dump looks` albedo / normals | 98.01 % / 99.73 % | 98.01 % / 99.73 % |
| M1 `godot.m1_load` E4 / E5 | 2.36 / 1.82 s | **2.33 / 1.81 s** |
| M1 `godot.m1` | PASS | PASS (`install.unchanged`: 256 files) |
| install unchanged | clean | clean: 1,918 files of the install have the same sizes and mtimes before and after every run |

Full `ctest --preset dev` with `OPENBF3_GODOT` set: 79 of 81 pass. `core.cache_data` and `core.res` are skipped
because their sample data are not set. `host.server` failed once in the parallel phase with 135 ticks in a 2 s sleep
against at most 130. The sleep overshoots under load; the test is untouched by the merges and passed under the quiet
lock (120 ticks, 59.98 Hz). Under `ctest --preset asan`, these 22 suites pass with no AddressSanitizer report:

* the S1-S4 contracts;
* `logic.*`, `sim.nav.unit`, `sim.vehicle.unit` (410 s, with `ExactPod`);
* `host.session.unit`, `host.scale.unit`, `host.registry`;
* `anim.unit`, `anim`, `anim.s3.unit`, `anim.s3`;
* `render.s3`, `render.look.unit`.

<!-- integration (integ/m2a1) -->
**Second integration (`integ/m2a1`, 2026-10-16).** From `main` 7b3e8fa (the integration above), each with its own
`--no-ff` merge commit and in this order: `m2a/design` (docs/design/m2a.md, the M2a wave-2/3 plan, docs only),
`ui/swf-fonts`, `ui/minimap-icons`, `m2a/weapons`, `m2a/vfx` (all branched from 111f937). Every shared file (README,
the CMakeLists, bf3dump's usage and dispatch, register_types.cpp) kept both sides under its track's comment. No code
had to change: the weapons core builds unchanged on S1's `CollisionWorld` / static-world additions (query counter,
terrain cover, water planes; an empty cover keeps M1's cache keys) and S2's vehicle code, and `Bf3MinimapHud` reads
the S1 / S3 / L1 `Bf3LevelSession` through its public accessors and stays inert under `--no-hud`, `--shots` and
`--m1-load-check`, so `godot.l1_look` and `godot.m1_load` still measure the game without it. The `dev` build and the
merged track's suites passed after every merge. Follow-ups from the first integration's verifier, in their own commit:
`host.server` is RUN_SERIAL (thresholds unchanged), the S2 "Measured" paragraph now opens with G2 report-only and G2x
the gate, and `bf3dump vehicle-replay` prints `gate G2x not judged` when `--modes` leaves out the `ExactPod` bit.

Full `ctest --preset dev` with `OPENBF3_GODOT`: 108 of 110 pass, `core.cache_data` and `core.res` skipped as before;
the timing tests ran under the quiet lock. `godot.s3_crowd` failed its timing gates once (G1b 2.26 ms, G2 p99 23 ms)
while another worktree's `ctest` ran `host.scale` beside it without the lock, and passed on the quiet rerun below.
Under `ctest --preset asan`, the 26 suites of the merged tracks (`ui1.contract`, `core.ui.swf*`, `core.ui.font*`,
`ui2.contract`, `ui.*`, `m2a.weapons.contract`, `sim.weapons*`, `tools.weapon_sim`, `fx.*`, `tools.fx_bench`) pass
with no AddressSanitizer report (`sim.weapons.perf` skips there by design). One run each on the integrated tree:

| track gate | accepted | integrated (`integ/m2a1`) |
|---|---|---|
| ui1 `core.ui.swf` G1 / G2 / G7 | 434 / 434 parsed, 0 errors; 162 sub-images, 427 labels; 86 placements | the same |
| ui1 `core.ui.font`, `godot.ui_fonts(_shots)` G3-G5 | 444 / 34 glyphs, É present, 25/25 | 444 / 444 / 34 / 34 glyphs, É present, `UI1 CHECK: PASS (25/25)` both runs |
| ui2 `ui.minimap`, `ui.hud.icons`, `ui.hud.sources` | 29/29 levels, 498 masks, Metro 5/5, Caspian 6/6 + 7/7; 161 icon types; 172/172 weapon icons; 84 / 1,215 / 58 | the same |
| ui2 `godot.ui2` G9-G11, G14 | 10/10, marker within 0.76 px | 10/10, marker 0.76; the six shots (`openBF3\b\shots\integ2\`) inspected: the minimap (satellite, combat area, flags, HQs, own marker, rotating and north-up) and the world icons with distances over the L1 look |
| weapons `bf3dump weapon-sim --check` | 38/38 | 38/38 |
| weapons W11 `sim.weapons.perf` (p99, quiet) | 0.223 ms | 0.208 ms |
| VFX G1 census, G4 oilfire | 1,101 / 3,410, 0 unknown; steady 80 | the same; 80..80 |
| VFX G9 `tools.fx_bench` (gate 50 ns) | 40.08 ns | 39.54 ns |
| S1 `host.scale` G1 p99 Metro / Caspian | 1.27 / 0.67 ms | 1.14 / 0.65 ms, both runs valid |
| S1 `sim.nav.bake` G4 Caspian / Kharg | 2.70 / 3.70 s | 2.54 / 3.47 s |
| S2 `sim.vehicle` G1 / G1r / G2x | 0 mismatches; `ExactPod` <= 1.91 mm | 0 mismatches on all 7 courses; 1.91 mm |
| S3 `godot.s3_crowd` (quiet rerun) | G1a p99 0.38 ms, G1b 1.82 ms, frame p99 10.4 ms | G1a p99 0.44 ms, G1b 1.92 ms (gate 2.0), frame p99 10.81 / 11.16 ms vsync off / on, G3 16 / 16 |
| S4 `logic.metro`, `logic.caspian` | PASS | 549 / 7,299 / 145, work 95,181, G4 hash 843761c3205e74bb; G6 PASS |
| L1 `godot.l1_look` (ratio <= 1.25, p99 <= 16.7 ms) | 1.073, 5.41 ms (first integration) | 1.071 (2.686 against 2.506 ms), 5.40 ms |
| M1 `godot.m1`, `godot.m1_load` E4 / E5 | PASS; 2.36 / 1.82 s | 16/16; 2.38 / 1.89 s |
| install unchanged | clean | clean (256 files in every install test) |

<!-- integration (integ/au) -->
**Third integration (`integ/au`, 2026-10-16).** `m2a/audio` (branched from 111f937) merged with `--no-ff` onto `main`
1874933 (the integration above). Shared files kept both sides, the main tree's blocks first and audio's after them; no
code had to change (audio's `bf3_godot` classes are new files, registered after ui2). One follow-up:
`OPENBF3_AUDIO_SHOTS` is passed through `file(TO_CMAKE_PATH)`, so the Windows path the design names works
(it reached `audio_gates.cmake` as invalid escapes). Full `ctest --preset dev` with `OPENBF3_GODOT`: 123 of 125 pass,
`core.cache_data` and `core.res` skipped as before; the timing rows ran under the quiet lock, the rest with `-j8`.
`ctest --preset asan -R "^audio\."`: 14 / 14, no AddressSanitizer report. Integrated against accepted: G1 909 waves,
12,955 segments, 0 frame / length mismatches; G2 4,290 chunks, 0 mismatches; G3 0.1301 / 0.1295 / 0.1295 ms per
channel-second (accepted 0.1306, gate 0.2); G4 834 patches + 1 master, 32,117 nodes / 46 types, 7,323 voices, 1,879
waves, 0 compile failures; G5 6 / 6 cases, 314 / 314 assertions; G6 warm load 57.5 ms (one run; accepted median 51.3,
gate 500); G7 38 / 38; G8 windowed 31 / 31, every mixer counter 0, replay bit-exact, max block 0.343 ms. Main-tree
gates: S1 `host.scale` G1 p99 1.18 / 0.75 ms (valid) and `sim.nav.bake` 2.53 / 3.60 s; S2 `sim.vehicle` G1 / G1r 0
mismatches, G2x 1.91 mm; S3 `godot.s3_crowd` PASS (G1a p99 0.42 ms, G1b 1.86 ms, frame p99 11.4 / 10.7 ms, G3 16 /
16); S4 `logic.metro` G4 hash 843761c3205e74bb; L1 `godot.l1_look` 1.085 and 1.064 (p99 5.7 / 6.0 ms); M1
`godot.m1` 16 / 16, E4 / E5 2.40 / 1.84 s; ui1 25 / 25; ui2 10 / 10 with the six shots (`openBF3\b\shots\integau\`);
`weapon-sim --check` 38 / 38, W11 p99 0.263 / 0.190 ms; VFX `fx.install` PASS, `tools.fx_bench` 38.5 ns; install
unchanged in every install test. What a listener should know: G1 says 909 Metro waves where the M2a plan says 908 (this
tree's `load_level` mount, a scope difference, open question 7); under reading R of the MultiCrossfader the M16A3's far
noise layer fades **in** over 20-50 m and every layer fades **out** over 250-300 m, not "fades in over 250-300 m" as the
plan words it (open question 1); and without the HDR window (A3) levels are low: a fixed window puts a 110 dB shot at
-11 dBFS before attenuation, so the mid and far M16A3 shots sit near -69 and -90 dBFS.

<!-- integration (integ/w2) -->
**Fourth integration: M2a wave 2 (`integ/w2`, 2026-10-17).** On `m2a/w2` f4bbc43 (the W2.0 contract with nt and lc
merged), each with its own `--no-ff` merge commit and in this order: `m2a/cl` 9a99e81 (it contains `m2a/sv`), `m2a/vm`
48c22ca, `m2a/fx2` ca561e9, `m2a/hud` 60cf30d, then `main` 925cd51 (`m2a/audio` A1 / A2, `terrain/collision`, the
S5 harness). The `dev` build passed after every merge. Every text conflict was two tracks' blocks appended at the same
place in a shared file (README, the CMakeLists, bf3dump's usage and dispatch, register_types.cpp); both were kept, the
earlier merge's first. m2a.md's adjacent E18 (lc) and E19 (vm) rule fixes were both kept. The one code conflict,
`level_load.h`, was TC's removal of S1's `terrain_cover` / `cover_water` next to sv's new gunplay fields; both kept, no
other change was needed (sv's gunplay, `find_sky_lane` and cl's Metro rows run on TC's whole-terrain world and give the
accepted numbers). Follow-ups in their own commits: m2a.md §15 now names sv's eight `game_server.cpp` insertion points
and accepts cl's `rewind_rows` lines in sv's `gunplay.{h,cpp}` (rule fix), and `tools\build.cmd` lists
`host.gunplay.client` among the timing rows. The merge resolves vm's `PENDING (sv)`: with sv's `step_weapons_m2a` the
sim now refuses zoom too (0 of 6,882 frames set `kZoomWasDown`, against 6,882 on vm's branch), and `m2a.contract`
prints no PENDING row.

Full `ctest --preset dev` with `OPENBF3_GODOT`: 142 of 144 pass, `core.cache_data` and `core.res` skipped as before
(114 rows with `-j8`, then the 30 timing rows one at a time in four quiet-lock windows, `host.gunplay.client` with
`OPENBF3_CL_TIMING=1`). Every row passed on its first run, `godot.s3_crowd` included. Under `ctest --preset asan`,
`m2a.contract`, `net.proto.unit`, `net.proto.fuzz`, `sim.lagcomp.unit`, `anim.server_pose`, `sim.weapons.fuzz`,
`host.gunplay.server`, `host.gunplay.client.unit`, `anim.viewmodel.unit`, `fx.render.unit`, `ui.hud.feed.unit` and
`ui.hud.feed.install` pass (12 / 12), with no AddressSanitizer report. One run each on the integrated tree (wall-clock
rows are noisy on this shared machine):

| track gate | accepted | integrated (`integ/w2`) |
|---|---|---|
| nt `net.proto.unit` sizes, `net.proto.fuzz`, `m2a.contract` | 64 players: Snapshot 1,195 B / 46 + SnapshotOthers 387 B / 17; fuzz 0 unstable; 7 / 7, PENDING (sv) only | the same; 7 / 7 (5,094 assertions), no PENDING |
| lc E18 (a) `bf3dump lagcomp --deviation` | 10 / 10 rows; worst transition head / chest 2.47 / 3.09 cm | the same numbers, 0 stand-in rows |
| lc E13 `bf3dump lagcomp --perf` (p99 <= 0.5 ms) | pairs 0.338, long 0.354 ms | pairs 0.298, long 0.205 ms |
| sv `host.gunplay.server` E1-E6, E17 | 31 shots / 30 gaps of 4,500,000; drop 0.1780 / 1.5993 m, expiry 975.0 m; STK 4 / 2 and 6 / 3; reloads 108,000,000 / 142,200,000; KS D 0.0158; hostile 0 / 900 | the same; hostile tick p99 0.113 ms (control 0.120) |
| cl E1 / E5, E12, E15 (`host.gunplay.client`) | 31 / 31, 7 / 7; gate 100 % (zone 100 %), control 2.40 %; loopback 0 / 0 reconciliations; 212 / 212 seeds | 31 / 31, 7 / 7; gate 500 / 500 (zone 100 %, (b) 500 / 500 equal), control 2.00 %, stress 40 / 40; loopback 600 s 0 motor / 0 weapon reconciliations, 422 server shots all rewound; 208 / 208 seeds |
| vm `godot.m2a_viewmodel`, `vm skin`, `vm clips` | 20 / 20; FOV worst 1.00 frame, recoil 7.91e-6°, springs 0.736 s; 0 stretched; E11 32 / 32 | the same |
| fx2 `godot.fx_demo`, `godot.fx_ref` (Q17, <= 1.25) | E8 (3) all frames, (6) 52 / 52, (4) 80; 64 lights; FX p99 0.225 ms; 1.015 | 3,667 / 3,667 frames, 52 / 52, 80 / 80; 64 lights; FX p99 0.213 ms; 0.775 / 0.752 = 1.031 |
| hud `godot.m2a_hud`, `ui.hud.feed.*` | 32 / 32 at three sizes; gap error 0.0000 px; 36 / 36 hits; 12 / 12 names and icons | the same |
| S1 `host.scale` G1 p99 Metro / MP_007; `sim.nav.bake` G4 | 1.18 / 0.75 ms; 2.53 / 3.60 s | 1.49 / 1.03 ms (both valid); 2.82 / 2.54 s |
| S2 `sim.vehicle`; S4 `logic.metro` G4 | 0 mismatches; 843761c3205e74bb | 0 mismatches on all courses; 843761c3205e74bb |
| S3 `godot.s3_crowd` | G1a p99 0.42, G1b 1.86 ms, frame p99 11.4 / 10.7 ms | first run PASS: G1a p99 0.43, G1b 1.80 ms, frame p99 13.8 / 13.2 ms, G3 16 / 16 |
| L1 `godot.l1_look`; M1 `godot.m1`, `godot.m1_load` E4 / E5 | 1.085, p99 5.7 ms; 16 / 16, 2.40 / 1.84 s | 1.061, p99 5.87 ms; 16 / 16, 2.40 / 1.82 s |
| wp `weapon-sim --check`, W11; VFX census, `tools.fx_bench` | 38 / 38, 0.263 ms; 1,101 / 3,410, 38.5 ns | 38 / 38, 0.219 ms; 1,101 / 3,410, 39.6 ns |
| ui1 / ui2; audio `godot.audio`; TC `sim.terrain` | 25 / 25; 10 / 10; 31 / 31; G1 230 runs | 25 / 25; 10 / 10 with the six shots; 31 / 31; 230 runs, 0 failed |
| install unchanged | clean | clean (256 files in every install test) |

Captures for the user's eye (gitignored) are in `openBF3\b\shots\integw2\` (fx2, hud, s3, ui2, audio WAVs). What
wave 3 still owes: E7 (audio through the game mixer, au3), E8 in the game and E9's firefight budget (fx3's triggers,
`godot.m2a_firefight`), E11 (the 4 kits in both teams' loadouts in the game), E13's `host.gunplay.perf` row with 64
firefight bots (bt), E14 (the client frame budget in the firefight), E16 (load and memory with weapons on
`m2a_metro.tscn`), E19's `godot.m2a` case "ads", and the gx Godot wiring:
`Bf3Viewmodel`, `Bf3Hud` (with `Bf3UiAssets` given `Bf3Hud::required_atlas_movies()`), `Bf3FxRenderer`, audio and the
3p crowd fed from cl's `present_weapon` / events in `level_session`, and `SharedLoadOptions::gunplay` turned on for the
M2a scene (it stays off by default, so M1's E4 / E5 are unchanged).

<!-- integration (integ/w3) -->
**Fifth integration: M2a wave 3 (`integ/w3`, 2026-10-19).** On `m2a/w3` 494b2df (the W3.0 contract with bt, tp, au3
and fx3 merged; its own merge is a no-op), each with its own `--no-ff` merge commit: `m2a/gx` 3203a74, then `main`
5d5ed94 (the E15 loopback length in `dev-timing`). Neither merge had a conflict. The branch pass
(`tools\build.cmd dev --strict`) passed after every change: 137 rows, 135 passed, `core.cache_data` and `core.res`
skipped as before, 0 failed, 0 STRICT violations (185-193 s). The first merge pass (`main-timing.ps1`, quiet lock)
passed realtime 10 / 10 and timing 16 / 16 and failed two window rows; both are fixed here, each in its own commit.
* **E16** (`godot.m2a_load`): the warm load missed (first controllable frame 6.40 s, limit 5 s). `Bf3GameCrowd::load`
  ran 2.9 s on the loader after the SharedLevel. tp's `load_body` (soldier assets, stance and move clips, with the
  1.1 s Ant source open on a second thread) now runs on its own thread beside `load_shared_level`, and `load()` adds
  only the 3p weapons (0.3 s). Under the lock: cold 5.31 s, warm 4.27 s, peak working set 1,625 MiB.
* **`godot.m2a.play`** failed `play.screenshots` 4 / 5 in both merge passes: no bot was framed for the ADS shot in the
  first and for the firefight shot in the second (every other play row passed). In the last 60 s each shot may now
  be taken unframed, and the row names that fallback. Rerun under the lock: PASS, 5 / 5, both shots framed (28.1 m
  and 27.9 m).

The second merge pass on the E16 fix again passed realtime and timing (`host.gunplay.client` 448 s) and 15 of the 16
window rows. Its one failure was the play row fixed above, which then passed alone under the lock. Gate numbers from
one quiet-lock tenure: E13 `host.gunplay.perf` tick p99 1.688 ms (gate 8.0), weapons + projectiles + rewind 7.5 % of
the mean tick, 64 / 64 bots fired. E9 / E14 `godot.m2a_firefight` (64 bots, 2560 x 1440): FX main p99 0.48 ms, FX GPU
0.18 ms, at most 3 lights and 1,568 particles, frame p99 6.23 ms, M2a main p99 0.88 ms, crowd step p99 0.20 ms, 0
underruns, 0 overflows. m2a.md §3's lane re-measure (`s3_crowd.tscn --s3-lanes=4`, the session's listen server idle,
`--walkers=0`) passed S3 CHECK: G1a p99 0.383 ms (limit 2.0, so the 4-lane rule stands), G1b 1.568 ms, G2 p99 9.52 /
9.82 ms, G3 16 / 16. `godot.s3_crowd` on S3's own 6 lanes passed in both merge passes. Under `ctest --preset asan`,
17 / 17 wave-3 Godot-free rows passed with no AddressSanitizer report: `m2a.contract3`, `host.gunplay.firefight` /
`.replay` / `.perf`, `s3.contract`, `anim.s3.unit`, `anim.s3`, `render.s3`, `render.crowd_weapons`,
`anim.server_pose`, `audio.router.unit`, `audio.game`, `audio.game.crowd`, `fx.triggers.unit`, `fx.install.cells`,
`fx.unit` and `fx.render.unit`. The install was unchanged (256 files) in every install row. Other changes:
`godot.m2a_firefight` COST is now 80, `godot.m2a.play` is in test_budget's `gate_length_rows`, and the
`tools<BS>uild.cmd` typo in fx3's notes is fixed. Captures (gitignored): `openBF3\b\shots\gx\` and
`openBF3\b\shots\integw3\s3_lanes4\`.

<!-- integration (integ/m2b-w1) -->
**Sixth integration: M2b wave 1 (`integ/m2b-w1`, 2026-10-19).** On `m2b/w1` 883c053, which holds the W1.0 contract
and nt, au, ru and bb merged. `main` 36eb5af is already in it, so its `--no-ff` merge was a no-op. The branch pass
(`tools\build.cmd dev`) passed: 146 rows, 144 passed, `core.cache_data` and `core.res` skipped as before, 0 failed,
203 s. The wave-1 merge pass ran the `timing` phase only (m2b.md §11.1 step 3), under the quiet lock: 18 rows, 16
passed. Two fixes came out of it, each in its own commit:
* **E13** (`host.tdm.client`): the row failed with bb's bots, in its script. The scripted fall now drops 46 m onto
  M1's open-sky E1d spot (`sim::find_drop_spot`), because 60 m above a spawn point is inside Metro's ceiling. The row
  runs 31 bots instead of 7, because 7 bots reached only 3 / 1 of the kill limit 5 in 30 s. Under the lock it passes
  in 26.2 s: respawn = death + 300, RoundOver after 11.3 s of round.
* **E8** (`host.tdm.perf`): it failed on perception p99 at 0.257 ms against a gate of 0.2. The named insertion
  `[ray any]` adds `CollisionWorld::ray_any`, an any-hit query with the same filters, and perception's
  line-of-sight rays use it. Bot behaviour is byte-identical: E1 RoundOver at tick 17,634 at 100 / 95, E2 pooled
  149 / 131. Re-measured under the lock: perception 0.180 ms, brains 0.298 ms, tick 1.319 ms (p99).

**E11** (`audio.game.underruns`) failed on the mix CPU share: p99 in the < 0.20 bucket against <= 0.10. Four named
insertions halve the mix cost without changing a sample (the 64-shooter output hash is equal before and after):
`[xas groups]` (the XAS decoder's group predictors run together, in SSE2 lanes), `[slot fast]` (the sampler's resampler
keeps its state in registers), `[lazy zero]` (silent voices are neither zeroed nor scanned) and `[biquad pair]`.
Re-measured under the lock, E11 passes: p50 and p99 < 0.08, max block 1.505 ms, 0 late blocks, 128 live voices at
most, 4.05 s for the 66 s render (was 8.64 s). `audio.decode.perf` and `audio.bank` pass, and the audio rows pass in
dev and under asan. The play check (user away, 2 minutes each, `--m2a-check=play --m2a-play-seconds=120`) ran before
these insertions; they leave the mix bit-identical:
* **M2a scene**: bots fire (6,362 remote shots), a fall death respawns, 120.9 s of audio mixed (12 late commands,
  reported), 0 event overflows, and the install is unchanged. The fall, team-switch and kit rows fail only because
  the case's 300 s schedule does not fit in 120 s.
* **`--tdm` preview** (31 TDM bots, `--kill-limit=30 --preround=3`): the `[m2b] match:` lines show pre-round, then
  in round, then a revive taking a kill back (RU 12 to 11), then `round over, US 30 - RU 24 of 30, US wins`, with
  bots fighting and respawning throughout.

Also: bb's two contract additions and au's sampler-voice cap are confirmed (m2b.md `## M2b integ-w1`), and a
backspace byte in bb's README row is fixed. Logs are in the scratchpad `m2b-integ-w1\`; captures (gitignored) are in
`openBF3\b\shots\integm2bw1\`.

<!-- integration (integ/m2b-w2) -->
**Seventh integration: M2b wave 2 (`integ/m2b-w2`, 2026-10-19).** On `m2b/w2` 4a7e9f4 (the W2.0 contract with ui,
cam and gx merged); `main` 545640c was already in it, so its `--no-ff` merge was a no-op. The branch pass
(`tools\build.cmd dev`) passed: 151 rows, 149 passed, `core.cache_data` and `core.res` skipped as before, 0 failed,
199.8 s. The wave-2 merge pass ran the `window` phase only (m2b.md §11.1 step 5; no timing row executes a file the
wave changed), under the quiet lock with the user away: 15 / 15 rows passed, among them E9 `godot.m2b.play` (59.7 s),
E12 `godot.m2b_load` (warm 4.27 s, limit 5 s) and `godot.m2a_firefight` (E14's 0 underruns with the A3 mixer). The
play check through `tools\play.cmd` (one scripted round, `--kill-limit=15`, the 10 s pre-round) passed 14 / 14 on its
second run: the spawn screen at join, two deploys with the chosen kits, two own deaths with the kill card and the
death view, a revive by a bot, 22 feed lines for 22 deaths, the scoreboard equal to MatchState in 151 Tab samples, and
the round end at US 15 - RU 5. Two changes, each in its own commit: `[revived seek]` (m2b_check.cpp: the first play
run failed `play.deploys` because a bot revived the player and the script stopped seeking) and `[passed output]`
(timing_pass.cmake keeps a passed row's whole output in the junit, which had cut E9's and E14's numbers). Every exit
criterion E1-E14 with its value and log: [docs/design/m2b-results.md](docs/design/m2b-results.md). Open: with the
play defaults (31 bots, spawn screen) the load to the spawn screen is 5.8-6.0 s, over E12's 5 s, which the row times
without bots; the minimap covers part of the spawn screen's ASSAULT card; the M1 debug overlay still shows. Logs are
in the scratchpad `m2b-integ-w2\`; captures (gitignored) are in `openBF3\b\shots\integm2bw2\`.

<!-- integration (integ/pt1) -->
**Playtest 1 fixes (`integ/pt1`, 2026-10-19).** From `main` 6a4061e, each with its own `--no-ff` merge commit, in
this order: `pt1/snd` (sound glitching), `pt1/rld` (reload sounds), `pt1/iff` (friend or foe), `pt1/run` (the running
animation); see the sections "PT1 snd" to "PT1 run" below. The only code conflict was snd's and rld's end blocks in
`patch_instance.cpp` / `.h` (both kept). The rest were end blocks of `tests/CMakeLists.txt` and this README, also
both kept. `audio.foley.install` on the merged mixer: the reload after a burst is still -19.3 dBFS (alone -19.2). The
30-round burst's loudest 20 ms went from -0.5 to -5.7 dBFS, because snd's limiter now catches the burst's float peaks
of about +6 dBFS. One addition, in its own commit: `[foley stats]` (game_audio.cpp prints a `[pt1] weapon foley:`
line after the audio health line, the play check's reload evidence). The branch pass (`tools\build.cmd dev`) passed:
158 rows, `core.cache_data` and `core.res` skipped as before, 0 failed, 208 s. The merge pass, under the quiet lock
with the user away, ran the rows the diff reaches:
* timing: `audio.game.underruns` (E11), passed: p99 under 0.08 against the 0.10 gate, 0 late blocks, max block 1.25 ms;
* window, 8/8 passed: `godot.s3_crowd`, `godot.ui2`, `godot.m2a_hud`, `godot.audio` (31/31), `godot.m2a_firefight`
  (E14: 0 underruns, frame p99 7.4 ms), `godot.m2a_load` (warm 4.44 s), `godot.m2b.play` and `godot.m2b_load` (E12
  warm 4.52 s, limit 5 s).

No other timing row runs a changed function. The play check (`tools\play.cmd --m2b-check=play --kill-limit=20`, the
10 s pre-round) passed 14/14. The round ended at 70.4 s, US 20 - RU 17. Its numbers:
* audio, the health line: 0 drop / pend / kill / resume cuts above -60 dBFS, 0 late blocks (max block 2.45 ms, audio
  thread priority 0 -> 2), and 582 frames over 0 dBFS before the limiter, 0 after;
* audio, `tools\audio_capture_check.py` on the 73.7 s `mix.wav`: 0 cuts visible in the mix, 0 flapping loops;
* reloads: 30 reloads and 40 foley one-shots;
* friend or foe, per frame: the minimap showed 13.1 friendly and 6.4 revealed enemy arrows (the M1 loop drew 27.4
  soldiers as enemies); 11.0 friendly icons, 1.79 friendly names and 0.040 enemy names;
* the crowd loaded BF3's run, sprint and sneak move groups and 8 (team, kit) meshes.

Open: 8 main-thread hitches over 50 ms (max 209 ms), and the load to the spawn screen with 31 bots is still 5.70 s.
The HUD finding is not in this integration. Logs are in the scratchpad `pt1-integ\`; the captures (mix.wav, cuts.csv,
5 screenshots; gitignored) are in `openBF3\b\shots\pt1-integ\play\`.

## M2a wave 2 contract (W2.0)

<!-- # M2a w2 -->
This is the integrator's contract commit for M2a wave 2, on branch `m2a/w2` from `main` 1874933. Design: [docs/design/m2a.md](docs/design/m2a.md)
§15. As-built notes, the pending-row convention and the deviations from the sketches:
[docs/design/m2a-w2.md](docs/design/m2a-w2.md).

The commit declares every header of the wave-2 interfaces column and the additive fields of the M1 and wp structs
(details in m2a-w2.md). It also adds `tests/gunplay_fixture.h` and the `m2a.contract` ctest.

* **Bodies are left to the tracks.** The only body is a forwarding stub for `step_weapons_m2a`.
* **Command pins.** `Command` is 24 B. No M1 test pinned its size or a command hash, so no M1 pin changed.
* **Pending rows.** The `m2a.contract` rows that need sv's R3 / `weapon_rules.cpp` or nt's v2 codecs print
  `PENDING (<key>)` until those tracks set `kSvRulesLanded` / `kNtLanded`. The §7.2 ack table runs now against a
  test-local reference.

Measured 2026-10-16 on the dev preset, with `OPENBF3_GODOT` set:

* **Full ctest, timing rows excluded** (`-E` godot.*, host.session*, host.server, host.scale*, sim.nav.bake,
  sim.weapons.perf, tools.fx_bench; `-j 6`): 90 of 92 passed, and `core.cache_data` and `core.res` were skipped as
  before.
* **Two timing rows failed under that parallel load.** `core.ui.swf.unit` took 1.16 s on a hostile case (limit 1 s) and
  `core.ui.font` took 1.81 s on its cold load (limit 1 s). Both passed when rerun alone.
* **Timing rows, one at a time under the quiet lock:** every row passed.
  - `host.session.unit` and `host.session` failed their clock-target check once (4 and 5 target changes, limit 2).
  - An A/B run of the E2 case then gave 0 target changes in both builds, twice each: main 1874933 (`wt/integ2`'s build)
    and this branch.
  - A rerun under a fresh lock passed both rows, with 0 target changes and install unchanged.
* **`m2a.contract`:** 7/7 cases pass on dev and asan, with no AddressSanitizer report. `m1.contract`, `net.proto.unit`
  and `m2a.weapons.contract` also pass on asan.

Timing rows are wall-clock and noisy on this shared machine.

## M2a protocol v2 (nt)

<!-- # M2a nt -->
Track nt of M2a wave 2 is on branch `m2a/nt` from the W2.0 contract. It writes the bodies of protocol v2
(`kProtocolVersion` 2) in `bf3_net`. The changes are the MatchInfo / Spawned / Debug / DebugReport fields, the
command view sample, `others_ack_bits` and the two ack predicates, the own weapons and the others' weapon fields in
the Snapshot, `SnapshotOthers`, `ShotEvents`, `CombatEvents` and `KitSelect`, plus the new `weapons_wire.cpp`.

Design: [docs/design/m2a.md](docs/design/m2a.md) §7. As built, with the wire layouts, measured sizes and
precision, deviations and the updated M1 rows: [docs/design/m2a-nt.md](docs/design/m2a-nt.md).

* **Sizes:**
  - Command packets are 33 / 70 B.
  - The Snapshot header with the own state is 97 B, and a 4-slot kit's weapons add 75 B.
  - An other is 178 bits. 49 others fit beside the own state, and a SnapshotOthers holds 53.
  - A 64-player match goes in a Snapshot (46 others with a kit) plus a SnapshotOthers (17), both ≤ 1,200 B.
  - A datagram holds 69 untraced shots or 58 traced ones.
* **Tests:**
  - `net.proto.unit` has an `M2a nt` block with round trips at the documented precision, sizes, the split, acks
    and the weapons wire.
  - `net.proto.fuzz` is new. It runs seeded mutations and random bytes of all 15 message types, counting
    refusals and checking decode → encode → decode stability; 3,501 / 3,501 structured malformed cases are
    refused.
  - `m2a.contract` runs its nt rows (`kNtLanded = true`).
* **M1 rows pinned to the v1 wire were updated to the v2 values.** Spawned is 85 B, command packets 33 / 70 / 33 B,
  a Snapshot holds 49 / 53 others, there are 15 known types, and Debug kind 8 is refused. Each row still checks the
  exact value.

Run: `ctest --preset dev -R "^(net\.|m2a\.contract$)"`; the host rows (`host.server`, `host.session*`) and the Godot
rows are timing rows, run under the quiet lock. Timing rows are wall-clock and noisy on this shared machine.

## M2a lc: lag compensation and hit capsules

<!-- # M2a lc -->
Design: [docs/design/m2a.md](docs/design/m2a.md) §6; as built, the contract fixes and the measured gates:
[docs/design/m2a-lc.md](docs/design/m2a-lc.md). The server's rewind (`bf3_host` `LagHistory` / `RewindProvider`,
`bf3/host/lag_comp.h`) poses each target from exactly the samples the shooter's client had (its acked Snapshot and
SnapshotOthers records), through the client's own sampler (`bf3_net` `sample_other`, `other_sample.h`), at
`rewind_time_q8 = sim_time_q8(t) - clamp(view_back - 256, 0, 21 x 256)`. Hit capsules come from the server pose
(`bf3_anim` `build_capsule_pose_table`: the 3p rig per stance at 33 pitch buckets and per transition at 16 fractions,
`bf3_sim` `posed_target`). wp's ballistics fill the query segment (R4) for the swept-AABB broad phase.

Commands (read-only; output is BF3 data, keep it out of the repo; each ends with `install.unchanged`):

```bat
b\dev\tools\bf3dump\bf3dump.exe lagcomp MP_Subway TeamDeathMatch0              :: the table, capsule placement readings, rig vs EBX
b\dev\tools\bf3dump\bf3dump.exe lagcomp MP_Subway TeamDeathMatch0 --deviation  :: E18 (a) rows, "E18 (a): PASS|FAIL"
b\dev\tools\bf3dump\bf3dump.exe lagcomp MP_Subway TeamDeathMatch0 --perf       :: E13 rows pairs / long (under the quiet lock)
```

Gates, measured 2026-10-16 on the dev preset (E13 is wall clock and **noisy** on this shared machine):

| gate | command | threshold | measured |
|---|---|---|---|
| E18 (a) | `ctest -R anim.server_pose` (install) | head / chest ≤ 3 cm steady (32 mid-pitches), ≤ 5 cm transitions (15 mid-fractions × 32); stand ≤ 3 cm of `CrowdEvaluator`; 0 stand-in rows | worst steady 0.18 cm, worst transition 3.09 cm (stand→prone chest), crowd 0.013 cm; 10/10 rows, 0 stand-in |
| E12 (a) | `ctest -R sim.lagcomp.unit` | server lists and samples bit-equal to the client's; cache on == off; window/track differences all classified | 8,900 rows, 0 differences; 17,636 provider rows equal; 1,438 Q22 rows, 0 unexplained |
| E13 | `bf3dump lagcomp MP_Subway TeamDeathMatch0 --perf` | provider p99 ≤ 0.5 ms per tick | pairs 0.338 ms, long 0.354 ms |
| R4 | `bf3dump weapon-sim --check` | 38/38, sim.weapons* bit-equal | 38/38; `--check` and `--all` output byte-equal to the build without R4 |

## M2a cl: client prediction and events

<!-- # M2a cl -->
Design: [docs/design/m2a.md](docs/design/m2a.md) §6.1–§6.3, §7, §8; as built, the contract fixes and the measured
gates: [docs/design/m2a-cl.md](docs/design/m2a-cl.md). The client predicts its own weapons with the server's code
(`bf3_sim` `WeaponLane`, stepped after the motor in prediction and in `catch_up`, first predictions only emit),
reconciles them by the Snapshot's own weapons block, rebases a health-only difference without a replay (R2), refuses a
server whose `weapon_hash` (or kit hash) differs before sending Ready (E6), sends the drawn frame's view sample and
`others_ack_bits` in every command, and hands the main thread `PresentEvent`s (`bf3/host/presentation_events.h`, an
SPSC ring with Predicted / Render / Immediate timelines) and the two latest predicted weapon views
(`present_weapon`, `bf3/host/presentation_weapon.h`) for vm, hud, fx3 and au3. Cosmetic projectiles
(`bf3/host/client_projectiles.h`) test own shots against the rewound records the server will use and fly remote shots
on the render timeline. `present_others` now calls lc's `net::sample_other` (R1). The client builds its gunplay inputs
(table, capsules, kits, shot frame) from sv's `SharedLevel::gunplay` by default. Every end-to-end case runs against the
real `ServerHost` with sv's `Gunplay`, with real clients as bots: the gates on Metro (read-only on the install, exit 77
without one), shorter install-free variants on a synthetic floor in the unit row.

Commands (the unit row needs no install; the Metro gates run only with `OPENBF3_CL_TIMING=1`, under the quiet lock):

```bat
ctest --preset dev -R host.gunplay.client.unit                     :: the branch pass's half: events, present_weapon, lane, projectiles, view sample (.pure)
ctest --preset dev-timing -R "^host\.gunplay\.client\.unit$"       :: the timing half (60 Hz loopback): E6, R2, SnapshotOthers, hostile, floor variants
ctest --preset dev -R host.session.unit                            :: M1's socket-free rows plus R1's equivalence (.pure; the loopback half is a timing row)
set OPENBF3_CL_TIMING=1
b\dev\tests\test_gunplay_client.exe --test-suite=host-gunplay-client  :: Metro: E1 + E5, E12 lagcomp (+ control, stress), E15 loopback (10 min), two clients, install unchanged
```

The merge pass runs `host.gunplay.client` through the `dev-timing` preset, which sets `OPENBF3_CL_LOOPBACK_S=60`
(E15's loopback plays 5 cycles of its 12 s script instead of 10 minutes, with the same checks); the direct command above
keeps the full 10 minutes for the M2a gate.

Gates, measured 2026-10-17 on the dev preset (the timing cases are wall clock and **noisy** on this shared machine):

| gate | threshold | measured |
|---|---|---|
| E1 (client, Metro) | predicted shots = server's field for field 31/31 | 31/31 (the install's M16A3); 30/30 gaps of 4,500,000; 36 predicted = 36 server shots |
| E5 (client, Metro) | server phase events = predicted | 7/7; empty 2.37 s → 30, tactical 1.8 s (commit +1.53 s) → 31 |
| E6 (client) | "weapon table mismatch", `weapon_hash_mismatch` 1, no Ready, no prediction | yes for a variant cfg, a missing cfg and a client without inputs (never spawned on the server); "kit table mismatch" for a kit hash |
| E12 (b) (Metro) | server hit ≥ 99 % of client hits, same zone ≥ 99 %; samples bit-equal except `late_sample`; control ≤ 50 % | 500/500 (100 %), zone 100 %; 500/500 samples equal to the server's rewound rows, `late_sample` 0; control 2.40 %; stress 29/29 (all clamped, reported) |
| E15 (Metro) | 0 weapon and 0 motor reconciliations in 10 min loopback; equal seeds for two clients | 0 and 0 (bots on the player's team: 0 health rebases); 212/212 seeds equal |
| R1 / R2 | M1's drawing unchanged; a hit's writes rebase | 150,196/150,196 bit-equal; 3 enemy hits, 3 rebases, 0 reconciliations |

## M2a sv: server gunplay

<!-- # M2a sv -->
Design: [docs/design/m2a.md](docs/design/m2a.md) §4, §5, §6 (server side), §10.1, §13, §14.1; as built, the
decisions and the measured gates: [docs/design/m2a-sv.md](docs/design/m2a-sv.md). The `GameServer` runs weapons,
projectiles, damage, deaths and kits when the level carries gunplay tables (`SharedLoadOptions::gunplay`, off by
default; `bf3_host` `Gunplay`, `bf3/host/gunplay.h`): every live soldier's weapons step after its motor step
(`step_weapons_m2a`), all projectiles fly once per tick through lc's `RewindProvider` (clients' shots against the
targets as that client drew them, server-side shooters at the present), damage is applied in (time, shooter, shot,
pellet) order, and deaths go out as `CombatEvents` through a `CombatEventSink` with the respawn decided by a
`RespawnPolicy` (M2b's seams). Snapshots carry the own weapons and the others' weapon fields, the overflow goes in a
`SnapshotOthers` of the same seq, the other shooters' shots in `ShotEvents`. `MatchInfo` carries the match seed,
`weapon_hash` (table ^ capsule table), `weapon_flags` (exact aim, infinite magazine; dev only) and `kit_hash`. Dev
requests `SpawnTarget` / `TargetInfo` place stationary target bots and return their server capsules.

Commands (read-only on the install; the cases print BF3-derived numbers, keep the logs out of the repo; the last case
prints `install.unchanged`). Every case runs the real `ServerHost` and `ClientSession` on Metro headless, except the
hostile case and E6's offset sample, which drive the `GameServer` without sockets:

```bat
ctest --preset dev-timing -R "^host\.gunplay\.server$"                                            :: all cases (a timing row, RUN_SERIAL, ~2-5 min)
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=timing      :: E1
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=drop        :: E2
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=stk         :: E3, E4
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=reload      :: E5
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=variants    :: E6
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=hostile     :: E17 (also under the asan preset)
b\dev\tests\test_host_gunplay_server.exe --test-suite=host-gunplay-server --test-case=install     :: E17 install unchanged
```

Gates, measured 2026-10-17 on the dev preset (server halves; the client halves are cl's `host.gunplay.client`;
wall-clock pacing, **noisy** on this shared machine):

| gate | threshold | measured |
|---|---|---|
| E1 | M16A3: 31 shots, first at the press tick's start, 30 gaps of 4,500,000, trace = server shots; other kit firearms: the table interval; semi one shot per tap | 31 / 30 / yes / 31 of 31 bit-equal; M4A1, M27 IAR, AK-74M, AKS-74u, RPK-74M at their intervals; MK11, SVD, M9, MP443 5 / 5 |
| E2 | 0.1779 ± 0.01 m at 100 m, 1.5981 ± 0.01 m at 300 m, expiry 975.0 ± 0.01 m | 0.1780-0.1786, 1.5993-1.6039, 975.0000 (sky lane in bounds 2,197.5 m) |
| E3 | M16A3 chest 4 / head 2 at 5 m, chest 6 / head 3 at 60 m; 25 / 50 / 22.75 per hit | 4 / 2, 6 / 3; 25.0000 / 50.0000 / 22.7500 |
| E4 | M16A3 head 50.000, MK11 head 117.500 (kill in 1), MK11 head at 60 m kills in 2 | 50.0000, 117.5000 (1), 2 |
| E5 | M16A3 108,000,000 / commit +91,800,000 → 31, empty 142,200,000 → 30; M9 81,000,000 / 96,000,000 | all equal |
| E6 | 4 distinct hashes; KS D ≤ 0.0364 over 2,000 offsets; recoil after 10 rounds within 1e-4° | 4; D 0.0158 each; diff ≤ 4.5e-9° |
| E17 | hostile client changes no other soldier, clamps counted, tick unaffected; install unchanged | 0 / 900 ticks differ; all clamps counted; 0 precondition failures; `install.unchanged: clean (256 files)` |
| G1 (regression) | `host.scale` tick p99 ≤ 8 ms under the quiet lock | 1.212 / 0.783 / 1.454 ms |
| asan | m2a.contract, net.proto.unit / fuzz, sim.lagcomp.unit, sim.weapons.fuzz, host.gunplay.server under `--preset asan` | 6 / 6, no sanitizer report |
## M2a vm: 1p viewmodel and ADS

The own soldier's first-person viewmodel and ADS camera (design and measured gates:
[docs/design/m2a-vm.md](docs/design/m2a-vm.md); plan m2a.md §11.1, §11.2, E19, E5, E11):

* `bf3_anim` `viewmodel_anim.h`: the Godot-free animator (base, action and ADS layers, procedural stand-ins), the
  sway, bob, kick and camera springs, `viewmodel_tuning.cfg` and its parser, `AdsCamera` (world FOV 55 -> 40
  vertical, viewmodel FOV RenderFov -> ZoomRenderFov, look scale) and the zoom refusal rule `ads_allowed`.
* `bf3_render` `viewmodel_set.h`: the US and RU sets of the 8 default loadouts, read from the data
  (`read_viewmodel_kits`: the mode's kits and `KitDef::default_unlock`, as sv's kit table) with arms, every item's
  1p mesh, role clips with counted stand-ins, ADS data from the aiming controller, the default customization's
  iron-sight accessories, and each material slot's alpha test from the level shader database. The knife's dog tags
  and chain are a counted stand-in: left out (`--vm-dogtags` draws them; design §2.7).
* `bf3_godot` `Bf3Viewmodel` (`viewmodel.h`) and the `fp_camera` M2a vm block (FOV, interpolated predicted recoil,
  presentation kick); the demo / check scene `res://scenes/m2a_viewmodel.tscn` plays every kit through the real
  weapon sim on a scripted timeline (cl's `present_weapon` and sv's zoom rule are faked in `m2a_fake` until they
  merge).

Run the demo windowed with `Godot --path bf3-opensource res://scenes/m2a_viewmodel.tscn` (kits loop with a status
label; `--kits=0,4` picks loadouts). Every gate has one command:

| gate | command | threshold | measured (2026-10-16) |
|---|---|---|---|
| E19 unit | `ctest --preset dev -R anim.viewmodel.unit` | E19 on the synthetic weapons at 30 / 60 / 144 / 240 fps | 9/9 cases, met |
| E19, E5, E11, looks (install) | `ctest --preset dev -R godot.m2a_viewmodel` (= `Godot --headless --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --vm-check`) | `M2A VIEWMODEL CHECK: PASS (20/20)`, install unchanged | 20/20: FOV 55 / 40, RenderFov 55, FOV time ±1 frame (worst 1.00) in both FOV-time modes and 0.15 s asserted in the generic mode, recoil worst 7.9e-6°, springs worst 0.736 s (≤ 1.0) and each spring within its own settle time, 0 ADS frames while deploying or sprinting, reload clip and refire ≤ 1 frame, 32/32 items of the loadouts read from the data covered (equal to m2a.md §5.6), alpha-tested slots cut out, knife dog tags hidden; met |
| E5 clip lengths, E11 report | `bf3dump vm clips --team all` | authored length next to ReloadTime, coverage per item, loadouts from the data | printed (design §4); E11 PASS 32/32 |
| skinned geometry | `bf3dump vm skin --team all` | no stretched skinned triangle through deploy, fire, reload | `VM SKIN: PASS` |
| captures | `Godot --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --vm-shots=<openBF3>\b\shots\vm` | hip, ADS, reload mid-clip PNGs | 38 PNGs (design §5) |
| hostile data | `ctest --preset asan -R anim.viewmodel.unit` (after `tools\build.cmd asan --no-test`) | no AddressSanitizer report | 9/9 cases, 6,155 assertions, no report (fuzz: 3,000 mutations, 792 parsed, 2,208 failed cleanly) |

The sim half of the zoom refusal prints `PENDING (sv)` until sv's rule lands in `step_weapons_m2a`. The FOV time
follows the aiming controller's per-transition time (iron sights 0.12 s in, 0.15 s out); `fov_transition_specific =
false` in `viewmodel_tuning.cfg` uses the generic 0.15 s, and the check measures both (m2a.md E19 was corrected).
The present cost (mean 12.7 µs per frame) is wall clock on a shared machine and noisy.

## M2a VFX2 renderer (fx2)

<!-- # M2a fx2 -->
VFX2 draws `bf3_fx`'s simulated particles in Godot. Design: [docs/design/m2a.md](docs/design/m2a.md) §12.1. As-built
notes, measurements and capture descriptions: [docs/design/m2a-fx2.md](docs/design/m2a-fx2.md).

* **`bf3::fx::RenderPrep`** (`render_prep.h`, Godot-free) turns `FxWorld`'s outputs into ≤ 24 instance streams:
  shader class × blend × world / first-person pass, 80 B per instance. Batches are sorted back-to-front by emitter
  (§16 Q10). It also builds the merged 64-light pool.
* **`prepare_atlas_pixels`** (`atlas_pixels.h`) caches the level's emitter atlas as BC3 + BC5 with every mip
  (`fx-atlas` cache entries).
* **`Bf3FxRenderer`** steps the world on Godot's WorkerThreadPool and draws one MultiMesh per batch with
  `res://shaders/fx_*.gdshader`. It also drives the omni lights.
* **Demo:** `<godot> --path bf3-opensource res://scenes/fx_demo.tscn` runs synthetic starts on Metro: 1p muzzle,
  concrete and brick impacts, a smoke pillar, the MP_012 oilfire and a light stress. With `-- --fx-check` it runs the
  gates and writes captures to `b/shots/fx2/`.

| gate | command | threshold | measured (2026-10-16, shared machine: noisy) |
|---|---|---|---|
| unit | `ctest --preset dev -R "^fx\.render\.unit$"` (also asan) | 9 cases | 9/9, 6,478 assertions |
| demo (E8 (3), (4), (6), lights, 1p, FX time) | `ctest --preset dev-window -R "^godot.fx_demo$"` with `OPENBF3_GODOT_WINDOWED=1` (a window row) | drawn == particles every frame; 52/52 atlas, 0 fallbacks; oilfire 80 ± 1 at 15 s; ≤ 64 lights; 1p batches | all met; FX main thread p50 0.108 / p99 0.225 ms |
| GPU (E9 item 8, §16 Q17) | `ctest --preset dev-window -R "^godot.fx_ref$"` with `OPENBF3_GODOT_WINDOWED=1` (a window row) | VFX2 16k ≤ 1.25 × MultiMesh 16k lit + soft | 0.769 / 0.758 ms = 1.015 |

Both Godot rows are timing rows: run them under the quiet lock. A CPU-bound frame roughly doubles the measured GPU time
on this machine (m2a-fx2.md §7).

## M2a hud (minimal HUD)

<!-- # M2a hud -->
Track `hud` of M2a wave 2, on branch `m2a/hud`. Design: [docs/design/m2a.md](docs/design/m2a.md) §11.3, §11.4, §16
Q7, gate E10. As built: [docs/design/m2a-hud.md](docs/design/m2a-hud.md).

* **Feed** (`bf3/host/hud_feed.h`, Godot-free): `make_hud_static` (one row per weapon def; `apply_item_names` gives
  the M67 its "M67 GRENADE"), `fill_hud` (ui2's `HudModel` from the presented weapon, the own soldier and the drained
  events; the hit indicator 0.3 s at level 1 per own server hit, §16 Q7), and the layout: the hudmpscreen slots read
  at run time, `hud_stage_to_window` (scale min(w/1280, h/720)), `crosshair_gap_px`.
* **Godot** (`src/bf3_godot/src/hud_controls.h`): `Bf3Hud` draws the model only: ammo (`$HUD_medium` at Ammo_01),
  crosshair (hidden in ADS), hit indicator (the hitindicator atlas art), weapon name and `_small` icon, health.
* **Check**: `res://scenes/m2a_hud.tscn -- --hud-check --hud-shots=<dir>` drives a scripted model at 1280 x 720,
  1920 x 1080 and 2560 x 1440 and compares every widget with the script every frame (ctest `godot.m2a_hud`; shots to
  `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\hud\`, gitignored).
* **Tests**: `ctest --preset dev -R "ui.hud.feed"` and the window row `godot.m2a_hud` (`ctest --preset dev-window -R godot.m2a_hud` with `OPENBF3_GODOT_WINDOWED=1`). Measured results and one
  command per gate: m2a-hud.md §6.

## M2a wave 3 contract (W3.0)

<!-- # M2a w3 -->
These are the integrator's contract commits for M2a wave 3, on branch `m2a/w3` from `main` 5e296e7 (wave 2 is on
main: sv, cl, vm, fx2 and hud are its ancestors). Design: [docs/design/m2a.md](docs/design/m2a.md) §15 "Wave 3".
As-built notes, the decisions on G1-G7 and the review items: [docs/design/m2a-w3.md](docs/design/m2a-w3.md).

* **Interfaces.** The commits declare the wave-3 headers. The Godot-free ones are `bf3/audio/weapon_audio.h`,
  `impact_audio.h`, `game_audio_check.h`, `bf3/fx/triggers.h`, `bf3/host/firefight_brain.h` and `client_wiring.h`,
  plus the crowd additions in `crowd_eval.h`. The Godot side is `game_audio.h`, `game_fx.h`, `game_crowd.h`,
  `m2a_check.h` and `m2a_game.h`. The tracks (bt, tp, au3, fx3, gx) write the bodies.
* **The seven gaps wave 2 left.**
  - G1: the others' team as 2 bits on the wire, so 52 others fit a snapshot without the own state.
  - G2: `present_other_weapon`.
  - G3: `GameServerConfig::firefight` and its init hook.
  - G4: `ClientSession::send_kit_select`.
  - G5: `make_client_gunplay_inputs`.
  - G6: `viewmodel_muzzle_transform`. weaponske01 has no muzzle joint, so it uses Wep_Root times the FireEffects1p
    offset.
  - G7: gx's hooks in the level session, which do nothing until gx fills them.
* **Changed pins.** `net.proto.unit`'s two capacity pins went from 53 to 52 (G1).
* **3p weapons per soldier (review fix).** S3 bakes one weapon into each team's mesh. tp draws the kit item each
  soldier holds without editing S3's loader or uploader: `bf3/render/crowd_weapons.h` loads each item as its own
  skinned mesh, `strip_team_weapon_parts` drops the baked weapon before upload, and `CrowdEvaluator::weapon_of` /
  `weapon_palette` pose it (m2a-w3.md §3, "tp's 3p weapon route").
* **Test.** `ctest --preset dev -R "^m2a\.contract3$"` compiles the Godot-free headers together. It pins the POD
  shapes, enum values, defaults, the crowd lane rule and the seed hashes, and runs G1-G5 and the check's case parser.
  The rows that need bt, tp, au3 or fx3 print `PENDING (<key>)` until each track sets its switch.

Measured 2026-10-18, after the review fix (head of `m2a/w3`):

* **`tools\build.cmd dev --strict`:** 128 rows, 126 passed and 2 skipped (`core.cache_data`, `core.res`), 178 s wall
  time, exit 0, only the existing budget warnings. `m2a.contract3` (10 cases, 333 assertions) took 0.24 s.
* **asan:** `m2a.contract3`, `m2a.contract`, `m1.contract`, `anim.s3.unit`, `net.proto.unit` and `net.proto.fuzz`
  passed, with no AddressSanitizer report.

Measured 2026-10-18, before the review fix:

* **`tools\build.cmd dev --strict`** (the branch pass, no timing, realtime or window rows): 128 rows, 126 passed and
  2 skipped (`core.cache_data`, `core.res`, as before), 176 s wall time, no budget violation. `m2a.contract3` took
  0.06 s.
* **asan:** `m2a.contract3`, `m2a.contract`, `m1.contract`, `net.proto.unit` and `net.proto.fuzz` passed, with no
  AddressSanitizer report.

## M2a bt: firefight bots

<!-- # M2a bt -->
Design: [docs/design/m2a.md](docs/design/m2a.md) §10.2, E13, E15. As-built notes, constants, decisions and the
measured gates: [docs/design/m2a-bt.md](docs/design/m2a-bt.md).

The firefight bots are `bf3_host` `FirefightBrain`s, added by `GameServer::init` through `GameServerConfig::firefight`
(W3.0's G3 hook). They read `ServerView` and fight:
* They walk the nav mesh (S1's `WalkerBrain` through the `BotNav` hook in `bot_fill.h`) toward the nearest enemy.
* They pick targets by eye-to-chest line of sight and turn at a bounded rate, with a seeded aim error. The weapon's
  own spread and recoil come from the sim.
* They fire bursts (or 2 Hz taps), reload, switch to the sidearm, strafe when close, and die and respawn through the
  `RespawnPolicy`.
* In **Pairs** mode (E13's budget scenario), 32 pairs stand on lanes in front of an observer pose and are placed with
  `place_soldier` after each respawn.
* In **Free** mode they spawn at the TDM spawns and hunt.

A human (a remote slot) is an enemy like any bot. Soldiers in slots 64 and above are never hit (lc's
`RewindProvider` covers slots 0-63), so runs use at most 64 slots for fighters.

Commands (read-only on the install; the output carries BF3-derived numbers, so keep it out of the repo; each prints
`install.unchanged`):

```bat
ctest --preset dev -R "^host\.gunplay\.(firefight|replay)$"                                :: functional rows (branch pass)
ctest --preset dev-timing -R "^host\.gunplay\.perf$"                                       :: E13, timing row (merge pass, quiet lock)
b\dev\tools\bf3dump\bf3dump.exe firefight MP_Subway TeamDeathMatch0 --mode=free --count=64 --seconds=60 --bots
b\dev\tools\bf3dump\bf3dump.exe firefight MP_Subway TeamDeathMatch0 --perf                 :: E13's row, headless
```

Gates, measured 2026-10-18 on the dev preset (branch, unlocked, shared machine):

| gate | threshold | measured |
|---|---|---|
| host.gunplay.firefight | 64 Free bots, 60 s: both teams kill, every bot fires, 0 stuck (20 s windows), ≥ 10 m per bot, 0 inside geometry / below ground / NaN, 0 preconditions, 0 team kills; plus 32 bots + 1 peer: the peer is hit | kills 117 / 100; 64 / 64 fired; 73 windows, 0 stuck; 7,249 checks, 0 / 0 / 0; peer hit 6 times, killed twice |
| host.gunplay.replay (E15) | equal per-tick world hashes for one seed over 3,600 ticks; a different seed differs | 0 / 3,600 differ; 3,600 / 3,600 differ for seed + 1 |
| host.gunplay.perf (E13) | server tick p99 ≤ 8.0 ms, 64 Pairs bots + 1 observer, 10 s + 120 s | indicative only (branch, unlocked): p99 1.909 ms; weapons + projectiles + rewind 7.7 % of the mean tick |

Branch pass: `tools\build.cmd dev --strict` ran 130 rows (128 passed, 2 skipped as before) in 408 s on a machine
loaded by other worktrees. The one STRICT violation is `sim.terrain.unit`, which took 40.1 s in the phase without a
COST and 0.85 s alone; bt did not touch it.

## M2a tp: 3p soldiers

<!-- # M2a tp -->
Track tp of M2a wave 3 is on branch `m2a/tp`, from the W3.0 contract. Design: [docs/design/m2a.md](docs/design/m2a.md)
§9, R5, E18 (b) and §16 Q13. As-built notes, measured gates and one command per gate:
[docs/design/m2a-tp.md](docs/design/m2a-tp.md).

* **Evaluator** (`bf3/anim/crowd_eval.h`, `crowd_clips.h`):
  - **R5:** per-soldier weapon binds, reload and `weapon_local` from `CrowdTeam::weapons`. Index 0 is S3's data. A
    team without items, or with S3's one weapon, is S3's evaluator bit for bit.
  - **Stance set:** lc's `ServerPoseClipNames` idles, aims and transitions. The crouch moves are `Rifle_Sneak*` and
    the prone moves `Rifle_Crawl*`.
  - **Fire:** `Rifle_CrouchFire` when crouched, a 3° / 50 ms kick when standing or prone.
  - **Death** (§16 Q13): blend to prone over 0.5 s, hidden after 3 s.
  - **Muzzle:** a per-soldier 3p muzzle (`muzzle_model`).
  - **Binds init fix:** each soldier's buffers are sized from the largest team.
* **3p weapons** (`bf3/render/crowd_weapons.h`, `crowd_weapons.cpp`): `load_crowd_weapons` loads one skinned 3p mesh
  per kit item, with binds, reload clip and rate, `weapon_local` and the FireEffects3p muzzle. Metro TDM gives 7 + 7
  items.
* **Godot** (`Bf3GameCrowd`, `game_crowd.cpp`):
  - `present_others` / `present_other_weapon` become crowd inputs. Wire team 1/2 maps to crowd team 0/1, and the def
    maps to the item.
  - RemoteShot, Death and Spawn events apply on their timeline.
  - Each soldier gets its own weapon instance, and the muzzles are published for fx3.
  - Headless, it poses without drawing.
  - gx still has to wire it into the level session's hooks.
* **Check:** `res://scenes/m2a_tp_crowd.tscn -- --tp-check [--tp-shots=<dir>]` drives 12 scripted soldiers on S3's
  Metro stage. The shots go to `b\shots\tp\` (gitignored).
* **Tests:**
  - `ctest --preset dev -R "^anim\.s3\.unit$"`: R5 bit-equal with one weapon, two weapons per team, the stance set.
  - `ctest --preset dev -R "^anim\.s3$"`: case "capsules" = E18 (b). Crouch head 0.077 cm, prone head 0.179 cm vs the
    3 cm gate.
  - `ctest --preset dev -R "^render\.crowd_weapons$"`: install.
  - `ctest --preset dev -R "^godot\.m2a_tp_crowd$"`: headless, `M2A TP CROWD CHECK: PASS`.
  - `m2a.contract3` has `kTpLanded` set.

## M2a au3: game audio

<!-- # M2a au3 -->
Track `au3` of M2a wave 3, on branch `m2a/au3`. Design: [docs/design/m2a.md](docs/design/m2a.md) §12.3, gate E7.
As built, with the measured results and one command per gate: [docs/design/m2a-au3.md](docs/design/m2a-au3.md).

* **Router** (`bf3/audio/weapon_audio.h`, Godot-free):
  - **Own fire** runs on the Predicted timeline: one first-person driver per trigger hold, with its rounds on the shot
    index grid.
  - **Others' fire** runs on the Render timeline. A RemoteShot starts the burst on its exact frame, and the newest
    records decide the end on time. The records also fill rounds whose datagram was lost.
  - **Reloads, the knife and the M67** are one-shots.
  - **Voice limit:** 32 drivers, the farthest released tail stolen, the rest culled; 64 one-shots; a 96-instance mixer.
  - **Listener:** after every listener move, every emitter is sent again.
* **Clock:** `SimMixerClock` places events on the mixer clock with the measured output latency and a bias.
* **Impacts** (`impact_audio.h`): fx3's impact and effect sound patches, compiled once.
* **Godot** (`src/bf3_godot/src/game_audio.h`): `Bf3GameAudio` owns the game mixer, its stream and the player
  "Bf3GameMixer". It has `load`, `build`, `step` and the client-local `audition`. Its Godot-free halves are in
  `bf3/audio/game_audio_frame.h`, and the offline rows run them.
* **E7** (`game_audio_check.h`): `GameAudioCheck` records the mix and the shooter's voice taps.
  `analyse_game_check` judges the recording with au's `analyse_check` and the in-game rows. Item 4's M1A2 patch is
  not on Metro's mount: it loads from MP_001 (`kAuditionSuperbundles`) with a bank of its own. A step that cannot run
  fails (`skipped.<step>`); it never reads as a pass.
* **Tests:**
  - `ctest --preset dev -R "^audio\.(router|game)"` runs the unit suite and the install rows. `audio.game` covers the
    kits, the offline E7 rehearsal and a scene for the ear; `audio.game.crowd` runs 64 shooters for 60 s.
  - The rows write their WAVs to `b\dev\shots\<row>\`, which is gitignored. Copies for listening are in
    `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\au3\`.
  - `audio.game.underruns` gates the underrun count (a wall-clock measure) and runs on main only.

## M2a fx3: VFX3 triggers

<!-- # M2a fx3 -->
VFX3 turns the game's PresentEvents into effects: shots, impacts, hits and detonations. Design:
[docs/design/m2a.md](docs/design/m2a.md) §12.2. As-built notes, contract corrections, measurements and capture
descriptions: [docs/design/m2a-fx3.md](docs/design/m2a-fx3.md).

* **`bf3::fx::FxTriggers`** (`triggers.h`, Godot-free) is fed by `on_muzzles` and `on_events`.
  - OwnShot: the def's FireEffects1p list at the own 1p muzzle, otherwise FireEffects3p in the world.
  - RemoteShot: FireEffects3p at the crowd muzzle or the aim fallback.
  - Tracers: on flagged shots, converging on the aim.
  - Impacts: the level's cell for the surface's PhysicsMaterialIndex, a variant chosen by the event's seed, with
    decal and sound cues.
  - Soldier contacts and Hits: the zone's cell. Detonations: the detonation effect and its scorch.

  Seeds come from the match seed, so two clients start the same variants (E15). It also covers culling and the
  predicted and render timelines.
* **`Bf3GameFx`** (`game_fx.h`) is what gx wires into the playable scene. It holds:
  - the load on the loader thread: its own uncached library, the atlas and the preload of every fire and impact
    blueprint;
  - fx2's renderer and light pool, and the decal pool (Godot `Decal`s). Every decal texture passes one rule
    (`fx::prepare_decal_texture`): a real alpha is kept, a magenta-keyed BC1 one is un-keyed to grey with alpha, and
    an opaque one is refused (Metro's brick texture is a channel mask, so brick hits leave no mark rather than a red
    square);
  - `own_muzzle` from G6, `sound_cues` / `effect_sound_cues` for au3, and a client-local `start_effect`.
* **Demo:** `<godot> --path bf3-opensource res://scenes/fx3_demo.tscn -- --fx3-check --fx3-shot-dir=<dir>` drives
  the game path on Metro. It captures the 1p muzzle flash, impacts on concrete, metal and dirt (a Sand sandbag),
  decals, a soldier hit and tracers.

| gate | command | threshold | measured (2026-10-18, branch, unlocked, shared machine: indicative only) |
|---|---|---|---|
| unit (incl. E15 late join) | `ctest --preset dev -R "^fx\.triggers\.unit$"` (also asan) | all cases | 15/15, 4,530 assertions, 0.21-0.35 s |
| install cells (E8 (1), (3), (5), (7)) | `ctest --preset dev -R "^fx\.install\.cells$"` (also asan) | 0 preload failures; 1p flash in [710, 790] / 1000; G5's cells; water on Caspian | 72/72; 759 / 1000; met; `fx_impact_water_s`; decal textures: brick refused, 8 drawn; 2.9-4.3 s |
| demo | `ctest --preset dev-window -R "^godot\.fx3_demo$"` with `OPENBF3_GODOT_WINDOWED=1` (a window row) | `FX3 DEMO: PASS`, 7 captures | PASS, 7/7; FX main thread p50 0.09 / p99 0.16 ms; 24.7 s |

Branch pass (2026-10-18; branch, unlocked, shared machine: indicative only):

* **asan:** fx.triggers.unit, fx.install.cells, fx.unit, fx.render.unit and m2a.contract3 passed, with no
  AddressSanitizer report.
* **`tools\build.cmd dev --strict`** ran twice while other sessions were testing.
  - The first run: all 130 rows functionally passed (2 skipped as on m2a/w3), with 2 budget violations from the load
    (fx.unit 36.5 s, fx.install.cells 32.2 s).
  - The second run: 5 budget violations, none on an fx3 row, and `core.ui.font`'s own cold-time check failed
    (1.64 s against 1.0 s).
  - Alone, the rows ran in 0.26 s (fx.triggers.unit), 0.49 s (fx.unit) and 3.06 s (fx.install.cells), with
    core.ui.font passing.
  - After the verifier's fixes: `tools\build.cmd dev --strict` exit 0, 128 passed and 2 skipped of 130, 0 STRICT
    violations, phase wall 193 s (m2a-fx3.md §10).

## M2a: the playable scene (gx)

<!-- # M2a gx -->
Track `gx` of M2a wave 3, on branch `m2a/gx` from the W3.0 contract. Design: [docs/design/m2a.md](docs/design/m2a.md)
§15 row gx. As built, with measured results and the screenshot descriptions:
[docs/design/m2a-gx.md](docs/design/m2a-gx.md).

**Play Metro TDM:** `tools\play.cmd`, which runs `<godot> --path bf3-opensource res://scenes/m2a_metro.tscn --
--bots=24`. Godot comes from `OPENBF3_GODOT` (default `C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe`).
Extra arguments pass through: `--kit=<0..3>` (assault, engineer, support, recon), `--team=<1|2>` (US, RU),
`--respawn=<s>` (default 5) and `--bots=<n>` (at most 32).

Controls are M1's mouse look, WASD, Shift, Space, Ctrl, Z and F6, plus:
* left mouse fire, right mouse ADS, R reload, B fire mode;
* 1 / 2 / G / F / wheel for the slots (primary, sidearm, grenade, knife);
* K / T for the next spawn's kit / team;
* Esc to release the mouse (every held button is released too; the click that captures it again never fires).

The scene shows the HUD, the viewmodel of the own team, the kill counter (`kills k deaths d   US a : RU b`) and the
death screen (killer slot or fall, respawn countdown; the viewmodel and the weapon HUD hidden).

Since the end-to-end step (`m2a/w3` merged, m2a-gx.md §9) the match has:
* bt's firefight bots, which fight the player and each other;
* tp's 3p soldiers with their weapons, which replace the capsules;
* au3's game audio: weapons, impacts and reloads in 3D, the own weapon in 1p;
* fx3's effects: muzzle flashes, tracers, shells, impact effects and decals.

A part that fails to load is dropped with an `[m2a] ERROR` line, and the game runs on without it.

Gates, measured 2026-10-18 on the branch after the verify fix (m2a-gx.md §10; windowed rows one at a time, unlocked,
shared machine; wall times indicative only).
Every windowed row needs `set OPENBF3_GODOT=<exe>` and `set OPENBF3_GODOT_WINDOWED=1`:

| gate | command | threshold | measured |
|---|---|---|---|
| functional cases (E1, E3, E4, E5, E7, E8, E10, E19) | `ctest --preset dev-window -R "^godot\.m2a$"` | `M2A CHECK: PASS`, 0 `ERROR` lines | 6 / 6 cases, 252 rows pass (range, reload, hud, ads, audio, fx): E19 FOV in 0.1167 s / out 0.150 s within 1 frame, recoil within 1.34e-5°, ADS refused while deploying and sprinting; E3 / E4 at 5 m and 60 m with the MK11; E5 per frame for the M16A3 and M9; E10 gap error 0 px, ammo at x 1194; Metro's 1,818 tags hold no Water; 0 ERROR lines; 393 s |
| kits (E11) | `ctest --preset dev-window -R "^godot\.m2a\.kits$"` | `M2A CHECK: PASS`, 0 `ERROR` lines | 276 / 276 rows: 8 loadouts, every item deploys, fires and (firearms) reloads with server events = predicted, E1 gaps for every kit firearm, the knife hit, the M67 fuse from the server's DetonationEvent exactly 3.6 s = TimeToLive; 161 s |
| 5-minute play, 0 errors | `ctest --preset dev-window -R "^godot\.m2a\.play$"` | `M2A CHECK: PASS`, 0 `ERROR` lines, killed by an enemy >= 1 | PASS. 16 bt bots, 8,456 remote shots, team kills US 55 : RU 51. The player: 406 own shots, 2 kills, 7 deaths (4 by bots), 7 respawns, the team switch. Crowd, FX and audio built (300.9 s mixed, 12,478 impact cells). 0 event overflows; 0 ERROR lines; 311 s |
| screenshots | the play row and the hud case (to `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\gx\`) | hip, ADS, reload, firefight, death; hud_hip, hud_reload, hud_ads | 5 / 5 and 3 / 3 (m2a-gx.md §10.3; the firefight and ADS bots are small: §10.4), plus E7's recording `gx_e7.wav` |
| branch pass | `tools\build.cmd dev --strict` | every row passes, no budget violation | after the verify fix: exit 0, 137 rows, 135 passed, 2 skipped (`core.cache_data`, `core.res`), 0 failed, 0 STRICT violations; 187 s |
| E16 on the M2a scene | `ctest --preset dev-window -R "^godot\.m2a_load$"` | cold <= 15 s, warm <= 5 s | the integrator's row. One run on the branch (unlocked, shared machine, indicative only): cold 4.34 s pass, warm 5.42 s over the threshold |
| E9, E13 / E14 | `ctest --preset dev-window -R "^godot\.m2a_firefight$"` | m2a.md E9, E14 | the integrator's row (64 bt bots, 2560 x 1440, FX on / off windows); not run on the branch (TIMING rule) |

## M2a gate pass

<!-- # M2a gate -->

**M2a results.** The integrator's gate pass (m2a.md §15.1 step 7, on `integ/w3`, 2026-10-19) ran every exit
criterion E1–E19 in the running game, with every wall-clock and GPU row under the quiet lock: all 19 pass, no
threshold changed, no WARN. Fire timing is tick-exact (31 shots, 30 gaps of 75 ms, predicted = server field for
field); the drop is 1.60 m at 300 m and the client's flight of the same shot ends 0.0000 mm from the server's;
shots to kill, headshot multipliers and reload times match the tables to 1e-3; lag-compensated hits 500 / 500 at
"150/2"; the server tick p99 is 1.69 ms with 64 firing bots, the client frame p99 6.06 ms at 2560 × 1440, Metro
loads in 5.4 s cold / 4.4 s warm, and the install is unchanged. The gate fixed two gaps (E16 now prints the VRAM,
E2's client half got its own case). Row by row, the playable check and what is left to judge by eye and ear:
[docs/design/m2a-gate.md](docs/design/m2a-gate.md). **Play it:** `tools\play.cmd` (Metro TDM, assault kit on US,
24 firefight bots; `--bots=N` up to 32, `--kit=K`, `--team=T`, `--respawn=S`).

## M2b wave 1 contract (W1.0)

<!-- # M2b W1.0 -->
These are the integrator's contract commits for M2b wave 1, on branch `m2b/w1` from `main` 36eb5af. Design:
[docs/design/m2b.md](docs/design/m2b.md) §2.6 and §11. As-built notes, and where the code was followed over the
design: [docs/design/m2b-w1.md](docs/design/m2b-w1.md).

* **Interfaces.** The commits declare §2.6 #1-#15 and #19-#22, for the tracks nt, ru, bb and au:
  - protocol v3 (`MatchState`, `PlayerList`, `SpawnRequest`, `ReviveEvent` and the new fields);
  - the client frame's match fields and `send_spawn_request`;
  - `PresentKind::Revive`;
  - `NativePortMeaning` 16-29;
  - the motor's immunity-abort fields;
  - `round_natives.h` and the scored spawn choice;
  - `perception.h` and `aim_model.h`;
  - ServerView's `downed`, `phase`, `input_restricted` and `kit`, with default bodies;
  - the kit and gunplay options;
  - `SharedLevel::logic` and `SharedLoadOptions::tdm`;
  - `tdm_rules.h` and `tdm_brain.h`;
  - GameServer's `[cfg]`, `[tdm bots]` and `[sv]`;
  - the A3 audio headers and MixerCore fields;
  - `tests/tdm_fixture.h`.
* **Stubs.** The tracks write the bodies. Until nt lands, the six v3 codecs are stubs: encoders return an empty
  vector, and decoders return the error "v3 codec: nt". `add_tdm_bots` refuses a config with bots until bb lands.
  Every stub line is marked `// M2b W1.0 stub`. `net::restrict_command`, the pre-round freeze both ends apply, is
  written in full.
* **Q8.** `godot.m2a`, `godot.m2a.kits` and `godot.m2a.play` carry the label `retired`, and the `dev-window` preset
  excludes it. They stay runnable by name: `ctest --test-dir b\dev -R "^godot\.m2a$"`.
* **Test.** `ctest --test-dir b\dev -R "^m2b\.contract$"` compiles every wave-1 header together. It pins:
  - the shapes (`PlayerRow` 32 B, `PresentEvent` 64 B);
  - the enum values;
  - the defaults of every new field;
  - the `restrict_command` rows;
  - the M2a hashes (`motor_params_hash` default `0x346282435cd97947`, `kit_table_hash` default `0xa23fb7a00909ad6d`).

  It also runs the stubs and GameServer::init with the hooks. A row that needs a track prints `PENDING (<key>)` until
  that track sets its switch.

Measured 2026-10-19, head of `m2b/w1`:

* **`tools\build.cmd dev`:** 138 rows, 136 passed and 2 skipped (`core.cache_data`, `core.res`, as before), with
  193.7 s of ctest wall time and exit 0. There were no budget violations, only the existing warnings.
* **`m2b.contract`:** 5 cases and 97 assertions, 0.02 s.
* **`ctest --preset dev-window -N`:** 13 rows, with the three retired rows excluded.

## M2b nt: protocol v3 and the client session

<!-- # M2b nt -->
Track nt of M2b wave 1, on branch `m2b/nt` from the W1.0 contract head 01a26e8. Design:
[docs/design/m2b.md](docs/design/m2b.md) §3.7 and §2.6 #11, #12. As-built notes, the measured results and one command
per gate: [docs/design/m2b-nt.md](docs/design/m2b-nt.md).

* **Protocol v3.** `kProtocolVersion` is 3. The six W1.0 stubs are real codecs: `MatchState` (40 bytes),
  `PlayerList` (up to 64 rows, names of at most 16 bytes of valid UTF-8) and `SpawnRequest` (3 bytes). The v3 fields
  are on the wire too: `MatchInfo::logic_digest` and `rules_flags`, `Spawned::revived`, and in `CombatEvents` the death
  stamp (respawn tick, revive end, killer health) and the revives. The decoders bound every count, check every enum
  and refuse trailing bytes. The encoders write one canonical form and count every change they make.
* **The client session.**
  - MatchState and PlayerList go into the `PresentationFrame` (`match`, `players`).
  - The own death's respawn tick and revive end go into `own_respawn_tick` and `own_revive_until`.
  - A revive becomes a `PresentKind::Revive` event. A revived life's Spawn event carries `present_flags::kRevived`.
  - Every predicted command gets the pre-round restriction (`net::restrict_command`), as the server's commands do.
  - Prediction stops at `MatchState::unspawn_tick` (`own_unspawned`).
  - A TDM server refuses a client whose load has no logic graph or a different logic digest.
  - `send_spawn_request(kit)` sends the Deploy.
* **The M2a gate's double starts (shot ids reused after a respawn).** The client emitted shots that it predicted
  after the server's death tick. The next life started at the server's shot index, so its real shots reused those ids,
  and the effect triggers dropped them as repeats. `OwnShotIds` now gives the own shots presented ids that never
  repeat. The server's later events about an own shot map to the same ids.
* **Tests.**
  - `ctest --test-dir b\dev -R "^net\.proto\.unit$"`: the v3 round trips and hostile bytes.
  - `ctest --test-dir b\dev -R "^net\.proto\.fuzz$"`: 18 message types, 0 unstable, every structured malformed case
    refused (E7's wire half).
  - `ctest --test-dir b\dev -R "^host\.client\.m2b\.unit$"`: reproduces the shot-id reuse on the client's weapon-lane
    path, then shows 0 repeats with the fix.
  - `ctest --test-dir b\dev -R "^m2b\.contract$"`: no nt PENDING left.

## M2b au: audio A3

<!-- # M2b au -->
Track `au` of M2b wave 1, on branch `m2b/au` from the W1.0 contract (01a26e8). Design:
[docs/design/m2b.md](docs/design/m2b.md) §6, gates E10 and E11, R5 and R6. As built, with the measured results and
one command per gate: [docs/design/m2b-au.md](docs/design/m2b-au.md).

* **Data** (Godot-free, read-only):
  - `read_hdr_setting` reads the one HdrSetting in `sound/soundstates`: window 45 dB, min top 80, min bottom 35,
    attack 0.05 s, release 0.035 to 0.85 s on a 5-point curve, headroom 5, overshoot 3.
  - `read_scopes` reads the 33 scopes of `sound/audiosystem`, and `map_patch_scopes` gives each compiled patch its
    `Scope`.
* **HDR window** (`hdr_window.h`): the mixer steps the window every block on the loudest HDR voice. An EnableHdr
  output then sounds at Loudness - top - Headroom (R5). A voice below the bottom is suspended: silent, still playing.
* **Culling** (`voice_cull.h`), in this order:
  1. pre-cull a starting HDR voice below bottom - 3 dB;
  2. the scopes: duplicates, then the closest N, then the newest or oldest N;
  3. the cap of 128 sampler voices (busy slots, not instances).

  A dropped one-shot comes back finished, and a culled loop waits as pending. The router's own limits became that
  one cap (R6).
* **Profiler:**
  - The mixer times every block into a 16-bucket histogram of its share of the block's 10.67 ms.
  - `format_mixer_profile` prints one line: the mix share, the voices, the culls, the suspended voices and the
    window.
  - `Bf3GameAudio` reads the A3 data at load, and `game_audio_profile_text()` is the readout for an overlay.
* **Render cost:** a silent voice chain skips its per-sample work and the mix stays exactly the same (`[quiet]`).
* **Tests:**
  - `ctest --test-dir b\dev -R "^audio\.hdr\."` runs E10. `audio.hdr.unit` is data-free; `audio.hdr.install` reads
    the retail setting and scopes, and exits 77 without an install.
  - `audio.game.crowd` runs E11's functional checks on 64 shooters: live voices at most 128, the cull counts and the
    histogram.
  - The p99 share gate (≤ 0.10) and the late-block gate run only in the merge pass, in `audio.game.underruns`.

Measured 2026-10-19 on `m2b/au` (wall-clock values indicative only):

* **E10:** met. Top 80 / 35 for the footstep. Top 110 in 5 blocks (53.3 ms) for the shot, bottom 65, the 63 dB source
  suspended.
* **E11 voices:** at most 128. Culled: 717 pre-cull, 207 scope, 397 cap.
* **64-shooter mix:** 8.5 s of wall time for 66 s of audio, against M2a's 10.2 s, with 0 late blocks.
* **E11 share:** p99 below 0.20, so the 0.10 gate is probably not met; docs/design/m2b-au.md §10 has the follow-up.

## M2b ru: TDM rules and the server flow

<!-- # M2b ru -->
Design: [docs/design/m2b.md](docs/design/m2b.md) §3.1-§3.6, §2.6 #1-#10 and #19-#22, the R table of §11; as built,
the measured gates and the open issues: [docs/design/m2b-ru.md](docs/design/m2b-ru.md).

* **Load.** `SharedLoadOptions::tdm` builds Metro TDM's logic graph as bf3dump does (digest `6f6a0693b3924bde`),
  reads the round natives' EBX fields (kill limit 100, 4 players to start, PlayerKilledDelay 5 s, AutoTeam, the
  pre-round input restriction), the data's scoring rows, `bf3-opensource/config/tdm_tuning.cfg` (every number the
  data does not decide, `measured = false`), the TDM kits (the defibrillator in the assault gadget slot) and the spawn
  immunity's abort inputs (into the motor params, so the client predicts them).
* **Rules.** `TdmRules` (`bf3_host`, `bf3/host/tdm_rules.h`) runs the round natives on the LogicRuntime and is the
  server's respawn policy and combat sink: kills and deaths per team and player, respawn at death + 5 s (a bot near a
  living assault teammate waits 8 s for a revive), regen after 10 s at 5 hp/s, the defibrillator's revive (a new life
  at the corpse with 20 hp, the same kit and team, the kill taken back, 100 points), SpawnRequest refusals, the scored spawn choice, AutoTeam
  teams and the bots' kits. The GameServer calls it through named insertions (`// M2b ru [..]`, R1-R10).
* **On the wire.** Since the w1 merge the v3 messages (nt's codecs: MatchState, PlayerList, SpawnRequest,
  Spawned.revived, the death stamps, ReviveEvent) travel, and host.tdm.server's scripted peers decode them.

Commands (read-only on the install; the cases print BF3-derived numbers, keep the logs out of the repo):

```bat
ctest --test-dir b\dev -R "^host\.tdm\.server$"                                                     :: all cases (~9 s, one Metro TDM load)
b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=tuning*     :: the tuning parser, hostile text
b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=health*     :: E3 regen and spawn immunity
b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=respawn*    :: E4 respawn, R10, the M2a hashes
b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=revive*     :: E5 revive
b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=hostile*    :: E7 server half
b\dev\tests\test_host_tdm.exe --test-suite=host-tdm-server --test-case=install*    :: install unchanged
ctest --preset dev-timing -R "^host\.tdm\.client$"                                                  :: E13 (timing; skipped until bb)
```

Measured 2026-10-19 on the dev preset, `m2b/ru` (tick counts, the same on every run):

| gate | measured |
|---|---|
| E3 | regen from T + 600 at 0.08334 hp per tick, a second drop restarts it; immunity 120 ticks, Fire / move at +30 end it at +30, server = local motor (0 mismatches) |
| E4 | respawn at death + 300 (+480 next to an assault teammate), the rules' kit; a deploy-screen Ready spawns nothing; M2a hashes kit `31b45dc7cc3f4271`, weapon `b8fd23d8ccfc4c63`, motor `6d75dd5f51d95090` (equal to main 47871fa's); a SpawnRequest for respawn_tick - 1 refused, for respawn_tick accepted, Spawned in that tick with the kit; the peer's DeathEvent respawn_tick = death + 300; MatchState / PlayerList at the peer = the rules'; a v2 Hello kicked |
| E5 | bot and peer revived at the corpse (0.000 m), 20 hp, life + 1, the same kit and team, enemy kills 1 -> 0, medic +100; living target and enemy medic refused; Spawned.revived = 1 and the ReviveEvent at the peers; deploy screen: 0 revives after the man-down window, a deploy ends the man-down at once |
| E7 | refusals counted by kind; 300 hostile v3 datagrams, all counted (298 bad, 2 judged by the rules); a SpawnRequest in RoundOver refused; asan: host.tdm.server, sim.weapons.unit, m2b.contract pass, no report |
| E13 | not run (timing row; skips with PENDING (bb) until bb's add_tdm_bots) |
| install.unchanged | clean (256 files) |
| branch pass | `tools\build.cmd dev` after the w1 merge (nt in): 140 rows, 138 passed, 2 skipped, 0 failed, 192.0 s |

## M2b bb: TDM bots

<!-- # M2b bb -->
Track bb of M2b wave 1, on branch `m2b/bb` from the W1.0 head 01a26e8. Design: [docs/design/m2b.md](docs/design/m2b.md)
§5, E1, E2, E6, E8. As-built notes, the named constants and the differences from the design:
[docs/design/m2b-bb.md](docs/design/m2b-bb.md).

The TDM bots are `bf3_host` `TdmBrain`s that `add_tdm_bots` puts on the server through `GameServerConfig::tdm_bots`.
They read only `ServerView` (with the rules' `downed()`, `phase()`, `input_restricted()` and `kit()`) and the
gunplay's shots:
* **Perception** (`bf3_sim` `perception.cpp`): every 6 ticks per bot, a 160° x 90° cone to 120 m, chest and head rays
  for the nearest 4 (the clear one is the aim point), detection faster for near, moving and firing enemies; hearing gives a position estimate, never a
  detection; 3 s to forget.
* **Aim** (`aim_model.cpp`): a skill-scaled error cone that narrows with time on target, lead and drop for projectile
  weapons, recoil compensation by skill.
* **Behaviour**: target scores with a switch margin and a reaction delay; bursts by range, aiming down the sights,
  no fire through a teammate, blind fire at a lost target, the M67, the knife from behind, reload and sidearm;
  strafing, hunting the last known enemy (given up after 20 s), a contact heat grid for roaming; retreat and regen
  wait; assault bots revive downed teammates with the defibrillator; idle in the pre-round. Seeds are
  `match_seed ^ slot`. The bots join through `GameServer::add_local_bot` (the named insertion `[bot spawn]`), so they
  spawn on the main nav component, never on the level's nav-island spawns. `[bot spawn]` and `[spawn transform]` are
  listed as contract additions in the `## M2b bb` block at the end of m2b.md, for the integrator to confirm.
* **Tuning**: `bf3-opensource/config/bot_tuning.cfg` holds every default (all [inferred], measured = false: no VU
  session measured BF3's bots). Unknown keys and out-of-range values fail the load.
* **Preview**: `tools\play.cmd --tdm --bots=31` runs the M2a scene with the TDM rules and TDM bots; the match state prints as `[m2b] match: ...` lines. `tools\play.cmd --help` lists the options.

Commands:

```bat
tools\build.cmd dev                                                        :: the branch pass
ctest --test-dir b\dev -R "^host\.tdm\.bots\.unit$" --output-on-failure   :: perception, aim, tuning, brain (no install)
ctest --test-dir b\dev -R "^m2b\.contract$"                               :: the W1.0 contract with kBbLanded
ctest --test-dir b\asan -R "^host\.tdm\.bots\.unit$" --output-on-failure  :: asan (after cmake --build --preset asan --target test_host_tdm)
ctest --test-dir b\dev -R "^host\.tdm\.(round|bots|replay)$"              :: E1, E2, E6 (Metro TDM; skip without an install)
ctest --preset dev-timing -R "^host\.tdm\.perf$"                          :: E8, timing row (merge pass on main)
```

Measured 2026-10-19 on the dev preset, `m2b/bb` after the merge of `m2b/w1` (nt, au, ru), unlocked, shared machine
(times indicative only):

| gate | threshold | measured |
|---|---|---|
| branch pass `tools\build.cmd dev` | every row passes or skips as before | 146 rows: 144 passed, 2 skipped (`core.cache_data`, `core.res` as before), 0 failed, 197.4 s, exit 0 |
| host.tdm.bots.unit | all cases pass, data-free budget 2 s | 7 cases passed, 0.10 s |
| m2b.contract | passes with every track landed | passed, 0.37 s |
| asan host.tdm.bots.unit | no asan report | passed before the merge (0.68 s); after the merge the verification pass ran it (0.60 s) and `host.tdm.replay` (195 s) under asan, 0 reports; not rerun after `[spawn transform]` |
| E1 host.tdm.round | m2b.md E1 | passes: 0 `Recorded` deliveries to the TDM types (70 handled; 42 to other types, printed), RoundOver at tick 17,634 (≤ 21,600), winner 1 at 100 / 95, the peer's MatchState the same, 0 runtime faults, rounds 1, the restart in place 0 / 0; 35.7 s in the pass (13.7 s alone). The first run after the merge failed on the `BlueprintTransform` property of the two HQ spawns at tick 1, fixed by the named insertion `[spawn transform]` in `round_natives.cpp` |
| E2 host.tdm.bots | pooled K/D in [0.75, 1.33], 0 sight, 0 stale aim, 0 stuck, 0 bad spawns, revives ≥ 1 | pooled 149 / 131 = 1.137 (seeds 1.190, 0.980, 1.282), 0 / 0 / 0 / 0, revives 4; 23.0 s in the pass (9.6 s alone) |
| E6 host.tdm.replay | same seed 0 differing ticks, another seed differs | 0 / 3,179 ticks differ, trace and MatchState bytes equal; seed 12 differs on 3,179; 20.7 s in the pass (7.3 s alone) |
| E8 host.tdm.perf | perception ≤ 0.2 ms/tick p99, brains ≤ 1.5 ms/tick p99, tick ≤ 4 ms p99 | not run: timing row, runs on main after the merge |

## M2b wave 2 contract (W2.0)

<!-- # M2b W2.0 -->
These are the integrator's contract commits for M2b wave 2, on branch `m2b/w2` from `main` 545640c (wave 1 merged).
Design: [docs/design/m2b.md](docs/design/m2b.md) §2.6 #16-#18 and §11. As-built notes, the insertions each track
makes, and where the code was followed over the design: [docs/design/m2b-w2.md](docs/design/m2b-w2.md).

* **Interfaces.** The commits declare §2.6 #16-#18, for the tracks ui, cam and gx:
  - ui: `HudKillCard`, `HudEndOfRound`, `HudSpawnScreen`, `HudPreRound` and their `HudModel` members; the M2b members
    of `HudFeedInput`, `HudKillCardSource` and `fill_hud_tdm` (hud_feed.h); `Bf3TdmHud` (hud_tdm.h), the screens'
    façade that gx builds;
  - cam: `CrowdSoldierInput::revived`, `CrowdStanceSet::corpse_seconds`, `kCrowdCorpseSeconds` 15 s,
    `Bf3GameCrowd::set_corpse_seconds`, `Bf3DeathCamera` (death_camera.h);
  - gx: `m2b_game.h`, `m2b_check.h`, `level_session_m2b.h` (`M2bGame`), `M2aGame::m2b`.
* **No stubs.** Nothing in the tree calls a wave-2 function yet. gx builds ui's and cam's parts only when their
  bodies exist (`OPENBF3_M2B_HAVE_UI` / `_CAM`, from the files' presence), so the three branches link alone.
* **Additive only.** `git diff --stat main..m2b/w2` shows 0 deleted lines.
* **Test.** `ctest --test-dir b\dev -R "^m2b\.contract2$"` compiles every wave-2 header together. It pins:
  - `HudModel` at 9,024 B (M2a 8,864, the M2a fields at their old offsets);
  - the layouts of the four new structs;
  - `kHudMaxPlayers == net::kMaxPlayerRows` (the M2b HUD's players are indexed by server slot);
  - the W2.0 constants and defaults;
  - the M2b check's case parser.

  ui's `fill_hud_tdm` row and cam's death-camera row print `PENDING (<key>)` until the tracks set their switches.

Measured 2026-10-19, `m2b/w2`, shared machine (times indicative only):

* **`tools\build.cmd dev`:** 147 rows, 145 passed and 2 skipped (`core.cache_data`, `core.res`, as before), with
  204.1 s of ctest wall time and exit 0. There were no budget violations, only the existing warnings.
* **`m2b.contract2`:** 4 cases and 84 assertions, 0.01 s. ui and cam are pending.

## M2b cam: death view and 3p corpse

<!-- # M2b cam -->
Track cam of M2b wave 2, on branch `m2b/cam` from the W2.0 head 5191059. Design:
[docs/design/m2b.md](docs/design/m2b.md) §4, §2.6 #17 and §12 J3. As-built notes, the measured results and one command
per gate: [docs/design/m2b-cam.md](docs/design/m2b-cam.md).

* **Corpses stay 15 s.** `Bf3GameCrowd::set_corpse_seconds` (the M2b scene calls it before `build()`) keeps a corpse
  drawn for `kCrowdCorpseSeconds`. M2a never calls it, so its 3 s stays. A bot that respawns before its corpse is gone
  draws its new life on a second crowd index, so the corpse stays where it fell. The own body is drawn as a corpse
  too, so the death camera has something to look at.
* **Revive stand-up.** A `Revive` event marks the revived slot. Its next life stands up from the corpse pose over
  0.5 s at the same place, instead of popping up standing.
* **The death camera** (`Bf3DeathCamera`, Godot-free): from the 1p eye it pulls back behind the corpse (1 s). With the
  killer camera on and the killer drawn, it swings to look past the corpse at the killer (0.75 s) and follows the
  killer. It is finished at 4 s (the kill card's time) and holds until the game ends it. A wall pulls it in. Every
  number is a named default with `measured = false`.
* **The defibrillator in hand.** Its 1p mesh resolves (`bf3dump vm skin weapons/gadgets/defibrillator/u_defib`: PASS,
  0 stretched surfaces). A TDM load calls `render::add_assault_gadget` and gets it as one more viewmodel item for the
  assault kits.
* **FX.** `FxTriggers::sane` now accepts `PresentKind::Revive` (the integ-w1 open item, which m2b-w2.md had given to
  gx). A Revive event starts no effect.
* **Still to do (gx).** gx wires these into the M2b scene: `set_corpse_seconds`, `add_assault_gadget`, and the death
  camera's begin / step / end and its transform onto the Camera3D. m2b-cam.md §6 lists each call. Until then nothing of
  cam can be seen in a scene, so there are no screenshots. The look is for the wave-2 playtest.
* **Tests.**
  - `ctest --test-dir b\dev -R "^m2b\.contract2$"`: cam's death-camera case runs (no PENDING (cam)).
  - `ctest --test-dir b\dev -R "^m2b\.cam\.unit$"` (new): the death camera without a killer, with the killer camera
    off, continuity, the killer lost, hostile inputs and params, the wall; the defib item set.
  - `ctest --test-dir b\dev -R "^anim\.s3\.unit$"`, case `revive`: 15 s corpses, the stand-up, a plain respawn, LOD,
    hostile corpse times.
  - `ctest --test-dir b\dev -R "^fx\.triggers\.unit$"`: a Revive event is accepted.

Measured 2026-10-19, `m2b/cam`, shared machine (times indicative only):

* **`tools\build.cmd dev`:** 148 rows, 146 passed and 2 skipped (`core.cache_data`, `core.res`, as before), 0 failed,
  331.3 s of ctest wall time, exit 0. Only the existing budget warnings, none on a cam row.
* **The cam rows:** `m2b.contract2` 4 cases, 95 assertions; `m2b.cam.unit` 3 cases, 701 assertions; the `revive`
  case 41 assertions; all under 1 s.
* **Asan:** the same four rows built and run under the asan preset passed with 0 reports.

## M2b ui: the TDM HUD screens

<!-- # M2b ui -->
Design: [docs/design/m2b.md](docs/design/m2b.md) §4, §2.6 #16 and E9. The as-built notes, the lookups, the measured
gates and the open issues are in [docs/design/m2b-ui.md](docs/design/m2b-ui.md).

* **Feed.** `host::fill_hud_tdm` (src/bf3_host/src/hud_feed_tdm.cpp) runs after `fill_hud` with the same drained
  events. It fills the M2b half of `HudModel` from the frame's `MatchState`, player rows and Death / Spawn events:
  - the players by server slot and the sorted team lists;
  - the kill counter;
  - the kill feed (a ring of 8);
  - the kill card (4 s after the own death);
  - the spawn screen (kit bar, deploy timer, revive window);
  - the pre-round countdown and freeze;
  - the round end.

  Hostile values are clamped or skipped and counted, never used as an index.
* **Screens.** `Bf3TdmHud` (src/bf3_godot/src/hud_tdm.cpp) builds six Controls under the game's parent:
  - the kill feed, top right, with the `_small` weapon icons and a drawn headshot mark, plus the team counter at the
    top centre;
  - the scoreboard, two teams, while Tab is held;
  - the spawn screen: four kit cards with their loadouts, and a Deploy button the game polls with
    `take_kit_click` / `take_deploy_click`;
  - the kill card;
  - the pre-round box;
  - the round-end panel.

  They draw with BF3's fonts ($HUD_medium, $Menu_medium, $Menu_bold), 24 localised strings, iconsingame icons and the
  hudmpscreen / spawnscreenpc placements, all read from the install at run time. A missing lookup is counted in
  `TdmHudRecords` and drawn with a fallback.
* **Wiring.** gx wires the screens into the M2b game. On this branch, `Bf3TdmHudDemo` drives them with hand-made
  models and saves screenshots.

Commands (read-only on the install; keep logs and screenshots out of the repo):

```bat
ctest --test-dir b\dev -R "^ui\.hud\.tdm\.(unit|install)$"      :: the feed from recorded frames; every lookup on the install
ctest --test-dir b\dev -R "^m2b\.contract2$"                   :: the contract, ui case active
:: the screens in Godot: a scratch scene with Bf3Localisation, Bf3UiAssets and Bf3TdmHudDemo (docs/design/m2b-ui.md §5)
"C:\Users\olku\BF3 reimplementation\Godot_v4.8-dev7_win64_console.exe" --path bf3-opensource --resolution 1280x720 <scratch>\tdm_hud_demo.tscn -- --tdm-shots=<dir>
```

Measured 2026-10-19, dev preset, `m2b/ui`:

| gate | measured |
|---|---|
| ui.hud.tdm.unit | 7 cases pass in 0.05 s (budget 2 s). The recorded round has 255 frames, 7 deaths = 7 feed lines, 2 kill cards and 0 mismatching frames; 10 or more hostile inputs were counted and clamped |
| ui.hud.tdm.install | 0.55 s (budget 15 s): 24/24 strings, 3/3 fonts, 5/5 icons, 3/3 placements, 13/13 kit items with name and `_small` icon; install.unchanged clean (256 files) |
| m2b.contract2 | 4 cases and 113 assertions pass, ui case active (cam still pending) |
| run time (demo) | headless 9/9 rows; windowed 16/16 rows and 7 screenshots in `openBF3\b\shots\ui\` (gitignored); 0 missing fonts, icons or strings |
| E9 | not run here: gx's `godot.m2b.play` in the wave-2 merge pass |
| branch pass | `tools\build.cmd dev`: 149 rows, 147 passed, 2 skipped (as before), 0 failed, 386.6 s ctest wall (indicative only) |

## M2b: the playable TDM scene (gx)

<!-- # M2b gx -->
`tools\play.cmd` now opens the M2b scene (`res://scenes/m2b_metro.tscn`): a Metro TDM round, you and 31 TDM bots
(16 v 16). The teams are AutoTeam's, and the round runs the Metro TDM logic graph. The track is on branch `m2b/gx`,
from the W2.0 contract. As-built notes, named defaults and gates:
[docs/design/m2b-gx.md](docs/design/m2b-gx.md).

* **Play.**
  - `tools\play.cmd` starts the round. Useful options: `--kill-limit=N`, `--preround=S`, `--bots=0..31` and
    `--deploy-screen=0` (M2a's automatic respawn).
  - `tools\play.cmd --m2a` keeps the M2a scene, and bb's `--tdm` preview still runs on it.
  - `tools\play.cmd --help` lists the keys.
* **Keys.**
  - On the spawn screen: 1-4 pick the kit (assault, engineer, support, recon), or click it. Enter (or the Deploy
    button) spawns once the respawn time is over. The game resends the request every 30 server ticks until the
    server spawns you.
  - In game: Tab shows the scoreboard, and 3 selects weapon slot 2 (assault: the defibrillator). The M2a weapon
    keys are unchanged.
* **ui and cam.** The TDM screens (ui: kill feed, scoreboard, spawn screen, kill card, pre-round, round end) and
  the death view and 15 s corpses (cam) are built when their code is in the tree, which it is since the merge of
  `m2b/w2` (reconfigure after a merge). Without them a status line shows the phase, the score, the kit and
  "ENTER: deploy". In an M2b session the assault kit carries the defibrillator in 1p (gx's `[defib]` line).
* **Test.**
  - `ctest --test-dir b\dev -R "^m2b\.gx\.unit$"`: the Godot-free rules (the args with hostile values, the spawn
    screen and Deploy rules, the resend, E9's verdicts and skips) and R13 (`FxTriggers` accepts Revive).
  - `godot.m2b.play` (E9, the scripted round) and `godot.m2b_load` (E12) are WINDOW rows for the merge pass.

Measured 2026-10-19, `m2b/gx`, shared machine (times indicative only):

* **`m2b.gx.unit`:** 5 cases, 197 assertions, pass.
* **The play check, headless, by hand** (`--m2b-check=play --kill-limit=15 --preround=0`):
  - all 7 gx rows pass, and the ui (6) and cam (1) rows skip "waiting for";
  - the run: the spawn screen at join, a deploy as assault, an own death at 27 s, a deploy as engineer, RoundOver
    at 39.6 s (RU 15 - US 8), 24 deaths received;
  - install unchanged: yes.
* **Branch pass:** 148 rows: 146 passed, 2 skipped (`core.cache_data`, `core.res`, as before), 0 failed; 331.7 s of ctest wall time; exit 0. One "budget VIOLATION" line is for `host.tdm.server` (wave 1's row: 63.1 s without COST under the parallel load), not a gx row.
* **After the ui and cam merge** (headless play check, by hand): M2B CHECK: PASS, 13 rows pass, `missing_lookups`
  skips headless (a window row), none "waiting for"; 23 feed lines for 23 deaths, 0 of 172 Tab samples off,
  1 death view with 0 bad inputs; RoundOver at 39.6 s (US 7 - RU 15); install unchanged: yes. The rows the merge
  touches (m2b.gx.unit, m2b.contract, m2b.contract2, m2b.cam.unit, ui.hud.tdm.unit / .install, fx.triggers.unit,
  godot.m2a_viewmodel, godot.m2a_tp_crowd) pass.
* **The verify fix: a reliable own death in E9** (m2b-gx.md §6.1). The play script now puts the player in front of
  the nearest enemy 6 s into a life and every 4 s until the first own death (the drop is the last resort at 26 s), a
  death right before RoundOver no longer counts against the after-death spawn screen, and the Tab scoreboard gives
  way to the round-end screen. One run each, by hand: headless PASS (own death at 9.0 s, RoundOver at 40.8 s);
  windowed PASS, 14/14 rows, 0 ERROR lines, `missing_lookups` 0 (own death at 12.7 s by a bot, deploy engineer at
  17.9 s, RoundOver at 33.8 s, 5 screenshots); install unchanged: yes.

## PT1 snd: sound glitching

<!-- # PT1 snd -->
Track `snd` of the first playtest fixes, on branch `pt1/snd` from main 6a4061e. The finding: "Sound glitching
sometimes". Root causes with the measurements, the fixes, before and after, and one command per check:
[docs/design/pt1-snd.md](docs/design/pt1-snd.md).

**What was wrong.** Measured in windowed M2b rounds of our own mixer, and in the offline 64-shooter render:

* **Hard cuts.** The 128-voice cap parked sounding loops and dropped sounding one-shots between two samples, and a
  Kill stopped a voice at once. In one round, 128 cuts were above -40 dBFS, the largest -16 dBFS.
* **Flapping.** Loops at the cap's edge were parked and resumed every 10 to 200 ms (offline: 1843 of 3234 parks
  shorter than 0.25 s).
* **Clipping.** The mix went over 0 dBFS in 43 to 176 episodes a round, up to +7.6 dBFS. The HDR energy terms are not
  applied (Q9), so nothing bounded the sum.
* **Late blocks.** Up to 107 ms, at the round end. Godot's audio thread runs at normal priority, and it stalled
  beside the main thread's hitches.

**What changed** (named defaults, [inferred], measured = false):

* `[fade]`: those cuts fade over one block (10.7 ms), and a resumed loop fades in.
* `[hold]`: the cap holds a sounding voice by 3 dB and its busiest 0.5 s. A parked loop resumes only with 8 voices to
  spare.
* `[limit]`: a peak limiter at -0.3 dBFS on the output, using the block as lookahead (no latency). A mix under the
  ceiling is left exactly as it was.
* `[prio]`: `THREAD_PRIORITY_HIGHEST` on Godot's audio thread.

**After**, in windowed rounds:

* 0 audible cuts;
* at most 7 loops parked more than twice, against 65;
* 0 frames over 0 dBFS;
* 0 late blocks in three rounds.

**Diagnostics:**

* Every game mixer prints one `[pt1] audio health:` line at shutdown: the cuts, call gaps, late blocks, the limiter,
  hitches and the thread priority.
* `--audio-capture=<dir>` records `mix.wav`, `cuts.csv` and `hitches.csv` (derived data, under `b\shots`).
  `python -I tools\audio_capture_check.py <dir>` analyses them.
* A/B switches: `--audio-cut-fade=0 --audio-cap-hold=0 --audio-limit=0 --audio-thread-priority=keep` give the merged
  mixer.

**Rows:**

* `audio.cuts.unit` (new, data-free, 7 cases);
* `audio.game.crowd`: the 64-shooter case now checks 0 audible cuts, flaps under 1 in 20 parks, and 0 overs.

`audio.game.underruns` (E11), `godot.m2a_firefight` (E14) and `godot.audio` (G8) are the merge-pass rows the diff
touches.

## PT1 rld: reload and weapon foley sounds

<!-- # PT1 rld -->
Track `rld` of the first playtest fixes, on branch `pt1/rld`. It fixes the M2b playtest finding "no sounds for
reloads". Root causes, the fix and the numbers: [docs/design/pt1-rld.md](docs/design/pt1-rld.md).

* **From the data** (`weapon_foley.h`, Godot-free): each kit weapon patch takes these inputs:
  - `Reload`, with `IsShortReload` picking between the long (empty) and the short (tactical) wave;
  - the reload's release event (#3520090073);
  - `Action`, with `ActionType` picking deploy, zoom, the dry-fire click, bipod, light or fire-mode switch;
  - `Bolt`.

  The reader gives each event's samplers, its distance reach and the outputs it feeds.
* **Posted where the weapon state changes:**
  - the own soldier's sim events (reload start and cancel, deploy, fire mode, bolt), at the frame the viewmodel uses
    for them;
  - the own presented weapons (zoom, dry fire);
  - the others' presented fields (`reload_seq`, a weapon change, ads), at the shooter, within the data's reach.
* **Why they were silent:** the voice cull counted a reload instance at its gunshot outputs (100.4 dB) instead of its
  Handling output (79.5 dB). The HDR window therefore put the reload 21 dB down. The cull now counts only the outputs
  the busy voices feed.
* **After firing:** every output of the kit firearms carries `BypassHeadroom = IsFirstPerson`. The mixer now keeps
  such an output at the window its instance would set alone (a named default, `measured = false`), so the own reload
  is not ducked under the own burst's reflections tail. The own gunshot sets the top itself and is unchanged.
* **Also fixed:**
  - the own 1p reload no longer stays where it started, and another soldier's foley moves with him;
  - the foley one-shots no longer get the trigger release 3 s later, which fired a phantom round on the M9, MP443,
    MK11 and SVD.
* **Tests:**
  - `ctest --test-dir b\dev -R "^audio\.foley\."`;
  - `audio.foley.install` runs the trigger table, every kit reload, 3p at 5 / 15 / 25 m, the offline mix and 300
    corrupted programs, and exits 77 without an install;
  - `audio.foley.unit` reads hostile synthetic programs.

Measured 2026-10-19 on `pt1/rld`:

* **Own M16A3 reload alone:** -39.6 dBFS before, -19.2 dBFS after (window top 100.4 -> 80.0 dB). Walking at 3 m/s:
  -50.8 -> -19.2 dBFS.
* **Every trigger:** the wave starts +0.0 ms after its event's frame. Tactical reloads play the short wave and empty
  ones the long wave, on all 10 kit firearms.
* **Reload 0.1 s after a 30-round burst:** -39.7 dBFS before, -19.3 dBFS after (alone: -19.2), while the window top
  stays at the tail's 100.4 dB.
* **Another soldier's reload, walking away at 3 m/s from 5 m:** its last 1.4 s -39.9 dBFS, against -26.6 dBFS standing
  (it stayed at the start before).

## PT1 iff: telling friend from foe

<!-- # PT1 iff -->
Playtest track "iff", on branch `pt1/iff` from main 6a4061e. The playtest said "can't tell who is friendly and who
isn't". The root causes with their evidence, the fix, the measured before / after and one command per check are in
[docs/design/pt1-iff.md](docs/design/pt1-iff.md).

* **Root causes.**
  - The M2b minimap still ran M1's loop, which drew every other soldier as an enemy: 31 orange arrows per frame, 15 of
    them teammates.
  - No code drew BF3's nametags (UINametagCompData) over soldiers.
  - The 3p crowd drew one appearance per team, the assault one, for every kit.
* **Minimap.** It now shows every live teammate as a blue Friendly arrow. An enemy shows only for 3.15 s after each of
  its shots (ActiveSpottedTime 7 x SpotOnFireMultiplier 0.45, from the soldier's SpottingTargetComponentData).
  M1 and M2a keep their minimap: the new code runs only once a MatchState arrives.
* **Nametags** (`pt1_iff.{h,cpp}`, rules in `bf3/core/ui/nametags.h`), from UINametagCompData:
  - over every teammate, the hud3dicons Friendly arrow, through walls;
  - within 15 m (ShowLabelRange), or when looked at, also the kit icon and the name in FriendlyTint (0, 0.8, 0.8);
  - over an enemy, nothing, except the name in EnemyTint (1, 0.38, 0) while the crosshair is on him and a ray to him
    is clear.
* **3p kits.** Each kit wears its own appearance, read from the kit's VisualTable, its default camo and that camo's
  LinkedTo. That gives 8 meshes instead of 2. S3's spec is unchanged.
* **Kill feed and scoreboard.** No change. Their colours are already relative to the local team, and the data names no
  colours for them.
* **Inferred, `measured = false`:** the stage units, the anchor, the look-at curve, the reveal rule and the kit-name
  rule (pt1-iff.md §3).
* **Look at it:**

  ```
  "%OPENBF3_GODOT%" --path bf3-opensource res://scenes/m2b_metro.tscn -- --deploy-screen=0 --preround=120 --iff-shots=b\shots\pt1-iff
  ```

  This saves a teammate and an enemy at 6, 15 and 35 m, then quits. Or play the M2b scene as usual.
* **Tests.**
  - `ctest --test-dir b\dev -R "^pt1\.iff\.unit$"` (new, data-free): the rules and recorded frames.
  - `ctest --test-dir b\dev -R "^pt1\.iff\.install$"` (new, 77 without an install): the data values, the 8 kit
    appearances, the crowd's kit parts, the fuzz and install.unchanged.

Measured 2026-10-19, `pt1/iff`, shared machine (times indicative only):

* **Capture**, 31 bots, pre-round, 1956 frames:
  - minimap: 15.0 Friendly arrows per frame, 0 enemies shown (16 hidden), 0 teammates as enemies (was 15.0);
  - nametags: 10.1 friendly icons, 1.30 friendly names and 0.363 enemy names per frame;
  - cost: 27.5 us of main thread per frame on average;
  - 6 of 6 views saved.
* **Live round** (`--iff-live=60 --preround=0`, 4872 frames): 13.3 Friendly and 6.7 revealed-enemy arrows per frame
  (8.1 enemies hidden), at most 12 revealed at once.
* **Rows:** `pt1.iff.unit` 8 cases, 1859 assertions, 0.24 s; `pt1.iff.install` 5 cases, 103 assertions, 1.7 s;
  install unchanged.

## PT1 run: the running animation

<!-- # PT1 run -->
Playtest finding PT1 "run", on branch `pt1/run`. The M2b playtest said: "the running animation is wrong and too fast".
Root causes with evidence, the fix, before / after numbers and one command per check:
[docs/design/pt1-run.md](docs/design/pt1-run.md).

* **3p.** The crowd now plays BF3's own move groups from s_basicassets (`bf3/anim/move_group.h`):
  * `MP_RunMoveGroup_PC` when standing;
  * `MP_SprintMoveGroup` while the soldier's kSprinting flag is set;
  * `MP_SneakMoveGroup_PC` when crouched.

  Each clip plays at its data rate. Prone keeps its old cycles. Before this, the crowd used S3's hand-made run blend.
  It had no sprint clip: at 6.5 m/s it played Rifle_RunFwd at 1.4×, 281 steps/min. BF3's Rifle_Sprint gives 251.
  Crouching was clamped, so the feet slid at 0.36 m/s (now 0.04).
* **1p.** The own arms and weapon now play BF3's additive run and sprint deltas, chosen per weapon type:
  * run: `1P_RunLoopChooser` at speed / 3.6 m/s;
  * sprint: `Wep_SprintDeltaChooser` at rate 1.

  These replace the procedural bob, which ran at 2.7 / 5.4 Hz at sprint. BF3's sprint delta runs at 1.67 / 3.33 Hz.
* **Motor.** The motor's speeds already matched PhysicsMotion (4.0, 6.5 and 2.0 m/s), so they are unchanged.
* **Before / after from one build.** `--pt1-off` draws the old cycles and the old bob.
* **Captures** (they go to `b\shots\pt1-run\`):
  * 3p: `Godot --path bf3-opensource res://scenes/m2a_tp_crowd.tscn -- --pt1-run [--pt1-off] --tp-shots=<dir>`, a
    0.6 s sequence of four runners;
  * 1p: `Godot --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --pt1-run-shots [--pt1-off] --vm-shots=<dir>`,
    the M16A4 running and sprinting.
* **Tests:**
  * `ctest --preset dev -R "^anim\.pt1_run\.unit$"`: data-free. It covers the clip choice per direction, the cycle
    rate per speed, the stride against the ground speed, hostile input, the crowd evaluator, and the 1p animator with
    synthetic deltas.
  * `ctest --preset dev -R "^anim\.pt1_run$"`: install, read-only. It covers the three groups as read, each gait's
    cycle and toe stride before and after, the 1p routes for six weapons and their cadence, and install unchanged.

Measured 2026-10-19 on `pt1/run`, on a shared machine, so the times are indicative only:

* **`anim.pt1_run.unit`:** 8 cases, 472 assertions, pass, 0.03 s (including the hostile move-group def case).
* **`anim.pt1_run`:** 4 cases, pass, about 1.6 s.
* **Changes after the fix:**
  * 3p run at 4.0 m/s: 0.600 s cycle (was 0.693).
  * Sprint at 6.5 m/s: 0.479 s (was 0.427).
  * Crouch at 2.0 m/s: stride / ground 0.990 (was 0.820).
  * 1p sprint sway: 1.60 / 3.30 Hz (was 2.70 / 5.40).
* **Branch pass** (`tools\build.cmd dev`): 153 rows, 151 passed and 2 skipped (`core.cache_data` and `core.res`, as
  before), 0 failed, 390.8 s of ctest wall time, exit 0.
  * In the pass, `anim.pt1_run.unit` took 0.04 s and `anim.pt1_run` took 3.6 s.
  * The three "budget VIOLATION" lines are for `anim`, `render` and `godot.m9_reload` under the parallel load. They
    are not PT1 rows.
* **Asan** (`ctest --preset asan -R "^anim\.pt1_run"`): both rows pass. `anim.pt1_run.unit` took 0.06 s and
  `anim.pt1_run` took 18.0 s.

## Review follow-up

* Verify: the branch compiles as handed over; branch pass 159 rows green in 197 s; dev-timing host.gunplay.* 4/4 (701 s); asan host/core/net.proto 54 + 4 rows green; a 64-player snapshot (41 others + 22 in SnapshotOthers) is byte-identical to main's encode, split, encode (fnv1a pinned from a main build); the source-digest kick has a test.
* Item 1, bf3_present: bf3_fx and bf3_audio link only bf3_present (on bf3_sim) and bf3_core, so test_fx and test_audio_unit no longer link bf3_host, bf3_net, bf3_anim or ENet; the audio glue is bf3_client_audio; branch pass 159 rows green in 195 s (198.5 s before); asan 69 + 4 rows green; audio.game.underruns and five window rows green.
* Item 2, checks out of the shipped DLL: OPENBF3_BUILD_CHECKS (ON in dev, OFF in release) gates bf3_godot's 19 check, demo and selftest sources and their 10 class registrations; the two *_headers.cpp are deleted; the sim gate code is the bf3_sim_harness library; release DLL 9,140,736 -> 7,946,752 bytes (-13.1 %); branch pass 159 rows green in 198.5 s; godot realtime 5/5, timing 4/4, window 15/15; asan 163/164 (core.ant timed out under -j16 load, 30 s alone), asan-realtime 4/4, asan-sweep 5/5 green.
* Item 3, one copy of each helper: `bf3/core/util/` (fmt, seconds_since/ms_since, finite, ASCII lower/iequals/trim, splitmix64, four percentile ranks) and `bf3/core/math.h` (one Vec3 for ant, audio, fx, res, sim, level, gameplay and render; one Transform for res, fx and vehicle drive) replace the per-file copies; the six tuning parsers are KeyDef tables over one key=value grammar with byte and line caps on all six, messages kept (parser sources 1,068 -> 519 lines); new row core.util.unit (10 cases, 1,072 assertions); branch pass 160 rows green in 196 s; asan 162/165 at -j16 (core.ant, fx.fuzz 30.5 s against a 30 s wall gate, host.tdm.server over their walls; 30 s, 56 s and 109 s alone), asan-realtime 4/4, asan-sweep 5/5 green.
* Item 4, protocol symmetry: every message and item has one `template <class Archive> serialize` run by BitWriteArchive, BitReadArchive and the constexpr BitCounter (`bit_archive.h`), which derives kOwnStateBits 616, kOtherBits 180, kShotMinBits 138, shot_bits() and the minimum sizes the decoders check, so a read can no longer drift from its write (protocol.cpp 2,260 -> 1,873 lines, weapons_wire.cpp 290 -> 286); 45 new golden pins (all 18 types plus the clamp, cut and drop paths) unchanged across the rewrite, net.proto.fuzz tallies identical (77,523 decoded+stable, 748,831 refused, 0 unstable), and across those 826,354 inputs the same verdicts and re-encoded bytes (ordered fnv1a equal); 987 truncated CommandPackets with tick 0 now say "truncated" before "implied tick or sequence below 0"; sample_combat no longer depends on operand evaluation order (asan and dev drew different times); branch pass 160 rows green in 194 s; asan net 5/5 green.
* Item 5, test infrastructure: the 30 pasted asan PATH blocks are one `ENVIRONMENT_MODIFICATION` over every row; `godot_common.cmake` holds the window probe (was in 13 scripts), `godot_run` and `godot_require_pass` (the run, print, no-install skip and pass-line tail of 19 scripts); `Mounted`/`open_or_skip` live once in `test_support.h` (were in 7 test files plus 2 inline copies); `ServerHost` paces on an injected `TickClock`, so host.server checks 121 ticks, 60.5 Hz and an overrun reset exactly on a stepped clock instead of 110..130 ticks after a 2 s sleep, and leaves realtime_retry (2.3 s -> 0.9 s); tests/CMakeLists.txt and the godot scripts -763 / +118 lines; ctest -N and every row property identical except the asan PATH now also on hygiene.rules and sim.terrain.split_cover; branch pass 160 rows green in 196 s; realtime 10/10, timing 18/18, window 15/15; asan 164/165 (core.level deep/shallow wall ratio 41.3 s vs 13.0 s under -j16 load, 7.7 s vs 7.6 s alone), asan-realtime 4/4.
* Item 6, names: the milestone-named production classes, files, scenes and log tags are named for what they are (M2aGame -> GameState, M2bGame -> TdmState, level_session_m2a/m2b -> level_session_game/tdm, Pt1Iff -> IffOverlay, m2a_game.h -> game_frame.h, s3_crowd -> crowd_scene, m2a_metro/m2b_metro.tscn -> metro_firefight/metro_tdm.tscn, play.cmd --m2a -> --firefight, CrowdM2aStats, CrowdPt1Run, s3_gate and the rest); the ten m2a_* hooks, nine m2b_gx_* free functions and the --tdm preview's static state are game_*/tdm_* members of Bf3LevelSession, and the two TDM option parsers are one parse_tdm_options that rejects out-of-range values (the --tdm preview used to clamp them); session sources 3,999 -> 3,773 lines; branch pass 160 rows green in 199 s (204 s before); realtime 10/10, timing 18/18, window 15/15.
* Item 8, SWF geometry: `SwfShape` keeps its fill and line styles, fill0/fill1 contours per fill (curves flattened to 1/40 px) and a nonzero trapezoid-sweep triangulation; over the 434 retail movies 1,159 shapes, 1,403 fill meshes (solid 315, clipped bitmap 0x41 1,014, 0x43 74, gradients 0), 9 line styles used, 0 open contours, 5,851 triangles, triangle area equal to contour area within 3.7e-7 relative; HeaderGradient hand-checked; open trapezoids are found by their left edge, so a row of 20,000 squares over 2 slabs sweeps in 8 ms (103 ms with the per-pair search).
* Item 8, SWF scene and text: PlaceObject2/3 read in full (glow 642, drop shadow 130, blur 19, gradient glow 1 decoded; blend modes, cache-as-bitmap, clip depth), DoAction frame scripts scanned to stop/play/goto ops (2,454 scripts), `SwfScene` builds the live display tree across imports (world matrices, colour transforms, masks, instance paths, frame stepping at the movie rate, goto/label, attachMovie, set visible/alpha/matrix/text, change lists) and `swf_text` lays out DefineEditText (HTML subset, DefineFont3 advances and kerning, leading, 2 px gutter, autoSize, GFx shrink/fit); hudmpscreen 417 nodes, hudtdmscreen 141, hudscreen 234, crosshairdefault 18, section 4 positions within 0.05 px, bgAmmo intro steps to its stop at frame 20, hudmessage Kill row attached and played; script visible/matrix/colour overrides outlive timeline seeks; node ids are slot plus generation, so hudmpscreen over 3,000 frames creates 3,849 nodes in 441 slots; DefineEditText's AutoSize flag sizes toward the field's align (0 of the 1,488 fields carry it); new row core.ui.scene.
* Item 8, AVM1 reader: `bf3/core/ui/avm1.h` decodes every action record from the SWF specification (ConstantPool, Push with all ten value types, DefineFunction/DefineFunction2 scopes and registers, Jump/If targets, With and Try blocks checked inside their block) and finds classes, members and Object.registerClass from the bytecode; all 434 movies decode with 0 errors in 0.06 s: 2,456 DoAction + 1,538 DoInitAction bodies = 1,638,265 B (the study's total) plus 1,244 PlaceObject clip-action tags = 97,086 B, 303,074 records, 6,174 functions, 0 unresolved constants or out-of-range registers; the 1,538 DoInitAction bodies are 872 classes, each the one its __Packages export names, and 666 registerClass stubs; actionscriptlibrary is 236 classes + 1 registerClass stub (89 B) in 237 DoInitAction tags (the study counted tags), menuactionscriptlibrary 110 + 1; the study's 23 classes have its sizes, which sum to 121,407 B (its 121,614 total is an addition slip); new rows core.ui.as2.unit and core.ui.as2, the decoder joins core.ui.swf.fuzz; `bf3dump swf as2 census` and `swf as2 <movie> [--class <name>]`; branch pass 162/162 green in 244 s, the three rows green under asan.
* Item 8, SWF renderer: `Bf3SwfView` draws a `SwfScene` through RenderingServer canvas items updated from its change list (bitmap and solid fills, strokes, DefineFont3 text clipped to its field, colour transforms as a shader (add scaled from movie units to 0..1; 9 fills' drawn colour equals `swf_apply` within 2 levels), add and layer blends, DropShadow/Glow/Blur as box-kernel passes on offscreen layers, clip-depth masks, scaling-grid nine-slice, `_xrotation`/`_yrotation` tilt as a homography about the stage centre with f = 1623.855, engine-drawn targets by renderer string, ShowAll stage fit, HUD-alone RGBA capture); vec4 instance uniforms go as Vector4 because Godot linearises Color ones; layer textures grow in 32 px steps instead of reallocating per frame (10.7 -> 2.8 ms); hudmpscreen + TDM hudtdmscreen with 16 text fields and one movie frame per frame cost 3.1-3.3 ms mean, 3.9-5.7 ms p95 of `sync()` CPU per frame at 1280x720 over four runs, with 130 + 226 of the 300 frames still re-planning the tree; new window row godot.swf_view (111 checks, 18 s); branch pass 163 rows green (core.cache_data and core.res skip) in 310 s.
* Item 8, swf clock: the core.ui.swf.unit hostile-input bounds (17 `hostile` cases, "1,100 maximal curves") measure the thread's CPU time through the new shared `bf3::core::thread_cpu_seconds` instead of wall time, so a -j16 pass no longer fails them: "mutants" measured 1.03 s wall under the branch pass and takes 0.20 s thread CPU against the unchanged 1.0 s bound; rows core.ui.swf.unit, core.util.unit green.
* Item 8, text bounds: `SwfScene::set_text_bounds` overrides a DefineEditText field's rectangle (AS2 `_width`/`_height` on a text field) without touching the parsed tag; layout and `Bf3SwfView` read it through `swf_node_text_bounds`, an unchanged rectangle keeps the text revision; rows core.ui.swf.unit, core.ui.scene, core.ui.swf green.
* Item 8, HUD feed: `HudFireMode` is the FireLogic id that the AS2 `m_firingModes` table indexes (single 0, bolt 1, auto 2, burst 3, hold-and-release 4, detonated 5; the 870 is bolt, the M67 hold-and-release with mask 1<<4); `fill_hud` writes the compass heading from the view yaw (yaw 212 deg -> 212) and the grenade count from the wcHgr slot (magazine + reserve) into the model; rows ui.hud.feed.unit, ui.hud.feed.install, ui.hud.unit, ui.hud.tdm.unit, ui.hud.tdm.install green.
* Item 8, HUD screen controllers: `HudScreen` reads a screen's UIScreenAsset widgets (MP_Subway: hudmpscreen 14, hudtdmscreen 7, crosshairdefault 1), finds each clip's AS2 class from its registerClass and construct params, runs constructors then onClipLoad in scene postorder, aligns widgets with UIBase.updateAlignment (safe 49.806/28.195) and drives one C++ controller per AS2 class from `HudModel` (`As2Clip` gives AS2 _x/_width/gotoAndStop semantics): TicketCounter, Ammo, Health, Compass, HudBackgroundWidget/HudBackground, Minimap, Crosshair, Hitindicator, LatencyIndicator and the hidden-until-data widgets; 33 + 11 + 2 instances driven, 0 failed AS2 ops, every missing label an AS2 no-op; TDM panel 179.014 x 291.396, renderHolder 42.52..230.52 before the -15 deg tilt, crosshair arms at +-44 for dispersion 44.125; new rows ui.hud.screen.unit and ui.hud.screen.{assets,ticket_counter,ammo,health,compass,background,minimap,crosshair,hidden,split_cover} (11 cases, 456 assertions, 0.5 s).
* Item 8, HUD engine parts: the parts the movies leave to the engine are numbers measured from the 18.57 footage in `hud_widgets.h` and the ui.hud.screen.unit case "engine-drawn parts": iBgDraw panel rgb (3.7, 123.2, 213.8) alpha 0.1706 with the AS2 fade fractions 1/42 x 1/10.5, EKG trace 78.7 px/s, period 1.04 s, peak 9 px, undershoot 3 px, rgb (201, 221, 232), minimap 1.375 m per local unit over the 64x64 minimapmask; rows ui.hud.* green in dev and asan.
* Item 8, gx shots path: godot_m2a.cmake defaulted its screenshot directory to the literal text `<build dir>/shots/gx`, so every save failed and godot.m2a (NO_ERRORS) and godot.m2a.play (play.screenshots 0 of 5) could not pass; the default is `${BUILD_DIR}/shots/gx` as in godot_m2b.cmake; godot.m2a passed in 405.6 s, godot.m2a.play in 309.2 s with 5/5 shots.
* Item 8, native in-round HUD: `Bf3NativeHud` draws hudmpscreen, the in-round hudtdmscreen widgets and crosshairdefault as three window-size `Bf3SwfView`s in the play scenes (built in 0.12 s: 38 instances / 33 controllers, 12 / 11, 2 / 2) and paints the iBgDraw, EKG and minimap targets; the M2a `Bf3Hud` controls and the M2b kill-counter strip are deleted (`Bf3HudItems` keeps the item index, icons and names for the kill feed and kill card); godot.m2a_hud reads the movies: arms at round((h/2) tan(spread)/tan(vfov/2)) with max |err| 0 over 864 frames per resolution, outer arm edge within 1.5 px, 26/26; godot.m2a hud.e10 0 bad over 410 frames; ui.hud.* asan green.
* Item 8, round start option: `--min-players=N` (0..32) overrides the data's RoundMinPlayerCount (4 on Metro TDM) through `MatchRulesConfig::min_players_override` and `RoundNativeConfig::min_players_override`; one peer stays in the pre-round by the data and starts the round with `--min-players=1` (host.tdm.server); rows host.tdm.server, m2b.gx.unit, m2b.contract, logic.unit, logic.metro green.
* Item 8, HUD moment fixture: `--native-hud-moments=<dir> --native-hud-ids=a,b` waits for the own soldier and the in-round phase (godot.native_hud runs `--min-players=1 --preround=0`, so the pre-round box no longer draws), teleports the soldier to each moments.json moment's world xz (variant 0; the floor nearest the nearest spawn's height that has room for the standing capsule) facing the compass heading, pins the 16 moments' HUD values and minimap pose on `Bf3NativeHud` and saves the 1280x720 composite and the HUD-only RGBA layer; godot.native_hud checks m02, m10, m13 (in round, pose, text, compass frame, engine targets) in 13.2 s; over all 16 moments 14 are placed (m01 has no floor at its xz, m15 is pushed 3.5 m off it); compare.py over all 16: RESULT FAIL, pooled mae_Y (threshold) TicketCounter 8.95 (13.19), Compass 17.19 (20.65), Minimap 35.91 (0.80), HudBackgroundWidget 10.89 (16.70), Health 8.64 (8.93), Ammo 9.54 (4.77), Crosshair 11.17 (6.24), SquadList 66.65, KillFeed 62.44, ScorePopup 36.62, KillLog 49.33 (not drawn), Outside 968 px per moment.
* Item 8, shared affine and degree constant: `SwfAffine` with `swf_affine`, `swf_concat`, `swf_inverse` (nullopt below |det| 1e-12) and `swf_apply` sits beside `swf_concat` in `swf_movie.h` and replaces the inverse and point-transform copies in `swf_view.cpp` and `native_hud.cpp`; `bf3::core::kDegToRad` and `kDegToRadF` in `math.h` replace the degrees-to-radians constants, literals and `x * (pi / 180)` factors of 28 files (every float copy was 0x1.1df46ap-6, as is `kDegToRadF`; the double `x * pi / 180` expressions in the HUD and check code now multiply by `kDegToRad`), and `hud_check`'s `deg_to_rad` helper is gone; 4 float `x * pi / 180` expressions that would round differently (walker_brain kSprintCone, fx_decals, level_scene, look_environment) stay; new cases in core.ui.swf.unit and core.util.unit; branch pass 174 rows, 0 failed, 2 skipped (core.cache_data, core.res), 228 s; window rows godot.swf_view, godot.m2a_hud, godot.native_hud, godot.m1_load green.
* Item 8, AS2 construction order: `HudScreen` runs the controllers' constructors parent-first in scene preorder as Flash does (onClipLoad stays postorder), so a HudBackgroundWidget sizes its renderHolder from the authored panel: TDM renderHolder 42.75..220.75 (was 42.5..226, retail about 221), ammo renderHolder right edge 1237 px (was 1247, retail 1235), the authored health and ammo panels no longer overlap (4.4 px apart) but their renderHolders still do, by about 11 px before the tilts; ui.hud.screen.background now expects 42.75 / 173.75 (was 42.5 / 183.5); rows ui.hud.*, core.ui.* (31), godot.native_hud, godot.m2a_hud, godot.swf_view green.
* Item 8, iBgDraw panels: over the 3,552 at-rest frames of 18.57 and 18.52 a panel is O = A*W + B with no blur (high-pass ratio 0.91 against A 0.84) and B blue above A's coverage times 255, so premultiplied light rather than one colour blended: ammo interior A (0.842, 0.853, 0.868), B (0.9, 31.7, 54.1); B ramps linearly from the renderHolder edge over w/20 and h/4.25 (`hud_bg_light_ramp_fraction_x/y` 1/21 and 1/5.25 of the renderHolder, the old fade fractions were half that) and the darkening edge sits a quarter into that ramp over 4 px; `paint_bg` draws exactly that with premultiplied blending and the layer shader keeps light above alpha; compare.py over this and the construction-order change: HudBackgroundWidget halo_Y 4.03 -> 1.92, halo_Cb 5.20 -> 2.04 (limit 1.87, FAIL), halo_Cr -5.42 -> -2.72 (2.20, FAIL), mae_Y 10.89 -> 9.97, tile_biasC 11.64 -> 12.53, Ammo mae_Y 9.54 -> 6.17 (4.77), Health 8.63 -> 7.28; rows ui.hud.*, core.ui.* (31), godot.native_hud, godot.swf_view, godot.m2a_hud green.
* Item 8, moment fixture: the soldier stands on the nearest clear standing floor to the reference xz (0.5 m rings, 8 m, 64 m for a pose moments.json marks ambiguous) and is re-teleported every frame until the capture, so nothing moves it; the reference's ambiguous poses (m01 ncc 0.277, m06, m08) print a NATIVE HUD FLAG line, the minimap keeps the reference pose; all 16 moments place and capture (m01 14.5 m from its xz, m15 0.5 m, the rest on it); godot.native_hud (m02, m10, m13) 10.0 s.
* Item 8, nametag debt: docs/architecture.md Known debts names `Bf3Nametags` (`iff_overlay.cpp`), a hand-built control, as the next widget stage's job, replaced by the native Icon3D nametags (`Widget.Icon3D.*`, `Data.UINametagComp`); rows hygiene.rules.
* Item 8, iBgDraw panel shape: light and darkening now share one ramp per axis, 0 up to 0.36 of the AS2 fade (w/20, h/4.25) in from the renderHolder edge and full at 1.05 (`HudBgDrawLook::ramp_start_of_fade` / `ramp_full_of_fade`, `hud_bg_ramp`; a least-squares fit over six dark-frame edge profiles of 18.57 gives 0.39 / 1.07), replacing the light ramp from the edge and the 4 px darkening step a quarter in; the darkening the verifier read above the minimap (0.95 at y 442, 0.91 at 448) is regression dilution: the same method on columns x 250..290 with no panel gives 0.93 and 0.88, and the frame std inside the panel at y 432..458 is 0.98 of outside; captured layer m13: junction x 1212..1228 1-a 0.73 -> 0.92..0.96 at y 598..606, TDM light at x 120..160 starts at y 452 (retail 453; it started at the renderHolder edge, y 431) and ends at 702 (retail 702..706; was the edge, about 724); compare.py HudBackgroundWidget halo_Y 1.93 -> 1.61, halo_Cb 2.04 -> -1.24, halo_Cr -2.72 -> -0.08, tile_biasC 12.53 -> 11.17, mae_Y 9.97 -> 9.39, Ammo halo_Cb -3.62 -> -5.48, Health halo_Cb -1.13 -> -4.77; rows ui.hud.*, core.ui.*, godot.native_hud, godot.swf_view, godot.m2a_hud green.
* Item 8, kill feed: hudmpscreen HudMessage_02 is the retail HudMessage, KillLogMessage and MessageBase AS2 run by their controllers (createEmptyMovieClip messageHolder, attachMovie "Kill" rows, textWidth + 5 layout, killer/msg/headshot/victim gaps 5/10/10, friendly/enemy/squad frames, `[{0:~}]` weapon text, KitRecon headshot icon, 0.4 s ExpoOut stacking by heightClip 26.0 px, startAnim done 3 frames in, outAnim removal 5 frames after its label at the outAnimationDone frame script, team-kill Game row at y -240 out after 5 s) fed by `HudMessageComp` from UIMessageCompData (Kill queue 4, 5 s, short 1 s); the Control kill feed (`hud_killfeed.cpp`) is deleted and godot.m2b.play counts native rows (kill_feed_lines 22 == 22 deaths); HudModel 9024 -> 9088 bytes (HudKill.weapon_name_sid_hash); rows ui.hud.*, core.ui.*, m2b.*, godot.native_hud, godot.m2a_hud, godot.m2b.play green, godot.m2b_load E5.warm 11.7 s under concurrent load.
* Item 8, kill log: hudtdmscreen HudMessage_00 runs LocalKillLogMessage (killer/msg/headshot/victim at the bytecode's 5/10/10 gaps, the row centred on -(victim._x + victim._width) / 2, x -93.9 for the test row) fed by the LocalKill queue (UIMessageCompData queue 2, 5 s, short 1 s, IsLocalPlayer); names untinted, no install data holds the colour table; rows ui.hud.* (ui.hud.screen.kill_log new), core.ui.*, m2b.*, godot.native_hud, godot.m2b.play green.
* Item 8, score popup: hudmpscreen HudMessage_01 (inverted) attaches ScoreMessage rows at x 140 ("ENEMY KILLED {0:s}" / "HEADSHOT {0:s}" formatted with the score, cursor_mc on the newest, loop 30 until the frame 58 script releases it after RemoveFlag, removed by outAnimComplete 29 frames after end 59) and ScoreAggregator_01 shows the running total (first score sets the text, the next plays start, ready again 12 frames later, 0 after ScoreAggregateTime 8 s plays out); the values are the install's persistence/scoring/scoring ID_SCORE_KILL 100 and ID_SCORE_HEADSHOT 10 that level_load.cpp reads into TdmTuning, pushed per own non-team kill; HudModel 9088 -> 9176 bytes (HudScoreFeed ring of 8); rows ui.hud.* (score_popup, score_aggregator new), core.ui.*, m2b.*, godot.native_hud, godot.m2b.play green.
* Item 8, alerts and indicators: HudAlertMessage runs its AS2 (warning/revive/waiting/death/failed/information types, the ID_H_ALERT texts, bracket colours and width, Timer, onShowComplete loop and onHideComplete, the 5 s SquadLeader and LastManStanding timer); the native HUD adds the hudscreen movie as a fourth view (DamageIndicator, LogMessage, TooltipMessage, ObjectiveMessage tilts per their onClipLoad, AlertManager hidden, InteractionManager shown, 0 unknown classes); the DamageIndicator is fed by HudDamageIndicatorComp (UIDamageIndicatorCompData MaxUpdateTime 5 s, 30 Hz, 3 newest shooters) from a HudDamageFeed the host fills from own-victim server hits with the bearing to the presented shooter; frame 91 rotates the arrow clockwise to the right (matrix b 1, tx 99.95); Hitindicator stays on the 0.3 s own-shooter hit; HudModel 9176 -> 9312 bytes; rows ui.hud.* (screen.damage_indicator, hud_alert, shared_screen new; feed.unit damage case), core.ui.*, m2b.*, godot.native_hud, godot.m2b.play, godot.m2a_hud, hygiene.rules.
* Item 8, moment fixture: every moment now replays its kill-feed rows (relation, weapon SID and its display text from gate.json, 63 rows) through the live HudMessageComp after a ScoreAggregateTime + 1 s flush, with killer and victim names built from W/M/H/N/E/I to the measured rect width through the probe hudmessage movie (within 4.45 px at 1280x720); the popup moments m06/m07/m12/m16 add the LocalKill row and an ENEMY KILLED score timed to the footage's Score frame (TdmTuning kill 100 and headshot 10 seeded 7.9 s earlier where the aggregator shows); rows mXX.kill_feed, kill_log and score_popup new in godot.native_hud, all 16 moments green in a full capture; compare.py core_Y KillFeed -93.9 -> 2.3, KillLog -91.8 -> 5.9, ScorePopup -93.1 -> -64.1 (all three still FAIL); the aggregator reads 210 where the footage reads 240; rows godot.native_hud, hygiene.rules.
* Item 8, kill-row fade: the AS2 has no alpha tween for Kill, LocalKill or Score rows (MessageBase sets _alpha 0 in the constructor and 100 in startAnim, the Tween mixin moves only _y, HudMessage touches no alpha); the fade is the row sprites' own `container` colour transforms, which the scene already draws: Kill and LocalKill in 0.25 / 0.50 / 0.75 / 1.00 over startAnim frames 1..4, out 1.00 / 0.80 / 0.60 / 0.40 / 0.20 over frames 9..13 and removed at 14 (0.167 s at 30 fps), Score scoringMessage 0 to 0.85 at frame 15, 0.50 through the loop, 0 by frame 76; the footage's kill rows fade out over about 28 frames (m11 row 0: 0.90 at -22, 0.50 at -14, 0.02 at +4), 5.6 times the movie's ramp, and UIMessageCompData has no fade field, so that is open; rows ui.hud.* (kill_feed, kill_log, score_popup now trace the drawn alpha per frame against the timeline), hygiene.rules.
* Item 8, fixture fades: the six kill rows the reference reads mid-fade are timed into the row timeline's own ramp, the frame picked by the container alpha the probe hudmessage movie gives (fading out: the push step whose 5 s expiry, on the HUD's own message clock, triggers outAnim the right number of frames before the capture; fading in: pushed that many frames before it): m03 row 0 frame 12 alpha 0.398 (reference 0.41), m11 and m14 row 0 frame 13 0.199 (0.06, 0.00; frame 14 removes the row and lifts the others), m15 row 0 frame 13 0.199 (0.28), m09 rows 3 and 4 frame 2 0.500 (0.28, 0.43); mXX.kill_feed now also checks those frames; all 16 moments green; compare.py KillFeed core_Y 2.27 -> 1.56 (m09 29.84 -> 18.47; m03, m11, m14, m15 unchanged because our row 0 sits above the reference rows' window), still FAIL; rows godot.native_hud, hygiene.rules.
* Item 8, kit HUD icons: `KitDef::hud_icon_value` is the kit's UIHudIcon enum value (MP_Subway usassault 70, usengineer 76: the 1-based frame of its `Kit*` label in mc_iconsIngameSmall), `KitLoadout::hud_icon` carries it and the TDM feed writes it to `HudPlayer::kit_icon` per player row (team 1/2 only; the field sits in HudPlayer's padding, HudModel stays 9312 bytes); rows ui.hud.tdm.unit, core.gameplay.tables.unit, m2b.*, hygiene.rules.
* Item 8, squad list: hudtdmscreen SquadList_00 runs Widget.Hud.SquadList (onClipLoad hides squadMember0..3, renderer t2, tints txt and the kit/leader/voip/booster icons with the engine colour slot 0; updateSquadListData shows rows only for a list longer than one, Dead alpha 50 and kit frame 145, leader star frame 7 at x 60/40, label x 70/48 with a booster column and 50/28 without, voip at label x + textWidth + 15) bound from `HudSquad::local_squad_members` (new `leader` slot, in padding) and the players' kit icons; slot 0's text colour e4ff9d (the squadlist txt field's authored colour, not a measurement) is set as `SwfScene::set_text_color` and AS2 `Color.setRGB` (`As2Clip::set_text_color`, `set_rgb`); the shadow and glow colours of that slot are neither in the install nor measurable (names are masked), so tint2Texts' filters are not applied; rows ui.hud.screen.squad_list (new), ui.hud.*, core.ui.*, m2b.*, hygiene.rules.
* Item 8, native nametags: the `Bf3Nametags` Control (`iff_overlay.cpp`, with its `--iff-shots` / `--iff-live` switches) is deleted; `NativeTags` keeps its rules (decide_nametag, the reveal, at most 4 line-of-sight rays a frame) and `Bf3NativeHud` draws the tags on its own canvas below the movies: UIHudIcon_Player in the friendly/enemy state sized by IconSize, the squad kit icon for squad mates, names in `$Menu_medium` at the UINametagCompData size and tint, and team mates and revealed enemies on the minimap; the name's kit column (16 in 18 units) and black glow are inherited constants, not measured, and the HealthBarSize bar is not drawn; row nametag_icons new in godot.m2b.play; rows m2b.*, pt1.iff.unit, pt1.iff.install, godot.m2b.play, godot.native_hud, hygiene.rules.
* Item 8, Icon3D: SupportIconManager, MapmarkerManager and AlertManager run Widget.Icon3D.Icon3DManager (template tag hidden, `_level0.<path>` published under the UIWidgetPathBinding's WidgetPathQuery DataKey, MapmarkerManager 1055499673 and SupportIconManager 2138355157, ComponentName from the category's ShortName, shown on initialize, aligned to the visible rect without the safe rect) and their tags run Icon3D, SupportIconTag, Mapmarkertag and Alerttag (create/remove timelines stopped by createIconComplete and removed by removeIconComplete, IsClose/LookingAt/Snap snapping, the Icons one-element wrap, red markers for UIHudIcon 18/35/15/4, spotted, team frames on RadioBeacon-type icons, grIcon snapped/normal); tags come from `HudClientState::world_tags`, which nothing in TDM fills, and UI3dIconCompData placement (snap, fade, scale) is not done; rows ui.hud.screen.icon3d (new), ui.hud.screen.*, ui.hud.*, core.ui.*, m2b.*, godot.native_hud, hygiene.rules.
* Item 8, fixture squad rows: `HudPlayer::booster_icon` (in padding, HudModel stays 9312 bytes) feeds the SquadList BoosterIcon; the moment fixture fills the local squad per moment from the reference's SquadList readings (kit 70/71 or dead 145, boosters 138..144, row 0 the leader) with names built from W/M/H/N/E/I to the masked name rects (155 px for the leader row, 176 px for the others) through the probe squadlist movie; new rows mXX.squad_list, all 16 moments green; compare.py SquadList core_Y -108.36 -> 3.62 (still FAIL on solid_Y 3.40 > 2.22, tile_solidY and a 0.2/0.3 px shift), Minimap -21.75 unchanged; the game itself has no squads or boosters; rows ui.hud.screen.squad_list, ui.hud.*, core.ui.*, m2a.*, m2b.*, godot.native_hud, hygiene.rules.
* Item 8, squad colour: the SquadList's slot-0 colour is read from the squadlist txt field's authored DefineEditText colour (e4ff9d, txtBg 006600) through `As2Clip::text_color` instead of the `kNametagUiColourSlot0` copy, which commit 47d5ba5 and this log called a measurement; nothing measured it (the reference tools take it as an input); Data.UINametagComp.getUIColor is native, so slot 0 is taken as that authored colour, the txt colour is unchanged and only the kit/leader/voip/booster setRGB tint shows; ui.hud.screen.squad_list checks the tint against the movie's edit-text colour on all 4 rows; rows ui.hud.*, core.ui.*, hygiene.rules.
* Item 8, native tags hook: the unused `NativeTagFixture` and `NativeTags::set_fixture` (nothing called them; there is no world-icon fixture, so the fixture capture draws no world nametags and no minimap unit icons) are removed; rows godot.m2b.play (nametag_icons), godot.native_hud, pt1.iff.unit, pt1.iff.install, hygiene.rules.
* Licence: openBF3 is GPL-3.0 (`LICENSE`, the GNU GPL 3.0 text, 674 lines); CLAUDE.md now allows porting OpenBF2's bot AI (`Code/BF2/AI`, GPL-3.0) for the bot, squad and commander AI, with each ported file named in architecture.md; IW4L, Rime, fun-bots, IceBloc, Frosty and VU stay excluded; README gains a licence and disclaimer section in line with EA's published mod terms (free and non-commercial, no implied EA or DICE endorsement, no EA logos or artwork, no game files); rows hygiene.rules.
* Item 7, server stream row: host.server.stream hashes every datagram GameServer sends (the snapshot's wall-clock echo_hold_100us masked) and every tick's soldiers, weapons, teams and kits for a TDM run (30 bots, two scripted peers sending ahead, replaced and late commands, a trace, target info, kit select, a reconnect; 2,196 datagrams) and a dev run (44 walkers, target and fill bots, all seven dev requests, a spawn sweep, three kicks, a full server; 1,637 datagrams, 800 SnapshotOthers) against hashes recorded on 855fa87 (stream 65c3baafc7e25bd5 / 8350cdbfd5bca6c8); 12.9 s alone, 9.2 s of it the Metro load.
* Item 7, GameServer link order: the Sv hooks' state and TdmRules are held by plain std::unique_ptr; the function-pointer deleters existed because GameServer's constructor and destructor were defined above the Sv definition, so default_delete<Sv> met an incomplete type; host.server.stream hashes unchanged.
* Item 7, server outbox: GameServer's pending datagrams, their send and the 1,024-reply cap are `Outbox` (`server_outbox.h`); host.server.stream hashes unchanged.
* Item 7, command buffers: each client's jitter buffer, sequence echo, late/replaced bookkeeping and command counters left Impl::Slot for `CommandBuffers` (`server_commands.cpp`, take / apply / stamp / stats); host.server.stream hashes unchanged.
* Item 7, match control: the TDM rules, the fixed respawn policy, rule orders, the player-list cadence, the step-failure count and the match messages (MatchInfo fields, join and change broadcasts) are `MatchControl` (`server_match.cpp`); eleven sv_* rules hooks are gone; host.server.stream hashes unchanged.
* Item 7, slot table: the soldier slot record and its lookups (peer, free slot, live positions, index) are `ServerSlot` / `SlotTable` (`server_slots.h`) with the one server copy of `soldier_alive`; host.server.stream hashes unchanged.
* Item 7, combat: gunplay, the shot frame, per-slot kit / next team / target bot / gunplay trace / lag-comp record and the rules tick states are `Combat` (`server_combat.cpp`); thirteen more sv_* hooks are gone; host.server.stream hashes unchanged.
* Item 7, combat log: the hit / death / revive / detonation sink, its respawn scheduling and TDM forwarding, and the per-snapshot CombatEvents send are `CombatLog` (`server_combat_log.cpp`); host.server.stream hashes unchanged.
* Item 7, snapshot writer: per-client snapshots, the SnapshotOthers overflow, the ShotEvents split, the snapshot sequence and counters are `SnapshotWriter` (`server_snapshots.cpp`); three more sv_* hooks are gone; host.server.stream hashes unchanged.
* Item 7, spawner: spawn choice and settling, queued placements, revive / unspawn, rule orders, the Spawned message and the spawn rng are `Spawner` (`server_spawner.cpp`); four more sv_* hooks are gone; host.server.stream hashes unchanged.
* Item 7, dev commands: debug requests and replies, the state trace and the spawn sweep are `DevCommands` (`server_dev.cpp`), and their fields left `ServerSlot`; the last debug sv_* hook is gone; host.server.stream hashes unchanged.
* Item 7, slot view: the ServerView over slots, rules and gunplay is `SlotView` (`server_slot_view.cpp`) held by value, so `Sv` and its deferred definition are gone and no sv_* hook remains; host.server.stream hashes unchanged.
* Item 7, module map: docs/architecture.md names the `server_*` parts GameServer runs its tick over.
* Item 7, bot decision record: TdmBotObserver gains `commanded` (each brain's tick, activity and Command), and host.tdm.replay hashes every bot command per tick (seed 11, a 20-kill round over at tick 2,580, 3,179 ticks, 86,912 commands) against hashes recorded on 43749b3 (decisions 93ffe576c7006a93, world 588730c21e9cbc7e) besides its same-seed replay checks.
* Item 7, bot helpers: the TDM brain's rng draws, angle and distance helpers and weapon slot numbers are one copy in `tdm_bot_common.h` for the bot parts that follow; host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot world scan: the per-tick scan every TDM brain shares (teams, alive, the recent-shot ring, heard shots, firing marks), the heat map and make_tdm_shared are `TdmShared` in `tdm_bot_shared.cpp`; host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot awareness: each brain's perception, the enemy shots passing its chest, its damage mark and the aims-at-me test are `BotAwareness` (`tdm_bot_awareness.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot targeting: target scoring and switching, the reaction delay, target loss with its blind-fire point, the engaged mark and the aim error draws are `BotTargeting` (`tdm_bot_targeting.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot navigation: nav goals and their walker resets, the walker step, the off-main-mesh island escape and the heat-map roam goal are `BotNavigation` (`tdm_bot_movement.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot strafe: the side-to-side fight movement (anchor, axis, half width, reversal timer and the ground-ahead probe) is `BotStrafe` (`tdm_bot_movement.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot look-around: the idle view sweep a brain makes when it cannot walk (hold on a noticed enemy, a random 60 to 150 degree turn every 90 to 180 ticks) is `BotLook` (`tdm_bot_movement.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot weapons: the brain's weapon slots (`BotArms`), and the weapon input it sends (sidearm switch, fire-mode stepping, reloads, bursts, taps, knife and defib presses, the friendly-line check, recoil compensation) are `BotWeapons` (`tdm_bot_weapons.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot revive: choosing a downed team mate to revive, aborting it (gone, expired, 900 ticks, shot at, an enemy in sight within 40 m) and the defib retry clock are `BotRevive` (`tdm_bot_objectives.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot grenade: the grenade throw at a lost target (start window, the arc pitch, switch, hold and after phases) is `BotGrenade` (`tdm_bot_objectives.cpp`), reading its grenade through `BotArms`; host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot retreat: the retreat away from a threat and the regen wait (start, clear, run-out and recovery checks, threat facing) are `BotRetreat` (`tdm_bot_objectives.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot hunt: the hunt for the nearest known enemy (searched tick, start, give-up block, goal at the last position) is `BotHunt` (`tdm_bot_objectives.cpp`); host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot activity choice: the per-tick choice between revive, grenade, retreat, regen wait, fight, hunt and roam is `choose_activity` over a `BotFrame` of the tick's sighting, arms and revive target; host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot intents: what each activity wants this tick (move, pose, aim, slot, trigger, sprint) is a `BotIntent` from `plan`, with `fight`, `approach_revive` and `walk` in place of the switch body and the aim and walk lambdas; host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, bot view and command: turning the view toward the intent (rate limits, fire cone, sprint cone) is `turn` and the fire request plus the quantised command is `compose`, leaving `run` as sense, choose, plan, turn, compose; host.tdm.replay decision and world hashes and host.server.stream hashes unchanged.
* Item 7, module map: the architecture bots row names the `tdm_brain.cpp` run phases and the `tdm_bot_*` parts and the state each owns.
* Item 8, HUD layer coverage: the moment capture draws each view a second time with add-blend (blend 8) pieces writing their alpha (`Bf3SwfView::set_additive_coverage`) and builds the HUD layer from the first pass's colour and the second pass's alpha, because additive pieces write alpha 0 and the layer had unpremultiplied the score popup's light by it (white text came out at alpha 3..104); compare.py ScorePopup pooled core_Y -64.11 -> -34.06, gain_Y m06 -0.88 -> -0.19, m07 -0.89 -> -0.33, m12 -0.73 -> -0.14, m16 -1.11 -> -0.33, state readings 7/2/6/4 -> 20/65/20/14 and m07 cursor 9 -> 0 (all as the reference); new row "add blend coverage" in godot.swf_view; rows godot.swf_view, godot.native_hud, hygiene.rules.
* Item 8, fixture name scale: names are fitted in HUD layer pixels per stage unit (the view's stage fit, 1.000 at 1280x720) instead of the window's canvas width over the view's visible width (1152/1280 = 0.9 under the project's canvas_items stretch), which drew every fitted name 1/0.9 too wide (KillLog killer 190 px for the 172 px rect, victim 295 px for 267) and moved the centred KillLog row 5 px left; the KillLog [M16A3] is now at x 557..628 (552..623 before, footage 556..625); compare.py KillLog pooled mae_Y m06 28.88 -> 14.38; new rows mXX.name_scale (stage width x fit scale = layer width), all 16 green; rows godot.native_hud, hygiene.rules.
* Item 8, play.cmd: without OPENBF3_GODOT the launcher takes the first of `..\Godot_v4.8-dev7_win64_console.exe` and `..\..\Godot_v4.8-dev7_win64_console.exe` (from the checkout) that exists, so a worktree under `wt\` finds the Godot next to the main checkout; checked from a scratch copy with Godot one level up, two levels up and absent (exit 2).
* Item 8, fixture scores: `SharedLevel` keeps the install's `ScoringTable` (`persistence/scoring/scoring`), and the popup moments push ID_SCORE_AVENGER 10 and ID_SCORE_SAVIOUR 20 from it between the kill and the headshot (the footage shows SAVIOR 20 under HEADSHOT 10), so the ScoreAggregator reads the footage's 240 (was 210) and the fixture checks it as a row instead of printing a flag; new case "scoring" in host.tdm.server; rows host.tdm.server, host.tdm.client, hygiene.rules.
* Item 8, kill log tint: `Util.ColorTinting` (tint2Texts, tintIcon) runs for LocalKillLogMessage with `_colConvert` [1, 3, 0], its colour table read from the hudmessage kill-row name sprite's squad, friendly and enemy frames (txt colour, drop-shadow colour, glow colour), set through the new `SwfScene::set_filters`, which the timeline no longer overwrites; the footage's kill log killer core reads (221, 254, 153) and the victim (255, 246, 187) against the squad frame's e4ff9d and the enemy frame's ffeab9; compare.py KillLog core_Y 10.74 unchanged (the names are masked); rows ui.hud.*, core.ui.*, godot.swf_view, godot.native_hud, hygiene.rules.
* Item 8, kill feed in the footage: measured on 18.57 at 199..206 s, a kill feed row fades in over about 8 frames (peak white 5 -> 242) and out over about 28 frames (240 -> about 100, then gone in one frame), its rows 18 px apart from y 100 directly under the VU watermark box (y 0..84); the kill log (LocalKill rows of the same hudmessage movie and classes) fades out in 5 frames at 268..271 s (peak 251 -> 126), the timeline's outAnim; no install data scales, moves or fades HudMessage_02, so the movie's placement and timeline stay and the debt names the measurement; docs only, rows hygiene.rules.
* Item 8, text grid fit: DefineEditText fields take FlashType and GridFit from their CSMTextSettings tag, and a pixel-fit field (the TicketCounter team labels, GridFit 1) lays its baseline on the window pixel grid (floor of the baseline's window y through the world, stage and tilt transforms); compare.py TicketCounter label shift_y 0.60 -> 0.26 on m01 m05 m10 m13, pooled shift_y 0.21 -> 0.10 (threshold 0.12).
* Item 8, minimap registration: the minimap profile (MinimapTextureOpacity 0.5, MapZoom steps 0/0.4/1.2, small-map step 0, large 2) is read from the install, and the moment fixture poses the minimap from the footage registration (detail texel and map rotation per moment) through minimap_uv_to_world instead of a fitted metres-per-unit per moment; rows mXX.minimap texel and rotation error 0, compare.py Minimap pooled core_Y -21.75 -> -2.17 together with the layering commit.
* Item 8, minimap layering: the map is the profile's MinimapTextureOpacity (0.5) of DetailTextureTint with no satellite term (measured slope <= 0 on m01 m02 m13 m14 m16), the detail ColorTint (0.3) laid over where the detail field is above AlphaThreshold 0.5 (it was inverted), then the hatched combat-area colour, as premultiplied layers; dark video Y 76.3 against 77.2 and 76.5, light 122.5 against 122.0; compare.py Minimap pooled core_Y -21.75 -> -2.17, mae_Y 36.8 -> 26.9.
* Item 8, minimap local marker: the local player is UIHudIcon_LocalPlayer in the Squad state (green) when in a squad and Default otherwise, as the footage shows, instead of UIHudIcon_Player Friendly.
* Item 8, text grid fit not applied: the pixel grid-fit baseline snap is removed again (CSMTextSettings stays parsed): of floor and round in window and in stage space, every variant moved the TicketCounter labels toward the footage (pooled shift_y 0.21 -> -0.07..0.11) but moved Ammo and KillLog away (KillLog shift_y -0.51 -> -0.71..-1.12, Ammo tile_hpY 33.1 -> 39.4..50.8), so the snap is not the engine rule.
* Item 8, transmittance capture: the moment capture writes <moment>_trans.png beside each layer (8-bit L, round(255*T), normal pieces multiply T by 1 - alpha, add pieces keep it); godot.swf_view probes outside 255, add 255, normal 0.4 153, 0.6 over 0.4 61, add over 0.6 102; composited as E + T*W the ScorePopup pooled core_Y is 11.47 against -34.06 straight-alpha (compare.py itself unchanged).
* Item 8, capture determinism: the moment fixture starts the EKG clock at 0 when it settles a moment (it carried the real-time dt from before manual mode, so two runs differed only in x 1058..1163, y 558..584 and moved Health core_Y between -1.98 and -2.03); godot.native_hud captures m02 a second time after m10 and m13 and m02.repeat requires an identical HUD layer and transmittance: 0 of 4608000 bytes differ.
* Item 8, HUD heading: the compass heading is the clockwise bearing, minus the view yaw, and damage bearings follow it (indicator frame = bearing - heading + 1); the retail minimap rotation and compass frame sum to 361.5 on average over the 10 still moments (minimap rotation = -heading), which only holds with the heading mirrored from the earlier +yaw.
* Item 8, compass frame rule: the cps frame is the heading rounded up (it was rounded to nearest); over the 10 still moments the footage's minimap heading minus its compass frame averages -0.625 with standard error 0.17 (rounded up predicts -0.5, nearest 0), held in ui.hud.screen.unit.
* Item 8, one state per moment: the moment fixture places the soldier at the registered minimap texel (the combat-area registration at m07, where the detail one is 34 texels off), yaws it to the compass frame's heading (k - 0.5), and poses the minimap and the compass from that same state; the old world_x/z put the soldier 124, 124 and 171 m from the minimap at m01 m06 m07; rows mXX.state and mXX.minimap (state within 2 texels of the independent combat-area registration, bound 8, and the registered heading inside the compass interval) pass on all 16, compare.py Compass state mismatches 17 on 16 moments -> 0, Compass mae_Y 18.07 -> 11.56, Minimap mae_Y 26.94 -> 21.55.
* Item 8, minimap zoom: the MapZoom rule of playingminimapinit is read and applied: InitSmall (True) and the profile steps give the small map step 0 = zoom 0 and the large map step 2 = zoom 1.2, the player centred unless the large map is at IsMaxZoom (step >= 2); the HUD's Size and Zoom bindings take it from the install instead of the defaults (ui.minimap.unit, minimap.reference, mXX.minimap).
* Item 8, minimap hatch: the combat-area multiply texture repeats CombatAreaMultiplyWrapAmount (70) times over the map uv, anchored to the map, instead of a 10.6-unit tile around the player; the footage's stripes correlate 0.507 on average with the map-anchored phase and -0.013 with the player-anchored one (map-anchored better on 14 of 16 moments); compare.py Minimap mae_Y 21.55 -> 20.76.
* Item 8, iBgDraw edges: the panel coverage and its light take separate ramps of the AS2 fade, coverage 0.26 -> 0.73 (centred on the setWidth/setHeight val edge) and light 0.27 -> 1.05, measured per line on six panel edges (Ammo left/right/bottom, Health top, TDM top/right) from the temporal std and mean of 3607 moving at-rest footage frames against our projected geometry (spread over edges 0.14/0.09 coverage, 0.07/0.12 light); compare.py Health halo_Cb -4.77 -> -2.76 (threshold 4.48), Ammo halo_Cb -5.48 -> -4.42 (5.18), HudBackgroundWidget tile_absY at the TDM right edge (t59026, t59027, t60027) passes in every moment; Minimap halo_Cb 0.45 -> 1.20 (0.77).
* Item 8, gap round 2 stopped: the owner deferred pixel-exact HUD work on 2026-10-24, so the in-round HUD stays short of the strict 1280x720 screenshot gate and no gate row is added; Known debts names the main visible differences, and the minimap zoom debt now says the large-map zoom reaches the Zoom binding but not the drawn scale.
* No Rust, export commands: `bf3dump export cache|res|ant` write the opt-in samples that `core.cache_data` (`OPENBF3_CACHE_TESTDATA`), `core.res` (`OPENBF3_RES_DATA`) and `core.ant` (`OPENBF3_ANT_DATA`) read, through the install readers, formats taken from those consumers; MP_Subway cache sample at 256 MiB 5,296 entries in 17.8 s (core.cache_data PASS 11.3 s), Subway + Skybar res 3,797 + 2,238 res and 3.52 GB in 9.5 + 5.0 s (core.res 5/5 PASS, 13,966 subsets, 1,522 Havok roots; resdump summaries over the export and from the install SHA-256 42ffd1a3... and f04df8c4..., as the probe export gave), Ant 397 banks + 88 chunks, 382 MB in 2.4 s (core.ant 8/8 PASS on the export, antdump parity 485/485 payloads); new install rows tools.export.cache (4.1 s), tools.export.res (0.5 s), tools.export.ant (9.1 s).
* No Rust, probes removed: research/probes/tools/ebx-dump (IW4L crate copies), res-oracle and level-oracle (path-dependent on IW4L) are deleted with their Cargo files and Cargo.lock; git ls-files lists no .rs, Cargo.toml or Cargo.lock; README names only C++ tools and keeps the probe parity results (bf3dump list 1,094,723 lines, find/types/dump bytes, res summaries, the 230-run level and collision listings, MP_007 static models) as history.
* VU harness removed: research/probes/tools/vu-harness (the parked, buggy Venice Unleashed Lua mod with its Python and PowerShell drivers, 88 files) is deleted; no ctest row, tool, script or doc outside research/ used it; git ls-files lists no .lua file.
* IW4L-ported units kept: docs/architecture.md Known debts now states the owner's 2026-10-24 decision to keep the container, EBX, payload, resource, level-soup and lag-compensation units as they are (handoff item 9 cancelled) instead of listing their re-derivation; architecture.md 190 -> 189 lines.
* Item 8, crosshair spread function: the spread at any time without a shot is spread_at (decay from the stored shot state, the configured MinSpreadRise, the stance/zoom/move minimum) and the sway context is weapon_sway_context (stance from target_pose, zoom button, Sprinting, Jumping when jumping/in air/falling, Moving above 0.1 m/s); fire_shot evaluates the same function, shot spreads bit-identical (memcmp row), no hash change.
* Item 8, crosshair presentation: present_weapon takes the client's sway context (weapon_sway_context of the predicted soldier and command, suppression 0 on the client) and presents spread_at of the shot state held at the frame time (the previous tick's state before a shot inside the current tick), so the HUD spread decays and follows stance, move, jump and zoom every frame instead of lerping the stored shot spread; no wire change, no hash change.
* Item 8, crosshair mapping: crosshairdefault's UICrosshairDataBinding DispersionBaseOffset (0.151) is read and sent as the Crosshair BaseValue, and Dispersion is 0.2 per degree of spread, measured on the 18.57 footage at FOV 100 (M16A3 + suppressor 85 standing / 99 walking, + heavy barrel 75 / 87, 10 px per 30 fps frame of decay), instead of the tan/vfov projection with no offset; ui.hud.feed.install replays the footage-shaped sequence through the sim, the feed and the binding and prints the sprint, jump, stop and burst arms where the sim differs.
* Item 8, crosshair walk row: godot.crosshair_walk (window) spawns in Metro TDM, stands at the m02 floor, walks forward 1.5 s and stops without touching the trigger, and checks that the live native HUD's crosshairdefault arms sit at the held item's standing hip minimum, open to the moving minimum on every walking frame and close again (M16A3: 65 / 75 / 65), with no shot fired.
* Item 8, crosshair walk ramps: in the 18.57 footage the arm opens over 6-8 frames when walking starts (heavy-barrel M16A3 75 -> 82 84 85 86 86 86 87) and closes over 7 when it stops (suppressed 98 -> 85), not in one frame; both equal the moving minimum blended by the install motor's speed / walk speed (4 m/s) within 1 px, where the sim switches at 0.1 m/s (5 and 6 px off); ui.hud.feed.install drives the walk with SoldierMotor and holds both ramps.
* Item 7, bot awareness damage mark: BotAwareness's damaged tick and the last health that fed it are removed; nothing read them (TdmBrain, the bot parts, the tests and the tools), and the install carries no bot data that would; host.tdm.replay decisions 93ffe576c7006a93 / world 588730c21e9cbc7e and host.server.stream 65c3baafc7e25bd5 / 8350cdbfd5bca6c8 unchanged, 23 host, net and m2b.contract rows with identical outputs.
* Item 7, defib retry clock: BotRevive alone writes it; BotWeapons reads defib_ready from the FireRequest and reports defib_fired in its WeaponCommand, after which the brain tells BotRevive (fired_defib, 66 ticks), so the mutable defib_next() reference and BotWeapons::input's tick out-parameter are gone; host.tdm.replay decisions 93ffe576c7006a93 / world 588730c21e9cbc7e and host.server.stream hashes unchanged, 23 host, net and m2b.contract rows with identical outputs.
* Item 7, bot sensing step: the shot check, the perception tick, target choice and tracking and the revive check that opened TdmBrain::run are its sense() step, returning the BotFrame, over BotAwareness::sense (shots, then perception when the stagger says so) and BotTargeting::sense (choice after a perception update, then tracking), with check_shots, perceive, choose and track private; run reads restricted, sense, choose_activity, plan, turn, compose; host.tdm.replay decisions 93ffe576c7006a93 / world 588730c21e9cbc7e and host.server.stream hashes unchanged, 23 host, net and m2b.contract rows with identical outputs.
* Item 7, slot ownership: ServerSlot is gone; each slot field has one owner that alone writes it and the other parts read it const: SlotTable's Seat (used, remote, dummy, bot, peer, team, the command source; admit_client / admit_local / admit_dummy / assign_team / clear), Soldiers (spawned, state, last command, events for the next snapshot; enter, replace, enter_probe, teleport, leave, command, step, take_rules, collect_events, take_events) and Spawner (the respawn wish and tick; ready, died, killed, spawn_due); the write-only spawn_index and source_command and the hello flag (always equal to remote) are dropped, and Spawner takes the revive sink per call so CombatLog can hold the Spawner; host.server.stream 65c3baafc7e25bd5 / 8350cdbfd5bca6c8 (states 770658d60fd190bc / a30c60cf985a7a37) and host.tdm.replay hashes unchanged, 23 host, net and m2b.contract rows with identical outputs.
* Item 7, client seam: ClientSession::Impl keeps the thread, the lifecycle and the stall; the receive, tick and publish loop moves into ClientLoop (client_loop.*), which reaches the server through a ClientTransport (EnetTransport over net::Host in client_session.cpp) and reads time from a MonotonicClock, so a test can drive it on a stepped clock; host.*, net.*, the contract rows and host.tdm.client pass as on c2264ba (37 + 1 rows).
* Item 7, host.client.stream: three ClientLoops (one lossy with predicted-shot logs and debug requests, one with clock shifts and a kit change, one refused) against a 26-bot TDM server and one among 44 walkers and target bots (300 SnapshotOthers), stepped at 8 iterations per tick, hash per tick what each client sends, every published frame, the presented others and weapons, the event feed, the predicted shots, timed events, own test samples and debug reports, and the final stats; the same hashes on two optimisation levels and under a -j6 pass, and one changed byte (the receive slew step 0.05 -> 0.06) fails five of them; 19.7 s alone.
* Item 7, ClientLink: the transport, the two NetSims, the connection and error state, snapshot sequence and ack bits, the command history and packet, and the debug, kit and spawn request queues move out of ClientLoop into client_link.*, which fills its own ClientStats fields; host.client.stream hashes unchanged.
* Item 7, ClientClock: the client clock (base, rate, next tick), the server-tick anchor and estimate, the hello and snapshot RTT with the sent-command times, the buffer-report slew, holdoff, reset and forced-forward, the shift and the burst rule move out of ClientLoop into client_clock.*; a reset hands the prediction discard tick back to the loop; host.client.stream hashes unchanged.
* Item 7, RemoteTracks: the other soldiers' sample tracks, the receive-time offset slew, the arrival interval statistics, the interpolation delay, the generation-reorder, timeout and others-ahead counts and the poses sampled for the projectiles move out of ClientLoop into client_tracks.*; host.client.stream hashes unchanged.
* Item 7, ClientEvents: the PresentationEvents feed with its overflow count, the own-shot id remap and resets, the revived-life flags of own spawns and the projectiles' own-event staging ring move out of ClientLoop into client_events.*; host.client.stream hashes unchanged.
* Item 7, ClientMatch: the TDM match state, the player table, the rules check, the own death (respawn and revive ticks), revive events, command restriction and the unspawned rule move out of ClientLoop into client_match.*; bad_gunplay_events stays the sum of the loop's and ClientMatch's counts; host.client.stream hashes unchanged.
* Item 7, ClientGunplay: the weapon table check, the predicted weapon lane and its emit sink, the received-record ring, command views, kit and team per life, the snapshot weapons wire, remote shots, combat events, the client projectiles and the predicted-shot, timed and own-sample logs move out of ClientLoop into client_gunplay.*; the loop attaches the lane to the prediction ring whenever one was made, as before; host.client.stream hashes unchanged.
* Item 7, ClientInput: the input mailbox and its mutex, the command sequence, the jump edge, the kept pose and the last stepped command that a free camera repeats move out of ClientLoop into client_input.*; host.client.stream hashes unchanged.
* Item 7, OwnPrediction: the soldier motor and its hash check, the prediction ring, the own life and spawn state, the last authoritative state, reconciliation, ring misses, base fills, the step precondition, the health rebase, the visual offset and its decay, and the soldier events published move out of ClientLoop into client_prediction.*; ClientLoop (client_loop.cpp 355 lines, was 1717) keeps only the receive, tick and publish order; host.client.stream hashes unchanged.
* Item 7, module map: architecture.md lists ClientLoop's client_* parts beside the server's (199 -> 200 lines); host.client.stream hashes unchanged.
* Screens S2a, controller registry: the AS2 controllers move out of hud_widgets.cpp's if-chain into one file per class family under src/bf3_core/src/ui/widgets/, each registering its classes in HudWidgetRegistry (the build globs the directory into register_hud_widget_files); 33 classes, the same as the chain; rows ui.hud.* (new registry case), m2b.contract/contract2/gx.unit/cam.unit, hygiene.rules, godot.native_hud (HUD layer and transmittance byte-identical to 6bbc2f6)
* Screens S2a, controller input hooks: HudWidgetController gains input_concept, key, mouse_move/press/release (HudMouse at stage coordinates) and focus_gained/lost with no-op defaults, the entry points the menu screens' controllers will take input through; no in-round controller handles input; rows ui.hud.* (new ui.hud.screen.input_hooks: 63 controllers over the 4 in-round screens, hooks unhandled, no scene change), m2b.contract/contract2/gx.unit/cam.unit/unit, hygiene.rules
* Screens S2a, moment fixture race: the native HUD moment run leaves WaitAlive only once Bf3NativeHud::minimap_ready() holds, within the same 90 s timeout (new failure row minimap_ready), so m02 no longer sees minimap maps 0 when the satellite texture lands after the spawn; rows godot.native_hud (window; HUD layer and transmittance byte-identical to 6bbc2f6), hygiene.rules
* Screens S2a, screen host: UiScreenHost (ui_screen_host.h) opens, shows, hides, orders and closes a screen (HudScreen plus its Bf3SwfView) by partition and lists its slots; Bf3NativeHud hosts its four in-round movies through it (the public view(i)/screen(i) go), and the moment helpers read screens through the interface; the native HUD moment run checks the lifecycle after its captures (host.slots/show_hide/order/close_open/foreign); rows godot.native_hud (window; HUD layer and transmittance byte-identical to 6bbc2f6), godot.m2b.play, godot.crosshair_walk, godot.m2a_hud, hygiene.rules
* Screens S2a, moment fixture split: native_hud_moments.cpp keeps the run (phases, captures, repeat compare, host check), the shared screen helpers and the group registry; the 16 in-round moments, their kill/popup/squad tables, settle and checks move to src/bf3_godot/src/moments/in_round.cpp as a NativeHudMomentGroup, and the build globs src/moments/ into register_native_hud_moment_files; rows godot.native_hud, all 16 moments (32 HUD layer and transmittance PNGs byte-identical to 6bbc2f6, the same 261 rows and flags in the same order), godot.crosshair_walk, hygiene.rules
* Screens S2a, screen records: UiScreenRecords (ui_screen_records.h) reports per screen partition the visible flag, the shown count and named facts, plus the lookup counts, and UiScreenRecordSet composes sources; Bf3TdmHud reports its five TDM screens (partitions in hud_screen.h, read from the install by ui.hud.screen.assets) through tdm_hud_records.cpp, Bf3NativeHud its four, and m2b_check and the TDM UI demo read through the interface instead of TdmHudRecords; rows ui.hud.*, m2b.contract2 (new records case), m2b.contract/gx.unit/cam.unit/unit, godot.m2b.play (same 15 rows, all PASS), the TDM UI demo (the same 9/9 rows and details as 6bbc2f6), godot.native_hud (new host.records), hygiene.rules
* Screens S2a, module map: architecture.md lists the screen host and records (ui_screen_host.h, ui_screen_records.h, tdm_hud_records.cpp, the controller input hooks) and the moment fixture's run and groups (native_hud_moments.cpp, src/bf3_godot/src/moments/), and the in-round HUD row names its four movies (200 -> 202 lines); rows hygiene.rules
* Screens S2a, screen focus: HudScreen::set_focus(bool) delivers focus_gained/focus_lost to every live controller once per change and has_focus() reports it, the one place a screen host fires the focus hooks from; rows ui.hud.* (30; ui.hud.screen.input_hooks gives and takes back focus on the 4 in-round screens, no scene change), m2b.contract/contract2/gx.unit/cam.unit, host.client.m2b.unit, hygiene.rules
* Screens S2a, host focus and modal: UiScreenSlot carries modal and focused, UiScreenHost gains set_screen_modal, focus_screen and clear_focus plus focused_screen()/top_modal_screen() read from the slots; Bf3NativeHud's four in-round screens are never modal and never take focus (both refused, clear_focus a no-op); new moment row host.focus_modal; rows godot.native_hud (window; HUD layer and transmittance byte-identical to 6bbc2f6), all 16 moments (32/32 layer and transmittance PNGs byte-identical, the same 258 rows in order plus 7 host.*), godot.m2b.play (15 play.* PASS), hygiene.rules
* Screens S2a, failed screen open: Bf3NativeHud::open_view frees the view it added when the movie or the HudScreen does not open (drop_view, shared with close_screen), so a failed reopen leaves the slot closed instead of an orphan Bf3SwfView; rows godot.native_hud (window; HUD layer and transmittance byte-identical to 6bbc2f6, 7 host.* PASS), godot.m2b.play (15 play.* PASS), hygiene.rules
* Screens S2a, module map: the screen host row names modal and focus (Bf3NativeHud's screens never modal, never focused) and HudScreen::set_focus as where the focus hooks fire (202 lines); rows hygiene.rules
* Item 8, crosshair move blend: weapon_sway_context blends the stance's Base dispersion into its Moving one by horizontal speed / walk_speed (the motor's full-stick on-ground speed for the pose and the velocity's direction) instead of switching above 0.1 m/s, so the presented arm rises and falls with the motor as in the 18.57 footage (sim ramp within 1 px of 82..87 and 98..85); host.server.stream, host.client.stream (alpha, bravo, delta) and host.tdm.replay re-pinned, reproduced exactly by the old threshold in an uncommitted scratch build; sim.weapons and host.gunplay.replay unchanged.
* Item 8, crosshair ramp rows: ui.hud.feed.install's move_onset and move_stop rows assert the sim's presented arms through the SoldierMotor walk (81..87 and 98..86, within 1 px of the footage) and that they equal the speed / walk speed blend frame for frame; the two INFO lines that printed the sim's thresholded ramp are gone.
