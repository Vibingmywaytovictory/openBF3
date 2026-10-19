// The 3p crowd clip set (S3; docs/design/s3.md §4.1, §5.2, §10). See crowd_clips.h.
#include "bf3/anim/crowd_clips.h"

#include "bf3/anim/clip_asset.h"
#include "bf3/core/gameplay/work_budget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bf3::anim {

using core::ant::AntLibrary;
using core::ant::AntPackage;
using core::ant::AntResult;
using core::ant::GdArray;
using core::ant::GdGuid;
using core::ant::GdStruct;

namespace {

// Ant AssetIds of SoldierWeaponData.WeaponStates[].Weapon: 0x15480000 | low 16 bits of the s_basicassets guid index
// (research 03 C8; s3.md §4.1).
constexpr uint32_t kAntRefHigh = 0x15480000u;
constexpr uint32_t kMaxRefHops = 16;
constexpr uint32_t kMaxChooserDepth = 8;

std::string hex(uint32_t p_v) {
	char b[16];
	std::snprintf(b, sizeof(b), "0x%x", p_v);
	return b;
}

// The controller a clip of p_package is bound to (package-local first, then static), as AntLibrary::bind finds it.
GdStruct controller_of(const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_clip) {
	const uint32_t index = p_clip.guid().index();
	GdStruct c = p_package.controller_for(index);
	if (!c.valid() && p_library.static_package() != nullptr && p_library.static_package() != &p_package) {
		c = p_library.static_package()->controller_for(index);
	}
	return c;
}

// Decodes and binds p_clip of p_package and tracks it on p_rig (when given).
core::Result<CrowdClipData> make_clip(const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_clip, const std::string &p_what) {
	CrowdClipData d;
	AntResult r = core::ant::decode_clip(p_clip, d.clip);
	if (!r.ok()) {
		return core::make_error(p_what + ": decode: " + r.message);
	}
	r = p_library.bind(p_package, p_clip, d.binding);
	if (!r.ok()) {
		return core::make_error(p_what + ": bind: " + r.message);
	}
	if (d.binding.rig == nullptr) {
		return core::make_error(p_what + ": no rig");
	}
	if (d.binding.rig->joints.size() > kMaxCrowdRigJoints) {
		return core::make_error(p_what + ": rig with " + std::to_string(d.binding.rig->joints.size()) + " joints (cap " + std::to_string(kMaxCrowdRigJoints) +
				")");
	}
	d.additive = d.clip.additive;
	d.controller_index = d.binding.controller.valid() ? d.binding.controller.guid().index() : 0;
	return d;
}

core::Status track_clip(CrowdClipData &r_clip, const Skeleton &p_rig, const std::string &p_what) {
	core::Result<ClipTracks> tracks = bind_clip_tracks(r_clip.binding, r_clip.clip, p_rig);
	if (!tracks) {
		return core::error_at(p_what, tracks.error());
	}
	r_clip.tracks = std::move(*tracks);
	r_clip.mask = mask_from_tracks(p_rig, r_clip.tracks);
	return core::Status::success();
}

// Model-space position of p_joint at p_frame.
Vec3 joint_model_position(const Skeleton &p_rig, const CrowdClipData &p_clip, int32_t p_joint, float p_frame, LocalPose &r_pose,
		core::ant::ClipPose &r_scratch, std::vector<Xform> &r_model) {
	sample_pose(p_rig, p_clip.clip, p_clip.tracks, p_frame, r_pose, r_scratch);
	model_pose(p_rig, r_pose, r_model);
	return r_model[size_t(p_joint)].t;
}

} // namespace

