#include "bf3/host/loadout.h"

#include "bf3/core/util/text.h"

#include <algorithm>
#include <map>
#include <set>

namespace bf3::host {

namespace {

namespace gp = core::gameplay;
using core::to_lower_ascii;

bool simulated(size_t p_slot) {
	return std::find(kKitSlots.begin(), kKitSlots.end(), uint8_t(p_slot)) != kKitSlots.end();
}

void fnv(uint64_t &r_hash, uint64_t p_value) {
	for (int i = 0; i < 8; ++i) {
		r_hash ^= (p_value >> (8 * i)) & 0xFFu;
		r_hash *= 0x100000001B3ull;
	}
}

bool contains(const std::vector<uint32_t> &p_ids, uint32_t p_id) {
	return std::find(p_ids.begin(), p_ids.end(), p_id) != p_ids.end();
}

std::string where_of(uint8_t p_team, uint8_t p_kit) {
	return "loadout team " + std::to_string(p_team) + " kit " +
			std::string(kit_role_name(KitRole(p_kit < kKitCount ? p_kit : 0)));
}

}

const LoadoutItem *LoadoutKit::find(size_t p_slot, uint32_t p_identifier) const {
	if (p_slot >= slots.size() || p_identifier == 0) {
		return nullptr;
	}
	for (const LoadoutItem &i : slots[p_slot]) {
		if (i.identifier == p_identifier) {
			return &i;
		}
	}
	return nullptr;
}

const LoadoutKit *LoadoutCatalog::kit(uint8_t p_team, uint8_t p_kit) const {
	if (empty() || p_team < 1 || p_team > kKitTeams || p_kit >= kKitCount) {
		return nullptr;
	}
	return &kits[size_t(p_team - 1)][p_kit];
}

std::vector<std::string> loadout_variants(std::span<const gp::KitDef> p_kits) {
	std::set<std::string> out;
	for (const gp::KitDef &k : p_kits) {
		for (const uint8_t s : kKitSlots) {
			const gp::KitSlot *slot = k.slot(gp::WeaponSlot(s));
			if (slot == nullptr) {
				continue;
			}
			for (const gp::KitUnlock &u : slot->unlocks) {
				if (u.type == "SoldierWeaponUnlockAsset") {
					out.insert(to_lower_ascii(u.unlock));
				}
			}
		}
	}
	return std::vector<std::string>(out.begin(), out.end());
}

core::Result<LoadoutCatalog> make_loadout_catalog(const core::EbxResolver &p_db,
		const gp::UnlockProgression &p_progression, std::span<const gp::KitDef> p_kits,
		std::span<const gp::WeaponDef> p_defs, const KitTable &p_table, gp::WorkBudget *p_budget) {
	gp::WorkBudget own;
	gp::WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	std::map<std::string, uint16_t> by_variant;
	for (size_t d = 0; d < p_defs.size() && d < size_t(sim::weapons::kNoWeapon); ++d) {
		by_variant.emplace(to_lower_ascii(p_defs[d].variant), uint16_t(d));
	}
	std::map<std::string, std::array<std::vector<uint32_t>, kLoadoutAccessories>> parts_of;
	LoadoutCatalog out;
	for (uint8_t t = 0; t < kKitTeams; ++t) {
		for (uint8_t k = 0; k < kKitCount; ++k) {
			const KitLoadout &l = p_table.loadouts[t][k];
			const std::string where = where_of(uint8_t(t + 1), k);
			const gp::KitDef *kit = nullptr;
			for (const gp::KitDef &d : p_kits) {
				if (to_lower_ascii(d.asset) == l.asset) {
					kit = &d;
					break;
				}
			}
			if (kit == nullptr) {
				return core::make_error(where + ": the kit " + l.asset + " was not read");
			}
			LoadoutKit &lk = out.kits[t][k];
			for (size_t s = 0; s < kLoadoutSlots; ++s) {
				const uint16_t def = l.defs[s];
				if (def != sim::weapons::kNoWeapon && def < p_defs.size()) {
					lk.defaults[s] = p_defs[def].unlock_identifier;
				} else if (const gp::KitUnlock *u = kit->default_unlock(gp::WeaponSlot(s))) {
					lk.defaults[s] = u->identifier;
				}
				const gp::KitSlot *slot = kit->slot(gp::WeaponSlot(s));
				if (slot == nullptr) {
					continue;
				}
				for (const gp::KitUnlock &u : slot->unlocks) {
					if (u.identifier == 0 || lk.find(s, u.identifier) != nullptr) {
						continue;
					}
					LoadoutItem item;
					item.identifier = u.identifier;
					item.unlock = to_lower_ascii(u.unlock);
					if (simulated(s)) {
						const auto it = by_variant.find(item.unlock);
						if (it == by_variant.end()) {
							return core::make_error(
									where + " slot " + std::to_string(s) + ": " + item.unlock + " has no weapon def");
						}
						item.def = it->second;
					}
					if (s == kLoadoutAccessoryWeapon) {
						auto cached = parts_of.find(item.unlock);
						if (cached == parts_of.end()) {
							core::Result<std::vector<gp::KitSlot>> parts =
									gp::read_weapon_customization(p_db, item.unlock, p_progression, &budget);
							if (!parts) {
								return core::error_at(where, parts.error());
							}
							if (parts->size() > kLoadoutAccessories) {
								return core::make_error(where + ": " + item.unlock + " has " +
										std::to_string(parts->size()) + " customization parts (limit " +
										std::to_string(kLoadoutAccessories) + ")");
							}
							std::array<std::vector<uint32_t>, kLoadoutAccessories> ids;
							for (size_t p = 0; p < parts->size(); ++p) {
								for (const gp::KitUnlock &a : (*parts)[p].unlocks) {
									if (a.identifier != 0 && !contains(ids[p], a.identifier)) {
										ids[p].push_back(a.identifier);
									}
								}
							}
							cached = parts_of.emplace(item.unlock, std::move(ids)).first;
						}
						item.accessories = cached->second;
					}
					lk.slots[s].push_back(std::move(item));
					++out.items;
				}
			}
			for (const gp::KitUnlock &u : kit->specializations) {
				if (u.identifier != 0 && !contains(lk.specializations, u.identifier)) {
					lk.specializations.push_back(u.identifier);
				}
			}
		}
	}
	if (const core::Status spent = budget.status(); !spent) {
		return core::error_at("loadout catalog", spent.error());
	}
	out.hash = loadout_catalog_hash(out);
	return out;
}

Loadout default_loadout(const LoadoutCatalog &p_catalog, const KitTable &p_table, uint8_t p_team, uint8_t p_kit) {
	Loadout out;
	out.defs = p_table.loadout(p_team, p_kit).defs;
	if (const LoadoutKit *k = p_catalog.kit(p_team, p_kit)) {
		out.choice.items = k->defaults;
	}
	return out;
}

core::Result<Loadout> resolve_loadout(const LoadoutCatalog &p_catalog, const KitTable &p_table, uint8_t p_team,
		uint8_t p_kit, const LoadoutChoice &p_choice) {
	const std::string where = where_of(p_team, p_kit);
	const LoadoutKit *k = p_catalog.kit(p_team, p_kit);
	if (k == nullptr) {
		return core::make_error(where + ": no loadout catalog for it");
	}
	Loadout out = default_loadout(p_catalog, p_table, p_team, p_kit);
	for (size_t s = 0; s < kLoadoutSlots; ++s) {
		const uint32_t id = p_choice.items[s];
		if (id == 0 || id == k->defaults[s]) {
			continue;
		}
		const LoadoutItem *item = k->find(s, id);
		if (item == nullptr) {
			return core::make_error(
					where + ": slot " + std::to_string(s) + " does not allow item " + std::to_string(id));
		}
		out.choice.items[s] = id;
		if (simulated(s)) {
			out.defs[s] = item->def;
		}
	}
	const LoadoutItem *weapon = k->find(kLoadoutAccessoryWeapon, out.choice.items[kLoadoutAccessoryWeapon]);
	for (size_t a = 0; a < kLoadoutAccessories; ++a) {
		const uint32_t id = p_choice.accessories[a];
		if (id == 0) {
			continue;
		}
		if (weapon == nullptr || !contains(weapon->accessories[a], id)) {
			return core::make_error(where + ": accessory slot " + std::to_string(a) +
					" of the primary does not allow item " + std::to_string(id));
		}
		out.choice.accessories[a] = id;
	}
	if (p_choice.specialization != 0) {
		if (!contains(k->specializations, p_choice.specialization)) {
			return core::make_error(where + ": no specialization " + std::to_string(p_choice.specialization));
		}
		out.choice.specialization = p_choice.specialization;
	}
	out.chosen = true;
	return out;
}

uint64_t loadout_catalog_hash(const LoadoutCatalog &p_catalog) {
	uint64_t h = 0xCBF29CE484222325ull;
	for (const auto &team : p_catalog.kits) {
		for (const LoadoutKit &k : team) {
			for (size_t s = 0; s < kLoadoutSlots; ++s) {
				fnv(h, k.defaults[s]);
				fnv(h, k.slots[s].size());
				for (const LoadoutItem &i : k.slots[s]) {
					fnv(h, i.identifier);
					fnv(h, i.def);
					for (const std::vector<uint32_t> &part : i.accessories) {
						fnv(h, part.size());
						for (const uint32_t a : part) {
							fnv(h, a);
						}
					}
				}
			}
			fnv(h, k.specializations.size());
			for (const uint32_t s : k.specializations) {
				fnv(h, s);
			}
		}
	}
	return h;
}

}
