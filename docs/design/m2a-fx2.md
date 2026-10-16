# M2a fx2: the VFX2 renderer, as built

Track fx2 of M2a wave 2 (docs/design/m2a.md §12.1, §15), on branch `m2a/fx2` from the W2.0 contract (b8bef3f) plus
7220da0 (build.cmd skips the timing rows). It turns `bf3_fx`'s simulated particles (VFX1, docs/design/m2a-vfx1.md)
into Godot draws: one MultiMesh per batch, three particle shaders, a pool of 64 effect lights and a first-person pass.
It also ships a demo on Metro, a GPU reference bench, and the tests and gates below.

Measured 2026-10-16 on the dev preset (Windows 11, AMD RX 9070 XT, Godot 4.8 dev7 Forward+ D3D12). The machine is
shared with other worktrees, so **every time below is noisy**. The timing and GPU rows ran under the quiet lock.

## 1. Files

| file | what |
|---|---|
| `src/bf3_fx/include/bf3/fx/render_prep.h`, `src/bf3_fx/src/render_prep.cpp` | `RenderPrep`: batch keys, instance streams (`FxInstance`, 80 B), per-emitter params rows, sort, the merged light pool, stats. Godot-free |
| `src/bf3_fx/include/bf3/fx/atlas_pixels.h`, `src/bf3_fx/src/atlas_pixels.cpp` | `prepare_atlas_pixels`: the level's emitter atlas pixels (BC3 base, NormalDXT5RGA → BC5 normal, every mip), cached as `fx-atlas` entries; the hostile-checked blob decoder |
| `src/bf3_godot/src/fx_renderer.{h,cpp}` | `Bf3FxRenderer`: steps an `FxWorld` on the WorkerThreadPool, uploads the batches, drives the light RIDs |
| `bf3-opensource/shaders/fx_common.gdshaderinc`, `fx_alpha.gdshader`, `fx_additive.gdshader`, `fx_premultiplied.gdshader` | particle shading |
| `src/bf3_godot/src/fx_demo.{h,cpp}`, `bf3-opensource/scenes/fx_demo.tscn` | `Bf3FxDemo`: synthetic starts on Metro, `--fx-check` (godot.fx_demo) and the captures |
| `src/bf3_godot/src/fx_ref.{h,cpp}`, `bf3-opensource/scenes/fx_ref.tscn` | `Bf3FxRef`: the Godot reference workloads and VFX2's 16k row (godot.fx_ref, E9 item 8) |
| `tests/fx_render_tests.cpp` | `fx.render.unit` (9 cases) |
| `tests/godot_fx_demo.cmake`, `tests/godot_fx_ref.cmake` | the two ctest wrappers |

Shared files took one `# M2a fx2` / `// M2a fx2` block at the end: `src/bf3_fx/CMakeLists.txt` (two sources),
`src/bf3_godot/CMakeLists.txt` (six sources, links `bf3_fx`), `src/bf3_godot/src/register_types.cpp` (three
classes; one include block, one registration block), and `tests/CMakeLists.txt` (fx.render.unit and the two
Godot rows). `docs/design/m2a.md` is unchanged: no rule in it proved wrong (§9 lists what changed against its
sketch).

## 2. Decisions

* **§16 Q10, emitter sort:** batches back-to-front by emitter depth, with no per-particle sort. Particles keep their
  simulation order inside an emitter. Additive needs no sort. Named in `render_prep.h`.
* **§16 Q17, the relative GPU row:** VFX2's 16k cost ≤ `kFxRefRatioMax` 1.25 × the reference MultiMesh 16k
  lit + soft row, both measured in the same run (`fx_ref.h`).
* **Batches:** 4 shader classes (SAQ, DAQ, WAQ, mesh) × 3 blends (alpha, additive, premultiplied) × 2 passes (world,
  first person) = 24 MultiMeshes at most (`kMaxFxBatches`). Draw order is the material `render_priority`:
  alpha and premultiplied before additive, the first-person pass last.
