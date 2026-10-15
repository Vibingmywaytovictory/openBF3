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

`ctest` runs `core.guard`, `core.formats` (container and EBX readers on synthetic bytes), `core.install`, `core.fuzz`,
`core.cache`, `core.cache_data`, `core.ant.unit`, `core.ant`, `core.res.unit`, `core.res`, `core.res.install`,
`core.gameplay.unit`, `core.gameplay.weapons`, `core.gameplay.fuzz`, `core.gameplay.inspect.unit`,
`core.gameplay.inspect`, `core.gameplay.tables.unit`, `core.gameplay.tables`, `core.level.unit`, `core.level`,
`core.level.collision.unit`, `core.level.collision`, `core.ui.text.unit`, `core.ui.text`, `core.ui.text.fuzz`,
`anim.unit`, `anim`, `render.unit`, `render`, `prepare.unit`, `prepare`, `sim.jolt`, `net.enet`, `host.server`,
`tools.numtext` and, when `OPENBF3_GODOT` names the Godot console executable, the headless project checks
`godot.s0_selftest`, `godot.inspector`, `godot.m9_reload` and `godot.collision`. The `*.unit` tests, `core.guard`,
`core.formats` and `tools.numtext` need no data. The install tests report *Skipped* when no install is found;
`core.cache_data` is skipped when `OPENBF3_CACHE_TESTDATA` is unset and `core.res` when `OPENBF3_RES_DATA` does not
point at an exported resource sample (see "Resource decoders"; `core.res.install` covers the same decoders straight
from the install). With an install `core.install` also reads every bundle manifest of the 53 non-chunk superbundles
and checks SHA1s through the CAS, binary, delta and tail-only paths, and parses every EBX partition of `@all-mp`
(50,486) with spot checks of known field values. `core.fuzz` (also skipped without an install) feeds seeded mutations
of real partitions, stored payloads and a `.toc` (in memory only) through every parser; run it under `ctest --preset
asan` after reader changes (`OPENBF3_FUZZ_ITERS` raises the default 250 mutants per input).

S2 vehicle replay adds `s2.contract` (no data) now and `sim.vehicle.unit` / `sim.vehicle` with the implementation (see "S2 vehicle replay").

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
:: M9 1P empty reload on BF3 arms ("Viewmodel (M9 reload)"); headless check: --headless ... -- --m9-reload-check
"%GODOT%" --path bf3-opensource res://scenes/m9_reload.tscn -- --quit-after=10 [--screenshot=C:\temp\m9.png --shot-times=0.1,1.2,1.3]
:: derived-data cache (README "Preparing the cache"): convert a level x mode's meshes + weapons into user://bf3cache
b\dev\tools\bf3dump\bf3dump.exe prepare MP_Subway TeamDeathMatch0 [--cache=<dir>] [--threads=N]
:: weapon inspector (README "Weapon inspector and localisation"): F2 in game; the M0 exit check (PASS/FAIL, exit 0/1);
:: one weapon's block on stdout
"%GODOT%" --path bf3-opensource -- --inspector [--inspector-select=weapons/m16a4/u_m16_burst]
"%GODOT%" --headless --path bf3-opensource -- --m0-inspector-check
"%GODOT%" --headless --path bf3-opensource -- --inspect-weapon=weapons/m16a4/u_m16a4 [--unlock=u_m16a4_bipod]...
:: Metro collision overlay (README "Collision overlay"); headless check: --headless ... -- --collision-check
"%GODOT%" --path bf3-opensource res://scenes/metro_collision.tscn -- [--level=XP3_Alborz --mode=ConquestLarge0 --quit-after=10 --screenshot=C:\temp\coll.png]
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

## Preparing the cache

Plan §2.4 step 3 and §3.12: converted data goes into the `Cache` above, one item at a time, keyed by content.
Two converters exist so far. Both are Godot-free and stream per item: a lane holds one item's arrays and frees them
before it takes the next.

