#pragma once

#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side::vm_script {

inline constexpr size_t kKitItems = 4;

struct Mark {
	double time_s = 0;
	uint8_t slot = 0;
	std::string label;
};

struct KitRun {
	size_t kit = 0;
	sim::Tick spawn = 0;
	std::array<uint16_t, kKitItems> defs{};
	std::vector<sim::weapons::SoldierWeaponsState> states;
	std::vector<uint8_t> sprinting, zoom_held;
	std::vector<float> speed, yaw_rate, pitch_rate;
	std::vector<double> yaw, pitch;
	std::vector<sim::weapons::TimedWeaponEvent> timed;
	std::vector<sim::weapons::ShotEvent> shots;
	std::vector<Mark> marks;
	std::string error;

	double start_s() const;
	double end_s() const;
	const Mark *mark(uint8_t p_slot, const std::string &p_label) const;
};

KitRun run_kit(const sim::weapons::WeaponTable &p_table, size_t p_kit, const std::array<uint16_t, kKitItems> &p_defs);

struct View {
	bf3::host::PresentedWeapon weapon;
	bool sprinting = false;
	bool zoom_held = false;
	float speed = 0, yaw_rate = 0, pitch_rate = 0;
	double yaw = 0, pitch = 0;
};
View view_at(const KitRun &p_run, const sim::weapons::WeaponTable &p_table, double p_t);

void events_between(const KitRun &p_run, double p_from, double p_to, std::vector<bf3::host::PresentEvent> &r_events);

}

namespace bf3::godot_side::scripted {

bf3::host::PresentedWeapon present_weapon(const sim::weapons::SoldierWeaponsState &p_prev, const sim::weapons::SoldierWeaponsState &p_curr, double p_fraction,
		double p_time_s, const sim::weapons::WeaponTable &p_table);

}
