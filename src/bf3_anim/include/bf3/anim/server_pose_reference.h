#pragma once
// E18 (a)'s reference evaluation of the server pose (docs/design/m2a.md E18, §6.5; as built: docs/design/m2a-lc.md;
// owner lc). Header-only and used only by the checks (ctest anim.server_pose and `bf3dump lagcomp --deviation`); the
// table builder (server_pose.cpp) does not include it.
//
// Per query, with no pitch buckets, no fraction steps, no LOD and no RS skeleton: bf3_anim's sample_pose of the
// stance idle at frame 0 (or of the transition clip at exactly fraction x its end frame), the root joint's translation
// back to rest, apply_additive of the stance's AimUp / AimDown delta at the exact weight min(1, |pitch| / full), then
// model_pose, and the hi-LOD capsules placed from their bones' model transforms in double. The placement rule
// (server_pose.h: start = bone transform x offset, end = start + length along the signed BoneAxis, the sign toward the
// bone's main child, a sphere at length <= 0.01) is written out again here rather than shared, so a slip in either
// copy shows up as a deviation. The same placement also turns S3's CrowdEvaluator model pose into capsules (the stand
// rows' independent code path).

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/server_pose.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/result.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::anim::server_pose_reference {

struct RefCapsule {
	double a[3]{};
	double b[3]{};
	double centre[3]{};
	size_t list_index = 0; // BoneCollisionData index
	int32_t joint = -1;
};

struct Query {
	bool transition = false;
	size_t row = 0; // stance 0..2, or transition 0..5 (core::gameplay::CapsuleTransition order)
	double pitch_deg = 0;
	double fraction = 0; // transition rows: 0..1 of the clip's own length
};

class Reference {
public:
	// p_capsules: the whole BoneCollisionData list (the hi-LOD ones are placed, in list order); p_aim: the full-aim
	// pitch per stance (the builder's report, checked separately against measure_aim).
	static core::Result<Reference> make(const ServerPoseClips &p_clips, std::span<const core::gameplay::HitCapsule> p_capsules,
			const std::array<StanceAim, core::gameplay::kCapsuleStances> &p_aim, AdditiveOrder p_order) {
		Reference r;
		r.clips_ = &p_clips;
		r.aim_ = p_aim;
		r.order_ = p_order;
		const Skeleton &rig = p_clips.rig;
		for (size_t i = 0; i < p_capsules.size(); ++i) {
			const core::gameplay::HitCapsule &c = p_capsules[i];
			if (!c.valid_hi_lod) {
				continue;
			}
			Placed p;
			p.list_index = i;
			p.joint = rig.find(c.bone);
			p.offset = { double(c.offset[0]), double(c.offset[1]), double(c.offset[2]) };
			p.length = double(c.length);
			const int axis = c.bone_axis >= 0 && c.bone_axis <= 2 ? c.bone_axis : 0;
			// The main child: the child joint with the longest rest translation; its component on the axis gives the sign.
			double best = -1, sign = 1;
			for (size_t j = 0; j < rig.size(); ++j) {
				if (p.joint < 0 || rig.bone(j).parent != p.joint) {
					continue;
				}
				const Vec3 &t = rig.bone(j).rest.t;
				const double len = std::sqrt(double(t.x) * t.x + double(t.y) * t.y + double(t.z) * t.z);
				if (len > best) {
					best = len;
					const double comp = axis == 0 ? t.x : axis == 1 ? t.y : t.z;
					sign = comp < 0 ? -1.0 : 1.0;
				}
			}
			p.dir = { axis == 0 ? sign : 0.0, axis == 1 ? sign : 0.0, axis == 2 ? sign : 0.0 };
			r.placed_.push_back(p);
		}
		if (r.placed_.empty()) {
			return core::make_error("server pose reference: no hi-LOD capsule");
		}
		static constexpr ServerPoseClip kUp[3] = { ServerPoseClip::StandAimUp, ServerPoseClip::CrouchAimUp, ServerPoseClip::ProneAimUp };
		static constexpr ServerPoseClip kDown[3] = { ServerPoseClip::StandAimDown, ServerPoseClip::CrouchAimDown, ServerPoseClip::ProneAimDown };
		core::ant::ClipPose scratch;
		for (size_t s = 0; s < core::gameplay::kCapsuleStances; ++s) {
			for (int k = 0; k < 2; ++k) {
				const ServerPoseClip role = k == 0 ? kUp[s] : kDown[s];
				LocalPose &d = k == 0 ? r.up_[s] : r.down_[s];
				d = identity_pose(rig);
				if (!p_clips.present[size_t(role)]) {
					continue;
				}
				const CrowdClipData &clip = p_clips.get(role);
				// The additive delta: the aim clip sampled onto identity at its last frame (S3's rule, crowd_eval.cpp).
				if (core::ant::sample_clip(clip.clip, float(clip.tracks.end_frame), scratch)) {
					apply_clip_pose(clip.tracks, scratch, d);
				}
			}
		}
		return r;
	}

	size_t capsule_count() const { return placed_.size(); }

	// The capsules of p_query (false: the clip is absent or does not sample).
	bool evaluate(const Query &p_query, std::vector<RefCapsule> &r_out) const {
		static constexpr ServerPoseClip kIdle[3] = { ServerPoseClip::StandIdle, ServerPoseClip::CrouchIdle, ServerPoseClip::ProneIdle };
		static constexpr ServerPoseClip kUp[3] = { ServerPoseClip::StandAimUp, ServerPoseClip::CrouchAimUp, ServerPoseClip::ProneAimUp };
		static constexpr ServerPoseClip kDown[3] = { ServerPoseClip::StandAimDown, ServerPoseClip::CrouchAimDown, ServerPoseClip::ProneAimDown };
		static constexpr size_t kFrom[core::gameplay::kCapsuleTransitions] = { 0, 0, 1, 1, 2, 2 };
		const Skeleton &rig = clips_->rig;
		size_t stance = 0;
		ServerPoseClip role = ServerPoseClip::StandIdle;
		float frame = 0;
		if (p_query.transition) {
			if (p_query.row >= core::gameplay::kCapsuleTransitions) {
				return false;
			}
			stance = kFrom[p_query.row];
			role = ServerPoseClip(size_t(ServerPoseClip::StandToCrouch) + p_query.row);
		} else {
			if (p_query.row >= core::gameplay::kCapsuleStances) {
				return false;
			}
			stance = p_query.row;
			role = kIdle[stance];
		}
		if (!clips_->present[size_t(role)]) {
			return false;
		}
		const CrowdClipData &clip = clips_->get(role);
		if (p_query.transition) {
			frame = float(std::clamp(p_query.fraction, 0.0, 1.0) * double(clip.tracks.end_frame));
		}
		LocalPose pose;
		core::ant::ClipPose scratch;
		if (!sample_pose(rig, clip.clip, clip.tracks, frame, pose, scratch)) {
			return false;
		}
		if (clips_->root_joint >= 0 && size_t(clips_->root_joint) < pose.joints.size()) {
			pose.joints[size_t(clips_->root_joint)].t = rig.bone(size_t(clips_->root_joint)).rest.t;
		}
		const double p = p_query.pitch_deg;
		if (p != 0.0 && std::isfinite(p)) {
			const bool up = p > 0;
			const double full = up ? aim_[stance].up_full_deg : aim_[stance].down_full_deg;
			const ServerPoseClip aim_role = up ? kUp[stance] : kDown[stance];
			if (full > 0 && clips_->present[size_t(aim_role)]) {
				const float w = float(std::min(1.0, std::fabs(p) / full));
				apply_additive(pose, up ? up_[stance] : down_[stance], clips_->get(aim_role).mask, w, order_);
			}
		}
		std::vector<Xform> model;
		model_pose(rig, pose, model);
		place(model, r_out);
		return true;
	}

	// The capsules placed from a rig model pose (the reference's own pose, or CrowdEvaluator::model). Unmapped
	// capsules (joint -1) come out at the origin.
	void place(std::span<const Xform> p_model, std::vector<RefCapsule> &r_out) const {
		r_out.assign(placed_.size(), RefCapsule());
		for (size_t c = 0; c < placed_.size(); ++c) {
			const Placed &p = placed_[c];
			RefCapsule &o = r_out[c];
			o.list_index = p.list_index;
			o.joint = p.joint;
			if (p.joint < 0 || size_t(p.joint) >= p_model.size()) {
				continue;
			}
			const Xform &x = p_model[size_t(p.joint)];
			rotate(x.q, p.offset, o.a);
			for (size_t k = 0; k < 3; ++k) {
				o.a[k] += k == 0 ? x.t.x : k == 1 ? x.t.y : x.t.z;
			}
			double d[3] = { 0, 0, 0 };
			if (p.length > double(kSphereCapsuleLength)) {
				const std::array<double, 3> v{ p.dir[0] * p.length, p.dir[1] * p.length, p.dir[2] * p.length };
				rotate(x.q, v, d);
			}
			for (size_t k = 0; k < 3; ++k) {
				o.b[k] = o.a[k] + d[k];
				o.centre[k] = 0.5 * (o.a[k] + o.b[k]);
			}
		}
	}

private:
	struct Placed {
		size_t list_index = 0;
		int32_t joint = -1;
		std::array<double, 3> offset{};
		std::array<double, 3> dir{};
		double length = 0;
	};

	// v' = q v q* in double (the quaternion normalised first).
	static void rotate(const Quat &p_q, const std::array<double, 3> &p_v, double (&r_out)[3]) {
		double w = p_q.w, x = p_q.x, y = p_q.y, z = p_q.z;
		const double n = std::sqrt(w * w + x * x + y * y + z * z);
		if (n > 0) {
			w /= n;
			x /= n;
			y /= n;
			z /= n;
		} else {
			w = 1;
			x = y = z = 0;
		}
		// t = 2 (q.xyz x v); v' = v + w t + q.xyz x t
		const double tx = 2 * (y * p_v[2] - z * p_v[1]);
		const double ty = 2 * (z * p_v[0] - x * p_v[2]);
		const double tz = 2 * (x * p_v[1] - y * p_v[0]);
		r_out[0] = p_v[0] + w * tx + (y * tz - z * ty);
		r_out[1] = p_v[1] + w * ty + (z * tx - x * tz);
		r_out[2] = p_v[2] + w * tz + (x * ty - y * tx);
	}

	const ServerPoseClips *clips_ = nullptr;
	std::array<StanceAim, core::gameplay::kCapsuleStances> aim_{};
	AdditiveOrder order_ = AdditiveOrder::LocalPost;
	std::vector<Placed> placed_;
	std::array<LocalPose, core::gameplay::kCapsuleStances> up_{}, down_{};
};

} // namespace bf3::anim::server_pose_reference
