// gdext-probe: proves godot-cpp against the exact Godot build and measures the APIs the
// BF3 reimplementation depends on. Every result line starts with "[probe]".

#include "probe_node.h"

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/e_net_connection.hpp>
#include <godot_cpp/classes/e_net_packet_peer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_intersect_ray_result3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/physics_test_motion_parameters3d.hpp>
#include <godot_cpp/classes/physics_test_motion_result3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture_streaming.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

namespace godot {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point p_t0) {
	return std::chrono::duration<double, std::milli>(Clock::now() - p_t0).count();
}

std::string fmt(const char *p_fmt, ...) {
	char buf[2048];
	va_list ap;
	va_start(ap, p_fmt);
	vsnprintf(buf, sizeof(buf), p_fmt, ap);
	va_end(ap);
	return std::string(buf);
}

std::string to_std(const String &p_s) {
	CharString cs = p_s.utf8();
	return std::string(cs.get_data());
}

double terrain_h(double x, double z) {
	return 4.0 * std::sin(x * 0.05) * std::cos(z * 0.037) + 1.5 * std::sin(x * 0.21 + z * 0.13);
}

struct Stats {
	double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};

Stats stats_of(std::vector<double> v) {
	Stats s;
	if (v.empty()) {
		return s;
	}
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double d : v) {
		sum += d;
	}
	s.mean = sum / v.size();
	s.p50 = v[v.size() / 2];
	s.p95 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.95))];
	s.p99 = v[std::min(v.size() - 1, (size_t)(v.size() * 0.99))];
	s.max = v.back();
	return s;
}

// A 24-vertex cube mesh through RenderingServer, scaled by p_s.
RID make_cube_mesh(RenderingServer *rs, float p_s) {
	static const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	PackedVector3Array verts, norms;
	PackedInt32Array idx;
	for (int f = 0; f < 6; f++) {
		Vector3 N(n[f][0], n[f][1], n[f][2]);
		Vector3 U = std::fabs(N.y) > 0.5f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
		Vector3 V = N.cross(U);
		int base = verts.size();
		Vector3 c = N * (0.5f * p_s);
		verts.push_back(c + (-U - V) * (0.5f * p_s));
		verts.push_back(c + (U - V) * (0.5f * p_s));
		verts.push_back(c + (U + V) * (0.5f * p_s));
		verts.push_back(c + (-U + V) * (0.5f * p_s));
		for (int k = 0; k < 4; k++) {
			norms.push_back(N);
		}
		// Godot front faces are clockwise; emit both orders is wasteful, pick (0,2,1),(0,3,2).
		idx.push_back(base + 0);
		idx.push_back(base + 2);
		idx.push_back(base + 1);
		idx.push_back(base + 0);
		idx.push_back(base + 3);
		idx.push_back(base + 2);
	}
	Array arrays;
	arrays.resize(RenderingServer::ARRAY_MAX);
	arrays[RenderingServer::ARRAY_VERTEX] = verts;
	arrays[RenderingServer::ARRAY_NORMAL] = norms;
	arrays[RenderingServer::ARRAY_INDEX] = idx;
	RID mesh = rs->mesh_create();
	rs->mesh_add_surface_from_arrays(mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

// DXT1 image with a full mip chain; every mip is a solid, distinct colour so screenshots show the resident mip.
Ref<Image> make_dxt1_image(int p_size) {
	static const uint16_t colours565[] = {
		0xF800, // 0 red
		0x07E0, // 1 green
		0x001F, // 2 blue
		0xFFE0, // 3 yellow
		0xF81F, // 4 magenta
		0x07FF, // 5 cyan
		0xFFFF, // 6 white
		0x8410, // 7 grey
		0xFC00, // 8 orange
		0x8010, // 9 purple
		0x0410, // 10 teal
		0x4208, // 11 dark grey
		0x0000, // 12 black
		0x0000, 0x0000, 0x0000
	};
	Ref<Image> layout = Image::create_empty(p_size, p_size, true, Image::FORMAT_DXT1);
	int64_t total = layout->get_data_size();
	int mips = layout->get_mipmap_count();
	PackedByteArray data;
	data.resize(total);
	uint8_t *w = data.ptrw();
	for (int m = 0; m <= mips; m++) {
		int64_t off = layout->get_mipmap_offset(m);
		int64_t end = (m < mips) ? layout->get_mipmap_offset(m + 1) : total;
		uint16_t c = colours565[std::min(m, 15)];
		for (int64_t b = off; b + 8 <= end; b += 8) {
			w[b + 0] = uint8_t(c & 0xFF);
			w[b + 1] = uint8_t(c >> 8);
			w[b + 2] = uint8_t(c & 0xFF);
			w[b + 3] = uint8_t(c >> 8);
			w[b + 4] = w[b + 5] = w[b + 6] = w[b + 7] = 0; // all texels = colour0
		}
	}
	return Image::create_from_data(p_size, p_size, true, Image::FORMAT_DXT1, data);
}

// Writes the GSTF v1 container read by StreamedTexture2D (scene/resources/streamed_texture.cpp, _save_data).
bool write_gstf(const String &p_path, const Ref<Image> &p_img) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	PackedByteArray data = p_img->get_data();
	int mips = p_img->get_mipmap_count();
	f->store_8('G');
	f->store_8('S');
	f->store_8('T');
	f->store_8('F');
	f->store_32(1); // version
	f->store_32(p_img->get_width());
	f->store_32(p_img->get_height());
	f->store_32(uint32_t(data.size()));
	f->store_32(uint32_t(p_img->get_format()));
	f->store_32(uint32_t(mips));
	for (int m = 0; m <= mips; m++) {
		int64_t off = p_img->get_mipmap_offset(m);
		int64_t end = (m < mips) ? p_img->get_mipmap_offset(m + 1) : data.size();
		f->store_32(uint32_t(off));
		f->store_32(uint32_t(end - off));
	}
	f->store_32(0); // flags
	f->store_32(0); // streaming_min (0 = project default)
	f->store_32(0); // streaming_max (0 = project default)
	f->store_32(0); // reserved
	f->store_buffer(data);
	f->close();
	return true;
}

} // namespace

void GDExtProbe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("on_nav_baked"), &GDExtProbe::on_nav_baked);
	ClassDB::bind_method(D_METHOD("on_nav_chunk_baked"), &GDExtProbe::on_nav_chunk_baked);
}

void GDExtProbe::log(const std::string &p_line) {
	UtilityFunctions::print(String::utf8(("[probe] " + p_line).c_str()));
}

void GDExtProbe::parse_args() {
	PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	for (int i = 0; i < args.size(); i++) {
		std::string a = to_std(args[i]);
		if (a.rfind("--nav=", 0) == 0) {
			nav_sizes.clear();
			std::string list = a.substr(6);
			size_t p = 0;
			while (p < list.size()) {
				size_t q = list.find(',', p);
				if (q == std::string::npos) {
					q = list.size();
				}
				nav_sizes.push_back(std::atoi(list.substr(p, q - p).c_str()));
				p = q + 1;
			}
		} else if (a.rfind("--cell=", 0) == 0) {
			nav_cell = std::atof(a.substr(7).c_str());
		} else if (a == "--chunks-only") {
			nav_chunks_only = true;
		} else if (a.rfind("--border=", 0) == 0) {
			nav_border = std::atof(a.substr(9).c_str());
		} else if (a.rfind("--chunk=", 0) == 0) {
			nav_chunk_size = std::atoi(a.substr(8).c_str());
		} else if (a == "--skip-nav") {
			skip_nav = true;
		} else if (a == "--no-monotone") {
			nav_monotone_too = false;
		} else if (a == "--skip-race") {
			skip_thread_race = true;
		} else if (a.rfind("--frames=", 0) == 0) {
			render_frames = std::atoi(a.substr(9).c_str());
		}
	}
}

