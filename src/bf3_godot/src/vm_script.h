#pragma once
// The vm demo's scripted weapon timeline (docs/design/m2a-vm.md; m2a.md §11.2 "vm's --vm-check scene drives the
// same rows on a scripted weapon timeline before the game wiring exists"). Godot-free.
//
// One default loadout (render::read_viewmodel_kits(): primary, sidearm, M67, knife in sim slots 0..3) runs through
// the real weapon simulation (bf3_sim step_weapons_m2a on a WeaponTable of the install's kit variants) at 60 Hz with
// a scripted input: zoom held through the spawn deploy (refused), a look-and-move burst then stillness (springs),
// iron sights with aimed shots, hip fire, a tactical reload with the trigger pressed in the tick the reload ends in,
// zoom held while sprinting (refused), the magazine emptied and an empty reload, a switch to the sidearm (deploy,
// ADS, shots, reload), the M67 (hold, release: thrown), the knife (one stab), stillness.
//
// What wave 3's cl provides is faked here, test-locally, in namespace m2a_fake: present_weapon (the state lerp
// between the two predicted ticks around the display time; host::present_weapon is declared, its body is cl's) and
// the OwnShot / OwnTimed PresentEvents (built from the run's ShotEvents and TimedWeaponEvents with the times cl
// stamps them with).

#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side::vm_script {

inline constexpr size_t kKitItems = 4; // sim slots 0..3 = kit slots 0, 1, 6, 7

struct Mark {
	double time_s = 0;
	uint8_t slot = 0;
	std::string label; // hip, ads, reload_start, reload_empty_start, sprint, settle, throw, knife
};

struct KitRun {
	size_t kit = 0; // index into render::read_viewmodel_kits()
	sim::Tick spawn = 0;
	std::array<uint16_t, kKitItems> defs{};
	std::vector<sim::weapons::SoldierWeaponsState> states; // [i] = after tick spawn + i ([0] = the spawn state)
	std::vector<uint8_t> sprinting, zoom_held; // per state (the tick's context and input)
	std::vector<float> speed, yaw_rate, pitch_rate; // per state
	std::vector<double> yaw, pitch; // the view after each state (integrated rates), radians
	std::vector<sim::weapons::TimedWeaponEvent> timed;
	std::vector<sim::weapons::ShotEvent> shots;
	std::vector<Mark> marks;
	std::string error;

	double start_s() const; // tick_end(spawn)
	double end_s() const;
	const Mark *mark(uint8_t p_slot, const std::string &p_label) const;
};

// Runs one kit's script (p_defs: the kit's four def ids in p_table). The match seed and spawn tick are fixed.
KitRun run_kit(const sim::weapons::WeaponTable &p_table, size_t p_kit, const std::array<uint16_t, kKitItems> &p_defs);

// The presented state at display time p_t (seconds on the sim clock; clamped to the run).
struct View {
	bf3::host::PresentedWeapon weapon;
	bool sprinting = false;
	bool zoom_held = false; // the scripted input (PresentedWeapon::zoomed is the sim's flag)
	float speed = 0, yaw_rate = 0, pitch_rate = 0;
	double yaw = 0, pitch = 0;
};
View view_at(const KitRun &p_run, const sim::weapons::WeaponTable &p_table, double p_t);
// The run's own events with time in (p_from, p_to] as PresentEvents (OwnTimed then OwnShot, by time).
void events_between(const KitRun &p_run, double p_from, double p_to, std::vector<bf3::host::PresentEvent> &r_events);

} // namespace bf3::godot_side::vm_script

namespace bf3::godot_side::m2a_fake {

// cl's present_weapon (wave 3) stand-in: continuous fields lerp at p_fraction, discrete ones from p_curr.
bf3::host::PresentedWeapon present_weapon(const sim::weapons::SoldierWeaponsState &p_prev, const sim::weapons::SoldierWeaponsState &p_curr, double p_fraction,
		double p_time_s, const sim::weapons::WeaponTable &p_table);

} // namespace bf3::godot_side::m2a_fake
