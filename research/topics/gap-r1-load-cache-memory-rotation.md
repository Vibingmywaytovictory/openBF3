# Gap r1: level load, cache footprint, RAM/VRAM budget, map rotation and loading protocol

Gap slug `load-cache-memory-rotation` (critic-r1.json, severity medium). Written 2026-10-14.

**Evidence tags.** [verified] = measured here or read in a primary source, with the path or URL given. [report claim] =
taken from another report or a secondary web source without re-checking. [inferred] = my own estimate or design.

**Machine.** AMD Ryzen 7 5800X (8 cores / 16 threads), 63.9 GiB RAM, RX 9070 XT. The BF3 install is on C:, a Kingston
SFYRD2000G NVMe [verified, `Get-CimInstance` / `Get-PhysicalDisk`]. All timings below were taken with a warm or
possibly-warm OS file cache. I could not flush the cache without admin tools; see §10.

**Tools and data.** Everything lives in scratch:
`C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\loading\`
(`SCR\` below).

| tool | path | what it does |
|---|---|---|
| `loadprobe` | `SCR\ws\src\main.rs`, built as `SCR\target\release\loadprobe.exe` | Rust; links copies of the iw4L crates `superbundle_bf3`, `ebx_bf3`, `bf3_install`, `resource_bf3`, `level_bf3`, `bf3_scene`. `census`: mount, load_level, traverse, then a per-resource census (DxTexture, MeshSet LODs, Havok, Enlighten, terrain streaming chunks, Ant banks, SoundDataAsset chunks), each keyed by CAS SHA1, plus an optional real GSTF write pass. `scene`: the full level_bf3 build (geometry, collision, lighting, vehicles), a Godot-layout mesh blob encode, and a collision export |
| `navbench` | `SCR\nav\navbench.cpp`, `SCR\nav\out\navbench.exe` | C++ with Recast/Detour built from a copy of `C:\Users\olku\recastnavigation`; tiled multithreaded bake plus Detour queries. Today I added `NAVBENCH_RADIUS`/`NAVBENCH_SLOPE` knobs |
| Jolt bench | `SCR\jolt\bench.gd` | measurement-only GDScript run in scratch, never part of the shipped project. Times Godot 4.8 dev7's Jolt cooking a concave shape. `run2.ps1` samples the private bytes of the real `Godot_v4.8-dev7_win64` child process |
| aggregators | `SCR\py\agg.py`, `agg29.py`, `cum.py` | outputs in `SCR\py\agg5.txt`, `SCR\py\agg29.txt` |

The install was opened with `File::open` only. Nothing was written outside scratch.

---

## 1. Answers on one page

1. **Per-level conversion on demand is fast enough. No mandatory "prepare install" pass is needed.**
   - Converting a level cold on this machine takes **~10–11 s** for the client and **~17–27 s** for the host or
     server, which adds the Jolt cook and the nav bake [verified per stage, §2.5].
   - Both are well under 07 §6.3's ≤ 60 s target. Both also fit inside a 45 s end-of-round intermission when run on
     4–8 background threads [inferred].
   - Keep an **optional** "Prepare levels" pass for slow machines and for dedicated servers.
2. **Keep resident textures (route B) as the default, and do not cache textures.**
   - A level's whole texture set decodes from the install in **2.1–2.5 s** on 16 threads [verified, 6 runs].
   - The GSTF route copies **7.5 GiB** of BC bytes for all 29 levels (union, SHA1-deduplicated), plus 0.4 GiB for the
     global superbundles [verified census]. It buys only streaming, not load speed.
3. **The cache is small without textures.**
   - All 29 levels need **≈ 2–4 GiB**: Godot mesh blobs 1.42 GiB (union), Jolt shapes, Detour tiles and scene blobs
     [verified/inferred, §3].
   - With GSTF the total is **≈ 10–12 GiB**. The install itself is 34.0 GiB.
4. **Correction to R10.** Caspian Conquest Large's mounted bundles hold **1,371 unique 2D textures = 1,658 MiB of BC
   chain** [verified]. The 1.56 GiB in R10 is close, but "4,545 images" does not match any unique count here (§10).
   - Only **415 MiB** of that is drawn by the static world (`scene_texture_bytes` 434,872,881 B).
   - The rest is all weapons (512 MiB), character kits (389 MiB) and vehicles (181 MiB) in the bundle.
   - Load those on first use, not at level load.
5. **Budgets** [inferred from measured parts, §4].
   - Client at Caspian Conquest Large with 64 players, resident route: **VRAM ≈ 1.5–2.5 GiB**, **RAM ≈ 1–1.5 GiB**
     steady state.
   - Dedicated server: **≈ 250–400 MiB** per instance (Godot headless baseline 90 MiB [verified]).
   - The cold-conversion transient in the Rust prototype peaks at **1.05–1.58 GiB** RSS, because it builds the whole
     scene in memory. The C++ port must stream mesh by mesh.
6. **Navigation is the slowest step.** Results with BF3's own capsule (r 0.35, climb 0.3, slope 50, cs 0.25, ch 0.1,
   64 m tiles, 16 threads) [verified]:
   - Caspian: **5.2 s**, 6.1 MiB (47.9 s on 1 thread).
   - Kharg: **11.8 s**, 20.0 MiB.
   - Bandar: **11.4 s**, 12.5 MiB.
   - Metro: 0.17 s; Nebandan: 1.7 s.

   Servers must pre-bake every level in the rotation at startup, next level first, and must never bake in the round
   gap.
7. **Protocol change.**
   - Add `RoundOver{…, next}` so clients can pre-convert during the intermission.
   - Add a `Loading{stage, progress, eta}` state with a stall timeout and a total deadline. The player's slot is held
     during it.
   - Run ENet service on the network thread so a busy main thread cannot trip ENet's 5–30 s peer timeout [verified
     values, enet.h].

   Details in §6.2.

---

## 2. Measurements

### 2.1 Mount, level database, traversal (ms, 16 threads)

Source: `SCR\out\<level>\log.txt` and `scene-log.txt` (2026-10-10), plus today's reruns in
`SCR\today\caspian_run1|2\log.txt` [verified].

| level / mode | open | mount | load_level (bundles + parse) | partitions | traverse (objects) | WS after load / peak MB |
|---|---|---|---|---|---|---|
| Metro (MP_Subway) TDM | 43 | 37 | 146–157 | 12,315 | 45 (13,280) | 27 / 150 |
| Caspian (MP_007) CQL | 44 | 27–48 | 115–272 | 12,005 | 48–58 (9,293) | 26 / 155–164 |
| Kharg (MP_018) CQL | 42 | 33 | 137–156 | 11,968 | 76 (7,679) | 26 / 160 |
| Bandar (XP3_Desert) CQL | 46 | 41 | 167–308 | 12,613 | 51–55 (11,534) | 29 / 166 |
| Nebandan (XP5_002) CQL | 58 | 46 | 147–317 | 12,049 | 26–30 (7,461) | 28 / 155 |

**Mount + database + traverse for one mode ≈ 0.2–0.45 s.** This confirms the handoff inputs that 07 §6.3 used.
The first run of the day was slower in parse (232 vs 79 ms), which may be cache warmth [verified].

### 2.2 Content of one level+mode (unique by CAS SHA1, within the mounted bundles)

Source: `SCR\py\agg5.txt` [verified census].

Column notes:
- **Mesh "Godot est."** = vertices × (40 B, +16 B if skinned) + indices × 2 or 4 B. This is the layout the probe's blob
  writer uses (pos f32×3, oct normal, oct tangent, colour, uv, uv2).
- **Audio** counts only SoundDataAsset chunks whose bytes resolve inside the mounted bundles. "Unresolved" counts chunk
  ids referenced but not found there.

| | Metro TDM | Caspian CQL | Kharg CQL | Bandar CQL | Nebandan CQL |
|---|---|---|---|---|---|
| 2D textures → GSTF MiB (count) | 1,596 (1,605) | **1,658 (1,371)** | 1,698 (1,388) | 1,796 (1,458) | 1,571 (1,336) |
| of which weapons / characters / vehicles MiB | 512 / 311 / 2 | 512 / 389 / 181 | 513 / 388 / 214 | 512 / 388 / 294 | 512 / 389 / 177 |
| cube/array/volume textures MiB | 17 | 9 | 16 | 10 | 9 |
| static-world textures actually drawn (`scene`) MiB | 584 (758 tex) | **415 (396)** | 388 (361) | 390 (379) | 306 (334) |
| mesh LODs (BF3 VB+IB MiB → Godot est. MiB) | 3,240 (142 → 202) | 2,523 (219 → 312) | 2,437 (228 → 316) | 2,575 (235 → 336) | 2,413 (198 → 284) |
| static-world mesh blob written (`scene`) MB | 85.2 | 66.0 | 74.4 | 104.3 | 51.0 |
| Havok res (count, MiB) | 706, 15 | 435, 18 | 404, 19 | 413, 16 | 396, 13 |
| collision soup tris (`scene`) | 474,668 | 2,536,980 | 2,636,455 | 2,053,835 | 1,057,452 |
| terrain chunks MiB (dedicated-server-persistent part) | 6 (5) | 20 (11) | 30 (21) | 48 (35) | 51 (12) |
| Enlighten atlases (`scene`, RGBA16×2 + RGBA8 + R8) MB / probes | 33.0 / 3,353 | 11.0 / 1,952 | 22.0 / 3,361 | 11.0 / 2,823 | 5.5 / 2,028 |
| Ant banks MiB (count) | 37 (5) | 38 (20) | 37 (16) | 37 (17) | 38 (14) |
| audio XAS resolved MiB (PCM16 equiv.) | 266 (894) | 220 (743) | 240 (808) | 309 (1,041) | 252 (850) |
| audio unresolved refs MiB | 2,506 | 1,959 | 2,506 | 1,959 | 1,959 |
| EBX partitions MiB | 51 | 53 | 54 | 57 | 54 |

- Audio PCM16 equivalent = XAS × 256/76. That ratio is my reading of EA-XAS v1 (76 B per 128 samples per channel)
  [inferred].
- Kharg check: the combat area measures 2.6 × 3.1 km, but the collision soup spans 8.8 × 8.8 km (`SCR\nav\kharg_cql_*.txt`)
  [verified].

### 2.3 Sharing across levels (by content SHA1)

Sources: `SCR\py\agg29.txt`, `SCR\py\cum.py`. Census of all 29 levels (`SCR\all\<level>\census.tsv`) with **all** mode
bundles of each level mounted [verified].

| asset | sum of per-level sizes | **union, 29 levels** | in every level | in ≥ 2 levels | in only one level |
|---|---|---|---|---|---|
| GSTF (2D BC, chain padded to 1×1) | 46.11 GiB | **7.50 GiB** (6,727 keys; + 401 MiB from global SBs) | 692 MiB | 4.04 GiB | 3.45 GiB |
| Godot mesh blob est. | 8.19 GiB | **1.42 GiB** (15,771 LOD keys) | 104 MiB | 0.84 GiB | 0.57 GiB |
| Havok res | 0.43 GiB | 0.26 GiB | 0 | 0.04 | 0.21 |
| terrain chunks | 0.65 GiB | 0.65 GiB | 0 | 0 | 0.65 |
| audio XAS (resolved) | 6.94 GiB | 2.23 GiB | 105 MiB | 0.47 | 1.76 |
| Ant banks | 1.06 GiB | 0.11 GiB | 15 MiB | 0.10 | 0.01 |

**Per level** (all modes):
- GSTF: 1,345–1,887 MiB, mean 1,628.
- Mesh: 170–381 MiB, mean 289.
- Bytes unique to one level: GSTF 57–289 MiB, mesh 3–59 MiB.
- The largest level-only GSTF sets are Metro 289, Factory 277, Skybar 204 and Palace 201 MiB.

**Cumulative GSTF cache over an alphabetical rotation** (levels → GiB):
1 → 1.55, 2 → 1.88, 5 → 2.95, 9 → 3.82, 13 → 4.52, 17 → 5.78, 21 → 6.40, 25 → 7.09, 29 → 7.50.

**Cumulative mesh cache:** 1 → 0.24 GiB, 29 → 1.42 GiB.

**GSTF union by category (MiB):** level 2,114, world-shared 1,765, xp-shared 1,021, characters 622, vehicles 544,
weapons 519, level-terrain 376, other 372, level-lighting 329.

### 2.4 Texture conversion: real GSTF files written (16 threads)

Source: `SCR\out\*\log.txt` and `SCR\today\caspian_run*` [verified].

| level | 2D textures | decode only (resident-route input) | 1-thread sample | decode + GSTF write | GSTF bytes written |
|---|---|---|---|---|---|
| Metro TDM | 1,605 | 1,989 ms (1.67 GB) | 229 MB/s | 2,757 ms | 1.674 GB |
| Caspian CQL (10-10 / today ×2) | 1,371 | 2,204 / 2,176 / 2,134 ms (1.73 GB) | 225–243 MB/s | 2,787 / 2,633 / 2,671 ms | 1.738 GB |
| Kharg CQL | 1,388 | 2,210 ms | 230 MB/s | 2,902 ms | 1.780 GB |
| Bandar CQL | 1,458 | 2,450 ms | 227 MB/s | 3,142 ms | 1.883 GB |
| Nebandan CQL | 1,336 | 2,294 ms | 207 MB/s | 2,896 ms | 1.648 GB |

- Decode runs at ≈ 0.75–0.8 GB/s on 16 threads against ≈ 0.23 GB/s per thread. **Decode stops scaling beyond about
  4 threads** [inferred from the two numbers]. Four background threads should therefore take about the same ~2–3 s.
- GSTF padding to 1×1 affects 36–42 textures per level, and the bytes change by < 0.3 %.
- Converter peak RSS in this pass is **162–198 MB**, because each texture is streamed on its own [verified].

### 2.5 Scene build, mesh blobs, collision (level_bf3 prototype; mostly single-threaded Rust)

Source: `SCR\out\*\scene-log.txt` [verified]. All times in ms.

| level | geometry (mesh / tex) | blob encode + write | collision build + export | vehicles | lighting | **scene total** | RSS peak / commit |
|---|---|---|---|---|---|---|---|
| Metro TDM | 3,005 (1,540 / 895) | 121 + 50 | 310 + 100 | 0 | 102 | **4,022** | 1.15 / 1.25 GB |
| Caspian CQL | 3,765 (875 / 775) | 62 + 39 | 1,478 + 579 | 1,272 | 52 | **7,592** | 1.35 / 1.59 GB |
| Kharg CQL | 3,584 (823 / 562) | 82 + 119 | 1,151 + 616 | 1,232 | 99 | **7,281** | 1.42 / 1.73 GB |
| Bandar CQL | 4,169 (968 / 573) | 104 + 161 | 826 + 504 | 1,695 | 48 | **7,892** | 1.58 / 1.83 GB |
| Nebandan CQL | 3,820 (699 / 517) | 64 + 72 | 1,157 + 281 | 1,394 | 25 | **7,120** | 1.05 / 1.24 GB |

### 2.6 Jolt static-world cook (Godot 4.8 dev7's Jolt, Caspian soup, 2,536,853 tris)

Sources: `SCR\jolt\out2_caspian_{0,64}.txt` and `.mem.tsv` [verified]. Memory is the private bytes of the
`Godot_v4.8-dev7_win64` process sampled every 100 ms.

| layout | cook time (1 thread) | resident shape memory (plateau after − before) | transient peak during cook |
|---|---|---|---|
| one MeshShape | 2,927 ms (4,481 ms on 10-10) | ≈ 41 MiB (≈ 17 B/tri) | ≈ +560 MiB (243 → 800+) |
| 928 shapes, 64 m tiles | **1,634 ms** | ≈ 31 MiB (≈ 13 B/tri) | ≈ +31 MiB |

- `OS.get_static_memory_usage` misses Jolt's allocations: it reported 0.16 MiB.
- On 10-10 the console-wrapper exe was sampled instead of the real Godot process, so those memory figures are invalid.
  The figures above come from today's rerun.
- Idle headless Godot (dev7 + Jolt) uses **90 MiB** private [verified, `SCR\jolt\out3_idle_0.txt`].
- **Use 64 m tiled shapes.** They cook 1.8× faster, avoid the 0.5 GiB spike, can cook in parallel on the job pool, and
  give a natural dirty unit for destruction [inferred].
- Shape-stream file size (`SaveWithChildren`) was not measured. I assume it is ≈ the resident size [inferred].

### 2.7 Detour bake from the level_bf3 collision soup

Sources: `SCR\nav\*_16t.txt` (placeholder agent r 0.5 / climb 0.5 / slope 45) and `SCR\nav\*_bf3capsule_*.txt`
(BF3 capsule r 0.35, h 1.8, climb 0.3, slope 50). All runs: cs 0.25, ch 0.1, 64 m tiles, triangles clipped to the
combat area + 32 m, water dropped [verified].

| level | tris kept | tiles (non-empty) | BF3 capsule, 16 thr | placeholder, 16 thr | navmesh MiB (BF3) | build heap peak | largest component | query mean / max |
|---|---|---|---|---|---|---|---|---|
| Metro TDM | 275,339 | 132 (33) | **0.17 s** | 0.20 s | 0.7 | 68 MiB | **23 %** (BF3) / 38 % | 0.03 / 0.07 ms |
| Caspian CQL | 2,484,333 | 928 (575) | **5.18 s** (4 thr 16.3 s placeholder; **1 thr 47.9 s**) | 6.07 s | 6.1 | 34 MiB (1 thr 9.7) | 70 % | 0.28 / 1.69 ms |
| Kharg CQL | 2,627,383 | 18,906 (2,235) | **11.84 s** | 15.41 s | **20.0** | 47 MiB | 83 % | 0.92 / 4.48 ms |
| Bandar CQL | 2,034,667 | 7,395 (1,596) | **11.39 s** | 14.41 s | 12.5 | 40 MiB | 83 % | 0.63 / 4.11 ms |
| Nebandan CQL | 1,000,858 | 437 (204) | **1.68 s** | 1.94 s | 1.9 | 30 MiB | 65 % | 0.08 / 0.38 ms |

- Rebaking the busiest tile takes 74–153 ms, and swapping it in takes 0–0.5 ms.
- **The level_bf3 soup still splits Metro.** With the BF3 capsule the largest component holds only 23 %. This
  reproduces 06 C11/R6 on a second, independently built soup: it is not an IW4L-soup artefact [verified that the split
  exists; cause still open].
- The 16-thread "sum of tile times" (82–245 s) is inflated by contention. Use the 1-thread run (47.9 s on Caspian) as
  the single-core cost.
- **Correction to 07 §6.2**, which said "bake 4–6 s per level" (IW4L): on big maps it is 5–12 s on 16 threads, about
  50–110 s on one core [verified Caspian; Kharg inferred by the tile-time ratio], and this is per hull class.

---

## 3. user:// footprint

**Per level** (Caspian CQL as the example; numbers from §2) [verified parts, inferred totals].

| item | resident route | GSTF route |
|---|---|---|
| textures | 0 (decode from the install at load) | 1.62 GiB, of which ~0.9–1.1 GiB is shared with other levels |
| mesh blobs, all bundle meshes | ≤ 312 MiB (static world alone 63 MiB) | same |
| Jolt shape streams | ≈ 31–41 MiB | same |
| Detour tiles | 6.1 MiB × (infantry + ~2 hull classes [inferred]) ≈ 15–20 MiB | same |
| scene/gameplay blobs, resolved defs | not measured; ~10–50 MiB [inferred] | same |
| **total, first level** | **≈ 0.4 GiB** | **≈ 2.0 GiB** |

**All 29 levels:**

| | resident route | GSTF route |
|---|---|---|
| total | **≈ 2–4 GiB** | **≈ 10–12 GiB** |
| how it adds up | mesh 1.42 GiB; shapes 13–17 B/tri × 0.5–2.6 M tris ≈ 0.4–1.2 GiB; nav ≈ 0.2–0.9 GiB; scene blobs | the resident total + 7.50 GiB textures + 0.40 GiB globals |

Do **not** cache decoded audio. PCM16 would be ≈ 743 MiB per level and ≈ 7.5 GiB for 29 levels [inferred ×3.37].
Godot 4.8 binds `AudioStreamPlayback._mix` and `AudioStreamPlaybackResampled._mix_resampled` as virtuals
[verified, `research\godot-api\extension_api.json`], so a GDExtension `AudioStreamXas` can decode XAS while it mixes.
Do not cache decoded Ant clips either: banks are 37–45 MiB per level and 0.11 GiB in total.

## 4. Memory budgets

### 4.1 Client, Caspian Conquest Large, 64 players, resident route, 1440p [inferred from the measured parts]

| item | VRAM | RAM | basis |
|---|---|---|---|
| static-world textures | 415 MiB | 0 (not kept on the CPU) | measured `scene_texture_bytes` |
| weapon / character / vehicle textures, on first use | ≤ 512 / 389 / 181 MiB, realistically 0.2–0.5 GiB | 0 | census by category |
| terrain heights (4097², R16) + terrain chunks + splat arrays | ≈ 50–150 MiB | ≈ 30 MiB | TerrainStats samples 4097²; chunks 20 MiB |
| static-world meshes | ≈ 63 MiB | ≈ 0 after upload | measured blob |
| soldier, vehicle and weapon meshes | ≤ 250 MiB | — | census Godot est. 312 − 63 |
| Enlighten atlases + probes | ≈ 11 MiB | small | measured |
| 64 skinned soldiers: per-instance compute-skin buffers | ≈ 40–60 MiB | bones 64 × 213 × 48 B ≈ 0.65 MiB | soldier LOD0 parts 5.7–13.3 k verts each (33 skinned meshes, median 5,719) |
| render targets, shadows, GI buffers | ≈ 0.4–0.8 GiB | — | not measured |
| Godot process baseline (windowed, RD) | — | ≈ 0.3–0.5 GiB | headless baseline 90 MiB measured |
| EBX gameplay data kept | — | ≈ 30–60 MiB | WS after load 26–29 MB |
| Ant banks (+ decoded tables optional) | — | 38 MiB (+ ≤ 100) | census |
| audio XAS kept compressed | — | ≈ 220 MiB (+ shared globals) | census |
| Jolt static shapes (shared by Prediction and Cosmetic worlds) | — | 31–41 MiB | measured proxy |
| **total** | **≈ 1.5–2.5 GiB** | **≈ 1.0–1.5 GiB** steady | |

- With GSTF streaming instead, the texture VRAM is bounded by `TextureStreaming` `memory_budget_mb` (default 512)
  (07 §1.3).
- During a cold conversion inside the client process, add the converter's transient: ≤ 0.2 GiB with per-item streaming
  (measured in the GSTF pass), versus 1.0–1.6 GiB for the prototype's whole-scene build.
- BF3's own figures, for scale: minimum 2 GB RAM / 512 MB VRAM, recommended 4 GB RAM / 1 GB VRAM, 20 GB disk [report
  claim, two secondary sources, §5]. BF3 fits this by streaming. We do not need to fit it on 12–16 GB GPUs.

### 4.2 Dedicated server, Caspian Conquest Large, 64 slots with bot fill [inferred from the measured parts]

| item | RAM | basis |
|---|---|---|
| Godot `--headless` baseline (dev7 + Jolt) | 90 MiB | measured |
| EBX gameplay tables, resolved defs, scene model | 30–80 MiB | load WS 26–29 MB; parse peak 150–200 MB transient |
| terrain heights, dedicated-server-persistent chunks | 11 MiB (Kharg 21, Bandar 35) | census `persistent_dedicated_server` |
| Jolt static world (tiled) | 31–41 MiB | measured proxy |
| Jolt dynamic: 64 soldiers, ~40 vehicles, destructible pieces | ≈ 5–20 MiB | not measured |
| Detour: infantry 6.1 MiB + 2–3 hull classes; one `dtNavMeshQuery` (65,535 nodes) per job thread × 16 | ≈ 20–30 + 30–50 MiB | measured mesh size; pool sizing inferred |
| lag-comp history: 64 × 32 ticks × ~64 B, + 40 vehicles | ≈ 0.2 MiB | 05 §5.4 sizing |
| ENet with 64 peers, snapshot baselines (64 × 63 ghosts × ack window) | ≈ 10–30 MiB | not measured |
| **steady total** | **≈ 250–400 MiB** | |
| cold-bake transient | + 35–50 MiB Recast heap; + 50 MB soup; + 31 MiB tiled Jolt cook (≈ 560 MiB if cooked as one shape) | measured |

A server that loaded like a client would also keep CPU copies of every texture and mesh in the dummy renderer
(+1.7–2 GiB, 07 K12). The server must not create render resources.

---

## 5. Web references

| fact | source | tag |
|---|---|---|
| BF3 PC minimum: 2 GB RAM, 512 MB VRAM, 20 GB disk. Recommended: 4 GB RAM, 1 GB VRAM (GTX 560 / HD 6950), quad core, Win7 64-bit | https://mp1st.com/news/battlefield-3-pc-minimum-and-recommended-system-specs-released (says these were published for the beta); https://www.systemrequirementslab.com/cyri/requirements/battlefield-3/11211 | [report claim, 2 secondary sources agree] |
| BF3 load on SSD vs HDD: under half the time; game launch saves > 30 s | https://principledtechnologies.com/dell/Alienware_X51_performance_0413.pdf; https://www.tomshardware.com/reviews/battlefield-rift-ssd,3062.html (BF3 load is almost entirely disk reads) | [report claim] |
| Players report "5–10 s map changes" on SSD + 6 GB RAM | https://www.techpowerup.com/forums/threads/battlefield-3-clubhouse.139681/page-378 | [report claim, anecdote] |
| DICE starts loading the next level on the end-of-round screen (BFV; J. Ajmal AMA, 2018-12-21) | https://mp1st.com/news/battlefield-5-quit-game-issue-addressed-by-dice-says-map-loading-is-main-cause | [report claim; BFV, not BF3] |
| Frostbite 2 streams in every frame, "from textures to objects to weapons to sounds and animations" (P. Bach). The GDC talk "Terrain in Battlefield 3" covers runtime terrain streaming | https://www.dsogaming.com/?p=17012; https://frostbite.com/frostbite/news/terrain-in-battlefield-3-a-modern-complete-and-scalable-system | [report claim] |
| BF3 RCON rotation model (protocol PDF R25): `mapList.add <map> <gamemode> <rounds>`, `setNextMapIndex`, `getMapIndices`, `getRounds`, `endRound <team>`, `runNextRound` ("without going through the end-of-round sequence"), `restartRound`; `vars.roundStartPlayerCount` (warm-up → pre-round), `roundRestartPlayerCount` (in-round → warm-up), `roundLockdownCountdown` (pre-round length); events `server.onLevelLoaded <level> <mode> <roundsPlayed> <roundsTotal>` ("Level has completed loading, and will start in a bit"), `server.onRoundOver`, `onRoundOverPlayers`, `onRoundOverTeamScores` | https://raw.githubusercontent.com/dcodeIO/BattleCon/master/eadocs/BF3/BF3%20PC%20Server%20Remote%20Administration%20Protocol.pdf; text in `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\measurement\dl-rconpdf\rcon.txt:357-367, 455-490, 1008-1022` | [verified text] |
| ENet 1.3.18: `ENET_PEER_TIMEOUT_LIMIT 32`, `TIMEOUT_MINIMUM 5000` ms, `TIMEOUT_MAXIMUM 30000` ms, `PING_INTERVAL 500` ms | https://raw.githubusercontent.com/lsalzman/enet/master/include/enet/enet.h | [verified] |
| `WorkerThreadPool.add_task(action, high_priority=false, description)`, `add_group_task(…, tasks_needed=-1, high_priority=false, …)` | `research\godot-api\extension_api.json` | [verified] |

No primary DICE source on superbundle and level-load internals turned up in this pass (§10).

---

## 6. Design

### 6.1 Rotation flow (BF3 semantics, our transport)

The rotation list follows BF3's model: a `MapList` of `{level, mode, rounds}` with a cursor and next index, admin
verbs as in RCON (§5), and `roundStartPlayerCount`/`roundRestartPlayerCount`/`roundLockdownCountdown`. Bots count
toward the player counts (`PreRoundEntityData` `RoundMinPlayerCount 4`, `RoundRestartCountdown 30`, topics\02-soldier-kits-modes.md:343) [inferred].

```
InRound ──tickets/time──► RoundOver (server keeps old level; scoreboard; intermission T_i, default 45 s)
   │                          │  S→C RoundOver{winner, scores, next{level, mode, source_digest, sim_digest, xpacks}, T_i}
   │                          │  client: check digests → Prepare(next) on background loader (≤ 4 threads, low priority)
   │                          │  C→S Prepare{next_source_digest, state: ready|converting(p)|missing_xpack|mismatch}
   │                          │  server: loads next level's sim data from its cache (≤ 2 s) on a loader thread
   ▼                          ▼
