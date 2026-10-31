#include "native_screens.h"

#include "godot_util.h"
#include "native_hud.h"
#include "swf_view.h"
#include "ui_assets.h"
#include "ui_stage.h"

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/swf_hit.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/host/ui_flow_session.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

using namespace godot;
namespace ui = bf3::core::ui;

namespace bf3::godot_side {

const char *ui_screen_phase_name(UiScreenPhase p_phase) {
	switch (p_phase) {
		case UiScreenPhase::Entering:
			return "entering";
		case UiScreenPhase::Shown:
			return "shown";
		case UiScreenPhase::Exiting:
			return "exiting";
	}
	return "?";
}

struct Bf3UiScreens::Entry {
	std::string partition;
	bool adopted = false;
	bool movie = false;
	uint64_t flow_id = 0;
	UiScreenPhase phase = UiScreenPhase::Entering;
	bool shown = true;
	bool modal = false;
	bool focused = false;
	int32_t order = 0;
	uint64_t shown_count = 1;
	bool refreshed = false;
	Bf3SwfView *view = nullptr;
	std::unique_ptr<ui::HudPopup> popup;
	std::unique_ptr<ui::SwfScene> scene;
	std::unique_ptr<ui::HudScreen> screen;
};

namespace {

constexpr std::string_view kPopupButtons = "PopupButtonsManager_01";

std::unique_ptr<ui::HudPopup> popup_of(const ui::UiFlowScreen &p_screen) {
	auto popup = std::make_unique<ui::HudPopup>();
	popup->title = p_screen.dialog_title;
	popup->text = p_screen.dialog_text;
	for (const ui::UiDialogButton &b : p_screen.dialog_buttons) {
		popup->buttons.push_back(ui::HudPopupButton{ b.label, b.input_concept });
	}
	return popup;
}

}

Bf3UiScreens::Bf3UiScreens() = default;

Bf3UiScreens::~Bf3UiScreens() = default;

void Bf3UiScreens::_bind_methods() {}

void Bf3UiScreens::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		hud_id_ = 0;
		for (std::unique_ptr<Entry> &e : entries_) {
			e->screen.reset();
		}
	}
}

Bf3NativeHud *Bf3UiScreens::hud() const {
	return hud_id_ != 0 ? Object::cast_to<Bf3NativeHud>(ObjectDB::get_instance(hud_id_)) : nullptr;
}

void Bf3UiScreens::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3UiScreens::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || manual_) {
		return;
	}
	step(p_delta);
}

void Bf3UiScreens::note(std::string p_line) {
	if (log_.size() < kUiScreensMaxLog) {
		log_.push_back(std::move(p_line));
	}
}

void Bf3UiScreens::error(std::string p_message) {
	note("error " + p_message);
	if (errors_.size() < kUiScreensMaxErrors) {
		errors_.push_back(std::move(p_message));
	}
}

Bf3UiScreens::Entry *Bf3UiScreens::find(std::string_view p_partition) {
	for (std::unique_ptr<Entry> &e : entries_) {
		if (e->partition == p_partition) {
			return e.get();
		}
	}
	return nullptr;
}

const Bf3UiScreens::Entry *Bf3UiScreens::find(std::string_view p_partition) const {
	for (const std::unique_ptr<Entry> &e : entries_) {
		if (e->partition == p_partition) {
			return e.get();
		}
	}
	return nullptr;
}

ui::HudScreen *Bf3UiScreens::screen_of(const Entry &p_entry) const {
	if (p_entry.adopted) {
		const Bf3NativeHud *h = hud();
		return h != nullptr ? h->find_screen(p_entry.partition).screen : nullptr;
	}
	return p_entry.screen.get();
}

Bf3SwfView *Bf3UiScreens::view_of(const Entry &p_entry) const {
	if (p_entry.adopted) {
		const Bf3NativeHud *h = hud();
		return h != nullptr ? h->find_screen(p_entry.partition).view : nullptr;
	}
	return p_entry.view;
}

