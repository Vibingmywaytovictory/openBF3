#include "bf3/present/vehicle_damage_fx.h"

#include "bf3/core/util/splitmix64.h"

#include <algorithm>
#include <set>
#include <utility>

namespace bf3::present {

VehicleDamageFx::VehicleDamageFx(std::vector<core::gameplay::VehicleDamageVisualsDef> p_kinds) :
		kinds_(std::move(p_kinds)) {}

size_t VehicleDamageFx::running() const {
	size_t n = 0;
	for (const auto &[spawner, shown] : shown_) {
		n += size_t(std::count(shown.on.begin(), shown.on.end(), true));
	}
	return n;
}

void VehicleDamageFx::stop_all(uint16_t p_spawner, Shown &r_shown, std::vector<VehicleFxCommand> &r_out) {
	for (size_t i = 0; i < r_shown.on.size(); ++i) {
		if (r_shown.on[i]) {
			r_out.push_back(
					{ VehicleFxOp::StopDamage, p_spawner, r_shown.generation, r_shown.kind, static_cast<uint16_t>(i) });
			r_shown.on[i] = false;
		}
	}
}

void VehicleDamageFx::step(
		std::span<const VehicleFxSight> p_seen, double p_now_s, uint64_t p_seed, std::vector<VehicleFxCommand> &r_out) {
	std::set<uint16_t> seen;
	for (const VehicleFxSight &s : p_seen) {
		if (s.kind >= kinds_.size() || !seen.insert(s.spawner).second) {
			continue;
		}
		const core::gameplay::VehicleDamageVisualsDef &def = kinds_[s.kind];
		auto [it, added] = shown_.try_emplace(s.spawner);
		Shown &shown = it->second;
		if (!added && (shown.generation != s.generation || shown.kind != s.kind)) {
			stop_all(s.spawner, shown, r_out);
		}
		if (added || shown.generation != s.generation || shown.kind != s.kind) {
			shown.generation = s.generation;
			shown.kind = s.kind;
			shown.on.assign(def.damage.size(), false);
		}
		if (s.explosion && def.wreck) {
			stop_all(s.spawner, shown, r_out);
			r_out.push_back({ VehicleFxOp::StartWreck, s.spawner, s.generation, s.kind, 0 });
			const std::vector<bool> effects = core::gameplay::roll_wreck_effects(
					*def.wreck, core::splitmix64(p_seed ^ (uint64_t(s.spawner) << 8) ^ s.generation));
			for (size_t e = 0; e < effects.size(); ++e) {
				if (effects[e]) {
					r_out.push_back({ VehicleFxOp::StartWreckEffect, s.spawner, s.generation, s.kind,
							static_cast<uint16_t>(e) });
				}
			}
			wrecks_.push_back({ s.spawner, s.generation, s.kind, p_now_s + double(def.wreck->time_to_live),
					!(def.wreck->time_to_live > 0.0f) });
		}
		if (s.hidden) {
			stop_all(s.spawner, shown, r_out);
			continue;
		}
		for (size_t i = 0; i < def.damage.size(); ++i) {
			const core::gameplay::VehicleDamageEffectDef &d = def.damage[i];
			const bool want = core::gameplay::health_compare(d.compare, s.health, d.threshold);
			if (want != shown.on[i]) {
				r_out.push_back({ want ? VehicleFxOp::StartDamage : VehicleFxOp::StopDamage, s.spawner, s.generation,
						s.kind, static_cast<uint16_t>(i) });
				shown.on[i] = want;
			}
		}
	}
	for (auto it = shown_.begin(); it != shown_.end();) {
		if (seen.count(it->first) != 0) {
			++it;
			continue;
		}
		stop_all(it->first, it->second, r_out);
		it = shown_.erase(it);
	}
	for (auto it = wrecks_.begin(); it != wrecks_.end();) {
		if (it->lasting || it->until > p_now_s) {
			++it;
			continue;
		}
		r_out.push_back({ VehicleFxOp::EndWreck, it->spawner, it->generation, it->kind, 0 });
		it = wrecks_.erase(it);
	}
}

}
