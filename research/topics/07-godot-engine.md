# 07 — Godot 4.8 as the platform: engine state, GDExtension/godot-cpp, threading, precision, server, runtime pipeline, build

Written 2026-10-10 for the BF3 multiplayer reimplementation (Godot 4.8, C++ only, BF3 install read as-is, derived data
only under `user://`). Scope: the platform questions. Level/render data formats are in the IW4L handoff and are not
repeated here.

**Evidence legend.** [verified] = checked by me in the local build, its API dump, the Godot source at the exact dev7
commit, the godot-cpp source, local data, or a primary page I read. [report claim] = stated by a source I did not
re-check. [inferred] = my reading or arithmetic.

**Abbreviations used in citations.**

* `API` = `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json` (dumped from the local dev7;
  header `version_full_name "Godot Engine v4.8.dev7.official"`, `precision "single"`, lines 2-10). `IFH` =
  `...\research\godot-api\gdextension_interface.h` beside it.
* `G:path:line` = Godot source at the dev7 commit, permalink
  `https://github.com/godotengine/godot/blob/c971f93e7e76b0ef919bf6009e7b868bea04db7f/<path>#L<line>`
  (I fetched that exact commit; `git log -1` = "c971f93e7e76… Tue Sep 29 2026 Merge pull request #123951").
* `C:path:line` = godot-cpp master at `b051e694cf493d417f5aa945c656cb77ae9011a9` (2026-10-09), permalink
  `https://github.com/godotengine/godot-cpp/blob/b051e694cf493d417f5aa945c656cb77ae9011a9/<path>#L<line>`.
* `DOC` = class reference XML dumped from the local editor with
  `Godot_v4.8-dev7_win64_console.exe --headless --doctool <dir>` (1,109 XML files; descriptions are empty in a doctool
  dump, so setting descriptions are quoted from `G:doc/classes/ProjectSettings.xml`).
* Local exe banner: `Godot Engine v4.8.dev7.official.c971f93e7` (`--help`) [verified].

---

## 0. Answers in one page

| question | answer | tag |
|---|---|---|
| 4.8 status | dev7 (commit c971f93e7, built 2026-10-01) is the **feature-freeze** snapshot; no beta/RC published as of 2026-10-10. By the 4.6/4.7 cadence, beta ≈ mid-Oct, RC ≈ Nov, stable ≈ late Nov–Dec 2026 | verified / inferred |
| texture streaming | Only `StreamedTexture2D` resources loaded **from a file** in the new `GSTF` (`.stex`) format stream. `ImageTexture`/`RenderingServer.texture_2d_create` textures do not, and the hook that would let them (`texture_2d_attach_streaming_state`, `TextureStreaming::texture_configure_streaming`) is not exposed to GDExtension. **Route: write GSTF files into `user://` from BF3 BC data (no re-encoding) and `StreamedTexture2D.load()` them.** 2D only (no arrays/cubes/3D/lightmaps), Forward+/Mobile only | verified |
| godot-cpp | godot-cpp is now versioned independently: **v10.0.0-stable (2026-09-15)**, master targets API 4.3–4.7 from bundled JSON; for 4.8-dev7 pass the dumped JSON (`GODOTCPP_CUSTOM_API_FILE` / `custom_api_file=`). dev7's interface JSON is identical to godot-cpp's bundled one (179 functions, none `@since 4.8`) | verified |
| module vs extension | GDExtension for everything; an engine module only buys (a) streaming of runtime textures without a disk cache, (b) access to Godot's own Jolt instance, (c) renderer internals. None is needed if we run our own Jolt and write GSTF | verified / inferred |
| precision | **Single.** BF3 bot waypoints reach 2,252 m from origin (Nebandan Flats), terrains ±4,096 m. float32 ulp: 61 µm @1 km, 122 µm @2 km, 244 µm @4 km, 488 µm @8 km. Only near-camera geometry (viewmodel/cockpit) shows it (≈1 px at 2.25 km, 1440p/90°); fix with a camera-relative vertex path, not a double engine | verified data / inferred |
| threading | Render runs on the main thread ("Safe" model; "Separate" is experimental). Godot physics queries are main-thread-only and spaces cannot be stepped individually, so **the authoritative sim cannot run on its own thread on Godot's PhysicsServer3D → link our own Jolt** into the extension and run the server tick on a dedicated thread | verified |
| dedicated server | `--headless` = headless display + dummy audio + dummy renderer. Dummy renderer **keeps CPU copies** of every texture/mesh created, so the server must not create render resources. Exported servers use normal templates with the `dedicated_server` feature | verified |
| runtime pipeline | Plain C++ loaders (no `ResourceFormatLoader` needed) → RenderingServer RIDs; cache blobs under `user://bf3cache/` keyed by the CAS SHA1 of the source payload + converter/engine/Jolt versions; meshes as pre-packed Godot surface buffers (`RenderingServer.mesh_add_surface(Dictionary)`), collision as Jolt binary shape streams, textures as GSTF | verified API / inferred design |
| layout | `core` (BF3 readers, no Godot) + `sim` (gameplay + own Jolt, no Godot) + `net` + `bf3_godot` GDExtension + Godot project; CMake with godot-cpp added first; doctest for the Godot-free libs; headless Godot tests driven by a C++ `MainLoop` (`--main-loop`) | verified mechanisms / inferred design |

---

## 1. Godot 4.8 status (as of 2026-10-10)

### 1.1 Timeline [verified: `gh api repos/godotengine/godot-builds/releases`, release blog posts]

| line | dev snapshots | beta | RC | stable |
|---|---|---|---|---|
| 4.6 | dev1 2025-09-30 … dev6 2025-12-05 | beta1 2025-12-10, beta3 2026-01-07 | rc1 2026-01-14, rc2 01-20 | **2026-01-26** (4.6.3 on 05-20) |
| 4.7 | dev1 2026-02-13 … dev5 04-17 | beta1 04-24 … beta5 06-03 | rc1 06-08 … rc3 06-15 | **2026-06-18** (4.7.1 07-14, 4.7.2 08-18) |
| 4.8 | dev1 07-06, dev2 07-21, dev3 08-07, dev4 08-26, dev5 09-10, dev6 09-15, **dev7 10-01** (commit c971f93e7e76b0ef919bf6009e7b868bea04db7f, blog dated 09-29) | none yet | none yet | — |

