#pragma once

#include "bf3/ai/behaviour/weapon_choice.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/result.h"
#include "bf3/sim/weapons/gadgets.h"
#include "bf3/sim/weapons/weapon_sim_def.h"
#include "vehicles/vehicle_hits.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
}

namespace bf3::host {

struct GunplayLevel;
struct LevelVehicles;
struct LevelVehicleKind;

inline constexpr const char *kAiWeaponsCacheKind = "ai-weapons";
inline constexpr uint32_t kAiWeaponsConverterVersion = 2;
// STAND-IN: a weaponTemplate strength is the targets of a class the weapon destroys per minute, derived from BF3's
// weapon data as BF3 ships no templates. Settle: compare with BF2's shipped templates' scale.
inline constexpr float kStrengthSeconds = 60.0f;

using WeaponTemplates = ai::WeaponTemplateMap;

struct StrengthTarget {
	ai::CombatStrengthType type = ai::CombatStrengthType::Infantry;
	uint8_t property = 0;
	bool soldier = false;
	float health = 0.0f;
	float armor = 1.0f;
};

struct TemplateSource {
	const GunplayLevel *gunplay = nullptr;
	std::shared_ptr<const sim::weapons::WeaponTable> table;
	std::vector<StrengthTarget> targets;
	VehicleDamageSource vehicle_damage;
	uint16_t defib_def = sim::weapons::kNoWeapon;
	float primary_magazines = 0.0f;
};

struct SeatArms {
	std::array<float, ai::kCombatStrengthTypeCount> strengths{};
	float min_range = 0.0f;
	float max_range = 0.0f;
	bool armed = false;

	bool operator==(const SeatArms &) const = default;
};

using VehicleSeatArms = std::vector<std::vector<SeatArms>>;

struct CachedWeaponTemplates {
	WeaponTemplates templates;
	core::CacheOutcome outcome = core::CacheOutcome::Uncached;
};

const sim::weapons::GadgetDef *gadget_def(const GunplayLevel &p_gunplay, uint16_t p_def);
ai::WeaponType weapon_type_of(const GunplayLevel &p_gunplay, uint16_t p_defib_def, uint16_t p_def);
ai::CombatStrengthType vehicle_strength_type(const LevelVehicleKind &p_kind);
float vehicle_basic_strength(const LevelVehicleKind &p_kind, float p_soldier_health);
core::Result<TemplateSource> template_source(const GunplayLevel &p_gunplay, const LevelVehicles *p_vehicles);
float hit_damage(const TemplateSource &p_source, uint16_t p_def, const sim::weapons::WeaponSimDef &p_weapon,
		const StrengthTarget &p_target);
float rounds_per_second(const sim::weapons::WeaponSimDef &p_weapon);
float weapon_reach(const sim::weapons::WeaponSimDef &p_weapon, bool p_gadget);
ai::WeaponTemplate derive_weapon_template(
		const TemplateSource &p_source, uint16_t p_def, const sim::weapons::WeaponSimDef &p_weapon);
core::Result<WeaponTemplates> derive_weapon_templates(const GunplayLevel &p_gunplay, const LevelVehicles *p_vehicles);
core::Result<VehicleSeatArms> derive_seat_arms(const GunplayLevel &p_gunplay, const LevelVehicles &p_vehicles);
std::string format_weapon_templates(const WeaponTemplates &p_templates);
core::Result<WeaponTemplates> read_weapon_templates(std::string_view p_text, std::string_view p_name);
core::Result<CachedWeaponTemplates> cached_weapon_templates(core::Cache *p_cache, const core::Sha256Digest &p_install,
		std::string_view p_level, std::string_view p_mode, const GunplayLevel &p_gunplay,
		const LevelVehicles *p_vehicles);

class BotWeaponTemplates {
public:
	static core::Result<std::shared_ptr<const BotWeaponTemplates>> create(const sim::weapons::WeaponTable &p_table,
			const GunplayLevel &p_gunplay, const WeaponTemplates &p_templates, const LevelVehicles *p_vehicles);

	const ai::WeaponTemplate &of(uint16_t p_def, uint8_t p_variant) const;
	ai::CombatStrengthType vehicle_type(uint8_t p_property) const;
	const SeatArms &seat(uint16_t p_kind, uint8_t p_seat) const;

private:
	WeaponTemplates owned_;
	std::vector<std::vector<const ai::WeaponTemplate *>> defs_;
	std::vector<std::pair<uint8_t, ai::CombatStrengthType>> vehicle_types_;
	VehicleSeatArms seats_;
};

bool fires_at(const GunplayLevel &p_gunplay, uint16_t p_def, const ai::WeaponTemplate &p_template, bool p_vehicle);
ai::WeaponChoice template_choice(const ai::WeaponTemplate &p_template, ai::CombatStrengthType p_type,
		ai::WeaponType p_weapon_type, int32_t p_ammo);
}
