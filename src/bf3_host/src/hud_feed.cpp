// The minimal HUD's feed and layout (bf3/host/hud_feed.h; docs/design/m2a.md §11.4, docs/design/m2a-hud.md).
// Godot-free and pure: fill_hud writes ui2's HudModel from the presented own weapon and the drained events; the
// layout half maps BF3's 1280 x 720 HUD stage into the window and finds the text slots of the hudmpscreen tree.
#include "bf3/host/hud_feed.h"

#include "bf3/core/ui/text_db.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace bf3::host {

namespace ui = bf3::core::ui;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxGapSpreadDeg = 89.0;
constexpr size_t kMaxLayoutDepth = size_t(ui::kSwfLayoutMaxDepth) * 4;

HudCrosshairKind crosshair_kind(const gp::WeaponDef &p_def, const sw::WeaponSimDef &p_sim) {
	if (p_def.weapon_class == "wcKnife") {
		return HudCrosshairKind::Knife;
	}
	if (p_def.weapon_class == "wcHgr") {
		return HudCrosshairKind::Grenade;
	}
	if (p_sim.projectile.kind == gp::ProjectileKind::Missile) {
		return HudCrosshairKind::Launcher;
	}
	if (p_sim.pellets > 1) {
		return HudCrosshairKind::Shotgun;
	}
	if (p_def.anim_base_set == "wabsPstl") {
		return HudCrosshairKind::Pistol;
	}
	return HudCrosshairKind::Rifle;
}

void clear_weapon(ui::HudWeapon &r_w) {
	// Everything fill_hud writes; the fields of later waves (secondary, heat, range, ...) are not touched.
	r_w.active_weapon_data = {};
	r_w.active_weapon_crosshair = 0;
	r_w.active_weapon_ammo = 0;
	r_w.active_weapon_clips_total = 0;
	r_w.active_weapon_clips_left = 0;
	r_w.active_weapon_reload_percent = 0;
	r_w.active_weapon_ammo_in_bag = 0;
	r_w.active_weapon_clip_capacity = 0;
	r_w.active_weapon_critical_ammo = false;
	r_w.active_weapon_is_reloading = false;
	r_w.active_weapon_ammo_total = 0;
	r_w.active_weapon_name = 0;
	r_w.active_weapon_zoomed = false;
	r_w.active_weapon_index = 0;
	r_w.dispersion = 0;
	r_w.absolute_dispersion = 0;
	r_w.zoomed = false;
	r_w.firing_mode = int32_t(HudFireMode::None);
	r_w.firing_mode_types = 0;
	r_w.active_weapon_show_ammo = false;
	r_w.show_crosshair = false;
	r_w.can_fire = false;
}

// Child of p_node with instance name p_name (the first in display order), or null.
const ui::SwfLayoutNode *child_named(const ui::SwfLayoutNode &p_node, std::string_view p_name, gp::WorkBudget &r_budget) {
	for (const ui::SwfLayoutNode &c : p_node.children) {
		if (!r_budget.charge(1)) {
			return nullptr;
		}
		if (c.instance == p_name) {
			return &c;
		}
	}
	return nullptr;
}

const ui::SwfLayoutNode *find_path(const ui::SwfLayoutNode &p_node, std::span<const std::string_view> p_path, gp::WorkBudget &r_budget) {
	const ui::SwfLayoutNode *n = &p_node;
	for (const std::string_view name : p_path) {
		n = child_named(*n, name, r_budget);
		if (n == nullptr) {
			return nullptr;
		}
	}
	return n;
}

// The first edit text under p_node (depth first, display order) whose initial text is p_text. Iterative: hostile
// trees may nest deeply; the depth is capped.
const ui::SwfLayoutNode *find_text(const ui::SwfLayoutNode &p_node, std::string_view p_text, gp::WorkBudget &r_budget, bool &r_too_deep) {
	std::vector<std::pair<const ui::SwfLayoutNode *, size_t>> stack; // node, depth
	stack.emplace_back(&p_node, 0);
	while (!stack.empty()) {
		if (!r_budget.charge(1)) {
			return nullptr;
		}
		const auto [n, depth] = stack.back();
		stack.pop_back();
		if (n->kind == ui::SwfLayoutKind::EditText && n->text.initial_text == p_text) {
			return n;
		}
		if (n->children.empty()) {
			continue;
		}
		if (depth + 1 > kMaxLayoutDepth) {
			r_too_deep = true;
			return nullptr;
		}
		for (size_t i = n->children.size(); i-- > 0;) {
			stack.emplace_back(&n->children[i], depth + 1);
		}
	}
	return nullptr;
}

