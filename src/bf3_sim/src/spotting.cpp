#include "bf3/sim/spotting.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::sim::spotting {

namespace {

struct Bound {
	const char *name;
	double max;
	float *out;
};

core::Status read_bounds(std::string_view p_type, std::span<const std::pair<std::string, double>> p_scalars,
		std::span<const Bound> p_bounds) {
	for (const Bound &b : p_bounds) {
		const std::optional<double> v = core::gameplay::scalar_field(p_scalars, b.name);
		if (!v || !std::isfinite(*v) || *v < 0.0 || *v > b.max) {
			return core::make_error("spotting: " + std::string(p_type) + "." + b.name +
					(v ? " = " + std::to_string(*v) + " is out of range" : " is missing"));
		}
		*b.out = float(*v);
	}
	return core::Status::success();
}

bool excluded(std::span<const std::pair<std::string, double>> p_scalars) {
	return p_scalars.empty() || core::gameplay::scalar_field(p_scalars, "Excluded").value_or(0.0) != 0.0;
}

}

core::Result<SpottingDef> make_spotting_def(const core::gameplay::SoldierDef &p_soldier) {
	SpottingDef out;
	if (!excluded(p_soldier.spotting)) {
		SpotterDef &s = out.spotter;
		float allowed = 0;
		float one = 0;
		const Bound bounds[] = { { "SpottingFov", 180.0, &s.fov_deg }, { "TeamOrderFov", 180.0, &s.team_order_fov_deg },
			{ "SpottingDistance", 100000.0, &s.distance },
			{ "PassiveSpottingTimeInterval", 3600.0, &s.passive_interval },
			{ "TimeRequiredToPassiveSpot", 3600.0, &s.passive_required },
			{ "CoolDownHistoryTime", 3600.0, &s.cooldown_history_time },
			{ "CoolDownAllowedSpotsWithinHistory", 1000.0, &allowed },
			{ "OnlyAllowedToHaveOneSpottedPlayer", 1.0, &one },
			{ "OrderNeedAmmoPercentage", 100.0, &s.order_need_ammo_percent },
			{ "OrderHealPercentage", 100.0, &s.order_heal_percent },
			{ "OrderRepairPercentage", 100.0, &s.order_repair_percent },
			{ "PickupOrderDistance", 100000.0, &s.pickup_order_distance },
			{ "HealOrderDistance", 100000.0, &s.heal_order_distance },
			{ "AmmoOrderDistance", 100000.0, &s.ammo_order_distance },
			{ "RepairOrderDistance", 100000.0, &s.repair_order_distance } };
		BF3_TRY(read_bounds("SpottingComponentData", p_soldier.spotting, bounds));
		s.cooldown_allowed = uint32_t(allowed);
		s.one_spotted = one != 0.0f;
		out.spotter_present = true;
	}
	if (!excluded(p_soldier.spotting_target)) {
		TargetDef &t = out.target;
		const Bound bounds[] = { { "ActiveSpottedTime", 3600.0, &t.active_time },
			{ "PassiveSpottedTime", 3600.0, &t.passive_time },
			{ "ActiveSpottedTimeMultiplier", 100.0, &t.active_multiplier },
			{ "PassiveSpottedTimeMultiplier", 100.0, &t.passive_multiplier },
			{ "SpotOnFireMultiplier", 100.0, &t.on_fire_multiplier } };
		BF3_TRY(read_bounds("SpottingTargetComponentData", p_soldier.spotting_target, bounds));
		out.target_present = true;
	}
	return out;
}

bool apply_active(SpotTarget &r_target, const TargetDef &p_def, uint8_t p_spotter) {
	if (r_target.state == SpotState::Unspottable || r_target.state == SpotState::Active) {
		return false;
	}
	r_target.remaining = p_def.active_time * p_def.active_multiplier;
	r_target.spotter = p_spotter;
	r_target.state = SpotState::Active;
	return true;
}

