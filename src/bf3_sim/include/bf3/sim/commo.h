#pragma once

#include "bf3/sim/sim_types.h"
#include "bf3/sim/spotting.h"

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace bf3::sim::commo {

enum class OrderType : uint8_t {
	None = 0,
	RequestHeal = 1,
	LetMeHeal = 2,
	RequestAmmo = 3,
	LetMeGiveAmmo = 4,
	LetMeRepair = 5,
	RequestRepair = 6,
	RequestRide = 7,
	GoGoGo = 8,
	FollowMe = 9,
	Negative = 10,
	Sorry = 11,
	ThankYou = 12,
	RogerThat = 13,
	NeedBackup = 14,
	GetIn = 15,
	GetOut = 16,
	Attack = 17,
	Count = 18,
};

bool issuer_voice(OrderType p_type);
bool server_voice(OrderType p_type);

struct Supplies {
	bool heals = false;
	bool ammo = false;
	bool repairs = false;
	bool revives = false;
};

struct OrderParty {
	Vec3 position{};
	float health = 0;
	float max_health = 0;
	bool in_vehicle = false;
	float vehicle_health = 0;
	float vehicle_max_health = 0;
	bool vehicle_has_free_seat = false;
	bool offers_pickup = false;
	bool low_ammo = false;
	Supplies supplies;
};

OrderType pick_order(const spotting::SpotterDef &p_def, const OrderParty &p_me, const OrderParty &p_mate);

struct WeaponAmmo {
	float in_weapon = 0;
	float reserve = 0;
	float capacity = 0;
	float count = 0;
	bool counted = true;
};

bool low_ammo(std::span<const WeaponAmmo> p_weapons, float p_need_percent);

inline constexpr int32_t kIconNeedBackup = 65;
inline constexpr int32_t kIconNeedAmmo = 66;
inline constexpr int32_t kIconNeedMedic = 67;
inline constexpr int32_t kIconNeedPickup = 68;
inline constexpr int32_t kIconNeedRepair = 69;
inline constexpr int32_t kIconSquadLeaderBg = 114;
inline constexpr int32_t kIconRevive = 119;
inline constexpr int32_t kIconNeedAmmoHighlight = 146;
inline constexpr int32_t kIconNeedMedicHighlight = 147;
inline constexpr int32_t kIconNeedRepairHighlight = 148;
inline constexpr int32_t kIconNeedPickupHighlight = 149;
inline constexpr int32_t kIconPlayerDead = 150;
inline constexpr int32_t kIconNone = 170;
inline constexpr float kOrderIconSeconds = 10.0f;
inline constexpr float kOrderBlinkSeconds = 2.0f;

int32_t highlight_icon(int32_t p_icon);
bool blink_high(double p_clock);

struct OrderIcon {
	uint64_t seen = 0;
	OrderType type = OrderType::None;
	uint8_t issuer = spotting::kNoSpotter;
	int32_t icon = kIconNone;
	float icon_timer = 0;
	float blink_timer = 0;
};
static_assert(std::is_trivially_copyable_v<OrderIcon>);

void receive_order(OrderIcon &r_icon, uint64_t p_update, OrderType p_type, uint8_t p_issuer, const Supplies &p_mine);
void step_order_icon(OrderIcon &r_icon, float p_dt, bool p_have_soldier);

struct NeedParty {
	float health = 0;
	float max_health = 0;
	bool in_vehicle = false;
	float vehicle_health = 0;
	float vehicle_max_health = 0;
	bool low_ammo = false;
	Supplies supplies;
	const spotting::SpotterDef *def = nullptr;
};

int32_t standing_need_icon(const NeedParty &p_me, const NeedParty &p_mate);
int32_t teammate_icon(const OrderIcon &p_order, uint8_t p_mate, int32_t p_standing, double p_clock);

struct SpotKind {
	bool active = false;
	bool spotter_known = false;
	uint8_t spotter_team = 0;
	bool spotter_flagged = false;
	bool spotter_leads_squad = false;
};

uint8_t spotted_kind(const SpotKind &p_spot, uint8_t p_viewer_team, bool p_no_3d_spotting);
bool minimap_shows_enemy(spotting::SpotState p_state, bool p_no_minimap_spotting);

}
