# Probe: C++ GDExtension toolchain against Godot 4.8.dev7, and the engine APIs the port depends on

Date 2026-10-10. Machine: Ryzen 7 5800X (16 threads), AMD Radeon RX 9070 XT, Windows 11 Pro 10.0.26200.
Source of the probe: `C:\Users\olku\BF3 reimplementation\research\probes\tools\gdext-probe\` (CMakeLists, `src\`,
README, the two project files). All build output, the godot-cpp clone and the project copy stayed in the scratch dir
`...\scratchpad\probe-godot\`. Neither the BF3 install nor `bf3-opensource` was touched (the project was copied; the
copy's `config/name` was changed so its `user://` is `%APPDATA%\Godot\app_userdata\gdext-probe-scratch`).
Raw logs: `...\scratchpad\probe-godot\run*.log` (scratch, not kept).

Tags: **[V]** verified here (measured, or read in code/the API dump at the cited line), **[RC]** report claim of a
source not re-checked, **[I]** inferred. Godot source links point at the exact build commit `c971f93e7`.

## 1. Verdict

The core goal works **[V]**: godot-cpp `master` @ `b051e694` generates bindings from the local
`research\godot-api\extension_api.json`, builds with MSVC 19.51 + Ninja via CMake in 140 s, and the resulting
`GDExtProbe : Node3D` loads in `Godot_v4.8-dev7` headless, windowed D3D12 and windowed Vulkan, runs every test and
quits with exit code 0. Nothing needed an engine module. Two real blockers for a naive setup were hit and solved
(MAX_PATH, missing `extension_list.cfg`); one engine API trap was found in navigation (`set_indices`), one in Jolt
threading; texture streaming of runtime-written files works.

## 2. Versions and pinning

