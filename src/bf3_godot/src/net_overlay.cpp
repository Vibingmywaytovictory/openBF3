#include "net_overlay.h"

#include "godot_util.h"
#include "native_screens.h"
#include "ui_assets.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

std::vector<ui::DebugFieldText> net_overlay_fields(
		const bf3::host::NetStats &p_net, const std::optional<bf3::audio::SoundFateTotals> &p_sounds) {
	std::vector<ui::DebugFieldText> out;
	for (const bf3::host::NetOverlayRow &r : bf3::host::net_overlay_rows(p_net)) {
		out.push_back({ r.label, r.value });
	}
	const auto count = [&](uint64_t p_n) {
		return p_sounds ? bf3::core::fmt("%llu", (unsigned long long)p_n) : std::string("no audio");
	};
	out.push_back({ "SOUNDS DROPPED", count(p_sounds ? p_sounds->other_drops() : 0) });
	out.push_back({ "DROPPED BY DATA", count(p_sounds ? p_sounds->data_drops() : 0) });
	out.push_back({ "SOUNDS REQUESTED", count(p_sounds ? p_sounds->requested() : 0) });
	return out;
}

NetOverlay::~NetOverlay() {
	if (Object *o = layer_ != nullptr ? ObjectDB::get_instance(layer_id_) : nullptr) {
		memdelete(Object::cast_to<CanvasLayer>(o));
	}
	layer_ = nullptr;
}

Bf3UiScreens *NetOverlay::screens() const {
	return screens_id_ != 0 ? Object::cast_to<Bf3UiScreens>(ObjectDB::get_instance(screens_id_)) : nullptr;
}

ui::HudScreen *NetOverlay::screen() const {
	Bf3UiScreens *s = screens();
	return s != nullptr && opened_ ? s->hud_screen(ui::kDebugScreenMovie) : nullptr;
}

void NetOverlay::attach(Node *p_parent, bool p_shown) {
	if (layer_ != nullptr || p_parent == nullptr) {
		return;
	}
	layer_ = memnew(CanvasLayer);
	layer_->set_name("NetOverlay");
	layer_->set_layer(kNetOverlayLayer);
	layer_id_ = layer_->get_instance_id();
	Bf3UiScreens *s = memnew(Bf3UiScreens);
	s->set_name("NetOverlayScreens");
	s->set_manual(true);
	layer_->add_child(s);
	screens_id_ = s->get_instance_id();
	p_parent->add_child(layer_);
	shown_ = p_shown;
	layer_->set_visible(shown_);
}

void NetOverlay::toggle() {
	shown_ = !shown_;
	if (layer_ != nullptr) {
		layer_->set_visible(shown_);
	}
	UtilityFunctions::print(to_godot(std::string("[net] overlay ") + (shown_ ? "shown" : "hidden")));
}

void NetOverlay::try_open(const bf3::host::NetStats &p_net, const SoundFatesSource &p_sounds) {
	Bf3UiAssets *assets = Bf3UiAssets::current();
	Bf3UiScreens *s = screens();
	if (s == nullptr || assets == nullptr || !assets->is_loaded() || !error_.empty()) {
		return;
	}
	fields_ = net_overlay_fields(p_net, p_sounds ? p_sounds() : std::nullopt);
	s->set_drawn(ui::kDebugScreenMovie, true);
	if (const bf3::core::Status st = s->open_asset_screen(ui::debug_screen_asset(fields_)); !st) {
		error_ = st.error().message;
		UtilityFunctions::print(to_godot("[net] no overlay: " + error_));
		return;
	}
	opened_ = true;
	UtilityFunctions::print(to_godot(bf3::core::fmt("[net] overlay: %s with %zu of its %zu DebugFields, %s",
			std::string(ui::kDebugScreenMovie).c_str(), fields_.size(), ui::kDebugScreenFields,
			shown_ ? "shown" : "hidden (F9 or --net-overlay shows it)")));
}

void NetOverlay::update(double p_now_s, const bf3::host::NetStats &p_net, const SoundFatesSource &p_sounds) {
	if (layer_ == nullptr || !shown_) {
		return;
	}
	if (!opened_) {
		try_open(p_net, p_sounds);
	}
	ui::HudScreen *hud = screen();
	if (hud == nullptr) {
		return;
	}
	if (p_net.revision != revision_) {
		revision_ = p_net.revision;
		fields_ = net_overlay_fields(p_net, p_sounds ? p_sounds() : std::nullopt);
		ui::debug_screen_show(*hud, fields_);
	}
	if (stepped_s_ >= 0.0 && p_now_s - stepped_s_ < kNetOverlayStepSeconds) {
		return;
	}
	const double dt = stepped_s_ < 0.0 ? 0.0 : std::clamp(p_now_s - stepped_s_, 0.0, 1.0);
	stepped_s_ = p_now_s;
	if (Bf3UiScreens *s = screens()) {
		s->step(dt);
	}
}

}
