#pragma once

#include "bf3/ai/vehicle/vehicle_change.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
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
		const sim::SoldierState &p_state, sim::Tick p_spawned, bool p_stuck);

class BotCrew {
public:
	void new_life(TdmShared &r_shared, uint8_t p_slot, sim::Tick p_tick);
	void drop(TdmShared &r_shared, uint8_t p_slot) { book(r_shared, p_slot, std::nullopt); }
	void decide(TdmShared &r_shared, uint8_t p_slot, const sim::SoldierState &p_state, bool p_stuck,
			float p_chosen_urgency);
	bool command(tdm_bot::BotBody &r_body, const tdm_bot::BotFrame &p_frame, sim::Command &r_cmd);
	const std::optional<ai::ChangeTarget> &target() const { return target_; }

private:
	void book(TdmShared &r_shared, uint8_t p_slot, std::optional<ai::ChangeTarget> p_target);
	bool press_use();

	std::optional<ai::ChangeTarget> target_;
	sim::Tick spawned_ = 0;
	bool use_down_ = false;
};

}
