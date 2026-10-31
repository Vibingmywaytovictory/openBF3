#include "ui_open_census_check.h"

#include "bf3_localisation.h"
#include "deploy_screens.h"
#include "godot_util.h"
#include "level_session_menu.h"
#include "native_eor.h"
#include "native_hud.h"
#include "native_options.h"
#include "swf_view.h"
#include "ui_assets.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <set>
#include <string_view>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

std::vector<std::string> census_partitions() {
	std::set<std::string> out;
	const auto add = [&out](auto p_list) {
		for (const std::string_view p : p_list) {
			out.emplace(p);
		}
	};
	add(kNativeHudPartitions);
	add(kMenuDrawnPartitions);
	add(ui::kOptionsScreens);
	add(kCustomizeScreenPartitions);
	add(kNativeEorScreens);
	add(std::initializer_list<std::string_view>{ kPopupBindKeyPartition, kPopupDelayPartition, kScreenAdjustPartition,
			kSpawnScreenPcPartition, kSpawnButtonScreenPartition, kSpawnTicketCounterPartition,
			ui::kKillScreenPartition, ui::kScoreboardHud32ScreenPartition, ui::kPreRoundWaitingScreenPartition });
	return std::vector<std::string>(out.begin(), out.end());
}

}

void Bf3UiOpenCensusCheck::_bind_methods() {}

void Bf3UiOpenCensusCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	partitions_ = census_partitions();
	set_process(true);
}

void Bf3UiOpenCensusCheck::add(std::string p_name, bool p_pass, std::string p_detail) {
	rows_.push_back(Row{ std::move(p_name), p_pass, std::move(p_detail) });
}

