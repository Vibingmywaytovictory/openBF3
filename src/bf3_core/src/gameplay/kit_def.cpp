// KitDef / TeamDef readers (research/topics/02-soldier-kits-modes.md §2).
#include "bf3/core/gameplay/kit_def.h"

#include "bf3/core/ids.h"
#include "gameplay_read.h"

namespace bf3::core::gameplay {

using namespace detail;

namespace {

// The progression tables, in the order a gate is attributed (an unlock granted by several keeps the
// first). Each holds BasicUnlockInfo structs {UnlockGuid = the unlock asset's instance GUID}.
struct ProgressionTable {
	const char *partition;
	UnlockGate gate;
};
constexpr ProgressionTable kProgressionTables[] = {
	{ "persistence/ranks/rankparams", UnlockGate::Rank },
	{ "persistence/unlocks/mpstaticunlocks", UnlockGate::Licence },
	{ "persistence/awards/awards", UnlockGate::Award },
	{ "persistence/scoring/scoring", UnlockGate::ScoringBucket },
};

void add_unlock_info(UnlockProgression &r_progression, const EbxValue &p_info, UnlockGate p_gate) {
	const std::optional<EbxValue> guid = p_info.get("UnlockGuid");
	if (const std::optional<Guid> g = guid ? guid->as_guid() : std::nullopt) {
		r_progression.gates.emplace(*g, p_gate);
	}
}

// UnlockInfos[] (RankLevelData, StaticUnlockList, AwardData) and Unlocks[].UnlockInfo
// (ScoringBucketData) of one instance.
void add_instance(UnlockProgression &r_progression, const EbxInstance &p_instance, UnlockGate p_gate) {
	if (const std::optional<EbxValue> infos = p_instance.get("UnlockInfos")) {
		if (const OptionalEbxArray array = infos->as_array()) {
			for (const EbxValue info : *array) {
				add_unlock_info(r_progression, info, p_gate);
			}
		}
	}
	if (const std::optional<EbxValue> unlocks = p_instance.get("Unlocks")) {
		if (const OptionalEbxArray array = unlocks->as_array()) {
			for (const EbxValue entry : *array) {
				if (const std::optional<EbxValue> info = entry.get("UnlockInfo")) {
					add_unlock_info(r_progression, *info, p_gate);
				}
			}
		}
	}
}

KitUnlock read_unlock(const EbxResolver &p_db, const EbxInstance &p_unlock, const UnlockProgression &p_progression) {
	KitUnlock out;
	out.unlock = partition_name(p_db, p_unlock);
	out.type = std::string(p_unlock.type_name());
	if (p_unlock.is_a("SoldierWeaponUnlockAsset")) {
		out.weapon = ref_name(p_db, p_unlock.data(), "Weapon");
	}
	out.gate = p_progression.gate(p_unlock.guid());
	const std::optional<EbxValue> extra = p_unlock.get("Extra");
	const std::optional<EbxRef> extra_ref = extra ? extra->as_ref() : std::nullopt;
	out.variant = extra_ref && !extra_ref->is_null();
	return out;
}

std::vector<KitSlot> read_table(const EbxResolver &p_db, const EbxStruct &p_kit, std::string_view p_field, const UnlockProgression &p_progression) {
	std::vector<KitSlot> out;
	const std::optional<EbxInstance> table = p_db.follow(p_kit, p_field);
	if (!table) {
		return out;
	}
	for (const EbxInstance &part : p_db.follow_all(table->data(), "UnlockParts")) {
		KitSlot slot;
		slot.ui_category_sid = text(part.data(), "UICategorySid");
		for (const EbxInstance &unlock : p_db.follow_all(part.data(), "SelectableUnlocks")) {
			slot.unlocks.push_back(read_unlock(p_db, unlock, p_progression));
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

const char *unlock_gate_name(UnlockGate p_gate) {
	switch (p_gate) {
		case UnlockGate::Free:
			return "free";
		case UnlockGate::Rank:
			return "rank";
		case UnlockGate::Licence:
			return "licence";
		case UnlockGate::Award:
			return "award";
		case UnlockGate::ScoringBucket:
			return "scoring";
	}
	return "?";
}

UnlockGate UnlockProgression::gate(const Guid &p_unlock) const {
	const auto it = gates.find(p_unlock);
	return it == gates.end() ? UnlockGate::Free : it->second;
}

UnlockProgression read_unlock_progression(const EbxResolver &p_db) {
	UnlockProgression out;
	for (const ProgressionTable &table : kProgressionTables) {
		const EbxPartition *partition = p_db.partition_by_name(table.partition);
		if (!partition) {
			continue;
		}
		++out.tables;
		for (size_t i = 0; i < partition->instance_count(); ++i) {
			if (const std::optional<EbxInstance> instance = partition->instance(i)) {
				add_instance(out, *instance, table.gate);
			}
		}
	}
	return out;
}

const KitUnlock *KitDef::default_unlock(WeaponSlot p_slot) const {
	const KitSlot *s = slot(p_slot);
	if (!s || !progression_known) {
		return nullptr;
	}
	for (const KitUnlock &u : s->unlocks) {
		if (u.gate == UnlockGate::Free && !u.variant) {
			return &u;
		}
	}
	return nullptr;
}

Result<KitDef> read_kit_def(const EbxResolver &p_db, std::string_view p_asset, const UnlockProgression *p_progression) {
	const std::string name = to_lower_ascii(p_asset);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("kit: no partition `" + name + "`");
	}
	const EbxInstance kit = partition->primary();
	if (!kit.is_a("VeniceSoldierCustomizationAsset")) {
		return make_error("kit: `" + name + "` is a " + std::string(kit.type_name()) + ", not a VeniceSoldierCustomizationAsset");
	}
	UnlockProgression own;
	if (!p_progression) {
		own = read_unlock_progression(p_db);
		p_progression = &own;
	}
	KitDef out;
	out.asset = name;
	out.label_sid = text(kit.data(), "LabelSid");
	out.hud_icon = text(kit.data(), "UIHudIcon");
	out.progression_known = p_progression->known();
	out.weapon_slots = read_table(p_db, kit.data(), "WeaponTable", *p_progression);
	for (KitSlot &slot : read_table(p_db, kit.data(), "SpecializationTable", *p_progression)) {
		for (KitUnlock &u : slot.unlocks) {
			out.specializations.push_back(std::move(u));
		}
	}
	for (const KitSlot &slot : read_table(p_db, kit.data(), "VisualTable", *p_progression)) {
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