void GDExtProbe::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	start_time = Clock::now();
	main_thread_id = std::this_thread::get_id();
	parse_args();
	headless = DisplayServer::get_singleton()->get_name() == String("headless");
	probe_info();
	probe_project_settings();
	probe_texture_streaming_info("startup");

	camera = memnew(Camera3D);
	add_child(camera);
	camera->set_position(Vector3(0, 90, 140));
	camera->look_at(Vector3(0, 0, 0), Vector3(0, 1, 0));
	camera->set_far(4000.0f);
	camera->make_current();
	DirectionalLight3D *sun = memnew(DirectionalLight3D);
	add_child(sun);
	sun->set_rotation_degrees(Vector3(-50, 30, 0));
	sun->set_shadow(true);

	probe_physics_create();
	probe_rendering_create();
	phase = 1;
	phase_frame = 0;
	last_frame = Clock::now();
	set_process(true);
	set_physics_process(true);
}

void GDExtProbe::next_phase() {
	phase++;
	phase_frame = 0;
	frame_ms.clear();
}

void GDExtProbe::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || phase == 0) {
		return;
	}
	double dt = ms_since(last_frame);
	last_frame = Clock::now();
	phase_frame++;
	if (ms_since(start_time) > 15 * 60 * 1000.0) {
		log("watchdog: 15 minutes elapsed, quitting");
		get_tree()->quit(2);
		phase = 99;
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();

	switch (phase) {
		case 1: // (a1) 10,000 instances of one mesh
		case 2: { // (a2) 10,000 instances over 1,000 meshes
			if (phase_frame > 5) {
				frame_ms.push_back(dt);
			}
			if (phase_frame == render_frames / 2) {
				log(fmt("RS frame %d info: objects=%lld primitives=%lld draw_calls=%lld texture_mem=%lld buffer_mem=%lld video_mem=%lld",
						phase_frame,
						(long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME),
						(long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME),
						(long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME),
						(long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED),
						(long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_BUFFER_MEM_USED),
						(long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_VIDEO_MEM_USED)));
				if (phase == 1) {
					save_screenshot("instances_one_mesh");
				}
			}
			if (phase_frame >= render_frames) {
				probe_rendering_frames_report(phase == 1 ? "a1 10k instances / 1 mesh" : "a2 10k instances / 1000 meshes");
				probe_rendering_free();
				if (phase == 1) {
					// rebuild with 1000 distinct meshes
					Clock::time_point t0 = Clock::now();
					for (int m = 0; m < 1000; m++) {
						rs_meshes.push_back(make_cube_mesh(rs, 0.5f + 0.001f * m));
					}
					double mesh_ms = ms_since(t0);
					RID scenario = get_world_3d()->get_scenario();
					t0 = Clock::now();
					for (int i = 0; i < 10000; i++) {
						RID inst = rs->instance_create2(rs_meshes[i % 1000], scenario);
						rs->instance_set_transform(inst, Transform3D(Basis(), Vector3(float((i % 100) - 50) * 2.0f, 0.5f, float((i / 100) - 50) * 2.0f)));
						rs_instances.push_back(inst);
					}
					log(fmt("a2 create: 1000 cube meshes via mesh_add_surface_from_arrays %.2f ms; 10000 instance_create2+set_transform %.2f ms",
							mesh_ms, ms_since(t0)));
				}
				next_phase();
			}
		} break;
		case 3: { // physics queries happen in _physics_process
			if (physics_done) {
				next_phase();
				probe_textures_create();
			}
		} break;
		case 4: { // (d) texture streaming observation, near
			double tn = ms_since(nav_async_start);
			if (phase_frame == 1 || phase_frame == 10 || phase_frame == 30 || phase_frame == 60 || phase_frame == 120 || phase_frame == 240 || (phase_frame % 3000) == 0) {
				probe_texture_streaming_info(fmt("near frame %d (%.0f ms into phase)", phase_frame, tn).c_str());
			}
			if (tn > 2500.0 && phase_frame >= 240) {
				probe_texture_streaming_info(fmt("near end frame %d (%.0f ms)", phase_frame, tn).c_str());
				save_screenshot("tex_near");
				// shrink the quads to a few pixels so feedback asks for a low mip
				tex_quad->set_position(Vector3(-0.6f, 0, -1500.0f));
				tex_quad2->set_position(Vector3(0.6f, 0, -1500.0f));
				next_phase();
				nav_async_start = Clock::now(); // reuse as phase timer
			}
		} break;
		case 5: { // (d) far
			double t = ms_since(nav_async_start);
			if (phase_frame == 1 || phase_frame == 60 || phase_frame == 240 || (phase_frame % 3000) == 0) {
				probe_texture_streaming_info(fmt("far frame %d (%.0f ms)", phase_frame, t).c_str());
			}
			if (t > 12000.0) {
				probe_texture_streaming_info("far end");
				// back to near for a final check that it streams up again
				tex_quad->set_position(Vector3(-0.6f, 0, -1.5f));
				tex_quad2->set_position(Vector3(0.6f, 0, -1.5f));
				next_phase();
				nav_async_start = Clock::now();
			}
		} break;
		case 6: {
			double t = ms_since(nav_async_start);
			if (t > 3000.0) {
				probe_texture_streaming_info("near again after 3 s");
				save_screenshot("tex_near_again");
				tex_quad->queue_free();
				tex_quad2->queue_free();
				tex_quad = tex_quad2 = nullptr;
				next_phase();
			}
		} break;
		case 7: { // (e) navigation
			if (skip_nav) {
				next_phase();
				next_phase();
				break;
			}
			probe_nav_winding();
			if (nav_chunks_only) {
				probe_nav_chunks_start();
			} else {
				probe_nav_sync();
				probe_nav_async_start();
			}
			next_phase();
		} break;
		case 8: { // poll async bake
			nav_async_frames++;
			if (phase_frame > 1) {
				nav_async_max_frame_ms = std::max(nav_async_max_frame_ms, dt);
				frame_ms.push_back(dt);
			}
			bool baking = NavigationServer3D::get_singleton()->is_baking_navigation_mesh(nav_async_mesh);
			if (!nav_chunk_started && (nav_async_done || (!baking && phase_frame > 2 && ms_since(nav_async_start) > 600000.0))) {
				Stats s = stats_of(frame_ms);
				log(fmt("e async: wall %.1f ms to callback (cb at %.1f ms, callback on main thread: %s), %d frames polled during bake, frame ms mean %.2f p95 %.2f max %.2f, polygons %lld, walkable area %.0f m2",
						ms_since(nav_async_start), nav_async_cb_ms, nav_cb_on_main ? "yes" : "no", nav_async_frames, s.mean, s.p95, s.max,
						(long long)nav_async_mesh->get_polygon_count(), nav_area(nav_async_mesh)));
				nav_async_mesh.unref();
				nav_async_src.unref();
				frame_ms.clear();
				probe_nav_chunks_start();
			} else if (nav_chunk_started && nav_chunks_left.load() == 0) {
				Stats s = stats_of(frame_ms);
				double area = 0;
				long long polys = 0;
				for (const Ref<NavigationMesh> &m : nav_chunks) {
					area += nav_area(m);
					polys += m->get_polygon_count();
				}
				log(fmt("e chunked async: %zu chunks done in %.1f ms wall; total polygons %lld, walkable area %.0f m2; frame ms during bakes mean %.2f p95 %.2f max %.2f",
						nav_chunks.size(), ms_since(nav_chunk_start), polys, area, s.mean, s.p95, s.max));
				nav_chunks.clear();
				nav_chunk_srcs.clear();
				next_phase();
			} else if (nav_chunk_started && ms_since(nav_chunk_start) > 600000.0) {
				log(fmt("e chunked async: timeout, %d chunks left", nav_chunks_left.load()));
				next_phase();
			}
		} break;
		case 9: {
			probe_enet();
			next_phase();
		} break;
		case 10: {
			if (skip_thread_race) {
				next_phase();
				break;
			}
			if (phase_frame == 1) {
				probe_physics_thread_race_start();
			} else {
				frame_ms.push_back(dt);
			}
			if (phase_frame >= 180) {
				probe_physics_thread_race_stop();
				next_phase();
			}
		} break;
		case 11: {
			log(fmt("done, total %.1f s; static memory %lld bytes, peak %lld bytes", ms_since(start_time) / 1000.0,
					(long long)OS::get_singleton()->get_static_memory_usage(), (long long)OS::get_singleton()->get_static_memory_peak_usage()));
			phase = 99;
			get_tree()->quit(0);
		} break;
		default:
			break;
	}
}