void Bf3UiScreens::adopt(Bf3NativeHud *p_hud) {
	hud_id_ = p_hud != nullptr ? p_hud->get_instance_id() : 0;
	if (p_hud == nullptr) {
		return;
	}
	for (size_t i = 0; i < kNativeHudViews; ++i) {
		if (find(kNativeHudPartitions[i]) != nullptr) {
			continue;
		}
		auto e = std::make_unique<Entry>();
		e->partition = std::string(kNativeHudPartitions[i]);
		e->adopted = true;
		e->order = int32_t(i);
		e->phase = UiScreenPhase::Shown;
		entries_.push_back(std::move(e));
	}
}

void Bf3UiScreens::follow(host::UiFlowSession *p_session) {
	session_ = p_session;
}

void Bf3UiScreens::set_options(const ui::OptionsModel *p_options) {
	options_ = p_options;
	client_.options = p_options;
}

void Bf3UiScreens::set_client(ui::HudClientState p_client) {
	client_ = std::move(p_client);
	client_.options = options_;
	client_.flow_data = [this](std::string_view p_category, int32_t p_key) -> std::optional<std::string> {
		return session_ != nullptr ? session_->data(p_category, p_key) : std::nullopt;
	};
}

void Bf3UiScreens::set_drawn(std::string_view p_partition, bool p_drawn) {
	if (p_drawn) {
		drawn_.emplace(p_partition);
	} else if (const auto it = drawn_.find(p_partition); it != drawn_.end()) {
		drawn_.erase(it);
	}
}

bf3::core::Status Bf3UiScreens::open_screen(std::string_view p_partition) {
	if (Entry *e = find(p_partition)) {
		Bf3NativeHud *h = e->adopted ? hud() : nullptr;
		if (h != nullptr) {
			BF3_TRY(h->open_screen(p_partition));
		}
		return bf3::core::Status::success();
	}
	if (db_ == nullptr) {
		return bf3::core::make_error(std::string(p_partition) + ": Bf3UiScreens has no resolver for screen assets");
	}
	const auto read_start = std::chrono::steady_clock::now();
	bf3::core::Result<ui::HudScreenAsset> asset = ui::read_hud_screen_asset(*db_, p_partition);
	stats_.asset_read_seconds += bf3::core::seconds_since(read_start);
	if (!asset) {
		return asset.error();
	}
	return open_entry(p_partition, *asset, nullptr);
}

bf3::core::Status Bf3UiScreens::open_movie_screen(std::string_view p_movie, ui::HudRootData *p_data) {
	if (find(p_movie) != nullptr) {
		return bf3::core::Status::success();
	}
	BF3_TRY(open_entry(p_movie, ui::hud_movie_screen_asset(p_movie), p_data));
	find(p_movie)->movie = true;
	return bf3::core::Status::success();
}

void Bf3UiScreens::free_view(Bf3SwfView *p_view) {
	remove_child(p_view);
	p_view->queue_free();
}

Bf3SwfView *Bf3UiScreens::take_idle_view(std::string_view p_partition) {
	const auto it = std::find_if(idle_views_.begin(), idle_views_.end(),
			[p_partition](const std::pair<std::string, uint64_t> &p_idle) { return p_idle.first == p_partition; });
	if (it == idle_views_.end()) {
		return nullptr;
	}
	Bf3SwfView *v = Object::cast_to<Bf3SwfView>(ObjectDB::get_instance(it->second));
	idle_views_.erase(it);
	if (v != nullptr) {
		v->set_visible(true);
		v->set_process(true);
		++stats_.views_reused;
	}
	return v;
}

void Bf3UiScreens::keep_idle_view(const std::string &p_partition, Bf3SwfView *p_view) {
	const auto same = std::find_if(idle_views_.begin(), idle_views_.end(),
			[&p_partition](const std::pair<std::string, uint64_t> &p_idle) { return p_idle.first == p_partition; });
	if (same != idle_views_.end()) {
		if (Bf3SwfView *old = Object::cast_to<Bf3SwfView>(ObjectDB::get_instance(same->second))) {
			free_view(old);
		}
		idle_views_.erase(same);
	}
	p_view->set_visible(false);
	p_view->set_process(false);
	p_view->release_root_target();
	p_view->release_page_slots();
	idle_views_.emplace_back(p_partition, p_view->get_instance_id());
	uint64_t bytes = 0;
	uint32_t items = 0;
	for (auto it = idle_views_.rbegin(); it != idle_views_.rend();) {
		Bf3SwfView *v = Object::cast_to<Bf3SwfView>(ObjectDB::get_instance(it->second));
		const uint64_t view_bytes = v != nullptr ? v->surface_bytes() : 0;
		const uint32_t view_items = v != nullptr ? v->instance_items() : 0;
		if (v == nullptr || bytes + view_bytes > kUiScreensIdleViewBytes ||
				items + view_items > kUiScreensIdleViewItems) {
			if (v != nullptr) {
				free_view(v);
				++stats_.views_dropped;
			}
			it = std::make_reverse_iterator(idle_views_.erase(std::next(it).base()));
		} else {
			bytes += view_bytes;
			items += view_items;
			++it;
		}
	}
}

