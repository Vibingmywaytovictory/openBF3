#include "ui_open_census_check.h"

#include "bf3_localisation.h"
#include "deploy_screens.h"
#include "godot_util.h"
#include "level_session_menu.h"
#include "native_eor.h"
#include "native_hud.h"
#include "native_options.h"
#include "native_screens.h"
#include "overview_map.h"
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

#include <algorithm>
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
	ui::SwfLayerWalkOptions walk_options;
	walk_options.fonts = Bf3UiAssets::current()->swf_fonts();
	const ui::SwfLayerWalk walk = ui::swf_layer_walk(*view->scene(), walk_options);
	const auto layers = [&walk](ui::SwfLayerKind p_kind) {
		return uint32_t(std::count_if(walk.layers.begin(), walk.layers.end(),
				[p_kind](const ui::SwfLayerWalkNode &p_l) { return p_l.kind == p_kind; }));
	};
	const uint32_t walk_filters = layers(ui::SwfLayerKind::Filter);
	const uint32_t walk_masks = layers(ui::SwfLayerKind::Mask);
	const uint32_t walk_blends = layers(ui::SwfLayerKind::Blend);
	if (walk_filters != stats.filter_layers || walk_masks != stats.mask_groups || walk_blends != stats.blend_layers ||
			walk.engine_targets != stats.engine_targets) {
		walk_differs_ += bf3::core::fmt(" %s: walk %u/%u/%u/%u, view %u/%u/%u/%u;", leaf.c_str(), walk_filters,
				walk_masks, walk_blends, walk.engine_targets, stats.filter_layers, stats.mask_groups,
				stats.blend_layers, stats.engine_targets);
	}
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

