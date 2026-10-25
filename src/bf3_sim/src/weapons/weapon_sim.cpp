#include "bf3/sim/weapons/weapon_sim.h"

#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "weapons_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::sim::weapons {

namespace {

constexpr int kMaxLoopEvents = 64;
constexpr SimTime kNever = std::numeric_limits<SimTime>::max();

struct Tick_ {
	const WeaponTable &table;
	const Mechanics &mech;
	SoldierWeaponsState &st;
	const Command &cmd;
	const WeaponContext &ctx;
	uint64_t match_seed;
	WeaponTickOutput &out;
	SimTime t0;
	SimTime t1;
	WeaponInput in;
	SwayContext sway;
	uint32_t events = 0;
	bool shot = false;
};

void timed(Tick_ &r_io, SimTime p_time, uint32_t p_event, uint8_t p_slot) {
	r_io.events |= p_event;
	if (r_io.out.timed_count < kMaxTimedEventsPerTick) {
		r_io.out.timed[r_io.out.timed_count++] = TimedWeaponEvent{ p_time, p_event, p_slot };
	} else {
		++r_io.out.timed_dropped;
	}
}

bool has_ammo(const Tick_ &p_io, const WeaponSlotState &p_s, const WeaponSimDef &p_d) {
	return p_io.table.infinite_magazine || p_d.unlimited_ammo || p_s.magazine > 0;
}

bool can_reload(const WeaponSlotState &p_s, const WeaponSimDef &p_d) {
	return p_d.reload_type != ReloadType::Unknown && p_d.magazine_capacity > 0 && p_s.magazine < p_d.magazine_capacity &&
			(p_s.reserve > 0 || p_d.unlimited_ammo);
}

const FireModeDef &mode_of(const WeaponSlotState &p_s, const WeaponSimDef &p_d) {
	return p_d.modes[p_s.mode < p_d.mode_count ? p_s.mode : 0];
}

bool is_tactical(const WeaponSlotState &p_s, const WeaponSimDef &p_d) {
	return p_d.reload_type == ReloadType::MagazineWithPossibleShorterReload && p_s.magazine > 0;
}

SimTime commit_time(const WeaponSlotState &p_s, const WeaponSimDef &p_d) {
	return p_s.phase_start + (is_tactical(p_s, p_d) ? p_d.reload_commit_tactical : p_d.reload_commit_empty);
}

void start_reload(Tick_ &r_io, uint8_t p_slot, WeaponSlotState &r_s, const WeaponSimDef &p_d, SimTime p_time) {
	r_s.phase = WeaponPhase::Reloading;
	r_s.phase_start = p_time;
	if (p_d.reload_type == ReloadType::SingleBullet) {
		r_s.phase_end = p_time + p_d.reload_delay;
	} else {
		r_s.phase_end = p_time + (is_tactical(r_s, p_d) ? p_d.reload_tactical : p_d.reload_empty);
	}
	r_s.flags = uint8_t(r_s.flags & ~slot_flags::kReloadCommitted);
	r_s.burst_left = 0;
	timed(r_io, p_time, weapon_events::kReloadStarted, p_slot);
}

void commit_reload(Tick_ &r_io, uint8_t p_slot, WeaponSlotState &r_s, const WeaponSimDef &p_d, SimTime p_time) {
	int32_t target = p_d.magazine_capacity;
	if (r_io.mech.empty_reload_unchambered && p_d.reload_type == ReloadType::MagazineWithPossibleShorterReload && r_s.magazine == 0 && target > 1) {
		target -= 1;
	}
	const int32_t need = std::max(0, target - r_s.magazine);
	const int32_t take = p_d.unlimited_ammo ? need : std::min(need, r_s.reserve);
	r_s.magazine += take;
	if (!p_d.unlimited_ammo) {
		r_s.reserve -= take;
	}
	r_s.flags = uint8_t(r_s.flags | slot_flags::kReloadCommitted);
	timed(r_io, p_time, weapon_events::kReloadCommitted, p_slot);
}

void finish_phase(WeaponSlotState &r_s) {
	r_s.phase = WeaponPhase::Ready;
	r_s.phase_start = 0;
	r_s.phase_end = 0;
	r_s.flags = uint8_t(r_s.flags & ~(slot_flags::kReloadCommitted | slot_flags::kBoltWaitsRelease));
}

bool fire_shot(Tick_ &r_io, uint8_t p_slot, WeaponSlotState &r_s, const WeaponSimDef &p_d, SimTime p_time, float p_power) {
	if (r_io.out.shot_count >= kMaxShotsPerTick) {
		++r_io.out.refused_shots;
		return false;
	}
	const Dispersion disp = effective_dispersion(p_d, r_io.sway);
	const float spread = spread_at(r_s.spread, r_s.spread_time, p_time, disp, r_io.mech);

	const uint32_t index = r_io.st.next_shot_index++;
	const uint64_t seed = shot_seed(r_io.match_seed, r_io.st.player_slot, index);
	const Vec3 aim = aim_direction(r_io.cmd.yaw, r_io.cmd.pitch, r_s.recoil);
	const Vec3 dir = sample_cone(aim, spread, unit_float(shot_random(seed, RngPurpose::SpreadRadius)),
			unit_float(shot_random(seed, RngPurpose::SpreadAngle)), r_io.mech.disc);
	const double frac = tick_fraction(p_time);
	const DVec3 e0 = detail::to_d(r_io.ctx.eye_start);
	const DVec3 e1 = detail::to_d(r_io.ctx.eye_end);
	const DVec3 eye = detail::add(e0, detail::scale(detail::sub(e1, e0), frac));
	const DVec3 origin = detail::add(eye, detail::frame_apply(detail::frame_of(detail::to_d(aim)), p_d.projectile.launch_offset));

	ShotEvent &e = r_io.out.shots[r_io.out.shot_count++];
	e = ShotEvent{};
	e.time = p_time;
	e.spawn_time = p_time + p_d.spawn_delay;
	e.seed = seed;
	e.player_slot = r_io.st.player_slot;
	e.shot_index = index;
	e.def = r_s.def;
	e.weapon_slot = p_slot;
	e.pellets = uint8_t(std::clamp(p_d.pellets, 1, int32_t(kMaxPellets)));
	e.origin = detail::to_f(origin);
	e.aim = aim;
	e.direction = dir;
	e.spread = spread;
	const size_t stance = std::min<size_t>(size_t(r_io.sway.stance), 2);
	e.pellet_cone = p_d.pellets > 1 ? p_d.pellet_cone[stance].min_angle : 0.0f;
	e.recoil = r_s.recoil;
	e.tracer = p_d.trace_frequency > 0 && (index % uint32_t(p_d.trace_frequency)) == 0;
	e.power = p_power;
	if (r_io.table.exact_aim) {

		e.direction = aim_direction(r_io.cmd.yaw, r_io.cmd.pitch, RecoilOffset{});
		e.pellet_cone = 0;
	}

	r_s.spread = spread_after_shot(spread, disp);
	r_s.spread_time = p_time;
	const Recoil rec = effective_recoil(p_d, r_io.sway);
	const bool first = (r_s.flags & slot_flags::kFirstShotArmed) != 0;
	r_s.recoil = recoil_after_shot(r_s.recoil, rec, first, p_d.first_shot_recoil_multiplier, unit_float(shot_random(seed, RngPurpose::RecoilHorizontal)));
	r_s.flags = uint8_t(r_s.flags & ~slot_flags::kFirstShotArmed);
	if (!r_io.table.infinite_magazine && !p_d.unlimited_ammo && r_s.magazine > 0) {
		--r_s.magazine;
	}
	r_io.events |= weapon_events::kShot;
	r_io.shot = true;
	return true;
}

enum class Action : uint8_t { None, DeployEnd, BoltEnd, Commit, ReloadEnd, ShellStart, ShellLoaded, InterruptReload, ReloadStart, Shot, HoldStart, Throw };

void process_active(Tick_ &r_io, uint8_t p_slot, WeaponSlotState &r_s, const WeaponSimDef &p_d, SimTime p_block_until, bool p_blocked) {
	const bool trigger_was = (r_s.flags & slot_flags::kTriggerDown) != 0;
	const bool press = r_io.in.fire && !trigger_was;
	const bool release = !r_io.in.fire && trigger_was;
	if (press) {
		r_s.flags = uint8_t(r_s.flags | slot_flags::kPressPending);
	}
	if (!r_io.in.fire) {
		r_s.flags = uint8_t(r_s.flags & ~slot_flags::kPressPending);
	}

	if (r_s.phase == WeaponPhase::Bolt && (r_s.flags & slot_flags::kBoltWaitsRelease) != 0 && !r_io.in.fire) {
		r_s.flags = uint8_t(r_s.flags & ~slot_flags::kBoltWaitsRelease);
		r_s.phase_start = r_io.t0;
		r_s.phase_end = r_io.t0 + p_d.bolt_cycle;
		timed(r_io, r_io.t0, weapon_events::kBoltStarted, p_slot);
	}
	if (r_s.phase == WeaponPhase::Holding && release) {
		r_s.flags = uint8_t(r_s.flags | slot_flags::kReleasePending);
	}
	if (r_io.in.fire && (p_blocked || p_block_until > r_io.t0 || r_s.phase == WeaponPhase::Deploying)) {
		r_io.events |= weapon_events::kFireBlocked;
	}
	if (press && !has_ammo(r_io, r_s, p_d) && !can_reload(r_s, p_d)) {
		r_io.events |= weapon_events::kDryFire;
	}
	const FireModeDef &mode = mode_of(r_s, p_d);
	const SimTime press_ready = press ? r_io.t0 + p_d.pre_fire_delay : r_io.t0;

	SimTime now = r_io.t0;
	for (int iter = 0; iter < kMaxLoopEvents; ++iter) {
		SimTime when = kNever;
		Action act = Action::None;
		auto offer = [&](SimTime p_time, Action p_action) {
			if (p_time < when) {
				when = p_time;
				act = p_action;
			}
		};
		const bool pending = (r_s.flags & slot_flags::kPressPending) != 0;
		switch (r_s.phase) {
			case WeaponPhase::Deploying:
				offer(std::max(now, r_s.phase_end), Action::DeployEnd);
				break;
			case WeaponPhase::Bolt:
				if ((r_s.flags & slot_flags::kBoltWaitsRelease) == 0) {
					offer(std::max(now, r_s.phase_end), Action::BoltEnd);
				}
				break;
			case WeaponPhase::Reloading:
			case WeaponPhase::ReloadShell:
				if (p_d.reload_type == ReloadType::SingleBullet) {

					if (pending && r_s.magazine > 0 && !p_blocked && !r_io.in.reload) {
						offer(now, Action::InterruptReload);
					}
					offer(std::max(now, r_s.phase_end), r_s.phase == WeaponPhase::Reloading ? Action::ShellStart : Action::ShellLoaded);
				} else if ((r_s.flags & slot_flags::kReloadCommitted) == 0) {
					offer(std::max(now, commit_time(r_s, p_d)), Action::Commit);
				} else {
					offer(std::max(now, r_s.phase_end), Action::ReloadEnd);
				}
				break;
			case WeaponPhase::Holding:
				if ((r_s.flags & slot_flags::kReleasePending) != 0) {
					offer(std::max(now, r_s.hold_start + p_d.hold_delay), Action::Throw);
				} else if (p_d.hold_max_time > 0 && p_d.hold_max_time < kMaxDefTime) {
					offer(std::max({ now, r_s.hold_start + p_d.hold_max_time, r_s.hold_start + p_d.hold_delay }), Action::Throw);
				}
				break;
			case WeaponPhase::Ready: {
				const bool ammo = has_ammo(r_io, r_s, p_d);
				if (r_io.in.reload && can_reload(r_s, p_d) && r_s.burst_left == 0) {
					offer(now, Action::ReloadStart);
					break;
				}
				if (!p_blocked && ammo) {
					const SimTime ready = std::max({ now, r_s.next_shot_time, p_block_until });
					switch (mode.logic) {
						case FireLogic::Automatic:
							if (r_io.in.fire) {
								offer(std::max(ready, press_ready), Action::Shot);
							}
							break;
						case FireLogic::Burst:
							if (r_s.burst_left > 0 || pending) {
								offer(std::max(ready, press_ready), Action::Shot);
							}
							break;
						case FireLogic::HoldAndRelease:
							if (pending) {
								offer(std::max(ready, press_ready), Action::HoldStart);
							}
							break;
						default:
							if (pending) {
								offer(std::max(ready, press_ready), Action::Shot);
							}
							break;
					}
				}
				if (!ammo && p_d.always_auto_reload && can_reload(r_s, p_d) && r_s.burst_left == 0 &&
						!(p_d.hold_off_reload_until_fire_release && r_io.in.fire)) {
					offer(std::max(now, r_s.next_shot_time), Action::ReloadStart);
				}
				break;
			}
			default:
				break;
		}
		if (act == Action::None || when >= r_io.t1) {
			break;
		}
		now = when;
		switch (act) {
			case Action::DeployEnd:
				finish_phase(r_s);
				timed(r_io, now, weapon_events::kDeployFinished, p_slot);
				break;
			case Action::BoltEnd:
				finish_phase(r_s);
				timed(r_io, now, weapon_events::kBoltFinished, p_slot);
				break;
			case Action::Commit:
				commit_reload(r_io, p_slot, r_s, p_d, now);
				break;
			case Action::ReloadEnd:
				finish_phase(r_s);
				timed(r_io, now, weapon_events::kReloadFinished, p_slot);
				break;
			case Action::ShellStart:
				if (r_s.magazine >= p_d.magazine_capacity || (r_s.reserve <= 0 && !p_d.unlimited_ammo)) {
					finish_phase(r_s);
					timed(r_io, now, weapon_events::kReloadFinished, p_slot);
				} else {
					r_s.phase = WeaponPhase::ReloadShell;
					r_s.phase_start = now;
					r_s.phase_end = now + p_d.reload_empty;
				}
				break;
			case Action::ShellLoaded:
				++r_s.magazine;
				if (!p_d.unlimited_ammo) {
					--r_s.reserve;
				}
				timed(r_io, now, weapon_events::kShellLoaded, p_slot);
				if (r_s.magazine >= p_d.magazine_capacity || (r_s.reserve <= 0 && !p_d.unlimited_ammo)) {
					finish_phase(r_s);
					timed(r_io, now, weapon_events::kReloadFinished, p_slot);
				} else {
					r_s.phase_start = now;
					r_s.phase_end = now + p_d.reload_empty;
				}
				break;
			case Action::InterruptReload:
				finish_phase(r_s);
				timed(r_io, now, weapon_events::kReloadFinished, p_slot);
				break;
			case Action::ReloadStart:
				start_reload(r_io, p_slot, r_s, p_d, now);
				break;
			case Action::HoldStart:
				r_s.phase = WeaponPhase::Holding;
				r_s.hold_start = now;
				r_s.flags = uint8_t(r_s.flags & ~(slot_flags::kPressPending | slot_flags::kReleasePending));
				timed(r_io, now, weapon_events::kHoldStarted, p_slot);
				break;
			case Action::Throw: {
				const double held = units_to_seconds(now - r_s.hold_start);
				const float power = std::clamp(float(double(p_d.hold_min_power) + double(p_d.hold_power_per_second) * held), p_d.hold_min_power,
						p_d.hold_max_power);
				if (!fire_shot(r_io, p_slot, r_s, p_d, now, power)) {
					iter = kMaxLoopEvents;
					break;
				}
				r_s.next_shot_time = now + mode.interval;
				r_s.phase = WeaponPhase::Ready;
				r_s.flags = uint8_t(r_s.flags & ~(slot_flags::kReleasePending | slot_flags::kPressPending));
				timed(r_io, now, weapon_events::kThrown, p_slot);
				break;
			}
			case Action::Shot: {
				if (!fire_shot(r_io, p_slot, r_s, p_d, now, 1.0f)) {
					iter = kMaxLoopEvents;
					break;
				}
				switch (mode.logic) {
					case FireLogic::Automatic:
						r_s.next_shot_time = now + mode.interval;
						break;
					case FireLogic::Burst:
						if (r_s.burst_left == 0) {
							r_s.burst_left = uint8_t(std::clamp(mode.bullets_per_burst, 1, int32_t(kMaxBulletsPerBurst)));
							r_s.flags = uint8_t(r_s.flags & ~slot_flags::kPressPending);
						}
						--r_s.burst_left;
						if (!has_ammo(r_io, r_s, p_d)) {
							r_s.burst_left = 0;
						}
						r_s.next_shot_time = now + (r_s.burst_left > 0 ? mode.burst_interval : mode.interval);
						break;
					case FireLogic::SingleWithBoltAction:
						r_s.next_shot_time = now + mode.interval;
						r_s.flags = uint8_t(r_s.flags & ~slot_flags::kPressPending);
						if (has_ammo(r_io, r_s, p_d) && p_d.bolt_cycle > 0) {
							r_s.phase = WeaponPhase::Bolt;
							if (p_d.bolt_hold_until_fire_release) {
								r_s.flags = uint8_t(r_s.flags | slot_flags::kBoltWaitsRelease);
								r_s.phase_start = now;
								r_s.phase_end = 0;
							} else {
								r_s.phase_start = now;
								r_s.phase_end = now + p_d.bolt_cycle;
								timed(r_io, now, weapon_events::kBoltStarted, p_slot);
							}
						}
						break;
					default:
						r_s.next_shot_time = now + mode.interval;
						r_s.flags = uint8_t(r_s.flags & ~slot_flags::kPressPending);
						break;
				}
				break;
			}
			case Action::None:
				break;
		}
	}
}

void leave_slot(Tick_ &r_io, uint8_t p_slot, WeaponSlotState &r_s, const WeaponSimDef &p_d, SimTime p_time) {
	if (r_s.phase == WeaponPhase::Reloading || r_s.phase == WeaponPhase::ReloadShell) {
		const bool committed = (r_s.flags & slot_flags::kReloadCommitted) != 0;
		if (p_d.reload_type == ReloadType::SingleBullet) {
			timed(r_io, p_time, weapon_events::kReloadCancelled, p_slot);
		} else if (!committed && p_d.switch_cancels_reload) {
			timed(r_io, p_time, weapon_events::kReloadCancelled, p_slot);
		} else if (!committed) {
			commit_reload(r_io, p_slot, r_s, p_d, p_time);
		}
	}
	finish_phase(r_s);
	r_s.burst_left = 0;
	r_s.flags = uint8_t(r_s.flags & ~(slot_flags::kPressPending | slot_flags::kReleasePending));
}

void sort_timed(WeaponTickOutput &r_out) {

	for (uint8_t i = 1; i < r_out.timed_count; ++i) {
		const TimedWeaponEvent e = r_out.timed[i];
		uint8_t j = i;
		while (j > 0 && r_out.timed[j - 1].time > e.time) {
			r_out.timed[j] = r_out.timed[j - 1];
			--j;
		}
		r_out.timed[j] = e;
	}
}

float eye_height_of(const MotorParams &p_motor, const SoldierState &p_state) {
	const size_t from = std::min<size_t>(uint8_t(p_state.pose), kPoseCount - 1);
	const size_t to = std::min<size_t>(uint8_t(p_state.target_pose), kPoseCount - 1);
	const float a = p_motor.poses[from].eye_height;
	if (from == to || p_state.transition_ticks == 0 || p_state.transition_total == 0) {
		return from == to ? a : p_motor.poses[to].eye_height;
	}
	const float progress = 1.0f - float(std::min(p_state.transition_ticks, p_state.transition_total)) / float(p_state.transition_total);
	return a + (p_motor.poses[to].eye_height - a) * progress;
}

}