ui::HudScreen *Bf3UiScreens::hud_screen(std::string_view p_partition) {
	const Entry *e = find(p_partition);
	return e != nullptr ? screen_of(*e) : nullptr;
}

Bf3SwfView *Bf3UiScreens::screen_view(std::string_view p_partition) {
	const Entry *e = find(p_partition);
	return e != nullptr ? view_of(*e) : nullptr;
}

bf3::core::Status Bf3UiScreens::open_entry(
		std::string_view p_partition, const ui::HudScreenAsset &p_asset, ui::HudRootData *p_data) {
	if (entries_.size() >= kUiScreensMax) {
		return bf3::core::make_error(
				std::string(p_partition) + ": more than " + std::to_string(kUiScreensMax) + " screens");
	}
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || !assets->is_loaded() || assets->movies() == nullptr) {
		return bf3::core::make_error(std::string(p_partition) + ": no Bf3UiAssets movies");
	}
	const auto open_start = std::chrono::steady_clock::now();
	auto e = std::make_unique<Entry>();
	e->partition = std::string(p_partition);
	e->order = next_order_++;
	ui::HudStageRects rects;
	ui::SwfScene *scene = nullptr;
	if (drawn_.count(p_partition) != 0) {
		Bf3SwfView *v = take_idle_view(p_partition);
		if (v == nullptr) {
			v = memnew(Bf3SwfView);
			v->set_name(to_godot(ui::ui_data_category_leaf(p_partition)));
			v->set_playing(false);
			v->set_skips_hidden_render(true);
			add_child(v);
		}
		e->view = v;
		if (const bf3::core::Status st = v->open(p_asset.movie); !st) {
			free_view(v);
			return bf3::core::make_error(p_asset.movie + ": " + st.error().message);
		}
		scene = v->scene();
		rects.visible = ui_stage(this).visible;
	} else {
		bf3::core::Result<ui::SwfScene> made = ui::SwfScene::open(*assets->movies(), p_asset.movie);
		if (!made) {
			return bf3::core::make_error(p_asset.movie + ": " + made.error().message);
		}
		e->scene = std::make_unique<ui::SwfScene>(std::move(*made));
		scene = e->scene.get();
		rects.visible = scene->stage();
	}
	rects.original = scene->stage();
	rects.safe_x = ui::hud_safe_offset(ui::kHudFootageSafeX, rects.visible.width(), safe_area_.width);
	rects.safe_y = ui::hud_safe_offset(ui::kHudFootageSafeY, rects.visible.height(), safe_area_.height);
	bf3::core::Result<std::unique_ptr<ui::HudScreen>> s =
			ui::HudScreen::open(*scene, *assets->movies(), p_asset, assets->swf_fonts(), client_, rects, p_data);
	if (!s) {
		if (e->view != nullptr) {
			free_view(e->view);
		}
		return bf3::core::make_error(p_asset.name + ": " + s.error().message);
	}
	e->screen = std::move(*s);
	const std::string partition = e->partition;
	e->screen->set_event_sink([this, partition](const ui::HudWidgetEvent &p_event) {
		++stats_.events_out;
		note("event " + ui::ui_data_category_leaf(partition) + "." + p_event.instance + " " + p_event.query);
		if (session_ != nullptr) {
			session_->widget_fired(partition, p_event.instance, p_event.query,
					p_event.param.defined() ? std::optional<std::string>(p_event.param.to_string()) : std::nullopt);
		}
	});
	stats_.open_seconds += bf3::core::seconds_since(open_start);
	const auto initialize_start = std::chrono::steady_clock::now();
	e->screen->initialize(model_);
	stats_.initialize_seconds += bf3::core::seconds_since(initialize_start);
	++stats_.opened;
	note("open " + ui::ui_data_category_leaf(partition) + (e->view != nullptr ? " drawn" : ""));
	entries_.push_back(std::move(e));
	arrange();
	return bf3::core::Status::success();
}