void Bf3UiOpenCensusCheck::census_hidden_screen(const std::string &p_partition, std::string_view p_live_target) {
	const std::string leaf = ui::ui_data_category_leaf(p_partition);
	const std::string movie = ui::hud_screen_movie(leaf);
	Bf3UiScreens *screens = memnew(Bf3UiScreens);
	screens->set_manual(true);
	add_child(screens);
	const auto finish_screens = [this, screens] {
		remove_child(screens);
		memdelete(screens);
	};
	screens->set_drawn(movie, true);
	const bf3::core::Status opened = screens->open_movie_screen(movie, nullptr);
	if (!opened) {
		add("a hidden screen renders nothing", false, opened.error().message);
		finish_screens();
		return;
	}
	screens->hide_screen(movie);
	Bf3SwfView *view = screens->screen_view(movie);
	view->set_process(false);
	view->set_target_size(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	const SwfViewCounters start = Bf3SwfView::counters();
	const auto requests = [&start] {
		return (unsigned long long)(Bf3SwfView::counters().viewport_render_requests - start.viewport_render_requests);
	};
	const auto running = [&start] {
		return (unsigned long long)(Bf3SwfView::counters().live_targets_running - start.live_targets_running);
	};
	view->sync();
	if (!p_live_target.empty()) {
		const bf3::core::Status renderer = view->set_renderer_string(p_live_target, kOverviewMapRenderer);
		view->sync();
		const bf3::core::Status live = view->set_engine_target_live(p_live_target, true);
		if (!renderer || !live) {
			add("a hidden live target stays stopped", false,
					!renderer ? renderer.error().message : live.error().message);
		}
		view->sync();
	}
	const unsigned long long hidden_requests = requests();
	const unsigned long long hidden_running = running();
	const uint32_t hidden_updates = view->stats().viewports_updated;
	add("a hidden screen renders nothing", hidden_requests == 0 && hidden_running == 0 && hidden_updates == 0,
			bf3::core::fmt("%s: %llu render request(s), %llu live target(s) running, %u viewport update(s) hidden",
					leaf.c_str(), hidden_requests, hidden_running, hidden_updates));
	screens->show_screen(movie);
	view->sync();
	const uint32_t shown_updates = view->stats().viewports_updated;
	const unsigned long long shown_requests = requests();
	const unsigned long long shown_running = running();
	add("and renders what it held once shown",
			shown_updates != 0 && shown_requests != 0 && shown_running == (p_live_target.empty() ? 0u : 1u),
			bf3::core::fmt("%s: %llu render request(s), %llu live target(s) running, %u viewport update(s) shown",
					leaf.c_str(), shown_requests, shown_running, shown_updates));
	if (!p_live_target.empty()) {
		screens->hide_screen(movie);
		view->sync();
		const unsigned long long stopped_running = running();
		const unsigned long long stopped_requests = requests() - shown_requests;
		add("a hidden live target stays stopped", stopped_running == 0 && stopped_requests == 0,
				bf3::core::fmt("%s: hidden again, %llu running, %llu render request(s)", leaf.c_str(), stopped_running,
						stopped_requests));
		screens->show_screen(movie);
		view->sync();
		const unsigned long long restarted = running();
		const unsigned long long close_requests = requests();
		screens->close_screen(movie);
		for (uint32_t i = 0; i < kUiOpenCensusCloseSteps && screens->phase(movie).has_value(); ++i) {
			screens->step(kUiOpenCensusCloseStepSeconds);
		}
		add("closing a screen stops its live target",
				!screens->phase(movie).has_value() && restarted == 1 && running() == 0 && !view->is_processing() &&
						requests() == close_requests,
				bf3::core::fmt("%s: %llu running before close, %llu after, %llu render request(s), %s, %s",
						leaf.c_str(), restarted, running(), requests() - close_requests,
						screens->phase(movie).has_value() ? "still open" : "closed",
						view->is_processing() ? "processing" : "idle"));
	}
	finish_screens();
}

void Bf3UiOpenCensusCheck::census_filter_swap(const std::string &p_partition) {
	const std::string leaf = ui::ui_data_category_leaf(p_partition);
	Bf3SwfView *view = memnew(Bf3SwfView);
	view->set_playing(false);
	view->set_target_size(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	add_child(view);
	const bf3::core::Status opened = view->open(ui::hud_screen_movie(leaf));
	if (!opened) {
		add("a swapped filter gets its viewports", false, opened.error().message);
		remove_child(view);
		memdelete(view);
		return;
	}
	view->sync();
	const ui::SwfLayerWalk walk = ui::swf_layer_walk(*view->scene(), ui::SwfLayerWalkOptions{});
	const auto layer = std::find_if(walk.layers.begin(), walk.layers.end(),
			[](const ui::SwfLayerWalkNode &p_l) { return p_l.kind == ui::SwfLayerKind::Filter; });
	if (layer == walk.layers.end()) {
		add("a swapped filter gets its viewports", false, leaf + " has no filter layer");
		remove_child(view);
		memdelete(view);
		return;
	}
	const uint64_t links_before = Bf3SwfView::counters().viewport_links_without_viewport;
	const uint32_t passes_before = view->stats().filter_passes;
	ui::SwfFilter recoloured = layer->filters.front();
	recoloured.color.r = uint8_t(recoloured.color.r ^ 0xff);
	const bf3::core::Status recolour = view->scene()->set_filters(layer->path, { recoloured });
	view->sync();
	const uint32_t after_recolour = view->filter_stages_without_viewport();
	const bf3::core::Status doubled = view->scene()->set_filters(layer->path, { recoloured, layer->filters.front() });
	view->sync();
	const uint32_t after_double = view->filter_stages_without_viewport();
	const uint64_t links = Bf3SwfView::counters().viewport_links_without_viewport - links_before;
	const uint32_t passes = view->stats().filter_passes;
	add("a swapped filter gets its viewports",
			recolour.ok() && doubled.ok() && after_recolour == 0 && after_double == 0 && passes == passes_before + 1,
			bf3::core::fmt("%s %s: %u, then %u stage(s) with none, %u to %u passes", leaf.c_str(), layer->path.c_str(),
					after_recolour, after_double, passes_before, passes));
	add("no engine call on a stage with no viewport", links == 0,
			bf3::core::fmt("%llu link(s)", (unsigned long long)links));
	remove_child(view);
	memdelete(view);
}

void Bf3UiOpenCensusCheck::finish() {
	done_ = true;
	set_process(false);
	census_hidden_screen(std::string(kIngameMenuPartition), {});
	census_hidden_screen(std::string(kSpawnScreenPcPartition), kSpawnScreenOverviewPath);
	census_filter_swap(std::string(kIngameMenuPartition));
	add("every view synced under the dummy renderer", synced_ == partitions_.size(),
			std::to_string(synced_) + " of " + std::to_string(partitions_.size()));
	add("the layer walk counts what the renderer plans", walk_differs_.empty(),
			walk_differs_.empty() ? "filters, masks and blends equal on every screen" : walk_differs_);
	add("the views built their items", with_items_ == partitions_.size(),
			std::to_string(with_items_) + " of " + std::to_string(partitions_.size()));
	const SwfViewCounters left = Bf3SwfView::counters();
	add("freed views hold nothing",
			left.live_surfaces == 0 && left.live_items == 0 && left.live_triangle_arrays == 0 &&
					left.live_targets_running == 0,
			bf3::core::fmt("%llu surfaces, %llu items, %llu arrays, %llu running targets",
					(unsigned long long)left.live_surfaces, (unsigned long long)left.live_items,
					(unsigned long long)left.live_triangle_arrays, (unsigned long long)left.live_targets_running));
	add("filter stages share their materials", filter_stages_ != 0 && filter_materials_ * 2 <= filter_stages_,
			bf3::core::fmt("%llu materials for %llu stages", (unsigned long long)filter_materials_,
					(unsigned long long)filter_stages_));
	add("freed views hold no filter material", left.live_filter_materials == 0 && left.live_filter_stages == 0,
			bf3::core::fmt("%llu materials, %llu stages", (unsigned long long)left.live_filter_materials,
					(unsigned long long)left.live_filter_stages));
	add("no viewport made only to be replaced", thrown_viewports_ == 0,
			bf3::core::fmt("%llu thrown away", (unsigned long long)thrown_viewports_));
	add("no triangle array of few triangles", left.smallest_triangle_array > kUiOpenCensusPrimitiveTriangles,
			bf3::core::fmt("smallest array has %llu triangles", (unsigned long long)left.smallest_triangle_array));
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
