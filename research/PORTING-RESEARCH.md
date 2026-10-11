# Porting Battlefield 3 multiplayer to Godot 4.8 in C++: research synthesis

## 0. Header

**Date** 2026-10-10. **Engine** Godot 4.8 dev7 (`v4.8.dev7.official.c971f93e7`, single precision); the RC/stable replaces
it later. **Language** C++ only (GDExtension through godot-cpp pinned to the exact build).

**Scope.** Battlefield 3 **multiplayer only** (no campaign or co-op), playable offline and on servers with bots as fill,
with BF3's own infantry weapons and vehicles (land, air, sea) on the real BF3 levels. Level loading and rendering were
researched in the IW4L handoff and are only referenced here.

**The user's principle, applied throughout.** This is a **reimplementation**: our own C++ code reads the original
Battlefield 3 files **as-is** from the user's retail install at runtime. Nothing in the install is ever modified,
patched, repacked, converted in place or replaced. There are no mod bundles and no Frosty-style projects, and the
original `bf3.exe` is never run or hooked. Converted or cached data lives only outside the install, under Godot's
`user://`, keyed by content digest so a patched install re-converts. Venice Unleashed, which runs the original game,
and fun-bots, which runs inside it, are knowledge sources, never the route. §2.1 turns this into an architectural
contract.

**How this was produced.**
* The IW4L handoff (`C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\15-GODOT-HANDOFF\HANDOFF.md` and
  `sections\04-gameplay-sim.md`).
* Eight topic reports (`topics/01`–`08`). Each ends with an **Independent verification** table written by a separate
  verifier who tried to refute every load-bearing claim. Where a verdict is *corrected* or *refuted*, this document
  uses the verifier's version. Unverifiable load-bearing claims become risks (§5).
* Three hands-on probes (`probes/`): an EBX data dump of the whole install, a full Ant animation decode rendered in
  Godot, and a godot-cpp toolchain and engine-API measurement probe.
* My own re-check of 31 Godot API names against `godot-api/extension_api.json` (python, 2026-10-10). All exist except
  `RenderingServer.instance_set_interpolated` and `PhysicsServer3D.space_step`, which are absent as the topics state.
  No Godot class name contains "Jolt" [verified].

**Evidence tags** (carried over from the source; the source is linked per section): **[verified]** = checked in data,
code, the API dump or a primary source by the cited report, probe or verifier; **[report claim]** = stated by a source
nobody re-checked; **[inferred]** = a reading or design choice. Frostbite type and field names are spelled as in the
EBX descriptors (PascalCase); the VU-Docs schema writes the same fields lowerCamel.

**Reading order.** §1 (one page) → §2.1 (the read-as-is contract) → §6 (milestones) → §5 (risks) → the §3 subsystem
you are building, then its topic report → §7 (oracles) while testing.

---

## 1. The answer on one page

**Architecture in one paragraph.** A Godot-independent C++ data library (`bf3_core`) mounts the retail install
read-only (base + `Update\Patch` + `Update\Xpack1–5`), parses toc/sb/cas, EBX and resources through their own
descriptors, and yields engine-free data: level scenes, collision, gameplay constants, weapon/soldier/vehicle tables
and Ant animation clips. Godot-independent libraries build the game on top of it: `bf3_anim` (pose evaluation, hit
capsules), `bf3_sim` (an authoritative fixed-tick simulation on its **own pinned Jolt** with soldiers, weapons,
vehicles, destruction, modes, Detour navigation and bots), `bf3_net` (own snapshot protocol over upstream ENet) and
`bf3_host` (server and client sessions). The authoritative server runs at 60 Hz on its own `std::thread` (listen
server) or in the same binary under `--headless` (dedicated). The local player is an ordinary client over an
in-process loopback. One GDExtension (`bf3_godot`) is the only code that touches Godot. It converts data into
RenderingServer instances, skeletons, materials, audio and HUD, and caches derived data under
`user://bf3cache/<digest>`. It reads input and presents interpolated, predicted state. Godot's physics, navigation and
high-level multiplayer stay out of the gameplay path. Clients prove they read identical data by exchanging digests
built from the SHA1s already in each install's manifests; the network never carries BF3 content.

```
   C:\Program Files\EA Games\Battlefield 3   (READ-ONLY: never written, patched, repacked or executed)
                 |  open for read only (one InstallReader); no write handle exists anywhere in the code
 ================|===================================================== read-as-is boundary ==========
                 v
  bf3_core  (no Godot)  layout.toc/cas.cat/sb -> bundles -> EBX/res/chunks -> scene, collision, gameplay tables,
                        weapon/soldier/vehicle defs, material grid, Ant clips; source digests from manifest SHA1s
     |              |                        |                                  |
     v              v                        v                                  v
  bf3_anim       bf3_sim (no Godot)        bf3_net (no Godot)                user://bf3cache/<digest>/...
  poses,         own Jolt worlds:          upstream ENet 1.3.18,             (outside the install; GSTF textures,
  capsules       Server/Prediction/        ghosts, priority, acked           mesh blobs, Jolt shape streams,
     |           Cosmetic (shared static   baselines, handshake digests      Detour tiles, decoded clips)
     |           shapes); soldier motor,         |                                  ^
     |           weapons, vehicles, rules,       |                                  |
     |           destruction, Detour, bots       |                                  |
     +-------------+-------------+---------------+                                  |
                   v             v                                                  |
            bf3_host: ServerHost (std::thread, 60 Hz sim / 30 Hz snapshots, job pool)  |
                      ClientSession (prediction, reconciliation, interpolation)        |
                   |  loopback (listen) or ENet (remote)                               |
 ==================|=================================================== Godot boundary ============|====
                   v                                                                               |
  bf3_godot (GDExtension, main thread + WorkerThreadPool): RenderingServer instances, Skeleton3D/RS skeletons,
  material-family shaders, StreamedTexture2D (GSTF), AudioStreamPlayer3D, Camera3D, HUD, input -> Command,
  DedicatedMainLoop (--headless, no render resources), shot harness            ---- writes only to ----+
```

**Key decisions** (details and evidence in §4).

| # | decision | choice |
|---|---|---|
| D1 | BF3 data access | Own C++ readers open the install read-only at runtime; every derived artefact goes under `user://bf3cache/`, keyed by CAS SHA1 plus converter, engine and Jolt versions; no bf3.exe, VU, mods or repacks; the network carries digests, never content |
| D2 | Godot integration | GDExtension only (no engine module, no custom engine build), godot-cpp master ≥ `b051e694` with the exe's own API **and** interface JSON; single precision; rebuild on every dev→RC→stable bump |
| D3 | Code layout | Godot-free `bf3_core`, `bf3_anim`, `bf3_sim`, `bf3_net`, `bf3_host`; one `bf3_godot` extension; doctest for the Godot-free code |
| D4 | Authority and tick | Authoritative server on its own thread, **60 Hz simulation, 30 Hz snapshots** (30 Hz sim as the fallback), every rate written per second; listen server = loopback client |
| D5 | Gameplay physics | **Own pinned Jolt** (`JPH_CROSS_PLATFORM_DETERMINISTIC`) in `bf3_sim`; Server, Prediction and Cosmetic worlds share the immutable static shapes; Godot's PhysicsServer3D stays empty |
| D6 | Soldier controller | Custom fixed-tick motor with POD state over the own Jolt world (not `CharacterBody3D`), driven by BF3's `CharacterPhysicsData` numbers |
| D7 | Weapons and hits | Fully data-driven from EBX (`GunSwayData`, `BulletEntityData`, level `MaterialGridData`); sub-tick firing; seeded RNG; server re-flies every bullet and validates client hit reports against the pose sample the client drew, ≤ 250 ms rewind |
| D8 | Animation | **Route A, decode BF3's own Ant clips**: clean-room C++ GenericData reader + DCT/Raw/Frame codecs; own C++ animation core and state logic; Godot skeletons only as the render sink; Frame `IdlePose`/`ZoomPose` stand in for undecoded VBR clips |
| D9 | Vehicles | One data-driven vehicle from `VehicleEntityData` on `JPH::VehicleConstraint` (wheeled/tracked/motorcycle controllers) plus own rotor, wing and float models; damage from health zones × material grid; driver prediction with Jolt save/restore and ε reconciliation |
| D10 | Networking | Own ghost/snapshot protocol (priority, per-client acked baselines, quantisation, ≤ 1,200 B datagrams) over **upstream ENet 1.3.18** linked into `bf3_net`; not SceneMultiplayer |
| D11 | Bots and navigation | Server-side fake clients emitting the same `Command` as humans; **Recast/Detour** navmeshes per agent class baked from the character-collision set; ROAD areas from BF3's `RoadData`; 2.5-D clearance grid for aircraft; fun-bots ideas only |
| D12 | Game modes | Constants resolved from EBX through the interface → `PropertyConnection` chain; rules (bleed, capture, MCOM, spawn) written in C++; no event-graph VM |
| D13 | Presentation | RenderingServer instances (no node per placement); resident BC textures first, then a GSTF `.stex` streaming cache loaded with `StreamedTexture2D`; 1p weapon via `fov_override`/`z_clip_scale` in the same HDR frame |
| D14 | Legal hygiene | Zero BF3 bytes in repo/releases/CI; no EA marks in the project name; only MIT/BSD/zlib code (MIT for our code); never disassemble `bf3.exe`; never copy Rime, fun-bots or IceBloc code |

**Top risks** (register in §5).
1. **64-player server CPU at 60 Hz.** Soldier motor sweeps, bot perception rays, Jolt steps and per-client encoding
   must fit 16.7 ms on one host thread plus a pool; nothing is measured on our own Jolt yet.
2. **Third-person animation as a system.** The clips decode, but we must replace BF3's Ant graph (5,729
   `ActorControllerAsset`s), compute twist/roll rig ops and fill 146 VBR clips. The cost of 64 soldiers with
   rendering on is unmeasured.
3. **Vehicle and flight feel.** EBX gives masses, curves and coefficients, but not BF3's force laws; tuning needs
   reference recordings.
4. **Prediction and reconciliation with Jolt.** Replay matches the server only within ε (different worlds, quantised
   snapshots). Neither soldier (`CharacterVirtual`) nor vehicle replay has been run.
5. **Mechanics whose semantics the data does not give**: spread distribution, recoil recovery, falloff shape, ticket
   bleed scaling, capture timing, `DamageProtectionThreshold`. A measurement pass in the user's BF3/VU is needed
   (knowledge only).
6. **Navigation from the right collision set.** Metro's navmesh splits where human traces walk through objects. BF3's
   own 0.3 m step makes the split worse on the IW4L soup, so collision must be rebuilt per Havok body type.
7. **Engine churn and legal exposure.** dev7 → RC API changes (`STREAMING_UV` → `STREAMING_LOD`, hash changes, GSTF
   version); trademark or IP letters if branding or content ever leaks.

---

## 2. Architecture

### 2.1 The read-as-is contract (the user's principle as architecture)

| rule | how the code enforces it | source |
|---|---|---|
| The install is opened **read-only**, in place | One `InstallReader` class in `bf3_core` is the only code that resolves install paths. It opens files for reading with shared read access and creates no write handle [inferred design; 08 §3.8 item 2] | [topics/08](topics/08-prior-art-legal.md) §3.8 |
| Nothing is written under the install root | The cache writer canonicalises every target path and refuses any path under the install root. A debug self-test records size and mtime of `layout.toc`, `cas.cat` and the `.sb`/`.toc` files before and after a session [inferred]; the verifiers used the same mtime check (`find -newermt`) to prove read-only runs [verified for the research runs] | [topics/04](topics/04-vehicles.md) verification extra-5 |
| Derived data lives only in `user://bf3cache/` | Key = CAS SHA1 of the source payload(s), which the manifests already carry, + converter version + Godot version/commit + format version (+ Jolt version for shapes). A patched install changes the SHA1, so the data re-converts. A per-file digest memo keyed by path+size+mtime must re-hash when any of the three changes [verified mechanism; inferred design] | [topics/07](topics/07-godot-engine.md) §6.2; [topics/05](topics/05-networking.md) verification extra-7 |
| Same partition name ≠ same bytes | Key caches by content SHA1, never by name: `s_basicassets` exists in four sizes (22,223,108 … 22,481,708 B) across superbundles [verified] | [probes/ebx-data-probe](probes/ebx-data-probe.md) §7, §8.5 |
| No original executable | `bf3.exe` is never started, hooked or disassembled. VU and fun-bots are documentation, and observing the user's own BF3/VU is reference measurement only (EU Directive 2009/24 Art 5(3)) [verified law text; inferred application] | [topics/08](topics/08-prior-art-legal.md) §1.1, §3.5 |
| No content over the wire | Every machine (client, listen host, dedicated server) reads **its own** install. The handshake compares digests (§2.7); a modified or mismatched install is rejected, never "fixed" [inferred design] | [topics/05](topics/05-networking.md) §7.6 |
| Caches are private | Converted textures, meshes and clips are private copies of copyrighted works. They are never uploaded, synced or distributed, and CI artefacts and repo fixtures hold numbers (counts, hashes) only [inferred] | [topics/08](topics/08-prior-art-legal.md) §3.8 items 1–3; [topics/07](topics/07-godot-engine.md) verification "principle" |
| Optional user-supplied inputs stay optional | A user's own fun-bots `.map` traces may be read as a validation oracle; they are never required, cached for redistribution or shipped (no licence) [verified licence state] | [topics/06](topics/06-bots.md) §1.13, §2.9 |

What the install holds [verified, 08 §4.2 + verification K11]: Origin build 1147186; `Update\Patch` manifest
`Authoritative` / `Version 7`; `Update\Xpack1–5` with `RequireVersion` 2/4/5/6/7 (Back to Karkand, Close Quarters,
Armored Kill, Aftermath, End Game); 29 MP levels; English localisation only; 34.0 GiB. The engine must still work with
any subset: list only levels whose packages exist and whose `RequireVersion` the patch satisfies [inferred].

### 2.2 Layers and modules

