#pragma once

#include "bf3/core/res/terrain.h"
#include "bf3/core/res/terrain_mask_field.h"
#include "bf3/core/res/terrain_visual.h"
#include "bf3/core/result.h"
#include "bf3/render/dxbc.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::render {

class ShaderLookup;

struct SceneScatterType {
	core::res::TerrainScatterType data;
	int32_t mesh = -1;
};

struct SceneScatterCombo {
	std::string shader;
	std::vector<uint8_t> layers;
	std::vector<uint8_t> scattered;
	std::vector<std::pair<uint32_t, std::string>> textures;
	std::shared_ptr<const std::vector<uint8_t>> program;
};

struct SceneScatterPlan {
	std::vector<SceneScatterType> types;
	std::vector<SceneScatterCombo> combos;
	uint32_t cell_instances = 0;
};

struct ScatterComboName {
	std::vector<uint8_t> layers;
	std::vector<uint8_t> scattered;
};

std::optional<ScatterComboName> terrain_scatter_combo(std::string_view p_shader, std::string_view p_asset);

SceneScatterPlan plan_terrain_scatter(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_mesh,
		std::vector<std::pair<std::string, std::string>> &r_failures);

struct ScatterImage {
	uint32_t width = 0;
	uint32_t height = 0;
	bool srgb = false;
	std::vector<std::vector<uint8_t>> mips;
};

core::Result<ScatterImage> load_scatter_image(const core::LevelMount &p_mount, const std::string &p_name);

struct ScatterHeights {
	std::array<float, 2> origin{};
	std::array<float, 2> spacing{};
	uint32_t width = 0;
	uint32_t height = 0;
	std::array<uint32_t, 4> drawn{};
	float base = 0.0f;
	float step = 0.0f;
	std::vector<uint16_t> samples;
	std::vector<uint8_t> holes;

	static ScatterHeights from_grid(
			const core::res::HeightGrid &p_grid, const std::optional<std::array<float, 4>> &p_crop);

	float metres(uint32_t p_x, uint32_t p_z) const { return base + float(samples[size_t(p_z) * width + p_x]) * step; }
	std::optional<float> surface(float p_x, float p_z) const;
	std::optional<std::array<float, 3>> normal(float p_x, float p_z) const;
	float drop_under(const std::array<std::array<float, 2>, 4> &p_quad, const std::array<float, 3> &p_point,
			const std::array<float, 3> &p_normal) const;
	float bilinear(float p_u, float p_v) const;
};

enum class ScatterSource : uint8_t {
	Zero,
	Mask,
	Height,
	Image,
};

struct ScatterSamplers {
	const core::res::TerrainMaskField *masks = nullptr;
	const ScatterHeights *heights = nullptr;
	std::span<const ScatterImage> images;
};

struct MaskScaleOutput {
	std::array<float, 8> mask{};
	std::array<float, 8> scale{};
};

class MaskScaleProgram {
public:
	struct Texture {
		uint32_t slot = 0;
		ScatterSource source = ScatterSource::Zero;
		uint32_t image = 0;
	};

	static core::Result<MaskScaleProgram> build(std::span<const uint8_t> p_dxbc, std::span<const uint8_t> p_layers,
			const core::res::TerrainMaskField &p_masks, const ScatterHeights &p_heights,
			const std::function<std::optional<int32_t>(uint32_t)> &p_image_of_slot);

	void measure(const ScatterSamplers &p_samplers);
	MaskScaleOutput run(const ScatterSamplers &p_samplers, float p_x, float p_z, float p_footprint) const;
	size_t instructions() const { return program_.code.size(); }

private:
	struct Sample {
		float texels_per_metre = 0.0f;
	};

	void execute(const ScatterSamplers &p_samplers, float p_x, float p_z, float p_footprint, bool p_every_branch,
			std::vector<std::array<float, 2>> *r_uvs, std::array<std::array<float, 4>, 4> &r_outputs) const;