core::Result<CrowdClipSet> load_crowd_clips(const core::ant::AntPackageSource &p_source, const AntLibrary &p_library, const CrowdClipNames &p_names) {
	CrowdClipSet set;
	core::Result<std::unique_ptr<AntPackage>> package = load_ant_package(p_source, p_names.package);
	if (!package) {
		return package.error();
	}
	set.package = std::move(*package);
	const AntPackage &pkg = *set.package;
	const std::string where = "crowd clips (" + pkg.name() + ")";

	// One pass over the package's clips: the controller name of each (cheap: no bind), counted per wanted name.
	core::gameplay::WorkBudget budget;
	std::array<uint32_t, kCrowdClips> matches{};
	std::array<GdStruct, kCrowdClips> found{};
	for (const uint32_t ci : AntLibrary::clips(pkg)) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdStruct clip = pkg.object(ci);
		const GdStruct controller = controller_of(p_library, pkg, clip);
		if (!controller.valid()) {
			continue;
		}
		const std::string_view name = controller.name();
		for (size_t k = 0; k < kCrowdClips; ++k) {
			if (name == p_names.controllers[k]) {
				++matches[k];
				found[k] = clip;
			}
		}
	}
	for (size_t k = 0; k < kCrowdClips; ++k) {
		const bool optional = k == size_t(CrowdClip::RunNoise);
		if (matches[k] == 0 && optional) {
			continue;
		}
		if (matches[k] != 1) {
			return core::make_error(where + ": controller " + p_names.controllers[k] + " occurs " + std::to_string(matches[k]) + " times (must be exactly once)");
		}
		core::Result<CrowdClipData> d = make_clip(p_library, pkg, found[k], where + " " + p_names.controllers[k]);
		if (!d) {
			return d.error();
		}
		set.clips[k] = std::move(*d);
	}
	// Every clip on the idle clip's rig.
	const core::ant::AntRig *rig = set.clips[size_t(CrowdClip::Idle)].binding.rig;
	for (size_t k = 0; k < kCrowdClips; ++k) {
		const core::ant::AntRig *other = set.clips[k].binding.rig;
		if (other != nullptr && other != rig) {
			return core::make_error(where + ": " + p_names.controllers[k] + " binds to rig " + std::string(other->name) + ", not " + std::string(rig->name));
		}
	}
	core::Result<Skeleton> skeleton = skeleton_from_rig(*rig);
	if (!skeleton) {
		return core::error_at(where + " rig", skeleton.error());
	}
	set.rig = std::move(*skeleton);
	for (size_t k = 0; k < kCrowdClips; ++k) {
		if (set.clips[k].binding.rig == nullptr) {
			continue;
		}
		if (core::Status s = track_clip(set.clips[k], set.rig, where + " " + p_names.controllers[k]); !s.ok()) {
			return s.error();
		}
	}
	set.root_joint = set.rig.find(p_names.root_joint);
	if (set.root_joint < 0) {
		return core::make_error(where + ": the rig has no joint " + p_names.root_joint);
	}

	// Locomotion: durations and the measured root motion (rig model space, last frame minus first).
	static constexpr std::array<std::pair<LocoClip, CrowdClip>, 5> kLoco{ { { LocoClip::Idle, CrowdClip::Idle }, { LocoClip::Fwd, CrowdClip::RunFwd },
			{ LocoClip::Bwd, CrowdClip::RunBwd }, { LocoClip::Left, CrowdClip::RunLeft }, { LocoClip::Right, CrowdClip::RunRight } } };
	LocalPose pose;
	core::ant::ClipPose scratch;
	std::vector<Xform> model;
	for (const auto &[loco, clip] : kLoco) {
		const CrowdClipData &c = set.get(clip);
		LocoClipInfo info;
		if (loco != LocoClip::Idle) {
			info.duration = c.tracks.duration_seconds();
			if (!(info.duration > 0.0f)) {
				return core::make_error(where + ": " + p_names.controllers[size_t(clip)] + " has no duration");
			}
			const Vec3 a = joint_model_position(set.rig, c, set.root_joint, 0.0f, pose, scratch, model);
			const Vec3 b = joint_model_position(set.rig, c, set.root_joint, float(c.tracks.end_frame), pose, scratch, model);
			const float dx = b.x - a.x, dz = b.z - a.z;
			const float len = std::sqrt(dx * dx + dz * dz);
			if (!(len > 1.0e-3f) || !std::isfinite(len)) {
				return core::make_error(where + ": " + p_names.controllers[size_t(clip)] + " has no root motion on " + p_names.root_joint);
			}
			info.root_speed = len / info.duration;
			info.root_dir = Vec3{ dx / len, 0.0f, dz / len };
		}
		set.loco[size_t(loco)] = info;
	}
	return set;
}

