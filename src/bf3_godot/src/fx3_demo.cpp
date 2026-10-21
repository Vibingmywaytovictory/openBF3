#include "fx3_demo.h"

#include "fx_renderer.h"
#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"
#include "viewmodel_muzzle.h"

#include "bf3/anim/crowd_stage.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/percentile.h"
#include "bf3/core/util/text.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/weapons/sim_time.h"

#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace bf3::godot_side {

using namespace godot;
namespace fx = bf3::fx;
namespace hk = bf3::host;

namespace {

constexpr uint8_t kOwnSlot = 0;
constexpr uint8_t kRemoteSlot = 5;
constexpr uint8_t kVictimSlot = 9;
constexpr uint32_t kChestZone = 2;
constexpr uint64_t kMatchSeed = 0xF3F3F3F3ull;
constexpr const char *kRifle = "weapons/m16a4/u_m16a4";
constexpr double kShotPeriod = 0.075;
constexpr double kRunSeconds = 15.0;
constexpr int kSettleFrames = 4;
constexpr int kFlashRetries = 24;
constexpr int kImpactsPerShot = 6;
constexpr float kImpactSpread = 0.18f;
constexpr float kSurfaceRay = 20.0f, kSurfaceMin = 1.5f, kSurfaceMax = 12.0f, kSurfaceBest = 4.0f;
constexpr float kViewDistance = 1.5f;
constexpr uint64_t kMinShotBytes = 50 * 1024;

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[fx3] " + p_line));
}

using bf3::core::fmt;

std::filesystem::path utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string path_text(const std::filesystem::path &p_path) {
	const std::u8string s = p_path.generic_u8string();
	return std::string(s.begin(), s.end());
}

using bf3::core::to_lower_ascii;

std::string leaf(std::string_view p_name) {
	const size_t s = p_name.rfind('/');
	return std::string(s == std::string_view::npos ? p_name : p_name.substr(s + 1));
}

fx::Vec3 fv(const Vector3 &p_v) {
	return fx::Vec3{ float(p_v.x), float(p_v.y), float(p_v.z) };
}

Vector3 gv(const bf3::anim::Vec3 &p_v) {
	return Vector3(p_v.x, p_v.y, p_v.z);
}

bf3::sim::Vec3 sv(const Vector3 &p_v) {
	return bf3::sim::Vec3{ float(p_v.x), float(p_v.y), float(p_v.z) };
}

using bf3::core::rounded_rank;

int category_of(std::string_view p_material) {
	const std::string n = to_lower_ascii(p_material);
	if (n == "concrete") {
		return 0;
	}
	if (n == "metal") {
		return 1;
	}
	for (const char *d : { "gravel", "sand", "grass", "dirt", "mud", "earth", "soil", "forest" }) {
		if (n.find(d) != std::string::npos) {
			return 2;
		}
	}
	return -1;
}

}

struct Bf3Fx3Demo::Loaded {
	struct Candidate {
		Vector3 point, normal;
		uint8_t material = 0;
	};
	std::vector<Candidate> dirt;
	std::shared_ptr<bf3::core::Cache> cache;
	std::shared_ptr<bf3::core::Install> install;
	std::shared_ptr<const hk::GunplayLevel> gunplay;
	Bf3GameFx fx;
	std::string error;
	std::vector<std::string> notes;
	double seconds = 0;
	std::atomic<bool> done{ false };
};

Bf3Fx3Demo::Bf3Fx3Demo() = default;

Bf3Fx3Demo::~Bf3Fx3Demo() {
	shutdown();
}

void Bf3Fx3Demo::_bind_methods() {}

void Bf3Fx3Demo::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		shutdown();
	}
}

void Bf3Fx3Demo::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	check_ = has_user_arg("fx3-check");
	no_cache_ = has_user_arg("no-cache");
	shot_dir_ = user_arg_value("fx3-shot-dir");
	if (shot_dir_.empty()) {
		const std::filesystem::path project = utf8_path(to_std(ProjectSettings::get_singleton()->globalize_path("res://")));
		shot_dir_ = path_text((project / "..").lexically_normal() / "b" / "shots" / "fx3");
	}
	if (!no_cache_) {
		const std::string root = user_arg_value("bf3-cache");
		cache_root_ = root.empty() ? default_cache_root() : utf8_path(root);
	}
	session_ = Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
	if (session_ == nullptr) {
		UtilityFunctions::push_error("[fx3] Bf3Fx3Demo needs a sibling Bf3LevelSession named Session");
		return;
	}
	log(std::string("fx3 demo: ") + (check_ ? "--fx3-check" : "interactive") + ", shots " + shot_dir_);
	if (check_) {
		DisplayServer *ds = DisplayServer::get_singleton();
		const int32_t primary = ds->get_primary_screen();
		ds->window_set_mode(DisplayServer::WINDOW_MODE_WINDOWED);
		ds->window_set_flag(DisplayServer::WINDOW_FLAG_BORDERLESS, true);
		ds->window_set_current_screen(primary);
		ds->window_set_position(ds->screen_get_position(primary));
		ds->window_set_size(Vector2i(1920, 1080));
	}
	set_process(true);
}

