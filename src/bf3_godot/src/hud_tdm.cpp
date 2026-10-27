#include "hud_tdm.h"

#include "hud_items.h"
#include "hud_scoreboard.h"
#include "hud_tdm_screen.h"
#include "pre_round.h"
#include "round_end.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "ui_assets.h"

#include "bf3/core/ui/text_db.h"
#include "bf3/host/hud_feed.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;
namespace host = bf3::host;

namespace {

constexpr uint32_t kHudSearchFrames = 120;

}

void TdmHudContext::resolve(Node *p_tree_node) {
	if (!text_done_) {
		resolve_text();
	}
	if (!assets_done_) {
		resolve_assets();
	}
	if (weapon_hud_id_ == 0 && p_tree_node != nullptr && p_tree_node->is_inside_tree()) {
		if (hud_search_countdown_ > 0) {
			--hud_search_countdown_;
		} else {
			hud_search_countdown_ = kHudSearchFrames;
			Window *root = p_tree_node->get_tree() != nullptr ? p_tree_node->get_tree()->get_root() : nullptr;
			if (root != nullptr) {
				const TypedArray<Node> found = root->find_children("*", "Bf3HudItems", true, false);
				if (!found.is_empty()) {
					if (Node *n = Object::cast_to<Node>(found[0])) {
						weapon_hud_id_ = n->get_instance_id();
					}
				}
			}
		}
	}
}

void TdmHudContext::resolve_text() {
	if (localisation == nullptr) {
		localisation = Bf3Localisation::current();
	}
	if (localisation == nullptr || (!localisation->is_loaded() && !localisation->has_failed())) {
		for (size_t i = 0; i < kTdmTextCount; ++i) {
			texts_[i] = std::string(kTdmTexts[i].sid);
		}
		return;
	}
	text_done_ = true;
	const std::shared_ptr<const ui::TextDatabase> db = localisation->database();
	for (size_t i = 0; i < kTdmTextCount; ++i) {
		const std::string_view sid = kTdmTexts[i].sid;
		const std::optional<std::string_view> t = db != nullptr ? db->find_sid(sid) : std::nullopt;
		if (t && !t->empty() && *t != "<MISSING>") {
			texts_[i] = std::string(*t);
		} else {
			texts_[i] = std::string(sid);
			++records.missing_strings;
			UtilityFunctions::push_warning(to_godot("Bf3TdmHud: string " + std::string(sid) + " is missing (drawn as the SID)"));
		}
	}

	std::string &hs = texts_[size_t(TdmText::Headshot)];
	const size_t arg = hs.find("{0:s}");
	if (arg != std::string::npos) {
		hs.erase(arg, 5);
		while (!hs.empty() && hs.back() == ' ') {
			hs.pop_back();
		}
	}
}

void TdmHudContext::resolve_assets() {
	if (assets == nullptr) {
		assets = Bf3UiAssets::current();
	}
	if (assets == nullptr || (!assets->is_loaded() && !assets->has_failed())) {
		return;
	}
	assets_done_ = true;
	for (size_t i = 0; i < kTdmFonts.size(); ++i) {
		const String name = to_godot(std::string(kTdmFonts[i]));
		const Ref<FontFile> file = assets->get_font_file(name);
		if (file.is_null()) {
			++records.missing_fonts;
			UtilityFunctions::push_warning(to_godot("Bf3TdmHud: font " + std::string(kTdmFonts[i]) + " is not a BF3 font (a system font is drawn)"));
		}
		fonts_[i] = assets->get_font(name);
	}
	const String movie = to_godot(std::string(kTdmIconMovie));
	for (size_t k = 0; k < kit_icons_.size(); ++k) {
		kit_icons_[k] = assets->get_region(movie, to_godot(std::string(kTdmKitIcons[k])));
		if (kit_icons_[k].is_null()) {
			++records.missing_icons;
		}
	}
	dead_icon_ = assets->get_region(movie, to_godot(std::string(kTdmDeadIcon)));
	if (dead_icon_.is_null()) {
		++records.missing_icons;
	}
	for (TdmPlacement &p : placements_) {
		const std::vector<ui::HudPlacement> *rows = assets->hud_placements(std::string(p.movie));
		bool found = false;
		if (rows != nullptr) {
			for (const ui::HudPlacement &r : *rows) {
				if (r.instance == p.instance) {
					p.x = r.matrix.tx;
					p.y = r.matrix.ty;
					p.anchor = r.anchor;
					found = true;
					break;
				}
			}
		}
		if (!found) {
			UtilityFunctions::push_warning(to_godot("Bf3TdmHud: no placement " + std::string(p.movie) + " " + std::string(p.instance) + " (the retail value is used)"));
		}
	}
}

