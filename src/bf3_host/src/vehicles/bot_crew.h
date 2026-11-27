#pragma once

#include "../tdm_bot_weapons_vehicles.h"
#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/vehicle/vehicle_change.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/vehicle_model.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bf3::host {

class ServerVehicles;
struct TdmShared;

namespace tdm_bot {
struct BotBody;
struct BotFrame;
}

float change_vehicle_max_speed(const sim::VehicleModel &p_model);

std::vector<ai::ChangeVehicle> change_vehicles(
		const TdmShared &p_shared, const ServerVehicles &p_vehicles, uint8_t p_slot, bool p_ground);

ai::ChangeBot change_bot(const TdmShared &p_shared, const ServerVehicles &p_vehicles, uint8_t p_slot,
		const sim::SoldierState &p_state, bool p_stuck, bool p_idle);

ai::ChangeStrengths change_strength_scales(const TdmShared &p_shared);
ai::EquipmentClass seat_equipment(const TdmShared &p_shared, uint8_t p_slot);
ai::ChangeStrengths change_soldier_strengths(const TdmShared &p_shared, uint8_t p_slot);
void ban_vehicle(TdmShared &r_shared, uint8_t p_slot);

class BotCrew {
public:
	void new_life(TdmShared &r_shared, uint8_t p_slot);
	void drop(TdmShared &r_shared, uint8_t p_slot) { book(r_shared, p_slot, std::nullopt); }
	ai::ChangeChoice evaluate(TdmShared &r_shared, uint8_t p_slot, uint32_t p_bot_id, const sim::SoldierState &p_state,
			bool p_stuck, bool p_idle, const sim::bots::Perception &p_perception,
			const std::optional<tdm_bot::SeenVehicle> &p_vehicle);
	void settle(TdmShared &r_shared, uint8_t p_slot, bool p_chosen, float p_chosen_urgency);
	bool command(tdm_bot::BotBody &r_body, const tdm_bot::BotFrame &p_frame, sim::Command &r_cmd);
	const std::optional<ai::ChangeTarget> &target() const { return target_; }

private:
	void book(TdmShared &r_shared, uint8_t p_slot, std::optional<ai::ChangeTarget> p_target);
	bool press_use();

	std::optional<ai::ChangeTarget> target_;
	std::optional<ai::ChangeChoice> evaluated_;
	bool use_down_ = false;
};

}
