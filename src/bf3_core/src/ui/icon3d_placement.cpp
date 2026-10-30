#include "bf3/core/ui/icon3d_placement.h"

#include "bf3/core/ebx_db.h"
#include "ui_ebx.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr float kMaxField = 1e6f;

float checked(std::optional<float> p_value, float p_default, float p_lo, float p_hi, uint32_t &r_clamped) {
	if (!p_value) {
		return p_default;
	}
	if (!std::isfinite(*p_value) || *p_value < p_lo || *p_value > p_hi) {
		++r_clamped;
		return p_default;
	}
	return *p_value;
}

float clamp01(float p_v) {
	return std::isfinite(p_v) ? std::clamp(p_v, 0.0f, 1.0f) : 0.0f;
}

}

Result<Icon3dStyle> read_icon3d_style(
		const EbxResolver &p_level, std::string_view p_partition, gameplay::WorkBudget *p_budget) {
	gameplay::WorkBudget local;
	gameplay::WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	BF3_TRY(budget.status());
	BF3_TRY(budget.charge_bytes(p_partition.size()));
	const EbxPartition *partition = p_level.partition_by_name(p_partition);
	if (partition == nullptr) {
		return make_error(std::string(p_partition) + ": 3D icon component partition does not resolve");
	}
	const EbxInstance primary = partition->primary();
	BF3_TRY(budget.charge_type(primary.data()));
	if (!primary.data().is_a("UI3dIconCompData")) {
		return make_error(std::string(p_partition) + ": primary instance is " + std::string(primary.type_name()) +
				", not a UI3dIconCompData");
	}
	const EbxStruct s = primary.data();
	Icon3dStyle out;
	const Icon3dStyle d;
	uint32_t &c = out.clamped;
	out.draw_distance = checked(detail::f32(s, "DrawDistance"), d.draw_distance, 0.0f, kMaxField, c);
	out.minimum_draw_distance =
			checked(detail::f32(s, "MinimumDrawDistance"), d.minimum_draw_distance, 0.0f, kMaxField, c);
	out.fade_distance = checked(detail::f32(s, "FadeDistance"), d.fade_distance, 0.0f, kMaxField, c);
	out.fade_end_distance = checked(detail::f32(s, "FadeEndDistance"), d.fade_end_distance, 0.0f, kMaxField, c);
	out.max_far_fade = checked(detail::f32(s, "MaxFarFade"), d.max_far_fade, 0.0f, 1.0f, c);
	out.minimum_fade_distance =
			checked(detail::f32(s, "MinimumFadeDistance"), d.minimum_fade_distance, 0.0f, kMaxField, c);
	out.max_close_fade = checked(detail::f32(s, "MaxCloseFade"), d.max_close_fade, 0.0f, 1.0f, c);
	out.scale_distance = checked(detail::f32(s, "ScaleDistance"), d.scale_distance, 0.0f, kMaxField, c);
	out.max_scale_mod = checked(detail::f32(s, "MaxScaleMod"), d.max_scale_mod, 0.0f, 1.0f, c);
	out.vertical_offset = checked(detail::f32(s, "VerticalOffset"), d.vertical_offset, -100.0f, 100.0f, c);
	if (const std::optional<std::array<float, 2>> po = detail::vec2(s, "PixelOffset")) {
		out.pixel_offset[0] = checked((*po)[0], d.pixel_offset[0], -1024.0f, 1024.0f, c);
		out.pixel_offset[1] = checked((*po)[1], d.pixel_offset[1], -1024.0f, 1024.0f, c);
	}
	out.snap_icons = detail::flag_or(s, "SnapIcons", d.snap_icons);
	out.circular_snap = detail::flag_or(s, "CircularSnap", d.circular_snap);
	out.only_show_snapped = detail::flag_or(s, "OnlyShowSnapped", d.only_show_snapped);
	out.snap_safe_zone = checked(detail::f32(s, "SnapSafeZone"), d.snap_safe_zone, 0.0f, 1.0f, c);
	out.snap_center_y_offset = checked(detail::f32(s, "SnapCenterYOffset"), d.snap_center_y_offset, -1.0f, 1.0f, c);
	out.tracker_radius_x = checked(detail::f32(s, "TrackerHudRadiusX"), d.tracker_radius_x, 0.0f, 1.0f, c);
	out.tracker_radius_y = checked(detail::f32(s, "TrackerHudRadiusY"), d.tracker_radius_y, 0.0f, 1.0f, c);
	out.from_data = true;
	BF3_TRY(budget.status());
	return out;
}

