#include "fx_demo.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"

#include "bf3/anim/crowd_stage.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/percentile.h"
#include "bf3/core/util/text.h"
#include "bf3/fx/atlas_pixels.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/sim/collision_world.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
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
#include <optional>

namespace bf3::godot_side {

using namespace godot;
namespace fx = bf3::fx;

namespace {

constexpr const char *kOilfire = "fx/ambient/levelspecific/mp_12/emitters_oilfire/em_amb_mp12_oilfire_l_fire_01";
constexpr const char *kMuzzle1p = "fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_1p";
constexpr const char *kFillout1p = "fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_fillout_1p";
constexpr const char *kShell = "fx/weapons/shells/fx_shellejection_m";
constexpr const char *kSmoke = "fx/ambient/generic/firesmoke/smoke/smokepillars/generic/fx_dark_smoke_01_m";

const Vector3 kMuzzleCamera(0.10f, -0.11f, -0.70f);

constexpr float kShellBack = 0.55f * 0.70f / 0.99f;

constexpr float kSurfaceRay = 60.0f, kSurfaceMin = 2.0f, kSurfaceMax = 45.0f;
constexpr double kMuzzlePeriod = 0.1;
constexpr double kImpactPeriod = 0.6;
constexpr double kStressAt = 16.0;
constexpr double kRunSeconds = 21.0;
constexpr double kOilGateAge = 15.0;
constexpr int kOilTarget = 80, kOilTolerance = 1;
constexpr uint32_t kStressEffects = 96;
constexpr int kSettleFrames = 4;
constexpr int kFlashRetries = 16;
constexpr double kSmokeStartAt = 7.5;
constexpr uint64_t kMinShotBytes = 50 * 1024;

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[fx] " + p_line));
}

using bf3::core::fmt;

std::filesystem::path utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string path_text(const std::filesystem::path &p_path) {
	const std::u8string s = p_path.generic_u8string();
	return std::string(s.begin(), s.end());
}

using bf3::core::rounded_rank;

const char *pass_text(bool p_ok) {
	return p_ok ? "PASS" : "FAIL";
}

fx::Vec3 fv(const Vector3 &p_v) {
	return fx::Vec3{ p_v.x, p_v.y, p_v.z };
}

Vector3 gv(const bf3::anim::Vec3 &p_v) {
	return Vector3(p_v.x, p_v.y, p_v.z);
}

using bf3::core::to_lower_ascii;

fx::Transform frame_y(const Vector3 &p_origin, const Vector3 &p_axis, const Vector3 &p_forward_hint) {
	const Vector3 u = p_axis.normalized();
	Vector3 f = p_forward_hint - u * p_forward_hint.dot(u);
	if (f.length() < 1e-4f) {
		f = Vector3(0, 0, 1) - u * u.z;
		if (f.length() < 1e-4f) {
			f = Vector3(1, 0, 0) - u * u.x;
		}
	}
	f = f.normalized();
	const Vector3 r = u.cross(f).normalized();
	fx::Transform t;
	t.right = fv(r);
	t.up = fv(u);
	t.forward = fv(f);
	t.trans = fv(p_origin);
	return t;
}

fx::Transform frame_of(const Vector3 &p_origin, const Vector3 &p_forward, const Vector3 &p_up_hint) {
	Vector3 f = p_forward.normalized();
	Vector3 r = p_up_hint.cross(f);
	if (r.length() < 1e-4f) {
		r = Vector3(0, 0, 1).cross(f);
	}
	r = r.normalized();
	const Vector3 u = f.cross(r).normalized();
	fx::Transform t;
	t.right = fv(r);
	t.up = fv(u);
	t.forward = fv(f);
	t.trans = fv(p_origin);
	return t;
}

std::shared_ptr<const fx::EffectDef> light_stress_def() {
	fx::EmitterDef e;
	e.name = "fx2/synthetic/light_stress";
	e.tmpl.max_count = 4;
	e.tmpl.lifetime = 0.01f;
	e.tmpl.flags = fx::kEmitterEmissive | fx::kEmitterActAsPointLight;
	e.tmpl.light.radius = 6.0f;
	e.tmpl.light.color = fx::Vec3{ 3.0f, 1.6f, 0.6f };
	e.slots[size_t(fx::ProcessorType::SpawnRate)].present = true;
	e.params.spawn_rate = 301.0f;
	e.params.distribute_over_time = false;
	e.slots[size_t(fx::ProcessorType::UpdateAge)].present = true;
	e.params.particle_lifetime = 3.0f;
	e.slots[size_t(fx::ProcessorType::UpdateLinearVelocity)].present = true;
	e.texture.texture = "fx/textures/muzzleflash/muzzleflash_01";
	e.texture.frame_count = 16;
	e.texture.frame_columns = 4;
	auto d = std::make_shared<fx::EffectDef>();
	d->name = "fx2/synthetic/light_stress";
	d->max_instance_count = uint16_t(kStressEffects);
	d->reset_instance_when_started = true;
	d->emitters.push_back(std::move(e));
	fx::EffectComponentDef c;
	c.kind = fx::ComponentKind::Emitter;
	c.emitter = 0;
	d->components.push_back(c);
	d->max_particles = 4;
	return d;
}

}