| library | Godot? | owns | key inputs → outputs | source |
|---|---|---|---|---|
| `bf3_core` | no | install mount (layout, Patch/Xpack layering, cas.cat, superbundles, delta bundles, chunks), EBX through its own descriptors, resources (MeshSet, DxTexture, Havok, terrain, XAS), level traversal, gameplay table readers, Ant GenericData reader + codecs, manifest digests | the install → POD data | handoff §3 (port the Rust crates `asset_transport/bf3`, `superbundle_bf3`, `ebx_bf3`, `resource_bf3`, `level_bf3`, `bf3_scene`) |
| `bf3_anim` | no | skeletons from EBX + Ant rig defaults, clip sampling, blending, two-bone IK, temporal LOD, reduced server pose for hit capsules | clips, signals → poses, capsules | [topics/03](topics/03-animation.md) §6.2; [probes/ant](probes/ant-animation-probe.md) §9 |
| `bf3_sim` | no | own Jolt (static world, soldiers, vehicles, projectiles as queries, destructible pieces), soldier motor, weapons, damage, vehicles, destruction, modes, spawns, scoring, Detour nav, bots, lag-compensation history | commands, tables → world state, events | [topics/02](topics/02-soldier-kits-modes.md), [01](topics/01-weapons.md), [04](topics/04-vehicles.md), [06](topics/06-bots.md) |
| `bf3_net` | no | upstream ENet 1.3.18 wrapper, loopback transport, netsim (BF3's own knobs), bit packing, ghosts, protocol, handshake digests, replays | state → datagrams | [topics/05](topics/05-networking.md) §9 |
| `bf3_host` | no | `ServerHost` (thread, fixed tick, sessions, bot fill, job pool, admin), `ClientSession` (prediction, reconciliation, interpolation buffers, double-buffered presentation state) | — | [topics/05](topics/05-networking.md) §4.3, §9 |
| `bf3_godot` | **yes** (only this) | registration, RS bridge, cache, material families, viewmodel, audio, HUD, input → `Command`, `GameSession` node, `DedicatedMainLoop`, test runner, debug overlays | POD → RIDs, nodes | [topics/07](topics/07-godot-engine.md) §6–§7 |
| tools | no | `bf3dump` CLI, cache inspector, `bf3nav` bake/coverage CLI, `loadbot` headless clients, master/introducer service | — | [topics/05](topics/05-networking.md) §9; [06](topics/06-bots.md) §5 B0 |

Third-party code, all permissive [verified licences in 05 §3.3, 06 §2.7, 08 §3.8]: godot-cpp (MIT), Jolt 5.x (MIT),
ENet 1.3.18 (MIT), Recast/Detour (zlib), doctest, zlib-ng or libdeflate. Optional with MIT notices:
NicknineTheEagle/Frostbite-Scripts. **Never copied**: Rime (no licence), fun-bots (no licence for `ext/` and
`mapfiles/`), IceBloc (GPL-2.0), AssetBankPlugin (GPL-3.0), VU-Docs schema (no licence; documentation only).

### 2.3 Threads

| thread | runs | rules |
|---|---|---|
| Godot main | input → `Command`; client prediction and reconciliation (same `bf3_sim` code in the client's Prediction world); interpolation; pushes `instance_set_transform`, skeleton bones, audio parameters, HUD | Godot API calls only here (plus the documented exceptions); 3D physics interpolation is node-only, so RS-instance entities are interpolated by our code [verified: `instance_set_interpolated` absent] |
| server (`std::thread`, not a WorkerThreadPool task) | 60 Hz: ENet service → commands → bots → sim (own Jolt `Update`) → destruction → rules → lag-comp history → ghost scoping/encoding → send | calls no Godot API [inferred design, 05 §6.4]; joined in `NOTIFICATION_PREDELETE` and in extension deinit before Godot singletons go away |
| server job pool | bot perception and brains, per-client snapshot encoding, Jolt jobs | between steps only; Jolt refuses queries during a step [verified, probe §6.2] |
| Godot WorkerThreadPool | rendering, texture streaming, pose evaluation (`add_group_task`), resource creation | `mesh_create_from_surfaces` and `texture_*_create` run on the calling thread; `mesh_add_surface`, `shader_create`, `material_create` from a worker are queued to the main thread [verified source, 07 verification K11/extra-2] |
| loader threads | `bf3_core` decode, conversion into `user://bf3cache` | never touch the install for writing |

Why not Godot's physics for the server [verified, 05 §6.1, 07 §4.2, with corrections]: `PhysicsServer3D` exposes no
per-space step or save/restore; the engine steps all active spaces from `Main::iteration` after `_physics_process`.
Getting a direct state is main-thread-only. Queries on an already obtained state can run on workers, but only in a
window the main thread opens; Jolt refuses roughly one query per step otherwise, and every query mutates the space
through `flush_pending_objects()`. The decisive reason is the step, not the query ban (05 K13, 07 extra-1).

### 2.4 Data flow

1. **Mount** (`bf3_core`): layout → superbundles → bundle manifests (each `ebx/res/chunks` entry has a SHA1 of its
   stored bytes) → partition index. Measured on this install: full bundle listing 3.3 s; one level superbundle
   (~12.5 k partitions) parses in 2.2 s; all MP/XP + global superbundles (50,486 partitions) in 32 s, 0 failures
   [verified, EBX probe §0].
2. **Resolve** level + mode: sub-worlds included by `SubWorldInclusionSetting` (handoff 04 §1); weapons are read from
   the level's own main bundle, since every level carries a copy (`weapons/m16a4/m16a4` in 47 superbundles) [verified].
   The soldier, kits and teams come from the mode bundles [verified].
3. **Derive** (cached by digest): collision → Jolt shape streams (`Shape::SaveWithChildren`, regenerate when Jolt
   changes); render meshes → Godot surface blobs; textures → resident BC or GSTF `.stex`; Detour tiles; resolved
   WeaponDef/VehicleDef/ModeConstants blobs; decoded clip tables (optional: DCT is cheap enough to sample directly).
4. **Simulate** (`bf3_sim` on the server thread): commands in, state out; bots write commands one tick ahead.
5. **Replicate** (`bf3_net`): ghosts per client by priority within 1,200 B; reliable events on ch1; destruction rows
   in the world ghost.
6. **Present** (`bf3_godot`): predicted own soldier/vehicle, interpolated others (render time = server time − ~75–100
   ms), animation evaluated on workers, pushed to RS on the main thread.

### 2.5 Where Godot is used and where it is bypassed

| need | Godot 4.8 used | bypassed by | why |
|---|---|---|---|
| world drawing | `RenderingServer.instance_create2`, `mesh_create_from_surfaces`, material-family `.gdshader`s, `OccluderInstance3D`/`occluder_set_mesh`, `CompositorEffect` | — | ~1 µs per instance; Forward+ batches identical surfaces automatically (10k instances of one mesh = 6 draw calls) [verified, probe §6.1] |
| textures | `ImageTexture`/`texture_2d_create` (resident), `StreamedTexture2D.load` of GSTF files (streaming), `TextureStreaming` overrides | — | only file-loaded GSTF streams; `texture_configure_streaming` is not bound [verified] |
| 1p weapon | `BaseMaterial3D.fov_override`, `z_clip_scale`; `PROJECTION_MATRIX`/`Z_CLIP_SCALE`/`IN_SHADOW_PASS` in our shaders | — | same HDR frame (handoff decision 2) [verified] |
| skinned soldiers | `Skeleton3D` + one shared `Skin`, or RS `skeleton_*` | `AnimationTree`/`AnimationPlayer` for the crowd | 64 × 213 bones: AnimationTree ~30–35 ms/frame vs own C++ 0.41–0.46 ms [verified, 03 §4.7 + C13] |
| audio | `AudioStreamPlayer3D` (`pitch_scale`, doppler), `AudioStreamSynchronized`/`Interactive` | — | EA-XAS decoded by `bf3_core` (handoff 04 §4.4) |
| input, UI, HUD, cameras | Godot input, `Camera3D`, Control/2D | — | — |
| gameplay physics | — | own Jolt in `bf3_sim` | no per-space step, no save/restore, no Jolt vehicle/character classes exposed [verified] |
| navigation | (optional debug drawing only) | Recast/Detour in `bf3_sim` | NavigationServer3D: 4–9 ms per path query on Metro vs ~0.1–0.17 ms for Detour; no area types; 6 s map rebuild per region change with default edge connections [verified, 06 §2.4 + C7/C8] |
| multiplayer | `UPNP`, optionally `Crypto` for identity keys | SceneMultiplayer, `MultiplayerSynchronizer`, Godot's ENet | full Variant state per synchronizer, no acked baselines or priority, main thread; Godot's ENet socket ignores RCVBUF (39–94.5 % loss on bursts) [verified] |
| ragdolls, debris | — | client Cosmetic Jolt world (same static shapes) | Godot physics would need a second copy of the static level collision (100k-tri body attach 66–82 ms) [verified probe; 05 K13 correction] |
| dedicated server | `--headless` (dummy renderer, dummy audio), `MainLoop` subclass | optionally a standalone `bf3_server` exe | dummy renderer keeps CPU copies of every 2D texture and mesh, so the server creates no render resources [verified, 07 K12] |

### 2.6 Listen and dedicated setups

* **Listen**: the Godot process starts `ServerHost` on a `std::thread`; the local player connects through the
  in-process loopback with the same message format as the network, so every code path is the remote path. Cap
  `threading/worker_pool/max_threads` so Godot's pool and the server pool do not oversubscribe [inferred, 05 §6.2].
* **Dedicated**: the same extension under `godot --headless`; entry is a `DedicatedMainLoop` (a `MainLoop`/`SceneTree`
  subclass registered by the extension). The editor/console binary accepts `--main-loop <class>` when the project has
  a main scene. Export templates built with the default `disable_path_overrides=true` ignore `--main-loop`, so they use
  `application/run/main_loop_type` [verified, 07 K16]. It loads only collision, gameplay EBX, destruction tables and
  nav: no textures, meshes, Enlighten, audio or scatter. Target server load ≤ 2 s from cache [inferred]. The operator
  needs their own BF3 install, read as-is [inferred from the principle].
* **Standalone escape hatch**: `bf3_core + bf3_sim + bf3_net + bf3_host` link into a console executable with no Godot
  [inferred]. Whether that still counts as "in Godot" is the user's call.
* Godot settings: keep `physics/3d/run_on_separate_thread = false`; render thread model "Safe" (Separate is
  experimental); set `application/run/max_fps` to 10–30 on dedicated servers [verified defaults; advice inferred].

### 2.7 Data-identity handshake

```
C→S ch1  Hello      {proto_ver, game_build, digest_scheme, client_pubkey, name}
S→C ch1  Challenge  {nonce, server_pubkey}
C→S ch1  Auth       {sign(nonce)}                                   identity only, per-install keypair in user://
S→C ch1  MatchInfo  {level, mode, sim_hz, snap_div, required_xpacks, source_digest, sim_digest, component digests, round_id, slot}
C→S ch1  Ready      {source_digest, sim_digest, component digests}  or  Mismatch{components}
S→C ch2  FullSync   {destruction rows, scores, flags}, then ghosts on ch0
```

* `source_digest` = hash over the sorted `(bundle name, bundle digest)` of every bundle the level+mode mounts, where a
  bundle digest hashes its manifest entries `(ebx name, sha1)`, `(res name, resType, sha1)`, `(chunk id, sha1)`.
  Nothing is re-hashed and nothing is written [verified that the SHA1 fields exist, handoff 01 §5; scheme inferred].
* Language bundles (`Loc/*`) are excluded so English and German installs play together [inferred, 05 §7.6].
* `sim_digest` = hash of the derived tables the server simulation uses: collision soup, destruction slot table,
  spawns/flags/combat areas, vehicle spawners, soldier/weapon/vehicle/projectile stats, converter version.
* **What it is not** [verified, 05 K15]: a consistency check, not anti-cheat. Manifest SHA1s prove the manifests match
  (patch level, XPacks), not the payload bytes, and a client can echo any digest. Delta-patched bundles (XP2–4
  levels, `Loc/*`) need base+delta reconstruction before their SHA1 table exists. Anti-cheat is server authority (§3.5).

### 2.8 Build and pinning (from the toolchain probe)

* godot-cpp has been versioned independently since v10 (10.0.0-stable 2026-09-15) and bundles API JSONs only up to
  4.7. For 4.8, pass **both** `GODOTCPP_CUSTOM_API_FILE` (the exe's `--dump-extension-api`) **and**
  `GODOTCPP_GDEXTENSION_DIR` holding the exe's `--dump-gdextension-interface-json` output. The custom API option alone
  keeps godot-cpp's bundled interface JSON [verified, probe §2, §8]. Use master ≥ `b051e694`, which includes the
  hot-reload crash fix `79c37b21`.
* Measured: full `template_debug` build 139.7 s (1,128 Ninja steps); `template_release` 127.6 s; incremental 12.5 s;
  MSVC 19.51, static CRT `/MT`, exceptions off [verified]. Every static library linked in (Jolt, ENet, Recast, zlib)
  must use the same CRT and be exception-free [inferred].
* Gotchas [verified]: object paths ≥ 261 chars fail with C1083 (use a short build root); a project never opened by
  the editor has no `.godot/extension_list.cfg` (run `--import` once, also in CI); a dev7-built DLL loads silently
  into any 4.8.x because only major.minor.patch is compared, and a changed method hash fails at first call. So:
  re-dump, `--validate-extension-api`, rebuild on every engine bump, and grep shaders for `STREAMING_UV` (→
  `STREAMING_LOD` in the RC, GH-123662).
* 4.8 timeline [verified, 07 K1]: dev7 = feature freeze (2026-09-29/10-01); no beta or RC on 2026-10-10. RC around
  November and stable late November–December 2026 are cadence estimates [inferred].

---

## 3. Subsystem plans

### 3.1 Soldier and movement ([topics/02](topics/02-soldier-kits-modes.md) §1, §4.2; [probes/ebx](probes/ebx-data-probe.md) §4)

**What BF3 does** [verified unless tagged]. One `SoldierBlueprint` (`characters/soldiers/mpsoldier`) serves US and RU;
kits and visuals differ. `SoldierEntityData`: `MaxHealth 100`, `LowHealthThreshold 30`, `UsePrediction true`.
`VeniceSoldierHealthModuleData`: `RegenerationDelay 10`, `RegenerationRate 5` (hp/s [inferred]),
`ImmortalTimeAfterSpawn 2` (cancelled by Jump/Throttle/Strafe/Fire/Melee/Grenade/Brake/Zoom inputs),
`ManDownStateTime 13`, `PostReviveHealth 20`, `TimeForCorpse 15`, `InteractiveManDown false`.
`CharacterPhysicsData`: `Mass 100`, `PhysicalRadius 0.35`, `MaxAscendAngle 50`, `WaterDepthLimit 1.4`,
`JumpPenaltyTime 1.0`/`Factor 0.3`, `RadiusToPredictCollisionOnCharacters 0.0`; unlimited sprint
(`SprintPowerDecreasePerSecond 0`).

| pose | `Height` | eye | transitions | OnGround speed | other states |
|---|---|---|---|---|---|
| stand | 1.80 | 1.60 | →crouch 0.3 s, →prone 0.7 s | 4.0 m/s, back/strafe ×0.75, `SprintMultiplier 1.625` (6.5 m/s [inferred]) | Jumping `JumpHeight 0.6`; InAir 6.0, `FreeFallVelocity 16`; Falling 15; Swimming 3.0; Climbing 1.32; Parachute 9.0, `TerminalVelocity 5` |
| crouch | 1.15 | 1.00 | →stand 0.3, →prone 0.5 | 2.0, ×0.8 | sprint ×0 (sprint input stands up [inferred]) |
| prone | 0.30 | 0.30 | →stand 0.7, →crouch 1.05 | 0.9, back ×0.5, strafe ×0.7 | prone footprint from `CollisionBoxMin/MaxExpand` (0.6 × 2.4 m box [inferred]) |

`StepHeight 0.3`; `ThrottleModifierCurve` (0,0)(0.1,0)(1,1). Fall damage (`CollisionData`, vertical and horizontal):
0 to 13.9 m/s, 20 at 14, 40 at 22, 100 at 30 m/s, i.e. no damage below ≈ 9.9 m and death at ≈ 46 m of drop
[inferred at g 9.81]. Vault, vault-up and dive are **authored root-motion curves** in `SpecialMovesComponentData`
(`MotionPoints`/`GravityPoints`; gravity onset 0.3–0.5 s per move), so they work without Ant [verified, 02 C16].
Suppression: `SuppressionSphereRadius 1.5`, decay 0.5 after `FallOffDelay 2`, thresholds 0.3/0.4/0.15,
`SuppressionAbortsHealthRegeneration true`. Spotting: 4° cone, 1,200 m, active 7 s, passive 1.5 s, 3 spots per 3 s.

**Godot 4.8 realisation.** None for gameplay. `CharacterBody3D.move_and_slide()` keeps floor state with no setter
(`was_on_floor` drives snapping), so a rollback cannot restore it; `body_test_motion` costs 24–30 µs per call in
Godot's Jolt for a long sweep, and `move_and_slide` itself calls it [verified, 02 C15]. The motor therefore runs on
our own Jolt.

**C++ design.** `SoldierMotor` in `bf3_sim`: `state' = step(state, command, world, dt)` with all state in a POD
struct (pose and transition timer, movement state, velocity, ground info, jump penalty timer, special-move id and t).
Collide-and-slide over our Jolt `NarrowPhaseQuery` (sweep ≤ 4 iterations, step-up ≤ 0.3 m, walkable ≤ 50°, ground hug
≤ 0.5 m when `GroundHugging`), or Jolt `CharacterVirtual` if its replay holds within ε. Hide either behind
`ICollisionWorld {sweepCapsule, overlapCapsule, ray}` so an own BVH can replace it [inferred, 02 §4.2]. Shapes: stand
capsule r 0.35 h 1.8, crouch h 1.15, prone sphere + box overlap. Other soldiers are not obstacles in prediction
(BF3's `RadiusToPredictCollisionOnCharacters 0`). Hit detection uses the bone capsules (§3.2), never the movement
capsule. Expect a spawn settle: BF3 spawn points sit closer to walls than the hull (handoff 04 §2.2).

**Replicated.** Others: quantised position, yaw/pitch, velocity, pose + transition t, movement state, sprint/zoom/
fire flags, weapon slot and id, health bucket, spotted bits, man-down. Owner only: full-precision state at the last
consumed command, exact health, ammo, suppression, regen timer.

**Test oracles.** Sprint 6.5 m/s; walk 4.0; crouch 2.0; prone 0.9; jump apex 0.6 m; pose transition times as above;
no fall damage at 9.8 m, 20 hp at ~10 m, death at ~46 m; regen 0→100 in 20 s after 10 s; spawn immunity 2 s. Budget:
64 soldiers × ~5 sweeps × 60 Hz ≈ 19k motion queries/s; at the probe's long-sweep cost that is up to ~0.5 core, so
measure short sweeps on our own Jolt (S1 in §6) [inferred, 02 C15].

**Open.** Semantics of `AccelerationGain 0.35`, `DecelerationGain −15`, `SprintGain`; the soldier gravity value;
`FreeFallVelocity` and the Falling velocity; the prone proxy; regen delay 10 s (data) vs 16 s (community); BF3's
respawn delay base behind `vars.playerRespawnTime`.

### 3.2 Weapons, ballistics and damage ([topics/01](topics/01-weapons.md); [probes/ebx](probes/ebx-data-probe.md) §2–§4)

**What BF3 does** [verified unless tagged].
* Chain: `SoldierWeaponUnlockAsset` → `SoldierWeaponBlueprint` → `SoldierWeaponData` → `WeaponFiringData` →
  `FiringFunctionData {Shot ShotConfigData, FireLogic FireLogicData, Ammo AmmoConfigData, OverHeat,
  WeaponDispersion, FireEffects1p/3p, Sound}`. Weapon EBX sits in every level's main bundle; `Win32/Weapons.sb` holds
  only 1p mesh/texture chunks.
* **Spread and recoil live in `GunSwayData`** (`WeaponFiringData.WeaponSway`), per stance × zoom × move state
  (`BaseValue`, `Moving`, and for stand also `Jumping`, `Sprinting`, `Vaulting*`) as `GunSwayDispersionData {MinAngle,
  MaxAngle, IncreasePerShot, DecreasePerSecond}` and `GunSwayRecoilData`. `FireLogicData.Recoil` and
  `FiringFunctionData.Dispersion` are zero on soldier weapons. On shotguns, `WeaponDispersion` is the pellet cone.
  The only first-shot field is `FirstShotRecoilMultiplier`. The same partition also holds an **AI** firing record
  (`ai_western_tracer`, v0 250, TTL 0.5 s); the loader must take the player `WeaponFiringData` [verified, 01 extra-6].
* Variants and attachments are unlocks: `WeaponModifierData {UnlockAsset, Modifiers[]}`, where
  `WeaponFiringDataModifier` replaces the whole firing data and Shot/Projectile/Magazine/Zoom/Misc modifiers patch
  fields; `GunSwayModifierData` multiplies dispersion and recoil per stance×zoom. The burst "M16A4" is
  `u_m16_burst` → `extra` → `u_m16a4_burst` on the M16A3 blueprint; applying the burst modifier to the M16A3
  reproduces Symthic's M16A4 row exactly (ADS 0.1, horizontal 0.075/0.3) [verified, 01 C5]. A modifier value of 0 means
  "unset" for `MaxAngleModifier` and `Lag*Mod` [verified data; inferred rule, 01 extra-7].
* Ballistics: muzzle speed = `ShotConfigData.InitialSpeed.z` (bullet records carry an overridden 350); constant
  gravity, **no drag field**; range = v0 × `TimeToLive`. Gravity −15 on automatic bullets, SKS, pistols and magnums;
  −9.81 on 7.62/5.8 `_semi`, `_bolt`, .338, buckshot, flechette, crossbow; −12 slugs; −7.2 40 mm; −1.5 RPG/SMAW
  [verified, 01 C3/extra-2].
* Damage: `StartDamage` to `DamageFalloffStartDistance`, then to `EndDamage` at `DamageFalloffEndDistance`, flat
  after (linear in between [report claim, Symthic]). The body-part multiplier is the level `MaterialGridData` cell
  (projectile `PhysicsPropertyIndex` row × hit-capsule row, row = `MaterialIndexMap[PhysicsPropertyIndex & 0xFF]`) →
  `MaterialRelationDamageData.DamageProtectionMultiplier` (which field scales damage is [inferred]; this one
  reproduces the community numbers).
* Hit zones: `characters/soldiers/defaultsoldierbonecollision` = 13 `BoneCollisionData` capsules, 9 hi-LOD (head
  r 0.15; upper-spine sphere r 0.25 "Chest"; lower spine r 0.23; arms r 0.08; legs r 0.10–0.11 with
  `DeactivateIfBehindWall`) and 4 lo-LOD (head + 3 on `AITrajectory`).
* Shipped netcode: `AllowClientSideDamageArbitration true`, `MaxAllowedLatency 0.85`, `MaxSimFps 30`,
  `IsSoldierDetailedCollisionEnabled false` (level defaults) [verified, 01 C10].

**Godot 4.8 realisation.** Bullets are queries, never bodies. If Godot physics is ever queried (tools, cosmetics):
`PhysicsDirectSpaceState3D.intersect_ray_into` + reused `PhysicsIntersectRayResult3D` (with `collider_face_index`,
which needs `physics/jolt_physics_3d/queries/enable_ray_cast_face_index`) costs 0.53–0.99 µs/ray vs 1.5–2.2 µs for
`intersect_ray` [verified, probe §6.2]. 1p render FOV: `fov_override` (vertical, KEEP_HEIGHT) and `z_clip_scale`
[verified]. Scope filters → `CompositorEffect` or a full-screen quad.

**C++ design** (01 §5). `WeaponDef resolve(blueprint, activeUnlocks)`: base, then every `WeaponModifierData` of each
active unlock in array order, then the `GunSwayModifierData` multipliers whose gates hold. Fixed-tick fire sim shared
by server, predicting client and bots: integer µs; sub-tick shots (`t = max(nextFire, tickStart)`, next =
t + 60e6/RPM; burst uses `RateOfFireForBurst`; bolt adds `BoltActionDelay + BoltActionTime`); per-weapon state
(spread, recoil pitch/yaw, shotSeq, mag, reload/bolt phases, first-shot flag, heat); counter-based RNG
`splitmix64(matchSeed, player, life, slot, shotSeq, pellet, purpose)`; disc sampling switchable (uniform area vs
uniform radius) until measured. Projectiles step `p += v·dt + ½g·dt²` and test segments against (a) the static world
and vehicles in our Jolt world, where per-subshape user data carries the BF3 material pair, (b) analytic soldier
capsules from a per-tick history ring (13 × 24 B per soldier per tick), (c) destructible pieces. Damage = falloff ×
grid multiplier × `BulletDamageModifier`; penetration only through `Penetrable` pairs without `NeverPenetrate`
[inferred]. Explosions: `BlastDamage` inside `InnerBlastRadius`, falling to 0 at `BlastRadius` (linear assumed),
occlusion rays, `ExplosionCoverDamageModifier`, soldier `ExplosionDamageModifier`; `DudExplosion` before
`TimeToArm*`.

**Hit registration (resolved, see §8 C1).** The client sends a fire command (tick + sub-tick fraction, spread seed
index, view after recoil). Both sides spawn the **same** bullet. When the client's bullet hits a soldier or vehicle it
sends `HitReport{fire_seq, bullet, target ghost, rendered sample (tick_a, tick_b, alpha), hit part}`. The server
flies its own bullet through its world and tests that target against the pose reconstructed at exactly that sample.
It accepts within tolerance if the sample is ≤ 250 ms old. Targets without a report are tested at the shooter's
render time. Bots fire at the present with no rewind. A shot fired later than the shooter's server-side death time +
1 tick is discarded (BF4's fix). Damage, death and armour stay server-side; hit markers come back on ch1.
No-spread cheats remain possible because clients must know the RNG to predict tracers (accepted trade-off) [inferred].

**Replicated.** `ShotEvent {shooter, weapon, shotSeq, t, origin, quantised dir}` to remote clients, who play tracers
and effects deterministically. Projectile ghosts for grenades, rockets and shells.

**Test oracles.** §7.1 (roster numbers, Symthic 50/62 match, grid multipliers, ROF, drop, shots to kill).

**Open.** Recoil recovery (`RecoilAmplitudeDecreaseFactor` as rate vs exponential; `ShootingRecoilDecreaseScale`
0.1); spread disc distribution and how fast the minimum rises on state changes; `GunSwayStanceTransition`
application; `DamageProtectionThreshold` and `DamagePenetrationMultiplier`; whether bullets pass through soldiers;
falloff distance basis; `ClientFireRateMultiplier 0.67`; `ReloadThreshold` semantics (0.85 rifles, 0.75 M9: plausibly
the fraction of the clip after which the magazine counts as refilled [inferred, Ant probe §5]); RPG direct hit
`MissileEntityData.Damage 225` vs community "100"; weapon sound-graph environment selection; whether
`RenderFov 55` is vertical.

### 3.3 Animation ([topics/03](topics/03-animation.md); [probes/ant-animation-probe](probes/ant-animation-probe.md))

**What BF3 does** [verified]. Ant animation data sits in EA GenericData banks. `AntPackageAsset.PackagingType`:
Static/Bundle → a res named `lowercase(Name)`; Chunk → the chunk `StreamingGuid` (88 per-weapon chunks, 8.26 MB, length
= `ChunkSize` for all). `s_BasicAssets` (Static, 22.5 MB, 62,766 objects, 261 layouts) holds the graph: 9,003
`ClipControllerAsset`, 5,729 `ActorControllerAsset`, skeletons, rigs and layouts. `b_BasicAssetsMP` (Bundle, 15.9 MB,
in 194 MP bundles) holds the 3p soldier clips. Banks are **self-describing** (`GD.REFL` layouts with names, offsets and
types). The package header is big-endian, but every GD blob on PC is flagged `'l'` (little-endian), which corrects
the handoff's "big-endian archive". Skeletons are plain EBX: `Venice1pSke01` 179 bones, `VeniceAntSke01` 213,
`WeaponSke01` 25, `ParachuteSke01` 42; `IK_Joint_LeftHand`/`RightHand` under `Wep_Root`. Gameplay drives Ant through
`AntRef.AssetId` signals (`AnimatedWeaponBinding`, health, ragdoll, vehicle entry…); `AssetId = 0x15480000 |
(s_BasicAssets object guid index & 0xFFFF)` resolved 206/206 soldier ids [verified, 03 C8]. The soldier has a
**Server**-realm 3p animatable. That is a design input, not BF3 parity: BF3 ships client-side damage arbitration, and
what its server pose is used for is not shown [verified, 03 C9].

**Probe result (the decision procedure already ran).**
* Gate G0 census, passed: 322 packages parse with 0 errors (s_BasicAssets in 1.2–1.4 s, the rest in 11.7 s). MP clips:
  DCT 3,298, Raw 1,490, Frame 659 (95 %), VBR 146, Curve 35 [verified].
* Gate G1 one clip right, passed: one decoder variant among eight passes every test (MSB-first bits, block-0 DC not
  stored, constant `DeltaBase` added to every block's DC, basis 0.25 / 0.5·cos, scale (1 + 0.1·QMS·i)/QMB): bits
  consumed = bits predicted (M9 ReloadFull 68,466/68,466); mean |q|−1 3–6·10⁻⁵; Undeploy frame 0 vs the uncompressed
  IdlePose 0.07°/0.4 mm. Whole install: 3,778/3,778 DCT clips, 0 errors, 1.7 s in C#. An independent Python decoder
  written from the spec agrees [verified, 03 C7]. Rendered in Godot dev7 on BF3's own meshes: M9 1p empty reload and
  fire, 3p rifle run, 3p M16 reload.
* Channel mapping: DOF set = `ClipControllerAsset.Target` → `LayoutHierarchyAsset.LayoutAssets` in order; a
  `DeltaTrajLayoutAsset` occupies 8 DOFs with no slots; `ChannelToDofAsset.StorageType` 1 = u8 per channel, 2 = u16
  big-endian per channel (IceBloc's byte-pair reading is wrong) → 5,592/5,592 clips, 684,459/684,459 channels land on
  a slot of their own kind.
* Rest pose and binding: animate in the **Ant rig's** hierarchy and defaults (`Venice1pAntSke01_New` 181 joints,
  `RIGWep` 215; `RigAsset.Default*Values` "`<Layout>.<Joint>.q/.t`"). EBX `ModelPose` serves only for inverse binds.
  Mesh bone hash = `hashQuick(lowercase(name))` (`h = 5381; h = h·33 ^ c`); subset bone slots hold skeleton bone
  indices directly; 0 unresolved weights on 5 meshes. Godot needs reversed triangle winding.
* Timing: `ClipControllerAsset.FPS` 30, `FPSScale` 0.5, `NumTicks = 2·EndFrame` → `EndFrame/30` s [inferred].
  `KeyTimes` can be sparse, so sample by key times. G1 criterion (d) in 03 was refuted (frames ≠ seconds); compare
  `EndFrame/FPS × ReloadThreshold` with `ReloadTime` instead (M9: 2.17 s × 0.75 = 1.63 ≈ 1.6) [inferred].

**Decision: Route A, a clean-room C++ port** (03 verification extra-8, 08 K16). About 1,000–1,500 C++ lines and 3–5
working days for parse + decode + playback [inferred, probe §9]. Write it from the probe's written spec, not from Rime
(no licence; its DCT file is probably IceBloc-derived) or IceBloc (GPL-2.0). Gaps and their stand-ins:
* VBR (146 MP clips, mostly pistol/launcher zoom in/out/fire): constant channels decode; animated channels do not.
  Stand-in: blend the uncompressed Frame `IdlePose` → `ZoomPose` over the zoom time. Decoding VBR is 2–5 days,
  uncertain.
* Curve (35 MP clips, e.g. `ClimbLadderUpConnect`): not attempted.
* Twist/roll joints come from rig ops (`RigOpListAsset`, `RotateAroundPivotRigOp`, `JointToEffectorRigOpAsset`);
  without them the 3p shoulder pinches in some frames.
* The Ant graph itself is **not interpreted**. Our own small C++ state logic: 1p idle/deploy/fire/reload/zoom/bolt
  driven by the weapon sim's signals; 3p locomotion blend (stance × direction × speed) + upper-body overlays per
  `WeaponAnimBaseSetEnum`; procedural lag/sway springs from `FirstPersonCameraData.WeaponLagSpringEffectData`,
  `CameraRecoilData` and `GunSwayData`, which BF3 layers on top of clips anyway.

**Godot 4.8 realisation** [verified API]. `Skeleton3D` (rest from the rig defaults) with **one shared `Skin` per
soldier** (different skins per part create several RS skeletons), or RS `skeleton_create`/`skeleton_bone_set_transform`
+ `instance_attach_skeleton`. `TwoBoneIK3D`/`ModifierBoneTarget3D` are optional client conveniences; own two-bone IK
(~40 lines) runs on the server too. Viewmodel: `fov_override` from `RenderFov 55` → `ZoomRenderFov` (10–28 by weapon),
`z_clip_scale`; generated viewmodel variants of our ShaderMaterials rewrite `PROJECTION_MATRIX` and write
`Z_CLIP_SCALE`, skipping both in `IN_SHADOW_PASS`. Caveats: SSAO/SSR at low clip scale, translucent sort, culling at
high FOV (set `custom_aabb`); TAA/FSR motion vectors with an overridden projection untested.

**C++ design** (03 §6.2). `bf3_core/gd` (reflection-driven reader: names, not compiled classes) → `bf3_core/ant`
(ClipSet, codecs, channel map) → `bf3_anim` (Pose SoA, sampler, blend, additive, masks, two-bone IK, temporal LOD,
`ClipSource` interface) → `bf3_godot` (`SoldierVisual`, `Viewmodel`). Pose evaluation per soldier in
`WorkerThreadPool` group tasks; bone upload on the main thread, only the bones the skins reference (64 × 213 full
uploads would be ~13.6k calls/frame) or GPU skinning through `mesh_surface_get_*_buffer_rd_rid` [verified API; 07
extra-4]. Temporal LOD tiers come from data: `default/settings_win32` `GameAnimationSettings` First 2 m/0.02 s …
Sixth 150 m/0.12 s, far 208 m [verified values, 03 C17; meaning inferred]. Server: reduced skeleton → 9 hi-LOD
capsules at tick rate, history stores root transform, stance, aim angles and pose phase, and capsules are recomputed on
demand for rewinds [inferred, 05 §5.4].

**Replicated.** Nothing per bone: remote soldiers are animated locally from replicated movement state (speed,
stance, aim pitch/yaw, weapon action, vehicle entry index); the server keeps its own pose history for hits.

**Test oracles.** 3,778/3,778 DCT bit-count matches; 5,592/684,459 mapping; M9 frame 0 vs IdlePose ≤ 0.1°;
float-for-float (≤ 1e-5) match with the probe's JSON outputs (`probes/ant/m9_*_Anim.json`); 64 × 213-bone pose
evaluation ≤ 0.5 ms single-thread [verified bench 0.41–0.46 ms]; main-thread animation + skeleton ≤ 2 ms at 1440p
[target, inferred].

**Open.** VBR bitstream; Curve; rig ops; additive-clip layering and `TrimOffset`; the 8 DeltaTraj DOFs; the meaning of
0x1548; `RagdollResource` layout (build ragdolls from the 9 capsules plus hand-set limits meanwhile); render cost of 64
skinned soldiers with shadows on the RX 9070 XT.

### 3.4 Vehicles per family ([topics/04](topics/04-vehicles.md); handoff 04 §4)

**Physics route (all families).** Own Jolt with `CROSS_PLATFORM_DETERMINISTIC`, `/fp:precise`, pinned commit. Jolt
5.6.0's vehicle sources are compiled into Godot but unreachable from GDExtension; Godot's `VehicleBody3D` is a
`btRaycastVehicle` port with no engine, gearbox, differential or tracks [verified, 04 C1–C4]. One generic vehicle
entity built from `VehicleEntityData`'s depth-first component tree. Component transforms compose, and composite mesh
parts are placed by the accumulated chain. The TV missile, SOFLAM, MAV, EOD bot, T-UGS, radio beacon and M224 are
`VehicleBlueprint`s too, spawned by `VehicleProjectileEntityData` [verified, 04 C10, 02 C7]. Roster: 49 MP blueprints,
2,048 spawn placements over 119 level×mode combinations, none on MP_Subway or XP2 maps. XP4/XP5 spawners live in
`dynamicspawn_*`/`xp5/dynamic_vehiclespawners/*` prefabs, which supersedes the handoff's "not found" [verified, 04 C6].

**Common chassis** [verified]: `BodyMass` (jeeps 1500, buggies 800, tanks/IFV/AA/helis 5000, jets 10000),
`CenterOfMass`, `InertiaModifier` (scales the shape inertia), `GravityModifier` (wheeled 1.85, tanks 1.6, air 1.0 —
apply as the Jolt gravity factor; IW4L dropped it), `MotionDampingData`, `AeroDynamicPhysicsData.BodyDrag`,
`VehicleInputData` inertia filters (inside the deterministic step), `AntiRollBar`, `UsePrediction true` (a generic
`ControllableEntityData` flag, weak evidence [verified, 04 extra-2]).

| family | data that drives it [verified] | model [inferred unless tagged] | first check |
|---|---|---|---|
| wheeled (Humvee, Vodnik, LAV-25, buggies, quad, DPV…) | `CombustionEngineConfigData` rpm→torque curves, `GearboxConfigData` ratios/speeds, `WheelConfigData` (Radius 0.46, Spring 0.4/0.5/0.06, Pacejka-like K terms shared by all ground vehicles, friction max factors, `SteeringType`, `DrivingType`) | `JPH::VehicleConstraint` + `WheeledVehicleController`, one wheel per `wptNormal` wheel; spring factors fitted to IW4L's static sag; `ForwardGearSpeeds` as shift thresholds to tune; subclass the controller if Jolt's tyre model cannot match | Humvee: 68 km/h at 6 s, ~10 m/s² braking (IW4L) [report claim]; every ground spawn settles within 1 cm (catch the VDV fall-through on Caspian) |
| tracked (M1A2, T-90, BMP-2, 2S25, Tunguska, AAV, skid loader) | `TrackWheelComponentData` per road wheel, `TrackComponentData.ScrollFactor`, `TrackedTurnSpeedLimit`, engine/gearbox/`Boost` | `TrackedVehicleController`; IW4L's yaw-rate governor (pivot 65° in 2 s) [report claim] reproduced, then limited by `TrackedTurnSpeedLimit` | T-90 and M1A2 top speeds within a few km/h of each other (IW4L got 38 vs 53 km/h) |
| helicopters (AH-1Z, Mi-28, Venom, Ka-60, AH-6, Z-11W) | `PropellerEngineConfigData` (`LiftForceSpringConstant 40000`, `DampingConstant 20000`, pitch/roll/banking strengths), `RotorParameters` (`CollectiveInputIdle 0.99`, `AngleOfAttack` curve), tail `ForceMagnitudeMultiplier 75`, `RotorComponentData` parts 10/11 | rotor spool; lift = spring-damper on commanded climb rate (0.99 idle explains BF3's slight sink); cyclic torques; horizontal force from tilt × `HorizontalForceModifier`; start from IW4L's tuned controller [report claim] and swap constants one at a time | hover hold, climb rate, speed at 25°/36° nose-down vs reference |
| jets (F/A-18, Su-35, F-35B, A-10, Su-25; spawn-in-air variants; AC-130/C-130) | `JetEngineConfigData` (thrust curve, `Boost.ForwardStrength 1.5` with no depletion), 5 `WingPhysicsData` + 12 flaps, `LiftCoefficient` and `MaxFlapAngleScaleFactor` curves, `LandingGearLogicData` (retract at 251, deploy at 250) | per-wing lift = Lift × coeff(AoA) × v²; flap authority × speed curve; stall beyond 18° (main roll pair) / 24° (pitch flap wing) [verified curves, 04 C12] | turn rate peaks where pitch authority peaks (250–305) and collapses by 320; matches the 310–315 km/h community sweet spot if the axis is km/h [report claim for the community value; units inferred] |
| boats/amphibious (RHIB; AAV, LAV-25, BMP-2, Vodnik) | `BoatFloatPhysicsData` (`Density 0.5`, `FilledDensity 1.2`, axis friction mods, `SubSurfaceSplits`), water-jet `JetEngineConfigData {IsWaterJetEngine, AngleInputYMultiplier}` | buoyancy samples in a `PhysicsStepListener`; water surface = level `WaterEntityData` mesh; jet thrust vector turned by input; wrecks sink | planing speed and turn vs reference; LAV enters and leaves water on Noshahr |
| motorbike (KLR650) | `MotorbikeData`, `DiffGearRatio 18.35` | `MotorcycleController` | — |
| stationary/gadget (TOW, Kornet, C-RAM, Pantsir, M224, SOFLAM, MAV, EOD, T-UGS, beacon, TV missile) | `RemoteEntryComponentData`, `LaserPainterData`, `LockingWeaponData {IsGuided}`, `ArmorMultiplier 99999` on C-RAM/Pantsir | same framework; remote entry for TV missile/SOFLAM/MAV/EOD | TOW follows the crosshair; TV missile lives 7 s |

**Turrets** [verified data; behaviour inferred]: `ChildRotationBodyData` has **no deg/s field**; rate emerges from
input × `AngularMomentumMultiplier`, capped by `AngularMomentumThreshold`, damped by `AngularMomentumDamping`.
`WorldSpaceLockEfficiency`: M1A2 main ring 0.0001 (T-90 0.0), its gun 0.85, HMG rings 1.0. So MBT main turrets are
**not** world-stabilised in yaw, unlike IW4L's every-ring-holds-world-yaw; gunner HMG rings are [verified, 04 C13].

**Combat and damage** [verified data; formula inferred]: muzzle speed = `ShotConfigData.InitialSpeed.z` (MBT cannon
200 m/s; bullet record 350). Per-vehicle `VehicleHealthZoneData` ×4 (MBT 1000, angle multipliers rear ×2.75,
sides ×1.8, front ×1; shields 175 on rear/left/right = reactive armour unlock), `DisabledDamageThreshold`
(MBT 500), decay and regen, `EmergencyRepairHealth` just above the disabled threshold (510/185). Damage multiplier =
level grid cell [projectile *property* row][vehicle material row] → `DamageProtectionMultiplier` (TankShell→MBT 0.9,
Rocket→MBT 2.0, BlastDamage→MBT 0, small arms → every vehicle 0). Proposed: `dmg = base × multiplier × weapon
DamageMultiplier × zoneFactor / ArmorMultiplier`. Shot sanity check: 300 × 0.9 = 270 = 27 % of a frontal MBT zone
[inferred]. Model reactive armour as **absorbing one whole hit per side** (community: AP shells are negated by
reactive armour) until measured, not as a 175-point pool [report claim + inferred, 04 extra-1]. Locks: heat 1.25 s
/ 550–650 m / 7.5–10°; laser-painted 1.25 s; flares 6 decoys, 11 s reload; ECM 4 s jamming in a 50 m cone; smoke 5 s.

**Seats, cameras, HUD, audio.** `EntryOrderNumber` sorts seats (do not assume 0 = driver; the driver is
`SeatType EST_Driver`). Entry radius 1.25–5 m; exit points with impulses; IW4L's enter/exit/seat-cycling rules carry
over [report claim]. Cameras: `StaticCameraData` free-look 120/120/85/40–45 with `LoosePartPhysics` shake springs,
`ChaseCameraData` spring camera, alternate views (FOV 15 zoom; FLIR is a separate FOV-55 view [verified, 04 C14]).
Rendering: one RS instance per rigid part group (wheels, turret rings, rotor states, flaps); CPU skinning cost IW4L
2.4 ms vs 0.13 ms for rigid parts [report claim]; tracks via an RS skeleton + UV scroll (`ScrollFactor`). Audio:
handoff 04 §4.4's hand-tuned mixer (gear bands, rpm targets) from replicated rpm/throttle/slip; the patch graph is
never evaluated.

**Networking** (04 §5, corrected by C15). The driver predicts in a small `PredictionWorld` (static shapes shared, own
vehicle dynamic, others kinematic) with `SaveState`/`RestoreState` of body + `VehicleConstraint` + controller and
replay. **Bit-exact zero-correction driving is plausible only** if the vehicle and the static bodies have the same
BodyIDs in both worlds (`CreateBodyWithID`), restores use full-precision body + constraint + controller state
including warm-start impulses, and no other body is within wheel-probe or hull reach. With quantised snapshots,
reconciliation leaves residual error, so ε must sit above the quantisation error. Passengers ride the interpolated
vehicle; gunners predict their own turret angles; seat changes are server events at an agreed tick; lag compensation
rewinds vehicle hulls too.

**Replicated.** Per vehicle ghost: position (cm), orientation (smallest-three), linear/angular velocity, the
controller state needed for exact restore (per-wheel suspension length and spin, steer, rpm, gear, clutch, rotor rpm,
flap angles, input-filter values) to the driver; a lighter row (pose, velocities, wheel visuals, turret angles, rpm for
audio) to others; health/shield/disabled flags; occupants; weapon heat/ammo/reload to the seat owner only. A parked
vehicle costs nothing after its first send (IW4L) [report claim]; moving ≈ 25–35 B quantised [inferred].

**Test oracles.** §7.4 and the per-family "first check" column above.

**Open.** Units of `ForwardGearSpeeds`, `MaxVelocity`, `VelocityDamageThreshold`, the jet speed axis,
`PowerFadeOutRange`; BF3's tyre, rotor and wing force laws; the turret controller law; the damage-angle function,
`DamageProtectionThreshold`, cells without damage data, splash split across zones; `WeaponInputRouterComponentData`;
AC-130 orbit logic; the 40 mm grid row.

### 3.5 Networking ([topics/05](topics/05-networking.md))

**What BF3 does** [verified values; meanings inferred]. Patch `initfs_Win32` `Scripts/Game.cfg` and level
`settings_win32`: `GameTimeSettings.MaxSimFps 30` (fixed 30 Hz sim, `ClampTicks 2`, `VisualFrameInterpolation true`);
`Server.OutgoingFrequency 10` / `Client.IncomingFrequency 15` (EBX defaults 15/10; which wins is unknown);
`OutgoingRate`/`IncomingRate 128000` (units unknown); `MaxServerFrameSize 2048`; `MaxGhostCount 8192`;
`MaxClientCount 68` ("regular + spectator"; the 64 + 4 split is inferred); `AllowClientSideDamageArbitration true`,
`MaxAllowedLatency 0.85`; `LocalVehicleSimulationEnabled true`; `UsePrediction true` on the soldier, on all 16
surveyed vehicles and on the AH-1Z and M1A2; a built-in network-conditions simulator (latency 0.1–0.2 s, drops, spikes, reorder, duplicate,
corrupt). BF4 (kept separate): 30 Hz normally, beta up to 144 Hz; DICE recommends one instance per machine and ≤ 60 Hz
for 64 players, and lists "spread/recoil not 1:1", "sprint acceleration not 1:1" above 30 Hz [verified PDF].

**Godot 4.8 realisation.** Not SceneMultiplayer: it sends the full Variant state per synchronizer per interval (4-byte
type header per value), has no acked baselines or priority, drops states over 1,350 B, and runs on the main thread
[verified, 05 K8]. Godot's `ENetConnection` is the same protocol but on Godot sockets that ignore RCVBUF/SNDBUF:
unreliable bursts lost 39–94.5 % (~66 KB delivered); `ENetMultiplayerPeer.create_server` passes the channel count
into `in_bandwidth` [verified, probe §6.6, 05 extra-2, 08 K13]. Used from Godot: `UPNP` (port mapping), optionally
`Crypto` (keys).

**C++ design.**
* **Transport**: upstream ENet 1.3.18 in `bf3_net` (256 KB socket buffers), MTU 1392, snapshot datagrams ≤ 1,200 B.
  ch0 unreliable unsequenced (snapshots S→C; commands + hit reports C→S, 3–4 redundant); ch1 reliable (control,
  events, kill feed, score, chat, level change); ch2 reliable bulk (join-time destruction rows). Never cap reliable
  traffic (IW4L's 64-pending limit failed sessions [verified, 05 K16]). GameNetworkingSockets (BSD-3) is the upgrade
  path if encryption + ICE are ever needed.
* **Tick**: 60 Hz sim, 30 Hz snapshots, configurable `{sim_hz 30/60, snapshot_divisor}`, fixed per match; integer
  ticks; the server repeats the last command when one is late. Clock: client runs ahead by RTT/2 + buffer, nudged
  ±5 % from a per-snapshot "ticks early" byte (BF3's `TimeNudgeBias` role).
* **Prediction**: own soldier and own vehicle by replaying commands through the same `bf3_sim` step; adopt
  destruction rows before replay; visual error decay (IW4L: 100 ms, snap under 0.1).
* **Interpolation**: render time = server time − d, d = 2 × snapshot interval + jitter, clamped 50–150 ms; Hermite;
  bounded dead reckoning ≤ 250 ms for far, low-rate ghosts.
* **Ghosts**: stable ids (soldier slots, vehicles by spawner index, projectiles `(number, generation)`, one world
  ghost); per client and ghost: scope, priority accumulator, acked baseline per field group; on loss resend the latest
  state. Priority tiers: ≤ 100 m every snapshot, 100–300 m ~10–15 Hz, 300–800 m ~5 Hz, beyond 1–2 Hz; boosts for the
  aim cone while zoomed, attackers, same vehicle/squad. Relevancy: teammates and vehicles at low rate; enemies within
  ~1.5 km or spotted.
* **Quantisation**: position 1/256 m (x,z) and 1/128 m (y); soldier yaw/pitch 14/12 bits; vehicle orientation
  smallest-three 35 bits; moving soldier delta ≈ 10–14 B; moving vehicle ≈ 25–35 B [inferred].
* **Session**: tiny C++ master (heartbeat list, UDP hole-punch introducer, optional relay); UPnP; per-install keypair;
  no EA/Origin accounts or ownership calls; server authority as anti-cheat; demos from server spectator streams
  (`{proto_ver, source_digest, sim_digest, level, mode, sim_hz, root_seed}`). BF3's kill cam is a camera move to the
  killer [report claim]; no CoD-style archive.

**Budgets** [inferred, arithmetic verified in 05 K14]: Caspian, 64 players, ~15 moving vehicles → ≈ 724 B per snapshot
≈ 21.7 KB/s average; cap 1,200 B × 30 = 36 KB/s payload ≈ 37.2 KB/s on the wire; upstream ≈ 4.7 KB/s per client;
server upstream with 63 remote humans ≈ 10.9 Mbit/s average, 18.1 at the cap. Bots cost no connection or upstream, but
every bot is a ghost in each human's snapshot. Server tick estimate 4–6 ms of 16.7 ms [inferred, 05 §10].

**Test oracles.** N1–N6 exit criteria (05 §11): no rubber-banding at rest under 150 ms/2 % netsim; correct hits at 100
ms RTT on two PCs; mismatched install rejected with a reason; 64 connections ≤ 36 KB/s/client with server tick < 8 ms;
vehicles without visible corrections on flat ground at 100 ms RTT; late joiner sees an RPG hole identically.

**Open.** Which BF3 rate pair is effective and in what units; BF3's interpolation delay; real bandwidth and CPU; the
language-bundle exclusion list; the Linux coexistence of a second Jolt (and Recast) with Godot's; `CharacterVirtual`
replay across corrections.

### 3.6 Bots and navigation ([topics/06](topics/06-bots.md))

**What BF3 provides** [verified]. **No navigation data in MP levels** (`PathfindingBlobInfo` zero, `AISystem` null;
pathfinding partitions only in COOP/SP). `RoadData` ribbons exist: Caspian has 134 (29 ribbons ≥ 7 m wide, 17.1 km;
`RibbonPoints` Left/Right are signed offsets, width = Left − Right) [verified, 06 C12]. SP/co-op AI data rides along
in MP vehicles (`DriverSettings` `LookAheadTime 1.2`, `MaxAcc 8`, `MaxDec 25`; Humvee `WantedSpeed 22`;
`AIWeaponData` with generic template values: weak tuning data [verified, 06 C15]). Spawner `BotBailOutDelay 0.5`.

**What fun-bots shows** [verified code; licence: none for `ext/` and `mapfiles/`, MIT only for `WebUI/` and the
fork's `astar-lib`]. Bots are real players writing `EntryInputActionEnum` levels (about every 0.13 s, staggered) plus
authoritative view angles (yaw slew 550°/s). Navigation is 185 human-recorded trace files (1,159,335 nodes, 9,857
paths, no air paths); coverage of the 230 level×mode rows: 160 direct, 39 by its own fallbacks, 31 missing (incl. 3
tutorial rows) [verified, 06 C5]. Ground bot-vs-bot acquisition needs a connected human (LOS rays run on clients); jet
and AA target picking and shoot-back-on-damage work without one [verified, 06 C3]. Helicopters fly to objective
+110 m and jets to +350 m with PIDs and no terrain avoidance. Ideas to reuse: time-sliced update tiers, attack:defend
4:1 slot weighting, PID flight, recoil read from the weapon state.

**Godot 4.8 realisation.** NavigationServer3D is not in the bot path [verified measurements on Metro, 744k tris]:
query mean 4–9 ms (max 31 ms) vs Detour 0.06–0.17 ms; one polygon per detail triangle (55,319 vs 14,797); default
search cap 4,096 polygons; no area types or tiles; default edge connections rebuild the map in ~6 s after any region
swap (48–96 ms with them off). If used for debug drawing: `append_arrays` (not `set_indices`, which rejects shared
vertices), Recast winding, edge connections off, AABB grown by `border_size` per chunk [verified, probe §5, §6.5].

**C++ design.**
* **Nav**: Recast/Detour vendored into `bf3_sim` (hidden visibility or a namespace to avoid Godot's Recast symbols on
  Linux). 64 m tiles, cs 0.25, ch 0.1 (climb/ch must be whole), BF3 capsule r 0.35, climb 0.3, slope 50. Baked from
  **exactly the set the character collides with** (Collision + Character Havok bodies, PLAYERCLIP, terrain with
  holes; never foliage or scatter), cached as `user://bf3cache/nav/<collision digest>-<params>.bin`. Measured
  [verified]: Metro bake 3.7 s on 1 thread / 0.5–0.7 s on 16, 2.6 MiB; synthetic 2 km terrain 8.3–9.7 s on 16
  threads, 3.1 MiB; busiest tile rebake 110–163 ms, swap 0.3–0.6 ms. One navmesh per vehicle hull class with ROAD areas
  stamped from `RoadData`; off-mesh links for drops, jumps and ladders; dirty-tile rebake on destruction. Aircraft: a
  2.5-D max-height clearance grid (8–16 m cells).
* **Brains** (`bf3_sim/bots`, reads an immutable `ISimView`, writes `Command`s only): team Director (1–2 Hz, objective
  slots by mode), squads (2 Hz), utility brain per bot (5–10 Hz staggered), perception (rays on the job pool between
  steps, 2 rays per candidate, detection time scaled by distance/stance/motion), aim controller (reaction time,
  angular error decaying from `e0` to `e∞` like BF3's `StartAccuracyPercentage → FinalAccuracyPercentage`,
  rate-limited view; lead from `ShotConfigData.InitialSpeed`, drop from `BulletEntityData.Gravity`; compensate a
  skill fraction of the recoil read from the shared weapon state), vehicle drivers (corridor + pure pursuit), heli and
  jet pilots (cascaded PIDs + clearance grid). Difficulty comes from perception, reaction and aim, **never** damage
  multipliers (fun-bots' 0.6). No teleports or seat warps; redeploy only after long stuck, logged.
* **Same `Command` as humans**: one movement/weapon/vehicle path, replication for free, recorded in demos; per-bot RNG
  from match seed + slot.

**Budget** [inferred, corrected in 06 C17]: ≈ 0.3–0.9 ms per 60 Hz tick single-threaded for 64 bots, dominated by
rays (64 × ≤ 6 candidates × 2 rays × 10 Hz ≈ 7,700 rays/s); brain and motor costs unmeasured.

**Test oracles.** B0: Metro CQL ≥ 97 % of the user's fun-bots trace nodes within 1 m of the mesh (opt-in oracle) and
all flags, HQs and spawns in one component; today only 58.9 % + 29.9 % with placeholders, and 43.4 % in the largest
component with the BF3 capsule on the IW4L soup [verified, 06 C11]. B2–B10 criteria (06 §5): bots-only CQL round ends
by tickets with each flag changing hands ≥ 2×; bots-only server has kills (unlike fun-bots without humans); ≥ 3 of 4
jeeps taken within 30 s; 0 terrain crashes in 10 min of bot flight; 64 bots ≤ 2 ms/tick p99.

**Open.** What the solid objects at Metro's split points are (gates, kiosks, Raycast-only shells); ladder entities
in MP data; `RoadData` junction topology and drivability; real Caspian bake time; per-bot brain/motor cost; Linux
symbol clash with Godot's Recast.

### 3.7 Game modes, spawns, kits, scoring ([topics/02](topics/02-soldier-kits-modes.md) §2–§3; handoff 04 §1–§2)

**What BF3 does** [verified unless tagged]. Mode logic is an event/property graph in prefabs; the entities' own fields
are readable constants, and each level's mode sub-world pushes per-level values through `InterfaceDescriptorData` →
`PropertyConnection`s (field ids are djb2-xor hashes of field names; designer-named ports stay hashes). Examples:
MP_001 CQS 250 tickets / `AreaValue 51`, CQL 400/34; Metro CQS 350/55, Rush attackers 100 lives and MCOM arm 2.5 s;
Squad Rush 20 lives, arm 5 s; Palace Domination 150/60; Tank Superiority 200/150; Air Superiority 250/55; CTF 3
captures [inferred] / 1200 s [inferred]; Scavenger runs Domination logic with 5 flags, `AreaValue 40`, 300 tickets for
**both** teams [verified, 02 extra-1/extra-2].
* Conquest `TicketCounterEntityData`: `TicketLossPerMin 15`, `TicketLossStart 100`, `TDTOnRespawned` (a respawn costs
  a ticket; a revive saves it [inferred]), `TicketLossWhenLostAll 999`, `HaltTicketLossOnEqualPointCount true`.
  Capture points: `TimeToGetControl 20`, `TimeToLoseControl 20`, `AddedMultiplierPerPlayer 0.34`,
  `MaxCaptureMultiplier 4`, `SeesawCapturing`, `CaptureRadius 0` → use the `Geometry` polygon (`VolumeVectorShapeData`,
  4–25 points, height 4–30 m). Domination flags 2/8 s; TS 10/5 s; AS 4/4 s with 0.5 per extra player.
* **Bleed rule** [inferred, fits every level read]: a team bleeds `TicketLossPerMin` (1 ticket per 4 s) while the
  enemy's summed `AreaValue` exceeds `TicketLossStart` 100 and the team's own; `AreaValue × flags` is tuned so a
  majority crosses 100 (2 × 51, 3 × 34, 2 × 55, 2 × 60, 1 × 150, 3 × 40). The community says the rate grows with the
  flag lead [report claim]: keep the rule behind one swappable function until measured.
* Rush: MCOM arm hold 2.5/5 s, disarm 5 s, radius 1.5 m, fuse **25 s** (45 s when the interface bool is set), attacker
  lives 75 by default (`SimpleReinforce`), defenders effectively 999; 5 bases default (levels set 4/2). TDM 100 kills;
  SQDM 4 × 50; Gun Master 9 presets (preset 0: 15 weapons × 2 kills, M320 × 1, knife × 1). Round: start at 4 players;
  auto-balance at a difference of 3 when ≥ 8 players; deserters struck after `TimeToReturn 10` s; kill cam 4 s.
* Combat areas: one polygon per team that excludes the enemy HQ; the playable region is the union, welded at 5 cm
  (handoff 04 §2.4). Spawns: spawner (`CharacterSpawnReferenceObjectData`: team, `SpawnProtectionRadius` 10/20/25, 3
  friendly deaths in 30 s, `TryToSpawnOutOfSight`) → `AlternateSpawnEntityData` points; flags own per-team spawners;
  vehicles with `UseAsSpawnPoint`; radio beacon `MobileSpawnComponentData` (2–10 m scatter); squad size 4 in every
  mode; per-mode player counts in `settings_win32` (CQL 64, CQS 32, TDM 32, SQDM 16…) [verified, 02 extra-4].
* Kits: `VeniceSoldierCustomizationAsset` `WeaponTable`/`SpecializationTable`/`VisualTable`; **the `UnlockParts` index
  is the `WeaponSlot`** (0 primary, 1 sidearm, 2 gadget 1 for Engineer/Recon, 4 kit item, 5 gadget 2, 6 grenade, 7
  knife). Specializations are float/int multipliers wired into soldier fields: Sprint 1.1, Ammo 1.5, Explosives 2.0,
  Grenades +1 (+3 underslung), Flak 0.85, Cover 0.5, Suppression 1.5; HealSpeed 0.8 is wired but in no kit.
* Scoring: 71 `ScoringTypeData` (kill 100, headshot +10, revive 100/110, CQ capture 250, MCOM destroyed 500, CTF capture
  500, suppression assist 50, spot assist 10…).

**C++ design.** `ModeConstants` resolved per level×mode by a resolver that walks interface → `PropertyConnection` →
entity field (handoff 04 §2.1 link walker). `GameMode` subclasses (Conquest family incl. Domination/TS/AS/Scavenger;
Rush/Squad Rush; TDM/SQDM; Gun Master; CTF) built from shared parts: `CapturePointSet` (polygon + height volume per
tick), `TicketCounter`, `LifeCounter`, `KillCounter`, `SpawnDirector`, `Scoreboard`, `RoundFlow`, `AutoBalance`. No
progression: every `SelectableUnlocks` entry is available and the loadout is stored in `user://loadout.json`.
Server-side knobs mirror BF3's `vars.*` (tickets %, respawn %, friendly fire, kill cam, 3D spotting).

**Replicated.** Tickets/lives/kills per team, flag owner + progress (i8), MCOM state, active base, CTF carrier, GM
level per player, round phase/timer; kill feed and score events on ch1.

**Test oracles.** §7.3. **Open.** Bleed scaling and `TicketLossStart`/`controlledAreaValue` semantics; capture timing
(do lose + get add for a flip); whether an interface field with no value pushes a default (Bazaar Rush arm time, disarm
override); Gun Master demotion; squad-spawn conditions; `settings_win32` per level (only Metro read); respawn delay base.

### 3.8 HUD, UI, audio (as far as researched)

* HUD data [verified]: weapon `HudData {CrosshairScaleMin 0, CrosshairScaleMax 5, LowAmmoWarning 0.3,
  ReloadPrompt 0.3}` (crosshair presumably follows current spread [inferred]); vehicle `VehicleHudData.MinimapIcon`
  (Jeep … StaticAA classes), per-seat `EntryComponentHudData`, camera `HudData` flags (range meter, velocity vector,
  climb rate, g-force, locking), `WarningSystemComponentData` lock warnings; `SoldierEntityData.LowHealthThreshold 30`.
  IW4L's vehicle HUD layout (health, km/h, heat/reload, altitude, rotor %, top-down outline with turret limits) is a
  good default (handoff 04 §4.5). Minimap: `UILevelDescriptionComponent.MinimapData` (`WorldCenter`, `WorldSize`); the
  square is not the play area; minimap images were never drawn (handoff 04 §2.5).
* UI: BF3's Scaleform/Gameface menus are skipped; our own Godot UI with nominative "requires your copy of Battlefield 3"
  text and no EA art (08 §3.7–§3.8).
* Audio [verified]: EA-XAS v1 ADPCM waves (handoff 02/04). A weapon `SoundPatchAsset` (M16A3: 107 graph nodes, 12
  crossfader groups with distance fades 0–20 … 300–301 m, noise layers × 6 environments, reflections, shell casings,
  handling) is a graph we do not evaluate. Start with distance crossfades + environment noise layers + per-shot
  one-shots; vehicle audio per handoff 04 §4.4 mixer; footsteps and impacts from the grid's sound relations. In Godot:
  `AudioStreamPlayer3D` per layer with `pitch_scale`, doppler on `Camera3D`. Environment selection per shot is not
  determined (01 §4).

### 3.9 Destruction in multiplayer (brief; handoff `sections\04-gameplay-sim.md` §5–§6)

Six mechanisms [verified census]: health-state props, breakables, explosive objects, predestruction (inferred
semantics), collapse chains through the event graph, bangers. Slot = one `PartComponentData` with ≥ 2 states or one
breakable part, numbered in object × component order (identical on every machine). Metro CQL 12,130 slots, Caspian
10,686; destructibles own 21–66 % of collision. In our design:
* **Damage** uses BF3's own `MaterialRelationDamageData` (`InflictsDemolitionDamage`, multipliers per projectile
  property) instead of IW4L's heuristics: BulletDamage → Concrete/Brick/Metal/Wood rows 0, → `DamageClass_1/2`,
  `Passthrough`, `BulletsOnly`, `Light`(124) 1.0 (0.75 penetration on some) [verified, 01 C9/extra-3].
* **Collision**: one Jolt body per destructible with a `MutableCompoundShape` whose sub-shape user data is the piece;
  piece key `(slot+1) << 3 | state`.
* **Network**: sorted `(slot, state)` rows in the world ghost, reliable by ack; late join gets the full set on ch2
  (worst case 13.8 k rows ≈ 28 KB). Clients adopt rows before prediction and derive FX/sound/debris from transitions
  with `seed = hash(slot, state, server_tick)`. Destruction is not rewound for lag compensation [inferred].
* **Prediction world caveat**: bodies added or removed inside a rollback window break `RestoreState`, so the client
  prediction world updates compound sub-shapes only at snapshot boundaries and holds no debris [verified, 04 C5].
* **Nav**: dirty-tile Detour rebake on the job pool, swapped at a tick boundary.
* Collapse rule (`hkdBuildingController`) is not in data; IW4L's flood fill + "rests on" graph is the fallback
  (handoff 04 §5.5).

---

## 4. Decision table

Confidence: H = verified premises and measured; M = verified premises, design inferred; L = rests on report claims or
unmeasured inference.

| # | decision | options considered | recommendation | evidence | conf. | how to validate early |
|---|---|---|---|---|---|---|
| D1 | BF3 data access | VU runtime; Frosty-style repack; extract once to a project folder; **read as-is + user:// cache** | read as-is at runtime, cache under `user://bf3cache/` keyed by CAS SHA1 + versions | user principle; 08 §3.2 (BYOD survivors ship nothing); 07 §6.2; EBX probe (73,371 partitions, 0 failures, read-only) | H | debug self-test: install file sizes/mtimes unchanged after a session; cache invalidates when a digest changes |
| D2 | Godot integration | engine module; custom engine build; **GDExtension** | GDExtension, godot-cpp ≥ `b051e694` + exe-dumped API and interface JSON, single precision | probe: built 140 s, ran headless/D3D12/Vulkan, exit 0; 07 §2.5 (module wins only cacheless streaming and Godot's own Jolt) | H | S0: extension loads in dev7; RC procedure `--validate-extension-api` |
| D3 | Code layout | one big extension; **Godot-free libraries + one extension** | `bf3_core/anim/sim/net/host` + `bf3_godot` | 07 §7; 05 §9; enables doctest, headless tests and a standalone server | M | doctest suite runs without Godot from slice one |
| D4 | Authority and tick | BF3's 30 Hz client arbitration; 20 Hz (IW4L); **60 Hz authoritative** | 60 Hz sim / 30 Hz snapshots, configurable; per-second rates; server on its own thread | BF3 `MaxSimFps 30`; DICE BF4 PDF (≤ 60 Hz for 64 players; 30 Hz tuning artefacts); IW4L 20–26 ms frame spikes with the tick in the frame | M | S1: 64 walkers at 60 Hz p99 ≤ 8 ms; else 30 Hz |
| D5 | Gameplay physics | Godot PhysicsServer3D; engine module into Godot's Jolt; **own Jolt**; tuned `VehicleBody3D` | own pinned Jolt with `CROSS_PLATFORM_DETERMINISTIC`, shared static shapes across Server/Prediction/Cosmetic worlds | API has no space step or save/restore and no Jolt classes; `VehicleBody3D` = btRaycastVehicle; Jolt docs: only shapes are shareable across systems | H (premises) / M (cost) | S0 link test (two Jolts in one process); measure Caspian static-world memory with two systems |
| D6 | Soldier controller | `CharacterBody3D`; **custom motor** (own collide-and-slide or `CharacterVirtual`) | custom POD-state motor over own Jolt behind `ICollisionWorld` | `move_and_slide` hidden floor state (02 C15); BF3 states (vault curves, ladders, swim, parachute) are data | M | M1: replay determinism within ε; S1: sweep cost for 64 soldiers |
| D7 | Weapons and hits | BF3 client arbitration (0.85 s); pure server rewind; **server-validated client hits** | seeded shared fire sim; server re-flies bullets; validates reported render samples ≤ 250 ms | 01 §3, 05 §5.4; BF4 "dying behind cover" posts; IW4L provenance code | M | M2 + M3: two-PC hit-reg test at 100 ms RTT; trade-window test |
| D8 | Animation | **A decode Ant**; B procedural 1p + CC0 3p; C hybrid | Route A clean-room C++ (DCT/Raw/Frame), own graph, Frame poses for VBR | Ant probe: 3,778/3,778 DCT, 5,592 clips mapped, renders; 03 verification extra-8 | H (decode) / L (graph, rig ops) | M0: M9 reload from C++; S3: 64 soldiers within 2 ms |
| D9 | Vehicles | custom raycast vehicle over Godot physics; engine module; **own Jolt `VehicleConstraint`**; `VehicleBody3D` | data-driven vehicle on own Jolt; own rotor/wing/float models; BF3 damage tables | 04 §1.4–1.5, C1–C5, C7–C8 | M | S2: Humvee replay bit-compare; M4 handling vs reference clips |
| D10 | Transport and replication | SceneMultiplayer; Godot ENet; **upstream ENet**; GNS; yojimbo | own ghosts over upstream ENet 1.3.18 | 05 K8 (SceneMultiplayer), probe §6.6 + 05 extra-2 (Godot ENet RCVBUF), 05 K17 (yojimbo ≤ 100 players, GNS heavy deps) | M | M3: loadbots to 64, ≤ 36 KB/s/client |
| D11 | Bots and navigation | NavigationServer3D; **Recast/Detour**; IW4L walk graph; fun-bots traces | Detour per agent class + ROAD areas + clearance grid; bots emit `Command`s | 06 §2.4 measurements, C7–C10; fun-bots unlicensed | H (nav) / M (behaviour) | S1: Metro bake with all flags in one component; B-step criteria |
| D12 | Game modes | event-graph VM; **resolved constants + C++ rules** | interface → PropertyConnection resolver; C++ rule classes | 02 §3.1, C8; handoff 04 §1 | M | M2/M4: resolved constants match §7.3; bleed measured once |
| D13 | Presentation | nodes per placement; MultiMesh only; **RS instances**; resident vs streamed textures | RS instances, MultiMesh for scatter/debris; resident BC first, GSTF streaming later; viewmodel by `fov_override` | probe §6.1 (6 draws for 10k instances), §6.4 (GSTF streams), 07 §1.3 | H | M1 Metro frame time; GSTF cache on Caspian (4,545 images) |
| D14 | Legal hygiene | ship extracted data; port community code; **BYOD engine, own code** | zero BF3 bytes, no EA marks, MIT, no disassembly, no unlicensed/GPL code | 08 §3 (OpenMW/CorsixTH vs re3/Revive); EU 2009/24 Arts 1(2), 5(3), 6, 8; US §1201(f) | M (not legal advice) | before any public repo: name, NOTICE, CI that rejects binary BF3-like files |

---

## 5. Risk register

| # | risk | likelihood | impact | mitigation | early test |
|---|---|---|---|---|---|
| R1 | 64-player server tick over budget at 60 Hz (motor sweeps ≈ 19k/s, bot rays ≈ 7.7k/s, Jolt, encode for 63 clients) | M | H | job pool between steps; staggered bots; priority-limited encode; 30 Hz sim fallback with per-second rates; own BVH behind `ICollisionWorld` if Jolt sweeps are slow | **S1** headless 64 walkers on Metro and Caspian collision, p99 ≤ 8 ms |
| R2 | 3p animation system: replacing the Ant graph, rig-op twist joints, VBR, 64-soldier render cost | M | H | own small state logic; Frame poses for VBR; temporal LOD tiers from data; skin only referenced bones or GPU skinning | **S3** 64 soldiers with decoded clips, windowed, ≤ 2 ms main thread; user judges the shoulder pinch |
| R3 | Vehicle/flight feel diverges from BF3 (force laws not in data) | H | M | start from IW4L's tuned controllers; swap constants one by one; reference clips recorded by the user in BF3/VU (knowledge only) | Humvee 0–60, braking, turning circle vs reference in M4 |
| R4 | Prediction corrections visible (Jolt replay only within ε; `CharacterVirtual` untested) | M | H | full-precision own-state block for the owner; same BodyIDs; ε above quantisation; visual smoothing; prediction world free of debris | **S2** vehicle replay bit-compare; M1 soldier replay at rest under 150 ms/2 % netsim |
| R5 | Mechanic semantics wrong (spread disc, recoil recovery, falloff shape, bleed scaling, capture timing, damage threshold, reactive armour) | H | M | swappable functions behind tables; one measurement pass in BF3/VU (fixed-wall shot harness, timed bleed, MCOM timings) before tuning | measurement pass scheduled before M2 tuning and M4 |
| R6 | Navmesh from the wrong collision set (Metro split; BF3 0.3 m step worse on the IW4L soup) | H | M | rebuild collision per Havok body type (Raycast vs Collision vs Character); fun-bots traces as opt-in oracle; off-mesh links | **S1** B0 criterion: all flags, HQs, spawns in one component |
| R7 | Engine churn dev7→RC/stable (`STREAMING_UV`→`STREAMING_LOD`, 9 changed hashes 4.7→dev7, GSTF format, Jolt version in shape caches) | H | M | pin exe SHA256 + JSON dumps; rebuild per bump; key caches by engine/Jolt/format version; probe-load one GSTF at startup | at RC: `--validate-extension-api`, rebuild, rerun headless suite |
| R8 | Second Jolt / Recast copy clashes with Godot's on Linux (symbol interposition) | L | M | hidden visibility, version script, namespaces | link + run test on Linux in S0 |
| R9 | Data-identity gaps: delta-patched bundles (XP2–4, `Loc/*`) need reconstruction before their SHA1 table exists; language bundle list unknown | M | M | reconstruct base+delta in `bf3_core` (needed anyway to read them); enumerate `Loc/*` from layout | M3 handshake between two installs with different languages (if available) |
| R10 | Texture streaming at scale (4,545 textures on Caspian; per-*material* feedback slot; D3D12 fragmentation untested) | M | M | resident BC first (fits 12–16 GB GPUs, ≈ 1.56 GiB likely BC bytes); GSTF behind a setting; raise `max_ops_per_second` during load | GSTF cache on Caspian in the look track |
| R11 | Legal: trademark/IP letter; accidental redistribution of BF3 bytes or caches | L | H | D14; NOTICE; no assets in repo or CI; comply on branding immediately | before going public |
| R12 | Scope: 29 levels × 16 mode ids, ~80 infantry items, 49 vehicle blueprints | H | M | everything data-driven; MVP on 2–3 levels; breadth only after the loop is proven | milestone exit criteria |
| R13 | Godot crowd/skeleton cost with rendering (skin compute, shadows) unmeasured | M | M | one Skin per soldier; RS skeletons; bone-texture GPU skinning for far LODs | S3 |
| R14 | Pose-history rewind cost and capsule accuracy for lag compensation (server must animate 64 soldiers) | M | M | reduced skeleton for 9 capsules; recompute on demand from root/stance/aim/phase history | M2 hit-reg tests |

---

## 6. Milestone plan (revised M0–M7)

**Rules.** Each milestone ends **playable or visibly verified in the running game**, not only in unit tests (handoff
decision 4). The biggest unknowns are attacked first by **spikes** with exit criteria. Sizes are in **slices**: one
weekly usage window (03 treats one as ≤ 5 agent working days). Slice counts are my estimates [inferred], scaled from
08 §5's developer-week figures with one slice ≈ one of 08's weeks, and with the animation row shrunk after the probe
(08 K16). The look track (handoff M1–M2: Enlighten, sun, terrain splat, scatter, wind) runs as its own slices beside
the gameplay track and is not re-planned here.

What changed vs the handoff's order [inferred]: the server thread, loopback client and prediction exist from the first
walkable milestone (the handoff had networking at M6); the animation route is already decided (handoff M7 → done by
the probe); collision moves into our own Jolt (handoff §6 had `StaticBody3D`); NavigationServer3D is replaced by
Detour; soldiers and weapons come before vehicles because every vehicle needs seats, weapons and hits.

| step | slices | content | exit criteria (in the running game unless noted) |
|---|---|---|---|
| **S0 Toolchain and skeleton** | 1 | repo layout (§2.2), pinned godot-cpp + JSON dumps, own Jolt, upstream ENet, Recast/Detour, doctest, `DedicatedMainLoop`/test runner, `InstallReader` with the read-only self-test, short build root, `--import` in CI | extension loads in dev7 headless and windowed; a Jolt world steps on a `std::thread` while Godot renders; ENet loopback ping; two Jolt copies coexist (Windows; Linux link test); install mtimes unchanged |
| **M0 Data layer in C++** | 4–6 | port `asset_transport/bf3`, `superbundle_bf3`, `ebx_bf3`, needed parts of `resource_bf3`/`level_bf3`; gameplay table readers (WeaponDef resolver, soldier, kits, mode constants, material grid, vehicle defs); Havok collision; skinned MeshSet; Ant GD reader + DCT/Raw/Frame + channel map | **M9 1p empty reload plays in Godot from C++ reading the install** on BF3 arms + M9 meshes with `fov_override`; an in-game inspector prints the resolved M16A3/M16A4-burst WeaponDef matching §7.1; Metro collision drawn as a debug overlay; handoff §9 counts pass in doctest |
| **M1 Metro walkable through the loopback server** | 5–7 | Metro static world (minimal look: RS instances, basic materials, sun); static world in own Jolt; `SoldierMotor` with BF3 numbers; `ServerHost` 60 Hz + loopback `ClientSession`; command/snapshot path; own-soldier prediction; free camera; shot harness | walk/sprint/crouch/prone/jump match §7.2 (6.5 m/s, 0.6 m, transition times, fall-damage thresholds); no rubber-banding at rest under 150 ms / 2 % netsim; spawn settle works on all Metro TDM points |
| **S1 Server scale spike** | 1 | Detour bake from the character-collision set (fix the Metro split); 64 headless bot walkers (motor + Detour) on Metro and Caspian collision at 60 Hz | p99 tick ≤ 8 ms with 64 walkers; all flags, HQs, spawns in one nav component; sweep cost measured. **Fail →** 30 Hz sim, fewer sweeps, own BVH |
| **S2 Vehicle replay spike** | 1 | Humvee on `VehicleConstraint` in ServerWorld + PredictionWorld; same BodyIDs; full-precision save/restore; recorded command log | free driving bit-identical in both worlds for 60 s; corrections < 2 cm at contacts with quantised snapshots. **Fail →** IW4L-style kinematic integrator querying the static world (04 §1.5) |
| **S3 Crowd animation spike** | 1 | `bf3_anim` with decoded `b_BasicAssetsMP` clips: 64 3p soldiers (run blend + upper-body reload), windowed on the RX 9070 XT; temporal LOD tiers from `GameAnimationSettings` | main-thread animation + skeleton ≤ 2 ms at 1440p; frame budget held; shoulder pinch judged by the user (decides whether rig ops are needed now) |
| *measurement pass* | (user, ~1) | in the user's BF3/VU, knowledge only: fixed-wall spread/recoil plots, falloff at distances, timed ticket bleed at 2/3 and 3/3 flags, capture times, Humvee/tank/heli/jet reference clips, turret traverse | numbers recorded in `SOURCES.md`; nothing touches the install |
| **M2a Infantry gunplay** | 6–9 | weapon sim (sub-tick, seeded), ballistics, material-grid damage, hit capsules from the server pose, server-validated hits; 1p viewmodel with Ant clips (deploy/fire/reload/zoom; VBR stand-in), springs; ADS; 4 kits × 1 primary + pistol + knife + M67; minimal HUD and weapon audio | shooting stationary bot targets on Metro reproduces §7.1: 800 RPM = 75 ms, 1.60 m drop at 300 m, shots-to-kill table, headshot ×2.0/2.35; 1p reload timing vs `ReloadTime` |
| **M2b Metro TDM with bots** | 5–8 | 3p soldiers, health/regen/death/respawn, kill feed, scoreboard; TDM rules; bots B2–B4 (walk, perceive, aim, fight, revive) | a TDM round to 100 kills against 16 bots on Metro; bots-only server produces kills; team K/D ≈ 1 |
| **M3 Two machines and scale** | 3–4 | ENet transport, digests handshake, ghosts with priority/quantisation, dedicated `--headless`, loadbots to 32 then 64, bandwidth overlay, master + UPnP | two PCs fight with correct hit-reg at 100 ms simulated RTT; a mismatched install is rejected with a reason; 64 connections ≤ 36 KB/s/client and tick < 8 ms |
| **M4 Caspian Conquest with land vehicles (MVP)** | 8–13 | terrain/look track for Caspian; Conquest Small/Large rules (resolver, flags, bleed, spawns, HQs, combat area); Humvee and T-90/M1A2 with seats, turrets, HMG/cannon, damage grid, disabled/decay/regen, wrecks and respawn; bots board and drive (hull-class navmesh + ROAD areas) | full Conquest Small round on Caspian with bots; Humvee and tank handling vs reference clips; RPG/tank-shell damage percentages vs reference; ≥ 3 of 4 jeeps taken by bots within 30 s |
| **M5 Air and sea** | 10–16 | AH-1Z/Mi-28 (rotor model, rockets, gunner, TV missile as a remote vehicle, flares/ECM, locks), F/A-18/Su-35 (wings, afterburner, gear, stall), RHIB/AAV (buoyancy), AA vehicles; bots fly (clearance grid + PIDs) | hover/climb/speed oracles; turn-rate peak near 305 on the flap curve; flare breaks a lock; 0 terrain crashes in 10 min of bot flight |
| **M6 Destruction** | 6–10 | slots/states/parts, wiring (`OnDestroyed`), collapse fallback, debris in the cosmetic world, effects as GPUParticles, destruction rows, dirty-tile nav rebake | an RPG hole appears identically for a late joiner; walking through a fresh hole predicts correctly; bots path through new openings |
| **M7 Breadth and 64 on Caspian (v1)** | 25–45 | all kits and gadgets (incl. MAV/EOD/T-UGS/M224 as vehicles), spotting, suppression, squads and beacons, attachments; remaining modes (Rush, SQDM, Domination, TS, AS, CTF, GM, Scavenger); remaining vehicle roster; remaining levels; audio/FX; server ops | a full 64-slot Conquest Large round on Caspian with bot fill inside §3.5 budgets; every vehicle spawns, drives/flies, fires, dies, respawns on its levels |

Totals [inferred]: MVP (through M4) ≈ 35–51 slices plus the remaining look slices, in line with 08 §5.1's 40–65
developer-weeks once the animation row shrinks; v1 (M5–M7) ≈ 41–71 further slices (08: 50–85 w). The S1–S3 spikes
come right after M1 because they need the collision world and decide D4, D9 and the animation budget.

---

## 7. Test oracles (consolidated)

Sources: W = [topics/01](topics/01-weapons.md); S = [topics/02](topics/02-soldier-kits-modes.md); A =
[topics/03](topics/03-animation.md) + [Ant probe](probes/ant-animation-probe.md); V = [topics/04](topics/04-vehicles.md);
N = [topics/05](topics/05-networking.md); B = [topics/06](topics/06-bots.md); G = [topics/07](topics/07-godot-engine.md)
+ [toolchain probe](probes/godot-gdextension-probe.md); E = [EBX probe](probes/ebx-data-probe.md).

### 7.1 Weapons [verified data unless tagged]

| item | value | src |
|---|---|---|
| M16A3 (`weapons/m16a4/m16a4`) | auto + single, 800 RPM (interval 75 ms), mag 31 × 5, reload 1.8/2.37 s, v0 650, 25 → 18.4 over 8–50 m, g −15, hip 2.5–7 +0.1, ADS 0.2–3 +0.1, recoil 0.26 up, horizontal −0.4..0.1, dec 18, FSRM 2.5, deploy 0.67 | W §1.9, E §3 |
| M16A4 burst (`u_m16_burst` → `u_m16a4_burst`) | 3 per burst, 800 RPM; = M16A3 × burst modifier (ADS ×0.5 → 0.1, horizontal ×0.75 → 0.075/0.3) = Symthic row | W C5 |
| AK-74M | 650 RPM, v0 600, 25 → 18.4, recoil 0.28 / −0.3..0.2 / 18, FSRM 1.5; Symthic "left" = data Max, "right" = −Min on all 50 matching rows | W C7, E §3 |
| M249 | 800 RPM, 100 × 3, reload 6.2, v0 620, hip 5–7, ADS 0.5, recoil 0.4 / ±0.4 / 12, FSRM 2.2 | E §3 |
| M98B | `RateOfFire 360` but bolt 1.3 s → 46.2 RPM; 95 → 59 over 15–150 m; v0 650; g −9.81; mag 6 × 7 | W C8, E §3 |
| other bolt actions | JNG-90 1.3, L96 1.38, M40A5 1.1, SV98 1.25 s → 46.2/43.5/54.5/48 RPM | W C8 |
| AN-94 | burst 2 @ 1200, `RateOfFire 600` | W C8 |
| 870MCS | 12 pellets 18 → 6 over 8–25 m, v0 300, pump 0.48 s (125 RPM), pellet cone 3°, reload 1.15 + 0.67/shell | W §2.7 |
| M9 | 400 RPM, 25 → 13.75 over 12–50 m, v0 380, 16 rounds, reload 1.35/1.6, `ReloadThreshold 0.75` | W, A C11 |
| RPG-7/SMAW | shot (0,0,40) → `MaxSpeed 100`, `EngineStrength 10`, g −1.5, `Damage 225` (community: 100, unresolved), blast 112 inner 0.35 r 4, `TimeToArm 0.5`, dud blast 56 | W §1.6, E §3 |
| M320 HE / M67 | v0 45 (+1.5 up), g −7.2, impact 100, blast 112 r 0.35–5, arm 0.5 / thrown 18 (+3 up), fuse 3.6 s, blast 112 r 1.5–7 | W §1.6 |
| ballistic drop (no drag) | v0 650, g 15: 100 m → 0.178 m, 300 m → 1.60 m; M98B 300 m → 1.05 m, 1,000 m → 11.6 m; max range = v0 × TTL (M16A3 975 m, M98B 3,250 m) | W §2.3 [inferred arithmetic] |
| body multipliers (Metro, MP_001, Caspian) | BulletDamage head 2.0 / body 1.0 / chest 1.0 / legs 0.91; Sniper 2.35/1.0/1.25/0.91; .338 2.35/1.0/1.06/0.91; buckshot 1.0/1.0/1.0/0.91; flechette 1.0; melee 0.67/0.5; RPG/40 mm impact 2.5; grenade body 0 | W C4, S C4, E §4 |
| shots to kill | M16A3 4 body / 2 head ≤ 8 m, 6 body / 3 head ≥ 50 m; .338 head 223 (OHK at any range); .338 chest OHK ≤ 17.5 m; 7.62 bolt chest OHK ≤ 15 m (SV98 ≤ 20 m); DMR head OHK ≤ 50.8 m (corrected); SKS head OHK ≤ 16.3 m | W §2.4 + extra-1 [inferred arithmetic] |
| suppression per bullet | 5.56 7, `_heavy` 10, `_cold` 4, 7.62 semi 20 (SKS 10), bolt/.338/slug 35, RPG/40 mm 50; soldier sphere 1.5 m, decay 0.5/s after 2 s | W C16, extra-2 |
| Symthic cross-check | 50 of 62 rows identical on 16 fields; the 12 differences explained (G17, burst, bolt-action NoZoom, SKS, LSAT, UMP-45, 93R) | W C6 |

### 7.2 Soldier [verified data unless tagged]

| item | value | src |
|---|---|---|
| health | 100; regen 5/s after 10 s (community 16 s); spawn immunity 2 s; man-down 13 s; revive → 20 hp; corpse 15 s | S C3 |
| capsule / poses | r 0.35; heights 1.8/1.15/0.3; eyes 1.6/1.0/0.3; step 0.3; slope 50°; mass 100 | S C2 |
| speeds | stand 4.0 (back/strafe ×0.75), sprint ×1.625 = 6.5 [inferred], crouch 2.0, prone 0.9, swim 3.0, ladder 1.32, parachute 9.0 (terminal 5) | S §1.3 |
| jump / fall | `JumpHeight 0.6`; fall damage 0 ≤ 13.9 m/s, 20 @ 14, 40 @ 22, 100 @ 30 (≈ 9.9 m / 46 m drops [inferred]) | S §1.4 |
| pose transitions | stand↔crouch 0.3; stand→prone 0.7; crouch→prone 0.5; prone→stand 0.7; prone→crouch 1.05 s | S §1.3 |
| specializations | Sprint 1.1, Ammo 1.5, Explosives 2.0, Grenades +1 (+3), Flak 0.85, Cover 0.5, Suppression 1.5 | S C6 |
| hit capsules | head r 0.15, upper spine sphere r 0.25, lower spine r 0.23, arms r 0.08 len 0.28, legs r 0.10–0.11 len 0.40–0.43 | W C4 |
| skeletons | 1p 179 bones (Ant rig 181), 3p 213 (rig 215), weapon 25, parachute 42 | A C3, extra-2 |

### 7.3 Modes [verified data unless tagged]

| item | value | src |
|---|---|---|
| conquest | `TicketLossPerMin 15`, `TicketLossStart 100`, capture 20/20 s, 0.34 per extra player, max ×4; MP_001 CQS 250/51, CQL 400/34; Metro CQS 350/55 | S C8, C9 |
| other modes | Domination 150/60, flags 2/8 s; TS 200/150; AS 250/55, flags 4/4 s; Scavenger 5 flags × 40, 300 each team; Rush 75 lives (Metro 100), defenders 999, arm 2.5 s, disarm 5 s, fuse 25 s; SQR 20 lives, arm 5 s; TDM 100; SQDM 4 × 50; GM 17-level ladder | S C8–C12, extras |
| player counts | Metro `settings_win32`: CQL 64, CQS 32, RL 32, TDM 32, SQDM 16, SR 8, DOM 16, GM 16, TS 24, AS 24, CTF 32; squad size 4 | S extra-4 |
| scoring | kill 100, headshot +10, revive 100/110, CQ capture 250, MCOM destroyed 500, CTF capture 500, suppression assist 50 | S C13 |

### 7.4 Vehicles [verified data unless tagged]

| item | value | src |
|---|---|---|
| masses / gravity | Humvee 1500 / 1.85; buggies 800 / 1.65; tanks 5000 / 1.6; helis 5000 / 1.0; jets 10000 / 1.0; RHIB 1000 / 1.25 | V §2.0 |
| health | MBT 1000 ×4 zones, angles F 81°×1, R 36°×2.75, L/R 45°×1.8, shields 175, disabled 500, emergency 510, regen 30/s after 12 s; attack heli 500/170/185; jets 500, regen 18/s after 10 s; C-RAM/Pantsir `ArmorMultiplier 99999` | V C8 |
| grid (MP_007) | TankShell→MBT 0.9, →IFV 1.1, →Attack heli 2; Rocket→MBT 2; BlastDamage→MBT 0; 25mm→Attack 1.75; BulletDamage→every vehicle 0 | V C7 |
| weapons | MBT cannon 200 m/s, 300 dmg, blast 100 r 5; HMG 600 m/s, 550 RPM, +0.075 heat/shot, −0.5/s (≈ 5.3 s to overheat [inferred]); jet cannon 900 m/s 2000 RPM; Hellfire guidance after 1.75 s; TV missile 7 s life | V §3.1, C9–C10 |
| AH-1Z | lift spring 40000 / damping 20000; `CollectiveInputIdle` 0.9 (engine) and 0.99 (`RotorParameters`); tail ×75; rotor parts 10/11 (tail 15/16) | V C11 |
| jets | pitch flap authority (250,1.75)(305,1.7)(320,1.05); lift peaks 18° (roll pair) / 24° (pitch flap wing); afterburner 1.5, `DissipationTime 9999` | V C12 |
| turrets | M1A2 ring lock 0.0001 (T-90 0.0), gun 0.85 (T-90 1.0), HMG 1.0; MBT gun −20…0° (T-90 −20…4°) | V C13 |
| IW4L feel targets | Humvee 68 km/h at 6 s, braking ~10 m/s²; T-90 pivot 65° in 2 s; heli 159 km/h at 25° nose-down | V §2 [report claim] |

### 7.5 Networking, animation, navigation, engine [verified unless tagged]

| item | value | src |
|---|---|---|
| BF3 net settings | `MaxSimFps 30`; `OutgoingFrequency 10` / `IncomingFrequency 15`; rates 128000/48000; `MaxServerFrameSize 2048`; `MaxGhostCount 8192`; `MaxClientCount 68`; ports 25100/25200, protocol 67 | N K1–K2 |
| our budgets | ≈ 21.7 KB/s average, cap 36 KB/s payload (37.2 on the wire), upstream ≈ 4.7 KB/s per client; tick 4–6 ms of 16.7 [inferred] | N K14 |
| Ant decode | 322 packages, 0 parse errors; 3,778/3,778 DCT clips bit-exact in 1.7 s (C#); 5,592/5,592 clips and 684,459 channels mapped; M9 frame 0 vs IdlePose 0.07° / 0.4 mm | A, probe §4–§5 |
| anim cost | 64 × 213 bones: own C++ 0.41–0.46 ms; AnimationPlayer ~10 ms; AnimationTree ~30–35 ms/frame | A C13 |
| Detour (Metro, 743,835 tris) | 14,797 polys, 2.6 MiB, 3.7 s on 1 thread, queries 0.06–0.17 ms mean; tile rebake 110–163 ms, swap 0.3–0.6 ms | B C7, C10 |
| Godot nav (Metro) | 55,319 polys; queries 4–9 ms mean; region swap ≈ 6 s with edge connections, 28–96 ms without | B C7–C8 |
| Godot rays (Jolt) | `intersect_ray` 1.5–2.2 µs; `intersect_ray_into` 0.53–0.99 µs; 4 threads 3.8–3.9k rays/ms; `body_test_motion` 24–30 µs (long sweep) | G probe §6.2 |
| RS | `instance_create2` 1.0–1.2 µs; 10k instances of 1 mesh = 6 draws (1.5 ms frame D3D12); 10k over 1,000 meshes = 2,748 draws | G probe §6.1 |
| GSTF streaming | 2048² DXT1 `.stex` in `user://` streams mip 3 (43,704 B) → mip 2 (174,776 B) in ~42 ms; decays after ~5 s | G probe §6.4 |
| godot-cpp build | 139.7 s debug, 127.6 s release, 12.5 s incremental | G probe §4 |
| install read | 73,371 partitions, 0 failures; one level 2.2 s; all MP/XP + global 32 s | E §0 |
| precision | max infantry distance 2,252 m (XP5_002; median 476 m); float32 ulp 244 µm at 2–4 km ≈ 1 px viewmodel error at 1440p/90° | G K9–K10 |

---

## 8. Contradictions found and how they were resolved

| # | contradiction | sources | resolution |
|---|---|---|---|
| C1 | Hit model: "server-authoritative rewind; client hints only select history samples" vs "server-validated client hit detection with the reported render sample"; BF3 itself used client arbitration | 01 §3 vs 05 §5.4; 01 C10 | Same mechanism seen from two sides: the server always re-flies the seeded bullet and tests the target at the sample the client says it drew (05's provenance method), ≤ 250 ms; unreported targets at the shooter's render time. The report is never proof, only which sample to test. BF3's 0.85 s client arbitration is not copied |
| C2 | Soldier motor over Godot `body_test_motion` vs gameplay physics in our own Jolt | 02 §4.1–4.2 vs 04 §0, 05 §6.1, 07 §4.3 | Motor design kept (POD state, `ICollisionWorld`), backend = own Jolt. 02's own verification (C15/extra-5) removed its "any thread" premise |
| C3 | Why own Jolt: "Godot forbids off-main-thread queries" | 05 §0/§6.1, 07 §0/§4.3 vs probe §6.2, 05 K13/extra-1, 07 extra-1 | Queries work on workers inside a window the main thread opens. The real reasons: no per-space step, no save/restore, no Jolt classes. Decision unchanged |
| C4 | Ragdolls/debris in Godot physics (`PhysicalBoneSimulator3D`, `RigidBody3D`) vs one static-collision copy | 03 §4.6, handoff §6, 05 §6.1 vs 05 K13 | Cosmetics and ragdolls live in the extension's own client Cosmetic Jolt world, which shares the static shapes; Godot physics would need a second static copy |
| C5 | Tick: BF3 30 Hz and 08's MVP row "30 Hz" vs 60 Hz in 01/04/05/06 | 08 §5.1, N K1 vs 05 §4.1 | 60 Hz sim / 30 Hz snapshots, configurable down to 30 Hz; every rate per second, so feel does not change |
| C6 | Transport: Godot `ENetConnection` (08 MVP row, handoff §6 "MultiplayerPeer") vs upstream ENet | 08 §5.1, handoff §6 vs 05 §3.3, probe §6.6, 05 extra-2 | Upstream ENet linked in `bf3_net`: Godot's socket shim ignores RCVBUF/SNDBUF, and `create_server` misroutes the channel count |
| C7 | Navigation: NavigationServer3D (handoff §6, 04/05 open) vs Detour | handoff §6, 05 §6.2 vs 06 §2 | Detour, measured ≈ 25–50× faster on an identical workload, with tiles, areas and 0.3 ms swaps |
| C8 | Animation: "biggest unknown, decide early; fallback procedural" and 08's "Ant decode = top risk, 4–12 w" vs the probe | handoff §1/§8, 03 §6, 08 §5.5 vs Ant probe, 03 extra-8, 08 K16 | Decode solved for DCT/Raw/Frame (95 %); Route A. The remaining risk is the graph, rig ops and VBR (R2) |
| C9 | Rest pose "EBX `LocalPose`" vs Ant rig defaults | 03 §1.4/§4.1 vs 03 extra-2, Ant probe §5 | Animate in the Ant rig hierarchy (181/215 joints) with `RigAsset` defaults; EBX `ModelPose` for inverse binds only |
| C10 | GD archive "big-endian" (handoff, 03 §1.3, EBX probe §7 "big-endian with GD.STRMl") | handoff 02 §10, 03 vs Ant probe §3.2, 03 extra-7 | Package header big-endian; every GD blob little-endian (`'l'`) on PC |
| C11 | `ChannelToDofAsset` storage type 2 = (target, slot) byte pairs (IceBloc) | 03 §1.4 vs Ant probe §5, 03 extra-1 | One u16 big-endian DOF index per channel |
| C12 | `DeltaBase` constant vs running DC predictor; IceBloc as the only decoder | 03 §1.5, §2 vs 03 C6/C7 | Constant per-DOF base added to every block (cumulative variant: \|q\|−1 ≈ 2–3·10⁻²); two independent decoders agree; Rime's DCT file is probably IceBloc-derived (GPL) [inferred], so clean room |
| C13 | Temporal-LOD tiers "engine settings, not in level bundles", inferred 25/60/120 m | 03 §4.7, §7 vs 03 C17 | Read from `default/settings_win32` `GameAnimationSettings`: 2/30/60/90/120/150 m, 0.02–0.12 s, far 208 m |
| C14 | Regen delay 10 s (data) vs 16 s (community); M98B falloff end 150 vs 120 m; spread decrease 15 vs 8; RPG direct 225 vs 100 | E §3, S C17 | Data wins (it is the final patch); each becomes a measurement-pass item (R5) |
| C15 | Gravity "−9.81 for semi/bolt" | 01 §1.6 vs 01 C3/extra-2 | SKS round −15 (suppression 10); crossbow BA −2, HE −11; slugs −12 |
| C16 | Bullets damage `StationaryDamage`/`Chickenwire`/`Light` | 01 §1.7 vs 01 C9/extra-3 | Only `Light`(124), `ChickenWire`(150), `Passthrough`, `DamageClass_1/2`, `BulletsOnly`, `AllProjectiles`, `CollResponseCombined` take bullet damage; StationaryDamage 0; 1,800 penetrable pairs |
| C17 | DMR headshot OHK ≤ 15 m | 01 §2.4 vs 01 extra-1 | ≈ 50.8 m (7.62/5.8 `_semi`); SKS ≈ 16.3 m |
| C18 | MBT turrets hold world yaw (IW4L, handoff 04 §4.2) | handoff vs 04 C13 | MBT main ring not stabilised (lock 0.0001/0.0); gun pitch mostly stabilised; HMG rings stabilised (1.0) |
| C19 | Vehicle prediction "zero corrections free-driving" | 04 §5.4 vs 04 C15, 05 K12 | Only with identical BodyIDs, full-precision restores incl. warm starts and no nearby bodies; reconcile with ε above quantisation |
| C20 | Reactive armour as a 175-point pool | 04 §3.4 vs 04 extra-1 | Absorb one whole hit per side until measured |
| C21 | Scavenger "3 conquest flags, Team1 300" | 02 §3.10/§3.1 vs 02 extra-1/extra-2 | 5 Domination flags, both teams 300; fits the majority bleed rule |
| C22 | Rush defenders 9999 lives | 02 §3.4 vs 02 extra-3 | Interface pushes 999 (effectively unlimited) |
| C23 | Level×mode count 230 (handoff) vs 227 (06 body) | handoff §9 / 04 §1 vs 06 C5/extra-5 | 230 rows including 3 MP_012 tutorial rows; 227 playable |
| C24 | XP4/XP5 vehicle spawners "not found" | handoff 04 §4.1/§11 vs 04 C6/extra-3 | Found in `dynamicspawn_*`, `outpostspawn_*`, `xp5/dynamic_vehiclespawners/*` prefabs |
| C25 | fun-bots "GPL" (task brief) / "no licence" | brief vs 08 K1, 06 C1 | No licence for bot logic and maps; MIT only for `WebUI/` metadata and the fork's `astar-lib`. Ideas only; traces opt-in, never shipped |
| C26 | VU "emulates Blaze" | 08 §0/§1.1 vs 08 K5/extra-8 | VU dropped Blaze emulation in Sept 2013 for its own backend (Zeus); it still runs the original game, so it remains knowledge only |
| C27 | Rime licence request to "OrfeasZ" | 08 §2.1 vs 08 K2 | Rime is multi-author (kiwidoggie, OrfeasZ, FlashHit, txt231, Powback, keku645); moot, since the probe re-derived the spec |
| C28 | IW4L's handshake digest omitted collision | 05 §2, §7.6 vs 05 K15 | Stale: IW4L's final `content_digest` hashes collision. Our `sim_digest` includes it anyway |
| C29 | Texture streaming feedback "per instance" | 07 §1.3 vs 07 K4 | One feedback slot per **material**: one near instance raises that material's textures everywhere |
| C30 | Build RIDs on workers with `mesh_add_surface` | 07 §6.1 vs 07 K11/extra-2 | Use `mesh_create_from_surfaces` on workers; `mesh_add_surface` is queued to the main thread |
| C31 | Bot CPU 1–2 ms (06 §0, 05 §10) vs 0.4–1 ms (06 §3.9) | 06, 05 vs 06 C17/extra-1 | ≈ 0.3–0.9 ms per tick single-threaded, estimate dominated by rays |
| C32 | Bandwidth "bots cost no bandwidth"; "BF3 capped 128 kbit/s [verified]" | 05 §7.4 vs 05 K14, extra-3 | Bots cost no connection or upstream but are ghosts downstream; BF3's 128000 has unknown units |
| C33 | Weapon EBX "3,497 partitions in Metro" | 01 §0 vs 01 C1 | 3,497 entries = 2,795 EBX + 702 res |
| C34 | GDExtension interface JSON "identical" | 07 §2.1 vs 07 K5, probe §2 | ABI-equivalent, not identical (one argument type); generate from the exe's own dump |

---

## 9. Open questions and next experiments

**Measurement pass in the user's BF3 or VU** (knowledge only, offline, nothing written to the install; EU Art 5(3)):
1. Fixed-wall shot plots (Symthic style) for 3 rifles, one LMG, one bolt action: spread disc distribution, minimum rise
   on state change, recoil recovery (rate vs exponential), when the first-shot multiplier re-arms.
2. Damage at 5/20/40/60 m on a friendly target with hit markers: falloff shape and distance basis.
3. Timed bleed at 2-of-3 and 3-of-3 flags; capture flip time with 1, 2 and 4 attackers; regen onset after damage.
4. RPG direct hit on infantry; tank shell on MBT front/side/rear with and without reactive armour.
5. Vehicle reference clips at fixed spots: Humvee 0–60, braking, full-lock circle; T-90 vs M1A2 top speed and pivot;
   AH-1Z hover, climb, 25°/36° dive speeds; F/A-18 turn rate vs HUD speed (settles the speed-axis units); turret
   traverse rates.
6. ADS zoom FOVs on screen (whether `RenderFov`/`FieldOfView` are vertical).

**Data experiments** (read-only tools in scratch):
7. Rebuild collision per Havok body type (Raycast / Collision / Character) and re-run the Metro nav coverage; identify
   the solid objects at the split points; look for ladder entities in MP data.
8. Delta-patched bundle reconstruction for XP2–4 and `Loc/*`, then the digest scheme over them; enumerate language
   bundles.
9. `settings_win32` per level (player counts, `GameModeSettings`); interface fields with no value (default push?).
10. VBR animated bitstream; `CurveAnimationAsset`; rig ops (`RigOpListAsset`) for twist/roll; additive clips and
    `TrimOffset`; DeltaTraj DOFs; the 0x1548 id prefix; `RagdollResource` (try the Havok reader).
11. Remaining open fields: `ForwardGearSpeeds`, `MaxVelocity`, `PowerFadeOutRange`, `VelocityDamageThreshold` units;
    `WeaponInputRouterComponentData`; the 40 mm grid row; `UsePrediction` on the remaining jets and helicopters;
    `RoadData` junctions; weapon sound-graph environment selection.

**Engine experiments**:
12. Two Jolt copies (and Recast) in one process on Linux; own-Jolt static world memory for Caspian with two systems.
13. Soldier motor sweep cost on own Jolt at 64 soldiers; `CharacterVirtual` replay within ε across corrections.
14. Vehicle replay bit-compare between ServerWorld and PredictionWorld (S2).
15. 64 skinned soldiers with rendering and shadows on the RX 9070 XT (S3); viewmodel with TAA/FSR and SSAO/SSR.
16. GSTF streaming with ~4.5k textures on D3D12 under a memory budget; per-material feedback with shared BF3 materials;
    whether GSTF stays v1 in 4.8-stable; `has_mipmaps()` returning 0 through godot-cpp.
17. At the RC: re-dump, `--validate-extension-api`, rebuild, check the 9 changed method hashes, `STREAMING_LOD`.
18. `mesh_create_from_surfaces` from worker threads at level load (code-read only so far); the ENet `create_server`
    argument mix-up at runtime (only relevant if Godot's ENet is ever used).

**Decisions for the user**:
19. Licence for our code (MIT recommended; GPL-3.0 if forks must stay open) before the first outside contribution;
    project name free of EA marks.
20. Whether a standalone `bf3_server` executable (no Godot) is acceptable as the dedicated server.
21. Whether to ask the fun-bots authors for permission to use their traces as a shipped validation set (optional; not
    needed for the plan).

---

## 10. Index of research files

| file | one line |
|---|---|
| [topics/01-weapons.md](topics/01-weapons.md) | BF3 infantry weapons: data chain, GunSwayData model, modifiers, ballistics, material-grid damage, 68-weapon roster, Symthic 50/62 match, C++ fire sim, hit registration |
| [topics/02-soldier-kits-modes.md](topics/02-soldier-kits-modes.md) | Soldier health/movement/poses/specials, kits and slots, specializations, gadgets, all MP mode constants and rules, scoring, motor design |
| [topics/03-animation.md](topics/03-animation.md) | Ant/GenericData format, skeletons, AntRef bindings, Godot 4.8 skeleton/IK/viewmodel APIs, animation cost bench, decision procedure |
| [topics/04-vehicles.md](topics/04-vehicles.md) | Vehicles on own Jolt: Godot/Jolt source findings, per-family data and models, roster (49/2,048), weapons, locks, damage grid, seats/cameras/turrets, networking, 10-step plan |
| [topics/05-networking.md](topics/05-networking.md) | BF3 netcode settings from the install, BF4 data, Godot multiplayer/ENet analysis, 60/30 Hz architecture, prediction, hits, ghosts, budgets, handshake, session, build order N1–N6 |
| [topics/06-bots.md](topics/06-bots.md) | fun-bots in depth (licence, traces, control), Detour vs NavigationServer3D measured on Metro, bot behaviour design, budgets, B0–B10 plan |
| [topics/07-godot-engine.md](topics/07-godot-engine.md) | Godot 4.8 status, texture streaming (GSTF), godot-cpp v10 pinning, precision, threading rules, dedicated server, runtime cache formats, repo/CMake/CI |
| [topics/08-prior-art-legal.md](topics/08-prior-art-legal.md) | VU, Rime, Frosty, Frostbite scripts, BYOD precedents, EU/US law, EA User Agreement, trademark practice, the user's install, MVP/v1 scope |
| [probes/ebx-data-probe.md](probes/ebx-data-probe.md) | Whole-install EBX dump (73,371 partitions): weapons, soldier, kits, material grid, vehicle AI, spawners, animation resources |
| [probes/ebx/](probes/ebx/) | Curated JSON dumps: 10 weapon closures + `_summary.json`, soldier/kits/teams, damage multipliers, Humvee/M1/AH-1Z, spawn refs, skeletons and Ant packages |
| [probes/ant-animation-probe.md](probes/ant-animation-probe.md) | Ant decode verdict: container format, bit-exact DCT spec, channel mapping, mesh binding, VBR findings, Godot renders, C++ port sizing |
| [probes/ant/](probes/ant/) | Census, layouts of `s_basicassets`, validation logs, mapping checks, decoded M9/3p clips as JSON oracles, skeleton comparisons, `shots/` renders |
| [probes/godot-gdextension-probe.md](probes/godot-gdextension-probe.md) | godot-cpp toolchain against dev7: commands, build times, errors and workarounds, RS/Jolt/nav/ENet/streaming measurements, setup gotchas |
| [probes/tools/ebx-dump/](probes/tools/ebx-dump/) | Read-only Rust EBX dumper (copies of the IW4L crates) and its scripts |
| [probes/tools/ant-probe/](probes/tools/ant-probe/) | C# Ant probe over a scratch Rime clone: bank census, parameterised DCT decoder, mapping solver, mesh export, throwaway Godot viewer |
| [probes/tools/gdext-probe/](probes/tools/gdext-probe/) | Minimal C++ GDExtension probe (CMake, sources, project files) used for the toolchain and API measurements |
| [godot-api/extension_api.json](godot-api/extension_api.json) | Full extension API of `v4.8.dev7.official.c971f93e7` (single precision), byte-identical to a fresh dump; use it to verify any Godot claim |
| [godot-api/gdextension_interface.h](godot-api/gdextension_interface.h) | GDExtension C interface header of the same build |
| `C:\Users\olku\iw4L\context\artifacts\2026-09-30-bf3-maps\15-GODOT-HANDOFF\HANDOFF.md` (+ `sections\01`–`04`) | The IW4L handoff: decisions already made, coordinates, Godot mapping for the level/look pipeline, lessons, golden counts; `04-gameplay-sim.md` covers modes, collision, vehicles, destruction, bots, networking lessons |
