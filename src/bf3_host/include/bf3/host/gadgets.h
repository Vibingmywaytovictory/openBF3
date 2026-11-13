#pragma once

#include "bf3/sim/weapons/gadgets.h"
#include "bf3/sim/weapons/guidance.h"
#include "bf3/sim/weapons/locking.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

struct GadgetStats {
	uint64_t deploys = 0;
	uint64_t refused = 0;
	uint64_t heal_ticks = 0;
	double healed = 0;
	uint64_t ammo_ticks = 0;
	uint64_t rounds_refilled = 0;
	uint64_t rounds_replenished = 0;
	uint64_t forgotten = 0;
	uint64_t remote_detonations = 0;
	uint64_t pack_detonations = 0;
	uint64_t lock_samples = 0;
	uint64_t lock_candidates = 0;
	uint64_t lock_rays = 0;
	uint64_t locks_gained = 0;
	uint64_t guided_launches = 0;
	uint64_t paint_samples = 0;
	uint64_t painted_candidates = 0;
	uint64_t radar_sweeps = 0;
	uint64_t sweep_hits = 0;
	sim::weapons::GadgetFieldStats field;
};

struct LevelVehicleKind;

class LockGuidance final : public sim::weapons::GuidanceTargets {
public:
	void set(std::span<const sim::weapons::LockCandidate> p_candidates);
	std::span<const sim::weapons::LockCandidate> candidates() const { return candidates_; }
	std::optional<sim::weapons::GuidanceTarget> guidance_target(uint32_t p_id) const override;

private:
	std::vector<sim::weapons::LockCandidate> candidates_;
};

sim::weapons::LockCandidate lock_candidate_of(const LevelVehicleKind &p_kind, uint16_t p_spawner, uint8_t p_team,
		const sim::Vec3 &p_com, const std::array<float, 4> &p_rotation, bool p_occupied);

uint64_t gadget_defs_hash(
		std::span<const sim::weapons::GadgetDef> p_gadgets, std::span<const sim::weapons::AmmoPickupDef> p_pickups);

}
