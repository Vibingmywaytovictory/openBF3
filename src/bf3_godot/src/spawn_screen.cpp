#include "spawn_screen.h"

#include "godot_util.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

Ref<StyleBoxFlat> flat(const Color &p_fill, const Color &p_border, int p_border_px) {
	Ref<StyleBoxFlat> b;
	b.instantiate();
	b->set_bg_color(p_fill);
	b->set_border_color(p_border);
	b->set_border_width_all(p_border_px);
	return b;
}

const Color kDeployFill(0.10f, 0.32f, 0.42f, 0.92f);
const Color kDeployHover(0.16f, 0.45f, 0.58f, 0.95f);
const Color kDeployDisabled(0.12f, 0.14f, 0.15f, 0.85f);
const Color kReviveColour(0.45f, 0.95f, 0.55f, 1.0f);

}

void Bf3TdmSpawnScreen::build_screen() {
	for (size_t k = 0; k < kits_.size(); ++k) {
		Button *b = memnew(Button);
		b->set_name(to_godot("Kit" + std::to_string(k)));
		b->set_focus_mode(FOCUS_NONE);
		b->set_flat(true);
		b->set_mouse_filter(MOUSE_FILTER_STOP);
		b->connect("pressed", callable_mp(this, &Bf3TdmSpawnScreen::on_kit).bind(int64_t(k)));
		add_child(b);
		kits_[k] = b;
	}
	deploy_ = memnew(Button);
	deploy_->set_name("Deploy");
	deploy_->set_focus_mode(FOCUS_NONE);
	deploy_->set_mouse_filter(MOUSE_FILTER_STOP);
	deploy_->connect("pressed", callable_mp(this, &Bf3TdmSpawnScreen::on_deploy));
	add_child(deploy_);
}

void Bf3TdmSpawnScreen::on_kit(int64_t p_kit) {
	if (is_visible_in_tree() && p_kit >= 0 && p_kit < int64_t(kits_.size())) {
		pending_kit_ = int(p_kit);
	}
}

void Bf3TdmSpawnScreen::on_deploy() {
	if (is_visible_in_tree() && ctx_ != nullptr && ctx_->model.spawn_screen.can_deploy) {
		pending_deploy_ = true;
	}
}

int Bf3TdmSpawnScreen::take_kit_click() {
	const int k = pending_kit_;
	pending_kit_ = -1;
	return k;
}

bool Bf3TdmSpawnScreen::take_deploy_click() {
	const bool d = pending_deploy_;
	pending_deploy_ = false;
	return d;
}

Rect2 Bf3TdmSpawnScreen::card_rect(size_t p_kit) const {
	const TdmPlacement p = ctx_->placement(kTdmKitBarPlacement);
	const float s = tdm_scale(*this);
	const Vector2 tl = tdm_at(*this, p.anchor, p.x + float(p_kit) * (kSpawnCardWidth + kSpawnCardGap), p.y);
	return Rect2(tl, Vector2(kSpawnCardWidth * s, kSpawnCardHeight * s));
}

Rect2 Bf3TdmSpawnScreen::deploy_rect() const {
	const float s = tdm_scale(*this);
	return Rect2(tdm_at(*this, ui::HudAnchor::BottomRight, kSpawnDeployLeft, kSpawnDeployTop), Vector2(kSpawnDeployWidth * s, kSpawnDeployHeight * s));
}

void Bf3TdmSpawnScreen::style_buttons() {
	const float s = tdm_scale(*this);
	const int32_t size = tdm_px(20.0f, s);
	if (styled_ && size == styled_size_) {
		return;
	}
	styled_ = true;
	styled_size_ = size;
	const int border = std::max(1, int(std::lround(2.0f * s)));
	const Ref<StyleBoxEmpty> none = memnew(StyleBoxEmpty);
	for (Button *b : kits_) {
		b->add_theme_stylebox_override("normal", none);
		b->add_theme_stylebox_override("focus", none);
		b->add_theme_stylebox_override("hover", flat(Color(1.0f, 1.0f, 1.0f, 0.06f), kTdmTextColour, border));
		b->add_theme_stylebox_override("pressed", flat(Color(1.0f, 1.0f, 1.0f, 0.12f), kTdmLocalColour, border));
		b->add_theme_stylebox_override("hover_pressed", flat(Color(1.0f, 1.0f, 1.0f, 0.12f), kTdmLocalColour, border));
	}
	deploy_->add_theme_stylebox_override("normal", flat(kDeployFill, kTdmFriendlyColour, border));
	deploy_->add_theme_stylebox_override("hover", flat(kDeployHover, kTdmTextColour, border));
	deploy_->add_theme_stylebox_override("pressed", flat(kDeployHover, kTdmLocalColour, border));
	deploy_->add_theme_stylebox_override("hover_pressed", flat(kDeployHover, kTdmLocalColour, border));
	deploy_->add_theme_stylebox_override("disabled", flat(kDeployDisabled, kTdmLineColour, border));
	deploy_->add_theme_stylebox_override("focus", none);
	deploy_->add_theme_font_override("font", ctx_->font(TdmFont::Title));
	deploy_->add_theme_font_size_override("font_size", size);
	deploy_->add_theme_color_override("font_color", kTdmTextColour);
	deploy_->add_theme_color_override("font_hover_color", kTdmTextColour);
	deploy_->add_theme_color_override("font_pressed_color", kTdmLocalColour);
	deploy_->add_theme_color_override("font_disabled_color", kTdmDimColour);
}

