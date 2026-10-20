#include "bf3/core/ui/nametags.h"

#include "ui_ebx.h"

#include <algorithm>
#include <cmath>
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

std::array<float, 4> checked_tint(const EbxStruct &p_s, std::string_view p_field, const std::array<float, 4> &p_default, uint32_t &r_clamped) {
	const std::optional<EbxStruct> v = detail::sub(p_s, p_field);
	if (!v) {
		return p_default;
	}
	std::array<float, 4> out = p_default;
	const char *names[4] = { "x", "y", "z", "w" };
	for (size_t i = 0; i < 4; ++i) {
		out[i] = checked(detail::f32(*v, names[i]), p_default[i], 0.0f, 1.0f, r_clamped);
	}
	return out;
}

}

Result<NametagStyle> read_nametag_style(const EbxResolver &p_level, gameplay::WorkBudget *p_budget, std::string_view p_partition) {
	gameplay::WorkBudget local;
	gameplay::WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	BF3_TRY(budget.status());
	BF3_TRY(budget.charge_bytes(p_partition.size()));
	const EbxPartition *partition = p_level.partition_by_name(p_partition);
	if (partition == nullptr) {
		return make_error(std::string(p_partition) + ": nametag component partition does not resolve");
	}
	const EbxInstance primary = partition->primary();
	BF3_TRY(budget.charge_type(primary.data()));
	if (!primary.data().is_a("UINametagCompData")) {
		return make_error(std::string(p_partition) + ": primary instance is " + std::string(primary.type_name()) + ", not UINametagCompData");
	}
	const EbxStruct s = primary.data();
	NametagStyle out;
	uint32_t &c = out.clamped;
	const NametagStyle d;
	out.friendly_tint = checked_tint(s, "FriendlyTint", d.friendly_tint, c);
	out.squad_tint = checked_tint(s, "SquadTint", d.squad_tint, c);
	out.enemy_tint = checked_tint(s, "EnemyTint", d.enemy_tint, c);
	out.draw_distance = checked(detail::f32(s, "DrawDistance"), d.draw_distance, 0.0f, kMaxField, c);
	out.minimum_draw_distance = checked(detail::f32(s, "MinimumDrawDistance"), d.minimum_draw_distance, 0.0f, kMaxField, c);
	out.fade_distance = checked(detail::f32(s, "FadeDistance"), d.fade_distance, 0.0f, kMaxField, c);
	out.fade_end_distance = checked(detail::f32(s, "FadeEndDistance"), d.fade_end_distance, 0.0f, kMaxField, c);
	out.max_far_fade = checked(detail::f32(s, "MaxFarFade"), d.max_far_fade, 0.0f, 1.0f, c);
	out.scale_distance = checked(detail::f32(s, "ScaleDistance"), d.scale_distance, 0.0f, kMaxField, c);
	out.max_scale_mod = checked(detail::f32(s, "MaxScaleMod"), d.max_scale_mod, 0.0f, 1.0f, c);
	out.icon_size = checked(detail::f32(s, "IconSize"), d.icon_size, 0.0f, 1024.0f, c);
	out.vertical_offset = checked(detail::f32(s, "VerticalOffset"), d.vertical_offset, -100.0f, 100.0f, c);
	if (const std::optional<std::array<float, 2>> po = detail::vec2(s, "PixelOffset")) {
		out.pixel_offset[0] = checked((*po)[0], d.pixel_offset[0], -1024.0f, 1024.0f, c);
		out.pixel_offset[1] = checked((*po)[1], d.pixel_offset[1], -1024.0f, 1024.0f, c);
	}
	out.min_look_at_threshold = checked(detail::f32(s, "MinLookAtThreshold"), d.min_look_at_threshold, 0.0f, 4096.0f, c);
	out.max_look_at_threshold = checked(detail::f32(s, "MaxLookAtThreshold"), d.max_look_at_threshold, 0.0f, 4096.0f, c);
	out.min_look_at_threshold_distance = checked(detail::f32(s, "MinLookAtThresholdDistance"), d.min_look_at_threshold_distance, 0.0f, kMaxField, c);
	out.show_label_range = checked(detail::f32(s, "ShowLabelRange"), d.show_label_range, 0.0f, kMaxField, c);
	out.name_font_size = checked(detail::f32(s, "NameFontSize"), d.name_font_size, 1.0f, 256.0f, c);
	out.name_glow_size = checked(detail::f32(s, "NameGlowSize"), d.name_glow_size, 0.0f, 256.0f, c);
	out.from_data = true;
	BF3_TRY(budget.status());
	return out;
}

