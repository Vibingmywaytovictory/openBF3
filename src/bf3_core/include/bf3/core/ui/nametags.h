#pragma once
// PT1 iff (docs/design/pt1-iff.md): telling friend from foe. BF3's own data for the 3D nametags over soldiers, the
// name under the crosshair and which soldiers the minimap shows; Godot-free, so the rules are unit-tested on recorded
// frames without an install or Godot.
//
// Data (read with the readers below, retail values as the defaults):
//   ui/uicomponents/uinametagcomp (UINametagCompData : UI3dIconCompData, Metro's superbundle):
//     FriendlyTint (0, 0.8, 0.8, 1), SquadTint (0.455, 0.863, 0.8, 1), EnemyTint (1, 0.376, 0, 1); DrawDistance 10000,
//     MinimumDrawDistance 0, FadeDistance 0 .. FadeEndDistance 10000, MaxFarFade 0; ScaleDistance 60, MaxScaleMod 0;
//     IconSize 30; VerticalOffset 0.24; PixelOffset (0, -4); MinLookAtThreshold 40, MaxLookAtThreshold 240,
//     MinLookAtThresholdDistance 50; ShowLabelRange 15; NameFontSize 12; NameGlowSize 8; HealthBarSize 78;
//     ShowMedicHealthThreshold 0.3; SnapIcons false; DataSources ShowNametags, ShowNametagsOnActors,
//     ShowNametagsEnemyActors [values verified: bf3dump dump ui/uicomponents/uinametagcomp]
//   characters/soldiers/mpsoldier: SpottingTargetComponentData ActiveSpottedTime 7, PassiveSpottedTime 1.5,
//     SpotOnFireMultiplier 0.45; SoldierEntityData ShowNametag true [verified: bf3dump dump characters/soldiers/mpsoldier]
//
// What the data does not say is inferred and named (measured = false, docs/design/pt1-iff.md §3):
//   * units: IconSize, PixelOffset, the look-at thresholds and the font sizes are units of BF3's 1280 x 720 HUD stage
//     (kNametagStageHeight); the Godot side scales them by its canvas height / 720.
//   * the anchor is VerticalOffset metres above the soldier's eye, then PixelOffset on screen.
//   * the look-at threshold (a radius around the screen centre) runs linearly from MaxLookAtThreshold at 0 m to
//     MinLookAtThreshold at MinLookAtThresholdDistance and beyond.
//   * friendlies: the icon always (DrawDistance 10000, through walls), the name within ShowLabelRange or when looked
//     at; enemies: nothing, except the name in EnemyTint while looked at with a clear line of sight (BF3 shows no enemy
//     marker unless spotted; M2b has no spotting).
//   * minimap: the local player, every friendly, and an enemy only while revealed by its own fire, for
//     ActiveSpottedTime x SpotOnFireMultiplier (3.15 s) after each shot.
// Squads are not in M2b: squad is 0 everywhere, so the Squad states never show (the rules take them for M3+).

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
inline constexpr float kNametagStageHeight = 720.0f; // BF3's HUD stage is 1280 x 720 [inferred]

// UINametagCompData's fields this track uses. from_data false: the retail values below (the reader failed).
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
	float icon_size = 30.0f; // stage units, the larger side
	float vertical_offset = 0.24f; // metres above the eye [inferred]
	std::array<float, 2> pixel_offset{ 0.0f, -4.0f }; // stage units
	float min_look_at_threshold = 40.0f; // stage units
	float max_look_at_threshold = 240.0f; // stage units
	float min_look_at_threshold_distance = 50.0f; // metres
	float show_label_range = 15.0f; // metres
	float name_font_size = 12.0f; // stage units
	float name_glow_size = 8.0f; // stage units
	bool from_data = false;
	uint32_t clamped = 0; // fields the reader replaced (non-finite, out of range): counted, never a crash
};

// SpottingTargetComponentData of the soldier blueprint.
struct SpottingRules {
	float active_spotted_time = 7.0f;
	float passive_spotted_time = 1.5f;
	float spot_on_fire_multiplier = 0.45f;
	bool from_data = false;
	uint32_t clamped = 0;
	// How long a shot shows the shooter on the enemy minimap [inferred: ActiveSpottedTime x SpotOnFireMultiplier].
	float fire_reveal_seconds() const { return active_spotted_time * spot_on_fire_multiplier; }
};

// The primary instance of p_partition (UINametagCompData). Values outside [0, 1e6] (tints [0, 1]) or not finite
// keep the retail default and count in `clamped`. Fails when the partition does not resolve or is another type.
Result<NametagStyle> read_nametag_style(const EbxResolver &p_level, gameplay::WorkBudget *p_budget = nullptr,
		std::string_view p_partition = kNametagCompPartition);
// The first SpottingTargetComponentData instance of p_partition (the soldier blueprint's partition).
Result<SpottingRules> read_spotting_rules(const EbxResolver &p_level, gameplay::WorkBudget *p_budget = nullptr,
		std::string_view p_partition = kSoldierBlueprintPartition);

// ---- the per-soldier decision (pure) -------------------------------------------------------------------------------

enum class TagRelation : uint8_t { None, Friendly, Squad, Enemy };

// One other soldier as the client presents it this frame. The caller projects the anchor (feet + eye height +
// VerticalOffset) and measures the crosshair distance in stage units (canvas distance / canvas scale).
struct NametagSubject {
	uint8_t slot = 0;
	uint8_t team = 0; // 0 unknown, 1, 2
	uint8_t squad = 0; // 0 none (M2b)
	bool alive = false;
	bool in_front = false; // the anchor is in front of the camera (not is_position_behind)
	float distance_m = 0.0f; // camera to anchor
	float crosshair_stage = 1e9f; // screen distance of the projected anchor from the screen centre, stage units
	bool line_of_sight = false; // a clear ray eye -> the soldier (the caller casts it only when it matters)
};

struct NametagViewer {
	uint8_t team = 0; // the local player's team; 0: unknown (nothing is tagged)
	uint8_t squad = 0;
};

struct NametagDecision {
	TagRelation relation = TagRelation::None;
	bool icon = false; // draw the hud3dicons frame
	bool name = false; // draw the name label
	float scale = 1.0f; // 1 - MaxScaleMod x clamp(d / ScaleDistance)
	float alpha = 1.0f; // 1 until FadeDistance, MaxFarFade from FadeEndDistance on
	bool needs_line_of_sight = false; // the name would show if a ray were clear (the caller casts one then)
};

TagRelation tag_relation(const NametagViewer &p_viewer, uint8_t p_team, uint8_t p_squad);
// The look-at radius at p_distance_m, stage units.
float look_at_threshold(const NametagStyle &p_style, float p_distance_m);
NametagDecision decide_nametag(const NametagStyle &p_style, const NametagViewer &p_viewer, const NametagSubject &p_subject);
// The tint of a relation (Friendly, Squad, Enemy; None: white).
std::array<float, 4> tag_tint(const NametagStyle &p_style, TagRelation p_relation);

// ---- the minimap (pure) ---------------------------------------------------------------------------------------------

// Whether the minimap shows another soldier and as which relation: friendlies and squad mates always while alive; an
// enemy while p_now_s < p_revealed_until_s (its last shot + SpottingRules::fire_reveal_seconds()); else None.
TagRelation minimap_relation(const NametagViewer &p_viewer, uint8_t p_team, uint8_t p_squad, bool p_alive, double p_now_s, double p_revealed_until_s);

} // namespace bf3::core::ui
