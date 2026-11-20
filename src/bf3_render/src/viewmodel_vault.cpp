#include "bf3/render/viewmodel_vault.h"

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/core/math.h"
#include "bf3/core/util/format.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace bf3::render {

namespace {

struct CameraTravel {
	float metres = 0.0f;
	float degrees = 0.0f;
};

CameraTravel camera_travel(const anim::Skeleton &p_rig, const ViewmodelItem &p_item, const ViewmodelClip &p_clip,
		const anim::VaultPlay &p_play) {
	CameraTravel out;
	const int32_t cam = p_rig.find("CameraJoint");
	if (cam < 0) {
		return out;
	}
	anim::LocalPose base = anim::rest_pose(p_rig);
	core::ant::ClipPose sample;
	const ViewmodelClip &idle = p_item.clips[size_t(ViewmodelClipRole::Idle)];
	if (idle.source == ViewmodelClipSource::Clip && core::ant::sample_clip(idle.clip.clip, 0.0f, sample)) {
		anim::apply_clip_pose(idle.tracks, sample, base);
	}
	std::vector<anim::Xform> before, after;
	anim::model_pose(p_rig, base, before);
	const anim::BoneMask mask = anim::mask_from_tracks(p_rig, p_clip.tracks);
	const uint32_t steps = std::max<uint32_t>(1, p_clip.tracks.end_frame);
	for (uint32_t i = 0; i <= steps; ++i) {
		const float frame =
				std::min(p_play.frame_at(p_play.seconds() * float(i) / float(steps)), float(p_clip.tracks.end_frame));
		anim::LocalPose delta = anim::identity_pose(p_rig);
		if (!core::ant::sample_clip(p_clip.clip.clip, frame, sample)) {
			continue;
		}
		anim::apply_clip_pose(p_clip.tracks, sample, delta);
		anim::LocalPose pose = base;
		anim::apply_additive(pose, delta, mask, 1.0f, anim::AdditiveOrder::LocalPost);
		anim::model_pose(p_rig, pose, after);
		const anim::Xform d = anim::xform_mul(anim::xform_inverse(before[size_t(cam)]), after[size_t(cam)]);
		out.metres = std::max(out.metres, std::sqrt(d.t.x * d.t.x + d.t.y * d.t.y + d.t.z * d.t.z));
		const float w = std::clamp(std::fabs(d.q.w), 0.0f, 1.0f);
		out.degrees = std::max(out.degrees, 2.0f * std::acos(w) / core::kDegToRadF);
	}
	return out;
}

}

std::shared_ptr<const ViewmodelVaultClips> viewmodel_vault_clips(const core::ant::AntLibrary &p_library,
		const anim::Skeleton &p_rig, const anim::VaultControllers &p_controllers, uint32_t p_ant_ref,
		const ViewmodelItem &p_item, std::vector<std::string> &r_notes) {
	const std::string where = p_item.unlock + " 1p vault";
	core::Result<uint32_t> type = anim::weapon_type_index(p_library, p_ant_ref);
	core::Result<uint32_t> specific = anim::weapon_choice_index(p_library, p_ant_ref);
	if (!type || !specific) {
		r_notes.push_back(
				where + ": " + (!type ? type.error().message : specific.error().message) + " (no vault clip)");
		return nullptr;
	}
	anim::ChooserIndices idx;
	idx.weapon_type = *type;
	idx.specific_weapon = int64_t(*specific);
	auto v = std::make_shared<ViewmodelVaultClips>();
	uint32_t loaded = 0;
	for (size_t k = 0; k < anim::kVaultAnimKinds; ++k) {
		const anim::VaultController &c = p_controllers.first_person[k];
		core::Result<anim::AntClipRoute> route = anim::route_vault(p_library, c, idx);
		if (!route) {
			r_notes.push_back(where + " " + c.name + ": " + route.error().message + " (no vault clip)");
			continue;
		}
		core::Result<anim::VaultPlay> play = anim::vault_play_of(*route);
		if (!play) {
			r_notes.push_back(where + " " + c.name + ": " + play.error().message + " (no vault clip)");
			continue;
		}
		ViewmodelClip clip;
		if (const core::Status st = load_route_clip(p_library, p_rig, *route, clip); !st.ok()) {
			r_notes.push_back(where + " " + c.name + ": " + st.error().message + " (no vault clip)");
			continue;
		}
		if (!clip.clip.clip.additive || clip.tracks.end_frame == 0) {
			r_notes.push_back(where + " " + c.name + ": " + clip.name + " is not an additive clip (no vault clip)");
			continue;
		}
		const CameraTravel travel = camera_travel(p_rig, p_item, clip, *play);
		r_notes.push_back(core::fmt("%s %s: %s -> %s, %u frames over %.3f s, CameraJoint up to %.3f m and %.1f deg off "
									"the idle pose",
				where.c_str(), c.name.c_str(), route->controller.c_str(), clip.name.c_str(), clip.tracks.end_frame,
				double(play->seconds()), double(travel.metres), double(travel.degrees)));
		v->clips[k] = std::move(clip);
		v->play[k] = *play;
		v->path[k] = c.name + " -> " + route->controller;
		++loaded;
	}
	return loaded > 0 ? std::shared_ptr<const ViewmodelVaultClips>(std::move(v)) : nullptr;
}

void load_viewmodel_vault(const core::ant::AntLibrary &p_library, const anim::Skeleton &p_rig,
		const anim::VaultControllers &p_controllers, uint32_t p_ant_ref, ViewmodelItem &r_item,
		std::vector<std::string> &r_notes) {
	if (std::shared_ptr<const ViewmodelVaultClips> v =
					viewmodel_vault_clips(p_library, p_rig, p_controllers, p_ant_ref, r_item, r_notes)) {
		r_item.vault = std::move(v);
	}
}

std::vector<std::shared_ptr<const ViewmodelVaultClips>> load_viewmodel_set_vaults(
		const ViewmodelSet &p_set, std::vector<std::string> &r_notes) {
	std::vector<std::shared_ptr<const ViewmodelVaultClips>> out(p_set.items.size());
	if (p_set.library == nullptr) {
		r_notes.push_back("1p vault: the set has no ANT library (no vault clips)");
		return out;
	}
	const core::Result<anim::VaultControllers> controllers = anim::find_vault_controllers(*p_set.library);
	if (!controllers) {
		r_notes.push_back("1p vault: " + controllers.error().message + " (no vault clips)");
		return out;
	}
	for (size_t i = 0; i < p_set.items.size(); ++i) {
		const ViewmodelItem &item = p_set.items[i];
		if (item.weapon_ant_ref) {
			out[i] =
					viewmodel_vault_clips(*p_set.library, p_set.rig, *controllers, *item.weapon_ant_ref, item, r_notes);
		}
	}
	return out;
}

anim::VmVault viewmodel_vault(const anim::Skeleton &p_rig, const ViewmodelItem &p_item) {
	anim::VmVault out;
	out.camera_joint = p_rig.find("CameraJoint");
	if (const ViewmodelVaultClips *v = p_item.vault.get()) {
		for (size_t k = 0; k < anim::kVaultAnimKinds; ++k) {
			const ViewmodelClip &c = v->clips[k];
			if (c.source != ViewmodelClipSource::Clip) {
				continue;
			}
			out.clips[k].clip = &c.clip.clip;
			out.clips[k].tracks = &c.tracks;
			out.clips[k].seconds = c.seconds;
			out.play[k] = v->play[k];
		}
	}
	return out;
}

}
