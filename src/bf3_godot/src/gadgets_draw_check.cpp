#include "gadgets_draw_check.h"

#include "game_deployables.h"
#include "level_session.h"
#include "viewmodel.h"

#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"
#include "bf3/host/kits.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <optional>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;
namespace sw = bf3::sim::weapons;

namespace {

constexpr double kScreenTimeout = 60.0;
constexpr double kDeployTimeout = 30.0;
constexpr double kFirstDeployPress = 1.0;
constexpr double kDeployPressEvery = 2.0;
constexpr double kSelectEvery = 1.0;
constexpr uint32_t kSelectPresses = 4;
constexpr double kSelectTimeout = 8.0;
constexpr double kLightSettle = 0.25;
constexpr double kHeldTimeout = 10.0;
constexpr double kFireHold = 0.3;
constexpr double kThrownTimeout = 8.0;
constexpr double kDeadTimeout = 15.0;
constexpr float kPositionTolerance = 1e-3f;

std::string short_name(const std::string &p_path) {
	return p_path.substr(p_path.rfind('/') + 1);
}

}

void GadgetsDrawRun::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0 : 1;
	UtilityFunctions::print(
			String::utf8(("GADGETS DRAW ROW " + p_name + " " + (p_pass ? "PASS" : "FAIL") + " " + p_detail).c_str()));
}

void GadgetsDrawRun::enter(Phase p_phase) {
	phase_ = p_phase;
	t_ = 0.0;
	next_press_ = 0.0;
	presses_ = 0;
}

bool GadgetsDrawRun::fail(const std::string &p_name, const std::string &p_detail) {
	row(p_name, false, p_detail);
	phase_ = Phase::Done;
	return true;
}

const sw::GadgetDef *GadgetsDrawRun::held_gadget(const GadgetsDrawInputs &p_in) const {
	if (p_in.weapon == nullptr || !p_in.weapon->valid || p_in.weapon->phase != sw::WeaponPhase::Ready) {
		return nullptr;
	}
	for (const sw::GadgetDef &g : p_in.gadgets) {
		if (g.def == p_in.weapon->def) {
			return &g;
		}
	}
	return nullptr;
}

void GadgetsDrawRun::finish_rows(const GadgetsDrawInputs &p_in) {
	const GameDeployablesStats s = p_in.deployables->stats();
	row("deployables", s.unknown_defs == 0 && s.created == thrown_ && s.shown == thrown_ && s.freed == 0,
			fmt("%u thrown, %llu nodes made, %u shown, %llu freed, %u deployables of a def with no mesh (the bags stay "
				"after their owner's death)",
					thrown_, (unsigned long long)s.created, s.shown, (unsigned long long)s.freed, s.unknown_defs));
}

