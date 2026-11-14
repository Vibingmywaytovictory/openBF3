#include "bot_weapon_templates.h"

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/util/text.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/sim/vehicles/vehicle_damage.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "vehicles/vehicle_hits.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;

constexpr std::string_view kScriptBreak = "\n\n";

float relation_damage(float p_base, const core::gameplay::DamageRelation &p_relation) {
	return p_relation.present ? sim::vehicles::material_relation_damage(
										p_base, p_relation.protection_multiplier, p_relation.protection_threshold)
							  : p_base;
}

std::optional<uint8_t> blast_property(const GunplayLevel &p_gunplay, uint16_t p_def) {
	if (p_def >= p_gunplay.defs.size() || p_gunplay.materials == nullptr ||
			!p_gunplay.defs[p_def].projectile.explosion.material.present) {
		return std::nullopt;
	}
	const sw::SurfacePair pair = p_gunplay.materials->pair_of(p_gunplay.defs[p_def].projectile.explosion.material);
	return pair.resolved ? std::optional<uint8_t>(pair.property) : std::nullopt;
}

float direct_damage(const sw::ProjectileSimDef &p_projectile) {
	if (!sw::fires_round(p_projectile)) {
		return 0.0f;
	}
	return p_projectile.kind == core::gameplay::ProjectileKind::Missile ? p_projectile.direct_damage
																		: p_projectile.start_damage;
}

float primary_magazines(const GunplayLevel &p_gunplay, const sw::WeaponTable &p_table) {
	float sum = 0.0f;
	int count = 0;
	for (const auto &team : p_gunplay.kits.loadouts) {
		for (const KitLoadout &kit : team) {
			const uint16_t d = kit.defs[kKitSlots[0]];
			if (d < p_table.defs.size() && p_table.defs[d].magazines > 0) {
				sum += float(p_table.defs[d].magazines);
				++count;
			}
		}
	}
	return count > 0 ? sum / float(count) : 0.0f;
}

float class_mean(const TemplateSource &p_source, ai::CombatStrengthType p_type,
		float (*p_per_target)(const TemplateSource &, uint16_t, const sw::WeaponSimDef &, const StrengthTarget &),
		uint16_t p_def, const sw::WeaponSimDef &p_weapon) {
	float sum = 0.0f;
	int count = 0;
	for (const StrengthTarget &t : p_source.targets) {
		if (t.type == p_type) {
			sum += p_per_target(p_source, p_def, p_weapon, t);
			++count;
		}
	}
	return count > 0 ? sum / float(count) : 0.0f;
}

float kills_per_minute(const TemplateSource &p_source, uint16_t p_def, const sw::WeaponSimDef &p_weapon,
		const StrengthTarget &p_target) {
	if (!(p_target.health > 0.0f)) {
		return 0.0f;
	}
	const float share = std::min(hit_damage(p_source, p_def, p_weapon, p_target) / p_target.health, 1.0f);
	return kStrengthSeconds * rounds_per_second(p_weapon) * share;
}

float repairs_per_minute(
		const TemplateSource &, uint16_t, const sw::WeaponSimDef &p_weapon, const StrengthTarget &p_target) {
	if (p_target.soldier || !(p_target.health > 0.0f)) {
		return 0.0f;
	}
	return kStrengthSeconds * rounds_per_second(p_weapon) * p_weapon.projectile.start_damage / p_target.health;
}

}

const sw::GadgetDef *gadget_def(const GunplayLevel &p_gunplay, uint16_t p_def) {
	if (p_def == sw::kNoWeapon) {
		return nullptr;
	}
	for (const sw::GadgetDef &g : p_gunplay.gadgets) {
		if (g.def == p_def) {
			return &g;
		}
	}
	return nullptr;
}

ai::WeaponType weapon_type_of(const GunplayLevel &p_gunplay, uint16_t p_defib_def, uint16_t p_def) {
	if (p_def == sw::kNoWeapon) {
		return ai::WeaponType::Default;
	}
	if (p_def == p_defib_def) {
		return ai::WeaponType::Resurrect;
	}
	if (p_def < p_gunplay.defs.size() && p_gunplay.defs[p_def].weapon_class == kRepairToolClass) {
		return ai::WeaponType::Heal;
	}
	const sw::GadgetDef *g = gadget_def(p_gunplay, p_def);
	if (g == nullptr) {
		return ai::WeaponType::Default;
	}
	if (g->kind == sw::GadgetKind::ExplosionPack && g->pack.remote_detonator) {
		return ai::WeaponType::C4;
	}
	if (g->kind == sw::GadgetKind::Supply) {
		return g->healing.active() ? ai::WeaponType::Heal : ai::WeaponType::Ammo;
	}
	return ai::WeaponType::Default;
}