void Bf3UiOpenCensusCheck::census_screen(const std::string &p_partition) {
	const std::string leaf = ui::ui_data_category_leaf(p_partition);
	const SwfViewCounters before = Bf3SwfView::counters();
	Bf3SwfView *view = memnew(Bf3SwfView);
	view->set_playing(false);
	view->set_target_size(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	add_child(view);
	const bf3::core::Status opened = view->open(ui::hud_screen_movie(leaf));
	if (!opened) {
		add(leaf, false, opened.error().message);
		remove_child(view);
		memdelete(view);
		return;
	}
	view->sync();
	const SwfViewCounters after = Bf3SwfView::counters();
	const SwfViewStats &stats = view->stats();
	synced_ += stats.structure_passes != 0 ? 1u : 0u;
	with_items_ += stats.items != 0 ? 1u : 0u;
	filter_stages_ += after.live_filter_stages - before.live_filter_stages;
	filter_materials_ += after.live_filter_materials - before.live_filter_materials;
	thrown_viewports_ +=
			(after.viewport_creates - before.viewport_creates) - (after.live_surfaces - before.live_surfaces);
	UtilityFunctions::print(to_godot(bf3::core::fmt("[census] %s: %llu viewport create(s), %llu resize(s), %llu live "
													"surface(s), %llu live item(s), %llu live triangle array(s), %u "
													"item(s) of the view, %u filter layer(s), %u mask group(s), %.1f "
													"KiB retained at %dx%d",
			leaf.c_str(), (unsigned long long)(after.viewport_creates - before.viewport_creates),
			(unsigned long long)(after.viewport_resizes - before.viewport_resizes),
			(unsigned long long)(after.live_surfaces - before.live_surfaces),
			(unsigned long long)(after.live_items - before.live_items),
			(unsigned long long)(after.live_triangle_arrays - before.live_triangle_arrays), stats.items,
			stats.filter_layers, stats.mask_groups, double(after.retained_bytes - before.retained_bytes) / 1024.0,
			view->target_size().x, view->target_size().y)));
	remove_child(view);
	memdelete(view);
}

void Bf3UiOpenCensusCheck::census_hidden_screen(const std::string &p_partition) {
	const std::string leaf = ui::ui_data_category_leaf(p_partition);
	Bf3SwfView *view = memnew(Bf3SwfView);
	view->set_playing(false);
	view->set_skips_hidden_render(true);
	view->set_target_size(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	add_child(view);
	view->set_visible(false);
	const bf3::core::Status opened = view->open(ui::hud_screen_movie(leaf));
	if (!opened) {
		add("a hidden screen renders nothing", false, opened.error().message);
		remove_child(view);
		memdelete(view);
		return;
	}
	view->sync();
	const uint32_t hidden = view->stats().viewports_updated;
	view->set_visible(true);
	view->sync();
	const uint32_t shown = view->stats().viewports_updated;
	add("a hidden screen renders nothing", hidden == 0,
			bf3::core::fmt("%s: %u viewport update(s) hidden", leaf.c_str(), hidden));
	add("and renders what it held once shown", shown != 0,
			bf3::core::fmt("%s: %u viewport update(s) shown", leaf.c_str(), shown));
	remove_child(view);
	memdelete(view);
}

void Bf3UiOpenCensusCheck::finish() {
	done_ = true;
	set_process(false);
	census_hidden_screen(std::string(kIngameMenuPartition));
	add("every view synced under the dummy renderer", synced_ == partitions_.size(),
			std::to_string(synced_) + " of " + std::to_string(partitions_.size()));
	add("the views built their items", with_items_ == partitions_.size(),
			std::to_string(with_items_) + " of " + std::to_string(partitions_.size()));
	const SwfViewCounters left = Bf3SwfView::counters();
	add("freed views hold nothing", left.live_surfaces == 0 && left.live_items == 0 && left.live_triangle_arrays == 0,
			bf3::core::fmt("%llu surfaces, %llu items, %llu arrays", (unsigned long long)left.live_surfaces,
					(unsigned long long)left.live_items, (unsigned long long)left.live_triangle_arrays));
	add("filter stages share their materials", filter_stages_ != 0 && filter_materials_ * 2 <= filter_stages_,
			bf3::core::fmt("%llu materials for %llu stages", (unsigned long long)filter_materials_,
					(unsigned long long)filter_stages_));
	add("freed views hold no filter material", left.live_filter_materials == 0 && left.live_filter_stages == 0,
			bf3::core::fmt("%llu materials, %llu stages", (unsigned long long)left.live_filter_materials,
					(unsigned long long)left.live_filter_stages));
	add("no viewport made only to be replaced", thrown_viewports_ == 0,
			bf3::core::fmt("%llu thrown away", (unsigned long long)thrown_viewports_));
	add("no triangle array of few triangles", left.small_triangle_arrays_emitted == 0,
			bf3::core::fmt("%llu emitted", (unsigned long long)left.small_triangle_arrays_emitted));
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || !loc->install_snapshot_at_start().valid) {
		add("install.unchanged", false, "no snapshot at start");
	} else {
		const bf3::core::SnapshotDiff d =
				bf3::core::diff_snapshots(loc->install_snapshot_at_start(), loc->reader().snapshot());
		add("install.unchanged", d.clean(), d.clean() ? "sizes and mtimes equal" : d.summary());
	}
	bool ok = true;
	for (const Row &r : rows_) {
		ok = ok && r.pass;
		UtilityFunctions::print(to_godot(
				bf3::core::fmt("  %-4s %-44s %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	UtilityFunctions::print(ok ? "UI OPEN CENSUS: PASS" : "UI OPEN CENSUS: FAIL");
	get_tree()->quit(ok ? 0 : 1);
}

void Bf3UiOpenCensusCheck::_process(double) {
	if (done_) {
		return;
	}
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || assets->has_failed() || ++waited_ > kUiOpenCensusMaxWaitFrames) {
		add("UI assets", false, assets == nullptr ? "no Bf3UiAssets in the scene" : to_std(assets->get_error()));
		finish();
		return;
	}
	if (!assets->is_loaded()) {
		return;
	}
	if (next_ < partitions_.size()) {
		census_screen(partitions_[next_++]);
		return;
	}
	finish();
}

}