struct Bf3FxDemo::Loaded {
	std::shared_ptr<bf3::core::Cache> cache;
	std::shared_ptr<bf3::core::Install> install;
	std::unique_ptr<bf3::core::gameplay::WeaponSources> sources;
	std::optional<fx::EmitterAtlas> atlas;
	std::optional<fx::ImpactTable> impacts;
	fx::AtlasPixels pixels;
	std::unique_ptr<fx::EffectLibrary> library;
	std::shared_ptr<const fx::EffectDef> oilfire, muzzle, fillout, shell, smoke, stress;
	std::optional<uint8_t> bullet;
	std::string error;
	std::vector<std::string> notes;
	double seconds = 0;
	std::atomic<bool> done{ false };
};

Bf3FxDemo::Bf3FxDemo() = default;

Bf3FxDemo::~Bf3FxDemo() {
	shutdown();
}

void Bf3FxDemo::_bind_methods() {}

void Bf3FxDemo::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		shutdown();
	}
}

void Bf3FxDemo::parse_args() {
	check_ = has_user_arg("fx-check");
	no_cache_ = has_user_arg("no-cache");
	shot_dir_ = user_arg_value("fx-shot-dir");
	{
		const std::string dv = user_arg_value("fx-debug-view");
		debug_view_ = dv.empty() ? 0 : std::clamp(std::atoi(dv.c_str()), 0, 3);
	}
	if (shot_dir_.empty()) {
		const std::filesystem::path project = utf8_path(to_std(ProjectSettings::get_singleton()->globalize_path("res://")));
		shot_dir_ = path_text((project / "..").lexically_normal() / "b" / "shots" / "fx2");
	}
	if (!no_cache_) {
		const std::string root = user_arg_value("bf3-cache");
		cache_root_ = root.empty() ? default_cache_root() : utf8_path(root);
	}
}

void Bf3FxDemo::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	parse_args();
	session_ = Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
	if (session_ == nullptr) {
		UtilityFunctions::push_error("[fx] Bf3FxDemo needs a sibling Bf3LevelSession named Session");
		return;
	}
	log(std::string("fx demo: ") + (check_ ? "--fx-check" : "interactive") + ", shots " + shot_dir_);
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