* dev7 is the feature freeze: "no new features will be integrated for Godot 4.8 moving forward" (dev7 post,
  https://godotengine.org/article/dev-snapshot-godot-4-8-dev-7/) [verified].
* Last dev → stable took 9 weeks for 4.7 and 7.5 weeks for 4.6, so 4.8 stable lands around late November to mid
  December 2026; the RC around November [inferred from the table].

### 1.2 dev7 vs the coming RC [verified: `gh search prs --merged-at ">2026-09-29"`]

127 PRs merged to master after the dev7 commit up to 2026-10-09. The ones that touch this port:

| PR | merged | effect on us |
|---|---|---|
| GH-123662 "Fix triplanar texturing streaming and other custom feedback" | 10-08 | **Replaces the spatial-shader built-in `STREAMING_UV` (dev7) with a single `STREAMING_LOD` output plus helper functions** (simple UV, gradients, manual mip). Shaders written against dev7's `STREAMING_UV` will not compile in the RC |
| GH-124080 "Consolidate Jolt decorator shapes into one and make it a leaf shape" | 10-02 | internal to Godot's Jolt module |
| GH-123602 "Fix CompressedTexture2D mipmap reporting" | 09-30 | texture info |
| GH-117078 "Change native MultiMesh buffer manipulation methods accessibility to public" | 10-05 | engine-internal C++ (module code only) |
| GH-123312, GH-124058 | 10-08/09 | shader compilation deadlock fix (Mobile), compressed embedded shaders |

Upgrade procedure dev7 → RC: dump the RC's `extension_api.json` and `gdextension_interface.json`, run
`<rc> --validate-extension-api <dev7 extension_api.json>` (option listed in `--help`) [verified option exists], rebuild
godot-cpp bindings from the RC dump, grep our shader sources for `STREAMING_UV`.

### 1.3 Texture streaming (GH-113429) — what it really covers

Facts [verified unless noted]:

* PR GH-113429 "Implement texture mip-level streaming" (tdaven), opened 2025-12-02, merged 2026-08-27, shipped in dev5
  (2026-09-10 post). PR text: targeted at 3D; "Currently not implemented for 3d textures or lightmaps"; uses a
  material feedback buffer; Forward+ and Mobile only, not Compatibility; no sparse/partially resident textures; mip swaps
  re-upload the whole remaining chain; D3D12/Vulkan fragmentation untested [report claim from the PR page].
* Classes in the dev7 API: `TextureStreaming` singleton (`API` singletons list; methods `get/set_min_lod_override`,
  `get/set_max_lod_override`, `get/set_memory_budget_mb_override`, `get_memory_budget_bytes_used`,
  `flush_texture_streaming`, signal `flush_completed`); `StreamedTexture2D : Texture2D` (instantiable; `load(path) ->
  Error`, `load_path`, `min_lod_override`, `max_lod_override`); editor-only `ResourceImporterStreamedTexture`
  (import type "Texture2D Streamed"). Nothing else: **no RenderingServer streaming method is bound**; the RS has
  `texture_2d_attach_streaming_state` internally (`G:servers/rendering/rendering_server.h:155`) but it is absent from
  `API`. `TextureStreaming::texture_configure_streaming(RID, format, w, h, min, max, Callable reload)` exists in
  `G:modules/texture_streaming/texture_streaming.h:630` and is not bound (`texture_streaming.cpp:111-126` binds only the
  members above).
* **Only textures loaded by `StreamedTexture2D` stream.** `StreamedTexture2D` reads a file through `FileAccess` (works with
  `user://` and absolute paths) in the GSTF format; `ResourceFormatLoaderStreamedTexture2D` recognises `.stex`
  (`G:scene/resources/streamed_texture.cpp:626-635`). `ImageTexture` / `RenderingServer.texture_2d_create` textures have no
  streaming state and are fully resident. There is no `FileAccess` extension point in `API` (only `FileAccess`,
  `DirAccess`, `PCKPacker`, `ZIPReader/Packer`), so the file must exist on disk.
* GSTF v1 layout (`G:scene/resources/streamed_texture.cpp:128-175` read, `265-325` write), little-endian `u32`s:
  `"GSTF"`, `version=1`, `width`, `height`, `data_size`, `Image::Format` enum value, `num_mipmaps` (count excluding mip 0,
  ≤ 16), then `(num_mipmaps+1) × {offset, size}` relative to the data start, `flags` (bit0 detect-normal, bit1
  detect-roughness, editor only), `streaming_min`, `streaming_max` (0 = project setting, else LOD+1), `reserved`, then the
  raw `Image` data. Width/height ≤ 16384. Any `Image::Format` is accepted, so BC data goes in **verbatim**:
  `FORMAT_DXT1=17, DXT3=18, DXT5=19, RGTC_R=20, RGTC_RG=21, BPTC_RGBA=22` (`API` enum `Image.Format`).
* Loading a mip level builds an `Image` from mip *k* to the end of the chain with `Image::set_data(..., mipmaps=true)`
  (`streamed_texture.cpp:260`), and `Image` rejects data whose size is not the exact full chain down to 1×1
  (`G:core/io/image.cpp:2554-2569`). **A GSTF must hold the complete chain to 1×1.** Swaps go through
  `RenderingServer.texture_replace_compatible` so the texture RID stays the same (`streamed_texture.cpp:105-119`).
* Mip choice: fragment shaders of materials that bind a streamed texture write the required LOD from UV derivatives
  (anisotropy-aware, `MAX_ANISOTROPY 16`) with `atomicMax` into a per-instance material feedback slot
  (`G:servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl:1361-1402`); the material
  storage registers every texture uniform whose texture has a streaming state, for any material type incl.
  `ShaderMaterial` (`G:servers/rendering/renderer_rd/storage_rd/material_storage.cpp:1001-1002, 1041-1043`). In dev7 the
  UV used is the shader's `UV` unless it writes `STREAMING_UV` (`G:servers/rendering/shader_types.cpp:149`); in the RC it
  becomes `STREAMING_LOD` (§1.2). One feedback value per material per instance: textures sampled with other UVs (UV2
  overlays, ×8 detail tiling) inherit the base UV's request [inferred from the shader: one slot per material].
* Threads: a feedback-processing thread "TexStreaming" and an I/O thread "TexStreaming IO"
  (`texture_streaming.cpp:290-291, 565, 769`); public API is thread-safe (`texture_streaming.h:39-45`).
* Residency: each texture keeps a mip in `[min_lod, max_lod]` (0 = full res); per-texture overrides intersect the
  system range; feedback raises quality, inactivity decays it by one mip per `inactivity_decay_rate_ms`, and a budget
  pass lowers the least important textures with a ±5 % hysteresis (`texture_streaming.cpp:602-700`).
* Project settings (`DOC` ProjectSettings; defaults): `rendering/textures/streaming/enabled=false` (restart required,
  `GLOBAL_DEF_RST`, `texture_streaming.cpp:226`), `max_lod=3` ("maximum LOD … that streamed textures can load. Lower
  values allow higher resolution textures but use more VRAM"), `min_lod=0`, `memory_budget_enabled=false`,
  `memory_budget_mb=512`, `max_ops_per_second=200`, `inactivity_decay_rate_ms=5000`. All but `enabled` can be changed at
  runtime (re-read on `settings_changed`, `texture_streaming.cpp:252, 270-277`).
* Follow-ups open on 2026-10-10: GH-123743 (import DDS as streamed), GH-116047 (faster `texture_replace`),
  GH-123107; `sampler2DStreamed` was discussed for later (PR thread) [verified via `gh search prs`; report claim for the
  discussion].

What it means for BF3 [inferred]:

| route | how | pros | cons |
|---|---|---|---|
| A. **GSTF cache** (recommended for streaming) | decode the DxTexture chunk (zlib only; BC stays BC), pad the chain to 1×1 if BF3 stops early, write `user://bf3cache/tex/<sha1>.stex`, `StreamedTexture2D::load()` | engine-driven feedback streaming, no module, budget via `TextureStreaming` | duplicates texture bytes on disk (Caspian ≈ 1.56 GiB per the handoff, deduplicated by SHA1 across levels); GSTF is version 1 and may change before stable → key the cache by engine version and probe-load one file at startup |
| B. **Fully resident** | `Image::create_from_data(BC)` → `ImageTexture`/`texture_2d_create` straight from the CAS decode | no disk cache; ≈2 s decode per level on all cores (handoff 01 §16, report claim) | full VRAM (≈1.56 GiB Caspian [report claim]); fine on 12–16 GB GPUs |
| C. own residency | `texture_2d_create(lower-mip Image)` + `texture_replace_compatible(old, new)` (bound in 4.8, `API`) driven by our own CPU distance/screen-size heuristic | no disk cache, works for arrays too | we write the policy; no GPU feedback |

Start with B (M1), add A once look work is stable; keep C as the fallback if GSTF changes. Texture2DArray (terrain
layers), cube maps, the 3D grading LUT and Enlighten atlases stay resident under every route.

### 1.4 Other 4.5–4.8 features that matter here

| version | feature (PR) | relevance | tag |
|---|---|---|---|
| 4.4 | Jolt as alternative 3D engine (GH-99895, merged 2024-12-11); ubershaders + pipeline pre-compilation (GH-90400, 2024-10-03) | physics backend; fewer first-use hitches | verified (gh) |
| 4.5 | Shader baker (GH-102552, 2025-05-28): exporter pre-compiles shaders of materials found in exported resources/scenes (`G:editor/export/shader_baker/shader_baker_export_plugin.cpp:277-298`) | ship our material-family `.gdshader` files and one material per variant in the project so they get baked; runtime-generated shader code is not baked | verified |
| 4.5 | SMAA 1x (GH-102330), stencil buffer (GH-80710), specular occlusion (GH-106145), bent normals (GH-89988); 3D physics interpolation moved to SceneTree (GH-104269, 2025-04-28); BoneConstraint3D family (GH-100984); GDExtension main-loop callbacks (GH-106030, 2025-06-10) | AA/look; interpolation is node-only now | verified (release page, gh) |
| 4.6 | Jolt default for new projects (GH-105737, 2025-09-30); IK framework `IKModifier3D` with TwoBoneIK3D, ChainIK3D, FABRIK3D, CCDIK3D, SplineIK3D, JacobianIK3D (GH-110120, 2025-11-04); SSR overhaul (GH-111210); glow before tonemap (GH-110671); D3D12 default for new Windows projects (GH-113213); GDExtension interface defined in JSON (GH-107845, 2025-11-24); profiler builds with Tracy/Perfetto (`profiler_path`, GH-104851, 2025-11-12); delta-encoded patch PCKs (GH-112011); LibGodot (GH-110863) | IK for soldiers/weapons; build-time tracing | verified (release page, gh) |
| 4.7 | HDR output; AreaLight3D (GH-108219); per-pass environment UBOs (GH-115177); RenderingDevice ray tracing (`blas_create`, `tlas_create`, `raytracing_pipeline_create`, `raytracing_list_*` in `API`) | RT available to compute/compositor work | verified (`API` diff vs godot-cpp's 4.5 JSON) |
| 4.8 dev1–7 | texture streaming (dev5); multi-bounce AO (GH-115426, dev4; the project already sets `lights_and_shadows/multi_bounce_occlusion/enabled=true`); directional lightmap specular (GH-109737); `Trail3D` (GH-117107); screen-space contact shadows for directional lights (GH-118045, dev6); animated decals (GH-115653); **non-allocating physics queries** `intersect_ray_into`, `cast_motion_into`, … with reusable `Physics*Result3D` objects (GH-113970, merged 2026-09-22); `BoneSpaceAdjuster3D`, bone `skin_scale`; `AnimationNodeObserver*`; Jolt 5.6.0 (GH-121260, dev2); `RenderingServer` adds `mesh_surface_get_{vertex,attribute,skin,index}_buffer_rd_rid`, `texture_replace_compatible`, `light_set_allow_contact_shadows` | RD buffer RIDs let compute shaders read mesh buffers (GPU skinning/scatter); non-allocating queries for any remaining Godot-physics raycasts | verified (`API` diff vs godot-cpp's 4.7 JSON; dev posts) |
| — | Mesh streaming | none exists (no PR found) | verified (`gh search prs "mesh streaming"`) |
| — | Multiplayer | no high-level multiplayer or ENet feature in 4.5–4.8 release pages; ENet 1.3.18 bundled (`G:thirdparty/README.md:243-246`); new `StreamPeerUDS`/`UDSServer`/`SocketServer` classes | we use our own protocol anyway | verified |
| — | Navigation | async region/map iterations (`navigation/world/{map,region}_use_async_iterations=true`), `bake_from_source_geometry_data_async`, `NavigationMeshSourceGeometryData3D.set_vertices/set_indices/append_arrays`, source-geometry parser callbacks; no NavigationServer3D method added since 4.5; Recast 1.6.0 bundled (`G:thirdparty/README.md:957-960`) | bake from our own collision arrays, headless-capable | verified (`API`, `DOC`) |

Anti-aliasing available [verified `API` RenderingServer enums]: MSAA 2×/4×/8×, FXAA, SMAA, TAA, FSR2 temporal
upscaling (`VIEWPORT_SCALING_3D_MODE_FSR2`), plus `screen_space_roughness_limiter`. Occlusion culling: CPU raster
occluders, created at runtime with `RenderingServer.occluder_create/occluder_set_mesh(vertices, indices)` and
enabled by `rendering/occlusion_culling/use_occlusion_culling` (default false) — BF3's own occluder meshes can feed it.
Compositor: `compositor_effect_*`, callback points pre-opaque … post-transparent.

### 1.5 Jolt in Godot 4.8 dev7 [verified]

* Jolt 5.6.0 (`G:thirdparty/README.md:512-515`). Godot's Jolt server is always wrapped in `PhysicsServer3DWrapMT`
  (`G:modules/jolt_physics/register_types.cpp:44-51`), steps through a `JPH::JobSystem` built on `WorkerThreadPool`
  native tasks (`G:modules/jolt_physics/spaces/jolt_job_system.cpp:106,163`). **No Jolt class is exposed**: no
  `VehicleConstraint`, `CharacterVirtual`, tracked or motorcycle controllers, although those Jolt sources are compiled
  into the engine (`G:thirdparty/jolt_physics/Jolt/Physics/Vehicle/*`, referenced only by `SCsub`); Godot's
  `VehicleBody3D` is its own raycast vehicle (`G:scene/3d/physics/vehicle_body_3d.cpp:485`).
* `precision=double` builds compile Jolt with `JPH_DOUBLE_PRECISION` (`G:modules/jolt_physics/SCsub:156-157`).
* Jolt-specific limits seen in code: shape casts only with convex shapes; `cast_motion` cannot return rest info; queries
  fail while the space is stepping (`G:modules/jolt_physics/spaces/jolt_physics_direct_space_state_3d.cpp:62,452,629`).
* All `physics/jolt_physics_3d/*` settings in dev7 with defaults (`DOC` ProjectSettings):

| setting | default | setting | default |
|---|---|---|---|
| collisions/active_edge_threshold | 0.87266463 (50°) | simulation/allow_sleep | true |
| collisions/collision_margin_fraction | 0.08 | simulation/baumgarte_stabilization_factor | 0.2 |
| joints/world_node | 0 | simulation/body_pair_contact_cache_angle_threshold | 0.034906585 |
| limits/max_angular_velocity | 47.12389 rad/s | simulation/body_pair_contact_cache_distance_threshold | 0.001 |
| limits/max_bodies | 10240 | simulation/body_pair_contact_cache_enabled | true |
| limits/max_body_pairs | 65536 | simulation/bounce_velocity_threshold | 1.0 |
| limits/max_contact_constraints | 20480 | simulation/continuous_cd_max_penetration | 0.25 |
| limits/max_linear_velocity | 500.0 m/s | simulation/continuous_cd_movement_threshold | 0.75 |
| limits/temporary_memory_buffer_size | 32 (MiB) | simulation/generate_all_kinematic_contacts | false |
| limits/world_boundary_shape_size | 2000.0 | simulation/penetration_slop | 0.02 |
| motion_queries/recovery_amount | 0.4 | simulation/position_steps | 2 |
| motion_queries/recovery_iterations | 4 | simulation/sleep_time_threshold | 0.5 |
| motion_queries/use_enhanced_internal_edge_removal | true | simulation/sleep_velocity_threshold | 0.03 |
| queries/enable_ray_cast_face_index | false | simulation/soft_body_point_radius | 0.01 |
| queries/use_enhanced_internal_edge_removal | false | simulation/speculative_contact_distance | 0.02 |
| | | simulation/use_enhanced_internal_edge_removal | true |
| | | simulation/velocity_steps | 10 |

Notes: `max_linear_velocity` 500 m/s is below rifle muzzle velocities, so bullets must be queries, not bodies
[inferred]. `enable_ray_cast_face_index` is needed to get BF3 per-triangle physics materials from `ConcavePolygonShape3D`
hits (+~25 % memory, Godot Jolt docs https://docs.godotengine.org/en/latest/tutorials/physics/using_jolt_physics.html)
[report claim]. Godot's docs call Jolt with `run_on_separate_thread` "experimental" [report claim, same page].

---

## 2. godot-cpp and GDExtension for 4.8

### 2.1 Which godot-cpp [verified]

* godot-cpp is versioned independently since v10: "Starting with version 10.x, godot-cpp is versioned independently from
  Godot"; v10 "can target Godot 4.3 or later" via `api_version` (README, https://github.com/godotengine/godot-cpp).
  Releases (GitHub API): 10.0.0-rc1 2026-01-26, rc2 2026-08-25 (API JSON updated to 4.7-beta1; `memnew(RefCounted)`
  returns `Ref<T>`; hot reload opt-in; `api_version` required; Variants 8-byte aligned; `godot::` → `::godot::`),
  **10.0.0-stable 2026-09-15**. The old per-Godot branches (`4.5`, …) are superseded.
* Bundled API files: `gdextension/extension_api-4-{3,4,5,6,7}.json` + one `gdextension_interface.json`; SCons
  `supported_api_versions = ["4.3"…"4.7"]` (`C:tools/godotcpp.py:180`), CMake `GODOTCPP_API_VERSION` strings
  `;4.3;4.4;4.5;4.6;4.7` (`C:cmake/godotcpp.cmake:131-136`). **No 4.8 JSON in master as of 2026-10-09.**
* For dev7/RC: `GODOTCPP_CUSTOM_API_FILE=<dump>` (CMake, "takes precedence over GODOTCPP_GDEXTENSION_DIR and
  GODOTCPP_API_VERSION", `C:cmake/godotcpp.cmake:142-146, 272-275`) or `scons custom_api_file=<dump>`. The interface
  JSON is always read from `GODOTCPP_GDEXTENSION_DIR/gdextension_interface.json` (`C:cmake/godotcpp.cmake:289`).
  I dumped dev7's (`--dump-gdextension-interface-json`) and compared: identical `format_version 1`, 147 types and 179
  interface functions by name. `IFH` has no `@since 4.8` entry (newest are `@since 4.7`: `classdb_construct_object3`,
  `classdb_register_extension_class6`, `variant_get_type_by_name`). So bundled interface + dev7 API JSON is a correct
  pairing today; for the RC re-dump both and put them in our own `GODOTCPP_GDEXTENSION_DIR`.
* There is no single "interface version number": compatibility is the engine version from `get_godot_version2`
  (`@since 4.5`) plus per-function `@since` tags; godot-cpp refuses to load into an *older* Godot than it was generated
  for, comparing major.minor.patch only (`C:src/godot.cpp:161-179`).
* The dev7 API has 33 classes more than 4.7 (incl. `StreamedTexture2D`, `TextureStreaming`, the `Physics*Result3D`
  family, `Trail3D`, `BoneSpaceAdjuster3D`) and none removed [verified diff].

### 2.2 CMake vs SCons [verified]

* Godot docs: CMake is "actively supported" but "considered secondary to the SCons build system"
  (https://docs.godotengine.org/en/latest/tutorials/scripting/cpp/build_system/cmake.html).
* CMake target `godot-cpp` (alias `godot::cpp`), C++17 (`C:cmake/godotcpp.cmake:380-387, 406`). Options:
  `GODOTCPP_TARGET` (template_debug|template_release|editor), `GODOTCPP_PRECISION` (single|double),
  `GODOTCPP_USE_HOT_RELOAD`, `GODOTCPP_DISABLE_EXCEPTIONS` (ON), `GODOTCPP_BUILD_PROFILE` (trim the API to the
  classes we use), `GODOTCPP_SYMBOL_VISIBILITY` (hidden), `GODOTCPP_ENABLE_TESTING` (`C:cmake/godotcpp.cmake:123-204`).
* MSVC: godot-cpp sets `CMAKE_MSVC_RUNTIME_LIBRARY` to static `/MT` unless `GODOTCPP_USE_STATIC_CPP=OFF`
  (`C:cmake/windows.cmake:74-87`); the docs say to include godot-cpp before other dependencies so the value propagates.
  Every static lib we link (core, sim, Jolt, zlib) must use the same runtime.
* SCons gained `production=yes` on 2026-10-08 (`C:tools/godotcpp.py:396, 471`).

### 2.3 `.gdextension`, compatibility, hot reload [verified]

* Required keys `configuration/entry_symbol` and `configuration/compatibility_minimum` (≥ 4.1)
  (`G:core/extension/gdextension_library_loader.cpp:317-343`). Check: engine ≥ minimum, by major.minor.patch.
  Optional `compatibility_maximum`; a missing part means 9999 (`:363-393`). **The status (dev7 vs rc1 vs stable) is not
  compared**, so a dev7-built library loads silently into 4.8.0-rc/stable. Method binds are then looked up by
  name + hash on first call (`C:binding_generator.py:2202`); a changed method yields "Method bind was not found. Likely
  the engine method changed to an incompatible version." in debug builds (`C:include/godot_cpp/core/error_macros.hpp:801-805`)
  and is unchecked in release. **Rebuild against each new build's dump.** Use `compatibility_minimum = "4.8"`.
* Hot reload: `configuration/reloadable = true`, read only in editor builds (`gdextension_library_loader.cpp:405-407`),
  plus godot-cpp built with `GODOTCPP_USE_HOT_RELOAD=ON` (defines `HOT_RELOAD_ENABLED`, `C:cmake/common_compiler_flags.cmake:169`).
  godot-cpp's "Fix for hot reload crash" (79c37b2, authored 2026-10-04, merged 2026-10-09 via PR #2060, contained in
  b051e69) — pin at or after it if relying on reload.
  Our threads (server tick, loaders) must be joined in the deinitialize callback or reload will crash [inferred].
* Windows: in the editor Godot loads a hidden copy `~<name>.dll` next to the original and copies/renames the PDB, so the
  DLL can be rebuilt while the editor runs (`G:platform/windows/os_windows.cpp:486-509`).
* MSVC debugging [inferred from the above]: build godot-cpp + extension as `template_debug` with `/Zi`; in Visual Studio
  set the debug command to `Godot_v4.8-dev7_win64_console.exe --path <project>` (game) or add `-e` (editor), or attach to
  the running game process (the editor launches the game as a separate process). godot-cpp ships natvis files
  (`C:natvis/`). Official engine builds come without engine PDBs I could find, so engine frames are unsymbolised
  (see §9).
* Registration: `GDREGISTER_CLASS` (runs in the editor too → tool classes), `GDREGISTER_RUNTIME_CLASS` (placeholder in the
  editor, runs only in the game), `GDREGISTER_ABSTRACT/VIRTUAL/INTERNAL_CLASS` (`C:include/godot_cpp/core/class_db.hpp:387-391`);
  editor plugins via `EditorPlugins::add_by_type<T>()` (`C:include/godot_cpp/classes/editor_plugin_registration.hpp:50`).
  GDExtension main-loop callbacks (startup, shutdown, per-frame after all `_process`) exist since 4.5 (`IFH:779-793`).

### 2.4 Pitfalls [verified where cited, else inferred]

1. **ABI pinning to dev builds**: see §2.3. Store the engine version string + commit + SHA256 of the exe next to the
   dumped JSON and fail the CMake configure if the dump's `header.version_full_name` differs from the pin [inferred].
2. **Variant overhead**: engine methods are called by `ptrcall` with native argument types; Variant boxing happens only
   for `Variant`, `Array`, `Dictionary`, `Callable` and packed-array parameters. Packed arrays are copy-on-write
   engine allocations, so building a `PackedByteArray` from a `std::vector` is one allocation + memcpy. Cross the boundary in
   bulk (`multimesh_set_buffer`, `mesh_add_surface(Dictionary)`), never per vertex/bone in hot loops [inferred].
3. **`memnew`/`Ref`**: in v10 `memnew` of a `RefCounted` returns `Ref<T>` (`C:include/godot_cpp/core/memory.hpp:123-133`,
   rc2 notes). `Ref` refcounting is atomic; pass `const Ref<T>&`. Never `delete` engine objects; `memdelete` non-refcounted
   ones only when not in the tree [inferred].
4. **StringName**: construction interns through a global table; cache `StringName`s as statics/members for
   `instance_geometry_set_shader_parameter`, `call`, signals [inferred].
5. **Exceptions are off** in godot-cpp (`GODOTCPP_DISABLE_EXCEPTIONS ON`); keep the Godot-free libraries exception-free
   too (error codes / `std::expected`) so nothing throws across the boundary [inferred].
6. **Threads**: see §4.2 for which engine calls are legal off the main thread. godot-cpp's lazy method-bind statics are
   C++11 thread-safe statics [inferred]. `WorkerThreadPool::add_native_task(void(*)(void*), void*, high_priority, desc)` and
   `add_native_group_task` avoid `Callable` (`C:src/classes/low_level.cpp:62-67`).
7. **Second Jolt copy** (if we link our own): on Windows each DLL has its own symbols; on Linux Godot is not linked with
   `-rdynamic`/`--export-dynamic` (no hit in `G:platform/linuxbsd/detect.py`, `G:SConstruct`) and godot-cpp defaults to
   hidden visibility, so no interposition is expected [inferred]. Do not pass Jolt objects between the two.

### 2.5 GDExtension vs engine module vs custom engine

| need | GDExtension | engine module (custom build) |
|---|---|---|
| all BF3 readers, sim, net, bots, RS-level drawing, materials, `CompositorEffect`, RD compute/ray tracing, editor tools | yes | yes |
| texture streaming of runtime textures without a disk cache | no (needs unbound `texture_configure_streaming` + `texture_2d_attach_streaming_state`) | yes |
| Jolt vehicles/characters inside Godot's own physics space | no (no Jolt API exposed) | yes (via `JoltSpace3D` internals) |
| own Jolt instance (vehicles, `CharacterVirtual`, shape caching, determinism) | yes (vendor Jolt) | yes |
| new shader built-ins, renderer passes beyond compositor callbacks, new Variant types | no | yes |
| `precision=double` | needs a double engine build + godot-cpp `precision=double` anyway | same |
| upgrade cost | regenerate bindings; forward-compatible within 4.x ("GDExtensions targeting an earlier version of Godot should work in later minor versions", godot-cpp README) | rebuild editor + every export template per platform; internal APIs change without notice (e.g. dev7's `STREAMING_UV` → RC `STREAMING_LOD`) |
| build time | godot-cpp bindings once per pin (minutes), our code incremental [inferred] | full engine build tens of minutes, header edits rebuild most of the engine [inferred] |

Conclusion [inferred]: nothing in the plan requires a module. The two module-only wins (cacheless streaming, Godot's own
Jolt) are better solved by GSTF files and our own Jolt.

---

## 3. Precision

### 3.1 How far BF3 MP play reaches from the origin [verified data]

From the unfun-bots waypoint graphs (`C:\Users\olku\unfun-bots\mapfiles\*.map`, columns `transX;transY;transZ` in BF3
world metres; 175 MP level/mode files with points, 29 levels; script in my scratch folder): median max(|x|,|z|) per file
465 m; 92 files ≤ 512 m; 22 > 1,024 m; 3 > 2,048 m. Per level maximum > 1 km: XP5_002 Nebandan Flats 2,252 m (Rush
Large: x −2,252…−1,649), XP5_004 Sabalan Pipeline 1,496 m, XP3_Alborz 1,327 m, XP3_Desert 1,318 m, XP5_003 Kiasar 1,275 m.
Waypoints are infantry routes; jets and the combat-area edge go further. Terrains are 4,096 or 8,192 m wide
(handoff `sections/03-level-and-look.md:22-34`), Bazaar's root bbox is ±512 (`02-resources.md:401`) [report claim], i.e.
≤ ±4,096 m if centred [inferred].

### 3.2 float32 vs float64 [verified arithmetic, numpy `spacing`]

| distance from origin | float32 ulp | max rounding (½ ulp) | float64 ulp | viewmodel 0.3 m from eye, 1 ulp error, 1440p at 90° hFOV (1280 px/rad) |
|---|---|---|---|---|
| 1 km | 61 µm | 31 µm | 1.1e-13 m | 0.20 mrad ≈ 0.26 px |
| 2 km | 122 µm | 61 µm | 2.3e-13 m | 0.41 mrad ≈ 0.52 px |
| 2,252 m (max infantry) / 4 km | 244 µm | 122 µm | 4.5e-13 m | 0.81 mrad ≈ 1.04 px |
| 4,096–8 km | 488 µm | 244 µm | 9.1e-13 m | 1.63 mrad ≈ 2.1 px |
| 8,192 m | 977 µm | 488 µm | 1.8e-12 m | 3.3 mrad ≈ 4.2 px |

### 3.3 Effect per subsystem

* **Rendering** [verified + inferred]. Single builds compute `modelview = view * model` in float in the vertex shader
  (`G:servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl:439`); double builds emulate
  double for the model origin (`:420-437`). Error is absolute (≈ ulp of the camera's distance), so it only shows on
  geometry close to the eye: viewmodel, scope, cockpit, the soldier's own body. Godot's docs put single-precision
  first-person games at "[2048; 4096]" (https://docs.godotengine.org/en/latest/tutorials/physics/large_world_coordinates.html)
  [verified page]. Fix without a double engine: draw first-person geometry with a spatial shader using
  `render_mode skip_vertex_transform` and a camera-relative model-view matrix computed on the CPU in double and passed as
  instance uniforms (4 `vec4`s of the 16 allowed per shader, `G:servers/rendering/shader_language.h:358`) [inferred].
* **Physics (Godot's Jolt or ours)** [report claim + inferred]. Jolt: "If all simulation takes place within roughly 5 km
  from the origin, floating point precision is accurate enough"; double mode is "5-10% slower"
  (https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/Architecture.md, Big Worlds). At 4 km the 244 µm
  ulp is 80× below `penetration_slop`/`speculative_contact_distance` (0.02 m).
* **Simulation/network** [inferred]. Per-tick displacement at 60 Hz: prone 0.5 m/s → 8.3 mm, so a 244 µm ulp at 4 km
  is ≤ 3 % of a step only for the slowest movement (≤ 1 % for crouch walk, 0.2 % sprint). Quantised snapshot positions
  and server reconciliation absorb this. Keep time as integer ticks, never accumulated float seconds.
* **Double build cost** [verified docs + source]: editor and every export template rebuilt with `precision=double`,
  godot-cpp with `GODOTCPP_PRECISION=double`, binaries suffixed `.double`, all clients and servers on the same build type
  (large-world docs); Jolt gets `JPH_DOUBLE_PRECISION` (`G:modules/jolt_physics/SCsub:156-157`); GPU stays float.

**Recommendation: single precision engine.** The worst map is inside both Godot's and Jolt's stated single-precision
comfort zone; the one visible artefact (sub-pixel to ~1 px viewmodel shimmer on the four biggest maps) has a cheap
targeted fix. If our own Jolt ever needs it, it can be built with `JPH_DOUBLE_PRECISION` independently of Godot.

---

## 4. Threading and performance

### 4.1 Thread model of a dev7 process [verified]

| thread | what runs | source |
|---|---|---|
| main | input, SceneTree `_process`/`_physics_process`, **RenderingServer frame (thread model "Safe", default 1)** | `rendering/driver/threads/thread_model` default 1 (`DOC`); "Separate" (2) prints "experimental … can lead to project crashes" (`G:main/main.cpp:2774, 3543-3546`); docs: "several known bugs … Not recommended for use in production" |
| physics | Godot physics step on main thread unless `physics/3d/run_on_separate_thread` (default false), which "restricts API access to only physics process" (ProjectSettings.xml) | `G:modules/jolt_physics/register_types.cpp:44-51` |
| WorkerThreadPool (all logical cores, `threading/worker_pool/max_threads=-1`) | culling (threaded above `rendering/limits/spatial_indexer/threaded_cull_minimum_instances=1000`), Jolt jobs, navigation baking/queries, resource loading; 30 % of threads reserved for low-priority tasks (`low_priority_thread_ratio=0.3`) | `DOC`, ProjectSettings.xml |
| TexStreaming, TexStreaming IO | texture feedback + mip I/O | `texture_streaming.cpp:290-291` |

### 4.2 What may be called from which thread [verified unless noted]

| server / object | rule | source |
|---|---|---|
| RenderingServer | any thread. Calls from a non-render thread are queued (`ASYNC_COND_PUSH`) and run at sync; **getters from another thread block on a round trip** (`push_and_ret`). Texture, mesh and shader *creation* runs directly on the calling thread on RD renderers (`can_create_resources_async() = true`) | `G:servers/rendering/rendering_server_default.h:117-145, 280-300`; `G:servers/server_wrap_mt_common.h:153-165`; `G:servers/rendering/renderer_rd/renderer_compositor_rd.h:149` |
| PhysicsServer3D (Jolt) | setters from other threads are queued; `space_get_direct_state`, `body_get_direct_state`, `body_test_motion`, `space_get_contacts` are **main-thread only**; queries fail while the space steps; there is **no per-space step** (only `space_set_active`; the engine steps all active spaces at the physics tick) | `G:servers/physics_3d/physics_server_3d_wrap_mt.h:119-123, 272-280`; `API` PhysicsServer3D |
| NavigationServer3D | "thread-safe and thread-friendly", queries can run in parallel; `navigation/pathfinding/max_threads=4` | docs https://docs.godotengine.org/en/latest/tutorials/performance/thread_safe_apis.html [verified page] |
| SceneTree / Nodes in the tree | main thread only (`call_deferred`) | same docs page |
| Resources | loading on threads fine; do not modify one resource from several threads | same docs page |
| TextureStreaming | public API thread-safe | `texture_streaming.h:39-45` |

### 4.3 The listen-server simulation thread

Consequences [inferred from 4.2]: an authoritative tick on its own thread cannot use Godot's physics (no own stepping,
no off-main-thread queries). Running it in `_physics_process` instead couples it to the render frame, which is what made
every 5th IW4L frame 20–26 ms (handoff `sections/04-gameplay-sim.md:467-468`) [report claim]. Design:

1. `sim` library (no Godot headers) owns its own Jolt `PhysicsSystem`: static world from BF3 Havok data, soldiers as
   `JPH::CharacterVirtual`, vehicles with Jolt's wheeled/tracked controllers (present in Jolt 5.6:
   `TrackedVehicleController`, `MotorcycleController`, `VehicleConstraint` under `Jolt/Physics/Vehicle/`, `CharacterVirtual`
   under `Jolt/Physics/Character/`) [verified files], bots' perception rays, projectile sweeps.
2. Server thread = a dedicated `std::thread`/`godot::Thread`, **not** a WorkerThreadPool task (it would pin a worker
   forever). Fixed tick (rate is a sim/net decision), sleeps to the next deadline.
3. Jolt's job system for that world: Godot's own pattern (a `JPH::JobSystem` over
   `WorkerThreadPool::add_native_task`, `jolt_job_system.cpp`) to avoid oversubscribing cores; or Jolt's
   `JobSystemThreadPool` with few threads on a dedicated server.
4. Client side: commands → server through an SPSC queue (in-process loopback with the same message format as the
   network); server → client snapshots through a lock-free ring. Main thread predicts the local player with the same `sim`
   code (second, client Jolt world), interpolates remote entities, and pushes `instance_set_transform`/skeleton bones to
   the RenderingServer. 3D physics interpolation is SceneTree/node-only since 4.5 (no `instance_set_interpolated` for 3D
   in `API`; only MultiMesh/canvas variants), so RS-instance entities are interpolated by our code anyway.
5. Godot's PhysicsServer3D is then needed only for cosmetic debris if at all (or debris also lives in the client Jolt
   world and is drawn via MultiMesh).

### 4.4 Draw cost: RS instances vs MultiMesh [verified]

* Forward+ merges consecutive render elements with equal sort keys (same mesh surface, material, LOD, lightmap state,
  mirror) into one instanced draw, up to 2^20−1 repeats; MultiMesh instances and skinned mesh instances do not repeat
  (`G:servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp:896-927, 608`;
  `render_forward_clustered.h:469`). So 20k RS instances of ~2.3k meshes do **not** mean 20k draws; per-instance
  shader parameters (instance uniforms, ≤ 16 per shader) do not break this [inferred: they live in the per-instance
  data buffer, not the sort key].
* RS instances: individually frustum/occlusion culled, LOD/visibility ranges per instance; cost is culling CPU, which
  is threaded above 1,000 instances. MultiMesh: one cull unit (custom AABB), cheapest CPU, best for scatter, grass,
  debris fragments; `multimesh_set_buffer` takes one `PackedFloat32Array` per update.
* Skinned soldiers: one skeleton RID per soldier (`skeleton_create`, `skeleton_allocate_data`,
  `skeleton_bone_set_transform` per bone; no bulk bone setter in `API`). 64 soldiers × ~80 bones ≈ 5k ptrcalls per
  frame — acceptable; GPU skinning via compute is possible since 4.8 exposes `mesh_surface_get_*_buffer_rd_rid`
  [inferred].

### 4.5 Memory budget for a big level [report claim from handoff + inferred]

Caspian: 4,545 images, 1.56 GiB GPU-ready texture bytes; 20,311 placements on 2,277 meshes; Metro 5.91 M LOD0
triangles (handoff §9). With streaming at defaults the resident floor is mip 3 = 1/64 of mip-0 bytes (~25 MB for
1.56 GiB) and `memory_budget_mb` caps the top; without streaming the whole 1.56 GiB is resident (fits 12–16 GB GPUs).
Note `max_ops_per_second=200`: after a spawn, bringing hundreds of visible textures to full res takes seconds
[inferred]. Every streaming setting except `enabled` is re-read on `ProjectSettings.settings_changed`
(`texture_streaming.cpp:252, 270-277`), which `ProjectSettings.set_setting` emits deferred (`G:core/config/project_settings.cpp:558-579`),
so the game can raise `max_ops_per_second`/`min_lod`/budget during a load screen and lower them after [verified].
The `TextureStreaming` singleton's overrides do the same for LOD range and budget.

### 4.6 Profiling [verified]

* Built-in: debugger profiler/monitors; `Performance.add_custom_monitor` for our counters; `--gpu-profile`,
  `--gpu-validation` (`--help`).
* Tracy/Perfetto/Instruments need an engine built with `profiler=tracy|perfetto|instruments` (+`profiler_path`,
  `profiler_track_memory`, `profiler_record_on_demand`) (`G:SConstruct:216-243`, GH-104851). Official builds are
  `profiler=none` [inferred from the default]. Our extension can embed its own Tracy client for our zones; if we ever
  build the engine with Tracy, link the extension to the same client instead of a second copy [inferred].
* GPU: PIX/RenderDoc on D3D12/Vulkan; `--generate-spirv-debug-info` for Vulkan.

---

## 5. Dedicated server

* `--headless` = `--display-driver headless --audio-driver Dummy` (`--help`) [verified]; rendering method becomes the
  dummy rasterizer. Exported projects become headless with the `dedicated_server` feature tag ("Export as dedicated
  server" export mode; per-resource Strip Visuals/Keep/Remove); no special server binary in Godot 4 — normal
  debug/release templates; Linux servers via systemd with `application/run/flush_stdout_on_print`
  (https://docs.godotengine.org/en/latest/tutorials/export/exporting_for_dedicated_servers.html) [verified page].
* What still works headless [verified in dummy code / inferred]: PhysicsServer3D (Jolt is display-independent),
  NavigationServer3D baking from arrays (CPU Recast), our own Jolt, file I/O. The dummy renderer **stores** what you give
  it: `texture_2d_initialize` keeps `p_image->duplicate()` (`G:servers/rendering/dummy/storage/texture_storage.h:83-87`)
  and `mesh_add_surface` keeps vertex/attribute/index data (`G:servers/rendering/dummy/storage/mesh_storage.h:79-100`).
  So a server that "loads the level like the client" holds every texture in RAM. **Server code path: never create render
  resources**; load only gameplay EBX, Havok collision, terrain heights, nav.
* Linux server: Godot Linux x86_64 export template + our `.so` (`linux.release.x86_64` entry in `.gdextension`; godot-cpp's
  test `.gdextension` shows the naming scheme, `C:test/project/my_test.gdextension`). Alternative [inferred]: because
  `core`+`sim`+`net` have no Godot dependency, a standalone `bf3_server` executable is possible and avoids the engine
  entirely; keep that door open by keeping Godot out of `sim`.
* The server also reads a BF3 install as-is (gameplay/collision data); its derived cache goes to its own user dir.

---

## 6. Runtime pipeline for BF3 data read as-is

### 6.1 Loader shape [verified API / inferred design]

* `ResourceFormatLoader` is extensible from GDExtension (`_get_recognized_extensions`, `_recognize_path`,
  `_handles_type`, `_load`, `_exists`, … in `API`) and registrable with `ResourceLoader.add_resource_format_loader`. It
  is not needed for the game: BF3 assets are addressed by install-relative names, not `res://` files. Use plain C++
  loaders: `core` mounts the install and yields engine-free data; `bf3_godot` turns it into RIDs/Resources on worker
  threads (RS creation is legal off the main thread on RD, §4.2) and attaches to the tree on the main thread.
* Optional `ResourceFormatLoader` for a tiny project-side reference file (e.g. `levels/mp_007.bf3level` containing level +
  mode) so a level can be opened/inspected in the editor; the loader resolves it against the install at load time.
* Editor tools: `GDREGISTER_CLASS` node classes run in the editor (e.g. a `Bf3LevelPreview` node that draws a level via RS
  in the 3D viewport), an `EditorPlugin` subclass for an asset browser/inspector; gameplay classes as
  `GDREGISTER_RUNTIME_CLASS` so they do not execute in the editor (§2.3).

### 6.2 Cache under `user://` [inferred design on verified mechanisms]

* Location: `user://bf3cache/` (= `<OS user data>/Godot/app_userdata/<project>` or a custom dir with
  `application/config/use_custom_user_dir`) [verified setting doc]. Never inside the install.
* Key = CAS SHA1 of the source payload(s) (already in `cas.cat`/manifests; the handoff notes SHA1 is of the stored
  bytes, `01-containers-ebx.md:116`) + converter version + Godot version/commit + format version (+ Jolt version for
  shapes). A patched install changes the SHA1 → automatic re-conversion; old entries are garbage-collected by LRU/size.
* Formats:

| data | cache form | load path | why |
|---|---|---|---|
| textures (streamed route) | GSTF `.stex`, BC verbatim, full chain to 1×1 | `StreamedTexture2D::load` | only file-backed GSTF streams (§1.3) |
| textures (resident route) | none | `Image::create_from_data` → `RenderingServer::texture_2d_create` | decode ≈ 2 s/level all cores |
| meshes | our blob: per surface Godot-format `vertex_data`/`attribute_data`/`skin_data`/`index_data` + `lods` + `aabb` + `format` | `RenderingServer::mesh_add_surface(RID, Dictionary)` (bound at `G:servers/rendering/rendering_server.cpp:2371` → `_dict_to_surf`, `:1936-1976`, which parses exactly these keys) | no per-vertex conversion at load; strides from `mesh_surface_get_format_*_stride` |
| static collision | Jolt `Shape::SaveWithChildren` streams (`Jolt/Physics/Collision/Shape/Shape.h:368-401`, which also warns saved state must be regenerated when Jolt changes) | `Shape::sRestoreWithChildren` | skips mesh-shape cooking |
| nav | baked polygons/tiles (Godot `NavigationMesh` arrays or Detour tiles) | `NavigationServer3D` region / Detour | bake cost 4–6 s per level in IW4L (handoff 04 §9) [report claim] |
| gameplay/scene model | our blob of the `bf3_scene` model | `core` | |

* Godot `.res` via `ResourceSaver` works for Godot-native resources but adds a Variant/property layer and ties the files
  to Godot's binary format; custom blobs are smaller, versioned by us, and readable by the Godot-free server [inferred].
  `FileAccess` has no mmap; read blobs in `core` with OS positional reads or mmap and make one `PackedByteArray` per GPU
  buffer [inferred].

### 6.3 Load-time targets for an MP map change [inferred targets on report-claim inputs]

Inputs (handoff `01-containers-ebx.md:307-320`, measured on this install): mount + manifests + GUID index 67–124 ms; level
db for one mode 82–154 ms; traverse 12–47 ms; all texture chunks of a level ≈ 2 s on all cores. Targets: warm cache
≤ 5 s to playable (scene model + mesh blobs + Jolt shapes + nav from cache, textures at `max_lod` then streamed); cold
first conversion ≤ 60 s per level with a progress screen; server load ≤ 2 s (no render data).

---

## 7. Repository layout, build, CI, tests

### 7.1 Layout [inferred design]

```
bf3/                                  (git repo; the Godot project bf3-opensource lives in game/ or is pointed at)
  CMakeLists.txt                      top level: options, pins, add_subdirectory order (godot-cpp first)
  cmake/                              warnings, MSVC/GCC flags, godot-pin check
  third_party/
    godot-cpp/                        submodule pinned to a commit (>= 79c37b2 for hot-reload fix; today b051e69)
    godot-api/4.8-dev7/               extension_api.json + gdextension_interface.json + godot-version.txt (version, commit, exe SHA256)
    JoltPhysics/                      submodule, own build (JPH_ flags identical for client+server)
    zlib-ng or libdeflate, doctest, tracy (optional)
  core/      bf3_core   static, no Godot: install/CAS/superbundle/EBX/resources/level → scene model; tests/
  sim/       bf3_sim    static, no Godot: soldiers, weapons, vehicles, rules, bots, own Jolt world; tests/
  net/       bf3_net    static, no Godot: protocol, snapshots/deltas, UDP transport, loopback; tests/
  godot/     bf3_godot  SHARED (GDExtension): registration, RS bridge, asset cache, nodes, editor plugin, test MainLoop
  server/    bf3_server optional standalone dedicated server (core+sim+net)
  tools/     bf3dump CLI, cache inspector, shot harness driver
  game/      project.godot, bf3.gdextension, bin/, shaders/*.gdshader (material families), minimal scenes
```

### 7.2 CMake sketch [verified option names; inferred structure]

```cmake
cmake_minimum_required(VERSION 3.28)
project(bf3 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)               # godot-cpp itself builds as C++17 (godotcpp.cmake:406)
set(GODOTCPP_GDEXTENSION_DIR  ${CMAKE_SOURCE_DIR}/third_party/godot-api/4.8-dev7 CACHE PATH "")
set(GODOTCPP_CUSTOM_API_FILE  ${GODOTCPP_GDEXTENSION_DIR}/extension_api.json      CACHE FILEPATH "")
set(GODOTCPP_TARGET template_debug CACHE STRING "")   # template_release for shipping, editor for tool-only code
add_subdirectory(third_party/godot-cpp)  # first: sets CMAKE_MSVC_RUNTIME_LIBRARY (/MT) for everything after
add_subdirectory(third_party/JoltPhysics/Build)
add_subdirectory(core) ; add_subdirectory(sim) ; add_subdirectory(net)
add_library(bf3_godot SHARED godot/src/register_types.cpp ...)
target_link_libraries(bf3_godot PRIVATE godot::cpp bf3_core bf3_sim bf3_net Jolt)
```

Build from a VS 2026 developer prompt (`vcvars64.bat`), Ninja Multi-Config or the VS generator; output DLL copied to
`game/bin/`. `game/bf3.gdextension`: `entry_symbol`, `compatibility_minimum = "4.8"`, `reloadable = true` (editor
only), `[libraries] windows.debug.x86_64 / windows.release.x86_64 / linux.release.x86_64`.

### 7.3 Tests and CI [verified mechanisms / inferred plan]

* `core`/`sim`/`net`: doctest executables, no Godot; golden counts from the handoff §9 (Metro 10,873 placements, Caspian
  20,311, …) as tests that skip when no install is configured.
* Headless Godot tests **without GDScript**: a C++ class deriving from `SceneTree` (or `MainLoop`, whose `_initialize`,
  `_process`, `_physics_process`, `_finalize` are virtual in `API`) registered by the extension and started with
  `godot --headless --path game --main-loop Bf3TestRunner -- --case=<name>`; `--main-loop` instantiates any ClassDB class
  deriving from MainLoop (`G:main/main.cpp:4144-4145, 4442-4450`); exit with `SceneTree::quit(code)`. Also settable as
  `application/run/main_loop_type`. Rendering tests need a GPU (shot harness on the dev machine, not CI).
* CI: Windows (MSVC) + Ubuntu (GCC/Clang) jobs: configure with the pinned API dump, build, run doctest, download the
  pinned Godot build from `godotengine/godot-builds` releases (verify SHA256), run headless tests. Tests that need the
  BF3 install run only on the user's machine (the install cannot be put in CI).

---

## 8. Recommendations (ordered)

1. Stay on dev7 for M0/M1; pin it (version string, commit c971f93e7, exe SHA256) and generate godot-cpp bindings from
   `research/godot-api/extension_api.json` with godot-cpp pinned to a commit at or after b051e69 (2026-10-09, has the
   hot-reload fix). Move to the RC with the
   dump + `--validate-extension-api` + rebuild procedure (§1.2); do not ship a dev7-built DLL into the RC.
2. Use GDExtension only; no engine module, no custom engine build, single precision.
3. Keep Godot out of `core`, `sim`, `net`; only `bf3_godot` includes godot-cpp. This keeps doctest fast, enables a
   standalone server, and isolates engine upgrades to one library.
4. Vendor Jolt into `sim` and run the authoritative tick on a dedicated thread; use Godot's PhysicsServer3D for nothing
   gameplay-relevant. Characters = `CharacterVirtual`, vehicles = Jolt vehicle controllers, cached shapes via
   `SaveWithChildren`.
5. Draw the static world with RenderingServer instances (sorted batching handles repeats), scatter/debris with
   MultiMesh; use runtime occluders from BF3's occluder meshes; enable occlusion culling.
6. Textures: resident `ImageTexture`/`texture_2d_create` from BC data first; add the GSTF streaming cache behind a
   setting once materials settle; write streaming feedback against the RC's `STREAMING_LOD`, not dev7's `STREAMING_UV`.
7. Ship material-family shaders as project `.gdshader` files with one material per variant in the project so the
   shader baker pre-compiles them; avoid per-asset generated shader code.
8. First-person geometry through a camera-relative vertex path (instance-uniform model-view in double on the CPU).
9. Dedicated server path never creates render resources; same binary in `--headless`, or later a standalone
   `bf3_server`.
10. Cache everything derived under `user://bf3cache/` keyed by CAS SHA1 + converter/engine/Jolt versions; never write
    to the install.
11. Tests: doctest for the Godot-free libs from day one; a C++ `SceneTree` test runner for headless engine tests; the
    shot harness for visuals.

---

## 9. Not determined

* Exact dates of 4.8 beta/RC/stable (only cadence-based estimates); the full list of API changes between dev7 and the
  RC (needs the RC dump + `--validate-extension-api`).
* Whether GSTF stays at version 1 in 4.8-stable and whether streaming will accept runtime/non-file textures, arrays or
  cubes before stable (open PRs GH-123743, GH-116047; `sampler2DStreamed` only discussed).
* Real-world behaviour of texture streaming with ~4.5k textures on D3D12 (the PR says D3D12 fragmentation is untested);
  the cost of the feedback atomics on our material count.
* Whether BF3 DxTexture mip chains always run to 1×1 (GSTF and `Image` require it); the two handoff examples do.
* Whether the 1.56 GiB Caspian figure is BC (GPU-ready) bytes or RGBA8 — the handoff's "a 4K texture chunk decodes to
  ~22 MB" (`01-containers-ebx.md:320`) is ambiguous with "largest texture seen: 2048²" (`02-resources.md:132`).
* godot-cpp's own measured build time and binary size against the dev7 API, and whether godot-cpp headers compile
  cleanly as C++20 under MSVC 18 (the probe agent is building this).
* Whether official Windows builds publish engine PDBs (I did not find them), which decides how deep MSVC debugging
  into the engine goes without a self-built editor.
* Whether a GDExtension class registered in ClassDB is already registered when `--main-loop` resolves its class name
  (code path read, not executed: extension classes register at scene level before `Main::start`, inferred).
* Per-call overhead numbers for GDExtension ptrcalls/Variant conversions on this build (no measurement made).
* Combat-area extents per level (waypoints are only infantry routes; `CombatAreaTriggerEntityData` polygons not read),
  so the aircraft distance bound is the terrain size, not measured.
* Godot NavigationServer3D determinism and its suitability for a standalone server (vs Detour in `sim`): left to the bots
  topic.

---

## Independent verification (2026-10-10)

Done by a separate verifier that tried to refute each claim. Method: Godot source was read from the commit object with
`git show HEAD:<path>` in a clone whose HEAD is `c971f93e7e76b0ef919bf6009e7b868bea04db7f`. This skips the working tree,
so line numbers are those of the dev7 commit (`G:`). godot-cpp was read the same way at `b051e694` (`C:`), which
`gh api repos/godotengine/godot-cpp/commits/master` returns as master. Other sources: `API`, fresh `gh` queries,
primary web pages, and four of my own headless runs of the local dev7 console exe. My scratch folder is
`C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\verify-07-godot\v2`
(`V2` below). Verdicts: confirmed / corrected (partly wrong, corrected text given) / refuted / unverifiable.

| id | claim (short) | verdict | evidence (source) | correction |
|---|---|---|---|---|
| K1 | dev7 = feature freeze, no beta/RC by 10-10, 4.6/4.7 last-dev→stable 7.5–9 wk | confirmed | `gh api repos/godotengine/godot-builds/releases` (`V2\rel.txt`): 4.8-dev1…dev7, dev7 published 2026-10-01T14:33Z, no 4.8 beta/rc. dev7 post dated 29 Sep 2026: "no new features will be integrated for Godot 4.8 moving forward", built from c971f93e…; commit date 2026-09-29T11:27Z (`gh api …/commits/c971f93e…`). 4.7 dev5 04-17 → stable 06-18 = 62 d (8.9 wk); 4.6 dev6 12-05 → stable 01-26 = 52 d (7.4 wk) | — ("released 10-01" is the GitHub release; the blog post and commit are dated 09-29) |
| K2 | only file-loaded `StreamedTexture2D` streams; RS/TextureStreaming hooks not bound | confirmed | `API`: TextureStreaming = 8 methods (lod/budget overrides, bytes_used, flush) + `flush_completed`. StreamedTexture2D = `load`, `get_load_path`, lod overrides. RenderingServer has no `*streaming*` method. Searching the dev7 commit for `texture_configure_streaming` and `texture_2d_attach_streaming_state`: the only users outside the module and renderers are `G:scene/resources/streamed_texture.cpp:118,382`. Declaration at `G:modules/texture_streaming/texture_streaming.h:630`; binds at `texture_streaming.cpp:111-129`. Hands-on: probe §6.4 (ImageTexture does not stream, GSTF in user:// does) | — |
| K3 | GSTF v1 = header + raw `Image` data in any format; read via FileAccess; full chain to 1×1 | confirmed | `G:scene/resources/streamed_texture.cpp:128-181` (`FileAccess::open`, "GSTF", version > 1 rejected, ≤ 16 mips, offset/size table, flags, min, max, reserved); `:223-224` limits dimensions to ≤ 16384; `:260` calls `set_data(w,h, mip_count>0, …)`; writer at `:265-323`. `G:core/io/image.cpp:2542-2570` requires `p_data.size() == _get_dst_image_size(…, -1)`, which is the chain down to 1×1 (`:1928-1975`). `API` Image.Format: DXT1 17, DXT3 18, DXT5 19, RGTC_R 20, RGTC_RG 21, BPTC_RGBA 22 | — (a file with `num_mipmaps=0` loads but cannot stream; a partial chain fails at every level except its coarsest) |
| K4 | feedback is per material **per instance** from UV derivatives; `STREAMING_UV`→`STREAMING_LOD` post-dev7; Forward+/Mobile, 2D only | corrected | The shader writes to `material_feedback.data[instances.data[i].material_feedback_index]` (`G:…/scene_forward_clustered.glsl:1396-1397`). The index is set per **material**: `render_forward_clustered.cpp:883-891` calls `feedback_buffer_material_index(surface->material->material_feedback_rid)`, which returns `material_info_owner.get_index(material)` (`texture_streaming.cpp:457-473`). Feedback runs only if the shader uses `UV` or `STREAMING_UV` (`glsl:883, 1360`). PR 123662 merged 2026-10-08T18:43Z (`gh pr view`); its diff swaps `built_ins["STREAMING_UV"]` (vec2) for `["STREAMING_LOD"]` (float) in `shader_types.cpp` (`V2\pr123662.diff`). Compatibility renderer: warning, no streaming (`texture_streaming.cpp:284-287`). PR 113429 text: "Currently not implemented for 3d textures or lightmaps" | **One feedback slot per Material**, holding the highest quality any fragment of any instance using that material asks for in the frame. One near instance raises the material's textures everywhere. In dev7, a material whose shader never reads `UV` gives no feedback (§1.3 says "per-instance") |
| K5 | godot-cpp v10 independent; bundled JSON 4.3–4.7; 4.8 needs custom API file; dev7 interface JSON **identical** to bundled (179 fns) | corrected | Releases: 10.0.0-rc1 2026-01-26, rc2 08-25, stable 09-15 (`gh api`). `C:gdextension/` holds `extension_api-4-{3..7}.json`; `C:cmake/godotcpp.cmake:131-146, 272-289`; `C:tools/godotcpp.py:180`. My own `--dump-gdextension-interface-json` (`V2\iface`) vs `C:gdextension/gdextension_interface.json`: same format_version 1, 147 types, 179 functions, same `since` histogram (4.1:136 … 4.7:3). **Not byte-identical**: `object_set_script_instance.p_script_instance` is typed `GDExtensionScriptInstancePtr` in dev7 and `GDExtensionScriptInstanceDataPtr` in godot-cpp, with different descriptions (dump line 8335-8342). Both are `typedef void *` (`IFH:576, 607`) | "ABI-equivalent, not identical" (one argument type and its doc text differ in a script-instance function we do not use). Keep generating from the engine's own dump via `GODOTCPP_GDEXTENSION_DIR`, as the probe did |
| K6 | compatibility gates compare major.minor.patch only; changed methods fail lazily | confirmed | `G:core/extension/gdextension_library_loader.cpp:324-394`: no status compared; a missing max part = 9999; the patch check is compiled out when patch = 0. `C:src/godot.cpp:158-182`. `C:binding_generator.py:2202`: static `classdb_get_method_bind(class, name, hash)` on first call. `C:include/godot_cpp/core/error_macros.hpp:800-806`: "Method bind was not found…", `DEBUG_ENABLED` only. `API` vs `C:…/extension_api-4-7.json`: 33 new classes, 173 new methods, 9 changed hashes (incl. `Image.generate_mipmaps`, `Image.compress`, `Skeleton3D.reset_bone_pose(s)`) | — (methods with a registered compatibility hash still resolve; I did not check which of the 9 have one) |
| K7 | Jolt always in `PhysicsServer3DWrapMT`; direct-state/test_motion main-thread-only; no per-space step; no Jolt classes | confirmed | `G:modules/jolt_physics/register_types.cpp:42-52`; `G:servers/physics_3d/physics_server_3d_wrap_mt.h:120-124, 127-130, 273-276, 279-282`: `ERR_FAIL_COND(!Thread::is_main_thread())` on the four getters/queries. `API` PhysicsServer3D has only `space_set_active`/`set_active`; `_step/_sync/_flush_queries` exist only as `PhysicsServer3DExtension` virtuals. No class name contains "Jolt"; only CharacterBody3D, VehicleBody3D/VehicleWheel3D | — (the getters are main-thread-only; queries on an already obtained state object are not, see extra-1) |
| K8 | an authoritative tick on its own thread needs its own Jolt (5.6, MIT, CharacterVirtual, vehicle controllers, shape serialisation) | confirmed | `G:thirdparty/README.md:512-516` (5.6.0, e77f1755, MIT) + `thirdparty/jolt_physics/LICENSE`. `Jolt/Physics/Vehicle/` has WheeledVehicleController, TrackedVehicleController, MotorcycleController, VehicleConstraint; `Character/` has CharacterVirtual. `Shape.h:367-401`: `SaveWithChildren`/`sRestoreWithChildren`, "will not be backwards compatible for newer library versions". The inference holds because stepping is engine-driven (K7) | — (the reason is the step, not queries; see extra-1) |
| K9 | single precision OK: max 2,252 m (XP5_002), median 465 m; ulps; Godot [2048;4096] FPS; Jolt ~5 km | corrected | My script `V2\wp3.py` (MP_/XP* only): 175 files with non-origin points; max 2,252.0 m XP5_002 Rush Large (3D radius 2,318.8 m); ≤ 512 m: 92; > 1,024: 22; > 2,048: 3, all as reported. **Median 475.7 m.** The 465 m comes from `…\godot-engine\scripts\extent.py`, which keeps 4 graphs whose only points are (0,0,0) (it gives "levels 179, median 464.69"). numpy spacing: 1,000 m 6.10e-5; 2,000 m 1.22e-4; 2,048–4,095 m 2.44e-4; 4,096–8,191 m 4.88e-4. Godot large-world doc row "[2048; 4096] … Maximum recommended single-precision range for a first-person 3D game". Jolt `Docs/Architecture.md:752` "roughly 5 km" (same text at e77f175) | Median 476 m, not 465 m (rest unchanged). Note that 2,252 m lies *inside* the docs' top first-person band, not well below it |
| K10 | single builds compute view·model in float, so only near-camera jitter (~1 px); fix with `skip_vertex_transform` + instance uniforms (16 max) | confirmed (inference) | `G:…/scene_forward_clustered.glsl:439` float `modelview` (double emulation only under `USE_DOUBLE_PRECISION`, `:420-437`). `G:servers/rendering/shader_language.h:358` `MAX_INSTANCE_UNIFORM_INDICES = 16`. `shader_language.cpp:9873-9876` rejects mat2/3/4 instance uniforms, so the matrix must be passed as vec4 rows (the report's 4 vec4s is fine; 3 suffice for 3×4). Arithmetic: 0.244 mm / 0.3 m × 1280 px/rad = 1.04 px | — |
| K11 | render "Safe" on main thread; Separate experimental; off-thread RS calls queued, getters block; texture/mesh/shader **creation** on the calling thread | corrected | `G:main/main.cpp:2773-2780` (default `RENDER_THREAD_SAFE`; editor forces 0), `:3543-3547` warning; ProjectSettings.xml:2949 "several known bugs … Not recommended for use in production". `rendering_server_default.h:117-119`, `server_wrap_mt_common.h:153-165` (`push_and_ret`). Caller-thread creation (when `can_create_resources_async`, `renderer_compositor_rd.h:149`) applies only to `texture_*_create` (`:136-205`), `mesh_create_from_surfaces` (`:366-390`), `shader_create_from_code` (`:291-302`) and `material_create_from_shader` (`:327-345`). Queued to the render (= main) thread: `shader_create()` (`:281-289`); `mesh_create()`/`material_create()` (`FUNCRIDSPLIT`, `server_wrap_mt_common.h:58-67`); `mesh_add_surface` (`:396`, `FUNC2` push `:267-276`); `shader_set_code` (`:311`). `API` binds neither `shader_create_from_code` nor `material_create_from_shader`; the `Shader`/`ShaderMaterial` resources call them (`G:scene/resources/shader.cpp:55`, `material.cpp:475`) | "On RD, `texture_*_create` and `mesh_create_from_surfaces` run on the calling thread, and so do `Shader`/`ShaderMaterial` resource creation. Raw RS `shader_create`/`material_create`/`mesh_create`/`mesh_add_surface`/`*_set_*` from a worker are queued and execute on the main thread at sync" |
| K12 | headless dummy renderer keeps CPU copies of all textures and meshes | corrected (minor) | `G:servers/rendering/dummy/storage/texture_storage.h:83-87` keeps `p_image->duplicate()` for **2D** textures; the layered and 3D initializers (`:88-89`) keep nothing. `mesh_storage.h:79-100` keeps every surface buffer. `--headless` gives the dummy renderer (`main.cpp:570, 1464, 2583-2584`) | "keeps every 2D texture image and every mesh surface buffer"; the conclusion (the server creates no render resources) stands |
| K13 | Forward+ merges identical consecutive surfaces up to 2^20−1; MultiMesh/skinned do not repeat | confirmed | `G:…/render_forward_clustered.cpp:896-929`: equal sort_key1/2, `uses_lightmap_specular` and mirror → repeat. `cant_repeat` = MULTIMESH flag or `mesh_instance` valid (skinned / blend shapes, `renderer_scene_cull.cpp:581`). `:608` instance_count; `.h:469` `MAX_REPEATS = (1<<20)-1`. Probe §6.1: 10k instances of 1 mesh = 6 draws | — |
| K14 | `mesh_add_surface(RID, Dictionary)` takes pre-packed vertex/attribute/skin/index/lods | confirmed | `G:servers/rendering/rendering_server.cpp:1936-1998` `_dict_to_surf`: required primitive, format, vertex_data, vertex_count, aabb; optional attribute_data, skin_data, index_data+index_count, uv_scale, lods[edge_length, index_data], bone_aabbs, blend_shape_data, material. `:2006-2007`, bind `:2371`. `mesh_create_from_surfaces` (bind `:2363`) uses the same parser. `API` has `mesh_surface_get_format_{offset,vertex,normal_tangent,attribute,skin,index}_stride` | — (for the worker-thread caveat see extra-2) |
| K15 | streaming settings except `enabled` re-read on `settings_changed` | confirmed | `G:modules/texture_streaming/texture_streaming.cpp:226` `GLOBAL_DEF_RST` enabled; `:252` connect; `:270-277` re-read; consumer `:811-812` reads `setting_max_ops_per_second` per pass. `G:core/config/project_settings.cpp:558-580` deferred emit. PR 113429 text says "settings are only read once on load", which is stale against the dev7 code | — |
| K16 | headless C++ tests via a registered SceneTree/MainLoop subclass + `--main-loop` | corrected | `G:main/main.cpp:4144-4145, 4442-4455`. My runs: `--main-loop SceneTree` and `--main-loop MainLoop` exit 0 on a scratch project with a main scene. A copy of the probe project (`V2\probecopy`) with `--main-loop GDExtProbe` prints "Invalid MainLoop type" (`main.cpp:4453`): the extension class **is** in ClassDB at that point, it just is not a MainLoop. **But** the editor/console exe run without `application/run/main_scene` and without a scene argument aborts with "Couldn't detect whether to run the editor, the project manager or a specific project" (`main.cpp:4376-4383`, reproduced in `V2\ml3.log`). `--main-loop` is option class X ("Only available in editor builds, and export templates compiled with `disable_path_overrides=false`", `--help`); `disable_path_overrides` defaults to True (`G:SConstruct:287-293, 1115-1116`), and templates then clear `main_loop_type` (`main.cpp:4355-4359`) | "Works with the editor/console binary if the test project has a main scene (an empty .tscn) or a scene is passed. In export templates use `application/run/main_loop_type` instead of `--main-loop`" |
| K17 | double builds use `JPH_DOUBLE_PRECISION`; Jolt double 5–10 % slower | confirmed | `G:modules/jolt_physics/SCsub:156-157`; Jolt `Docs/Architecture.md:764` "5-10% slower" (master and e77f175) | — |
| K18 | shader baker bakes only materials present in exported resources/scenes | confirmed | `G:editor/export/shader_baker/shader_baker_export_plugin.cpp:277-296` bakes the native shader version of each exported `Material`; `:298+` scene walk adds generated Label3D/Sprite3D materials. PR 102552 merged 2025-05-28, milestone 4.5 (`gh`) | — |
| extra-1 | §4.3/§0: Godot physics allows "no off-main-thread queries" | corrected | Probe §6.2 (`research/probes/godot-gdextension-probe.md:128-146`) ran 40k `intersect_ray_into` over 1–8 `std::thread`s with no errors while the main thread blocked in `_physics_process`. The actual limit: `G:modules/jolt_physics/spaces/jolt_physics_direct_space_state_3d.cpp:452-454` refuses the query while stepping, then `flush_pending_objects()` mutates the space | "Queries on a `PhysicsDirectSpaceState3D` obtained on the main thread can run on workers inside a window the main thread opens (not stepping, no body adds)." The design conclusion (own Jolt for an independent tick) is unchanged |
| extra-2 | §6.1/§6.2: build RIDs on worker threads, meshes via `mesh_add_surface(Dictionary)` | corrected | See K11: `mesh_add_surface` from a worker is queued (`rendering_server_default.h:396`), so the GPU upload runs on the main thread at sync. `mesh_create_from_surfaces` runs on the caller and also generates pipelines (`:366-390`). Its GDExtension wrapper ignores `blend_shape_count` (`rendering_server.cpp:1999-2005`) | Use `mesh_create_from_surfaces(TypedArray<Dictionary>)` from loader threads. Meshes with blend shapes go through `mesh_create` + `mesh_set_blend_shape_count` + `mesh_add_surface` (queued). Create shaders/materials through `Shader`/`ShaderMaterial` |
| extra-3 | §3.3: prone 0.5 m/s → 8.3 mm/tick, ulp ≤ 3 % | corrected | EBX probe (`research/probes/ebx-data-probe.md`): prone speed 0.9 m/s | 15 mm/tick at 60 Hz; 244 µm = 1.6 %. Conclusion unchanged |
| extra-4 | §4.4: "64 soldiers × ~80 bones ≈ 5k ptrcalls" | corrected | Ant probe/EBX probe: 3P skeleton `veniceantske01` 213 bones (1P 179). `API` has no bulk bone setter (`skeleton_bone_set_transform` only) | Up to 64 × 213 ≈ 13.6k calls/frame if the full skeleton is uploaded. Upload only the bones the skin references (e.g. 70 on `arms1p_rus_mesh`) or skin on the GPU via `mesh_surface_get_*_buffer_rd_rid` |
| extra-5 | §9: unknown whether extension classes are registered when `--main-loop` resolves | confirmed (resolved) | K16 run: `--main-loop GDExtProbe` reaches the cast (`main.cpp:4453`), so the class existed | Remove from §9 |
| extra-6 | §3.1: terrains 4,096 or 8,192 m | corrected | Handoff `sections/03-level-and-look.md:31`: XP2_Palace terrain 1024 | 1,024 / 4,096 / 8,192 m |
| extra-7 | §1.4 Navigation: bake from `set_vertices/set_indices` | corrected | `G:scene/resources/3d/navigation_mesh_source_geometry_data_3d.cpp:47-52` has `ERR_FAIL_COND(vertices.size() < p_indices.size())` with `vertices` a `Vector<float>` (`.h:40`). Shared-vertex meshes (indices > 3 × vertices) are rejected. Matches the probe | Use `append_arrays` (or `add_faces`) for indexed BF3 collision |
| extra-8 | §7.2/§7.3 build & CI steps | corrected (incomplete) | Probe report: a project with no `.godot/extension_list.cfg` leaves the extension class unregistered and the run hangs, fixed by one `--import`; object paths ≥ 261 chars fail with C1083 under MSVC, fixed by a short build root [report claim from the probe, not re-run]. Plus the main-scene requirement from K16 | CI: run `godot --headless --path game --import` once before tests; keep the CMake binary dir short; give the test project a main scene |
| extra-9 | §9: is the 1.56 GiB Caspian figure BC or RGBA8? | unverifiable | Handoff `01-containers-ebx.md:320` "4K texture chunk decodes to ~22 MB": 4096² × 1 B/px × 4/3 = 22.4 MB matches BC3/BC5/BC7; RGBA8 would be ~89 MB [inferred, not checked in data] | Likely BC (GPU-ready) bytes |
| extra-10 | spot checks of other cited facts | confirmed | PR numbers, titles and merge dates in §1.2/§1.4 (`gh pr view` on 22 PRs, all as stated; 123743/116047/123107 open). ENet 1.3.18 and Recast 1.6.0 (`G:thirdparty/README.md:243-247, 957-961`). Godot Jolt docs: face index "+about 25%", separate thread "experimental". `low_priority_thread_ratio` 0.3 and `navigation/pathfinding/max_threads` 4 (ProjectSettings.xml:3615, 2497). Thread-safe-APIs doc: navigation "thread-safe and thread-friendly". No `-rdynamic` in linuxbsd/SConstruct. `os_windows.cpp:486-509` `~` copy + PDB. godot-cpp 79c37b2 (authored 10-04, PR #2060 merged 10-09) is an ancestor of b051e69 (compare: ahead_by 5). `C:cmake/windows.cmake:73-86` static CRT default. `C:src/classes/low_level.cpp:62-68`. 33 new classes vs 4.7, none removed. RS methods new in 4.8: `texture_replace_compatible`, `mesh_surface_get_*_buffer_rd_rid`, `light_set_allow_contact_shadows`. No NavigationServer3D/WorkerThreadPool method added since 4.5 | — (line drift only: `reloadable` is read at `gdextension_library_loader.cpp:408-409`, not 405-407) |
| principle | recommendations vs "BF3 files read as-is" | confirmed | §5, §6.2, §8.10: all derived data (GSTF, mesh blobs, Jolt shapes, nav) goes under `user://bf3cache/`, keyed by the CAS SHA1 of the stored payload + converter/engine/Jolt versions. The server reads the install as-is. No install write, no mod bundle, no running or hooking bf3.exe. The test/CI plan keeps install-dependent tests on the user's machine | No violation. [inferred] caches hold converted BF3 content and should never be shipped or shared |

**Could not determine (verification).**

* Which of the 9 methods whose hash changed between 4.7 and dev7 have compatibility binds. This decides whether a 4.7-built
  call would still resolve.
* Whether official 4.8 export templates are built with the default `disable_path_overrides=True`. I read the SConstruct
  default, not the release build scripts.
* The practical effect of per-material streaming feedback on BF3's shared materials. Nothing was measured.
* `mesh_create_from_surfaces` called from a worker thread was not executed; the reading is from code only.
* Whether `physics/3d/run_on_separate_thread=true` would allow a usable off-main tick on Godot's Jolt. Not tested; the
  step stays engine-driven (`physics_server_3d_wrap_mt.h`).
* The probe's ENet, navigation and build-path numbers were not re-run.
