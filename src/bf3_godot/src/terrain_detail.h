#pragma once

#include "bf3/core/job_queue.h"
#include "bf3/render/terrain_detail.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

godot::Ref<godot::Image> image_from_bytes(uint32_t p_width, uint32_t p_height, godot::Image::Format p_format,
		const std::vector<uint8_t> &p_data, bool p_mipmaps);

struct DetailDrawStats {
	uint64_t windows = 0;
	uint64_t failures = 0;
	double build_seconds = 0.0;
	float mask_texel = 0.0f;
	float colour_texel = 0.0f;
	std::string last_error;
};

class TerrainDetailDraw {
public:
	TerrainDetailDraw() = default;
	~TerrainDetailDraw();
	TerrainDetailDraw(const TerrainDetailDraw &) = delete;
	TerrainDetailDraw &operator=(const TerrainDetailDraw &) = delete;

	void begin(std::string p_name, std::shared_ptr<const bf3::render::TerrainDetailSource> p_source,
			const bf3::render::TerrainDetailLayout &p_layout, std::vector<godot::RID> p_materials,
			const godot::Transform3D &p_terrain);
	bool begun() const { return source_ != nullptr; }

	void update(const godot::Vector3 &p_camera);
	void free_all();

	DetailDrawStats stats() const { return stats_; }

private:
	struct Built {
		bf3::render::TerrainDetail detail;
		godot::Ref<godot::Image> mask;
		godot::Ref<godot::Image> colour;
		double seconds = 0.0;
		std::string error;
	};

	void apply(Built &r_built);
	void report(const godot::Vector3 &p_local);

	std::string name_;
	std::shared_ptr<const bf3::render::TerrainDetailSource> source_;
	bf3::render::TerrainDetailLayout layout_;
	std::vector<godot::RID> materials_;
	godot::Transform3D terrain_;
	std::unique_ptr<bf3::core::JobQueue> jobs_;
	std::shared_ptr<std::mutex> ready_mutex_ = std::make_shared<std::mutex>();
	std::shared_ptr<std::optional<Built>> ready_ = std::make_shared<std::optional<Built>>();
	bool building_ = false;
	std::optional<bf3::render::TerrainDetail> shown_;
	godot::RID mask_texture_;
	godot::RID colour_texture_;
	DetailDrawStats stats_;
};

}