void Bf3UiScreens::apply_visibility(Entry &r_entry) {
	if (r_entry.view != nullptr) {
		r_entry.view->set_visible(r_entry.shown);
	}
}

bool Bf3UiScreens::show_screen(std::string_view p_partition) {
	Entry *e = find(p_partition);
	if (e == nullptr) {
		return false;
	}
	if (e->adopted) {
		Bf3NativeHud *h = hud();
		return h != nullptr && h->show_screen(p_partition);
	}
	e->shown_count += e->shown ? 0 : 1;
	e->shown = true;
	apply_visibility(*e);
	return true;
}

bool Bf3UiScreens::hide_screen(std::string_view p_partition) {
	Entry *e = find(p_partition);
	if (e == nullptr) {
		return false;
	}
	if (e->adopted) {
		Bf3NativeHud *h = hud();
		return h != nullptr && h->hide_screen(p_partition);
	}
	e->shown = false;
	apply_visibility(*e);
	return true;
}

bool Bf3UiScreens::order_screen(std::string_view p_partition, int32_t p_order) {
	Entry *e = find(p_partition);
	if (e == nullptr) {
		return false;
	}
	e->order = p_order;
	if (Bf3NativeHud *h = e->adopted ? hud() : nullptr) {
		h->order_screen(p_partition, p_order);
	}
	arrange();
	return true;
}

void Bf3UiScreens::arrange() {
	std::vector<Entry *> own;
	for (std::unique_ptr<Entry> &e : entries_) {
		if (!e->adopted && e->view != nullptr) {
			own.push_back(e.get());
		}
	}
	std::stable_sort(
			own.begin(), own.end(), [](const Entry *p_a, const Entry *p_b) { return p_a->order < p_b->order; });
	int32_t at = 0;
	for (Entry *e : own) {
		move_child(e->view, at++);
	}
}

void Bf3UiScreens::begin_close(Entry &r_entry) {
	if (r_entry.phase == UiScreenPhase::Exiting) {
		return;
	}
	r_entry.phase = UiScreenPhase::Exiting;
	if (r_entry.adopted) {
		return;
	}
	if (r_entry.focused) {
		r_entry.focused = false;
		if (r_entry.screen != nullptr) {
			r_entry.screen->set_focus(false);
		}
	}
	if (r_entry.screen != nullptr) {
		r_entry.screen->begin_exit();
	}
}

bool Bf3UiScreens::close_screen(std::string_view p_partition) {
	Entry *e = find(p_partition);
	if (e == nullptr) {
		return false;
	}
	begin_close(*e);
	return true;
}

void Bf3UiScreens::drop(size_t p_index) {
	Entry &e = *entries_[p_index];
	if (e.adopted) {
		e.flow_id = 0;
		e.phase = UiScreenPhase::Shown;
		return;
	}
	closed_shown_[e.partition] += e.shown_count;
	e.screen.reset();
	if (e.view != nullptr) {
		const std::string owner = view_owner(e);
		bg_.forget(owner);
		effects_seen_.erase(owner);
		keep_idle_view(e.partition, e.view);
		e.view = nullptr;
	}
	entries_.erase(entries_.begin() + std::ptrdiff_t(p_index));
}

bool Bf3UiScreens::set_screen_modal(std::string_view p_partition, bool p_modal) {
	Entry *e = find(p_partition);
	if (e == nullptr) {
		return false;
	}
	if (e->adopted) {
		return !p_modal;
	}
	e->modal = p_modal;
	return true;
}

bool Bf3UiScreens::focus_screen(std::string_view p_partition) {
	const Entry *target = find(p_partition);
	if (target == nullptr || target->adopted) {
		return false;
	}
	for (std::unique_ptr<Entry> &e : entries_) {
		const bool want = e->partition == p_partition;
		if (e->adopted || e->focused == want) {
			continue;
		}
		e->focused = want;
		if (e->screen != nullptr) {
			e->screen->set_focus(want);
		}
	}
	return true;
}

