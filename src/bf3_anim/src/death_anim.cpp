#include "bf3/anim/death_anim.h"

#include "bf3/core/util/format.h"

#include <algorithm>
#include <cmath>

namespace bf3::anim {

using bf3::core::fmt;

// INFERRED: the graph's RagdollBlend (notes R5-DT1 E) is the largest RagdollBlendValue tag of the playing death clip.
// Settle: a note on how the death clips drive RagdollBlend.
float CrowdDeathClip::ragdoll_target(float p_seconds) const {
	const float tick = p_seconds * kAntTicksPerSecond;
	float target = 0.0f;
	for (const AntFloatStateTag &t : ragdoll) {
		target = std::max(target, t.at(tick));
	}
	return std::clamp(target, 0.0f, 1.0f);
}

// INFERRED: DeathNode takes its first transition, in data order, whose one required signal is set.
// Settle: a note on StateFlow transition priority.
CrowdDeathNode CrowdDeathClips::node(const CrowdDeathChoice &p_choice) const {
	for (const CrowdDeathNode n : order) {
		if ((n == CrowdDeathNode::Explosion && p_choice.explosion) || (n == CrowdDeathNode::Prone && p_choice.prone) ||
				(n == CrowdDeathNode::Crouch && p_choice.crouch) || n == CrowdDeathNode::Stand) {
			return n;
		}
	}
	return CrowdDeathNode::Stand;
}

uint8_t CrowdDeathClips::choose(const CrowdDeathChoice &p_choice) const {
	switch (node(p_choice)) {
		case CrowdDeathNode::Prone:
			return prone;
		case CrowdDeathNode::Crouch: {
			// INFERRED: the crouch chooser takes the last Threshold at or below RandomValue, else its first entry.
			// Settle: a note on LinearSignalChooserPolicyAsset.
			uint8_t chosen = crouch.empty() ? kNoCrowdDeathClip : crouch.front().second;
			for (const auto &[threshold, clip] : crouch) {
				if (threshold <= p_choice.random_value) {
					chosen = clip;
				}
			}
			return chosen;
		}
		default:
			// STAND-IN: the explosion and stand nodes play no clip (their ContextDatabases are not evaluated).
			// Settle: a note on how a ContextDatabase matches.
			return kNoCrowdDeathClip;
	}
}

namespace {

core::Result<CrowdDeathClip> death_clip(
		const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const AntClipRoute &p_route) {
	core::Result<VaultPlay> play = vault_play_of(p_route);
	if (!play) {
		return core::error_at("death clip " + p_route.controller, play.error());
	}
	core::Result<CrowdClipData> clip = load_route_clip(p_set, p_library, p_route, "death clip " + p_route.controller);
	if (!clip) {
		return clip.error();
	}
	if (clip->tracks.end_frame == 0 || clip->mask.size() != p_set.rig.size()) {
		return core::make_error("death clip " + p_route.controller + ": not a rig-sized clip");
	}
	CrowdDeathClip out;
	out.clip = std::move(*clip);
	out.play = *play;
	out.controller = p_route.controller;
	for (const AntFloatStateTag &t : p_route.float_tags) {
		if (t.signal == kRagdollBlendSignal) {
			out.ragdoll.push_back(t);
		}
	}
	return out;
}

}

core::Result<CrowdDeathClips> load_crowd_death_clips(
		const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const ChooserIndices &p_indices) {
	core::Result<AntDeathFlow> flow = read_death_flow(p_library, p_indices);
	if (!flow) {
		return flow.error();
	}
	CrowdDeathClips out;
	for (const AntDeathNode &n : flow->nodes) {
		if (!n.requires_false.empty() || n.requires_true.size() > 1) {
			return core::make_error("death flow: node " + n.name + " has entry conditions beyond one required signal");
		}
		const std::string_view cond =
				n.requires_true.empty() ? std::string_view() : std::string_view(n.requires_true.front());
		if (cond.empty()) {
			out.order.push_back(CrowdDeathNode::Stand);
		} else if (cond == kDeathExplosionSignal) {
			out.order.push_back(CrowdDeathNode::Explosion);
		} else if (cond == kDeathProneSignal) {
			out.order.push_back(CrowdDeathNode::Prone);
		} else if (cond == kDeathCrouchSignal) {
			out.order.push_back(CrowdDeathNode::Crouch);
		} else {
			return core::make_error(
					"death flow: node " + n.name + " needs " + std::string(cond) + ", which no death input sets");
		}
	}
	if (flow->crouch_signal != kDeathRandomValueSignal) {
		return core::make_error("death flow: the crouch chooser reads " + flow->crouch_signal);
	}
	core::Result<CrowdDeathClip> prone = death_clip(p_set, p_library, flow->prone);
	if (!prone) {
		return prone.error();
	}
	out.prone = uint8_t(out.clips.size());
	out.clips.push_back(std::move(*prone));
	for (const AntLinearPick &p : flow->crouch) {
		if (out.clips.size() >= kNoCrowdDeathClip) {
			return core::make_error(fmt("death flow: more than %u clips", unsigned(kNoCrowdDeathClip)));
		}
		core::Result<CrowdDeathClip> c = death_clip(p_set, p_library, p.route);
		if (!c) {
			return c.error();
		}
		out.crouch.emplace_back(p.threshold, uint8_t(out.clips.size()));
		out.clips.push_back(std::move(*c));
	}
	return out;
}

core::Result<RagdollRig> RagdollRig::make(
		const Skeleton &p_rig, std::span<const std::string> p_bones, std::span<const Vec3> p_pivots) {
	if (p_bones.size() != p_pivots.size() || p_bones.empty()) {
		return core::make_error(fmt("ragdoll rig: %zu bones for %zu pivots", p_bones.size(), p_pivots.size()));
	}
	RagdollRig out;
	for (size_t b = 0; b < p_bones.size(); ++b) {
		const int32_t j = p_rig.find(p_bones[b]);
		if (j < 0) {
			return core::make_error("ragdoll rig: no joint " + p_bones[b] + " in the rig");
		}
		if (std::find(out.joints.begin(), out.joints.end(), j) != out.joints.end()) {
			return core::make_error("ragdoll rig: joint " + p_bones[b] + " carries two bodies");
		}
		out.joints.push_back(j);
		// INFERRED: a ragdoll body sits on its rig joint offset by minus the joint pivot, with the joint's rotation.
		// Settle: the RagdollResource body frames (notes R5-DT2 A).
		out.bone_to_body.push_back(Xform{ Quat{}, vec_scale(p_pivots[b], -1.0f) });
	}
	return out;
}

void RagdollRig::bodies(std::span<const Xform> p_model, const Xform &p_root, std::span<Xform> r_bodies) const {
	for (size_t b = 0; b < joints.size() && b < r_bodies.size(); ++b) {
		const size_t j = size_t(joints[b]);
		r_bodies[b] = j < p_model.size() ? xform_mul(p_root, xform_mul(p_model[j], bone_to_body[b]))
										 : xform_mul(p_root, bone_to_body[b]);
		r_bodies[b].q = quat_normalized(r_bodies[b].q);
	}
}

void RagdollRig::joints_model(std::span<const Xform> p_bodies, const Xform &p_root, std::span<Xform> r_model) const {
	const Xform to_model = xform_inverse(p_root);
	for (size_t b = 0; b < joints.size() && b < p_bodies.size() && b < r_model.size(); ++b) {
		r_model[b] = xform_mul(to_model, xform_mul(p_bodies[b], xform_inverse(bone_to_body[b])));
		r_model[b].q = quat_normalized(r_model[b].q);
	}
}

void ragdoll_local_pose(const Skeleton &p_rig, const LocalPose &p_pose, std::span<const int32_t> p_joints,
		std::span<const Xform> p_model, LocalPose &r_out) {
	const size_t n = std::min(p_rig.size(), p_pose.joints.size());
	r_out.joints.assign(p_pose.joints.begin(), p_pose.joints.begin() + std::ptrdiff_t(n));
	std::vector<int32_t> override_of(n, -1);
	for (size_t b = 0; b < p_joints.size() && b < p_model.size(); ++b) {
		if (p_joints[b] >= 0 && size_t(p_joints[b]) < n) {
			override_of[size_t(p_joints[b])] = int32_t(b);
		}
	}
	std::vector<Xform> model(n);
	for (size_t j = 0; j < n; ++j) {
		const int32_t parent = p_rig.bone(j).parent;
		if (override_of[j] >= 0) {
			model[j] = p_model[size_t(override_of[j])];
			r_out.joints[j] = parent < 0 ? model[j] : xform_mul(xform_inverse(model[size_t(parent)]), model[j]);
			r_out.joints[j].q = quat_normalized(r_out.joints[j].q);
		} else {
			model[j] = parent < 0 ? p_pose.joints[j] : xform_mul(model[size_t(parent)], p_pose.joints[j]);
		}
	}
}

}
