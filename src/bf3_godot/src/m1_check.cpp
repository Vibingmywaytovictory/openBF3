#include "m1_check.h"

#include "godot_util.h"
#include "input_mapper.h"
#include "level_session.h"
#include "level_upload.h"
#include "loading_screen.h"

#include "bf3/host/game_server.h"
#include "bf3/net/loopback_ping.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi after windows.h
#include <psapi.h>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace net = bf3::net;
namespace sim = bf3::sim;

namespace {

std::string fmt(const char *p_fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, p_fmt);
	vsnprintf(buf, sizeof(buf), p_fmt, args);
	va_end(args);
	return buf;
}

double mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

float horizontal(const sim::Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[2] * p_v[2]);
}

float distance3(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// sim.motor's heading_distance: the distance travelled along the course heading.
float heading_distance(const sim::Vec3 &p_from, const sim::Vec3 &p_to, uint16_t p_yaw) {
	const float a = sim::yaw_radians(p_yaw);
	return (p_to[0] - p_from[0]) * std::sin(a) + (p_to[2] - p_from[2]) * std::cos(a);
}

double seconds_between(uint64_t p_from_ns, uint64_t p_to_ns) {
	return p_to_ns > p_from_ns ? double(p_to_ns - p_from_ns) / 1e9 : 0.0;
}

uint64_t peak_working_set_bytes() {
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? uint64_t(pmc.PeakWorkingSetSize) : 0;
}

float percentile(std::vector<float> p_values, double p_q) {
	if (p_values.empty()) {
		return 0.0f;
	}
	std::sort(p_values.begin(), p_values.end());
	return p_values[std::min(p_values.size() - 1, size_t(double(p_values.size()) * p_q))];
}

const char *kSpeedNames[4] = { "walk", "sprint", "crouch", "prone" };
constexpr float kSpeedExpected[4] = { 4.0f, 6.5f, 2.0f, 0.9f };
constexpr sim::Pose kSpeedPose[4] = { sim::Pose::Stand, sim::Pose::Stand, sim::Pose::Crouch, sim::Pose::Prone };

struct ChainStep {
	InputAction press;
	sim::Pose to;
	sim::Pose from;
	uint32_t ticks;
};
// S->C, C->P, P->S, S->P, P->C, C->S: every transition of docs §6.5 once, by the mapper's toggles.
constexpr ChainStep kChain[6] = {
	{ InputAction::Crouch, sim::Pose::Crouch, sim::Pose::Stand, 18 },
	{ InputAction::Prone, sim::Pose::Prone, sim::Pose::Crouch, 30 },
	{ InputAction::Prone, sim::Pose::Stand, sim::Pose::Prone, 42 },
	{ InputAction::Prone, sim::Pose::Prone, sim::Pose::Stand, 42 },
	{ InputAction::Crouch, sim::Pose::Crouch, sim::Pose::Prone, 63 },
	{ InputAction::Crouch, sim::Pose::Stand, sim::Pose::Crouch, 18 },
};

struct DropCase {
	float h;
	float min_damage, max_damage;
	bool dies;
};
constexpr DropCase kDrops[3] = { { 9.8f, 0, 0, false }, { 10.1f, 20, 21, false }, { 46.0f, 100, 100, true } };

struct Profile {
	const char *netsim;
	uint64_t seed;
	bool gate;
};
// Orchestrator decision on docs §16 question 2: 150 ms RTT / 2 % is the gate; 150 ms one way a reported stress row.
constexpr Profile kProfiles[2] = { { "150/2", 11, true }, { "lat=150-165,loss=2", 12, false } };

constexpr double kE2IdleS = 30.0;
constexpr int kE2Cycles = 4;
constexpr double kE2WalkS = 2.0;
constexpr double kE2RestS = 13.0;

bool same_bytes(const PackedByteArray &p_a, const PackedByteArray &p_b) {
	return p_a.size() == p_b.size() && (p_a.size() == 0 || std::memcmp(p_a.ptr(), p_b.ptr(), size_t(p_a.size())) == 0);
}

} // namespace

int print_check_table(const char *p_title, const std::vector<CheckRow> &p_rows) {
	size_t checks = 0, passed = 0;
	bool failed = false;
	UtilityFunctions::print("");
	UtilityFunctions::print(p_title);
	for (const CheckRow &r : p_rows) {
		static const char *names[] = { "PASS", "FAIL", "INFO" };
		UtilityFunctions::print(to_godot(fmt("  %-5s %-24s ", names[std::clamp(r.result, 0, 2)], r.check.c_str()) + r.detail));
		if (r.result != 2) {
			++checks;
			passed += r.result == 0 ? 1 : 0;
			failed = failed || r.result != 0;
		}
	}
	UtilityFunctions::print(to_godot(fmt("%s: %s (%zu/%zu)", p_title, failed ? "FAIL" : "PASS", passed, checks)));
	return failed ? 1 : 0;
}

CheckRow check_surface_format() {
	RenderingServer *rs = RenderingServer::get_singleton();
	struct Case {
		const char *name;
		bool normals, tangents, uv0, uv1;
		size_t vertices;
	};
	const Case cases[] = {
		{ "normals+tangents+uv0+uv1", true, true, true, true, 6 },
		{ "normals+uv0 (no tangents)", true, false, true, false, 6 },
		{ "positions only", false, false, false, false, 6 },
		{ "70,000 vertices (uint32 indices)", true, true, true, false, 70000 },
	};
	size_t equal = 0, round_trip = 0;
	std::string detail;
	for (const Case &c : cases) {
		bf3::render::PreparedSurface s;
		for (size_t i = 0; i < c.vertices; ++i) {
			const float a = float(i) * 0.7f;
			s.positions.insert(s.positions.end(), { 100.0f + std::sin(a) * 3.0f, 60.0f + float(i % 7) * 0.25f, -500.0f + std::cos(a * 1.3f) * 2.0f });
			if (c.normals) {
				// Mixed signs, a few exact axes and negative Z (the octahedron's lower half).
				float n[3] = { std::sin(a * 2.1f), std::cos(a * 0.9f), std::sin(a * 0.37f) - 0.4f };
				if (i % 5 == 0) {
					n[0] = 0.0f;
					n[1] = (i % 10 == 0) ? 1.0f : -1.0f;
					n[2] = 0.0f;
				}
				const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				s.normals.insert(s.normals.end(), { n[0] / l, n[1] / l, n[2] / l });
				if (c.tangents) {
					float t[3] = { std::cos(a), 0.3f, -std::sin(a) };
					const float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
					s.tangents.insert(s.tangents.end(), { t[0] / tl, t[1] / tl, t[2] / tl, (i % 3 == 0) ? -1.0f : 1.0f });
				}
			}
			if (c.uv0) {
				s.uv0.insert(s.uv0.end(), { float(i) * 0.125f, 1.0f - float(i % 13) * 0.03f });
			}
			if (c.uv1) {
				s.uv1.insert(s.uv1.end(), { float(i % 4) * 0.5f, float(i % 9) * 0.1f });
			}
		}
		for (size_t i = 0; i + 2 < c.vertices; i += 3) {
			s.indices.insert(s.indices.end(), { int32_t(i), int32_t(i + 2), int32_t(i + 1) });
		}
		const Dictionary mine = encode_rs_surface(s, RID());
		// Godot's own conversion of the same arrays.
		Array arrays;
		arrays.resize(RenderingServer::ARRAY_MAX);
		PackedVector3Array pos;
		pos.resize(int64_t(c.vertices));
		for (size_t i = 0; i < c.vertices; ++i) {
			pos.set(int64_t(i), Vector3(s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]));
		}
		arrays[RenderingServer::ARRAY_VERTEX] = pos;
		if (c.normals) {
			PackedVector3Array nrm;
			nrm.resize(int64_t(c.vertices));
			for (size_t i = 0; i < c.vertices; ++i) {
				nrm.set(int64_t(i), Vector3(s.normals[i * 3], s.normals[i * 3 + 1], s.normals[i * 3 + 2]));
			}
			arrays[RenderingServer::ARRAY_NORMAL] = nrm;
		}
		if (c.tangents) {
			PackedFloat32Array tan;
			tan.resize(int64_t(s.tangents.size()));
			std::memcpy(tan.ptrw(), s.tangents.data(), s.tangents.size() * sizeof(float));
			arrays[RenderingServer::ARRAY_TANGENT] = tan;
		}
		if (c.uv0) {
			PackedVector2Array uv;
			uv.resize(int64_t(c.vertices));
			for (size_t i = 0; i < c.vertices; ++i) {
				uv.set(int64_t(i), Vector2(s.uv0[i * 2], s.uv0[i * 2 + 1]));
			}
			arrays[RenderingServer::ARRAY_TEX_UV] = uv;
		}
		if (c.uv1) {
			PackedVector2Array uv;
			uv.resize(int64_t(c.vertices));
			for (size_t i = 0; i < c.vertices; ++i) {
				uv.set(int64_t(i), Vector2(s.uv1[i * 2], s.uv1[i * 2 + 1]));
			}
			arrays[RenderingServer::ARRAY_TEX_UV2] = uv;
		}
		PackedInt32Array idx;
		idx.resize(int64_t(s.indices.size()));
		std::memcpy(idx.ptrw(), s.indices.data(), s.indices.size() * sizeof(int32_t));
		arrays[RenderingServer::ARRAY_INDEX] = idx;
		const RID reference = rs->mesh_create();
		rs->mesh_add_surface_from_arrays(reference, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
		const Dictionary godot_surface = rs->mesh_get_surface(reference, 0);
		const AABB aabb_mine = mine["aabb"];
		const AABB aabb_godot = godot_surface["aabb"];
		const auto bytes = [](const Dictionary &p_d, const char *p_key) {
			return p_d.has(p_key) ? PackedByteArray(p_d[p_key]) : PackedByteArray();
		};
		const bool same = int64_t(mine["format"]) == int64_t(godot_surface["format"]) && int64_t(mine["vertex_count"]) == int64_t(godot_surface["vertex_count"]) &&
				int64_t(mine["index_count"]) == int64_t(godot_surface["index_count"]) && same_bytes(bytes(mine, "vertex_data"), bytes(godot_surface, "vertex_data")) &&
				same_bytes(bytes(mine, "attribute_data"), bytes(godot_surface, "attribute_data")) &&
				same_bytes(bytes(mine, "index_data"), bytes(godot_surface, "index_data")) && aabb_mine.is_equal_approx(aabb_godot);
		equal += same ? 1 : 0;
		// The creation path the upload uses (mesh_create_from_surfaces) gives the same surface back.
		TypedArray<Dictionary> list;
		list.push_back(mine);
		const RID created = rs->mesh_create_from_surfaces(list, 0);
		const Dictionary back = rs->mesh_get_surface(created, 0);
		const bool rt = int64_t(back["format"]) == int64_t(mine["format"]) && same_bytes(bytes(back, "vertex_data"), bytes(mine, "vertex_data")) &&
				same_bytes(bytes(back, "attribute_data"), bytes(mine, "attribute_data")) && same_bytes(bytes(back, "index_data"), bytes(mine, "index_data"));
		round_trip += rt ? 1 : 0;
		if (!same || !rt) {
			detail += fmt(" [%s: format %lld vs %lld, vertex %lld vs %lld B%s]", c.name, (long long)int64_t(mine["format"]), (long long)int64_t(godot_surface["format"]),
					(long long)bytes(mine, "vertex_data").size(), (long long)bytes(godot_surface, "vertex_data").size(), rt ? "" : ", round trip differs");
		}
		rs->free_rid(created);
		rs->free_rid(reference);
	}
	CheckRow row;
	row.check = "upload.surface_format";
	row.result = (equal == std::size(cases) && round_trip == std::size(cases)) ? 0 : 1;
	row.detail = fmt("%zu / %zu synthetic surfaces encoded byte for byte as mesh_add_surface_from_arrays does (vertex, attribute and index streams, format, "
					 "AABB); %zu / %zu read back unchanged after mesh_create_from_surfaces",
						 equal, std::size(cases), round_trip, std::size(cases)) +
			detail;
	return row;
}

