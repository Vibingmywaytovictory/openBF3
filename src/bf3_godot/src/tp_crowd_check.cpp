// Bf3TpCrowdCheck: tp's check of Bf3GameCrowd in the level (M2a tp; docs/design/m2a-tp.md). See tp_crowd_check.h.
#include "tp_crowd_check.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"

#include "bf3/anim/crowd_stage.h"
#include "bf3/core/install.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/weapons/sim_time.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr double kStep = 1.0 / 60.0; // the scripted clock advances one fixed step per display frame
constexpr double kEnd = 9.0; // seconds of script
constexpr double kDeathAt = 2.0; // soldier 9 dies (Death event), its snapshots stop 0.1 s later
constexpr double kRespawnAt = 7.0; // soldier 9 back with a new generation
constexpr double kSwitchAt = 6.0; // soldier 0 switches M16A4 -> M9

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[tp] " + p_line));
}

std::string fmt(const char *p_format, ...) {
	char buf[512];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

std::filesystem::path utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string path_text(const std::filesystem::path &p_path) {
	const std::u8string u = p_path.u8string();
	return std::string(u.begin(), u.end());
}

sim::weapons::SimTime sim_time(double p_seconds) {
	return sim::weapons::SimTime(std::llround(p_seconds * double(sim::weapons::kTimeUnitsPerSecond)));
}

} // namespace

// The twelve scripted soldiers.
struct Bf3TpCrowdCheck::Script {
	struct Soldier {
		uint8_t slot = 0;
		uint8_t team = 1; // wire team
		const char *variant = "";
		uint16_t def = sim::weapons::kNoWeapon;
		sim::Pose pose = sim::Pose::Stand;
		bool firearm = true;
		anim::Vec3 feet{};
		float yaw = 0.0f;
	};
	anim::CrowdStageSpec stage;
	std::vector<Soldier> soldiers;
	uint16_t m9 = sim::weapons::kNoWeapon;
	uint32_t unknown_defs = 0;
};

Bf3TpCrowdCheck::Bf3TpCrowdCheck() = default;

Bf3TpCrowdCheck::~Bf3TpCrowdCheck() {
	shutdown();
}

void Bf3TpCrowdCheck::_bind_methods() {}

void Bf3TpCrowdCheck::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		shutdown();
	}
}

void Bf3TpCrowdCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	check_ = has_user_arg("tp-check");
	shot_dir_ = user_arg_value("tp-shots");
	shots_ = has_user_arg("tp-shots") && DisplayServer::get_singleton()->get_name() != String("headless");
	if (shots_ && shot_dir_.empty()) {
		const std::filesystem::path project = utf8_path(to_std(ProjectSettings::get_singleton()->globalize_path("res://")));
		shot_dir_ = path_text((project / "..").lexically_normal() / "b" / "shots" / "tp");
	}
	if (!has_user_arg("no-cache")) {
		const std::string root = user_arg_value("bf3-cache");
		cache_root_ = root.empty() ? default_cache_root() : utf8_path(root);
	}
	session_ = Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
	if (session_ == nullptr) {
		UtilityFunctions::push_error("[tp] Bf3TpCrowdCheck needs a sibling Bf3LevelSession named Session");
		return;
	}
	log(std::string("crowd check: ") + (check_ ? "--tp-check" : "interactive") + (shots_ ? ", shots " + shot_dir_ : ", no shots"));
	set_process(true);
}

