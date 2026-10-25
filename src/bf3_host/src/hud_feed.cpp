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
	r_w.secondary_weapon_ammo = 0;
}

}

HudFireMode hud_fire_mode(gp::FireLogic p_logic) {
	switch (p_logic) {
		case gp::FireLogic::Single:
			return HudFireMode::Single;
		case gp::FireLogic::SingleWithBoltAction:
			return HudFireMode::SingleBolt;
		case gp::FireLogic::Automatic:
			return HudFireMode::Auto;
		case gp::FireLogic::Burst:
			return HudFireMode::Burst;
		case gp::FireLogic::HoldAndRelease:
			return HudFireMode::HoldAndRelease;
		case gp::FireLogic::Detonated:
			return HudFireMode::Detonated;
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

const HudStaticItem *HudStatic::item(uint32_t p_item_id) const {
	const auto it = std::lower_bound(items.begin(), items.end(), p_item_id,
			[](const HudStaticItem &p_a, uint32_t p_id) { return p_a.item_id < p_id; });
	return it != items.end() && it->item_id == p_item_id ? &*it : nullptr;
}

void hud_text(ui::HudText &r_out, std::string_view p_text) {
	r_out.fill(char(0));
	const size_t n = std::min(p_text.size(), r_out.size() - 1);
	std::copy_n(p_text.begin(), n, r_out.begin());
}

size_t apply_item_table(HudStatic &r_static, const ui::ItemIconIndex &p_items) {
	r_static.items.clear();
	for (const ui::ItemIcon &icon : p_items.icons()) {
		if (icon.item_id == 0) {
			continue;
		}
		HudStaticItem it;
		it.item_id = icon.item_id;
		it.name_sid_hash = icon.name_sid.empty() ? 0 : ui::text_hash(icon.name_sid);
		hud_text(it.image_path, icon.path);
		r_static.items.push_back(it);
	}
	std::sort(r_static.items.begin(), r_static.items.end(),
			[](const HudStaticItem &p_a, const HudStaticItem &p_b) { return p_a.item_id < p_b.item_id; });
	r_static.items.erase(
			std::unique(r_static.items.begin(), r_static.items.end(),
					[](const HudStaticItem &p_a, const HudStaticItem &p_b) { return p_a.item_id == p_b.item_id; }),
			r_static.items.end());
	return r_static.items.size();
}

ui::HudItem hud_item(const HudStatic &p_static, uint32_t p_item_id) {
	ui::HudItem out;
	out.item_id = p_item_id;
	if (const HudStaticItem *it = p_static.item(p_item_id)) {
		out.name_sid_hash = it->name_sid_hash;
		out.image_path = it->image_path;
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

namespace {

void push_damage(const HudFeedInput &p_input, const PresentEvent &p_hit, HudFeedState &r_state, ui::HudModel &r_model) {
	const auto it = std::find_if(p_input.others.begin(), p_input.others.end(), [&](const present::PresentedSoldier &s) { return s.slot == p_hit.slot; });
	if (it == p_input.others.end()) {
		++r_state.damage_unplaced;
		return;
	}
	ui::HudDamageFeed &f = r_model.damage;
	++f.newest_serial;
	ui::HudDamageHit &h = f.hits[f.newest_serial % f.hits.size()];
	h.serial = f.newest_serial;
	h.shooter = p_hit.slot;
	h.bearing_deg = hud_bearing_deg(p_input.own_position, it->position);
	h.damage = std::isfinite(p_hit.value) ? std::max(p_hit.value, 0.0f) : 0.0f;
	++r_state.damage_shown;
}

}

void fill_hud(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudFeedState &r_state,
		ui::HudModel &r_model) {
	++r_state.frames;
	for (const PresentEvent &e : p_drained) {
		if (e.kind != PresentKind::Hit) {
			continue;
		}
		if ((e.flags & present_flags::kOwnVictim) != 0 && e.slot != p_input.damage_own_slot) {
			push_damage(p_input, e, r_state, r_model);
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
		w.secondary_weapon_ammo = p_input.weapons != nullptr ? hud_grenade_count(*p_input.weapons, p_static) : 0;
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
	s.compass_angle = p_input.alive ? hud_compass_heading_deg(p_input.view_yaw_rad) : 0.0f;
	++r_model.serial;
}

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

float hud_compass_heading_deg(float p_view_yaw_rad) {
	if (!std::isfinite(p_view_yaw_rad)) {
		return 0.0f;
	}
	const double deg = std::fmod(-double(p_view_yaw_rad) * 180.0 / kPi, 360.0);
	return float(deg < 0.0 ? deg + 360.0 : deg);
}

float hud_bearing_deg(const sim::Vec3 &p_from, const sim::Vec3 &p_to) {
	const double dx = double(p_to[0]) - double(p_from[0]);
	const double dz = double(p_to[2]) - double(p_from[2]);
	if (!std::isfinite(dx) || !std::isfinite(dz) || (dx == 0.0 && dz == 0.0)) {
		return 0.0f;
	}
	return hud_compass_heading_deg(float(std::atan2(dx, dz)));
}

int32_t hud_grenade_count(const net::WeaponsWire &p_weapons, const HudStatic &p_static) {
	const int32_t grenade_class = hud_weapon_class("wcHgr");
	for (const net::WeaponSlotWire &slot : p_weapons.slots) {
		const HudStaticRow *row = p_static.row(slot.def);
		if (row != nullptr && row->weapon_class == grenade_class) {
			return std::max(slot.magazine, 0) + std::max(slot.reserve, 0);
		}
	}
	return 0;
}


}