core::Result<uint32_t> weapon_choice_index(const AntLibrary &p_library, uint32_t p_ant_ref_asset_id) {
	const AntPackage *stat = p_library.static_package();
	const std::string where = "weapon AntRef " + std::to_string(p_ant_ref_asset_id);
	if (stat == nullptr) {
		return core::make_error(where + ": the library has no static package");
	}
	if ((p_ant_ref_asset_id & 0xFFFF0000u) != kAntRefHigh) {
		return core::make_error(where + ": not an s_basicassets AntRef (high bits " + hex(p_ant_ref_asset_id & 0xFFFF0000u) + ")");
	}
	const uint32_t low = p_ant_ref_asset_id & 0xFFFFu;
	core::gameplay::WorkBudget budget;
	GdStruct weapon;
	uint32_t hits = 0;
	for (size_t i = 0; i < stat->object_count(); ++i) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdStruct o = stat->object(i);
		if ((o.guid().index() & 0xFFFFu) != low || o.type_name() != "WeaponAsset") {
			continue;
		}
		++hits;
		weapon = o;
	}
	if (hits != 1) {
		return core::make_error(where + ": " + std::to_string(hits) + " WeaponAssets with guid index low bits " + hex(low) + " (must be exactly one)");
	}
	// WeaponAsset.WeaponEnum -> EnumerationEnumeratorPair.Enumerator -> EnumerationValueAsset.Value.
	uint32_t hops = 0;
	auto follow = [&](const GdStruct &p_from, const char *p_field, const char *p_type, GdStruct &r_to) -> core::Status {
		if (++hops > kMaxRefHops) {
			return core::make_error(where + ": more than " + std::to_string(kMaxRefHops) + " reference hops");
		}
		if (core::Status s = budget.charge(1); !s.ok()) {
			return s;
		}
		GdGuid g;
		if (!p_from.read_guid(p_field, g) || g.is_null()) {
			return core::make_error(where + ": " + std::string(p_from.type_name()) + " '" + std::string(p_from.name()) + "' has no " + p_field);
		}
		r_to = p_library.resolve(*stat, g);
		if (!r_to.valid()) {
			return core::make_error(where + ": " + p_field + " " + hex(g.index()) + " does not resolve");
		}
		if (r_to.type_name() != p_type) {
			return core::make_error(where + ": " + p_field + " is a " + std::string(r_to.type_name()) + ", not a " + p_type);
		}
		return core::Status::success();
	};
	GdStruct pair, value;
	if (core::Status s = follow(weapon, "WeaponEnum", "EnumerationEnumeratorPair", pair); !s.ok()) {
		return s.error();
	}
	if (core::Status s = follow(pair, "Enumerator", "EnumerationValueAsset", value); !s.ok()) {
		return s.error();
	}
	uint32_t index = 0;
	if (!value.read_u32("Value", index)) {
		return core::make_error(where + ": EnumerationValueAsset '" + std::string(value.name()) + "' has no Value");
	}
	return index;
}

