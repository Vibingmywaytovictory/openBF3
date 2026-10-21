#include "bf3/sim_harness/shooting_range.h"

#include "bf3/sim/weapons/surface_materials.h"
#include "weapons/weapons_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace bf3::sim::weapons {

namespace {

using namespace detail;

void add_quad(std::vector<StaticTriangle> &r_out, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, const Vec3 &p_d, uint32_t p_material) {
	StaticTriangle t;
	t.tag.material = p_material;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

uint16_t yaw_to(const Vec3 &p_from, const Vec3 &p_to) {
	return quantize_yaw(std::atan2(p_to[0] - p_from[0], p_to[2] - p_from[2]));
}
int16_t pitch_to(const Vec3 &p_from, const Vec3 &p_to) {
	const float dx = p_to[0] - p_from[0], dy = p_to[1] - p_from[1], dz = p_to[2] - p_from[2];
	return quantize_pitch(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
}

double ms_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p_start).count();
}

const core::gameplay::PoseDef *pose_def(const core::gameplay::SoldierDef &p_soldier, Pose p_pose) {
	return p_soldier.pose(core::gameplay::SoldierPose(std::min<size_t>(size_t(p_pose), kPoseCount - 1)));
}

WeaponInput script_input(const WeaponTable &p_table, const RangeScenario &p_scenario, const SoldierWeaponsState &p_state, Tick p_tick, bool p_script,
		uint32_t p_index, bool p_prev_fire) {
	WeaponInput in;
	in.zoom = p_scenario.ads;
	if (!p_script) {
		return in;
	}
	const WeaponSlotState &s = p_state.current();
	switch (p_scenario.script) {
		case FireScript::Hold:
			in.fire = true;
			break;
		case FireScript::Tap: {

			if (p_prev_fire) {
				break;
			}
			const SimTime end = tick_end(p_tick);
			const bool ammo = p_table.infinite_magazine || s.magazine > 0;
			bool ready = false;
			switch (s.phase) {
				case WeaponPhase::Ready:
					ready = s.next_shot_time < end && s.burst_left == 0;
					break;
				case WeaponPhase::Bolt:
				case WeaponPhase::Deploying:
					ready = (s.flags & slot_flags::kBoltWaitsRelease) == 0 && s.phase_end < end && s.next_shot_time < end;
					break;
				default:
					break;
			}
			in.fire = ammo && ready;
			break;
		}
		case FireScript::TapEvery:
			in.fire = (p_index % std::max<uint32_t>(1, p_scenario.tap_period)) == 0;
			break;
		case FireScript::ReloadAt:
			in.fire = true;
			in.reload = p_index == p_scenario.reload_tick;
			break;
		case FireScript::Throw:
			in.fire = p_index < p_scenario.hold_ticks;
			break;
	}
	return in;
}

}

std::vector<StaticTriangle> range_wall_triangles(std::span<const float> p_wall_distances, float p_eye_height, float p_extent, float p_depth,
		float p_wall_half_width, uint32_t p_material_value) {
	std::vector<StaticTriangle> out;
	const float slab_y = p_eye_height - p_depth;

	constexpr float kCell = 500.0f;
	const float lo = -100.0f;
	const int cells = int(std::ceil((p_extent - lo) / kCell));
	for (int ix = 0; ix < cells && ix < 1000; ++ix) {
		for (int iz = 0; iz < cells && iz < 1000; ++iz) {
			const float x0 = lo + kCell * float(ix), x1 = std::min(p_extent, x0 + kCell);
			const float z0 = lo + kCell * float(iz), z1 = std::min(p_extent, z0 + kCell);
			add_quad(out, { x0, slab_y, z0 }, { x0, slab_y, z1 }, { x1, slab_y, z1 }, { x1, slab_y, z0 }, p_material_value);
		}
	}
	const float top = p_eye_height + 50.0f;
	for (const float d : p_wall_distances) {
		const float w = p_wall_half_width;
		add_quad(out, { -w, slab_y, d }, { -w, top, d }, { w, top, d }, { w, slab_y, d }, p_material_value);
	}
	return out;
}