void Bf3UiScreens::clear_focus() {
	for (std::unique_ptr<Entry> &e : entries_) {
		if (e->adopted || !e->focused) {
			continue;
		}
		e->focused = false;
		if (e->screen != nullptr) {
			e->screen->set_focus(false);
		}
	}
}

UiScreenSlot Bf3UiScreens::screen_slot(size_t p_index) const {
	if (p_index >= entries_.size()) {
		return {};
	}
	const Entry &e = *entries_[p_index];
	if (e.adopted) {
		const Bf3NativeHud *h = hud();
		UiScreenSlot s = h != nullptr ? h->find_screen(e.partition) : UiScreenSlot{};
		s.partition = e.partition;
		return s;
	}
	return UiScreenSlot{ e.partition, e.screen.get(), e.view, e.shown, e.order, e.modal, e.focused };
}

bool Bf3UiScreens::reports(std::string_view p_partition) const {
	const Entry *e = find(p_partition);
	if (e != nullptr && e->adopted) {
		const Bf3NativeHud *h = hud();
		return h != nullptr && h->reports(p_partition);
	}
	return e != nullptr || closed_shown_.find(p_partition) != closed_shown_.end();
}

std::optional<UiScreenRecord> Bf3UiScreens::screen_record(std::string_view p_partition) const {
	const Entry *e = find(p_partition);
	const auto closed = closed_shown_.find(p_partition);
	const uint64_t shown_before = closed != closed_shown_.end() ? closed->second : 0;
	if (e == nullptr) {
		if (closed == closed_shown_.end()) {
			return std::nullopt;
		}
		UiScreenRecord r;
		r.shown = shown_before;
		return r;
	}
	if (e->adopted) {
		const Bf3NativeHud *h = hud();
		return h != nullptr ? h->screen_record(p_partition) : std::nullopt;
	}
	UiScreenRecord r;
	r.visible = e->shown && e->phase != UiScreenPhase::Exiting && is_visible_in_tree();
	r.shown = shown_before + e->shown_count;
	if (e->screen != nullptr) {
		add_screen_widget_facts(*e->screen, r);
	}
	return r;
}

std::optional<UiScreenPhase> Bf3UiScreens::phase(std::string_view p_partition) const {
	const Entry *e = find(p_partition);
	return e != nullptr ? std::optional<UiScreenPhase>(e->phase) : std::nullopt;
}

void Bf3UiScreens::sync_flow() {
	if (session_->stats().reloads != reloads_seen_) {
		reloads_seen_ = session_->stats().reloads;
		for (std::unique_ptr<Entry> &e : entries_) {
			if (e->flow_id == 0) {
				continue;
			}
			e->flow_id = 0;
			if (!e->adopted) {
				note("close " + ui::ui_data_category_leaf(e->partition) + " (flow reloaded)");
				begin_close(*e);
			}
		}
	}
	ui::UiFlow &flow = session_->flow();
	for (const uint64_t id : flow.take_closing()) {
		bool hosted = false;
		for (std::unique_ptr<Entry> &e : entries_) {
			if (e->flow_id == id) {
				hosted = true;
				note("close " + ui::ui_data_category_leaf(e->partition));
				begin_close(*e);
			}
		}
		if (!hosted) {
			session_->screen_exited(id);
		}
	}
	for (const uint64_t id : flow.take_opened()) {
		const ui::UiFlowScreen *f = flow.screen(id);
		if (f == nullptr) {
			continue;
		}
		const std::string partition = f->partition;
		const bool modal = f->modal;
		const bool focusable = f->focusable;
		for (size_t i = 0; i < entries_.size(); ++i) {
			Entry &old = *entries_[i];
			if (old.partition == partition && old.phase == UiScreenPhase::Exiting) {
				const uint64_t old_id = old.flow_id;
				drop(i);
				if (old_id != 0) {
					session_->screen_exited(old_id);
				}
				break;
			}
		}
		std::unique_ptr<ui::HudPopup> popup = f->dialog ? popup_of(*f) : nullptr;
		client_.popup = popup.get();
		const bf3::core::Status opened = open_screen(partition);
		client_.popup = nullptr;
		if (!opened) {
			++stats_.failed;
			error(opened.error().message);
			session_->screen_initialized(id);
			continue;
		}
		Entry *e = find(partition);
		if (e == nullptr) {
			continue;
		}
		const bool reopened = e->flow_id != 0 && e->flow_id != id && e->phase == UiScreenPhase::Shown;
		e->flow_id = id;
		if (e->adopted) {
			e->phase = UiScreenPhase::Entering;
			continue;
		}
		if (reopened) {
			note("reopened " + ui::ui_data_category_leaf(partition));
			session_->screen_initialized(id);
		}
		if (popup != nullptr && e->screen != nullptr && e->popup == nullptr) {
			e->popup = std::move(popup);
			e->screen->focus_widget(std::string(ui::kHudScreenInstance) + "." + std::string(kPopupButtons));
		}
		e->modal = modal;
		if (focusable) {
			focus_screen(partition);
		}
	}
	for (std::unique_ptr<Entry> &e : entries_) {
		if (e->adopted || e->flow_id == 0 || e->phase == UiScreenPhase::Exiting) {
			continue;
		}
		const ui::UiFlowScreen *f = flow.screen(e->flow_id);
		if (f != nullptr && f->shown != e->shown) {
			e->shown_count += f->shown ? 1 : 0;
			e->shown = f->shown;
			apply_visibility(*e);
		}
	}
	const ui::UiFlowScreen *focus = flow.focused() ? flow.screen(*flow.focused()) : nullptr;
	const Entry *target = focus != nullptr ? find(focus->partition) : nullptr;
	if (target != nullptr && !target->adopted && !target->focused && target->phase != UiScreenPhase::Exiting) {
		focus_screen(focus->partition);
	}
}

