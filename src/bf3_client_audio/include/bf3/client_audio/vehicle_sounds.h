#pragma once

#include "bf3/audio/patch_instance.h"
#include "bf3/audio/vehicle_audio.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/vehicle_sound.h"
#include "bf3/core/result.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/presented_vehicle.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf3::audio {

struct VehicleAudioAssets {
	std::vector<core::gameplay::VehicleSoundDef> defs;
	std::vector<std::shared_ptr<const core::audio::SoundProgram>> programs;
	std::vector<uint16_t> engine_program;
	std::vector<float> disabled_fraction;
	std::vector<std::vector<std::pair<int8_t, uint16_t>>> turret_programs;
};

core::Result<VehicleAudioAssets> load_vehicle_audio(
		const core::EbxResolver &p_db, const host::LevelVehicles &p_level, std::vector<std::string> &r_report);

struct VehicleTurretMotion {
	uint32_t key = 0;
	sim::Tick tick = 0;
	float angle = 0.0f;
	float speed = 0.0f;
	bool seen = false;
};

struct VehicleSoundMemory {
	std::vector<VehicleTurretMotion> turrets;
};

inline constexpr uint32_t kVehicleEngineSound = 0;

constexpr uint32_t vehicle_sound_key(uint16_t p_spawner, uint8_t p_generation, uint32_t p_sound) {
	return (uint32_t(p_spawner) << 16) | (uint32_t(p_generation) << 8) | (p_sound & 0xFFu);
}

struct VehicleListenerView {
	ListenerState listener;
	uint8_t own_slot = 0xFF;
};

void vehicle_sound_sources(std::span<const host::PresentedVehicle> p_vehicles, const host::LevelVehicles &p_level,
		const VehicleAudioAssets &p_assets, const VehicleListenerView &p_view, VehicleSoundMemory &r_memory,
		std::vector<VehicleSoundSource> &r_out);

}
