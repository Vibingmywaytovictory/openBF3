#pragma once

#include "bf3/fx/fx_types.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/level_load.h"
#include "bf3/host/presentation_events.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

namespace bf3::core {
class Cache;
}

namespace bf3::godot_side {

struct M2aLoadInputs {
	const host::LevelDatabase *db = nullptr;
	std::shared_ptr<const host::SharedLevel> shared;
	core::Cache *cache = nullptr;
	unsigned threads = 0;
	const std::atomic<bool> *stop = nullptr;
	host::LoadProgress *progress = nullptr;
	uint8_t own_team = 1;
};

struct M2aFrame {
	const host::PresentationFrame *frame = nullptr;
	host::PresentClock clock;
	uint64_t now_ns = 0;
	double dt = 0.0;
	std::span<const host::PresentEvent> events;
	std::span<const host::PresentedSoldier> others;
	std::span<const host::PresentedOtherWeapon> other_weapons;
	std::span<const fx::MuzzleSample> muzzles;

	fx::Transform camera;
	fx::Vec3 camera_velocity;
	bool free_camera = false;
	bool own_alive = false;
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0;
};

}
