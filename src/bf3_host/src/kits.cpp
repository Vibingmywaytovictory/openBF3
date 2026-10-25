#include "bf3/host/kits.h"

#include "bf3/core/util/fnv.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cctype>

namespace bf3::host {

namespace {

using bf3::core::to_lower_ascii;

bool role_of(std::string_view p_asset, KitRole &r_role) {
	const std::string a = to_lower_ascii(p_asset);
	const size_t slash = a.rfind('/');
	const std::string name = slash == std::string::npos ? a : a.substr(slash + 1);
	static const std::pair<const char *, KitRole> kRoles[] = { { "assault", KitRole::Assault }, { "engineer", KitRole::Engineer },
		{ "support", KitRole::Support }, { "recon", KitRole::Recon } };
	for (const auto &[word, role] : kRoles) {
		if (name.find(word) != std::string::npos) {
			r_role = role;
			return true;
		}
	}
	return false;
}

}

const char *kit_role_name(KitRole p_role) {
	switch (p_role) {
		case KitRole::Assault:
			return "assault";
		case KitRole::Engineer:
			return "engineer";
		case KitRole::Support:
			return "support";
		case KitRole::Recon:
			return "recon";
	}
	return "?";
}

const KitLoadout &KitTable::loadout(uint8_t p_team, uint8_t p_kit) const {
	const size_t team = p_team >= 1 && p_team <= kKitTeams ? size_t(p_team - 1) : 0;
	const size_t kit = p_kit < kKitCount ? size_t(p_kit) : 0;
	return loadouts[team][kit];
}

core::Result<KitTable> make_kit_table(std::span<const core::gameplay::TeamDef> p_teams, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs) {
	return make_kit_table(p_teams, p_kits, p_defs, KitTableOptions{});
}

core::Result<KitTable> make_kit_table(std::span<const core::gameplay::TeamDef> p_teams, std::span<const core::gameplay::KitDef> p_kits,
		std::span<const core::gameplay::WeaponDef> p_defs, const KitTableOptions &p_options) {
	using core::gameplay::WeaponSlot;
	size_t gadget = p_defs.size();
	if (!p_options.assault_gadget.empty()) {
		const std::string want = to_lower_ascii(p_options.assault_gadget);
		for (size_t d = 0; d < p_defs.size(); ++d) {
			if (to_lower_ascii(p_defs[d].variant) == want) {
				gadget = d;
				break;
			}
		}
		if (gadget == p_defs.size() || gadget >= size_t(sim::weapons::kNoWeapon)) {
			return core::make_error("kits: the assault gadget " + p_options.assault_gadget + " has no weapon def");
		}
	}
	KitTable table;
	for (uint8_t t = 0; t < kKitTeams; ++t) {
		const int team_id = int(t) + 1;
		const core::gameplay::TeamDef *team = nullptr;
		for (const core::gameplay::TeamDef &d : p_teams) {
			if (d.team == team_id) {
				team = &d;
				break;
			}
		}
		if (team == nullptr) {
			return core::make_error("kits: the mode has no team " + std::to_string(team_id));
		}
		std::array<bool, kKitCount> filled{};
		for (const std::string &asset : team->kits) {
			KitRole role = KitRole::Assault;
			if (!role_of(asset, role)) {
				table.skipped.push_back(asset + ": not one of the four kit roles");
				continue;
			}
			if (filled[size_t(role)]) {
				return core::make_error("kits: team " + std::to_string(team_id) + " lists two " + kit_role_name(role) + " kits");
			}
			const std::string want = to_lower_ascii(asset);
			const core::gameplay::KitDef *kit = nullptr;
			for (const core::gameplay::KitDef &k : p_kits) {
				if (to_lower_ascii(k.asset) == want) {
					kit = &k;
					break;
				}
			}
			if (kit == nullptr) {
				return core::make_error("kits: team " + std::to_string(team_id) + " kit " + asset + " was not read");
			}
			KitLoadout &l = table.loadouts[t][size_t(role)];
			l.kit = uint8_t(role);
			l.team = uint8_t(team_id);
			l.defs.fill(sim::weapons::kNoWeapon);
			l.active = kKitSlots[0];
			l.hud_icon = kit->hud_icon_value;
			l.asset = want;
			for (size_t s = 0; s < kit->weapon_slots.size() && s < core::gameplay::kWeaponSlotCount; ++s) {
				const core::gameplay::KitUnlock *u = kit->default_unlock(WeaponSlot(s));
				const bool resolved = std::find(kKitSlots.begin(), kKitSlots.end(), uint8_t(s)) != kKitSlots.end();
				if (!resolved) {
					if (u != nullptr) {
						table.skipped.push_back(std::string(kit_role_name(role)) + "/" + std::to_string(team_id) + "/" + std::to_string(s) + ": " + u->unlock);
					}
					continue;
				}
				if (u == nullptr) {
					return core::make_error("kits: " + asset + " slot " + std::to_string(s) + " has no default unlock" +
							(kit->progression_known ? "" : " (the progression tables were not found)"));
				}
				const std::string unlock = to_lower_ascii(u->unlock);
				size_t id = p_defs.size();
				for (size_t d = 0; d < p_defs.size(); ++d) {
					if (to_lower_ascii(p_defs[d].variant) == unlock) {
						id = d;
						break;
					}
				}
				if (id == p_defs.size() || id >= size_t(sim::weapons::kNoWeapon)) {
					return core::make_error("kits: " + asset + " slot " + std::to_string(s) + " unlock " + u->unlock + " has no weapon def");
				}
				l.defs[s] = uint16_t(id);
				l.unlocks[s] = unlock;
			}
			for (const uint8_t s : kKitSlots) {
				if (l.defs[s] == sim::weapons::kNoWeapon) {
					return core::make_error("kits: " + asset + " has no weapon in slot " + std::to_string(s));
				}
			}
			if (gadget != p_defs.size() && role == KitRole::Assault) {
				l.defs[kKitGadgetSlot] = uint16_t(gadget);
				l.unlocks[kKitGadgetSlot] = to_lower_ascii(p_options.assault_gadget);
				const std::string skipped = std::string(kit_role_name(role)) + "/" + std::to_string(team_id) + "/" + std::to_string(kKitGadgetSlot) + ": ";
				std::erase_if(table.skipped, [&](const std::string &p_entry) { return p_entry.rfind(skipped, 0) == 0; });
			}
			filled[size_t(role)] = true;
		}
		for (uint8_t k = 0; k < kKitCount; ++k) {
			if (!filled[k]) {
				return core::make_error("kits: team " + std::to_string(team_id) + " has no " + kit_role_name(KitRole(k)) + " kit");
			}
		}
	}
	if (gadget != p_defs.size()) {
		table.defib_def = uint16_t(gadget);
	}
	table.hash = kit_table_hash(table);
	return table;
}

uint64_t kit_table_hash(const KitTable &p_table) {
	uint64_t h = core::kFnv1aOffset;
	for (const auto &team : p_table.loadouts) {
		for (const KitLoadout &l : team) {
			core::fnv1a_value(h, l.team);
			core::fnv1a_value(h, l.kit);
			for (const uint16_t d : l.defs) {
				core::fnv1a_value(h, d);
			}
			core::fnv1a_value(h, l.active);
		}
	}
	if (p_table.defib_def != sim::weapons::kNoWeapon) {
		core::fnv1a_value(h, p_table.defib_def);
	}
	return h;
}

}