LevelChange (at T_i end) ─► Loading (per client) ─► Ready ─► PreRound (lockdown countdown) ─► InRound
```

- Clients whose `Prepare` reported `mismatch` or `missing_xpack` get a clear reason at RoundOver and are disconnected
  at LevelChange. They are not left waiting.
- `runNextRound` skips the intermission: clients that have not pre-converted show the loading screen. That is the
  only path where a cold client is seen.

### 6.2 Changes to §2.7 (handshake and level change)

```
S→C ch1  MatchInfo   {…existing…, phase: Loading|PreRound|InRound|RoundOver, next?: {level, mode, source_digest}}
C→S ch1  Loading     {round_id, stage: mount|db|textures|meshes|shapes|nav|upload, progress_permille, eta_s,
                      cache: warm|partial|cold}                   every 0.5–1 s while not Ready
C→S ch1  Ready       {…existing…}
S→C ch1  RoundOver   {winner, scores, next{level, mode, source_digest, sim_digest, required_xpacks}, intermission_s}
C→S ch1  Prepare     {next_source_digest, state, progress_permille}
S→C ch1  LevelChange {level, mode, sim_digest, round_id, load_deadline_s}
S→C ch1  Kick        {reason: LoadTimeout|LoadStalled|Mismatch|MissingXpack, detail}
```

**Rules** [inferred design]:
- ENet service, keepalive and `Loading` sends run on the `bf3_net` thread, never the Godot main thread. ENet drops a
  silent peer after 5–30 s (§5).
- **Slot hold.** A loading client keeps its slot, team and squad. Bot fill counts it as occupied, so no bot is spawned
  into a held slot.
- **Stall timeout.** Kick when no `Loading` progress increase arrives for 30 s.
- **Deadline.** `load_deadline_s` defaults to 120 s, the same as IW4L's `LOADING_DEADLINE_MS` (05 §7.6). The server
  raises it to 300 s for a client that reported `cache: cold` at join.
- **Joining mid-round** uses the same `Loading` state after `MatchInfo`. The server shows loading players on its
  scoreboard.
- The round's PreRound countdown starts when `roundStartPlayerCount` is reached, counting bots and ready humans.
  Loading humans do not block it.

### 6.3 "Prepare levels" pass (optional)

- **Entry points:** a client menu item ("Prepare levels: all / installed XPacks / this server's rotation") and a CLI,
  `bf3dump prepare [--levels …] [--server]`.
- **Work per level+mode:** mount → db → scene blobs, mesh blobs, Jolt shapes (and nav when `--server` or listen
  hosting), plus GSTF if that route is enabled.
- **Scheduling:** dedupe by key before scheduling, so shared weapon and character assets convert once.
- **UI:** progress shows `level i/n · stage · MiB done/total · ETA`.
- **Cost on this machine** [inferred from §2]:
  - Client, resident route: 29 × ~7.5 s scene build ≈ **3.5–4 min**.
  - Plus GSTF: ~7.5 GiB at ~0.6 GB/s ≈ +15 s.
  - Server: + nav 29 × 0.2–12 s × hull classes ≈ **+3–6 min** on 16 threads.
  - A 4-core HDD machine is several times slower. Install reads alone are ≈ 4 min at 150 MB/s [inferred].
- **Resumability:** every finished key is committed atomically (write to a temp file, then rename), so a cancelled
  pass loses nothing.

### 6.4 Cache budget and eviction

- **Layout:** `user://bf3cache/v<fmt>/<kind>/<k[0:2]>/<key>.<ext>`.
  - The key is the CAS SHA1 of the inputs + converter version + Godot commit (+ Jolt version for shapes; + Recast params
    for nav). This is 07 §6.2's scheme.
  - A per-level manifest `levels/<source_digest>-<mode>.lvl` lists the keys it needs, with sizes.