void Bf3Fx3Demo::start_loader() {
	started_ = true;
	const bf3::core::InstallReader &reader = session_->install();
	const bf3::core::PathGuard guard = reader.write_guard();
	const std::filesystem::path root = cache_root_;
	const std::shared_ptr<const hk::SharedLevel> level = session_->shared_level();
	loaded_ = std::make_unique<Loaded>();
	Loaded *out = loaded_.get();
	const std::atomic<bool> *stop = &stop_;
	loader_ = std::thread([out, &reader, guard, root, level, stop]() {
		auto body = [&]() {
			const auto t0 = std::chrono::steady_clock::now();
			if (!level) {
				out->error = "the session has no shared level";
				return;
			}
			if (!root.empty()) {
				AcquiredCache acquired = acquire_cache(root, guard);
				if (acquired.status == bf3::core::CacheStatus::Ok) {
					out->cache = acquired.cache;
				}
				out->notes.push_back("cache " + path_text(root) + (out->cache ? " opened" : " not used (" + std::string(bf3::core::cache_status_name(acquired.status)) + ")"));
			}
			bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(reader);
			if (!install) {
				out->error = "install: " + install.error().message;
				return;
			}
			out->install = *install;

			const auto tg = std::chrono::steady_clock::now();
			bf3::core::Result<std::shared_ptr<const hk::GunplayLevel>> gl = hk::load_gunplay_level(out->install, level->level, level->mode);
			if (!gl) {
				out->error = "gunplay: " + gl.error().message;
				return;
			}
			out->gunplay = *gl;
			out->notes.push_back(fmt("gunplay level %s / %s: %zu defs in %.2f s", level->level.c_str(), level->mode.c_str(), out->gunplay->defs.size(),
					std::chrono::duration<double>(std::chrono::steady_clock::now() - tg).count()));

			{
				constexpr size_t kDirtPerMaterial = 64;
				bf3::core::Result<bf3::core::level::LevelCollision> col = bf3::core::level::load_level_collision(out->install, level->level, level->mode);
				if (col) {
					std::map<uint8_t, size_t> taken;
					size_t index = 0;
					for (const bf3::core::level::SoupTile &tile : col->soup.tiles) {
						for (const bf3::core::level::SoupTriangle &tri : tile.triangles) {
							++index;
							const uint8_t phys = out->gunplay->materials->static_pair(tri.material).physics_material;
							if ((phys != 11 && phys != 12 && phys != 16 && phys != 136) || taken[phys] >= kDirtPerMaterial || index % 7 != 0) {
								continue;
							}
							const Vector3 a(tri.v[0][0], tri.v[0][1], tri.v[0][2]), b(tri.v[1][0], tri.v[1][1], tri.v[1][2]), c(tri.v[2][0], tri.v[2][1], tri.v[2][2]);
							Vector3 n = (b - a).cross(c - a);
							if (n.length() < 1e-6f) {
								continue;
							}
							n = n.normalized();
							if (n.y < 0.0f) {
								n = -n;
							}
							if (n.y < 0.7f) {
								continue;
							}
							++taken[phys];
							out->dirt.push_back({ (a + b + c) / 3.0f, n, phys });
						}
					}
					out->notes.push_back(fmt("dirt candidates: %zu up-facing triangles (Gravel %zu, Sand %zu, Grass %zu, Forest %zu)", out->dirt.size(), taken[11], taken[12],
							taken[16], taken[136]));
				} else {
					out->notes.push_back("dirt candidates: " + col.error().message);
				}
			}
			auto shared = std::make_shared<hk::SharedLevel>(*level);
			shared->gunplay = out->gunplay;
			hk::LevelDatabase db;
			db.install = out->install;
			GameLoadInputs in;
			in.db = &db;
			in.shared = shared;
			in.cache = out->cache.get();
			in.stop = stop;
			const bf3::core::Status st = out->fx.load(in);
			out->cache.reset();
			if (!st) {
				out->error = st.error().message;
				return;
			}
			out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		};
		body();
		out->done.store(true, std::memory_order_release);
	});
}

void Bf3Fx3Demo::set_pose(const Vector3 &p_position, const Vector3 &p_target) {
	const Vector3 d = (p_target - p_position).normalized();
	camera_->look_at_from_position(p_position, p_target, std::fabs(d.y) > 0.999f ? Vector3(0, 0, 1) : Vector3(0, 1, 0));
}

