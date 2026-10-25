#include "level_session_game.h"
#include "level_session_tdm.h"

#include "bf3_localisation.h"
#include "game_audio.h"
#include "game_crowd.h"
#include "fx_renderer.h"
#include "game_fx.h"
#include "godot_cache.h"
#include "godot_util.h"
#include "hud_items.h"
#include "native_hud.h"
#include "ui_assets.h"
#include "viewmodel.h"
#include "viewmodel_muzzle.h"

#include "bf3/core/install.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/kits.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace render = bf3::render;
namespace sim = bf3::sim;
namespace sw = bf3::sim::weapons;

namespace {

constexpr uint32_t kMaxPlayableBots = 32;
constexpr uint32_t kPlayableDefaultBots = 24;
constexpr uint32_t kPlayCaseBots = 16;
constexpr uint32_t kFirefightBots = 64;
constexpr size_t kMaxPredictedKept = 1u << 16;

double steady_s() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

using bf3::core::to_lower_ascii;

const char *team_name(uint8_t p_team) {
	return p_team == 1 ? "US" : (p_team == 2 ? "RU" : "?");
}

Label *make_label(int p_size, HorizontalAlignment p_align) {
	Label *l = memnew(Label);
	l->add_theme_color_override("font_color", Color(1, 1, 1));
	l->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9f));
	l->add_theme_constant_override("shadow_offset_x", 1);
	l->add_theme_constant_override("shadow_offset_y", 1);
	l->add_theme_font_size_override("font_size", p_size);
	l->set_horizontal_alignment(p_align);
	l->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	return l;
}

Ref<StandardMaterial3D> tint_material(const Color &p_color) {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_albedo(p_color);
	m->set_roughness(0.8f);
	return m;
}

fx::Vec3 to_fx(const Vector3 &p_v) {
	return fx::Vec3{ p_v.x, p_v.y, p_v.z };
}

fx::Transform fx_frame_of(const Vector3 &p_origin, const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 f = p_forward.normalized();
	Vector3 r = p_up_hint.cross(f);
	if (r.length() < 1e-4f) {
		r = Vector3(0, 0, 1).cross(f);
	}
	r = r.normalized();
	fx::Transform t;
	t.right = to_fx(r);
	t.up = to_fx(f.cross(r).normalized());
	t.forward = to_fx(f);
	t.trans = to_fx(p_origin);
	return t;
}

}

void ServerShotRecorder::weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sw::SoldierWeaponsState &p_after, const sw::WeaponTickOutput &p_out) {
	(void)p_after;
	if (int(p_slot) != slot_.load(std::memory_order_acquire) || (p_out.shot_count == 0 && p_out.timed_count == 0)) {
		return;
	}
	std::lock_guard lock(mutex_);
	for (uint8_t i = 0; i < p_out.shot_count && i < p_out.shots.size(); ++i) {
		if (shots_.size() >= kMaxRecords) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		shots_.push_back({ p_command.tick, p_out.shots[i] });
	}
	for (uint8_t i = 0; i < p_out.timed_count && i < p_out.timed.size(); ++i) {
		if (timed_.size() >= kMaxRecords) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		timed_.push_back({ p_command.tick, p_out.timed[i] });
	}
}

