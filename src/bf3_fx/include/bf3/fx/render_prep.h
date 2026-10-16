#pragma once
// VFX2 render preparation (docs/design/m2a.md §12.1; owner fx2; as built: docs/design/m2a-fx2.md): FxWorld's outputs
// and particles of one frame turned into instance streams, one per (shader class x blend x pass) batch, sorted, plus
// the merged light pool. Godot-free; Bf3FxRenderer uploads the streams with multimesh_set_buffer (one MultiMesh per
// batch, <= kMaxFxBatches) and drives <= kMaxFxLights OmniLight3D RIDs from the light list. Main thread, inside the
// FX budget (E9: FX main-thread time p99 <= 1.0 ms, which includes this).
//
// Rules: transparent batches back-to-front by emitter depth along the view; particles keep simulation order inside
// an emitter (§16 Q10, taken 2026-10-16: no per-particle sort); additive needs no sort. Lights: the lights of one
// EmitterOutput merge into one (summed intensity, intensity-weighted centre, largest radius); priority = intensity /
// distance^2; beyond kMaxFxLights the rest are dropped and counted (E9: <= 64 in every frame). 1p effects (muzzle)
// go to the FirstPerson pass (drawn with the viewmodel's FOV override and z-clip scale). The drawn instance count
// equals FxWorld::particles().size() every frame (E8 (3)).
//
// Instance layout (FxInstance, 80 B, Godot's MultiMesh 3D layout with colours and custom data in float format):
//   transform  3x4 row-major [X Y Z | O] (Godot Transform3D rows): O = the particle's world position; the columns
//              depend on the shader class (fx_*.gdshader reads them the same way):
//                Saq   X = size.x (cos r, sin r, 0), Y = size.y (-sin r, cos r, 0): a 2-D basis in the camera
//                      plane (the shader billboards), Z = 0
//                Daq   X = (size.x, 0, 0), Y = (0, size.y, 0), Z = the unit velocity (world): the quad's long axis
//                      follows the velocity projected onto the view plane, its width faces the camera
//                Waq   a quad in the world XZ plane (WorldAlignedQuad) or in the emitter frame's right / up plane
//                      (Quad), rotated by r about its normal; Z = the unit normal
//                Mesh  Euler(axis degrees, Y then X then Z [inferred]) x diag(size): a unit stand-in mesh (the
//                      particle meshes are not loaded in VFX2; reported)
//              Quads span -0.5..0.5 x size (size = the full extent [inferred]).
//   color      HDR linear rgb (UpdateColor), a = transparency (UpdateTransparency)
//   custom     x = the output's row in emitter_params() (exact in a float), y = flipbook frame (integer part =
//              frame index; frames are not blended, open question VFX1 Q6), z = alpha level min and max packed
//              (fx_pack_alpha_levels, 10 bits each, exact in a float), w = alpha level scale
// The shaders' alpha [inferred, an erosion reading of the alpha levels; the user judges the look]:
//   a' = saturate((tex.a - min) / (max - min)), a = saturate((a' - 1 + scale) x exponent) x transparency
// which is the texture's alpha unchanged at the defaults (min 0, max 1, scale 1, exponent 1).
// The per-emitter constants (atlas rects, flipbook layout, alpha exponent, soft fade, lighting) are one
// FxEmitterParams row per EmitterOutput, uploaded as a small float texture.

#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/fx_types.h"
#include "bf3/fx/simulator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace bf3::fx {

inline constexpr size_t kMaxFxBatches = 24;
inline constexpr size_t kMaxFxLights = 64;
// Light priority = intensity / max(distance^2, kFxLightMinDistance2) so a light at the camera stays finite.
inline constexpr float kFxLightMinDistance2 = 0.01f;
// The atlas entry an emitter without a resolved texture draws with (m2a-vfx1.md §6.9), counted in
// RenderPrepStats::unresolved_textures.
inline constexpr const char *kFxDefaultTexture = "systems/emitter/defaulttexture";

enum class FxShaderClass : uint8_t { Saq = 0, Daq = 1, Waq = 2, Mesh = 3, Count }; // screen / direction / world aligned quads, meshes
enum class FxBlend : uint8_t { Alpha = 0, Additive = 1, Premultiplied = 2, Count };
enum class FxPass : uint8_t { World = 0, FirstPerson = 1, Count };
inline constexpr size_t kFxShaderClassCount = size_t(FxShaderClass::Count);
inline constexpr size_t kFxBlendCount = size_t(FxBlend::Count);
inline constexpr size_t kFxPassCount = size_t(FxPass::Count);
static_assert(kFxShaderClassCount * kFxBlendCount * kFxPassCount == kMaxFxBatches);