void Bf3FxDemo::start_loader() {
	started_ = true;
	const bf3::core::InstallReader &reader = session_->install();
	const bf3::core::PathGuard guard = reader.write_guard();
	const std::filesystem::path root = cache_root_;
	loaded_ = std::make_unique<Loaded>();
	Loaded *out = loaded_.get();
	loader_ = std::thread([out, &reader, guard, root]() {
		auto body = [&]() {
			const auto t0 = std::chrono::steady_clock::now();
			auto fail = [out](std::string p_error) {
				out->error = std::move(p_error);
			};
			if (!root.empty()) {
				AcquiredCache acquired = acquire_cache(root, guard);
				if (acquired.status == bf3::core::CacheStatus::Ok) {
					out->cache = acquired.cache;
					out->notes.push_back("cache " + path_text(root) + (acquired.shared ? " shared" : " opened"));
				} else {
					out->notes.push_back("cache " + path_text(root) + " not used (" + bf3::core::cache_status_name(acquired.status) + ")");
				}
			} else {
				out->notes.push_back("cache disabled (--no-cache)");
			}
			bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(reader);
			if (!install) {
				return fail("install: " + install.error().message);
			}
			out->install = *install;
			bf3::core::Result<bf3::core::gameplay::WeaponSources> src = bf3::core::gameplay::open_weapon_sources(out->install, "Win32/Levels/MP_Subway/MP_Subway");
			if (!src) {
				return fail("Metro: " + src.error().message);
			}
			out->sources = std::make_unique<bf3::core::gameplay::WeaponSources>(std::move(*src));
			bf3::core::EbxLibrary &lib = *out->sources->level;
			lib.load_all();
			bf3::core::gameplay::WorkBudget budget{ fx::kDefaultCompileWorkLimit };
			bf3::core::Result<fx::EmitterAtlas> atlas = fx::EmitterAtlas::load(lib, lib.mount(0), fx::EmitterAtlas::emitter_system_of("MP_Subway"), budget);
			if (!atlas) {
				return fail("atlas: " + atlas.error().message);
			}
			out->atlas.emplace(std::move(*atlas));
			bf3::core::gameplay::WorkBudget pixel_budget{ fx::kDefaultCompileWorkLimit };
			bf3::core::Result<fx::AtlasPixels> pixels = fx::prepare_atlas_pixels(*out->atlas, lib.mount(0), out->cache.get(), pixel_budget);
			if (!pixels) {
				return fail("atlas pixels: " + pixels.error().message);
			}
			out->pixels = std::move(*pixels);
			out->notes.push_back(fmt("atlas pixels %s in %.2f s (base %u x %u, %zu mips, %.1f MiB; normal %u x %u, %zu mips, %.1f MiB)",
					out->pixels.from_cache ? "from the cache" : "converted", out->pixels.seconds, out->pixels.base.width, out->pixels.base.height,
					out->pixels.base.mips.size(), double(out->pixels.base.bytes.size()) / 1048576.0, out->pixels.normal.width, out->pixels.normal.height,
					out->pixels.normal.mips.size(), double(out->pixels.normal.bytes.size()) / 1048576.0));
			const std::optional<std::string> grid = bf3::core::gameplay::MaterialGrid::grid_of_level(lib, out->sources->level_partition);
			if (!grid) {
				return fail("no material grid for " + out->sources->level_partition);
			}
			bf3::core::gameplay::WorkBudget grid_budget{ fx::kDefaultCompileWorkLimit };
			bf3::core::Result<fx::ImpactTable> impacts = fx::ImpactTable::load(lib, *grid, grid_budget);
			if (!impacts) {
				return fail("impact table: " + impacts.error().message);
			}
			out->impacts.emplace(std::move(*impacts));
			out->bullet = out->impacts->material_by_name("Bullet");
			fx::EffectLibrary::Sources s;
			s.fx.db = &lib;
			s.fx.digest = bf3::core::gameplay::library_partition_digest(lib);
			s.cache = out->cache.get();
			s.atlas = &*out->atlas;
			s.impacts = &*out->impacts;
			out->library = std::make_unique<fx::EffectLibrary>(s);
			for (auto [name, slot] : { std::pair{ kMuzzle1p, &out->muzzle }, std::pair{ kFillout1p, &out->fillout }, std::pair{ kShell, &out->shell },
						 std::pair{ kSmoke, &out->smoke } }) {
				bf3::core::Result<std::shared_ptr<const fx::EffectDef>> d = out->library->get(name);
				if (!d) {
					return fail(std::string(name) + ": " + d.error().message);
				}
				*slot = *d;
			}

			{
				bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> mp12 = bf3::core::EbxLibrary::open(out->install, { "Win32/Levels/MP_012/MP_012" });
				if (!mp12) {
					return fail("MP_012: " + mp12.error().message);
				}
				(*mp12)->load_all();
				fx::CompileContext ctx;
				ctx.db = mp12->get();
				bf3::core::Result<fx::EffectDef> d = fx::compile_emitter_effect(kOilfire, ctx);
				if (!d) {
					return fail(std::string(kOilfire) + ": " + d.error().message);
				}
				out->oilfire = std::make_shared<const fx::EffectDef>(std::move(*d));
			}
			out->stress = light_stress_def();
			out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		};
		body();
		out->done.store(true, std::memory_order_release);
	});
}

void Bf3FxDemo::set_pose(const Pose &p_pose) {
	const Vector3 d = (p_pose.target - p_pose.position).normalized();
	camera_->look_at_from_position(p_pose.position, p_pose.target, std::fabs(d.y) > 0.999f ? Vector3(0, 0, 1) : Vector3(0, 1, 0));
}

fx::FxView Bf3FxDemo::view() const {
	const Transform3D t = camera_->get_global_transform();
	fx::FxView v;
	v.camera_position = fv(t.origin);
	v.camera_forward = fv(-t.basis.get_column(2));
	return v;
}

fx::Transform Bf3FxDemo::muzzle_transform() const {
	const Transform3D t = camera_->get_global_transform();
	const Vector3 forward = -t.basis.get_column(2);
	return frame_y(t.xform(kMuzzleCamera), forward, t.basis.get_column(1));
}

void Bf3FxDemo::start_effect(const std::shared_ptr<const fx::EffectDef> &p_def, const fx::Transform &p_t, bool p_first_person) {
	fx::EffectStart s;
	s.transform = p_t;
	s.seed = 0x9E3779B97F4A7C15ull * (frames_ + 1) + muzzle_starts_ * 31 + impact_starts_ * 7;
	s.first_person = p_first_person;
	bf3::core::Result<fx::EffectHandle> h = world_->start(p_def, s);
	if (!h) {
		notes_.push_back("start refused: " + p_def->name + ": " + h.error().message);
	}
}

void Bf3FxDemo::fire_muzzle() {
	const fx::Transform m = muzzle_transform();
	start_effect(loaded_->muzzle, m, true);
	start_effect(loaded_->fillout, m, true);
	fx::Transform shell = m;
	shell.trans = fx::Vec3{ m.trans.x - m.up.x * kShellBack, m.trans.y - m.up.y * kShellBack + 0.02f, m.trans.z - m.up.z * kShellBack };
	start_effect(loaded_->shell, shell, true);
	++muzzle_starts_;
}