void GDExtProbe::_physics_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || phase == 0) {
		return;
	}
	physics_frames++;
	if (phase == 3 && !physics_done && physics_frames > 3) {
		probe_physics_queries();
		probe_physics_threads();
		physics_done = true;
	}
}

void GDExtProbe::_exit_tree() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	race_stop = true;
	if (race_thread.joinable()) {
		race_thread.join();
	}
	probe_rendering_free();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (RID b : race_bodies) {
		ps->free_rid(b);
	}
	race_bodies.clear();
	RID rids[] = { ph_concave_body, ph_hm_body, ph_hm2_body, ph_capsule_body, ph_concave_shape, ph_hm_shape, ph_hm2_shape, ph_capsule_shape };
	for (RID r : rids) {
		if (r.is_valid()) {
			ps->free_rid(r);
		}
	}
}

// ---------------------------------------------------------------- (g) and (c)

void GDExtProbe::probe_info() {
	Dictionary v = Engine::get_singleton()->get_version_info();
	log("g engine version_info: " + to_std(UtilityFunctions::str(v)));
	log(fmt("g compiled against GODOT_VERSION %d.%d.%d; real_t is %s (sizeof %d); godot-cpp precision macro %s",
			GODOT_VERSION_MAJOR, GODOT_VERSION_MINOR, GODOT_VERSION_PATCH,
			sizeof(real_t) == 8 ? "double" : "float", int(sizeof(real_t)),
#ifdef REAL_T_IS_DOUBLE
			"REAL_T_IS_DOUBLE"
#else
			"single"
#endif
			));
	OS *os = OS::get_singleton();
	log("g OS " + to_std(os->get_name()) + ", cpu " + to_std(os->get_processor_name()) + fmt(" x%d", os->get_processor_count()) +
			", display server " + to_std(DisplayServer::get_singleton()->get_name()) +
			", rendering method " + to_std(RenderingServer::get_singleton()->get_current_rendering_method()) +
			", driver " + to_std(RenderingServer::get_singleton()->get_current_rendering_driver_name()));
	log("g video adapter: " + to_std(RenderingServer::get_singleton()->get_video_adapter_name()) + " / " +
			to_std(RenderingServer::get_singleton()->get_video_adapter_vendor()) + " api " +
			to_std(RenderingServer::get_singleton()->get_video_adapter_api_version()));
	log("g PhysicsServer3D object: " + to_std(UtilityFunctions::str(PhysicsServer3D::get_singleton())) +
			fmt(" is_class(JoltPhysicsServer3D)=%d is_class(PhysicsServer3DWrapMT)=%d", int(PhysicsServer3D::get_singleton()->is_class("JoltPhysicsServer3D")), int(PhysicsServer3D::get_singleton()->is_class("PhysicsServer3DWrapMT"))) +
			", NavigationServer3D object: " + to_std(UtilityFunctions::str(NavigationServer3D::get_singleton())));
	log(fmt("g physics ticks/s %d, user dir ", Engine::get_singleton()->get_physics_ticks_per_second()) + to_std(os->get_user_data_dir()));
	log(fmt("g debug build of engine: %s; extension built with DEBUG_ENABLED: %s",
			os->is_debug_build() ? "yes" : "no",
#ifdef DEBUG_ENABLED
			"yes"
#else
			"no"
#endif
			));
}

void GDExtProbe::probe_project_settings() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	TypedArray<Dictionary> props = ps->get_property_list();
	int jolt = 0;
	for (int i = 0; i < props.size(); i++) {
		Dictionary d = props[i];
		String name = d["name"];
		std::string n = to_std(name);
		std::string lower = n;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		bool is_jolt = lower.find("jolt") != std::string::npos;
		bool is_stream = lower.find("textures/streaming") != std::string::npos;
		bool is_other = n == "physics/3d/physics_engine" || n == "physics/3d/run_on_separate_thread" ||
				n == "physics/common/physics_ticks_per_second" || n == "navigation/baking/thread_model/baking_use_multiple_threads" ||
				n == "navigation/baking/thread_model/baking_use_high_priority_threads" || n == "rendering/rendering_device/driver.windows" ||
				n == "navigation/baking/use_crash_prevention_checks" || n == "physics/3d/default_gravity";
		if (is_jolt) {
			jolt++;
		}
		if (is_jolt || is_stream || is_other) {
			log("c setting " + n + " = " + to_std(UtilityFunctions::str(ps->get_setting(name))) +
					fmt(" (type %d)", int(d["type"])));
		}
	}
	log(fmt("c %d project settings total, %d contain 'jolt'", int(props.size()), jolt));
}

// ---------------------------------------------------------------- (a)

