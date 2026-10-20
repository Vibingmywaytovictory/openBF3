#include "bf3/anim/layers.h"

#include "bf3/core/ant/ant_clip.h"

#include <algorithm>
#include <cmath>

namespace bf3::anim {

namespace {

float quat_dot(const Quat &p_a, const Quat &p_b) {
	return p_a.x * p_b.x + p_a.y * p_b.y + p_a.z * p_b.z + p_a.w * p_b.w;
}

Quat nlerp(const Quat &p_a, const Quat &p_b, float p_t) {
	const float s = quat_dot(p_a, p_b) < 0.0f ? -1.0f : 1.0f;
	return quat_normalized(Quat{ p_a.x + (s * p_b.x - p_a.x) * p_t, p_a.y + (s * p_b.y - p_a.y) * p_t, p_a.z + (s * p_b.z - p_a.z) * p_t,
			p_a.w + (s * p_b.w - p_a.w) * p_t });
}

Vec3 lerp(const Vec3 &p_a, const Vec3 &p_b, float p_t) {
	return Vec3{ p_a.x + (p_b.x - p_a.x) * p_t, p_a.y + (p_b.y - p_a.y) * p_t, p_a.z + (p_b.z - p_a.z) * p_t };
}

float clamp01(float p_w) {
	return p_w > 0.0f ? (p_w < 1.0f ? p_w : 1.0f) : 0.0f;
}

}

BoneMask mask_from_tracks(const Skeleton &p_skeleton, const ClipTracks &p_tracks) {
	BoneMask m;
	m.weight.assign(p_skeleton.size(), 0.0f);
	for (const int32_t b : p_tracks.rotation_bone) {
		if (b >= 0 && size_t(b) < m.weight.size()) {
			m.weight[size_t(b)] = 1.0f;
		}
	}
	for (const int32_t b : p_tracks.translation_bone) {
		if (b >= 0 && size_t(b) < m.weight.size()) {
			m.weight[size_t(b)] = 1.0f;
		}
	}
	return m;
}

BoneMask mask_union(const BoneMask &p_a, const BoneMask &p_b) {
	BoneMask m;
	m.weight.assign(std::max(p_a.size(), p_b.size()), 0.0f);
	for (size_t i = 0; i < m.weight.size(); ++i) {
		const float a = i < p_a.size() ? p_a.weight[i] : 0.0f;
		const float b = i < p_b.size() ? p_b.weight[i] : 0.0f;
		m.weight[i] = std::max(a, b);
	}
	return m;
}

LocalPose identity_pose(const Skeleton &p_skeleton) {
	LocalPose p;
	p.joints.assign(p_skeleton.size(), xform_identity());
	return p;
}

void blend_poses(const LocalPose &p_a, const LocalPose &p_b, float p_weight, LocalPose &r_out) {
	const size_t n = std::min(p_a.joints.size(), p_b.joints.size());
	r_out.joints.resize(n);
	const float w = clamp01(p_weight);
	for (size_t j = 0; j < n; ++j) {
		const Xform &a = p_a.joints[j];
		const Xform &b = p_b.joints[j];
		r_out.joints[j] = Xform{ nlerp(a.q, b.q, w), lerp(a.t, b.t, w) };
	}
}

void blend_poses(const LocalPose *const *p_poses, const float *p_weights, size_t p_count, LocalPose &r_out) {
	if (p_count == 0 || p_poses == nullptr || p_weights == nullptr) {
		return;
	}
	float total = 0.0f;
	size_t n = SIZE_MAX;
	size_t first = SIZE_MAX;
	for (size_t k = 0; k < p_count; ++k) {
		if (p_poses[k] != nullptr && p_weights[k] >= 1.0e-4f) {
			total += p_weights[k];
			n = std::min(n, p_poses[k]->joints.size());
			first = first == SIZE_MAX ? k : first;
		}
	}
	if (first == SIZE_MAX || !(total > 0.0f) || !std::isfinite(total)) {
		if (p_poses[0] != nullptr && p_poses[0] != &r_out) {
			r_out = *p_poses[0];
		}
		return;
	}

	const float inv = 1.0f / total;
	r_out.joints.resize(n);
	for (size_t j = 0; j < n; ++j) {
		const Quat ref = p_poses[first]->joints[j].q;
		Quat q{ 0.0f, 0.0f, 0.0f, 0.0f };
		Vec3 t{};
		for (size_t k = first; k < p_count; ++k) {
			if (p_poses[k] == nullptr || !(p_weights[k] >= 1.0e-4f)) {
				continue;
			}
			const float w = p_weights[k] * inv;
			const Xform &x = p_poses[k]->joints[j];
			const float s = quat_dot(ref, x.q) < 0.0f ? -w : w;
			q.x += x.q.x * s;
			q.y += x.q.y * s;
			q.z += x.q.z * s;
			q.w += x.q.w * s;
			t.x += x.t.x * w;
			t.y += x.t.y * w;
			t.z += x.t.z * w;
		}
		r_out.joints[j] = Xform{ quat_normalized(q), t };
	}
}

void override_masked(LocalPose &r_base, const LocalPose &p_layer, const BoneMask &p_mask, float p_weight) {
	const float w = clamp01(p_weight);
	if (w <= 0.0f) {
		return;
	}
	const size_t n = std::min({ r_base.joints.size(), p_layer.joints.size(), p_mask.size() });
	for (size_t j = 0; j < n; ++j) {
		const float m = clamp01(p_mask.weight[j]) * w;
		if (m <= 0.0f) {
			continue;
		}
		if (m >= 1.0f) {
			r_base.joints[j] = p_layer.joints[j];
			continue;
		}
		const Xform &a = r_base.joints[j];
		const Xform &b = p_layer.joints[j];
		r_base.joints[j] = Xform{ nlerp(a.q, b.q, m), lerp(a.t, b.t, m) };
	}
}

void apply_additive(LocalPose &r_base, const LocalPose &p_delta, const BoneMask &p_mask, float p_weight, AdditiveOrder p_order) {
	const float w = clamp01(p_weight);
	if (w <= 0.0f) {
		return;
	}
	const size_t n = std::min({ r_base.joints.size(), p_delta.joints.size(), p_mask.size() });
	const Quat identity{};
	for (size_t j = 0; j < n; ++j) {
		const float m = clamp01(p_mask.weight[j]) * w;
		if (m <= 0.0f) {
			continue;
		}
		const Xform &d = p_delta.joints[j];
		const Quat dq = m >= 1.0f ? quat_normalized(d.q) : core::ant::quat_slerp(identity, quat_normalized(d.q), m);
		Xform &b = r_base.joints[j];
		b.q = quat_normalized(p_order == AdditiveOrder::LocalPost ? quat_mul(b.q, dq) : quat_mul(dq, b.q));
		b.t = Vec3{ b.t.x + d.t.x * m, b.t.y + d.t.y * m, b.t.z + d.t.z * m };
	}
}

}
