#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::sim::weapons {

enum class GadgetKind : uint8_t { None = 0, Supply = 1, SpawnBeacon = 2 };
const char *gadget_kind_name(GadgetKind p_kind);

struct SupplyUnitDef {
	float radius = 0;
	float inc_speed = 0;
	float capacity = 0;
	float refill_speed = 0;
	bool infinite = false;

	bool active() const { return radius > 0.0f && inc_speed > 0.0f; }
	bool operator==(const SupplyUnitDef &) const = default;
};

struct AmmoPickupDef {
	int32_t amount = -1;
	float delay_multiplier = 1;
	bool auto_replenish = false;
	float auto_replenish_delay = 0;

	bool operator==(const AmmoPickupDef &) const = default;
};

struct GadgetDef {
	std::string weapon;
	std::string projectile_type;
	GadgetKind kind = GadgetKind::None;
	uint16_t def = kNoWeapon;
	SupplyUnitDef healing;
	SupplyUnitDef ammo;
	bool team_specific = false;
	bool exclude_self = false;
	bool supply_soldiers = false;
	uint8_t max_count = 1;
	float max_attach_inclination_deg = 90;
	float ground_ray_m = 0;
	float ground_flatness = 0;

	bool operator==(const GadgetDef &) const = default;
};

core::Result<AmmoPickupDef> read_ammo_pickup(const core::EbxResolver &p_db, std::string_view p_weapon);
core::Result<GadgetDef> read_gadget_def(const core::EbxResolver &p_db, std::string_view p_weapon);

inline constexpr float kGadgetGravity = 9.81f;
inline constexpr Tick kGadgetFlightTicks = 10u * kSimHz;
inline constexpr size_t kMaxDeployables = 512;
inline constexpr float kGadgetSlideLift = 0.01f;

struct Deployable {
	uint16_t id = 0;
	uint16_t def = kNoWeapon;
	uint8_t gadget = 0;
	uint8_t owner = 0;
	uint8_t team = 0;
	bool resting = false;
	uint16_t flight = 0;
	Tick deployed = 0;
	Vec3 position{};
	Vec3 velocity{};
	float healing_points = 0;
	float ammo_points = 0;
};

enum class DeployOutcome : uint8_t { Thrown = 0, Placed = 1, NoGround = 2, Steep = 3, NotGadget = 4, Full = 5 };

struct GadgetThrow {
	uint16_t def = kNoWeapon;
	uint8_t owner = 0;
	uint8_t team = 0;
	Tick tick = 0;
	Vec3 origin{};
	Vec3 direction{};
	Vec3 launch_velocity{};
};

struct SupplyReceiver {
	uint8_t slot = 0;
	uint8_t team = 0;
	bool alive = false;
	Vec3 position{};
};

struct SupplyGrant {
	float health = 0;
	float ammo = 0;
};

struct GadgetFieldStats {
	uint64_t thrown = 0, placed = 0, landed = 0, no_ground = 0, steep = 0, full = 0, replaced = 0, lost = 0,
			 removed = 0;
	uint64_t slides = 0;
};

Vec3 shot_frame_vector(const Vec3 &p_direction, const Vec3 &p_local);

class GadgetField {
public:
	GadgetField() = default;
	explicit GadgetField(std::vector<GadgetDef> p_defs);

	const GadgetDef *gadget_of(uint16_t p_def) const;
	std::span<const GadgetDef> defs() const { return defs_; }

	DeployOutcome deploy(const GadgetThrow &p_throw, const CollisionWorld *p_world, uint16_t *r_id = nullptr);
	void step(const CollisionWorld &p_world);
	void supply(std::span<const SupplyReceiver> p_receivers, float p_seconds, std::span<SupplyGrant> r_grants);
	size_t remove_owner(uint8_t p_owner);
	bool remove(uint16_t p_id);
	void clear();

	std::span<const Deployable> live() const { return live_; }
	const Deployable *find(uint16_t p_id) const;
	const GadgetFieldStats &stats() const { return stats_; }
	uint64_t revision() const { return revision_; }

private:
	uint16_t next_id();
	void erase_at(size_t p_index);

	std::vector<GadgetDef> defs_;
	std::vector<Deployable> live_;
	uint16_t next_id_ = 1;
	uint64_t revision_ = 0;
	GadgetFieldStats stats_;
};

struct AmmoRefillState {
	std::array<float, kMaxWeaponSlots> progress{};
	std::array<Tick, kMaxWeaponSlots> low_since{};
	std::array<int32_t, kMaxWeaponSlots> caps{};
};

void arm_ammo_refill(AmmoRefillState &r_state, const SoldierWeaponsState &p_weapons);

int32_t refill_ammo(SoldierWeaponsState &r_weapons, const WeaponTable &p_table,
		std::span<const AmmoPickupDef> p_pickups, float p_points, AmmoRefillState &r_state);

int32_t replenish_ammo(SoldierWeaponsState &r_weapons, const WeaponTable &p_table,
		std::span<const AmmoPickupDef> p_pickups, Tick p_tick, AmmoRefillState &r_state);

}
