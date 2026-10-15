// GunSwayData reader: BF3's spread (deviation) and recoil model per stance x zoom (topics/01 §1.3).
//
// GunSwayData
//   Stand  GunSwayStandData       {NoZoom, Zoom : GunSwayBaseMoveJumpData}
//   Crouch GunSwayCrouchProneData {NoZoom, Zoom : GunSwayBaseMoveData}
//   Prone  GunSwayCrouchProneData
//     GunSwayBaseMoveJumpData = {BaseValue, Moving, Jumping, Sprinting, VaultingSmallObject,
//                                VaultingMediumObject : GunSwayDispersionData, Recoil, GunSwayLag}
//     GunSwayBaseMoveData     = {BaseValue, Moving, Recoil, GunSwayLag}
//   ProneToCrouch .. StandToProne : GunSwayStanceTransition {MaxPenaltyValue, CoolDown}
//   SuppressionModifierUnzoomed / Zoomed : GunSwayStanceZoomModifierData
//   Modifiers[] -> GunSwayModifierData {UnlockAsset, <Stance><Zoom|NoZoom>Modifier, gates}
//   FirstShotRecoilMultiplier, ShootingRecoilDecreaseScale, (Gameplay)DeviationScaleFactor(No)Zoom,
//   CameraRecoil -> CameraRecoilData
#include "weapon_readers.h"

#include <algorithm>

