#include "tp_crowd_check.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/crowd_stage.h"
#include "bf3/core/gameplay/death_body.h"
#include "bf3/core/install.h"
#include "bf3/core/util/format.h"
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

constexpr double kStep = 1.0 / 60.0;
constexpr double kEnd = 9.0;
constexpr double kDeathAt = 2.0;
constexpr double kRespawnAt = 7.0;
constexpr double kSwitchAt = 6.0;
constexpr double kHoldAt = 3.0;
constexpr double kThrowAt = 3.6;
constexpr size_t kThrower = 11;
constexpr double kReleaseLeaveTick = 115.0;
constexpr float kCorpseSeconds = 4.0f;
constexpr double kHiddenAt = kDeathAt + double(kCorpseSeconds) + 0.2;
constexpr double kReviveAt = kDeathAt + double(kCorpseSeconds) - 0.03;
constexpr size_t kRevived = 9;
constexpr size_t kReviver = 6;

struct Kill {
	size_t victim;
	size_t killer;
	core::gameplay::DeathHitDirection direction;
	bool explosion;
	const char *name;
};

constexpr Kill kKills[] = {
	{ 9, 3, core::gameplay::DeathHitDirection::Front, false, "standing, shot from the front" },
	{ 1, 6, core::gameplay::DeathHitDirection::Front, false, "crouching" },
	{ 2, 7, core::gameplay::DeathHitDirection::Front, false, "prone" },
	{ 10, 0, core::gameplay::DeathHitDirection::Back, false, "standing, shot from behind" },
	{ 4, kThrower, core::gameplay::DeathHitDirection::Left, true, "standing, killed by an explosion" },
};

bool killed(size_t p_k) {
	return std::any_of(std::begin(kKills), std::end(kKills), [&](const Kill &p_kill) { return p_kill.victim == p_k; });
}

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[tp] " + p_line));
}

using bf3::core::fmt;

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

}