core::Result<RangeResult> run_range(const WeaponTable &p_table, const CollisionWorld &p_world, const core::gameplay::SoldierDef &p_soldier,
		std::span<const core::gameplay::HitCapsule> p_capsules, const RangeLane &p_lane, const RangeScenario &p_scenario) {
	const auto start = std::chrono::steady_clock::now();
	if (p_scenario.weapon >= p_table.defs.size()) {
		return core::make_error("run_range: weapon " + std::to_string(p_scenario.weapon) + " is not in the table");
	}
	if (p_table.materials == nullptr) {
		return core::make_error("run_range: the table has no SurfaceMaterialTable");
	}
	for (const Pose pose : { Pose::Stand, Pose::Crouch, Pose::Prone }) {
		if (pose_def(p_soldier, pose) == nullptr) {
			return core::make_error("run_range: the soldier lacks a pose");
		}
	}
	if (p_scenario.ticks > 60 * 60 * int(kSimHz) || p_scenario.settle_ticks > 60 * int(kSimHz)) {
		return core::make_error("run_range: a run longer than an hour or a settle longer than a minute");
	}
	RangeResult result;
	result.lane = p_lane;
	const DVec3 dir = range_heading(p_lane.yaw);
	const float shooter_eye = pose_def(p_soldier, p_scenario.shooter_pose)->eye_height;
	const Vec3 eye{ p_lane.feet[0], p_lane.feet[1] + shooter_eye, p_lane.feet[2] };

	std::vector<HitTarget> targets;
	std::vector<RangeTargetSpec> specs = p_scenario.targets;
	for (size_t i = 0; i < specs.size(); ++i) {
		const RangeTargetSpec &spec = specs[i];
		if (!(spec.distance > 0) || spec.distance > p_lane.length - 0.5f) {
			return core::make_error("run_range: a target at " + std::to_string(spec.distance) + " m lies past the lane (" + std::to_string(p_lane.length) +
					" m)");
		}
		Vec3 feet = to_f(add(to_d(p_lane.feet), scale(dir, double(spec.distance))));
		bool found = false;
		const Vec3 floor = range_floor_under(p_world, feet, 1.0f, 1.0f, found);
		if (found && std::fabs(floor[1] - p_lane.feet[1]) <= kRangeLaneFloorTolerance) {
			feet = floor;
		}
		core::Result<HitTarget> t = standin_target(p_soldier, p_capsules, *p_table.materials, spec.pose, feet, uint16_t(p_lane.yaw + 32768u),
				uint32_t(i + 1), 2);
		if (!t) {
			return t.error();
		}
		targets.push_back(*t);
		RangeTargetResult r;
		r.distance = spec.distance;
		r.health = p_soldier.max_health;
		result.targets.push_back(r);
	}
	StaticTargets provider({});
	std::vector<uint32_t> shots_at(targets.size(), 0);

	const std::shared_ptr<const WeaponTable> table(std::shared_ptr<const WeaponTable>(), &p_table);
	ProjectileWorld projectiles(table);
	SoldierWeaponsState st;
	st.player_slot = p_scenario.player_slot;
	const uint16_t loadout[1] = { p_scenario.weapon };
	if (core::Status s = spawn_soldier_weapons(st, p_table, loadout, 0, 0); !s.ok()) {
		return s.error();
	}
	WeaponContext ctx;
	ctx.eye_start = eye;
	ctx.eye_end = eye;
	ctx.sway.stance = Stance(std::min<size_t>(size_t(p_scenario.shooter_pose), 2));
	ctx.sway.zoom = p_scenario.ads ? Zoom::Zoom : Zoom::NoZoom;
	ctx.sway.move = SwayMove::Base;

	WeaponTickOutput out;
	ProjectileStepOutput pout;
	Tick tick = 0;
	auto live_target = [&]() -> int {
		int best = -1;
		for (size_t i = 0; i < targets.size(); ++i) {
			if (!result.targets[i].killed && (best < 0 || specs[i].distance < specs[size_t(best)].distance)) {
				best = int(i);
			}
		}
		return best;
	};
	bool prev_fire = false;
	auto run_tick = [&](bool p_script, uint32_t p_index) {
		++tick;
		const int aim_index = live_target();
		Command c;
		c.tick = tick;
		c.pose = p_scenario.shooter_pose;
		Vec3 point{};
		if (aim_index >= 0 && zone_aim_point(targets[size_t(aim_index)], specs[size_t(aim_index)].aim_zone, point)) {
			c.yaw = yaw_to(eye, point);
			c.pitch = pitch_to(eye, point);
		} else {
			c.yaw = p_lane.yaw;
			c.pitch = 0;
		}
		const WeaponInput in = script_input(p_table, p_scenario, st, tick, p_script, p_index, prev_fire);
		prev_fire = in.fire;
		c.buttons = with_weapon_input(0, in);
		ctx.yaw = c.yaw;
		ctx.pitch = c.pitch;
		step_soldier_weapons(p_table, st, c, ctx, p_scenario.match_seed, out);
		result.weapon_events.push_back(st.events);
		for (uint8_t i = 0; i < out.shot_count; ++i) {
			ShotEvent shot = out.shots[i];
			if (p_scenario.exact_aim) {
				shot.direction = aim_direction(c.yaw, c.pitch, RecoilOffset{});
				shot.pellet_cone = 0;
			}
			result.shots.push_back(shot);
			if (aim_index >= 0) {
				++shots_at[size_t(aim_index)];
			}
			(void)projectiles.spawn(shot);
		}
		provider.targets().clear();
		for (size_t i = 0; i < targets.size(); ++i) {
			if (!result.targets[i].killed) {
				provider.targets().push_back(targets[i]);
			}
		}
		pout.clear();
		projectiles.step(tick, p_world, provider, pout);
		for (const DamageEvent &d : pout.damage) {
			if (d.target == 0 || d.target > targets.size()) {
				continue;
			}
			RangeTargetResult &r = result.targets[d.target - 1];
			if (r.killed) {
				continue;
			}
			r.health -= d.amount;
			r.damage += d.amount;
			++r.hits[std::min<size_t>(size_t(d.zone), kHitZoneCount - 1)];
			if (r.health <= 0.0f) {
				r.health = 0.0f;
				r.killed = true;
				r.killing_shot = d.shot_index;
				r.kill_time = d.time;
				r.shots_at_kill = shots_at[d.target - 1];
			}
		}
		result.damage.insert(result.damage.end(), pout.damage.begin(), pout.damage.end());
		result.impacts.insert(result.impacts.end(), pout.impacts.begin(), pout.impacts.end());
		result.detonations.insert(result.detonations.end(), pout.detonations.begin(), pout.detonations.end());
		for (const ImpactEvent &e : pout.impacts) {
			result.static_hits += e.kind == ImpactKind::Static && !e.penetrated ? 1 : 0;
		}
		result.invisible_passes += pout.invisible_passes;
		result.penetrations += pout.penetrations;
		result.collision_before_raycast += pout.collision_before_raycast;
		result.peak_live_projectiles = std::max(result.peak_live_projectiles, projectiles.live().size());
	};

	for (uint32_t i = 0; i < 60 * kSimHz && st.current().phase != WeaponPhase::Ready; ++i) {
		run_tick(false, 0);
	}
	for (uint32_t i = 0; i < p_scenario.ticks; ++i) {
		run_tick(true, i);
	}
	for (uint32_t i = 0; i < p_scenario.settle_ticks; ++i) {
		run_tick(false, 0);
	}
	result.refused_projectiles = projectiles.refused();
	result.shot_list_hash = shot_list_hash(result.shots);
	result.impact_list_hash = impact_list_hash(result.impacts);
	result.seconds_wall = ms_since(start) / 1000.0;
	return result;
}