CheckRow check_input_keys() {
	struct Case {
		InputAction action;
		Key key;
	};
	const Case cases[] = {
		{ InputAction::Jump, KEY_SPACE },
		{ InputAction::Crouch, KEY_CTRL },
		{ InputAction::Prone, KEY_Z },
		{ InputAction::FreeCamera, KEY_F6 },
	};
	struct Mods {
		const char *name;
		bool shift, ctrl;
	};
	const Mods mods[] = { { "none", false, false }, { "shift", true, false }, { "ctrl", false, true }, { "shift+ctrl", true, true } };
	enum class Kind { Press, Echo, Release };
	const auto fired = [](const InputMapper &p_m, InputAction p_action) {
		switch (p_action) {
			case InputAction::Jump:
				return p_m.sample().jump_presses == 1;
			case InputAction::Crouch:
				return p_m.requested_pose() == sim::Pose::Crouch;
			case InputAction::Prone:
				return p_m.requested_pose() == sim::Pose::Prone;
			case InputAction::FreeCamera:
				return p_m.free_camera();
			default:
				return false;
		}
	};
	size_t ok = 0, total = 0;
	std::string detail;
	for (const Case &c : cases) {
		for (const Mods &m : mods) {
			for (const Kind kind : { Kind::Press, Kind::Echo, Kind::Release }) {
				InputMapper mapper;
				mapper.install_actions();
				Ref<InputEventKey> ev;
				ev.instantiate();
				ev->set_keycode(c.key);
				ev->set_physical_keycode(c.key);
				ev->set_pressed(kind != Kind::Release);
				ev->set_echo(kind == Kind::Echo);
				ev->set_shift_pressed(m.shift);
				ev->set_ctrl_pressed(m.ctrl);
				mapper.handle_event(ev);
				const bool want = kind == Kind::Press;
				const bool got = fired(mapper, c.action);
				++total;
				if (got == want) {
					++ok;
				} else {
					detail += fmt(" [%s with %s, %s: %s]", input_action_name(c.action), m.name, kind == Kind::Press ? "press" : (kind == Kind::Echo ? "echo" : "release"),
							got ? "fired" : "did not fire");
				}
			}
		}
	}
	CheckRow row;
	row.check = "input.keys";
	row.result = ok == total ? 0 : 1;
	row.detail = fmt("%zu / %zu key events through handle_event as expected (Space / Ctrl / Z / F6 pressed with none / Shift / Ctrl / Shift+Ctrl held fire "
					 "jump / crouch / prone / free camera; echoes and releases do nothing)",
						 ok, total) +
			detail;
	return row;
}

// ---- ShotRunner -------------------------------------------------------------------------------------------------

bool ShotRunner::parse(const std::string &p_shots, const std::string &p_spawns, std::vector<FreeShot> &r_free, std::vector<SpawnShot> &r_spawn,
		std::string &r_error) {
	{
		std::stringstream ss(p_shots);
		std::string item;
		while (std::getline(ss, item, ';')) {
			if (item.empty()) {
				continue;
			}
			const size_t at = item.rfind('@');
			FreeShot shot;
			bool ok = at != std::string::npos && at > 0;
			if (ok) {
				std::stringstream vs(item.substr(at + 1));
				std::string v;
				size_t n = 0;
				while (ok && std::getline(vs, v, ',')) {
					char *end = nullptr;
					const float f = std::strtof(v.c_str(), &end);
					ok = n < 5 && end != v.c_str() && std::isfinite(f);
					if (ok) {
						shot.view[n++] = f;
					}
				}
				ok = ok && n == 5;
			}
			if (!ok) {
				r_error = "--shots entry is not <png>@x,y,z,yaw,pitch: " + item;
				return false;
			}
			shot.path = item.substr(0, at);
			r_free.push_back(shot);
		}
	}
	{
		std::stringstream ss(p_spawns);
		std::string item;
		while (std::getline(ss, item, ',')) {
			if (item.empty()) {
				continue;
			}
			SpawnShot shot;
			const size_t colon = item.find(':');
			const std::string index = item.substr(0, colon);
			char *end = nullptr;
			const unsigned long v = std::strtoul(index.c_str(), &end, 10);
			if (end == index.c_str() || *end != 0 || v > 100000) {
				r_error = "--shot-spawns entry is not <index>[:stand|crouch|prone|walk]: " + item;
				return false;
			}
			shot.index = uint32_t(v);
			if (colon != std::string::npos) {
				shot.mode = item.substr(colon + 1);
				if (shot.mode != "stand" && shot.mode != "crouch" && shot.mode != "prone" && shot.mode != "walk") {
					r_error = "--shot-spawns mode is not stand, crouch, prone or walk: " + item;
					return false;
				}
			}
			r_spawn.push_back(shot);
		}
	}
	return true;
}

ShotRunner::ShotRunner(Bf3LevelSession &p_session, std::vector<FreeShot> p_free, std::vector<SpawnShot> p_spawn, std::string p_dir) :
		session_(p_session), free_(std::move(p_free)), spawn_(std::move(p_spawn)), dir_(std::move(p_dir)) {}

