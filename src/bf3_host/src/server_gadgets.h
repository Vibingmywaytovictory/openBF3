#pragma once

#include "bf3/host/gadgets.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/gadgets.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

struct GunplayLevel;
class SquadTable;

struct GadgetSoldier {
	sim::SoldierState *state = nullptr;
	sim::weapons::SoldierWeaponsState *weapons = nullptr;
	uint8_t team = 0;
};

class ServerGadgets {
public:
	ServerGadgets(const GunplayLevel &p_level, std::shared_ptr<const sim::weapons::WeaponTable> p_table);

	bool is_gadget(uint16_t p_def) const { return field_.gadget_of(p_def) != nullptr; }
	sim::weapons::DeployOutcome deploy(const sim::weapons::ShotEvent &p_shot, uint8_t p_team, sim::Tick p_tick,
			const sim::CollisionWorld *p_world);
	void armed(uint8_t p_slot, const sim::weapons::SoldierWeaponsState &p_weapons);
	void step(sim::Tick p_tick, const sim::CollisionWorld *p_world, std::span<const GadgetSoldier> p_soldiers,
			float p_max_health);
	void blast(std::span<const sim::weapons::DetonationEvent> p_detonations, const sim::CollisionWorld *p_world,
			std::span<const uint8_t> p_teams, bool p_friendly_fire);
	void forget(uint8_t p_slot);
	void owner_left(uint8_t p_slot);
	void clear();

	std::optional<sim::Vec3> squad_beacon(uint8_t p_slot, uint8_t p_team, const SquadTable *p_squads) const;
	std::span<const sim::weapons::Deployable> live() const { return field_.live(); }
	uint64_t revision() const { return field_.revision(); }
	const GadgetStats &stats() const;

private:
	const GunplayLevel *level_ = nullptr;
	std::shared_ptr<const sim::weapons::WeaponTable> table_;
	sim::weapons::GadgetField field_;
	std::array<sim::weapons::AmmoRefillState, net::kMaxSlots> refill_{};
	std::vector<sim::weapons::SupplyReceiver> receivers_;
	std::vector<sim::weapons::SupplyGrant> grants_;
	std::vector<uint8_t> receiver_slots_;
	mutable GadgetStats stats_;
	sim::Tick tick_ = 0;
};

inline constexpr sim::Tick kDeployableScopeTicks = 30;

struct DeployableSyncStats {
	uint64_t messages = 0, added = 0, removed = 0, out_of_scope = 0;
};

class DeployableSync {
public:
	void configure(size_t p_slots, bool p_scoped);
	void reset(size_t p_slot);
	std::optional<std::vector<uint8_t>> update(size_t p_slot, uint8_t p_team, const sim::Vec3 &p_position,
			std::span<const sim::weapons::Deployable> p_live, uint64_t p_revision, sim::Tick p_tick);
	const DeployableSyncStats &stats() const { return stats_; }

private:
	struct Viewer {
		std::vector<uint16_t> known;
		uint64_t revision = UINT64_MAX;
		sim::Tick checked = 0;
	};
	std::vector<Viewer> viewers_;
	bool scoped_ = true;
	DeployableSyncStats stats_;
};

}
