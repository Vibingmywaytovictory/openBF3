#include "vault_run_check.h"

#include "level_session.h"
#include "viewmodel.h"

#include "bf3/core/math.h"
#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim_harness/vault_spots.h"

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
constexpr double kRunTimeout = 15.0;
constexpr double kAfterSeconds = 2.0;
constexpr double kRequestSeconds = 1.0;
constexpr float kArriveXz = 0.25f;
constexpr float kArriveY = 0.5f;
constexpr float kPressAt = 1.0f;
constexpr std::array<double, 3> kShotTimes{ 0.12, 0.3, 0.5 };
constexpr float kCameraBack = 0.6f;
constexpr float kCameraUp = 1.9f;
constexpr float kCameraPitch = -0.2f;
constexpr float kClipTimeTolerance = 2.0f * sim::kTickSeconds;
constexpr float kCameraMovedMetres = 0.001f;
constexpr float kCameraMovedDegrees = 0.1f;
constexpr uint8_t kNoClip = 0xFF;
}

VaultRunViewmodel vault_run_viewmodel(const Bf3Viewmodel *p_viewmodel) {
	VaultRunViewmodel v;
	if (p_viewmodel == nullptr || !p_viewmodel->is_visible()) {
		return v;
	}
	const anim::ViewmodelOutput &o = p_viewmodel->output();
	v.clip = o.vault_clip;
	v.seconds = o.vault_seconds;
	const anim::Vec3 t = o.camera_offset.t;
	v.camera_metres = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
	v.camera_degrees = 2.0f * std::acos(std::clamp(std::fabs(o.camera_offset.q.w), 0.0f, 1.0f)) / core::kDegToRadF;
	return v;
}

VaultRunCheck::VaultRunCheck(std::string p_shots) :
		shots_(std::move(p_shots)), other_frame_(std::make_unique<host::PresentationFrame>()) {}

VaultRunCheck::~VaultRunCheck() = default;

void VaultRunCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(
			String::utf8(("VAULT RUN ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void VaultRunCheck::teleport(host::ClientSession &r_client) {
	net::DebugRequest q;
	q.kind = net::DebugKind::Teleport;
	q.id = ++request_id_;
	q.position = spot_->feet;
	q.yaw = spot_->yaw;
	r_client.send_debug(q);
}

bool VaultRunCheck::arrived(const sim::Vec3 &p_at) const {
	return horizontal_distance(p_at, spot_->feet) <= kArriveXz && std::fabs(p_at[1] - spot_->feet[1]) <= kArriveY;
}

void VaultRunCheck::follow(Runner &r_runner, const sim::SoldierState &p_state, const VaultRunInputs &p_in,
		const char *p_who, double p_delta) {
	if (!r_runner.pressed) {
		return;
	}
	r_runner.since_press += p_delta;
	if (p_state.mode == sim::MoveMode::AnimationControlled) {
		if (!r_runner.vaulted) {
			r_runner.vaulted = true;
			r_runner.kind = p_state.vault.kind;
			r_runner.variant = p_state.vault.variant;
			r_runner.distance = p_state.vault.distance;
		}
		++r_runner.vault_frames;
	}
	r_runner.highest = std::max(r_runner.highest, p_state.position[1] - spot_->feet[1]);
	if (r_runner.shots < kShotTimes.size() && r_runner.since_press >= kShotTimes[r_runner.shots] && !shots_.empty() &&
			p_in.shot) {
		(void)p_in.shot(shots_ + "/" + p_who + "_" + std::to_string(r_runner.shots + 1) + ".png");
		++r_runner.shots;
	}
}

void VaultRunCheck::runner_rows(const Runner &p_runner, const sim::SoldierState &p_state, const char *p_who) {
	const std::string who(p_who);
	row(who + ".vaults", p_runner.vaulted,
			p_runner.vaulted
					? fmt("jump pressed %.1f m from a %.2f m obstacle while walking: vault %s, variant %u, measured "
						  "%.2f m away, %u "
						  "frames animation controlled, feet up to %.2f m",
							  double(kPressAt), double(spot_->top), sim::vault_kind_name(sim::VaultKind(p_runner.kind)),
							  unsigned(p_runner.variant), double(p_runner.distance), p_runner.vault_frames,
							  double(p_runner.highest))
					: fmt("jump pressed %.1f m from a %.2f m obstacle while walking: no vault, feet up to %.2f m",
							  double(kPressAt), double(spot_->top), double(p_runner.highest)));
	row(who + ".crosses", sim::vault_spot_crossed(*spot_, p_state.position),
			fmt("%.1f s after the press the soldier is %.2f m along the run with the face at %.2f m, %.2f m above the "
				"start",
					kAfterSeconds, double(sim::vault_spot_along(*spot_, p_state.position)), double(spot_->face),
					double(p_state.position[1] - spot_->feet[1])));
}

void VaultRunCheck::watch_own_clip(const sim::SoldierState &p_state, const VaultRunInputs &p_in) {
	if (!own_.pressed || !p_in.own_viewmodel) {
		return;
	}
	const VaultRunViewmodel v = p_in.own_viewmodel();
	if (v.clip == kNoClip) {
		return;
	}
	++own_.clip_frames;
	own_.wrong_clip += own_.vaulted && v.clip != own_.kind ? 1u : 0u;
	own_.clip_seconds = std::max(own_.clip_seconds, v.seconds);
	if (p_state.mode == sim::MoveMode::AnimationControlled) {
		++own_.timed;
		own_.time_error =
				std::max(own_.time_error, std::fabs(v.seconds - float(p_state.vault.ticks) * sim::kTickSeconds));
	}
	own_.camera_metres = std::max(own_.camera_metres, v.camera_metres);
	own_.camera_degrees = std::max(own_.camera_degrees, v.camera_degrees);
}

void VaultRunCheck::watch_other_clip(const sim::SoldierState &p_state, const VaultRunInputs &p_in) {
	if (!other_.pressed || !p_in.crowd_vault) {
		return;
	}
	uint8_t kind = kNoClip;
	float seconds = 0.0f;
	if (!p_in.crowd_vault(other_slot_, kind, seconds)) {
		return;
	}
	++other_.clip_frames;
	other_.wrong_clip += other_.vaulted && kind != other_.kind ? 1u : 0u;
	other_.clip_seconds = std::max(other_.clip_seconds, seconds);
	if (p_state.mode == sim::MoveMode::AnimationControlled) {
		++other_.timed;
		const float ahead = seconds - float(p_state.vault.ticks) * sim::kTickSeconds;
		other_.lead = std::max(other_.lead, ahead);
		other_.lag = std::max(other_.lag, -ahead);
	}
}

void VaultRunCheck::clip_rows(const Runner &p_runner, const char *p_who, const char *p_clip) {
	const std::string who(p_who);
	const bool own = who == "own";
	const bool timed = p_runner.timed > 0 &&
			(own ? p_runner.time_error <= kClipTimeTolerance : p_runner.lead <= kClipTimeTolerance);
	row(who + ".vault_clip", p_runner.vaulted && p_runner.clip_frames > 0 && p_runner.wrong_clip == 0 && timed,
			own ? fmt("%s vault clip on %u frames (%u of another kind), up to %.3f s into it; on the %u "
					  "animation-controlled frames its time is the move's ticks within %.3f s (limit %.3f s)",
						  p_clip, p_runner.clip_frames, p_runner.wrong_clip, double(p_runner.clip_seconds),
						  p_runner.timed, double(p_runner.time_error), double(kClipTimeTolerance))
				: fmt("%s vault clip on %u frames (%u of another kind), up to %.3f s into it; on the %u frames the "
					  "vaulter's own move runs the clip trails its ticks by up to %.3f s and never leads them by more "
					  "than %.3f s (limit %.3f s)",
						  p_clip, p_runner.clip_frames, p_runner.wrong_clip, double(p_runner.clip_seconds),
						  p_runner.timed, double(p_runner.lag), double(std::max(0.0f, p_runner.lead)),
						  double(kClipTimeTolerance)));
}

void VaultRunCheck::drive_other(bool p_forward) {
	host::InputSample s;
	s.yaw = yaw_;
	s.life = std::numeric_limits<uint32_t>::max();
	s.pose_request = host::PoseRequest::Set;
	s.pose = sim::Pose::Stand;
	s.move_forward = p_forward ? 1.0f : 0.0f;
	s.jump_presses = other_jumps_;
	other_client_->set_input(s);
}

void VaultRunCheck::aim_free_camera(Bf3LevelSession &r_session) {
	const float sy = std::sin(yaw_), cy = std::cos(yaw_);
	r_session.mapper().script_view(yaw_, kCameraPitch);
	r_session.cameras().place_free(
			Vector3(spot_->feet[0] - sy * kCameraBack, spot_->feet[1] + kCameraUp, spot_->feet[2] - cy * kCameraBack),
			yaw_, kCameraPitch);
}

bool VaultRunCheck::step(Bf3LevelSession &r_session, const VaultRunInputs &p_in, double p_delta) {
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
			sim::SoldierMotor motor;
			if (!motor.init(*world, level->motor).ok()) {
				row("spot", false, "no motor on the level's collision world");
				phase_ = Phase::Done;
				return true;
			}
			const std::vector<sim::VaultSpot> spots = sim::find_vault_spots(motor, *world, level->spawns, 1);
			row("spot", !spots.empty(),
					spots.empty() ? std::string("no low obstacle ahead of a spawn point")
								  : fmt("spawn point %zu, heading %u: a %.2f m obstacle %.2f m ahead", spots[0].point,
											unsigned(spots[0].yaw), double(spots[0].top), double(spots[0].face)));
			if (spots.empty()) {
				phase_ = Phase::Done;
				return true;
			}
			spot_ = std::make_unique<sim::VaultSpot>(spots[0]);
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
							fmt("the soldier is %.2f m from the run's start after %.0f s",
									double(horizontal_distance(f.own_curr.position, spot_->feet)), kArriveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			t_ = 0.0;
			phase_ = Phase::OwnRun;
			return false;
		}
		case Phase::OwnRun: {
			t_ += p_delta;
			mapper.script_view(yaw_, 0.0f);
			mapper.script_hold(InputAction::Forward, true);
			if (!own_.pressed && spot_->face - sim::vault_spot_along(*spot_, f.own_curr.position) <= kPressAt) {
				mapper.script_press(InputAction::Jump);
				own_.pressed = true;
				own_.vaults_before = p_in.foley_vaults ? p_in.foley_vaults() : 0;
			}
			follow(own_, f.own_curr, p_in, "own", p_delta);
			watch_own_clip(f.own_curr, p_in);
			if (own_.since_press >= kAfterSeconds || t_ > kRunTimeout) {
				mapper.script_hold(InputAction::Forward, false);
				own_.vaults_after = p_in.foley_vaults ? p_in.foley_vaults() : 0;
				runner_rows(own_, f.own_curr, "own");
				clip_rows(own_, "own", "1p");
				row("own.camera",
						own_.camera_metres >= kCameraMovedMetres || own_.camera_degrees >= kCameraMovedDegrees,
						fmt("the 1p vault clip moves the camera up to %.3f m and %.1f deg off the pose without it",
								double(own_.camera_metres), double(own_.camera_degrees)));
				row("own.vault_sound", own_.vaults_after > own_.vaults_before,
						fmt("the movement patch got %u Vault events during the run",
								unsigned(own_.vaults_after - own_.vaults_before)));
				t_ = 0.0;
				phase_ = Phase::OwnAfter;
			}
			return false;
		}
		case Phase::OwnAfter: {
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
			return false;
		}
		case Phase::JoinOther:
		case Phase::OtherAlive: {
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
			other_slot_ = other_frame_->stats.slot;
			if (!arrived(other_frame_->own_curr.position)) {
				if (t_ > kArriveTimeout) {
					row("other.placed", false,
							fmt("the second soldier is %.2f m from the run's start after %.0f s",
									double(horizontal_distance(other_frame_->own_curr.position, spot_->feet)),
									kArriveTimeout));
					phase_ = Phase::Done;
					return true;
				}
				return false;
			}
			t_ = 0.0;
			phase_ = Phase::OtherRun;
			return false;
		}
		case Phase::OtherRun: {
			t_ += p_delta;
			aim_free_camera(r_session);
			(void)other_client_->read_presentation(*other_frame_);
			const sim::SoldierState &state = other_frame_->own_curr;
			if (!other_.pressed && spot_->face - sim::vault_spot_along(*spot_, state.position) <= kPressAt) {
				++other_jumps_;
				other_.pressed = true;
			}
			drive_other(true);
			follow(other_, state, p_in, "other", p_delta);
			watch_other_clip(state, p_in);
			if (other_.since_press >= kAfterSeconds || t_ > kRunTimeout) {
				drive_other(false);
				runner_rows(other_, state, "other");
				clip_rows(other_, "other", "3p (the first client's crowd)");
				phase_ = Phase::OtherAfter;
			}
			return false;
		}
		case Phase::OtherAfter:
			mapper.set_free_camera(false);
			r_session.cameras().set_free(false);
			phase_ = Phase::Done;
			return true;
		case Phase::Done:
			return true;
	}
	return true;
}

}
