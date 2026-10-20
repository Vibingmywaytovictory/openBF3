#pragma once

#include <cstdint>

namespace bf3::sim::weapons {

enum class DiscDistribution : uint8_t { UniformArea = 0, UniformRadius = 1 };

enum class RecoveryShape : uint8_t { Linear = 0, Exponential = 1 };

enum class FirstShotRearm : uint8_t { OnTriggerRelease = 0, WhenRecovered = 1 };

enum class MinSpreadRise : uint8_t { Immediate = 0, AtDecreaseRate = 1 };

enum class FalloffShape : uint8_t { Linear = 0 };

enum class FalloffBasis : uint8_t { StraightLine = 0, PathLength = 1 };

enum class BlastFalloff : uint8_t { Linear = 0 };

struct Mechanics {
	DiscDistribution disc = DiscDistribution::UniformArea;
	RecoveryShape recovery = RecoveryShape::Linear;
	FirstShotRearm first_shot_rearm = FirstShotRearm::OnTriggerRelease;
	MinSpreadRise min_spread_rise = MinSpreadRise::Immediate;
	FalloffShape falloff = FalloffShape::Linear;
	FalloffBasis falloff_basis = FalloffBasis::StraightLine;
	BlastFalloff blast_falloff = BlastFalloff::Linear;

	bool stance_transition_penalty = false;

	float suppression_threshold = 0.3f;

	bool apply_protection_threshold = false;

	bool bullets_pass_soldiers = false;

	bool blast_occlusion = true;

	bool empty_reload_unchambered = true;

	bool sprint_blocks_fire = true;

	bool spawn_protection_blocks = true;

	float bullet_damage_modifier = 1.0f;

	bool measured = false;
};

}