float icon3d_scale(const Icon3dStyle &p_style, float p_distance_m) {
	if (!(p_style.scale_distance > 0.0f)) {
		return 1.0f;
	}
	return 1.0f - p_style.max_scale_mod * clamp01(p_distance_m / p_style.scale_distance);
}

float icon3d_alpha(const Icon3dStyle &p_style, float p_distance_m) {
	float alpha = 1.0f;
	if (p_style.fade_end_distance > p_style.fade_distance) {
		const float t =
				clamp01((p_distance_m - p_style.fade_distance) / (p_style.fade_end_distance - p_style.fade_distance));
		alpha = 1.0f + (p_style.max_far_fade - 1.0f) * t;
	} else if (p_distance_m >= p_style.fade_distance && p_style.fade_distance > 0.0f) {
		alpha = p_style.max_far_fade;
	}
	if (p_style.minimum_fade_distance > 0.0f && p_distance_m < p_style.minimum_fade_distance) {
		const float t = clamp01(p_distance_m / p_style.minimum_fade_distance);
		alpha *= p_style.max_close_fade + (1.0f - p_style.max_close_fade) * t;
	}
	return clamp01(alpha);
}

SwfPoint icon3d_snap_origin(const Icon3dStyle &p_style, const SwfRect &p_visible) {
	return { 0.5f * (p_visible.x_min + p_visible.x_max),
		0.5f * (p_visible.y_min + p_visible.y_max) + p_style.snap_center_y_offset * 0.5f * p_visible.height() };
}

SwfRect icon3d_snap_box(const Icon3dStyle &p_style, const SwfRect &p_visible) {
	const float cx = 0.5f * (p_visible.x_min + p_visible.x_max);
	const float cy = 0.5f * (p_visible.y_min + p_visible.y_max);
	const float hx = 0.5f * p_visible.width() * p_style.snap_safe_zone;
	const float hy = 0.5f * p_visible.height() * p_style.snap_safe_zone;
	return { cx - hx, cx + hx, cy - hy, cy + hy };
}

Icon3dPlacement place_icon3d(
		const Icon3dStyle &p_style, float p_distance_m, bool p_in_front, SwfPoint p_stage, const SwfRect &p_visible) {
	Icon3dPlacement out;
	if (!std::isfinite(p_distance_m) || !std::isfinite(p_stage.x) || !std::isfinite(p_stage.y) ||
			!(p_visible.width() > 0.0f) || !(p_visible.height() > 0.0f)) {
		return out;
	}
	if (p_distance_m < p_style.minimum_draw_distance ||
			(p_style.draw_distance > 0.0f && p_distance_m > p_style.draw_distance)) {
		return out;
	}
	out.scale = icon3d_scale(p_style, p_distance_m);
	out.alpha = icon3d_alpha(p_style, p_distance_m);
	const SwfPoint at{ p_stage.x + p_style.pixel_offset[0], p_stage.y + p_style.pixel_offset[1] };
	const SwfRect box = icon3d_snap_box(p_style, p_visible);
	const bool inside = p_in_front && at.x >= box.x_min && at.x <= box.x_max && at.y >= box.y_min && at.y <= box.y_max;
	if (inside) {
		out.shown = !p_style.only_show_snapped;
		out.x = at.x;
		out.y = at.y;
		return out;
	}
	if (!p_style.snap_icons) {
		return out;
	}
	const SwfPoint raw = icon3d_snap_origin(p_style, p_visible);
	const SwfPoint o{ std::clamp(raw.x, box.x_min, box.x_max), std::clamp(raw.y, box.y_min, box.y_max) };
	float dx = at.x - o.x;
	float dy = at.y - o.y;
	if (std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f) {
		dy = 1.0f;
	}
	const float rx = 0.5f * p_visible.width() * p_style.tracker_radius_x;
	const float ry = 0.5f * p_visible.height() * p_style.tracker_radius_y;
	if (p_style.circular_snap && rx > 0.0f && ry > 0.0f) {
		const float k = 1.0f / std::sqrt((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry));
		out.x = o.x + dx * k;
		out.y = o.y + dy * k;
	} else {
		float t = std::numeric_limits<float>::max();
		t = dx > 0.0f ? std::min(t, (box.x_max - o.x) / dx) : dx < 0.0f ? std::min(t, (box.x_min - o.x) / dx) : t;
		t = dy > 0.0f ? std::min(t, (box.y_max - o.y) / dy) : dy < 0.0f ? std::min(t, (box.y_min - o.y) / dy) : t;
		t = std::max(t, 0.0f);
		out.x = o.x + dx * t;
		out.y = o.y + dy * t;
	}
	out.shown = true;
	out.snapped = true;
	return out;
}

}