	dxbc::Program program_;
	uint32_t constant_slot_ = 0;
	std::vector<std::array<float, 4>> constants_;
	std::vector<Texture> textures_;
	std::vector<Sample> samples_;
	std::vector<int32_t> skip_;
};

struct ScatterType {
	uint32_t source = 0;
	int32_t mesh = -1;
	uint8_t layer = 0;
	uint8_t tree_level = 0;
	float density = 0.0f;
	std::array<float, 2> scale_min{};
	std::array<float, 2> scale_max{};
	float cell = 0.0f;
	uint32_t candidates = 0;
	float footprint = 0.0f;
	bool upright = false;
};

struct ScatterInstance {
	std::array<float, 3> position{};
	float yaw = 0.0f;
	std::array<float, 2> scale{};
	std::array<float, 3> up{ 0.0f, 1.0f, 0.0f };
};

std::array<float, 12> scatter_instance_rows(const ScatterInstance &p_instance);

using ScatterFootprint = std::optional<std::array<float, 4>>;

struct ScatterBatch {
	uint32_t type = 0;
	uint32_t candidates = 0;
	uint32_t evaluated = 0;
	double coverage = 0.0;
	std::vector<ScatterInstance> instances;
};

struct ScatterCellId {
	uint8_t level = 0;
	int32_t x = 0;
	int32_t z = 0;
	friend bool operator==(const ScatterCellId &, const ScatterCellId &) = default;
};

struct ScatterCellStats {
	uint64_t candidates = 0;
	uint64_t masked = 0;
	uint64_t evaluated = 0;
	uint64_t instances = 0;
	uint64_t unmatched = 0;
	double coverage = 0.0;
};

class TerrainScatter {
public:
	struct Input {
		const SceneScatterPlan *plan = nullptr;
		const core::res::TerrainStreamingTree *tree = nullptr;
		const core::res::HeightGrid *heights = nullptr;
		std::optional<std::array<float, 4>> crop;
		core::res::TerrainChunkFetch fetch;
		std::function<core::Result<ScatterImage>(const std::string &)> image;
	};

	static core::Result<std::shared_ptr<const TerrainScatter>> build(
			const Input &p_input, std::vector<std::pair<std::string, std::string>> &r_failures);

	std::span<const ScatterType> types() const { return types_; }
	const std::vector<uint8_t> &levels() const { return levels_; }
	const std::array<float, 4> &bounds() const { return bounds_; }
	float cell_size(uint8_t p_level) const;
	float view_distance(uint8_t p_level) const { return cell_size(p_level); }
	std::array<float, 4> cell_rect(const ScatterCellId &p_cell) const;
	std::vector<ScatterCellId> cells_within(uint8_t p_level, float p_x, float p_z, float p_distance) const;

	std::vector<ScatterBatch> generate(const ScatterCellId &p_cell, ScatterCellStats *r_stats = nullptr,
			std::span<const ScatterFootprint> p_footprints = {}) const;
	std::optional<std::array<float, 2>> mask_scale(uint32_t p_type, float p_x, float p_z) const;

	const core::res::TerrainMaskField &masks() const { return masks_; }
	const ScatterHeights &heights() const { return heights_; }
	size_t programs() const { return programs_.size(); }
	uint64_t bytes() const;

private:
	struct Program {
		std::vector<uint8_t> layers;
		std::vector<uint8_t> scattered;
		MaskScaleProgram code;
	};

	int32_t program_for(uint8_t p_layer, uint64_t p_present) const;

	std::vector<ScatterType> types_;
	std::vector<uint8_t> levels_;
	std::array<float, 4> bounds_{};
	std::array<float, 4> drawn_{};
	core::res::TerrainMaskField masks_;
	ScatterHeights heights_;
	std::vector<ScatterImage> images_;
	std::vector<Program> programs_;
};

inline constexpr uint32_t kScatterImageMaxSide = 512;

}