void GDExtProbe::probe_rendering_create() {
	RenderingServer *rs = RenderingServer::get_singleton();
	Clock::time_point t0 = Clock::now();
	rs_mesh = make_cube_mesh(rs, 1.0f);
	double mesh_ms = ms_since(t0);
	RID scenario = get_world_3d()->get_scenario();
	t0 = Clock::now();
	for (int i = 0; i < 10000; i++) {
		RID inst = rs->instance_create2(rs_mesh, scenario);
		rs_instances.push_back(inst);
	}
	double create_ms = ms_since(t0);
	t0 = Clock::now();
	for (int i = 0; i < 10000; i++) {
		rs->instance_set_transform(rs_instances[i], Transform3D(Basis(), Vector3(float((i % 100) - 50) * 2.0f, 0.5f, float((i / 100) - 50) * 2.0f)));
	}
	double xform_ms = ms_since(t0);
	log(fmt("a1 create: cube mesh %.3f ms; 10000 instance_create2 %.2f ms (%.2f us each); 10000 instance_set_transform %.2f ms",
			mesh_ms, create_ms, create_ms * 1000.0 / 10000.0, xform_ms));
}

void GDExtProbe::probe_rendering_frames_report(const char *p_label) {
	Stats s = stats_of(frame_ms);
	log(fmt("%s: %d frames, frame ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f (%s)", p_label, int(frame_ms.size()), s.mean, s.p50, s.p95, s.p99, s.max,
			headless ? "headless dummy renderer: not GPU work" : "windowed"));
}

void GDExtProbe::probe_rendering_free() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return;
	}
	Clock::time_point t0 = Clock::now();
	size_t n = rs_instances.size();
	for (RID r : rs_instances) {
		rs->free_rid(r);
	}
	rs_instances.clear();
	for (RID r : rs_meshes) {
		rs->free_rid(r);
	}
	rs_meshes.clear();
	if (rs_mesh.is_valid()) {
		rs->free_rid(rs_mesh);
		rs_mesh = RID();
	}
	if (n) {
		log(fmt("a free: %zu instances + meshes freed in %.2f ms", n, ms_since(t0)));
	}
}

void GDExtProbe::save_screenshot(const char *p_name) {
	if (headless) {
		return;
	}
	Ref<ViewportTexture> vt = get_viewport()->get_texture();
	if (vt.is_null()) {
		return;
	}
	Ref<Image> img = vt->get_image();
	if (img.is_null()) {
		log("screenshot: no image");
		return;
	}
	String path = String("user://") + p_name + ".png";
	img->save_png(path);
	log("screenshot " + to_std(ProjectSettings::get_singleton()->globalize_path(path)));
}

// ---------------------------------------------------------------- (b)

void GDExtProbe::probe_physics_create() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	RID space = get_world_3d()->get_space();

	// ConcavePolygonShape3D, ~100k triangles, centred at origin.
	const int N = concave_grid;
	const double S = concave_spacing;
	const double half = N * S * 0.5;
	PackedVector3Array faces;
	faces.resize(N * N * 6);
	Vector3 *fw = faces.ptrw();
	int k = 0;
	for (int z = 0; z < N; z++) {
		for (int x = 0; x < N; x++) {
			double x0 = x * S - half, x1 = (x + 1) * S - half, z0 = z * S - half, z1 = (z + 1) * S - half;
			Vector3 a(x0, terrain_h(x0, z0), z0), b(x1, terrain_h(x1, z0), z0), c(x1, terrain_h(x1, z1), z1), d(x0, terrain_h(x0, z1), z1);
			fw[k++] = a;
			fw[k++] = b;
			fw[k++] = c;
			fw[k++] = a;
			fw[k++] = c;
			fw[k++] = d;
		}
	}
	Clock::time_point t0 = Clock::now();
	ph_concave_shape = ps->concave_polygon_shape_create();
	Dictionary cd;
	cd["faces"] = faces;
	cd["backface_collision"] = false;
	ps->shape_set_data(ph_concave_shape, cd);
	double shape_ms = ms_since(t0);
	t0 = Clock::now();
	ph_concave_body = ps->body_create();
	ps->body_set_mode(ph_concave_body, PhysicsServer3D::BODY_MODE_STATIC);
	ps->body_add_shape(ph_concave_body, ph_concave_shape);
	ps->body_set_space(ph_concave_body, space);
	double body_ms = ms_since(t0);
	log(fmt("b concave: %d triangles, shape_set_data %.2f ms, body create+add+space %.2f ms", N * N * 2, shape_ms, body_ms));

	// HeightMapShape3D 1025x1025 (2^n+1) at x=+1000 and 1024x1024 at x=-1000.
	auto make_hm = [&](int p_w, Vector3 p_origin, RID &r_shape, RID &r_body, const char *p_label) {
		PackedFloat32Array heights;
		heights.resize(p_w * p_w);
		float *hw = heights.ptrw();
		float mn = 1e9f, mx = -1e9f;
		for (int z = 0; z < p_w; z++) {
			for (int x = 0; x < p_w; x++) {
				// HeightMapShape3D is centred on its body: sample (x,z) sits at local (x-(w-1)/2, z-(w-1)/2).
				float h = float(terrain_h(p_origin.x + x - (p_w - 1) * 0.5, p_origin.z + z - (p_w - 1) * 0.5));
				hw[z * p_w + x] = h;
				mn = std::min(mn, h);
				mx = std::max(mx, h);
			}
		}
		Clock::time_point t1 = Clock::now();
		r_shape = ps->heightmap_shape_create();
		Dictionary hd;
		hd["width"] = p_w;
		hd["depth"] = p_w;
		hd["heights"] = heights;
		hd["min_height"] = mn;
		hd["max_height"] = mx;
		ps->shape_set_data(r_shape, hd);
		double sm = ms_since(t1);
		t1 = Clock::now();
		r_body = ps->body_create();
		ps->body_set_mode(r_body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_add_shape(r_body, r_shape);
		ps->body_set_state(r_body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), p_origin));
		ps->body_set_space(r_body, space);
		log(fmt("b %s: %dx%d heights, shape_set_data %.2f ms, body %.2f ms", p_label, p_w, p_w, sm, ms_since(t1)));
	};
	make_hm(1025, Vector3(1000, 0, 0), ph_hm_shape, ph_hm_body, "heightmap 2^10+1");
	make_hm(1024, Vector3(-1000, 0, 0), ph_hm2_shape, ph_hm2_body, "heightmap 2^10");

	// Capsule kinematic body for body_test_motion.
	ph_capsule_shape = ps->capsule_shape_create();
	Dictionary capd;
	capd["radius"] = 0.4;
	capd["height"] = 1.8;
	ps->shape_set_data(ph_capsule_shape, capd);
	ph_capsule_body = ps->body_create();
	ps->body_set_mode(ph_capsule_body, PhysicsServer3D::BODY_MODE_KINEMATIC);
	ps->body_add_shape(ph_capsule_body, ph_capsule_shape);
	ps->body_set_state(ph_capsule_body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), Vector3(0, 50, 0)));
	ps->body_set_space(ph_capsule_body, space);

	// Is a just-created static body visible to queries before the first physics step?
	PhysicsDirectSpaceState3D *ss = ps->space_get_direct_state(space);
	Ref<PhysicsRayQueryParameters3D> q;
	q.instantiate();
	q->set_from(Vector3(1.0f, 100, 1.0f));
	q->set_to(Vector3(1.0f, -100, 1.0f));
	Dictionary r = ss->intersect_ray(q);
	log(std::string("b immediate ray (same frame as body creation, before any physics step): ") + (r.is_empty() ? "MISS" : "HIT " + to_std(UtilityFunctions::str(r["position"]))));
}

