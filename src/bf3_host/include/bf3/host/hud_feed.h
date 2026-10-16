#pragma once
// The minimal HUD's feed (docs/design/m2a.md §11.4; owner hud): fills ui2's HudModel from the presented own weapon,
// the own soldier and the drained presentation events. Pure and Godot-free (ui.hud.feed.unit tests it); the Godot
// controls (Bf3Hud: ammo, crosshair, hit indicator, weapon name and icon, health) read only the HudModel. Nothing in
// the HUD reads the sim directly (§11.3).
//
// Fed fields (ui2's M2a-tagged sources): active_weapon_ammo, _clips_left, _clip_capacity, _is_reloading,
// _reload_percent, _critical_ammo, active_weapon_name, active_weapon_data (item id -> the _small icon through
// ItemIconIndex), dispersion (degrees, the interpolated predicted spread), zoomed, firing_mode, show_crosshair,
// show_hitindicator / hitindicator_level, the soldier's pose and health.
// Hit indicator (§16 Q7, taken 2026-10-16): shown kHitIndicatorSeconds per server-confirmed hit whose shooter is the
// own soldier, level kHitIndicatorLevel; no client-predicted markers.

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::host {

inline constexpr double kHitIndicatorSeconds = 0.3; // §16 Q7
inline constexpr float kHitIndicatorLevel = 1.0f; // §16 Q7

enum class HudCrosshairKind : uint8_t { None = 0, Rifle = 1, Pistol = 2, Shotgun = 3, Launcher = 4, Grenade = 5, Knife = 6 };

// Per weapon def (index = def id of the match's WeaponTable).
struct HudStaticRow {
	uint32_t name_sid_hash = 0; // text_hash of UIWeaponDescription.Name (WeaponDef::name_sid_hash)
	uint32_t item_id = 0; // the unlock identifier (ItemIconIndex -> the _small icon)
	int32_t weapon_class = 0; // WeaponDef class as the resolver's enum index (HudWeaponSlot::weapon_class)
	int32_t clip_capacity = 0;
	uint32_t fire_modes = 0; // bit per FiringModeTypes entry of the def's modes
	HudCrosshairKind crosshair = HudCrosshairKind::None;
	bool show_ammo = true; // false for the knife
};
static_assert(std::is_trivially_copyable_v<HudStaticRow>);

struct HudStatic {
	std::vector<HudStaticRow> rows;
	const HudStaticRow *row(uint16_t p_def) const { return p_def < rows.size() ? &rows[p_def] : nullptr; }
};

// Built from the match's table and the WeaponDefs it was made from (p_defs[i] is the source of p_table.defs[i]).
// Fails when the spans differ in length.
core::Result<HudStatic> make_hud_static(const sim::weapons::WeaponTable &p_table, std::span<const core::gameplay::WeaponDef> p_defs);

struct HudFeedInput {
	PresentedWeapon weapon; // present_weapon of this display frame
	bool alive = false;
	float health = 0; // the own soldier's presented health
	sim::Pose pose = sim::Pose::Stand;
	double now_s = 0; // presentation clock (seconds), for the hit indicator's timer
};

// What the feed keeps between frames (the hit indicator's timer, counters for the gates).
struct HudFeedState {
	double hit_indicator_until_s = 0;
	uint64_t hits_shown = 0; // server hits by the own soldier that raised the indicator (E3, E10)
};

void fill_hud(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudFeedState &r_state,
		core::ui::HudModel &r_model);

} // namespace bf3::host
