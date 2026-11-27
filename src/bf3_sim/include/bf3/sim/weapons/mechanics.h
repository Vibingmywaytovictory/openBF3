#pragma once

#include <cstdint>

namespace bf3::sim::weapons {

enum class FalloffShape : uint8_t { Linear = 0 };

enum class FalloffBasis : uint8_t { StraightLine = 0, PathLength = 1 };

enum class BlastFalloff : uint8_t { Linear = 0 };

struct Mechanics {
	// SOURCE: notes R-A4 3 (linear blend between the falloff distances, d the straight line from the bullet's origin) and
	// R-G4 1 (blast linear from InnerBlastRadius to BlastRadius), the three defaults below.
	FalloffShape falloff = FalloffShape::Linear;
	FalloffBasis falloff_basis = FalloffBasis::StraightLine;
	BlastFalloff blast_falloff = BlastFalloff::Linear;

	// STAND-IN: GunSwayStanceTransition's MaxPenaltyValue and CoolDown are not applied (the switch has no reader).
	// Settle: notes Q2 B3's transition blend (MaxPenaltyValue to the record over CoolDown, modifiers off).
	bool stance_transition_penalty = false;

	// STAND-IN: off and without a reader, where notes R-A4 3 zero a bullet hit below DamageProtectionThreshold.
	// Settle: apply R-A4 3's threshold to bullet hits.
	bool apply_protection_threshold = false;

	// STAND-IN: off and without a reader. Settle: notes R-A4 4 (pass when the hit material has the penetrable bit).
	bool bullets_pass_soldiers = false;

	// INFERRED: a blast is occluded by the static world (R-G4 1 names an occlusion factor without its source).
	// Settle: a note on the explosion occlusion query and DisableStaticEntityOcclusion.
	bool blast_occlusion = true;

	// SOURCE: notes R-A3 F (an empty rtMagazineWithPossibleShorterReload reload adds capacity - 1).
	bool empty_reload_unchambered = true;

	// SOURCE: notes R-A3 K (fire held back while sprinting and until SoldierSprintSettingsData.RecoverTime after it).
	// INFERRED: the hold-back's meaning, as the R-A3 checker leaves it. Settle: a note on the sprint input block.
	bool sprint_blocks_fire = true;

	// SOURCE: notes R2-A5 A2 (an immortal soldier's health does not change).
	// INFERRED: spawn protection is that immortal state, as R2-A5 infers. Settle: R2-A5 A2's label.
	bool spawn_protection_blocks = true;

	// SOURCE: notes R2-A5 A4 (a damage packet with the bullet flag is multiplied by SyncedGameSettings.BulletDamageModifier).
	// INFERRED: that flag marks bullet hits, as R2-A5 infers from the field name. Settle: R2-A5 A4's flag meaning.
	float bullet_damage_modifier = 1.0f;

	bool measured = false;
};

}