void GDExtProbe::probe_physics_queries() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	RID space = get_world_3d()->get_space();
	PhysicsDirectSpaceState3D *ss = ps->space_get_direct_state(space);
	std::mt19937 rng(1234);
	const double half = concave_grid * concave_spacing * 0.5 - 1.0;
	std::uniform_real_distribution<double> ux(-half, half);
	std::uniform_real_distribution<double> uh(0.0, 1022.0);

	struct Ray {
		Vector3 from, to;
	};
	auto run = [&](const char *p_label, const std::vector<Ray> &p_rays, bool p_into) {
		Ref<PhysicsRayQueryParameters3D> q;
		q.instantiate();
		Ref<PhysicsIntersectRayResult3D> res;
		res.instantiate();
		int hits = 0;
		double err_sum = 0;
		Clock::time_point t0 = Clock::now();
		for (const Ray &ray : p_rays) {
			q->set_from(ray.from);
			q->set_to(ray.to);
			if (p_into) {
				if (ss->intersect_ray_into(q, res)) {
					hits++;
					Vector3 p = res->get_position();
					err_sum += std::fabs(p.y - terrain_h(p.x, p.z));
				}
			} else {
				Dictionary r = ss->intersect_ray(q);
				if (!r.is_empty()) {
					hits++;
					Vector3 p = r["position"];
					err_sum += std::fabs(p.y - terrain_h(p.x, p.z));
				}
			}
		}
		double ms = ms_since(t0);
		log(fmt("b rays %s (%s): %zu rays %.2f ms (%.2f us/ray), hits %d, mean |y-h| %.3f m", p_label, p_into ? "intersect_ray_into" : "intersect_ray",
				p_rays.size(), ms, ms * 1000.0 / p_rays.size(), hits, hits ? err_sum / hits : 0.0));
	};

	std::vector<Ray> down_concave, down_hm, side_concave;
	for (int i = 0; i < 10000; i++) {
		double x = ux(rng), z = ux(rng);
		down_concave.push_back({ Vector3(x, 100, z), Vector3(x, -100, z) });
		double hx = uh(rng) - 511.0, hz = uh(rng) - 511.0;
		// heightmap is centred on its body origin (1000,0,0); terrain_h was sampled at (x+1000, z) for grid x,z in [0,1024]
		down_hm.push_back({ Vector3(1000 + hx, 100, hz), Vector3(1000 + hx, -100, hz) });
		double x2 = ux(rng) * 0.7, z2 = ux(rng) * 0.7;
		side_concave.push_back({ Vector3(x2, 2.0, z2), Vector3(x2 + 100.0, 2.0, z2 + 30.0) });
	}
	run("down/concave", down_concave, false);
	run("down/concave", down_concave, true);
	run("down/heightmap1025", down_hm, false);
	run("down/heightmap1025", down_hm, true);
	run("100m horizontal/concave", side_concave, true);

	// body_test_motion with the capsule: 1,000 drops of 10 m onto the concave terrain.
	Ref<PhysicsTestMotionParameters3D> mp;
	mp.instantiate();
	Ref<PhysicsTestMotionResult3D> mr;
	mr.instantiate();
	int col = 0;
	double depth_err = 0;
	Clock::time_point t0 = Clock::now();
	for (int i = 0; i < 1000; i++) {
		double x = ux(rng) * 0.9, z = ux(rng) * 0.9;
		double ground = terrain_h(x, z);
		mp->set_from(Transform3D(Basis(), Vector3(x, ground + 6.0, z)));
		mp->set_motion(Vector3(0, -10, 0));
		if (ps->body_test_motion(ph_capsule_body, mp, mr)) {
			col++;
			// capsule bottom = centre - 0.9; travel should stop near ground + 0.9
			double centre_y = ground + 6.0 + mr->get_travel().y;
			depth_err += std::fabs((centre_y - 0.9) - terrain_h(x, z));
		}
	}
	double ms = ms_since(t0);
	log(fmt("b body_test_motion capsule r0.4 h1.8: 1000 tests %.2f ms (%.2f us each), collided %d, mean |foot-h| %.3f m (slopes make this > 0)",
			ms, ms, col, col ? depth_err / col : 0.0));
}

void GDExtProbe::probe_physics_threads() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PhysicsDirectSpaceState3D *ss = ps->space_get_direct_state(get_world_3d()->get_space());
	const double half = concave_grid * concave_spacing * 0.5 - 1.0;
	const int total = 40000;
	for (int threads : { 1, 2, 4, 8 }) {
		std::atomic<int> hits{ 0 };
		Clock::time_point t0 = Clock::now();
		std::vector<std::thread> pool;
		for (int t = 0; t < threads; t++) {
			pool.emplace_back([&, t]() {
				std::mt19937 rng(777 + t);
				std::uniform_real_distribution<double> ux(-half, half);
				Ref<PhysicsRayQueryParameters3D> q;
				q.instantiate();
				Ref<PhysicsIntersectRayResult3D> res;
				res.instantiate();
				int h = 0;
				for (int i = 0; i < total / threads; i++) {
					double x = ux(rng), z = ux(rng);
					q->set_from(Vector3(x, 100, z));
					q->set_to(Vector3(x, -100, z));
					if (ss->intersect_ray_into(q, res)) {
						h++;
					}
				}
				hits += h;
			});
		}
		for (std::thread &th : pool) {
			th.join();
		}
		double ms = ms_since(t0);
		log(fmt("b threads (main thread blocked in _physics_process, space not stepping): %d std::thread(s) x %d rays: %.2f ms wall, %.0f rays/ms, hits %d/%d",
				threads, total / threads, ms, total / ms, hits.load(), total));
	}
}

void GDExtProbe::probe_physics_thread_race_start() {
	// A worker thread casts rays continuously while the main loop keeps running frames and stepping
	// physics (500 falling spheres make the steps non-trivial). Started here, stopped some frames later.
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	RID space = get_world_3d()->get_space();
	race_sphere_shape = ps->sphere_shape_create();
	ps->shape_set_data(race_sphere_shape, 0.5);
	for (int i = 0; i < 500; i++) {
		RID b = ps->body_create();
		ps->body_set_mode(b, PhysicsServer3D::BODY_MODE_RIGID);
		ps->body_add_shape(b, race_sphere_shape);
		ps->body_set_state(b, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), Vector3(float((i % 25) - 12) * 3.0f, 20.0f + float(i / 25) * 2.0f, float((i / 25) % 20 - 10) * 3.0f)));
		ps->body_set_space(b, space);
		race_bodies.push_back(b);
	}
	PhysicsDirectSpaceState3D *ss = ps->space_get_direct_state(space);
	const double half = concave_grid * concave_spacing * 0.5 - 1.0;
	race_stop = false;
	race_done = 0;
	race_hits = 0;
	race_physics_start = physics_frames;
	race_thread = std::thread([this, ss, half]() {
		std::mt19937 rng(99);
		std::uniform_real_distribution<double> ux(-half, half);
		Ref<PhysicsRayQueryParameters3D> q;
		q.instantiate();
		Ref<PhysicsIntersectRayResult3D> res;
		res.instantiate();
		while (!race_stop.load()) {
			double x = ux(rng), z = ux(rng);
			q->set_from(Vector3(x, 100, z));
			q->set_to(Vector3(x, -100, z));
			if (ss->intersect_ray_into(q, res)) {
				race_hits++;
			}
			race_done++;
		}
	});
	log("b race: worker thread started (500 rigid spheres falling, main loop keeps stepping)");
}