ai::CombatStrengthType vehicle_strength_type(const LevelVehicleKind &p_kind) {
	return p_kind.model != nullptr && land_tracked(*p_kind.model) ? ai::CombatStrengthType::HeavyArmour
																  : ai::CombatStrengthType::LightArmour;
}

core::Result<TemplateSource> template_source(const GunplayLevel &p_gunplay, const LevelVehicles *p_vehicles) {
	if (p_gunplay.materials == nullptr) {
		return core::make_error("weapon templates: the level has no surface materials");
	}
	core::Result<std::shared_ptr<const sw::WeaponTable>> table =
			make_match_table(p_gunplay, sw::Mechanics{}, false, false);
	if (!table) {
		return core::error_at("weapon templates", table.error());
	}
	TemplateSource out;
	out.gunplay = &p_gunplay;
	out.table = *table;
	out.defib_def = p_gunplay.kits.defib_def;
	out.primary_magazines = primary_magazines(p_gunplay, *out.table);
	const sw::SurfaceMaterialTable &materials = *p_gunplay.materials;
	StrengthTarget soldier;
	soldier.soldier = true;
	soldier.property = materials.zone_property(sw::HitZone::Chest);
	soldier.health = p_gunplay.max_health;
	out.targets.push_back(soldier);
	if (p_vehicles != nullptr) {
		for (const LevelVehicleKind &k : p_vehicles->kinds) {
			const sw::SurfacePair pair = materials.pair_of(k.material);
			if (!pair.resolved) {
				continue;
			}
			StrengthTarget t;
			t.property = pair.property;
			t.health = sim::vehicles::full_vehicle_health(k.damage);
			t.armor = k.damage.armor_multiplier;
			t.type = vehicle_strength_type(k);
			out.targets.push_back(t);
		}
	}
	return out;
}

float hit_damage(const TemplateSource &p_source, uint16_t p_def, const sw::WeaponSimDef &p_weapon,
		const StrengthTarget &p_target) {
	const sw::ProjectileSimDef &p = p_weapon.projectile;
	const sw::SurfaceMaterialTable &materials = *p_source.gunplay->materials;
	const sw::HitZone chest = sw::HitZone::Chest;
	float direct = direct_damage(p);
	if (p_target.soldier) {
		direct *= p.zone[size_t(chest)].protection_multiplier;
	} else {
		direct = relation_damage(direct, materials.relation(p.property, p_target.property));
	}
	direct *= float(std::max(p_weapon.pellets, 1));
	float blast = 0.0f;
	if (p.kind != core::gameplay::ProjectileKind::None && p.explosion.present) {
		const sw::ExplosionSimDef &x = p.explosion;
		if (p_target.soldier) {
			blast = sim::vehicles::material_relation_damage(
					x.blast_damage, x.zone_multiplier[size_t(chest)], x.zone_threshold[size_t(chest)]);
		} else {
			const std::optional<uint8_t> property = blast_property(*p_source.gunplay, p_def);
			blast = property ? relation_damage(x.blast_damage, materials.relation(*property, p_target.property))
							 : x.blast_damage;
		}
	}
	const float damage = direct + blast;
	return p_target.armor > 0.0f ? damage / p_target.armor : 0.0f;
}

float rounds_per_second(const sw::WeaponSimDef &p_weapon) {
	float period = 0.0f;
	for (uint8_t m = 0; m < p_weapon.mode_count && m < p_weapon.modes.size(); ++m) {
		const float s = float(sw::sim_shot_interval_seconds(p_weapon, m));
		if (s > 0.0f && (period == 0.0f || s < period)) {
			period = s;
		}
	}
	const float magazine = float(std::max(p_weapon.magazine_capacity, 1));
	const float reload = p_weapon.unlimited_ammo ? 0.0f : float(sw::units_to_seconds(p_weapon.reload_empty));
	const float cycle = magazine * period + reload;
	return cycle > 0.0f ? magazine / cycle : 0.0f;
}

float weapon_reach(const sw::WeaponSimDef &p_weapon, bool p_gadget) {
	const sw::ProjectileSimDef &p = p_weapon.projectile;
	const float speed = std::max(p.muzzle_speed, p.max_speed);
	float reach = float(sw::units_to_seconds(p.time_to_live)) * speed;
	const float gravity = p_gadget ? sw::kGadgetGravity : std::fabs(p.gravity);
	if (p.engine_acceleration == 0.0f && gravity > 0.0f) {
		reach = std::min(reach, p.muzzle_speed * p.muzzle_speed / gravity);
	}
	return reach;
}

