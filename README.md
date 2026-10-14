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
`core.fuzz`, `core.cache`, `core.cache_data`, `core.ant.unit`, `core.ant`, `core.res.unit`, `core.res`,
`core.res.install`, `core.gameplay.unit`, `core.gameplay.weapons`, `core.gameplay.fuzz`, `core.ui.text.unit`,
`core.ui.text`, `core.ui.text.fuzz`, `sim.jolt`, `net.enet`, `host.server` and `tools.numtext`. The `*.unit`
tests, `core.guard`, `core.formats` and `tools.numtext` need no data. The install tests report *Skipped* when no
install is found; `core.cache_data` is skipped when `OPENBF3_CACHE_TESTDATA` is unset and `core.res` when
`OPENBF3_RES_DATA` does not point at an exported resource sample (see "Resource decoders"; `core.res.install` covers
the same decoders straight from the install). With an install `core.install` also
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
:: weapon inspector (README "Weapon inspector and localisation"): F2 in game; the M0 exit check (PASS/FAIL, exit 0/1);
:: one weapon's block on stdout
"%GODOT%" --path bf3-opensource -- --inspector [--inspector-select=weapons/m16a4/u_m16_burst]
"%GODOT%" --headless --path bf3-opensource -- --m0-inspector-check
"%GODOT%" --headless --path bf3-opensource -- --inspect-weapon=weapons/m16a4/u_m16a4 [--unlock=u_m16a4_bipod]...
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