void Bf3Fx3Demo::find_surfaces() {
	const fx::ImpactTable &table = *loaded_->fx.impacts();
	const bf3::sim::weapons::SurfaceMaterialTable &materials = *loaded_->gunplay->materials;
	const uint32_t mask = bf3::sim::query_mask::kProjectile;

	std::vector<Vector3> eyes{ main_position_ };
	for (const bf3::sim::SpawnPoint &s : session_->shared_level()->spawns) {
		eyes.push_back(Vector3(s.position[0], s.position[1] + 1.7f, s.position[2]));
	}
	Surface *best[3] = { &concrete_, &metal_, &dirt_ };
	std::map<std::string, int> seen;
	uint64_t rays = 0;
	for (const Vector3 &eye : eyes) {
		for (int yi = 0; yi < 24; ++yi) {
			const float yaw = 2.0f * 3.14159265f * float(yi) / 24.0f;
			for (const float pitch : { -0.7f, -0.4f, -0.15f, 0.0f, 0.2f }) {
				const Vector3 dir(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
				bf3::sim::StaticHit hit;
				++rays;
				if (!collision_->ray(sv(eye), sv(dir * kSurfaceRay), mask, &hit)) {
					continue;
				}
				const uint8_t phys = materials.static_pair(hit.tag.material).physics_material;
				const std::string name(table.material_name(phys));
				++seen[name];
				const int cat = category_of(name);
				const float dist = hit.fraction * kSurfaceRay;
				const Vector3 n = Vector3(hit.normal[0], hit.normal[1], hit.normal[2]).normalized();
				if (cat < 0 || dist < kSurfaceMin || dist > kSurfaceMax || std::fabs(n.dot(dir)) < 0.5f) {
					continue;
				}
				Surface &b = *best[cat];
				if (b.found && std::fabs(dist - kSurfaceBest) >= std::fabs(b.distance - kSurfaceBest)) {
					continue;
				}
				const fx::ImpactCell *cell = table.select(bullet_, phys);
				if (cell == nullptr || cell->effects.empty()) {
					continue;
				}

				const Vector3 p(hit.position[0], hit.position[1], hit.position[2]);
				const Vector3 cam = p + n * kViewDistance + Vector3(0, 0.25f, 0);
				if (collision_->ray(sv(p + n * 0.05f), sv(cam - (p + n * 0.05f)), bf3::sim::query_mask::kAll, nullptr)) {
					continue;
				}
				b.found = true;
				b.distance = dist;
				b.point = p;
				b.normal = n;
				b.material = phys;
				b.name = name;
				b.effect = leaf(cell->effects.front());
			}
		}
	}

	for (const uint8_t want : { uint8_t(12), uint8_t(16), uint8_t(136), uint8_t(11) }) {
		if (dirt_.found && dirt_.name.find("(triangle)") != std::string::npos) {
			break;
		}
		for (const Loaded::Candidate &c : loaded_->dirt) {
			if (c.material != want) {
				continue;
			}
			const Vector3 from = c.point + c.normal * 0.05f;
			const Vector3 cam = c.point + c.normal * kViewDistance + Vector3(0, 0.25f, 0);
			if (collision_->ray(sv(from), sv(c.normal * 3.0f), bf3::sim::query_mask::kAll, nullptr) ||
					collision_->ray(sv(from), sv(cam - from), bf3::sim::query_mask::kAll, nullptr)) {
				continue;
			}
			const fx::ImpactCell *cell = table.select(bullet_, c.material);
			if (cell == nullptr || cell->effects.empty()) {
				continue;
			}
			dirt_.found = true;
			dirt_.distance = 0;
			dirt_.point = c.point;
			dirt_.normal = c.normal;
			dirt_.material = c.material;
			dirt_.name = std::string(table.material_name(c.material)) + " (triangle)";
			dirt_.effect = leaf(cell->effects.front());
			break;
		}
	}

	{
		Vector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
		for (const Vector3 &e : eyes) {
			lo = Vector3(std::min(lo.x, e.x), std::min(lo.y, e.y), std::min(lo.z, e.z));
			hi = Vector3(std::max(hi.x, e.x), std::max(hi.y, e.y), std::max(hi.z, e.z));
		}
		auto rank = [](std::string_view p_name) {
			const std::string n = to_lower_ascii(p_name);
			return n.find("sand") != std::string::npos ? 3 : (n.find("grass") != std::string::npos ? 2 : 1);
		};
		int best_rank = dirt_.found ? (dirt_.name.find("(triangle)") != std::string::npos ? 9 : 0) : -1;
		for (float x = lo.x - 60.0f; x <= hi.x + 60.0f; x += 3.0f) {
			for (float z = lo.z - 60.0f; z <= hi.z + 60.0f; z += 3.0f) {
				const Vector3 top(x, hi.y + 60.0f, z);
				bf3::sim::StaticHit hit;
				++rays;
				if (!collision_->ray(sv(top), sv(Vector3(0, -160.0f, 0)), mask, &hit)) {
					continue;
				}
				const uint8_t phys = materials.static_pair(hit.tag.material).physics_material;
				const std::string name(table.material_name(phys));
				++seen[name];
				const Vector3 n = Vector3(hit.normal[0], hit.normal[1], hit.normal[2]).normalized();
				if (category_of(name) != 2 || n.y < 0.7f || rank(name) <= best_rank) {
					continue;
				}
				const fx::ImpactCell *cell = table.select(bullet_, phys);
				if (cell == nullptr || cell->effects.empty()) {
					continue;
				}
				best_rank = rank(name);
				dirt_.found = true;
				dirt_.distance = 0;
				dirt_.point = Vector3(hit.position[0], hit.position[1], hit.position[2]);
				dirt_.normal = n;
				dirt_.material = phys;
				dirt_.name = name + " (open ground)";
				dirt_.effect = leaf(cell->effects.front());
			}
		}
	}
	std::string line = fmt("surfaces (%llu rays from %zu eyes and a downward grid):", (unsigned long long)rays, eyes.size());
	for (const auto &[n, c] : seen) {
		line += " " + n + " x" + std::to_string(c);
	}
	notes_.push_back(line);
}

void Bf3Fx3Demo::place_soldier() {

	const Vector3 fwd = ((main_target_ - main_position_) * Vector3(1, 0, 1)).normalized();
	bf3::sim::StaticHit hit;
	if (!collision_->ground_below(sv(main_position_ + fwd * 4.0f), 4.0f, bf3::sim::query_mask::kCollision, &hit)) {
		notes_.push_back("soldier stand-in: no ground 4 m in front of the main pose");
		return;
	}
	const Vector3 facing = -fwd;
	const Vector3 feet(hit.position[0], hit.position[1], hit.position[2]);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	Ref<CapsuleMesh> mesh;
	mesh.instantiate();
	mesh->set_radius(0.3f);
	mesh->set_height(1.8f);
	Ref<StandardMaterial3D> mat;
	mat.instantiate();
	mat->set_albedo(Color(0.35f, 0.37f, 0.33f));
	mesh->surface_set_material(0, mat);
	mi->set_mesh(mesh);
	mi->set_name("SoldierStandIn");
	add_child(mi);
	mi->set_global_position(feet + Vector3(0, 0.9f, 0));
	soldier_ = mi;
	soldier_normal_ = facing;
	soldier_point_ = feet + Vector3(0, 1.35f, 0) + soldier_normal_ * 0.3f;
	soldier_found_ = true;
}

bool Bf3Fx3Demo::build(std::string *r_error) {
	for (const std::string &n : loaded_->notes) {
		log(n);
	}
	for (const std::string &n : loaded_->fx.notes()) {
		log(n);
	}
	log(fmt("fx3 data loaded in %.2f s", loaded_->seconds));
	{
		const std::filesystem::path dir = utf8_path(shot_dir_);
		std::error_code ec;
		shot_dir_ok_ = session_->install().write_guard().allows_write(dir / "fx3_probe.png");
		if (shot_dir_ok_) {
			std::filesystem::create_directories(dir, ec);
			shot_dir_ok_ = !ec;
		}
		log(std::string("shot folder ") + shot_dir_ + (shot_dir_ok_ ? "" : " REFUSED: no shots"));
	}
	if (!loaded_->fx.build(this, r_error)) {
		return false;
	}
	for (size_t i = 0; i < loaded_->gunplay->defs.size(); ++i) {
		if (loaded_->gunplay->defs[i].variant == kRifle) {
			rifle_ = uint16_t(i);
		}
	}
	if (rifle_ == 0xFFFF || rifle_ >= loaded_->fx.fire_effects()->defs.size()) {
		*r_error = std::string("no ") + kRifle + " def";
		return false;
	}
	bullet_ = loaded_->fx.fire_effects()->defs[rifle_].bullet_material;
	const std::shared_ptr<const hk::SharedLevel> level = session_->shared_level();
	bf3::core::Result<std::unique_ptr<bf3::sim::CollisionWorld>> cw = bf3::sim::CollisionWorld::create(level->static_world);
	if (!cw) {
		*r_error = "CollisionWorld: " + cw.error().message;
		return false;
	}
	collision_ = std::move(*cw);
	const bf3::anim::CrowdStageSpec stage;
	const bf3::anim::StageCamera sc = bf3::anim::stage_camera(stage);
	main_position_ = gv(sc.position);
	main_target_ = gv(sc.target);

	for (const bf3::sim::SpawnPoint &sp : level->spawns) {
		const Vector3 eye(sp.position[0], sp.position[1] + 1.7f, sp.position[2]);
		const float yaw = float(sp.yaw) * (2.0f * 3.14159265f / 65536.0f);
		for (const float turn : { 0.0f, 1.5707963f, 3.14159265f, -1.5707963f }) {
			const Vector3 f(std::sin(yaw + turn), 0, std::cos(yaw + turn));
			bf3::sim::StaticHit g;
			if (collision_->ray(sv(eye), sv(f * 12.0f), bf3::sim::query_mask::kAll, nullptr) ||
					!collision_->ground_below(sv(eye + f * 4.0f), 4.0f, bf3::sim::query_mask::kCollision, &g)) {
				continue;
			}
			main_position_ = eye;
			main_target_ = eye + f * 10.0f - Vector3(0, 0.8f, 0);
			main_pose_line_ = fmt("main pose: spawn point %u (%.1f, %.1f, %.1f)", sp.index, eye.x, eye.y, eye.z);
			break;
		}
		if (!main_pose_line_.empty()) {
			break;
		}
	}
	notes_.push_back(main_pose_line_.empty() ? std::string("main pose: the crowd stage camera") : main_pose_line_);
	camera_ = memnew(Camera3D);
	camera_->set_name("Fx3Camera");
	camera_->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
	camera_->set_near(0.05f);
	camera_->set_far(4000.0f);
	camera_->set_fov(sc.fov_y_deg);
	add_child(camera_);
	set_pose(main_position_, main_target_);
	camera_->make_current();

	std::string dump = user_arg_value("fx3-dump-decals");
	if (!dump.empty() && !session_->install().write_guard().allows_write(utf8_path(dump) / "probe.png")) {
		log("--fx3-dump-decals refused (inside the install)");
		dump.clear();
	}
	for (const std::string &l : loaded_->fx.decal_texture_report(dump)) {
		log("decal texture " + l);
	}
	find_surfaces();
	place_soldier();
	const char *names[3] = { "concrete", "metal", "dirt" };
	const Surface *s[3] = { &concrete_, &metal_, &dirt_ };
	for (int i = 0; i < 3; ++i) {
		notes_.push_back(s[i]->found ? fmt("%s: \"%s\" (%u) at %.1f m, cell effect %s", names[i], s[i]->name.c_str(), unsigned(s[i]->material), s[i]->distance,
											   s[i]->effect.c_str())
									 : fmt("%s: no surface found", names[i]));
		log(notes_.back());
	}
	shots_ = {
		{ "fx3_muzzle_1p", 2.0, 0, 1.0 / 60.0 },
		{ "fx3_impact_concrete", 3.5, 1, 0.1 },
		{ "fx3_impact_metal", 5.0, 2, 0.1 },
		{ "fx3_impact_dirt", 6.5, 3, 0.1 },
		{ "fx3_decals_concrete", 9.0, 6, 0.0 },
		{ "fx3_hit_soldier", 10.5, 4, 0.08 },
		{ "fx3_tracers", 12.0, 5, 0.03 },
	};
	built_ = true;
	return true;
}

void Bf3Fx3Demo::own_shot(bool p_tracer) {
	const Transform3D t = camera_->get_global_transform();
	const Vector3 forward = -t.basis.get_column(2);
	hk::PresentEvent e;
	e.kind = hk::PresentKind::OwnShot;
	e.timeline = hk::PresentTimeline::Predicted;
	e.slot = kOwnSlot;
	e.def = rifle_;
	e.id = shot_id_++;
	e.flags = p_tracer ? hk::present_flags::kTracer : 0;
	e.pos[0] = float(t.origin.x), e.pos[1] = float(t.origin.y), e.pos[2] = float(t.origin.z);
	e.dir[0] = float(forward.x), e.dir[1] = float(forward.y), e.dir[2] = float(forward.z);
	e.seed = hk::effect_seed(kMatchSeed, kOwnSlot, e.id);
	e.extra = 1;
	events_.push_back(e);
}

void Bf3Fx3Demo::impact(const Surface &p_surface, int p_index) {

	const Vector3 n = p_surface.normal;
	Vector3 a = n.cross(Vector3(0, 1, 0));
	if (a.length() < 1e-3f) {
		a = n.cross(Vector3(1, 0, 0));
	}
	a = a.normalized();
	const Vector3 b = n.cross(a).normalized();
	const float ang = 2.39996f * float(p_index), r = kImpactSpread * std::sqrt(float(p_index) / float(kImpactsPerShot));
	const Vector3 p = p_surface.point + a * (r * std::cos(ang)) + b * (r * std::sin(ang));
	hk::PresentEvent e;
	e.kind = hk::PresentKind::Impact;
	e.timeline = hk::PresentTimeline::Predicted;
	e.slot = kOwnSlot;
	e.def = rifle_;
	e.id = shot_id_++;
	e.material = p_surface.material;
	e.pos[0] = float(p.x), e.pos[1] = float(p.y), e.pos[2] = float(p.z);
	e.dir[0] = float(n.x), e.dir[1] = float(n.y), e.dir[2] = float(n.z);
	e.seed = hk::impact_seed(hk::effect_seed(kMatchSeed, kOwnSlot, e.id), 0, 0);
	events_.push_back(e);
}

void Bf3Fx3Demo::soldier_hit() {
	hk::PresentEvent e;
	e.kind = hk::PresentKind::Hit;
	e.timeline = hk::PresentTimeline::Render;
	e.slot = kRemoteSlot;
	e.aux = kVictimSlot;
	e.def = rifle_;
	e.id = remote_id_++;
	e.material = kChestZone;
	e.value = 25.0f;
	e.extra = 7500;
	e.pos[0] = float(soldier_point_.x), e.pos[1] = float(soldier_point_.y), e.pos[2] = float(soldier_point_.z);
	e.dir[0] = float(soldier_normal_.x), e.dir[1] = float(soldier_normal_.y), e.dir[2] = float(soldier_normal_.z);
	events_.push_back(e);
}

void Bf3Fx3Demo::remote_burst() {

	const Vector3 fwd = ((main_target_ - main_position_) * Vector3(1, 0, 1)).normalized();
	const Vector3 right = fwd.cross(Vector3(0, 1, 0)).normalized();
	const Vector3 eye = main_position_ + right * 6.0f + fwd * 3.0f;
	for (int i = 0; i < 3; ++i) {
		const Vector3 aim = (main_position_ + fwd * 30.0f - right * (8.0f + 4.0f * float(i)) - eye).normalized();
		hk::PresentEvent e;
		e.kind = hk::PresentKind::RemoteShot;
		e.timeline = hk::PresentTimeline::Render;
		e.slot = kRemoteSlot;
		e.def = rifle_;
		e.id = remote_id_++;
		e.flags = hk::present_flags::kTracer;
		e.pos[0] = float(eye.x), e.pos[1] = float(eye.y), e.pos[2] = float(eye.z);
		e.dir[0] = float(aim.x), e.dir[1] = float(aim.y), e.dir[2] = float(aim.z);
		e.seed = hk::effect_seed(kMatchSeed, kRemoteSlot, e.id);
		e.extra = 1;
		events_.push_back(e);
	}
}

bool Bf3Fx3Demo::flash_present() const {
	const fx::FxWorld *w = loaded_->fx.world();
	for (const fx::EmitterOutput &o : w->outputs()) {
		if (o.first_person && o.particle_count > 0 && o.def != nullptr && o.emitter < o.def->emitters.size() &&
				o.def->emitters[o.emitter].name.find("_1p_flash") != std::string::npos) {
			return true;
		}
	}
	return false;
}

void Bf3Fx3Demo::step_fx(double p_dt) {
	const Transform3D t = camera_->get_global_transform();
	GameFrame f;
	const int64_t now = int64_t(t_ * double(bf3::sim::weapons::kTimeUnitsPerSecond));
	for (hk::PresentEvent &e : events_) {
		e.time = now;
	}
	f.clock.predicted_valid = f.clock.render_valid = true;
	f.clock.predicted = f.clock.render = now;
	f.dt = p_dt;
	f.events = events_;

	ViewmodelMuzzle vm;
	vm.valid = true;
	vm.source = ViewmodelMuzzleSource::CameraStandIn;
	vm.weapon = t;
	vm.muzzle = Transform3D(t.basis, t.xform(kMuzzleCameraStandIn));
	const fx::MuzzleSample own = muzzle_sample(vm, kOwnSlot, fx::Vec3{});
	f.muzzles = std::span<const fx::MuzzleSample>(&own, 1);
	f.camera = to_fx_transform(t);
	f.camera.forward = fv(-t.basis.get_column(2));
	f.camera.right = fv(-t.basis.get_column(0));
	f.own_alive = true;
	f.own_slot = kOwnSlot;
	f.own_team = 1;
	loaded_->fx.step(f);
	events_.clear();
	const GameFxStats s = loaded_->fx.stats();
	const Bf3FxRenderer *r = loaded_->fx.renderer();
	if (r != nullptr) {
		fx_ms_.push_back(r->stats().step_ms + r->stats().prepare_ms + r->stats().upload_ms);
		if (r->stats().drawn_instances != s.particles) {
			++mismatch_frames_;
		}
		fp_frames_ += r->prep().stats().first_person_instances > 0 ? 1 : 0;
	}
	max_decals_ = std::max(max_decals_, s.decals_live);
	++frames_;
}

void Bf3Fx3Demo::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || session_ == nullptr || shut_down_ || finished_) {
		return;
	}
	if (!session_->done()) {
		return;
	}
	if (!started_) {
		start_loader();
		return;
	}
	if (!built_) {
		if (!loaded_->done.load(std::memory_order_acquire)) {
			return;
		}
		if (loader_.joinable()) {
			loader_.join();
		}
		std::string error = loaded_->error;
		if (error.empty() && !build(&error) && error.empty()) {
			error = "build failed";
		}
		if (!error.empty()) {
			log("FAILED: " + error);
			UtilityFunctions::push_error(to_godot("[fx3] " + error));
			finish(1);
		}
		return;
	}
	if (frozen_) {

		step_fx(0.0);
		if (--settle_ > 0) {
			return;
		}
		const Shot &s = shots_[shot_index_];
		const std::string path = path_text(utf8_path(shot_dir_) / (s.name + ".png"));
		const bool saved = shot_dir_ok_ && session_->save_screenshot(path);
		std::error_code ec;
		const uint64_t bytes = saved ? uint64_t(std::filesystem::file_size(utf8_path(path), ec)) : 0;
		const bool ok = saved && !ec && bytes >= kMinShotBytes;
		shots_ok_ += ok ? 1 : 0;
		const GameFxStats st = loaded_->fx.stats();
		shot_lines_.push_back(fmt("%s %s (%llu bytes; %u particles, %u lights, %u decals live%s)", s.name.c_str(), ok ? "saved" : "FAILED", (unsigned long long)bytes,
				st.particles, st.lights, st.decals_live, s.kind == 0 ? (flash_present() ? fmt(", 1p flash present after %d re-fires", flash_retries_).c_str() : ", 1p flash ABSENT") : ""));
		frozen_ = false;
		shot_active_ = false;
		++shot_index_;
		set_pose(main_position_, main_target_);
		return;
	}
	const double dt = std::clamp(p_delta, 0.0, 0.1);
	t_ += dt;
	if (check_ && !shot_active_ && shot_index_ < shots_.size() && t_ >= shots_[shot_index_].at) {
		const Shot &s = shots_[shot_index_];
		shot_active_ = true;
		shot_left_ = s.advance;
		const Surface *surface = s.kind == 1 ? &concrete_ : (s.kind == 2 ? &metal_ : (s.kind == 3 ? &dirt_ : nullptr));
		switch (s.kind) {
			case 0:
				set_pose(main_position_, main_target_);
				own_shot(false);
				break;
			case 1:
			case 2:
			case 3:
				if (surface->found) {
					set_pose(surface->point + surface->normal * kViewDistance + Vector3(0, 0.25f, 0), surface->point);
					for (int i = 0; i < kImpactsPerShot; ++i) {
						impact(*surface, i);
					}
				}
				break;
			case 6:
				if (concrete_.found) {
					set_pose(concrete_.point + concrete_.normal * kViewDistance + Vector3(0, 0.25f, 0), concrete_.point);
				}
				break;
			case 4:
				if (soldier_found_) {
					set_pose(soldier_point_ + soldier_normal_ * 2.2f + Vector3(0, 0.1f, 0), soldier_point_);
					soldier_hit();
				}
				break;
			default:
				set_pose(main_position_, main_target_);
				remote_burst();
				break;
		}
	}

	if (!shot_active_ && t_ >= next_shot_) {
		own_shot(shot_id_ % 3 == 0);
		next_shot_ = t_ + kShotPeriod;
	}
	step_fx(dt);
	if (shot_active_) {
		shot_left_ -= dt;
		const Shot &s = shots_[shot_index_];
		if (shot_left_ <= 0.0 && s.kind == 0 && !flash_present() && flash_retries_ < kFlashRetries) {
			++flash_retries_;
			own_shot(false);
			shot_left_ = s.advance;
		} else if (shot_left_ <= 0.0) {
			frozen_ = true;
			settle_ = kSettleFrames;
		}
	}
	if (check_ && t_ >= kRunSeconds && shot_index_ >= shots_.size()) {
		report_and_finish();
	}
}