core::Result<CrowdClipData> load_chosen_clip(const CrowdClipSet &p_set, const AntLibrary &p_library, std::string_view p_chooser, uint32_t p_index) {
	const std::string where = "chooser " + std::string(p_chooser) + "[" + std::to_string(p_index) + "]";
	if (!p_set.package || p_library.static_package() == nullptr) {
		return core::make_error(where + ": no package or no static package");
	}
	const AntPackage &pkg = *p_set.package;
	const AntPackage &stat = *p_library.static_package();
	core::gameplay::WorkBudget budget;
	// The chooser: unique by name among the IndexChooserControllerAssets of s_basicassets and the package.
	GdStruct chooser;
	const AntPackage *chooser_package = nullptr;
	uint32_t hits = 0;
	for (const AntPackage *p : { &stat, &pkg }) {
		for (size_t i = 0; i < p->object_count(); ++i) {
			if (core::Status s = budget.charge(1); !s.ok()) {
				return core::error_at(where, s.error());
			}
			const GdStruct o = p->object(i);
			if (o.name() == p_chooser && o.type_name() == "IndexChooserControllerAsset") {
				++hits;
				chooser = o;
				chooser_package = p;
			}
		}
	}
	if (hits != 1) {
		return core::make_error(where + ": " + std::to_string(hits) + " IndexChooserControllerAssets of that name (must be exactly one)");
	}
	// ChoiceAssetList[index], through chooser fallbacks at most kMaxChooserDepth deep, to a ClipControllerAsset.
	GdStruct controller;
	const AntPackage *controller_package = nullptr;
	for (uint32_t depth = 0;; ++depth) {
		if (depth >= kMaxChooserDepth) {
			return core::make_error(where + ": chooser fallbacks deeper than " + std::to_string(kMaxChooserDepth));
		}
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdArray list = chooser.array("ChoiceAssetList");
		if (!list.valid() || p_index >= list.size()) {
			return core::make_error(where + ": index outside ChoiceAssetList (" + std::to_string(list.size()) + " entries)");
		}
		GdGuid g;
		if (!list.read_guid(p_index, g) || g.is_null()) {
			return core::make_error(where + ": empty ChoiceAssetList entry");
		}
		const AntPackage &target_package = p_library.package_of(*chooser_package, g);
		const GdStruct entry = target_package.find(g.index());
		if (!entry.valid()) {
			return core::make_error(where + ": entry " + hex(g.index()) + " does not resolve");
		}
		if (entry.type_name() == "ClipControllerAsset") {
			controller = entry;
			controller_package = &target_package;
			break;
		}
		if (entry.type_name() == "IndexChooserControllerAsset") {
			chooser = entry;
			chooser_package = &target_package;
			continue;
		}
		return core::make_error(where + ": entry " + hex(g.index()) + " is a " + std::string(entry.type_name()));
	}
	// The controller's clip: its Anim reference index names the clip in the package (as AntLibrary::bind maps it).
	GdGuid anim;
	if (!controller.read_guid("Anim", anim) || anim.is_null()) {
		return core::make_error(where + ": controller " + hex(controller.guid().index()) + " has no Anim");
	}
	GdStruct clip = pkg.find(anim.index());
	const AntPackage *clip_package = &pkg;
	if (!clip.valid()) {
		clip = controller_package->find(anim.index());
		clip_package = controller_package;
	}
	if (!clip.valid() || !core::ant::is_clip(clip)) {
		return core::make_error(where + ": controller " + hex(controller.guid().index()) + " Anim " + hex(anim.index()) + " is not a clip of " + pkg.name());
	}
	core::Result<CrowdClipData> d = make_clip(p_library, *clip_package, clip, where);
	if (!d) {
		return d.error();
	}
	if (d->binding.controller.guid().index() != controller.guid().index()) {
		return core::make_error(where + ": the clip binds to controller " + hex(d->binding.controller.guid().index()) + ", not the chosen " +
				hex(controller.guid().index()));
	}
	const core::ant::AntRig *rig = p_set.get(CrowdClip::Idle).binding.rig;
	if (rig != nullptr && d->binding.rig != rig) {
		return core::make_error(where + ": the clip binds to rig " + std::string(d->binding.rig->name) + ", not " + std::string(rig->name));
	}
	if (core::Status s = track_clip(*d, p_set.rig, where); !s.ok()) {
		return s.error();
	}
	return d;
}

