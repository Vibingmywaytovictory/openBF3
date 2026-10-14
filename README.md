# openBF3

openBF3 is an independent, from-scratch reimplementation of the multiplayer game of Battlefield 3: infantry, bots,
weapons and vehicles, built on Godot 4.8 with all game code in C++ (a GDExtension, no GDScript). It ships no game
content. At runtime it reads the data of your own retail Battlefield 3 install. The project is not affiliated with
or endorsed by Electronic Arts or DICE. The design is in [research/PORTING-RESEARCH.md](research/PORTING-RESEARCH.md);
this tree implements milestone steps S0 (toolchain and skeleton) and M0.1 (install mount, container readers and the EBX
reader in C++).

## The read-as-is rule

* The install (default `C:\Program Files\EA Games\Battlefield 3`) is read in place and never modified. Only
  `bf3_core::InstallReader` resolves install paths. It opens files with `GENERIC_READ` and full sharing, and nothing
  in the code base opens a write handle there.
* Every write target goes through `PathGuard`, which canonicalises the path (8.3 names, casing, `/`, `..`,
  junctions, trailing dots, streams, hard links) and refuses anything under the install root.
* Derived data goes only to `user://bf3cache/`, keyed by content digest. Nothing from the install goes into this repo:
  no dumps, no decoded assets, no partition text. Tests read the install at runtime and assert only numbers.
* The S0 self-test records size and mtime of `Data/layout.toc`, every `cas.cat` and every `.toc`/`.sb` under
  `Data/Win32` and `Update/*/Data/Win32` before and after a session, and fails on any difference.

Install location, first match wins: `--bf3-install=<dir>` (user arg), env `OPENBF3_INSTALL`, project setting
`openbf3/install/path`, then the default path above.

## Prerequisites (Windows x64)

