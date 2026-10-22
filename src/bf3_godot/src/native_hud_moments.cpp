#include "native_hud_moments.h"

#include "level_session.h"
#include "native_hud.h"
#include "swf_view.h"

#include "bf3/core/ui/hud_widgets.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr std::array<NativeHudMoment, 16> kMoments{ {
		{ "m01", 20, 100, 1, 100, 0, 4, 295, 44.125f, true, -140.0f, -455.0f, 1.470f },
		{ "m02", 25, 217, 1, 100, 9, 12, 212, 95.250f, true, -208.0f, -508.0f, 1.370f },
		{ "m03", 31, 124, 1, 100, 50, 61, 270, 95.250f, true, -251.0f, -500.0f, 1.380f },
		{ "m04", 31, 217, 1, 100, 73, 83, 49, 99.250f, true, -217.0f, -557.0f, 1.310f },
		{ "m05", 30, 217, 1, 100, 74, 83, 82, 97.250f, true, -217.0f, -557.0f, 1.325f },
		{ "m06", 6, 217, 1, 94, 79, 85, 68, 115.375f, true, -129.0f, -535.0f, 1.405f },
		{ "m07", 31, 192, 1, 94, 79, 85, 289, 96.125f, true, -112.0f, -543.0f, 1.230f },
		{ "m08", 21, 192, 1, 94, 79, 87, 286, 72.250f, false, -232.0f, -554.0f, 1.370f },
		{ "m09", 31, 168, 1, 100, 81, 88, 285, 115.250f, true, -222.0f, -558.0f, 1.310f },
		{ "m10", 31, 168, 1, 74, 85, 89, 87, 99.250f, true, -207.0f, -548.0f, 1.375f },
		{ "m11", 0, 0, 0, 87, 85, 89, 69, 4.250f, false, -207.0f, -549.0f, 1.390f },
		{ "m12", 17, 217, 0, 100, 87, 89, 302, 115.250f, true, -210.0f, -543.0f, 1.375f },
		{ "m13", 31, 217, 1, 100, 90, 93, 3, 92.000f, true, -174.0f, -516.0f, 1.380f },
		{ "m14", 31, 217, 1, 100, 92, 98, 192, 115.250f, true, -171.0f, -533.0f, 1.385f },
		{ "m15", 31, 217, 1, 100, 92, 99, 135, 115.250f, true, -173.0f, -542.0f, 1.390f },
		{ "m16", 17, 199, 0, 100, 87, 89, 80, 97.250f, true, -212.0f, -548.0f, 1.375f },
} };

constexpr double kSettleSeconds = 3.0;
constexpr double kSettleStep = 1.0 / 30.0;
constexpr int32_t kRenderFrames = 3;
constexpr double kAliveTimeout = 90.0;
constexpr double kArriveTimeout = 10.0;
constexpr float kArriveXz = 0.25f;
constexpr float kArriveY = 0.5f;
constexpr float kScanAboveSpawns = 3.0f;
constexpr float kScanBelowSpawns = 5.0f;
constexpr float kScanStep = 0.05f;
constexpr uint32_t kScanMaxFloors = 64;
constexpr float kFloorNormalY = 0.7f;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

std::string xyz(const sim::Vec3 &p_v) {
	return "(" + std::to_string(p_v[0]) + ", " + std::to_string(p_v[1]) + ", " + std::to_string(p_v[2]) + ")";
}

bool at_feet(const sim::Vec3 &p_at, const sim::Vec3 &p_feet) {
	const float dx = p_at[0] - p_feet[0];
	const float dz = p_at[2] - p_feet[2];
	return std::sqrt(dx * dx + dz * dz) <= kArriveXz && std::fabs(p_at[1] - p_feet[1]) <= kArriveY;
}

const char *text_of(const Bf3NativeHud &p_hud, const char *p_path) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const Bf3SwfView *v = p_hud.view(i);
		const ui::SwfSceneNode *n = v != nullptr && v->scene() != nullptr ? v->scene()->find_node(p_path) : nullptr;
		if (n != nullptr) {
			return n->text.c_str();
		}
	}
	return nullptr;
}

const ui::SwfSceneNode *node_of(const Bf3NativeHud &p_hud, const char *p_path) {
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		const Bf3SwfView *v = p_hud.view(i);
		const ui::SwfSceneNode *n = v != nullptr && v->scene() != nullptr ? v->scene()->find_node(p_path) : nullptr;
		if (n != nullptr) {
			return n;
		}
	}
	return nullptr;
}

}

std::span<const NativeHudMoment> native_hud_moments() {
	return kMoments;
}