* **Render meshes** (`bf3_render` `mesh_cache.h`). One item is one MeshSet res plus a request: the LODs, the subset
  filter and, for skinned meshes, the skin lane binds. It yields one `PreparedMesh` per LOD, exactly what
  `prepare_mesh` returns: the ArrayMesh-layout arrays of every surface and its material binding
  (`MeshAsset.Materials` index and the subset's material name). Composite meshes also get the part of each vertex
  (`PreparedSurface::parts`), so hidden destruction parts can be dropped when a level is instanced.
  * Variation textures, hidden parts and baked transforms are applied per placement, so one blob serves every variant
    and every level whose bytes are the same.
  * Key: kind `mesh`. The sources are the manifest SHA-1 of the res, then of every chunk the requested LODs read
    (`LevelMount::chunk_source_sha1s`, which mirrors `read_chunk`'s choice of bytes). It also covers the converter
    version, the engine id, the blob format and `lods=..;filter=..;binds=<SHA-256 of the lane binds | none>`.
  * The key comes from the few-KB res payload alone (`read_mesh_head`). A hit reads no chunk. A miss reads only the
    chunks of LODs `0..max(requested)`.
  * Blob `OBF3MESH` v1, layout in the header. `decode_mesh_blob` checks every count against the bytes left before it
    allocates. It also checks the index range, the bone signs, the category and the mask bits, and refuses trailing
    bytes.
* **WeaponDefs** (`bf3_core` `gameplay/weapon_blob.h`). The blob `OBF3WDEF` v1 is every field in declaration order;
  `encode(decode(b)) == b`.
  * The sources are the EBX partitions the resolver touched, known only after it runs. A recording `EbxResolver`
    lists them, absent names included.
  * The list is stored under `weapon-deps`, keyed by the variant partition's SHA-1. The def is stored under `weapon`,
    keyed by the current SHA-1 of every listed partition (zero for an absent one) and the list's SHA-256.
  * A warm lookup reads the list and takes each SHA-1 from the mount's manifests: no EBX is parsed or resolved.
* `cached_convert` (`cache/cached_convert.h`) runs one item:
  * **hit**: the entry passed the cache's checks and the decoder;
  * **converted**: this call wrote the entry;
  * **reused**: another writer stored the same key meanwhile;
  * **uncached**: no cache, or the put failed.
  A blob that passes the SHA-256 but not the decoder is removed and reconverted, like a damaged one
  (`dropped_corrupt`).
* The engine id in every key is the pinned engine API folder, `4.8.dev7.c971f93e7`. The game and `bf3dump` therefore
  compute the same keys, and an engine bump re-converts everything.
* Textures are not cached (plan §2.4: resident BC data is decoded from the install at every load).

`bf3dump prepare <LEVEL> <MODE> [--cache=<dir>] [--threads=N] [--no-weapons] [--no-meshes]` does the following:

* Loads and traverses the level x mode and takes the mesh census.
* Converts each distinct MeshSet of a kept scene mesh: the census's LODs (at most 4), or LOD 0 when every kept variant
  is world kind. Drawn subsets, unskinned.
* Converts every `SoldierWeaponUnlockAsset` under `weapons/` of the level database, with Win32/UI for the name SID.
* Prints items, hits, misses (converted, reused, uncached), failures, dropped entries, bytes written and read, the
  cache's entries and bytes before and after, timings and peak working set.

Lanes default to all cores. The root defaults to the folder Godot's `user://bf3cache` resolves to for this project,
`%APPDATA%\Godot\app_userdata\bf3-opensource\bf3cache`. This was checked: the game's log prints
`C:\Users\<you>\AppData\Roaming\Godot\app_userdata\bf3-opensource\bf3cache`, and `bf3dump prepare` then opened the 2
entries the game had written there. Writes go through `InstallReader::write_guard()`.

**In the game** (`bf3_godot` `godot_cache.h`):

* `acquire_cache` opens the root on the calling loader thread, so journal replay and the tree scan stay off the main
  thread. Every holder in the process shares one `Cache`, which closes when the last holder releases it.
* If the root is held by another process (`Busy`) or refused, the loaders convert without a cache.
* The viewmodel demo's loader reads each mesh's res, binds its LOD to the shared skin and calls `load_mesh_cached`.
  The status label's second line and the log show `converted` on a cold cache and `hit` on a warm one.
* `--bf3-cache=<dir>` picks another root; `--no-cache` turns the cache off.
* `--m9-reload-check` adds two rows:
  * `cache.meshes`: both meshes went through the cache;
  * `cache.equal_fresh`: the meshes the bridge drew equal a fresh conversion from the install, array for array.

**Measured 2026-10-15** (Ryzen 7 5800X, 16 threads, NVMe, warm OS file cache, dev preset; scratch roots):

| run | Metro TDM (MP_Subway TeamDeathMatch0) |
|---|---|
| items | 619 MeshSets (840 kept census variants, 10,873 placements; 921,956 vertices, 715,047 triangles over the prepared LODs) + 172 weapon variants = 963 entries (with 172 dependency lists) |
| cold, 16 lanes (3 runs) | total 1.25-1.46 s: load + traverse 0.19-0.21 s, census 0.04 s, meshes 0.57-0.66 s, weapons 0.23-0.42 s; 963 puts |
| warm, 16 lanes (3 runs) | total 0.68-0.91 s: 963 of 963 hits, 0 puts, 0 misses; meshes 0.04-0.13 s (53.6 MiB read, SHA-256 checked, decoded), weapons 0.03-0.05 s |
| cold / warm, 4 lanes | 1.65-2.07 s / 0.95-1.32 s |
| peak working set | 182-190 MiB cold, 174-181 MiB warm (target §7.9: converter peak RSS <= 0.5 GiB) |
| cache size | 54.2 MiB in 963 entries (53.6 MiB of mesh blobs, 0.4 MiB of WeaponDefs) |

* Caspian CQL (MP_007 ConquestLarge0): 251 MeshSets, 58.2 MiB, cold 0.70 s, warm 0.29 s with 595 of 595 hits;
  peak 217 MiB.
* XP3_Alborz CQL into the same root: 19 of 146 meshes were already there (same bytes as Caspian's). 7 of 172
  weapons were hits; the other 165 differ because XP3 ships its own copies of the weapon partitions. Warm: 490 of
  490 hits.
* A second run over a prepared root writes no entry. The journal still gains its touch records (LRU recency) and a
  close record.
* Plan §7.9's load targets (client cold <= 15 s, warm <= 5 s) cover a whole level load inside Godot, which does not
  exist yet. The prepare stage measured here is 1.3-2.1 s cold and 0.7-1.3 s warm on Metro.

The viewmodel demo, windowed (RX 9070 XT, D3D12, fresh scratch root, then the same root again):

* Cold: load 1.34 s, meshes 39 ms (arms converted 25.1 ms, M9 14.1 ms, including the flushed puts).
* Warm: load 1.29 s, meshes 2.1 ms (hits in 1.40 ms and 0.65 ms).
* Uncached conversion takes 3.3 ms and 1.2 ms. The Ant banks (1.0 s) dominate the load either way.
* The cold and warm screenshots at clip time 1.2 s are pixel-identical below the status label.

**Tests.**

* `prepare.unit`, no data:
  * mesh and WeaponDef blobs round-trip bit for bit (NaN and -0 included); every proper prefix is refused, and so are
    hostile counts (2^60 LODs or surfaces, 4 G vertices), a bad magic or version, unknown mask bits, an index past
    the vertices and a negative bone;
  * a key changes with the converter version, a changed or reordered source SHA-1, the LODs, the filter and the lane
    binds; a weapon key changes when a dependency is patched or an absent one appears;
  * 48 items prepared twice give 48 hits and 0 puts;
  * a flipped payload byte on disk and a blob that passes the SHA-256 but not the decoder are both dropped and
    reconverted, and the next run is all hits;
  * a root under the protected root is refused and the items convert uncached.
* `prepare`, install; 77 = skipped without one:
  * the arms, `m9_1p_mesh` and the 5 LODs of `m9_3p_mesh` go into a scratch root: converted, then hits, both equal
    to a fresh conversion; a damaged M9 entry is dropped, reconverted, then a hit;
  * the skinned viewmodel meshes through `load_viewmodel`: cold and warm equal the uncached load;
  * all 172 MP_Subway WeaponDefs round-trip their blob, and `u_m9` and `u_m16_burst` give the same block cold and
    warm;
  * the install snapshot is unchanged.
  * About 6 s.
* `godot.m9_reload` runs with the default root.
* `prepare.unit` and `prepare` also pass under the `asan` preset.

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

## Viewmodel (M9 reload)

The M0 exit demo (plan §6 M0): BF3's M9 empty reload plays in Godot on BF3's 1P arms and M9 meshes, read from the
install by C++ at runtime, with `fov_override`. Three pieces, two of them Godot-free:

* **`bf3_anim`** (`src/bf3_anim`, plan §2.2): `Skeleton` from the Ant rig (`skeleton_from_rig`: the 181 joints of
  `Venice1pAntSke01`, rest = rig defaults) or from an EBX `SkeletonAsset` (`read_ebx_skeleton`: BoneNames, Hierarchy,
  LocalPose, ModelPose); `bind_clip_tracks` maps a bound clip's channels to bones by name; `sample_pose` (rest +
  `sample_clip`), `model_pose` (local -> model space), `pose_diff`; `SkinBinds` maps a skinned LOD's bone slots
  (EBX skeleton indices) to the posed skeleton by name, with inverse bind = inverse(EBX `ModelPose`), in one list
  that several meshes share; `skin_matrices`; `load_ant_library` / `load_clip` (a package's clip, decoded and bound).
* **`bf3_render`** (`src/bf3_render`): what the Godot bridge uploads, prepared on loader threads.
  * `prepare_mesh` gives the ArrayMesh layout: positions and normals 1:1, tangent + binormal sign as Godot's
    `ARRAY_TANGENT`, UVs as stored, colours, 4 bones/weights per vertex as skin bind indices. Every triangle is
    emitted as (i0, i2, i1) because Godot's front faces are clockwise. One-bone subsets such as the M9's store
    all-zero weights and get weight 1.
  * `prepare_texture` keeps the stored BC1/BC2/BC3/BC4/BC5 blocks with all mips. Godot dev7 wants the chain down
    to 1x1 (12 levels for 2048²), which BF3 stores; a shorter chain is completed. Normal maps are handed out as X/Y
    in R/G because Godot's material reads only `.xy`: NormalDXN/DXN and NormalDXT1 as stored, NormalDXT5 (X in
    alpha, Y in green) transcoded to BC5.
  * `mesh_materials` reads a MeshAsset's `Materials`, their shader graph and vector parameters, and the textures of
    the level's `MeshVariationDatabase` entry for the mesh and variation (a minimal port of IW4L `level_bf3`
    materials.rs).
  * `load_viewmodel` puts it together from `Win32/Levels/MP_Subway/MP_Subway`: the 1P arms of the US assault
    appearance (`persistence/unlocks/soldiers/visual/mp/us/upperbody/mp_us_assault_upperbody01` ->
    `arms1p_bareglove03_mesh`, variation `arms1p_bareglove03_afro`, skeleton `venice1pske01`),
    `weapons/m9/m9_1p_mesh` (skeleton `weaponske01`), their materials and Diffuse/Normal textures, the `m9`
    package's `ReloadEmpty Anim`, and the M9 WeaponDef (`weapons/m9/u_m9`: `RenderFov` 55, empty reload 1.60 s).
* **`bf3_godot`**: `render_bridge.*` is generic: `make_texture` (`Image::create_from_data` with the prepared chain),
  `make_array_mesh`, `make_skeleton`, `make_skin`, `make_standard_material` (Diffuse -> albedo, Normal -> normal
  map, `DiffuseDarkening` tints, `SmoothnessRegular` -> roughness; viewmodel flags `use_fov_override` with the
  weapon's `RenderFov` and `use_z_clip_scale` 0.25). `Bf3ViewmodelDemo` (`viewmodel_demo.*`, scene
  `scenes/m9_reload.tscn`) loads on a `std::thread` behind a progress label. The main thread then builds one
  `Skeleton3D` from the rig, one `Skin` (74 binds: 69 arms + 5 M9) shared by both `MeshInstance3D`s, the materials,
  and a camera on `CameraJoint` turned 180 deg about Y (Frostbite looks down +Z). It plays the clip at 30 fps in a
  loop (65 frames, 2.167 s) and sets only the 96 rotation and 39 translation bones the clip drives. User args:
  `--quit-after`, `--screenshot`, `--shot-times=<clip seconds,...>` (holds each pose 4 frames and saves
  `<stem>_t<time>.png`), `--clip-speed`, `--m9-reload-check`, `--bf3-install`.

**Check** (`-- --m9-reload-check`; ctest `godot.m9_reload` runs it when `OPENBF3_GODOT` names the console exe and
reports *Skipped* otherwise). Measured 2026-10-15 with `OPENBF3_ANT_ORACLE` set:

| check | result |
|---|---|
| Skeleton3D global poses vs `bf3_anim` model space, 9 frames x 181 bones | max 4e-5 deg, 0.0004 mm |
| Skeleton3D vs the Ant probe's `m9_ReloadEmpty_Anim.json` (an independent decode), 57 keys x 181 bones | max 0.0039 deg, 0.056 mm (the JSON has 5 decimals) |
| Godot bone pose x `Skin` bind vs `bf3_anim::skin_matrices`, 74 binds | max 4.9e-7 |
| grip and camera | RightHand 7.3 cm from Wep_Root; Wep_Root at camera (0.07, -0.09, -0.34) m; skinned M9 vertices at camera z -0.58..-0.30 m; right elbow at camera x +0.28, left -0.02 (not mirrored) |
| motion | Wep_Mag up to 27.2 cm from the gun (frame 16), seated again at frame 65 (0.00 mm); the slide moves 36.1 mm from frame 0 (locked back) to 65 |
| materials | 5 of 5 drawn surfaces with fov_override 55 (from `RenderFov`) and z_clip_scale; 5 albedo textures, 4 normal maps (the M9's variation entry lists only Diffuse and Specular) |
| install | 256 index files: sizes and mtimes equal |

Windowed (RX 9070 XT, 1152x648, D3D12): load 1.3-1.7 s (Ant banks 1.0-1.3 s, EBX 0.12-0.16 s, meshes 4-6 ms, eight
2048² textures 20 ms, warm file cache), then 0.03-0.07 s upload. Frame time with vsync 5.56 ms (the 180 Hz display);
with `--disable-vsync` mean 0.55 ms, p99 1.39 ms; sampling, bone upload and camera take 14-23 µs per frame.
Screenshots at clip times 0.1 s (magazine sliding out, slide locked back), 1.2 s (left hand seating the new
magazine) and 1.3 s (slide released, two-hand grip) show textured gloves, skin and pistol, no inside-out faces, the
arms drawn over world props, and the same motion as the Ant probe's renders.

**Tests.**
* `anim.unit` (no data): quaternion and matrix algebra, model pose, clip channels by name, skin binds.
* `anim` (install; *Skipped* without one):
  * the rig skeleton and its 135 bound channels;
  * Undeploy frame 0 vs IdlePose in model space: 0.126 deg / 0.64 mm (probe, local: 0.07 deg / 0.4 mm);
  * rig rest vs `venice1pske01` LocalPose: 168 / 167 of 179 bones (probe: 168 / 167);
  * the arms (69 bones) and the M9 (5: Wep_Root, Wep_Trigger, Wep_Slide, Wep_Mag, Wep_Physic1 = hash `0xa6d64831`)
    bind by name. At the rig rest pose all 69 arm skin matrices are the identity within 2.3e-6, so the rig and
    `venice1pske01` share model space;
  * the reload keeps the hand on the grip and closes its loop (0.30 deg, 1.9 mm without the slide);
  * with `OPENBF3_ANT_ORACLE`, model space equals the probe JSON within 0.004 deg / 0.06 mm.
* `render.unit` (no data): Godot mip chain lengths, BC4 encode/decode, the BC3 normal -> BC5 transcode.
* `render` (install): the whole viewmodel loads (counts, formats, every vertex skinned to a valid bind, every chain
  of Godot's length), plus the NormalDXT5RGA census below.

**NormalDXT5RGA** (open in the handoff). MP_Subway has two such textures, both particle/UI smoke normals
(`emittertexturenormal_win32`, `smoke_normal_02`); no mesh material there uses the format. Over texels with
A >= 0.5, G matches the reconstructed Z: mean |G - (z+1)/2| is 0.015 for X,Y = R,B, 0.025 for B,A and 0.144 for R,A.
A behaves like coverage and B is far from centred (mean 0.13). The layout is not settled, so such textures are
uploaded as stored BC3 and not reinterpreted. MP_Subway has no NormalDXT5 texture: the arms use NormalDXN, the
sleeves NormalDXT1.

**Not done yet.**
* Roll/twist joints driven by rig ops are not computed (probe §8): the forearm skin twists slightly at the left
  wrist in the slide-release frames.
* The specular, smoothness/fresnel, AO, camo-mask and detail textures are not used: StandardMaterial3D gets
  albedo, a normal map and a roughness from `SmoothnessRegular`. Reading `DiffuseDarkening` as an albedo
  multiplier is inferred.
* The normal maps use the data's own tangent frame (Godot's binormal = cross(normal, tangent) x sign, as in
  Frostbite); this was judged visually only.

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
  unresolved slots included, a shared array once per scan), each connection candidate a resolution examines, one per
  triangle the census examines, and 1 + length/16 for every data string or name it copies, keys, compares, parses or
  prints (`charged_str`, `charged_enum_name`, `charge_text`; nothing else in `src/level` reads string values), so text
  work and kept copies are at most 16 bytes per unit whatever one string's length or how many slots name it.
  `EbxPartition` indexes its string table's NULs at parse, so decoding a string is O(log n), not a scan, and it
  flattens each type's fields along its base chain into one table sorted by name hash, so a field lookup (`get`,
  `follow`, a transform's 16 row reads, the resolver's `data_field`) and an enumerator name are one binary search,
  whatever the type layout: a deep or wide hostile type costs a unit what a shallow one does. The parser rejects types
  with more than 1,024 descriptors along their base chain and tables of more than 4 lookups per descriptor (retail,
  all 501,952 partitions of the 53 non-chunk superbundles: at most 96 and 1.96), and its check pays for each type's
  descriptors before scanning them, so parsing is linear in the partition's size. Once a charge
  fails the budget stays exhausted, every later call returns the budget error, and `World::traverse` checks it again
  after the walk. The comment block at the top of `src/level/world.cpp` is the cost model: every loop reachable from
  the public entry points and what bounds it. Beside the budget: depth limits (48 blueprint levels, 8 layer levels,
  32 connection hops and 2^16 hops per resolution), caps of 256 k placed objects, 256 k scopes, 64 k static model
  groups, 4,096 kept sub-world names and 4,096 level-description modes, 1,024-byte names for lookups, each group res
  parsed once per traversal, and a static model group whose instances need more subparts than its res holds is an
  error. Retail use over all 230 level x mode runs: at most 26.9 k units for load + walk (MP_Subway RushLarge0: 19.0 k
  walker steps, 542 resolve hops at most on any run, no resolution capped) and 412 k for everything `bf3dump level`
  does (load, walk, census, collision, listing with every printed name charged; MP_Subway RushLarge0), 10 times below
  the budget. `core.level.unit` and `core.level` build
  hostile partitions for every shape found so far, and each one ends on the budget in under a second in the dev
  build: a WorldPart listing itself twice, the same with a static model group of 200 k empty members, 10,000 objects
  whose `Excluded` is fed by self-looping connections, a loop through a prefab's interface, K references to one
  N-slot list (`Objects`, inclusion `Settings`, shared `EnabledOptions`, `MeshVariationDatabase` entries, component
  `HealthStates`, `RigidBodies`, level-description `Mode` lists), K start-hidden checks over M connections, and K
  slots naming one long string (a 4 MB `BundleName` on 2^18 sub-world slots and on a self-listing fan-out, 2^16 mode
  names of one 1 MB string, long inclusion criteria and options, a 1 MB `BundleName` on a self-looping resolver hop,
  4 MB collision asset names), deep and wide type layouts (a fan-out through types 27-30 deep and 32 descriptors wide
  runs at the shallow form's rate; layouts past the parser's caps fail to parse) and a 16 MB partition of 255-wide
  overlapping value types, whose check fails in under a second (88 s before).
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