bool ShotRunner::frame(uint64_t p_now_ns) {
	const size_t total = spawn_.size() + free_.size();
	if (next_ >= total) {
		return true;
	}
	InputMapper &mapper = session_.mapper();
	++frames_;
	if (next_ < spawn_.size()) {
		const SpawnShot &shot = spawn_[next_];
		const std::shared_ptr<const host::SharedLevel> level = session_.shared_level();
		switch (state_) {
			case 0: {
				if (level == nullptr || shot.index >= level->spawns.size() || session_.client() == nullptr) {
					lines_.push_back(fmt("spawn shot %u: no such spawn point", shot.index));
					++failed_;
					++next_;
					return false;
				}
				mapper.set_scripted(true);
				mapper.release_all();
				mapper.set_free_camera(false);
				net::DebugRequest q;
				q.kind = net::DebugKind::SpawnAt;
				q.id = ++request_id_;
				q.index = shot.index;
				session_.client()->send_debug(q);
				life_ = mapper.adopted_life();
				state_ = 1;
				state_ns_ = p_now_ns;
				return false;
			}
			case 1:
				if (mapper.adopted_life() > life_) {
					// SpawnAt respawns a live soldier: the commands already sent for ticks after the spawn (the previous
					// shot's pose) reach the new life (docs §8.2 assumption 2), so the adopted target pose can be that
					// pose. Each shot starts from Stand; the toggles below then act from Stand.
					mapper.set_requested_pose(sim::Pose::Stand);
					if (shot.mode == "crouch") {
						mapper.script_press(InputAction::Crouch);
					} else if (shot.mode == "prone") {
						mapper.script_press(InputAction::Prone);
					} else if (shot.mode == "walk") {
						mapper.script_hold(InputAction::Forward, true);
					}
					state_ = 2;
					state_ns_ = p_now_ns;
				} else if (seconds_between(state_ns_, p_now_ns) > 10.0) {
					lines_.push_back(fmt("spawn shot %u: no new life within 10 s", shot.index));
					++failed_;
					++next_;
					state_ = 0;
				}
				return false;
			case 2: {
				// Long enough for the slowest transition from the previous shot's pose (P->C 63 ticks, 1.05 s) plus settling.
				const double wait = 1.6;
				if (seconds_between(state_ns_, p_now_ns) >= wait) {
					state_ = 3;
					frames_ = 0;
				}
				return false;
			}
			default:
				if (frames_ < 30) {
					return false; // drawn a few times before the capture (docs §11.8)
				}
				{
					const std::string path = (dir_.empty() ? std::string(".") : dir_) + "/spawn_" + std::to_string(shot.index) + "_" + shot.mode + ".png";
					const host::PresentationFrame &f = session_.frame();
					const Vector3 eye = session_.cameras().last_eye();
					if (session_.save_screenshot(path)) {
						++saved_;
						lines_.push_back(fmt("%s: point %u %s, feet (%.2f, %.3f, %.2f), eye (%.2f, %.3f, %.2f) = feet + %.3f m, yaw %.1f deg, %.2f m/s", path.c_str(),
								shot.index, shot.mode.c_str(), f.own_curr.position[0], f.own_curr.position[1], f.own_curr.position[2], eye.x, eye.y, eye.z,
								eye.y - f.own_curr.position[1], double(mapper.view().yaw) * 180.0 / 3.14159265358979323846, double(horizontal(f.own_curr.velocity))));
					} else {
						++failed_;
						lines_.push_back("screenshot failed: " + path);
					}
					UtilityFunctions::print(to_godot("[m1] shot " + lines_.back()));
				}
				mapper.release_all();
				++next_;
				state_ = 0;
				return false;
		}
	}
	const FreeShot &shot = free_[next_ - spawn_.size()];
	const float deg = 3.14159265358979323846f / 180.0f;
	if (state_ == 0) {
		mapper.set_scripted(true);
		mapper.release_all();
		mapper.set_free_camera(true);
		session_.cameras().set_free(true);
		mapper.script_view(shot.view[3] * deg, shot.view[4] * deg);
		session_.cameras().place_free(Vector3(shot.view[0], shot.view[1], shot.view[2]), mapper.free_view().yaw, mapper.free_view().pitch);
		frames_ = 0;
		state_ = 1;
		state_ns_ = p_now_ns; // L1 look: the settle time runs from here
		return false;
	}
	if (frames_ < 30 || seconds_between(state_ns_, p_now_ns) < settle_s_) {
		last_ns_ = p_now_ns;
		return false;
	}
	// L1 look: --shot-measure=<n> frames timed at each view before its screenshot (frame interval and the viewport's
	// GPU render time; README "L1 look", ctest godot.l1_look).
	if (measure_frames_ > 0 && measured_ < measure_frames_) {
		RenderingServer *rs = RenderingServer::get_singleton();
		const RID viewport = session_.get_viewport()->get_viewport_rid();
		if (measured_ == 0) {
			rs->viewport_set_measure_render_time(viewport, true);
			view_cpu_ms_ = view_gpu_ms_ = 0.0;
			view_cpu_samples_.clear();
			view_gpu_samples_.clear();
			view_cpu_samples_.reserve(measure_frames_);
			view_gpu_samples_.reserve(measure_frames_);
		} else {
			const double interval_ms = double(p_now_ns - last_ns_) / 1e6;
			const double gpu_ms = rs->viewport_get_measured_render_time_gpu(viewport);
			view_cpu_ms_ += interval_ms;
			view_gpu_ms_ += gpu_ms;
			view_cpu_samples_.push_back(float(interval_ms));
			view_gpu_samples_.push_back(float(gpu_ms));
		}
		last_ns_ = p_now_ns;
		++measured_;
		return false;
	}
	if (measure_frames_ > 0) {
		const double n = double(measure_frames_ - 1);
		total_cpu_ms_ += view_cpu_ms_;
		total_gpu_ms_ += view_gpu_ms_;
		total_frames_ += measure_frames_ - 1;
		UtilityFunctions::print(to_godot(fmt("[look] frame time at %s: %.3f ms per frame (interval), GPU %.3f ms, over %u frames", shot.path.c_str(),
				view_cpu_ms_ / n, view_gpu_ms_ / n, measure_frames_ - 1)));
		if (!view_cpu_samples_.empty()) {
			const auto median_of = [](std::vector<float> &r_v) {
				const size_t mid = r_v.size() / 2;
				std::nth_element(r_v.begin(), r_v.begin() + std::ptrdiff_t(mid), r_v.end());
				return double(r_v[mid]);
			};
			const double cpu_median = median_of(view_cpu_samples_);
			const double gpu_median = median_of(view_gpu_samples_);
			median_cpu_sum_ms_ += cpu_median;
			median_gpu_sum_ms_ += gpu_median;
			++median_views_;
			UtilityFunctions::print(to_godot(fmt("[look] frame time median at %s: %.3f ms per frame (interval), GPU %.3f ms", shot.path.c_str(), cpu_median,
					gpu_median)));
		}
		measured_ = 0;
	}
	if (session_.save_screenshot(shot.path)) {
		++saved_;
		lines_.push_back(fmt("%s: free camera (%.1f, %.1f, %.1f) yaw %.1f pitch %.1f", shot.path.c_str(), shot.view[0], shot.view[1], shot.view[2], shot.view[3],
				shot.view[4]));
	} else {
		++failed_;
		lines_.push_back("screenshot failed: " + shot.path);
	}
	UtilityFunctions::print(to_godot("[m1] shot " + lines_.back()));
	UtilityFunctions::print(to_godot("[m1] shot " + session_.look_summary()));
	++next_;
	state_ = 0;
	if (next_ >= total) {
		mapper.set_free_camera(false);
		session_.cameras().set_free(false);
		if (measure_frames_ > 0 && total_frames_ > 0) {
			UtilityFunctions::print(to_godot(fmt("[look] frame time mean: %.3f ms per frame (interval), GPU %.3f ms, %llu frames at %zu views",
					total_cpu_ms_ / double(total_frames_), total_gpu_ms_ / double(total_frames_), (unsigned long long)total_frames_, free_.size())));
		}
		if (median_views_ > 0) {
			UtilityFunctions::print(to_godot(fmt("[look] frame time median: %.3f ms per frame (interval), GPU %.3f ms, mean of the per-view medians at %zu views",
					median_cpu_sum_ms_ / double(median_views_), median_gpu_sum_ms_ / double(median_views_), median_views_)));
		}
	}
	return next_ >= total;
}

// ---- M1Check ----------------------------------------------------------------------------------------------------

M1Check::M1Check(Bf3LevelSession &p_session, uint32_t p_courses) :
		session_(p_session), want_courses_(p_courses) {}

M1Check::~M1Check() = default;

