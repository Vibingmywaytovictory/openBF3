#include "bf3/host/duel.h"

#include "bf3/core/util/format.h"
#include "bf3/host/gunplay.h"
#include "bf3/sim/command.h"

#include <array>
#include <cmath>
#include <memory>

namespace bf3::host {

namespace {

sim::Vec3 add(const sim::Vec3 &p_a, const sim::Vec3 &p_b, float p_s) {
	return sim::Vec3{ p_a[0] + p_b[0] * p_s, p_a[1] + p_b[1] * p_s, p_a[2] + p_b[2] * p_s };
}

float flat_distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

double percent(uint64_t p_part, uint64_t p_whole) {
	return p_whole > 0 ? 100.0 * double(p_part) / double(p_whole) : 0.0;
}

}

std::optional<DuelSite> find_duel_site(
		const sim::CollisionWorld &p_world, const SharedLevel &p_level, float p_distance, float p_half) {
	const sim::CollisionWorld &w = p_world;
	const float eye = p_level.gunplay != nullptr && !p_level.gunplay->soldier.poses.empty()
			? p_level.gunplay->soldier.poses[0].eye_height
			: 1.6f;
	constexpr float kRadius = 0.35f, kHeight = 1.8f, kLift = 0.15f;
	const sim::Vec3 up{ 0.0f, 1.0f, 0.0f };
	DuelSite out;
	for (const sim::SpawnPoint &sp : p_level.spawns) {
		sim::StaticHit g;
		if (!w.ground_below(add(sp.position, up, 1.0f), 3.0f, sim::query_mask::kMovement, &g)) {
			continue;
		}
		const sim::Vec3 feet = g.position;
		if (w.overlap_capsule(add(feet, up, kLift), kRadius, kHeight - kLift, sim::query_mask::kMovement, 0.01f)) {
			continue;
		}
		const sim::Vec3 eye_at = add(feet, up, eye);
		for (uint32_t h = 0; h < 64; ++h) {
			++out.candidates;
			const uint16_t yaw = uint16_t(h * 1024u);
			const float yr = sim::yaw_radians(yaw);
			const sim::Vec3 dir{ std::sin(yr), 0.0f, std::cos(yr) };
			const sim::Vec3 across{ std::cos(yr), 0.0f, -std::sin(yr) };
			if (w.ray(eye_at, add({}, dir, p_distance + 3.0f), sim::query_mask::kProjectile)) {
				continue;
			}
			sim::StaticHit gc;
			if (!w.ground_below(add(add(feet, dir, p_distance), up, 1.5f), 4.0f, sim::query_mask::kMovement, &gc)) {
				continue;
			}
			const sim::Vec3 centre = gc.position;
			bool ok = std::fabs(centre[1] - feet[1]) <= 0.5f;
			for (float o = -p_half; ok && o <= p_half + 1e-3f; o += 0.5f) {
				const sim::Vec3 at = add(centre, across, o);
				sim::StaticHit gp;
				ok = w.ground_below(add(at, up, 1.0f), 2.0f, sim::query_mask::kMovement, &gp) &&
						std::fabs(gp.position[1] - centre[1]) <= 0.1f;
				for (float hgt : { 0.3f, 1.0f, 1.6f }) {
					const sim::Vec3 p = add(at, up, hgt);
					ok = ok &&
							!w.ray(eye_at, { p[0] - eye_at[0], p[1] - eye_at[1], p[2] - eye_at[2] },
									sim::query_mask::kProjectile);
				}
			}
			ok = ok &&
					!w.sweep_capsule(add(add(centre, across, -p_half), up, kLift), kRadius, kHeight - kLift,
							add({}, across, 2.0f * p_half), sim::query_mask::kMovement);
			if (!ok) {
				continue;
			}
			out.shooter = feet;
			out.shooter_yaw = yaw;
			out.target = centre;
			out.target_yaw = uint16_t(yaw + 32768u);
			out.spawn_index = sp.index;
			out.distance = p_distance;
			return out;
		}
	}
	return std::nullopt;
}

core::Result<DuelSite> duel_site(const SharedLevel &p_level) {
	if (p_level.static_world == nullptr) {
		return core::make_error("duel site: the level has no static world");
	}
	core::Result<std::unique_ptr<sim::CollisionWorld>> made = sim::CollisionWorld::create(p_level.static_world);
	if (!made) {
		return core::error_at("duel site", made.error());
	}
	for (float d : { 20.0f, 16.0f, 12.0f }) {
		if (const std::optional<DuelSite> s = find_duel_site(**made, p_level, d, kDuelLaneHalf)) {
			return *s;
		}
	}
	return core::make_error(
			"duel site: no 12-20 m site among " + std::to_string(p_level.spawns.size()) + " spawns x 64 headings");
}

sim::Vec3 present_own_eye(const PresentationFrame &p_frame) {
	const float t = float(p_frame.tick_fraction);
	sim::Vec3 e;
	for (size_t k = 0; k < 3; ++k) {
		e[k] = p_frame.own_prev.position[k] + (p_frame.own_curr.position[k] - p_frame.own_prev.position[k]) * t;
	}
	e[1] += p_frame.eye_height_prev + (p_frame.eye_height_curr - p_frame.eye_height_prev) * t;
	return e;
}

ViewAim aim_at(const sim::Vec3 &p_eye, const sim::Vec3 &p_point) {
	const float dx = p_point[0] - p_eye[0], dy = p_point[1] - p_eye[1], dz = p_point[2] - p_eye[2];
	return ViewAim{ std::atan2(dx, dz), std::atan2(dy, std::sqrt(dx * dx + dz * dz)) };
}

void DuelStrafe::set_lane(const sim::Vec3 &p_origin, float p_yaw0) {
	lane_origin_ = p_origin;
	lane_yaw0_ = p_yaw0;
	++lane_generation_;
}

void DuelStrafe::step(const sim::SoldierState &p_own, double p_t, InputSample &r_input) {
	if (!init_ || life_ != p_own.life || generation_ != lane_generation_) {
		init_ = true;
		life_ = p_own.life;
		generation_ = lane_generation_;
		origin_ = lane_generation_ != 0 ? lane_origin_ : p_own.position;
		yaw0_ = lane_generation_ != 0 ? lane_yaw0_ : sim::yaw_radians(p_own.yaw);
		dir_ = 1;
		leg_start_ = p_t;
	}
	const float side = yaw0_ + 1.5707963f;
	const float sx = std::sin(side), sz = std::cos(side);
	const float off = (p_own.position[0] - origin_[0]) * sx + (p_own.position[2] - origin_[2]) * sz;
	const int before = dir_;
	if (off > kDuelLaneTurn) {
		dir_ = -1;
	} else if (off < -kDuelLaneTurn) {
		dir_ = 1;
	} else if (p_t - leg_start_ > kDuelLegSeconds) {
		dir_ = -dir_;
	}
	if (dir_ != before) {
		leg_start_ = p_t;
	}
	r_input.yaw = dir_ > 0 ? side : yaw0_ - 1.5707963f;
	r_input.pitch = 0.0f;
	r_input.move_forward = 1.0f;
	r_input.held_buttons = 0;
}

DuelShooter::DuelShooter(const DuelSite &p_site, uint32_t p_shots) : site_(p_site), want_shots_(p_shots) {}

void DuelShooter::fail(const std::string &p_why) {
	failure_ = p_why;
	phase_ = DuelPhase::Failed;
}

void DuelShooter::enter(DuelPhase p_phase, double p_now_s) {
	phase_ = p_phase;
	phase_s_ = p_now_s;
	sent_s_ = -1.0;
	settled_s_ = -1.0;
	deployed_s_ = -1.0;
}

DuelPhase DuelShooter::frame(
		double p_now_s, uint64_t p_now_ns, const PresentationFrame &p_frame, bool p_alive, const DuelShooterIo &p_io) {
	if (phase_ == DuelPhase::Done || phase_ == DuelPhase::Failed) {
		return phase_;
	}
	if (phase_s_ < 0.0) {
		phase_s_ = p_now_s;
	}
	const bool playing = p_alive && p_frame.valid;
	if (!playing && (phase_ == DuelPhase::Teleport || phase_ == DuelPhase::Settle || phase_ == DuelPhase::Fire)) {
		if (zoomed_) {
			p_io.hold_zoom(false);
			zoomed_ = false;
		}
		enter(DuelPhase::Spawn, p_now_s);
	}
	std::array<PresentedSoldier, 8> drawn;
	const size_t n = playing ? present_others(p_frame, p_now_ns, drawn) : 0;
	const PresentedSoldier *target = n > 0 ? &drawn[0] : nullptr;
	const auto aim = [&] {
		const ViewAim a = aim_at(present_own_eye(p_frame),
				{ target->position[0], target->position[1] + kDuelAimHeight, target->position[2] });
		p_io.view(a.yaw, a.pitch);
	};
	switch (phase_) {
		case DuelPhase::Spawn:
			if (playing) {
				life_ = p_frame.own_curr.life;
				enter(DuelPhase::Teleport, p_now_s);
				break;
			}
			if (p_now_s - phase_s_ > kDuelSpawnTimeoutSeconds) {
				fail(core::fmt("no spawn within %.0f s", kDuelSpawnTimeoutSeconds));
				break;
			}
			if (deployed_s_ < 0.0 || p_now_s - deployed_s_ >= 1.0) {
				p_io.deploy();
				deployed_s_ = p_now_s;
			}
			break;
		case DuelPhase::Teleport:
			p_io.view(sim::yaw_radians(site_.shooter_yaw), 0.0f);
			if (p_frame.own_curr.life != life_) {
				life_ = p_frame.own_curr.life;
				sent_s_ = -1.0;
			}
			if (sent_s_ < 0.0 || p_now_s - sent_s_ >= kDuelTeleportResendSeconds) {
				net::DebugRequest q;
				q.kind = net::DebugKind::Teleport;
				q.id = ++teleports_;
				q.position = { site_.shooter[0], site_.shooter[1] + 0.05f, site_.shooter[2] };
				q.yaw = site_.shooter_yaw;
				p_io.debug(q);
				sent_s_ = p_now_s;
			}
			if (p_frame.server_own_valid && p_frame.server_own.life == life_ &&
					flat_distance(p_frame.server_own.position, site_.shooter) <= kDuelAtSiteMetres) {
				enter(DuelPhase::Settle, p_now_s);
			} else if (p_now_s - phase_s_ > kDuelTeleportTimeoutSeconds) {
				fail(core::fmt("not at the duel site %.0f s after %u teleport requests (are dev commands on?)",
						kDuelTeleportTimeoutSeconds, teleports_));
			}
			break;
		case DuelPhase::Settle:
			if (target == nullptr) {
				settled_s_ = -1.0;
			} else {
				aim();
				const float speed = std::sqrt(
						target->velocity[0] * target->velocity[0] + target->velocity[2] * target->velocity[2]);
				if (flat_distance(target->position, site_.target) <= kDuelLaneNearMetres &&
						speed > kDuelMovingMetresPerSecond) {
					settled_s_ = settled_s_ < 0.0 ? p_now_s : settled_s_;
				}
			}
			if (settled_s_ >= 0.0 && p_now_s - settled_s_ >= kDuelSettleSeconds) {
				base_shots_ = p_frame.stats.predicted_shots;
				last_press_ = 0;
				enter(DuelPhase::Fire, p_now_s);
			} else if (p_now_s - phase_s_ > kDuelSettleTimeoutSeconds) {
				fail(core::fmt("no target strafed on the lane within %.0f s", kDuelSettleTimeoutSeconds));
			}
			break;
		case DuelPhase::Fire:
			if (!zoomed_) {
				p_io.hold_zoom(true);
				zoomed_ = true;
			}
			if (target != nullptr) {
				aim();
				if (p_frame.tick >= last_press_ + kDuelFireTicks) {
					p_io.fire();
					last_press_ = p_frame.tick;
				}
			}
			shots_ = p_frame.stats.predicted_shots - base_shots_;
			if (shots_ >= want_shots_ || p_now_s - phase_s_ > double(want_shots_) / 4.0 + 60.0) {
				p_io.hold_zoom(false);
				zoomed_ = false;
				enter(DuelPhase::Tail, p_now_s);
			}
			break;
		case DuelPhase::Tail:
			if (p_now_s - phase_s_ >= kDuelTailSeconds) {
				enter(DuelPhase::Done, p_now_s);
			}
			break;
		case DuelPhase::Done:
		case DuelPhase::Failed:
			break;
	}
	return phase_;
}

std::string DuelShooter::report(const ClientStats &p_stats) const {
	const OwnHitCounts &h = p_stats.own_hits;
	return core::fmt("shots %llu of %u, predicted hits %llu, confirmed %llu (%.2f %%), same zone %llu (%.2f %%), "
					 "unpredicted %llu; rtt %.1f ms, late_sample %llu, track_timeout %llu, teleports %u%s%s",
			(unsigned long long)shots_, want_shots_, (unsigned long long)h.predicted, (unsigned long long)h.confirmed,
			percent(h.confirmed, h.predicted), (unsigned long long)h.same_zone, percent(h.same_zone, h.confirmed),
			(unsigned long long)h.unpredicted, p_stats.rtt_ms, (unsigned long long)p_stats.late_sample,
			(unsigned long long)p_stats.track_timeout, teleports_,
			failure_.empty() ? "" : ", failed: ", failure_.c_str());
}

}