void GDExtProbe::probe_physics_thread_race_stop() {
	race_stop = true;
	if (race_thread.joinable()) {
		race_thread.join();
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (RID b : race_bodies) {
		ps->free_rid(b);
	}
	race_bodies.clear();
	if (race_sphere_shape.is_valid()) {
		ps->free_rid(race_sphere_shape);
		race_sphere_shape = RID();
	}
	Stats s = stats_of(frame_ms);
	log(fmt("b race: worker cast %d rays (%d hits) across %d physics steps / %d frames; frame ms mean %.2f max %.2f; see engine errors above (if any) for 'while stepping' complaints",
			race_done.load(), race_hits.load(), physics_frames - race_physics_start, phase_frame, s.mean, s.max));
}

// ---------------------------------------------------------------- (d)

void GDExtProbe::probe_texture_streaming_info(const char *p_label) {
	TextureStreaming *ts = TextureStreaming::get_singleton();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!ts) {
		log(std::string("d TextureStreaming singleton is null (") + p_label + ")");
		return;
	}
	log(fmt("d TextureStreaming [%s]: bytes_used %lld, min_lod_override %d, max_lod_override %d, memory_budget_mb_override %lld; RS texture_mem %lld; streamed tex size %s",
			p_label, (long long)ts->get_memory_budget_bytes_used(), int(ts->get_min_lod_override()), int(ts->get_max_lod_override()),
			(long long)ts->get_memory_budget_mb_override(), (long long)rs->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED),
			streamed.is_valid() ? to_std(UtilityFunctions::str(streamed->get_size())).c_str() : "-"));
}

void GDExtProbe::probe_textures_create() {
	RenderingServer *rs = RenderingServer::get_singleton();
	probe_texture_streaming_info("before textures");
	Clock::time_point t0 = Clock::now();
	Ref<Image> img = make_dxt1_image(2048);
	double img_ms = ms_since(t0);
	log(fmt("d runtime DXT1 Image 2048^2: format %d, mipmaps %d, data %lld bytes, is_compressed %d, built in %.2f ms",
			int(img->get_format()), int(img->get_mipmap_count()), (long long)img->get_data_size(), int(img->is_compressed()), img_ms));
	{
		Ref<Image> d = img->duplicate();
		d->decompress();
		std::string cols;
		for (int m = 0; m <= 4; m++) {
			int64_t off = d->get_mipmap_offset(m);
			PackedByteArray px = d->get_data();
			cols += fmt(" mip%d=(%d,%d,%d,%d)", m, px[off], px[off + 1], px[off + 2], d->get_format() == Image::FORMAT_RGBA8 ? int(px[off + 3]) : -1);
		}
		log(fmt("d decompressed check: format %d,", int(d->get_format())) + cols);
	}
	int64_t tm0 = rs->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
	t0 = Clock::now();
	image_tex = ImageTexture::create_from_image(img);
	log(fmt("d ImageTexture::create_from_image %.2f ms; format %d has_mipmaps %d; RS texture_mem delta %lld",
			ms_since(t0), int(image_tex->get_format()), int(image_tex->has_mipmaps()),
			(long long)(rs->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED) - tm0)));
	probe_texture_streaming_info("after ImageTexture");

	String path = "user://probe_dxt1_2048.stex";
	t0 = Clock::now();
	bool ok = write_gstf(path, img);
	double wms = ms_since(t0);
	streamed.instantiate();
	t0 = Clock::now();
	Error e = streamed->load(path);
	log(fmt("d GSTF write %s %.2f ms; StreamedTexture2D::load -> error %d in %.2f ms; size %dx%d format %d has_mipmaps %d", ok ? "ok" : "FAILED", wms, int(e),
			ms_since(t0), int(streamed->get_width()), int(streamed->get_height()), int(streamed->get_format()), int(streamed->has_mipmaps())));
	probe_texture_streaming_info("after StreamedTexture2D::load");

	auto make_quad = [&](const Ref<Texture2D> &p_tex, float p_x, float p_y, float p_size, Color p_col) {
		Ref<QuadMesh> qm;
		qm.instantiate();
		qm->set_size(Vector2(p_size, p_size));
		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		mat->set_albedo(p_col);
		if (p_tex.is_valid()) {
			mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, p_tex);
		}
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_mesh(qm);
		mi->set_material_override(mat);
		camera->add_child(mi);
		mi->set_position(Vector3(p_x, p_y, -1.5f));
		return mi;
	};
	tex_quad = make_quad(streamed, -0.6f, 0.0f, 1.2f, Color(1, 1, 1));
	tex_quad2 = make_quad(image_tex, 0.6f, 0.0f, 1.2f, Color(1, 1, 1));
	// controls: solid red material without texture, and an uncompressed RGBA8 green texture (small, bottom corners)
	Ref<Image> green = Image::create_empty(64, 64, false, Image::FORMAT_RGBA8);
	green->fill(Color(0, 1, 0, 1));
	make_quad(Ref<Texture2D>(), -1.0f, -0.55f, 0.15f, Color(1, 0, 0));
	make_quad(ImageTexture::create_from_image(green), 1.0f, -0.55f, 0.15f, Color(1, 1, 1));
	nav_async_start = Clock::now(); // phase timer for the near/far observation
}

// ---------------------------------------------------------------- (e)

void GDExtProbe::build_terrain(int p_size, double p_step, std::vector<float> &r_verts, std::vector<int32_t> &r_indices, bool p_flip) {
	int n = int(p_size / p_step);
	int vn = n + 1;
	r_verts.resize(size_t(vn) * vn * 3);
	r_indices.resize(size_t(n) * n * 6);
	double half = p_size * 0.5;
	for (int z = 0; z < vn; z++) {
		for (int x = 0; x < vn; x++) {
			double px = x * p_step - half, pz = z * p_step - half;
			size_t i = (size_t(z) * vn + x) * 3;
			r_verts[i] = float(px);
			r_verts[i + 1] = float(terrain_h(px, pz));
			r_verts[i + 2] = float(pz);
		}
	}
	size_t k = 0;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			int a = z * vn + x, b = a + 1, c = a + vn + 1, d = a + vn;
			if (!p_flip) {
				r_indices[k++] = a; r_indices[k++] = b; r_indices[k++] = c;
				r_indices[k++] = a; r_indices[k++] = c; r_indices[k++] = d;
			} else {
				r_indices[k++] = a; r_indices[k++] = c; r_indices[k++] = b;
				r_indices[k++] = a; r_indices[k++] = d; r_indices[k++] = c;
			}
		}
	}
}

