#pragma once

#include "bf3/ai/behaviour/weapon_choice.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/vehicles/vehicle_targets.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_weapons.h"
#include "vehicles/vehicle_hits.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace bf3::host {
class Gunplay;
}

namespace bf3::host::tdm_bot {

inline constexpr float kBackOffMetres = 9.0f;
inline constexpr float kBackOffGoalMetres = 30.0f;
inline constexpr float kDetonateDelayS = 0.7f;

enum class ExplosivePhase : uint8_t { Approach, Place, Wait, BackOff, Detonate };

struct VehicleCheck {
	sim::Tick tick = 0;
	uint8_t slot = 0xFF;
	const sim::SoldierState *self = nullptr;
	sim::Vec3 eye{};
	float view_yaw = 0.0f;
};

struct VehicleAim {
	uint32_t id = 0;
	sim::Vec3 centre{};
	sim::Vec3 velocity{};
	sim::Vec3 near_point{};
	float surface = 0.0f;
	uint8_t weapon_slot = 0xFF;
	ai::WeaponType type = ai::WeaponType::Default;
	sim::Tick since = 0;
	sim::Tick seen = 0;
	bool visible = false;

	bool held() const { return id != 0; }
};

ai::WeaponType weapon_type(const Gunplay &p_gunplay, uint16_t p_def);
float weapon_range(const Gunplay &p_gunplay, uint16_t p_def);
std::array<ai::WeaponChoice, sim::weapons::kMaxWeaponSlots> vehicle_weapon_choices(
		const Gunplay &p_gunplay, const VehicleDamageSource &p_source, const BotArms &p_arms, uint8_t p_property);
float hull_surface(const sim::vehicles::VehicleTarget &p_vehicle, const sim::Vec3 &p_from, sim::Vec3 &r_near);
std::optional<uint8_t> defib_slot(const Gunplay &p_gunplay, const BotArms &p_arms);
sim::Vec3 beside_hull_point(const VehicleAim &p_aim, const sim::Vec3 &p_eye, const sim::Vec3 &p_feet, float p_gap);

class BotVehicleWeapons {
public:
	void new_life();

	bool sense(TdmShared &r_shared, const BotArms &p_arms, const VehicleCheck &p_check);

	const VehicleAim &enemy() const { return enemy_; }
	ExplosivePhase phase() const { return phase_; }

	bool track(TdmShared &r_shared, const sim::Vec3 &p_eye);
	void explosive_step(TdmShared &r_shared, const BotArms &p_arms, uint8_t p_slot, sim::Tick p_tick);
	bool detonate_due(sim::Tick p_tick) const { return phase_ == ExplosivePhase::Detonate && p_tick >= detonate_at_; }
	sim::Vec3 back_off_goal(const sim::Vec3 &p_from, float p_turn) const;

private:
	const VehicleDamageSource &source(const Gunplay &p_gunplay);

	VehicleDamageSource source_;
	bool configured_ = false;
	VehicleAim enemy_;
	sim::Tick seen_tick_ = 0;
	ExplosivePhase phase_ = ExplosivePhase::Approach;
	int32_t rounds_before_ = 0;
	sim::Tick detonate_at_ = 0;
};

}