* Visual Studio 2026 (18) with MSVC 19.51+ (the bundled CMake and Ninja work; CMake ≥ 3.25)
* Python 3 (godot-cpp's binding generator), git
* Godot `4.8.dev7.official.c971f93e7` (console and GUI exe). Other builds need an API re-dump (see below).
* A Battlefield 3 install with Update/Patch, needed only to run the game and the install tests

## Clone, build, test

```bat
git clone --recurse-submodules <repo-url> openBF3
cd openBF3
tools\build.cmd dev            :: = vcvars64 + cmake --preset dev + cmake --build --preset dev + ctest --preset dev
```

From a VS x64 developer prompt the three steps are `cmake --preset dev`, `cmake --build --preset dev` and
`ctest --preset dev`. Presets: `dev` (optimised, godot-cpp `template_debug`, everyday use), `debug` (unoptimised,
`/MTd`), `release` (`template_release`, for shipping) and `asan` (`/fsanitize=address`, libraries, tools and tests
only, no extension DLL). Build trees go to `b\<preset>`. The short path matters:
godot-cpp's generated object paths break MAX_PATH (C1083) under deep roots. The longest object path is 119
characters below the checkout (`b\release\gcpp\...plane_tracking.cpp.obj`), so keep the checkout path under about
140 characters. The extension DLL is written to `bf3-opensource\bin\` (gitignored).

Every target uses the static CRT (`/MT`, or `/MTd` in `debug`) and builds with C++ exceptions off
(`_HAS_EXCEPTIONS=0`). This covers godot-cpp, Jolt, ENet, Recast, libdeflate and doctest.

`ctest` runs `core.guard`, `core.formats` (container and EBX readers on synthetic bytes), `core.install`,
`core.cache`, `core.cache_data`, `core.ant.unit`, `core.ant`, `core.res.unit`, `core.res`, `sim.jolt`, `net.enet` and
`host.server`. `core.install` reports *Skipped* when no install is found, `core.cache_data` when
`OPENBF3_CACHE_TESTDATA` is unset, `core.ant` without its export data and `core.res` when `OPENBF3_RES_DATA` does not
point at an exported resource sample (see "Resource decoders"). With an install `core.install` also
reads every bundle manifest of the 53 non-chunk superbundles and checks SHA1s through the CAS, binary, delta and
tail-only paths, and parses every EBX partition of `@all-mp` (50,486) with spot checks of known field values.
`core.fuzz` (also skipped without an install) feeds seeded mutations of real partitions, stored payloads and a
`.toc` (in memory only) through every parser; run it under `ctest --preset asan` after reader changes
(`OPENBF3_FUZZ_ITERS` raises the default 250 mutants per input).

## Run

```bat
set "GODOT=C:\path\to\Godot_v4.8-dev7_win64_console.exe"
:: once per fresh checkout, so Godot registers the extension (.godot\extension_list.cfg):
"%GODOT%" --headless --path bf3-opensource --import
:: S0 self-test, headless: prints a PASS/FAIL table and exits with code 0 (all pass) or 1
"%GODOT%" --headless --path bf3-opensource -- --s0-selftest [--selftest-seconds=5]
:: windowed game (D3D12): server thread + overlay; optional timed screenshot
"%GODOT%" --path bf3-opensource [-- --quit-after=10 --screenshot=C:\temp\s0.png]
:: dedicated server: no scene, no renderer
"%GODOT%" --headless --path bf3-opensource --main-loop DedicatedMainLoop -- --dedicated-seconds=10
```

Known issue: on a project that has never been opened, the first `--import` registers the extension and writes
`extension_list.cfg`, then exits with 0xC0000005 during editor shutdown. Later runs exit 0. Writing a
one-line `.godot\extension_list.cfg` containing `res://openbf3.gdextension` has the same effect. If the extension
is not loaded, the scene runs without `GameSession` and never quits, so scripts should run with a timeout and check
for the `S0 SELFTEST: PASS` line.

## Inspecting the install: bf3dump

`b\dev\tools\bf3dump\bf3dump.exe [--install=<dir>] <command>` reads the install through `bf3_core` only (no Godot).
It writes only to the output path you give, and refuses paths inside the install. Its output is BF3 data, so keep it
out of the repo.

| command | output |
|---|---|
| `list <out.tsv>` | every ebx/res entry, plus the chunk count, of every bundle of every non-chunk, non-Loc superbundle: `superbundle bundle kind name res_type original_size` (1,094,723 lines, byte-identical to the IW4L-based `ebx-dump list` probe) |
| `chunk <sb> <guid>...` | whether the chunk is in the bundle index, its decoded size, its first 48 bytes |
| `reshead <sb> <res>...` | res type, 16-byte meta, decoded size, first 64 bytes |
| `chunks <sb>` | the bundle chunk index: id, sha1, stored size, tail-only flag, range |
| `verify <sb[,sb..]\|@all>` | loads every bundle, checks every stored SHA1 and decodes every ebx/res/chunk |
| `info` | packages, layout, CAS catalogs, superbundle sites |
| `find <sb> <needle>...` | partitions whose lowercase name contains a needle: name, primary type, superbundle, bundle |
| `types <sb> <Type>...` | every instance of the exact types: type, partition, instance GUID |
| `dump <sb> <out_dir> <depth> <follow> <root>...` | one JSON per root: the root partition and the breadth-first closure of referenced partitions whose name contains one of the comma-separated `<follow>` substrings (`*` = all, `-` = none) up to `<depth>`; `fx/` and `sound/` are followed only with `*`; mesh, texture, shader and sound asset partitions are listed but not expanded |
| `ebxload <sb> [--walk]` | parses every partition on all cores and reports counts, failures, bytes and timing; `--walk` also decodes every value |
| `numcheck` | self-check of the JSON number text rules (no install needed) |

For the EBX commands `<sb>` is a superbundle name, a comma-separated list (the first superbundle that holds a
partition wins) or `@all-mp` (every MP/XP level superbundle plus Globals, Weapons, UI and default_settings_Win32).
`find`, `types` and `dump` print the same bytes as the IW4L-based `ebx-dump` probe: JSON keys sorted, base-type fields
first, references as `{"$ref": partition or "internal", "instance", "type"}`, floats as Rust's f32 `Display` text
(shortest round-trip digits, exact ties rounded away from zero) read back as f64 and written like serde_json's zmij
(`2.7849858e+23`, `1e-7`, fixed notation for exponents -5..15). `bf3dump numcheck` (ctest `tools.numtext`) checks
these rules without an install.

## Engine pinning

godot-cpp is built from the API of the exact engine build, not from its bundled JSONs:
`engine-api/4.8.dev7.c971f93e7/` holds `extension_api.json` (`--dump-extension-api`) and
`gdextension_interface.json` (`--dump-gdextension-interface-json`), passed as `GODOTCPP_CUSTOM_API_FILE` and
`GODOTCPP_GDEXTENSION_DIR`. A dev7 DLL loads silently into any 4.8.x, and a changed method hash only fails at its
first call. So on every engine bump (dev → RC → stable): dump both JSONs into a new
`engine-api/<version>.<hash>/` folder, run `<godot> --headless --validate-extension-api <old extension_api.json>`,
point `OPENBF3_ENGINE_API` at the new folder and rebuild from clean.

## Derived-data cache

`bf3_core` `Cache` (`include/bf3/core/cache/`, plan §2.1/§2.4, gap load §6.4) stores converted blobs under one
absolute root. `bf3_godot` passes the globalised `user://bf3cache` and `InstallReader::write_guard()`. One process
opens a root at a time; a second `open` gets `Busy`.

```
<root>/v1/lock                         held exclusively while open
<root>/v1/index.log                    append-only journal: put / touch / remove / pin / close
<root>/v1/<kind>/<k[0:2]>/<k>.bin      128-byte header (magic, version, key, kind, payload size,
                                       payload SHA-256, CRC) + payload, payload 64-byte aligned
```

* **Key** (`make_cache_key`): `k` is the SHA-256 of a length-prefixed encoding of the kind, the source digests
  (the CAS SHA-1s the bundle manifests carry, in converter order), the converter version, the engine
  version+commit, the blob format version, and optionally the Jolt version and other parameters. Content changes
  and tool changes both produce new keys. Old blobs are never looked up again and age out through LRU. SHA-1 and
  SHA-256 come from Windows CNG (BCrypt) behind a portable `Hasher` interface. A changed key encoding needs a new
  `v<n>`; a doctest pins one key value to catch accidental changes.
* **Writes cannot reach the install.**
  * The guard checks the textual path first.
  * The root is opened as a handle, and its *final* path (after links) must lie outside every protected root.
  * Every directory below the root is opened relative to its parent handle, with `NtCreateFile`, and never through
    a reparse point: a junction or symlink inside the tree is refused.
  * The target directory's final path is checked again.
  * The temp file is created relative to that verified handle, written and flushed. It is then renamed with
    `NtSetInformationFile(FileRenameInformationEx, RootDirectory = that handle, replace)`.

  So a junction swapped in after the check cannot move the write. The directory handles below the root do not
  share delete access, so a verified directory cannot be renamed or moved while the cache holds it.
  Hard-linked, directory and reparse-point targets are refused. A hard-linked file is never adopted, served,
  replaced or deleted, whether or not the journal names it: `get` treats it as damaged (miss) and the delete is
  refused. Deletes (eviction, damaged entries, temp files) open the name relative to the verified handle and
  check the link count and the final path of the handle they delete through.

  Do not use `SetFileInformationByHandle(FileRenameInfo)` here: it rejects a non-NULL `RootDirectory`. With a
  NULL one, it resolves a bare name against the process's current directory.
* **Atomic and validated.** Readers see either the old state or a complete entry. `get` maps the file read-only and
  checks magic, version, CRC, key, kind and size. It checks the payload SHA-256 on first use in each process, on
  every read (`CacheVerify::Always`), or when asked (`get(key, true)`, `verify`). A damaged or partial entry is
  deleted and reported as a miss (`dropped_corrupt`).
* **Crash recovery at `open`.**
  * Stray `*.tmp` files are deleted.
  * Journal replay stops at the first short or CRC-failing record.
  * The tree is reconciled with the journal. A file without a record is adopted as least recently used, but only
    if it is single-linked and its header names its key. A record without a file is dropped. A size mismatch is
    deleted.
  * The journal is rewritten atomically whenever anything was repaired or it holds over 2× garbage.
* **Concurrency.** Any thread may call any method, `open` and `close` included. The open state sits behind an
  atomic `shared_ptr`. `close` stops new calls (they return `Closed`), waits for in-flight puts, reads, deletes
  and producers, and only then writes the close record and closes the directory handles. File I/O runs outside
  the index lock. Puts of the same key are deduplicated: one writer runs, the others wait and get `reused`.
  `get_or_put` runs its producer once per key across threads. As with any C++ object, destroying or
  move-assigning a `Cache` must not overlap another call on that same object.
* **Eviction** is explicit (`evict_to`, `enforce_budget`: above the budget, evict down to 90 %), meant for round
  end and never mid-round. It removes the least recently used entries first and skips persistent pins
  (`set_pinned`) and entries with a live `CacheEntry` view. LRU recency is persisted as touch records at
  `flush`/`close`; after a crash, recency since the last flush is lost.

Tests: `core.cache` (scratch trees in `%TEMP%`; a temp directory plays the protected install) and `core.cache_data`.
`core.cache_data` reads real payloads from `OPENBF3_CACHE_TESTDATA`, checks BCrypt SHA-1 against the manifest SHA-1
of every stored payload, and round-trips the decoded payloads. It reports *Skipped* without the variable. That data is
exported from your own install by the research `ebx-dump` tool (`export <superbundle> <out_dir> <max_mb>`, an
addition in a scratch copy) and never enters the repo.

## Resource decoders (`bf3_core`, `include/bf3/core/res/`)

C++ port of IW4L `resource_bf3` (Apache-2.0) for the binary `res` payloads: `MeshSet` (rigid, skinned, composite:
LODs, subsets, vertex declarations, index buffers, UV sets, colour, tangents/binormal signs, bone indices/weights,
bone tables, part bounds/transforms), `DxTexture` (formats, sRGB flag, mips, face-major chunks, ranged
`logicalOffset` views, a reference CPU decode for L8/L16/ARGB8888/DXT1/DXT5/DXT5A) and Havok `HavokPhysicsData` /
`HavokDestructionPhysicsData` (packfile + external fixups, the shape tree Mopp / List / ConvexTranslate /
ConvexTransform / ExtendedMesh / CompressedMesh / Box / Sphere / Capsule / Cylinder / ConvexVertices, convex radius,
part connections, static-model-group subparts, triangle and convex-piece extraction). Namespace `bf3::core::res`.
The decoders take spans (`res` payload, 16-byte `resMeta`, decompressed chunk); they never open files, so the
container layer only hands them bytes. Every offset/length read from the data is bounds-checked; failures are
`ResError` values (no exceptions) with the same texts as IW4L. BF3's texture format table has no BC6H/BC7.

**Parity.** `research/probes/tools/res-oracle` (Rust, path-depends on IW4L `resource_bf3` and the `ebx-dump` install
readers) exports a sample from your install and prints a canonical summary per resource (counts, declarations,
FNV-1a hashes of every decoded position/normal/tangent/UV/colour/part/bone/index array, mip bytes, texels, the full
Havok shape tree, tessellated triangles and convex pieces). `tools/resdump` prints the same summary through the C++
decoders. The exported bytes stay outside the repo (they are BF3 data):

```bat
set CARGO_TARGET_DIR=<scratch>\target
cargo build --release --manifest-path research\probes\tools\res-oracle\Cargo.toml
res-oracle export Win32/Levels/MP_Subway/MP_Subway subway <data>
res-oracle export Win32/Levels/XP2_Skybar/XP2_Skybar skybar <data>
res-oracle summary <data> subway skybar > rust.txt   &  b\dev\tools\resdump\resdump.exe <data> subway skybar > cpp.txt
res-oracle trunc   <data> subway skybar > rust_t.txt &  b\dev\tools\resdump\resdump.exe --trunc <data> subway skybar > cpp_t.txt
set OPENBF3_RES_DATA=<data>& ctest --preset dev -R core.res
```

Result on 2026-10-14: every MeshSet, DxTexture and Havok res of MP_Subway (1,341 / 1,717 / 730 + 9 destruction) and
of the patched expansion level XP2_Skybar (824 / 1,138 / 253 + 23), 2.46 GB of chunks: the 109,001-line summaries are
byte-identical, and so are the 46,534 lines of the damaged-input run (payloads cut at 1/4, 1/2, 3/4, len-1;
geometry and texture chunks cut short; a view at a logicalOffset between mips). Floats are bit-identical because the
port keeps IW4L's operation order and float literals and the hull keeps Rust's tie-breaks. Decoded: 13,966 subsets,
9.96 M triangles, 29,180 mip images, 1,522 Havok roots. `core.res` also runs 8,256 randomly damaged decodes. Timing
(16 threads shared with other builds, so noisy; resdump `--time`, chunk files read before the clock): MeshSet parse +
decode of all subsets 0.87-1.0 s for 2,165 res; DxTexture parse + mip split 0.01 s for 2,855; Havok parse + shape
tree + all triangles (hulls included) 0.97-1.15 s for 1,015. Whole summary pass (decode + hashing): C++ 10.4-14.8 s
over two runs, Rust 15.5 s (warm cache).

**Skinned meshes (for the M9 integration).** Measured on all 561 skinned MeshSets (1,679 LODs) of the sample:

* A vertex's BoneIndices lane `l` (UByte4) selects `subset.bone_slots[l]`, which is a **skeleton bone index**
  (`MeshLod::skeleton_bone`). Every weighted lane is `< bone_slots.size()`, and every slot value appears in the LOD's
  bone table `MeshLod::bones` = `{skeleton index, name hash}` (`find_bone`). BoneWeights are UByte4N and sum to 1.
  `bones_per_vertex` is 1 (3,607 subsets), 2 (172) or 4 (461).
* Bone name hash = `hash_quick(lowercase(name))`: matched for `Wep_Root`, `Wep_Trigger`, `Wep_Slide`, `Wep_Mag`,
  `Wep_Extra1` and the biped names (`Hips`, `Spine`, `Spine2`, `Neck`, `Head`, `LeftHand`, `RightForeArm`, ...).
* `weapons/m9/m9_1p_mesh`: 1 LOD, 5 bones (skeleton 1 Wep_Root, 3 Wep_Trigger, 4 Wep_Slide, 7 Wep_Mag, 9 = hash
  `0xa6d64831`, name not found yet), 2 subsets (one ZOnly), 6,705 vertices, 1 bone per vertex. `m9_3p_mesh`: 5 LODs
  with 5/4/3 bones. 1p arms (`arms1p_bareglove03`, `arms1p_rus`): 69/70 bones, skeleton indices up to 150, 4 bones per
  vertex. 3p soldiers are split into upperbody / lowerbody / head / headgear meshes (59-64 bones, skeleton indices up
  to 208). The decoder does not skin: positions are the bind pose in mesh space.

## Level traversal (`bf3_core`, `include/bf3/core/level/`)

C++ port of IW4L `level_bf3` (Apache-2.0: `level.rs`, `mode.rs`, `info.rs`, `world.rs`, `transform.rs`,
`variations.rs`, `connections.rs` and the counting parts of `geometry/` and `collision/`), namespace
`bf3::core::level`. Everything stays in Frostbite space (metres, Y up, rows right/up/forward/trans, child-in-parent =
row-vector product); there is no IW conversion.

* `load_level(install, "MP_Subway", "TeamDeathMatch0")` mounts the level read-only and picks the mode's bundles: the
  level bundle, its `_settings_win32` / `_gameconfiglight_win32` bundles, then breadth-first every
  `SubWorldReferenceObjectData` the mode includes (`AutoLoad` and the mode in `EnabledOptions` of every `GameMode`
  inclusion setting), nested ones too; absent bundles are listed (Metro has no `Conquest_Large`). Every partition is
  parsed into one `EbxDb`. `level_info` gives names, `WorldSizeXZ`, the modes and the minimap without the rest.
* `World::traverse` walks `LevelData`: layers (`WorldPartReferenceObjectData`), sub-worlds (own scope and
  `MeshVariationDatabase`), `ReferenceObjectData` -> `ObjectBlueprint` objects / prefab scopes, spawn references,
  `StaticModelGroupEntityData` members (from `InstanceTransforms`, else the next `ShapesSubpart` of the group's
  `HavokPhysicsData`, one cursor over the members, `InstanceScale`, per-instance variation), other entities at their
  `Transform`. `Excluded`, `BlueprintTransform` and `Transform` fed by `PropertyConnection`s are resolved through the
  scope interfaces and child-prefab output ports (`Scopes::property`). Each `PlacedObject` has its kind, world
  transform, entity, reference, sub-world, scope, variation hash, group slot and, for meshes, the `MeshAsset`, entity
  type and `PhysicsData`.
* `census.h`: `hidden_parts` (composite parts only later health states show), `starts_hidden` (`Visible` fed by a
  `BoolEntityData` with `DefaultValue` false: ruins, Metro's backdrops), `mesh_census` (the scene meshes and
  instances the placements need: one mesh per (asset, variation entry, hidden parts, bake of a non-uniform transform),
  dropped when no LOD draws a triangle, world kind when placed at most 4 times with a placed radius of at least 16 m)
  and `collision_placements` (each static model group's Havok res at the group transform, and each placed mesh
  object's physics asset prebaked closest to its scale, with the frame the res's shapes go through). The decoders of
  `include/bf3/core/res/` turn those res into triangles and convex pieces.
* Hostile data fails with an error, never crashes or hangs. One `WorkBudget` (`budget.h`, 2^22 units) pays for the
  load, the traversal, the property resolver and every helper run on the `World` (`starts_hidden`, `hidden_parts`,
  `mesh_census`, `collision_placements`; `includes`, `subworld_references` and `read_level_info` take one too). Work
  is charged before it is done, where it is done: the raw length of every data array before it is scanned (null and
  unresolved slots included, a shared array once per scan), each connection candidate a resolution examines, one unit
  per 64 bytes of a constant or name it parses or looks up, one per triangle the census examines. Once a charge
  fails the budget stays exhausted, every later call returns the budget error, and `World::traverse` checks it again
  after the walk. The comment block at the top of `src/level/world.cpp` is the cost model: every loop reachable from
  the public entry points and what bounds it. Beside the budget: depth limits (48 blueprint levels, 8 layer levels,
  32 connection hops and 2^16 hops per resolution), caps of 1 M placed objects, 256 k scopes, 64 k static model
  groups and 4,096 kept sub-world names, 1,024-byte names for lookups, each group res parsed once per traversal, and
  a static model group whose instances need more subparts than its res holds is an error. Retail use over all 230
  level x mode runs: at most 26.6 k units for load + walk (MP_Subway RushLarge0: 19.0 k walker steps, 542 resolve
  hops at most on any run, no resolution capped) and 178 k for everything `bf3dump level` does (load, walk, census,
  collision, listing; MP_003 RushLarge0), 23 times below the budget. `core.level.unit` and `core.level` build
  hostile partitions for every shape found so far, and each one ends on the budget in under a second in the dev
  build: a WorldPart listing itself twice, the same with a static model group of 200 k empty members, 10,000 objects
  whose `Excluded` is fed by self-looping connections, a loop through a prefab's interface, K references to one
  N-slot list (`Objects`, inclusion `Settings`, shared `EnabledOptions`, `MeshVariationDatabase` entries, component
  `HealthStates`, `RigidBodies`, level-description `Mode` lists), and K start-hidden checks over M connections.
* A `World` is not thread-safe: `Scopes` keeps resolution state in its const methods (`property`, `interface`). The
  `WorkBudget` itself is atomic; `mesh_census` charges it from its parallel part, which otherwise only reads the
  partitions and the mount.

`bf3dump level <LEVEL> <MODE> [--legacy-mvdb]` prints the canonical listing (sub-worlds, groups, every placement with
its transform as f32 bit patterns, the mesh census variants and totals, the collision placements; every section
sorted); `bf3dump levelmodes <LEVEL>` lists a level's modes. **Parity**: `research/probes/tools/level-oracle` (Rust,
path-depends on IW4L `level_bf3`/`ebx_bf3`/`resource_bf3` and the `ebx-dump` install readers) prints the same listing
through IW4L's own traversal:

