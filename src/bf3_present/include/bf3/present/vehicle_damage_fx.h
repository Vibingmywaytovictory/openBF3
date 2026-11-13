#pragma once

#include "bf3/core/gameplay/vehicle_damage_visuals.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace bf3::present {

struct VehicleFxSight {
	uint16_t spawner = 0;
	uint8_t generation = 0;
	uint16_t kind = 0;
	float health = 1.0f;
	bool hidden = false;
	bool explosion = false;
};

enum class VehicleFxOp : uint8_t { StartDamage, StopDamage, StartWreck, StartWreckEffect, EndWreck };

struct VehicleFxCommand {
	VehicleFxOp op = VehicleFxOp::StartDamage;
	uint16_t spawner = 0;
	uint8_t generation = 0;
	uint16_t kind = 0;
	uint16_t effect = 0;
	bool operator==(const VehicleFxCommand &) const = default;
};

class VehicleDamageFx {
public:
	explicit VehicleDamageFx(std::vector<core::gameplay::VehicleDamageVisualsDef> p_kinds);

	void step(std::span<const VehicleFxSight> p_seen, double p_now_s, uint64_t p_seed,
			std::vector<VehicleFxCommand> &r_out);
	size_t running() const;
	size_t wrecks() const { return wrecks_.size(); }

private:
	struct Shown {
		uint8_t generation = 0;
		uint16_t kind = 0;
		std::vector<bool> on;
	};
	struct Wreck {
		uint16_t spawner = 0;
		uint8_t generation = 0;
		uint16_t kind = 0;
		double until = 0;
		bool lasting = false;
	};

	void stop_all(uint16_t p_spawner, Shown &r_shown, std::vector<VehicleFxCommand> &r_out);

	std::vector<core::gameplay::VehicleDamageVisualsDef> kinds_;
	std::map<uint16_t, Shown> shown_;
	std::vector<Wreck> wrecks_;
};

}