// One particle instance, 80 bytes: MultiMesh's transform (12 floats, 3x4 row-major), colour (4) and custom data (4:
// emitter params row, flipbook frame, packed alpha levels, alpha level scale; header comment).
struct FxInstance {
	float transform[12];
	float color[4];
	float custom[4];
};
static_assert(sizeof(FxInstance) == 80);
static_assert(std::is_trivially_copyable_v<FxInstance>);

// The per-emitter constants of one EmitterOutput (64 B, four RGBA32F texels in the params texture).
// base_rect sizes of kFxRectWhite: the emitter draws without a texture (a ParticleMesh naming none: white); a zero
// size (nothing resolved and no default entry, e.g. an empty atlas) samples the whole base texture.
inline constexpr float kFxRectWhite = -1.0f;

struct FxEmitterParams {
	float base_rect[4]; // atlas UV min (u, v) and size (du, dv) of the texture entry (base atlas); see kFxRectWhite
	float flipbook[4]; // frame count, frame columns, alpha exponent, soft fade multiplier
	float normal_rect[4]; // normal atlas UV min and size; size 0 = no normal map
	float lighting[4]; // light multiplier, light wrap-around, emissive (0 / 1), cull threshold
};
static_assert(sizeof(FxEmitterParams) == 64);
static_assert(std::is_trivially_copyable_v<FxEmitterParams>);

// Alpha level min / max packed into one float: round(min x 1023) x 1024 + round(max x 1023), both clamped to [0, 1]
// (< 2^20, exact in a float); the shaders unpack with floor / mod.
inline constexpr uint32_t kFxAlphaLevelSteps = 1023;
float fx_pack_alpha_levels(float p_min, float p_max);
void fx_unpack_alpha_levels(float p_packed, float &r_min, float &r_max);

struct FxBatchKey {
	FxShaderClass shader = FxShaderClass::Saq;
	FxBlend blend = FxBlend::Alpha;
	FxPass pass = FxPass::World;

	bool operator==(const FxBatchKey &) const = default;
};
// The batch an emitter draws in (its EmittableType and render fields; Trail and Point draw as Saq [inferred]).
// Blend [inferred, the user judges the look]: Emissive emitters (fire, flashes, sparks) add, every other emitter
// blends with its alpha; no retail field selects premultiplied alpha, so that batch stays empty in VFX2 (its shader
// exists for VFX3's decals and tuning).
FxBatchKey fx_batch_key(const EmitterDef &p_emitter, bool p_first_person);
// Index of a key in [0, kMaxFxBatches): pass-major, then blend, then shader class (the draw order of batches()).
size_t fx_batch_index(const FxBatchKey &p_key);
FxBatchKey fx_batch_key_at(size_t p_index);

struct FxBatch {
	FxBatchKey key;
	uint32_t first_instance = 0; // into RenderPrep::instances()
	uint32_t instance_count = 0;
	uint32_t emitters = 0; // emitter outputs drawn in this batch
	Vec3 aabb_min, aabb_max; // world bounds of the instances (positions grown by the largest extent)
};

struct FxLight {
	Vec3 position; // world, intensity-weighted centre of the merged lights
	float radius = 0; // the largest
	Vec3 color; // HDR linear, intensity-weighted
	float intensity = 0; // summed
	float priority = 0; // intensity / distance^2 to the camera
	bool first_person = false; // from a 1p effect (the muzzle lights)
};
static_assert(std::is_trivially_copyable_v<FxLight>);

struct RenderPrepStats {
	uint32_t instances = 0; // == FxWorld::particles().size() (E8 (3))
	uint32_t batches = 0;
	uint32_t lights = 0;
	uint32_t light_candidates = 0; // merged lights before the pool cap, this frame
	uint64_t lights_dropped = 0; // beyond kMaxFxLights, since creation
	uint64_t unresolved_textures = 0; // emitter outputs whose texture the atlas does not resolve (drawn with the default entry), since creation
	uint32_t unresolved_this_frame = 0;
	uint32_t mesh_standins = 0; // ParticleMesh instances drawn with the stand-in mesh, this frame
	uint32_t first_person_instances = 0; // instances in FirstPerson batches, this frame
	uint64_t frames = 0;
	double prepare_ms = 0; // the last prepare(), wall ms
};

class RenderPrep {
public:
	RenderPrep();
	~RenderPrep();
	RenderPrep(const RenderPrep &) = delete;
	RenderPrep &operator=(const RenderPrep &) = delete;

	// After the frame's last FxWorld::end_step. The spans below stay valid until the next prepare().
	void prepare(const FxWorld &p_world, const FxView &p_view, const EmitterAtlas &p_atlas);
	std::span<const FxInstance> instances() const;
	std::span<const FxBatch> batches() const; // draw order
	std::span<const FxLight> lights() const; // by priority, <= kMaxFxLights
	std::span<const FxEmitterParams> emitter_params() const; // one row per FxWorld::outputs() entry, same order
	const RenderPrepStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::fx
