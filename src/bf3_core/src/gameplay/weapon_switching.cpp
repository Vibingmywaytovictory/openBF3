#include "bf3/core/gameplay/weapon_switching.h"

#include "gameplay_read.h"

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr std::string_view kSlotPrefix = "wsSlot";
constexpr uint8_t kMaxSwitchSlots = 16;

std::optional<uint8_t> slot_of(std::string_view p_name) {
	if (p_name.size() <= kSlotPrefix.size() || p_name.substr(0, kSlotPrefix.size()) != kSlotPrefix) {
		return std::nullopt;
	}
	uint32_t v = 0;
	for (const char c : p_name.substr(kSlotPrefix.size())) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		v = v * 10 + uint32_t(c - '0');
		if (v >= kMaxSwitchSlots) {
			return std::nullopt;
		}
	}
	return uint8_t(v);
}

}

const DirectWeaponSwitch *WeaponSwitching::direct_of(std::string_view p_action) const {
	for (const DirectWeaponSwitch &d : direct) {
		if (d.action == p_action) {
			return &d;
		}
	}
	return nullptr;
}

const WeaponSwitch *WeaponSwitching::switch_of(std::string_view p_action, uint8_t p_from_slot) const {
	for (const WeaponSwitch &s : map) {
		if (s.action == p_action && s.from_slot == p_from_slot) {
			return &s;
		}
	}
	return nullptr;
}

bool WeaponSwitching::switches_back(std::string_view p_action, uint8_t p_from_slot) const {
	if (const WeaponSwitch *s = switch_of(p_action, p_from_slot)) {
		return s->fire_and_switch_back;
	}
	const DirectWeaponSwitch *d = direct_of(p_action);
	return d != nullptr && d->fire_and_switch_back;
}

Result<WeaponSwitching> read_weapon_switching(const EbxResolver &p_db, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string where(kWeaponSwitchingPartition);
	const EbxPartition *partition = p_db.partition_by_name(kWeaponSwitchingPartition);
	if (partition == nullptr) {
		return make_error(where + ": partition not found");
	}
	const EbxInstance root = partition->primary();
	if (!root.is_a("SoldierWeaponSwitchingData")) {
		return make_error(where + ": a " + std::string(root.type_name()) + ", not a SoldierWeaponSwitchingData");
	}
	BF3_TRY(budget.charge_type(root.data()));
	WeaponSwitching out;
	const OptionalEbxArray direct = array_of(budget, root.data(), "DirectSwitchMap");
	const OptionalEbxArray map = array_of(budget, root.data(), "SwitchMap");
	if (!direct || !map) {
		return make_error(where + ": no DirectSwitchMap or SwitchMap array");
	}
	for (const EbxValue element : *direct) {
		const std::optional<EbxStruct> e = element.as_struct();
		const std::optional<uint8_t> to = e ? slot_of(text_view(*e, "ToWeapon")) : std::nullopt;
		const std::optional<bool> back = e ? boolean(*e, "FireAndSwitchBackToPrev") : std::nullopt;
		const std::optional<bool> quick = e ? boolean(*e, "UseQuickSwitch") : std::nullopt;
		if (!to || !back || !quick) {
			return make_error(where +
					": a DirectSwitchMap row without ToWeapon wsSlotN, FireAndSwitchBackToPrev or UseQuickSwitch");
		}
		DirectWeaponSwitch d;
		d.action = text(budget, *e, "Action");
		d.to_slot = *to;
		d.fire_and_switch_back = *back;
		d.quick_switch = *quick;
		out.direct.push_back(std::move(d));
	}
	for (const EbxValue element : *map) {
		const std::optional<EbxStruct> e = element.as_struct();
		const std::optional<uint8_t> from = e ? slot_of(text_view(*e, "FromWeapon")) : std::nullopt;
		const std::optional<bool> back = e ? boolean(*e, "FireAndSwitchBackToPrev") : std::nullopt;
		const OptionalEbxArray to = e ? array_of(budget, *e, "ToWeapon") : OptionalEbxArray();
		if (!from || !back || !to) {
			return make_error(
					where + ": a SwitchMap row without FromWeapon wsSlotN, FireAndSwitchBackToPrev or ToWeapon");
		}
		WeaponSwitch s;
		s.action = text(budget, *e, "Action");
		s.from_slot = *from;
		s.fire_and_switch_back = *back;
		for (const EbxValue t : *to) {
			const std::optional<uint8_t> named = slot_of(text_view(t));
			const std::optional<int32_t> value = t.enum_value();
			const std::optional<double> n = value ? std::optional<double>(double(*value)) : number(t);
			if (!named && (!n || *n < 0.0 || *n >= double(kMaxSwitchSlots))) {
				return make_error(where + ": a SwitchMap ToWeapon entry is not a slot");
			}
			s.to_slots.push_back(named ? *named : uint8_t(*n));
		}
		out.map.push_back(std::move(s));
	}
	if (const Status spent = budget.status(); !spent) {
		return error_at(where, spent.error());
	}
	return out;
}

}