core::Result<RangePerfResult> run_range_perf(const WeaponTable &p_table, const CollisionWorld &p_world, std::span<const SpawnPoint> p_spawns,
		float p_eye_height, uint16_t p_weapon, uint32_t p_shooters, uint32_t p_ticks, uint64_t p_match_seed) {
	if (!p_table.infinite_magazine) {
		return core::make_error("run_range_perf: the table must be built with infinite_magazine (W11 is the full-duty load)");
	}
	if (p_weapon >= p_table.defs.size()) {
		return core::make_error("run_range_perf: weapon " + std::to_string(p_weapon) + " is not in the table");
	}
	if (p_shooters == 0 || p_shooters > p_spawns.size()) {
		return core::make_error("run_range_perf: " + std::to_string(p_shooters) + " shooters, " + std::to_string(p_spawns.size()) + " spawn points");
	}
	if (p_ticks == 0 || p_ticks > 60 * 60 * kSimHz) {
		return core::make_error("run_range_perf: tick count out of range");
	}
	RangePerfResult r;
	r.shooters = p_shooters;
	struct Shooter {
		SoldierWeaponsState state;
		WeaponContext ctx;
		uint16_t yaw = 0;
	};
	std::vector<Shooter> shooters(p_shooters);
	double lane_sum = 0;
	const uint16_t loadout[1] = { p_weapon };
	for (uint32_t i = 0; i < p_shooters; ++i) {
		Shooter &s = shooters[i];
		const std::optional<RangeLane> lane = find_range_lane(p_world, p_spawns.subspan(i, 1), p_eye_height, 0.0f, 64);
		bool found = false;
		const Vec3 feet = lane ? lane->feet : range_floor_under(p_world, p_spawns[i].position, 1.0f, 2.0f, found);
		s.yaw = lane ? lane->yaw : p_spawns[i].yaw;
		lane_sum += lane ? double(lane->length) : 0.0;
		s.ctx.eye_start = { feet[0], feet[1] + p_eye_height, feet[2] };
		s.ctx.eye_end = s.ctx.eye_start;
		s.ctx.yaw = s.yaw;
		s.state.player_slot = i;
		if (core::Status st = spawn_soldier_weapons(s.state, p_table, loadout, 0, 0); !st.ok()) {
			return st.error();
		}

		s.state.slots[0].phase = WeaponPhase::Ready;
		s.state.slots[0].phase_start = 0;
		s.state.slots[0].phase_end = 0;
	}
	r.mean_lane_length = float(lane_sum / double(p_shooters));
	const std::shared_ptr<const WeaponTable> table(std::shared_ptr<const WeaponTable>(), &p_table);
	ProjectileWorld projectiles(table);
	const StaticTargets none({});
	WeaponTickOutput out;
	ProjectileStepOutput pout;
	r.tick_ms.reserve(p_ticks);
	for (Tick t = 1; t <= p_ticks; ++t) {
		const auto start = std::chrono::steady_clock::now();
		for (Shooter &s : shooters) {
			Command c;
			c.tick = t;
			c.yaw = s.yaw;
			WeaponInput in;
			in.fire = true;
			c.buttons = with_weapon_input(0, in);
			step_soldier_weapons(p_table, s.state, c, s.ctx, p_match_seed, out);
			for (uint8_t k = 0; k < out.shot_count; ++k) {
				(void)projectiles.spawn(out.shots[k]);
				++r.shots;
			}
		}
		pout.clear();
		projectiles.step(t, p_world, none, pout);
		r.tick_ms.push_back(ms_since(start));
		r.peak_live_projectiles = std::max(r.peak_live_projectiles, projectiles.live().size());
	}
	std::vector<double> sorted = r.tick_ms;
	std::sort(sorted.begin(), sorted.end());
	double sum = 0;
	for (const double v : sorted) {
		sum += v;
	}
	const size_t n = sorted.size();
	auto rank = [&](double p_q) { return sorted[std::min(n - 1, size_t(std::ceil(p_q * double(n))) - 1)]; };
	r.mean_ms = sum / double(n);
	r.p50_ms = rank(0.50);
	r.p99_ms = rank(0.99);
	r.max_ms = sorted.back();
	return r;
}

