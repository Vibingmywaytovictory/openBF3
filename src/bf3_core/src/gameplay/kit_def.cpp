// KitDef / TeamDef readers (research/topics/02-soldier-kits-modes.md §2).
#include "bf3/core/gameplay/kit_def.h"

#include "bf3/core/ids.h"
#include "gameplay_read.h"

namespace bf3::core::gameplay {

using namespace detail;

namespace {

KitUnlock read_unlock(const EbxResolver &p_db, const EbxInstance &p_unlock) {
	KitUnlock out;
	out.unlock = partition_name(p_db, p_unlock);
	out.type = std::string(p_unlock.type_name());
	if (p_unlock.is_a("SoldierWeaponUnlockAsset")) {
		out.weapon = ref_name(p_db, p_unlock.data(), "Weapon");
	}
	return out;
}

std::vector<KitSlot> read_table(const EbxResolver &p_db, const EbxStruct &p_kit, std::string_view p_field) {
	std::vector<KitSlot> out;
	const std::optional<EbxInstance> table = p_db.follow(p_kit, p_field);
	if (!table) {
		return out;
	}
	for (const EbxInstance &part : p_db.follow_all(table->data(), "UnlockParts")) {
		KitSlot slot;
		slot.ui_category_sid = text(part.data(), "UICategorySid");
		for (const EbxInstance &unlock : p_db.follow_all(part.data(), "SelectableUnlocks")) {
			slot.unlocks.push_back(read_unlock(p_db, unlock));
		}
		out.push_back(std::move(slot));
	}
	return out;
}

} // namespace

const char *weapon_slot_name(WeaponSlot p_slot) {
	switch (p_slot) {
		case WeaponSlot::Primary:
			return "primary";
		case WeaponSlot::Sidearm:
			return "sidearm";
		case WeaponSlot::Gadget1:
			return "gadget1";
		case WeaponSlot::Unused3:
			return "slot3";
		case WeaponSlot::KitItem:
			return "kit_item";
		case WeaponSlot::Gadget2:
			return "gadget2";
		case WeaponSlot::Grenade:
			return "grenade";
		case WeaponSlot::Knife:
			return "knife";
	}
	return "?";
}

const KitSlot *KitDef::slot(WeaponSlot p_slot) const {
	const size_t at = static_cast<size_t>(p_slot);
	return at < weapon_slots.size() ? &weapon_slots[at] : nullptr;
}

const KitUnlock *KitDef::default_unlock(WeaponSlot p_slot) const {
	const KitSlot *s = slot(p_slot);
	return (s && !s->unlocks.empty()) ? &s->unlocks.front() : nullptr;
}

Result<KitDef> read_kit_def(const EbxResolver &p_db, std::string_view p_asset) {
	const std::string name = to_lower_ascii(p_asset);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("kit: no partition `" + name + "`");
	}
	const EbxInstance kit = partition->primary();
	if (!kit.is_a("VeniceSoldierCustomizationAsset")) {
		return make_error("kit: `" + name + "` is a " + std::string(kit.type_name()) + ", not a VeniceSoldierCustomizationAsset");
	}
	KitDef out;
	out.asset = name;
	out.label_sid = text(kit.data(), "LabelSid");
	out.hud_icon = text(kit.data(), "UIHudIcon");
	out.weapon_slots = read_table(p_db, kit.data(), "WeaponTable");
	for (KitSlot &slot : read_table(p_db, kit.data(), "SpecializationTable")) {
		for (KitUnlock &u : slot.unlocks) {
			out.specializations.push_back(std::move(u));
		}
	}
	for (const KitSlot &slot : read_table(p_db, kit.data(), "VisualTable")) {
		out.visual_unlocks += slot.unlocks.size();
	}
	return out;
}

Result<TeamDef> read_team_data(const EbxResolver &p_db, const EbxInstance &p_team_data) {
	if (!p_team_data.is_a("TeamData")) {
		return make_error("team: " + std::string(p_team_data.type_name()) + " is not a TeamData");
	}
	TeamDef out;
	const EbxStruct t = p_team_data.data();
	out.team_data = partition_name(p_db, p_team_data);
	out.faction = text(t, "Faction");
	out.soldier = ref_name(p_db, t, "Soldier");
	for (const EbxInstance &kit : p_db.follow_all(t, "SoldierCustomization")) {
		out.kits.push_back(partition_name(p_db, kit));
	}
	for (const EbxInstance &v : p_db.follow_all(t, "VehicleCustomization")) {
		out.vehicle_customizations.push_back(partition_name(p_db, v));
	}
	return out;
}

} // namespace bf3::core::gameplay