void ServerShotRecorder::damage(uint32_t p_tick, const sw::DamageEvent &p_event, bool p_applied, float p_health_after) {
	std::lock_guard lock(mutex_);
	if (damage_.size() >= kMaxRecords) {
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	damage_.push_back({ p_tick, p_event, p_applied, p_health_after });
}

void ServerShotRecorder::projectiles(uint32_t p_tick, const sw::ProjectileStepOutput &p_out, std::span<const sw::Projectile> p_live) {
	(void)p_live;
	const int slot = slot_.load(std::memory_order_acquire);
	if (p_out.detonations.empty() || slot < 0) {
		return;
	}
	std::lock_guard lock(mutex_);
	for (const sw::DetonationEvent &d : p_out.detonations) {
		if (int(d.player_slot) != slot) {
			continue;
		}
		if (detonations_.size() >= kMaxRecords) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		detonations_.push_back({ p_tick, d });
	}
}

void ServerShotRecorder::take_detonations(std::vector<Detonation> &r_detonations) {
	std::lock_guard lock(mutex_);
	r_detonations.insert(r_detonations.end(), detonations_.begin(), detonations_.end());
	detonations_.clear();
}

void ServerShotRecorder::take(std::vector<Shot> &r_shots, std::vector<Timed> &r_timed, std::vector<Damage> &r_damage) {
	std::lock_guard lock(mutex_);
	r_shots.insert(r_shots.end(), shots_.begin(), shots_.end());
	r_timed.insert(r_timed.end(), timed_.begin(), timed_.end());
	r_damage.insert(r_damage.end(), damage_.begin(), damage_.end());
	shots_.clear();
	timed_.clear();
	damage_.clear();
}

bool GameState::wants_case(M2aCase p_case) const {
	return std::find(cases.begin(), cases.end(), p_case) != cases.end();
}

int GameState::item_of(uint8_t p_team, uint16_t p_def) const {
	if (p_team < 1 || p_team > 2) {
		return -1;
	}
	const auto it = def_items[size_t(p_team - 1)].find(p_def);
	return it != def_items[size_t(p_team - 1)].end() ? it->second : -1;
}

bool GameState::slot_selectable(const bf3::net::WeaponSlotWire &p_slot) const {
	if (p_slot.def == sw::kNoWeapon) {
		return false;
	}
	if (p_slot.magazine > 0 || p_slot.reserve > 0 || p_slot.def >= def_switch_when_empty.size() ||
			def_switch_when_empty[p_slot.def]) {
		return true;
	}
	return gunplay != nullptr && gunplay->table != nullptr && p_slot.def < gunplay->table->defs.size() &&
			gunplay->table->defs[p_slot.def].unlimited_ammo;
}

bool GameState::hud_ready() const {
	return (hud_items == nullptr || hud_items->is_loaded()) && (native_hud == nullptr || native_hud->is_ready());
}

void Bf3LevelSession::GameDeleter::operator()(Game *p_session) const {
	delete p_session;
}

void Bf3LevelSession::game_parse_args() {
	const bool scene_game = has_meta("openbf3_game") && bool(get_meta("openbf3_game"));
	const bool check_case = has_check_arg("m2a-check");
	const bool check_firefight = has_check_arg("m2a-firefight");
	const bool check = check_case || check_firefight;
	if (!scene_game && !check) {
		return;
	}
	game_.reset(new Game());
	Game &g = *game_;
	g.enabled = true;
	g.playable = !check;
	std::string error;
	if (check_case) {
		bf3::core::Result<std::vector<M2aCase>> cases = parse_m2a_cases(user_arg_value("m2a-check"));
		if (!cases) {
			error = cases.error().message;
		} else {
			g.cases = std::move(*cases);
		}
	}
	if (check_firefight && !g.wants_case(M2aCase::Firefight)) {
		g.cases.push_back(M2aCase::Firefight);
	}
	if (has_user_arg("kit")) {
		const int kit = std::atoi(user_arg_value("kit").c_str());
		if (kit < 0 || kit >= int(host::kKitCount)) {
			error = "--kit=" + user_arg_value("kit") + ": not 0..3 (assault, engineer, support, recon)";
		}
		g.kit = uint8_t(std::clamp(kit, 0, int(host::kKitCount) - 1));
	}
	if (has_user_arg("team")) {
		const int team = std::atoi(user_arg_value("team").c_str());
		if (team != 1 && team != 2) {
			error = "--team=" + user_arg_value("team") + ": not 1 or 2";
		}
		g.team = team == 2 ? 2 : 1;
	}
	g.respawn_s = g.checks() ? 1.0 : 5.0;
	if (has_user_arg("respawn")) {
		const double s = std::atof(user_arg_value("respawn").c_str());
		if (!(s >= 0.1 && s <= 120.0)) {
			error = "--respawn=" + user_arg_value("respawn") + ": not 0.1..120 seconds";
		}
		g.respawn_s = std::clamp(std::isfinite(s) ? s : 5.0, 0.1, 120.0);
	}
	g.next_kit = g.kit;
	g.next_team = g.team;
	g.record_predicted = g.wants_case(M2aCase::Range) || g.wants_case(M2aCase::Reload) || g.wants_case(M2aCase::Kits);
	g.check_options.shots_dir = utf8_path(user_arg_value("m2a-shots"));
#if OPENBF3_BUILD_CHECKS
	if (has_check_arg("native-hud-moments")) {
		g.hud_moments = std::make_unique<NativeHudMomentRun>(user_arg_value("native-hud-moments"), user_arg_value("native-hud-ids"));
	}
	if (has_check_arg("crosshair-walk")) {
		g.crosshair_walk = std::make_unique<CrosshairWalkRun>();
	}
#endif
	if (has_user_arg("m2a-play-seconds")) {
		g.check_options.play_seconds = std::clamp(std::atof(user_arg_value("m2a-play-seconds").c_str()), 5.0, 3600.0);
	}

	const bool firefight_case = g.wants_case(M2aCase::Firefight);
	if (!has_user_arg("bots")) {
		bots_ = g.playable ? kPlayableDefaultBots : (firefight_case ? kFirefightBots : (g.wants_case(M2aCase::Play) ? kPlayCaseBots : 0u));
	}
	g.firefight_pairs = firefight_case;
	if (firefight_case && bots_ > kFirefightBots) {
		bots_ = kFirefightBots;
	} else if (!firefight_case && bots_ > kMaxPlayableBots) {
		UtilityFunctions::print(to_godot("[game] --bots=" + std::to_string(bots_) + " lowered to " + std::to_string(kMaxPlayableBots) +
				" (the player must stay inside the lag-compensation slots)"));
		bots_ = kMaxPlayableBots;
	}
	nav_overlay_ = bots_ > 0 && has_user_arg("nav-overlay");
	tdm_parse_args(error);
	if (!has_user_arg("walkers")) {
		walkers_ = 0;
	}
	if (g.checks() && quit_after_ <= 0.0) {

		quit_after_ = 1.0e9;
	}
	mapper_.enable_weapons(true);

	g.vm_tuning_source = "res://config/viewmodel_tuning.cfg";
	if (FileAccess::file_exists(to_godot(g.vm_tuning_source))) {
		bf3::core::Result<bf3::anim::ViewmodelTuning> t = bf3::anim::parse_viewmodel_tuning(to_std(FileAccess::get_file_as_string(to_godot(g.vm_tuning_source))));
		if (t) {
			g.vm_tuning = *t;
		} else {
			error = g.vm_tuning_source + ": " + t.error().message;
		}
	} else {
		g.vm_tuning_source = "(missing: built-in defaults)";
	}

	const bool windowed = DisplayServer::get_singleton()->get_name() != String("headless");
	if (windowed) {
		if (Bf3Localisation::current() == nullptr) {
			g.localisation = memnew(Bf3Localisation);
			g.localisation->set_name("GameLocalisation");
			add_child(g.localisation);
		}
		if (Bf3UiAssets::current() == nullptr) {
			g.ui_assets = memnew(Bf3UiAssets);
			g.ui_assets->set_name("GameUiAssets");
			add_child(g.ui_assets);
		}
	}
	std::string cases_text;
	for (M2aCase c : g.cases) {
		cases_text += (cases_text.empty() ? "" : ",") + std::string(m2a_case_name(c));
	}
	char line[512];
	std::snprintf(line, sizeof(line), "[game] %s: kit %s, team %s, respawn %.1f s, bots %u, walkers %u%s%s", g.playable ? "playable scene" : "check run",
			host::kit_role_name(host::KitRole(g.kit)), team_name(g.team), g.respawn_s, bots_, walkers_, cases_text.empty() ? "" : ", cases ",
			cases_text.c_str());
	UtilityFunctions::print(line);
	if (!error.empty()) {
		UtilityFunctions::print(to_godot("[game] bad arguments: " + error));
		std::lock_guard lock(loader_mutex_);
		load_error_ = error;
		load_failed_.store(true, std::memory_order_release);
	}
}

void Bf3LevelSession::game_load_options(host::SharedLoadOptions &r_options) {
	if (!game_) {
		return;
	}
	r_options.gunplay = true;
	tdm_load_options(r_options);
	if (!scene_) {
		return;
	}

	Game *g = game_.get();

	g->crowd = std::unique_ptr<Bf3GameCrowd, void (*)(Bf3GameCrowd *)>(new Bf3GameCrowd(), &delete_part<Bf3GameCrowd>);
	g->crowd_loader = std::thread([this, g]() {
		const double t0 = steady_s();
		std::shared_ptr<bf3::core::Cache> cache;
		if (!no_cache_) {
			AcquiredCache acquired = acquire_cache(cache_root_, guard_);
			if (acquired.status == bf3::core::CacheStatus::Ok) {
				cache = acquired.cache;
			}
		}
		if (const bf3::core::Status s = g->crowd->load_body(install_, cache.get(), &stop_); !s.ok()) {
			g->crowd_body_error = s.error().message;
		}
		cache.reset();
		g->crowd_body_seconds = steady_s() - t0;
	});
	g->vm_loader = std::thread([this, g]() {
		const double t0 = steady_s();
		std::shared_ptr<bf3::core::Cache> cache;
		if (!no_cache_) {
			AcquiredCache acquired = acquire_cache(cache_root_, guard_);
			if (acquired.status == bf3::core::CacheStatus::Ok) {
				cache = acquired.cache;
			}
		}
		const std::string superbundle = "Win32/Levels/" + level_ + "/" + level_;
		const auto fail = [&](const std::string &p_error) {
			g->vm_error = p_error;
			g->vm_seconds = steady_s() - t0;
			g->vm_done.store(true, std::memory_order_release);
		};
		bf3::core::Result<std::unique_ptr<render::ViewmodelSources>> sources = render::open_viewmodel_sources(install_, superbundle, &g->vm_progress);
		if (!sources) {
			fail("viewmodel sources: " + sources.error().message);
			return;
		}
		if (stop_.load(std::memory_order_acquire)) {
			fail("stopped");
			return;
		}
		bf3::core::Result<std::vector<render::ViewmodelKit>> kits = render::read_viewmodel_kits(*(*sources)->ebx, render::viewmodel_level_id(superbundle), mode_);
		if (!kits) {
			fail("viewmodel kits: " + kits.error().message);
			return;
		}
		g->vm_kits = std::move(*kits);
		if (g->tdm) {
			render::add_assault_gadget(g->vm_kits, host::kTdmAssaultGadget);
		}
		for (const int team : { 1, 2 }) {
			if (stop_.load(std::memory_order_acquire)) {
				fail("stopped");
				return;
			}
			render::ViewmodelSetSpec spec = render::team_viewmodel_set(g->vm_kits, team);
			spec.superbundle = superbundle;
			bf3::core::Result<std::unique_ptr<render::ViewmodelSet>> set = render::load_viewmodel_set(**sources, spec, &g->vm_progress, cache.get());
			if (!set) {
				fail(std::string(team == 1 ? "US" : "RU") + " viewmodel set: " + set.error().message);
				return;
			}
			g->vm_sets[size_t(team - 1)] = std::move(*set);
		}
		g->vm_seconds = steady_s() - t0;
		cache.reset();
		g->vm_done.store(true, std::memory_order_release);
	});
}

bool Bf3LevelSession::game_load_assets(const host::LevelDatabase &p_db, bf3::core::Cache *p_cache, unsigned p_threads, std::string *r_error) {
	if (!game_) {
		return true;
	}
	Game &g = *game_;
	if (g.vm_loader.joinable()) {
		g.vm_loader.join();
	}
	const std::shared_ptr<const host::SharedLevel> shared = shared_level();
	if (shared == nullptr || shared->gunplay == nullptr) {
		*r_error = "the level loaded without gunplay tables";
		return false;
	}
	bf3::core::Result<std::shared_ptr<const host::ClientGunplayInputs>> inputs = host::make_client_gunplay_inputs(*shared);
	if (!inputs) {
		*r_error = inputs.error().message;
		return false;
	}
	g.gunplay = *inputs;
	g.def_switch_when_empty.clear();
	for (const bf3::core::gameplay::WeaponDef &d : shared->gunplay->defs) {
		g.def_switch_when_empty.push_back(d.allow_switching_out_of_ammo);
	}
	bf3::core::Result<host::HudStatic> hud_static = host::make_hud_static(*g.gunplay->table, shared->gunplay->defs);
	if (hud_static) {
		g.hud_static = std::move(*hud_static);
	} else {
		g.hud_static_error = hud_static.error().message;
	}
	if (g.checks()) {

		bf3::core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(shared->static_world);
		if (world) {
			g.world = std::move(*world);
		}
		g.recorder = std::make_shared<ServerShotRecorder>();
	}

	GameLoadInputs li;
	li.db = &p_db;
	li.shared = shared;
	li.cache = p_cache;
	li.threads = p_threads;
	li.stop = &stop_;
	li.progress = &progress_;
	li.own_team = g.team;
	(void)li;
	if (scene_) {
		const auto note = [&g](const std::string &p_line) {
			if (g.part_notes.size() < 64) {
				g.part_notes.push_back(p_line);
			}
		};
		{
			const double t0 = steady_s();
			g.fx = std::unique_ptr<Bf3GameFx, void (*)(Bf3GameFx *)>(new Bf3GameFx(), &delete_part<Bf3GameFx>);
			if (const bf3::core::Status s = g.fx->load(li); !s.ok()) {
				note("[game] ERROR fx load: " + s.error().message);
				g.fx.reset();
			} else {
				const GameFxStats fs = g.fx->stats();
				char line[256];
				std::snprintf(line, sizeof(line), "[game] fx loaded in %.2f s: %u blueprints preloaded, %u failures, %u decal templates", steady_s() - t0, fs.preloaded,
						fs.preload_failures, fs.decal_templates);
				note(line);
			}
		}
		{
			const double t0 = steady_s();
			g.audio = std::unique_ptr<Bf3GameAudio, void (*)(Bf3GameAudio *)>(new Bf3GameAudio(), &delete_part<Bf3GameAudio>);
			if (!g.wants_case(M2aCase::Audio)) {

				audio::GameAudioLoadOptions ao;
				ao.audition_superbundles.clear();
				g.audio->set_load_options(ao);
			}
			const std::vector<std::string> names = g.fx != nullptr ? g.fx->impact_sound_names() : std::vector<std::string>();
			if (const bf3::core::Status s = g.audio->load(li, names); !s.ok()) {
				note("[game] ERROR audio load: " + s.error().message);
				g.audio.reset();
			} else {
				const GameAudioStats as = g.audio->stats();
				char line[256];
				std::snprintf(line, sizeof(line), "[game] audio loaded in %.2f s: %u programs, %u failures, %zu impact sounds", steady_s() - t0, as.programs, as.program_failures,
						names.size());
				note(line);
			}
		}
		{
			const double t0 = steady_s();

			if (g.crowd_loader.joinable()) {
				g.crowd_loader.join();
			}
			const double t_join = steady_s();
			if (g.crowd == nullptr) {
				g.crowd = std::unique_ptr<Bf3GameCrowd, void (*)(Bf3GameCrowd *)>(new Bf3GameCrowd(), &delete_part<Bf3GameCrowd>);
			}
			if (!g.crowd_body_error.empty()) {
				note("[game] ERROR crowd load: " + g.crowd_body_error);
				g.crowd.reset();
			} else if (const bf3::core::Status s = g.crowd->load(li); !s.ok()) {
				note("[game] ERROR crowd load: " + s.error().message);
				g.crowd.reset();
			} else {
				char line[192];
				std::snprintf(line, sizeof(line), "[game] crowd loaded in %.2f s after the SharedLevel (waited %.2f s for its body thread, %.2f s there)", steady_s() - t0,
						t_join - t0, g.crowd_body_seconds);
				note(line);
			}
		}
	}
	return true;
}

void Bf3LevelSession::game_server_config(host::GameServerConfig &r_game) {
	if (!game_) {
		return;
	}
	Game &g = *game_;
	r_game.respawn_ticks = uint32_t(std::clamp(std::lround(g.respawn_s * double(sim::kSimHz)), 1L, 120L * long(sim::kSimHz)));
	r_game.gunplay.player_kit = g.kit;
	r_game.gunplay.player_team = g.team;
	if (g.checks()) {
		r_game.gunplay.observer = g.recorder;
		if (g.wants_case(M2aCase::Range) || g.wants_case(M2aCase::Kits) || g.wants_case(M2aCase::Fx)) {

			r_game.gunplay.exact_aim = true;
			r_game.gunplay.target_bots = 1;
			r_game.gunplay.target_team = g.team == 1 ? 2 : 1;
		}
	}

	if (bots_ > 0 && r_game.bots != nullptr) {
		auto fill = std::make_shared<host::BotFillConfig>(*r_game.bots);
		fill->count = 0;
		r_game.bots = fill;
		auto ff = std::make_shared<host::FirefightConfig>();
		ff->mode = g.firefight_pairs ? host::FirefightMode::Pairs : host::FirefightMode::Free;
		ff->count = bots_;
		ff->walk = !g.firefight_pairs;
		g.firefight_stats = std::make_shared<host::FirefightStats>();
		ff->stats = g.firefight_stats;
		r_game.firefight = ff;
		g.firefight_bots = bots_;
		r_game.max_slots = std::max<uint32_t>(64u, bots_ + 1u + r_game.walkers + r_game.gunplay.target_bots);
		char line[256];
		std::snprintf(line, sizeof(line), "[game] firefight bots: %u (%s), max slots %u", bots_, g.firefight_pairs ? "pairs" : "free", r_game.max_slots);
		g.part_notes.push_back(line);
	}
	tdm_server_config(r_game);
}

void Bf3LevelSession::game_client_config(host::ClientSessionConfig &r_config) const {
	if (!game_) {
		return;
	}
	const Game &g = *game_;
	r_config.gunplay = g.gunplay;
	r_config.send_kit_select = true;
	r_config.kit_select = bf3::net::KitSelect{ g.kit, g.team };
	r_config.record_predicted = g.record_predicted;
}

void Bf3LevelSession::game_presented(double p_delta, uint64_t p_now_ns) {
	(void)p_delta;
	if (!game_ || client_ == nullptr) {
		return;
	}
	Game &g = *game_;

	g.events.clear();
	{
		std::array<host::PresentEvent, 256> chunk{};
		size_t n = 0;
		while ((n = client_->events().drain(chunk)) > 0) {
			g.events.insert(g.events.end(), chunk.begin(), chunk.begin() + n);
			if (n < chunk.size()) {
				break;
			}
		}
	}
	g.events_drained += g.events.size();
	if (g.record_predicted) {
		client_->take_predicted(g.predicted_shots, g.predicted_timed);
		if (g.predicted_shots.size() > kMaxPredictedKept || g.predicted_timed.size() > kMaxPredictedKept) {
			g.predicted_shots.clear();
			g.predicted_timed.clear();
		}
	} else if (g.checks()) {
		std::vector<host::PredictedShot> s;
		std::vector<host::PredictedTimed> t;
		client_->take_predicted(s, t);
	}
	if (g.recorder != nullptr) {
		g.recorder->take(g.server_shots, g.server_timed, g.server_damage);
		g.recorder->take_detonations(g.server_detonations);
		if (g.server_detonations.size() > kMaxPredictedKept) {
			g.server_detonations.clear();
		}
		if (!g.record_predicted) {
			g.server_shots.clear();
			g.server_timed.clear();
			if (g.server_damage.size() > kMaxPredictedKept) {
				g.server_damage.clear();
			}
		}
	}

	const host::PresentationFrame &f = *frame_;
	g.spawned = f.valid && f.stats.spawned;
	g.own_slot = g.spawned ? f.stats.slot : uint8_t(0xFF);
	g.own_team = f.own_team == 1 || f.own_team == 2 ? f.own_team : g.team;
	const bool state_dead = g.spawned && (f.own_curr.mode == sim::MoveMode::Dead || (f.own_curr.flags & sim::soldier_flags::kAlive) == 0);
	g.alive = g.spawned && !state_dead && f.own_curr.life > 0;
	if (g.recorder != nullptr && g.spawned) {
		g.recorder->watch_slot(int(f.stats.slot));
	}
	g.weapon = host::PresentedWeapon();
	const std::shared_ptr<const host::SharedLevel> level = shared_level();
	if (g.alive && f.own_weapons_valid && g.gunplay != nullptr && g.gunplay->table != nullptr && level != nullptr) {
		g.weapon = host::present_weapon(f.own_weapons_prev, f.own_weapons_curr, f.tick, host::present_own_fraction(f, p_now_ns), *g.gunplay->table,
				sw::suppressed_sway(sw::weapon_sway_context(level->motor, f.own_curr, f.own_command), 0.0f, g.gunplay->table->mechanics));
	}

	for (const host::PresentEvent &e : g.events) {
		switch (e.kind) {
			case host::PresentKind::Spawn:
				g.slot_team[e.slot] = uint8_t(e.extra == 1 || e.extra == 2 ? e.extra : 0);
				break;
			case host::PresentKind::OwnShot:
				++g.own_shots_seen;
				break;
			case host::PresentKind::RemoteShot:
				++g.remote_shots_seen;
				break;
			case host::PresentKind::Hit:
				++g.hits_seen;
				break;
			case host::PresentKind::Death: {
				++g.deaths_seen;
				const bool own_victim = (e.flags & host::present_flags::kOwnVictim) != 0;
				const bool own_shooter = (e.flags & host::present_flags::kOwnShooter) != 0;
				const bool suicide = e.slot == e.aux;
				if (own_victim) {
					++g.deaths;
					if (!g.dead || g.death_ns == 0) {
						g.death_ns = p_now_ns;
					}
					g.dead = true;
					g.killed_by = e.slot;
					g.killed_by_self = suicide;
					g.deaths_by_enemy += suicide ? 0u : 1u;
				}
				if (own_shooter && !own_victim) {
					++g.kills;
				}
				if (!suicide) {
					const uint8_t killer_team = own_shooter ? g.own_team : g.slot_team[e.slot];
					++g.team_kills[killer_team <= 2 ? killer_team : 0];
				}
				break;
			}
			default:
				break;
		}
	}
	if (state_dead && !g.dead) {
		g.dead = true;
		g.killed_by = 0xFF;
		g.killed_by_self = false;
		g.death_ns = p_now_ns;
	}
	if (g.alive && f.own_curr.life != g.own_life) {
		if (g.own_life != 0) {
			++g.respawns;
		}
		g.own_life = f.own_curr.life;
		g.dead = false;
		g.death_ns = 0;
		g.killed_by = 0xFF;
	}

	uint16_t selectable = 0;
	if (f.own_weapons_valid) {
		for (size_t s = 0; s < sw::kMaxWeaponSlots; ++s) {
			if (g.slot_selectable(f.own_weapons_curr.slots[s])) {
				selectable = uint16_t(selectable | (1u << s));
			}
		}
	}
	mapper_.set_weapon_slots(g.weapon.valid ? g.weapon.active_slot : uint8_t(0),
			g.weapon.valid ? g.weapon.pending_slot : uint8_t(0xFF), selectable);
	mapper_.set_active_weapon(
			g.weapon.valid ? g.weapon.phase : sw::WeaponPhase::None, g.weapon.magazine, g.weapon.reserve);
	mapper_.set_weapons_allowed(done_ && g.alive && g.weapon.valid);
	tdm_presented(p_now_ns);

	if (const uint32_t k = mapper_.take_kit_presses(); k > 0) {
		g.next_kit = uint8_t((g.next_kit + k) % host::kKitCount);
		g.kit_pending = true;
	}
	if (const uint32_t t = mapper_.take_team_presses(); t > 0) {
		if (t % 2 == 1) {
			g.next_team = g.next_team == 1 ? 2 : 1;
		}
		g.kit_pending = true;
	}
	if (g.kit_pending && g.spawned) {
		client_->send_kit_select(bf3::net::KitSelect{ g.next_kit, g.next_team });
		g.kit_pending = false;
		++g.kit_selects_sent;
		UtilityFunctions::print(to_godot(std::string("[game] next spawn: ") + host::kit_role_name(host::KitRole(g.next_kit)) + " " + team_name(g.next_team)));
	}
}

namespace {

void build_game_ui(Node *p_parent, GameState &r_game, bool p_windowed) {
	r_game.layer = memnew(CanvasLayer);
	r_game.layer->set_name("GameUi");
	p_parent->add_child(r_game.layer);
	if (p_windowed) {
		r_game.native_hud = memnew(Bf3NativeHud);
		r_game.native_hud->set_name("NativeHud");
		r_game.layer->add_child(r_game.native_hud);
		r_game.hud_items = memnew(Bf3HudItems);
		r_game.hud_items->set_name("HudItems");
		r_game.layer->add_child(r_game.hud_items);
	}
	r_game.score_label = make_label(16, HORIZONTAL_ALIGNMENT_RIGHT);
	r_game.score_label->set_name("Score");
	r_game.layer->add_child(r_game.score_label);
	r_game.kit_label = make_label(14, HORIZONTAL_ALIGNMENT_RIGHT);
	r_game.kit_label->set_name("NextSpawn");
	r_game.layer->add_child(r_game.kit_label);
	r_game.death_label = make_label(30, HORIZONTAL_ALIGNMENT_CENTER);
	r_game.death_label->set_name("DeathScreen");
	r_game.death_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	r_game.death_label->set_visible(false);
	r_game.layer->add_child(r_game.death_label);
	r_game.tints[0] = tint_material(Color(0.62f, 0.62f, 0.62f));
	r_game.tints[1] = tint_material(Color(0.25f, 0.55f, 1.0f));
	r_game.tints[2] = tint_material(Color(1.0f, 0.28f, 0.2f));
	r_game.built = true;
}

void map_viewmodel_items(GameState &r_game) {
	if (r_game.gunplay == nullptr) {
		return;
	}
	for (uint8_t team = 1; team <= 2; ++team) {
		const render::ViewmodelSet *set = r_game.vm_sets[size_t(team - 1)].get();
		if (set == nullptr) {
			continue;
		}
		std::unordered_map<uint16_t, int> &map = r_game.def_items[size_t(team - 1)];
		for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
			const host::KitLoadout &l = r_game.gunplay->kits.loadout(team, kit);
			for (size_t s = 0; s < sw::kMaxWeaponSlots; ++s) {
				if (l.defs[s] == sw::kNoWeapon || l.unlocks[s].empty()) {
					continue;
				}
				const std::string want = to_lower_ascii(l.unlocks[s]);
				for (size_t i = 0; i < set->items.size(); ++i) {
					if (to_lower_ascii(set->items[i].unlock) == want) {
						map[l.defs[s]] = int(i);
						break;
					}
				}
			}
		}
	}
}

void step_game_parts(Node3D &r_parent, GameState &r_game, const host::PresentationFrame &p_frame, uint64_t p_now_ns, double p_delta, Camera3D *p_camera,
		Bf3Viewmodel *p_own_vm, bool p_free_camera, const std::array<host::PresentedOtherWeapon, bf3::net::kMaxSlots> &p_others, size_t p_other_count) {
	r_game.ms_crowd = r_game.ms_fx = r_game.ms_audio = 0;
	std::string error;
	if (r_game.fx != nullptr && r_game.fx->loaded() && !r_game.fx->built()) {
		if (!r_game.fx->build(&r_parent, &error)) {
			UtilityFunctions::print(to_godot("[game] ERROR fx build: " + error));
			r_game.fx.reset();
		} else {
			const GameFxStats fs = r_game.fx->stats();
			UtilityFunctions::print(to_godot("[game] fx built: atlas " + std::to_string(fs.atlas_entries) + " entries, " + std::to_string(fs.atlas_fallbacks) +
					" fallbacks, " + std::to_string(fs.decal_textures) + " decal textures"));
			for (const std::string &n : r_game.fx->notes()) {
				UtilityFunctions::print(to_godot("[game] fx: " + n));
			}
		}
	}
	if (r_game.audio != nullptr && r_game.audio->loaded() && !r_game.audio->built()) {
		if (!r_game.audio->build(&r_parent, &error)) {
			UtilityFunctions::print(to_godot("[game] ERROR audio build: " + error));
			r_game.audio.reset();
		} else {
			for (const std::string &n : r_game.audio->load_report()) {
				UtilityFunctions::print(to_godot("[game] audio: " + n));
			}
		}
	}
	if (r_game.crowd != nullptr && r_game.crowd->loaded() && !r_game.crowd->built()) {
		if (!r_game.crowd->build(&r_parent, &error)) {
			UtilityFunctions::print(to_godot("[game] ERROR crowd build: " + error));
			r_game.crowd.reset();
		} else {
			UtilityFunctions::print(to_godot("[game] crowd built: " + std::to_string(r_game.crowd->stats().lanes) + " pose lanes"));
		}
	}
	r_game.crowd_built = r_game.crowd != nullptr && r_game.crowd->built();
	GameFrame mf;
	mf.frame = &p_frame;
	mf.clock = host::present_clock(p_frame, p_now_ns);
	mf.now_ns = p_now_ns;
	mf.dt = p_delta;
	mf.events = r_game.events;
	std::array<host::PresentedSoldier, bf3::net::kMaxSlots> soldiers{};
	const size_t n = p_frame.valid ? host::present_others(p_frame, p_now_ns, soldiers) : 0;
	mf.others = std::span<const host::PresentedSoldier>(soldiers.data(), n);
	mf.other_weapons = std::span<const host::PresentedOtherWeapon>(p_others.data(), std::min(n, p_other_count));
	mf.free_camera = p_free_camera;
	mf.own_alive = r_game.alive;
	mf.own_slot = r_game.own_slot;
	mf.own_team = r_game.own_team;

	Camera3D *active = p_camera;
	if (Viewport *vp = r_parent.get_viewport()) {
		active = vp->get_camera_3d() != nullptr ? vp->get_camera_3d() : p_camera;
	}
	if (r_game.listener_override) {
		mf.camera = fx_frame_of(r_game.listener_position, r_game.listener_forward, Vector3(0, 1, 0));
		mf.free_camera = true;
		r_game.camera_valid = false;
	} else if (active != nullptr) {
		const Transform3D t = active->get_global_transform();
		mf.camera = fx_frame_of(t.origin, -t.basis.get_column(2), t.basis.get_column(1));
		const double dt = std::clamp(p_delta, 1e-4, 0.25);
		if (r_game.camera_valid) {
			const Vector3 v = (t.origin - r_game.camera_prev) / float(dt);
			mf.camera_velocity = v.length() < 200.0f ? to_fx(v) : fx::Vec3{};
		}
		r_game.camera_prev = t.origin;
		r_game.camera_valid = true;
	}

	using bf3::core::ms_since;
	auto t0 = std::chrono::steady_clock::now();
	if (r_game.crowd_built) {
		r_game.crowd->step(mf);
	}
	r_game.ms_crowd = ms_since(t0);
	std::vector<fx::MuzzleSample> muzzles;
	if (p_own_vm != nullptr && p_own_vm->is_visible() && r_game.alive && !p_free_camera && r_game.weapon.valid) {
		const fx::Vec3 vel{ p_frame.own_curr.velocity[0], p_frame.own_curr.velocity[1], p_frame.own_curr.velocity[2] };
		fx::MuzzleSample s;
		if (r_game.fx != nullptr) {
			s = r_game.fx->own_muzzle(*p_own_vm, r_game.weapon.def, r_game.own_slot, vel);
		}
		if (s.valid) {
			++r_game.own_muzzles_fx;
		} else {
			s = muzzle_sample(viewmodel_muzzle_transform(*p_own_vm), r_game.own_slot, vel);
			r_game.own_muzzles_standin += s.valid ? 1 : 0;
		}
		if (s.valid) {
			muzzles.push_back(s);
		}
	}
	if (r_game.crowd_built) {
		const std::span<const fx::MuzzleSample> crowd = r_game.crowd->muzzles();
		muzzles.insert(muzzles.end(), crowd.begin(), crowd.end());
	}
	mf.muzzles = muzzles;

	t0 = std::chrono::steady_clock::now();
	std::span<const fx::ImpactSoundCue> cues;
	std::vector<audio::EffectSoundRequest> effect_sounds;
	if (r_game.fx != nullptr && r_game.fx->built()) {
		if (p_own_vm != nullptr) {
			const float fov = p_own_vm->applied_weapon_fov();
			if (fov > 0.0f && fov != r_game.fp_fov_applied) {
				r_game.fx->set_first_person_projection(fov, 1.0f);
				r_game.fp_fov_applied = fov;
			}
		}
		if (!r_game.fx_off) {
			r_game.fx->step(mf);
			cues = r_game.fx->sound_cues();
			for (const fx::EffectSoundCue &c : r_game.fx->effect_sound_cues()) {
				audio::EffectSoundRequest r;
				r.patch = c.patch;
				r.effect = c.effect;
				r.component = c.component;
				r.position = audio::Vec3{ c.position.x, c.position.y, c.position.z };
				r.start = c.start;
				effect_sounds.push_back(r);
			}
		} else if (Bf3FxRenderer *renderer = Object::cast_to<Bf3FxRenderer>(r_parent.get_node_or_null(NodePath("GameFx")))) {
			renderer->hide_all();
		}
	}
	r_game.ms_fx = ms_since(t0);
	t0 = std::chrono::steady_clock::now();
	if (r_game.audio != nullptr && r_game.audio->built()) {
		r_game.audio->step(mf, cues, effect_sounds);
	}
	r_game.ms_audio = ms_since(t0);
}

}

