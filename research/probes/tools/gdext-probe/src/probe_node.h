#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/navigation_mesh.hpp>
#include <godot_cpp/classes/navigation_mesh_source_geometry_data3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/streamed_texture2d.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>

namespace godot {

// One node that runs every probe in sequence over several frames and quits.
// Results are printed as lines starting with "[probe]" so they can be grepped from the log.
class GDExtProbe : public Node3D {
	GDCLASS(GDExtProbe, Node3D)

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _process(double p_delta) override;
	void _physics_process(double p_delta) override;
	void _exit_tree() override;

	void on_nav_baked();

private:
	using Clock = std::chrono::steady_clock;

	// configuration from user args (after "--")
	std::vector<int> nav_sizes = { 250, 1000 };
	double nav_cell = 0.25;
	bool nav_monotone_too = true;
	bool skip_nav = false;
	bool nav_chunks_only = false;
	double nav_border = 1.0;
	int nav_chunk_size = 0;
	bool skip_thread_race = false;
	int render_frames = 120;

	int phase = 0;
	int phase_frame = 0;
	int physics_frames = 0;
	bool physics_done = false;
	bool headless = false;
	Clock::time_point last_frame;
	Clock::time_point start_time;
	std::vector<double> frame_ms;

	// (a)
	RID rs_mesh;
	std::vector<RID> rs_meshes;
	std::vector<RID> rs_instances;
	Camera3D *camera = nullptr;

	// (b)
	RID ph_concave_shape, ph_concave_body;
	RID ph_hm_shape, ph_hm_body;
	RID ph_hm2_shape, ph_hm2_body;
	RID ph_capsule_shape, ph_capsule_body;
	int concave_grid = 224; // 224*224*2 = 100,352 triangles
	double concave_spacing = 2.0;
	std::thread race_thread;
	std::atomic<bool> race_stop{ false };
	std::atomic<int> race_done{ 0 };
	std::atomic<int> race_hits{ 0 };
	int race_physics_start = 0;
	RID race_sphere_shape;
	std::vector<RID> race_bodies;

	// (d)
	Ref<StreamedTexture2D> streamed;
	Ref<ImageTexture> image_tex;
	MeshInstance3D *tex_quad = nullptr;
	MeshInstance3D *tex_quad2 = nullptr;

	// (e)
	Ref<NavigationMesh> nav_async_mesh;
	Ref<NavigationMeshSourceGeometryData3D> nav_async_src;
	Clock::time_point nav_async_start;
	bool nav_async_done = false;
	double nav_async_cb_ms = -1.0;
	int nav_async_frames = 0;
	double nav_async_max_frame_ms = 0.0;
	bool nav_winding_flip = false;
	bool nav_cb_on_main = false;
	std::thread::id main_thread_id;
	std::vector<Ref<NavigationMesh>> nav_chunks;
	std::vector<Ref<NavigationMeshSourceGeometryData3D>> nav_chunk_srcs;
	std::atomic<int> nav_chunks_left{ 0 };
	bool nav_chunk_started = false;
	Clock::time_point nav_chunk_start;
	double nav_area(const Ref<NavigationMesh> &p_mesh);
	void probe_nav_chunks_start();

public:
	void on_nav_chunk_baked();

private:

	void parse_args();
	void log(const std::string &p_line);
	void next_phase();

	void probe_info();
	void probe_project_settings();
	void probe_rendering_create();
	void probe_rendering_frames_report(const char *p_label);
	void probe_rendering_free();
	void probe_physics_create();
	void probe_physics_queries();
	void probe_physics_threads();
	void probe_physics_thread_race_start();
	void probe_physics_thread_race_stop();
	void probe_texture_streaming_info(const char *p_label);
	void probe_textures_create();
	void probe_nav_winding();
	void probe_nav_sync();
	void probe_nav_async_start();
	void probe_enet();
	void save_screenshot(const char *p_name);
	void build_terrain(int p_size, double p_step, std::vector<float> &r_verts, std::vector<int32_t> &r_indices, bool p_flip);
};

} // namespace godot