**Loading from the install (`res/res_loader.h`).** The API the Godot bridge calls: given a `LevelMount` whose bundles
are indexed (`load_all_bundles` / `load_bundles`) and a res name (matched like `read_res`: case-insensitive, `\` =
`/`), it reads the payload and `resMeta` (`LevelMount::read_res`), every chunk the res names
(`LevelMount::read_chunk`, always the *whole* chunk: a level bundle's ranged tail is completed from the CAS or the
chunk tocs) and the ranged view's `logicalOffset` (`chunk_info`), and decodes:

```cpp
Result<LoadedMeshSet> m = load_mesh_set(mount, "weapons/m9/m9_1p_mesh"); // MeshSet + its LOD chunks
m->decode(0, SubsetFilter::Drawn, subsets);                               // LOD 0 without ZOnly subsets
Result<LoadedTexture> t = load_texture(mount, "weapons/m9/m9_d");        // DxTexture + whole chunk; t->mips(...)
Result<LoadedHavok> h = load_havok(mount, name);                          // either Havok type, root shapes decoded
```

Each `Loaded*` owns every byte its data refers to (`RawRes::payload`, `RawChunk::bytes`); the only views,
`MipSlice::data` and the spans from `geometry()` / `whole()`, point into those vectors and stay valid while the object
lives (moving it keeps them; nothing stores a view, so copies are safe). Errors are `Result` values: container
failures keep `LevelMount`'s message, decode failures read `res <name>: <ResError text>`; a res whose chunk cannot be
read fails as a whole. `read_raw_res` / `read_raw_chunk` give the bytes alone, and `decodable_res_names` lists a
mount's MeshSet / DxTexture / Havok res the way the oracle's export does (first spelling per lowercase name, sorted).
Loading is thread-safe across res (`LevelMount` reads are). `core.res.install` (exit 77 = skipped without an install)
loads every MeshSet, DxTexture and Havok res of MP_Subway straight from the install on all cores, decodes every LOD,
mip chain (and the 1,313 ranged views at their `logicalOffset`) and shape tree with triangles, convex pieces,
part connections and group subparts: 0 errors, 1,341 / 1,717 / 730 + 9 res, 9,263 subsets, 5,366,947 triangles,
17,545 mip images, 1,130 Havok roots (the totals `core.res` gets from the export), plus the M9 1p mesh checks below;
about 2 s in the dev preset with a warm file cache.

**Hostile input.** Besides bounds-checking every read, the walks are bounded. The Havok shape graph is a DAG in the
file but a tree in memory, so a shared child is decoded once per use: `shapes()` refuses cycles (`shape cycle`),
nesting past 64 (retail: 4), a decoded tree larger than 128 bytes per packfile data byte + 64 MiB (`shape tree
size`; retail peak 28.6 per byte, XP3_Alborz's static model group: 107 MB from 3.76 MB, 187,197 nodes) and convex hull
work (sum of n^2 over the leaves `havok_triangles` hulls) past 64 per data byte + 32 M (`convex hull work`; retail
peak 19.1 M), so the triangles and convex pieces of an accepted tree are bounded too. `group_subparts` walks
breadth first with parent links: cycles and more than data / 4 + 4,096 visits are errors (a self-listing ListShape
used to double its pending list per level). `MeshLod::decode` refuses subsets whose ranges add up to more than 8x the
LOD's vertex or index region (retail: 2x, double-sided foliage), and `split_mips` an empty mip. The budgets were
measured on every res of the 29 multiplayer levels (14,472 Havok res, 31,984 MeshSets): none comes near them, and the
parity summaries below are unchanged. `core.res.unit` builds hostile packfiles in memory (a list listing itself, a
two-list cycle, a 1,000-deep MOPP chain, a 16-wide 12-deep DAG of lists, a 1,024-point convex leaf listed 4,096
times): each fails with its error in well under a second.

**Parity.** `research/probes/tools/res-oracle` (Rust, path-depends on IW4L `resource_bf3` and the `ebx-dump` install
readers) exports a sample from your install and prints a canonical summary per resource (counts, declarations,
FNV-1a hashes of every decoded position/normal/tangent/UV/colour/part/bone/index array, mip bytes, texels, the full
Havok shape tree, tessellated triangles and convex pieces). `tools/resdump` prints the same summary through the C++
decoders, either over an export or straight from the install (`--install <superbundle> <tag>...`, through
`res_loader.h`, in the export's order). The exported bytes stay outside the repo (they are BF3 data):

```bat
set CARGO_TARGET_DIR=<scratch>\target
cargo build --release --manifest-path research\probes\tools\res-oracle\Cargo.toml
res-oracle export Win32/Levels/MP_Subway/MP_Subway subway <data>
res-oracle export Win32/Levels/XP2_Skybar/XP2_Skybar skybar <data>
res-oracle summary <data> subway skybar > rust.txt   &  b\dev\tools\resdump\resdump.exe <data> subway skybar > cpp.txt
res-oracle trunc   <data> subway skybar > rust_t.txt &  b\dev\tools\resdump\resdump.exe --trunc <data> subway skybar > cpp_t.txt
set OPENBF3_RES_DATA=<data>& ctest --preset dev -R core.res
rem no export needed: the same two summaries from the install
resdump.exe --install Win32/Levels/MP_Subway/MP_Subway subway Win32/Levels/XP2_Skybar/XP2_Skybar skybar > inst.txt
resdump.exe --trunc --install Win32/Levels/MP_Subway/MP_Subway subway Win32/Levels/XP2_Skybar/XP2_Skybar skybar > inst_t.txt
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
over two runs, Rust 15.5 s (warm cache). Install mode (2026-10-14, after the hostile-input bounds above): both
`resdump --install` outputs are byte-identical to the oracle's summary and damaged-input run of the export (109,001 and
46,534 lines, SHA-256 `42ffd1a3...` and `f04df8c4...`), and to a fresh oracle build's; the summary pass reads the 2.46 GB
of chunks from the install in 16 s, the damaged-input pass takes 6 s.

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

## Ant animations (`bf3_core`, `include/bf3/core/ant/`)

`AntInstallSource` reads the Ant animation banks from the install as is: every `AntPackageAsset` and AssetBank res
of the 53 non-chunk superbundles (first superbundle in layout order wins), res by name and Chunk packages by
`StreamingGuid`, in about 1 s; `AntPackage`/`AntLibrary` parse them and decode DCT, Raw and Frame clips. `antdump`
(`tools/antdump/README.md`) runs the census, clip dumps and oracle comparisons on `--install`, and documents the
M9 1P empty reload clip and its mapping onto the 1P skeleton for Godot playback.

