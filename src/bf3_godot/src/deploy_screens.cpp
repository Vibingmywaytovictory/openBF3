#include "deploy_screens.h"

#include "native_flow.h"
#include "native_hud.h"
#include "native_screens.h"
#include "native_tags.h"
#include "overview_map.h"
#include "swf_view.h"
#include "ui_cursor.h"
#include "ui_post_process.h"

#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/ui_post_process.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/ui_flow_session.h"

#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <limits>

namespace bf3::godot_side {

namespace {

const char *flow_bool(bool p_value) {
	return p_value ? "True" : "False";
}

std::optional<int32_t> flow_index(std::string_view p_text, size_t p_count) {
	int32_t index = 0;
	if (p_count == 0 ||
			bf3::core::parse_i32_value(p_text, 0.0, double(p_count) - 1.0, index).fault != bf3::core::KeyFault::None) {
		return std::nullopt;
	}
	return index;
}

}

bf3::core::Result<bf3::net::LoadoutRequest> deploy_loadout_request(const DeployPick &p_pick,
		const bf3::host::LoadoutCatalog &p_catalog, const bf3::host::KitTable &p_table, uint8_t p_team,
		bf3::host::LoadoutChoice &r_choice) {
	const bf3::host::LoadoutKit *kit = p_catalog.kit(p_team, p_pick.kit);
	if (p_pick.slot >= bf3::host::kLoadoutKitItems || kit == nullptr ||
			p_pick.accessory >= int8_t(bf3::host::kLoadoutAccessories)) {
		return bf3::core::make_error("deploy pick: no kit item " + std::to_string(p_pick.slot));
	}
	bf3::host::LoadoutChoice next = r_choice == bf3::host::LoadoutChoice{}
			? bf3::host::default_loadout(p_catalog, p_table, p_team, p_pick.kit).choice
			: r_choice;
	const bf3::host::LoadoutKitItem item = bf3::host::LoadoutKitItem(p_pick.slot);
	if (p_pick.accessory >= 0) {
		next.accessories[size_t(p_pick.accessory)] = p_pick.item;
	} else if (!bf3::host::set_loadout_item(*kit, item, p_pick.item, next)) {
		return bf3::core::make_error(
				"deploy pick: kit item " + std::to_string(p_pick.slot) + " has no item " + std::to_string(p_pick.item));
	} else if (item == bf3::host::LoadoutKitItem::Primary) {
		next.accessories = {};
	}
	if (const bf3::core::Result<bf3::host::Loadout> r =
					bf3::host::resolve_loadout(p_catalog, p_table, p_team, p_pick.kit, next);
			!r) {
		return r.error();
	}
	r_choice = next;
	bf3::net::LoadoutRequest q;
	q.kit = p_pick.kit;
	std::copy(next.items.begin(), next.items.end(), q.items.begin());
	q.accessories = next.accessories;
	q.specialization = next.specialization;
	return q;
}

void DeployScreens::draw(Bf3UiScreens &r_screens) {
	for (const std::string_view p : bf3::core::ui::kDeployDrawnPartitions) {
		r_screens.set_drawn(p, true);
	}
	for (const std::string_view p : kCustomizeScreenPartitions) {
		r_screens.set_drawn(p, true);
	}
}

std::optional<uint8_t> DeployScreens::feed(bf3::host::UiFlowSession &r_session, const bf3::core::ui::HudModel &p_model,
		bool p_scoreboard_held, uint8_t p_spawn_kit) {
	std::optional<uint8_t> chosen;
	if (const std::optional<std::string> v = r_session.data(kUiKitCompPartition, kUiKitSelectedKitKey);
			v && kit_written_ && *v != *kit_written_) {
		if (const std::optional<int32_t> k = flow_index(*v, bf3::core::ui::kHudKitCount)) {
			chosen = uint8_t(*k);
			++stats_.kits_from_flow;
		}
	}
	if (const std::optional<std::string> v =
					r_session.data(kUiCustomizationCompPartition, kUiCustomizationSelectedSlotKey)) {
		if (const std::optional<int32_t> s = flow_index(*v, bf3::core::ui::kHudSpawnKitItems)) {
			slot_ = *s;
		}
	}
	if (const std::optional<std::string> v =
					r_session.data(kUiCustomizationCompPartition, kUiCustomizationSetWeaponKey);
			v && !v->empty()) {
		const bf3::core::ui::HudItemList &list = p_model.customization.kit_grid_data[size_t(slot_)];
		if (const std::optional<int32_t> i = flow_index(*v, size_t(list.count))) {
			pick_ = DeployPick{ uint8_t(chosen.value_or(p_spawn_kit)), uint8_t(slot_), list.items[size_t(*i)].item_id };
			++stats_.picks_from_flow;
		}
		r_session.set_data(kUiCustomizationCompPartition, kUiCustomizationSetWeaponKey, std::string());
	}
	const uint8_t pick_kit = chosen.value_or(p_spawn_kit);
	const bf3::core::ui::HudCustomization &c = p_model.customization;
	for (size_t i = 0; i < kUiCustomizationItemKeys.size(); ++i) {
		if (const std::optional<std::string> v =
						r_session.data(kUiCustomizationCompPartition, kUiCustomizationItemKeys[i]);
				v && !v->empty()) {
			if (const std::optional<int32_t> at = flow_index(*v, size_t(c.kit_grid_data[i].count))) {
				pick_ = DeployPick{ pick_kit, uint8_t(i), c.kit_grid_data[i].items[size_t(*at)].item_id };
				++stats_.picks_from_flow;
			}
			r_session.set_data(kUiCustomizationCompPartition, kUiCustomizationItemKeys[i], std::string());
		}
	}
	for (size_t a = 0; a < kUiCustomizationAccessoryKeys.size(); ++a) {
		if (const std::optional<std::string> v =
						r_session.data(kUiCustomizationCompPartition, kUiCustomizationAccessoryKeys[a]);
				v && !v->empty()) {
			if (const std::optional<int32_t> at = flow_index(*v, size_t(c.accessory_grid[a].count))) {
				pick_ = DeployPick{ pick_kit, 0, c.accessory_grid[a].items[size_t(*at)].item_id, int8_t(a) };
				++stats_.picks_from_flow;
			}
			r_session.set_data(kUiCustomizationCompPartition, kUiCustomizationAccessoryKeys[a], std::string());
		}
	}
	const std::string kit = std::to_string(chosen.value_or(p_spawn_kit));
	r_session.set_data(kUiKitCompPartition, kUiKitSelectedKitKey, kit);
	kit_written_ = kit;
	r_session.set_data(kUiSpawnPointCompPartition, kUiSpawnPointCanSpawnKey, flow_bool(p_model.spawn_points.can_spawn));
	r_session.set_data(
			kUiSpawnPointCompPartition, kUiSpawnPointHasValidSpawnKey, flow_bool(p_model.spawn_points.has_valid_spawn));
	r_session.set_data(kUiSpawnLogicCompPartition, kUiSpawnLogicTimeKey,
			std::to_string(bf3::core::ui::hud_spawn_timer_seconds(p_model.spawn_logic.time_to_respawn)));
	r_session.set_data(kUiTeamCompPartition, kUiTeamScoreboardEnabledKey, flow_bool(p_scoreboard_held));
	return chosen;
}

bool DeployScreens::take_action(const bf3::core::ui::UiFlowAction &p_action) {
	if (p_action.key == bf3::core::ui::kPostProcessEffectActionKey) {
		if (const std::optional<float> p = bf3::core::ui::ui_flow_action_float(p_action)) {
			glitch_ = *p;
		}
		return false;
	}
	if (p_action.key == bf3::core::ui::kSelectSpawnPointActionKey) {
		if (const std::optional<int32_t> point = bf3::core::ui::hud_selected_spawn_point(p_action.params)) {
			spawn_point_ = *point;
			++stats_.spawn_selections;
		}
		return false;
	}
	if (p_action.name == kUiFlowSpawnAction) {
		++stats_.spawn_actions;
		return true;
	}
	return false;
}

void DeployScreens::apply(bf3::core::ui::HudModel &r_model) const {
	r_model.customization.selected_slot = slot_;
	r_model.spawn_points.selected_point = int8_t(spawn_point_);
}

void DeployScreens::frame(Bf3UiScreens &r_screens, Bf3NativeHud *p_hud, godot::Node *p_parent,
		const std::string &p_level, bool p_cursor) {
	Bf3UiPostProcess *post = p_parent != nullptr ? Bf3UiPostProcess::ensure(p_parent, p_level) : nullptr;
	if (glitch_ && post != nullptr) {
		post->trigger_glitch(*glitch_);
		++stats_.glitches;
	}
	glitch_.reset();
	const std::optional<UiScreenPhase> phase = r_screens.phase(kSpawnScreenPcPartition);
	const bool up = phase.has_value() && *phase != UiScreenPhase::Exiting;
	Bf3SwfView *view = up ? r_screens.screen_view(kSpawnScreenPcPartition) : nullptr;
	if (up && !shown_) {
		shown_ = true;
		++stats_.spawn_screens_shown;
		if (post != nullptr) {
			post->dim_in();
			++stats_.dim_ins;
		}
	} else if (!up && shown_) {
		shown_ = false;
		if (post != nullptr) {
			post->dim_out();
			++stats_.dim_outs;
		}
	}
	Bf3OverviewMap *overview = p_parent != nullptr ? Bf3OverviewMap::ensure(p_parent) : nullptr;
	const uint64_t view_id = view != nullptr ? uint64_t(view->get_instance_id()) : 0;
	const uint64_t view_open = view != nullptr ? view->open_serial() : 0;
	if (overview != nullptr && (view_id != overview_view_ || view_open != overview_open_)) {
		if (godot::Object *old = godot::ObjectDB::get_instance(godot::ObjectID(overview_view_))) {
			overview->detach(godot::Object::cast_to<Bf3SwfView>(old));
		}
		overview_view_ = 0;
		overview_open_ = 0;
		if (view != nullptr) {
			if (const bf3::core::Status st = overview->attach(view, kSpawnScreenOverviewPath); st) {
				overview_view_ = view_id;
				overview_open_ = view_open;
				++stats_.overview_attaches;
			} else {
				stats_.last_error = st.error().message;
			}
		}
	}
	if (overview != nullptr && overview_view_ != 0 && p_hud != nullptr && p_hud->tags() != nullptr) {
		overview->set_marks(overview_marks(p_hud->tags()->minimap(), p_hud->downed_marks()));
	}
	const bool cursor = up && p_cursor;
	if (cursor != cursor_ && p_parent != nullptr) {
		if (Bf3UiCursor *c = Bf3UiCursor::ensure(p_parent)) {
			c->set_follow_mouse(cursor);
			if (cursor) {
				c->show_cursor();
			} else {
				c->hide_cursor();
			}
		}
		cursor_ = cursor;
	}
}

}