static Ref<NavigationMeshSourceGeometryData3D> to_source(const std::vector<float> &v, const std::vector<int32_t> &idx) {
	PackedFloat32Array pv;
	pv.resize(int64_t(v.size()));
	std::memcpy(pv.ptrw(), v.data(), v.size() * sizeof(float));
	PackedInt32Array pi;
	pi.resize(int64_t(idx.size()));
	std::memcpy(pi.ptrw(), idx.data(), idx.size() * sizeof(int32_t));
	Ref<NavigationMeshSourceGeometryData3D> src;
	src.instantiate();
	// NOT set_vertices()+set_indices(): set_indices() fails with ERR_FAIL_COND(vertices.size() < p_indices.size())
	// (it compares the float count with the index count), which rejects any shared-vertex grid with more than
	// 3 indices per vertex. append_arrays() has no such check.
	src->append_arrays(pv, pi);
	return src;
}

void GDExtProbe::probe_nav_winding() {
	NavigationServer3D *ns = NavigationServer3D::get_singleton();
	for (int flip = 0; flip < 2; flip++) {
		std::vector<float> v;
		std::vector<int32_t> idx;
		build_terrain(40, 1.0, v, idx, flip != 0);
		Ref<NavigationMesh> nm;
		nm.instantiate();
		nm->set_cell_size(float(nav_cell));
		ns->bake_from_source_geometry_data(nm, to_source(v, idx), Callable());
		log(fmt("e winding test 40 m: triangle order %s (a,b,c with b=+x, c=+x+z) -> %lld polygons",
				flip ? "a,c,b" : "a,b,c", (long long)nm->get_polygon_count()));
		if (flip == 0 && nm->get_polygon_count() == 0) {
			nav_winding_flip = true;
		}
	}
}

void GDExtProbe::probe_nav_sync() {
	NavigationServer3D *ns = NavigationServer3D::get_singleton();
	for (size_t s = 0; s < nav_sizes.size(); s++) {
		int size = nav_sizes[s];
		std::vector<float> v;
		std::vector<int32_t> idx;
		Clock::time_point t0 = Clock::now();
		build_terrain(size, 1.0, v, idx, nav_winding_flip);
		Ref<NavigationMeshSourceGeometryData3D> src = to_source(v, idx);
		double build_ms = ms_since(t0);
		for (int part = 0; part < ((nav_monotone_too && s + 1 == nav_sizes.size()) ? 2 : 1); part++) {
			Ref<NavigationMesh> nm;
			nm.instantiate();
			nm->set_cell_size(float(nav_cell));
			nm->set_cell_height(0.25f);
			nm->set_agent_radius(0.5f);
			nm->set_agent_height(1.75f);
			nm->set_agent_max_climb(0.5f);
			if (part == 1) {
				nm->set_sample_partition_type(NavigationMesh::SAMPLE_PARTITION_MONOTONE);
			}
			int64_t mem0 = OS::get_singleton()->get_static_memory_usage();
			t0 = Clock::now();
			ns->bake_from_source_geometry_data(nm, src, Callable());
			double ms = ms_since(t0);
			log(fmt("e sync bake %d m x %d m, 1 m triangles (%zu tris), cell %.2f, partition %s: %.1f ms (geometry build %.1f ms); polygons %lld, vertices %lld, walkable area %.0f m2; static mem after-before %lld MB, peak %lld MB",
					size, size, idx.size() / 3, nav_cell, part ? "monotone" : "watershed", ms, build_ms, (long long)nm->get_polygon_count(),
					(long long)nm->get_vertices().size(), nav_area(nm), (long long)((OS::get_singleton()->get_static_memory_usage() - mem0) >> 20),
					(long long)(OS::get_singleton()->get_static_memory_peak_usage() >> 20)));
		}
	}
}

void GDExtProbe::probe_nav_async_start() {
	int size = nav_sizes.empty() ? 250 : nav_sizes.back();
	std::vector<float> v;
	std::vector<int32_t> idx;
	build_terrain(size, 1.0, v, idx, nav_winding_flip);
	nav_async_src = to_source(v, idx);
	nav_async_mesh.instantiate();
	nav_async_mesh->set_cell_size(float(nav_cell));
	nav_async_mesh->set_cell_height(0.25f);
	nav_async_mesh->set_agent_radius(0.5f);
	nav_async_mesh->set_agent_height(1.75f);
	nav_async_mesh->set_agent_max_climb(0.5f);
	nav_async_done = false;
	nav_async_frames = 0;
	nav_async_max_frame_ms = 0;
	frame_ms.clear();
	nav_async_start = Clock::now();
	NavigationServer3D::get_singleton()->bake_from_source_geometry_data_async(nav_async_mesh, nav_async_src, callable_mp(this, &GDExtProbe::on_nav_baked));
	log(fmt("e async bake %d m started; call returned after %.2f ms; is_baking=%d", size, ms_since(nav_async_start),
			int(NavigationServer3D::get_singleton()->is_baking_navigation_mesh(nav_async_mesh))));
}

double GDExtProbe::nav_area(const Ref<NavigationMesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return 0.0;
	}
	PackedVector3Array v = p_mesh->get_vertices();
	double area = 0.0;
	for (int i = 0; i < p_mesh->get_polygon_count(); i++) {
		PackedInt32Array poly = p_mesh->get_polygon(i);
		double a = 0.0;
		for (int k = 0; k < poly.size(); k++) {
			Vector3 p = v[poly[k]], q = v[poly[(k + 1) % poly.size()]];
			a += double(p.x) * q.z - double(q.x) * p.z;
		}
		area += std::fabs(a) * 0.5;
	}
	return area;
}