core::Result<FireTimingResult> run_fire_timing(const WeaponTable &p_table, const RangeScenario &p_scenario, float p_eye_height) {
	if (p_scenario.weapon >= p_table.defs.size()) {
		return core::make_error("run_fire_timing: weapon " + std::to_string(p_scenario.weapon) + " is not in the table");
	}
	if (p_scenario.ticks > 60 * 60 * int(kSimHz) || p_scenario.settle_ticks > 60 * 60 * int(kSimHz)) {
		return core::make_error("run_fire_timing: a run longer than an hour");
	}
	FireTimingResult r;
	SoldierWeaponsState st;
	st.player_slot = p_scenario.player_slot;
	const uint16_t loadout[1] = { p_scenario.weapon };
	if (core::Status s = spawn_soldier_weapons(st, p_table, loadout, 0, 0); !s.ok()) {
		return s.error();
	}
	WeaponContext ctx;
	ctx.eye_start = { 0, p_eye_height, 0 };
	ctx.eye_end = ctx.eye_start;
	ctx.sway.stance = Stance(std::min<size_t>(size_t(p_scenario.shooter_pose), 2));
	ctx.sway.zoom = p_scenario.ads ? Zoom::Zoom : Zoom::NoZoom;
	WeaponTickOutput out;
	Tick tick = 0;
	bool prev_fire = false;
	auto run_tick = [&](bool p_script, uint32_t p_index) {
		++tick;
		Command c;
		c.tick = tick;
		c.pose = p_scenario.shooter_pose;
		const WeaponInput in = script_input(p_table, p_scenario, st, tick, p_script, p_index, prev_fire);
		if (in.fire && !prev_fire) {
			r.presses.push_back(tick);
		}
		prev_fire = in.fire;
		c.buttons = with_weapon_input(0, in);
		step_soldier_weapons(p_table, st, c, ctx, p_scenario.match_seed, out);
		r.shots.insert(r.shots.end(), out.shots.begin(), out.shots.begin() + out.shot_count);
		r.timed.insert(r.timed.end(), out.timed.begin(), out.timed.begin() + out.timed_count);
		r.refused_shots += out.refused_shots;
		r.timed_dropped += out.timed_dropped;
		r.states.push_back(st);
	};
	for (uint32_t i = 0; i < 60 * kSimHz && st.current().phase != WeaponPhase::Ready; ++i) {
		run_tick(false, 0);
	}
	r.first_script_tick = tick + 1;
	for (uint32_t i = 0; i < p_scenario.ticks; ++i) {
		run_tick(true, i);
	}
	for (uint32_t i = 0; i < p_scenario.settle_ticks; ++i) {
		run_tick(false, 0);
	}
	return r;
}