* **Blend per emitter:** the `Emissive` flag selects additive (unshaded, no fog). Every other emitter is
  alpha-blended and lit. No retail field selects premultiplied; the shader is kept for VFX3's decals and tuning.
* **Particle meshes are not loaded.** A `ParticleMesh` instance draws as a stand-in box `kFxMeshStandinMeters`
  (3 cm) × size, white when the emitter names no texture. They are counted in `RenderPrepStats::mesh_standins`.
* **Frames are not blended.** The flipbook draws the integer part of the frame (VFX1 open question Q6).
* **Half-res particles stay deferred,** as m2a.md §12.1 allows.

## 3. Atlas (`atlas_pixels.h`)

`prepare_atlas_pixels` runs on the demo's loader thread:

1. It reads the level's two emitter atlas textures through `res_loader` (Metro: base 4096² DXT5, 13 mips; normal
   1024 × 2048 NormalDXT5RGA, 12 mips).
2. It keeps BC3 as is. It converts the normal map to BC5: R is the colour block's red re-encoded as BC4, and G is the
   alpha block copied.
3. It stores both chains as one `fx-atlas` cache entry, keyed by the two resources' entry SHA-1s and chunk SHA-1s.

Measured on a cold cache: 0.10 s to convert. A warm cache loads in 0.03 s. The base takes 21.3 MiB and the normal
2.7 MiB.

The blob decoder (`decode_atlas_pixels`, magic "FXATLASP") refuses the following, each with an error and without
allocating first:

* a truncated header or chain;
* a level size that does not match its mip chain;
* more than 16 mips or more than 256 MiB.

`fx.render.unit` runs 4,000 seeded mutations through it on the dev and asan presets.

`Bf3FxRenderer::upload_atlas` makes two `ImageTexture`s (`FORMAT_DXT5` sRGB via `source_color`, `FORMAT_RGTC_RG`)
with every mip and checks the mip count. An atlas entry counts as uploaded when its texture uploaded and its rect lies
inside [0, 1] (E8 (6)).

## 4. Shading (`fx_common.gdshaderinc`)

### 4.1 Instances

The instance layout is documented in `render_prep.h`'s header comment:

* the transform carries the class's basis and the world position;
* COLOR is HDR rgb plus transparency;
* INSTANCE_CUSTOM holds the params row, the frame, the packed alpha min / max and the alpha scale.

The per-emitter constants are one `FxEmitterParams` row (4 RGBA32F texels: base rect, flipbook, normal rect,
lighting). They are uploaded as a 256-wide float texture, and only when the rows change. The vertex shader builds the
quad per class:

* SAQ billboards in view space;
* DAQ stretches along the screen-projected velocity;
* WAQ and the mesh use the instance basis.

The shaders use `skip_vertex_transform`. In a MultiMesh, Godot's MODEL_MATRIX already includes the instance
transform; the bench confirmed this (VFX2 and the reference rows draw the same picture).

### 4.2 Alpha levels [inferred]