- **Index:** one append-only journal (`index.log`), compacted at startup. It holds key → size, last_used, and
  pinned flag. There is no database dependency.
- **Default budgets:**
  - Resident route: unlimited by default, because everything fits in ≈ 2–4 GiB.
  - GSTF route: 12 GiB, adjustable down to 3 GiB.
  - Free disk below 2 GiB → stop caching new GSTF and fall back to resident textures for that level.
- **Eviction:** LRU by level manifest first, then by key, among keys no pinned manifest references. These are always
  pinned:
  - the current level;
  - the `next` level;
  - on servers, every level in the `MapList`;
  - globals.

  Evict when the total exceeds the budget × 1.0, down to 0.9, at the round end and never mid-round.
- **Validation:** at startup, probe-load one GSTF and one shape stream (07 R7). On a version mismatch, the whole
  `v<fmt>` tree is dropped lazily.

### 6.5 Server pre-bake

- At startup, for each `MapList` entry (current first, then the next, then the rest): mount → collision soup → tiled
  Jolt shapes → nav per hull class → sim tables → manifest.
- The server accepts connections as soon as the **current** level is ready. A cold Caspian start takes ≈ 10 s on
  16 threads (collision 2.1 + Jolt 1.6 tiled, parallelisable + nav 5.2); Kharg ≈ 16 s.
