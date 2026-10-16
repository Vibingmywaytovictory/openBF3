# Terrain collision everywhere: design

Status: design revision 2 (2026-10-16). Interface headers are in the tree (§13); the implementation has not started.
Track "Terrain collision everywhere", branch `terrain/collision` (based on main 7b3e8fa). It merges before the
parallel branches ui/swf-fonts, ui/minimap-icons, m2a/weapons, m2a/audio, m2a/vfx and m2a/design (§14.3).

Revision 2 (after review of revision 1):
* G3 now has pass thresholds for the largest level × mode and for Metro, measured by one ctest (`tools.tc_cost`, §2,
  §4.5).
* G1 covers all 199 terrain runs. Each mode's terrain record is checked against the cooked field by its content
  digest, and each mode's own combat-area tiles are cast. It adds an exhaustive field-against-record check and rays
  into both triangles of every mismatch cell (§2, §12.1).
* G5's command now runs `sim.vehicle.unit` as well (G1m and the synthetic courses). A new synthetic case, G1t, drives
  three vehicles over a heightfield with a patch (§2, §12.1).
* Capsule sweep and overlap cost against the field were measured (§4.3). The Havok geometry around the Caspian spawns
  was measured too (§9).
* The plan deviation from D5 / D17 is recorded (§5.2). The Jolt citation now names the triangle walker.
* The "OBF3JTER" decode list is complete, `hash_soup_terrain` takes a `WorkBudget`, and the empty cases are defined:
  an out-of-range cell, and an all-hole field (§11).
* Other changes:
  * the nav-world key gets `terrain=` (§10);
  * `StaticWorldShapes` bounds include the terrain (§5.3);
  * the IW4L commit and the G2 wall time are recorded (§6);
  * the asan regex is wider (G7);
  * the direction of the S1 G2 count change is predicted (§7).

Tags: **[verified]** means checked in this tree or on the install on 2026-10-16, by the command given.
**[measured]** means a timing or size from a run on this machine; timings are noisy (shared machine, see G3).
**[inferred]** means not checked.

---

## 1. Goal

Every terrain cell of every TerrainEntityData has collision. Holes stay exactly as now: a cell is open when one of
its four samples is a hole. The cells no longer depend on where the Havok placements are.

Today the soup keeps a terrain cell only when it lies in a 64 m tile that holds a Havok triangle or touches one
(the IW4L near-object rule). Open ground away from objects has no collision:

* on Caspian CQL, 35 of 273 combat-area tiles had no soup triangle at all;
* on Kharg CQL, 1,119 of 1,494 had none (s1.md §5.2).

S1 patched this with `SoupOptions::terrain_cover`, extra tiles fed with the combat-area tiles. That patch has costs:
* it leaks into five APIs;
* it adds a key parameter to the caches;
* it makes the client prediction world depend on the server's cover (the S1 "client cover parity" risk).

This track replaces the rule and removes the patch:

* the soup's terrain becomes a **record**: the height grid plus the object's frame, with no triangles in the tiles;
* the static world gets **one Jolt `HeightFieldShape` per terrain**, plus a tiny patch mesh so holes match the soup
  exactly (§5);
* `terrain_cover` and `cover_water` go away, and their callers get simpler;
* `kCollisionSoupVersion` goes to 2;
* the water rule moves to the nav bake, per triangle (§7). The nav stays bounded to the combat areas;
* the level-oracle (Rust) changes in one place (the `near` filter goes), so the 230-run parity stays byte-identical
  (§6);
* the Godot collision view and the M1 game use the new default. The overlay's mesh rows keep their meaning (§8);
* the 24 Caspian RushLarge0 spawns that sit about 30 m under the terrain are explained (§9, G6).