void Bf3UiScreens::deliver_widget_events() {
	const auto start = std::chrono::steady_clock::now();
	for (const host::UiFlowWidgetEvent &w : session_->take_widget_events()) {
		Entry *e = find(w.partition);
		ui::HudScreen *s = e != nullptr ? screen_of(*e) : nullptr;
		if (s == nullptr) {
			continue;
		}
		++stats_.events_in;
		const ui::As2Value param = w.value ? ui::As2Value::of(*w.value) : ui::As2Value::of(0);
		const bool taken = s->deliver_in_event(w.instance, w.query, param);
		note("deliver " + ui::ui_data_category_leaf(w.partition) + "." + w.instance + " " + w.query +
				(taken ? "" : " (no port)"));
	}
	stats_.event_seconds += bf3::core::seconds_since(start);
}

void Bf3UiScreens::report_protocol() {
	for (size_t i = 0; i < entries_.size();) {
		Entry &e = *entries_[i];
		ui::HudScreen *s = screen_of(e);
		if (e.phase == UiScreenPhase::Entering && (e.adopted || (s != nullptr && s->enter_complete()))) {
			e.phase = UiScreenPhase::Shown;
			++stats_.entered;
			note("entered " + ui::ui_data_category_leaf(e.partition));
			if (session_ != nullptr && e.flow_id != 0) {
				session_->screen_initialized(e.flow_id);
			}
		}
		if (e.phase == UiScreenPhase::Exiting && (e.adopted || s == nullptr || s->exit_complete())) {
			++stats_.exited;
			note("exited " + ui::ui_data_category_leaf(e.partition));
			const uint64_t id = e.flow_id;
			const bool adopted = e.adopted;
			drop(i);
			if (session_ != nullptr && id != 0) {
				session_->screen_exited(id);
			}
			if (!adopted) {
				continue;
			}
		}
		++i;
	}
}

void Bf3UiScreens::step(double p_seconds) {
	const double dt = std::isfinite(p_seconds) && p_seconds > 0.0 ? p_seconds : 0.0;
	if (session_ != nullptr) {
		sync_flow();
		deliver_widget_events();
	}
	for (std::unique_ptr<Entry> &e : entries_) {
		if (e->adopted || e->screen == nullptr) {
			continue;
		}
		ui::SwfScene *scene = e->view != nullptr ? e->view->scene() : e->scene.get();
		if (e->view != nullptr) {
			ui::HudStageRects rects = e->screen->stage_rects();
			rects.visible = ui_stage(this).visible;
			rects.original = scene->stage();
			rects.safe_x = ui::hud_safe_offset(ui::kHudFootageSafeX, rects.visible.width(), safe_area_.width);
			rects.safe_y = ui::hud_safe_offset(ui::kHudFootageSafeY, rects.visible.height(), safe_area_.height);
			e->screen->set_stage_rects(rects);
		}
		const auto refresh_start = std::chrono::steady_clock::now();
		e->screen->refresh(model_);
		if (!e->refreshed) {
			e->refreshed = true;
			stats_.first_refresh_seconds += bf3::core::seconds_since(refresh_start);
		}
		e->screen->advance(dt);
		if (const bf3::core::Status st = scene->advance_seconds(dt); !st) {
			error(e->partition + ": " + st.error().message);
		}
		if (e->view != nullptr) {
			const std::string owner = view_owner(*e);
			const uint64_t revision = e->screen->effects_revision();
			if (const auto it = effects_seen_.find(owner); it == effects_seen_.end() || it->second != revision) {
				effects_seen_[owner] = revision;
				apply_hud_view_effects(*e->view, *e->screen);
			}
			bg_.paint_all(*e->view, owner);
		}
	}
	report_protocol();
}