ai::WeaponTemplate derive_weapon_template(
		const TemplateSource &p_source, uint16_t p_def, const sw::WeaponSimDef &p_weapon) {
	ai::WeaponTemplate w;
	const sw::ProjectileSimDef &p = p_weapon.projectile;
	if (p.kind == core::gameplay::ProjectileKind::None) {
		return w;
	}
	const GunplayLevel &level = *p_source.gunplay;
	const sw::GadgetDef *gadget = gadget_def(level, p_def);
	w.drag = p.drag;
	w.exit_velocity = p.muzzle_speed;
	w.is_thrown = p_weapon.mode_count > 0 && p_weapon.modes[0].logic == core::gameplay::FireLogic::HoldAndRelease;
	w.max_range = weapon_reach(p_weapon, gadget != nullptr);
	if (p.explosion.present) {
		w.explosion_radius = p.explosion.radius;
		const float arming = float(sw::units_to_seconds(p.time_to_arm)) * p.muzzle_speed;
		w.min_range = std::min(std::max(arming, p.explosion.radius), w.max_range);
	}
	const size_t infantry = size_t(ai::CombatStrengthType::Infantry);
	const ai::WeaponType type = weapon_type_of(level, p_source.defib_def, p_def);
	std::array<float, ai::kCombatStrengthTypeCount> per_minute{};
	if (type == ai::WeaponType::Resurrect) {
		per_minute[infantry] = kStrengthSeconds * rounds_per_second(p_weapon);
	} else if (type == ai::WeaponType::Heal && gadget != nullptr) {
		per_minute[infantry] =
				level.max_health > 0.0f ? kStrengthSeconds * gadget->healing.inc_speed / level.max_health : 0.0f;
	} else if (type == ai::WeaponType::Ammo && gadget != nullptr) {
		per_minute[infantry] = p_source.primary_magazines > 0.0f
				? kStrengthSeconds * gadget->ammo.inc_speed / p_source.primary_magazines
				: 0.0f;
	} else {
		const auto per_target = type == ai::WeaponType::Heal ? repairs_per_minute : kills_per_minute;
		for (size_t c = 0; c < ai::kCombatStrengthTypeCount; ++c) {
			per_minute[c] = class_mean(p_source, ai::CombatStrengthType(c), per_target, p_def, p_weapon);
		}
	}
	for (size_t c = 0; c < ai::kCombatStrengthTypeCount; ++c) {
		w.strengths[c] = int32_t(std::lround(per_minute[c]));
	}
	return w;
}

core::Result<WeaponTemplates> derive_weapon_templates(const GunplayLevel &p_gunplay, const LevelVehicles *p_vehicles) {
	core::Result<TemplateSource> source = template_source(p_gunplay, p_vehicles);
	if (!source) {
		return source.error();
	}
	const sw::WeaponTable &t = *source->table;
	WeaponTemplates out;
	const auto add = [&](uint16_t p_def, const sw::WeaponSimDef &p_weapon) {
		if (!p_weapon.variant.empty() && out.find(p_weapon.variant) == out.end()) {
			out.emplace(p_weapon.variant, derive_weapon_template(*source, p_def, p_weapon));
		}
	};
	for (size_t d = 0; d < t.defs.size() && d < size_t(sw::kNoWeapon); ++d) {
		add(uint16_t(d), t.defs[d]);
		if (d < t.variants.size()) {
			for (const sw::WeaponSimDef &v : t.variants[d]) {
				add(uint16_t(d), v);
			}
		}
	}
	return out;
}

std::string format_weapon_templates(const WeaponTemplates &p_templates) {
	std::string out;
	for (const auto &[name, w] : p_templates) {
		out += ai::format_weapon_template(name, w);
		out += "\n";
	}
	return out;
}

core::Result<WeaponTemplates> read_weapon_templates(std::string_view p_text, std::string_view p_name) {
	ai::AiConfig config;
	size_t at = 0;
	while (at < p_text.size()) {
		const size_t end = std::min(p_text.find(kScriptBreak, at), p_text.size());
		BF3_TRY(ai::run_ai_script(p_text.substr(at, end - at), std::string(p_name), config));
		at = end + kScriptBreak.size();
	}
	return config.weapon_templates;
}