void M1Check::add(const std::string &p_check, int p_result, const std::string &p_detail) {
	rows_.push_back({ p_check, p_result, p_detail });
	failed_ = failed_ || p_result == 1;
	UtilityFunctions::print(to_godot(fmt("[m1-check] %s %s: ", p_result == 0 ? "PASS" : (p_result == 1 ? "FAIL" : "INFO"), p_check.c_str()) + p_detail));
}

void M1Check::fail_phase(const std::string &p_check, const std::string &p_why) {
	add(p_check, 1, p_why);
}

bool M1Check::timed_out(uint64_t p_now_ns, double p_seconds) const {
	return seconds_between(phase_ns_, p_now_ns) > p_seconds;
}

void M1Check::prepare() {
	const auto t0 = std::chrono::steady_clock::now();
	const std::shared_ptr<const host::SharedLevel> level = session_.shared_level();
	session_.mapper().set_scripted(true);
	{
		const host::LoadProgress &p = session_.progress();
		const SessionTimings &t = session_.timings();
		const auto stage_s = [&](host::LoadStage p_stage) {
			const uint64_t a = p.start_us[size_t(p_stage)].load(), b = p.end_us[size_t(p_stage)].load();
			return a != 0 && b >= a ? double(b - a) / 1e6 : 0.0;
		};
		add("load", 2,
				fmt("%s %s without the scene (docs 11.1): process start -> spawned %.2f s, _ready -> spawned %.2f s (mount %.3f, db %.3f, shapes %.3f, connect %.3f, "
					"spawn %.3f s), static world %s, %zu tiles, %zu spawn points; ",
						session_.level_id().c_str(), session_.mode_id().c_str(), t.done_from_process_s, t.done_from_ready_s, stage_s(host::LoadStage::Mount),
						stage_s(host::LoadStage::Database), stage_s(host::LoadStage::Shapes), stage_s(host::LoadStage::Connect), stage_s(host::LoadStage::Spawn),
						cache_warmth_name(host::CacheWarmth(p.warmth.load())), level ? level->static_world->tile_count() : size_t(0),
						level ? level->spawns.size() : size_t(0)) +
						session_.cache_summary());
	}
	for (CheckRow (*row)() : { &check_surface_format, &check_input_keys }) {
		rows_.push_back(row());
		failed_ = failed_ || rows_.back().result == 1;
		UtilityFunctions::print(to_godot("[m1-check] " + rows_.back().check + ": " + rows_.back().detail));
	}
	if (level == nullptr) {
		fail_phase("prepare", "no shared level");
		phase_ = Phase::Finish;
		return;
	}
	// The check's own CollisionWorld and motor over the same shapes and params (docs §10.3: each test computes its
	// rows in-process, so no test reads another's output).
	bf3::core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
	if (!world) {
		fail_phase("prepare", "CollisionWorld: " + world.error().message);
		phase_ = Phase::Finish;
		return;
	}
	world_ = std::move(*world);
	const bf3::core::Status s = motor_.init(*world_, level->motor);
	if (!s.ok()) {
		fail_phase("prepare", "motor: " + s.error().message);
		phase_ = Phase::Finish;
		return;
	}
	// E3's headless rows and the E1 courses: settle_spawn on every point; the first points whose settled feet have a
	// 20 m find_clear_heading course (the defaults sim.motor uses, so the same courses: points 7, 20, 22, 23, 36, 70).
	for (const sim::SpawnPoint &p : level->spawns) {
		headless_rows_.push_back(sim::settle_spawn(motor_, *world_, p, 1, 0));
	}
	for (size_t i = 0; i < level->spawns.size() && courses_.size() < want_courses_; ++i) {
		const sim::SettleResult settled = sim::settle_spawn(motor_, *world_, level->spawns[i], 1, 1000);
		if (!settled.ok) {
			continue;
		}
		const std::optional<uint16_t> heading = sim::find_clear_heading(motor_, *world_, settled.state.position, 20.0f);
		if (heading) {
			courses_.push_back({ uint32_t(i), settled.state.position, *heading });
		}
	}
	drop_ = sim::find_drop_spot(motor_, *world_);
	prepare_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::string points;
	for (const Course &c : courses_) {
		points += (points.empty() ? "" : ", ") + std::to_string(c.point);
	}
	add("prepare", courses_.size() == want_courses_ && drop_.has_value() ? 0 : 1,
			fmt("in-process world and motor over the session's shapes: %zu settle rows, %zu courses (points %s), drop spot %s, %.2f s", headless_rows_.size(),
					courses_.size(), points.c_str(),
					drop_ ? fmt("(%.2f, %.3f, %.2f)", (*drop_)[0], (*drop_)[1], (*drop_)[2]).c_str() : "none", prepare_seconds_));
	phase_ = courses_.empty() ? (drop_ ? Phase::E1dTeleport : Phase::E3) : Phase::E1Teleport;
}

bool M1Check::teleport(const sim::Vec3 &p_feet, uint16_t p_yaw, uint64_t p_now_ns) {
	host::ClientSession *client = session_.client();
	if (client == nullptr) {
		return false;
	}
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = p_feet;
	q.yaw = p_yaw;
	client->send_debug(q);
	phase_ns_ = p_now_ns;
	return true;
}

bool M1Check::at_teleport(const sim::Vec3 &p_feet) const {
	const host::PresentationFrame &f = session_.frame();
	return f.valid && f.server_own_valid && distance3(f.server_own.position, p_feet) < 0.02f && distance3(f.own_curr.position, p_feet) < 0.02f &&
			horizontal(f.own_curr.velocity) < 1e-3f && f.own_curr.pose == sim::Pose::Stand && f.own_curr.transition_ticks == 0;
}

void M1Check::send_trace(uint32_t p_ticks) {
	net::DebugRequest q;
	q.kind = net::DebugKind::Trace;
	q.id = ++trace_id_;
	q.ticks = std::min(p_ticks, net::kMaxTraceTicks);
	server_own_.clear();
	session_.client()->send_debug(q);
}

std::optional<net::DebugReport> M1Check::take_report(uint32_t p_id) {
	net::DebugReport r;
	std::optional<net::DebugReport> found;
	while (session_.client()->take_debug_report(r)) {
		if (r.id == p_id) {
			found = std::move(r);
		}
		r = net::DebugReport();
	}
	return found;
}

void M1Check::collect_snapshot_states() {
	const host::PresentationFrame &f = session_.frame();
	if (f.valid && f.server_own_valid) {
		server_own_[f.server_own.tick] = f.server_own;
	}
}

void M1Check::compare_snapshots(const std::vector<sim::SoldierState> &p_trace) {
	for (const sim::SoldierState &e : p_trace) {
		const auto it = server_own_.find(e.tick);
		if (it != server_own_.end()) {
			++snapshots_compared_;
			snapshots_equal_ += it->second == e ? 1 : 0;
		}
	}
	server_own_.clear();
}