- The rest bakes at low priority on the job pool. On this 16-thread machine the bake gets at most half the pool while
  a round runs, so the 60 Hz tick keeps its budget.
- If a `MapList` change adds an unbaked level, the server bakes it immediately. If the bake is not finished by
  RoundOver, the server extends the intermission (max +60 s) or skips to the next baked entry, and logs it.
- **Key nav by collision digest, not by mode.** Bake per level over the union of its modes' combat areas, so modes
  share tiles. Mode-specific subworlds that change collision produce a different digest and therefore a separate bake.

### 6.6 Load threading in bf3_godot

| stage | thread | notes |
|---|---|---|
| mount, db, traverse, scene model | loader `std::thread` (bf3_core, no Godot API) | 0.2–0.45 s |
| texture decode | WorkerThreadPool `add_group_task(…, tasks_needed=4–8, high_priority=false)` in the intermission; `high_priority=true` behind a loading screen | does not scale past ~4 threads (§2.4) |
| `texture_2d_create` / `Image::create_from_data` | worker | runs on the calling thread (§2.3); upload cost not measured |
| GSTF route: write `.stex`, then `StreamedTexture2D::load` | worker writes, main thread loads | raise `max_ops_per_second` during the loading screen (07 §4.5) |
| mesh blob read → `mesh_create_from_surfaces` | worker | ok off the main thread; `mesh_add_surface`, `material_create` and `shader_create` get queued to main (§2.3) |
| `instance_create2` / `set_base` / `set_transform` for ~20 k placements | main thread, batched ≤ 2 ms per frame | ~1 µs per instance (probe §6.1) ≈ 20–30 ms in total |
| Jolt shape restore / cook | bf3_sim loader or job pool (no Godot) | tiled shapes in parallel |
| Detour load / bake | job pool | bake only on host or server |