## Weapon tables (`bf3_core`, `include/bf3/core/gameplay/`)

The first gameplay table readers (plan §3.2, `research/topics/01-weapons.md`). Namespace `bf3::core::gameplay`; they
read through any `EbxResolver` (an `EbxLibrary` over the level superbundle, since every MP level's main bundle carries
the full weapon EBX) and return plain data that no longer points into a partition.

* `resolve_weapon(db, partition, {unlocks, ui})` -> `WeaponDef`. `partition` is a variant
  (`SoldierWeaponUnlockAsset`, e.g. `weapons/m16a4/u_m16_burst`) or a blueprint. Chain: unlock -> blueprint ->
  `SoldierWeaponData` -> `WeaponFiringData` (deploy) -> `FiringFunctionData` (fire modes, rate of fire, burst, bolt
  action, hold-and-release, ammo, reload tactical/empty/per-shell with delay and `ReloadThreshold`, overheat, shot
  speed/position, pellets, pellet cone) -> projectile (`BulletEntityData` damage and falloff, gravity, TTL, arming;
  `MissileEntityData` engine, max speed, direct damage; `GrenadeEntityData` bounce and fuse; explosion and dud
  explosion; `WeaponSuppressionData`; material pair) and `GunSwayData` (per stance x zoom x move state
  min/max/+shot/-second, recoil up/left/right/max/decrease, first-shot recoil multiplier, stance transitions,
  suppression modifiers, camera spring). The weapon-name SID comes from `ui/uiweaponmetadata` (Win32/UI) by the
  unlock identifier, with its text-database hash (`ID_P_WNAME_M16A4` -> `0fd83dfa`).
* Unlocks: the variant and its `Extra` chain plus requested attachments are active. A `WeaponFiringDataModifier`
  replaces the firing data (and with it the gun sway), the other modifiers patch fields in array order (shot,
  projectile, magazine, deploy, zoom FOV, misc flags OR-ed, class, aiming controller, sound, anim type), and
  `GunSwayModifierData` of an active unlock multiply the sway (a `MaxAngleModifier` or `Lag*Mod` of 0 = unset; bipod
  and laser modifiers are kept as `conditional`). The AI firing data in the same partition is never applied unless
  asked for. The rule is [inferred] in topics/01 §1.5; the Symthic match below supports it.
* `MaterialGrid` reads the level's `MaterialGridData` (found through `LevelData.RuntimeMaterialGrid`) and its
  `materials/materialcontainer`: `damage(projectile property, target property)` gives the
  `MaterialRelationDamageData`/`PenetrationData` cell, row = `MaterialIndexMap[PhysicsPropertyIndex & 0xFF]`.
  `read_hit_capsules` reads the 13 soldier capsules and `body_multipliers` the head/body/chest/legs
  `DamageProtectionMultiplier` for a projectile. On the delta levels (XP2-XP4) the level's own container copy lacks
  pairs the patched grid uses (measured: 3,839 on every XP2 level, 1,775 on XP3, 1,071 on XP4; the handoff quotes 3,842
  for XP2_Palace), so `load` takes fallback resolvers (any base level, e.g. MP_Subway); after it 0 pairs stay
  unresolved and the body multipliers equal Metro's.

`bf3dump weapon <partition> [--unlock <u_..>]... [--level <sb>]` prints one resolved WeaponDef with the derived
numbers (shot interval, effective RPM, max range, drop at 100/300 m) and the level's body multipliers for its
projectile; a blueprint resolves through its base variant. `bf3dump weapons [--level <sb>]` prints one TSV row per
weapon variant. The level defaults to MP_Subway. Examples: `bf3dump weapon weapons/m16a4/m16a4` (M16A3),
`bf3dump weapon weapons/m16a4/u_m16_burst` (M16A4: burst 3 @ 800, ADS 0.1, horizontal -0.3..0.075),
`bf3dump weapon weapons/m16a4/u_m16a4 --unlock u_m16a4_sound_suppressor --unlock u_m16a4_bipod`.