void Bf3FxDemo::start_impact(int p_which) {
	const bool concrete = p_which == 0;
	if (concrete ? !concrete_found_ : !brick_found_) {
		return;
	}
	const Vector3 p = concrete ? concrete_point_ : brick_point_;
	const Vector3 n = concrete ? concrete_normal_ : brick_normal_;
	const std::shared_ptr<const fx::EffectDef> &def = concrete ? impact_concrete_ : impact_brick_;
	if (!def) {
		return;
	}

	start_effect(def, frame_y(p + n * 0.01f, n, Vector3(0, 1, 0)), false);
	++impact_starts_;
}

bool Bf3FxDemo::flash_present(std::string *r_where) const {
	for (const fx::EmitterOutput &o : world_->outputs()) {
		if (o.first_person && o.particle_count > 0 && o.def != nullptr && o.emitter < o.def->emitters.size() &&
				o.def->emitters[o.emitter].name.find("_1p_flash") != std::string::npos) {
			if (r_where != nullptr && o.first_particle < world_->particles().size()) {
				const fx::ParticleVertex &v = world_->particles()[o.first_particle];
				const Vector3 c = camera_->get_global_transform().affine_inverse().xform(Vector3(v.position.x, v.position.y, v.position.z));
				*r_where = fmt(" at camera (%.2f, %.2f, %.2f) m, size %.2f, colour (%.0f, %.0f, %.0f) a %.2f", c.x, c.y, c.z, v.size.x, v.color.x, v.color.y,
						v.color.z, v.color.w);
			}
			return true;
		}
	}
	return false;
}

std::string Bf3FxDemo::flash_line() const {
	std::string where;
	const bool present = flash_present(&where);
	return fmt("; flash %s after %d re-fires%s", present ? "present" : "ABSENT", shot_retries_, where.c_str());
}

void Bf3FxDemo::frame_oilfire() {
	Vector3 sum;
	uint32_t n = 0;
	const std::span<const fx::ParticleVertex> ps = world_->particles();
	for (const fx::EmitterOutput &o : world_->outputs()) {
		if (o.def != loaded_->oilfire.get()) {
			continue;
		}
		for (uint32_t i = 0; i < o.particle_count && o.first_particle + i < ps.size(); ++i) {
			const fx::Vec3 &q = ps[o.first_particle + i].position;
			sum += Vector3(q.x, q.y, q.z);
			++n;
		}
	}
	if (n == 0) {
		return;
	}
	const Vector3 c = sum / float(n);
	float r = 0;
	for (const fx::EmitterOutput &o : world_->outputs()) {
		if (o.def != loaded_->oilfire.get()) {
			continue;
		}
		for (uint32_t i = 0; i < o.particle_count && o.first_particle + i < ps.size(); ++i) {
			const fx::Vec3 &q = ps[o.first_particle + i].position;
			r = std::max(r, (Vector3(q.x, q.y, q.z) - c).length());
		}
	}
	const Vector3 fwd = ((main_pose_.target - main_pose_.position) * Vector3(1, 0, 1)).normalized();
	Pose pose;
	pose.position = c - fwd * std::clamp(2.0f * r, 60.0f, 1500.0f) + Vector3(0, 5.0f, 0);
	pose.target = c;
	set_pose(pose);
	oil_framed_ = true;
	oil_frame_line_ = fmt("oilfire framed: %u particles, centre %.0f m above the start, radius %.0f m", n, c.y - oil_point_.y, r);
}

void Bf3FxDemo::start_light_stress() {
	stress_started_ = true;
	const Vector3 f = (main_pose_.target - main_pose_.position) * Vector3(1, 0, 1);
	const Vector3 fwd = f.normalized();
	const Vector3 right = fwd.cross(Vector3(0, 1, 0)).normalized();
	const Vector3 base = main_pose_.position + Vector3(0, -0.4f, 0);
	for (uint32_t i = 0; i < kStressEffects; ++i) {
		const float col = float(i % 12) - 5.5f, row = float(i / 12);
		const Vector3 p = base + fwd * (4.0f + row * 2.0f) + right * (col * 1.5f);
		start_effect(loaded_->stress, frame_of(p, fwd, Vector3(0, 1, 0)), false);
	}
}

