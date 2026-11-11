#pragma once

#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_weapons.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace bf3::host::tdm_bot {

enum class SupplyNeed : uint8_t { Health = 0, Ammo = 1 };
inline constexpr uint8_t kNoSupplySlot = 0xFF;

struct SupportSense {
	bool heal = false;
	bool supply = false;
	bool call = false;
};

struct SupportWay {
	std::optional<sim::Vec3> point;
	float free_within_m = 0.0f;
	bool following = false;
	uint64_t squad_mates = 0;
};

struct SupportCheck {
	sim::Tick tick = 0;
	uint8_t slot = 0xFF;
	const sim::SoldierState *self = nullptr;
	std::span<const sim::SoldierState> states;
	bool engaged = false;
	SupportWay way;
};

class BotSupport {
public:
	void new_life();
	SupportSense update(TdmShared &r_shared, const BotArms &p_arms, const SupportCheck &p_check);

	const sim::Vec3 *target(SupplyNeed p_need) const;
	uint8_t gadget_slot(SupplyNeed p_need) const { return slots_[size_t(p_need)]; }
	const std::optional<sim::Vec3> &medkit() const { return medkit_; }

	void pursue(SupplyNeed p_need, sim::Tick p_tick) { pursued_[size_t(p_need)] = p_tick; }
	void thrown(TdmShared &r_shared, sim::Tick p_tick, uint8_t p_weapon_slot);
	void post_call(TdmShared &r_shared, sim::Tick p_tick, uint8_t p_slot);

private:
	struct Target {
		int slot = -1;
		sim::Vec3 at{};
		bool called = false;
	};

	Target choose(const TdmShared &p_shared, SupplyNeed p_need, const SupportCheck &p_check, int p_keep) const;
	bool own_bag_busy(const TdmShared &p_shared, SupplyNeed p_need, const SupportCheck &p_check) const;
	bool update_call(TdmShared &r_shared, const SupportCheck &p_check);

	std::array<uint8_t, 2> slots_{ kNoSupplySlot, kNoSupplySlot };
	std::array<Target, 2> targets_{};
	std::array<sim::Tick, 2> since_{}, pursued_{};
	sim::Tick updated_ = 0, rethrow_until_ = 0;
	bool calling_ = false, posted_ = false;
	sim::Tick call_since_ = 0, call_blocked_until_ = 0;
	std::optional<sim::Vec3> medkit_;
};

std::optional<uint8_t> ask_medic(TdmShared &r_shared, uint8_t p_slot);
bool may_help(const ai::BehaviourTuning &p_tuning, const SupportWay &p_way, const sim::Vec3 &p_self,
		const sim::Vec3 &p_mate, size_t p_mate_slot);
uint8_t supply_slot(const Gunplay &p_gunplay, const BotArms &p_arms, SupplyNeed p_need);
bool needs_supply(const Gunplay &p_gunplay, const ai::BehaviourTuning &p_tuning, uint8_t p_slot,
		const sim::SoldierState &p_state, SupplyNeed p_need);
bool supplied(const Gunplay &p_gunplay, uint8_t p_slot, uint8_t p_team, const sim::Vec3 &p_at, SupplyNeed p_need,
		int p_owner = -1);

}