core::Status spawn_soldier_weapons(SoldierWeaponsState &r_state, const WeaponTable &p_table, std::span<const uint16_t> p_loadout, uint8_t p_active,
		Tick p_spawn_tick) {
	if (p_loadout.size() > kMaxWeaponSlots) {
		return core::make_error("spawn_soldier_weapons: " + std::to_string(p_loadout.size()) + " slots (limit " + std::to_string(kMaxWeaponSlots) + ")");
	}
	for (const uint16_t id : p_loadout) {
		if (id != kNoWeapon && id >= p_table.defs.size()) {
			return core::make_error("spawn_soldier_weapons: def " + std::to_string(id) + " is not in the table (" + std::to_string(p_table.defs.size()) + ")");
		}
	}
	if (p_active >= p_loadout.size() || p_loadout[p_active] == kNoWeapon) {
		return core::make_error("spawn_soldier_weapons: active slot " + std::to_string(p_active) + " holds no weapon");
	}
	const uint32_t player = r_state.player_slot;
	const uint32_t next = r_state.next_shot_index;
	r_state = SoldierWeaponsState{};
	r_state.player_slot = player;
	r_state.next_shot_index = next;
	r_state.tick = p_spawn_tick;
	r_state.active = p_active;
	for (size_t i = 0; i < p_loadout.size(); ++i) {
		WeaponSlotState &s = r_state.slots[i];
		s = WeaponSlotState{};
		s.def = p_loadout[i];
		if (s.def == kNoWeapon) {
			continue;
		}
		const WeaponSimDef &d = p_table.defs[s.def];
		s.phase = WeaponPhase::Ready;
		s.magazine = d.magazine_capacity;
		s.reserve = d.unlimited_ammo ? 0 : d.magazines * d.magazine_capacity;
		s.flags = slot_flags::kFirstShotArmed;
	}
	WeaponSlotState &a = r_state.slots[p_active];
	a.phase = WeaponPhase::Deploying;
	a.phase_start = tick_end(p_spawn_tick);
	a.phase_end = a.phase_start + p_table.defs[a.def].deploy_time;
	return core::Status::success();
}