void Bf3FxDemo::find_surfaces() {
	const bf3::sim::CollisionWorld &cw = *world_collision_;
	const fx::ImpactTable &table = *loaded_->impacts;
	const Vector3 eye = main_pose_.position;
	const Vector3 f0 = ((main_pose_.target - main_pose_.position) * Vector3(1, 0, 1)).normalized();
	const uint32_t mask = bf3::sim::query_mask::kProjectile;
	struct Best {
		bool found = false;
		float dist = 1e9f;
		Vector3 p, n;
		std::string material;
		std::string effect;
	};
	Best concrete, brick;
	std::map<std::string, int> seen;
	for (int ring = 0; ring < 2 && !(concrete.found && brick.found); ++ring) {
		const int yaws = ring == 0 ? 49 : 145;
		const float yaw_span = ring == 0 ? 1.2f : 3.14159265f;
		for (int yi = 0; yi < yaws; ++yi) {
			const float yaw = -yaw_span + 2.0f * yaw_span * float(yi) / float(yaws - 1);
			for (const float pitch : { -0.35f, -0.2f, -0.08f, 0.0f, 0.1f, 0.25f, 0.4f, 0.55f }) {
				const Vector3 d = f0.rotated(Vector3(0, 1, 0), yaw);
				const Vector3 dir = (d * std::cos(pitch) + Vector3(0, std::sin(pitch), 0)).normalized() * kSurfaceRay;
				bf3::sim::StaticHit hit;
				if (!cw.ray(bf3::sim::Vec3{ eye.x, eye.y, eye.z }, bf3::sim::Vec3{ dir.x, dir.y, dir.z }, mask, &hit)) {
					continue;
				}
				const std::optional<uint8_t> surface = table.surface_material(hit.tag.material & 0xFFFFFFu);
				if (!surface) {
					continue;
				}
				const std::string name = to_lower_ascii(table.material_name(*surface));
				++seen[name];
				const float dist = hit.fraction * kSurfaceRay;
				if (dist < kSurfaceMin || dist > kSurfaceMax) {
					continue;
				}
				Best *b = name.find("concrete") != std::string::npos ? &concrete : (name.find("brick") != std::string::npos ? &brick : nullptr);
				if (b == nullptr || b->found && std::fabs(dist - 8.0f) >= std::fabs(b->dist - 8.0f)) {
					continue;
				}
				const fx::ImpactCell *cell = loaded_->bullet ? table.select(*loaded_->bullet, *surface) : nullptr;
				if (cell == nullptr || cell->effects.empty()) {
					continue;
				}
				b->found = true;
				b->dist = dist;
				b->p = Vector3(hit.position[0], hit.position[1], hit.position[2]);
				b->n = Vector3(hit.normal[0], hit.normal[1], hit.normal[2]).normalized();
				b->material = std::string(table.material_name(*surface));
				b->effect = cell->effects.front();
			}
		}
	}
	std::string seen_line = "surfaces hit by the ray fan:";
	for (const auto &[n, c] : seen) {
		seen_line += " " + n + " x" + std::to_string(c);
	}
	notes_.push_back(seen_line);
	for (int k = 0; k < 2; ++k) {
		Best &b = k == 0 ? concrete : brick;
		std::string &line = k == 0 ? concrete_line_ : brick_line_;
		std::shared_ptr<const fx::EffectDef> &def = k == 0 ? impact_concrete_ : impact_brick_;
		if (!b.found) {
			line = std::string(k == 0 ? "concrete" : "brick") + fmt(": no surface within %.0f..%.0f m of the stage camera", kSurfaceMin, kSurfaceMax);
			continue;
		}
		bf3::core::Result<std::shared_ptr<const fx::EffectDef>> d = loaded_->library->get(b.effect);
		if (!d) {
			line = b.effect + ": " + d.error().message;
			b.found = false;
			continue;
		}
		def = *d;
		line = fmt("%s (material \"%s\") at %.1f m: %s", k == 0 ? "concrete" : "brick", b.material.c_str(), b.dist, b.effect.c_str());
		(k == 0 ? concrete_point_ : brick_point_) = b.p;
		(k == 0 ? concrete_normal_ : brick_normal_) = b.n;
		(k == 0 ? concrete_found_ : brick_found_) = true;
	}
}

