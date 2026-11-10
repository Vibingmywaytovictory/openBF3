#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/commo.h"
#include "bf3/sim/spotting.h"
#include "bf3/sim/weapons/gadgets.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

inline constexpr float kSpotSubjectRadius = 0.35f;

struct SpotSubject {
	uint8_t slot = 0;
	uint8_t team = 0;
	sim::Vec3 centre{};
	float half_height = 0.9f;
	bool seated = false;
	sim::commo::OrderParty party;
};

struct SpotterInput {
	double clock = 0;
	float dt = 0;
	bool pressed = false;
	uint8_t own_slot = 0;
	uint8_t own_team = 0;
	sim::Vec3 eye{};
	sim::Vec3 view{};
	sim::commo::OrderParty me;
	std::span<const SpotSubject> subjects;
	std::span<const net::SpotRow> states;
	const sim::CollisionWorld *world = nullptr;
};

struct SpotterOutput {
	std::optional<net::SpotRequest> spot;
	std::optional<net::OrderRequest> order;
	bool refused = false;
	bool scanned = false;
};

sim::commo::Supplies supplies_of(std::span<const uint16_t> p_defs, std::span<const sim::weapons::GadgetDef> p_gadgets);

std::vector<sim::spotting::RayHit> spot_ray_hits(const sim::Vec3 &p_from, const sim::Vec3 &p_to,
		std::span<const SpotSubject> p_subjects, uint8_t p_own_slot, const sim::CollisionWorld *p_world);

std::optional<uint8_t> order_teammate(const sim::spotting::SpotterDef &p_def, const SpotterInput &p_in);

class Spotter {
public:
	explicit Spotter(const sim::spotting::SpotterDef &p_def) : def_(p_def) {}

	SpotterOutput update(const SpotterInput &p_in);
	const sim::spotting::SpotterDef &def() const { return def_; }
	bool blocked(const SpotterInput &p_in) const;

private:
	void collect(const SpotterInput &p_in, bool p_crosshair_ray);

	sim::spotting::SpotterDef def_;
	sim::spotting::SpotCooldown cooldown_;
	sim::spotting::PassiveScan scan_;
	sim::spotting::PotentialList potential_;
	bool active_pending_ = false;
};

}
