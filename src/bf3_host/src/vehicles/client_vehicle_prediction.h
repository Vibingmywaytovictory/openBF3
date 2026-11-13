#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/sim/command.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/vehicle_world.h"
#include "client_vehicles.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace bf3::host {

inline constexpr uint32_t kVehiclePredictionRing = 64;
inline constexpr float kVehicleBlendMinM = 0.03f;
inline constexpr float kVehicleBlendMaxM = 3.0f;
inline constexpr float kVehicleBlendLongestS = 0.25f;

struct VehicleBlend {
	sim::Vec3 offset{};
	float remaining = 0;
	float total = 1;

	sim::Vec3 shown() const;
	void correct(const sim::Vec3 &p_error);
	void decay(float p_seconds);
};

using CommandAt = std::function<const sim::Command *(sim::Tick)>;

class ClientVehiclePrediction final : public sim::SeatSource {
public:
	core::Status init(const SharedLevel &p_level);
	void reconcile(const ClientVehicles &p_seen, uint8_t p_own_slot, const CommandAt &p_commands, sim::Tick p_next);
	void step(const sim::Command &p_command);
	std::optional<sim::Vec3> seat_at(sim::Tick p_tick) const override;
	void publish(PresentationFrame &r_frame) const;
	void stats(ClientStats &r_stats) const;

	bool driving() const { return driving_; }

private:
	struct Entry {
		bool used = false;
		sim::Tick tick = 0;
		sim::VehicleWorldSnapshot snapshot;
		sim::VehicleState state;
	};

	const Entry *entry(sim::Tick p_tick) const;
	void keep(sim::Tick p_tick);
	void stop_driving();
	bool start(const net::VehicleWire &p_wire, sim::Tick p_tick);
	void correct(const net::VehicleWire &p_wire, sim::Tick p_tick);
	void replay(sim::Tick p_from, sim::Tick p_next, const CommandAt &p_commands);
	bool advance(const sim::VehicleCommand &p_command);
	std::optional<sim::Vec3> seat_of(const sim::VehicleState &p_state) const;

	std::shared_ptr<const LevelVehicles> level_;
	std::unique_ptr<sim::VehicleWorld> world_;
	std::vector<Entry> ring_;

	bool seated_ = false;
	sim::Tick seated_from_ = 0;
	uint16_t spawner_ = 0;
	uint8_t seat_ = 0;
	uint8_t generation_ = 0;
	sim::VehicleState seen_state_;
	bool driving_ = false;
	sim::Tick newest_ = 0;
	sim::Tick reconciled_ = 0;
	VehicleBlend blend_;

	uint64_t predicted_ticks_ = 0;
	uint64_t replayed_ticks_ = 0;
	uint64_t matches_ = 0;
	uint64_t corrections_ = 0;
	uint64_t starts_ = 0;
	uint64_t failures_ = 0;
	float max_correction_m_ = 0.0f;
};

}