Bf3UiScreens::Entry *Bf3UiScreens::input_target() {
	Entry *modal = nullptr;
	Entry *focused = nullptr;
	for (std::unique_ptr<Entry> &e : entries_) {
		if (e->adopted || e->phase == UiScreenPhase::Exiting || !e->shown) {
			continue;
		}
		if (e->modal && (modal == nullptr || e->order >= modal->order)) {
			modal = e.get();
		}
		if (e->focused) {
			focused = e.get();
		}
	}
	return modal != nullptr ? modal : focused;
}

std::string Bf3UiScreens::view_owner(const Entry &p_entry) {
	return p_entry.partition + "#" + std::to_string(p_entry.order);
}

Bf3UiScreens::Entry *Bf3UiScreens::background_listener(const Entry *p_target, int32_t p_concept, bool p_pressed) {
	if (p_target != nullptr && (p_target->modal || !p_target->adopted)) {
		return nullptr;
	}
	std::vector<Entry *> stack;
	for (std::unique_ptr<Entry> &e : entries_) {
		if (e.get() != p_target && !e->adopted && e->shown && e->phase != UiScreenPhase::Exiting) {
			stack.push_back(e.get());
		}
	}
	std::sort(stack.begin(), stack.end(), [](const Entry *a, const Entry *b) { return a->order > b->order; });
	for (Entry *e : stack) {
		ui::HudScreen *s = screen_of(*e);
		if (s != nullptr && s->input_concept(p_concept, p_pressed)) {
			return e;
		}
	}
	return nullptr;
}

bool Bf3UiScreens::input_concept(int32_t p_concept, bool p_pressed) {
	++stats_.concepts;
	Entry *t = input_target();
	ui::HudScreen *s = t != nullptr ? screen_of(*t) : nullptr;
	const bool taken = s != nullptr && s->input_concept(p_concept, p_pressed);
	if (taken) {
		note("concept " + std::string(ui::ui_input_concept_name(ui::UiInputConcept(p_concept))) + " to " +
				ui::ui_data_category_leaf(t->partition));
	}
	if (const Entry *b = background_listener(t, p_concept, p_pressed)) {
		note("concept " + std::string(ui::ui_input_concept_name(ui::UiInputConcept(p_concept))) + " to " +
				ui::ui_data_category_leaf(b->partition));
		return true;
	}
	if (!p_pressed || session_ == nullptr) {
		return taken;
	}
	++stats_.concepts_to_flow;
	note("concept " + std::string(ui::ui_input_concept_name(ui::UiInputConcept(p_concept))) + " to the flow");
	const bool found = session_->input_action(p_concept);
	stats_.concepts_found += found ? 1u : 0u;
	return found || taken;
}

bool Bf3UiScreens::key(int32_t p_code, bool p_pressed) {
	++stats_.keys;
	Entry *t = input_target();
	ui::HudScreen *s = t != nullptr ? screen_of(*t) : nullptr;
	return s != nullptr && s->key(p_code, p_pressed);
}

std::optional<Vector2> Bf3UiScreens::stage_point(const Entry &p_entry, Vector2 p_viewport) const {
	const Bf3SwfView *v = view_of(p_entry);
	if (v == nullptr || !v->is_visible_in_tree()) {
		return std::nullopt;
	}
	const std::optional<ui::SwfPoint> at = v->canvas_to_stage(p_viewport);
	return at ? std::optional<Vector2>(Vector2(at->x, at->y)) : std::nullopt;
}

