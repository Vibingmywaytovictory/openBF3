#include "hud_feed_tdm_parts.h"

#include "bf3/host/kits.h"

#include <algorithm>

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;

namespace {

void fill_list(const HudStatic &p_static, std::span<const uint32_t> p_ids, uint32_t p_selected, ui::HudItemList &r_list,
		int16_t &r_selected) {
	r_list = ui::HudItemList();
	r_selected = -1;
	for (const uint32_t id : p_ids) {
		if (r_list.count >= r_list.items.size()) {
			break;
		}
		if (id == p_selected && p_selected != 0) {
			r_selected = int16_t(r_list.count);
		}
		r_list.items[r_list.count++] = hud_item(p_static, id);
	}
}

std::vector<uint32_t> ids_of(const std::vector<LoadoutItem> &p_items) {
	std::vector<uint32_t> out;
	out.reserve(p_items.size());
	for (const LoadoutItem &i : p_items) {
		out.push_back(i.identifier);
	}
	return out;
}

ui::HudItem item_or_empty(const HudStatic &p_static, uint32_t p_id) {
	return p_id != 0 ? hud_item(p_static, p_id) : ui::HudItem();
}

}

Loadout frame_loadout(Frame &r_frame, uint8_t p_kit) {
	const HudFeedInput &in = r_frame.input;
	Loadout out;
	out.defs = net::kNoSpawnDefs;
	if (in.kits == nullptr || p_kit >= kKitCount) {
		return out;
	}
	if (in.loadouts == nullptr || in.loadouts->empty()) {
		out.defs = in.kits->loadout(r_frame.my_team, p_kit).defs;
		return out;
	}
	if (in.loadout_choices[p_kit] == LoadoutChoice{}) {
		return default_loadout(*in.loadouts, *in.kits, r_frame.my_team, p_kit);
	}
	core::Result<Loadout> chosen =
			resolve_loadout(*in.loadouts, *in.kits, r_frame.my_team, p_kit, in.loadout_choices[p_kit]);
	if (!chosen) {
		++r_frame.state.bad_inputs;
		return default_loadout(*in.loadouts, *in.kits, r_frame.my_team, p_kit);
	}
	return std::move(*chosen);
}

void feed_loadout(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	const HudStatic &st = r_frame.statics;
	ui::HudModel &model = r_frame.model;
	const uint8_t kit = model.spawn_screen.selected_kit < kKitCount ? model.spawn_screen.selected_kit : 0;
	ui::HudCustomization &c = model.customization;
	c = ui::HudCustomization();
	c.customization_team = r_frame.my_team;
	model.kit.selected_kit = kit;
	model.kit.number_of_kits = int32_t(kKitCount);
	model.kit.selected_kit_name = 0;
	const LoadoutKit *selected = in.loadouts != nullptr ? in.loadouts->kit(r_frame.my_team, kit) : nullptr;
	if (selected == nullptr || in.kits == nullptr) {
		return;
	}
	model.kit.selected_kit_name = selected->label_sid_hash;
	Loadout current;
	for (uint8_t k = 0; k < kKitCount; ++k) {
		const LoadoutKit *lk = in.loadouts->kit(r_frame.my_team, k);
		const Loadout &l = r_frame.loadouts[k];
		ui::HudKitEntry &e = c.kits_data[k];
		e.name_sid_hash = lk != nullptr ? lk->label_sid_hash : 0u;
		e.hud_icon = in.kits->loadout(r_frame.my_team, k).hud_icon;
		for (size_t i = 0; i < kHudSpawnSlots.size(); ++i) {
			e.items[i] = item_or_empty(st, l.choice.items[kHudSpawnSlots[i]]);
		}
		if (k == kit) {
			current = l;
		}
	}
	const LoadoutChoice &choice = current.choice;
	c.kit_primary_weapon = item_or_empty(st, choice.items[size_t(core::gameplay::WeaponSlot::Primary)]);
	c.kit_secondary_weapon = item_or_empty(st, choice.items[size_t(core::gameplay::WeaponSlot::Sidearm)]);
	c.kit_gadget1 = item_or_empty(st, choice.items[size_t(core::gameplay::WeaponSlot::Gadget1)]);
	c.kit_gadget2 = item_or_empty(st, choice.items[kHudGadget2Slot]);
	c.kit_specialization = item_or_empty(st, choice.specialization);
	c.weapon_accessory_main = c.kit_primary_weapon;
	c.weapon_accessory1 = item_or_empty(st, choice.accessories[0]);
	c.weapon_accessory2 = item_or_empty(st, choice.accessories[1]);
	c.weapon_accessory3 = item_or_empty(st, choice.accessories[2]);
	c.weapon_accessory4 = item_or_empty(st, choice.accessories[3]);
	for (size_t i = 0; i < kHudSpawnSlots.size(); ++i) {
		const std::vector<uint32_t> ids = ids_of(selected->slots[kHudSpawnSlots[i]]);
		fill_list(st, ids, choice.items[kHudSpawnSlots[i]], c.kit_grid_data[i], c.kit_grid_selected[i]);
	}
	const LoadoutItem *weapon = selected->find(kLoadoutAccessoryWeapon, choice.items[kLoadoutAccessoryWeapon]);
	for (size_t a = 0; a < c.accessory_grid.size(); ++a) {
		const std::span<const uint32_t> ids =
				weapon != nullptr ? std::span<const uint32_t>(weapon->accessories[a]) : std::span<const uint32_t>();
		fill_list(st, ids, choice.accessories[a], c.accessory_grid[a], c.accessory_selected[a]);
	}
}

}