void Bf3Fx3Demo::report_and_finish() {
	std::vector<std::string> report;
	auto row = [&report](const char *p_gate, bool p_ok, const std::string &p_text) {
		report.push_back(fmt("%-10s %s  %s", p_gate, p_ok ? "PASS" : "FAIL", p_text.c_str()));
		return p_ok;
	};
	const GameFxStats s = loaded_->fx.stats();
	const fx::FxTriggerStats &t = s.triggers;
	bool all = true;
	all &= row("load", s.preload_failures == 0 && s.preloaded > 0,
			fmt("%u blueprints preloaded, %u failed; atlas %u entries, %u outside the upload; %.2f s", s.preloaded, s.preload_failures, s.atlas_entries,
					s.atlas_fallbacks, s.load_seconds));
	all &= row("draw", mismatch_frames_ == 0 && frames_ > 0,
			fmt("drawn instances == FxWorld::particles() in %llu of %llu frames; %llu frames with 1p instances", (unsigned long long)(frames_ - mismatch_frames_),
					(unsigned long long)frames_, (unsigned long long)fp_frames_));

	bool shell_3p = false;
	for (const fx::FireEffect &e : loaded_->fx.fire_effects()->defs[rifle_].third_person) {
		shell_3p |= e.shell;
	}
	const uint64_t shells_expected = t.own_shots + (shell_3p ? t.remote_shots : 0);
	all &= row("shots", t.own_shots > 0 && t.muzzle_starts > 0 && t.shells >= t.own_shots && t.shells <= shells_expected && t.double_starts == 0,
			fmt("%llu own + %llu remote shots (3p list %s a shell): %llu muzzle starts, %llu shells, %llu tracers (%llu tracer shots), %llu aim fallbacks, %llu culled starts, %llu double starts",
					(unsigned long long)t.own_shots, (unsigned long long)t.remote_shots, shell_3p ? "with" : "without", (unsigned long long)t.muzzle_starts, (unsigned long long)t.shells,
					(unsigned long long)t.tracers, (unsigned long long)t.tracer_shots, (unsigned long long)t.muzzle_fallbacks, (unsigned long long)t.culled, (unsigned long long)t.double_starts));
	all &= row("impacts", concrete_.found && metal_.found && dirt_.found && t.impacts > 0,
			fmt("%llu cells started, %llu culled, %llu empty; %llu impact sound cues", (unsigned long long)t.impacts, (unsigned long long)t.impacts_culled,
					(unsigned long long)t.impacts_empty, (unsigned long long)t.sounds));
	all &= row("decals", s.decals_placed > 0 && max_decals_ <= fx::kMaxDecals,
			fmt("%llu placed (max %u live), %llu replaced, %llu without a template, %llu untextured; %u templates (%u untextured), %u textures",
					(unsigned long long)s.decals_placed, max_decals_, (unsigned long long)s.decals_replaced, (unsigned long long)s.decals_no_template,
					(unsigned long long)s.decals_untextured, s.decal_templates, s.decal_templates_untextured, s.decal_textures));
	all &= row("hit", soldier_found_ && t.soldier_hits > 0, fmt("%llu soldier cells (Hit, chest)", (unsigned long long)t.soldier_hits));
	row("FX time", true,
			fmt("Bf3GameFx main thread (step + prepare + upload, wall) p50 %.3f ms, p99 %.3f ms over %zu frames "
				"(branch, unlocked, shared machine: indicative only)",
					rounded_rank(fx_ms_, 0.5), rounded_rank(fx_ms_, 0.99), fx_ms_.size()));
	all &= row("captures", shots_ok_ == shots_.size(), fmt("%u of %zu in %s (the look is the user's judgement)", shots_ok_, shots_.size(), shot_dir_.c_str()));
	for (const std::string &l : shot_lines_) {
		report.push_back("           " + l);
	}
	for (const std::string &n : notes_) {
		report.push_back("           " + n);
	}
	const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(session_->install_snapshot_at_start(), session_->install().snapshot());
	all &= row("install", d.clean(), d.clean() ? "unchanged (layout.toc, cas.cat, every .sb/.toc: sizes and mtimes)" : d.summary());
	log("---- FX3 DEMO CHECK ----");
	for (const std::string &l : report) {
		log(l);
	}
	log(std::string("FX3 DEMO: ") + (all ? "PASS" : "FAIL"));
	finish(all ? 0 : 1);
}

void Bf3Fx3Demo::finish(int p_code) {
	if (finished_) {
		return;
	}
	finished_ = true;
	shutdown();
	session_->finish(p_code);
}

void Bf3Fx3Demo::shutdown() {
	if (shut_down_) {
		return;
	}
	shut_down_ = true;
	stop_.store(true);
	if (loader_.joinable()) {
		loader_.join();
	}
	if (loaded_) {
		loaded_->fx.shutdown();
	}
	collision_.reset();
}

}

void openbf3_register_m2a_fx3() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3Fx3Demo);
}