- In a listen server, cap Godot's pool (`threading/worker_pool/max_threads`) so it does not oversubscribe the server
  job pool (§2.6). Intermission pre-conversion uses low-priority tasks only.
- **The C++ converter must stream per mesh and per texture.** The Rust prototype's whole-scene build peaks at
  1.0–1.6 GB, while per-item streaming peaks at 0.16–0.2 GB.

### 6.7 Load-time targets, revised (replacing 07 §6.3's inferred targets)

| case | target | basis |
|---|---|---|
| client, cold, one level, 16 threads | ≤ 15 s (prototype: 10.4 s Caspian = scene 7.6 + GSTF 2.8; resident route ≈ 9.8 s) | measured stages |
| client, cold, inside intermission, 4 low-priority threads | ≤ 40 s | inferred |
| client, warm | ≤ 5 s to playable | read ≤ 0.4 GB of blobs from NVMe + 2.2 s texture decode + upload + instancing; not measured end to end |
| host or server, cold | ≤ 25 s (Kharg ≈ 16 s, Bandar ≈ 15 s) | measured stages |
| server, warm | ≤ 2 s | db 0.3 s + shape/nav restore; inferred |

## 7. Milestone placement

**No S-spike is needed for the measurements:** this report supplies them. Implementation:

| milestone | work |
|---|---|
| **M0** | digest-keyed cache module with atomic writes and a journal; per-item streaming converters; `bf3dump prepare` CLI |
| **M1** | loading screen with `stage`/progress for Metro. Oracles: cold ≤ 15 s, warm ≤ 5 s, peak converter RSS ≤ 0.5 GiB |
| **S1** | tiled Jolt shapes (64 m) and the Caspian + Kharg nav bakes with the BF3 capsule as timing oracles: Caspian ≤ 6 s, Kharg ≤ 13 s on 16 threads |
| **M3** | `Loading`/`Prepare`/`RoundOver.next`/`LevelChange` deadline, slot hold, stall kick, ENet on the net thread, server startup pre-bake (current level first), intermission pre-convert. Oracle: 3-level rotation with 2 clients, one with an empty cache, and no kicks |
| **M4** | Caspian budget check. Oracles: VRAM ≤ 2.5 GiB, client RAM ≤ 1.5 GiB, server ≤ 0.4 GiB at 64 slots |
| **M7** | full MapList admin (RCON-like verbs), "Prepare levels" UI, LRU eviction and the GSTF budget, all 29 levels |