Ref<Font> TdmHudContext::font(TdmFont p_font) const {
	const Ref<Font> &f = fonts_[size_t(p_font)];
	if (f.is_valid()) {
		return f;
	}
	return ThemeDB::get_singleton()->get_fallback_font();
}

const std::string &TdmHudContext::text(TdmText p_key) const {
	static const std::string kEmpty;
	return size_t(p_key) < texts_.size() ? texts_[size_t(p_key)] : kEmpty;
}

const std::string &TdmHudContext::localised(uint32_t p_sid_hash) {
	static const std::string kEmpty;
	if (p_sid_hash == 0 || !text_done_) {
		return kEmpty;
	}
	auto it = names_.find(p_sid_hash);
	if (it != names_.end()) {
		return it->second;
	}
	const std::shared_ptr<const ui::TextDatabase> db = localisation != nullptr ? localisation->database() : nullptr;
	const std::optional<std::string_view> t = db != nullptr ? db->find(p_sid_hash) : std::nullopt;
	std::string value = t && *t != "<MISSING>" ? std::string(*t) : std::string();
	if (value.empty()) {
		++records.missing_strings;
		char hex[16];
		std::snprintf(hex, sizeof(hex), "%08x", p_sid_hash);
		UtilityFunctions::push_warning(to_godot(std::string("Bf3TdmHud: weapon name ") + hex + " is missing"));
	}
	return names_.emplace(p_sid_hash, std::move(value)).first->second;
}

Ref<Texture2D> TdmHudContext::kit_icon(int p_kit) const {
	return p_kit >= 0 && size_t(p_kit) < kit_icons_.size() ? kit_icons_[size_t(p_kit)] : Ref<Texture2D>();
}

Ref<Texture2D> TdmHudContext::weapon_icon(uint32_t p_item_id) {
	if (p_item_id == 0) {
		return Ref<Texture2D>();
	}
	Bf3HudItems *hud = weapon_hud_id_ != 0 ? Object::cast_to<Bf3HudItems>(ObjectDB::get_instance(weapon_hud_id_)) : nullptr;
	if (hud == nullptr || !hud->is_loaded()) {
		if (weapon_hud_id_ != 0 && hud == nullptr) {
			weapon_hud_id_ = 0;
		}
		return Ref<Texture2D>();
	}
	const Ref<Texture2D> icon = hud->icon(p_item_id);
	if (icon.is_null() && missing_items_.insert(p_item_id).second) {
		++records.missing_icons;
		UtilityFunctions::push_warning(to_godot("Bf3TdmHud: no _small icon for item " + std::to_string(p_item_id)));
	}
	return icon;
}

TdmPlacement TdmHudContext::placement(const TdmPlacement &p_fallback) const {
	for (const TdmPlacement &p : placements_) {
		if (p.movie == p_fallback.movie && p.instance == p_fallback.instance) {
			return p;
		}
	}
	return p_fallback;
}

double TdmHudContext::now_s() {
	return double(Time::get_singleton()->get_ticks_usec()) * 1e-6;
}

float tdm_scale(const Control &p_control) {
	const Vector2 s = p_control.get_size();
	return host::hud_scale(s.x, s.y);
}

