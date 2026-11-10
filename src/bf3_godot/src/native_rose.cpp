#include "native_rose.h"

#include "swf_view.h"
#include "ui_assets.h"

#include <array>
#include <string_view>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

constexpr std::array<std::string_view, 12> kRoseClips{ "L0", "L1", "L2", "L3", "L4", "R0", "R1", "R2", "R3", "R4", "M",
	"D" };
constexpr int32_t kRoseMiddle = 10;
constexpr int32_t kRoseResetEvent = 1;

}

godot::Vector2 rose_button_position(const ui::As2Clip &p_button) {
	const ui::As2Clip bg = p_button.child("bg");
	return godot::Vector2(p_button.x() + bg.x(), p_button.y() + bg.y());
}

std::optional<int32_t> rose_button_at(
		const std::array<godot::Vector2, 12> &p_buttons, std::array<bool, 12> p_shown, godot::Vector2 p_cursor) {
	if (p_cursor.length() < kRoseDeadZone) {
		return kRoseMiddle;
	}
	const godot::Vector2 dir = p_cursor.normalized();
	std::optional<int32_t> best;
	float best_cos = -2.0f;
	for (int32_t i = 0; i < int32_t(p_buttons.size()); ++i) {
		const godot::Vector2 b = p_buttons[size_t(i)] - p_buttons[size_t(kRoseMiddle)];
		if (i == kRoseMiddle || !p_shown[size_t(i)] || b.length() <= 0.0f) {
			continue;
		}
		const float c = dir.dot(b.normalized());
		if (c > best_cos) {
			best_cos = c;
			best = i;
		}
	}
	return best;
}

bf3::core::Result<std::unique_ptr<NativeRose>> NativeRose::open(godot::Node &r_parent,
		const ui::HudScreenAsset &p_asset, ui::CommoRoseLayout p_layout, const ui::HudClientState &p_client,
		const ui::HudStageRects &p_rects) {
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || assets->movies() == nullptr) {
		return bf3::core::make_error(p_asset.partition + ": no Bf3UiAssets movies");
	}
	std::unique_ptr<NativeRose> out(new NativeRose());
	out->layout_ = std::move(p_layout);
	Bf3SwfView *v = memnew(Bf3SwfView);
	v->set_name("CommoRose");
	v->set_playing(false);
	v->set_skips_hidden_render(true);
	r_parent.add_child(v);
	out->view_ = v;
	if (const bf3::core::Status st = v->open(p_asset.movie); !st) {
		return bf3::core::make_error(p_asset.movie + ": " + st.error().message);
	}
	ui::HudStageRects rects = p_rects;
	rects.original = v->scene()->stage();
	bf3::core::Result<std::unique_ptr<ui::HudScreen>> s =
			ui::HudScreen::open(*v->scene(), *assets->movies(), p_asset, assets->swf_fonts(), p_client, rects);
	if (!s) {
		return bf3::core::make_error(p_asset.name + ": " + s.error().message);
	}
	out->screen_ = std::move(*s);
	v->set_visible(false);
	return out;
}

NativeRose::~NativeRose() = default;

std::string NativeRose::rose_path() const {
	for (const std::unique_ptr<ui::HudWidgetState> &w : screen_->widgets()) {
		if (w->class_name == "Widget.Hud.ComRose") {
			return w->path;
		}
	}
	return {};
}

std::optional<godot::Vector2> NativeRose::button_offset(int32_t p_button) const {
	const std::string path = rose_path();
	if (path.empty() || p_button < 0 || p_button >= int32_t(kRoseClips.size())) {
		return std::nullopt;
	}
	const ui::As2Clip rose = screen_->clip(path).child("ComRose");
	const ui::As2Clip b = rose.child(kRoseClips[size_t(p_button)]);
	const ui::As2Clip m = rose.child(kRoseClips[size_t(kRoseMiddle)]);
	if (!b.exists() || !m.exists()) {
		return std::nullopt;
	}
	return rose_button_position(b) - rose_button_position(m);
}

const ui::CommoRoseEntry *NativeRose::selected_entry() const {
	for (const ui::CommoRoseEntry &e : layout_.entries) {
		if (e.button == selected_) {
			return &e;
		}
	}
	return nullptr;
}

void NativeRose::step(
		const ui::HudModel &p_model, const NativeRoseState &p_state, const ui::HudStageRects &p_rects, double p_dt) {
	if (screen_ == nullptr || view_ == nullptr) {
		return;
	}
	ui::HudStageRects rects = p_rects;
	rects.original = view_->scene()->stage();
	screen_->set_stage_rects(rects);
	model_ = p_model;
	model_.commo_rose.in_vehicle = p_state.in_vehicle;
	model_.commo_rose.alive = p_state.alive;
	model_.commo_rose.leader = p_state.leader;
	if (!initialized_) {
		model_.commo_rose.selected_button = -1;
		screen_->initialize(model_);
		initialized_ = true;
	}
	if (p_state.open && !shown_) {
		shown_ = true;
		++opens_;
		selected_ = -1;
		const std::string path = rose_path();
		if (ui::HudWidgetController *c = screen_->controller(path)) {
			c->in_event(*screen_->host(path), kRoseResetEvent, ui::As2Value::of(0));
		}
		view_->set_visible(true);
	} else if (!p_state.open && shown_) {
		shown_ = false;
		view_->set_visible(false);
	}
	if (!shown_) {
		return;
	}
	const std::string path = rose_path();
	std::array<godot::Vector2, 12> buttons{};
	std::array<bool, 12> visible{};
	if (!path.empty()) {
		const ui::As2Clip rose = screen_->clip(path).child("ComRose");
		for (size_t i = 0; i < kRoseClips.size(); ++i) {
			const ui::As2Clip b = rose.child(kRoseClips[i]);
			buttons[i] = rose_button_position(b);
			visible[i] = b.exists() && b.visible();
		}
	}
	selected_ = rose_button_at(buttons, visible, p_state.cursor).value_or(-1);
	model_.commo_rose.selected_button = selected_;
	screen_->refresh(model_);
	screen_->advance(p_dt);
	(void)view_->scene()->advance_seconds(p_dt);
}

}