**Tests.** `core.gameplay.unit` (no install: falloff, interval, drop, SID hash, modifier math);
`core.gameplay.weapons` (exits 77 without an install) asserts the §7.1 rows on MP_Subway: M16A3, M16A4 burst,
AK-74M, M249, M98B and JNG-90/L96/M40A5/SV98 (46.2/43.5/54.5/48 RPM), AN-94 (2 @ 1200), 870MCS (12 pellets, 125 RPM,
3 deg cone, 1.15 + 0.67/shell), M9, RPG-7/SMAW, M320 HE/M67, attachments, the UGL variant; the body multipliers on
Metro, MP_001, Caspian and XP2_Palace (bullet 2.0/1.0/1.0/0.91, Sniper 2.35/1.0/1.25/0.91, .338 2.35/1.0/1.06/0.91,
buckshot 1.0/1.0/1.0/0.91, flechette 1.0, melee 0.67/0.5, missile/40 mm 2.5, grenade 0); the arithmetic checks
(75 ms, 1.60 m at 300 m, 975 m and 3,250 m ranges, shots to kill, one-shot ranges 17.5/15/20/50.8/16.3 m); and that
every weapon variant of all 29 MP levels resolves (5,053 resolutions, 0 errors, about 7 s). `core.gameplay.fuzz`
pushes seeded mutations of the M16A4 partitions and of the Metro grid through the readers (no crash under ASan).

**Parity, measured 2026-10-14** (comparison scripts and data stay outside the repo):

* EBX probe (`research/probes/ebx-data-probe.md` §3, its 10-weapon summary): 158 of 158 compared fields identical.
* topics/01 roster (Rime dump, 172 unlock assets, base firing data): 101 identical; the other 71 are variants whose
  modifiers the roster did not apply (UGL and M26 underslung variants, silenced pistols, M16A4/M4 burst, G17, the
  standalone M320/M26 ammo variants) plus 4 launchers whose direct-hit damage the roster put in the bullet-damage
  column.
* Symthic Aftermath chart (62 rows, 16 fields, topics/01 C6): base blueprints 50/62 identical, the same 12
  differences as topics/01 §2.8. Resolving the variants (u_m16_burst, u_m4, u_glock17) brings 52/62, and Symthic's
  bolt-action convention (effective RPM, hip recoil block) 57/62. The 5 left: M93R and UMP-45 SIPS, SKS recoil
  decrease (Symthic shows the NoZoom 15, data Zoom 9), LSAT ADS 0.4 vs 0.5, M4 empty reload 2.6 (the burst firing data
  of u_m4) vs Symthic 2.48 (the M4A1 base value): post-Aftermath patches or chart errors; the install is the final
  patch. Max range = v0 x TTL on 62/62.
* Every MP level carries the same data: the 172 variants shared by all 29 levels resolve to identical rows; XP2 and
  XP4 levels add 8 gun-master/premium variants, MP_012 adds the M136.
* §7.1 quotes the M98B drop at 300 m as 1.05 m; the arithmetic gives 1.045 m (the test checks 1.0447).

## Text database

`bf3_core` `ui/text_db.h` (namespace `bf3::core::ui`) reads BF3's localised UI strings from the install at runtime,
clean-room from `research/topics/gap-r1-ui-hud-minimap-localisation.md` §4 (plan §3.8, §7.8):

* **Locating.** `localization/localization` (`LocalizationAsset`, `Win32/Globals`) lists 11 `UITextDatabase`
  partitions. Each names a binary chunk and a histogram chunk with their decoded sizes. The chunks are toc-level
  chunks of the chunk-only superbundle `Win32/Loc/<lang>` (`en` is the only one installed), read through
  `Install::chunk_toc` (the patch toc wins over the base toc). `open_text_database(install)` takes the first listed
  database whose chunks are installed (`localization/us_loc` on an English install); `load_text_database` takes a
  partition name or an `EbxResolver` you already have.
