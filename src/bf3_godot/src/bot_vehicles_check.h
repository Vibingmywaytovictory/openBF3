#pragma once

#include "game_vehicles.h"

#include "bf3/host/client_session.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3LevelSession;

struct BotVehiclesInputs {
	std::function<bool(const std::string &)> shot;
	std::function<bool(uint8_t, sim::Vec3 &)> drawn_seated;
	std::span<const SeatedSoldierRoot> seat_roots;
};

class BotVehiclesCheck {
public:
	explicit BotVehiclesCheck(std::string p_shots);
	bool step(Bf3LevelSession &r_session, const BotVehiclesInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitSeated, Frame, Sides, Drive, Crew, CrewSides, Done };

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void aim(Bf3LevelSession &r_session) const;
	bool follow(const host::PresentationFrame &p_frame);
	void see_drawn(const host::PresentationFrame &p_frame, const BotVehiclesInputs &p_in, int p_only = -1);
	bool find_crew(const host::PresentationFrame &p_frame, const BotVehiclesInputs &p_in);

	std::string shots_;
	Phase phase_ = Phase::WaitSeated;
	double t_ = 0.0;
	double framed_ = 0.0;
	uint16_t spawner_ = 0;
	sim::Vec3 at_{};
	sim::Vec3 boarded_{};
	double driving_ = 0.0;
	float heading_ = 0.0f;
	float side_ = 0.0f;
	uint32_t seated_ = 0;
	uint32_t vehicles_ = 0;
	std::vector<SeatedSoldierRoot> last_roots_;
	uint32_t drawn_ = 0;
	uint32_t on_entry_ = 0;
	float drawn_off_ = -1.0f;
	float worst_off_ = 0.0f;
	int crew_slot_ = -1;
	size_t crew_seat_ = 0;
	double crew_wait_ = 0.0;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}