* a' = saturate((tex.a − min) / (max − min)). When the window has closed (min ≥ max), a' = step(min, tex.a).
* a = saturate((a' − 1 + scale) × max(exponent, 1)) × transparency.

This is an erosion reading. The texture's alpha passes unchanged at the defaults (0, 1, 1, 1). I considered an
image-"levels" reading, a = a'^exponent × scale, and dropped it without building it: with the retail exponents of
5–10 it keeps only alpha near 1 (0.8^10 = 0.11), so the muzzle and impact smoke would all but vanish. The data
also points to erosion: scale curves grow 0.1 → 1 over a life while transparency fades.

Retail curves cross: the concrete debris arm's min rises 0 → 1 while its max falls 1.57 → 0.02. The closed-window
rule keeps that from inverting the alpha, which the first build did; the result was a black opaque mass.

### 4.3 Lighting [inferred]

The alpha-blended rows use Godot's lit pipeline (`diffuse_lambert_wrap`, ROUGHNESS = the emitter's wrap-around,
specular off), with the emitter's normal-map rect when it has one. The light multiplier m is read as the lit share:

* ALBEDO = rgb × m;
* EMISSION = rgb × (1 − m) × `kFxSelfLitScale` 0.3.

The scale is there because retail unlit colours are relative to BF3's HDR scene level. At 1.0 the shell smoke
(colour 3, m 0.02) drew a white card.

Additive rows draw rgb × `kFxEmissiveScale` 0.125 with no fog. The oilfire's colour is 0.03 × (900, 388, 188); at
1.0 it tonemapped to white. With fog on, each additive layer also added the fog colour.

Both constants are named in `fx_renderer.h` and are the user's to judge.

### 4.4 Soft particles, 1p pass

* **Soft fade:** a = a × saturate((particle depth − scene depth) / (|soft| × extent)), from `hint_depth_texture`.
  The 1p pass has no soft fade.
* **First person:** batches use the viewmodel's projection, the same code as StandardMaterial3D's `fov_override`
  (55°), with `Z_CLIP_SCALE` 0.25 (render_bridge MaterialOptions defaults). The flag is set per batch, and only the
  FirstPerson batches carry it.

## 5. Renderer (`Bf3FxRenderer`)

* **Stepping:** `step_world()` runs the frame's sub-steps. Each sub-step's emitter items go to Godot's
  WorkerThreadPool (`add_group_task`, ≤ `kFxStepTasks` 4 tasks). A sub-step with fewer than `kFxSerialItems` (16)
  items runs on the main thread.
* **Drawing:** `draw()` runs `RenderPrep::prepare`, then calls `multimesh_set_buffer` per batch with the whole
  capacity. Capacity starts at 64 and doubles. The renderer sets `visible_instances` to the batch's count and gives
  each batch a custom AABB.
* **Lights:** up to 64 omni lights, without shadows. Energy = summed intensity × the colour's largest component ×
  `kFxLightEnergyScale` 1.0, and the light colour is that colour normalised.
* **Merging:** RenderPrep merges each EmitterOutput's lights into one: intensities summed, centre weighted by
  intensity, the largest radius. It keeps the top 64 by intensity / max(d², 0.01) and counts the rest as dropped.
* **Timing:** the renderer times step, prepare and upload separately (`FxRendererStats`).
* **Clean-up:** `clear()` and `NOTIFICATION_PREDELETE` free every RID the renderer made.

## 6. Demo (`res://scenes/fx_demo.tscn`, `--fx-check`)

The demo starts from a `Bf3LevelSession` on Metro (MP_Subway), with no walkers and no HUD.

**Loader thread.** It loads the following in about 0.5 s:

* the weapon sources;
* the atlas pixels;
* the impact table;
* the `EffectLibrary`;
* the M16A4 `FireEffects1p` effects (flash `fx_muzz_generic_rifle_1p`, fill-out, shell `fx_shellejection_m`);
* the dark smoke pillar `fx_dark_smoke_01_m`;
* the MP_012 oilfire emitter, compiled by `compile_emitter_effect`;
* a light-stress effect.

**Main thread.** It builds the renderer, the collision world and the S3 stage camera. A ray fan (8 pitches, up to
145 yaws, kept within 2–45 m, nearest to 8 m wins) finds:

* concrete: physics material "Concrete" at 8.7 m → `fx_impact_concrete_01_s_xp2`;
* brick: "Brick" at 38.1 m → `fx_impact_concrete_01_s`, which is what the impact table's brick cell names.

**Timeline (s):**

| time (s) | event |
|---|---|
| 0.5 on | 1p muzzle at 10 Hz (flash, fill-out and shell at the camera-space muzzle (0.10, −0.11, −0.70) m) |
| every 0.6 | an impact, alternating concrete and brick |
| 7.5 | smoke pillar 10 m ahead |
| 16 | the light stress: 96 effects, more than 64 lights |

The run lasts 21 s.

**Effect frames [inferred; VFX3 owns the real triggers]:** local +Y is the barrel for the muzzle and the surface
normal for an impact. BF3's spawn direction defaults to +Y, and the 1p flash's direction and box offset lie along +Y.
The first build used +Z, which put the flash 2 m above the muzzle.

**Captures:** each capture poses the camera, starts its effect, advances, freezes the world and settles for 4 frames.
`save_screenshot` then writes to `--fx-shot-dir`, which the wrapper defaults to `<checkout>/b/shots/fx2` (gitignored);
the install's write guard is checked first.

* The muzzle shot fires again until the flash component (p 0.75) is present, at most 16 times.
* The oilfire shot frames the live plume.

**Diagnostics:** `--fx-debug-view=1|2|3` shows the unlit colour, drops the normal map, or shows the normal map's
texels. Section 8 has what they showed.

## 7. Reference bench (`res://scenes/fx_ref.tscn`, `--fx-ref-check`)

**Scene:** 1920 × 1080 borderless, vsync off. A floor, a sun without shadows and a 256² synthetic puff texture.
16,384 quads of 1 m sit at seeded positions in a 40 × 7.5 × 30 m box in front of the camera.

**Method:** S3 G1b. Each row alternates on and off windows of 5 s and drops the first 1 s of each window. The row's
cost is the p50 GPU time of the on windows minus the p50 of the off windows (viewport measured render time), over 3
pairs per row.

**Rows:**

* `ref-unlit` and `ref-lit-soft`: MultiMesh with a StandardMaterial3D billboard; the lit row has a 1 m proximity
  fade.
* `+64`: the same plus 64 omni lights (range 6 m).
* `gpu-16x1000`: GPUParticles3D.
* `vfx2`: one FxWorld of 16 effects × 1,024 particles. Each effect is a synthetic SAQ emitter with a Box spawn over
  one of the 16 sub-boxes, lit, soft 1 m, drawn by `Bf3FxRenderer`.

**Pass:** cost(vfx2) ≤ 1.25 × cost(ref-lit-soft), 16,384 drawn instances, and 0 mismatching frames.

**GPU power-state finding.** The first VFX2 workload was 16,384 one-particle effects. It cost 5.8 ms of main-thread
CPU per frame, and its measured GPU cost came out at 2.1 × the reference. The same GPU work on static buffers measured
0.76 ms. With a 6 ms busy wait added to the frame (`--fx-ref-static --fx-ref-busy=6`) it measured 1.56 ms.

So a CPU-bound frame roughly doubles the measured viewport GPU time on this machine, most likely because the GPU
drops its clocks between frames. The bench therefore uses 16 box emitters × 1,024, which costs 0.14 + 0.19 + 0.38 ms
on the main thread (step + prepare + upload).

Anyone who reads the GPU rows of a CPU-heavy scene should know about this effect, the firefight gate E9 included.
`--fx-ref-static`, `--fx-ref-nosoft` and `--fx-ref-busy=<ms>` stay in as diagnostics.

## 8. Captures (`b/shots/fx2/`, from the gate run of §10)

These need the user's judgement. What I saw in the final captures:

* **fx2_muzzle_1p**
  - A small yellow-white flash core with orange fire petals and dark grey-blue smoke puffs, slightly right of and
    below the centre.
  - Three or four shell stand-in boxes fall on the stage below the view. They are tinted by the muzzle light, and
    earlier shells are still falling.
  - The flash sits 2.7 m ahead of the camera, not on the barrel, because VFX1 reads the flash's Box pivot (0, 2, 0)
    as metres (§9).
  - The white lower third of the frame is the S3 stage's untextured floor (it is brown in the S3 look), not FX.
* **fx2_impact_concrete**
  - The debris smoke arms appear as dark blue-grey streaks fanning off a wall edge against the sky, with a small
    puff and white debris specks.
  - The left half is the wall face in shadow. Lit smoke in shadow is dark, because it gets ambient light only.
* **fx2_impact_brick**
  - At 38 m and 0.12 s the impact is a faint grey smear in the middle of the brick wall, with a few specks and one
    small chip stand-in.
  - Close up the effect is small, and grey dust on red brick reads weakly.
* **fx2_smoke**
  - The dark smoke pillar is near-black billows with hard cut-out edges. Its colour is
    0.03 × the colour curve's 0.46 = 0.014, which is black smoke by the data.
  - The edges come from the erosion with exponent 10 (§4.2).
  - Some billows end in straight edges: quad borders where the eroded alpha reaches the quad edge. This is a
    sorting / erosion artefact to judge.
* **fx2_oilfire**
  - Seen from about 370 m, the plume is a faint pale pink-orange column rising about 80 m above its start, against a
    bright sky.
  - After `kFxEmissiveScale` the square quad borders that showed at scale 1.0 are gone. The plume is faint, and the
    user may want a larger scale.
* **fx2_lights**
  - The 96-effect stress puts 64 lights on the courtyard floor, which glows orange-yellow. The dark smoke pillar
    stands black in front of it.
* **Atlas frames:**
  - I saw no cell bleed (no neighbouring cell's edge inside a quad) in any capture. The turbulent smoke and the
    muzzle petals show distinct cells.
  - The first build's capture had a large yellow ring with stair-step edges in front of the camera. It looked like BC3
    alpha blocks amplified by the erosion and a 125 HDR colour. It does not appear in the final captures; that it is
    gone, rather than just out of frame, is not proven.
* **Blending and sorting:**
  - Batches sort by emitter, so smoke of different emitters can pop when their depth order swaps (§2 Q10).
  - No case of an additive batch drawing behind alpha smoke showed.

**Diagnostic findings** (scratch runs with `--fx-debug-view`; not committed images):

* The "black smoke" of the first build was the data (the dark pillar), not the lighting. The unlit colour view was
  black too.
* The white and dark bands in the lower half are level geometry.
* The giant beige wash in the first muzzle capture was the 1 m mesh stand-in of the shell at 0.15 m from the camera.

## 9. Against m2a.md §12.1's sketch

* The atlas upload uses `Image::create_from_data` + `ImageTexture` directly, not `render_bridge::make_texture`. The
  data is already in block format, so that is all it needs.
* m2a.md lists the custom data as frame, alpha levels and soft fade. As built it is params row, frame, packed alpha
  min / max and alpha scale; the soft fade moved into the per-emitter row.
* m2a.md measured the reference row at 1.05 ms. This run measured 0.758 ms, from the same method on the same machine
  (noise, driver). The relative gate is unaffected.

**Found in VFX1 and not fixed here (not fx2's files):** `BoxEvaluatorData`'s pivot is read as metres (m2a-vfx1.md
§6, "[inferred full extents]"). That places the 1p flash 2 m along the barrel. A pivot in half-extent units would put
it 0.25 m ahead, which looks more likely.

## 10. Gates, measured (2026-10-16, dev preset, shared machine)

| gate | threshold | measured | command |
|---|---|---|---|
| fx.render.unit | 9 cases pass: batch keys, alpha packing, sort / layout / params / AABB, fallbacks, Waq / mesh stand-in, lights (merge, 64 pool, priority, 1p), BC5, image prep, blob round trip and 4,000 mutations | 9/9 cases, 6,478 assertions; asan 9/9, no report | `ctest --preset dev -R "^fx\.render\.unit$"` (and `--preset asan`) |
| E8 (3) drawn == particles | equal in every frame | 3,347 / 3,347 frames | `ctest --preset dev -R "^godot\.fx_demo$"` (OPENBF3_GODOT set) |
| E8 (6) atlas | 52 / 52 entries, 0 default-texture fallbacks | 52 / 52, 0 | same |
| E8 (4) oilfire | 80 ± 1 drawn after 15 s | 80 drawn / 80 live at 15.004 s | same |
| effect lights | ≤ 64 in every frame, under a > 64 stress | max 64; 108 merged candidates, 20,406 drops over the run | same |
| 1p muzzle pass | every 1p particle in a FirstPerson batch | 191 starts, 3,292 frames, 0 mismatches | same |
| FX main-thread time | printed (E9's p99 ≤ 1.0 ms is wave 3's gate) | p50 0.108 ms, p99 0.225 ms (step 0.062 / 0.129, prepare 0.015 / 0.035, upload 0.029 / 0.084) | same |
| install unchanged | layout.toc, cas.cat, every .sb/.toc | unchanged | same |
| godot.fx_ref (E9 item 8, §16 Q17) | cost(vfx2) ≤ 1.25 × cost(ref-lit-soft) | 0.769 / 0.758 = 1.015 | `ctest --preset dev -R "^godot\.fx_ref$"` (OPENBF3_GODOT set) |

godot.fx_demo passes when every row passes, the concrete and brick surfaces are found and 6 / 6 captures are written.

**fx_ref rows** (GPU cost, ms): ref-unlit 0.154, ref-lit-soft 0.758, ref-lit-soft+64 2.003, gpu-16x1000 0.722,
vfx2 0.769, vfx2+64 1.966. VFX2's main-thread p50 in the bench is step 0.141, prepare 0.185 and upload 0.381 ms.

**Full ctest, timing rows excluded** (`tools\build.cmd dev`, which skips the TIMING regex): 90 tests, all passed;
`core.cache_data` and `core.res` were skipped as before. §11 covers the timing rows.

## 11. Quiet lock and timing runs

godot.fx_demo and godot.fx_ref ran under the lock "fx2 2026-10-16T14:11", which was created when no lock existed and
held for 2 minutes before the rows ran (14:11–14:17).

The other timing rows (build.cmd's TIMING regex) ran one session at a time under fresh "fx2" locks, after the
full ctest:

* **14:18–14:28:** host.scale, host.session*, host.server, sim.nav.bake, sim.weapons.perf, tools.fx_bench,
  logic.perf, tools.ebx_memory, tools.m1_prepare and render.scene. 8 of 11 passed.
  - `host.server` failed with 137 ticks against a limit of 130.
  - `host.session.unit` and `host.session` failed their clock-target check, with 4 and 6 target changes against a
    limit of 2.
  - Rerun alone under a new lock (14:28–14:35), all three passed. fx2 does not touch `bf3_host`. These rows are
    known to be load-sensitive (m2a-w2.md).
* **14:35–14:56:** godot.m9_reload, s0_selftest, inspector, collision, m1, m1_load, l1_look, ui_fonts,
  ui_fonts_shots and ui2 passed. `godot.s3_crowd` failed its timing gates (G1b / G2) on both of its internal runs.
  - It failed again alone (15:30–15:40). The step p50 varied 0.34–0.90 ms between runs, a sign of machine load.
  - **A/B:** the base build, `wt/w2` at 7220da0 (this branch's base, no fx2 code), failed the same gates under an
    "fx2" lock at 16:22–16:34 (G1a p99 2.47 ms, G2-off p50 13.7 ms, then G2-on p50 13.5 ms).
  - So the s3_crowd failure is the machine's load at the time, not fx2. It is reported as an open issue, not fixed.

**Incident.** Earlier exploratory runs did overlap another track's lock: "nt 2026-10-16T12:29:13". They were the
first `--fx-check` demo run and the quick fx_ref runs (12:33–12:38). I did not gate those runs on the lock. Their
numbers (fx_ref vfx2 / ref = 0.778 / 0.754) are not used above. Every later build and run waited for the lock.
