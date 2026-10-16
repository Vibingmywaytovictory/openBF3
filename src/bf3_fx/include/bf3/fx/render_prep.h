#pragma once
// VFX2 render preparation (docs/design/m2a.md §12.1; owner fx2): FxWorld's outputs and particles of one frame turned
// into instance streams, one per (shader class x blend x pass) batch, sorted, plus the merged light pool. Godot-free;
// Bf3FxRenderer uploads the streams with multimesh_set_buffer (one MultiMesh per batch, <= kMaxFxBatches) and drives
// <= kMaxFxLights OmniLight3D RIDs from the light list. Main thread, inside the FX budget (E9: FX main-thread time
// p99 <= 1.0 ms, which includes this).
//
// Rules: transparent batches back-to-front by emitter depth along the view; particles keep simulation order inside
// an emitter (§16 Q10, taken 2026-10-16: no per-particle sort); additive needs no sort. Lights: the lights of one
// EmitterOutput merge into one (summed intensity, intensity-weighted centre, largest radius); priority = intensity /
// distance^2; beyond kMaxFxLights the rest are dropped and counted (E9: <= 64 in every frame). 1p effects (muzzle)
// go to the FirstPerson pass (drawn with the viewmodel's FOV override and z-clip scale). The drawn instance count
// equals FxWorld::particles().size() every frame (E8 (3)).

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

enum class FxShaderClass : uint8_t { Saq = 0, Daq = 1, Waq = 2, Mesh = 3, Count }; // screen / direction / world aligned quads, meshes
enum class FxBlend : uint8_t { Alpha = 0, Additive = 1, Premultiplied = 2, Count };
enum class FxPass : uint8_t { World = 0, FirstPerson = 1, Count };

// One particle instance, 80 bytes: MultiMesh's transform (12 floats, 3x4 row-major), colour (4) and custom data (4:
// flipbook frame, alpha min, alpha max x scale packed, soft fade).
struct FxInstance {
	float transform[12];
	float color[4];
	float custom[4];
};
static_assert(sizeof(FxInstance) == 80);
static_assert(std::is_trivially_copyable_v<FxInstance>);

struct FxBatchKey {
	FxShaderClass shader = FxShaderClass::Saq;
	FxBlend blend = FxBlend::Alpha;
	FxPass pass = FxPass::World;

	bool operator==(const FxBatchKey &) const = default;
};
// The batch an emitter draws in (its EmittableType and render fields; Trail and Point draw as Saq [inferred]).
FxBatchKey fx_batch_key(const EmitterDef &p_emitter, bool p_first_person);

struct FxBatch {
	FxBatchKey key;
	uint32_t first_instance = 0; // into RenderPrep::instances()
	uint32_t instance_count = 0;
	uint32_t emitters = 0; // emitter outputs drawn in this batch
};

struct FxLight {
	Vec3 position; // world, intensity-weighted centre of the merged lights
	float radius = 0; // the largest
	Vec3 color; // HDR linear, intensity-weighted
	float intensity = 0; // summed
	float priority = 0; // intensity / distance^2 to the camera
};
static_assert(std::is_trivially_copyable_v<FxLight>);

struct RenderPrepStats {
	uint32_t instances = 0; // == FxWorld::particles().size() (E8 (3))
	uint32_t batches = 0;
	uint32_t lights = 0;
	uint64_t lights_dropped = 0; // beyond kMaxFxLights, since creation
	uint64_t unresolved_textures = 0; // emitters whose texture the atlas does not resolve (drawn with the default entry)
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
	const RenderPrepStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::fx