float axis_length(float p_a, float p_b) {
	const float l = std::sqrt(p_a * p_a + p_b * p_b);
	return std::isfinite(l) ? l : 0.0f;
}

HudTextSlot text_slot(const ui::SwfLayoutNode *p_node, std::string p_path) {
	HudTextSlot s;
	s.path = std::move(p_path);
	if (p_node == nullptr || p_node->kind != ui::SwfLayoutKind::EditText) {
		return s;
	}
	s.found = true;
	s.x = p_node->stage.tx;
	s.y = p_node->stage.ty;
	s.scale_x = axis_length(p_node->stage.a, p_node->stage.b);
	s.scale_y = axis_length(p_node->stage.c, p_node->stage.d);
	s.bounds = p_node->text.bounds;
	s.size = p_node->text.size;
	s.align = p_node->text.align;
	s.font = p_node->text.font;
	s.color = p_node->text.color;
	s.initial_text = p_node->text.initial_text;
	return s;
}

HudWidgetSlot widget_slot(const ui::SwfLayoutNode *p_node, std::string_view p_instance) {
	HudWidgetSlot w;
	w.instance = std::string(p_instance);
	if (p_node == nullptr) {
		return w;
	}
	w.found = true;
	w.x = p_node->stage.tx;
	w.y = p_node->stage.ty;
	w.anchor = ui::hud_anchor_for(w.x, w.y, ui::SwfRect{ 0.0f, kHudStageWidth, 0.0f, kHudStageHeight });
	return w;
}

} // namespace

HudFireMode hud_fire_mode(gp::FireLogic p_logic) {
	switch (p_logic) {
		case gp::FireLogic::Single:
		case gp::FireLogic::SingleWithBoltAction:
			return HudFireMode::Single;
		case gp::FireLogic::Burst:
			return HudFireMode::Burst;
		case gp::FireLogic::Automatic:
			return HudFireMode::Auto;
		default:
			return HudFireMode::None;
	}
}

int32_t hud_weapon_class(std::string_view p_weapon_class) {
	for (size_t i = 0; i < kHudWeaponClassNames.size(); ++i) {
		if (kHudWeaponClassNames[i] == p_weapon_class) {
			return int32_t(i + 1);
		}
	}
	return 0;
}

core::Result<HudStatic> make_hud_static(const sw::WeaponTable &p_table, std::span<const gp::WeaponDef> p_defs) {
	if (p_defs.size() != p_table.defs.size()) {
		return core::make_error("hud static: " + std::to_string(p_defs.size()) + " weapon defs for a table of " + std::to_string(p_table.defs.size()));
	}
	HudStatic out;
	out.rows.reserve(p_defs.size());
	for (size_t i = 0; i < p_defs.size(); ++i) {
		const gp::WeaponDef &d = p_defs[i];
		const sw::WeaponSimDef &s = p_table.defs[i];
		if (d.unlock_identifier != s.unlock_identifier) {
			return core::make_error("hud static: def " + std::to_string(i) + " (" + d.variant + ") is not the table's " + s.variant);
		}
		HudStaticRow r;
		r.name_sid_hash = d.name_sid_hash;
		r.item_id = d.unlock_identifier;
		r.weapon_class = hud_weapon_class(d.weapon_class);
		r.clip_capacity = s.magazine_capacity;
		r.magazines = s.magazines;
		r.unlimited_ammo = s.unlimited_ammo;
		const size_t modes = std::min<size_t>(s.mode_count, r.mode_kinds.size());
		for (size_t m = 0; m < modes; ++m) {
			r.mode_kinds[m] = hud_fire_mode(s.modes[m].logic);
			if (r.mode_kinds[m] != HudFireMode::None) {
				r.fire_modes |= 1u << uint32_t(r.mode_kinds[m]);
			}
		}
		r.crosshair = crosshair_kind(d, s);
		r.show_ammo = r.crosshair != HudCrosshairKind::Knife;
		out.rows.push_back(r);
	}
	return out;
}

size_t apply_item_names(HudStatic &r_static, const ui::ItemIconIndex &p_items) {
	size_t filled = 0;
	for (HudStaticRow &r : r_static.rows) {
		if (r.name_sid_hash != 0 || r.item_id == 0) {
			continue;
		}
		const ui::ItemIcon *icon = p_items.find(r.item_id);
		if (icon == nullptr || icon->name_sid.empty()) {
			continue;
		}
		r.name_sid_hash = ui::text_hash(icon->name_sid);
		++filled;
	}
	return filled;
}

