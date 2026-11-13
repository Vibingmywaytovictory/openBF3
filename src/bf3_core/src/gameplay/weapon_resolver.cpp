#include "bf3/core/gameplay/weapon_resolver.h"

#include "weapon_readers.h"

#include "bf3/core/ids.h"

#include <algorithm>
#include <array>

namespace bf3::core::gameplay {

using detail::ActiveUnlocks;
using detail::Fields;

namespace {

constexpr int kMaxExtraDepth = 8;

std::string folder_of(std::string_view p_partition) {
	const size_t slash = p_partition.rfind('/');
	return slash == std::string_view::npos ? std::string() : std::string(p_partition.substr(0, slash));
}

void activate(Fields &r_f, const EbxInstance &p_unlock, ActiveUnlocks &r_active) {
	EbxInstance at = p_unlock;
	r_active.add(at.guid(), r_f.partition_name(at.partition()));
	for (int depth = 0; depth < kMaxExtraDepth; ++depth) {
		const EbxStruct d = at.data();
		if (!d.get("Extra")) {
			return;
		}
		const std::optional<EbxRef> extra = r_f.ref(d, "Extra");
		if (!extra || extra->is_null()) {
			return;
		}
		const std::optional<Guid> id = r_f.target_guid(at.partition(), *extra);
		if (!id || !r_active.add(*id, r_f.target_partition(at.partition(), *extra))) {
			return;
		}
		const std::optional<EbxInstance> next = r_f.db().resolve(at.partition(), *extra);
		if (!next) {
			return;
		}
		at = *next;
	}
}

std::string ref_partition(Fields &r_f, const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxRef> r = r_f.ref(p_s, p_field);
	return r ? r_f.target_partition(p_s.partition(), *r) : std::string();
}

struct ModifierEntry {
	std::string unlock;
	std::vector<EbxInstance> modifiers;
};

std::vector<ModifierEntry> active_modifiers(Fields &r_f, const EbxStruct &p_swd, const ActiveUnlocks &p_active) {
	std::vector<ModifierEntry> out;
	const OptionalEbxArray entries = r_f.array(p_swd, "WeaponModifierData");
	if (!entries) {
		return out;
	}
	for (const EbxValue element : *entries) {
		const std::optional<EbxStruct> e = element.as_struct();
		if (!e) {
			r_f.fail(r_f.where(p_swd, "WeaponModifierData") + ": element is not a struct");
			return out;
		}
		const std::optional<EbxRef> unlock = r_f.ref(*e, "UnlockAsset");
		if (!unlock || unlock->is_null()) {
			continue;
		}
		const std::optional<Guid> id = r_f.target_guid(e->partition(), *unlock);
		if (!id || !p_active.contains(*id)) {
			continue;
		}
		ModifierEntry entry;
		entry.unlock = r_f.target_partition(e->partition(), *unlock);
		const OptionalEbxArray mods = r_f.array(*e, "Modifiers");
		if (!mods) {
			return out;
		}
		for (const EbxValue m : *mods) {
			const std::optional<EbxRef> r = m.as_ref();
			if (!r) {
				r_f.fail(r_f.where(*e, "Modifiers") + ": element is not a reference");
				return out;
			}
			if (r->is_null()) {
				continue;
			}
			const std::optional<EbxInstance> mod = r_f.follow_ref(e->partition(), *r, "", r_f.where(*e, "Modifiers"));
			if (!mod) {
				return out;
			}
			entry.modifiers.push_back(*mod);
		}
		out.push_back(std::move(entry));
	}
	return out;
}

void read_locking_weapon(Fields &r_f, const EbxInstance &p_locking, LockingWeapon &r_out) {
	const EbxStruct l = p_locking.data();
	r_out.present = true;
	r_out.fire_only_when_locked = r_f.flag(l, "FireOnlyWhenLockedOn");
	r_out.is_guided = r_f.flag(l, "IsGuided");
	r_out.is_guided_when_zoomed = r_f.flag(l, "IsGuidedWhenZoomed");
	r_out.is_homing = r_f.flag(l, "IsHoming");
	r_out.laser_painter = p_locking.is_a("LaserPainterData");
	r_out.time_object_is_painted = r_out.laser_painter ? r_f.f32(l, "TimeObjectIsPainted") : 0.0f;
	if (const std::optional<EbxInstance> c = r_f.follow(l, "LockingController", "LockingControllerData", false)) {
		detail::read_locking_controller(r_f, *c, r_out.primary);
	}
	if (const std::optional<EbxInstance> c =
					r_f.follow(l, "SecondaryLockingController", "LockingControllerData", false)) {
		detail::read_locking_controller(r_f, *c, r_out.secondary);
	}
}

void read_firing_data(Fields &r_f, const EbxInstance &p_wfd, WeaponDef &r_w) {
	const EbxStruct w = p_wfd.data();
	r_w.firing_data = r_f.partition_name(p_wfd.partition()) + "#" + p_wfd.guid().to_string();
	r_w.deploy_time = r_f.f32(w, "DeployTime");
	r_w.alt_deploy_time = r_f.f32(w, "AltDeployTime");
	r_w.disable_zoom_on_deploy_time = r_f.f32_or(w, "DisableZoomOnDeployTime", 0);
	r_w.support_delay_stand = r_f.f32_or(w, "SupportDelayStand", 0);
	r_w.support_delay_prone = r_f.f32_or(w, "SupportDelayProne", 0);
	r_w.reload_whole_mags = r_f.flag_or(w, "ReloadWholeMags", false);
}

void read_aiming(Fields &r_f, std::string_view p_controller, WeaponAiming &r_aiming) {
	const EbxPartition *part = p_controller.empty() ? nullptr : r_f.db().partition_by_name(p_controller);
	if (part == nullptr) {
		return;
	}
	const std::optional<EbxInstance> sim =
			r_f.follow(part->primary().data(), "Data", "SoldierAimingSimulationData", true);
	if (!sim) {
		return;
	}
	const EbxStruct d = sim->data();
	constexpr std::array<std::string_view, 3> kPoseFields = { "StandPose", "CrouchPose", "PronePose" };
	for (size_t i = 0; i < kPoseFields.size(); ++i) {
		const std::optional<EbxStruct> pose = r_f.sub(d, kPoseFields[i]);
		if (!pose) {
			return;
		}
		r_aiming.poses[i] = AimingPose{ r_f.f32(*pose, "MinimumPitch"), r_f.f32(*pose, "MaximumPitch"),
			r_f.f32(*pose, "SpeedMultiplier") };
	}
	const OptionalEbxArray levels = r_f.array(d, "ZoomLevels");
	if (!levels) {
		return;
	}
	for (const EbxValue element : *levels) {
		const std::optional<EbxRef> r = element.as_ref();
		if (!r || r_aiming.zoom_levels.size() >= kMaxZoomLevels) {
			r_f.fail(r_f.where(d, "ZoomLevels") + (r ? ": more than 8 levels" : ": element is not a reference"));
			return;
		}
		const std::optional<EbxInstance> level =
				r_f.follow_ref(d.partition(), *r, "ZoomLevelData", r_f.where(d, "ZoomLevels"));
		if (!level) {
			return;
		}
		const EbxStruct z = level->data();
		r_aiming.zoom_levels.push_back(
				ZoomLook{ r_f.f32(z, "LookSpeedMultiplier"), r_f.f32(z, "SprintLookSpeedMultiplier") });
	}
	r_aiming.present = r_f.ok();
}

bool apply_modifier(Fields &r_f, const EbxInstance &p_mod, WeaponDef &r_w, std::optional<EbxInstance> &r_projectile) {
	const EbxStruct m = p_mod.data();
	if (p_mod.is_a("WeaponShotModifier")) {
		r_w.fire.initial_speed = r_f.vec3(m, "InitialSpeed");
		const int32_t pellets = r_f.i32(m, "NumberOfBulletsPerShell");
		if (pellets >= 0) {
			r_w.fire.bullets_per_shell = pellets;
		}
	} else if (p_mod.is_a("WeaponProjectileModifier")) {
		if (std::optional<EbxInstance> p = r_f.follow(m, "ProjectileData", "ProjectileEntityData", false)) {
			r_projectile = p;
		}
	} else if (p_mod.is_a("WeaponMagazineModifier")) {
		const int32_t cap = r_f.i32(m, "MagazineCapacity");
		const int32_t mags = r_f.i32(m, "NumberOfMagazines");
		if (cap >= 0) {
			r_w.fire.magazine_capacity = cap;
		}
		if (mags >= 0) {
			r_w.fire.number_of_magazines = mags;
		}
	} else if (p_mod.is_a("WeaponDeployTimeModifier")) {
		const float deploy = r_f.f32(m, "DeployTime");
		const float alt = r_f.f32(m, "AltDeployTime");
		const float zoom = r_f.f32_or(m, "DisableZoomOnDeployTime", -1);
		if (deploy >= 0) {
			r_w.deploy_time = deploy;
		}
		if (alt >= 0) {
			r_w.alt_deploy_time = alt;
		}
		if (zoom >= 0) {
			r_w.disable_zoom_on_deploy_time = zoom;
		}
	} else if (p_mod.is_a("WeaponZoomModifier")) {
		r_w.zoom_render_fov = r_f.f32(m, "ZoomRenderFov");
	} else if (p_mod.is_a("WeaponMiscModifier")) {

		r_w.enable_breath_control |= r_f.flag(m, "EnableBreathControl");
		r_w.can_be_in_supported_shooting |= r_f.flag(m, "CanBeInSupportedShooting");
		r_w.silenced |= r_f.flag(m, "IsSilenced");
		r_w.fire.unzoom_on_bolt_action |= r_f.flag(m, "UnZoomOnBoltAction");
		r_w.fire.hold_bolt_action_until_zoom_release |= r_f.flag(m, "HoldBoltActionUntilZoomRelease");
	} else if (p_mod.is_a("WeaponClassModifier")) {
		r_w.weapon_class = r_f.enum_text(m, "WeaponClass");
	} else if (p_mod.is_a("WeaponAimingSimulationModifier")) {
		r_w.aiming_controller = ref_partition(r_f, m, "AimingController");
	} else if (p_mod.is_a("WeaponSoundModifier")) {
		r_w.sound = ref_partition(r_f, m, "Sound");
	} else if (p_mod.is_a("WeaponAnimTypeModifier")) {
		r_w.weapon_anim_type = r_f.enum_text(m, "WeaponAnimType");
	} else {
		return false;
	}
	return true;
}

}

Result<WeaponDef> resolve_weapon(const EbxResolver &p_db, std::string_view p_partition, const WeaponResolveOptions &p_options) {
	Fields f(p_db);
	const EbxPartition *root = p_db.partition_by_name(p_partition);
	if (root == nullptr) {
		return make_error(std::string(p_partition) + ": partition not found");
	}
	WeaponDef w;
	ActiveUnlocks active;
	const EbxInstance primary = root->primary();
	std::optional<EbxInstance> blueprint;
	if (primary.is_a("SoldierWeaponUnlockAsset")) {
		w.variant = f.partition_name(*root);
		w.unlock_identifier = f.u32(primary.data(), "Identifier");
		blueprint = f.follow(primary.data(), "Weapon", "SoldierWeaponBlueprint", true);
		activate(f, primary, active);
	} else if (primary.is_a("SoldierWeaponBlueprint")) {
		blueprint = primary;
	} else {
		return make_error(std::string(p_partition) + ": a " + std::string(primary.type_name()) +
				", not a SoldierWeaponUnlockAsset or SoldierWeaponBlueprint");
	}
	if (!blueprint) {
		return f.error();
	}
	w.blueprint = f.partition_name(blueprint->partition());
	w.blueprint_name = f.text(blueprint->data(), "Name");

	const std::string folder = folder_of(w.blueprint);
	for (const std::string &u : p_options.unlocks) {
		const std::string name = (u.find('/') == std::string::npos && !folder.empty()) ? folder + "/" + u : u;
		const EbxPartition *up = p_db.partition_by_name(name);
		if (up == nullptr) {
			return make_error(name + ": unlock partition not found");
		}
		if (!up->primary().is_a("UnlockAssetBase")) {
			return make_error(name + ": a " + std::string(up->primary().type_name()) + ", not an unlock asset");
		}
		activate(f, up->primary(), active);
	}
	w.active_unlocks = active.names;

	const std::optional<EbxInstance> swd = f.follow(blueprint->data(), "Object", "SoldierWeaponData", true);
	if (!swd) {
		return f.error();
	}
	const EbxStruct s = swd->data();
	w.weapon_class = f.enum_text(s, "WeaponClass");
	w.anim_base_set = f.enum_text(s, "AnimBaseSet");
	w.damage_giver_name = f.text_or(s, "DamageGiverName");
	w.render_fov = f.f32(s, "RenderFov");
	w.silenced = f.flag_or(s, "IsSilenced", false);
	w.enable_breath_control = f.flag_or(s, "EnableBreathControl", false);
	w.can_be_in_supported_shooting = f.flag_or(s, "CanBeInSupportedShooting", false);
	w.allow_switching_out_of_ammo = f.flag_or(s, "AllowSwitchingToWeaponOutOfAmmo", true);
	w.aiming_controller = ref_partition(f, s, "AimingController");
	if (const std::optional<EbxInstance> lw = p_db.follow(swd->data(), "CustomWeaponType");
			lw && lw->is_a("LockingWeaponData")) {
		read_locking_weapon(f, *lw, w.locking);
	}

	const std::vector<ModifierEntry> mods = active_modifiers(f, s, active);
	std::optional<EbxInstance> wfd = f.follow(s, "WeaponFiring", "WeaponFiringData", true);
	for (const ModifierEntry &e : mods) {
		for (const EbxInstance &m : e.modifiers) {
			if (m.is_a("WeaponFiringDataModifier")) {
				wfd = f.follow(m.data(), "WeaponFiring", "WeaponFiringData", true);
				w.applied_modifiers.push_back(e.unlock + ": " + std::string(m.type_name()));
			}
		}
	}
	if (!f.ok() || !wfd) {
		return f.error();
	}
	read_firing_data(f, *wfd, w);
	const std::optional<EbxInstance> ffd = f.follow(wfd->data(), "PrimaryFire", "FiringFunctionData", true);
	if (!ffd) {
		return f.error();
	}
	detail::read_fire_function(f, *ffd, w.fire);
	std::optional<EbxInstance> projectile;
	if (const std::optional<EbxStruct> shot = f.sub(ffd->data(), "Shot")) {
		projectile = f.follow(*shot, "ProjectileData", "ProjectileEntityData", false);
	}
	w.sound = ref_partition(f, ffd->data(), "Sound");
	if (!f.ok()) {
		return f.error();
	}

	for (const ModifierEntry &e : mods) {
		for (const EbxInstance &m : e.modifiers) {
			if (m.is_a("WeaponFiringDataModifier")) {
				continue;
			}
			if (apply_modifier(f, m, w, projectile)) {
				w.applied_modifiers.push_back(e.unlock + ": " + std::string(m.type_name()));
			}
		}
	}
	if (projectile) {
		detail::read_projectile(f, *projectile, w.projectile);
	}
	if (const std::optional<EbxInstance> sway = f.follow(wfd->data(), "WeaponSway", "GunSwayData", false)) {
		detail::read_gun_sway(f, *sway, active, w.sway);
	}
	read_aiming(f, w.aiming_controller, w.aiming);
	if (!f.ok()) {
		return f.error();
	}
	if (p_options.ui != nullptr && w.unlock_identifier != 0) {
		w.name_sid = weapon_name_sid(*p_options.ui, w.unlock_identifier);
		w.name_sid_hash = w.name_sid.empty() ? 0 : loc_sid_hash(w.name_sid);
	}
	return w;
}

Result<WeaponDef> resolve_vehicle_weapon(const EbxResolver &p_db, std::string_view p_firing) {
	Fields f(p_db);
	const EbxPartition *root = p_db.partition_by_name(p_firing);
	if (root == nullptr) {
		return make_error(std::string(p_firing) + ": partition not found");
	}
	const EbxInstance primary = root->primary();
	if (!primary.is_a("WeaponFiringDataAsset")) {
		return make_error(
				std::string(p_firing) + ": a " + std::string(primary.type_name()) + ", not a WeaponFiringDataAsset");
	}
	const std::optional<EbxInstance> wfd = f.follow(primary.data(), "Data", "WeaponFiringData", true);
	if (!wfd) {
		return f.error();
	}
	WeaponDef w;
	w.variant = f.partition_name(*root);
	w.blueprint = w.variant;
	read_firing_data(f, *wfd, w);
	const std::optional<EbxInstance> ffd = f.follow(wfd->data(), "PrimaryFire", "FiringFunctionData", true);
	if (!ffd) {
		return f.error();
	}
	detail::read_fire_function(f, *ffd, w.fire);
	std::optional<EbxInstance> projectile;
	if (const std::optional<EbxStruct> shot = f.sub(ffd->data(), "Shot")) {
		projectile = f.follow(*shot, "ProjectileData", "ProjectileEntityData", false);
	}
	w.sound = ref_partition(f, ffd->data(), "Sound");
	if (projectile) {
		detail::read_projectile(f, *projectile, w.projectile);
	}
	if (!f.ok()) {
		return f.error();
	}
	return w;
}

Result<WeaponAiming> read_weapon_aiming(const EbxResolver &p_db, std::string_view p_controller) {
	Fields f(p_db);
	WeaponAiming a;
	read_aiming(f, p_controller, a);
	if (!f.ok()) {
		return f.error();
	}
	if (!a.present) {
		return make_error(std::string(p_controller) + ": no aiming controller partition");
	}
	return a;
}

Result<LockingWeapon> read_locking_weapon_data(const EbxResolver &p_db, const EbxInstance &p_locking) {
	if (!p_locking.is_a("LockingWeaponData")) {
		return make_error("a " + std::string(p_locking.type_name()) + ", not a LockingWeaponData");
	}
	Fields f(p_db);
	LockingWeapon w;
	read_locking_weapon(f, p_locking, w);
	if (!f.ok()) {
		return f.error();
	}
	return w;
}

Result<std::vector<WeaponUnlockModifier>> weapon_unlock_modifiers(
		const EbxResolver &p_db, std::string_view p_partition, std::string_view p_type) {
	Fields f(p_db);
	const EbxPartition *root = p_db.partition_by_name(p_partition);
	if (root == nullptr) {
		return make_error(std::string(p_partition) + ": partition not found");
	}
	if (!root->primary().is_a("SoldierWeaponUnlockAsset")) {
		return make_error(std::string(p_partition) + ": not a SoldierWeaponUnlockAsset");
	}
	const std::optional<EbxInstance> blueprint =
			f.follow(root->primary().data(), "Weapon", "SoldierWeaponBlueprint", true);
	const std::optional<EbxInstance> swd =
			blueprint ? f.follow(blueprint->data(), "Object", "SoldierWeaponData", true) : std::nullopt;
	if (!swd) {
		return f.error();
	}
	std::vector<WeaponUnlockModifier> out;
	const OptionalEbxArray entries = f.array(swd->data(), "WeaponModifierData");
	if (!entries) {
		return out;
	}
	for (const EbxValue element : *entries) {
		const std::optional<EbxStruct> e = element.as_struct();
		const std::optional<EbxRef> unlock = e ? f.ref(*e, "UnlockAsset") : std::nullopt;
		const OptionalEbxArray mods = unlock && !unlock->is_null() ? f.array(*e, "Modifiers") : OptionalEbxArray();
		if (!mods) {
			continue;
		}
		for (const EbxValue m : *mods) {
			const std::optional<EbxRef> r = m.as_ref();
			const std::optional<EbxInstance> mod =
					r && !r->is_null() ? f.follow_ref(e->partition(), *r, "", f.where(*e, "Modifiers")) : std::nullopt;
			if (mod && mod->is_a(p_type)) {
				out.push_back(
						WeaponUnlockModifier{ to_lower_ascii(f.target_partition(e->partition(), *unlock)), *mod });
			}
		}
	}
	if (!f.ok()) {
		return f.error();
	}
	return out;
}

Result<std::vector<WeaponUnlockSound>> weapon_unlock_sounds(const EbxResolver &p_db, std::string_view p_partition) {
	const Result<std::vector<WeaponUnlockModifier>> mods =
			weapon_unlock_modifiers(p_db, p_partition, "WeaponSoundModifier");
	if (!mods) {
		return mods.error();
	}
	Fields f(p_db);
	std::vector<WeaponUnlockSound> out;
	for (const WeaponUnlockModifier &m : *mods) {
		out.push_back(WeaponUnlockSound{ m.unlock, ref_partition(f, m.modifier.data(), "Sound") });
	}
	if (!f.ok()) {
		return f.error();
	}
	return out;
}

std::vector<std::string> list_weapon_variants(const EbxLibrary &p_library) {
	std::vector<std::string> out;
	for (const EbxLibrary::Entry &e : p_library.entries()) {
		if (e.name.starts_with("weapons/") && e.partition->primary().is_a("SoldierWeaponUnlockAsset")) {
			out.emplace_back(e.name);
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string base_variant_of(const EbxLibrary &p_library, std::string_view p_blueprint) {
	const EbxPartition *bp = p_library.partition_by_name(p_blueprint);
	if (bp == nullptr) {
		return {};
	}
	const std::string prefix = folder_of(to_lower_ascii(p_blueprint)) + "/";
	std::string best;
	for (const EbxLibrary::Entry &e : p_library.entries()) {
		if (!e.name.starts_with(prefix)) {
			continue;
		}
		const EbxInstance u = e.partition->primary();
		if (!u.is_a("SoldierWeaponUnlockAsset")) {
			continue;
		}
		const std::optional<EbxValue> weapon = u.get("Weapon");
		const std::optional<EbxValue> extra = u.get("Extra");
		const std::optional<EbxRef> wr = weapon ? weapon->as_ref() : std::nullopt;
		const std::optional<EbxRef> er = extra ? extra->as_ref() : std::nullopt;
		if (!wr || wr->kind != EbxRef::Kind::External || wr->partition != bp->guid() || !er || !er->is_null()) {
			continue;
		}
		if (best.empty() || e.name.size() < best.size() || (e.name.size() == best.size() && e.name < best)) {
			best = std::string(e.name);
		}
	}
	return best;
}

std::string weapon_name_sid(const EbxResolver &p_ui, uint32_t p_unlock_identifier) {
	const EbxPartition *meta = p_ui.partition_by_name("ui/uiweaponmetadata");
	if (meta == nullptr) {
		return {};
	}
	for (const EbxInstance &item : p_ui.follow_all(meta->primary().data(), "Items")) {
		if (!item.is_a("UIItemDescription")) {
			continue;
		}
		const std::optional<EbxValue> ids = item.get("ItemIds");
		const OptionalEbxArray list = ids ? ids->as_array() : OptionalEbxArray();
		if (!list) {
			continue;
		}
		for (const EbxValue id : *list) {
			if (id.as_u32() == std::optional<uint32_t>(p_unlock_identifier)) {
				const std::optional<EbxValue> name = item.get("Name");
				const std::optional<std::string_view> text = name ? name->as_str() : std::nullopt;
				return text ? std::string(*text) : std::string();
			}
		}
	}
	return {};
}

}
