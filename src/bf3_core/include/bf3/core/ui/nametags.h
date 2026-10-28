#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bf3::core::ui {

inline constexpr std::string_view kNametagCompPartition = "ui/uicomponents/uinametagcomp";
inline constexpr std::string_view kSoldierBlueprintPartition = "characters/soldiers/mpsoldier";

struct NametagStyle {
	std::array<float, 4> friendly_tint{ 0.0f, 0.8f, 0.8f, 1.0f };
	std::array<float, 4> squad_tint{ 0.45490196f, 0.8627451f, 0.8f, 1.0f };
	std::array<float, 4> enemy_tint{ 1.0f, 0.3764706f, 0.0f, 1.0f };
	float draw_distance = 10000.0f;
	float minimum_draw_distance = 0.0f;
	float fade_distance = 0.0f;
	float fade_end_distance = 10000.0f;
	float max_far_fade = 0.0f;
	float scale_distance = 60.0f;
	float max_scale_mod = 0.0f;
	float icon_size = 30.0f;
	float vertical_offset = 0.24f;
	std::array<float, 2> pixel_offset{ 0.0f, -4.0f };
	float min_look_at_threshold = 40.0f;
	float max_look_at_threshold = 240.0f;
	float min_look_at_threshold_distance = 50.0f;
	float show_label_range = 15.0f;
	float name_font_size = 12.0f;
	float name_glow_size = 8.0f;
	bool from_data = false;
	uint32_t clamped = 0;
};

struct SpottingRules {
	float active_spotted_time = 7.0f;
	float passive_spotted_time = 1.5f;
	float spot_on_fire_multiplier = 0.45f;
	bool from_data = false;
	uint32_t clamped = 0;

	float fire_reveal_seconds() const { return active_spotted_time * spot_on_fire_multiplier; }
};

Result<NametagStyle> read_nametag_style(const EbxResolver &p_level, gameplay::WorkBudget *p_budget = nullptr,
		std::string_view p_partition = kNametagCompPartition);

Result<SpottingRules> read_spotting_rules(const EbxResolver &p_level, gameplay::WorkBudget *p_budget = nullptr,
		std::string_view p_partition = kSoldierBlueprintPartition);

enum class TagRelation : uint8_t { None, Friendly, Squad, Enemy };

struct NametagSubject {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint8_t squad = 0;
	bool alive = false;
	bool in_front = false;
	float distance_m = 0.0f;
	float crosshair_stage = 1e9f;
	bool line_of_sight = false;
};

struct NametagViewer {
	uint8_t team = 0;
	uint8_t squad = 0;
};

struct NametagDecision {
	TagRelation relation = TagRelation::None;
	bool icon = false;
	bool name = false;
	float scale = 1.0f;
	float alpha = 1.0f;
	bool needs_line_of_sight = false;
};

TagRelation tag_relation(const NametagViewer &p_viewer, uint8_t p_team, uint8_t p_squad);

float look_at_threshold(const NametagStyle &p_style, float p_distance_m);
NametagDecision decide_nametag(const NametagStyle &p_style, const NametagViewer &p_viewer, const NametagSubject &p_subject);

std::array<float, 4> tag_tint(const NametagStyle &p_style, TagRelation p_relation);

TagRelation minimap_relation(const NametagViewer &p_viewer, uint8_t p_team, uint8_t p_squad, bool p_alive, double p_now_s, double p_revealed_until_s);

}