bool GadgetsDrawRun::step(Bf3LevelSession &r_session, const GadgetsDrawInputs &p_in, double p_delta) {
	const host::PresentationFrame &f = r_session.frame();
	InputMapper &mapper = r_session.mapper();
	t_ += p_delta;
	if (p_in.deployables == nullptr || !p_in.deployables->built()) {
		return t_ > kScreenTimeout ? fail("built", "the deployables were not loaded or built") : false;
	}
	const uint8_t want_kit = kGadgetsDrawKits[kit_];
	switch (phase_) {
		case Phase::WaitScreen: {
			const bool in_round = f.match_valid && f.match.phase == net::MatchPhase::InRound;
			if (!in_round || !p_in.spawn_screen || p_in.alive) {
				return t_ > kScreenTimeout
						? fail("spawn_screen",
								  fmt("no spawn screen after %.0f s for kit %u", kScreenTimeout, unsigned(want_kit)))
						: false;
			}
			mapper.set_scripted(true);
			mapper.release_all();
			mapper.script_press(InputMapper::TdmAction(uint8_t(InputMapper::TdmAction::Kit1) + want_kit));
			kit_name_ = host::kit_role_name(host::KitRole(want_kit));
			enter(Phase::Deploying);
			next_press_ = kFirstDeployPress;
			return false;
		}
		case Phase::Deploying: {
			if (!p_in.alive || p_in.weapon == nullptr || !p_in.weapon->valid) {
				if (p_in.spawn_screen && t_ >= next_press_) {
					p_in.deploy();
					next_press_ = t_ + kDeployPressEvery;
				}
				return t_ > kDeployTimeout
						? fail(kit_name_ + ".deploy", fmt("not alive %.0f s after choosing the kit", kDeployTimeout))
						: false;
			}
			if (f.own_kit != want_kit) {
				return fail(kit_name_ + ".deploy",
						fmt("deployed as kit %u, chose %u", unsigned(f.own_kit), unsigned(want_kit)));
			}
			enter(kit_ == 0 ? Phase::Light : Phase::Select);
			return false;
		}
		case Phase::Light: {
			if (p_in.own_vm == nullptr) {
				return fail("weapon_light_toggle", "no own viewmodel");
			}
			if (t_ < kLightSettle) {
				return false;
			}
			const bool want = light_step_ % 2 == 0;
			if (p_in.own_vm->weapon_light() != want || mapper.weapon_light() != want) {
				return fail("weapon_light_toggle",
						fmt("after %u presses of bf3_weapon_light: the mapper's light %s, the own viewmodel's %s, want "
							"%s",
								unsigned(light_step_), mapper.weapon_light() ? "on" : "off",
								p_in.own_vm->weapon_light() ? "on" : "off", want ? "on" : "off"));
			}
			if (light_step_ == 2) {
				row("weapon_light_toggle", true,
						"on at the spawn, off after one bf3_weapon_light press, on after the second, in the mapper and "
						"the own viewmodel");
				enter(Phase::Select);
				return false;
			}
			mapper.script_press(InputMapper::TdmAction::WeaponLight);
			++light_step_;
			t_ = 0.0;
			return false;
		}
		case Phase::Select: {
			if (const sw::GadgetDef *g = held_gadget(p_in)) {
				gadget_ = short_name(g->weapon);
				enter(Phase::Held);
				return false;
			}
			if (t_ >= next_press_ && presses_ < kSelectPresses) {
				mapper.script_press(InputMapper::TdmAction::Gadget);
				++presses_;
				next_press_ = t_ + kSelectEvery;
			}
			return t_ > kSelectTimeout
					? fail(kit_name_ + ".gadget",
							  fmt("no deployable gadget held after %u presses of bf3_gadget", presses_))
					: false;
		}
		case Phase::Held: {
			const bf3::render::ViewmodelItem *item = p_in.own_vm != nullptr ? p_in.own_vm->current_item() : nullptr;
			const bool drawn = item != nullptr && item->kind == bf3::render::ViewmodelItemKind::Gadget &&
					p_in.own_vm->weapon_drawn();
			if (!drawn && t_ <= kHeldTimeout) {
				return false;
			}
			if (next_press_ == 0.0) {
				row(kit_name_ + "." + gadget_ + ".viewmodel", drawn,
						drawn ? "the own viewmodel draws " + short_name(item->unlock) + " as a gadget item"
							  : fmt("the own viewmodel shows %s %.0f s after the gadget is held",
										item != nullptr ? short_name(item->unlock).c_str() : "no item", kHeldTimeout));
				mapper.script_hold(InputMapper::WeaponAction::Fire, true);
				next_press_ = t_ + kFireHold;
				return false;
			}
			if (t_ >= next_press_) {
				mapper.script_hold(InputMapper::WeaponAction::Fire, false);
				enter(Phase::Thrown);
			}
			return false;
		}
		case Phase::Thrown: {
			const net::DeployableWire *mine = nullptr;
			for (size_t i = 0; i < f.deployable_count && i < f.deployables.size(); ++i) {
				const net::DeployableWire &d = f.deployables[i];
				if (d.owner == p_in.own_slot && seen_.count(d.id) == 0) {
					mine = &d;
				}
			}
			const std::optional<GameDeployableView> v =
					mine != nullptr ? p_in.deployables->view(mine->id) : std::nullopt;
			if (!v) {
				return t_ > kThrownTimeout ? fail(kit_name_ + "." + gadget_ + ".drawn",
													 fmt("%s %.0f s after the throw",
															 mine == nullptr ? "no new own deployable in the frame"
																			 : "no node for the new deployable",
															 kThrownTimeout))
										   : false;
			}
			seen_.insert(mine->id);
			++thrown_;
			const Vector3 want(mine->position[0], mine->position[1], mine->position[2]);
			const float off = v->position.distance_to(want);
			row(kit_name_ + "." + gadget_ + ".drawn",
					v->def == mine->def && v->in_tree && v->visible && v->def_mesh && v->surfaces > 0 &&
							off <= kPositionTolerance,
					fmt("deployable %u (def %u): node in the tree %s, visible %s, the def's mesh %s with %d surfaces, "
						"%.4f m from the sent position",
							unsigned(mine->id), unsigned(mine->def), v->in_tree ? "yes" : "no",
							v->visible ? "yes" : "no", v->def_mesh ? "yes" : "no", v->surfaces, double(off)));
			if (kit_ + 1 >= kGadgetsDrawKits.size()) {
				finish_rows(p_in);
				phase_ = Phase::Done;
				return true;
			}
			p_in.suicide();
			enter(Phase::Dead);
			return false;
		}
		case Phase::Dead: {
			if (p_in.alive) {
				return t_ > kDeadTimeout ? fail(kit_name_ + ".suicide",
												   fmt("still alive %.0f s after the suicide request", kDeadTimeout))
										 : false;
			}
			++kit_;
			enter(Phase::WaitScreen);
			return false;
		}
		case Phase::Done:
			return true;
	}
	return true;
}

}