## Soldier, kit, mode and vehicle tables

`bf3_core` reads the gameplay numbers straight from EBX (namespace `bf3::core::gameplay`,
`include/bf3/core/gameplay/`). Every reader takes an `EbxResolver`; a level superbundle is enough because it carries
its own `characters/`, `gameplay/`, `vehicles/`, `settings_win32` and `persistence/scoring` partitions.

| reader | source | holds |
|---|---|---|
| `read_soldier_def` | `characters/soldiers/mpsoldier` (SoldierBlueprint) | health, regen, spawn immunity, man-down/revive, capsule, poses (height, eye, step, transitions), per-state per-pose speeds and direction factors, sprint, jump, swim/ladder/parachute, fall-damage curve, suppression, spotting, the 18 specialization values and the field each one feeds |
| `read_mode_def` | `levels/<id>/<id>` LevelData + its sub-worlds for the mode, `settings_win32`, `description_win32` | teams and their kits, ticket counters, capture points (HQs flagged), Rush life counters and MCOMs (arm, disarm, radius, fuse), kill counters, vehicle spawns, player count, rounds, squad size, the sub-world interface constants |
| `read_kit_def`, `read_team_data`, `read_unlock_progression` | `gameplay/kits/*`, `gameplay/teams/*`, `persistence/ranks/rankparams`, `persistence/unlocks/mpstaticunlocks`, `persistence/awards/awards`, `persistence/scoring/scoring` | the 8 weapon slots of a kit with their unlock lists, which progression table gates each unlock, the starting default per slot, specializations, visual unlocks |
| `read_vehicle_def` | a VehicleBlueprint | mass, gravity modifier, inertia, the four health zones (health, shield, angle, multiplier), disabled / emergency / regeneration / decay values, armour multiplier, seats with weapons and cameras, turrets with limits and world-space lock |
| `read_scoring` | `persistence/scoring/scoring` | score per event SID |