bool apply_passive(SpotTarget &r_target, const TargetDef &p_def, uint8_t p_spotter, bool p_firing) {
	switch (r_target.state) {
		case SpotState::Unspottable:
			return false;
		case SpotState::Active:
			if (!p_firing) {
				r_target.remaining = p_def.active_time * p_def.active_multiplier;
			}
			return false;
		case SpotState::Radar:
			r_target.remaining =
					p_def.passive_time * p_def.passive_multiplier * (p_firing ? p_def.on_fire_multiplier : 1.0f);
			return false;
		case SpotState::None:
		case SpotState::Passive:
			break;
	}
	const bool changed = r_target.state != SpotState::Passive || r_target.spotter != p_spotter;
	r_target.remaining = p_def.passive_time * p_def.passive_multiplier * (p_firing ? p_def.on_fire_multiplier : 1.0f);
	r_target.spotter = p_spotter;
	r_target.state = SpotState::Passive;
	return changed;
}

bool step_target(SpotTarget &r_target, float p_dt) {
	if (r_target.state == SpotState::None) {
		return false;
	}
	if (r_target.remaining > 0.0f) {
		r_target.remaining -= p_dt;
		return false;
	}
	r_target.state = SpotState::None;
	r_target.spotter = kNoSpotter;
	return true;
}

float crosshair_cosine(const Vec3 &p_eye, const Vec3 &p_view, const Vec3 &p_centre) {
	const Vec3 d = core::vec::sub(p_centre, p_eye);
	const float len = core::vec::length(d);
	if (!(len > 0.0f)) {
		return 1.0f;
	}
	return core::vec::dot(p_view, core::vec::scale(d, 1.0f / len));
}

bool in_spot_cone(
		const Vec3 &p_eye, const Vec3 &p_view, const Vec3 &p_centre, float p_half_angle_deg, float p_distance) {
	const Vec3 d = core::vec::sub(p_centre, p_eye);
	if (core::vec::dot(d, d) > p_distance * p_distance) {
		return false;
	}
	const float limit = float(std::cos(double(p_half_angle_deg * 3.1415927f / 180.0f)));
	return limit <= crosshair_cosine(p_eye, p_view, p_centre);
}

bool SpotCooldown::press(double p_clock, const SpotterDef &p_def) {
	history_.push_back(p_clock);
	std::erase_if(history_, [&](double t) { return p_clock - t > double(p_def.cooldown_history_time); });
	return history_.size() <= p_def.cooldown_allowed;
}

bool SpotCooldown::ready(double p_clock, const SpotterDef &p_def) const {
	const size_t held = size_t(std::count_if(history_.begin(), history_.end(),
			[&](double t) { return p_clock - t <= double(p_def.cooldown_history_time); }));
	return held < p_def.cooldown_allowed;
}

std::optional<uint8_t> first_enemy_hit(std::span<const RayHit> p_hits, uint8_t p_own_team) {
	for (const RayHit &h : p_hits.first(std::min(p_hits.size(), kRayHits))) {
		if (h.spottable && h.team != 0 && h.team != p_own_team) {
			return h.slot;
		}
	}
	return std::nullopt;
}

bool PotentialList::add(uint8_t p_slot, float p_cosine) {
	for (size_t i = 0; i < count_; ++i) {
		if (entries_[i].slot == p_slot) {
			return true;
		}
	}
	if (count_ == entries_.size()) {
		return false;
	}
	entries_[count_++] = Candidate{ p_slot, p_cosine };
	return true;
}

void PotentialList::trim_to_nearest_crosshair() {
	std::optional<Candidate> best;
	for (const Candidate &c : entries()) {
		if (c.cosine > 0.0f && (!best || c.cosine > best->cosine)) {
			best = c;
		}
	}
	count_ = 0;
	if (best) {
		entries_[count_++] = *best;
	}
}

bool PassiveScan::scan_due(float p_dt, bool p_pressed, bool p_blocked, const SpotterDef &p_def) {
	if (p_pressed) {
		return false;
	}
	timer_ -= p_dt;
	if (timer_ > 0.0f || p_blocked) {
		return false;
	}
	timer_ = p_def.passive_interval;
	return true;
}

bool PassiveScan::send_due(bool p_list_empty, bool p_active_pending, const SpotterDef &p_def) {
	if (!primed_ || p_list_empty || p_active_pending) {
		accumulator_ = p_def.passive_required;
		primed_ = true;
		if (p_list_empty || p_active_pending) {
			return false;
		}
	}
	accumulator_ -= p_def.passive_interval;
	if (accumulator_ < 0.0f) {
		accumulator_ = p_def.passive_required;
		return true;
	}
	return false;
}

}