core::Result<CachedWeaponTemplates> cached_weapon_templates(core::Cache *p_cache, const core::Sha256Digest &p_install,
		std::string_view p_level, std::string_view p_mode, const GunplayLevel &p_gunplay,
		const LevelVehicles *p_vehicles) {
	core::CacheKeyInputs in;
	in.kind = kAiWeaponsCacheKind;
	in.converter_version = kAiWeaponsConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kAiWeaponsConverterVersion;
	in.params = "install=" + p_install.hex() + ";level=" + core::to_lower_ascii(p_level) +
			";mode=" + core::to_lower_ascii(p_mode);
	const std::optional<core::CacheKey> key = core::make_cache_key(in);
	if (!key) {
		return core::make_error("ai weapons: no cache key for " + std::string(p_level));
	}
	CachedWeaponTemplates out;
	const auto accept = [&](std::span<const uint8_t> p_payload) -> core::Status {
		const std::string text(p_payload.begin(), p_payload.end());
		core::Result<WeaponTemplates> read = read_weapon_templates(text, kAiWeaponsCacheKind);
		if (!read) {
			return read.error();
		}
		out.templates = std::move(*read);
		return core::Status::success();
	};
	const auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
		core::Result<WeaponTemplates> derived = derive_weapon_templates(p_gunplay, p_vehicles);
		if (!derived) {
			return derived.error();
		}
		out.templates = std::move(*derived);
		const std::string text = format_weapon_templates(out.templates);
		return std::vector<uint8_t>(text.begin(), text.end());
	};
	core::Result<core::CachedItem> item = core::cached_convert(p_cache, *key, accept, produce);
	if (!item) {
		return core::error_at("ai weapons", item.error());
	}
	out.outcome = item->outcome;
	return out;
}

core::Result<std::shared_ptr<const BotWeaponTemplates>> BotWeaponTemplates::create(const sw::WeaponTable &p_table,
		const GunplayLevel &p_gunplay, const WeaponTemplates &p_templates, const LevelVehicles *p_vehicles) {
	auto out = std::make_shared<BotWeaponTemplates>();
	std::optional<TemplateSource> source;
	const auto resolve = [&](uint16_t p_def,
								 const sw::WeaponSimDef &p_weapon) -> core::Result<const ai::WeaponTemplate *> {
		if (const ai::WeaponTemplate *given = ai::named_weapon_template(p_templates, p_weapon.variant)) {
			return given;
		}
		const auto kept = out->owned_.find(p_weapon.variant);
		if (kept != out->owned_.end()) {
			return &kept->second;
		}
		if (!source) {
			core::Result<TemplateSource> made = template_source(p_gunplay, p_vehicles);
			if (!made) {
				return made.error();
			}
			source = std::move(*made);
		}
		return &out->owned_.emplace(p_weapon.variant, derive_weapon_template(*source, p_def, p_weapon)).first->second;
	};
	out->defs_.resize(std::min(p_table.defs.size(), size_t(sw::kNoWeapon)));
	for (size_t d = 0; d < out->defs_.size(); ++d) {
		core::Result<const ai::WeaponTemplate *> base = resolve(uint16_t(d), p_table.defs[d]);
		if (!base) {
			return base.error();
		}
		out->defs_[d].push_back(*base);
		if (d < p_table.variants.size()) {
			for (const sw::WeaponSimDef &v : p_table.variants[d]) {
				core::Result<const ai::WeaponTemplate *> variant = resolve(uint16_t(d), v);
				if (!variant) {
					return variant.error();
				}
				out->defs_[d].push_back(*variant);
			}
		}
	}
	if (p_vehicles != nullptr && p_gunplay.materials != nullptr) {
		for (const LevelVehicleKind &k : p_vehicles->kinds) {
			const sw::SurfacePair pair = p_gunplay.materials->pair_of(k.material);
			if (pair.resolved) {
				out->vehicle_types_.push_back({ pair.property, vehicle_strength_type(k) });
			}
		}
	}
	return std::shared_ptr<const BotWeaponTemplates>(std::move(out));
}

const ai::WeaponTemplate &BotWeaponTemplates::of(uint16_t p_def, uint8_t p_variant) const {
	static const ai::WeaponTemplate kNone;
	if (p_def >= defs_.size()) {
		return kNone;
	}
	const std::vector<const ai::WeaponTemplate *> &d = defs_[p_def];
	return p_variant < d.size() ? *d[p_variant] : *d.front();
}

ai::CombatStrengthType BotWeaponTemplates::vehicle_type(uint8_t p_property) const {
	for (const auto &[property, type] : vehicle_types_) {
		if (property == p_property) {
			return type;
		}
	}
	return ai::CombatStrengthType::LightArmour;
}

ai::WeaponChoice template_choice(const ai::WeaponTemplate &p_template, ai::CombatStrengthType p_type,
		ai::WeaponType p_weapon_type, int32_t p_ammo) {
	ai::WeaponChoice c;
	c.type = p_weapon_type;
	c.indirect = p_template.indirect;
	c.ammo = p_ammo;
	c.strength = p_template.strength(p_type);
	c.min_range = p_template.min_range;
	c.max_range = p_template.max_range;
	return c;
}
}
