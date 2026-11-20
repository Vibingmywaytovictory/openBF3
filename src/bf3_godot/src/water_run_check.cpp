#include "water_run_check.h"

#include "level_session.h"

#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/water.h"
#include "bf3/sim_harness/water_spots.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;
using bf3::core::vec::horizontal_distance;

namespace {

constexpr double kAliveTimeout = 90.0;
constexpr double kArriveTimeout = 10.0;
constexpr double kRunTimeout = 25.0;
constexpr double kStillSeconds = 1.5;
constexpr double kRequestSeconds = 1.0;
constexpr float kArriveXz = 0.25f;
constexpr float kArriveY = 0.5f;
constexpr float kAtSurface = 0.1f;
constexpr float kAboveBed = 0.5f;
constexpr float kBankReach = 3.0f;
constexpr std::array<float, 3> kShotAlong{ 0.4f, 0.7f, 1.0f };
constexpr float kCameraShort = 7.0f;
constexpr float kCameraUp = 2.5f;
}

WaterRunCheck::WaterRunCheck(std::string p_shots) :
		shots_(std::move(p_shots)), other_frame_(std::make_unique<host::PresentationFrame>()) {}

WaterRunCheck::~WaterRunCheck() = default;

void WaterRunCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(
			String::utf8(("WATER RUN ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void WaterRunCheck::teleport(host::ClientSession &r_client) {
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = spot_->bank;
	q.yaw = spot_->yaw;
	r_client.send_debug(q);
}

bool WaterRunCheck::arrived(const sim::Vec3 &p_at) const {
	return horizontal_distance(p_at, spot_->bank) <= kArriveXz && std::fabs(p_at[1] - spot_->bank[1]) <= kArriveY;
}

void WaterRunCheck::water_rows(const sim::SoldierState &p_state, const Runner &p_runner, const char *p_who) {
	const std::string who(p_who);
	const std::optional<float> surface = water_->height(p_state.position[0], p_state.position[2]);
	const float level = surface.value_or(std::numeric_limits<float>::quiet_NaN());
	const std::optional<float> bed = sim::bed_below(*world_, p_state.position[0], p_state.position[2], level);
	const float feet = p_state.position[1];
	row(who + ".swims", p_state.mode == sim::MoveMode::Swimming,
			fmt("%.1f s still after walking %.1f m from the bank into water %.2f m deep: move mode %u, %u frames "
				"swimming",
					kStillSeconds, double(spot_->run), double(spot_->surface - spot_->deep[1]), unsigned(p_state.mode),
					p_runner.swim_frames));
	row(who + ".at_surface", surface.has_value() && std::fabs(feet - (level - body_under_water_)) <= kAtSurface,
			fmt("feet at %.2f m, the water surface at %.2f m, the data's BodyUnderWater %.2f m puts them at %.2f m",
					double(feet), double(level), double(body_under_water_), double(level - body_under_water_)));
	row(who + ".above_bed", bed.has_value() && feet - *bed >= kAboveBed,
			fmt("feet %.2f m above the river bed at %.2f m", double(bed ? feet - *bed : 0.0f),
					double(bed.value_or(0.0f))));
}

void WaterRunCheck::shoot(Runner &r_runner, const WaterRunInputs &p_in, const char *p_who) {
	if (!shots_.empty() && p_in.shot) {
		(void)p_in.shot(shots_ + "/" + p_who + "_" + std::to_string(r_runner.shots + 1) + ".png");
	}
	++r_runner.shots;
}

void WaterRunCheck::drive_other(bool p_forward) {
	host::InputSample s;
	s.yaw = yaw_;
	s.life = std::numeric_limits<uint32_t>::max();
	s.pose_request = host::PoseRequest::Set;
	s.pose = sim::Pose::Stand;
	s.move_forward = p_forward ? 1.0f : 0.0f;
	other_client_->set_input(s);
}

void WaterRunCheck::aim_free_camera(Bf3LevelSession &r_session) {
	const float sy = std::sin(yaw_), cy = std::cos(yaw_);
	const float along = std::max(0.0f, spot_->run - kCameraShort);
	const float pitch = -std::atan2(kCameraUp, spot_->run - along);
	r_session.mapper().script_view(yaw_, pitch);
	r_session.cameras().place_free(
			Vector3(spot_->bank[0] + sy * along, spot_->surface + kCameraUp, spot_->bank[2] + cy * along), yaw_, pitch);
}

bool WaterRunCheck::step(Bf3LevelSession &r_session, const WaterRunInputs &p_in, double p_delta) {
	const host::PresentationFrame &f = r_session.frame();
	InputMapper &mapper = r_session.mapper();
	switch (phase_) {
		case Phase::WaitAlive: {
			waited_ += p_delta;
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			const sim::CollisionWorld *world = r_session.collision_world();
			const std::shared_ptr<const host::SharedLevel> level = r_session.shared_level();
			if (!p_in.alive || !in_round || world == nullptr || level == nullptr) {
				if (waited_ > kAliveTimeout) {
					row("alive_in_round", false,
							fmt("after %.0f s: soldier %s, round %s", kAliveTimeout,
									p_in.alive ? "alive" : "not spawned", in_round ? "in round" : "not in round"));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			row("water", level->water != nullptr,
					level->water != nullptr ? fmt("%zu water meshes, %zu triangles", level->water->body_count(),
													  level->water->triangle_count())
											: std::string("the level has no water mesh"));
			if (level->water == nullptr) {
				phase_ = Phase::Done;
				return true;
			}
			sim::SoldierMotor motor;
			if (!motor.init(*world, level->motor).ok()) {
				row("spot", false, "no motor on the level's collision world");
				phase_ = Phase::Done;
				return true;
			}
			const std::optional<sim::WaterSpot> spot =
					sim::find_water_spot(motor, *world, *level->water, level->spawns);
			row("spot", spot.has_value(),
					spot ? fmt("bank (%.1f, %.1f, %.1f), %.1f m to water %.2f m deep at (%.1f, %.1f), surface %.2f m",
								   double(spot->bank[0]), double(spot->bank[1]), double(spot->bank[2]),
								   double(spot->run), double(spot->surface - spot->deep[1]), double(spot->deep[0]),
								   double(spot->deep[2]), double(spot->surface))
						 : std::string("no water deeper than 2.5 m with a walkable bank near a spawn point"));
			if (!spot) {
				phase_ = Phase::Done;
				return true;
			}
			spot_ = std::make_unique<sim::WaterSpot>(*spot);
			water_ = level->water.get();
			world_ = world;
			body_under_water_ = level->motor.swim.body_under_water;
			yaw_ = sim::yaw_radians(spot_->yaw);
			mapper.set_scripted(true);
			mapper.release_all();
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			t_ = 0.0;
			phase_ = Phase::OwnArrive;
			return false;
		}
		case Phase::OwnArrive: {
			t_ += p_delta;
			mapper.script_view(yaw_, 0.0f);
			if (host::ClientSession *client = r_session.client()) {
				teleport(*client);
			}
			if (!arrived(f.own_curr.position)) {
				if (t_ > kArriveTimeout) {
					row("own.placed", false,
							fmt("the soldier is %.2f m from the bank after %.0f s",
									double(horizontal_distance(f.own_curr.position, spot_->bank)), kArriveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			t_ = 0.0;
			phase_ = Phase::OwnIn;
			return false;
		}
		case Phase::OwnIn: {
			t_ += p_delta;
			mapper.script_view(yaw_, 0.0f);
			mapper.script_hold(InputAction::Forward, true);
			own_.swim_frames += f.own_curr.mode == sim::MoveMode::Swimming ? 1 : 0;
			const float along = sim::water_spot_along(*spot_, f.own_curr.position);
			if (own_.shots < kShotAlong.size() && along >= spot_->run * kShotAlong[own_.shots]) {
				shoot(own_, p_in, "own");
			}
			if (along >= spot_->run || t_ > kRunTimeout) {
				mapper.script_hold(InputAction::Forward, false);
				t_ = 0.0;
				phase_ = Phase::OwnStill;
			}
			return false;
		}
		case Phase::OwnStill: {
			t_ += p_delta;
			mapper.script_view(yaw_, 0.0f);
			own_.swim_frames += f.own_curr.mode == sim::MoveMode::Swimming ? 1 : 0;
			if (t_ < kStillSeconds) {
				return false;
			}
			water_rows(f.own_curr, own_, "own");
			t_ = 0.0;
			phase_ = Phase::OwnOut;
			return false;
		}
		case Phase::OwnOut: {
			t_ += p_delta;
			mapper.script_view(yaw_ + sim::kPi, 0.0f);
			mapper.script_hold(InputAction::Forward, true);
			const bool out = f.own_curr.mode == sim::MoveMode::OnGround &&
					sim::water_spot_along(*spot_, f.own_curr.position) <= 0.0f &&
					horizontal_distance(f.own_curr.position, spot_->bank) <= kBankReach;
			if (out || t_ > kRunTimeout) {
				mapper.script_hold(InputAction::Forward, false);
				row("own.leaves", out,
						fmt("walking back to the bank: move mode %u, %.1f m from the bank after %.1f s",
								unsigned(f.own_curr.mode),
								double(horizontal_distance(f.own_curr.position, spot_->bank)), t_));
				std::string error;
				const bool joined = r_session.join_extra_client(&error, true);
				other_client_ = r_session.extra_client();
				row("other.joined", joined && other_client_ != nullptr, joined ? "a second client joined" : error);
				if (!joined || other_client_ == nullptr) {
					phase_ = Phase::Done;
					return true;
				}
				t_ = 0.0;
				next_request_ = 0.0;
				phase_ = Phase::JoinOther;
			}
			return false;
		}
		case Phase::JoinOther: {
			t_ += p_delta;
			drive_other(false);
			const bool have = other_client_->read_presentation(*other_frame_) && other_frame_->valid;
			if (!have || !host::present_own_alive(*other_frame_)) {
				if (t_ >= next_request_) {
					other_client_->send_spawn_request(0);
					next_request_ = t_ + kRequestSeconds;
				}
				if (t_ > kAliveTimeout) {
					row("other.alive", false,
							fmt("the second client's soldier did not spawn in %.0f s", kAliveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			mapper.set_free_camera(true);
			r_session.cameras().set_free(true);
			t_ = 0.0;
			phase_ = Phase::OtherArrive;
			return false;
		}
		case Phase::OtherArrive: {
			t_ += p_delta;
			aim_free_camera(r_session);
			drive_other(false);
			teleport(*other_client_);
			(void)other_client_->read_presentation(*other_frame_);
			if (!arrived(other_frame_->own_curr.position)) {
				if (t_ > kArriveTimeout) {
					row("other.placed", false,
							fmt("the second soldier is %.2f m from the bank after %.0f s",
									double(horizontal_distance(other_frame_->own_curr.position, spot_->bank)),
									kArriveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			t_ = 0.0;
			phase_ = Phase::OtherIn;
			return false;
		}
		case Phase::OtherIn: {
			t_ += p_delta;
			aim_free_camera(r_session);
			(void)other_client_->read_presentation(*other_frame_);
			const sim::SoldierState &state = other_frame_->own_curr;
			other_.swim_frames += state.mode == sim::MoveMode::Swimming ? 1 : 0;
			drive_other(true);
			const float along = sim::water_spot_along(*spot_, state.position);
			if (other_.shots < kShotAlong.size() && along >= spot_->run * kShotAlong[other_.shots]) {
				shoot(other_, p_in, "other");
			}
			if (along >= spot_->run || t_ > kRunTimeout) {
				drive_other(false);
				t_ = 0.0;
				phase_ = Phase::OtherStill;
			}
			return false;
		}
		case Phase::OtherStill: {
			t_ += p_delta;
			aim_free_camera(r_session);
			drive_other(false);
			(void)other_client_->read_presentation(*other_frame_);
			other_.swim_frames += other_frame_->own_curr.mode == sim::MoveMode::Swimming ? 1 : 0;
			if (t_ < kStillSeconds) {
				return false;
			}
			shoot(other_, p_in, "other");
			water_rows(other_frame_->own_curr, other_, "other");
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			phase_ = Phase::Done;
			return true;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