Vector2 tdm_at(const Control &p_control, ui::HudAnchor p_anchor, float p_stage_x, float p_stage_y) {
	const Vector2 s = p_control.get_size();
	const host::HudPoint p = host::hud_stage_to_window(p_anchor, p_stage_x, p_stage_y, s.x, s.y);
	return Vector2(p.x, p.y);
}

int32_t tdm_px(float p_stage_px, float p_scale) {
	return std::max(1, int32_t(std::lround(p_stage_px * p_scale)));
}

float tdm_text_width(const Ref<Font> &p_font, const std::string &p_text, int32_t p_size) {
	if (p_font.is_null() || p_text.empty()) {
		return 0.0f;
	}
	return float(p_font->get_string_size(to_godot(p_text), HORIZONTAL_ALIGNMENT_LEFT, -1, p_size).x);
}

float tdm_draw_text(CanvasItem &p_item, const Ref<Font> &p_font, Vector2 p_at, const std::string &p_text, int32_t p_size, const Color &p_colour,
		int p_align) {
	if (p_font.is_null() || p_text.empty()) {
		return 0.0f;
	}
	const String text = to_godot(p_text);
	const float w = float(p_font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size).x);
	float x = p_at.x;
	if (p_align == 1) {
		x -= w;
	} else if (p_align == 2) {
		x -= 0.5f * w;
	}
	const float shadow = std::max(1.0f, std::round(float(p_size) / 16.0f));
	Color sh = kTdmShadowColour;
	sh.a *= p_colour.a;
	p_item.draw_string(p_font, Vector2(x + shadow, p_at.y + shadow), text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size, sh);
	p_item.draw_string(p_font, Vector2(x, p_at.y), text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size, p_colour);
	return w;
}

void tdm_draw_panel(CanvasItem &p_item, const Rect2 &p_rect, const Color &p_fill, const Color &p_line, float p_line_width) {
	p_item.draw_rect(p_rect, p_fill, true);
	if (p_line_width > 0.0f && p_line.a > 0.0f) {
		p_item.draw_rect(p_rect, p_line, false, p_line_width);
	}
}

std::string tdm_player_name(const ui::HudModel &p_model, uint8_t p_slot) {
	if (p_slot >= ui::kHudMaxPlayers) {
		return std::string();
	}
	const ui::HudPlayerName &n = p_model.players[p_slot].name;
	const size_t len = size_t(std::find(n.begin(), n.end(), '\0') - n.begin());
	if (len == 0) {
		return "#" + std::to_string(int(p_slot));
	}
	return std::string(n.data(), len);
}

Color tdm_player_colour(const ui::HudModel &p_model, uint8_t p_slot) {
	if (p_slot >= ui::kHudMaxPlayers) {
		return kTdmTextColour;
	}
	if (p_slot == p_model.local_player) {
		return kTdmLocalColour;
	}
	const int friendly = p_model.team.friendly_team != 0 ? p_model.team.friendly_team : 1;
	return p_model.players[p_slot].team == friendly ? kTdmFriendlyColour : kTdmEnemyColour;
}

void Bf3TdmScreen::_notification(int p_what) {
	if (p_what == NOTIFICATION_DRAW && ctx_ != nullptr) {
		draw_screen();
	}
}

void Bf3TdmScreen::show_if(bool p_visible) {
	if (is_visible() != p_visible) {
		set_visible(p_visible);
	}
	if (p_visible) {
		queue_redraw();
	}
}

struct Bf3TdmHud::Impl {
	TdmHudContext ctx;
	uint64_t root_id = 0;
	Control *root = nullptr;
	Bf3TdmScoreboard *scoreboard = nullptr;
	Bf3TdmPreRound *pre_round = nullptr;
	Bf3TdmRoundEnd *round_end = nullptr;

