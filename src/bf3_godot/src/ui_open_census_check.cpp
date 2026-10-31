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
	uint64_t slots_after = 0;
	uint64_t creates_total = 0;
	for (const std::string &partition : partitions_) {
		const std::string leaf = ui::ui_data_category_leaf(partition);
		const SwfViewCounters before = Bf3SwfView::counters();
		const UiPagesCounters pages_before = pages->counters();
		Bf3SwfView *view = memnew(Bf3SwfView);
		view->set_playing(false);
		view->set_process(false);
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
		uint64_t own_surfaces = 0;
		for (size_t i = 0; i < plan.layers.size(); ++i) {
			const ui::SwfPlannedLayer &l = plan.layers[i];
			if (l.klass() == ui::SwfLayerClass::OwnSurface || l.klass() == ui::SwfLayerClass::Clip) {
				own_surfaces += l.kind == ui::SwfLayerKind::Mask ? 2u : 1u + l.filters;
			}
		}
		const uint64_t creates = synced.viewport_creates - before.viewport_creates;
		creates_total += creates;
		const uint64_t surfaces = synced.live_surfaces - before.live_surfaces;
		if (surfaces != own_surfaces + 1) {
			surfaces_differ += bf3::core::fmt(" %s: %llu live, plan %llu;", leaf.c_str(), (unsigned long long)surfaces,
					(unsigned long long)(own_surfaces + 1));
		}
		if (plan.counts.pages != 0 && own_surfaces == 0) {
			++page_only;
			if (creates != 1 || synced.viewport_resizes - before.viewport_resizes != 1) {
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
		const uint64_t settled_passes = pages->counters().passes;
		pages->end_pass();
		view->sync();
		pages->end_pass();
		view->sync();
		if (pages->counters().passes != settled_passes) {
			redraws += " " + leaf;
		}
		remove_child(view);
		memdelete(view);
		slots_after = std::max(slots_after, pages->counters().live_slots);
	}
	add("a page-only screen makes only its root viewport", page_only != 0 && root_only_differs.empty(),
			root_only_differs.empty() ? bf3::core::fmt("%u screens, %llu viewports over all 30", page_only,
												(unsigned long long)creates_total)
									  : root_only_differs);
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
	census_page_overflow(*pages);
	remove_child(pages);
	memdelete(pages);
}

Bf3SwfView *Bf3UiOpenCensusCheck::open_paged_view(const std::string &p_leaf) {
	Bf3SwfView *view = memnew(Bf3SwfView);
	view->set_playing(false);
	view->set_process(false);
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
	const uint64_t before_text = passes();
	const bf3::core::Status text = view->scene()->set_text(layer->path, "CENSUS");
	view->sync();
	settle_paged_view(r_pages, *view);
	add("a changed layer fills again", text.ok() && passes() == before_text + 1,
			bf3::core::fmt("%llu pass(es)", (unsigned long long)(passes() - before_text)));
	remove_child(view);
	memdelete(view);
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

void Bf3UiOpenCensusCheck::census_pages() {
	Bf3UiPages *pages = memnew(Bf3UiPages);
	add_child(pages);
	const UiPagesCounters made = pages->counters();
	const ui::SwfPageSizes sizes = ui::swf_page_sizes(kUiOpenCensusWidth, kUiOpenCensusHeight);
	pages->ensure(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	const UiPagesCounters at_720 = pages->counters();
	pages->ensure(Vector2i(kUiOpenCensusWidth, kUiOpenCensusHeight));
	const UiPagesCounters again = pages->counters();
	add("the pages are two viewports, kept",
			made.live_viewports == 2 && at_720.live_viewports == 2 && again.pages_made == at_720.pages_made,
			bf3::core::fmt("%llu live, %llu made", (unsigned long long)again.live_viewports,
					(unsigned long long)again.pages_made));
	add("the pages cost their formula", at_720.bytes == sizes.result_bytes() + sizes.scratch_bytes(),
			bf3::core::fmt("%.1f MiB for R %dx%d and S %dx%d at %dx%d", double(at_720.bytes) / (1024.0 * 1024.0),
					sizes.result_width, sizes.result_height, sizes.scratch_width, sizes.scratch_height,
					kUiOpenCensusWidth, kUiOpenCensusHeight));
	const uint64_t generation = pages->generation();
	pages->ensure(Vector2i(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2));
	const UiPagesCounters at_1440 = pages->counters();
	add("a window size that needs more remakes the pages",
			pages->generation() != generation && at_1440.live_viewports == 2 &&
					at_1440.bytes ==
							ui::swf_page_sizes(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2).result_bytes() +
									ui::swf_page_sizes(kUiOpenCensusWidth * 2, kUiOpenCensusHeight * 2).scratch_bytes(),
			bf3::core::fmt("%.1f MiB, %llu live", double(at_1440.bytes) / (1024.0 * 1024.0),
					(unsigned long long)at_1440.live_viewports));
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
