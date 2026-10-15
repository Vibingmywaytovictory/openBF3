// The 3p crowd clip set (S3; docs/design/s3.md §4.1, §5.2, §10). See crowd_clips.h.
#include "bf3/anim/crowd_clips.h"

#include "bf3/anim/clip_asset.h"
#include "bf3/core/gameplay/work_budget.h"

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

} // namespace bf3::anim