```bat
set CARGO_TARGET_DIR=<scratch>\target
cargo build --release --manifest-path research\probes\tools\level-oracle\Cargo.toml
level-oracle level MP_Subway TeamDeathMatch0 > rust.txt  &  b\dev\tools\bf3dump\bf3dump.exe level MP_Subway TeamDeathMatch0 > cpp.txt
```

Result on 2026-10-14: every mode of every one of the 29 MP levels (230 level x mode runs, 5.7 k-18.3 k lines each) is
byte-identical, transforms bit for bit; no MeshSet failed to decode. Load + traverse + census take 0.15-3.4 s per
run, median 0.24 s (warm file cache, 16 threads shared with other builds). Handoff §9 / sections/03 against `ctest -R core.level`:

| level x mode | handoff | measured |
|---|---|---|
| Metro TDM | 10,873 placements -> 842 meshes (762 model, 80 world), 10,571 instances | identical; groups 499 / 7,869 / 877 explicit / 6,992 Havok, 0 `user_data` mismatches; 710 composite placements hide parts; 64 start hidden (42 + 22 backdrops); 12,315 partitions |
| Bazaar CQL | 620 meshes, 5,783 instances | 5,783 instances; 613 meshes with today's variation lookup, **620 (579 / 41) with `--legacy-mvdb`** (see below); group 381 / 3,646 / 3,292 |
| Caspian CQS | 20,311 placements on 2,277 meshes, 12,725 placed hidden | traversal: 6,847 mesh placements -> 304 meshes, 6,846 instances; group 170 / 4,444 / 4,264; 538 composite placements hide parts (sections/03 §2.5) |
| Palace Domination (IW4L geometry report) | 4,175 placements, 704 meshes (632 / 72), 4,131 instances | identical with `--legacy-mvdb` (701 without) |