uint64_t shot_list_hash(std::span<const ShotEvent> p_shots) {
	Fnv64 h;
	h.add(uint64_t(p_shots.size()));
	for (const ShotEvent &s : p_shots) {
		h.add(s.time);
		h.add(s.spawn_time);
		h.add(s.seed);
		h.add(s.player_slot);
		h.add(s.shot_index);
		h.add(s.def);
		h.add(s.weapon_slot);
		h.add(s.pellets);
		h.add3(s.origin);
		h.add3(s.aim);
		h.add3(s.direction);
		h.add(s.spread);
		h.add(s.pellet_cone);
		h.add(s.recoil.pitch);
		h.add(s.recoil.yaw);
		h.add(s.tracer);
		h.add(s.power);
	}
	return h.value();
}

uint64_t impact_list_hash(std::span<const ImpactEvent> p_impacts) {
	Fnv64 h;
	h.add(uint64_t(p_impacts.size()));
	for (const ImpactEvent &e : p_impacts) {
		h.add(e.time);
		h.add3(e.position);
		h.add3(e.normal);
		h.add3(e.velocity);
		h.add(e.player_slot);
		h.add(e.shot_index);
		h.add(e.def);
		h.add(e.pellet);
		h.add(e.kind);
		h.add(e.grid_pair);
		h.add(e.material_value);
		h.add(e.physics_material);
		h.add(e.physics_property);
		h.add(e.surface);
		h.add(e.layer);
		h.add(e.penetrated);
		h.add(e.target);
		h.add(e.zone);
	}
	return h.value();
}

}