	bool alive() const { return root != nullptr && ObjectDB::get_instance(root_id) != nullptr; }
	template <typename F>
	void each(F &&p_f) {
		Bf3TdmScreen *screens[] = { pre_round, round_end, scoreboard };
		for (Bf3TdmScreen *s : screens) {
			if (s != nullptr) {
				p_f(*s);
			}
		}
	}
};

Bf3TdmHud::Bf3TdmHud() : impl_(std::make_unique<Impl>()) {}

Bf3TdmHud::~Bf3TdmHud() {
	shutdown();
}

bool Bf3TdmHud::build(Node *p_parent, Bf3Localisation *p_localisation, Bf3UiAssets *p_ui_assets, std::string *r_error) {
	if (impl_->alive()) {
		return true;
	}
	if (p_parent == nullptr) {
		if (r_error != nullptr) {
			*r_error = "Bf3TdmHud: no parent node";
		}
		return false;
	}
	Impl &m = *impl_;
	m.ctx.localisation = p_localisation;
	m.ctx.assets = p_ui_assets;
	m.root = memnew(Control);
	m.root->set_name("Bf3TdmHud");
	m.root->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	m.root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	m.pre_round = memnew(Bf3TdmPreRound);
	m.round_end = memnew(Bf3TdmRoundEnd);
	m.scoreboard = memnew(Bf3TdmScoreboard);
	const char *names[] = { "PreRound", "RoundEnd", "Scoreboard" };
	size_t i = 0;
	m.each([&](Bf3TdmScreen &s) {
		s.set_name(names[i++]);
		s.set_context(&m.ctx);
		s.set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		s.set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		s.set_visible(false);
		m.root->add_child(&s);
	});
	p_parent->add_child(m.root);
	m.root_id = m.root->get_instance_id();
	m.each([&](Bf3TdmScreen &s) { s.build_screen(); });
	m.ctx.resolve(m.root);
	UtilityFunctions::print("Bf3TdmHud: built (scoreboard, pre-round, round end)");
	return true;
}

bool Bf3TdmHud::built() const {
	return impl_->alive();
}

void Bf3TdmHud::set_model(const ui::HudModel &p_model) {
	Impl &m = *impl_;
	if (!m.alive()) {
		return;
	}
	m.ctx.model = p_model;
	m.ctx.resolve(m.root);
	m.ctx.records.model_serial = p_model.serial;
	++m.ctx.records.draws;
	m.each([](Bf3TdmScreen &s) { s.update_screen(); });
}

bool Bf3TdmHud::wants_mouse() const {
	const ui::HudModel &m = impl_->ctx.model;
	return impl_->alive() && m.end_of_round.visible;
}

TdmHudRecords Bf3TdmHud::records() const {
	return impl_->ctx.records;
}

bool Bf3TdmHud::reports(std::string_view p_partition) const {
	return built() && tdm_hud_reports(p_partition);
}

std::optional<UiScreenRecord> Bf3TdmHud::screen_record(std::string_view p_partition) const {
	return built() ? tdm_hud_screen_record(impl_->ctx.records, p_partition) : std::nullopt;
}

std::optional<UiLookupRecord> Bf3TdmHud::lookup_record() const {
	return built() ? std::optional<UiLookupRecord>(tdm_hud_lookup_record(impl_->ctx.records)) : std::nullopt;
}

void Bf3TdmHud::shutdown() {
	if (!impl_) {
		return;
	}
	Impl &m = *impl_;
	if (m.alive()) {
		m.root->queue_free();
	}
	m.root = nullptr;
	m.root_id = 0;
	m.scoreboard = nullptr;
	m.pre_round = nullptr;
	m.round_end = nullptr;
}

}

void openbf3_register_tdm_hud() {
	GDREGISTER_CLASS(bf3::godot_side::Bf3TdmScreen);
	GDREGISTER_CLASS(bf3::godot_side::Bf3TdmScoreboard);
	GDREGISTER_CLASS(bf3::godot_side::Bf3TdmPreRound);
	GDREGISTER_CLASS(bf3::godot_side::Bf3TdmRoundEnd);
}