Out of scope: destruction, terrain decals and paving (IW4L's block merging), swimming, and LOD for the overlay.

---

## 2. Exit gates

Every gate has one documented command. Timings are noisy, because the machine is shared (the noise rule is below the
table).

| id | what | command | pass threshold |
|---|---|---|---|
| G1 | coverage | ctest **`sim.terrain`** (`test_sim_terrain --test-suite=sim-terrain`, RUN_SERIAL), over **all 230 runs** in `levelmodes` order (§12.1 has the procedure): (a) every run's soup terrain record and its content digest; (b) one terrain-only world per distinct digest; (c) a 16 m lattice of vertical rays into both triangles of a cell, the exhaustive field check, and rays into each mismatch cell, per digest; (d) every run's own combat-area tile centres cast against its digest's world; (e) Caspian CQL, Kharg CQL and Metro TDM through the full static world as well | **0** lattice or tile-centre points of non-open cells without a terrain hit within **5 cm** of the expected height; **0** terrain hits in open cells. Every run's record digest equals its level's; the 31 runs without terrain have no record. Exhaustive: field + patch triangles = record cells one to one on every cell, and every height is within `max_error`. `holed` = 1 / 2 / 1 on MP_001 / MP_003 / XP4_Parl and 0 elsewhere. **0** combat-area tiles without ground on Caspian CQL and Kharg CQL (before: 35/273 and 1,119/1,494). Printed per level: lattice points, occluded, worst error, `holed`, `patch_triangles` |
| G2 | parity | `pwsh research/probes/tools/level-oracle/parity.ps1 -Out <scratch>` (new): `bf3dump collision` and `level-oracle collision` for every level × mode, compared byte for byte | **230 / 230** runs identical; 0 missing, 0 non-finite. The script prints the IW4L commit the oracle builds against (`git -C <IW4L> rev-parse HEAD`) and the wall time; both go in the README row (§6) |
| G3 | cost | ctest **`tools.tc_cost`** (new, `tests/tc_cost.cmake`, RUN_SERIAL; under the quiet lock): `bf3dump shapes <L> <M> --cache=<fresh scratch root>` cold, warm, warm in child processes for MP_Subway TDM, XP3_Shield RushLarge0 (largest soup today) and XP3_Alborz SquadRush0 (largest Havok soup after TC). Then ctest **`godot.m1_load`** (windowed). One command for both: `ctest --preset dev -R "^(tools\.tc_cost\|godot\.m1_load)$"` | Per run, the §4.5 limits: Shield RL and Alborz SR cold shapes total ≤ **6.0 s**, warm ≤ **0.5 s**; cold peak working set and peak private bytes ≤ **1.5 GiB**, warm ≤ **0.5 GiB**; Jolt resident ≤ **150 MiB**; cache ≤ **160 MiB**. Metro TDM cold ≤ **1.4 s**, warm ≤ **0.1 s**, peak ≤ **0.6 GiB**, resident ≤ **30 MiB**. **E4 ≤ 15 s and E5 ≤ 5 s** still pass. The before / after table is printed (§4.4 before). Report-only under asan |
| G4 | S1 re-run | ctest **`host.scale`** (G1, G3 rows), ctest **`sim.nav.bake`** (G4), ctest **`sim.nav`** (G2 rows) | `host.scale` p99 ≤ **8.0 ms** on Metro and Caspian (valid runs); the G3 six numbers reported. Bake ≤ **6.0 s** Caspian and ≤ **13.0 s** Kharg, with `tiles_wet` / `triangles_unwalkable` printed beside S1's 1,290 dropped cover tiles. G2 rows reported with the S1 numbers beside them (485/566, 661/709); the prediction is in §7 (no row may fall without a named cause) |
| G5 | S2 re-run | `ctest --preset dev -R "^sim\.vehicle(\.unit)?$"` (`sim.vehicle.unit`: the 7 synthetic courses, G1m, and the new G1t on a heightfield; `sim.vehicle`: the 7 MP_007 courses) | G1, G1r, G1m and G1t: 0 mismatching ticks (bit-identical) on all 14 courses and both groups; G2x `ExactPod` max **< 0.02 m** per found course |
| G6 | Caspian spawns | ctest **`sim.terrain`**, case "Caspian RushLarge0 underground spawns" (§9) | the evidence holds as asserted: the cause is in the data; no TC fix |
| G7 | hygiene | full `ctest --preset dev` with `OPENBF3_GODOT`; `tools\build.cmd asan` then `ctest --preset asan -R "core\.level\.collision\|sim\.world\|sim\.terrain\|sim\.nav\.unit\|sim\.vehicle\.unit\|tc\.contract"`; `install.unchanged` rows; a windowed capture (§8.3) | all pass (77 = skipped only where it is today); install unchanged in every run; capture saved to `openBF3\b\shots\tc\` (never committed) and inspected |

G1 is stated against the grid floats, so the error includes the field's 16-bit quantisation (≤ 1.1 mm on 23 of the
24 terrains and 11.1 mm on MP_013, §5.4). 5 cm is the task's threshold. The real margin is printed.

The timing rows (G3, G4's bake and tick, and G5) follow one noise rule. Each runs under the quiet lock (§12.4). A
failing timing is re-run once under the lock, and the second result counts; both are reported.

---

## 3. The current rule and its callers [verified]

**The rule** is in `src/bf3_core/src/level/collision_soup.cpp`:

* `terrain_grid` (line 369): the height grid at the first quadtree depth whose spacing reaches 2 m.
  The loop is `while (depth < max_level && span / (cells << depth) > spacing * 1.001 && (cells << (depth + 1)) < 4097)`.
* `dry_cover` (467): S1's water rule over the cover tiles.
* `TileMask` / `near_tiles` (548, 562): the Havok tiles and their 8 neighbours, plus the cover.
* `terrain_triangles` (606): each cell whose centre tile is in the mask and which has no hole corner. It emits
  (a, d, b) and (a, c, d), faced up, rows along +Z.
* The terrain loop in `build_collision_soup` (1109–1180): sorts the cover, applies `dry_cover`, adds the terrain
  triangles to the tiles, and checks `kMaxSoupTriangles`.

**The oracle**: `research/probes/tools/level-oracle/src/collision.rs` `terrain()` (386) skips a cell when
`!near.contains(&(tx, tz))`. The `near` set is built at line 572 from the Havok tiles' 8-neighbourhoods.

**Every use of `terrain_cover` / `cover_water`** (grep over src, tools, tests):

| where | what | after TC |
|---|---|---|
| `collision_soup.h` (SoupOptions, `CollisionSoup::cover_tiles_wet`) | the option and the counter | removed |
| `collision_soup.cpp:463–532, 614, 1114, 1160` | `dry_cover`, the mask, the loop | removed; the loop keeps the record |
| `static_world.h` (`StaticWorldLoadOptions::terrain_cover`, `cover_water`) | cold-path soup options | removed |
| `static_world_cache.cpp:556–559, 681–682, 731, 758`; `static_world_internal.h:119–122` (`terrain_cover_digest`, `cover_water_digest`) | `cover=` / `water=` in the "jolt-world" key | removed. The key gets the soup version 2 instead (§10) |
| `nav.h:12, 286, 300`; `nav_cache.cpp:225–227, 323–324` | nav-world key over the world options' cover | the key takes `NavBakeOptions::water` (`water=`); no cover |
| `level_load.h:114–133`; `level_load.cpp:275–280` (`SharedLoadOptions::terrain_cover`, `SharedLevel::terrain_cover`, `cover_water`) | pass-through and record | removed |
| `bot_fill.h:5, 118`; `bot_fill.cpp:153–161` (cover mismatch check), `173–174` (`nav_options.world.terrain_cover`) | the bots' nav load must match the shared level's cover | the mismatch check goes; water goes to `NavBakeOptions::water` |
| `scale_bench.h:6`; `scale_bench.cpp:222–223, 699–700` | bench soup and shared level options | removed; `tiles_wet` becomes the nav's unwalkable-triangle count |
| `level_session.cpp:392–393` (Godot `--bots`) | cover and water into the shared load | removed; water into the bot nav options |
| `tools/bf3dump/nav_commands.cpp:172–173, 485–486` | `nav bake` / `nav components` | same as the bench |
| `tests/s1_contract_tests.cpp:133–139`; `tests/sim_nav_install_tests.cpp:181–182, 340–341` | defaults and the gate runs | contract lines removed; tests pass water to the bake |
| `docs/design/s1.md` §5.2, `s2.md` §4.4 / §13 | prose | a note pointing here |

Other consumers of soup terrain triangles that change meaning:
* `collision_prep.cpp:139` (layer 4);
* `collision_check.cpp:51–143` (load, recompute and oracle rows);
* `collision_overlay.cpp:293, 572–582` (status text);
* `scale_bench.cpp:718` (the `kSoupTerrain` test for "tiles with objects");
* `tests/collision_tests.cpp:227–283` (`Expected`);
* `tests/sim_world_install_tests.cpp:635–` (the brute-force ray reference over the soup);
* `tests/sim_world_support.h:77`, `sim_world_tests.cpp:349` (synthetic tags: unchanged).

**Parallel branches** [verified with `git grep` on each branch head]:
* None of the six branches references `terrain_cover`, `cover_water` or `cover_tiles_wet`.
* m2a/weapons (`tests/weapons_install_tests.cpp`) sums `soup.tiles[*].triangles` and checks the sum equals
  `soup.triangles`. TC keeps that identity (`triangles` = tile triangles = Havok only, §5.1).
* All branches carry `sim_world_install_tests.cpp:753` (`hit.tile == only->tri->tile`) and `sim_world_tests.cpp:60`
  (`shapes->tile(hit.tile)` on a synthetic world without terrain). The first gets terrain reference triangles with
  `tile = kNoStaticTile` (§12.2). The second is unaffected.

---

## 4. Data facts [verified]

### 4.1 Census

Source: a scratch probe over `bf3_core` (`terrain_grid`'s rule, the streaming tree, `height_grid`) and Jolt 5.6.
It ran on all 230 level × mode runs, `levelmodes` order, on 2026-10-16. The probe is not committed. Its output is in
the scratchpad: `tc-design/census/{jolt,facts}.txt`.

* **199 runs have exactly one TerrainEntityData; 31 have none.** The 31 are MP_011 (7 modes) and the four Close
  Quarters levels XP2_Factory, XP2_Office, XP2_Palace and XP2_Skybar (6 each). This matches the parity run's
  199 T lines.
* **24 terrain levels**, and every mode of a level reads the same terrain: same asset, grid, holes and height range
  in every mode.
* **Every frame is the identity** (199/199: right, up and forward unit vectors, translation 0, scale 1).
* **Every spacing is exactly 2 m.** Every origin is −(side − 1) m, so the grids are 64 m tile-aligned: a 64 m tile is
  exactly 32 × 32 cells.
* **Mismatch** (below) counts cells where Jolt's hole rule differs from ours. A cell differs when its only hole
  corner is b or c (§5.2).

| level | side | depth | hole samples | open cells | kept cells | terrain triangles after | mismatch cells | grid heights (m) |
|---|---|---|---|---|---|---|---|---|
| MP_001 | 513 | 2 | 180,608 | 180,224 | 81,920 | 163,840 | 1 | 61.2–80.3 |
| MP_003 | 4097 | 5 | 10,487,808 | 10,485,760 | 6,291,456 | 12,582,912 | 2 | 133.3–639.9 |
| MP_007 Caspian | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 114.4–720.6 |
| MP_012 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 100.5–507.3 |
| MP_013 | 4097 | 5 | 4,195,328 | 4,194,304 | 12,582,912 | 25,165,824 | 0 | 0.0–1626.9 |
| MP_017 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 59.4–381.7 |
| MP_018 Kharg | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 32.9–319.7 |
| MP_Subway Metro | 513 | 2 | 164,224 | 163,840 | 98,304 | 196,608 | 0 | 53.7–74.2 |
| XP1_001 | 1025 | 3 | 0 | 0 | 1,048,576 | 2,097,152 | 0 | 122.5–281.9 |
| XP1_002 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 0.0–889.5 |
| XP1_003 | 2049 | 4 | 0 | 0 | 4,194,304 | 8,388,608 | 0 | 49.5–299.5 |
| XP1_004 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 50.9–126.9 |
| XP3_Alborz | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 80.7–906.2 |
| XP3_Desert | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 14.3–439.5 |
| XP3_Shield | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 40.0–934.0 |
| XP3_Valley | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 32.4–936.9 |
| XP4_FD | 2049 | 4 | 3,147,776 | 3,145,728 | 1,048,576 | 2,097,152 | 0 | 54.3–296.7 |
| XP4_Parl | 4097 | 5 | 14,685,184 | 14,680,064 | 2,097,152 | 4,194,304 | 1 | 45.9–447.0 |
| XP4_Quake | 1025 | 3 | 787,456 | 786,432 | 262,144 | 524,288 | 0 | 87.9–222.4 |
| XP4_Rubble | 513 | 2 | 65,536 | 65,536 | 196,608 | 393,216 | 0 | 13.1–225.7 |
| XP5_001 | 4097 | 5 | 1,048,576 | 1,048,576 | 15,728,640 | 31,457,280 | 0 | 47.0–720.4 |
| XP5_002 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 0.0–834.4 |
| XP5_003 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 0.0–784.2 |
| XP5_004 | 4097 | 5 | 0 | 0 | 16,777,216 | 33,554,432 | 0 | 0.0–355.9 |

"Terrain triangles after" is 2 × kept cells. No retail terrain cell can be degenerate: with spacing 2 and an identity
frame, two corners differ in x or z by 2 m.

**Largest after TC.**
* Terrain: 16 of the 24 levels reach the whole 4097² grid, 33,554,432 triangles. That is exactly `kMaxSoupTriangles`
  (2^25).
* Havok soups, from the soup listings in `w3b-coll/all2`:
  * XP3_Alborz SquadRush0 and RushLarge0: 2,662,845 Havok triangles each, the largest;
  * MP_018 CQL: 2,072,342;
  * MP_007 CQL: 1,802,167;
  * XP3_Shield RushLarge0: 1,319,099 Havok plus 2,695,168 terrain, 4,014,267 in all, today's largest soup;
  * Metro TDM: 940,091 Havok plus 174,080 terrain.

### 4.2 What triangles would cost

* A whole 4097² terrain as soup triangles: 33.5 M × 48 B = 1.6 GB per soup. That is over the soup cap before any
  Havok triangle.
* As a Jolt MeshShape: about 19 B per triangle (measured on the M1 tiles: 72.6 MiB resident for 4.01 M triangles on
  Shield RL, §4.4), so about 640 MB per level, plus about 16,384 extra tile shapes.
* The triangle form is out. This is why the terrain becomes a record.

### 4.3 Jolt HeightFieldShape [measured, scratch probe, one thread, 2026-10-16]

One `HeightFieldShapeSettings` over the whole grid:
* offset (origin x, 0, origin z), scale (2, 1, 2);
* holes = `cNoCollisionValue`;
* built single-threaded.

Accuracy = max |`GetPosition` − grid height| over every non-hole sample. Rays = 200,000 random vertical
`CastRay`s.

| bits / block | size (4097²) | max error, worst level | ray |
|---|---|---|---|
| 8 / 2 | 59.4 MiB | 1.59 m | 0.70–1.22 µs |
| 8 / 4 | 31.4 MiB | 2.86 m | 0.70–1.27 µs |
| 12 / 4 | 39.4 MiB | 17.8 cm | 0.73–1.49 µs |
| **16 / 4** | **47.4 MiB** | **11.1 mm (MP_013); ≤ 1.1 mm on the other 23** | **0.72–1.34 µs** |
| 16 / 8 | 40.5 MiB | 11.2 mm (MP_013) | 1.03–1.70 µs |

The ray column covers the levels where every ray hits; rays into holes are cheaper.

* Build time at 16 / 4: 0.51–1.07 s for a 4097² grid, 0.10–0.17 s for 2049², 0.05 s for 1025², and 0.006–0.021 s
  for 513². Metro is 0.75 MiB with 0.2 mm error.
* Reading the grid (`terrain_grid`) takes 0.09–0.42 s per 4097² terrain.
* Per-tile fields (one 33 × 33 field per 64 m tile, 16 / 8) measured 65 MiB and 0.1 s wall on 16 lanes. That variant
  is rejected in §5.2.

**Capsule sweeps and overlaps** [measured, probe `--sweep`, 2026-10-16, not under the lock; the machine was busy].
Setup:
* the capsule is r 0.35 m, half height 0.55 m;
* 50,000 points in the grid's centre 64 m tile, each at least 3 m from the tile border;
* the cast starts 0.3 m above standing height and moves (±0.5, −0.6, ±0.5) m;
* the overlap is the capsule 5 cm into the ground;
* two rounds each against the whole field (16 / 4) and against that tile's terrain as a MeshShape (today's form);
* shape-level calls (`CollisionDispatch`), so no broadphase.

| level | field: cast / collide | tile MeshShape: cast / collide |
|---|---|---|
| MP_007 Caspian | 2.92–2.98 / 1.79–1.84 µs | 1.80–1.94 / 1.16–1.17 µs |
| MP_018 Kharg | 2.86–3.03 / 2.05–2.12 µs | 2.66 / 1.32 µs (round 2: 5.3 / 7.2, noise) |
| XP3_Alborz | 2.77–2.90 / 2.02–2.08 µs | 2.04–2.11 / 1.28–1.49 µs |
| MP_Subway Metro | 2.55 / 1.78 µs (round 1: 14.4 / 22.6, cold) | 1.89–1.98 / 1.30 µs |

Both forms give the same hits (for example 49,890 / 50,000 casts and 84,759 / 84,758 contacts on Caspian). The field
costs about **+1 µs per capsule query on the terrain** (1.4–1.6 ×). Jolt's field walker decodes a block's samples
for every query, while a MeshShape reads ready vertices.

What this costs at S1's measured load [computed]:
* Caspian makes 16,430 queries per second of sim time (s1.md §19.1 G3).
* Even if every query touched the terrain, that is +16 ms of CPU per second, about 0.27 ms per 60 Hz tick summed over
  the lanes.
* Caspian's p99 tick is 0.64 ms and the gate is 8 ms.

G4 re-measures it (`host.scale` G1 and G3 `us_sweep`), and §15 lists the risk.

Why MP_013's error is larger: Jolt first quantises every height to 16 bits over the field's whole range
(1,627 m / 65,534 = 24.8 mm per step), and then per block. The bound is half a step, 12.4 mm. On the other levels,
BF3's own samples already lie on a 1/65,535 grid of the tree's `world_size_y`, so the error stays under a millimetre.
`StaticTerrainInfo::max_error` records the measured value per terrain (§5.4).

### 4.4 Before numbers for G3 [measured 2026-10-16, under the quiet lock, this tree before TC]

`bf3dump shapes <L> <M> --cache=<fresh root>`, then the same twice more on that root. Fields: shapes total
(soup + cook / restore), peak working set / peak private bytes, and Jolt resident.

| level × mode | cold | warm 1 | warm 2 | resident | cache |
|---|---|---|---|---|---|
| MP_Subway TDM | 0.681 s (soup 0.380, cook 0.268); 293.1 / 308.2 MiB | 0.036 s; 160.6 / 157.3 MiB | 0.035 s; 158.6 / 156.0 MiB | 22.6 MiB | 22.6 MiB |
| XP3_Shield RL | 2.876 s (soup 1.170, cook 1.676); 549.9 / 676.6 MiB | 0.092 s; 211.2 / 208.6 MiB | 0.086 s; 211.4 / 208.9 MiB | 72.6 MiB | 72.7 MiB |
| XP3_Alborz SR | 2.513 s (soup 1.588, cook 0.896); 644.7 / 710.6 MiB | 0.054 s; 218.6 / 215.5 MiB | 0.050 s; 217.2 / 214.0 MiB | 78.3 MiB | 78.4 MiB |

Load and traverse take 0.17–0.19 s on top of each row. Install unchanged in every run.

**Expected after [inferred, from §4.3]:**
* Resident is the Havok share of today's MeshShapes plus 47.4 MiB of field:
  * Shield RL: about 24 + 47 ≈ 71 MiB;
  * Alborz SR: about 48 + 47 ≈ 95 MiB;
  * Metro: about 19 + 0.8 ≈ 20 MiB.
* Cold time: about −1 s from no longer making and cooking 2.7 M terrain triangles, and +0.6–1.1 s for the field
  cook, which runs in parallel with the tile cooks. Plus about 0.05 s for the grid digest.
* Warm time: +0.03–0.1 s to read and verify a 47 MiB entry.
* Cache: the terrain entry is per level and shared by its modes, so about +0.9 GB if every level is prepared.
* Cold peak private bytes: the soup loses its terrain triangles (Shield RL: −129 MB) and holds the record instead
  (84 MB). The field cook adds its transient buffers:
  * the settings' float copy, 67 MB;
  * Jolt's 16-bit quantised copy, 34 MB;
  * the range blocks and the active-edge pass, about 20 MB;
  * the finished 47 MiB shape.
  That is about +150 to +250 MB over today, about 0.85–0.95 GiB on Alborz SR.

### 4.5 G3 limits and why

`tools.tc_cost` checks these on every run it makes. Each limit is about 2 × the expected value, so the noise on a
shared machine (§4.3: 1.08–1.84 s for the same field cook) does not fail it. A limit is still low enough that a
real regression fails, for example a whole terrain as triangles (minutes, GBs) or a field cooked per mode.

| run | cold shapes total | warm shapes total | cold peak WS / private | warm peak WS / private | resident | cache |
|---|---|---|---|---|---|---|
| XP3_Shield RL | ≤ 6.0 s (before 2.88; expected ≤ 3.5) | ≤ 0.5 s (before 0.09; expected ≤ 0.25) | ≤ 1.5 GiB (before 0.54 / 0.66) | ≤ 0.5 GiB (before 0.21) | ≤ 150 MiB (before 72.6; expected ≈ 71) | ≤ 160 MiB |
| XP3_Alborz SR | ≤ 6.0 s (before 2.51) | ≤ 0.5 s (before 0.05) | ≤ 1.5 GiB (before 0.63 / 0.69) | ≤ 0.5 GiB (before 0.21) | ≤ 150 MiB (before 78.3; expected ≈ 95) | ≤ 160 MiB |
| MP_Subway TDM | ≤ 1.4 s (before 0.68) | ≤ 0.1 s (before 0.036) | ≤ 0.6 GiB (before 0.29 / 0.30) | ≤ 0.3 GiB (before 0.16) | ≤ 30 MiB (before 22.6) | ≤ 40 MiB |

Why these limits:
* **Memory headroom.** 1.5 GiB is 2.3 % of the machine's 64 GB. A server holds one static world per rotation level
  it pre-bakes (D17), so even the 8 biggest levels at once stay under 1 GiB resident.
* **Load time.** The largest level × mode builds its static world cold in at most 6 s, which is under E4's 15 s budget
  for a whole Metro load. Warm, it restores in at most 0.5 s.
* **Both limits stay inside the M1 / S1 budgets that already exist.** The `godot.m1_load` E4 / E5 rows still gate
  Metro end to end.

The first after-run's numbers replace "expected" here and in the README. A limit is not raised without a stated cause.

---

## 5. Representation

### 5.1 The soup: a terrain record (decided)

`SoupTerrain` gains `std::shared_ptr<const res::HeightGrid> grid` and `res::Transform frame`, both set for a Built
terrain. These are in the header already, null and identity for now. After TC:

* **The tiles hold Havok triangles only.**
  * `CollisionSoup::triangles`, `tiles` and `sum` count only Havok triangles, which keeps m2a/weapons' identity.
  * `terrain_triangles` = 2 × kept cells. That equals the listing's count whenever no terrain triangle is
    degenerate or non-finite, which holds on retail (§4.1).
  * `SoupTerrain::cells` / `open` cover the whole grid.
  * `SoupTerrain::triangles` / `hash` are filled only by `hash_soup_terrain`. The soup build does not hash 2.6 GB
    of triangle bytes.
* **Cells on demand** (collision_soup.h, TC block):
  * `soup_terrain_cell(terrain, placement, x, z)`: one cell, the same two triangles, quantisation and drop rules as
    today;
  * `append_soup_terrain_tile(terrain, placement, tile, out, budget)`: the triangles whose centroid tile is `tile`,
    in emit order, visiting only the cells that can reach it;
  * `hash_soup_terrain(terrain, placement, threads)`: the T and K numbers for the whole grid (§6).
* **Bounds.** Up to `kMaxSoupTerrains` = 8 records per soup (retail: 1). `kMaxSoupTriangles` now bounds the Havok
  triangles alone.
* **Memory.** A 4097² record is 67 MB of floats plus 16.8 MB of hole bytes, shared by `shared_ptr` with whatever
  holds the soup. Today Shield RL's 2.7 M terrain triangles cost 129 MB.

### 5.2 The static world: one HeightFieldShape per terrain (decided)

Three representations were weighed:

| option | memory, 4097² | build | seams | verdict |
|---|---|---|---|---|
| soup triangles in tile MeshShapes | about 640 MB plus 1.6 GB soup | minutes | none | infeasible (§4.2) |
| one HeightFieldShape per 64 m tile | 65 MiB (16 / 8) | 0.1 s on 16 lanes | 16,384 field borders, quantised independently, plus 16,384 bodies | rejected: a ray or capsule exactly on a border meets two different quantisations; more bodies |
| **one HeightFieldShape per terrain plus a patch mesh** | **47.4 MiB (16 / 4)** | **0.5–1.1 s, one lane, parallel with the tiles** | **none** | **chosen** |

The choice costs about +1 µs per capsule query on the terrain against a terrain MeshShape (§4.3). At S1's load that
is under 0.3 ms per tick. The triangle form cannot be built at all, and per-tile fields have seams, so the field
wins on every criterion that can fail.

**Plan deviation (recorded).** PORTING-RESEARCH D5 ("static world as 64 m tiled shapes", §4 table), D17 ("64 m tiled
Jolt shapes") and §2.4 step 3 ("Jolt shape streams in 64 m tiles") assume that the terrain is tiled like the Havok
triangles. After TC the Havok triangles stay in 64 m tiles, but each terrain is one field.
* Why: the numbers above. Per-tile fields put 16,384 quantisation seams and bodies into the world and save nothing.
* What D5 and D17 wanted still holds:
  * the shapes are immutable and shared by the Server, Prediction and Cosmetic worlds;
  * the level converts on demand;
  * the field is a content-keyed cache entry that a level's modes share.
* Later terrain deformation (BF3's TerrainDestruction) stays possible on the one field.
  `HeightFieldShape::SetHeights` rewrites a sample rectangle in place and keeps the shape's identity, so the
  BodyIDs and S2's static-body order stay as they are.

Settings:
* `kStaticTerrainBits` = 16 and `kStaticTerrainBlock` = 4. 16 bits keeps the field within 1.1 mm of the grid on
  23 of 24 terrains (the nav proof 1 tolerance is 1 mm, §5.4); block 4 is 7 MiB larger than 8 but rays are 30 %
  faster;
* `mActiveEdgeCosThresholdAngle` = the tiles' `active_edge_degrees`;
* no materials.

Frame:
* the field sits at the frame's translation and rotation (body transform), with the uniform scale folded into the
  field's scale;
* a frame that is not a rotation times a positive uniform scale fails the build with an error naming the terrain
  (retail: all identity);
* the sample layout is X fastest, rows along +Z, the same as `res::HeightGrid`;
* Jolt fields are square. Every retail grid is square; a non-square grid is padded to a square with no-collision
  samples. Jolt pads the side up to a multiple of the block with no-collision samples itself (4097 becomes 4100),
  so no triangle appears past the grid's edge [verified: HeightFieldShape.cpp:703].

Jolt's diagonal runs from (x, y) to (x + 1, y + 1), the same as our a → d, so its two triangles per cell are ours.
[verified, `extern/JoltPhysics/.../HeightFieldShape.cpp`]:
* The triangle walker that rays, casts and collides use is `HeightFieldShape::DecodingContext::WalkHeightField`
  (declared at line 1758; the triangle loop is at lines 1966–2003).
  * It skips the cell when (x, y) or (x + 1, y + 1) has no collision (line 1975).
  * Triangle 0 is (x, y), (x, y + 1), (x + 1, y + 1). It is skipped when (x, y + 1) has none (line 1985).
  * Triangle 1 is (x, y), (x + 1, y + 1), (x + 1, y). It is skipped when (x + 1, y) has none (line 1996).
* `ProjectOntoSurface` (lines 898–928) and the debug `Draw` (lines 1619–1644) apply the same per-vertex rule.
  Revision 1 cited only `ProjectOntoSurface`.
* `sim.terrain.unit` proves the rule over all 2^9 hole patterns of a 3 × 3 block (§12.1).

**Holes, exactly.**
* Jolt drops a triangle when any of its three samples is `cNoCollisionValue`. The soup opens a cell when any of its
  four samples is a hole.
* They differ only for a cell whose single hole corner is b or c. Jolt then keeps the triangle without that corner.
  This happens in 4 cells on retail: MP_001 1, MP_003 2, XP4_Parl 1 [verified, §4.1].
* The cook makes the field exact:
  1. For each such cell, mark its sample a as no-collision in the field. Every triangle of that cell then contains a
     no-collision sample.
  2. Marking a sample only removes triangles, so the field's triangles are now a subset of the soup's cells. The
     only soup triangles it lacks are those of the (at most 4) cells around each marked sample.
  3. Those triangles go into one small patch `MeshShape` per terrain, in a second static body. A marked sample
     removes at most 4 triangles the soup keeps: 1 each from the cells where it is corner b or c, 2 from the cell
     where it is corner d. That is at most 8 on retail (MP_003).
* `StaticTerrainInfo::holed` and `patch_triangles` report the counts. `sim.terrain.unit` proves the equality on
  synthetic grids with every hole pattern of a 3 × 3 block (2^9). On the install, G1's exhaustive check proves it on
  every cell of all 24 terrains, and G1 prints the counts beside §4.1's census (1 / 2 / 1).
* **No collision at all.** A field with no collision sample left gets no field body, because Jolt's `GetLocalBounds`
  would give a point box (line 1577). This covers an all-hole grid, or one whose only kept cells all sit in the
  patch. An empty patch gets no patch body. The terrain keeps its `StaticTerrainInfo` entry, so the indices stay the
  soup's. `sim.terrain.unit` covers an all-hole grid and a grid whose only kept cells are patch cells.

### 5.3 Bodies and hits

* A terrain field body and its patch body get user data `terrain × 4 + kStaticTerrainBodyLayer` (3). Tile bodies keep
  `tile × 4 + layer` with layer < 3, so `describe_hit` separates them with the low two bits.
* Both bodies sit in the Collision object layer, so the motor, vehicles, bullets and sight all see them.
* Bodies are created after the tile bodies in terrain order, in CollisionWorld and VehicleWorld alike. This keeps
  S2's "statics identical to CollisionWorld" (s2.md §6.1).
* VehicleWorld's friction combine reads the tag, and the terrain's surface is dirt, the same as today's terrain cells.
* A terrain hit returns `tile = kNoStaticTile`, `terrain = index`, layer Collision and tag `static_terrain_tag()`
  (material 0, dirt, collision, `kSoupTerrain`).
* The new `StaticHit::terrain` field defaults to `kNoStaticTerrain`. No caller in the tree or the branches calls
  `shapes->tile(hit.tile)` on an install world (§3).
* `StaticWorldShapes::min()` / `max()` include the terrain bodies' bounds after TC. Today their users are tests on
  synthetic or Havok-only worlds (`sim_world_tests.cpp:128`, `sim_motor_tests.cpp:1794`,
  `sim_vehicle_tests.cpp:973`). Those worlds have no terrain, so their bounds do not change. G1's lattice uses each
  terrain's own `StaticTerrainInfo` bounds, and the nav region uses the combat tiles, so neither depends on these
  bounds.

### 5.4 Accuracy and the S1 nav proof 1

* The nav input keeps the grid floats (`append_soup_terrain_tile`), and the field differs from them by up to
  `max_error`.
* `nav_check_input_against_world` reads terrain triangles back from the field (`GetPosition` / `IsNoCollision` per
  sample over the box) and from the patch.
* It matches them within `max(p_tolerance, max_error + 0.1 mm)` and reports `terrain_matched` and
  `terrain_tolerance`.
* This is a **widened tolerance for terrain only**, because the field stores 16-bit samples:
  * Caspian 0.7 mm and Kharg 0.3 mm, so the S1 gates' effective tolerance stays 1 mm;
  * MP_013 needs 11.2 mm. MP_013 is in no S1 gate.
* Havok triangles keep 1 mm.

Rejected alternative: bake the nav from Jolt's dequantised heights. That would couple `nav_tile_inputs(soup)` to the
cooked shapes. The 1 mm principle holds on every gated level anyway.

---

## 6. Oracle, listing and parity

**Hash definition: unchanged.**
* T line: FNV-1a over every kept triangle's bytes in emit order.
* K line: the count and wrapping sum of all triangles (Havok and terrain) per tile.
* Totals: triangles include terrain.
* Only the cell set grows. The 230 runs stay comparable with one oracle change and no format change.

**Oracle** (`src/collision.rs`):
* `terrain()` loses its `near` parameter and the `!near.contains` test (lines 386, 414);
* `run()` loses the `near` set (572–575);
* the doc comments at 26 and 382 follow.

Cost [inferred]: 33.5 M triangles through FNV plus one BTreeMap entry each, about 5–8 s per big run, so about
+20–40 min over the 230 runs. `parity.ps1` prints the measured wall time per run and in total, and the README row
records it. If it is too slow, both tools switch to a per-row hash together (§16, question 2).

**IW4L commit.** The oracle path-depends on `C:/Users/olku/iw4L/crates/{level_bf3, ebx_bf3, resource_bf3,
bf3_scene}` (Cargo.toml). At this revision the checkout is `49aca70d5a8ccb1660c8178f98a8d2f120339461`, the `bf3`
branch merge of 2026-10-08, with a clean tree [verified: `git -C C:\Users\olku\iw4L rev-parse HEAD`,
`git status --short` empty].
* `parity.ps1` prints the commit, and a dirty flag, as its first line.
* The README collision row records the commit next to the 230 / 230 result.
* If the checkout moves before G2 runs, the new commit is the one recorded.

**Listing** (`tools/bf3dump/collision_commands.cpp`):
* `collision_listing` calls `hash_soup_terrain` per Built terrain. Its T line prints the record's numbers.
* It merges the terrain tile sums into the Havok tiles for the K lines (about 16,400 K lines on a 4097² level, today
  at most about 1,300).
* The totals and histograms add the terrain: surface dirt and body collision, 2 × kept cells each.
* The tile sums run in parallel per row; the running hash is one serial pass [inferred 2–4 s per 4097² terrain].

**Parity script** `research/probes/tools/level-oracle/parity.ps1` (new):
* runs both tools per level × mode into a scratch folder;
* compares byte for byte;
* prints `SAME` / `DIFF` per run and the totals;
* exits 1 on any DIFF.

It is G2's single command. Earlier parity runs used ad-hoc loops; this records the method.

**Numbers that change with the parity run:**
* the README collision table (`README.md:944–945`);
* `collision_check.cpp` `kOracle` (godot.collision "oracle" row: placements, triangles with terrain, terrain,
  destroyed, degenerate, tiles, sum);
* `tests/collision_tests.cpp` `Expected` (`core.level.collision`).

The row and the test compare `soup.triangles + hash.triangles` and the merged tiles and sum, because the soup alone
no longer holds terrain. `core.level.collision.unit`'s terrain cases (`terrain_tests.cpp`, synthetic trees) move
from "kept near Havok tiles" to "every non-hole cell". They add `soup_terrain_cell` /
`append_soup_terrain_tile` / `hash_soup_terrain` equality with the old whole-mask output on the same grid, and the
mismatch-cell patterns.

---

## 7. Nav water rule

The rule moves out of the soup, from cover tiles to terrain triangles. A terrain triangle is unwalkable
(`RC_NULL_AREA`) when:
* its centroid lies inside a water plane (`water_plane_covers`), and
* its highest corner lies more than `kCoverWaterDepth` (1.8 m) below the highest such plane (`nav_terrain_wet`).

Havok triangles are never marked, so piers, decks and boats stay walkable. The triangles stay in the input, so
proof 1 still matches the motor set. Only their area changes.

* **Input.** `nav_tile_inputs(soup, tiles, params, mask, water)`:
  * Havok triangles as today;
  * terrain triangles from `append_soup_terrain_tile` over the 3 × 3 tiles around each requested tile, clipped by
    the border box like the others;
  * `NavTileInput::unwalkable[i]` set by the rule.
  * The digest hashes `unwalkable` when non-empty. Changing the planes changes the tile keys.
* **Bake.** `nav_bake.cpp` marks those triangles `RC_NULL_AREA` before `rcRasterizeTriangles`. A tile whose input is
  all unwalkable terrain bakes to no tile, without running Recast, so Kharg's sea tiles cost what they cost under
  S1's dropped cover.
* **Key.** `NavBakeOptions::water` holds the planes (`read_water_planes`, as `LevelCombatTiles::water` gives them
  today). The nav-world key gets `;water=<SHA-256 of the planes>` when non-empty, replacing the world options'
  `cover=` / `water=`.
* **Region**: unchanged. `level_combat_tiles` plus 32 m. The nav stays bounded to the combat areas, while the motor
  now has ground everywhere.
* **Expected effect on G4 [inferred].** The input of each region tile gains the terrain of its border ring where
  S1's cover lacked it. Bake time should stay near S1's 2.8–3.9 s (Caspian) and 3.8–4.3 s (Kharg), well inside 6 s
  and 13 s.
* **Kharg: the meaning changes.** S1 dropped a whole cover tile when no terrain sample in it lay above
  water − 1.8 m: 1,290 of 1,614 cover tiles (s1.md §19.2). The per-triangle rule drops a tile only when all of its
  input is wet terrain. A coastal tile that mixes dry and wet terrain now runs Recast, with null-area triangles,
  where S1 dropped or kept it whole.
  * Expected: `tiles_wet` a little below 1,290, because coastal tiles with some dry triangles now bake. The Kharg bake
    therefore costs a little more than S1's 3.8–4.3 s.
  * `sim.nav.bake` prints `tiles_wet` and `triangles_unwalkable` (`NavStats`) beside S1's 1,290, so the move in
    bake time can be explained.
  * Caspian's lake (67.26 m) lies under all of its terrain (114.4 m and up), so Caspian marks nothing.
  * Metro's pond (62.56 m) can mark terrain below 60.76 m. Metro's terrain reaches down to 53.7 m.
* **Expected effect on S1 G2 (the `sim.nav` rows) [inferred].** S1 already ran its G2 with the combat-area tiles
  as cover, so inside the region the motor and the nav already had terrain.
  * TC only adds terrain: the region's border ring, and ground outside the combat tiles. It removes no Havok
    triangle and no terrain cell inside the region, and walkable area only connects components.
  * The spawn judge's `floor_at` casts from 1 m above the point to 2 m below it (`spawn.cpp:42`, `:107`), so only a
    point within that window of new ground can change.
  * So every row should stay the same or rise. The Caspian RL 24 stay off-mesh: the terrain is 29–32 m above them,
    outside the window. A row that falls is a defect, and G4 names its cause before it counts.

---

## 8. Godot collision view and the M1 game

### 8.1 Overlay

The soup's tiles no longer hold terrain. A whole terrain as overlay vertices would be 33.5 M × 3 vertices × 36 B =
3.6 GB. So layer 4 becomes a **window**:

* the terrain triangles of the 64 m tiles within `terrain_radius` (512 m, about 200 tiles, about 0.4 M triangles) of
  the camera, at most `max_terrain_triangles` (2 M);
* made by `prepare_collision_meshes(soup, CollisionPrepOptions)` on the loader thread;
* re-prepared by `prepare_terrain_window` when the camera leaves the window's inner half (layer-4 RIDs only).

**The mesh rows keep their meaning** over the slots the overlay holds:
* one mesh per (tile, layer) slot;
* 3 × the slot's triangles as vertices;
* first and last triangle bit for bit.

The rows compare Havok slots against the soup tiles as today, and terrain slots against `append_soup_terrain_tile`
for the window's tiles. `meshes.vertices` = 3 × (Havok triangles + window terrain triangles).

The other rows:
* `load` adds "every terrain has its record";
* `godot_free.recompute` compares the two loads' `hash_soup_terrain`;
* `oracle` uses §6's merged numbers;
* the status text prints the window.

### 8.2 M1 game and bots

The game loads `load_static_world_cached` through `load_shared_level`, so the motor, VehicleWorld and the client
prediction world all get the terrain bodies with no option. The S1 `--bots` path stops passing a cover; the bots'
nav gets the level's water planes.

Removing the cover removes the S1 risk that the client's world differs from the server's.

### 8.3 Capture (G7)

A windowed run of `m1_metro.tscn -- --level=MP_007 --mode=ConquestLarge0 --shots=...` and `metro_collision.tscn --
--level=MP_007 --mode=ConquestLarge0 --screenshot=...`, at a combat-area tile that had no soup triangle before TC
(`sim.terrain` prints those tiles). The own soldier must stand on the ground, and the overlay must show the terrain
layer there. The shots go to `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\tc\`, are never committed, and are
inspected by eye.

---

## 9. The 24 Caspian RushLarge0 spawns under the terrain (G6)

**Finding: the cause is in the data, not ours** [verified facts; one inference, tagged].

Sources:
* `bf3dump spawns MP_007 RushLarge0`;
* the `levels/mp_007/rush` and `.../layer0_rush_logic` EBX dumps;
* `bf3dump logic instantiate|trace MP_007 RushLarge0 --script rush` (S4 tooling);
* the probe's terrain heights (`TCPROBE_AT`);
* scratch `tc-design/{spawns_rl.tsv, dump/, logic_rl.txt, logic_trace_rl.txt}`.

1. Spawn points 0–23 are direct AlternateSpawnEntityData entities of WorldPartData
   `levels/mp_007/rush/layer0_rush_logic`, at y 104.13–104.44 m. All 24 WorldPartReferenceObjectData in
   `levels/mp_007/rush` have identity BlueprintTransforms, so no parent transform is missing in our placement.
2. The terrain there is at y 133.0–136.6 m, from the grid, with no hole. The points are 28.6–32.2 m under it.
3. Their spawners are CharacterSpawnReferenceObjectData 7a66f074 (Team1) and 5b305b02 (Team2), at y ≈ 109,
   `Enabled=false`. They are enabled by SyncedBools 2539/2540 ← 1468/1469 of the `full_rush_level` setup.
4. The S4 trace opens them only as **group 4**, at tick 14,480, after base 3 falls at tick 14,473. Groups 1–3 open at
   62, 3,668 and 9,074.
5. The defenders' LifeCounter (team2) has `BaseCount = 3` (interface value #1694831539 = 3). Metro's is 4.
6. **There is no underground room.** The RushLarge0 soup without terrain was searched with the probe's
   `--havok-near` (scratch `tc-design/rev2/havok_near{1,2,3}.txt`). It has **0 Havok triangles** with a corner in
   x −112..−58, z 343..370 (the points' footprint plus 6 m), at y 98–112 and over the whole column y 60–140. In the
   wider box x −160..−10, y 60–200, z 300–420 it has 5,210 triangles in 22 placements: the StaticModelGroup, linden
   and aspen trees, and azalea bushes. All of them lie at y ≥ 128.8 m, on the surface. So nothing exists at y 104
   for a soldier to stand on. The points are parked, not placed in a room our reader fails to build.
7. **[inferred, the same assumption as s4.md §9.1 / open question 12]** The round ends when BaseCount bases have
   fallen (RoundOver). Caspian's RushLarge0 therefore ends at base 3, group 4 belongs to a stage that is never
   played, and its spawns are parked underground.

**Consequences.**
* TC changes nothing here: the points were already under terrain under the near rule.
* The spawn and G2 judges (S1 G2 rows, the bot spawn filter) should judge only points of played stages. That is
  M2b's, with S4 open question 12.
* **Until M2b**, these 24 points are still counted:
  * S1 G2's Caspian RL row keeps them as failing (`o24`);
  * the bot spawn filter (s1.md §8) still has them in its input and refuses them only because `settle_spawn` finds
    no floor there. The `host.scale` gates run ConquestLarge0, so the gates never see these points.
  TC changes neither. G4 reports the row with this note.
* **Test** (`sim.terrain`, install case): asserts facts 1–6 through the readers:
  * the point count and y range;
  * the identity transforms;
  * the terrain height and no hole above each point (via the record);
  * `Enabled=false` on both spawners;
  * the group-4 opening tick after base 3's fall;
  * `BaseCount = 3`;
  * no Havok soup triangle in the footprint box from y 60 to 140.
* If a later reading shows stage 4 is played, the test fails, which is exactly when this explanation stops holding.

---

## 10. Caching: versions and keys

| entry / constant | before | after |
|---|---|---|
| `kCollisionSoupVersion` | 1 | **2** (terrain record, whole-grid rule, Havok-only tiles) |
| "jolt-world" key params | source digest, soup version 1, soup options, build params, `cover=` / `water=` when non-empty | source digest, soup version 2, soup options (terrain spacing), build params, terrain bits and block. No cover or water |
| "OBF3JWLD" | v1: tiles | **v2**: tiles, then per terrain (object, content digest); `kStaticWorldFormatVersion` 2 |
| "jolt-tile" | content keyed (tile input SHA-256) | unchanged. Havok-only tiles hash differently where terrain was, so those are new entries; tiles without terrain are reused |
| **"jolt-terrain"** (new) | — | params = content digest (SHA-256 over width, height, origin, spacing, frame, every height's bits and every hole byte) + bits / block + build params; converter `kStaticWorldConverterVersion`; `jolt_version` = `jolt_build_id()`. Content keyed: a level's modes share one entry |
| "OBF3JTER" v1 (new) | — | `StaticTerrainInfo`, the HeightFieldShape's `SaveBinaryState` stream, the patch MeshShape's stream (or a zero length) |
| nav tile key | input digest (triangles and layers) | the same, plus `unwalkable` when non-empty |
| nav world key | world options' cover / water; `region=` via `terrain_cover_digest` | `NavBakeOptions::water` digest (`water=`). With `links=jolt`, also `terrain=<bits>/<block>`, because the drop links are swept against the fields. `region=` keeps its digest of the sorted tile list: `terrain_cover_digest` is renamed `tile_list_digest` in `static_world_internal.h`, with the same bytes, so region keys are unchanged |

* **Warm path**: source digest → "jolt-world" v2 → every "jolt-tile" and every "jolt-terrain" → restore. No soup,
  no grid read.
* **Cold path**: the soup (with its records) → tiles and terrain fields cooked on the lanes (the field on one lane,
  in parallel with the tiles) → put the tiles, the terrains, then the world.
* Old entries are never read: version 1 keys differ. The cache's own eviction handles them.

---

## 11. Hostile input bounds

| input | bound | on violation |
|---|---|---|
| terrain objects per soup | `kMaxSoupTerrains` = 8 | soup error |
| grid side | `res::kMaxGridSide` = 4097 (`height_grid`'s own bound) | the terrain is Missing (as today) |
| cell visits | every cell that `append_soup_terrain_tile` or the static-world cook visits is charged to a `WorkBudget`: the soup budget, or the cook's own budget of `kMaxGridSide`² × 2 units per terrain. `hash_soup_terrain(…, WorkBudget &r_budget, threads)` visits each cell exactly twice (tile sums, serial hash), so it charges 2 × (w − 1) × (h − 1) units before it starts. `WorkBudget` is thread-safe, and the caller sizes it (2^25 for 4097²) | error, no partial shape or hash |
| cell index | `soup_terrain_cell` on a null grid or outside 0..w − 2 × 0..h − 2 | an empty cell with `open = true`; the grid is never indexed |
| no collision | a field with no collision sample, or an empty patch | no body for it; the `StaticTerrainInfo` entry stays (§5.2) |
| non-finite heights | a non-finite sample becomes a hole for the field and gives a non-finite count for the soup cells (quantise rule) | counted, as today |
| frame | must be a rotation × positive uniform scale (determinant > 0, orthogonal within 1e-4) | terrain cook error |
| height range | finite, span ≤ 1e6 m | cook error |
| patch | ≤ 4 triangles per holed sample; `holed` ≤ the grid's cells | cook error |
| "OBF3JTER" decode | We parse the whole HeightFieldShape header ourselves before `RestoreBinaryState`, because Jolt validates none of it. The checks:<br>• the Shape base: sub type HeightField, user data 0;<br>• `mOffset` and `mScale` finite, with scale x / z > 0 and y ≠ 0;<br>• `mSampleCount` from 2 to 4097 rounded up to the block, and a multiple of `mBlockSize`;<br>• `mBlockSize` 2..8 and `mBitsPerSample` 1..16;<br>• `num_blocks = mSampleCount / mBlockSize` ≤ 1 << `cNumBitsXY` (Jolt's constructor check, HeightFieldShape.cpp:477);<br>• sub-shape-ID bits `2 × bitwidth(mSampleCount − 1) + 1` ≤ `SubShapeID::MaxBits` (:484, :1451);<br>• `mMinSample` ≤ `mMaxSample` ≤ 0xFFFE, or `mMinSample` = 0xFFFF (no collision anywhere);<br>• `mMaterialIndices` empty and `mNumBitsPerMaterialIndex` = 0. The cook sets no materials; Jolt then stores none (:741), and `RestoreBinaryState` reserves `1 << bits` entries (:2701);<br>• the has-heights flag set, and the bytes that follow equal `mRangeBlocksSize`, `mHeightSamplesSize` and `mActiveEdgesSize` from `AllocateBuffers`'s formulas.<br>The restored field must match the blob's `StaticTerrainInfo` (sizes, bounds). The patch is walked like a tile MeshShape, and trailing bytes are refused | decode error → treated as a miss and recooked |
| nav water planes | `read_water_planes`' own bounds (polygons over `kMaxCombatAreaPoints` = 4096 points refused, the World's budget charged); the rule's work is planes × points per triangle, charged | error |
| overlay window | `max_terrain_triangles` | the window shrinks (reported) |

Fuzz: `sim.terrain.unit` mutates "OBF3JTER" blobs (every header field, truncations, sizes ± 1) and expects errors,
never a crash. The asan run covers it (G7).

---

## 12. Test plan

### 12.1 New

* **`sim.terrain.unit`** (no data):
  * cell rule against Jolt on all 2^9 hole patterns of a 3 × 3 block, plus random grids. The field plus patch
    triangles equal `soup_terrain_cell` output one to one, and rays agree;
  * frames: identity, rotated, scaled, refused (mirror, shear);
  * `append_soup_terrain_tile` against a whole-grid pass on aligned and unaligned grids;
  * `hash_soup_terrain` with 1 and N threads, equal;
  * blob round trip and hostile blobs;
  * `nav_terrain_wet` cases;
  * `describe_hit` on terrain (`kNoStaticTile`, the tag);
  * an all-hole grid, and a grid whose only kept cells are patch cells: no field body, the info entry stays, rays
    miss or hit only the patch;
  * `soup_terrain_cell` on a null grid and out of range; `hash_soup_terrain` with a budget one unit short fails.
* **`sim.terrain`** (install, RUN_SERIAL, G1 and G6). The G1 procedure:
  1. **Every run.** For each of the 230 runs in `levelmodes` order: load, traverse, and build the soup (default
     options). Compute each Built terrain's content digest from its record: `static_terrain_view`, then
     `static_terrain_content`, the digest the "jolt-terrain" key and `StaticTerrainInfo::content` use.
     The 31 runs without terrain must have none. Every run of a level must give the same digest (24 digests, one per
     terrain level). The runs go 4 at a time on lanes. Each soup peaks at about 0.7 GB, so 4 lanes stay under 3 GB.
     Estimated 2–5 min [inferred: 0.2 s load plus 0.4–1.6 s soup per run].
  2. **Per digest, once.** `build_static_world_terrain({}, {view})` from the first run's record.
     * Exhaustive check: for every cell, the field's per-triangle no-collision state (`IsNoCollision` on the
       triangle's three samples) plus the patch's triangles equals `soup_terrain_cell`'s output one to one. Every
       non-hole field sample is within `max_error` of the grid. `holed` and `patch_triangles` are printed, and
       `holed` must equal §4.1's mismatch column.
     * Lattice: a 16 m lattice over the terrain's extent. Each lattice point gives two rays, one at +(1.0, 0.6) m
       into triangle (a, d, b) and one at +(0.6, 1.0) m into (a, c, d).
     * Mismatch cells: rays into both triangles of every mismatch cell and of its 8 neighbours.
     * A ray starts 0.25 m above the expected height (the cell's triangle plane from the grid floats) and runs
       0.5 m down.
     * About 1.5–3 s per 4097² terrain: the cook, about 0.2 s for the exhaustive pass, and 0.52 M rays at about
       1 µs each.
  3. **Per run.** Every combat-area tile centre of the run's own mode (`level_combat_tiles`), at both triangle
     offsets, cast against its digest's world.
  4. **Full worlds.** Caspian CQL, Kharg CQL and Metro TDM through `load_static_world_cached` on a scratch root,
     cold. The lattice and tile centres are cast again. A non-terrain hit is re-cast past and counted as occluded.
     Then warm on the same root: a world hit, the terrains hit, and identical hits on 10,000 rays.
  5. **G6** (§9), then install unchanged.
* **`tools.tc_cost`** (install, RUN_SERIAL, `tests/tc_cost.cmake`, the `tools.ebx_memory` / `tools.m1_prepare`
  pattern). For each G3 run, on its own fresh scratch root, it runs `bf3dump shapes` cold, warm and warm in child
  processes. It parses `shapes total`, `peak working set`, `peak private bytes`, `resident` and `cache` from the
  output, and checks §4.5's limits. It prints the before / after table, and `install.unchanged` must be yes in every
  run. Exit 77 without an install; report-only under asan.
* **`sim.vehicle.unit` G1t** (new case in `sim_vehicle_tests.cpp`, no data). The world is
  `build_static_world_terrain` with:
  * a 65 × 65 field at 2 m with a 4° slope and bumps;
  * one single-corner hole, so the world has a patch;
  * one Havok MeshShape tile (a ramp) beside it.
  Three Humvees drive courses that cross the field, the patch triangles next to the hole and the field-to-tile
  border, touching each other as in G1m. Gates: G1 (two runs bit-identical) and G1r (restore and replay), as G1m.
  This gates multi-vehicle determinism over HeightFieldShape and patch bodies, which the synthetic courses, all
  MeshShape, never touch.
* **`tc.contract`** (now): the header additions compile together; constants and defaults (§13).

### 12.2 Changed

* `core.level.collision.unit` / `core.level.collision`: the new rule and numbers (§6).
* `godot.collision`: §8.1.
* `sim.world`: the brute-force reference adds the terrain triangles of each sampled box (`append_soup_terrain_tile`,
  tile = `kNoStaticTile`). Its ray tolerance is 2 mm and Metro's field error is 0.2 mm [verified §4.3]. The Metro
  cold / warm case adds the terrain entry.
* `sim.nav.unit` / `sim.nav` / `sim.nav.bake`: water through `NavBakeOptions`; proof 1 with the terrain tolerance
  (§5.4).
* `host.scale.unit` / `host.scale`, `s1.contract`: the cover lines go.
* `sim.vehicle` and `s2.contract`: the course finder's "keep legs on terrain tiles" check becomes always true. Courses
  may move, and G5 re-runs them (`sim.vehicle` and `sim.vehicle.unit` together).
* `sim.nav.bake`: prints `NavStats::tiles_wet` and `triangles_unwalkable` beside S1's 1,290 (G4, §7).

### 12.3 Unchanged and must pass

The rest of the full ctest (82 tests at this revision).

### 12.4 Timing runs

`quiet.lock` protocol:
1. Check for the lock; wait if another track holds it.
2. Write "terrain/collision <time>" into it.
3. Wait about 2 minutes.
4. Run only the timing commands.
5. Delete the lock (held ≤ 45 min).

G3 (`tools.tc_cost`, `godot.m1_load`), G4 (`host.scale`, `sim.nav.bake`) and G5 timings are taken this way. A
failing timing is re-run once under the lock, and both results are reported (§2).

---

## 13. Interfaces in the tree (this revision)

Every addition is declared, documented and additive. Nothing calls a declared-only function yet, following
m1.md §15.1's rule: a function is either implemented or only declared, and declared-only functions link until a
caller exists. So no stub `.cpp` is needed. Behaviour is unchanged (`kCollisionSoupVersion` stays 1 until the
implementation; `tc.contract` checks this).

| header | additions |
|---|---|
| `bf3/core/level/collision_soup.h` | forward `res::HeightGrid`; `SoupTerrain::grid`, `SoupTerrain::frame`; `kMaxSoupTerrains`; `SoupTerrainCell` + `soup_terrain_cell` (rev 2: null grid / out of range defined); `append_soup_terrain_tile`; `SoupTerrainTileSum`, `SoupTerrainHash` + `hash_soup_terrain` (rev 2: takes `WorkBudget &`) |
| `bf3/sim/static_world.h` | `kStaticTerrainBits`, `kStaticTerrainBlock`, `kStaticTerrainBodyLayer`, `kMaxStaticTerrains`, `kStaticTerrainCacheKind`, `static_terrain_tag()`; `StaticTerrainView`, `StaticTerrainInfo`; rev 2: `static_terrain_view`, `static_terrain_content`; `StaticWorldShapes::terrain_count` / `terrain`; bounds include the terrain; `StaticWorldStats` terrain fields; `build_static_world_terrain`; `encode_static_terrain_blob`, `decode_static_terrain_blob` (rev 2: the full header check list), `static_terrain_key_inputs`; the empty-field rule and the D5 / D17 deviation note |
| `bf3/sim/collision_world.h` | `kNoStaticTile`, `kNoStaticTerrain`, `StaticHit::terrain` |
| `bf3/sim/nav.h` | `NavTileInput::unwalkable`; `nav_terrain_wet`; the `nav_tile_inputs` overload with water planes; `NavBakeOptions::water` (rev 2: the `terrain=` / `region=` key note); `NavInputCheck::terrain_matched`, `terrain_tolerance`; rev 2: `NavStats::tiles_wet`, `triangles_unwalkable` |
| `bf3/render/collision_prep.h` | `CollisionPrepOptions`; the `prepare_collision_meshes` overload; `prepare_terrain_window` |
| `tests/tc_contract_tests.cpp` + `tests/CMakeLists.txt` TC block | ctest `tc.contract` |

Removed by the implementation, not now: `SoupOptions::terrain_cover` / `cover_water`,
`CollisionSoup::cover_tiles_wet`, `StaticWorldLoadOptions::terrain_cover` / `cover_water`,
`SharedLoadOptions::terrain_cover` / `cover_water`, `SharedLevel::terrain_cover`, and the `scale_bench.h` cover
mention. None is used on a parallel branch (§3).

---

## 14. Files

### 14.1 Creates

* `src/bf3_core/src/level/soup_terrain.cpp`: the cell functions and the hash, split out of collision_soup.cpp.
* `src/bf3_sim/src/static_world_terrain.cpp`: field and patch cook, digest, blob, validator.
* `tests/terrain_collision_tests.cpp` (`sim.terrain.unit`, `sim.terrain`) and `tests/tc_contract_tests.cpp` (exists).
* `tests/tc_cost.cmake` (`tools.tc_cost`, G3).
* `research/probes/tools/level-oracle/parity.ps1`.
* `tools/bf3dump/terrain_commands.{h,cpp}`: `bf3dump terrain <LEVEL> <MODE> [--grid=16] [--all]`, G1's numbers by
  hand.
* This document.

### 14.2 Modifies

* **bf3_core**:
  * `collision_soup.{h,cpp}`: the record; remove the cover and `dry_cover`; version 2;
  * `water_plane.h`: comment only.
* **bf3_sim**:
  * `static_world.{h,cpp}`, `static_world_cache.cpp`, `static_world_internal.h`: terrain bodies, keys, world blob
    v2; remove the cover digests;
  * `collision_world.{h,cpp}`, `collision_world_internal.h`: `describe_hit`, terrain bodies;
  * `vehicle_world.cpp`: terrain bodies in the same order;
  * `nav.{h,cpp}`, `nav_bake.cpp`, `nav_cache.cpp`, `nav_validate.cpp`: terrain input, water rule, proof 1.
* **bf3_host**: `level_load.{h,cpp}`, `bot_fill.{h,cpp}`, `scale_bench.{h,cpp}`: remove the cover; water to the nav.
* **bf3_render**: `collision_prep.{h,cpp}`: the window.
* **bf3_godot**: `collision_overlay.{h,cpp}`, `collision_check.cpp`, `level_session.cpp` (`--bots` options only).
* **tools/bf3dump**:
  * `collision_commands.cpp`: the listing;
  * `nav_commands.cpp`: remove the cover;
  * `bf3dump.cpp`: usage and the `terrain` dispatch;
  * `CMakeLists.txt`: one line.
* **level-oracle** `src/collision.rs`: §6.
* **tests**:
  * `collision_tests.cpp`, `terrain_tests.cpp`;
  * `sim_world_install_tests.cpp`, `sim_nav_tests.cpp`, `sim_nav_install_tests.cpp`;
  * `sim_vehicle_tests.cpp`: the G1t case. Its world (`build_static_world_terrain`) and its three courses (start
    placements and command scripts for `run_vehicle_group`) are local to the test, so
    `build_synthetic_vehicle_ground` and the 7 synthetic courses stay as they are;
  * `s1_contract_tests.cpp`, `host_scale_tests.cpp`;
  * `CMakeLists.txt` (TC block).
* **docs**: `README.md` (collision table, the terrain sentence, the overlay rows); `docs/design/s1.md` §5.2 and
  `s2.md` §4.4 (one note each pointing here).

### 14.3 Merge notes

* The branches that merge after this one touch none of the soup, static-world or overlay files except as listed in
  §3 [verified]. m2a/weapons' `CollisionWorld` rays and sweeps keep their signatures. `StaticHit` gains one trailing
  field, and terrain hits carry `kNoStaticTile`.
* Shared files (`tests/CMakeLists.txt`, `tools/bf3dump/CMakeLists.txt`, `bf3dump.cpp`, `README.md`) get blocks headed
  `# TC` or `<!-- TC -->`, with no reformatting.

---

## 15. Risks

| risk | effect | mitigation |
|---|---|---|
| Oracle and listing time grows (33.5 M triangles per big run) | the G2 parity run takes longer (estimate +20–40 min over 230 runs) | the hash definition is kept for parity. If too slow, a per-row hash (parallel) in both tools is a follow-up, and the parity is rerun |
| Jolt heightfield and capsule contacts behave differently from MeshShape terrain (active edges, ghost bumps at cell edges) | motor or vehicle feel; S2 G2x moves | same active-edge angle as tiles; G5 re-run including G1t; `sim.motor` unchanged tests |
| Capsule queries on the field cost about +1 µs each (§4.3, 1.4–1.6 × a terrain MeshShape) | tick time | at S1's 16,430 queries per second this is ≤ 0.27 ms per tick summed over the lanes; G4 re-measures `host.scale` p99 and `us_sweep`. If it ever matters, the fallback is per-tile terrain MeshShapes generated around the soldiers, not a format change |
| S2 courses move (the finder no longer avoids terrain holes) | G2x numbers change | G5 reports per course; ExactPod < 2 cm is the gate |
| MP_013's 11 mm field error | nav proof 1 needs the terrain tolerance | stated and reported (§5.4); no S1 gate uses MP_013 |
| Memory: field 47 MiB plus record 84 MB during the cold path; +0.9 GB of cache over all levels | peak working set on the cold path | G3 reports. The record is freed with the soup after the cook; warm paths read no record |
| Overlay window streaming | stutter while re-preparing | loader thread; only layer-4 RIDs swap |
| A level with a non-identity terrain frame (none on retail) | cook error | refused loudly. The test covers rotated and scaled frames |
| Nav proof 1 reads the field by `GetPosition` | needs bf3_sim internal access | as today for MeshShapes (`nav_validate.cpp` is internal) |
| Whole terrain in the static world makes far-away queries hit terrain | bot sight rays and bullets now hit hills they passed through before | intended (BF3 behaviour) |

---

## 16. Open questions

1. G6 rests on BaseCount meaning "bases until RoundOver" [inferred, as s4.md]. Confirming it is M2b's.
2. The oracle's FNV chain is serial. If G2 becomes too slow, should both tools switch to a per-row hash?
   (The plan is to keep it for this track.)
3. The overlay window radius (512 m) is a guess for a usable debug view. It can be tuned after the capture.
4. The nav proof-1 tolerance is widened for terrain only (MP_013 needs 11.2 mm, §5.4). The alternative, baking the
   nav from Jolt's dequantised heights, was rejected because the gated levels stay at 1 mm.
5. S2's MP_007 courses may move once the terrain is whole, because the finder's terrain-tile constraint becomes
   always true. The G2x numbers will move, and G5 gates them again.