void step_soldier_weapons(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, const WeaponContext &p_context,
		uint64_t p_match_seed, WeaponTickOutput &r_out) {
	r_out.shot_count = 0;
	r_out.events = 0;
	r_out.refused_shots = 0;
	r_out.timed_count = 0;
	r_out.timed_dropped = 0;
	if (p_command.tick != r_state.tick + 1) {
		return;
	}
	Tick_ io{ p_table, p_table.mechanics, r_state, p_command, p_context, p_match_seed, r_out, tick_start(p_command.tick), tick_end(p_command.tick),
		weapon_input(p_command), p_context.sway };
	io.sway = suppressed_sway(p_context.sway, p_context.suppression, p_table.mechanics);
	r_state.events = 0;

	bool blocked = !p_context.alive;
	SimTime block_until = io.t0;
	if (p_table.mechanics.sprint_blocks_fire) {
		if (p_context.sprinting) {
			blocked = true;
			r_state.sprint_recover_until = io.t1 + p_table.sprint_recover_time;
		} else {
			block_until = std::max(io.t0, r_state.sprint_recover_until);
		}
	}
	const size_t defs = p_table.defs.size();
	auto valid = [&](const WeaponSlotState &p_s) { return p_s.def != kNoWeapon && p_s.def < defs; };

	if (p_context.alive && r_state.active < kMaxWeaponSlots) {

		if (io.in.select_slot < kMaxWeaponSlots && io.in.select_slot != r_state.active) {
			WeaponSlotState &to = r_state.slots[io.in.select_slot];
			if (valid(to)) {
				WeaponSlotState &from = r_state.slots[r_state.active];
				const uint8_t trigger = uint8_t(from.flags & slot_flags::kTriggerDown);
				if (valid(from)) {
					leave_slot(io, r_state.active, from, p_table.defs[from.def], io.t0);
				}
				r_state.active = io.in.select_slot;
				to.phase = WeaponPhase::Deploying;
				to.phase_start = io.t0;
				to.phase_end = io.t0 + p_table.defs[to.def].deploy_time;
				to.burst_left = 0;
				to.flags = uint8_t((to.flags & ~(slot_flags::kTriggerDown | slot_flags::kPressPending | slot_flags::kReleasePending)) | trigger);
				timed(io, io.t0, weapon_events::kDeployStarted, r_state.active);
			} else {
				io.events |= weapon_events::kSwitchRefused;
			}
		}
		WeaponSlotState &s = r_state.slots[r_state.active];
		if (valid(s)) {
			const WeaponSimDef &d = p_table.defs[s.def];
			const uint8_t steps = fire_mode_steps(s.mode_counter, io.in.fire_mode_counter);
			if (steps > 0 && d.mode_count > 1) {
				s.mode = uint8_t((s.mode + steps) % d.mode_count);
				s.burst_left = 0;
				timed(io, io.t0, weapon_events::kFireModeChanged, r_state.active);
			}
			process_active(io, r_state.active, s, d, block_until, blocked);
		}
	} else if (io.in.fire) {
		io.events |= weapon_events::kFireBlocked;
	}

	for (uint8_t i = 0; i < kMaxWeaponSlots; ++i) {
		WeaponSlotState &s = r_state.slots[i];
		s.mode_counter = io.in.fire_mode_counter;
		if (!valid(s)) {
			continue;
		}
		const WeaponSimDef &d = p_table.defs[s.def];
		const bool active = i == r_state.active;
		s.recoil = recover_recoil_tick(s.recoil, d, io.sway, active && io.shot, p_table.mechanics);
		if (active) {
			s.flags = uint8_t(io.in.fire ? (s.flags | slot_flags::kTriggerDown) : (s.flags & ~slot_flags::kTriggerDown));
			s.flags = uint8_t(io.in.zoom ? (s.flags | slot_flags::kZoomWasDown) : (s.flags & ~slot_flags::kZoomWasDown));
		}
		if ((p_table.mechanics.first_shot_rearm == FirstShotRearm::OnTriggerRelease && active && !io.in.fire) ||
				(p_table.mechanics.first_shot_rearm == FirstShotRearm::WhenRecovered && s.recoil.pitch == 0.0f && s.recoil.yaw == 0.0f)) {
			s.flags = uint8_t(s.flags | slot_flags::kFirstShotArmed);
		}
	}
	sort_timed(r_out);
	r_state.tick = p_command.tick;
	r_state.events = io.events;
	r_out.events = io.events;
}