| item | value | tag |
|---|---|---|
| Godot | `4.8.dev7.official.c971f93e7` (`version_info.hash` `c971f93e7e76b0ef919bf6009e7b868bea04db7f`, `hex` 264192, precision single) | [V] |
| API JSON | `research\godot-api\extension_api.json` is byte-identical to a fresh `--dump-extension-api` of the console exe (SHA-256 `B9B10796E8806E0E83EA4601B3F4C97E07406ACCEDAF598937050B4ABF59AFA1`) | [V] |
| godot-cpp | `master` @ `b051e694cf493d417f5aa945c656cb77ae9011a9` (2026-10-09). Since v10, godot-cpp is versioned independently of Godot and targets 4.3+ via `api_version`/`GODOTCPP_API_VERSION` (bundled JSONs 4.3–4.7) or a custom API file ([README](https://github.com/godotengine/godot-cpp/blob/master/README.md) §Versioning). There is no 4.8 branch or bundled 4.8 JSON; `master` = tag `10.0.0-stable` (`507ed9d8`, 2026-09-15) + 8 commits incl. "Fix for hot reload crash" (`79c37b21`) | [V] |
| interface JSON | godot-cpp now generates `gdextension_interface.h` from `gdextension_interface.json` (`cmake/godotcpp.cmake:288`), taken from `GODOTCPP_GDEXTENSION_DIR`, *not* from the custom API option. The dev7 dump (`--dump-gdextension-interface-json`) differs from godot-cpp's bundled copy in one entry only (`object_set_script_instance` argument type `GDExtensionScriptInstancePtr` vs `...DataPtr`, doc text); 179 functions on both sides | [V] |
| toolchain | VS 18 Community, MSVC 19.51.36260 (toolset 14.51.36231), CMake 4.3.3, Ninja 1.13.2 (VS-bundled, `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`), Python 3.14.4 (binding generator) | [V] |
| 4.7 → 4.8-dev7 API delta | 33 new classes (incl. `TextureStreaming`, `StreamedTexture2D`, `ResourceImporterStreamedTexture`, `Physics*Result3D` ×6, `BoneSpaceAdjuster3D`, `Line3D`, `Trail3D`), 173 new methods on existing classes, 9 methods with a changed hash (`Image.generate_mipmaps`, `Image.compress`, `Image.compress_from_channels`, `Skeleton3D.reset_bone_pose(s)`, `RenderingDevice.texture_create_shared_from_slice`, 2 `DisplayServer`, 1 OpenXR); none removed (diff of godot-cpp's `extension_api-4-7.json` vs the dev7 dump) | [V] |

## 3. Exact commands

```bat
:: 1. dump both JSONs from the exact build (run in an empty scratch dir; files land in the CWD)
Godot_v4.8-dev7_win64_console.exe --headless --dump-extension-api --dump-gdextension-interface-json
:: 2. clone
git clone https://github.com/godotengine/godot-cpp   (then: master @ b051e694)
:: 3. short path (see §5, MAX_PATH)
subst Q: <scratch>\probe-godot
:: 4. build (cmd.exe)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S Q:\gdext-probe -B Q:\bd -G Ninja -DCMAKE_BUILD_TYPE=Release -DGODOT_CPP_DIR=Q:\godot-cpp ^
  "-DGODOTCPP_CUSTOM_API_FILE=C:/Users/olku/BF3 reimplementation/research/godot-api/extension_api.json" ^
  -DGODOTCPP_GDEXTENSION_DIR=Q:\gdext-api -DGODOTCPP_TARGET=template_debug -DPROBE_OUT_DIR=Q:\project\bin
cmake --build Q:\bd
:: 5. register the extension in the project copy once, then run
Godot_v4.8-dev7_win64_console.exe --headless --path <copy> --import
Godot_v4.8-dev7_win64_console.exe --headless --path <copy> -- --nav=250,500,1000
Godot_v4.8-dev7_win64_console.exe --path <copy> -- --skip-nav                 (windowed, D3D12)
Godot_v4.8-dev7_win64_console.exe --rendering-driver vulkan --path <copy> -- --skip-nav --skip-race
```

`.gdextension` used: `entry_symbol = "gdext_probe_init"`, `compatibility_minimum = "4.8"`, `reloadable = true`,
`windows.debug.x86_64` / `windows.release.x86_64` → `res://bin/libgdext_probe.windows.template_{debug,release}.x86_64.dll`.

## 4. Build results [V]

| build | time | notes |
|---|---|---|
| first attempt, long path | failed after 79.5 s at object 594/1128 | `fatal error C1083: Cannot open compiler generated file: '': Invalid argument` on `...\gen\src\classes\open_xr_spatial_capability_configuration_plane_tracking.cpp.obj` (path length 261) |
| `template_debug`, full (via `Q:`) | 139.7 s wall (configure 2.5 s; "There are 2201 Files to generate"; 1,128 Ninja steps; MSVC `/MP15`) | 0 errors, no warnings from the probe sources at `/W4` |
| `template_release`, full | 127.6 s | |
| incremental (probe .cpp only) | 12.5 s | the configure step re-runs the glob check each time |
| outputs | `libgdext_probe.windows.template_debug.x86_64.dll` 609,792 B; `...template_release...` 589,312 B; `libgodot-cpp.windows.template_debug.x86_64.lib` static | |

godot-cpp CMake defaults that matter **[V]**: static CRT `/MT` (`GODOTCPP_USE_STATIC_CPP` ON, `cmake/windows.cmake:73,83`);
exceptions off (`_HAS_EXCEPTIONS=0`, `cmake/common_compiler_flags.cmake:173`; `std::thread`/`<atomic>` still work);
`DEBUG_ENABLED` defined unless target is `template_release`; `GODOTCPP_USE_HOT_RELOAD` defaults to empty (off,
`cmake/godotcpp.cmake:169`); godot-cpp's `windows.cmake` writes its PDB to `${CMAKE_SOURCE_DIR}/bin`, i.e. into the
consumer's **source** tree; the probe's CMakeLists overrides `PDB_OUTPUT_DIRECTORY` to keep sources clean.

## 5. Every error met, and the workaround

| # | symptom | cause | workaround | tag |
|---|---|---|---|---|
| 1 | C1083 "Cannot open compiler generated file" mid-build | object path > 260 chars (scratch path ~150 chars + godot-cpp's generated names up to ~110) | build from a short root (`subst`, or a short checkout such as `C:\bf3\`); enabling Win32 long paths was not tried (it is a system setting) | [V] |
| 2 | first run: `ERROR: Cannot get class 'GDExtProbe'` + "A placeholder will be created instead"; the process never exits | a project that the editor never opened has no `.godot\extension_list.cfg`, so no extension is loaded at runtime | run `--headless --path <proj> --import` once (3.8 s; it returned exit code 5 but wrote `extension_list.cfg` containing `res://bin/gdext_probe.gdextension` and a `gdext_probe.gdextension.uid`) | [V] |
| 3 | `ERROR: Condition "vertices.size() < p_indices.size()" is true` at `navigation_mesh_source_geometry_data_3d.cpp:48`, bake returns 0 polygons in 0.0 ms | `set_indices()` compares the **float** count of the vertex array with the index count ([source L47](https://github.com/godotengine/godot/blob/c971f93e7/scene/resources/3d/navigation_mesh_source_geometry_data_3d.cpp#L47)); any shared-vertex mesh with > 3 indices per vertex (every grid/terrain) is rejected | use `append_arrays(vertices, indices)` ([L59](https://github.com/godotengine/godot/blob/c971f93e7/scene/resources/3d/navigation_mesh_source_geometry_data_3d.cpp#L59)), which has no check. Looks like an engine bug worth reporting upstream | [V] |
| 4 | navmesh bake gives 0 polygons with "Godot" triangle order | raw `append_arrays`/`set_*` data goes to Recast unchanged and must have its right-hand normal `(v1−v0)×(v2−v0)` pointing up; `add_mesh`, `add_mesh_array`, `add_faces` swap indices 1↔2 themselves ([L137-139, L194-196](https://github.com/godotengine/godot/blob/c971f93e7/scene/resources/3d/navigation_mesh_source_geometry_data_3d.cpp#L137)) | 40 m test tile: order (a,b,c) with RH normal −Y → 0 polygons; (a,c,b) RH normal +Y → 139 polygons. Godot render/physics front faces are the opposite winding. Per the handoff (HANDOFF.md §5), BF3 triangles are already CCW around their normals, so BF3 collision soup can go into `append_arrays` **without** a swap, while the render and `ConcavePolygonShape3D` uploads need the swap | [V] probe; [I] BF3 consequence |
| 5 | `WARNING: Property agent_height is ceiled to cell_height voxel units and loses precision` (`nav_mesh_generator_3d.cpp:367`) | agent_height 1.8 not a multiple of cell_height 0.25 | choose values on the voxel grid (1.75) | [V] |
| 6 | `ERROR: intersect_ray must not be called while the physics space is being stepped.` (`jolt_physics_direct_space_state_3d.cpp:452`) | a worker thread queried while the main thread stepped the Jolt space | see §6.2: only query off-thread inside a window the game controls | [V] |
| 7 | `PhysicsServer3D::get_class()` reports `PhysicsServer3D`; `is_class("JoltPhysicsServer3D")` = 0 | Jolt's server is not registered in ClassDB (consistent with "no Jolt classes in the API dump") | identify Jolt by `physics/3d/physics_engine = "Jolt Physics"` and by Jolt's error paths | [V] |
| 8 | ENet burst loss | see §6.6 | pace sends, service every tick | [V]/[I] |

## 6. Measurements

### 6.1 (a) RenderingServer: 10,000 instances via `instance_create2` on the world scenario

Cube mesh (24 verts, 12 tris) built with `mesh_add_surface_from_arrays`, default material, one shadowed
`DirectionalLight3D`, 1152×648 window, vsync off (`override.cfg`), frame time measured between `_process` calls.

| case | create | frame ms (mean / p50 / p95 / p99) | RS info at frame 60 | tag |
|---|---|---|---|---|
| a1: 10k instances of 1 mesh, D3D12 | `instance_create2` ×10k 10.2–12.2 ms (1.0–1.2 µs each); `instance_set_transform` ×10k 0.5–0.7 ms | 1.47–1.61 / 1.01–1.16 / 1.55–1.68 / 15–18 | objects 11,100, primitives 133,200, **draw calls 6** (automatic instancing), video mem 79 MB | [V] |
| a2: 10k instances over 1,000 meshes, D3D12 | 1,000 meshes 17.9–26.6 ms; 10k instances+xforms 9.7–12.5 ms | 2.56–2.98 / 2.46–2.85 / 3.18–3.76 / 3.6–4.4 | **draw calls 2,748**, video mem 210 MB | [V] |
| a1, Vulkan | — | 1.96 / 1.52 / 2.27 / 16.4 | draw calls 6 | [V] |
| a2, Vulkan | — | 4.18 / 3.87 / 6.15 / 7.0 | draw calls 2,748 | [V] |
| free 10k instances + meshes | 26–35 ms windowed, 17–19 ms headless | | | [V] |
| headless (dummy RS) | same create cost; frames 6.90 ms flat (headless loop paces at ~145 Hz) | | all RS info = 0 | [V] |

Reading: per-instance creation (~1 µs) is no concern for BF3's ~20k placements (~25 ms). Godot instances
identical mesh+material automatically (6 draws for 10k); distinct meshes cost ~2.7 draws each across passes
(depth/color/shadow) [I], so Caspian's 2,277 meshes / 6,166 LOD surfaces will be draw-bound before
instance-bound; merge static meshes per cell or use `MultiMesh` for repeats. D3D12 was ~30 % faster than Vulkan on the
distinct-mesh case on this GPU [V for this machine only].

### 6.2 (b) PhysicsServer3D on Jolt

Concave: 224×224 grid, 2 m spacing (448 m square), 100,352 triangles, `backface_collision=false`. Heightmaps:
1025² and 1024² samples, 1 m spacing. Rays from `PhysicsDirectSpaceState3D` obtained with
`space_get_direct_state(get_world_3d()->get_space())`, run inside `_physics_process`.

| test | result | tag |
|---|---|---|
| concave `shape_set_data` / body create+add+`body_set_space` | 1.7–2.1 ms / 66–82 ms (the BVH cost lands on body attach) | [V] |
| heightmap 1025² / 1024² `shape_set_data` / body | 5.9–6.6 ms / 41–55 ms; 1024² no slower than 2ⁿ+1 | [V] |
| ray visible before first physics step? | **yes**: a ray cast in the same frame as body creation hit (Jolt flushes pending bodies inside each query, [L454](https://github.com/godotengine/godot/blob/c971f93e7/modules/jolt_physics/spaces/jolt_physics_direct_space_state_3d.cpp#L454)) | [V] |
| 10k vertical rays, concave, `intersect_ray` (Dictionary) | 1.54–1.99 µs/ray, 10,000 hits, mean \|y−h\| 0.028 m (= triangle-vs-analytic error) | [V] |
| same, `intersect_ray_into` + reused `PhysicsIntersectRayResult3D` (new in 4.8) | **0.53–0.70 µs/ray** (≈3× faster) | [V] |
| 10k vertical rays, heightmap 1025² | 1.78–2.24 µs (Dictionary) / 0.65–0.99 µs (`_into`), \|y−h\| 0.003 m once samples are centred: sample (x,z) sits at local `(x−(w−1)/2, z−(w−1)/2)`, row-major `heights[z*w+x]` | [V] |
| 10k 100 m near-horizontal rays (bot sight lines), concave, `_into` | 0.74–0.86 µs/ray, 8,245 hits | [V] |
| `body_test_motion`, kinematic capsule r 0.4 h 1.8, 10 m drop onto the concave mesh | 24–30 µs per call, 1,000/1,000 collided, foot error 0.035 m | [V] |
| `template_release` DLL vs `template_debug` DLL | rays ~20 % faster (0.53 vs 0.66 µs `_into`) | [V] |

Threads, 40,000 `intersect_ray_into` rays split over N `std::thread`s while the main thread blocks inside
`_physics_process` (space not stepping); each thread owns its query/result `Ref`s **[V]**:

| threads | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| rays/ms (4 runs) | 1,665–1,915 | 2,654–2,991 | **3,801–3,916** | 1,635–1,874 |

No errors, identical hit counts. 4 threads give 2.1–2.3×; 8 threads are slower than 2 (contention whose source was
not identified [I]: Jolt's query path, engine-side object locks or Ref traffic).

Race test: one worker thread casting rays continuously for 180 frames while the main loop steps the space with 500
falling rigid spheres: 1.0–1.7 M rays, no crash, and **one refused query per physics step** (72–74 `ERROR ... being
stepped` over 74–77 steps headless; 7 over 8 steps windowed). The guard is `ERR_FAIL_COND_V_MSG(space->is_stepping())`
([L452](https://github.com/godotengine/godot/blob/c971f93e7/modules/jolt_physics/spaces/jolt_physics_direct_space_state_3d.cpp#L452)),
a check, not a lock, and every query then calls `space->flush_pending_objects()`, which mutates the space [V source].
So: off-thread queries are safe only in a window the game guarantees (no step running, no bodies being added from
another thread), e.g. a job phase the main thread opens inside `_physics_process` and joins before returning [I].
`physics/3d/run_on_separate_thread` is `false` in this project [V]; the same guard exists for
`body_test_motion` ("maybe from move_and_slide?", [L867](https://github.com/godotengine/godot/blob/c971f93e7/modules/jolt_physics/spaces/jolt_physics_direct_space_state_3d.cpp#L867)) [V].

### 6.3 (c) ProjectSettings containing "jolt" (from `ProjectSettings::get_property_list()` in C++) [V]

981 settings in total, 32 contain `jolt`, all under `physics/jolt_physics_3d/` (values = defaults of this build):

| setting | value |
|---|---|
| simulation/velocity_steps, position_steps | 10, 2 |
| simulation/use_enhanced_internal_edge_removal | true |
| simulation/generate_all_kinematic_contacts | false |
| simulation/penetration_slop, speculative_contact_distance | 0.02, 0.02 |
| simulation/baumgarte_stabilization_factor | 0.2 |
| simulation/soft_body_point_radius | 0.01 |
| simulation/bounce_velocity_threshold | 1.0 |
| simulation/allow_sleep, sleep_velocity_threshold, sleep_time_threshold | true, 0.03, 0.5 |
| simulation/continuous_cd_movement_threshold, continuous_cd_max_penetration | 0.75, 0.25 |
| simulation/body_pair_contact_cache_enabled, _distance_threshold, _angle_threshold | true, 0.001, 0.0349 |
| queries/use_enhanced_internal_edge_removal | false |
| queries/enable_ray_cast_face_index | false (needed for `get_collider_face_index`, i.e. BF3 per-triangle surface materials) |
| motion_queries/use_enhanced_internal_edge_removal, recovery_iterations, recovery_amount | true, 4, 0.4 |
| collisions/collision_margin_fraction, active_edge_threshold | 0.08, 0.8727 |
| joints/world_node | 0 |
| limits/temporary_memory_buffer_size (MB), world_boundary_shape_size | 32, 2000.0 |
| limits/max_linear_velocity, max_angular_velocity | 500.0, 47.12 |
| limits/max_bodies, max_body_pairs, max_contact_constraints | **10,240**, 65,536, 20,480 |

Related: `physics/3d/physics_engine = "Jolt Physics"`, `physics/common/physics_ticks_per_second = 60`,
`physics/3d/run_on_separate_thread = false`, `navigation/baking/thread_model/baking_use_multiple_threads = true`,
`..._high_priority_threads = true`, `navigation/baking/use_crash_prevention_checks = true`,
`rendering/rendering_device/driver.windows = "d3d12"`. `limits/max_bodies` 10,240 matters: Caspian has 20k
placements; collision must be merged per cell, or the limit raised.

### 6.4 (d) TextureStreaming

API exposed to GDExtension **[V]** (API dump; [`_bind_methods` L111-126](https://github.com/godotengine/godot/blob/c971f93e7/modules/texture_streaming/texture_streaming.cpp#L111)):
`get/set_min_lod_override`, `get/set_max_lod_override`, `get/set_memory_budget_mb_override`,
`get_memory_budget_bytes_used`, `flush_texture_streaming`. Unset overrides read −1 / −1 / 4294967295.
The registration call `texture_configure_streaming(RID, format, w, h, min_lod, max_lod, Callable reload)`
([header L630](https://github.com/godotengine/godot/blob/c971f93e7/modules/texture_streaming/texture_streaming.h#L630))
is **not bound**, so an extension cannot make its own texture stream. The only streaming resource is
`StreamedTexture2D` (bound: `load(path)`, `get_load_path`, `set/get_min_lod_override`, `set/get_max_lod_override`).

Project settings ([L226-237](https://github.com/godotengine/godot/blob/c971f93e7/modules/texture_streaming/texture_streaming.cpp#L226)) [V]:
`rendering/textures/streaming/enabled` (default false, restart required: `GLOBAL_DEF_RST`), `memory_budget_enabled`
false, `memory_budget_mb` 512, `max_ops_per_second` 200, `min_lod` 0 (best mip), `max_lod` 3 (worst mip, i.e. a 2048²
texture never drops below 256²), `inactivity_decay_rate_ms` 5000.

Test: a runtime DXT1 `Image` 2048², full chain (11 mips, 2,796,216 B, each mip a distinct solid colour; CPU decompress
check mip0 red, mip1 green, mip2 blue, mip3 yellow), shown on two unshaded quads ~340 px tall: left
`StreamedTexture2D`, right `ImageTexture::create_from_image`.

| observation | result | tag |
|---|---|---|
| `ImageTexture` from runtime BC1 + mips | renders; fully resident; `get_memory_budget_bytes_used` unchanged → **runtime `ImageTexture`s do not stream** | [V] |
| GSTF file written by the extension | wrote the `StreamedTexture2D` container ourselves (magic `GSTF`, version 1, w, h, data size, `Image::Format`, mip count, (offset,size)×(mips+1), flags, streaming_min, streaming_max, reserved, data; format from [`_save_data` L265-323](https://github.com/godotengine/godot/blob/c971f93e7/scene/resources/streamed_texture.cpp#L265)) to `user://probe_dxt1_2048.stex` in 1.5–2.0 ms; `StreamedTexture2D::load("user://...")` → OK in 5.8–8.3 ms | [V] |
| does it stream? (windowed, streaming enabled) | **yes**. bytes_used 0 → 43,704 by frame 10 (= mip 3..11 chain; initial mip is `max_lod`, [L589](https://github.com/godotengine/godot/blob/c971f93e7/scene/resources/streamed_texture.cpp#L589)) → 174,776 by ~42 ms (= mip 2 chain; the quad needs LOD ≈2.6) and stays; quad moved 1,500 m away: drops back to 43,704 after ~5.0 s (one `inactivity_decay_rate_ms` step; floor = `max_lod` 3); moved back: 174,776 again within 3 s | [V] |
| RS texture memory | streaming on: +3.01 MB for both textures (ImageTexture full + streamed mip-2 chain); streaming off (`override.cfg`): +5.65 MB (two full chains) | [V] |
| visual | both quads identical colour (201,203,168) = trilinear 0.6×yellow(mip 3)+0.4×blue(mip 2) in linear, i.e. LOD 2.6 sampled correctly on both; same on Vulkan and with streaming off; uncompressed RGBA8 and untextured control quads correct | [V] |
| headless | TextureStreaming singleton exists but bytes stay 0 (no renderer, no feedback); RS info all 0 | [V] |

Consequence for the port: the `user://` cache should write converted BF3 textures as GSTF `.stex` files and load them
with `StreamedTexture2D::load`; that gives engine mip streaming without the editor importer and without touching the
BF3 install [I]. Caveats: GSTF v1 is an internal format (read rejects `version > 1`), so key the cache by engine version
as well as content digest; the editor-only roughness/normal detection flags are irrelevant at runtime; `get_mipmap_count()`/
`has_mipmaps()` on these textures reported 0 through godot-cpp although the data has mips (not investigated) [V/I].

### 6.5 (e) NavigationServer3D: `bake_from_source_geometry_data` (Recast)

Source: analytic rolling terrain (slopes ≤ ~27°), 1 m quads → 2 triangles each, via `append_arrays`. NavigationMesh:
cell_size 0.25, cell_height 0.25, agent radius 0.5, height 1.75 (1.8 in the first run), max_climb 0.5. Headless,
times wall-clock.

| bake | time | polygons / verts | walkable area | tag |
|---|---|---|---|---|
| 250 m², watershed, sync (125k tris) | 0.83–0.86 s | 161 / 127 | 62,001 m² (=249²) | [V] |
| 500 m², watershed, sync (500k tris) | 3.2–4.4 s | 152 / 127 | 249,001 m² | [V] |
| **1 km², watershed, sync (2 M tris)** | **13.8–17.1 s** | 138 / 127 | 998,001 m² (=999²: 0.5 m agent-radius erosion at the outer edge) | [V] |
| 1 km², monotone, sync | 5.8–7.2 s | 138 / 127 | 998,001 m² | [V] |
| 1 km², watershed, `bake_from_source_geometry_data_async` | 13.9–15.0 s to callback; call returns in 0.1 ms; callback runs **on the main thread**; main loop kept running (frame max 11–17 ms headless) | 138 | 998,001 m² | [V] |
| **1 km² as 16 chunks of 250 m, all async at once** (chunk geometry + 4 m margin, `filter_baking_aabb` grown by `border_size` on each side) | **1.72–1.96 s** wall; frame max 26–112 ms while the pool is busy | 2,479 | 998,001 m² (seamless) | [V] |
| same, AABB **not** grown, border 0 / 0.5 / 1 / 2 | ~2 s | 2,488–2,496 | 992,016 / 992,016 / 984,064 / 968,256 m²: each chunk loses max(agent radius, border) on every side → 1–4 m gaps between chunks | [V] |
| peak static memory during 1 km² bake | 167 MB process peak | | | [V] |

Reading: chunked async baking is 7–9× faster than a monolithic bake and is the only practical route for BF3's
multi-km² maps; `border_size` is an inward margin of `filter_baking_aabb`, so the AABB must be the chunk grown by
`border_size` (the NavigationMesh class doc and a forum answer say the same [RC]: [docs](https://docs.godotengine.org/en/4.3/classes/class_navigationmesh.html),
[forum](https://forum.godotengine.org/t/navigation-cell-border-size-always-rounded-up-instead-of-being-set-to-0/80559)).
Bake results belong in the `user://` cache keyed by level digest. The low polygon counts are an artefact of the
obstacle-free test terrain; real levels will be far denser [I].

### 6.6 (f) ENetConnection, two hosts in-process on 127.0.0.1 [V]

Server `create_host_bound("127.0.0.1", 47123, 8, 2)`, client `create_host(1, 2)` + `connect_to_host`; connected in
5.0–5.6 ms. 1,000 packets, each carrying sequence + send timestamp.

| mode | delivered | latency ms (mean / p99 / max) | notes |
|---|---|---|---|
| 100 B unreliable (flags 0), send + `service(0)` per packet | 1000/1000, in order | 0.012–0.020 / 0.020–0.036 / 0.07–0.12 | 13.5–22.4 ms for all |
| 100 B unreliable, burst of 1000 then flush | **609/1000 (39 % loss)** | 1.5–2.3 | identical with `FLAG_UNSEQUENCED` on channel 1 |
| 1200 B unreliable, burst | **55/1000 (94.5 % loss)** | 5.8–7.6 | |
| 100 B reliable, burst | 1000/1000 | 5.8–6.2 / 10.9–11.9 / 11.9 | 6.5–12.2 ms total |

Delivered bytes per burst ≈ 66 KB in both unreliable cases. Godot's ENet socket wrapper ignores `ENET_SOCKOPT_RCVBUF`
(`set_option` returns −1, [enet_godot.cpp L137-139](https://github.com/godotengine/godot/blob/c971f93e7/thirdparty/enet/enet_godot.cpp#L137))
although ENet asks for 256 KB ([host.c L65](https://github.com/godotengine/godot/blob/c971f93e7/thirdparty/enet/host.c#L65)) [V], so the
OS default UDP receive buffer applies and overflows when the receiver does not drain between sends [I; the Windows
default size was not measured]. ENet's own throttle stayed at 32/32 and reported 0 loss. For BF3 snapshots (a few KB
per client per tick, serviced every tick) this is irrelevant; for a level-join burst use reliable channels or pacing.

### 6.7 (g) engine version and precision [V]

`Engine::get_version_info()` = `{major 4, minor 8, patch 0, hex 264192, status "dev7", build "official", hash
"c971f93e7e76...", timestamp 0, string "4.8-dev7 (official)"}`; the extension was compiled against
`GODOT_VERSION 4.8.0`; `real_t` is `float` (4 bytes), no `REAL_T_IS_DOUBLE`; API header `precision: single`.
`OS::is_debug_build()` = true with the editor binary. Adapter (windowed): "AMD Radeon RX 9070 XT", D3D12 feature level
12_0 / Vulkan 1.4.349; headless reports empty adapter strings. Single precision means positions lose sub-mm precision
beyond ~8 km from origin [I]; BF3 maps (≤ ~4 km) fit, but large-coordinate levels should be recentred at load [I].

## 7. What is meaningful headless

| test | headless (dummy renderer) | windowed |
|---|---|---|
| (a) RS create/free cost | yes (CPU command cost only) | yes |
| (a) frame time, draw calls, memory | no (all `get_rendering_info` = 0; loop paces at 6.9 ms) | yes |
| (b) physics, threads, race | yes | yes (same numbers ±15 %) |
| (c) project settings | yes | yes |
| (d) TextureStreaming | no (bytes 0; no feedback) — only file write/load cost | yes |
| (e) navigation | yes | yes |
| (f) ENet | yes | yes |
| screenshots (`get_viewport()->get_texture()->get_image()`) | no | yes |

A dedicated server build can therefore be fully headless; bots, physics, nav and networking do not need a GPU.

## 8. Gotchas a new project setup must handle

1. **Pin both JSONs from the exact exe**: pass `GODOTCPP_CUSTOM_API_FILE` *and* `GODOTCPP_GDEXTENSION_DIR` (the
   latter holds `gdextension_interface.json` from `--dump-gdextension-interface-json`); the custom API option alone
   still uses godot-cpp's bundled interface JSON [V]. Re-dump and rebuild on every engine update (dev7 → RC → stable).
2. **Version gate is coarse.** godot-cpp refuses to load only when the engine's major.minor(.patch) is *older* than the
   API it was built from ([godot.cpp L158-182](https://github.com/godotengine/godot-cpp/blob/b051e694/src/godot.cpp#L158)); the
   `status` (dev7/rc1) is not compared, so a dev7-built DLL loads into any 4.8.x. A method whose hash changed between
   dev builds then fails at first call with `ERR_PRINT_ONCE("Method bind was not found. Likely the engine method
   changed to an incompatible version.")` ([error_macros.hpp L803](https://github.com/godotengine/godot-cpp/blob/b051e694/include/godot_cpp/core/error_macros.hpp#L803))
   and returns a default value, not a crash [V code; not triggered here]. Engine-side compat hashes cover released
   versions only [I]. No API-mismatch warning was printed in any run [V].
3. **`.gdextension`**: `compatibility_minimum` is mandatory and must be ≥ 4.1 (engine refuses otherwise,
   [gdextension_library_loader.cpp L324-360](https://github.com/godotengine/godot/blob/c971f93e7/core/extension/gdextension_library_loader.cpp#L324));
   set it to `"4.8"` because the code uses 4.8-only classes; `compatibility_maximum` (4.3+) can lock out 4.9. Library
   keys match feature tags in file order: `debug` matches the editor binary and debug templates, `release` only
   release templates ([docs 4.6](https://docs.godotengine.org/en/4.6/tutorials/scripting/gdextension/gdextension_file.html)) [RC].
4. **Debug vs release**: a `template_release` DLL loaded fine into the editor binary when mapped to the `debug` key
   (no cross-check) and ran ~20 % faster on ray-heavy code [V]. `template_debug` keeps `DEBUG_ENABLED` (godot-cpp
   error checks); use it for development, ship `template_release`. An `editor` target is only needed for editor
   plugins/tools code.
5. **`extension_list.cfg`**: a fresh checkout must be opened once by the editor or run with `--import` before the
   game can load its extension; `.godot/` is gitignored in this project (`.gitignore`), so CI must do the same [V].
   Godot also writes `<name>.gdextension.uid` next to the file (keep it in VCS) [V].
6. **Hot reload**: needs `reloadable = true` in `.gdextension` *and* godot-cpp built with `GODOTCPP_USE_HOT_RELOAD=ON`
   (default off). In the editor, the engine opens a temp copy of the DLL (`generate_temp_files = is_editor_hint()`,
   [loader L202](https://github.com/godotengine/godot/blob/c971f93e7/core/extension/gdextension_library_loader.cpp#L202)) so the
   linker can overwrite the original; a running game (non-editor) locks the DLL, so rebuild only with the game closed
   [V source; hot reload itself not exercised]. The docs say reloadable is for development only [RC].
7. **Short build path** (MAX_PATH, §5 #1), static CRT `/MT`, no exceptions: third-party C++ libs linked into the
   extension must use the same CRT and not rely on exceptions [V settings; I consequence].
8. **Windows console exe**: `Godot_v4.8-dev7_win64_console.exe` is a wrapper that starts `Godot_v4.8-dev7_win64.exe`
   as a child; killing a hung run needs both PIDs; piping its stdout through a truncating reader (PowerShell
   `Select-Object -First`) stopped the run early [V]. Use `--quit-after`/own quit logic and log to files.
9. **Navigation**: use `append_arrays`, give raw triangles Recast winding, keep agent values on the voxel grid,
   bake in chunks with the AABB grown by `border_size` (§5, §6.5).
10. **Jolt**: queries are main-thread (or caller-guarded) only; prefer the new `*_into` query variants with reused
    result objects; mind `limits/max_bodies` = 10,240 and `queries/enable_ray_cast_face_index` = false (§6.2–6.3).

## 9. Could not determine

* Hot reload end-to-end (editor GUI + rebuild) was not exercised; only the source path and flags were read.
* Whether any of the 4.8 dev7 methods used here change hash before RC/stable (cannot be tested without the RC).
* Why 8 query threads are slower than 2–4 (lock/contention source not profiled).
* GPU-side cost split (frame times are CPU-side between `_process` calls; no GPU timestamps); `objects=11100` for
  10,000 instances (+1,100) was not broken down by pass.
* Why `has_mipmaps()` / `get_mipmap_count()` return 0 through godot-cpp for both mipmapped textures.
* TextureStreaming behaviour under a memory budget (`memory_budget_enabled`), with thousands of textures, or the
  frame cost of the feedback pass; and whether GSTF v1 survives into 4.8 stable unchanged.
* The Windows default UDP receive buffer size (the ENet loss explanation rests on Godot ignoring `RCVBUF`, verified,
  plus the OS default, not measured).
* Navigation bake time on real BF3 collision (buildings, many regions), and agent/edge parameters that match BF3
  soldier movement.
* One headless chunk run's log ended mid-run with no error and no exit code captured; an identical rerun completed
  (exit 0). Cause unknown (likely the harness, not the engine).
* Win32 long-path support as an alternative to the short build root (system setting, deliberately not changed).
