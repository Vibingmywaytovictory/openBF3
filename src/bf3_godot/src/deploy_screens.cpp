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

std::optional<float> flow_power(const std::vector<std::string> &p_params) {
	float power = 0.0f;
	if (p_params.empty() ||
			bf3::core::parse_float_value(p_params.front(), 0.0, std::numeric_limits<float>::max(), power).fault !=
					bf3::core::KeyFault::None) {
		return std::nullopt;
	}
	return power;
}

}

bf3::core::Result<bf3::net::LoadoutRequest> deploy_loadout_request(const DeployPick &p_pick,
		const bf3::host::LoadoutCatalog &p_catalog, const bf3::host::KitTable &p_table, uint8_t p_team,
		bf3::host::LoadoutChoice &r_choice) {
	if (p_pick.slot >= bf3::host::kHudSpawnSlots.size()) {
		return bf3::core::make_error("deploy pick: no slot " + std::to_string(p_pick.slot));
	}
	bf3::host::LoadoutChoice next = r_choice == bf3::host::LoadoutChoice{}
			? bf3::host::default_loadout(p_catalog, p_table, p_team, p_pick.kit).choice
			: r_choice;
	const size_t slot = bf3::host::kHudSpawnSlots[p_pick.slot];
	next.items[slot] = p_pick.item;
	if (slot == bf3::host::kLoadoutAccessoryWeapon) {
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
	for (const std::string_view p :
			{ kSpawnScreenPcPartition, kSpawnButtonScreenPartition, kSpawnTicketCounterPartition }) {
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
		if (const std::optional<float> p = flow_power(p_action.params)) {
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
	if (overview != nullptr && view_id != overview_view_) {
		if (godot::Object *old = godot::ObjectDB::get_instance(godot::ObjectID(overview_view_))) {
			overview->detach(godot::Object::cast_to<Bf3SwfView>(old));
		}
		overview_view_ = 0;
		if (view != nullptr) {
			if (const bf3::core::Status st = overview->attach(view, kSpawnScreenOverviewPath); st) {
				overview_view_ = view_id;
				++stats_.overview_attaches;
			} else {
				stats_.last_error = st.error().message;
			}
		}
	}
	if (overview != nullptr && overview_view_ != 0 && p_hud != nullptr && p_hud->tags() != nullptr) {
		overview->set_marks(overview_marks(p_hud->tags()->minimap()));
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