bool Bf3FxDemo::build(std::string *r_error) {
	for (const std::string &n : loaded_->notes) {
		log(n);
	}
	log(fmt("fx data loaded in %.2f s", loaded_->seconds));
	{
		const std::filesystem::path dir = utf8_path(shot_dir_);
		std::error_code ec;
		shot_dir_ok_ = session_->install().write_guard().allows_write(dir / "fx2_probe.png");
		if (shot_dir_ok_) {
			std::filesystem::create_directories(dir, ec);
			shot_dir_ok_ = !ec;
		}
		log(std::string("shot folder ") + shot_dir_ + (shot_dir_ok_ ? "" : " REFUSED (inside the install or not creatable): no shots"));
	}
	renderer_ = memnew(Bf3FxRenderer);
	renderer_->set_name("FxRenderer");
	renderer_->set_debug_view(debug_view_);
	add_child(renderer_);
	std::string err;
	if (!renderer_->upload_atlas(loaded_->pixels, &err)) {
		*r_error = err;
		return false;
	}
	const fx::EmitterAtlas &atlas = *loaded_->atlas;
	atlas_entries_ = uint32_t(atlas.entries().size());
	for (const fx::AtlasEntry &e : atlas.entries()) {
		const bool inside = std::min(e.min_uv.x, e.max_uv.x) >= 0.0f && std::max(e.min_uv.x, e.max_uv.x) <= 1.0f && std::min(e.min_uv.y, e.max_uv.y) >= 0.0f &&
				std::max(e.min_uv.y, e.max_uv.y) <= 1.0f;
		if (inside && renderer_->stats().atlas_ok) {
			++atlas_uploaded_;
		}
	}
	atlas_line_ = fmt("base %s %u x %u (%u mips, BC3 sRGB), normal %s %u x %u (%u mips, BC5)", atlas.base().partition.c_str(), atlas.base().width,
			atlas.base().height, renderer_->stats().atlas_mips_base, atlas.normal().partition.c_str(), atlas.normal().width, atlas.normal().height,
			renderer_->stats().atlas_mips_normal);
	log("atlas uploaded: " + atlas_line_);
	const std::shared_ptr<const bf3::host::SharedLevel> level = session_->shared_level();
	if (!level || !level->static_world) {
		*r_error = "no static world";
		return false;
	}
	bf3::core::Result<std::unique_ptr<bf3::sim::CollisionWorld>> cw = bf3::sim::CollisionWorld::create(level->static_world);
	if (!cw) {
		*r_error = "CollisionWorld: " + cw.error().message;
		return false;
	}
	world_collision_ = std::move(*cw);

	const bf3::anim::CrowdStageSpec stage;
	const bf3::anim::StageCamera sc = bf3::anim::stage_camera(stage);
	main_pose_.position = gv(sc.position);
	main_pose_.target = gv(sc.target);
	camera_ = memnew(Camera3D);
	camera_->set_name("FxCamera");
	camera_->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
	camera_->set_near(0.05f);
	camera_->set_far(4000.0f);
	camera_->set_fov(sc.fov_y_deg);
	add_child(camera_);
	set_pose(main_pose_);
	camera_->make_current();

	find_surfaces();
	log(concrete_line_);
	log(brick_line_);
	const Vector3 fwd = ((main_pose_.target - main_pose_.position) * Vector3(1, 0, 1)).normalized();
	{
		const Vector3 above = main_pose_.position + fwd * 10.0f + Vector3(0, 2.0f, 0);
		bf3::sim::StaticHit hit;
		smoke_point_ = above - Vector3(0, 3.6f, 0);
		if (world_collision_->ground_below(bf3::sim::Vec3{ above.x, above.y, above.z }, 10.0f, bf3::sim::query_mask::kCollision, &hit)) {
			smoke_point_ = Vector3(hit.position[0], hit.position[1], hit.position[2]);
		}
	}

	oil_pose_.position = main_pose_.position + Vector3(0, 120.0f, 0);
	oil_point_ = oil_pose_.position + fwd * 200.0f - Vector3(0, 30.0f, 0);
	oil_pose_.target = oil_point_ + Vector3(0, 30.0f, 0);

	fx::FxWorldConfig cfg;
	world_ = std::make_unique<fx::FxWorld>(cfg);
	start_effect(loaded_->oilfire, frame_of(oil_point_, fwd, Vector3(0, 1, 0)), false);

	shots_ = {
		{ "fx2_muzzle_1p", 3.0, 0, 1.0 / 60.0 },
		{ "fx2_impact_concrete", 5.0, 1, 0.12 },
		{ "fx2_impact_brick", 7.0, 2, 0.12 },
		{ "fx2_smoke", 13.0, 3, 0.25 },
		{ "fx2_oilfire", 15.1, 4, 0.25 },
		{ "fx2_lights", 17.0, 5, 0.0 },
	};
	RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
	built_ = true;
	return true;
}

void Bf3FxDemo::frame_checks() {
	const fx::RenderPrepStats &ps = renderer_->prep().stats();
	const FxRendererStats &rs = renderer_->stats();
	const size_t particles = world_->particles().size();
	if (rs.drawn_instances != particles || ps.instances != particles) {
		++count_mismatch_frames_;
	}
	uint32_t fp_expected = 0;
	for (const fx::EmitterOutput &o : world_->outputs()) {
		if (o.first_person) {
			fp_expected += o.particle_count;
		}
	}
	uint32_t fp_in_fp_batches = 0;
	for (const fx::FxBatch &b : renderer_->prep().batches()) {
		if (b.key.pass == fx::FxPass::FirstPerson) {
			fp_in_fp_batches += b.instance_count;
		}
	}
	if (fp_expected != ps.first_person_instances || fp_in_fp_batches != fp_expected) {
		++fp_mismatch_frames_;
	}
	if (fp_expected > 0) {
		++fp_frames_;
	}
	max_lights_ = std::max(max_lights_, rs.lights);
	max_light_candidates_ = std::max(max_light_candidates_, ps.light_candidates);
	max_instances_ = std::max<uint32_t>(max_instances_, uint32_t(particles));
}