void M1Check::analyse_segment(const std::vector<sim::SoldierState> &p_trace) {
	const Course &c = courses_[course_];
	const std::string where = fmt("point %u ", c.point);
	if (segment_ < 4) {
		const float expected = kSpeedExpected[segment_];
		SpeedResult &r = speeds_[segment_];
		++r.measured;
		size_t start = p_trace.size();
		for (size_t i = 0; i < p_trace.size(); ++i) {
			if (horizontal(p_trace[i].velocity) > 1e-4f) {
				start = i;
				break;
			}
		}
		if (start + 120 > p_trace.size()) {
			e1_failures_.push_back(where + kSpeedNames[segment_] + ": the trace holds no 120 moving ticks");
			return;
		}
		float lo = 1e9f, hi = 0.0f;
		for (size_t i = start + 59; i < start + 120; ++i) {
			const float v = horizontal(p_trace[i].velocity);
			lo = std::min(lo, v);
			hi = std::max(hi, v);
		}
		const float mean = heading_distance(p_trace[start + 59].position, p_trace[start + 119].position, c.heading) / (60.0f * sim::kTickSeconds);
		const bool pose_ok = p_trace[start].pose == kSpeedPose[segment_] && p_trace[start].target_pose == kSpeedPose[segment_] && p_trace[start].transition_ticks == 0;
		// docs §1 E1a on Metro (sim.motor's reading): the window's mean +-1 %, no tick above +1 %, every tick >= 95 %.
		const bool ok = pose_ok && lo >= 0.95f * expected && hi <= 1.01f * expected && std::fabs(mean - expected) <= 0.01f * expected;
		r.lo = std::min(r.lo, lo);
		r.hi = std::max(r.hi, hi);
		r.worst_mean_error = std::max(r.worst_mean_error, std::fabs(mean - expected) / expected);
		r.passed += ok ? 1 : 0;
		UtilityFunctions::print(to_godot(fmt("[m1-check] E1a %s %s: ticks %u..%u, speed %.5f..%.5f m/s, mean %.5f (expected %.2f)%s", where.c_str(), kSpeedNames[segment_],
				p_trace[start + 59].tick, p_trace[start + 119].tick, double(lo), double(hi), double(mean), double(expected), pose_ok ? "" : ", wrong pose")));
		if (!ok) {
			e1_failures_.push_back(where + kSpeedNames[segment_] + fmt(": %.4f..%.4f mean %.4f%s", lo, hi, mean, pose_ok ? "" : " (pose)"));
		}
		return;
	}
	if (segment_ == 4) {
		++jumps_;
		size_t j = p_trace.size();
		for (size_t i = 0; i < p_trace.size(); ++i) {
			if ((p_trace[i].events & sim::soldier_events::kJumped) != 0) {
				j = i;
				break;
			}
		}
		if (j == 0 || j >= p_trace.size()) {
			e1_failures_.push_back(where + "jump: no take-off inside the trace");
			return;
		}
		const float take_off = p_trace[j - 1].position[1];
		float apex = -1.0f;
		bool landed = false;
		for (size_t i = j; i < p_trace.size(); ++i) {
			apex = std::max(apex, p_trace[i].position[1] - take_off);
			landed = landed || (p_trace[i].events & sim::soldier_events::kLanded) != 0;
		}
		const bool ok = std::fabs(apex - 0.6f) <= 0.001f && landed;
		apex_lo_ = std::min(apex_lo_, apex);
		apex_hi_ = std::max(apex_hi_, apex);
		jumps_ok_ += ok ? 1 : 0;
		UtilityFunctions::print(to_godot(fmt("[m1-check] E1b %s: take-off tick %u, apex %.6f m, landed %d", where.c_str(), p_trace[j].tick, double(apex), landed ? 1 : 0)));
		if (!ok) {
			e1_failures_.push_back(where + fmt("jump: apex %.6f, landed %d", apex, landed ? 1 : 0));
		}
		return;
	}
	++chains_;
	std::vector<int64_t> counts;
	std::string ticks;
	bool order_ok = true;
	for (size_t i = 0; i < p_trace.size(); ++i) {
		const sim::SoldierState &e = p_trace[i];
		if ((e.events & sim::soldier_events::kPoseStarted) == 0) {
			continue;
		}
		const size_t k = counts.size();
		if (k < 6 && (e.pose != kChain[k].from || e.target_pose != kChain[k].to)) {
			order_ok = false;
		}
		int64_t finished = -1;
		for (size_t j = i; j < p_trace.size(); ++j) {
			if (p_trace[j].transition_ticks == 0 && (p_trace[j].events & sim::soldier_events::kPoseFinished) != 0) {
				finished = int64_t(p_trace[j].tick);
				break;
			}
		}
		counts.push_back(finished >= 0 ? finished - int64_t(e.tick) + 1 : -1);
		static const char kPoseLetter[3] = { 'S', 'C', 'P' };
		ticks += fmt(" %c->%c %lld", kPoseLetter[std::min<size_t>(size_t(e.pose), 2)], kPoseLetter[std::min<size_t>(size_t(e.target_pose), 2)], (long long)counts.back());
	}
	bool ok = order_ok && counts.size() == 6;
	for (size_t k = 0; ok && k < 6; ++k) {
		ok = counts[k] == int64_t(kChain[k].ticks);
	}
	chains_ok_ += ok ? 1 : 0;
	chain_ticks_ = ticks;
	UtilityFunctions::print(to_godot(fmt("[m1-check] E1c %s: ticks%s%s", where.c_str(), ticks.c_str(), order_ok ? "" : " (order)")));
	if (!ok) {
		e1_failures_.push_back(where + "transitions:" + ticks + (order_ok ? "" : " (wrong order)"));
	}
}

void M1Check::analyse_drop(const std::vector<sim::SoldierState> &p_trace) {
	const DropCase &d = kDrops[drop_index_];
	const sim::Vec3 &spot = *drop_;
	const float top = spot[1] + d.h;
	size_t first = p_trace.size();
	for (size_t i = 0; i < p_trace.size(); ++i) {
		if (std::fabs(p_trace[i].position[0] - spot[0]) < 1e-3f && std::fabs(p_trace[i].position[2] - spot[2]) < 1e-3f && p_trace[i].position[1] > spot[1] + 0.5f * d.h) {
			first = i;
			break;
		}
	}
	const sim::SoldierState *landed = nullptr;
	for (size_t i = first; i < p_trace.size(); ++i) {
		if ((p_trace[i].events & sim::soldier_events::kLanded) != 0) {
			landed = &p_trace[i];
			break;
		}
	}
	if (landed == nullptr) {
		add(fmt("E1d.%.1fm", d.h), 1, first < p_trace.size() ? "no landing inside the trace" : "the teleport is not in the trace");
		return;
	}
	const float fall = top - landed->position[1];
	const float expected = std::sqrt(2.0f * 9.81f * d.h);
	const bool died = (landed->events & sim::soldier_events::kDied) != 0;
	const bool ok = std::fabs(fall - d.h) <= 0.001f && std::fabs(landed->impact_speed - expected) <= 1e-3f && landed->damage >= d.min_damage &&
			landed->damage <= d.max_damage && died == d.dies && (landed->mode == sim::MoveMode::Dead) == d.dies;
	add(fmt("E1d.%.1fm", d.h), ok ? 0 : 1,
			fmt("Teleport to (%.2f, %.3f, %.2f) at rest: fell %.5f m (h %.1f, within 1 mm), impact %.5f m/s (sqrt(2gh) %.5f), damage %.3f (expected %.0f-%.0f), "
				"health %.3f, %s; landed at server tick %u",
					spot[0], top, spot[2], fall, d.h, landed->impact_speed, expected, landed->damage, d.min_damage, d.max_damage, landed->health,
					died ? "died" : "alive", landed->tick));
}

void M1Check::finish_e1() {
	if (courses_.empty()) {
		return;
	}
	for (size_t k = 0; k < 4; ++k) {
		const SpeedResult &r = speeds_[k];
		add(fmt("E1a.%s", kSpeedNames[k]), r.measured == courses_.size() && r.passed == r.measured ? 0 : 1,
				fmt("%zu / %zu courses: steady speed %.4f..%.4f m/s over ticks 60-120 (expected %.2f; per tick >= 95 %%, <= +1 %%), worst mean error %.3f %%",
						r.passed, courses_.size(), r.lo, r.hi, kSpeedExpected[k], double(r.worst_mean_error) * 100.0));
	}
	add("E1b.jump", jumps_ == courses_.size() && jumps_ok_ == jumps_ ? 0 : 1,
			fmt("%zu / %zu courses: apex %.6f..%.6f m (0.6 +-0.001), landed", jumps_ok_, courses_.size(), apex_lo_, apex_hi_));
	add("E1c.transitions", chains_ == courses_.size() && chains_ok_ == chains_ ? 0 : 1,
			fmt("%zu / %zu courses: S->C 18, C->P 30, P->S 42, S->P 42, P->C 63, C->S 18 ticks expected; last measured:%s (finished - started + 1, PoseStarted -> "
				"PoseFinished)",
					chains_ok_, courses_.size(), chain_ticks_.c_str()));
	for (const std::string &f : e1_failures_) {
		add("E1.failure", 2, f);
	}
}

