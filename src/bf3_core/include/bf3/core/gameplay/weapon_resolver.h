#pragma once
// Resolves soldier weapons from EBX into WeaponDef (plan §3.2 "C++ design", topics/01 §1.5, §5.1).
//
// Weapons are read from the level's own bundles (plan §2.4 step 2): every MP level's main bundle
// carries the full weapon EBX, so the resolver takes the level's EbxResolver (an EbxLibrary over
// the level superbundle). The weapon-name SID lives in `ui/uiweaponmetadata` (Win32/UI), so a
// second resolver for the UI superbundle is optional.
//
// Resolution rule (topics/01 §1.5; [inferred], consistent with all data seen):
//   1. Active unlocks: the variant (a SoldierWeaponUnlockAsset) itself and its Extra chain, then each
//      requested unlock (a SoldierWeaponUnlockAsset adds its Extra chain too).
//   2. WeaponFiringDataModifier of an active unlock replaces the whole WeaponFiringData (the last one
//      in WeaponModifierData order wins); its WeaponSway replaces the GunSwayData with it.
//   3. Every other modifier of an active unlock patches fields, in WeaponModifierData order:
//      WeaponShotModifier (InitialSpeed; NumberOfBulletsPerShell unless -1), WeaponProjectileModifier,
//      WeaponMagazineModifier (each field unless -1), WeaponDeployTimeModifier (each time unless < 0),
//      WeaponZoomModifier, WeaponMiscModifier (its flags are OR-ed: suppressor + bipod stays
//      silenced), WeaponClassModifier, WeaponAimingSimulationModifier, WeaponSoundModifier,
//      WeaponAnimTypeModifier. Effects/animation modifiers are only listed.
//   4. GunSwayModifierData whose unlock is active multiply the stance x zoom states (min, max, +shot,
//      -second; vertical recoil per shot x RecoilMagnitudeMod, horizontal x RecoilAngleMod; a
//      MaxAngleModifier or Lag*Mod of 0 leaves the value). Gated ones (OnlyInSupportedShooting,
//      OnlyOnWeaponLightEnabled) are kept apart in GunSway::conditional.
// The AI firing data in the same partition (`weapons/common/ai_unlock`) applies only when asked for.
//
// Every reference that does not resolve, every missing field and every wrong type is an error; a
// hostile partition cannot loop (the Extra chain is capped) or allocate beyond its own arrays.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

struct WeaponResolveOptions {
	// Extra unlocks: partition names, or names relative to the weapon's folder ("u_m16a4_foregrip").
	std::vector<std::string> unlocks;
	// Resolver holding `ui/uiweaponmetadata` (e.g. an EbxLibrary over Win32/UI); null = no SID.
	const EbxResolver *ui = nullptr;
};

// p_partition is a SoldierWeaponUnlockAsset (a variant: "weapons/m16a4/u_m16_burst") or a
// SoldierWeaponBlueprint ("weapons/m16a4/m16a4", resolved without a variant).
Result<WeaponDef> resolve_weapon(const EbxResolver &p_db, std::string_view p_partition, const WeaponResolveOptions &p_options = {});

// Partitions of every SoldierWeaponUnlockAsset under weapons/ in the library, sorted by name. Parses
// the library's partitions on demand (load_all() first is faster).
std::vector<std::string> list_weapon_variants(const EbxLibrary &p_library);

// The base variant of a blueprint: a SoldierWeaponUnlockAsset in the same folder whose Weapon is the
// blueprint and whose Extra is null, the shortest name first; "" when there is none.
std::string base_variant_of(const EbxLibrary &p_library, std::string_view p_blueprint);

// The weapon-name SID of an unlock identifier from `ui/uiweaponmetadata` (UIWeaponDescription.Name
// whose ItemIds holds it); "" when absent.
std::string weapon_name_sid(const EbxResolver &p_ui, uint32_t p_unlock_identifier);

} // namespace bf3::core::gameplay
