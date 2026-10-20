#pragma once

#include "bf3/host/level_load.h"
#include "bf3/render/level_scene.h"

#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bf3::godot_side {

godot::Dictionary encode_rs_surface(const bf3::render::PreparedSurface &p_surface, const godot::RID &p_material);

struct UploadStats {

	uint64_t tasks = 0;
	double c1_work_seconds = 0.0;
	double c1_max_task_ms = 0.0;
	uint64_t meshes = 0;
	uint64_t terrains = 0;
	uint64_t textures = 0;
	uint64_t surfaces = 0;
	uint64_t vertices = 0;
	uint64_t triangles = 0;
	uint64_t mesh_bytes = 0;
	uint64_t texture_bytes = 0;
	uint64_t empty_meshes = 0;

	double c2_work_seconds = 0.0;
	uint64_t c2_frames = 0;
	double c2_max_frame_ms = 0.0;
	uint64_t materials = 0;
	uint64_t texture_bindings = 0;
	uint64_t instances = 0;

	uint64_t failures = 0;
	uint64_t queued_peak = 0;

	uint64_t first_item_us = 0;
	uint64_t last_c1_us = 0;
	uint64_t last_c2_us = 0;
};

class LevelUpload final : public bf3::render::SceneSink {
public:
	struct Options {
		unsigned max_tasks = 3;
		double task_budget_ms = 4.0;
	};

	LevelUpload();
	~LevelUpload() override;
	LevelUpload(const LevelUpload &) = delete;
	LevelUpload &operator=(const LevelUpload &) = delete;

	void set_lanes(bf3::render::SceneLanes *p_lanes) { lanes_ = p_lanes; }

	void begin(std::shared_ptr<const bf3::render::LevelScenePlan> p_plan, const godot::RID &p_scenario, bf3::host::LoadProgress *p_progress,
			const Options &p_options);
	bool begun() const { return plan_ != nullptr; }

	void lanes_finished();
	bool lanes_done() const { return lanes_done_.load(std::memory_order_acquire); }

	void mesh(bf3::render::PreparedSceneMesh &&p_mesh, uint64_t p_bytes) override;
	void texture(uint32_t p_texture, bf3::render::PreparedTexture &&p_data, uint64_t p_bytes) override;
	void terrain(uint32_t p_terrain, bf3::render::PreparedMesh &&p_mesh, uint64_t p_bytes) override;
	void failure(const std::string &p_item, const std::string &p_error) override;

	double pump(const godot::Callable &p_task, double p_budget_ms);

	void run_task();

	bool finished() const;

	void wait_tasks();

	void free_all();

	UploadStats stats() const;
	std::vector<std::pair<std::string, std::string>> failures() const;
	size_t mesh_rids() const;
	const std::vector<godot::RID> &instances() const { return instances_; }
	godot::RID mesh_rid(uint32_t p_mesh) const;
	godot::RID material_rid(uint32_t p_material) const;
	godot::RID texture_rid(uint32_t p_texture) const;

private:
	enum class Kind : uint8_t { Mesh, Texture, Terrain };
	struct Item {
		Kind kind = Kind::Mesh;
		uint32_t index = 0;
		uint64_t bytes = 0;
		std::unique_ptr<bf3::render::PreparedSceneMesh> mesh;
		std::unique_ptr<bf3::render::PreparedTexture> texture;
		std::unique_ptr<bf3::render::PreparedMesh> terrain;
	};
	struct Done {
		Kind kind = Kind::Mesh;
		uint32_t index = 0;
		godot::RID rid;
	};

	void push(Item &&p_item);
	void create_materials(double p_deadline_us);
	void apply_done(double p_deadline_us);
	void create_instances(double p_deadline_us);

	std::shared_ptr<const bf3::render::LevelScenePlan> plan_;
	bf3::render::SceneLanes *lanes_ = nullptr;
	bf3::host::LoadProgress *progress_ = nullptr;
	Options options_;
	godot::RID scenario_;

	mutable std::mutex queue_mutex_;
	std::deque<Item> queue_;
	std::vector<std::pair<std::string, std::string>> failures_;
	std::atomic<bool> lanes_done_{ false };

	mutable std::mutex done_mutex_;
	std::vector<Done> done_;
	std::atomic<uint32_t> in_flight_{ 0 };
	std::vector<int64_t> task_ids_;

	std::atomic<bool> materials_ready_{ false };
	std::vector<godot::RID> shaders_;
	std::vector<godot::RID> materials_;
	std::vector<godot::RID> terrain_materials_;
	size_t next_material_ = 0;
	std::vector<godot::RID> textures_;
	std::vector<godot::RID> meshes_;
	std::vector<godot::RID> terrain_meshes_;
	std::vector<std::vector<uint32_t>> instances_by_mesh_;
	std::vector<std::vector<std::pair<uint32_t, bool>>> texture_users_;
	std::deque<std::pair<uint32_t, size_t>> pending_instances_;
	std::deque<uint32_t> pending_terrains_;
	std::vector<godot::RID> instances_;
	size_t meshes_done_ = 0;
	size_t textures_done_ = 0;

	mutable std::mutex stats_mutex_;
	UploadStats stats_;
};

}
