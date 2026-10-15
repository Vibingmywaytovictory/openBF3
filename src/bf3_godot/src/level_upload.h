#pragma once
// Phase C of the M1 level load (docs/design/m1.md §11.2-§11.4, §12.1): the prepared scene (bf3_render
// level_scene.h) into RenderingServer resources.
//
// * SceneSink: the scene lanes hand their items over on the lane threads; the sink only queues them (never blocks,
//   never calls Godot).
// * C1, Godot WorkerThreadPool tasks: the main thread submits short tasks (high priority behind the loading screen)
//   while fewer than max_tasks (3) are in flight; each takes queued items worth about task_budget_ms (4 ms) of work,
//   creates their RenderingServer resources on the calling worker (texture_2d_create from an Image; for meshes
//   mesh_create_from_surfaces with surface dictionaries in the RenderingServer's own vertex layout, below), frees the
//   CPU copies, calls SceneLanes::release_bytes and ends. No task waits for items, so C1 never holds every pool
//   thread (project.godot caps the pool at 4, docs §3) and Godot's own pool work always finds one. These two calls are
//   the documented exception to "Godot calls on the main thread only" (gap load §6.6: both run on the calling thread).
// * C2, main thread, at most p_budget_ms per frame (12 ms behind the loading screen, vsync off; docs §12.1): the
//   shaders and materials (RenderingServer shader_create / material_create, which are queued to the main thread
//   anyway), the texture bindings of the materials as textures arrive, and instance_create2 / instance_set_transform
//   for each instance of each mesh that is ready (RS instances, not nodes, plan §2.5).
//
// Materials (docs §11.3): one RS material per SceneMaterialPlan on one of four spatial shaders (opaque or alpha
// scissor x back-face culled or double-sided) with the plan's albedo (sRGB) and normal map (two channels, Z rebuilt by
// Godot), the DiffuseDarkening tint and the SmoothnessRegular roughness, as render_bridge's make_standard_material
// reads them. A material without an albedo texture draws flat grey (the T5 note: 273 of Metro's 602 materials name no
// texture); a texture that fails to prepare is listed and its materials draw flat (docs §11.3 "never fails the load").
// Terrains draw with one flat material each (the plan's colour).
//
// Surface layout (Godot 4.2+ "version 2", uncompressed; verified against mesh_add_surface_from_arrays by
// --m1-check's upload.surface_format row): vertex_data = positions (12 B each) followed by one interleaved
// normal + tangent stream (two octahedral uint16 pairs each); attribute_data = the UV streams interleaved; index_data
// = uint16 indices for <= 65,536 vertices, else uint32. Without prepared tangents the tangent is derived from the
// normal exactly as Godot derives it, so a mesh without tangents gets the bytes Godot itself would write.

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

// One RenderingServer surface dictionary (mesh_create_from_surfaces / mesh_add_surface) for a prepared surface;
// p_material may be invalid. Any thread (pure CPU work plus the RenderingServer's format queries).
godot::Dictionary encode_rs_surface(const bf3::render::PreparedSurface &p_surface, const godot::RID &p_material);

struct UploadStats {
	// C1 (worker tasks)
	uint64_t tasks = 0;
	double c1_work_seconds = 0.0; // summed over tasks
	double c1_max_task_ms = 0.0;
	uint64_t meshes = 0; // RS meshes created (plan meshes)
	uint64_t terrains = 0;
	uint64_t textures = 0;
	uint64_t surfaces = 0;
	uint64_t vertices = 0;
	uint64_t triangles = 0;
	uint64_t mesh_bytes = 0; // prepared bytes handed over by the lanes
	uint64_t texture_bytes = 0;
	uint64_t empty_meshes = 0; // no drawable surface left (no instance)
	// C2 (main thread)
	double c2_work_seconds = 0.0; // summed over frames
	uint64_t c2_frames = 0;
	double c2_max_frame_ms = 0.0;
	uint64_t materials = 0;
	uint64_t texture_bindings = 0;
	uint64_t instances = 0;
	// Lanes -> sink
	uint64_t failures = 0;
	uint64_t queued_peak = 0; // items waiting for a task at once
	// Wall clock (steady_clock microseconds, level_load.h's clock): first item queued, last resource created.
	uint64_t first_item_us = 0;
	uint64_t last_c1_us = 0;
	uint64_t last_c2_us = 0;
};

