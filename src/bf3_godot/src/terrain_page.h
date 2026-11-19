#pragma once

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

struct PageDrawStats {
	uint64_t near_windows = 0;
	float near_texel = 0.0f;
	float far_texel = 0.0f;
	float far_side = 0.0f;
	size_t materials = 0;
	std::optional<std::array<float, 3>> colour;
	std::optional<std::array<float, 3>> normal;
};

class TerrainPageDraw {
public:
	TerrainPageDraw() = default;
	~TerrainPageDraw();
	TerrainPageDraw(const TerrainPageDraw &) = delete;
	TerrainPageDraw &operator=(const TerrainPageDraw &) = delete;

	void begin(std::string p_name, const godot::RID &p_terrain_mesh, const godot::Transform3D &p_terrain,
			const godot::AABB &p_world_bounds, float p_near_texel, std::vector<godot::RID> p_materials);
	bool begun() const { return scenario_.is_valid(); }

	void update(const godot::Vector3 &p_camera, uint64_t p_ground);
	void free_all();

	PageDrawStats stats() const { return stats_; }

private:
	struct Page {
		godot::RID viewport;
		godot::RID camera;
		godot::RID texture;
	};

	Page make_page(uint32_t p_layer);
	void aim(const Page &p_page, float p_x, float p_z, float p_side) const;
	void set_param(const char *p_name, const godot::Variant &p_value) const;
	void report(const godot::Vector3 &p_camera);

	std::string name_;
	std::vector<godot::RID> materials_;
	godot::RID scenario_;
	godot::RID environment_;
	godot::RID instance_;
	std::array<Page, 2> near_{};
	std::array<godot::RID, 2> far_textures_{};
	bool far_pending_ = false;
	uint64_t requested_frame_ = 0;
	float low_ = 0.0f;
	float high_ = 0.0f;
	float near_side_ = 0.0f;
	std::array<float, 2> far_centre_{};
	std::optional<uint64_t> ground_;
	std::optional<std::array<float, 2>> shown_;
	std::optional<std::array<float, 2>> pending_;
	PageDrawStats stats_;
};

}