core::Result<AimMeasure> measure_aim(const CrowdClipSet &p_set, AdditiveOrder p_order) {
	const Skeleton &rig = p_set.rig;
	const int32_t wep = rig.find("Wep_Root");
	if (wep < 0) {
		return core::make_error("aim: the rig has no Wep_Root");
	}
	core::ant::ClipPose scratch;
	LocalPose base;
	const CrowdClipData &idle = p_set.get(CrowdClip::Idle);
	if (!sample_pose(rig, idle.clip, idle.tracks, 0.0f, base, scratch)) {
		return core::make_error("aim: the idle clip does not sample");
	}
	std::vector<Xform> model;
	model_pose(rig, base, model);
	AimMeasure m;
	float best = -2.0f;
	for (int a = 0; a < 3; ++a) {
		Vec3 axis{};
		(a == 0 ? axis.x : a == 1 ? axis.y : axis.z) = 1.0f;
		const Vec3 v = quat_rotate(model[size_t(wep)].q, axis);
		for (const float sign : { 1.0f, -1.0f }) {
			if (v.z * sign > best) {
				best = v.z * sign;
				m.barrel_axis = a;
				m.barrel_negative = sign < 0.0f;
			}
		}
	}
	auto barrel = [&](const std::vector<Xform> &p_model, double &r_elevation, double &r_azimuth) {
		Vec3 axis{};
		(m.barrel_axis == 0 ? axis.x : m.barrel_axis == 1 ? axis.y : axis.z) = m.barrel_negative ? -1.0f : 1.0f;
		const Vec3 d = quat_rotate(p_model[size_t(wep)].q, axis);
		r_elevation = std::asin(std::clamp(double(d.y), -1.0, 1.0)) * 57.29577951308232;
		r_azimuth = std::atan2(double(d.x), double(d.z)) * 57.29577951308232;
	};
	double rest_el = 0.0, rest_az = 0.0;
	barrel(model, rest_el, rest_az);
	m.monotonic = true;
	for (const CrowdClip clip : { CrowdClip::AimUp, CrowdClip::AimDown }) {
		const CrowdClipData &c = p_set.get(clip);
		if (c.mask.size() != rig.size()) {
			return core::make_error("aim: no aim clip");
		}
		LocalPose delta = identity_pose(rig);
		if (!core::ant::sample_clip(c.clip, float(c.tracks.end_frame), scratch)) {
			return core::make_error("aim: an aim clip does not sample");
		}
		apply_clip_pose(c.tracks, scratch, delta);
		double last = 0.0, el = 0.0, az = 0.0;
		for (const float w : { 0.25f, 0.5f, 0.75f, 1.0f }) {
			LocalPose p = base;
			apply_additive(p, delta, c.mask, w, p_order);
			std::vector<Xform> pm;
			model_pose(rig, p, pm);
			barrel(pm, el, az);
			const double mag = std::fabs(el - rest_el);
			m.monotonic = m.monotonic && mag >= last - 0.01;
			last = mag;
		}
		const float de = float(el - rest_el);
		const float daz = float(std::fabs(std::remainder(az - rest_az, 360.0)));
		if (clip == CrowdClip::AimUp) {
			m.up_deg = de;
			m.up_azimuth_deg = daz;
		} else {
			m.down_deg = de;
			m.down_azimuth_deg = daz;
		}
	}
	return m;
}

} // namespace bf3::anim

// ---- M2a tp (docs/design/m2a.md §9; docs/design/m2a-tp.md): the crouch and prone moves, the crouch fire, the deaths ----