void Bf3FxDemo::_process(double p_delta) {
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
		if (!loaded_->error.empty()) {
			log("load failed: " + loaded_->error);
			UtilityFunctions::push_error(to_godot("[fx] load failed: " + loaded_->error));
			finish(1);
			return;
		}
		std::string error;
		if (!build(&error)) {
			log("build failed: " + error);
			finish(1);
			return;
		}
		return;
	}
	if (frozen_) {

		renderer_->draw(*world_, view(), *loaded_->atlas);
		frame_checks();
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
		shot_lines_.push_back(fmt("%s %s (%llu bytes, %u particles, %u lights)%s", s.name.c_str(), ok ? "saved" : "FAILED", (unsigned long long)bytes,
				renderer_->stats().drawn_instances, renderer_->stats().lights,
				s.kind == 0 ? flash_line().c_str()
						: (s.kind == 4 ? ("; " + oil_frame_line_).c_str() : "")));
		frozen_ = false;
		shot_active_ = false;
		++shot_index_;
		set_pose(main_pose_);
		return;
	}
	const double dt = std::clamp(p_delta, 0.0, 0.1);
	t_ += dt;
	const bool shooting_other = shot_active_ && shots_[shot_index_].kind != 0;

	if (check_ && !shot_active_ && shot_index_ < shots_.size() && t_ >= shots_[shot_index_].at) {
		const Shot &s = shots_[shot_index_];
		shot_active_ = true;
		shot_advance_left_ = s.advance;
		switch (s.kind) {
			case 0:
				set_pose(main_pose_);
				fire_muzzle();
				next_muzzle_ = t_ + kMuzzlePeriod;
				break;
			case 1:
			case 2: {
				const Vector3 p = s.kind == 1 ? concrete_point_ : brick_point_;
				const Vector3 n = s.kind == 1 ? concrete_normal_ : brick_normal_;
				Pose pose;
				pose.position = p + n * 2.2f + Vector3(0, 0.3f, 0);
				pose.target = p;
				set_pose(pose);
				start_impact(s.kind == 1 ? 0 : 1);
				break;
			}
			case 3: {
				Pose pose = main_pose_;
				pose.target = smoke_point_ + Vector3(0, 2.0f, 0);
				set_pose(pose);
				break;
			}
			case 4:
				set_pose(oil_pose_);
				oil_framed_ = false;
				break;
			default:
				set_pose(main_pose_);
				break;
		}
	}

	if (!shooting_other && t_ >= next_muzzle_) {
		fire_muzzle();
		next_muzzle_ = t_ + kMuzzlePeriod;
	}
	if (!shot_active_ && t_ >= next_impact_) {
		start_impact(impact_turn_++ % 2);
		next_impact_ = t_ + kImpactPeriod;
	}
	if (!smoke_started_ && t_ >= kSmokeStartAt) {
		smoke_started_ = true;
		const Vector3 fwd = ((main_pose_.target - main_pose_.position) * Vector3(1, 0, 1)).normalized();
		start_effect(loaded_->smoke, frame_of(smoke_point_, fwd, Vector3(0, 1, 0)), false);
	}
	if (!stress_started_ && t_ >= kStressAt) {
		start_light_stress();
	}
	renderer_->step_world(*world_, dt, view());
	oil_age_ += dt;
	renderer_->draw(*world_, view(), *loaded_->atlas);
	const FxRendererStats &rs = renderer_->stats();
	fx_ms_.push_back(rs.step_ms + rs.prepare_ms + rs.upload_ms);
	step_ms_.push_back(rs.step_ms);
	prep_ms_.push_back(rs.prepare_ms);
	upload_ms_.push_back(rs.upload_ms);
	++frames_;
	frame_checks();
	if (oil_count_at_15_ < 0 && oil_age_ >= kOilGateAge) {

		const auto outs = world_->outputs();
		int count = 0, drawn = 0;
		for (size_t i = 0; i < outs.size(); ++i) {
			if (outs[i].def == loaded_->oilfire.get()) {
				count += int(outs[i].particle_count);
				for (const fx::FxInstance &in : renderer_->prep().instances()) {
					drawn += uint32_t(in.custom[0] + 0.5f) == uint32_t(i) ? 1 : 0;
				}
			}
		}
		oil_count_at_15_ = count;
		oil_drawn_at_15_ = drawn;
		log(fmt("oilfire at %.3f s: %d live, %d drawn", oil_age_, count, drawn));
	}
	if (shot_active_) {
		shot_advance_left_ -= dt;
		const Shot &s = shots_[shot_index_];
		if (s.kind == 4 && !oil_framed_) {
			frame_oilfire();
		}
		if (shot_advance_left_ <= 0.0 && s.kind == 0 && !flash_present() && shot_retries_ < kFlashRetries) {
			++shot_retries_;
			fire_muzzle();
			next_muzzle_ = t_ + kMuzzlePeriod;
			shot_advance_left_ = s.advance;
		} else if (shot_advance_left_ <= 0.0) {
			frozen_ = true;
			settle_ = kSettleFrames;
		}
	}
	if (!check_) {
		return;
	}
	if (t_ >= kRunSeconds && shot_index_ >= shots_.size()) {
		report_and_finish();
	}
}