struct Bf3TpCrowdCheck::Script {
	struct Soldier {
		uint8_t slot = 0;
		uint8_t team = 1;
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
		GameLoadInputs in;
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

bool pt1_run_mode();
void pt1_run_soldier(size_t p_k, host::PresentedSoldier &r_o);
bool pt1_run_shot_at(double p_t, double p_step);

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
		const bool victim = killed(k);
		const bool revived = k == kRevived && p_t >= kReviveAt;
		if (victim && !revived && p_t > kDeathAt + 0.1 && p_t < kRespawnAt) {
			continue;
		}
		host::PresentedSoldier o;
		o.slot = d.slot;
		o.generation = victim && (revived || p_t >= kRespawnAt) ? 2 : 1;
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
			case 0:
				if (p_t >= kSwitchAt && s.m9 != sim::weapons::kNoWeapon) {
					w.def = s.m9;
				}
				break;
			case 4:
				w.reload_seq = uint8_t((p_t >= 1.0 ? 1 : 0) + (p_t >= 5.0 ? 1 : 0));
				break;
			case 5:
				o.velocity = sim::Vec3{ std::sin(d.yaw) * 1.5f, 0.0f, std::cos(d.yaw) * 1.5f };
				break;
			case 6:
				o.pitch = 0.52f;
				break;
			case kThrower:
				if (p_t >= kHoldAt && p_t < kThrowAt) {
					w.phase = sim::weapons::WeaponPhase::Holding;
				}
				break;
			case 8: {
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
		if (pt1_run_mode()) {
			pt1_run_soldier(k, o);
		}
		r_others.push_back(o);
		r_weapons.push_back(w);

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
		for (const Kill &kill : kKills) {
			event(host::PresentKind::Death, s.soldiers[kill.killer].slot, s.soldiers[kill.victim].slot, kDeathAt);
			core::gameplay::DeathRecord record;
			record.valid = true;
			record.direction = kill.direction;
			record.explosion = kill.explosion;
			present::set_death_record(r_events.back(), record);
		}
	}
	if (crossed(kReviveAt)) {
		event(host::PresentKind::Revive, s.soldiers[kReviver].slot, s.soldiers[kRevived].slot, kReviveAt);
	}
	if (crossed(kThrowAt - 0.05)) {
		event(host::PresentKind::RemoteShot, s.soldiers[kThrower].slot, 0, kThrowAt);
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
		row("t 1.5 s: no soldier throwing", st.throwing == 0, fmt("%u", st.throwing));
		row("t 1.5 s: unknown weapons 0", st.unknown_weapons == 0 && script_->unknown_defs == 0, fmt("%u (script defs missing %u)", st.unknown_weapons, script_->unknown_defs));

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
		row("t 2.4 s: the dying soldiers still drawn", st.soldiers == 12, fmt("%u", st.soldiers));
		const GameCrowdCorpse front = crowd_.corpse_of(soldiers[kKills[0].victim].slot);
		row("t 2.4 s: the soldier shot from the front is a ragdoll", front.in_world && st.ragdoll_failures == 0,
				fmt("in the world %d, %u ragdolls, %llu started, %llu failed", int(front.in_world), st.ragdolls,
						(unsigned long long)st.ragdolls_started, (unsigned long long)st.ragdoll_failures));
	} else if (std::fabs(p_t - 3.2) < kStep * 0.5) {
		for (const Kill &kill : kKills) {
			const GameCrowdCorpse c = crowd_.corpse_of(soldiers[kill.victim].slot);
			const bool clip = c.clip != anim::kNoCrowdDeathClip;
			const bool sequence = c.start_age >= 0.0f && c.start_age == c.rise_age;
			const bool first_frame = c.start_age <= float(kStep * 1.5);
			const bool ok = clip ? sequence && c.start_weight < 1.0f && (first_frame || c.start_speed > 0.0f)
								 : sequence && first_frame && c.start_weight == 1.0f;
			row(fmt("t 3.2 s: %s: %s", kill.name,
						clip ? "its death clip, blended into a ragdoll as the clip's RagdollBlend rises"
							 : "no death clip (AQ-DT-1), all ragdoll on the first frame"),
					c.active && ok,
					fmt("clip %u, RagdollBlend rose at %.3f s, ragdoll from %.3f s at weight %.3f and %.2f m/s",
							unsigned(c.clip), double(c.rise_age), double(c.start_age), double(c.start_weight),
							double(c.start_speed)));
		}
		bool handed_off = false;
		for (const Kill &kill : kKills) {
			const GameCrowdCorpse c = crowd_.corpse_of(soldiers[kill.victim].slot);
			handed_off = handed_off ||
					(c.clip != anim::kNoCrowdDeathClip && c.rise_age > float(kStep * 1.5) &&
							c.start_age == c.rise_age && c.start_speed > 0.0f);
		}
		row("t 3.2 s: a death clip played alone, then handed its motion to the ragdoll", handed_off,
				handed_off ? "yes" : "no clip phase before the ragdoll");
		row("t 3.2 s: five ragdolls, none failed",
				st.ragdolls == 5 && st.ragdolls_started == 5 && st.ragdoll_failures == 0,
				fmt("%u in the world, %llu started, %llu failed", st.ragdolls, (unsigned long long)st.ragdolls_started,
						(unsigned long long)st.ragdoll_failures));
	} else if (std::fabs(p_t - (kDeathAt + double(kCorpseSeconds) - 0.05)) < kStep * 0.5) {
		for (const Kill &kill : kKills) {
			const GameCrowdCorpse c = crowd_.corpse_of(soldiers[kill.victim].slot);
			row(fmt("t %.2f s: %s: all ragdoll, lying, frozen", p_t, kill.name),
					c.weight == 1.0f && c.hips_height >= 0.0f && c.hips_height < 0.4f && c.frozen,
					fmt("weight %.3f, hips %.3f m above the death position, frozen at %.2f s", double(c.weight),
							double(c.hips_height), double(c.frozen_age)));
		}
	} else if (std::fabs(p_t - 3.4) < kStep * 0.5) {
		row("t 3.4 s: the holding M67 soldier draws the 3P throw-in", st.throwing == 1,
				fmt("%u throwing", st.throwing));
	} else if (std::fabs(p_t - 4.6) < kStep * 0.5) {
		row("t 4.6 s: the thrown M67 plays the 3P release", st.throwing == 1, fmt("%u throwing", st.throwing));
	} else if (std::fabs(p_t - kHiddenAt) < kStep * 0.5) {
		row(fmt("t %.1f s: four dead soldiers hidden after %.0f s, the revived one drawn", p_t, double(kCorpseSeconds)),
				st.soldiers == 8, fmt("%u", st.soldiers));
		row(fmt("t %.1f s: the hidden corpses' ragdolls removed, the revived one's kept", p_t), st.ragdolls == 1,
				fmt("%u", st.ragdolls));
		row(fmt("t %.1f s: 7 muzzles", p_t), st.muzzles == 7, fmt("%u", st.muzzles));
	} else if (std::fabs(p_t - (kReviveAt + 0.5)) < kStep * 0.5) {
		const GameCrowdCorpse c = crowd_.corpse_of(soldiers[kRevived].slot);
		row(fmt("t %.2f s: revive: the ragdoll blends out, holding at its hips and head", p_t),
				c.reviving && c.in_world && c.weight == 0.0f && c.root_fraction == 1.0f && c.root_offset > 0.05f,
				fmt("blending out %d, weight %.3f, root fraction %.3f, root %.3f m from the soldier", int(c.reviving),
						double(c.weight), double(c.root_fraction), double(c.root_offset)));
	} else if (std::fabs(p_t - (kReviveAt + 1.35)) < kStep * 0.5) {
		const GameCrowdCorpse c = crowd_.corpse_of(soldiers[kRevived].slot);
		row(fmt("t %.2f s: revive: after the 1.0 s hold the root moves to the soldier", p_t),
				c.reviving && c.root_fraction > 0.0f && c.root_fraction < 1.0f,
				fmt("root fraction %.3f, root %.3f m from the soldier", double(c.root_fraction),
						double(c.root_offset)));
	} else if (std::fabs(p_t - (kReviveAt + 1.85)) < kStep * 0.5) {
		const GameCrowdCorpse c = crowd_.corpse_of(soldiers[kRevived].slot);
		row(fmt("t %.2f s: revive: the blend-out ended after 1.7 s, the ragdoll removed", p_t),
				!c.active && st.ragdoll_revives == 1 && st.ragdolls == 0,
				fmt("corpse active %d, %llu blend-outs, %u ragdolls", int(c.active),
						(unsigned long long)st.ragdoll_revives, st.ragdolls));
	} else if (std::fabs(p_t - 8.0) < kStep * 0.5) {
		row("t 8.0 s: the respawned soldier drawn", st.soldiers == 12, fmt("%u", st.soldiers));
		row("t 8.0 s: 10 muzzles", st.muzzles == 10, fmt("%u", st.muzzles));
		row("t 8.0 s: the throw ended", st.throwing == 0, fmt("%u throwing", st.throwing));
		const double branch = kThrowAt + kReleaseLeaveTick / 60.0;
		const double leave = branch + double(anim::kCrowdLayerRampSeconds);
		row("t 8.0 s: the release leaves on End Branch_3P", throw_last_on_ >= branch && throw_last_on_ <= leave + kStep,
				fmt("last drawn at %.4f s; End Branch_3P (tick %.0f) at %.4f s, left after the ramp at %.4f s",
						throw_last_on_, kReleaseLeaveTick, branch, leave));
	}
	if (p_t > kThrowAt && p_t < kThrowAt + 3.0 && st.throwing == 1) {
		throw_last_on_ = p_t;
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
		crowd_.set_corpse_seconds(kCorpseSeconds);
		if (!crowd_.build(this, &err)) {
			log("build failed: " + err);
			finish(1);
			return;
		}
		built_ = true;

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

	t_ += kStep;
	++frame_;
	std::vector<host::PresentedSoldier> others;
	std::vector<host::PresentedOtherWeapon> weapons;
	std::vector<host::PresentEvent> events;
	scripted_frame(t_, others, weapons, events);
	GameFrame f;
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
		for (const double at : { 1.5, 2.25, 2.6, 3.4, 3.9, 4.3, 5.9, 6.5, 7.3 }) {
			if (pt1_run_mode()) {
				break;
			}
			if (std::fabs(t_ - at) < kStep * 0.5) {
				const std::string path = path_text(utf8_path(shot_dir_) / ("tp_crowd_" + std::to_string(shot_index_++) + ".png"));
				log(std::string("shot ") + path + (session_->save_screenshot(path) ? "" : " FAILED"));
			}
		}
		if (pt1_run_mode() && pt1_run_shot_at(t_, kStep)) {
			const std::string path = path_text(utf8_path(shot_dir_) / ("pt1_run_" + std::to_string(shot_index_++) + ".png"));
			log(std::string("shot ") + path + (session_->save_screenshot(path) ? "" : " FAILED"));
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

}

void openbf3_register_m2a_tp() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3TpCrowdCheck);
}

#include "bf3/sim/soldier_state.h"

namespace bf3::godot_side {

bool pt1_run_mode() {
	static const bool on = has_user_arg("pt1-run");
	return on;
}

void pt1_run_soldier(size_t p_k, host::PresentedSoldier &r_o) {
	const float s = std::sin(r_o.yaw), c = std::cos(r_o.yaw);
	switch (p_k) {
		case 1:
			r_o.velocity = sim::Vec3{ s * 4.0f, 0.0f, c * 4.0f };
			break;
		case 2:
			r_o.velocity = sim::Vec3{ s * 6.5f, 0.0f, c * 6.5f };
			r_o.flags = uint8_t(r_o.flags | sim::soldier_flags::kSprinting);
			break;
		case 10:
			r_o.velocity = sim::Vec3{ c * 3.0f, 0.0f, -s * 3.0f };
			break;
		case 11:
			r_o.velocity = sim::Vec3{ s * 2.0f, 0.0f, c * 2.0f };
			r_o.pose = r_o.target_pose = sim::Pose::Crouch;
			break;
		default:
			break;
	}
}

bool pt1_run_shot_at(double p_t, double p_step) {
	for (int i = 0; i <= 12; ++i) {
		if (std::fabs(p_t - (3.0 + 0.05 * i)) < p_step * 0.5) {
			return true;
		}
	}
	return false;
}

}