bool Bf3LevelSession::game_frame(double p_delta, uint64_t p_now_ns) {
	if (!game_) {
		return false;
	}
	Game &g = *game_;
	++g.frames;
	const auto frame_t0 = std::chrono::steady_clock::now();
	using bf3::core::ms_since;
	const host::PresentationFrame &f = *frame_;
	if (!g.built) {
		for (const std::string &n : g.part_notes) {
			UtilityFunctions::print(to_godot(n));
		}
		g.part_notes.clear();
		build_game_ui(this, g, !headless_);
		g.base_sensitivity = mapper_.mouse_sensitivity;
		if (!g.hud_static_error.empty()) {
			UtilityFunctions::print(to_godot("[game] HUD feed without weapon rows: " + g.hud_static_error));
		}
	}

	if (!g.vm_built && g.vm_done.load(std::memory_order_acquire)) {
		g.vm_built = true;
		if (g.vm_loader.joinable()) {
			g.vm_loader.join();
		}
		if (!g.vm_error.empty()) {
			UtilityFunctions::print(to_godot("[game] no viewmodel: " + g.vm_error));
		} else {
			map_viewmodel_items(g);
			for (size_t t = 0; t < 2; ++t) {
				Bf3Viewmodel *vm = memnew(Bf3Viewmodel);
				vm->set_name(t == 0 ? "ViewmodelUS" : "ViewmodelRU");
				add_child(vm);
				if (!vm->build(*g.vm_sets[t], g.vm_tuning)) {
					UtilityFunctions::print(to_godot(std::string("[game] viewmodel ") + (t == 0 ? "US" : "RU") + ": the set has no items or no CameraJoint"));
					vm->queue_free();
					continue;
				}
				vm->set_visible(false);
				g.vm[t] = vm;
			}
			char line[256];
			std::snprintf(line, sizeof(line), "[game] viewmodel sets in %.2f s: US %zu items, RU %zu items; %zu + %zu defs mapped to items (tuning %s)", g.vm_seconds,
					g.vm_sets[0]->items.size(), g.vm_sets[1]->items.size(), g.def_items[0].size(), g.def_items[1].size(), g.vm_tuning_source.c_str());
			UtilityFunctions::print(line);
		}
	}

	if (g.frames == 1 || g.frames == 60) {
		RenderingServer *rs_info = RenderingServer::get_singleton();
		if (rs_info != nullptr) {
			const double to_mib = 1.0 / (1024.0 * 1024.0);
			char line[256];
			std::snprintf(line, sizeof(line), "[game] video memory at frame %llu after Done: used %.1f MiB (textures %.1f MiB, buffers %.1f MiB)",
					static_cast<unsigned long long>(g.frames), double(rs_info->get_rendering_info(RenderingServer::RENDERING_INFO_VIDEO_MEM_USED)) * to_mib,
					double(rs_info->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED)) * to_mib,
					double(rs_info->get_rendering_info(RenderingServer::RENDERING_INFO_BUFFER_MEM_USED)) * to_mib);
			UtilityFunctions::print(line);
		}
	}
	if (g.hud_items != nullptr && g.hud_items->is_loaded() && !g.hud_names_applied) {
		g.hud_names_applied = true;
		const size_t filled = g.hud_items->item_index() != nullptr ? host::apply_item_names(g.hud_static, *g.hud_items->item_index()) : 0;
		const size_t items = g.hud_items->item_index() != nullptr
				? host::apply_item_table(g.hud_static, *g.hud_items->item_index())
				: 0;
		UtilityFunctions::print(to_godot("[game] HUD items loaded" +
				(g.hud_items->load_error().empty() ? std::string() : " (" + g.hud_items->load_error() + ")") + ": " +
				std::to_string(g.hud_static.rows.size()) + " weapon rows, " + std::to_string(filled) +
				" names from the item index, " + std::to_string(items) + " items, " +
				std::to_string(g.hud_items->icon_count()) + " icons"));
	}
	const bool free_camera = mapper_.free_camera();

	Camera3D *camera = cameras_.first_person();
	Bf3Viewmodel *own_vm = g.own_viewmodel();
	if (g.shown_team != g.own_team) {
		for (size_t t = 0; t < 2; ++t) {
			if (g.vm[t] != nullptr && int(t + 1) != g.own_team) {
				g.vm[t]->select_item(-1);
				g.vm[t]->set_visible(false);
			}
		}
		g.shown_team = g.own_team;
	}
	g.cues.clear();
	const ViewAngles &view = mapper_.view();
	const float dt = float(std::clamp(p_delta, 1e-4, 0.25));
	const float yaw_rate = g.prev_view_valid ? float((double(view.yaw) - g.prev_yaw) / double(dt)) : 0.0f;
	const float pitch_rate = g.prev_view_valid ? float((double(view.pitch) - g.prev_pitch) / double(dt)) : 0.0f;
	auto part_t0 = std::chrono::steady_clock::now();
	g.prev_yaw = view.yaw;
	g.prev_pitch = view.pitch;
	g.prev_view_valid = true;
	if (own_vm != nullptr && camera != nullptr) {
		const bool show = g.alive && g.weapon.valid && !free_camera;
		if (show) {
			const int item = g.item_of(g.own_team, g.weapon.def);
			if (item < 0) {
				++g.vm_unmapped;
			}
			if (item != own_vm->item()) {
				own_vm->select_item(item);
			}
			g.vm_item = item;
			viewmodel_cues_from(g.events, g.weapon.magazine, g.cues);
			for (const bf3::anim::VmCue &c : g.cues) {
				g.vm_shot_cues += c.kind == bf3::anim::VmCue::Kind::Shot ? 1 : 0;
			}
			const bool sprinting = (f.own_curr.flags & sim::soldier_flags::kSprinting) != 0;
			const float speed = std::hypot(f.own_curr.velocity[0], f.own_curr.velocity[2]);
			ViewmodelFrame vf = viewmodel_frame_from(g.weapon, cameras_.last_eye(), view.yaw, view.pitch, dt, sprinting, speed, yaw_rate, pitch_rate);
			vf.grounded = (f.own_curr.flags & sim::soldier_flags::kGrounded) != 0;
			own_vm->present(vf, g.cues, camera);
			mapper_.mouse_sensitivity = g.base_sensitivity * std::clamp(own_vm->look_scale(), 0.05f, 4.0f);
		} else {
			mapper_.mouse_sensitivity = g.base_sensitivity;
		}
		own_vm->set_visible(show && own_vm->item() >= 0);
	}
	g.ms_vm = ms_since(part_t0);
	part_t0 = std::chrono::steady_clock::now();

	if (g.native_hud != nullptr) {
		host::HudFeedInput in;
		in.weapon = g.weapon;
		in.weapons = &f.own_weapons_curr;
		in.alive = g.alive;
		in.health = f.own_curr.health;
		in.view_yaw_rad = view.yaw;
		in.pose = f.own_curr.pose;
		in.now_s = double(p_now_ns) * 1e-9;
		in.damage_own_slot = g.own_slot;
		in.own_position = f.own_curr.position;
		std::array<host::PresentedSoldier, bf3::net::kMaxSlots> hud_others{};
		const size_t hud_other_count = f.valid ? host::present_others(f, p_now_ns, hud_others) : 0;
		in.others = std::span<const host::PresentedSoldier>(hud_others.data(), hud_other_count);
		host::fill_hud(in, g.events, g.hud_static, g.hud_state, g.hud_model);
		g.native_hud->set_model(g.hud_model);
		if (camera != nullptr) {
			g.native_hud->set_vfov_deg(camera->get_fov());
		}
		const Vector3 eye = cameras_.last_eye();
		NativeHudMinimapPose pose;
		pose.x = eye.x;
		pose.z = eye.z;
		pose.yaw = view.yaw;
		pose.valid = true;
		g.native_hud->set_minimap_pose(pose);
		g.native_hud->set_world(this, free_camera ? nullptr : camera, !free_camera && g.alive);
		if (!g.native_hud->fixture()) {
			g.native_hud->set_visible(!free_camera && g.alive);
		}
	}