void Bf3FxDemo::report_and_finish() {
	auto row = [this](const char *p_gate, bool p_ok, const std::string &p_text) {
		report_.push_back(fmt("%-10s %s  %s", p_gate, pass_text(p_ok), p_text.c_str()));
		return p_ok;
	};
	const fx::RenderPrepStats &ps = renderer_->prep().stats();
	bool all = true;
	all &= row("E8(3)", count_mismatch_frames_ == 0 && frames_ > 0,
			fmt("drawn instances == FxWorld::particles() in %llu of %llu frames (max %u particles, %llu MultiMesh reallocations)",
					(unsigned long long)(frames_ - count_mismatch_frames_), (unsigned long long)frames_, max_instances_, (unsigned long long)renderer_->stats().reallocations));
	all &= row("E8(6)", atlas_uploaded_ == atlas_entries_ && atlas_entries_ > 0 && ps.unresolved_textures == 0,
			fmt("atlas %u / %u entries uploaded (%s); %llu default-texture fallbacks over the run", atlas_uploaded_, atlas_entries_, atlas_line_.c_str(),
					(unsigned long long)ps.unresolved_textures));
	all &= row("E8(4)", oil_count_at_15_ >= 0 && std::abs(oil_drawn_at_15_ - kOilTarget) <= kOilTolerance && oil_drawn_at_15_ == oil_count_at_15_,
			fmt("oilfire at 15 s: %d drawn, %d live (target %d +- %d)", oil_drawn_at_15_, oil_count_at_15_, kOilTarget, kOilTolerance));
	all &= row("lights", max_lights_ <= fx::kMaxFxLights && max_light_candidates_ > fx::kMaxFxLights,
			fmt("max %u effect lights in a frame (<= %zu); max %u merged candidates (stress > 64), %llu dropped by the pool", max_lights_, fx::kMaxFxLights,
					max_light_candidates_, (unsigned long long)ps.lights_dropped));
	all &= row("1p pass", fp_frames_ > 0 && fp_mismatch_frames_ == 0 && muzzle_starts_ > 0,
			fmt("%llu muzzle starts (flash, fill-out, shell); %llu frames with 1p particles, all in FirstPerson batches (FOV override 55, z-clip 0.25); %llu mismatching frames",
					(unsigned long long)muzzle_starts_, (unsigned long long)fp_frames_, (unsigned long long)fp_mismatch_frames_));
	row("FX time", true,
			fmt("main thread (step + prepare + upload, wall) p50 %.3f ms, p99 %.3f ms over %zu frames (step p50 %.3f / "
				"p99 %.3f, prepare p50 %.3f / p99 %.3f, upload p50 %.3f / p99 %.3f); E9's 1.0 ms p99 is wave 3's gate; "
				"shared machine: noisy",
					rounded_rank(fx_ms_, 0.5), rounded_rank(fx_ms_, 0.99), fx_ms_.size(), rounded_rank(step_ms_, 0.5),
					rounded_rank(step_ms_, 0.99), rounded_rank(prep_ms_, 0.5), rounded_rank(prep_ms_, 0.99),
					rounded_rank(upload_ms_, 0.5), rounded_rank(upload_ms_, 0.99)));
	row("impacts", concrete_found_ && brick_found_, concrete_line_ + "; " + brick_line_ + fmt("; %llu impact starts", (unsigned long long)impact_starts_));
	row("shots", shots_ok_ == shots_.size(), fmt("%u of %zu captures in %s (the look is the user's judgement)", shots_ok_, shots_.size(), shot_dir_.c_str()));
	for (const std::string &l : shot_lines_) {
		report_.push_back("           " + l);
	}
	row("stats", true,
			fmt("mesh stand-ins (last frame) %u, params uploads %llu, MultiMesh capacity %u", ps.mesh_standins, (unsigned long long)renderer_->stats().params_uploads,
					renderer_->stats().capacity));
	for (const std::string &n : notes_) {
		report_.push_back("           " + n);
	}
	const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(session_->install_snapshot_at_start(), session_->install().snapshot());
	all &= row("install", d.clean(), d.clean() ? "unchanged (layout.toc, cas.cat, every .sb/.toc: sizes and mtimes)" : d.summary());
	log("---- FX DEMO CHECK (shared machine: timings are noisy) ----");
	for (const std::string &l : report_) {
		log(l);
	}
	log(std::string("FX DEMO: ") + (all ? "PASS" : "FAIL"));
	finish(all ? 0 : 1);
}

void Bf3FxDemo::finish(int p_code) {
	if (finished_) {
		return;
	}
	finished_ = true;
	shutdown();
	session_->finish(p_code);
}

void Bf3FxDemo::shutdown() {
	if (shut_down_) {
		return;
	}
	shut_down_ = true;
	if (loader_.joinable()) {
		loader_.join();
	}
	if (renderer_ != nullptr) {
		renderer_->clear();
	}
	world_.reset();
	world_collision_.reset();
	if (loaded_) {
		loaded_->library.reset();
		loaded_->sources.reset();
		loaded_->cache.reset();
	}
}

}