* The Metro and Bazaar mesh counts are IW4L's geometry builder at commit bab7c1a (2026-10-01). On 2026-10-07
  (066d34c) IW4L stopped using a `MeshVariationDatabase` entry with an empty `Materials` list (the mesh falls back
  to its default entry; Caspian's service buildings had gone white), which merges variants: Bazaar 620 -> 613
  meshes, Palace 704 -> 701. The C++ follows today's rule; `CensusOptions::legacy_variations` / `--legacy-mvdb`
  reproduce the handoff.
* Caspian's 20,311 / 2,277 / 12,725 are the IW engine's static-model count of a later IW4L build (`bf3 static models:
  20311 placements on 2277 meshes (... 12725 placed hidden for destruction)` in its world report), after the
  geometry builder had gained destruction views: each destruction state's part is drawn by an instance of its own,
  hidden until the state shows. That is a renderer count, not a traversal one; the per-state expansion belongs to the
  destruction work, not to this traversal. Its 20,311 - 12,725 = 7,586 shown instances are 739 more than the
  traversal's 6,847 mesh placements; that remainder is not attributed.
* sections/03's "Metro 702" composite placements that hide parts is Metro ConquestSmall0 (IW4L probe
  `geom_MP_Subway_ConquestSmall0.txt`); Metro TDM's is 710 (`3-CONVERTER-PART/geometry.md`), which `core.level` asserts.