std::vector<Bf3UiScreens::Entry *> Bf3UiScreens::mouse_screens() {
	Entry *t = input_target();
	if (t != nullptr && t->modal) {
		return { t };
	}
	std::vector<Entry *> out;
	for (std::unique_ptr<Entry> &e : entries_) {
		if (!e->adopted && e->phase != UiScreenPhase::Exiting && e->shown && e->view != nullptr) {
			out.push_back(e.get());
		}
	}
	std::sort(out.begin(), out.end(), [](const Entry *p_a, const Entry *p_b) { return p_a->order > p_b->order; });
	if (t != nullptr && std::find(out.begin(), out.end(), t) == out.end()) {
		out.push_back(t);
	}
	return out;
}

bool Bf3UiScreens::mouse_move(Vector2 p_viewport) {
	++stats_.mouse;
	bool any = false;
	for (Entry *e : mouse_screens()) {
		ui::HudScreen *s = screen_of(*e);
		const std::optional<Vector2> at = stage_point(*e, p_viewport);
		any = (s != nullptr && at && s->mouse_move(at->x, at->y)) || any;
	}
	return any;
}

bool Bf3UiScreens::mouse_button(Vector2 p_viewport, int32_t p_button, bool p_pressed) {
	++stats_.mouse;
	Entry *target = nullptr;
	if (!p_pressed && !pressed_screen_.empty()) {
		target = find(pressed_screen_);
	}
	if (target == nullptr) {
		const std::vector<Entry *> screens = mouse_screens();
		for (Entry *e : screens) {
			ui::HudScreen *s = screen_of(*e);
			const std::optional<Vector2> at = stage_point(*e, p_viewport);
			if (s != nullptr && at && !s->hit_target(at->x, at->y).empty()) {
				target = e;
				break;
			}
		}
		if (target == nullptr) {
			target = input_target();
		}
	}
	pressed_screen_ = p_pressed && target != nullptr ? target->partition : std::string();
	ui::HudScreen *s = target != nullptr ? screen_of(*target) : nullptr;
	const std::optional<Vector2> at = target != nullptr ? stage_point(*target, p_viewport) : std::nullopt;
	return s != nullptr && at && s->mouse_button(at->x, at->y, p_button, p_pressed);
}

std::optional<ui::SwfPoint> Bf3UiScreens::clip_stage_centre(std::string_view p_partition, std::string_view p_path) {
	Entry *e = find(p_partition);
	ui::HudScreen *s = e != nullptr ? screen_of(*e) : nullptr;
	const Bf3SwfView *v = e != nullptr ? view_of(*e) : nullptr;
	const ui::SwfSceneNode *n = s != nullptr ? s->scene().find_node(p_path) : nullptr;
	if (n == nullptr || v == nullptr || !v->is_visible_in_tree()) {
		return std::nullopt;
	}
	const ui::SwfRect b = ui::swf_node_local_bounds(s->scene(), *n);
	const ui::SwfMatrix w = ui::swf_world_matrix(s->scene(), n->id);
	return ui::SwfPoint{ w.tx + w.a * (b.x_min + b.x_max) * 0.5f, w.ty + w.d * (b.y_min + b.y_max) * 0.5f };
}

std::optional<Vector2> Bf3UiScreens::clip_centre(std::string_view p_partition, std::string_view p_path) {
	const std::optional<ui::SwfPoint> at = clip_stage_centre(p_partition, p_path);
	const Entry *e = at ? find(p_partition) : nullptr;
	const Bf3SwfView *v = e != nullptr ? view_of(*e) : nullptr;
	if (v == nullptr) {
		return std::nullopt;
	}
	return v->stage_to_canvas(*at);
}

bool Bf3UiScreens::mouse_wheel(Vector2 p_viewport, float p_delta) {
	++stats_.mouse;
	Entry *t = input_target();
	ui::HudScreen *s = t != nullptr ? screen_of(*t) : nullptr;
	const std::optional<Vector2> at = t != nullptr ? stage_point(*t, p_viewport) : std::nullopt;
	return s != nullptr && at && s->mouse_wheel(at->x, at->y, p_delta);
}

}

void openbf3_register_native_screens() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiScreens);
}