void GDExtProbe::probe_nav_chunks_start() {
	// Chunked bake as Godot recommends for large worlds: one NavigationMesh per chunk with
	// filter_baking_aabb + border_size, geometry cut to the chunk plus a margin, all baked async in parallel.
	const int size = nav_sizes.empty() ? 250 : nav_sizes.back();
	const int chunk = nav_chunk_size > 0 ? nav_chunk_size : std::max(50, size / 4);
	const double margin = 4.0;
	const double half = size * 0.5;
	nav_chunks.clear();
	nav_chunk_srcs.clear();
	int n_chunks = (size / chunk) * (size / chunk);
	nav_chunks_left = n_chunks;
	nav_chunk_started = true;
	nav_chunk_start = Clock::now();
	for (int cz = 0; cz < size / chunk; cz++) {
		for (int cx = 0; cx < size / chunk; cx++) {
			double x0 = -half + cx * chunk, z0 = -half + cz * chunk;
			double gx0 = std::max(-half, x0 - margin), gz0 = std::max(-half, z0 - margin);
			double gx1 = std::min(half, x0 + chunk + margin), gz1 = std::min(half, z0 + chunk + margin);
			int nx = int(gx1 - gx0), nz = int(gz1 - gz0);
			std::vector<float> v;
			std::vector<int32_t> idx;
			v.reserve(size_t(nx + 1) * (nz + 1) * 3);
			for (int z = 0; z <= nz; z++) {
				for (int x = 0; x <= nx; x++) {
					double px = gx0 + x, pz = gz0 + z;
					v.push_back(float(px));
					v.push_back(float(terrain_h(px, pz)));
					v.push_back(float(pz));
				}
			}
			for (int z = 0; z < nz; z++) {
				for (int x = 0; x < nx; x++) {
					int a = z * (nx + 1) + x, b = a + 1, c = a + nx + 2, d = a + nx + 1;
					if (nav_winding_flip) {
						idx.insert(idx.end(), { a, c, b, a, d, c });
					} else {
						idx.insert(idx.end(), { a, b, c, a, c, d });
					}
				}
			}
			Ref<NavigationMeshSourceGeometryData3D> src = to_source(v, idx);
			Ref<NavigationMesh> nm;
			nm.instantiate();
			nm->set_cell_size(float(nav_cell));
			nm->set_cell_height(0.25f);
			nm->set_agent_radius(0.5f);
			nm->set_agent_height(1.75f);
			nm->set_agent_max_climb(0.5f);
			nm->set_border_size(float(nav_border));
			// border_size is an inward margin of the baking AABB: grow the AABB by it so the chunk itself stays fully covered.
			const float bs = float(nav_border);
			nm->set_filter_baking_aabb(AABB(Vector3(float(x0) - bs, -100, float(z0) - bs), Vector3(float(chunk) + 2 * bs, 200, float(chunk) + 2 * bs)));
			nav_chunks.push_back(nm);
			nav_chunk_srcs.push_back(src);
		}
	}
	double prep = ms_since(nav_chunk_start);
	nav_chunk_start = Clock::now();
	for (size_t i = 0; i < nav_chunks.size(); i++) {
		NavigationServer3D::get_singleton()->bake_from_source_geometry_data_async(nav_chunks[i], nav_chunk_srcs[i], callable_mp(this, &GDExtProbe::on_nav_chunk_baked));
	}
	log(fmt("e chunked async: %d chunks of %d m (+%.0f m margin, border_size %.2f) over %d m x %d m queued; geometry prep %.1f ms", n_chunks, chunk, margin, nav_border, size, size, prep));
}

void GDExtProbe::on_nav_chunk_baked() {
	nav_chunks_left--;
}

void GDExtProbe::on_nav_baked() {
	nav_async_cb_ms = ms_since(nav_async_start);
	nav_async_done = true;
	nav_cb_on_main = std::this_thread::get_id() == main_thread_id;
	log(fmt("e async callback fired after %.1f ms (on main thread? see frames)", nav_async_cb_ms));
}

// ---------------------------------------------------------------- (f)

void GDExtProbe::probe_enet() {
	const int port = 47123;
	Ref<ENetConnection> server, client;
	server.instantiate();
	client.instantiate();
	Error e1 = server->create_host_bound("127.0.0.1", port, 8, 2, 0, 0);
	Error e2 = client->create_host(1, 2, 0, 0);
	Ref<ENetPacketPeer> cpeer = client->connect_to_host("127.0.0.1", port, 2, 0);
	Ref<ENetPacketPeer> speer;
	Clock::time_point t0 = Clock::now();
	bool sc = false, cc = false;
	while (ms_since(t0) < 3000 && !(sc && cc)) {
		Array ev = server->service(1);
		if (int(ev[0]) == ENetConnection::EVENT_CONNECT) {
			sc = true;
			speer = Object::cast_to<ENetPacketPeer>(ev[1].operator Object *());
		}
		Array ev2 = client->service(1);
		if (int(ev2[0]) == ENetConnection::EVENT_CONNECT) {
			cc = true;
		}
	}
	log(fmt("f ENet hosts: server err %d, client err %d, connected server=%d client=%d in %.1f ms", int(e1), int(e2), int(sc), int(cc), ms_since(t0)));
	if (!(sc && cc)) {
		return;
	}

	auto now_us = []() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); };
	auto run = [&](const char *p_label, int p_flags, int p_channel, bool p_burst, int p_size) {
		const int count = 1000;
		std::vector<double> lat;
		std::vector<bool> seen(count, false);
		int received = 0, out_of_order = 0, last_seq = -1;
		auto drain = [&]() {
			for (;;) {
				Array ev = server->service(0);
				int type = int(ev[0]);
				if (type == ENetConnection::EVENT_NONE || type == ENetConnection::EVENT_ERROR) {
					break;
				}
				if (type == ENetConnection::EVENT_RECEIVE) {
					ENetPacketPeer *pp = Object::cast_to<ENetPacketPeer>(ev[1].operator Object *());
					while (pp && pp->get_available_packet_count() > 0) {
						PackedByteArray pkt = pp->get_packet();
						if (pkt.size() >= 12) {
							uint32_t seq;
							uint64_t ts;
							std::memcpy(&seq, pkt.ptr(), 4);
							std::memcpy(&ts, pkt.ptr() + 4, 8);
							if (seq < (uint32_t)count && !seen[seq]) {
								seen[seq] = true;
								received++;
								lat.push_back((now_us() - ts) / 1000.0);
								if ((int)seq < last_seq) {
									out_of_order++;
								}
								last_seq = (int)seq;
							}
						}
					}
				}
			}
		};
		Clock::time_point ts0 = Clock::now();
		for (int i = 0; i < count; i++) {
			PackedByteArray pkt;
			pkt.resize(p_size);
			std::memset(pkt.ptrw(), 0, p_size);
			uint32_t seq = i;
			uint64_t ts = now_us();
			std::memcpy(pkt.ptrw(), &seq, 4);
			std::memcpy(pkt.ptrw() + 4, &ts, 8);
			cpeer->send(p_channel, pkt, p_flags);
			if (!p_burst) {
				client->service(0);
				drain();
			}
		}
		client->flush();
		double send_ms = ms_since(ts0);
		Clock::time_point tw = Clock::now();
		while (received < count && ms_since(tw) < 1000.0) {
			client->service(0);
			drain();
		}
		Stats s = stats_of(lat);
		log(fmt("f ENet %s: %d x %d B, %s; sent in %.2f ms, received %d/%d (%.1f%% loss), out-of-order %d, latency ms mean %.3f p50 %.3f p99 %.3f max %.3f; total %.1f ms",
				p_label, count, p_size, p_burst ? "burst then flush" : "send+service per packet", send_ms, received, count,
				100.0 * (count - received) / count, out_of_order, s.mean, s.p50, s.p99, s.max, ms_since(ts0)));
	};
	run("unreliable(sequenced) ch0", 0, 0, false, 100);
	run("unreliable(sequenced) ch0", 0, 0, true, 100);
	run("unsequenced ch1", ENetPacketPeer::FLAG_UNSEQUENCED, 1, true, 100);
	run("unreliable ch0 1200B", 0, 0, true, 1200);
	run("reliable ch1", ENetPacketPeer::FLAG_RELIABLE, 1, true, 100);
	log(fmt("f ENet client stats: RTT %.1f ms, packet loss %.0f/65536, throttle %.0f/32; host sent packets %.0f",
			cpeer->get_statistic(ENetPacketPeer::PEER_ROUND_TRIP_TIME), cpeer->get_statistic(ENetPacketPeer::PEER_PACKET_LOSS),
			cpeer->get_statistic(ENetPacketPeer::PEER_PACKET_THROTTLE), client->pop_statistic(ENetConnection::HOST_TOTAL_SENT_PACKETS)));
	cpeer->peer_disconnect_now(0);
	client->destroy();
	server->destroy();
}

} // namespace godot