void Bf3TdmSpawnScreen::update_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudSpawnScreen &sp = c.model.spawn_screen;
	TdmHudRecords &rec = c.records;
	const bool v = sp.visible;
	rec.spawn_screen_visible = v;
	rec.deploy_enabled = v && sp.can_deploy;
	rec.selected_kit = sp.selected_kit;
	if (v && !was_visible_) {
		++rec.spawn_screen_shown;
		pending_kit_ = -1;
		pending_deploy_ = false;
	}
	was_visible_ = v;
	show_if(v);
	if (!v || deploy_ == nullptr) {
		return;
	}
	if (c.sources_done()) {
		style_buttons();
	}
	for (size_t k = 0; k < kits_.size(); ++k) {
		const Rect2 r = card_rect(k);
		kits_[k]->set_position(r.position);
		kits_[k]->set_size(r.size);
	}
	const Rect2 d = deploy_rect();
	deploy_->set_position(d.position);
	deploy_->set_size(d.size);
	deploy_->set_disabled(!sp.can_deploy);
	std::string text = c.text(TdmText::Deploy);
	if (!sp.can_deploy) {
		text = c.text(TdmText::TimeToDeploy);
		if (!sp.at_join && sp.seconds_to_deploy > 0.0f) {
			text += "  " + std::to_string(int(std::ceil(sp.seconds_to_deploy)));
		}
	}
	if (text != deploy_text_) {
		deploy_text_ = text;
		deploy_->set_text(to_godot(text));
	}
}

void Bf3TdmSpawnScreen::draw_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudModel &m = c.model;
	const ui::HudSpawnScreen &sp = m.spawn_screen;
	const float s = tdm_scale(*this);
	draw_rect(Rect2(Vector2(), get_size()), kTdmDimBackColour, true);
	const Ref<Font> title = c.font(TdmFont::Title);
	const Ref<Font> text = c.font(TdmFont::Text);
	const Ref<Font> num = c.font(TdmFont::Number);

	const Rect2 first = card_rect(0);
	tdm_draw_text(*this, title, first.position + Vector2(0.0f, -12.0f * s), c.text(TdmText::DeployAs), tdm_px(18.0f, s), kTdmTextColour);
	for (size_t k = 0; k < kits_.size(); ++k) {
		const Rect2 r = card_rect(k);
		const bool selected = sp.selected_kit == k;
		tdm_draw_panel(*this, r, selected ? Color(0.10f, 0.22f, 0.28f, 0.92f) : kTdmPanelColour, selected ? kTdmLocalColour : kTdmLineColour,
				std::max(1.0f, (selected ? 2.0f : 1.0f) * s));
		const Ref<Texture2D> icon = c.kit_icon(int(k));
		const float isz = 24.0f * s;
		const Vector2 icon_at = r.position + Vector2(8.0f * s, 6.0f * s);
		if (icon.is_valid()) {
			draw_texture_rect(icon, Rect2(icon_at, Vector2(isz, isz)), false, selected ? kTdmLocalColour : kTdmTextColour);
		}
		tdm_draw_text(*this, title, icon_at + Vector2(isz + 6.0f * s, 19.0f * s), c.text(kTdmKitNames[k]), tdm_px(15.0f, s),
				selected ? kTdmLocalColour : kTdmTextColour);
		const std::array<uint32_t, ui::kHudSpawnKitItems> &items = sp.kit_items[k];
		size_t row = 0;
		for (size_t i = 0; i < items.size(); ++i) {
			const Ref<Texture2D> w = c.weapon_icon(items[i]);
			if (w.is_null()) {
				continue;
			}
			const Vector2 at = r.position + Vector2(12.0f * s + float(row % 2) * 88.0f * s, 40.0f * s + float(row / 2) * 26.0f * s);
			draw_texture_rect(w, Rect2(at, Vector2(64.0f * s, 16.0f * s)), false);
			++row;
		}
	}

	const TdmPlacement cp = c.placement(kTdmSpawnCounterPlacement);
	const ui::HudKillCounter &kc = m.kill_counter;
	if (kc.team1_max_kills > 0) {
		const Vector2 at = tdm_at(*this, cp.anchor, cp.x, cp.y + 26.0f);
		const bool us = kc.player_team != 2;
		const std::string line_f = c.text(us ? TdmText::TeamUs : TdmText::TeamRu) + "  " + std::to_string(kc.player_team_kills);
		const std::string line_e = std::to_string(kc.enemy_team_kills) + "  " + c.text(us ? TdmText::TeamRu : TdmText::TeamUs);
		tdm_draw_text(*this, num, at - Vector2(30.0f * s, 0.0f), line_f, tdm_px(22.0f, s), kTdmFriendlyColour, 1);
		tdm_draw_text(*this, num, at, "/ " + std::to_string(kc.team1_max_kills), tdm_px(14.0f, s), kTdmDimColour, 2);
		tdm_draw_text(*this, num, at + Vector2(30.0f * s, 0.0f), line_e, tdm_px(22.0f, s), kTdmEnemyColour, 0);
	}

	if (sp.revive_possible && !sp.at_join) {
		const Rect2 d = deploy_rect();
		const std::string line = std::string(kTdmOwnTexts.revive_possible) + "  " + std::to_string(int(std::ceil(sp.seconds_revive_left)));
		tdm_draw_text(*this, text, Vector2(d.position.x + d.size.x, d.position.y - 10.0f * s), line, tdm_px(15.0f, s), kReviveColour, 1);
	}
}

}