## 8. Corrections to the master plan

- **R10 / 07 §4.5.** Caspian CQL's 2D textures are 1,658 MiB (1,371 unique) [verified]. Only 415 MiB of them are
  drawn by the static world.
- **07 §6.2 nav row.** The bake costs 5–12 s on 16 threads for big maps, per hull class. It was "4–6 s".
- **§3.6 Open: "real Caspian bake time".** Now measured: 5.18 s on 16 threads, 47.9 s on 1 thread, 6.1 MiB.
- **§2.6 / 07 §6.3.** Server ≤ 2 s holds only warm. A cold server needs 10–16 s per level, hence the pre-bake (§6.5).
- **D5 "measure Caspian static-world memory".** Measured: ≈ 31 MiB tiled, ≈ 41 MiB as one shape, with a 0.5 GiB
  transient for one shape.
- **R6** is reinforced. The Metro split also appears on the level_bf3 soup, and with the BF3 capsule the largest
  component holds 23 %.

## 9. Second-source re-check of the 5 most load-bearing claims

1. **Caspian texture bytes and decode time.** The 10-10 run and two runs today agree: 1,371 textures, 1.734 GB,
   decode 2.13–2.20 s, GSTF 2.63–2.79 s. The 29-level census (all modes) gives MP_007 = 1,738 MiB, consistent with
   the mode-only 1,658 MiB. The handoff's independent 1.56 GiB has the same order [verified ×3].