void fill_hud(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudFeedState &r_state,
		ui::HudModel &r_model) {
	++r_state.frames;
	for (const PresentEvent &e : p_drained) {
		if (e.kind != PresentKind::Hit) {
			continue;
		}
		if ((e.flags & present_flags::kOwnShooter) != 0) {
			r_state.hit_indicator_until_s = p_input.now_s + kHitIndicatorSeconds;
			++r_state.hits_shown;
		} else {
			++r_state.hits_ignored;
		}
	}

	ui::HudWeapon &w = r_model.weapon;
	clear_weapon(w);
	const PresentedWeapon &pw = p_input.weapon;
	const HudStaticRow *row = pw.valid ? p_static.row(pw.def) : nullptr;
	if (p_input.alive && row != nullptr) {
		const int32_t magazine = std::max(pw.magazine, 0);
		const int32_t reserve = std::max(pw.reserve, 0);
		const int32_t capacity = pw.magazine_capacity > 0 ? pw.magazine_capacity : row->clip_capacity;
		w.active_weapon_data.item_id = row->item_id;
		w.active_weapon_data.name_sid_hash = row->name_sid_hash;
		w.active_weapon_data.weapon_class = row->weapon_class;
		w.active_weapon_data.fire_modes = row->fire_modes;
		w.active_weapon_crosshair = uint32_t(row->crosshair);
		w.active_weapon_ammo = magazine;
		w.active_weapon_ammo_in_bag = reserve;
		w.active_weapon_ammo_total = magazine + reserve;
		w.active_weapon_clip_capacity = capacity;
		w.active_weapon_clips_left = capacity > 0 ? int32_t((int64_t(reserve) + capacity - 1) / capacity) : 0;
		w.active_weapon_clips_total = row->magazines;
		const bool reloading = pw.phase == sw::WeaponPhase::Reloading || pw.phase == sw::WeaponPhase::ReloadShell;
		w.active_weapon_is_reloading = reloading;
		const float progress = std::isfinite(pw.phase_progress) ? std::clamp(pw.phase_progress, 0.0f, 1.0f) : 0.0f;
		w.active_weapon_reload_percent = reloading ? progress * 100.0f : 0.0f;
		w.active_weapon_critical_ammo = row->show_ammo && capacity > 1 && float(magazine) <= float(capacity) * kCriticalAmmoFraction;
		w.active_weapon_show_ammo = row->show_ammo;
		w.active_weapon_name = row->name_sid_hash;
		w.active_weapon_index = pw.active_slot;
		const float spread = std::isfinite(pw.spread_deg) ? std::max(pw.spread_deg, 0.0f) : 0.0f;
		w.dispersion = spread;
		w.absolute_dispersion = spread;
		w.zoomed = pw.zoomed;
		w.active_weapon_zoomed = pw.zoomed;
		w.firing_mode = int32_t(pw.mode < row->mode_kinds.size() ? row->mode_kinds[pw.mode] : HudFireMode::None);
		w.firing_mode_types = row->fire_modes;
		w.can_fire = pw.phase == sw::WeaponPhase::Ready;
		w.show_crosshair = !pw.zoomed && row->crosshair != HudCrosshairKind::None;
	}
	const bool hit = p_input.now_s < r_state.hit_indicator_until_s;
	w.show_hitindicator = hit;
	w.hitindicator_level = hit ? kHitIndicatorLevel : 0.0f;

	ui::HudSoldier &s = r_model.soldier;
	s.is_alive = p_input.alive;
	s.health = std::isfinite(p_input.health) ? std::max(p_input.health, 0.0f) : 0.0f;
	s.pose = int32_t(p_input.pose);
	s.first_person = p_input.alive;
	s.show_hud_ammo = w.active_weapon_show_ammo;
	++r_model.serial;
}

// ---- layout ------------------------------------------------------------------------------------------------------------

float hud_scale(float p_width, float p_height) {
	if (!(p_width > 0.0f) || !(p_height > 0.0f)) {
		return 0.0f;
	}
	return std::min(p_width / kHudStageWidth, p_height / kHudStageHeight);
}

HudPoint hud_stage_to_window(ui::HudAnchor p_anchor, float p_stage_x, float p_stage_y, float p_width, float p_height) {
	const int a = std::clamp(int(p_anchor), 0, 8);
	const float col = float(a % 3) * 0.5f;
	const float row = float(a / 3) * 0.5f;
	const float s = hud_scale(p_width, p_height);
	HudPoint p;
	p.x = col * p_width + (p_stage_x - col * kHudStageWidth) * s;
	p.y = row * p_height + (p_stage_y - row * kHudStageHeight) * s;
	return p;
}

