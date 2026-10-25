#include "crosshair_walk_check.h"

#include "level_session.h"
#include "native_hud.h"
#include "native_hud_moments.h"
#include "moments/in_round_moments.h"

#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;
namespace sw = bf3::sim::weapons;

namespace {

constexpr double kAliveTimeout = 90.0;
constexpr double kArriveTimeout = 10.0;
constexpr double kStandSeconds = 1.0;
constexpr double kWalkSeconds = 1.5;
constexpr double kStopSeconds = 1.0;
constexpr double kStandSettleSeconds = 0.5;
constexpr double kWalkSettleSeconds = 0.3;
constexpr double kStopSettleSeconds = 0.5;
constexpr float kWalkingSpeed = 0.5f;
constexpr float kStoppedSpeed = 0.05f;
constexpr float kNearRadius = 8.0f;
constexpr float kArriveXz = 0.25f;
constexpr float kArriveY = 0.5f;
constexpr uint32_t kMinStillFrames = 10;
constexpr uint32_t kMinWalkFrames = 20;
constexpr double kRampSeconds = 0.5;
constexpr uint32_t kMinRampFrames = 3;
constexpr std::string_view kPlaceMoment = "m02";

float horizontal_speed(const sim::Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[2] * p_v[2]);
}

float horizontal_distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0];
	const float dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

}

