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
#include "ui_pages.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <string_view>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

namespace {

std::optional<Rect2> mask_window_rect(const Bf3SwfView &p_view, ui::SwfNodeId p_mask) {
	const std::optional<ui::SwfRectMask> mask =
			ui::swf_rect_mask(*p_view.scene(), Bf3UiAssets::current()->swf_fonts(), p_mask);
	if (!mask) {
		return std::nullopt;
	}
	const SwfStageFit fit = p_view.stage_fit();
	const double x0 = std::round(double(mask->stage.x_min) * fit.scale_x + fit.offset_x);
	const double y0 = std::round(double(mask->stage.y_min) * fit.scale_y + fit.offset_y);
	const double x1 = std::round(double(mask->stage.x_max) * fit.scale_x + fit.offset_x);
	const double y1 = std::round(double(mask->stage.y_max) * fit.scale_y + fit.offset_y);
	return Rect2(real_t(x0), real_t(y0), real_t(x1 - x0), real_t(y1 - y0));
}

uint64_t planned_surfaces(const ui::SwfLayerPlan &p_plan) {
	uint64_t out = 0;
	for (const ui::SwfPlannedLayer &l : p_plan.layers) {
		if (l.klass() == ui::SwfLayerClass::OwnSurface) {
			out += l.kind == ui::SwfLayerKind::Mask ? 2u : 1u + l.filters;
		}
	}
	return out;
}

std::string rect_text(const std::optional<Rect2> &p_rect) {
	return p_rect ? bf3::core::fmt("%.0f,%.0f %.0fx%.0f", double(p_rect->position.x), double(p_rect->position.y),
							double(p_rect->size.x), double(p_rect->size.y))
				  : std::string("none");
}

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
	filter_kernels_ += ui::swf_layer_plan(walk, view->target_size().x, view->target_size().y).counts.distinct_kernels;
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
	ui::SwfLayerWalkOptions walk_options;
	walk_options.fonts = Bf3UiAssets::current()->swf_fonts();
	const ui::SwfLayerWalk walk = ui::swf_layer_walk(*view->scene(), walk_options);
	const auto layer = std::find_if(walk.layers.begin(), walk.layers.end(),
			[](const ui::SwfLayerWalkNode &p_l) { return p_l.kind == ui::SwfLayerKind::Filter && p_l.has_nodes; });
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

void Bf3UiOpenCensusCheck::census_paged_screens() {
	Bf3UiPages *pages = memnew(Bf3UiPages);
	add_child(pages);
	uint32_t page_only = 0;
	uint32_t paged_total = 0;
	uint32_t fills_total = 0;
	uint32_t worst_passes = 0;
	std::string root_only_differs;
	std::string surfaces_differ;
	std::string slots_differ;
	std::string still_pending;
	std::string redraws;
	std::string clips_differ;
	std::string clip_rects_differ;
	uint32_t clips_total = 0;
	uint32_t pages_in_clips = 0;
	uint64_t slots_after = 0;
	uint64_t creates_total = 0;
	uint32_t frame_views = 0;
	uint32_t off_frame = 0;
	uint64_t own_surfaces_total = 0;
	std::string frame_redraws;
	for (const std::string &partition : partitions_) {
		const std::string leaf = ui::ui_data_category_leaf(partition);
		const SwfViewCounters before = Bf3SwfView::counters();
		const UiPagesCounters pages_before = pages->counters();
		Bf3SwfView *view = memnew(Bf3SwfView);
		view->set_playing(false);
		view->set_process(false);
		view->set_frame_order(0);
		add_child(view);
		if (const bf3::core::Status opened = view->open(ui::hud_screen_movie(leaf)); !opened) {
			add("a paged screen opens", false, opened.error().message);
			remove_child(view);
			memdelete(view);
			continue;
		}
		view->sync();
		const SwfViewCounters synced = Bf3SwfView::counters();
		ui::SwfLayerWalkOptions walk_options;
		walk_options.fonts = Bf3UiAssets::current()->swf_fonts();
		const ui::SwfLayerWalk walk = ui::swf_layer_walk(*view->scene(), walk_options);
		const ui::SwfLayerPlan plan = ui::swf_layer_plan(walk, view->target_size().x, view->target_size().y);
		const uint64_t own_surfaces = planned_surfaces(plan);
		uint32_t clips = 0;
		for (size_t i = 0; i < plan.layers.size(); ++i) {
			if (walk.layers[i].route == ui::SwfLayerClass::Clip) {
				++clips;
				const std::optional<Rect2> rect = view->clip_rect(walk.layers[i].path);
				const std::optional<Rect2> want = mask_window_rect(*view, walk.layers[i].owner);
				if (rect != want) {
					clip_rects_differ += bf3::core::fmt(" %s %s: %s, mask %s;", leaf.c_str(),
							walk.layers[i].path.c_str(), rect_text(rect).c_str(), rect_text(want).c_str());
				}
				pages_in_clips += uint32_t(std::count_if(walk.layers[i].children.begin(), walk.layers[i].children.end(),
						[&plan](uint32_t p_child) { return plan.layers[p_child].klass() == ui::SwfLayerClass::Page; }));
			}
		}
		if (view->clip_groups() != clips || view->clips_misplaced() != 0) {
			clips_differ += bf3::core::fmt(" %s: %u clips, %u misplaced, plan %u;", leaf.c_str(), view->clip_groups(),
					view->clips_misplaced(), clips);
		}
		clips_total += clips;
		const uint64_t creates = synced.viewport_creates - before.viewport_creates;
		creates_total += creates;
		const uint64_t surfaces = synced.live_surfaces - before.live_surfaces;
		UtilityFunctions::print(to_godot(bf3::core::fmt("[census pages] %s: %llu surface(s), %llu viewport create(s), "
														"%u paged, %u clip(s) at %dx%d",
				leaf.c_str(), (unsigned long long)surfaces, (unsigned long long)creates, view->paged_layers(), clips,
				view->target_size().x, view->target_size().y)));
		if (surfaces != own_surfaces) {
			surfaces_differ += bf3::core::fmt(" %s: %llu live, plan %llu;", leaf.c_str(), (unsigned long long)surfaces,
					(unsigned long long)own_surfaces);
		}
		frame_views += view->draws_in_frame() && !view->presents_own_root() ? 1u : 0u;
		off_frame += view->surfaces_off_frame();
		own_surfaces_total += own_surfaces;
		if (plan.counts.pages != 0 && own_surfaces == 0) {
			++page_only;
			if (creates != 0 || synced.viewport_resizes - before.viewport_resizes != 0) {
				root_only_differs +=
						bf3::core::fmt(" %s: %llu creates, %llu resizes;", leaf.c_str(), (unsigned long long)creates,
								(unsigned long long)(synced.viewport_resizes - before.viewport_resizes));
			}
		}
		const uint64_t slots = pages->counters().live_slots - pages_before.live_slots;
		if (slots != plan.counts.pages || view->paged_layers() != plan.counts.pages) {
			slots_differ += bf3::core::fmt(" %s: %llu slots, %u paged, plan %u;", leaf.c_str(),
					(unsigned long long)slots, view->paged_layers(), plan.counts.pages);
		}
		paged_total += view->paged_layers();
		uint32_t passes = 0;
		uint32_t fills = view->stats().page_fills;
		for (uint32_t i = 0; i < kUiOpenCensusMaxPasses && view->page_fills_pending(); ++i) {
			pages->end_pass();
			view->sync();
			fills += view->stats().page_fills;
			++passes;
		}
		worst_passes = std::max(worst_passes, passes);
		fills_total += fills;
		if (view->page_fills_pending()) {
			still_pending += " " + leaf;
		}
		pages->end_pass();
		view->sync();
		const uint64_t settled_passes = pages->counters().passes;
		const uint64_t settled_frames = pages->counters().frame_updates;
		pages->end_pass();
		view->sync();
		pages->end_pass();
		view->sync();
		if (pages->counters().passes != settled_passes) {
			redraws += " " + leaf;
		}
		if (pages->counters().frame_updates != settled_frames) {
			frame_redraws += " " + leaf;
		}
		remove_child(view);
		memdelete(view);
		slots_after = std::max(slots_after, pages->counters().live_slots);
	}
	add("every rect mask is a clip with no surface", clips_total != 0 && pages_in_clips != 0 && clips_differ.empty(),
			clips_differ.empty()
					? bf3::core::fmt("%u clips, %u paged layers directly inside one", clips_total, pages_in_clips)
					: clips_differ);
	add("a clip's rect is its mask's window rect", clip_rects_differ.empty(),
			clip_rects_differ.empty() ? "equal on every clip" : clip_rects_differ);
	add("a page-only screen makes no viewport", page_only != 0 && root_only_differs.empty(),
			root_only_differs.empty() ? bf3::core::fmt("%u screens, %llu viewports over all 30", page_only,
												(unsigned long long)creates_total)
									  : root_only_differs);
	add("every game UI view draws in F, with no present of its own", frame_views == partitions_.size(),
			bf3::core::fmt("%u of %zu", frame_views, partitions_.size()));
	add("own surfaces link to F", off_frame == 0 && own_surfaces_total != 0,
			bf3::core::fmt("%u of %llu off F", off_frame, (unsigned long long)own_surfaces_total));
	add("a settled screen asks F for no update", frame_redraws.empty(),
			frame_redraws.empty() ? "no update on later syncs" : frame_redraws);
	add("no surface for an empty layer", surfaces_differ.empty(),
			surfaces_differ.empty() ? "surfaces equal the plan on every screen" : surfaces_differ);
	add("the renderer pages what the plan pages", paged_total != 0 && slots_differ.empty(),
			slots_differ.empty() ? bf3::core::fmt("%u paged layers", paged_total) : slots_differ);
	add("every layer fills within the passes", still_pending.empty() && fills_total >= paged_total,
			still_pending.empty() ? bf3::core::fmt("%u fills, worst %u pass(es)", fills_total, worst_passes)
								  : still_pending);
	add("a settled screen asks for no pass", redraws.empty(), redraws.empty() ? "no pass on later syncs" : redraws);
	add("a freed view gives its slots back", slots_after == 0,
			bf3::core::fmt("%llu slots left", (unsigned long long)slots_after));
	census_page_life(*pages);
	census_hidden_pages(*pages);
	census_page_overflow(*pages);
	census_clips(*pages);
	census_frame(*pages);
	census_undrawn_pass(*pages);
	census_screen_band(*pages);
	remove_child(pages);
	memdelete(pages);
}

Bf3SwfView *Bf3UiOpenCensusCheck::open_paged_view(const std::string &p_leaf) {
	Bf3SwfView *view = memnew(Bf3SwfView);
	view->set_playing(false);
	view->set_process(false);
	view->set_frame_order(0);
	view->set_skips_hidden_render(true);
	add_child(view);
	if (const bf3::core::Status opened = view->open(ui::hud_screen_movie(p_leaf)); !opened) {
		add("a paged screen opens", false, opened.error().message);
		remove_child(view);
		memdelete(view);
		return nullptr;
	}
	return view;
}

void Bf3UiOpenCensusCheck::settle_paged_view(Bf3UiPages &r_pages, Bf3SwfView &r_view) {
	for (uint32_t i = 0; i < kUiOpenCensusMaxPasses && r_view.page_fills_pending(); ++i) {
		r_pages.end_pass();
		r_view.sync();
	}
}

void Bf3UiOpenCensusCheck::census_page_life(Bf3UiPages &r_pages) {
	Bf3SwfView *view = open_paged_view(ui::ui_data_category_leaf(kIngameMenuPartition));
	if (view == nullptr) {
		return;
	}
	const auto passes = [&r_pages] { return r_pages.counters().passes; };
	view->sync();
	const uint32_t paged = view->paged_layers();
	add("a new layer's quad waits for its first write",
			paged != 0 && view->paged_quads_shown() == 0 && view->page_fills_pending(),
			bf3::core::fmt("%u paged, %u shown after the fill frame", paged, view->paged_quads_shown()));
	settle_paged_view(r_pages, *view);
	add("and shows on the frame after it", view->paged_quads_shown() == paged,
			bf3::core::fmt("%u of %u shown", view->paged_quads_shown(), paged));
	ui::SwfLayerWalkOptions walk_options;
	walk_options.fonts = Bf3UiAssets::current()->swf_fonts();
	const ui::SwfLayerWalk walk = ui::swf_layer_walk(*view->scene(), walk_options);
	const auto layer = std::find_if(walk.layers.begin(), walk.layers.end(), [&view](const ui::SwfLayerWalkNode &p_l) {
		const ui::SwfSceneNode *n = view->scene()->node(p_l.owner);
		return p_l.route == ui::SwfLayerClass::Page && p_l.has_nodes && n != nullptr &&
				n->kind == ui::SwfNodeKind::EditText;
	});
	if (layer == walk.layers.end()) {
		add("a blinking layer keeps its texels", false, "no paged text layer");
		remove_child(view);
		memdelete(view);
		return;
	}
	const uint64_t before_blink = passes();
	const bf3::core::Status hide = view->scene()->set_visible(layer->path, false);
	view->sync();
	settle_paged_view(r_pages, *view);
	const uint32_t shown_hidden = view->paged_quads_shown();
	const bf3::core::Status show = view->scene()->set_visible(layer->path, true);
	view->sync();
	settle_paged_view(r_pages, *view);
	add("a blinking layer keeps its texels",
			hide.ok() && show.ok() && passes() == before_blink && shown_hidden + 1 == paged &&
					view->paged_quads_shown() == paged,
			bf3::core::fmt("%s: %llu pass(es), %u then %u of %u shown", layer->path.c_str(),
					(unsigned long long)(passes() - before_blink), shown_hidden, view->paged_quads_shown(), paged));
	const auto moved_by = [&](double p_window_pixels) {
		ui::SwfMatrix m = view->scene()->node(layer->owner)->matrix;
		const uint64_t before = passes();
		m.tx += float(p_window_pixels / view->stage_fit().scale_x);
		const bf3::core::Status st = view->scene()->set_matrix(layer->path, m);
		view->sync();
		const uint32_t moves = view->stats().page_moves;
		settle_paged_view(r_pages, *view);
		return st.ok() ? std::pair<uint64_t, uint32_t>(passes() - before, moves)
					   : std::pair<uint64_t, uint32_t>(UINT64_MAX, 0);
	};
	const auto whole = moved_by(kUiOpenCensusWholeMove);
	add("a whole-pixel move only moves the quad", whole.first == 0 && whole.second == 1,
			bf3::core::fmt("%llu pass(es), %u quad move(s)", (unsigned long long)whole.first, whole.second));
	const auto fractional = moved_by(kUiOpenCensusFractionalMove);
	add("a fractional move takes a pass", fractional.first == 1 && fractional.second == 0,
			bf3::core::fmt("%llu pass(es), %u quad move(s)", (unsigned long long)fractional.first, fractional.second));
	const uint64_t before_text = passes();
	const bf3::core::Status text = view->scene()->set_text(layer->path, kUiOpenCensusLongText);
	view->sync();
	settle_paged_view(r_pages, *view);
	add("a changed layer fills again", text.ok() && passes() == before_text + 1,
			bf3::core::fmt("%llu pass(es)", (unsigned long long)(passes() - before_text)));
	const uint64_t arrays_filled = Bf3SwfView::counters().live_triangle_arrays;
	for (uint32_t i = 0; i < kUiOpenCensusIdlePasses; ++i) {
		r_pages.end_pass();
		view->sync();
	}
	const uint64_t arrays_idle = Bf3SwfView::counters().live_triangle_arrays;
	add("an idle paged text gives its arrays back", arrays_idle < arrays_filled,
			bf3::core::fmt("%llu arrays after its fill, %llu idle", (unsigned long long)arrays_filled,
					(unsigned long long)arrays_idle));
	const uint64_t before_refill = passes();
	const auto refill = moved_by(kUiOpenCensusFractionalMove);
	const uint64_t arrays_after = Bf3SwfView::counters().live_triangle_arrays;
	add("and takes them again for its next pass", refill.first == 1 && arrays_after == arrays_idle + 1,
			bf3::core::fmt("%llu pass(es), %llu arrays", (unsigned long long)(passes() - before_refill),
					(unsigned long long)arrays_after));
	remove_child(view);
	memdelete(view);
}

void Bf3UiOpenCensusCheck::census_hidden_pages(Bf3UiPages &r_pages) {
	const std::string movie = ui::hud_screen_movie(ui::ui_data_category_leaf(kIngameMenuPartition));
	Bf3UiScreens *screens = memnew(Bf3UiScreens);
	screens->set_manual(true);
	screens->set_frame_band(true);
	add_child(screens);
	screens->set_drawn(movie, true);
	if (const bf3::core::Status opened = screens->open_movie_screen(movie, nullptr); !opened) {
		add("a hidden screen takes no slots", false, opened.error().message);
		remove_child(screens);
		memdelete(screens);
		return;
	}
	screens->hide_screen(movie);
	Bf3SwfView *view = screens->screen_view(movie);
	view->set_process(false);
	const uint64_t passes_start = r_pages.counters().passes;
	view->sync();
	view->sync();
	const uint64_t hidden_slots = r_pages.counters().live_slots;
	const uint64_t hidden_passes = r_pages.counters().passes - passes_start;
	add("a hidden screen takes no slots", hidden_slots == 0 && hidden_passes == 0,
			bf3::core::fmt(
					"%llu slots, %llu passes", (unsigned long long)hidden_slots, (unsigned long long)hidden_passes));
	screens->show_screen(movie);
	view->sync();
	const uint64_t shown_slots = r_pages.counters().live_slots;
	settle_paged_view(r_pages, *view);
	const uint64_t shown_passes = r_pages.counters().passes - passes_start;
	add("and fills once shown",
			shown_slots != 0 && shown_slots == view->paged_layers() && shown_passes != 0 &&
					view->paged_quads_shown() == view->paged_layers(),
			bf3::core::fmt("%llu slots, %llu passes, %u of %u shown", (unsigned long long)shown_slots,
					(unsigned long long)shown_passes, view->paged_quads_shown(), view->paged_layers()));
	screens->hide_screen(movie);
	view->sync();
	screens->show_screen(movie);
	view->sync();
	settle_paged_view(r_pages, *view);
	add("hiding and showing again costs no pass",
			r_pages.counters().live_slots == shown_slots && r_pages.counters().passes - passes_start == shown_passes,
			bf3::core::fmt("%llu slots, %llu more passes", (unsigned long long)r_pages.counters().live_slots,
					(unsigned long long)(r_pages.counters().passes - passes_start - shown_passes)));
	const uint64_t resizes_open = Bf3SwfView::counters().viewport_resizes;
	const bool shown_at_close = view->shown_in_frame();
	UiPagesClock clock{ kUiOpenCensusDrivenClock, kUiOpenCensusDrivenClock };
	uint64_t last_step_updates = 0;
	screens->close_screen(movie);
	for (uint32_t i = 0; i < kUiOpenCensusCloseSteps && screens->phase(movie).has_value(); ++i) {
		r_pages.drive_clock(clock);
		r_pages.mark_frame();
		++clock.process_frames;
		++clock.draws;
		r_pages.drive_clock(clock);
		const uint64_t before_step = r_pages.counters().frame_updates;
		screens->step(kUiOpenCensusCloseStepSeconds);
		last_step_updates = r_pages.counters().frame_updates - before_step;
	}
	r_pages.drive_clock(std::nullopt);
	add("a closing screen redraws F in the step that hides it",
			shown_at_close && !view->shown_in_frame() && last_step_updates == 1,
			bf3::core::fmt("%s at close, %s after, %llu update(s) in the last step",
					shown_at_close ? "shown" : "not shown", view->shown_in_frame() ? "shown" : "not shown",
					(unsigned long long)last_step_updates));
	add("a closed screen holds no slots", !screens->phase(movie).has_value() && r_pages.counters().live_slots == 0,
			bf3::core::fmt("%llu slots, %s", (unsigned long long)r_pages.counters().live_slots,
					screens->phase(movie).has_value() ? "still open" : "closed"));
	const uint64_t resizes_closed = Bf3SwfView::counters().viewport_resizes - resizes_open;
	add("and leaves F, whose size it does not touch", resizes_closed == 0 && !view->shown_in_frame(),
			bf3::core::fmt("%llu resize(s), %s", (unsigned long long)resizes_closed,
					view->shown_in_frame() ? "still shown" : "not shown"));
	remove_child(screens);
	memdelete(screens);
}

void Bf3UiOpenCensusCheck::census_page_overflow(Bf3UiPages &r_pages) {
	std::vector<Bf3SwfView *> views;
	const auto sync_all = [&views] {
		for (Bf3SwfView *v : views) {
			v->sync();
		}
	};
	const auto pending = [&views] {
		return std::any_of(views.begin(), views.end(), [](const Bf3SwfView *p_v) { return p_v->page_fills_pending(); });
	};
	const auto settle_all = [&] {
		for (uint32_t i = 0; i < kUiOpenCensusMaxPasses && pending(); ++i) {
			r_pages.end_pass();
			sync_all();
		}
	};
	const auto stages_without = [&views] {
		uint32_t without = 0;
		for (const Bf3SwfView *v : views) {
			without += v->filter_stages_without_viewport();
		}
		return without;
	};
	for (const std::string &partition : partitions_) {
		if (Bf3SwfView *v = open_paged_view(ui::ui_data_category_leaf(partition))) {
			views.push_back(v);
		}
	}
	sync_all();
	settle_all();
	ui::SwfPageSizes small_scratch;
	small_scratch.result_width = 2048;
	small_scratch.result_height = 1024;
	small_scratch.scratch_width = kUiOpenCensusSmallPage;
	small_scratch.scratch_height = kUiOpenCensusSmallPage / 2;
	const UiPagesCounters before_small = r_pages.counters();
	r_pages.use_sizes(small_scratch);
	sync_all();
	settle_all();
	const UiPagesCounters after_small = r_pages.counters();
	add("remade pages refill every view, over several passes",
			after_small.passes - before_small.passes > 1 && !pending() && stages_without() == 0,
			bf3::core::fmt("%llu pass(es) for %zu views with S %dx%d",
					(unsigned long long)(after_small.passes - before_small.passes), views.size(),
					small_scratch.scratch_width, small_scratch.scratch_height));
	add("a layer over S falls back to its own surface",
			after_small.own_surface_fallbacks > before_small.own_surface_fallbacks,
			bf3::core::fmt("%llu fallback(s)",
					(unsigned long long)(after_small.own_surface_fallbacks - before_small.own_surface_fallbacks)));
	ui::SwfPageSizes small_result;
	small_result.result_width = kUiOpenCensusSmallPage;
	small_result.result_height = kUiOpenCensusSmallPage / 2;
	small_result.scratch_width = 1024;
	small_result.scratch_height = 512;
	r_pages.use_sizes(small_result);
	sync_all();
	settle_all();
	const UiPagesCounters after_full = r_pages.counters();
	add("a full R falls back to own surfaces",
			after_full.own_surface_fallbacks > after_small.own_surface_fallbacks && stages_without() == 0 && !pending(),
			bf3::core::fmt("%llu fallback(s), %llu px of %d used",
					(unsigned long long)(after_full.own_surface_fallbacks - after_small.own_surface_fallbacks),
					(unsigned long long)after_full.used_px, small_result.result_width * small_result.result_height));
	for (Bf3SwfView *v : views) {
		remove_child(v);
		memdelete(v);
	}
	add("and every slot comes back", r_pages.counters().live_slots == 0,
			bf3::core::fmt("%llu slots left", (unsigned long long)r_pages.counters().live_slots));
}

void Bf3UiOpenCensusCheck::census_clips(Bf3UiPages &r_pages) {
	r_pages.use_sizes(ui::swf_page_sizes(kUiOpenCensusWidth, kUiOpenCensusHeight));
	Bf3SwfView *view = nullptr;
	std::string leaf;
	std::string mask;
	std::string child;
	const auto walk_view = [](const Bf3SwfView &p_view, std::vector<std::string> p_tilted = {}) {
		ui::SwfLayerWalkOptions options;
		options.fonts = Bf3UiAssets::current()->swf_fonts();
		options.tilted_paths = std::move(p_tilted);
		return ui::swf_layer_walk(*p_view.scene(), options);
	};
	const auto planned = [&walk_view](const Bf3SwfView &p_view, std::vector<std::string> p_tilted = {}) {
		return planned_surfaces(ui::swf_layer_plan(
				walk_view(p_view, std::move(p_tilted)), p_view.target_size().x, p_view.target_size().y));
	};
	for (const std::string &partition : partitions_) {
		leaf = ui::ui_data_category_leaf(partition);
		view = open_paged_view(leaf);
		if (view == nullptr) {
			continue;
		}
		view->sync();
		const ui::SwfLayerWalk walk = walk_view(*view);
		const ui::SwfLayerPlan plan = ui::swf_layer_plan(walk, view->target_size().x, view->target_size().y);
		for (size_t i = 0; i < plan.layers.size() && mask.empty(); ++i) {
			for (const uint32_t c : walk.layers[i].children) {
				if (plan.layers[i].klass() == ui::SwfLayerClass::Clip &&
						plan.layers[c].klass() == ui::SwfLayerClass::Page) {
					mask = walk.layers[i].path;
					child = walk.layers[c].path;
				}
			}
		}
		if (!mask.empty()) {
			break;
		}
		remove_child(view);
		memdelete(view);
		view = nullptr;
	}
	if (view == nullptr) {
		add("a clip keeps its rect through a reopen", false, "no screen has a page inside a clip");
		return;
	}
	const auto owner = [&view, &mask] { return view->scene()->find(mask); };
	const auto rect_is_mask = [&] { return view->clip_rect(mask) == mask_window_rect(*view, owner()); };
	settle_paged_view(r_pages, *view);
	const uint32_t paged = view->paged_layers();
	const uint32_t clips = view->clip_groups();
	const std::optional<Rect2> first = view->clip_rect(mask);
	const uint64_t creates_open = Bf3SwfView::counters().viewport_creates;
	const uint64_t live_items = Bf3SwfView::counters().live_items;
	const bf3::core::Status reopened = view->open(ui::hud_screen_movie(leaf));
	view->sync();
	settle_paged_view(r_pages, *view);
	add("a clip keeps its rect through a reopen",
			reopened.ok() && view->clip_rect(mask) == first && first.has_value() && view->clips_misplaced() == 0 &&
					view->clip_groups() == clips && view->paged_layers() == paged &&
					Bf3SwfView::counters().viewport_creates == creates_open &&
					Bf3SwfView::counters().live_items <= live_items,
			bf3::core::fmt("%s %s: %s then %s, %llu create(s), %llu items then %llu", leaf.c_str(), mask.c_str(),
					rect_text(first).c_str(), rect_text(view->clip_rect(mask)).c_str(),
					(unsigned long long)(Bf3SwfView::counters().viewport_creates - creates_open),
					(unsigned long long)live_items, (unsigned long long)Bf3SwfView::counters().live_items));
	const ui::SwfMatrix placed = view->scene()->node(owner())->matrix;
	ui::SwfMatrix moved = placed;
	moved.tx += float(kUiOpenCensusWholeMove / view->stage_fit().scale_x);
	const uint64_t creates_move = Bf3SwfView::counters().viewport_creates;
	const bf3::core::Status move = view->scene()->set_matrix(mask, moved);
	view->sync();
	add("a moved mask moves its clip",
			move.ok() && rect_is_mask() && view->clip_rect(mask) != first &&
					Bf3SwfView::counters().viewport_creates == creates_move,
			bf3::core::fmt("%s then %s, mask %s", rect_text(first).c_str(), rect_text(view->clip_rect(mask)).c_str(),
					rect_text(mask_window_rect(*view, owner())).c_str()));
	const bf3::core::Status moved_back = view->scene()->set_matrix(mask, placed);
	const bf3::core::Status faded = view->scene()->set_alpha(mask, 0.0f);
	view->sync();
	add("a mask at alpha 0 still clips", moved_back.ok() && faded.ok() && view->clip_rect(mask) == first,
			rect_text(view->clip_rect(mask)));
	const auto leaves_clip = [&](const std::string &p_name,
									 const std::function<bf3::core::Status(Bf3SwfView &)> &p_change,
									 const std::function<bf3::core::Status(Bf3SwfView &)> &p_restore,
									 std::vector<std::string> p_tilted) {
		Bf3SwfView *other = open_paged_view(leaf);
		if (other == nullptr) {
			return;
		}
		other->sync();
		settle_paged_view(r_pages, *other);
		const uint32_t other_paged = other->paged_layers();
		const uint32_t other_clips = other->clip_groups();
		const std::optional<Rect2> other_rect = other->clip_rect(mask);
		const uint64_t surfaces_clipped = Bf3SwfView::counters().live_surfaces;
		const uint64_t planned_clipped = planned(*other);
		const bf3::core::Status changed = p_change(*other);
		other->sync();
		const uint64_t surfaces_after = Bf3SwfView::counters().live_surfaces;
		const uint64_t planned_after = planned(*other, std::move(p_tilted));
		const uint32_t clips_after = other->clip_groups();
		add(p_name,
				changed.ok() && clips_after + 1 == other_clips && planned_after >= planned_clipped + 2 &&
						surfaces_after - surfaces_clipped == planned_after - planned_clipped,
				bf3::core::fmt("%u of %u clips, %llu more surface(s), plan %llu more", clips_after, other_clips,
						(unsigned long long)(surfaces_after - surfaces_clipped),
						(unsigned long long)(planned_after - planned_clipped)));
		const bf3::core::Status restored = p_restore(*other);
		other->sync();
		settle_paged_view(r_pages, *other);
		add("and its clip returns with no item out of place",
				restored.ok() && other->clip_groups() == other_clips && other->clips_misplaced() == 0 &&
						other->clip_rect(mask) == other_rect && other->paged_layers() == other_paged,
				bf3::core::fmt("%u of %u clips, %u misplaced, %s", other->clip_groups(), other_clips,
						other->clips_misplaced(), rect_text(other->clip_rect(mask)).c_str()));
		remove_child(other);
		memdelete(other);
	};
	leaves_clip(
			"a tilted mask takes its surfaces, and its pages their own",
			[&mask](Bf3SwfView &p_v) { return p_v.set_tilt(mask, SwfTilt{ kUiOpenCensusTiltDegrees, 0.0f, 0.0f }); },
			[&mask](Bf3SwfView &p_v) { return p_v.clear_tilt(mask); }, { mask });
	ui::SwfMatrix turned = placed;
	turned.b = kUiOpenCensusMaskTurn;
	turned.c = -kUiOpenCensusMaskTurn;
	leaves_clip(
			"a turned mask leaves its clip in the same sync",
			[&mask, &turned](Bf3SwfView &p_v) { return p_v.scene()->set_matrix(mask, turned); },
			[&mask, &placed](Bf3SwfView &p_v) { return p_v.scene()->set_matrix(mask, placed); }, {});
	const bf3::core::Status renderer = view->set_renderer_string(child, kOverviewMapRenderer);
	view->sync();
	add("an engine target in a clip links to F",
			renderer.ok() && view->stats().engine_targets == 1 && view->engine_targets_unlinked() == 0 &&
					view->surfaces_off_frame() == 0 && view->clips_misplaced() == 0 && view->clip_groups() == clips,
			bf3::core::fmt("%s: %u target(s), %u unlinked, %u off F, %u misplaced", child.c_str(),
					view->stats().engine_targets, view->engine_targets_unlinked(), view->surfaces_off_frame(),
					view->clips_misplaced()));
	remove_child(view);
	memdelete(view);
}

void Bf3UiOpenCensusCheck::census_frame(Bf3UiPages &r_pages) {
	Node2D *present = memnew(Node2D);
	add_child(present);
	r_pages.attach_present(present);
	UiPagesClock clock{ kUiOpenCensusDrivenClock, kUiOpenCensusDrivenClock };
	r_pages.drive_clock(clock);
	const auto marked_then_drawn = [&] {
		r_pages.mark_frame();
		++clock.process_frames;
		++clock.draws;
		r_pages.drive_clock(clock);
	};
	Bf3SwfView *first = open_paged_view(ui::ui_data_category_leaf(kIngameMenuPartition));
	Bf3SwfView *second = open_paged_view(ui::ui_data_category_leaf(kSpawnButtonScreenPartition));
	const auto finish = [&] {
		for (Bf3SwfView *v : { first, second }) {
			if (v != nullptr) {
				remove_child(v);
				memdelete(v);
			}
		}
		r_pages.attach_present(nullptr);
		r_pages.drive_clock(std::nullopt);
		remove_child(present);
		memdelete(present);
	};
	if (first == nullptr || second == nullptr) {
		add("two views draw in F", false, "a screen did not open");
		finish();
		return;
	}
	second->set_frame_order(1);
	const uint64_t creates = Bf3SwfView::counters().viewport_creates;
	const auto sync_both = [&] {
		first->sync();
		second->sync();
	};
	const auto updates = [&r_pages] { return r_pages.counters().frame_updates; };
	const auto settle_both = [&] {
		for (uint32_t i = 0;
				i < kUiOpenCensusMaxPasses && (first->page_fills_pending() || second->page_fills_pending()); ++i) {
			r_pages.end_pass();
			sync_both();
		}
	};
	const uint64_t presents = r_pages.counters().frame_presents;
	sync_both();
	add("a view stays out of F until its first fills are done",
			first->paged_layers() != 0 && first->page_fills_pending() && !first->shown_in_frame(),
			bf3::core::fmt("%u paged, %s", first->paged_layers(), first->shown_in_frame() ? "shown" : "not shown"));
	settle_both();
	r_pages.end_pass();
	sync_both();
	add("and enters it once they are", first->shown_in_frame() && second->shown_in_frame(),
			bf3::core::fmt("%s, %s", first->shown_in_frame() ? "shown" : "not shown",
					second->shown_in_frame() ? "shown" : "not shown"));
	add("two views share F and its one present",
			first->draws_in_frame() && second->draws_in_frame() && !first->presents_own_root() &&
					!second->presents_own_root() && r_pages.counters().frame_presents == presents + 1 &&
					r_pages.presented_at() == first->present_rect() &&
					Bf3SwfView::counters().viewport_creates == creates,
			bf3::core::fmt("%llu present draw(s), %llu more viewports",
					(unsigned long long)(r_pages.counters().frame_presents - presents),
					(unsigned long long)(Bf3SwfView::counters().viewport_creates - creates)));
	ui::SwfLayerWalkOptions walk_options;
	walk_options.fonts = Bf3UiAssets::current()->swf_fonts();
	const ui::SwfLayerWalk walk = ui::swf_layer_walk(*first->scene(), walk_options);
	const auto paged = std::find_if(walk.layers.begin(), walk.layers.end(),
			[](const ui::SwfLayerWalkNode &p_l) { return p_l.route == ui::SwfLayerClass::Page && p_l.has_nodes; });
	if (paged != walk.layers.end()) {
		r_pages.end_pass();
		const uint64_t before_move = updates();
		const uint64_t passes_move = r_pages.counters().passes;
		ui::SwfMatrix m = first->scene()->node(paged->owner)->matrix;
		m.tx += float(kUiOpenCensusWholeMove / first->stage_fit().scale_x);
		const bf3::core::Status moved = first->scene()->set_matrix(paged->path, m);
		first->sync();
		add("a moved layer redraws F and takes no pass",
				moved.ok() && updates() == before_move + 1 && r_pages.counters().passes == passes_move,
				bf3::core::fmt("%s: %llu update(s), %llu pass(es)", paged->path.c_str(),
						(unsigned long long)(updates() - before_move),
						(unsigned long long)(r_pages.counters().passes - passes_move)));
	} else {
		add("a moved layer redraws F and takes no pass", false, "no paged layer");
		finish();
		return;
	}
	add("views share one item material",
			first->item_material().is_valid() && first->item_material() == second->item_material(),
			first->item_material() == second->item_material() ? "one" : "one per view");
	add("views draw in their order", first->frame_draw_index() == 0 && second->frame_draw_index() == 1,
			bf3::core::fmt("%d, %d", first->frame_draw_index(), second->frame_draw_index()));
	marked_then_drawn();
	const uint64_t before_order = updates();
	first->set_frame_order(2);
	add("a new order redraws F", first->frame_draw_index() == 2 && updates() == before_order + 1,
			bf3::core::fmt("index %d, %llu update(s)", first->frame_draw_index(),
					(unsigned long long)(updates() - before_order)));
	first->set_frame_order(0);
	marked_then_drawn();
	const uint64_t before_hide = updates();
	const bf3::core::Status hidden_text = first->scene()->set_text(paged->path, kUiOpenCensusLongText);
	first->set_visible(false);
	const bool hidden_at_once = !first->shown_in_frame();
	const uint64_t hide_updates = updates() - before_hide;
	r_pages.end_pass();
	first->sync();
	add("a hidden view has no fill pending, whatever changed in it", hidden_text.ok() && !first->page_fills_pending(),
			first->page_fills_pending() ? "pending" : "none pending");
	add("hiding a view takes it out of F at once, with no sync", hidden_at_once && hide_updates == 1,
			bf3::core::fmt(
					"%s, %llu update(s)", hidden_at_once ? "out" : "still in", (unsigned long long)hide_updates));
	const uint64_t before_hidden_sync = updates();
	r_pages.end_pass();
	first->sync();
	add("a hidden view asks F for nothing", updates() == before_hidden_sync,
			bf3::core::fmt("%llu update(s)", (unsigned long long)(updates() - before_hidden_sync)));
	first->set_visible(true);
	const bool waits = !first->shown_in_frame();
	first->sync();
	add("and showing it brings it back on its next sync", waits && first->shown_in_frame(),
			bf3::core::fmt(
					"%s before the sync, %s after", waits ? "out" : "in", first->shown_in_frame() ? "in" : "out"));
	settle_both();
	r_pages.end_pass();
	sync_both();
	const uint64_t before_parent = updates();
	const bf3::core::Status reopened =
			first->open(ui::hud_screen_movie(ui::ui_data_category_leaf(kIngameMenuPartition)));
	r_pages.end_pass();
	first->sync();
	add("a reopened view keeps F and its index",
			reopened.ok() && first->draws_in_frame() && first->frame_draw_index() == 0 && updates() != before_parent &&
					Bf3SwfView::counters().viewport_creates == creates,
			bf3::core::fmt("index %d, %llu update(s)", first->frame_draw_index(),
					(unsigned long long)(updates() - before_parent)));
	settle_both();
	r_pages.end_pass();
	sync_both();
	marked_then_drawn();
	const uint64_t before_free = updates();
	remove_child(second);
	memdelete(second);
	second = nullptr;
	add("freeing a shown view redraws F", updates() == before_free + 1,
			bf3::core::fmt("%llu update(s)", (unsigned long long)(updates() - before_free)));
	finish();
}

void Bf3UiOpenCensusCheck::census_undrawn_pass(Bf3UiPages &r_pages) {
	Bf3SwfView *view = open_paged_view(ui::ui_data_category_leaf(kIngameMenuPartition));
	if (view == nullptr) {
		return;
	}
	const auto finish = [&] {
		r_pages.drive_clock(std::nullopt);
		remove_child(view);
		memdelete(view);
	};
	UiPagesClock clock{ kUiOpenCensusDrivenClock * 2, kUiOpenCensusDrivenClock * 2 };
	r_pages.drive_clock(clock);
	view->sync();
	const uint32_t paged = view->paged_layers();
	const UiPagesCounters filled = r_pages.counters();
	const auto frame = [&](uint64_t p_draws) {
		++clock.process_frames;
		clock.draws += p_draws;
		r_pages.drive_clock(clock);
		view->sync();
	};
	for (uint32_t i = 0; i < kUiOpenCensusUndrawnFrames; ++i) {
		frame(0);
	}
	const UiPagesCounters held = r_pages.counters();
	add("a pass no frame drew stays up, and its layers wait",
			paged != 0 && filled.pass_items != 0 && held.pass_items >= filled.pass_items &&
					held.passes == filled.passes && view->paged_quads_shown() == 0 && view->page_fills_pending(),
			bf3::core::fmt("%u paged, %u shown, %llu then %llu items up, %llu more pass(es)", paged,
					view->paged_quads_shown(), (unsigned long long)filled.pass_items,
					(unsigned long long)held.pass_items, (unsigned long long)(held.passes - filled.passes)));
	ui::SwfLayerWalkOptions walk_options;
	walk_options.fonts = Bf3UiAssets::current()->swf_fonts();
	const ui::SwfLayerWalk walk = ui::swf_layer_walk(*view->scene(), walk_options);
	const auto layer = std::find_if(walk.layers.begin(), walk.layers.end(), [&view](const ui::SwfLayerWalkNode &p_l) {
		const ui::SwfSceneNode *n = view->scene()->node(p_l.owner);
		return p_l.route == ui::SwfLayerClass::Page && p_l.has_nodes && n != nullptr &&
				n->kind == ui::SwfNodeKind::EditText;
	});
	if (layer == walk.layers.end()) {
		add("a layer that outgrows its slot leaves the undrawn pass", false, "no paged text layer");
		finish();
		return;
	}
	const bf3::core::Status grown = view->scene()->set_text(layer->path, kUiOpenCensusLongerText);
	frame(0);
	const uint64_t left = r_pages.counters().pass_items_left - held.pass_items_left;
	add("a layer that outgrows its slot leaves the undrawn pass", grown.ok() && left == 2,
			bf3::core::fmt("%s: %llu item(s) left", layer->path.c_str(), (unsigned long long)left));
	frame(1);
	const uint32_t shown_after_draw = view->paged_quads_shown();
	settle_paged_view(r_pages, *view);
	add("and its layers show once a frame has drawn it", shown_after_draw != 0 && view->paged_quads_shown() == paged,
			bf3::core::fmt(
					"%u shown after the draw, %u of %u settled", shown_after_draw, view->paged_quads_shown(), paged));
	finish();
}

void Bf3UiOpenCensusCheck::census_screen_band(Bf3UiPages &r_pages) {
	const std::string one = ui::hud_screen_movie(ui::ui_data_category_leaf(kIngameMenuPartition));
	const std::string two = ui::hud_screen_movie(ui::ui_data_category_leaf(kSpawnButtonScreenPartition));
	Bf3UiScreens *band = memnew(Bf3UiScreens);
	band->set_manual(true);
	band->set_frame_band(true);
	add_child(band);
	Bf3UiScreens *loading = memnew(Bf3UiScreens);
	loading->set_manual(true);
	add_child(loading);
	const auto finish = [&] {
		remove_child(band);
		memdelete(band);
		remove_child(loading);
		memdelete(loading);
	};
	band->set_drawn(one, true);
	band->set_drawn(two, true);
	loading->set_drawn(one, true);
	const bf3::core::Status opened_one = band->open_movie_screen(one, nullptr);
	const bf3::core::Status opened_two = band->open_movie_screen(two, nullptr);
	const bf3::core::Status opened_off = loading->open_movie_screen(one, nullptr);
	if (!opened_one || !opened_two || !opened_off) {
		add("screens draw above the HUD band", false, "a screen did not open");
		finish();
		return;
	}
	Bf3SwfView *view_one = band->screen_view(one);
	Bf3SwfView *view_two = band->screen_view(two);
	Bf3SwfView *view_off = loading->screen_view(one);
	for (Bf3SwfView *v : { view_one, view_two, view_off }) {
		v->set_process(false);
		v->sync();
	}
	const int32_t hud_band = int32_t(kNativeHudViews);
	const int32_t at_one = view_one->frame_draw_index();
	const int32_t at_two = view_two->frame_draw_index();
	add("screens draw above the HUD band", at_one >= hud_band && at_two > at_one,
			bf3::core::fmt("%d and %d, the HUD below %d", at_one, at_two, hud_band));
	band->order_screen(one, band->screen_slot(1).order + 1);
	add("and follow the order they are given",
			view_one->frame_draw_index() > view_two->frame_draw_index() && view_two->frame_draw_index() >= hud_band,
			bf3::core::fmt("%d and %d", view_one->frame_draw_index(), view_two->frame_draw_index()));
	add("a stack off the band keeps its own root, as the loading screen does",
			!view_off->draws_in_frame() && view_off->presents_own_root() && r_pages.frame_viewport().is_valid(),
			view_off->draws_in_frame() ? "draws in F" : "own root");
	finish();
}

void Bf3UiOpenCensusCheck::census_pages() {
	Bf3UiPages *pages = memnew(Bf3UiPages);
	add_child(pages);
	const UiPagesCounters made = pages->counters();
	const ui::SwfPageSizes sizes = ui::swf_page_sizes(kUiOpenCensusWidth, kUiOpenCensusHeight);
	pages->ensure(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	const UiPagesCounters at_720 = pages->counters();
	pages->ensure(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	const UiPagesCounters again = pages->counters();
	const uint64_t frame_bytes = ui::swf_target_bytes(kUiOpenCensusWidth, kUiOpenCensusHeight, ui::kSwfTargetFactor4x);
	add("the pages are three viewports, kept",
			made.live_viewports == 3 && at_720.live_viewports == 3 && again.pages_made == at_720.pages_made,
			bf3::core::fmt("%llu live, %llu made", (unsigned long long)again.live_viewports,
					(unsigned long long)again.pages_made));
	add("S and R draw before F", pages->pages_chain_into_frame(),
			pages->pages_chain_into_frame() ? "S under R under F" : "R is not F's child, or S not R's");
	add("the pages cost their formula", at_720.bytes == sizes.result_bytes() + sizes.scratch_bytes() + frame_bytes,
			bf3::core::fmt("%.1f MiB for R %dx%d, S %dx%d and F at %dx%d", double(at_720.bytes) / (1024.0 * 1024.0),
					sizes.result_width, sizes.result_height, sizes.scratch_width, sizes.scratch_height,
					kUiOpenCensusWidth, kUiOpenCensusHeight));
	const uint64_t generation = pages->generation();
	const RID frame = pages->frame_viewport();
	pages->end_pass();
	const uint64_t frame_updates = pages->counters().frame_updates;
	pages->ensure(Vector2i(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2));
	const UiPagesCounters at_1440 = pages->counters();
	add("a window size that needs more remakes the pages",
			pages->generation() != generation && at_1440.live_viewports == 3 &&
					at_1440.bytes ==
							ui::swf_page_sizes(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2).result_bytes() +
									ui::swf_page_sizes(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2)
											.scratch_bytes() +
									frame_bytes * 4,
			bf3::core::fmt("%.1f MiB, %llu live", double(at_1440.bytes) / (1024.0 * 1024.0),
					(unsigned long long)at_1440.live_viewports));
	add("a resize keeps F and draws it again",
			pages->frame_viewport() == frame &&
					pages->frame_size() == Vector2i(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2) &&
					at_1440.frame_updates == frame_updates + 1 && pages->pages_chain_into_frame(),
			bf3::core::fmt("%s, %dx%d, %llu update(s)",
					pages->frame_viewport() == frame ? "same viewport" : "another viewport", pages->frame_size().x,
					pages->frame_size().y, (unsigned long long)(at_1440.frame_updates - frame_updates)));
	Node2D *present = memnew(Node2D);
	add_child(present);
	pages->attach_present(present);
	const Rect2 window(0.0f, 0.0f, real_t(kUiOpenCensusWidth), real_t(kUiOpenCensusHeight));
	const uint64_t presents = pages->counters().frame_presents;
	pages->present_frame(window);
	pages->present_frame(window);
	const uint64_t once = pages->counters().frame_presents - presents;
	pages->present_frame(Rect2(window.position, window.size * 2.0f));
	const uint64_t resized = pages->counters().frame_presents - presents;
	add("F has one present item, drawn again only for a new rect",
			once == 1 && resized == 2 && pages->presented_at().size == window.size * 2.0f,
			bf3::core::fmt("%llu draw(s) for one rect, %llu with a second", (unsigned long long)once,
					(unsigned long long)resized));
	pages->attach_present(nullptr);
	remove_child(present);
	memdelete(present);
	remove_child(pages);
	const bool gone = pages->counters().live_viewports == 0 && Bf3UiPages::current() == nullptr;
	memdelete(pages);
	add("leaving the tree frees the pages", gone, gone ? "no viewport left" : "still live");
}

void Bf3UiOpenCensusCheck::finish() {
	done_ = true;
	set_process(false);
	census_pages();
	census_paged_screens();
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
	add("filter stages share one material per kernel", filter_stages_ != 0 && filter_materials_ <= filter_kernels_,
			bf3::core::fmt("%llu materials for %llu stages, %llu distinct kernels",
					(unsigned long long)filter_materials_, (unsigned long long)filter_stages_,
					(unsigned long long)filter_kernels_));
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