Mode constants are mostly not on the entities: the level's sub-world interface (e.g. CQS `InitialTicketCount
Int32 250`) feeds them through PropertyConnections and prefab ports. `LogicWorld` (a port of IW4L `level_bf3`
connections/world/level, Apache-2.0) walks the included sub-worlds and their prefabs into scopes and resolves a
field's effective value along those chains (depth and step limits, cycle safe). Specialization pairs reach their
target fields through Float/IntHub entities whose port ids are designer hashes; the reader matches each pair to the
target field with the longest common name prefix (`UnderslungBoostL1` -> `UnderslungGrenadeIncrease`).
Every reader runs on one `WorkBudget` (`work_budget.h`, default 1M units): the walk, every property query, connection
set, sub-world inclusion test and array scan of the read is charged before it is done (null and unresolvable array
entries, repeated references to one object, arrays shared by many instances and connection loops all count), so
hostile data ends with "work budget of N units exhausted" within a second instead of running for minutes. The rule
is that total work stays O((budget + input size) x log(input size)): every loop iteration and every recursive call
over the data charges at least one unit before it runs (the property resolver never calls itself at depth 0, where a
call would return uncharged, and charges each connection it follows), and everything one iteration does is O(log n)
or charged by its size. The EBX reader makes that possible: every field lookup, enum decode and string decode is a
binary search in tables built at parse time (see "Level traversal"), and type chains wider than 1,024 descriptors are
rejected, so examining an instance costs one unit whatever its type's layout, an array its raw element count, a
`fields()` pass 1 + descriptors / 8 and a string copy its length / 64. A caller can pass one budget to several reads
(`bf3dump kits` shares one between the mode, the progression tables and every kit). Retail needs at most 26,121
units for a mode (MP_Subway Rush), 32,929 for `kits` with every kit, 3,958 for the soldier and 1,554 for a vehicle
(30x headroom or more); the walk caps `max_walk_steps` / `max_query_steps` stay on top (retail: under 8k and 4k
steps). Indexes keyed by data (connection targets, unlock GUIDs) are sorted or ordered, and
the hash maps of GUIDs, SHA1s and instance keys hash with a per-process random seed (`ids.h` `hash_keyed`), so crafted
keys cannot pile into one bucket and make a charged entry cost O(N). The cost model is written down at the top of
`src/gameplay/logic_world.cpp`; bf3dump prints the units a command used on stderr.

The kit tables have no "default" flag, so a slot's default is its first unlock that none of the four persistence
progression tables grants (rank, licence, award or scoring-bucket unlock) and that is not an ammo/scope variant
(`Extra` set). That gives the starting loadouts: US assault m16a4, m9, medkit, m67 and the stock knife (the razor
knife listed first is a licence unlock); engineer SMAW/RPG-7 and repair tool; support ammo box; recon radio beacon;
assault gadget 1 has no default (every launcher is score-gated). Without those tables the default is unknown.

Inspect with bf3dump (`<sb>` as for the EBX commands; `mode` and `kits` open the level's superbundle):

| command | output |
|---|---|
| `soldier [<sb>]` | the SoldierDef of MP_001's `mpsoldier` (or the given superbundles) |
| `mode <level> [<mode>]` | the mode list, or the ModeDef of one mode (`CQS`, `CQL`, `RL`, `SR`, `TDM`, `SQDM`, `DOM`, `GM`, `TS`, `SCV`, `CTF`, `AS` or the full id) |
| `kits <level> <mode> [--all]` | per team, each kit's slots with the default unlock and weapon (`--all` lists every option with its gate) |
| `vehicle <partition> [<sb>]` | the VehicleDef (default `@all-mp`) |
| `scoring [<sb>]` | the scoring table |

`ctest -R core.gameplay.tables` runs `core.gameplay.tables.unit` (no data) and `core.gameplay.tables` (needs the install, exits 77
without it). Checked on 2026-10-14 against the research values: soldier 100 hp, regen 5/s after 10 s, 2 s spawn
immunity, 13 s man-down; walk 4, sprint 6.5, crouch 2, prone 0.9, swim 3, ladder 1.32, parachute 9 m/s; jump 0.6 m;
fall damage 0 below 13.9 m/s up to 100 at 30 m/s. Modes: MP_001 CQS 250 tickets / AreaValue 51 / 3 flags / 32 players,
CQL 400/34/5/64, Rush 75 attacker lives, MCOM arm 2.5 s, disarm 5 s, fuse 25 s, Squad Rush 20 lives, TDM 100 kills,
SQDM 4 x 50; Metro CQS 350/55; Caspian CQL 300/34. Vehicles: M1A2/T-90 5000 kg, gravity 1.6, 1000 hp per zone, rear
x2.75 below 36 deg, sides x1.8, disabled at 500, M1A2 ring lock 0.0001 and gun 0.85 (T-90 0 and 1, gun -20..4 deg);
helicopters 5000 kg / 500 hp / disabled 170; jets 10000 kg, regen 18/s after 10 s; Humvee 1500 kg / 1.85; buggies
800 / 1.65; RHIB 1000 / 1.25; C-RAM and Pantsir armour multiplier 99999. The tables read the data as it is: values the
round logic changes at run time (which Rush bases are active, scripted ticket bleed) are not evaluated.

## Collision overlay

The M0 exit "Metro collision drawn as a debug overlay" (plan §6 M0): the collision a level starts with, read from
the install by C++ at runtime and drawn in Godot, one mesh per 64 m tile. Three pieces, two of them Godot-free:

* **Collision soup** (`bf3_core`, `include/bf3/core/level/collision_soup.h`; parts ported from IW4L `level_bf3`
  `collision/` mod.rs, havok.rs and material.rs, Apache-2.0). `build_collision_soup` takes the traversal's
  `collision_placements` (each static model group's `HavokPhysicsData` at the group transform, each mesh object's
  physics asset prebaked closest to its scale). It lists objects the level starts hidden but does not build them,
  loads each distinct res once on all cores, and walks each root shape's leaves (Mopp, List, ConvexTranslate,
  ConvexTransform, ExtendedMesh shape subparts = group instances, then triangle subparts) through the
  `res/havok.h` decoders. Pieces of 64 leaves run in parallel, then each placement is hashed in walk order. For
  every triangle it keeps:
  * the world-space corners;
  * the `MaterialFlagsAndIndices` value;
  * a surface class: the `RuntimeMaterialGrid` row voted from its assets' names, else the owner mesh's name, else
    concrete;
  * the rigid-body kind (collision, raycast, character).
  A triangle whose part only a later health state shows is dropped (`destroyed`). Flags mark see-through
  (material bit 27) and invisible blockers (mesh names with `invisible` / `collisionmesh`).
* **Terrain** (`res/terrain.h`, port of the heightfield parts of IW4L `resource_bf3` terrain.rs). The `Terrain` res
  names its `TerrainStreamingTree`. The tree's heightfield quadtree (embedded tiles, streamed lod0 chunks,
  data-less and disabled nodes) becomes a `HeightGrid` with the Rust code's float order. The soup takes the first
  depth whose spacing reaches 2 m (Metro: depth 2, 513² samples, 2 m; Alborz: depth 5, 4097², 2 m). It keeps every
  cell whose centre lies in a tile that holds Havok triangles or touches one, unless a corner is a hole. Each cell
  gives two triangles faced +Y, with surface dirt and flag `kSoupTerrain`. IW4L's near-area cap, flat-block
  merging and paved decals are not ported.
* Every corner is quantised to whole millimetres. Triangles with a non-finite or > 1e7 m coordinate are dropped
  (`nonfinite`), and so are triangles with two equal corners (`degenerate`). The FNV-1a 64 of the nine values,
  the material and the class bytes gives each placement a stream hash; tiles and the soup get a wrapping sum.
  The tile is floor(centroid / 64 m) over X and Z. Bounds: the World's `WorkBudget` pays for the EBX reads, the
  Havok decoders' own budgets bound the shapes, `kMaxSoupTriangles` = 2^25 bounds the soup, and the height grid
  is limited to 4097 samples a side. A res that does not load marks its placement `Missing` and the soup goes on.
* **`bf3_render`** `prepare_collision_meshes` builds one unindexed vertex array per (tile, layer) on the loader
  thread. Corners are emitted (v0, v2, v1) because Godot's front faces are clockwise, with barycentric UVs for the
  wireframe. Each face gets its surface colour, shaded by its facing to a fixed light and varied ±6 % per
  placement. Character bodies are amber, invisible blockers magenta and terrain olive; see-through faces are
  lighter. There are five layers: collision, raycast, character, invisible blockers, terrain.
* **`bf3_godot`** `Bf3CollisionOverlay` (`collision_overlay.*`, scene `scenes/metro_collision.tscn`):
  * A loader `std::thread` runs `load_level_collision` and the preparation while a label shows the stage.
  * The main thread then creates one `RenderingServer` mesh and instance per prepared array, in 12 ms slices per
    frame. They share one unshaded, double-sided `ShaderMaterial`: back faces at 55 %, and a barycentric wireframe
    that fades out over 120 m.
  * A free-fly camera starts at the first `AlternateSpawn` (TDM spawn) at eye height, else above the level centre.
  * HUD: placements, triangles, terrain triangles, tiles and meshes, load + prepare + upload time, fps, position.
  * Controls: WASD, Q/E, Shift ×6, wheel for speed; hold the right mouse button (or click, Esc releases) to look.
    F3 cycles solid + wire, wire, solid. 1–5 toggle the layers, R goes back to the spawn, N to the next spawn,
    T to a top view.
  * User args: `--level`, `--mode` (default `MP_Subway` / `TeamDeathMatch0`), `--camera=x,y,z,yaw,pitch`,
    `--view=spawn|top`, `--wire=0|1|2`, `--layers=<digits>`, `--quit-after`, `--screenshot`,
    `--shots=<png>@x,y,z,yaw,pitch;...`, `--collision-check`, `--bf3-install`.

`bf3dump collision <LEVEL> <MODE>` prints the soup in canonical form: sorted `T` lines per placement and terrain
(state, counts, hash), `K` tiles (count, sum), then the totals and the surface and body histograms; timings go to
stderr. **Parity**: `research/probes/tools/level-oracle collision <LEVEL> <MODE>` prints the same through IW4L's
own crates (`HavokPhysicsData::shapes`, `resource_bf3::triangles`, `level_bf3::collision::Materials`,
`TerrainStreamingTree::height_grid`). Result on 2026-10-15: all 230 level x mode runs of the 29 MP levels are
byte-identical. That covers every placement hash, every tile sum and the 199 terrain grids (depth 2-5); MP_011 and the
four Close Quarters levels have no terrain. No res was missing and no triangle was non-finite. The largest soup is
XP3_Shield RushLarge0 with 4.01 M triangles (2.70 M of them terrain). The most work-budget units any run spent was
209,790 of 4,194,304.

| level x mode | placements | triangles (terrain) | destroyed | degenerate | tiles | sum | time |
|---|---|---|---|---|---|---|---|
| MP_Subway TeamDeathMatch0 | 2,122 (119 res) | 1,114,171 (174,080) | 72,733 | 34 | 170 | `2f7addcdc0b29fac` | 0.54 s |
| XP3_Alborz ConquestLarge0 | 519 (56 res) | 3,992,285 (1,331,200) | 56,015 | 0 | 650 | `b6df96c3401645bf` | 1.70 s |

Metro's bodies: 969,031 collision (174,080 of them terrain), 142,908 raycast, 2,232 character; all 32,400 material
grid pairs resolve.

**Check** (`-- --collision-check`; ctest `godot.collision` runs it when `OPENBF3_GODOT` names the console exe and
skips with 77 otherwise or without an install). It prints a PASS/FAIL table and exits 0 or 1. Rows:
* load: nothing missing or non-finite, the terrain built;
* godot_free.recompute: a second `load_level_collision` gives the same placement hashes, terrain hashes and totals;
* oracle: the level-oracle numbers above;
* meshes: one valid mesh with one surface per (tile, layer) slot, 3 × the slot's triangles as vertices
  (3,342,513 for Metro), the soup's first and last triangle bit for bit as (v0, v2, v1), the shared material;
* tiles: every tile has its meshes and instances;
* camera: starts at spawn 1;
* install.unchanged: sizes and mtimes of the 256 index files.
`core.level.collision.unit` covers synthetic shapes, the word table, hashes, mirrored frames, deep trees and a
synthetic terrain tree, plus 3,000 bit-flipped trees and every truncation. `core.level.collision` checks Metro and
Alborz against the oracle numbers and feeds 32 mutated group res through the soup.

Measured (dev build, 16 threads, RX 9070 XT, D3D12, 1152×648, warm file cache, idle machine):
* `bf3dump collision` totals. Metro TDM: 0.54 s (mount 0.15, traverse 0.03, soup 0.36: decode 0.04, triangles 0.22,
  terrain 0.04, tiles 0.04). Alborz CQL: 1.70 s (soup 1.53: decode 0.20, triangles 0.70, terrain 0.42). XP3_Shield
  TDM-CQ: 1.30 s.
* In Godot, Metro: load 0.57 s, prepare 0.02 s and upload 0.07 s on the main thread for 376 meshes.
* Frame time is pinned at the 180 Hz vsync (5.56 ms) both at the spawn and in the top view. Viewport CPU/GPU: spawn
  0.13 / 0.38 ms (0.29 M primitives, 89 draw calls); top view with everything in sight 0.26 / 0.79 ms (1.12 M
  primitives, 381 draw calls).
* Alborz CQL in the same scene (`--level=XP3_Alborz --mode=ConquestLarge0`): load 1.75 s, upload 0.20 s for 1,168
  meshes, top view 0.72 / 1.58 ms for 3.98 M primitives.
* ASan: `core.level.collision` takes about 11 min (the 32 mutants about 9), so its timeout there is 1,800 s.

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

Tests: `host.session.unit` (no data, about 70 s) and `host.session` (install; skipped without one; about 190 s);
both are `RUN_SERIAL` (wall-clock timing).

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

Tests: `godot.m1` (headless `--m1-check`, about 6 minutes) and `godot.m1_load` (windowed `--m1-load-check` cold
then warm on a fresh root under `%TEMP%`, 8 spawn shots each; exit 77 = skipped without a window). Both need
`OPENBF3_GODOT` and an install, and both are `RUN_SERIAL`.

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
| G2 corrections with quantised 30 Hz ghosts | max (and p99) < 0.02 m per found course at 100 and 150 ms replays | `ctest --preset dev -R "^sim\.vehicle$" --output-on-failure` |
| X same hashes from a second binary | report only: `bf3dump vehicle-replay MP_007 RushLarge0 --hash-out <file>` from the dev and release builds, then a file compare | manual |
| H handling numbers | report only (unverified placeholders) | `ctest --preset dev -R "^sim\.vehicle$"`, or `bf3dump vehicle-replay MP_007 RushLarge0 --handling` |
| S spawns settle | report only: < 1 cm drift over the last 60 settle ticks per Humvee spawner | `ctest --preset dev -R "^sim\.vehicle$"` |
| install unchanged | sizes and mtimes equal | `ctest --preset dev -R "^sim\.vehicle$"` |
| contract / units | pass | `ctest --preset dev -R "^(s2\.contract\|sim\.vehicle\.unit)$"` |

S2 passes only when C, D, G1 (with G1r), G1m, G2, install unchanged and contract / units all pass. Under `ctest
--preset asan`, `sim.vehicle` runs the G2 `OwnHistory` mode only (the ablations are `dev` / `release`);
`sim.vehicle.unit` runs in full under both. `sim.vehicle` prints its steps and seconds per phase; its TIMEOUT is set
from the first measured run (docs/design/s2.md §8.4).

`s2.contract` runs now; `sim.vehicle.unit`, `sim.vehicle` and the `bf3dump vehicledrive` / `vehicle-replay` commands come
with the implementation.