void CrosshairWalkRun::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(String::utf8(("CROSSHAIR WALK ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void CrosshairWalkRun::teleport(Bf3LevelSession &r_session) {
	r_session.mapper().script_view(yaw_, 0.0f);
	host::ClientSession *client = r_session.client();
	if (client == nullptr) {
		return;
	}
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = feet_;
	q.yaw = sim::quantize_yaw(yaw_);
	client->send_debug(q);
}

void CrosshairWalkRun::sample(Tally &r_tally, float p_arm, double p_want) {
	++r_tally.frames;
	if (double(p_arm) == p_want) {
		++r_tally.matched;
	} else if (r_tally.first_bad < 0.0f) {
		r_tally.first_bad = p_arm;
	}
}

void CrosshairWalkRun::sample_ramp(Ramp &r_ramp, float p_arm, double p_want) {
	++r_ramp.frames;
	if (double(p_arm) == p_want) {
		++r_ramp.matched;
	} else if (r_ramp.first_bad < 0.0f) {
		r_ramp.first_bad = p_arm;
		r_ramp.first_bad_want = p_want;
	}
	const double lo = std::min(stand_arm_, moving_arm_);
	const double hi = std::max(stand_arm_, moving_arm_);
	r_ramp.between += double(p_arm) > lo && double(p_arm) < hi ? 1 : 0;
	if (r_ramp.last >= 0.0) {
		r_ramp.largest_step = std::max(r_ramp.largest_step, std::fabs(double(p_arm) - r_ramp.last));
	}
	r_ramp.last = p_arm;
}

double CrosshairWalkRun::blend_arm(const host::PresentationFrame &p_frame, const CrosshairWalkInputs &p_in) const {
	const sw::WeaponSimDef &def = p_in.table->defs[p_in.weapon->def];
	const sw::SwayContext sway = sw::suppressed_sway(sw::weapon_sway_context(level_->motor, p_frame.own_curr, p_frame.own_command), 0.0f, p_in.table->mechanics);
	return core::ui::hud_crosshair_arm(sw::effective_dispersion(def, sway).min_angle, offset_);
}

void CrosshairWalkRun::ramp_row(const std::string &p_name, const Ramp &p_ramp, const std::string &p_what) {
	const double span = std::fabs(moving_arm_ - stand_arm_);
	const bool pass = p_ramp.frames > 0 && p_ramp.matched == p_ramp.frames && p_ramp.between >= kMinRampFrames && p_ramp.largest_step < span;
	row(p_name, pass,
			p_what + fmt(" in the first %.1f s: %u / %u frames at the sim's speed / walk speed blend arm of the drawn frame, %u frames between %.0f and %.0f, largest step %.0f px of %.0f",
							 kRampSeconds, p_ramp.matched, p_ramp.frames, p_ramp.between, stand_arm_, moving_arm_, p_ramp.largest_step, span) +
					(p_ramp.first_bad >= 0.0f ? fmt(", first other arm %.2f (blend %.0f)", p_ramp.first_bad, p_ramp.first_bad_want) : std::string()));
}

void CrosshairWalkRun::finish_rows(const CrosshairWalkInputs &p_in) {
	const auto tally = [](const Tally &p_t, double p_want) {
		return fmt("%u / %u frames at arm %.0f", p_t.matched, p_t.frames, p_want) + (p_t.first_bad >= 0.0f ? fmt(", first other arm %.2f", p_t.first_bad) : std::string());
	};
	row("arms_from_data", stand_arm_ < moving_arm_, fmt("the held item's hip minimums give arm %.0f standing and %.0f moving", stand_arm_, moving_arm_));
	row("stand", stand_.frames >= kMinStillFrames && stand_.matched == stand_.frames, "standing: " + tally(stand_, stand_arm_));
	row("walk", walk_.frames >= kMinWalkFrames && walk_.matched == walk_.frames,
			"walking forward, no trigger: " + tally(walk_, moving_arm_) + fmt(", %.2f m walked", walked_m_));
	row("stop", stop_.frames >= kMinStillFrames && stop_.matched == stop_.frames, "stopped again: " + tally(stop_, stand_arm_));
	ramp_row("walk_rise", rise_, "the arm rises as the walk starts");
	ramp_row("stop_fall", fall_, "the arm falls as the soldier stops");
	const int32_t magazine = p_in.weapon != nullptr ? p_in.weapon->magazine : -1;
	row("no_shot", p_in.own_shots == shots_at_start_ && magazine == magazine_at_start_,
			fmt("own shots %llu -> %llu, magazine %d -> %d", (unsigned long long)shots_at_start_, (unsigned long long)p_in.own_shots, magazine_at_start_, magazine));
}

bool CrosshairWalkRun::step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const CrosshairWalkInputs &p_in, double p_delta) {
	const host::PresentationFrame &f = r_session.frame();
	switch (phase_) {
		case Phase::WaitAlive: {
			waited_ += p_delta;
			if (!r_hud.is_ready()) {
				return false;
			}
			if (r_hud.has_failed()) {
				row("built", false, r_hud.error());
				phase_ = Phase::Done;
				return true;
			}
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			const bool ready = p_in.alive && p_in.weapon != nullptr && p_in.weapon->valid && p_in.weapon->phase == sw::WeaponPhase::Ready && p_in.table != nullptr &&
					p_in.weapon->def < p_in.table->defs.size();
			if (!ready || !in_round) {
				if (waited_ > kAliveTimeout) {
					row("alive_in_round", false, fmt("after %.0f s: soldier %s, round %s", kAliveTimeout, p_in.alive ? "alive" : "not spawned", in_round ? "in round" : "not in round"));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			const std::optional<bf3::core::ui::MinimapLevel> map = r_hud.minimap_level();
			if (!map) {
				if (waited_ > kAliveTimeout) {
					row("minimap", false, fmt("after %.0f s: no minimap level to place the %s registration texel", kAliveTimeout, std::string(kPlaceMoment).c_str()));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			InputMapper &mapper = r_session.mapper();
			mapper.set_scripted(true);
			mapper.release_all();
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			const NativeHudMoment *place = nullptr;
			for (const NativeHudMoment &m : native_hud_moments()) {
				place = std::string_view(m.id) == kPlaceMoment ? &m : place;
			}
			const sim::CollisionWorld *world = r_session.collision_world();
			const std::shared_ptr<const host::SharedLevel> level = r_session.shared_level();
			const std::array<float, 2> at_xz = place != nullptr ? native_hud_moment_world_xz(*place, *map) : std::array<float, 2>{};
			const std::optional<sim::Vec3> feet = place != nullptr && world != nullptr && level != nullptr
					? native_hud_moment_floor_near(*world, level->motor, level->spawns, at_xz[0], at_xz[1], kNearRadius)
					: std::nullopt;
			if (!feet) {
				row("place", false, "no standing floor near the " + std::string(kPlaceMoment) + " moment");
				phase_ = Phase::Done;
				return true;
			}
			feet_ = *feet;
			yaw_ = native_hud_moment_view_yaw(*place);
			teleport(r_session);
			t_ = 0.0;
			phase_ = Phase::Arrive;
			return false;
		}
		case Phase::Arrive: {
			t_ += p_delta;
			teleport(r_session);
			const sim::Vec3 &at = f.own_curr.position;
			if (horizontal_distance(at, feet_) > kArriveXz || std::fabs(at[1] - feet_[1]) > kArriveY) {
				if (t_ > kArriveTimeout) {
					row("place", false, fmt("the soldier is %.2f m from the teleport target after %.0f s", horizontal_distance(at, feet_), kArriveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			const sw::WeaponSimDef &def = p_in.table->defs[p_in.weapon->def];
			sw::SwayContext moving;
			moving.move = sw::SwayMove::Moving;
			const double offset = r_hud.readout().crosshair_base_offset;
			level_ = r_session.shared_level();
			if (level_ == nullptr) {
				row("level", false, "no shared level to take the motor's walk speed from");
				phase_ = Phase::Done;
				return true;
			}
			offset_ = offset;
			drawn_arm_ = blend_arm(f, p_in);
			stand_arm_ =
					core::ui::hud_crosshair_arm(sw::effective_dispersion(def, sw::SwayContext{}).min_angle, offset);
			moving_arm_ = core::ui::hud_crosshair_arm(sw::effective_dispersion(def, moving).min_angle, offset);
			shots_at_start_ = p_in.own_shots;
			magazine_at_start_ = p_in.weapon->magazine;
			t_ = 0.0;
			phase_ = Phase::Stand;
			return false;
		}
		case Phase::Stand: {
			t_ += p_delta;
			r_session.mapper().script_view(yaw_, 0.0f);
			const NativeHudReadout r = r_hud.readout();
			if (t_ >= kStandSettleSeconds) {
				sample(stand_, r.crosshair_widget ? r.crosshair_arm : -1.0f, stand_arm_);
			}
			drawn_arm_ = blend_arm(f, p_in);
			if (t_ >= kStandSeconds) {
				walk_from_ = f.own_curr.position;
				r_session.mapper().script_hold(InputAction::Forward, true);
				t_ = 0.0;
				phase_ = Phase::Walk;
			}
			return false;
		}
		case Phase::Walk: {
			t_ += p_delta;
			r_session.mapper().script_view(yaw_, 0.0f);
			r_session.mapper().script_hold(InputAction::Forward, true);
			const NativeHudReadout r = r_hud.readout();
			if (t_ < kRampSeconds) {
				sample_ramp(rise_, r.crosshair_widget ? r.crosshair_arm : -1.0f, drawn_arm_);
			}
			drawn_arm_ = blend_arm(f, p_in);
			if (t_ >= kWalkSettleSeconds && horizontal_speed(f.own_curr.velocity) > kWalkingSpeed) {
				sample(walk_, r.crosshair_widget ? r.crosshair_arm : -1.0f, moving_arm_);
			}
			if (t_ >= kWalkSeconds) {
				r_session.mapper().script_hold(InputAction::Forward, false);
				walked_m_ = horizontal_distance(f.own_curr.position, walk_from_);
				t_ = 0.0;
				phase_ = Phase::Stop;
			}
			return false;
		}
		case Phase::Stop: {
			t_ += p_delta;
			r_session.mapper().script_view(yaw_, 0.0f);
			const NativeHudReadout r = r_hud.readout();
			if (t_ < kRampSeconds) {
				sample_ramp(fall_, r.crosshair_widget ? r.crosshair_arm : -1.0f, drawn_arm_);
			}
			drawn_arm_ = blend_arm(f, p_in);
			if (t_ >= kStopSettleSeconds && horizontal_speed(f.own_curr.velocity) < kStoppedSpeed) {
				sample(stop_, r.crosshair_widget ? r.crosshair_arm : -1.0f, stand_arm_);
			}
			if (t_ >= kStopSeconds) {
				finish_rows(p_in);
				phase_ = Phase::Done;
				return true;
			}
			return false;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
