#pragma once

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

inline constexpr float kFxLightMinDistance2 = 0.01f;

inline constexpr const char *kFxDefaultTexture = "systems/emitter/defaulttexture";

enum class FxShaderClass : uint8_t { Saq = 0, Daq = 1, Waq = 2, Mesh = 3, Count };
enum class FxBlend : uint8_t { Alpha = 0, Additive = 1, Premultiplied = 2, Count };
enum class FxPass : uint8_t { World = 0, FirstPerson = 1, Count };
inline constexpr size_t kFxShaderClassCount = size_t(FxShaderClass::Count);
inline constexpr size_t kFxBlendCount = size_t(FxBlend::Count);
inline constexpr size_t kFxPassCount = size_t(FxPass::Count);
static_assert(kFxShaderClassCount * kFxBlendCount * kFxPassCount == kMaxFxBatches);

inline constexpr float kFxMeshStandinMeters = 0.03f;

struct FxInstance {
	float transform[12];
	float color[4];
	float custom[4];
};
static_assert(sizeof(FxInstance) == 80);
static_assert(std::is_trivially_copyable_v<FxInstance>);

inline constexpr float kFxRectWhite = -1.0f;

struct FxEmitterParams {
	float base_rect[4];
	float flipbook[4];
	float normal_rect[4];
	float lighting[4];
};
static_assert(sizeof(FxEmitterParams) == 64);
static_assert(std::is_trivially_copyable_v<FxEmitterParams>);

inline constexpr uint32_t kFxAlphaLevelSteps = 1023;
float fx_pack_alpha_levels(float p_min, float p_max);
void fx_unpack_alpha_levels(float p_packed, float &r_min, float &r_max);

struct FxBatchKey {
	FxShaderClass shader = FxShaderClass::Saq;
	FxBlend blend = FxBlend::Alpha;
	FxPass pass = FxPass::World;

	bool operator==(const FxBatchKey &) const = default;
};

FxBatchKey fx_batch_key(const EmitterDef &p_emitter, bool p_first_person);

size_t fx_batch_index(const FxBatchKey &p_key);
FxBatchKey fx_batch_key_at(size_t p_index);

struct FxBatch {
	FxBatchKey key;
	uint32_t first_instance = 0;
	uint32_t instance_count = 0;
	uint32_t emitters = 0;
	Vec3 aabb_min, aabb_max;
};

struct FxLight {
	Vec3 position;
	float radius = 0;
	Vec3 color;
	float intensity = 0;
	float priority = 0;
	bool first_person = false;
};
static_assert(std::is_trivially_copyable_v<FxLight>);

struct RenderPrepStats {
	uint32_t instances = 0;
	uint32_t batches = 0;
	uint32_t lights = 0;
	uint32_t light_candidates = 0;
	uint64_t lights_dropped = 0;
	uint64_t unresolved_textures = 0;
	uint32_t unresolved_this_frame = 0;
	uint32_t mesh_standins = 0;
	uint32_t first_person_instances = 0;
	uint64_t frames = 0;
	double prepare_ms = 0;
};

class RenderPrep {
public:
	RenderPrep();
	~RenderPrep();
	RenderPrep(const RenderPrep &) = delete;
	RenderPrep &operator=(const RenderPrep &) = delete;

	void prepare(const FxWorld &p_world, const FxView &p_view, const EmitterAtlas &p_atlas);
	std::span<const FxInstance> instances() const;
	std::span<const FxBatch> batches() const;
	std::span<const FxLight> lights() const;
	std::span<const FxEmitterParams> emitter_params() const;
	const RenderPrepStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