* **Format.** Binary `0x00039000`: header, NUL-terminated tag, `count × (u32 hash, u32 offset)`, NUL-terminated
  strings. Histogram `0x00039001`: `u16` code units. A byte below 0x80 is ASCII, otherwise the histogram gives the code
  unit or a page selected by the next byte. Strings are kept as UTF-8. `TextDatabase::parse` checks every header
  field, table bound, string offset and terminator, and bounds the total decoding work. Malformed sequences inside a
  string are counted (`decode_errors`) and do not fail the parse.
* **Keys.** `text_hash(sid)`: `h = 0xFFFFFFFF; h = h * 33 + c` over the SID exactly as written (additive, unlike the
  xor hash of EBX names). Use `find(hash)`, `find_sid(sid)`, `resolves(sid)` and `translate(sid)`. `translate`
  returns the SID itself when there is no usable entry (plan §2.1).
* **SID census** (`ui/text_census.h`). A SID is a CString field value that is entirely `ID_[A-Z0-9_]*`.
  `sid_census` collects the distinct SIDs of a partition set and counts how many resolve.
  `research_census_partitions()` holds the 28 partitions with SIDs out of the 170 the research dumped.

`bf3dump text <sid-or-hash>...` prints `input  hash  text`. A hash is 8 hex digits or `0x…`, and an absent key prints
`absent -> <fallback>`. `bf3dump text --census [<sb-spec>]` prints the database partition, language and Loc
superbundle; both chunk GUIDs, sizes and SHA-1s; the entry count, tag, decode errors and duplicate hashes; then the
EBX SID census, listing each SID that does not resolve. Without `<sb-spec>` the census uses the research set; with
one (for example `@all-mp`) it loads and scans every partition of those superbundles.
`bf3dump text --tsv <out.tsv>` writes every entry as `hash \t text`, escaping `\\ \t \n \r`. The output is BF3 text,
so keep it outside the repo.

Tests: `core.ui.text.unit` builds synthetic databases with no data. It covers the hash, direct, paged and
surrogate-pair decoding, every malformed-sequence and header error, duplicates, the fallback, the overlap bound and
2,000 mutants. `core.ui.text` reads the retail database and `core.ui.text.fuzz` feeds 300 seeded in-memory mutants of
the retail chunks to the parser. Both report *Skipped* (77) without an install.

**Parity** (2026-10-14, against the research decoder and its outputs, kept outside the repo):

* `us_loc` has 15,515 entries, tag `Global`, 0 decode errors and 0 duplicate hashes. Both chunk SHA-1s equal the
  research's extracted chunks.
* `bf3dump text --tsv` is identical to the Python decoder's output for all 15,515 strings, after both are escaped the
  same way.
* `ID_H_US_A` → `34a54e7a`, `ID_H_US_B` → `34a54e7b`, `ID_P_WNAME_M16A4` → `0fd83dfa` → "M16A3".
* The EBX census finds 1,271 distinct SIDs, of which 1,247 resolve. That is the research set exactly. The research
  collected every whole-string `ID_…` value in its UI-mapping JSON dumps (the Grand Bazaar level dump came after the
  count).
* Two corrections to the research table. First, the 24 SIDs that do not resolve have **no entry at all**: no entry
  holds `<MISSING>`, which was the research script's placeholder. Second, `ID_P_WNAME_M16` does exist and resolves to
  "M16A4".

## Weapon inspector and localisation

The M0 exit criterion "an in-game inspector prints the resolved M16A3/M16A4-burst WeaponDef matching §7.1 and its
localised name" (plan §6), with the localisation of plan §2.5 / §3.8.