void M1Check::judge_e2() {
	const Profile &p = kProfiles[profile_];
	const std::vector<host::RestCycleResult> cycles = host::judge_rest(samples_);
	host::ClientSession *client = session_.client();
	const host::ClientStats s = client != nullptr ? client->stats() : host::ClientStats();
	const uint64_t server_failures = session_.server() != nullptr ? session_.server()->game_stats().step_precondition_failures : 0;
	size_t ok = 0, pushed = 0;
	std::string periods;
	float jitter = 0.0f;
	for (const host::RestCycleResult &c : cycles) {
		ok += c.ok ? 1 : 0;
		pushed += c.pushed ? 1 : 0;
		jitter = std::max(jitter, c.eye_jitter_m);
		periods += fmt(" [%s rest %.2f s after the stop%s, jitter %.7f m%s%s]", c.ok ? "ok" : "FAIL", double(c.rest_ns - c.stop_ns) / 1e9, c.pushed ? ", pushed" : "",
				double(c.eye_jitter_m), c.ok ? "" : ": ", c.ok ? "" : c.failure.data());
	}
	const uint64_t presented = session_.others_presented(), extrapolated = session_.others_extrapolated();
	const double extrapolated_pct = presented > 0 ? 100.0 * double(extrapolated) / double(presented) : 0.0;
	const bool sound = s.step_precondition_failures == 0 && server_failures == 0 && s.hash_mismatches == 0;
	const bool all_ok = cycles.size() == size_t(1 + kE2Cycles) && ok == cycles.size();
	const int result = p.gate ? ((all_ok && sound && extrapolated_pct < 5.0) ? 0 : 1) : (sound ? 2 : 1);
	add(std::string("E2.") + p.netsim, result,
			fmt("%s: %zu / %zu rest periods ok (30 s idle + %d walk-stop cycles, %zu samples, one per presented frame, eye from the camera), %zu windows moved by "
				"an edge correction; reconciliations %llu, corrections > 1 mm %llu (max %.4f m), max eye jitter %.7f m; RTT %.1f ms (min %.1f), netsim one-way "
				"%.1f + %.1f ms; precondition failures %llu client / %llu server, hash mismatches %llu; others %llu presented, %.2f %% extrapolated;",
					p.gate ? "the gate" : "stress row, reported (not gated)", ok, cycles.size(), kE2Cycles, samples_.size(), pushed,
					(unsigned long long)s.reconciliations, (unsigned long long)s.corrections_over_1mm, double(s.max_correction_m), double(jitter), s.rtt_ms,
					s.rtt_min_ms, s.netsim_out.mean_delay_ms, s.netsim_in.mean_delay_ms, (unsigned long long)s.step_precondition_failures,
					(unsigned long long)server_failures, (unsigned long long)s.hash_mismatches, (unsigned long long)presented, extrapolated_pct) +
					periods);
}

void M1Check::finish_rows() {
	add("E1.snapshots", snapshots_compared_ > 0 && snapshots_equal_ == snapshots_compared_ ? 0 : 1,
			fmt("%llu snapshot own states seen during the traces equal the trace entry of their tick bit for bit (of %llu compared)",
					(unsigned long long)snapshots_equal_, (unsigned long long)snapshots_compared_));
	const bf3::core::InstallSnapshot &before = session_.install_snapshot_at_start();
	if (before.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, session_.install().snapshot());
		add("install.unchanged", d.clean() ? 0 : 1, d.clean() ? fmt("%zu files: sizes and mtimes equal", before.files.size()) : d.summary());
	} else {
		add("install.unchanged", 1, "no snapshot at start");
	}
	add("memory", 2, fmt("peak working set %.1f MiB (headless, no scene)", mib(peak_working_set_bytes())));
	exit_code_ = print_check_table("M1 CHECK", rows_);
}

