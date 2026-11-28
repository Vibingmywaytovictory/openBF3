#pragma once

#include "bf3/ai/actionplan/aimer.h"
#include "bf3/ai/bot/infantry.h"
#include "bf3/ai/vehicle/seat_fire.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

struct TdmShared;

namespace tdm_bot {
struct BotBody;
}

std::optional<sim::Vec3> seat_view(const TdmShared &p_shared, uint8_t p_slot);

class BotGunner {
public:
	void new_life();
	ai::SeatFireSense evaluate(const TdmShared &p_shared, uint8_t p_slot, const sim::bots::Perception &p_perception);
	const ai::SeatFireSense &sense() const { return sense_; }
	bool command(tdm_bot::BotBody &r_body, sim::Tick p_tick, bool p_fire, sim::Command &r_cmd);

private:
	ai::SeatFireSense sense_;
	int target_slot_ = -1;
	int weapon_ = -1;
	bool target_vehicle_air_ = false;
	int fired_target_ = -1;
	sim::Tick target_since_ = 0;
	sim::Tick fire_at_ = 0;
	sim::Tick last_command_ = 0;
	ai::Aimer aimer_;
	int aimer_slot_ = -1;
	std::vector<ai::SeatFireWeapon> weapons_;
	std::vector<size_t> weapon_index_;
	std::vector<ai::SeatFireTarget> targets_;
	std::vector<uint8_t> target_slots_;
};

}