namespace bf3::core::gameplay::detail {

namespace {

constexpr const char *kStanceFields[3] = { "Stand", "Crouch", "Prone" };
constexpr const char *kZoomFields[2] = { "NoZoom", "Zoom" };
constexpr const char *kMoveFields[size_t(SwayMove::Count)] = { "BaseValue", "Moving", "Jumping", "Sprinting", "VaultingSmallObject",
	"VaultingMediumObject" };
constexpr const char *kTransitionFields[size_t(StanceTransition::Count)] = { "ProneToCrouch", "ProneToStand", "CrouchToProne", "CrouchToStand",
	"StandToCrouch", "StandToProne" };
constexpr const char *kModifierFields[3][2] = {
	{ "StandNoZoomModifier", "StandZoomModifier" },
	{ "CrouchNoZoomModifier", "CrouchZoomModifier" },
	{ "ProneNoZoomModifier", "ProneZoomModifier" },
};

Dispersion read_dispersion(Fields &r_f, const EbxStruct &p_s) {
	Dispersion d;
	d.min_angle = r_f.f32(p_s, "MinAngle");
	d.max_angle = r_f.f32(p_s, "MaxAngle");
	d.increase_per_shot = r_f.f32(p_s, "IncreasePerShot");
	d.decrease_per_second = r_f.f32(p_s, "DecreasePerSecond");
	return d;
}

Recoil read_recoil(Fields &r_f, const EbxStruct &p_s) {
	Recoil r;
	r.up_per_shot = r_f.f32(p_s, "RecoilAmplitudeIncPerShot");
	r.up_max = r_f.f32(p_s, "RecoilAmplitudeMax");
	r.horizontal_min = r_f.f32(p_s, "HorizontalRecoilAmplitudeIncPerShotMin");
	r.horizontal_max = r_f.f32(p_s, "HorizontalRecoilAmplitudeIncPerShotMax");
	r.horizontal_amplitude_max = r_f.f32(p_s, "HorizontalRecoilAmplitudeMax");
	r.decrease_factor = r_f.f32(p_s, "RecoilAmplitudeDecreaseFactor");
	return r;
}

SwayModifier read_modifier(Fields &r_f, const EbxStruct &p_s) {
	SwayModifier m;
	if (const std::optional<EbxStruct> d = r_f.sub(p_s, "DispersionMod")) {
		m.min_angle = r_f.f32(*d, "MinAngleModifier");
		m.max_angle = r_f.f32(*d, "MaxAngleModifier");
		m.increase_per_shot = r_f.f32(*d, "IncreasePerShotModifier");
		m.decrease_per_second = r_f.f32(*d, "DecreasePerSecondModifier");
	}
	m.recoil_magnitude = r_f.f32(p_s, "RecoilMagnitudeMod");
	m.recoil_angle = r_f.f32(p_s, "RecoilAngleMod");
	m.lag_yaw = r_f.f32(p_s, "LagYawMod");
	m.lag_pitch = r_f.f32(p_s, "LagPitchMod");
	return m;
}

SwayState read_state(Fields &r_f, const EbxStruct &p_s) {
	SwayState s;
	for (size_t m = 0; m < size_t(SwayMove::Count); ++m) {
		// Base and Moving exist on every stance; the others only on stand.
		const bool required = m < 2;
		if (!required && !p_s.get(kMoveFields[m])) {
			continue;
		}
		if (const std::optional<EbxStruct> d = r_f.sub(p_s, kMoveFields[m])) {
			s.move[m] = read_dispersion(r_f, *d);
			s.has_move[m] = true;
		}
	}
	if (const std::optional<EbxStruct> r = r_f.sub(p_s, "Recoil")) {
		s.recoil = read_recoil(r_f, *r);
	}
	return s;
}

} // namespace

bool ActiveUnlocks::contains(const Guid &p_guid) const {
	return std::find(guids.begin(), guids.end(), p_guid) != guids.end();
}

bool ActiveUnlocks::add(const Guid &p_guid, std::string p_name) {
	if (contains(p_guid)) {
		return false;
	}
	guids.push_back(p_guid);
	names.push_back(std::move(p_name));
	return true;
}

void read_gun_sway(Fields &r_f, const EbxInstance &p_sway, const ActiveUnlocks &p_active, GunSway &r_sway) {
	const EbxStruct g = p_sway.data();
	r_sway.partition = r_f.partition_name(p_sway.partition());
	for (size_t stance = 0; stance < 3; ++stance) {
		const std::optional<EbxStruct> st = r_f.sub(g, kStanceFields[stance]);
		if (!st) {
			return;
		}
		for (size_t zoom = 0; zoom < 2; ++zoom) {
			if (const std::optional<EbxStruct> z = r_f.sub(*st, kZoomFields[zoom])) {
				r_sway.state[stance][zoom] = read_state(r_f, *z);
			}
		}
	}
	for (size_t t = 0; t < size_t(StanceTransition::Count); ++t) {
		if (const std::optional<EbxStruct> tr = r_f.sub(g, kTransitionFields[t])) {
			if (const std::optional<EbxStruct> p = r_f.sub(*tr, "MaxPenaltyValue")) {
				r_sway.transitions[t].max_penalty = read_dispersion(r_f, *p);
			}
			r_sway.transitions[t].cool_down = r_f.f32(*tr, "CoolDown");
		}
	}
	if (const std::optional<EbxStruct> s = r_f.sub(g, "SuppressionModifierUnzoomed")) {
		r_sway.suppression[0] = read_modifier(r_f, *s);
	}
	if (const std::optional<EbxStruct> s = r_f.sub(g, "SuppressionModifierZoomed")) {
		r_sway.suppression[1] = read_modifier(r_f, *s);
	}
	r_sway.first_shot_recoil_multiplier = r_f.f32(g, "FirstShotRecoilMultiplier");
	r_sway.shooting_recoil_decrease_scale = r_f.f32(g, "ShootingRecoilDecreaseScale");
	r_sway.deviation_scale = { r_f.f32(g, "DeviationScaleFactorNoZoom"), r_f.f32(g, "DeviationScaleFactorZoom") };
	r_sway.gameplay_deviation_scale = { r_f.f32(g, "GameplayDeviationScaleFactorNoZoom"), r_f.f32(g, "GameplayDeviationScaleFactorZoom") };
	if (const std::optional<EbxInstance> cam = r_f.follow(g, "CameraRecoil", "CameraRecoilData", false)) {
		const EbxStruct c = cam->data();
		r_sway.camera_spring_constant = r_f.f32(c, "SpringConstant");
		r_sway.camera_spring_damping = r_f.f32(c, "SpringDamping");
		r_sway.camera_spring_min_threshold_angle = r_f.f32_or(c, "SpringMinThresholdAngle", 0);
	}

	// Modifiers, in array order. An entry whose unlock is not active is skipped without resolving it.
	const OptionalEbxArray mods = r_f.array(g, "Modifiers");
	if (!mods) {
		return;
	}
	r_sway.modifiers_total = mods->size();
	for (const EbxValue element : *mods) {
		const std::optional<EbxRef> r = element.as_ref();
		if (!r) {
			r_f.fail(r_f.where(g, "Modifiers") + ": element is not a reference");
			return;
		}
		if (r->is_null()) {
			continue;
		}
		// A modifier partition the resolver does not hold cannot say which unlock it serves: counted,
		// not fatal (a partial library still resolves the base weapon).
		if (!r_f.db().resolve(p_sway.partition(), *r)) {
			++r_sway.modifiers_unresolved;
			continue;
		}
		const std::optional<EbxInstance> mod = r_f.follow_ref(p_sway.partition(), *r, "GunSwayModifierData", r_f.where(g, "Modifiers"));
		if (!mod) {
			return;
		}
		const EbxStruct md = mod->data();
		const std::optional<EbxRef> unlock = r_f.ref(md, "UnlockAsset");
		if (!unlock || unlock->is_null()) {
			continue; // ungated by unlock: never seen in the data; not applied
		}
		const std::optional<Guid> id = r_f.target_guid(mod->partition(), *unlock);
		if (!id || !p_active.contains(*id)) {
			continue;
		}
		SwayGatedModifier gm;
		gm.unlock = r_f.target_partition(mod->partition(), *unlock);
		gm.unlock_instance = *id;
		gm.only_in_supported_shooting = r_f.flag(md, "OnlyInSupportedShooting");
		gm.only_on_weapon_light_enabled = r_f.flag(md, "OnlyOnWeaponLightEnabled");
		for (size_t stance = 0; stance < 3; ++stance) {
			for (size_t zoom = 0; zoom < 2; ++zoom) {
				if (const std::optional<EbxStruct> s = r_f.sub(md, kModifierFields[stance][zoom])) {
					gm.mod[stance][zoom] = read_modifier(r_f, *s);
				}
			}
		}
		if (!r_f.ok()) {
			return;
		}
		if (gm.only_in_supported_shooting || gm.only_on_weapon_light_enabled) {
			r_sway.conditional.push_back(std::move(gm));
		} else {
			apply_sway_modifier(r_sway.state, gm);
			r_sway.applied.push_back(std::move(gm));
		}
	}
}

} // namespace bf3::core::gameplay::detail