bool M1Check::frame(uint64_t p_now_ns) {
	InputMapper &mapper = session_.mapper();
	host::ClientSession *client = session_.client();
	const host::PresentationFrame &f = session_.frame();
	if (phase_ != Phase::Prepare && phase_ != Phase::Finish && phase_ != Phase::Done && client == nullptr) {
		fail_phase("session", "no client");
		phase_ = Phase::Finish;
	}
	switch (phase_) {
		case Phase::Prepare:
			prepare();
			phase_ns_ = p_now_ns;
			step_ = 0;
			return false;

		case Phase::E1Teleport: {
			const Course &c = courses_[course_];
			if (step_ == 0) {
				mapper.release_all();
				mapper.set_requested_pose(sim::Pose::Stand);
				mapper.script_view(sim::yaw_radians(c.heading), 0.0f);
				teleport(c.feet, c.heading, p_now_ns);
				step_ = 1;
			} else if (step_ == 1) {
				if (at_teleport(c.feet)) {
					step_ = 2;
					phase_ns_ = p_now_ns;
				} else if (timed_out(p_now_ns, 10.0)) {
					fail_phase("E1.teleport", fmt("point %u: the soldier did not arrive at the course start within 10 s", c.point));
					phase_ = Phase::E3;
					step_ = 0;
				}
			} else if (seconds_between(phase_ns_, p_now_ns) >= 0.35) {
				phase_ = Phase::E1Segment;
				step_ = 0;
				phase_ns_ = p_now_ns;
			}
			return false;
		}

		case Phase::E1Segment: {
			collect_snapshot_states();
			if (segment_ < 4) {
				const uint32_t pose_ticks = segment_ >= 2 ? 80u : 0u;
				if (step_ == 0) {
					send_trace(pose_ticks + 150);
					if (segment_ == 2) {
						mapper.script_press(InputAction::Crouch);
					} else if (segment_ == 3) {
						mapper.script_press(InputAction::Prone);
					}
					step_tick_ = f.tick;
					step_ = 1;
				} else if (step_ == 1 && f.tick >= step_tick_ + pose_ticks) {
					mapper.script_hold(InputAction::Forward, true);
					mapper.script_hold(InputAction::Sprint, segment_ == 1);
					walk_tick_ = f.tick;
					step_ = 2;
				} else if (step_ == 2 && f.tick >= walk_tick_ + 128) {
					mapper.release_all();
					phase_ = Phase::E1Report;
					phase_ns_ = p_now_ns;
				}
			} else if (segment_ == 4) {
				if (step_ == 0) {
					send_trace(100);
					step_tick_ = f.tick;
					step_ = 1;
				} else if (f.tick >= step_tick_ + 6) {
					mapper.script_press(InputAction::Jump);
					phase_ = Phase::E1Report;
					phase_ns_ = p_now_ns;
				}
			} else {
				// The chain: press, wait for the predicted transition to end, 6 ticks, next. Odd steps wait for transition
				// step_ / 2; even steps (> 0) follow transition step_ / 2 - 1 and press transition step_ / 2.
				const int chain = step_ % 2 == 1 ? step_ / 2 : step_ / 2 - 1;
				if (step_ == 0) {
					send_trace(380);
					mapper.script_press(kChain[0].press);
					step_ = 1;
				} else if (step_ % 2 == 1) {
					const sim::Pose to = kChain[chain].to;
					if (f.own_curr.pose == to && f.own_curr.target_pose == to && f.own_curr.transition_ticks == 0) {
						step_tick_ = f.tick;
						++step_;
					} else if (timed_out(p_now_ns, 20.0)) {
						e1_failures_.push_back(fmt("point %u transitions: step %d did not finish", courses_[course_].point, chain));
						phase_ = Phase::E1Report;
						phase_ns_ = p_now_ns;
					}
				} else if (f.tick >= step_tick_ + 6) {
					if (chain + 1 >= 6) {
						phase_ = Phase::E1Report;
						phase_ns_ = p_now_ns;
					} else {
						mapper.script_press(kChain[chain + 1].press);
						++step_;
					}
				}
			}
			return false;
		}

		case Phase::E1Report: {
			collect_snapshot_states();
			std::optional<net::DebugReport> r = take_report(trace_id_);
			if (!r) {
				if (timed_out(p_now_ns, 30.0)) {
					e1_failures_.push_back(fmt("point %u segment %zu: no trace report within 30 s", courses_[course_].point, segment_));
				} else {
					return false;
				}
			} else {
				compare_snapshots(r->trace);
				analyse_segment(r->trace);
			}
			++segment_;
			if (segment_ >= 6) {
				segment_ = 0;
				++course_;
			}
			step_ = 0;
			phase_ns_ = p_now_ns;
			if (course_ >= courses_.size()) {
				finish_e1();
				phase_ = drop_ ? Phase::E1dTeleport : Phase::E3;
				mapper.release_all();
				mapper.set_requested_pose(sim::Pose::Stand);
			} else {
				phase_ = Phase::E1Teleport;
			}
			return false;
		}

		case Phase::E1dTeleport: {
			const DropCase &d = kDrops[drop_index_];
			if (step_ == 0) {
				// Alive, the life adopted, at rest.
				if (f.valid && f.own_curr.mode != sim::MoveMode::Dead && mapper.adopted_life() == f.own_curr.life && horizontal(f.own_curr.velocity) < 1e-3f) {
					mapper.release_all();
					mapper.set_requested_pose(sim::Pose::Stand);
					drop_life_ = f.own_curr.life;
					send_trace(d.h > 20.0f ? 260 : 160);
					teleport({ (*drop_)[0], (*drop_)[1] + d.h, (*drop_)[2] }, 0, p_now_ns);
					phase_ = Phase::E1dReport;
				} else if (timed_out(p_now_ns, 15.0)) {
					fail_phase(fmt("E1d.%.1fm", d.h), "the soldier is not alive and at rest");
					phase_ = Phase::E3;
					step_ = 0;
				}
			}
			return false;
		}

		case Phase::E1dReport: {
			collect_snapshot_states();
			std::optional<net::DebugReport> r = take_report(trace_id_);
			if (!r) {
				if (timed_out(p_now_ns, 30.0)) {
					fail_phase(fmt("E1d.%.1fm", kDrops[drop_index_].h), "no trace report within 30 s");
					phase_ = Phase::E3;
					step_ = 0;
				}
				return false;
			}
			compare_snapshots(r->trace);
			analyse_drop(r->trace);
			phase_ = Phase::E1dRespawn;
			phase_ns_ = p_now_ns;
			return false;
		}

		case Phase::E1dRespawn: {
			// After a death the server respawns the slot (respawn_ticks); the mapper adopts the new life.
			const bool dead_case = kDrops[drop_index_].dies;
			const bool ready = dead_case ? (mapper.adopted_life() > drop_life_ && seconds_between(phase_ns_, p_now_ns) > 1.5) : seconds_between(phase_ns_, p_now_ns) > 0.5;
			if (ready || timed_out(p_now_ns, 15.0)) {
				++drop_index_;
				step_ = 0;
				phase_ns_ = p_now_ns;
				phase_ = drop_index_ >= std::size(kDrops) ? Phase::E3 : Phase::E1dTeleport;
			}
			return false;
		}

		case Phase::E3: {
			if (step_ == 0) {
				net::DebugRequest q;
				q.kind = net::DebugKind::SpawnSweep;
				q.id = ++request_id_;
				q.ticks = 60;
				trace_id_ = q.id; // the report id to wait for
				client->send_debug(q);
				step_ = 1;
				phase_ns_ = p_now_ns;
				return false;
			}
			std::optional<net::DebugReport> r = take_report(trace_id_);
			if (!r) {
				if (timed_out(p_now_ns, 60.0)) {
					fail_phase("E3.sweep", "no SpawnSweep report within 60 s");
					phase_ = Phase::E2Start;
					step_ = 0;
				}
				return false;
			}
			const std::shared_ptr<const host::SharedLevel> level = session_.shared_level();
			size_t ok = 0, equal = 0;
			float worst_lift = 0.0f, worst_shift = 0.0f, worst_gap = 0.0f;
			std::string bad;
			for (size_t i = 0; i < r->sweep.size() && i < headless_rows_.size(); ++i) {
				const net::SpawnSweepRow &row = r->sweep[i];
				const sim::SettleResult &h = headless_rows_[i];
				const bool same = row.index == level->spawns[i].index && row.ok == h.ok && row.point == level->spawns[i].position && row.settled == h.state.position &&
						row.lift == h.lift && row.shift == h.shift && row.gap == h.gap && row.grounded == ((h.state.flags & sim::soldier_flags::kGrounded) != 0) &&
						row.overlapping == h.overlapping;
				ok += row.ok ? 1 : 0;
				equal += same ? 1 : 0;
				if (!same || !row.ok) {
					bad += fmt(" %u%s", row.index, row.ok ? "(differs)" : "(not ok)");
				}
				worst_lift = std::fabs(row.lift) > std::fabs(worst_lift) ? row.lift : worst_lift;
				worst_shift = std::max(worst_shift, row.shift);
				worst_gap = std::max(worst_gap, row.gap);
			}
			const size_t points = level->spawns.size();
			const bool pass = r->sweep.size() == points && ok == points && equal == points;
			add("E3.sweep", pass ? 0 : 1,
					fmt("SpawnSweep (128 slots, one batch, 60 live ticks): %zu rows, %zu ok, %zu equal field for field to the in-process settle_spawn rows (of %zu "
						"points); worst lift %.3f m, shift %.3f m, gap %.5f m; %.2f s through the server; %s",
							r->sweep.size(), ok, equal, points, worst_lift, worst_shift, worst_gap, seconds_between(phase_ns_, p_now_ns), r->text.c_str()) +
							bad);
			phase_ = Phase::E2Start;
			step_ = 0;
			phase_ns_ = p_now_ns;
			return false;
		}

		case Phase::E2Start: {
			if (profile_ >= std::size(kProfiles)) {
				phase_ = Phase::Finish;
				return false;
			}
			std::string error;
			if (!session_.restart_client(kProfiles[profile_].netsim, kProfiles[profile_].seed, &error)) {
				fail_phase(std::string("E2.") + kProfiles[profile_].netsim, "client restart: " + error);
				phase_ = Phase::Finish;
				return false;
			}
			phase_ = Phase::E2Spawn;
			step_ = 0;
			phase_ns_ = p_now_ns;
			return false;
		}

		case Phase::E2Spawn: {
			// The new client's first life, then SpawnAt the first course's point (a known 20 m course).
			if (step_ == 0) {
				if (f.valid && f.own_curr.life > 0 && mapper.adopted_life() == f.own_curr.life) {
					e2_life_ = f.own_curr.life;
					net::DebugRequest q;
					q.kind = net::DebugKind::SpawnAt;
					q.id = ++request_id_;
					q.index = courses_.empty() ? 0 : courses_[0].point;
					client->send_debug(q);
					step_ = 1;
					phase_ns_ = p_now_ns;
				} else if (timed_out(p_now_ns, 15.0)) {
					fail_phase(std::string("E2.") + kProfiles[profile_].netsim, "the new client did not spawn within 15 s");
					phase_ = Phase::Finish;
				}
			} else if (mapper.adopted_life() > e2_life_ && f.own_curr.life == mapper.adopted_life()) {
				phase_ = Phase::E2Settle;
				phase_ns_ = p_now_ns;
			} else if (timed_out(p_now_ns, 15.0)) {
				fail_phase(std::string("E2.") + kProfiles[profile_].netsim, "SpawnAt gave no new life within 15 s");
				phase_ = Phase::Finish;
			}
			return false;
		}

		case Phase::E2Settle:
			if (!courses_.empty()) {
				mapper.script_view(sim::yaw_radians(courses_[0].heading), 0.0f);
			}
			if (seconds_between(phase_ns_, p_now_ns) >= 1.2) {
				samples_.clear();
				session_.reset_others_counts();
				record_start_ns_ = p_now_ns;
				phase_ = Phase::E2Record;
			}
			return false;

		case Phase::E2Record: {
			const double t = seconds_between(record_start_ns_, p_now_ns);
			const double total = kE2IdleS + double(kE2Cycles) * (kE2WalkS + kE2RestS);
			if (t >= total) {
				mapper.release_all();
				judge_e2();
				++profile_;
				phase_ = Phase::E2Start;
				return false;
			}
			// docs §1 E2: 30 s idle after the spawn, then walk 2 s / rest 13 s, forward and back along the course.
			int forward = 0;
			if (t >= kE2IdleS) {
				const int k = int((t - kE2IdleS) / (kE2WalkS + kE2RestS));
				const double in_cycle = std::fmod(t - kE2IdleS, kE2WalkS + kE2RestS);
				if (k < kE2Cycles && in_cycle < kE2WalkS) {
					forward = (k % 2 == 0) ? 1 : -1;
				}
			}
			mapper.script_hold(InputAction::Forward, forward > 0);
			mapper.script_hold(InputAction::Back, forward < 0);
			if (f.valid) {
				host::RestSample r;
				r.time_ns = p_now_ns;
				r.tick = f.tick;
				r.input_idle = f.own_command.move_forward == 0 && f.own_command.move_right == 0 && f.own_command.buttons == 0;
				r.horizontal_speed = horizontal(f.own_curr.velocity);
				// The presented eye, read back from the first-person camera this frame placed.
				const Vector3 eye = session_.cameras().first_person()->get_global_transform().origin;
				r.eye = { eye.x, eye.y, eye.z };
				r.visual_offset_m = std::sqrt(f.visual_offset[0] * f.visual_offset[0] + f.visual_offset[1] * f.visual_offset[1] + f.visual_offset[2] * f.visual_offset[2]);
				r.reconciliations = f.stats.reconciliations;
				r.last_reconcile_tick = f.stats.last_reconcile_tick;
				samples_.push_back(r);
			}
			return false;
		}

		case Phase::Finish:
			finish_rows();
			phase_ = Phase::Done;
			return true;

		case Phase::Done:
			return true;
	}
	return false;
}

// ---- M1LoadCheck ------------------------------------------------------------------------------------------------

M1LoadCheck::M1LoadCheck(Bf3LevelSession &p_session, bool p_warm, ShotRunner *p_shots) :
		session_(p_session), warm_(p_warm), shots_(p_shots) {}