ui::HudModel native_hud_moment_model(const NativeHudMoment &p_moment, const ui::HudModel &p_base, float p_vfov_deg, float p_stage_height) {
	ui::HudModel m = p_base;
	m.soldier.is_alive = true;
	m.soldier.health = float(p_moment.health);
	m.soldier.compass_angle = float(p_moment.compass_frame);
	ui::HudWeapon &w = m.weapon;
	w.active_weapon_ammo = p_moment.clip;
	w.active_weapon_ammo_in_bag = p_moment.reserve;
	w.secondary_weapon_ammo = p_moment.grenades;
	w.active_weapon_show_ammo = true;
	w.active_weapon_critical_ammo = false;
	w.firing_mode = 2;
	w.firing_mode_types = (1u << 0) | (1u << 2);
	w.active_weapon_crosshair = 1;
	w.zoomed = !p_moment.crosshair;
	const double half = double(p_vfov_deg) * kDegToRad / 2.0;
	w.dispersion = p_stage_height > 0.0f && p_vfov_deg > 0.0f ? float(std::atan(double(p_moment.dispersion_px) * std::tan(half) / (double(p_stage_height) / 2.0)) / kDegToRad) : 0.0f;
	w.absolute_dispersion = w.dispersion;
	m.team.friendly_team = 1;
	m.team.enemy_team1 = 2;
	m.team.friendly_team_name = m.team.team1_name;
	m.team.enemy_team1_name = m.team.team2_name;
	m.kill_counter.player_team = 1;
	m.kill_counter.team1_kills = p_moment.tickets_us;
	m.kill_counter.team2_kills = p_moment.tickets_ru;
	m.kill_counter.team_kills_list.count = 2;
	m.kill_counter.team_kills_list.kills[0] = p_moment.tickets_us;
	m.kill_counter.team_kills_list.kills[1] = p_moment.tickets_ru;
	m.kill_counter.player_team_kills = p_moment.tickets_us;
	m.kill_counter.enemy_team_kills = p_moment.tickets_ru;
	++m.serial;
	return m;
}

float native_hud_moment_view_yaw(const NativeHudMoment &p_moment) {
	return float(double(p_moment.compass_frame) * kDegToRad);
}

NativeHudMinimapPose native_hud_moment_pose(const NativeHudMoment &p_moment) {
	NativeHudMinimapPose p;
	p.x = p_moment.world_x;
	p.z = p_moment.world_z;
	p.yaw = native_hud_moment_view_yaw(p_moment);
	p.metres_per_local_unit = p_moment.minimap_metres_per_unit;
	p.valid = true;
	return p;
}

std::optional<sim::Vec3> native_hud_moment_feet(const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns, float p_x,
		float p_z) {
	const sim::SpawnPoint *nearest = nullptr;
	float best = 0.0f;
	float low = std::numeric_limits<float>::infinity();
	float high = -std::numeric_limits<float>::infinity();
	for (const sim::SpawnPoint &sp : p_spawns) {
		const float dx = sp.position[0] - p_x;
		const float dz = sp.position[2] - p_z;
		const float d = dx * dx + dz * dz;
		if (nearest == nullptr || d < best) {
			nearest = &sp;
			best = d;
		}
		low = std::min(low, sp.position[1]);
		high = std::max(high, sp.position[1]);
	}
	if (nearest == nullptr) {
		return std::nullopt;
	}
	const float bottom = low - kScanBelowSpawns;
	const auto closer = [&](const sim::Vec3 &p_floor, const std::optional<sim::Vec3> &p_best) {
		return !p_best || std::fabs(p_floor[1] - nearest->position[1]) < std::fabs((*p_best)[1] - nearest->position[1]);
	};
	std::optional<sim::Vec3> clear_floor;
	std::optional<sim::Vec3> any_floor;
	float y = high + kScanAboveSpawns;
	for (uint32_t n = 0; n < kScanMaxFloors && y > bottom; ++n) {
		sim::StaticHit hit;
		if (!p_world.ground_below(sim::Vec3{ p_x, y, p_z }, y - bottom, sim::query_mask::kMovement, &hit)) {
			break;
		}
		y = hit.position[1] - kScanStep;
		if (hit.normal[1] < kFloorNormalY) {
			continue;
		}
		const sim::Vec3 capsule_base{ hit.position[0], hit.position[1] + p_motor.tuning.character_padding, hit.position[2] };
		const bool clear = !p_world.overlap_capsule(capsule_base, p_motor.radius, p_motor.poses[size_t(sim::Pose::Stand)].height, sim::query_mask::kMovement,
				p_motor.tuning.pose_change_max_penetration);
		if (clear && closer(hit.position, clear_floor)) {
			clear_floor = hit.position;
		}
		if (closer(hit.position, any_floor)) {
			any_floor = hit.position;
		}
	}
	return clear_floor ? clear_floor : any_floor;
}