Result<SpottingRules> read_spotting_rules(const EbxResolver &p_level, gameplay::WorkBudget *p_budget, std::string_view p_partition) {
	gameplay::WorkBudget local;
	gameplay::WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	BF3_TRY(budget.status());
	BF3_TRY(budget.charge_bytes(p_partition.size()));
	const EbxPartition *partition = p_level.partition_by_name(p_partition);
	if (partition == nullptr) {
		return make_error(std::string(p_partition) + ": soldier partition does not resolve");
	}

	BF3_TRY(budget.charge(partition->instance_count()));
	const std::vector<EbxInstance> found = partition->instances_of("SpottingTargetComponentData");
	if (found.empty()) {
		return make_error(std::string(p_partition) + ": no SpottingTargetComponentData");
	}
	BF3_TRY(budget.charge_type(found.front().data()));
	const EbxStruct s = found.front().data();
	SpottingRules out;
	const SpottingRules d;
	out.active_spotted_time = checked(detail::f32(s, "ActiveSpottedTime"), d.active_spotted_time, 0.0f, 3600.0f, out.clamped);
	out.passive_spotted_time = checked(detail::f32(s, "PassiveSpottedTime"), d.passive_spotted_time, 0.0f, 3600.0f, out.clamped);
	out.spot_on_fire_multiplier = checked(detail::f32(s, "SpotOnFireMultiplier"), d.spot_on_fire_multiplier, 0.0f, 100.0f, out.clamped);
	out.from_data = true;
	BF3_TRY(budget.status());
	return out;
}

TagRelation tag_relation(const NametagViewer &p_viewer, uint8_t p_team, uint8_t p_squad) {
	if (p_viewer.team == 0 || p_team == 0) {
		return TagRelation::None;
	}
	if (p_team != p_viewer.team) {
		return TagRelation::Enemy;
	}
	return p_viewer.squad != 0 && p_squad == p_viewer.squad ? TagRelation::Squad : TagRelation::Friendly;
}

float look_at_threshold(const NametagStyle &p_style, float p_distance_m) {
	const float span = p_style.min_look_at_threshold_distance;
	const float t = span > 0.0f && std::isfinite(p_distance_m) ? std::clamp(p_distance_m / span, 0.0f, 1.0f) : 1.0f;
	return p_style.max_look_at_threshold + (p_style.min_look_at_threshold - p_style.max_look_at_threshold) * t;
}

NametagDecision decide_nametag(const NametagStyle &p_style, const NametagViewer &p_viewer, const NametagSubject &p_subject) {
	NametagDecision out;
	out.relation = tag_relation(p_viewer, p_subject.team, p_subject.squad);
	const float d = p_subject.distance_m;
	if (out.relation == TagRelation::None || !p_subject.alive || !p_subject.in_front || !std::isfinite(d) || d < 0.0f ||
			d < p_style.minimum_draw_distance || d > p_style.draw_distance) {
		return NametagDecision{ out.relation, false, false, 1.0f, 1.0f, false };
	}
	const float ts = p_style.scale_distance > 0.0f ? std::clamp(d / p_style.scale_distance, 0.0f, 1.0f) : 1.0f;
	out.scale = 1.0f - p_style.max_scale_mod * ts;
	const float span = std::max(1e-3f, p_style.fade_end_distance - p_style.fade_distance);
	const float tf = std::clamp((d - p_style.fade_distance) / span, 0.0f, 1.0f);
	out.alpha = std::clamp(1.0f + (p_style.max_far_fade - 1.0f) * tf, 0.0f, 1.0f);
	const bool looked_at = std::isfinite(p_subject.crosshair_stage) && p_subject.crosshair_stage <= look_at_threshold(p_style, d);
	if (out.relation == TagRelation::Enemy) {

		out.needs_line_of_sight = looked_at;
		out.name = looked_at && p_subject.line_of_sight;
		return out;
	}
	out.icon = true;
	out.name = d <= p_style.show_label_range || looked_at;
	return out;
}

std::array<float, 4> tag_tint(const NametagStyle &p_style, TagRelation p_relation) {
	switch (p_relation) {
		case TagRelation::Friendly:
			return p_style.friendly_tint;
		case TagRelation::Squad:
			return p_style.squad_tint;
		case TagRelation::Enemy:
			return p_style.enemy_tint;
		case TagRelation::None:
			break;
	}
	return { 1.0f, 1.0f, 1.0f, 1.0f };
}

TagRelation minimap_relation(const NametagViewer &p_viewer, uint8_t p_team, uint8_t p_squad, bool p_alive, double p_now_s, double p_revealed_until_s) {
	if (!p_alive) {
		return TagRelation::None;
	}
	const TagRelation r = tag_relation(p_viewer, p_team, p_squad);
	if (r == TagRelation::Enemy) {
		return std::isfinite(p_now_s) && std::isfinite(p_revealed_until_s) && p_now_s < p_revealed_until_s ? TagRelation::Enemy : TagRelation::None;
	}
	return r;
}

}