* **One block for the tool and the game.** `bf3_core` `gameplay/weapon_format.h` holds the WeaponDef text block that
  `bf3dump weapon` used to build itself (`format_weapon`, moved without changing a byte: the stdout and stderr of
  `bf3dump weapon` for 14 cases, including a blueprint, attachments, a missile, a shotgun, a pistol, a UGL, delta and
  base levels and an unknown partition, and of `bf3dump weapons` on MP_Subway and XP2_Palace, are identical before and
  after). Given a text database it adds a `name` line after `name sid`: the SID's text, or the SID itself and
  `(not in the text database)`. `bf3dump weapon <partition> --name` prints it (`ID_P_WNAME_M16A4` -> `M16A3`,
  u_m16_burst's `ID_P_WNAME_M16` -> `M16A4`). `gameplay/weapon_catalog.h` holds the sources both use: the level's
  `EbxLibrary` (default MP_Subway) plus Win32/UI, the level's material grid (MP_Subway's container as the fallback on
  the delta levels) and the hit capsules; a blueprint resolves through its base variant. `WeaponCatalog` parses the
  level once, lists its variants and resolves any of them thread-safely.
* **Localisation.** `Bf3Localisation` (a node in `main.tscn`) locates the install like `GameSession`, opens it and
  loads `localization/us_loc` on its own thread, then, on the main thread, registers a `Bf3Translation` (a
  `Translation` whose `_get_message` looks the SID up in bf3_core's `TextDatabase`) for locale `en` with the
  `TranslationServer`. `tr("ID_P_WNAME_M16A4")` returns "M16A3"; a message that is not a SID (`ID_[A-Z0-9_]*`) or
  that the database does not resolve gets no answer from it, so `tr()` returns the SID itself (plan §2.1). Load time
  on this machine: 0.07 s.
* **Inspector.** `Bf3WeaponInspector` (a `CanvasLayer` in `main.tscn`) toggles with **F2**. On first use it builds a
  `WeaponCatalog` on a worker thread from the install `Bf3Localisation` opened (MP_Subway: 12,524 partitions, 172
  variants, all resolved, 0.15-0.4 s warm) and lists the M16A3 (`weapons/m16a4/m16a4`, through `u_m16a4`) and the
  M16A4 burst (`weapons/m16a4/u_m16_burst`) first, then every variant by partition. Names in the list and the title go
  through `tr()`; variants whose SID has no text (the M26 MASS underslung variants) show their SID. The right side
  shows the block, the same text `bf3dump weapon --name` prints. User args: `--inspector` (show at start),
  `--inspector-select=<partition>`, `--inspector-level=<superbundle>`, `--inspect-weapon=<partition>
  [--unlock=<u_..>]...` (prints the block to stdout and quits, 1 on error; identical to `bf3dump weapon ... --name`
  for the M16A4 with suppressor and bipod) and `--m0-inspector-check`.
* **The check.** `--m0-inspector-check` resolves both weapons through the same worker path as the panel and compares
  them with §7.1: 800 RPM / 75 ms, mag 31 x 5, reload 1.8/2.37, v0 650, 25 -> 18.4 over 8-50 m, g -15, hip 2.5-7
  +0.1, ADS 0.2-3 +0.1, recoil 0.26 up, -0.4..0.1, decrease 18, FSRM 2.5, deploy 0.67; burst 3 @ 800, ADS min 0.1,
  horizontal 0.075 left / 0.3 right; names "M16A3" / "M16A4" through `tr()` and in the block; `tr()` of an unknown
  SID; 15,515 strings; every variant resolves; the install snapshot (sizes and mtimes of `layout.toc`, `cas.cat`
  and every `.toc`/`.sb`) unchanged. It prints both blocks and one `M0 CHECK PASS|FAIL` line per item, then
  `M0 INSPECTOR CHECK: PASS (54/54)`, and exits 0 or 1 (also 54/54 with `--inspector-level` XP2_Palace).

Tests: `core.gameplay.inspect.unit` (the `name` line on a synthetic WeaponDef and text database, no install) and
`core.gameplay.inspect` (WeaponCatalog over MP_Subway with the retail text database; 77 = skipped without an
install). `godot.inspector` and `godot.s0_selftest` run the Godot project headless (`--m0-inspector-check`,
`--s0-selftest`) through `tests/godot_check.cmake` when `OPENBF3_GODOT` names the console executable, and are
skipped (77) otherwise; they share a resource lock, since both use the project and its DLL (CMake 3.29+).

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
