#pragma once

#include "bf3/sim/weapons/gadgets.h"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <string>

namespace bf3::host {
struct PresentedWeapon;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3Viewmodel;
class GameDeployables;

struct GadgetsDrawInputs {
	bool alive = false;
	bool spawn_screen = false;
	uint8_t own_slot = 0xFF;
	const bf3::host::PresentedWeapon *weapon = nullptr;
	const GameDeployables *deployables = nullptr;
	const Bf3Viewmodel *own_vm = nullptr;
	std::span<const sim::weapons::GadgetDef> gadgets;
	std::function<void()> deploy;
	std::function<void()> suicide;
};

inline constexpr std::array<uint8_t, 3> kGadgetsDrawKits{ 0, 2, 3 };

class GadgetsDrawRun {
public:
	bool step(Bf3LevelSession &r_session, const GadgetsDrawInputs &p_in, double p_delta);
	int exit_code() const { return failures_ == 0 && rows_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitScreen, Deploying, Light, Select, Held, Thrown, Dead, Done };

	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void enter(Phase p_phase);
	bool fail(const std::string &p_name, const std::string &p_detail);
	const sim::weapons::GadgetDef *held_gadget(const GadgetsDrawInputs &p_in) const;
	void finish_rows(const GadgetsDrawInputs &p_in);

	Phase phase_ = Phase::WaitScreen;
	double t_ = 0.0;
	double next_press_ = 0.0;
	size_t kit_ = 0;
	uint32_t presses_ = 0;
	uint8_t light_step_ = 0;
	std::string kit_name_;
	std::string gadget_;
	std::set<uint16_t> seen_;
	uint32_t thrown_ = 0;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
};

}