uint64_t soldier_weapons_hash(const SoldierWeaponsState &p_state) {
	detail::Fnv64 h;
	h.add(p_state.tick);
	h.add(p_state.player_slot);
	h.add(p_state.next_shot_index);
	h.add(p_state.active);
	h.add(p_state.pending);
	h.add(p_state.reserved0);
	h.add(p_state.reserved1);
	h.add(p_state.events);
	h.add(p_state.sprint_recover_until);
	for (const WeaponSlotState &s : p_state.slots) {
		h.add(s.def);
		h.add(s.phase);
		h.add(s.mode);
		h.add(s.mode_counter);
		h.add(s.burst_left);
		h.add(s.flags);
		h.add(s.reserved0);
		h.add(s.magazine);
		h.add(s.reserve);
		h.add(s.phase_start);
		h.add(s.phase_end);
		h.add(s.next_shot_time);
		h.add(s.spread);
		h.add(s.spread_time);
		h.add(s.recoil.pitch);
		h.add(s.recoil.yaw);
		h.add(s.hold_start);
	}
	return h.value();
}

SwayContext weapon_sway_context(const MotorParams &p_motor, const SoldierState &p_after, const Command &p_command) {
	SwayContext sway;
	sway.stance = Stance(std::min<uint8_t>(uint8_t(p_after.target_pose), 2));
	sway.zoom = (p_command.buttons & weapon_buttons::kZoom) != 0 ? Zoom::Zoom : Zoom::NoZoom;
	const float hs2 = p_after.velocity[0] * p_after.velocity[0] + p_after.velocity[2] * p_after.velocity[2];
	if ((p_after.flags & soldier_flags::kSprinting) != 0) {
		sway.move = SwayMove::Sprinting;
	} else if (p_after.mode == MoveMode::Jumping || p_after.mode == MoveMode::InAir || p_after.mode == MoveMode::Falling) {
		sway.move = SwayMove::Jumping;
	} else if (hs2 > 0.0f) {
		sway.move = SwayMove::Moving;
		const float walk = walk_speed(p_motor, p_after);
		sway.move_fraction = walk > 0.0f ? std::min(1.0f, std::sqrt(hs2) / walk) : 1.0f;
	} else {
		sway.move = SwayMove::Base;
	}
	return sway;
}

WeaponContext make_weapon_context(const MotorParams &p_motor, const SoldierState &p_before, const SoldierState &p_after, const Command &p_command,
		float p_suppression) {
	WeaponContext c;
	c.eye_start = { p_before.position[0], p_before.position[1] + eye_height_of(p_motor, p_before), p_before.position[2] };
	c.eye_end = { p_after.position[0], p_after.position[1] + eye_height_of(p_motor, p_after), p_after.position[2] };
	c.yaw = p_command.yaw;
	c.pitch = p_command.pitch;
	c.sway = weapon_sway_context(p_motor, p_after, p_command);
	c.alive = (p_after.flags & soldier_flags::kAlive) != 0 && p_after.mode != MoveMode::Dead;
	c.sprinting = (p_after.flags & soldier_flags::kSprinting) != 0;
	c.suppression = std::isfinite(p_suppression) ? p_suppression : 0.0f;
	return c;
}

}