bool M1LoadCheck::frame(uint64_t p_now_ns, double p_delta) {
	if (start_ns_ == 0) {
		start_ns_ = p_now_ns;
		session_.mapper().set_scripted(true); // the first-person view at the spawn, no input
		return false;
	}
	if (phase_ == 0) {
		frame_ms_.push_back(float(p_delta * 1000.0));
		if (seconds_between(start_ns_, p_now_ns) >= 5.0) {
			phase_ = 1;
		}
		return false;
	}
	if (phase_ == 1) {
		if (shots_ != nullptr && !shots_->empty() && !shots_->frame(p_now_ns)) {
			return false;
		}
		phase_ = 2;
	}
	report();
	return true;
}

void M1LoadCheck::report() {
	std::vector<CheckRow> rows;
	const auto add = [&](const std::string &p_check, int p_result, const std::string &p_detail) { rows.push_back({ p_check, p_result, p_detail }); };
	const SessionTimings &t = session_.timings();
	const host::LoadProgress &p = session_.progress();
	const auto stage_s = [&](host::LoadStage p_stage) {
		const uint64_t a = p.start_us[size_t(p_stage)].load(), b = p.end_us[size_t(p_stage)].load();
		return a != 0 && b >= a ? double(b - a) / 1e6 : 0.0;
	};
	const auto at_s = [&](uint64_t p_us) { return p_us >= t.ready_us && p_us != 0 ? double(p_us - t.ready_us) / 1e6 : 0.0; };
	const double limit = warm_ ? 5.0 : 15.0;
	add(warm_ ? "E5.warm" : "E4.cold", t.done_from_process_s <= limit ? 0 : 1,
			fmt("process start -> first controllable frame %.2f s (<= %.0f s); _ready -> Done %.2f s; process start -> _ready %.2f s", t.done_from_process_s, limit,
					t.done_from_ready_s, t.process_start_to_ready_s));
	const auto warmth = host::CacheWarmth(p.warmth.load());
	const std::optional<bf3::render::SceneLaneStats> lanes = session_.lane_stats();
	const std::shared_ptr<const bf3::render::LevelScenePlan> plan = session_.scene_plan();
	size_t requests_used = 0;
	if (plan != nullptr) {
		std::vector<bool> used(plan->requests.size(), false);
		for (const bf3::render::SceneMeshPlan &m : plan->meshes) {
			if (m.request < used.size() && !used[m.request]) {
				used[m.request] = true;
				++requests_used;
			}
		}
	}
	{
		const bool shapes_ok = warm_ ? warmth == host::CacheWarmth::Warm : warmth == host::CacheWarmth::Cold;
		const bool meshes_ok = lanes.has_value() &&
				(warm_ ? (lanes->cache_converted == 0 && lanes->cache_hits == requests_used) : (lanes->cache_hits == 0 && lanes->cache_converted == requests_used));
		add("cache.warmth", shapes_ok && meshes_ok ? 0 : 1,
				fmt("%s run: static world %s; mesh blobs %llu hits, %llu converted of %zu requests; ", warm_ ? "warm" : "cold", cache_warmth_name(warmth),
						lanes ? (unsigned long long)lanes->cache_hits : 0ull, lanes ? (unsigned long long)lanes->cache_converted : 0ull, requests_used) +
						session_.cache_summary());
	}
	add("phase.A", 2,
			fmt("loader: mount %.3f s, db %.3f s, census %.3f s, plan %.3f s (plan at +%.3f s after _ready), shapes %.3f s (SharedLevel at +%.3f s), cache open %.3f s",
					stage_s(host::LoadStage::Mount), stage_s(host::LoadStage::Database), t.seconds_census, t.seconds_plan, at_s(t.plan_us),
					stage_s(host::LoadStage::Shapes), at_s(t.shared_us), t.seconds_cache_open));
	if (lanes) {
		add("phase.B", 2,
				fmt("scene lanes: wall %.3f s (lanes done at +%.3f s); work meshes %.3f s, textures %.3f s, terrain %.3f s; items %llu meshes, %llu textures, %llu "
					"terrains; %.1f MiB meshes, %.1f MiB textures; peak in flight %.1f MiB; %llu failures",
						lanes->wall_seconds, at_s(t.lanes_us), lanes->mesh_seconds, lanes->texture_seconds, lanes->terrain_seconds,
						(unsigned long long)lanes->mesh_items, (unsigned long long)lanes->texture_items, (unsigned long long)lanes->terrain_items,
						mib(lanes->mesh_bytes), mib(lanes->texture_bytes), mib(lanes->peak_inflight_bytes), (unsigned long long)lanes->failures));
	}
	const UploadStats u = session_.upload().stats();
	add("phase.C1", 2,
			fmt("worker tasks: %llu tasks, work %.3f s (max task %.2f ms), %llu meshes + %llu terrains (%llu surfaces, %llu vertices, %llu triangles), %llu textures; "
				"last C1 at +%.3f s; peak queue %llu items",
					(unsigned long long)u.tasks, u.c1_work_seconds, u.c1_max_task_ms, (unsigned long long)u.meshes, (unsigned long long)u.terrains,
					(unsigned long long)u.surfaces, (unsigned long long)u.vertices, (unsigned long long)u.triangles, (unsigned long long)u.textures,
					at_s(u.last_c1_us), (unsigned long long)u.queued_peak));
	add("phase.C2", 2,
			fmt("main thread: work %.3f s over %llu frames (max %.2f ms in a frame), %llu materials, %llu texture bindings, %llu instances; upload finished at +%.3f s",
					u.c2_work_seconds, (unsigned long long)u.c2_frames, u.c2_max_frame_ms, (unsigned long long)u.materials, (unsigned long long)u.texture_bindings,
					(unsigned long long)u.instances, at_s(t.upload_us)));
	add("phase.D", 2,
			fmt("server + client started at +%.3f s (connect %.3f s), spawned %.3f s after; Done at +%.3f s", at_s(t.session_us), stage_s(host::LoadStage::Connect),
					stage_s(host::LoadStage::Spawn), at_s(t.done_us)));
	add("loading.frames", 2,
			fmt("%llu frames behind the loading screen (vsync off), mean %.2f ms, max %.2f ms", (unsigned long long)t.loading_frames,
					t.loading_frames ? t.loading_frame_ms_sum / double(t.loading_frames) : 0.0, t.loading_frame_ms_max));
	if (plan != nullptr) {
		const std::vector<std::pair<std::string, std::string>> failures = session_.upload().failures();
		const size_t meshes = session_.upload().mesh_rids();
		const size_t expected_instances = plan->instances.size() + plan->terrains.size();
		const bool ok = meshes == plan->meshes.size() && u.instances == expected_instances && u.terrains == plan->terrains.size() &&
				u.textures + failures.size() == plan->textures.size() && u.materials == plan->materials.size() + plan->terrains.size();
		std::string list;
		for (size_t i = 0; i < failures.size() && i < 8; ++i) {
			list += " [" + failures[i].first + ": " + failures[i].second + "]";
		}
		add("upload.counts", ok ? 0 : 1,
				fmt("RS meshes %zu / %zu plan meshes, instances %llu / %zu (plan instances + terrains), textures %llu + %zu failed / %zu, materials %llu / %zu",
						meshes, plan->meshes.size(), (unsigned long long)u.instances, expected_instances, (unsigned long long)u.textures, failures.size(),
						plan->textures.size(), (unsigned long long)u.materials, plan->materials.size() + plan->terrains.size()) +
						list);
	}
	add("frames.after_done", 2,
			fmt("%zu frames over 5 s after Done at the spawn (first person, vsync restored): p50 %.2f ms, p99 %.2f ms", frame_ms_.size(),
					double(percentile(frame_ms_, 0.5)), double(percentile(frame_ms_, 0.99))));
	add("memory.peak", 2,
			fmt("game peak working set %.1f MiB (reported, not gated: E6 gates the converter, tools.m1_prepare; orchestrator decision on docs §16 question 4); "
				"loader sample %.1f MiB",
					mib(peak_working_set_bytes()), mib(p.peak_working_set.load())));
	rows.push_back(check_surface_format());
	if (shots_ != nullptr && !shots_->empty()) {
		add("shots", shots_->failed() == 0 ? 0 : 1, fmt("%zu saved, %zu failed", shots_->saved(), shots_->failed()));
		for (const std::string &l : shots_->lines()) {
			add("shot", 2, l);
		}
	}
	const bf3::core::InstallSnapshot &before = session_.install_snapshot_at_start();
	if (before.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, session_.install().snapshot());
		add("install.unchanged", d.clean() ? 0 : 1, d.clean() ? fmt("%zu files: sizes and mtimes equal", before.files.size()) : d.summary());
	} else {
		add("install.unchanged", 1, "no snapshot at start");
	}
	exit_code_ = print_check_table("M1 LOAD CHECK", rows);
}

} // namespace bf3::godot_side