namespace bf3::anim {

namespace {

// Root displacement of a move clip over its cycle (rig model space, last frame minus first; y dropped), as
// load_crowd_clips measures the stand cycles.
bool move_root_motion(const CrowdClipSet &p_set, const CrowdClipData &p_clip, Vec3 &r_displacement, float &r_duration) {
	r_duration = p_clip.tracks.duration_seconds();
	if (!(r_duration > 0.0f) || p_set.root_joint < 0 || size_t(p_set.root_joint) >= p_set.rig.size()) {
		return false;
	}
	LocalPose pose;
	core::ant::ClipPose scratch;
	std::vector<Xform> model;
	const Vec3 a = joint_model_position(p_set.rig, p_clip, p_set.root_joint, 0.0f, pose, scratch, model);
	const Vec3 b = joint_model_position(p_set.rig, p_clip, p_set.root_joint, float(p_clip.tracks.end_frame), pose, scratch, model);
	r_displacement = Vec3{ b.x - a.x, 0.0f, b.z - a.z };
	return std::isfinite(r_displacement.x) && std::isfinite(r_displacement.z);
}

bool loco_info(const Vec3 &p_displacement, float p_duration, LocoClipInfo &r_info) {
	const float len = std::sqrt(p_displacement.x * p_displacement.x + p_displacement.z * p_displacement.z);
	if (!(len > 1.0e-3f) || !std::isfinite(len) || !(p_duration > 0.0f)) {
		return false;
	}
	r_info.duration = p_duration;
	r_info.root_speed = len / p_duration;
	r_info.root_dir = Vec3{ p_displacement.x / len, 0.0f, p_displacement.z / len };
	return true;
}

} // namespace

core::Result<CrowdMoveClips> load_crowd_move_clips(const CrowdClipSet &p_set, const AntLibrary &p_library, const CrowdMoveClipNames &p_names) {
	if (!p_set.package || p_set.rig.size() == 0) {
		return core::make_error("crowd move clips: the crowd clip set has no package or no rig");
	}
	CrowdMoveClips out;
	const AntPackage &pkg = *p_set.package;
	const std::string where = "crowd move clips (" + pkg.name() + ")";
	core::gameplay::WorkBudget budget;
	std::array<uint32_t, kCrowdMoveClips> matches{};
	std::array<GdStruct, kCrowdMoveClips> found{};
	for (const uint32_t ci : AntLibrary::clips(pkg)) {
		if (core::Status s = budget.charge(1); !s.ok()) {
			return core::error_at(where, s.error());
		}
		const GdStruct clip = pkg.object(ci);
		const GdStruct controller = controller_of(p_library, pkg, clip);
		if (!controller.valid()) {
			continue;
		}
		const std::string_view name = controller.name();
		for (size_t k = 0; k < kCrowdMoveClips; ++k) {
			if (name == p_names.controllers[k]) {
				++matches[k];
				found[k] = clip;
			}
		}
	}
	const core::ant::AntRig *rig = p_set.get(CrowdClip::Idle).binding.rig;
	for (size_t k = 0; k < kCrowdMoveClips; ++k) {
		const std::string what = where + " " + p_names.controllers[k];
		if (matches[k] != 1) {
			out.notes.push_back(what + ": occurs " + std::to_string(matches[k]) + " times (must be exactly once): not used");
			continue;
		}
		core::Result<CrowdClipData> d = make_clip(p_library, pkg, found[k], what);
		if (!d) {
			out.notes.push_back(d.error().message + ": not used");
			continue;
		}
		if (rig != nullptr && d->binding.rig != rig) {
			out.notes.push_back(what + ": binds to rig " + std::string(d->binding.rig->name) + ", not " + std::string(rig->name) + ": not used");
			continue;
		}
		if (core::Status s = track_clip(*d, p_set.rig, what); !s.ok()) {
			out.notes.push_back(s.error().message + ": not used");
			continue;
		}
		if (d->tracks.rotation_bone.empty() && d->tracks.translation_bone.empty()) {
			out.notes.push_back(what + ": no tracks on the rig: not used");
			continue;
		}
		out.clips[k] = std::move(*d);
		out.present[k] = true;
	}
	// The stance move tables (crouch, prone), each direction from its measured root motion.
	struct Dir {
		LocoClip loco;
		CrowdMoveClip clip;
	};
	static constexpr Dir kDirs[kCrowdMoveStances][4] = {
		{ { LocoClip::Fwd, CrowdMoveClip::SneakFwd }, { LocoClip::Bwd, CrowdMoveClip::SneakBwdLeft }, { LocoClip::Left, CrowdMoveClip::SneakLeft },
				{ LocoClip::Right, CrowdMoveClip::SneakRight } },
		{ { LocoClip::Fwd, CrowdMoveClip::CrawlFwd }, { LocoClip::Bwd, CrowdMoveClip::CrawlBwd }, { LocoClip::Left, CrowdMoveClip::CrawlLeft },
				{ LocoClip::Right, CrowdMoveClip::CrawlRight } },
	};
	static constexpr const char *kStanceName[kCrowdMoveStances] = { "crouch", "prone" };
	for (size_t st = 0; st < kCrowdMoveStances; ++st) {
		bool ok = true;
		std::string line = std::string(kStanceName[st]) + " moves:";
		for (const Dir &d : kDirs[st]) {
			Vec3 disp{};
			float duration = 0.0f;
			bool have = false;
			if (st == 0 && d.loco == LocoClip::Bwd) {
				// The two backward diagonals at one phase: mean duration, mean displacement.
				Vec3 a{}, b{};
				float da = 0.0f, db = 0.0f;
				have = out.has(CrowdMoveClip::SneakBwdLeft) && out.has(CrowdMoveClip::SneakBwdRight) &&
						move_root_motion(p_set, out.get(CrowdMoveClip::SneakBwdLeft), a, da) && move_root_motion(p_set, out.get(CrowdMoveClip::SneakBwdRight), b, db);
				if (have) {
					duration = 0.5f * (da + db);
					// Displacements per second, averaged, times the mean duration (the blend plays both at one phase).
					disp = Vec3{ 0.5f * (a.x / da + b.x / db) * duration, 0.0f, 0.5f * (a.z / da + b.z / db) * duration };
				}
			} else {
				have = out.has(d.clip) && move_root_motion(p_set, out.get(d.clip), disp, duration);
			}
			LocoClipInfo info;
			if (!have || !loco_info(disp, duration, info)) {
				ok = false;
				line += " " + std::to_string(int(d.loco)) + " missing";
				continue;
			}
			out.loco[st][size_t(d.loco)] = info;
			char b[96];
			std::snprintf(b, sizeof(b), " %d: %.2f m/s over %.2f s dir (%.2f, %.2f)", int(d.loco), double(info.root_speed), double(info.duration),
					double(info.root_dir.x), double(info.root_dir.z));
			line += b;
		}
		out.moves_ok[st] = ok;
		out.notes.push_back(line + (ok ? "" : " -> the stand cycles move this stance"));
	}
	return out;
}

} // namespace bf3::anim

