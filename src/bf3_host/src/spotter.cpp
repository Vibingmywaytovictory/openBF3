#include "bf3/host/spotter.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace sp = sim::spotting;
namespace cm = sim::commo;

bool place_spot_body(SpotSubject &r_subject, const core::gameplay::SoldierDef &p_soldier, sim::Pose p_pose,
		const sim::Vec3 &p_feet) {
	const core::gameplay::PoseDef *pose = p_soldier.pose(core::gameplay::SoldierPose(p_pose));
	const float r = p_soldier.radius;
	if (pose == nullptr || !(r > 0.0f) || !std::isfinite(r) || !(pose->height > 0.0f) || !std::isfinite(pose->height)) {
		return false;
	}
	const float top = std::max(r, pose->height - r);
	r_subject.body.a = { p_feet[0], p_feet[1] + r, p_feet[2] };
	r_subject.body.b = { p_feet[0], p_feet[1] + top, p_feet[2] };
	r_subject.body.radius = r;
	r_subject.centre = { p_feet[0], p_feet[1] + 0.5f * (r + top), p_feet[2] };
	return true;
}

sim::commo::Supplies supplies_of(std::span<const uint16_t> p_defs, std::span<const sim::weapons::GadgetDef> p_gadgets) {
	cm::Supplies out;
	for (const uint16_t d : p_defs) {
		if (d == sim::weapons::kNoWeapon) {
			continue;
		}
		for (const sim::weapons::GadgetDef &g : p_gadgets) {
			if (g.def == d) {
				out.heals = out.heals || g.healing.active();
				out.ammo = out.ammo || g.ammo.active();
			}
		}
	}
	return out;
}

std::vector<sp::RayHit> spot_ray_hits(const sim::Vec3 &p_from, const sim::Vec3 &p_to,
		std::span<const SpotSubject> p_subjects, uint8_t p_own_slot, const sim::CollisionWorld *p_world) {
	float wall = 1.0f;
	if (p_world != nullptr) {
		sim::StaticHit hit;
		if (p_world->ray(p_from, core::vec::sub(p_to, p_from), sim::query_mask::kProjectile, &hit)) {
			wall = hit.fraction;
		}
	}
	std::vector<std::pair<float, sp::RayHit>> hits;
	for (const SpotSubject &s : p_subjects) {
		if (s.slot == p_own_slot) {
			continue;
		}
		float f = 0.0f;
		sim::Vec3 normal{};
		if (sim::weapons::segment_capsule(p_from, p_to, s.body, f, normal) && f <= wall) {
			hits.push_back({ f, sp::RayHit{ true, s.slot, s.team } });
		}
	}
	std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	std::vector<sp::RayHit> out;
	for (size_t i = 0; i < hits.size() && i < sp::kRayHits; ++i) {
		out.push_back(hits[i].second);
	}
	return out;
}

std::optional<uint8_t> order_teammate(const sim::spotting::SpotterDef &p_def, const SpotterInput &p_in) {
	std::optional<uint8_t> best;
	float best_cos = 0.0f;
	for (const SpotSubject &s : p_in.subjects) {
		if (s.slot == p_in.own_slot || s.team != p_in.own_team || p_in.own_team == 0 ||
				!sp::in_spot_cone(p_in.eye, p_in.view, s.centre, p_def.team_order_fov_deg, p_def.distance)) {
			continue;
		}
		const float c = sp::crosshair_cosine(p_in.eye, p_in.view, s.centre);
		if (c > best_cos) {
			best_cos = c;
			best = s.slot;
		}
	}
	return best;
}

bool Spotter::blocked(const SpotterInput &p_in) const {
	if (!def_.one_spotted) {
		return false;
	}
	return std::any_of(p_in.states.begin(), p_in.states.end(),
			[&](const net::SpotRow &r) { return r.state == sp::SpotState::Active && r.spotter == p_in.own_slot; });
}

bool Spotter::ready(const SpotterInput &p_in) const {
	return !blocked(p_in) && cooldown_.ready(p_in.clock, def_);
}

void Spotter::collect(const SpotterInput &p_in, bool p_crosshair_ray) {
	std::vector<const SpotSubject *> cone;
	for (const SpotSubject &s : p_in.subjects) {
		if (cone.size() == sp::kMaxSpotTargets) {
			break;
		}
		const float fov = def_.fov_deg * (s.seated ? sp::kSeatedConeWidening : 1.0f);
		if (s.slot != p_in.own_slot && sp::in_spot_cone(p_in.eye, p_in.view, s.centre, fov, def_.distance)) {
			cone.push_back(&s);
		}
	}
	const auto take = [&](const std::vector<sp::RayHit> &p_hits) {
		if (const std::optional<uint8_t> e = sp::first_enemy_hit(p_hits, p_in.own_team)) {
			for (const SpotSubject &s : p_in.subjects) {
				if (s.slot == *e) {
					potential_.add(s.slot, sp::crosshair_cosine(p_in.eye, p_in.view, s.centre));
				}
			}
		}
	};
	if (p_crosshair_ray) {
		take(spot_ray_hits(p_in.eye, core::vec::add(p_in.eye, core::vec::scale(p_in.view, def_.distance)),
				p_in.subjects, p_in.own_slot, p_in.world));
	}
	for (const SpotSubject *s : cone) {
		if (s->team != p_in.own_team) {
			take(spot_ray_hits(p_in.eye, s->centre, p_in.subjects, p_in.own_slot, p_in.world));
		}
	}
}

SpotterOutput Spotter::update(const SpotterInput &p_in) {
	SpotterOutput out;
	const bool is_blocked = blocked(p_in);
	if (p_in.pressed) {
		const bool allowed = cooldown_.press(p_in.clock, def_);
		if (!allowed || is_blocked) {
			out.refused = true;
		} else {
			active_pending_ = true;
			collect(p_in, true);
			if (const std::optional<uint8_t> mate = order_teammate(def_, p_in)) {
				for (const SpotSubject &s : p_in.subjects) {
					if (s.slot != *mate) {
						continue;
					}
					const cm::OrderType o = cm::pick_order(def_, p_in.me, s.party);
					if (o != cm::OrderType::None) {
						out.order = net::OrderRequest{ o, s.slot };
					}
				}
			}
		}
	} else if (scan_.scan_due(p_in.dt, false, is_blocked, def_)) {
		active_pending_ = false;
		out.scanned = true;
		collect(p_in, false);
	}
	if (active_pending_) {
		if (def_.one_spotted) {
			potential_.trim_to_nearest_crosshair();
		}
		if (!potential_.empty()) {
			net::SpotRequest r;
			r.type = uint8_t(sp::SpotType::Active);
			for (const sp::Candidate &c : potential_.entries()) {
				r.targets.push_back(c.slot);
			}
			out.spot = std::move(r);
		}
		potential_.clear();
		active_pending_ = false;
		scan_.send_due(true, true, def_);
		return out;
	}
	if (scan_.send_due(potential_.empty(), false, def_)) {
		if (def_.one_spotted) {
			potential_.trim_to_nearest_crosshair();
		}
		if (!potential_.empty()) {
			net::SpotRequest r;
			r.type = uint8_t(sp::SpotType::Passive);
			for (const sp::Candidate &c : potential_.entries()) {
				r.targets.push_back(c.slot);
			}
			out.spot = std::move(r);
		}
		potential_.clear();
	}
	return out;
}

}