2. **The 29-level GSTF union of 7.50 GiB.** Recomputed from a different census column (BC chain bytes, `tex_bc`):
   7.52 GiB. The two agree to 0.3 %, which is the padding [verified ×2].
3. **Caspian nav bake.** Four bakes in two sessions: placeholder 16 threads 6.07 s, placeholder 4 threads 16.3 s, BF3
   capsule 16 threads 5.18 s, BF3 capsule 1 thread 47.9 s. They are mutually consistent and of the same order as 06's
   independent Metro/synthetic numbers [verified].
4. **Jolt static-world memory of 31–41 MiB.** Two layouts and a 10-10 timing run were measured by process
   private bytes. Godot's own counter cannot confirm it, and I had no independent Jolt-side source, so this is a
   single-method measurement [verified, one method].
5. **BF3 RAM/VRAM requirements and the RCON rotation verbs.** The requirements come from two secondary sites that
   agree. The RCON verbs come from EA's R25 PDF only, which is a primary source, but I found no second one. ENet
   timeouts come from the header itself [verified/report claim as tagged].

## 10. What I could not determine

- **Cold-disk load times.** I could not flush the OS standby cache without admin tools. Today's first run (after
  4 days) was barely slower, which suggests NVMe or cache effects are small here, but HDD and SATA numbers are unknown.