// ---- PT1 run (docs/design/pt1-run.md): BF3's move groups on the crowd rig (crowd_clips.h) ----------------------------

namespace bf3::anim {

namespace {

float pt1_wrap_pi(float p_a) {
	constexpr float kPi = 3.14159265358979f;
	if (!std::isfinite(p_a) || std::fabs(p_a) > 1.0e4f) {
		return 0.0f;
	}
	while (p_a > kPi) {
		p_a -= 2.0f * kPi;
	}
	while (p_a <= -kPi) {
		p_a += 2.0f * kPi;
	}
	return p_a;
}

} // namespace

core::Result<CrowdMoveGroupSet> load_crowd_move_groups(const CrowdClipSet &p_set, const AntLibrary &p_library, const CrowdMoveGroupNames &p_names) {
	if (!p_set.package || p_set.rig.size() == 0 || p_set.root_joint < 0 || p_library.static_package() == nullptr) {
		return core::make_error("crowd move groups: no clip package, rig, root joint or static package");
	}
	const AntPackage &pkg = *p_set.package;
	const core::ant::AntRig *rig = p_set.get(CrowdClip::Idle).binding.rig;
	CrowdMoveGroupSet out;
	LocalPose pose;
	core::ant::ClipPose scratch;
	std::vector<Xform> model;
	for (size_t r = 0; r < kCrowdMoveGroupRoles; ++r) {
		CrowdMoveGroup &g = out.groups[r];
		const std::string where = "move group " + p_names.groups[r];
		core::Result<MoveGroupDef> def = read_move_group(p_library, p_names.groups[r], p_names.indices);
		if (!def) {
			out.notes.push_back(def.error().message);
			continue;
		}
		g.def = std::move(*def);
		std::string why;
		for (const AntClipRoute &route : g.def.clips) {
			GdStruct clip = pkg.find(route.anim_index);
			const AntPackage *clip_package = &pkg;
			if ((!clip.valid() || !core::ant::is_clip(clip)) && route.controller_package != nullptr) {
				clip = route.controller_package->find(route.anim_index);
				clip_package = route.controller_package;
			}
			if (!clip.valid() || !core::ant::is_clip(clip)) {
				why = route.controller + ": Anim " + hex(route.anim_index) + " is not a clip of " + pkg.name();
				break;
			}
			core::Result<CrowdClipData> d = make_clip(p_library, *clip_package, clip, where + " " + route.controller);
			if (!d) {
				why = d.error().message;
				break;
			}
			if (d->binding.rig != rig) {
				why = route.controller + " binds to rig " + std::string(d->binding.rig->name) + ", not the crowd rig";
				break;
			}
			if (core::Status s = track_clip(*d, p_set.rig, where + " " + route.controller); !s.ok()) {
				why = s.error().message;
				break;
			}
			MoveGroupClipTiming tm;
			Vec3 dir{};
			if (d->tracks.end_frame > 1) {
				tm.seconds = d->tracks.duration_seconds();
				const Vec3 a = joint_model_position(p_set.rig, *d, p_set.root_joint, 0.0f, pose, scratch, model);
				const Vec3 b = joint_model_position(p_set.rig, *d, p_set.root_joint, float(d->tracks.end_frame), pose, scratch, model);
				const float dx = b.x - a.x, dz = b.z - a.z;
				const float len = std::sqrt(dx * dx + dz * dz);
				if (tm.seconds > 0.0f && std::isfinite(len) && len > 1.0e-3f) {
					tm.root_velocity = len / (tm.seconds * kAntTicksPerSecond);
					dir = Vec3{ dx / len, 0.0f, dz / len };
				}
			}
			g.clips.push_back(std::move(*d));
			g.timing.push_back(tm);
			g.root_dir.push_back(dir);
		}
		if (!why.empty()) {
			out.notes.push_back(where + ": " + why + " (the stance keeps its cycles)");
			g.clips.clear();
			g.timing.clear();
			g.root_dir.clear();
			continue;
		}
		// The outer X sign that puts the moving clips' measured root headings on their directions' X.
		float err[2] = { 0.0f, 0.0f }, worst[2] = { 0.0f, 0.0f };
		uint32_t measured = 0;
		for (const MoveGroupDirection &d : g.def.directions) {
			for (const MoveGroupEntry &e : d.entries) {
				const Vec3 &rd = g.root_dir[e.clip];
				if (!(e.velocity > 0.0f) || (rd.x == 0.0f && rd.z == 0.0f)) {
					continue;
				}
				++measured;
				const float h = std::atan2(rd.x, rd.z);
				for (int s = 0; s < 2; ++s) {
					const float off = std::fabs(pt1_wrap_pi(d.x - (s == 0 ? 1.0f : -1.0f) * h));
					err[s] += off;
					worst[s] = std::max(worst[s], off);
				}
			}
		}
		if (measured == 0) {
			out.notes.push_back(where + ": no moving clip with root motion (the stance keeps its cycles)");
			continue;
		}
		const int s = err[1] <= err[0] ? 1 : 0;
		g.x_sign = s == 0 ? 1.0f : -1.0f;
		g.worst_direction_deg = worst[s] * 57.2957795f;
		g.ok = true;
		char line[256];
		std::snprintf(line, sizeof(line), "%s: %zu directions, %zu clips, OneToOneSpeed %g, MinSpeed %g m/tick, X sign %+.0f (worst direction %.1f deg)",
				where.c_str(), g.def.directions.size(), g.def.clips.size(), double(g.def.one_to_one_speed), double(g.def.min_speed), double(g.x_sign),
				double(g.worst_direction_deg));
		out.notes.push_back(line);
		for (size_t c = 0; c < g.clips.size(); ++c) {
			std::snprintf(line, sizeof(line), "  %s: %u frames, %.3f s, root %.3f m/s", g.def.clips[c].controller.c_str(), g.clips[c].tracks.end_frame,
					double(g.timing[c].seconds), double(g.timing[c].root_velocity * kAntTicksPerSecond));
			out.notes.push_back(line);
		}
	}
	return out;
}

} // namespace bf3::anim
