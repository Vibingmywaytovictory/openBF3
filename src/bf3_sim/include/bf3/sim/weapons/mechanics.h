#pragma once
// Every weapon mechanic whose semantics the data does not give, as one switchable choice (docs/design/
// m2a-weapons.md §6.9; plan D7, R5, §3.13 harness items H:W1-H:W9).
//
// The data gives the numbers (GunSwayData, BulletEntityData, the grid); it does not say how BF3 uses some of them.
// Each such rule is a field here with the default the plan prescribes without a measurement ("uniform-area disc
// + linear recovery marked unmeasured", plan §3.13 "Without VU"), and `measured` stays false until S5 Session 1
// picks a variant and research/measurements/SOURCES.md cites the run. The simulation reads only this struct, so a
// measured choice is a data change of one field, never a code change; the struct is part of weapon_table_hash,
// so a server and a client that chose differently refuse each other (MatchInfo) instead of mispredicting.

#include <cstdint>

namespace bf3::sim::weapons {

// H:W1. Where inside the spread cone a shot lands: radius = spread x sqrt(u) (uniform over the disc's area) or
// spread x u (uniform in radius: centre-biased).
enum class DiscDistribution : uint8_t { UniformArea = 0, UniformRadius = 1 };
// H:W3. How RecoilAmplitudeDecreaseFactor brings the recoil offset back: by `factor` degrees per second
// (Symthic's reading) or by exp(-factor x t) (the alternative). Both are applied per whole tick (§5.6).
enum class RecoveryShape : uint8_t { Linear = 0, Exponential = 1 };
// H:W4. When FirstShotRecoilMultiplier applies again after a shot.
enum class FirstShotRearm : uint8_t { OnTriggerRelease = 0, WhenRecovered = 1 };
// H:W2. How fast the minimum spread rises when the state's minimum goes up (stand -> moving): at once, or at the
// state's DecreasePerSecond rate.
enum class MinSpreadRise : uint8_t { Immediate = 0, AtDecreaseRate = 1 };
// H:W5. Bullet damage between the falloff distances (the only shape anyone reported: linear, Symthic 1.7).
enum class FalloffShape : uint8_t { Linear = 0 };
// The falloff distance: straight line from the shot origin to the hit, or the path flown (they differ by < 1 mm
// at 300 m for a rifle).
enum class FalloffBasis : uint8_t { StraightLine = 0, PathLength = 1 };
// W-C. Blast damage between InnerBlastRadius and BlastRadius (not in data; linear assumed, topics/01 §2.4).
enum class BlastFalloff : uint8_t { Linear = 0 };

struct Mechanics {
	DiscDistribution disc = DiscDistribution::UniformArea;
	RecoveryShape recovery = RecoveryShape::Linear;
	FirstShotRearm first_shot_rearm = FirstShotRearm::OnTriggerRelease;
	MinSpreadRise min_spread_rise = MinSpreadRise::Immediate;
	FalloffShape falloff = FalloffShape::Linear;
	FalloffBasis falloff_basis = FalloffBasis::StraightLine;
	BlastFalloff blast_falloff = BlastFalloff::Linear;
	// GunSwayStanceTransition.MaxPenaltyValue / CoolDown (how they apply is not known): off by default.
	bool stance_transition_penalty = false;
	// Shots count as "suppressed" spread (SuppressionModifier*) at or above this suppression level
	// (SoldierSuppressionComponentData.SuppressionLowThreshold 0.3 [verified data; trigger inferred]).
	float suppression_threshold = 0.3f;
	// H:W6. DamageProtectionThreshold (2 on the Sniper chest cells, 15 on blasts): unknown, not applied by default.
	bool apply_protection_threshold = false;
	// Whether a bullet continues through a soldier (the soldier pairs are flagged penetrable, x0.8 in the cell):
	// not determined (topics/01 §1.7); a bullet stops at the first soldier by default.
	bool bullets_pass_soldiers = false;
	// Explosion occlusion: rays from the blast to the target's capsules against the static world's Collision layer.
	// DisableStaticEntityOcclusion is true on every M2a explosive and its meaning is open, so the default keeps
	// occlusion against level geometry on.
	bool blast_occlusion = true;
	// Empty reload of rtMagazineWithPossibleShorterReload gives capacity - 1 (no round chambered; Symthic "30+1").
	bool empty_reload_unchambered = true;
	// Fire while sprinting is refused, and for SoldierSprintSettingsData.RecoverTime (0.12 s) after it [inferred].
	bool sprint_blocks_fire = true;
	// Spawn protection (ImmortalTimeAfterSpawn) refuses damage [inferred].
	bool spawn_protection_blocks = true;
	// The level's SyncedGameSettings.BulletDamageModifier (1 on Metro [verified, topics/01 C10]).
	float bullet_damage_modifier = 1.0f;
	// false until S5 Session 1 decides H:W1-H:W9; reports print "unmeasured" for every row that depends on a choice.
	bool measured = false;
};

} // namespace bf3::sim::weapons