void Bf3TpCrowdCheck::start_loader() {
	started_ = true;
	shared_ = session_->shared_level();
	const core::InstallReader &install = session_->install();
	const core::PathGuard guard = install.write_guard();
	loader_ = std::thread([this, &install, guard]() {
		if (!cache_root_.empty()) {
			AcquiredCache acquired = acquire_cache(cache_root_, guard);
			if (acquired.status == core::CacheStatus::Ok) {
				cache_ = acquired.cache;
			}
		}
		core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(install);
		if (!opened) {
			load_error_ = opened.error().message;
			load_done_.store(true, std::memory_order_release);
			return;
		}
		db_ = std::make_unique<host::LevelDatabase>();
		db_->install = *opened;
		// The session's SharedLevel carries the gunplay tables once gx turns SharedLoadOptions::gunplay on; until then the
		// check loads them itself (the kits name the crowd's items).
		std::shared_ptr<const host::SharedLevel> shared = shared_;
		if (shared && !shared->gunplay) {
			core::Result<std::shared_ptr<const host::GunplayLevel>> g = host::load_gunplay_level(*opened, shared->level, shared->mode);
			if (g) {
				auto copy = std::make_shared<host::SharedLevel>(*shared);
				copy->gunplay = *g;
				shared = copy;
			} else {
				log("gunplay level not loaded: " + g.error().message + " (item 0 only)");
			}
		}
		shared_ = shared;
		M2aLoadInputs in;
		in.db = db_.get();
		in.shared = shared_;
		in.cache = cache_.get();
		in.stop = &stop_;
		const auto t0 = std::chrono::steady_clock::now();
		const core::Status st = crowd_.load(in);
		if (!st.ok()) {
			load_error_ = st.error().message;
		} else {
			log(fmt("crowd loaded in %.2f s (branch, unlocked, shared machine: indicative only)",
					std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
		}
		load_done_.store(true, std::memory_order_release);
	});
}

void Bf3TpCrowdCheck::scripted_frame(double p_t, std::vector<host::PresentedSoldier> &r_others, std::vector<host::PresentedOtherWeapon> &r_weapons,
		std::vector<host::PresentEvent> &r_events) {
	const Script &s = *script_;
	r_others.clear();
	r_weapons.clear();
	r_events.clear();
	const double prev = p_t - kStep;
	auto crossed = [&](double p_at) { return prev < p_at && p_at <= p_t; };
	auto event = [&](host::PresentKind p_kind, uint8_t p_slot, uint8_t p_aux, double p_time) {
		host::PresentEvent e;
		e.kind = p_kind;
		e.timeline = host::PresentTimeline::Render;
		e.slot = p_slot;
		e.aux = p_aux;
		e.time = sim_time(p_time);
		r_events.push_back(e);
	};
	for (size_t k = 0; k < s.soldiers.size(); ++k) {
		const Script::Soldier &d = s.soldiers[k];
		const bool victim = k == 9;
		if (victim && p_t > kDeathAt + 0.1 && p_t < kRespawnAt) {
			continue; // the server no longer sends a dead soldier
		}
		host::PresentedSoldier o;
		o.slot = d.slot;
		o.generation = victim && p_t >= kRespawnAt ? 2 : 1;
		o.position = sim::Vec3{ d.feet.x, d.feet.y, d.feet.z };
		o.yaw = d.yaw;
		o.pitch = 0.0f;
		o.pose = d.pose;
		o.target_pose = d.pose;
		host::PresentedOtherWeapon w;
		w.valid = true;
		w.slot = d.slot;
		w.generation = o.generation;
		w.def = d.def;
		w.team = d.team;
		w.sample_tick = uint32_t(p_t * 60.0);
		switch (k) {
			case 0: // stand idle; switches to the M9 at kSwitchAt
				if (p_t >= kSwitchAt && s.m9 != sim::weapons::kNoWeapon) {
					w.def = s.m9;
				}
				break;
			case 4: // reloads at 1 s and 5 s
				w.reload_seq = uint8_t((p_t >= 1.0 ? 1 : 0) + (p_t >= 5.0 ? 1 : 0));
				break;
			case 5: // crouch, moving forward at 1.5 m/s (in place: the animation only)
				o.velocity = sim::Vec3{ std::sin(d.yaw) * 1.5f, 0.0f, std::cos(d.yaw) * 1.5f };
				break;
			case 6: // aims up 30 degrees
				o.pitch = 0.52f;
				break;
			case 8: { // stand -> prone over 1 s, prone 2 s, prone -> stand over 1 s, stand 2 s (a 6 s cycle)
				const double c = std::fmod(p_t, 6.0);
				if (c < 2.0) {
					o.pose = o.target_pose = sim::Pose::Stand;
				} else if (c < 3.0) {
					o.pose = sim::Pose::Stand;
					o.target_pose = sim::Pose::Prone;
					o.transition = float(c - 2.0);
				} else if (c < 5.0) {
					o.pose = o.target_pose = sim::Pose::Prone;
				} else {
					o.pose = sim::Pose::Prone;
					o.target_pose = sim::Pose::Stand;
					o.transition = float(c - 5.0);
				}
				break;
			}
			default:
				break;
		}
		r_others.push_back(o);
		r_weapons.push_back(w);
		// Fire: soldier 3 every 0.3 s, soldier 7 every 0.2 s (RemoteShot, sent up to 0.1 s ahead of its render time).
		for (const auto [who, every] : { std::pair{ size_t(3), 0.3 }, std::pair{ size_t(7), 0.2 } }) {
			if (k != who) {
				continue;
			}
			const double ahead = p_t + 0.1;
			const double next = std::floor(ahead / every) * every;
			if (next > prev + 0.1 && next <= ahead && next > 0.4) {
				event(host::PresentKind::RemoteShot, d.slot, 0, next);
			}
		}
	}
	if (crossed(kDeathAt - 0.05)) {
		event(host::PresentKind::Death, s.soldiers[3].slot, s.soldiers[9].slot, kDeathAt); // killed by soldier 3
	}
}

void Bf3TpCrowdCheck::check_at(double p_t) {
	const GameCrowdStats st = crowd_.stats();
	auto row = [&](const std::string &p_name, bool p_ok, const std::string &p_measured) {
		rows_.push_back(fmt("%-44s %s  %s", p_name.c_str(), p_ok ? "PASS" : "FAIL", p_measured.c_str()));
		if (!p_ok) {
			failures_.push_back(p_name);
		}
	};
	const auto &soldiers = script_->soldiers;
	if (std::fabs(p_t - 1.5) < kStep * 0.5) {
		row("t 1.5 s: 12 soldiers drawn", st.soldiers == 12, fmt("%u", st.soldiers));
		row("t 1.5 s: 12 weapon instances", st.weapon_instances == 12, fmt("%u", st.weapon_instances));
		row("t 1.5 s: 10 muzzles (no knife, no M67)", st.muzzles == 10, fmt("%u", st.muzzles));
		row("t 1.5 s: unknown weapons 0", st.unknown_weapons == 0 && script_->unknown_defs == 0, fmt("%u (script defs missing %u)", st.unknown_weapons, script_->unknown_defs));
		// Soldier 0 (stand, pitch 0): the muzzle sits ahead of the soldier at chest height, its effect +Y along the view.
		bool found = false;
		for (const fx::MuzzleSample &m : crowd_.muzzles()) {
			if (m.slot != soldiers[0].slot || m.first_person) {
				continue;
			}
			found = true;
			const anim::Vec3 f{ std::sin(soldiers[0].yaw), 0.0f, std::cos(soldiers[0].yaw) };
			const float along = m.transform.up.x * f.x + m.transform.up.y * f.y + m.transform.up.z * f.z;
			const anim::Vec3 d{ m.transform.trans.x - soldiers[0].feet.x, m.transform.trans.y - soldiers[0].feet.y, m.transform.trans.z - soldiers[0].feet.z };
			const float ahead = d.x * f.x + d.z * f.z;
			row("t 1.5 s: soldier 0 muzzle +Y along the view", along > 0.6f, fmt("cos %.3f", double(along)));
			row("t 1.5 s: soldier 0 muzzle ahead, chest height", ahead > 0.3f && ahead < 1.6f && d.y > 0.9f && d.y < 1.7f,
					fmt("ahead %.2f m, height %.2f m", double(ahead), double(d.y)));
		}
		if (!found) {
			row("t 1.5 s: soldier 0 muzzle", false, "none");
		}
	} else if (std::fabs(p_t - 2.4) < kStep * 0.5) {
		row("t 2.4 s: the dying soldier still drawn", st.soldiers == 12, fmt("%u", st.soldiers));
	} else if (std::fabs(p_t - 5.5) < kStep * 0.5) {
		row("t 5.5 s: the dead soldier hidden after 3 s", st.soldiers == 11, fmt("%u", st.soldiers));
		row("t 5.5 s: 9 muzzles", st.muzzles == 9, fmt("%u", st.muzzles));
	} else if (std::fabs(p_t - 8.0) < kStep * 0.5) {
		row("t 8.0 s: the respawned soldier drawn", st.soldiers == 12, fmt("%u", st.soldiers));
		row("t 8.0 s: 10 muzzles", st.muzzles == 10, fmt("%u", st.muzzles));
	}
}

void Bf3TpCrowdCheck::_process(double p_delta) {
	(void)p_delta;
	if (done_ || session_ == nullptr) {
		return;
	}
	if (session_->failed()) {
		log("session failed");
		finish(1);
		return;
	}
	if (!session_->done()) {
		return;
	}
	if (!started_) {
		start_loader();
		return;
	}
	if (!load_done_.load(std::memory_order_acquire)) {
		return;
	}
	if (!built_) {
		if (loader_.joinable()) {
			loader_.join();
		}
		if (!load_error_.empty()) {
			log("load failed: " + load_error_);
			finish(1);
			return;
		}
		std::string err;
		if (!crowd_.build(this, &err)) {
			log("build failed: " + err);
			finish(1);
			return;
		}
		built_ = true;
		// The script on S3's validated stage (s3.md §5.8): two rows of six cells, the soldiers turned toward the camera.
		script_ = std::make_unique<Script>();
		Script &s = *script_;
		s.stage.rows = 2;
		s.stage.cols = 6;
		s.stage.camera_back = 6.0f;
		s.stage.camera_up = 1.6f;
		s.stage.target_height = 0.8f;
		const host::GunplayLevel *g = shared_ ? shared_->gunplay.get() : nullptr;
		auto def_of = [&](const char *p_variant) -> uint16_t {
			if (g != nullptr) {
				for (size_t d = 0; d < g->defs.size(); ++d) {
					if (g->defs[d].variant == p_variant) {
						return uint16_t(d);
					}
				}
			}
			++s.unknown_defs;
			return sim::weapons::kNoWeapon;
		};
		const struct {
			uint8_t team;
			const char *variant;
			sim::Pose pose;
			bool firearm;
		} kSoldiers[12] = {
			{ 1, "weapons/m16a4/u_m16a4", sim::Pose::Stand, true }, { 1, "weapons/m4a1/u_m4a1", sim::Pose::Crouch, true },
			{ 1, "weapons/m27iar/u_m27iar", sim::Pose::Prone, true }, { 1, "weapons/mk11/u_mk11", sim::Pose::Stand, true },
			{ 1, "weapons/m9/u_m9", sim::Pose::Stand, true }, { 1, "weapons/m16a4/u_m16a4", sim::Pose::Crouch, true },
			{ 2, "weapons/ak74m/u_ak74m", sim::Pose::Stand, true }, { 2, "weapons/aks74u/u_aks74u", sim::Pose::Crouch, true },
			{ 2, "weapons/rpk/u_rpk-74m", sim::Pose::Stand, true }, { 2, "weapons/svd/u_svd", sim::Pose::Stand, true },
			{ 2, "weapons/knife/u_knife", sim::Pose::Stand, false }, { 2, "weapons/m67/u_m67", sim::Pose::Stand, false },
		};
		const float face = s.stage.heading + 3.14159265f;
		for (uint32_t i = 0; i < 12; ++i) {
			Script::Soldier d;
			d.slot = uint8_t(10 + i);
			d.team = kSoldiers[i].team;
			d.variant = kSoldiers[i].variant;
			d.def = def_of(kSoldiers[i].variant);
			d.pose = kSoldiers[i].pose;
			d.firearm = kSoldiers[i].firearm;
			d.feet = anim::stage_cell(s.stage, i);
			d.yaw = face - 0.6f;
			s.soldiers.push_back(d);
		}
		s.m9 = def_of("weapons/m9/u_m9");
		camera_ = memnew(Camera3D);
		camera_->set_name("TpCrowdCamera");
		camera_->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
		camera_->set_near(0.05f);
		camera_->set_far(500.0f);
		add_child(camera_);
		const anim::StageCamera sc = anim::stage_camera(s.stage);
		camera_->set_fov(sc.fov_y_deg);
		camera_->look_at_from_position(Vector3(sc.position.x, sc.position.y, sc.position.z), Vector3(sc.target.x, sc.target.y, sc.target.z), Vector3(0, 1, 0));
		camera_->make_current();
		return;
	}
	// One scripted step.
	t_ += kStep;
	++frame_;
	std::vector<host::PresentedSoldier> others;
	std::vector<host::PresentedOtherWeapon> weapons;
	std::vector<host::PresentEvent> events;
	scripted_frame(t_, others, weapons, events);
	M2aFrame f;
	f.clock.render_valid = true;
	f.clock.render = sim_time(t_);
	f.clock.predicted_valid = true;
	f.clock.predicted = sim_time(t_);
	f.dt = kStep;
	f.events = events;
	f.others = others;
	f.other_weapons = weapons;
	const anim::StageCamera sc = anim::stage_camera(script_->stage);
	f.camera.trans = fx::Vec3{ sc.position.x, sc.position.y, sc.position.z };
	crowd_.step(f);
	const GameCrowdStats st = crowd_.stats();
	if (t_ > kSwitchAt && t_ < kSwitchAt + 0.2 && st.weapon_rebases > 0) {
		rebases_after_switch_ += st.weapon_rebases;
	}
	check_at(t_);
	if (shots_) {
		for (const double at : { 1.5, 2.25, 2.6, 6.5 }) {
			if (std::fabs(t_ - at) < kStep * 0.5) {
				const std::string path = path_text(utf8_path(shot_dir_) / ("tp_crowd_" + std::to_string(shot_index_++) + ".png"));
				log(std::string("shot ") + path + (session_->save_screenshot(path) ? "" : " FAILED"));
			}
		}
	}
	if (t_ >= kEnd) {
		const GameCrowdStats end = crowd_.stats();
		rows_.push_back(fmt("%-44s %s  %.0f", "t 6.0 s: the weapon switch rebases", rebases_after_switch_ >= 1 ? "PASS" : "FAIL", rebases_after_switch_));
		if (rebases_after_switch_ < 1) {
			failures_.push_back("rebase");
		}
		rows_.push_back(fmt("%-44s %s  %llu of %llu", "no frozen frame", end.frozen_frames == 0 ? "PASS" : "FAIL", (unsigned long long)end.frozen_frames,
				(unsigned long long)end.frames));
		if (end.frozen_frames != 0) {
			failures_.push_back("frozen");
		}
		rows_.push_back(fmt("%-44s %s  %u", "pose lanes (crowd_lanes(hc, true))", end.lanes >= 2 ? "PASS" : "FAIL", end.lanes));
		for (const std::string &r : rows_) {
			log(r);
		}
		const bool pass = failures_.empty() && rows_.size() >= 14;
		log(std::string("M2A TP CROWD CHECK: ") + (pass ? "PASS" : "FAIL"));
		finish(check_ ? (pass ? 0 : 1) : 0);
	}
}

void Bf3TpCrowdCheck::finish(int p_code) {
	if (done_) {
		return;
	}
	done_ = true;
	shutdown();
	if (session_ != nullptr) {
		session_->finish(p_code);
	}
}

void Bf3TpCrowdCheck::shutdown() {
	if (shut_down_) {
		return;
	}
	shut_down_ = true;
	stop_.store(true, std::memory_order_relaxed);
	if (loader_.joinable()) {
		loader_.join();
	}
	crowd_.shutdown();
	db_.reset();
	cache_.reset();
}

} // namespace bf3::godot_side

// Called from register_types.cpp (M2a tp named insertion), as hud_controls.cpp registers the M2a hud classes.
void openbf3_register_m2a_tp() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3TpCrowdCheck);
}