NativeHudMomentRun::NativeHudMomentRun(std::string p_out_dir, std::string_view p_ids) : out_dir_(std::move(p_out_dir)) {
	std::vector<std::string_view> ids;
	for (size_t at = 0; at < p_ids.size();) {
		const size_t end = std::min(p_ids.find(',', at), p_ids.size());
		if (end > at) {
			ids.push_back(p_ids.substr(at, end - at));
		}
		at = end + 1;
	}
	for (const NativeHudMoment &m : kMoments) {
		if (ids.empty() || std::find(ids.begin(), ids.end(), std::string_view(m.id)) != ids.end()) {
			moments_.push_back(&m);
		}
	}
	for (const char *sub : { "captures", "layers" }) {
		DirAccess::make_dir_recursive_absolute(String::utf8((out_dir_ + "/" + sub).c_str()));
	}
}

void NativeHudMomentRun::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(String::utf8(("NATIVE HUD ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void NativeHudMomentRun::check(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment) {
	const std::string id = p_moment.id;
	const host::PresentationFrame &f = r_session.frame();
	row(id + ".in_round", f.match_valid && f.match.phase == net::MatchPhase::InRound,
			std::string("match phase ") + (f.match_valid ? std::to_string(int(f.match.phase)) : std::string("not received")) + ", want " +
					std::to_string(int(net::MatchPhase::InRound)));
	const uint16_t want_yaw = sim::quantize_yaw(native_hud_moment_view_yaw(p_moment));
	row(id + ".pose", at_feet(f.own_curr.position, feet_) && f.own_curr.yaw == want_yaw,
			"feet " + xyz(f.own_curr.position) + ", yaw " + std::to_string(f.own_curr.yaw) + ", want " + xyz(feet_) + ", yaw " + std::to_string(want_yaw));
	const auto text_is = [&](const char *p_what, const char *p_path, const std::string &p_want) {
		const char *t = text_of(r_hud, p_path);
		row(id + "." + p_what, t != nullptr && p_want == t, std::string(p_path) + " '" + (t != nullptr ? t : "missing") + "', want '" + p_want + "'");
	};
	text_is("clip", "instance1.Ammo_01.Ammo.clip.cltxt.txtDisplay", std::to_string(p_moment.clip));
	text_is("reserve", "instance1.Ammo_01.Ammo.amholder.amtxt.txtDisplay", std::to_string(p_moment.reserve));
	text_is("grenades", "instance1.Ammo_01.Ammo.grtxt.txtDisplay", std::to_string(p_moment.grenades));
	text_is("health", "instance1.Health_01.Health.healthHolder.amtxt.txtDisplay", std::to_string(p_moment.health));
	const ui::SwfSceneNode *cps = node_of(r_hud, "instance1.Compass_01.multiplayerCompass.cps");
	row(id + ".compass", cps != nullptr && int32_t(cps->frame) == p_moment.compass_frame,
			"cps frame " + (cps != nullptr ? std::to_string(cps->frame) : std::string("missing")) + ", want " + std::to_string(p_moment.compass_frame));
	const NativeHudPaintStats &ps = r_hud.paint_stats();
	row(id + ".engine_targets", ps.bg_panels > 0 && ps.ekg_traces > 0 && ps.minimap_maps > 0,
			"iBgDraw panels " + std::to_string(ps.bg_panels) + ", EKG traces " + std::to_string(ps.ekg_traces) + ", minimap maps " + std::to_string(ps.minimap_maps) +
					", minimap icons " + std::to_string(ps.minimap_icons));
}

bool NativeHudMomentRun::step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, double p_delta) {
	switch (phase_) {
		case Phase::WaitAlive: {
			waited_ += p_delta;
			if (!r_hud.is_ready()) {
				return false;
			}
			if (r_hud.has_failed() || moments_.empty()) {
				row("built", false, r_hud.has_failed() ? r_hud.error() : std::string("no moment selected"));
				phase_ = Phase::Done;
				return true;
			}
			const host::PresentationFrame &f = r_session.frame();
			const bool alive = r_hud.model().soldier.is_alive;
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			if (!alive || !in_round) {
				if (waited_ > kAliveTimeout) {
					row("alive_in_round", false,
							"after " + std::to_string(kAliveTimeout) + " s: own soldier " + (alive ? "alive" : "not spawned") + ", round " +
									(in_round ? "in round" : "not in round"));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			r_hud.set_manual(true);
			InputMapper &mapper = r_session.mapper();
			mapper.set_scripted(true);
			mapper.release_all();
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			index_ = 0;
			phase_ = Phase::Place;
			return false;
		}
		case Phase::Place: {
			const NativeHudMoment &m = *moments_[index_];
			host::ClientSession *client = r_session.client();
			const sim::CollisionWorld *world = r_session.collision_world();
			const std::shared_ptr<const host::SharedLevel> level = r_session.shared_level();
			const std::optional<sim::Vec3> feet =
					world != nullptr && level != nullptr ? native_hud_moment_feet(*world, level->motor, level->spawns, m.world_x, m.world_z) : std::nullopt;
			if (client == nullptr) {
				row(std::string(m.id) + ".place", false, "no client");
				phase_ = Phase::Done;
				return true;
			}
			if (!feet) {
				row(std::string(m.id) + ".place", false, "no floor under (" + std::to_string(m.world_x) + ", " + std::to_string(m.world_z) + ")");
				feet_ = r_session.frame().own_curr.position;
				phase_ = Phase::Settle;
				return false;
			}
			feet_ = *feet;
			const float yaw = native_hud_moment_view_yaw(m);
			r_session.mapper().script_view(yaw, 0.0f);
			net::DebugRequest q;
			q.kind = net::DebugKind::Teleport;
			q.id = ++request_id_;
			q.position = feet_;
			q.yaw = sim::quantize_yaw(yaw);
			client->send_debug(q);
			arriving_ = 0.0;
			phase_ = Phase::Arrive;
			return false;
		}
		case Phase::Arrive: {
			const NativeHudMoment &m = *moments_[index_];
			arriving_ += p_delta;
			const sim::Vec3 &at = r_session.frame().own_curr.position;
			if (!at_feet(at, feet_)) {
				if (arriving_ > kArriveTimeout) {
					row(std::string(m.id) + ".place", false, "the own soldier is at " + xyz(at) + " " + std::to_string(kArriveTimeout) + " s after the teleport to " + xyz(feet_));
					phase_ = Phase::Settle;
					return false;
				}
				return false;
			}
			phase_ = Phase::Settle;
			return false;
		}
		case Phase::Settle: {
			const NativeHudMoment &m = *moments_[index_];
			const Bf3SwfView *v = r_hud.view(0);
			const float stage_h = v != nullptr ? v->visible_rect().height() : 720.0f;
			const float vfov = r_hud.client().vertical_fov_deg > 0.0f ? r_hud.client().vertical_fov_deg : 55.0f;
			r_hud.set_fixture(native_hud_moment_model(m, r_hud.model(), vfov, stage_h), native_hud_moment_pose(m));
			for (double t = 0.0; t < kSettleSeconds; t += kSettleStep) {
				r_hud.step(kSettleStep);
			}
			r_hud.set_visible(true);
			frames_ = 0;
			phase_ = Phase::Render;
			return false;
		}
		case Phase::Render:
			r_hud.set_visible(true);
			if (++frames_ < kRenderFrames) {
				return false;
			}
			phase_ = Phase::Capture;
			return false;
		case Phase::Capture: {
			const NativeHudMoment &m = *moments_[index_];
			Ref<Image> composite = r_hud.get_viewport() != nullptr ? r_hud.get_viewport()->get_texture()->get_image() : Ref<Image>();
			Ref<Image> layer = r_hud.capture_layer();
			bool ok = composite.is_valid() && layer.is_valid();
			std::string detail;
			if (ok) {
				composite->convert(Image::FORMAT_RGB8);
				const std::string cp = out_dir_ + "/captures/" + m.id + ".png";
				const std::string lp = out_dir_ + "/layers/" + m.id + ".png";
				ok = composite->save_png(String::utf8(cp.c_str())) == OK && layer->save_png(String::utf8(lp.c_str())) == OK;
				detail = std::to_string(composite->get_width()) + "x" + std::to_string(composite->get_height()) + " composite, " +
						std::to_string(layer->get_width()) + "x" + std::to_string(layer->get_height()) + " HUD layer -> " + cp;
				ok = ok && composite->get_width() == 1280 && composite->get_height() == 720 && layer->get_width() == 1280 && layer->get_height() == 720;
			} else {
				detail = "no viewport image or HUD layer";
			}
			row(std::string(m.id) + ".capture", ok, detail);
			captured_ += ok ? 1 : 0;
			check(r_session, r_hud, m);
			if (++index_ >= moments_.size()) {
				phase_ = Phase::Done;
				return true;
			}
			phase_ = Phase::Place;
			return false;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
