#pragma once

#include "bf3/ai/behaviour/vehicle_gadgets.h"
#include "bf3/ai/behaviour/weapon_choice.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/vehicles/vehicle_targets.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_weapons.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace bf3::host {
class Gunplay;
}

namespace bf3::host::tdm_bot {

using ai::ExplosivePhase;

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
const ai::WeaponTemplate &slot_template(const TdmShared &p_shared, const BotArms &p_arms, uint8_t p_slot);
float weapon_range(const TdmShared &p_shared, const BotArms &p_arms, uint8_t p_slot);
std::array<ai::WeaponChoice, sim::weapons::kMaxWeaponSlots> template_choices(
		const TdmShared &p_shared, const BotArms &p_arms, ai::CombatStrengthType p_type);
float hull_surface(const sim::vehicles::VehicleTarget &p_vehicle, const sim::Vec3 &p_from, sim::Vec3 &r_near);
std::array<ai::WeaponChoice, sim::weapons::kMaxWeaponSlots> firing_choices(
		const TdmShared &p_shared, const BotArms &p_arms, ai::CombatStrengthType p_type, bool p_vehicle);
std::optional<uint8_t> defib_slot(const TdmShared &p_shared, const BotArms &p_arms);

struct SoldierWeapon {
	uint8_t slot = kSlotPrimary;
	Trigger trigger = Trigger::Aimed;
};
std::optional<SoldierWeapon> soldier_weapon(const TdmShared &p_shared, const BotArms &p_arms, float p_distance);
sim::Vec3 beside_hull_point(const VehicleAim &p_aim, const sim::Vec3 &p_eye, const sim::Vec3 &p_feet, float p_gap);

class BotVehicleWeapons {
public:
	void new_life();

	bool sense(TdmShared &r_shared, const BotArms &p_arms, const VehicleCheck &p_check);
	bool sense_repair(TdmShared &r_shared, const BotArms &p_arms, const VehicleCheck &p_check);

	const VehicleAim &enemy() const { return enemy_; }
	const VehicleAim &repair() const { return repair_; }
	ExplosivePhase phase() const { return phase_; }

	bool track(TdmShared &r_shared, const sim::Vec3 &p_eye, bool p_repair);
	void explosive_step(TdmShared &r_shared, const BotArms &p_arms, uint8_t p_slot, sim::Tick p_tick);
	bool detonate_due(sim::Tick p_tick) const { return phase_ == ExplosivePhase::Detonate && p_tick >= detonate_at_; }
	sim::Vec3 back_off_goal(const sim::Vec3 &p_from, float p_turn) const;

private:
	VehicleAim enemy_;
	VehicleAim repair_;
	sim::Tick seen_tick_ = 0;
	ExplosivePhase phase_ = ExplosivePhase::Approach;
	int32_t rounds_before_ = 0;
	sim::Tick detonate_at_ = 0;
};

}
