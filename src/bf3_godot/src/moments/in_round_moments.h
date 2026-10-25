#pragma once

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/minimap.h"

#include <array>
#include <cstdint>
#include <span>

namespace bf3::godot_side {

struct NativeHudSquadRow {
	int16_t kit = 0;
	int16_t booster = 0;
	bool leader = false;
};

struct NativeHudMoment {
	const char *id = "";
	int32_t clip = 0;
	int32_t reserve = 0;
	int32_t grenades = 0;
	int32_t health = 0;
	int32_t tickets_us = 0;
	int32_t tickets_ru = 0;
	int32_t compass_frame = 0;
	float dispersion_px = 0.0f;
	bool crosshair = false;
	float map_texel_x = 0.0f;
	float map_texel_y = 0.0f;
	float map_rotation_deg = 0.0f;
	float area_texel_x = 0.0f;
	float area_texel_y = 0.0f;
	float area_rotation_deg = 0.0f;
	int32_t compass_next = 0;
	bool detail_ambiguous = false;
};

std::span<const NativeHudMoment> native_hud_moments();

bf3::core::ui::HudModel native_hud_moment_model(const NativeHudMoment &p_moment, const bf3::core::ui::HudModel &p_base, double p_crosshair_base_offset);
float native_hud_moment_heading_deg(const NativeHudMoment &p_moment);
float native_hud_moment_view_yaw(const NativeHudMoment &p_moment);
std::array<float, 2> native_hud_moment_texel(const NativeHudMoment &p_moment);
std::array<float, 2> native_hud_moment_world_xz(const NativeHudMoment &p_moment, const bf3::core::ui::MinimapLevel &p_map);
float native_hud_moment_registered_heading_deg(const NativeHudMoment &p_moment);

}
