#include "bf3/anim/pose.h"

#include "bf3/core/ant/ant_library.h"

#include <algorithm>
#include <string>

namespace bf3::anim {

using core::ant::ChannelTrack;
using core::ant::ClipBinding;
using core::ant::ClipPose;
using core::ant::DecodedClip;
using core::ant::DofKind;

LocalPose rest_pose(const Skeleton &p_skeleton) {
	LocalPose p;
	p.joints.reserve(p_skeleton.size());
	for (const Bone &b : p_skeleton.bones()) {
		p.joints.push_back(b.rest);
	}
	return p;
}

core::Result<ClipTracks> bind_clip_tracks(const ClipBinding &p_binding, const DecodedClip &p_clip, const Skeleton &p_skeleton) {
	if (p_binding.rig == nullptr) {
		return core::make_error("clip tracks: the binding has no rig");
	}
	if (p_binding.tracks.size() != p_clip.channel_count()) {
		return core::make_error("clip tracks: " + std::to_string(p_binding.tracks.size()) + " tracks for " + std::to_string(p_clip.channel_count()) +
				" channels");
	}
	ClipTracks t;
	t.fps = p_binding.fps > 0.0f ? p_binding.fps : 30.0f;
	t.end_frame = p_clip.end_frame;
	t.rotation_bone.assign(p_clip.num_quats, -1);
	t.translation_bone.assign(p_clip.num_vec3s, -1);
	for (size_t c = 0; c < p_binding.tracks.size(); ++c) {
		const ChannelTrack &track = p_binding.tracks[c];
		int32_t bone = -1;
		if (track.joint >= 0 && size_t(track.joint) < p_binding.rig->joints.size() && track.kind_ok) {
			bone = p_skeleton.find(p_binding.rig->joints[size_t(track.joint)].name);
		}
		if (c < p_clip.num_quats) {
			t.rotation_bone[c] = bone;
		} else if (c < size_t(p_clip.num_quats) + p_clip.num_vec3s) {
			const bool translation = track.dof < p_binding.dofs.size() && p_binding.dofs[track.dof].kind == DofKind::Translation;
			if (!translation) {
				bone = -1;
			}
			t.translation_bone[c - p_clip.num_quats] = bone;
		} else {
			bone = -1;
		}
		if (bone >= 0) {
			++t.channels_bound;
		} else {
			++t.channels_unbound;
		}
	}
	return t;
}

void apply_clip_pose(const ClipTracks &p_tracks, const ClipPose &p_sample, LocalPose &r_pose) {
	const size_t nq = std::min(p_tracks.rotation_bone.size(), p_sample.quats.size());
	for (size_t c = 0; c < nq; ++c) {
		const int32_t b = p_tracks.rotation_bone[c];
		if (b >= 0 && size_t(b) < r_pose.joints.size()) {
			r_pose.joints[size_t(b)].q = p_sample.quats[c];
		}
	}
	const size_t nv = std::min(p_tracks.translation_bone.size(), p_sample.vec3s.size());
	for (size_t c = 0; c < nv; ++c) {
		const int32_t b = p_tracks.translation_bone[c];
		if (b >= 0 && size_t(b) < r_pose.joints.size()) {
			r_pose.joints[size_t(b)].t = p_sample.vec3s[c];
		}
	}
}

bool sample_pose(const Skeleton &p_skeleton, const DecodedClip &p_clip, const ClipTracks &p_tracks, float p_frame, LocalPose &r_pose, ClipPose &r_scratch) {
	r_pose.joints.resize(p_skeleton.size());
	for (size_t i = 0; i < p_skeleton.size(); ++i) {
		r_pose.joints[i] = p_skeleton.bone(i).rest;
	}
	if (!core::ant::sample_clip(p_clip, p_frame, r_scratch)) {
		return false;
	}
	apply_clip_pose(p_tracks, r_scratch, r_pose);
	return true;
}

void model_pose(const Skeleton &p_skeleton, const LocalPose &p_pose, std::vector<Xform> &r_model) {
	r_model.resize(p_skeleton.size());
	for (size_t i = 0; i < p_skeleton.size(); ++i) {
		const Xform &local = i < p_pose.joints.size() ? p_pose.joints[i] : p_skeleton.bone(i).rest;
		const int32_t parent = p_skeleton.bone(i).parent;
		r_model[i] = parent < 0 ? Xform{ quat_normalized(local.q), local.t } : xform_mul(r_model[size_t(parent)], local);
	}
}

PoseDiff pose_diff(const std::vector<Xform> &p_a, const std::vector<Xform> &p_b, const std::vector<bool> &p_mask) {
	PoseDiff d;
	const size_t n = std::min(p_a.size(), p_b.size());
	for (size_t i = 0; i < n; ++i) {
		if (!p_mask.empty() && (i >= p_mask.size() || !p_mask[i])) {
			continue;
		}
		const double deg = quat_angle_deg(p_a[i].q, p_b[i].q);
		const double m = vec_dist(p_a[i].t, p_b[i].t);
		if (deg > d.max_deg) {
			d.max_deg = deg;
			d.worst_rotation_bone = int32_t(i);
		}
		if (m > d.max_m) {
			d.max_m = m;
			d.worst_translation_bone = int32_t(i);
		}
	}
	return d;
}

}