class LevelUpload final : public bf3::render::SceneSink {
public:
	struct Options {
		unsigned max_tasks = 3; // C1 tasks in flight (docs §3: one of the pool's 4 threads stays free)
		double task_budget_ms = 4.0; // work per task before it ends
	};

	LevelUpload();
	~LevelUpload() override;
	LevelUpload(const LevelUpload &) = delete;
	LevelUpload &operator=(const LevelUpload &) = delete;

	// Before SceneLanes::start (any thread): the lanes whose bytes the tasks release.
	void set_lanes(bf3::render::SceneLanes *p_lanes) { lanes_ = p_lanes; }
	// Main thread, once the plan exists (the lanes may already be handing items over): precomputes the instance and
	// texture lists, creates the shaders. p_progress gets the Meshes / Materials / Textures / Upload counts.
	void begin(std::shared_ptr<const bf3::render::LevelScenePlan> p_plan, const godot::RID &p_scenario, bf3::host::LoadProgress *p_progress,
			const Options &p_options);
	bool begun() const { return plan_ != nullptr; }
	// Any thread (the loader, after SceneLanes::wait): no more sink calls will come.
	void lanes_finished();
	bool lanes_done() const { return lanes_done_.load(std::memory_order_acquire); }

	// SceneSink (lane threads): queue only.
	void mesh(bf3::render::PreparedSceneMesh &&p_mesh, uint64_t p_bytes) override;
	void texture(uint32_t p_texture, bf3::render::PreparedTexture &&p_data, uint64_t p_bytes) override;
	void terrain(uint32_t p_terrain, bf3::render::PreparedMesh &&p_mesh, uint64_t p_bytes) override;
	void failure(const std::string &p_item, const std::string &p_error) override;

	// Main thread, every frame: reaps finished tasks, submits C1 tasks (p_task runs run_task on a pool thread), then
	// runs C2 work for at most p_budget_ms. Returns the C2 milliseconds spent.
	double pump(const godot::Callable &p_task, double p_budget_ms);
	// A WorkerThreadPool thread (through p_task).
	void run_task();
	// Everything uploaded: the lanes finished, nothing queued or in flight, every ready mesh instanced.
	bool finished() const;
	// Main thread: waits for the tasks in flight (shutdown or a failed load).
	void wait_tasks();
	// Main thread: frees every RS object this upload made (after wait_tasks).
	void free_all();

	UploadStats stats() const;
	std::vector<std::pair<std::string, std::string>> failures() const;
	size_t mesh_rids() const;
	const std::vector<godot::RID> &instances() const { return instances_; }
	godot::RID mesh_rid(uint32_t p_mesh) const; // invalid until uploaded
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

	// Lane threads -> tasks.
	mutable std::mutex queue_mutex_;
	std::deque<Item> queue_;
	std::vector<std::pair<std::string, std::string>> failures_;
	std::atomic<bool> lanes_done_{ false };

	// Tasks -> main thread.
	mutable std::mutex done_mutex_;
	std::vector<Done> done_;
	std::atomic<uint32_t> in_flight_{ 0 };
	std::vector<int64_t> task_ids_; // main thread: submitted, not yet reaped

	// Main thread state (the tasks read shaders_ / materials_ / terrain_materials_ after materials_ready_).
	std::atomic<bool> materials_ready_{ false };
	std::vector<godot::RID> shaders_; // 4 variants
	std::vector<godot::RID> materials_; // per SceneMaterialPlan
	std::vector<godot::RID> terrain_materials_; // per SceneTerrainPlan
	size_t next_material_ = 0;
	std::vector<godot::RID> textures_; // per SceneTexturePlan
	std::vector<godot::RID> meshes_; // per SceneMeshPlan
	std::vector<godot::RID> terrain_meshes_;
	std::vector<std::vector<uint32_t>> instances_by_mesh_; // plan instance indices per plan mesh
	std::vector<std::vector<std::pair<uint32_t, bool>>> texture_users_; // per texture: (material, is normal map)
	std::deque<std::pair<uint32_t, size_t>> pending_instances_; // (mesh, next instance in its list)
	std::deque<uint32_t> pending_terrains_;
	std::vector<godot::RID> instances_;
	size_t meshes_done_ = 0;
	size_t textures_done_ = 0;

	mutable std::mutex stats_mutex_;
	UploadStats stats_;
};

} // namespace bf3::godot_side