## Layout and pinned third-party code

| path | what |
|---|---|
| `src/bf3_core` | `InstallReader`, `ReadOnlyFile`, `PathGuard`, install snapshot, derived-data `Cache`, `ant/` animation banks, `res/` resource decoders. Containers ported from IW4L: `Install` (packages, layout, CAS catalogs), `LevelMount` (base, patch and delta bundle merge; ebx/res/chunk index and payloads), DbObject, toc, binary and CAS manifests, block framing, delta bundles. EBX ported from IW4L: `EbxPartition` (read through the partition's own type descriptors, validated in full), values, `EbxDb`, and `EbxLibrary` (partition lookup by name and GUID across mounted superbundles, lazy or bulk-parallel loading). Godot-free |
| `src/bf3_sim` | own Jolt world at a fixed 60 Hz, Recast/Detour link check (Godot-free) |
| `src/bf3_net` | upstream ENet wrapper, loopback ping (Godot-free) |
| `src/bf3_host` | `ServerHost` on its own `std::thread`, `LoopbackClient` (Godot-free) |
| `src/bf3_godot` | the GDExtension: `GameSession`, `DedicatedMainLoop`, S0 self-test (the only Godot code) |
| `tools/bf3dump` | command-line install inspector on `bf3_core` |
| `tests/` | doctest executables run by ctest |
| `tools/resdump` | canonical summary of exported resources through `bf3::core::res` (parity with the Rust oracle) |
| `bf3-opensource/` | the Godot project (Forward+, D3D12, Jolt); `scenes/main.tscn` is the main scene |

| submodule | pin | licence |
|---|---|---|
| `extern/godot-cpp` | `b051e694` (master, 2026-10-09; includes hot-reload fix `79c37b21`) | MIT |
| `extern/JoltPhysics` | `v5.6.0`, built with `CROSS_PLATFORM_DETERMINISTIC` | MIT |
| `extern/enet` | `v1.3.18` | MIT |
| `extern/recastnavigation` | `v1.6.0` | zlib |
| `extern/doctest` | `v2.5.3` | MIT |
| `extern/libdeflate` | `v1.26` (zlib inflate of payload blocks) | MIT |

Follow-ups: GitHub Actions CI (configure, build, ctest, headless self-test without an install); the Linux build and
its symbol-visibility test for our Jolt next to Godot's.