- **The warm end-to-end load inside Godot.** GPU upload of 0.4–1.7 GB of BC data, `mesh_create_from_surfaces` for
  ~1.2 k meshes and ~20 k instance creations were not run in Godot.
- **The real VRAM of render targets and shadows, and of Godot's compute-skinning buffers** for 64 soldiers.
- **Jolt shape-stream file size** (`SaveWithChildren`), and Jolt memory measured from Jolt itself.
- **Nav bakes for vehicle hull classes,** and how many classes there are. Only infantry was baked.
- **The size of scene and gameplay blobs.** The format is not designed yet.
- **What "unresolved" audio is** (1.96–2.51 GiB of SoundDataAsset chunk refs per level not found in the mounted
  bundles: other superbundles, chunk TOCs, or unused variants). So the per-level audio RAM figure covers resolved
  chunks only.
- **Where the handoff's "Caspian 4,545 images" comes from.** No unique count here matches it: unique 2D textures are
  1,371 for CQL and 1,496 for all modes.
- **The collision set.** The level_bf3 soup is not proven to be "exactly the character-collision set" (Havok body
  types are not separated), so the nav sizes and times may shift once R6 is fixed.
- **BF3's own end-of-round intermission length** and whether BF3 itself preloads during it. The only source is a BFV
  statement.
- **XAS decode CPU cost** for streaming decode while mixing (assumed cheap; not measured).
- **The `threading/worker_pool/low_priority_thread_ratio` default** in dev7. I did not check it.