double crosshair_gap_px(double p_height, double p_spread_deg, double p_vfov_deg) {
	if (!(p_height > 0.0) || !std::isfinite(p_height) || !(p_spread_deg >= 0.0) || !std::isfinite(p_spread_deg) || !(p_vfov_deg > 0.0) ||
			!(p_vfov_deg < 180.0)) {
		return 0.0;
	}
	const double spread = std::min(p_spread_deg, kMaxGapSpreadDeg) * kPi / 180.0;
	const double half_fov = p_vfov_deg * 0.5 * kPi / 180.0;
	return p_height * 0.5 * std::tan(spread) / std::tan(half_fov);
}

core::Result<HudScreenLayout> make_hud_screen_layout(const ui::SwfLayoutNode &p_root, gp::WorkBudget &r_budget) {
	HudScreenLayout out;
	const size_t used_before = r_budget.used();
	const ui::SwfLayoutNode *screen = child_named(p_root, "instance1", r_budget);
	BF3_TRY(r_budget.status());
	if (screen == nullptr) {
		return core::make_error("hud layout: the screen tree has no instance1");
	}
	bool too_deep = false;
	const auto widget = [&](std::string_view p_instance) {
		const ui::SwfLayoutNode *n = child_named(*screen, p_instance, r_budget);
		if (n == nullptr) {
			out.missing.push_back(std::string(p_instance));
		}
		return std::pair<const ui::SwfLayoutNode *, HudWidgetSlot>(n, widget_slot(n, p_instance));
	};
	const auto path_text = [&](const ui::SwfLayoutNode *p_widget, std::string_view p_instance, std::initializer_list<std::string_view> p_path) {
		std::string name(p_instance);
		for (const std::string_view part : p_path) {
			name += "/" + std::string(part);
		}
		const ui::SwfLayoutNode *n = p_widget != nullptr ? find_path(*p_widget, std::span<const std::string_view>(p_path.begin(), p_path.size()), r_budget) : nullptr;
		HudTextSlot s = text_slot(n, name);
		if (!s.found) {
			out.missing.push_back(name);
		}
		return s;
	};
	const auto initial_text = [&](const ui::SwfLayoutNode *p_node, std::string_view p_where, std::string_view p_text) {
		const std::string name = std::string(p_where) + "/\"" + std::string(p_text) + "\"";
		const ui::SwfLayoutNode *n = p_node != nullptr ? find_text(*p_node, p_text, r_budget, too_deep) : nullptr;
		HudTextSlot s = text_slot(n, name);
		if (!s.found) {
			out.missing.push_back(name);
		}
		return s;
	};

	auto [ammo, ammo_slot] = widget("Ammo_01");
	auto [health, health_slot] = widget("Health_01");
	auto [hit, hit_slot] = widget("Hitindicator_01");
	(void)hit;
	out.ammo = ammo_slot;
	out.health = health_slot;
	out.hit_indicator = hit_slot;
	out.ammo_clip = path_text(ammo, "Ammo_01", { "Ammo", "clip", "cltxt", "txtDisplay" });
	out.ammo_reserve = path_text(ammo, "Ammo_01", { "Ammo", "amholder", "amtxt", "txtDisplay" });
	out.ammo_slash = initial_text(ammo, "Ammo_01", "/");
	out.health_value = path_text(health, "Health_01", { "Health", "healthHolder", "amtxt", "txtDisplay" });
	out.health_percent = initial_text(health, "Health_01", "%");
	BF3_TRY(r_budget.status());
	if (too_deep) {
		return core::make_error("hud layout: the screen tree nests deeper than " + std::to_string(kMaxLayoutDepth));
	}
	out.nodes = r_budget.used() - used_before;
	return out;
}

HudTextAnchor hud_text_anchor(const HudTextSlot &p_slot, float p_ascent_per_px) {
	// Flash draws an edit text's line inside its bounds with a 2 px gutter on every side; the baseline sits one
	// ascent below the top gutter.
	constexpr float kGutter = 2.0f;
	HudTextAnchor a;
	a.align = p_slot.align;
	a.size = p_slot.size * p_slot.scale_y;
	const float left = p_slot.bounds.x_min + kGutter;
	const float right = p_slot.bounds.x_max - kGutter;
	float fx = left;
	if (p_slot.align == 1) {
		fx = right;
	} else if (p_slot.align == 2) {
		fx = 0.5f * (left + right);
	}
	a.x = p_slot.x + fx * p_slot.scale_x;
	const float ascent = std::isfinite(p_ascent_per_px) ? p_ascent_per_px : 0.0f;
	a.baseline = p_slot.y + (p_slot.bounds.y_min + kGutter) * p_slot.scale_y + ascent * a.size;
	return a;
}

} // namespace bf3::host