#if OPENBF3_BUILD_CHECKS
	if (g.hud_moments != nullptr && g.native_hud != nullptr && !g.finished && g.hud_moments->step(*this, *g.native_hud, p_delta)) {
		g.finished = true;
		UtilityFunctions::print(to_godot(std::string("NATIVE HUD MOMENTS: ") + (g.hud_moments->exit_code() == 0 ? "PASS" : "FAIL")));
		finish(g.hud_moments->exit_code());
		return true;
	}
	if (g.crosshair_walk != nullptr && g.native_hud != nullptr && !g.finished) {
		CrosshairWalkInputs in;
		in.alive = g.alive;
		in.own_shots = g.own_shots_seen;
		in.weapon = &g.weapon;
		in.table = g.gunplay != nullptr ? g.gunplay->table.get() : nullptr;
		if (g.crosshair_walk->step(*this, *g.native_hud, in, p_delta)) {
			g.finished = true;
			UtilityFunctions::print(to_godot(std::string("CROSSHAIR WALK: ") + (g.crosshair_walk->exit_code() == 0 ? "PASS" : "FAIL")));
			finish(g.crosshair_walk->exit_code());
			return true;
		}
	}
#endif
	g.ms_hud = ms_since(part_t0);
	if (tdm_frame(p_now_ns, p_delta, camera, free_camera)) {
		return true;
	}

	const double render_ticks = host::present_render_ticks(f, p_now_ns);
	std::array<host::PresentedOtherWeapon, bf3::net::kMaxSlots> others{};
	const size_t n_others = f.valid ? host::present_other_weapons(f, render_ticks, others) : 0;
	for (size_t i = 0; i < n_others; ++i) {
		if (others[i].valid && (others[i].team == 1 || others[i].team == 2)) {
			g.slot_team[others[i].slot] = others[i].team;
		}
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (scene_ && rs != nullptr) {
		for (size_t i = 0; i < other_instances_.size(); ++i) {
			if (g.crowd_built) {
				rs->instance_set_visible(other_instances_[i], false);
				continue;
			}
			const uint8_t team = i < n_others ? others[i].team : uint8_t(0);
			const size_t tint = team == 0 ? 0 : (team == g.own_team ? 1 : 2);
			if (g.tints[tint].is_valid()) {
				rs->instance_geometry_set_material_override(other_instances_[i], g.tints[tint]->get_rid());
			}
		}
	}
	step_game_parts(*this, g, f, p_now_ns, p_delta, camera, own_vm, free_camera, others, n_others);
	g.ms_total = ms_since(frame_t0);

	if (hud_ != nullptr && done_ && hud_timer_ == 0.0 && g.firefight_bots > 0) {
		const uint32_t shown = std::min<uint32_t>(f.other_count, g.firefight_bots + uint32_t(walkers_));
		hud_->set_text(get_status_text() + to_godot(" | firefight bots " + std::to_string(g.firefight_bots) + (g.firefight_pairs ? " (bt, pairs)" : " (bt, free)") +
				", others in the snapshot " + std::to_string(shown)));
	}

	const Vector2 vp = get_viewport() != nullptr ? get_viewport()->get_visible_rect().size : Vector2(1280, 720);
	if (g.score_label != nullptr) {
		char line[256];
		std::snprintf(line, sizeof(line), "kills %u   deaths %u      US %u : RU %u", g.kills, g.deaths, g.team_kills[1], g.team_kills[2]);
		g.score_label->set_text(line);
		g.score_label->set_position(Vector2(vp.x - 520.0f, 8.0f));
		g.score_label->set_size(Vector2(510.0f, 24.0f));
	}
	if (g.kit_label != nullptr) {
		char line[256];
		std::snprintf(line, sizeof(line), "%s %s   next spawn: %s %s (K kit, T team)%s", team_name(g.own_team),
				host::kit_role_name(host::KitRole(std::min<uint8_t>(f.own_kit, host::kKitCount - 1))), host::kit_role_name(host::KitRole(g.next_kit)),
				team_name(g.next_team), g.vm_done.load(std::memory_order_acquire) ? "" : "   (loading the viewmodel)");
		g.kit_label->set_text(line);
		g.kit_label->set_position(Vector2(vp.x - 620.0f, 30.0f));
		g.kit_label->set_size(Vector2(610.0f, 22.0f));
	}
	if (g.death_label != nullptr) {
		g.death_label->set_visible(g.dead && !free_camera);
		if (g.dead) {
			const double left = std::max(0.0, g.respawn_s - double(p_now_ns - std::min(p_now_ns, g.death_ns)) * 1e-9);
			std::string who;
			if (g.killed_by == 0xFF) {
				who = "YOU DIED";
			} else if (g.killed_by_self) {
				who = "YOU DIED (fall)";
			} else {
				who = "KILLED BY slot " + std::to_string(g.killed_by) + " (" + team_name(g.slot_team[g.killed_by]) + ")";
			}
			char line[256];
			std::snprintf(line, sizeof(line), "%s\nrespawn in %.1f s", who.c_str(), left);
			g.death_label->set_text(to_godot(line));
			g.death_label->set_position(Vector2(0, vp.y * 0.30f));
			g.death_label->set_size(Vector2(vp.x, 90.0f));
		}
	}

#if OPENBF3_BUILD_CHECKS
	if (g.checks() && g.check == nullptr && !g.finished && (g.vm_built || !scene_) && g.hud_ready()) {
		g.check = std::make_unique<M2aCheck>(*this, g, g.cases, g.check_options);
	}
	if (g.check != nullptr && !g.finished) {
		if (g.check->frame(p_now_ns, p_delta)) {
			g.finished = true;
			UtilityFunctions::print(to_godot(g.check->report()));
			finish(g.check->exit_code());
		}
		return true;
	}
#endif
	return g.checks();
}

void Bf3LevelSession::game_shutdown() {
	if (!game_) {
		return;
	}
	Game &g = *game_;
	if (g.vm_loader.joinable()) {
		g.vm_loader.join();
	}
	if (g.crowd_loader.joinable()) {
		g.crowd_loader.join();
	}
#if OPENBF3_BUILD_CHECKS
	g.check.reset();
#endif
	tdm_shutdown();
	if (g.crowd != nullptr) {
		g.crowd->shutdown();
	}
	if (g.fx != nullptr) {
		g.fx->shutdown();
	}
	if (g.audio != nullptr) {
		g.audio->shutdown();
	}
	if (g.recorder != nullptr) {
		g.recorder->watch_slot(-1);
	}
}

const sim::CollisionWorld *Bf3LevelSession::collision_world() const {
	return game_ ? game_->world.get() : nullptr;
}

Bf3UiAssets *Bf3LevelSession::ui_assets() const {
	return game_ ? game_->ui_assets : nullptr;
}

}
