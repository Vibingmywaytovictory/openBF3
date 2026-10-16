// The server pose of the hit capsules (plan R14; docs/design/m2a.md §6.5; as built: docs/design/m2a-lc.md). See
// server_pose.h.
//
// Per stance row: the stance idle at frame 0, the root joint's translation back to rest (the sim owns the root, as
// S3's evaluator does), the stance's AimUp / AimDown additive at the bucket's weight (LocalPost, S3's order), the
// model pose, and the hi-LOD capsules placed from their bones' model transforms. Per transition row: the transition
// clip at each fraction of its own length with the source stance's aim. A row whose clips are not usable comes from
// the caller's stand-in layout and is tagged.
#include "bf3/anim/server_pose.h"

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/pose.h"
#include "bf3/core/gameplay/work_budget.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace bf3::anim {

using core::ant::AntLibrary;
using core::ant::AntPackage;
using core::ant::AntResult;
using core::ant::GdStruct;
using core::gameplay::CapsuleLocal;
using core::gameplay::CapsulePoseTable;
using core::gameplay::kCapsulePitchBuckets;
using core::gameplay::kCapsuleStances;
using core::gameplay::kCapsuleTransitionFractions;
using core::gameplay::kCapsuleTransitions;

namespace {

constexpr uint8_t kFlagBehindWall = 1u << 0; // = sim::weapons::zone_capsule_flags::kDeactivateIfBehindWall

// The idle / aim up / aim down roles of a stance, and the transition rows' source and target stances.
constexpr ServerPoseClip kIdle[kCapsuleStances] = { ServerPoseClip::StandIdle, ServerPoseClip::CrouchIdle, ServerPoseClip::ProneIdle };
constexpr ServerPoseClip kAimUp[kCapsuleStances] = { ServerPoseClip::StandAimUp, ServerPoseClip::CrouchAimUp, ServerPoseClip::ProneAimUp };
constexpr ServerPoseClip kAimDown[kCapsuleStances] = { ServerPoseClip::StandAimDown, ServerPoseClip::CrouchAimDown, ServerPoseClip::ProneAimDown };
constexpr uint8_t kTransitionFrom[kCapsuleTransitions] = { 0, 0, 1, 1, 2, 2 };
constexpr uint8_t kTransitionTo[kCapsuleTransitions] = { 1, 2, 0, 2, 0, 1 };

GdStruct controller_of(const AntLibrary &p_library, const AntPackage &p_package, const GdStruct &p_clip) {
	const uint32_t index = p_clip.guid().index();
	GdStruct c = p_package.controller_for(index);
	if (!c.valid() && p_library.static_package() != nullptr && p_library.static_package() != &p_package) {
		c = p_library.static_package()->controller_for(index);
	}
	return c;
}

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
		return core::make_error(p_what + ": rig with " + std::to_string(d.binding.rig->joints.size()) + " joints");
	}
	d.additive = d.clip.additive;
	d.controller_index = d.binding.controller.valid() ? d.binding.controller.guid().index() : 0;
	return d;
}

class Fnv {
public:
	template <typename T>
	void add(const T &p_v) {
		static_assert(std::is_trivially_copyable_v<T>);
		unsigned char b[sizeof(T)];
		std::memcpy(b, &p_v, sizeof(T));
		for (const unsigned char c : b) {
			h_ ^= c;
			h_ *= 0x100000001B3ull;
		}
	}
	void str(std::string_view p_s) {
		add(uint64_t(p_s.size()));
		for (const char c : p_s) {
			add(c);
		}
	}
	template <typename T>
	void vec(const std::vector<T> &p_v) {
		add(uint64_t(p_v.size()));
		for (const T &x : p_v) {
			add(x);
		}
	}
	uint64_t value() const { return h_; }

private:
	uint64_t h_ = 0xCBF29CE484222325ull;
};

// One placed capsule: its joint and the bone-local start and direction.
struct Placement {
	size_t list_index = 0; // index into CapsuleTableInputs::capsules (BoneCollisionData order)
	int32_t joint = -1;
	Vec3 offset{}; // model space of the bind pose (see server_pose.h)
	Quat bind_inverse{}; // the conjugate of the joint's bind (rest) model rotation
	Vec3 dir{}; // unit, bone-local
	float length = 0;
	float radius = 0;
	uint8_t zone = 0, property = 0, flags = 0;
};

bool usable_aim(const Skeleton &p_rig, const ServerPoseClips &p_clips, ServerPoseClip p_clip) {
	const size_t k = size_t(p_clip);
	return p_clips.present[k] && p_clips.clips[k].additive && p_clips.clips[k].mask.size() == p_rig.size();
}

bool usable_pose(const Skeleton &p_rig, const ServerPoseClips &p_clips, ServerPoseClip p_clip) {
	const size_t k = size_t(p_clip);
	return p_clips.present[k] && !p_clips.clips[k].additive && p_clips.clips[k].mask.size() == p_rig.size();
}

// The additive delta of an aim clip: sampled onto identity at its last frame (crowd_eval.cpp's rule).
bool aim_delta(const Skeleton &p_rig, const CrowdClipData &p_clip, LocalPose &r_delta, core::ant::ClipPose &r_scratch) {
	r_delta = identity_pose(p_rig);
	if (!core::ant::sample_clip(p_clip.clip, float(p_clip.tracks.end_frame), r_scratch)) {
		return false;
	}
	apply_clip_pose(p_clip.tracks, r_scratch, r_delta);
	return true;
}

} // namespace

core::Result<ServerPoseClips> load_server_pose_clips(const core::ant::AntPackageSource &p_source, const AntLibrary &p_library,
		const ServerPoseClipNames &p_names) {
	ServerPoseClips set;
	core::Result<std::unique_ptr<AntPackage>> package = load_ant_package(p_source, p_names.package);
	if (!package) {
		return package.error();
	}
	set.package = std::move(*package);
	const AntPackage &pkg = *set.package;
	const std::string where = "server pose clips (" + pkg.name() + ")";

	core::gameplay::WorkBudget budget;
	std::array<uint32_t, kServerPoseClips> matches{};
	std::array<GdStruct, kServerPoseClips> found{};
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
		for (size_t k = 0; k < kServerPoseClips; ++k) {
			if (name == p_names.controllers[k]) {
				++matches[k];
				found[k] = clip;
			}
		}
	}
	for (size_t k = 0; k < kServerPoseClips; ++k) {
		if (matches[k] != 1) {
			set.notes.push_back(p_names.controllers[k] + ": occurs " + std::to_string(matches[k]) + " times (must be exactly once)");
			continue;
		}
		core::Result<CrowdClipData> d = make_clip(p_library, pkg, found[k], p_names.controllers[k]);
		if (!d) {
			set.notes.push_back(d.error().message);
			continue;
		}
		set.clips[k] = std::move(*d);
		set.present[k] = true;
	}
	// The rig: the stand idle's, or the first clip that has one.
	const core::ant::AntRig *rig = nullptr;
	if (set.present[size_t(ServerPoseClip::StandIdle)]) {
		rig = set.clips[size_t(ServerPoseClip::StandIdle)].binding.rig;
	}
	for (size_t k = 0; k < kServerPoseClips && rig == nullptr; ++k) {
		if (set.present[k]) {
			rig = set.clips[k].binding.rig;
		}
	}
	if (rig == nullptr) {
		return core::make_error(where + ": no named clip binds to a rig");
	}
	for (size_t k = 0; k < kServerPoseClips; ++k) {
		if (set.present[k] && set.clips[k].binding.rig != rig) {
			set.notes.push_back(p_names.controllers[k] + ": binds to rig " + std::string(set.clips[k].binding.rig->name) + ", not " + std::string(rig->name));
			set.present[k] = false;
		}
	}
	core::Result<Skeleton> skeleton = skeleton_from_rig(*rig);
	if (!skeleton) {
		return core::error_at(where + " rig", skeleton.error());
	}
	set.rig = std::move(*skeleton);
	for (size_t k = 0; k < kServerPoseClips; ++k) {
		if (!set.present[k]) {
			continue;
		}
		core::Result<ClipTracks> tracks = bind_clip_tracks(set.clips[k].binding, set.clips[k].clip, set.rig);
		if (!tracks) {
			set.notes.push_back(p_names.controllers[k] + ": " + tracks.error().message);
			set.present[k] = false;
			continue;
		}
		set.clips[k].tracks = std::move(*tracks);
		set.clips[k].mask = mask_from_tracks(set.rig, set.clips[k].tracks);
	}
	set.root_joint = set.rig.find(p_names.root_joint);
	if (set.root_joint < 0) {
		set.notes.push_back("the rig has no joint " + p_names.root_joint + " (no root reset)");
	}
	return set;
}

std::array<float, kCapsulePitchBuckets> capsule_pitch_buckets(const CrowdConfig &p_aim) {
	std::array<float, kCapsulePitchBuckets> p{};
	const double down = std::max(1.0, double(p_aim.aim_down_full_pitch_deg));
	const double up = std::max(1.0, double(p_aim.aim_up_full_pitch_deg));
	for (size_t i = 0; i <= kCapsuleDownBuckets; ++i) {
		p[i] = float(-down + down * double(i) / double(kCapsuleDownBuckets));
	}
	for (size_t i = 1; i <= kCapsuleUpBuckets; ++i) {
		p[kCapsuleDownBuckets + i] = float(up * double(i) / double(kCapsuleUpBuckets));
	}
	p[kCapsuleDownBuckets] = 0.0f;
	return p;
}

float capsule_transition_fraction(size_t p_step) {
	return float(double(std::min(p_step, kCapsuleTransitionFractions - 1)) / double(kCapsuleTransitionFractions - 1));
}

std::array<StanceAim, kCapsuleStances> server_pose_stance_aim(const ServerPoseClips &p_clips, const CrowdConfig &p_aim, std::vector<std::string> *r_notes) {
	std::array<StanceAim, kCapsuleStances> out{};
	for (StanceAim &a : out) {
		a.up_full_deg = p_aim.aim_up_full_pitch_deg;
		a.down_full_deg = p_aim.aim_down_full_pitch_deg;
	}
	static const char *kName[kCapsuleStances] = { "stand", "crouch", "prone" };
	for (size_t s = 1; s < kCapsuleStances; ++s) {
		if (!usable_pose(p_clips.rig, p_clips, kIdle[s]) || !usable_aim(p_clips.rig, p_clips, kAimUp[s]) || !usable_aim(p_clips.rig, p_clips, kAimDown[s])) {
			if (r_notes != nullptr) {
				r_notes->push_back(std::string(kName[s]) + " aim: clips not usable, the stand limits are used");
			}
			continue;
		}
		// S3's measure_aim on a clip set made of this stance's idle and aims.
		CrowdClipSet view;
		view.rig = p_clips.rig;
		view.root_joint = p_clips.root_joint;
		view.clips[size_t(CrowdClip::Idle)] = p_clips.get(kIdle[s]);
		view.clips[size_t(CrowdClip::AimUp)] = p_clips.get(kAimUp[s]);
		view.clips[size_t(CrowdClip::AimDown)] = p_clips.get(kAimDown[s]);
		core::Result<AimMeasure> m = measure_aim(view, p_aim.aim_order);
		if (!m || !(m->up_deg >= kMinAimFullDeg) || !(-m->down_deg >= kMinAimFullDeg)) {
			if (r_notes != nullptr) {
				r_notes->push_back(std::string(kName[s]) + " aim: " + (m ? "up " + std::to_string(m->up_deg) + " / down " + std::to_string(m->down_deg) + " deg" : m.error().message) +
						", the stand limits are used");
			}
			continue;
		}
		out[s].up_full_deg = m->up_deg;
		out[s].down_full_deg = -m->down_deg;
		out[s].measured = true;
	}
	return out;
}

float server_pose_aim_weight(const StanceAim &p_aim, float p_pitch_deg) {
	if (!std::isfinite(p_pitch_deg) || p_pitch_deg == 0.0f) {
		return 0.0f;
	}
	const float full = p_pitch_deg > 0.0f ? p_aim.up_full_deg : p_aim.down_full_deg;
	if (!(full > 0.0f)) {
		return 0.0f;
	}
	return std::min(1.0f, std::fabs(p_pitch_deg) / full);
}

float capsule_axis_sign(const Skeleton &p_rig, int32_t p_bone, int32_t p_axis) {
	if (p_bone < 0 || size_t(p_bone) >= p_rig.size() || p_axis < 0 || p_axis > 2) {
		return 1.0f;
	}
	float best = -1.0f;
	float sign = 1.0f;
	for (size_t i = size_t(p_bone) + 1; i < p_rig.size(); ++i) {
		if (p_rig.bone(i).parent != p_bone) {
			continue;
		}
		const Vec3 &t = p_rig.bone(i).rest.t;
		const float len = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
		if (len > best) {
			best = len;
			const float c = p_axis == 0 ? t.x : p_axis == 1 ? t.y : t.z;
			sign = c < 0.0f ? -1.0f : 1.0f;
		}
	}
	return sign;
}

core::Result<CapsulePoseTable> build_capsule_pose_table(const Skeleton &p_rig, const ServerPoseClips &p_clips, const CapsuleTableInputs &p_inputs) {
	return build_capsule_pose_table(p_rig, p_clips, p_inputs, nullptr);
}

core::Result<CapsulePoseTable> build_capsule_pose_table(const Skeleton &p_rig, const ServerPoseClips &p_clips, const CapsuleTableInputs &p_inputs,
		CapsuleTableReport *r_report) {
	const auto t0 = std::chrono::steady_clock::now();
	CapsuleTableReport report;
	const std::string where = "capsule pose table";
	if (p_inputs.capsules.empty()) {
		return core::make_error(where + ": no capsules");
	}
	if (p_inputs.property.size() != p_inputs.capsules.size() || p_inputs.zone.size() != p_inputs.capsules.size()) {
		return core::make_error(where + ": the property / zone lists do not match the capsule list");
	}
	if (p_rig.size() == 0 || p_rig.size() > kMaxCrowdRigJoints) {
		return core::make_error(where + ": a rig of " + std::to_string(p_rig.size()) + " joints");
	}
	// The bind pose: the rig's rest in model space (= the EBX skeleton's ModelPose of the same rig, verified by
	// bf3dump lagcomp's lagcomp.ebx rows).
	std::vector<Xform> bind;
	model_pose(p_rig, rest_pose(p_rig), bind);
	// The hi-LOD capsules, in list order.
	std::vector<Placement> placed;
	size_t mapped = 0;
	for (size_t i = 0; i < p_inputs.capsules.size(); ++i) {
		const core::gameplay::HitCapsule &c = p_inputs.capsules[i];
		if (!c.valid_hi_lod) {
			continue;
		}
		if (placed.size() >= core::gameplay::kMaxTableCapsules) {
			return core::make_error(where + ": more than " + std::to_string(core::gameplay::kMaxTableCapsules) + " hi-LOD capsules");
		}
		if (!std::isfinite(c.radius) || !(c.radius > 0.0f) || c.radius > 2.0f || !std::isfinite(c.length) || c.length < 0.0f || c.length > 4.0f ||
				!std::isfinite(c.offset[0]) || !std::isfinite(c.offset[1]) || !std::isfinite(c.offset[2]) || std::fabs(c.offset[0]) > 4.0f ||
				std::fabs(c.offset[1]) > 4.0f || std::fabs(c.offset[2]) > 4.0f) {
			return core::make_error(where + ": capsule " + c.bone + " has an unusable radius, length or offset");
		}
		if (p_inputs.zone[i] > 3) {
			return core::make_error(where + ": capsule " + c.bone + " has zone " + std::to_string(p_inputs.zone[i]));
		}
		Placement p;
		p.list_index = i;
		p.joint = p_rig.find(c.bone);
		int32_t axis = c.bone_axis;
		if (axis < 0 || axis > 2) {
			report.notes.push_back("capsule " + c.bone + ": BoneAxis " + std::to_string(axis) + " taken as 0");
			axis = 0;
		}
		const float sign = capsule_axis_sign(p_rig, p.joint, axis);
		p.dir = Vec3{ axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f };
		p.offset = Vec3{ c.offset[0], c.offset[1], c.offset[2] };
		p.bind_inverse = p.joint >= 0 ? quat_conj(bind[size_t(p.joint)].q) : Quat{};
		p.length = c.length;
		p.radius = c.radius;
		p.zone = p_inputs.zone[i];
		p.property = p_inputs.property[i];
		p.flags = c.deactivate_if_behind_wall ? kFlagBehindWall : uint8_t(0);
		if (p.joint >= 0) {
			++mapped;
		} else {
			report.notes.push_back("capsule " + c.bone + ": no rig joint of that name (placed from the stand-in)");
		}
		report.joint.push_back(p.joint);
		placed.push_back(p);
	}
	if (placed.empty()) {
		return core::make_error(where + ": no hi-LOD capsule");
	}
	if (mapped == 0) {
		return core::make_error(where + ": no capsule bone maps to a rig joint");
	}
	const size_t n = placed.size();
	for (size_t s = 0; s < kCapsuleStances; ++s) {
		if (p_inputs.standin[s].size() != n) {
			return core::make_error(where + ": the stand-in layout of stance " + std::to_string(s) + " has " + std::to_string(p_inputs.standin[s].size()) +
					" capsules, not " + std::to_string(n));
		}
	}

	CapsulePoseTable t;
	t.version = core::gameplay::kCapsuleTableVersion;
	t.capsule_count = uint8_t(n);
	t.pitch_deg = capsule_pitch_buckets(p_inputs.aim);
	t.steady.resize(kCapsuleStances * kCapsulePitchBuckets * n);
	t.transition.resize(kCapsuleTransitions * kCapsuleTransitionFractions * kCapsulePitchBuckets * n);
	report.aim = server_pose_stance_aim(p_clips, p_inputs.aim, &report.notes);

	core::ant::ClipPose scratch;
	LocalPose pose;
	std::vector<Xform> model;
	// The capsules of one evaluated model pose into p_out (n entries); unmapped capsules take the stand-in of p_stance.
	auto place = [&](const std::vector<Xform> &p_model, size_t p_stance, CapsuleLocal *r_out) {
		for (size_t c = 0; c < n; ++c) {
			const Placement &p = placed[c];
			CapsuleLocal &e = r_out[c];
			e = p_inputs.standin[p_stance][c];
			if (p.joint >= 0) {
				const Xform &x = p_model[size_t(p.joint)];
				const Vec3 a = vec_add(x.t, quat_rotate(quat_mul(x.q, p.bind_inverse), p.offset));
				const Vec3 b = p.length > kSphereCapsuleLength ? vec_add(a, quat_rotate(x.q, vec_scale(p.dir, p.length))) : a;
				e.a[0] = a.x;
				e.a[1] = a.y;
				e.a[2] = a.z;
				e.b[0] = b.x;
				e.b[1] = b.y;
				e.b[2] = b.z;
			}
			e.radius = p.radius;
			e.zone = p.zone;
			e.capsule = uint8_t(std::min<size_t>(p.list_index, 255));
			e.property = p.property;
			e.flags = p.flags;
		}
	};
	// Pose = p_clip at p_frame, root reset, the stance's aim at p_pitch_deg; false when the clip does not sample.
	auto evaluate = [&](const CrowdClipData &p_clip, float p_frame, const LocalPose *p_up, const LocalPose *p_down, size_t p_aim_stance, float p_pitch_deg,
							std::vector<Xform> &r_model) {
		if (!sample_pose(p_rig, p_clip.clip, p_clip.tracks, p_frame, pose, scratch)) {
			return false;
		}
		if (p_clips.root_joint >= 0 && size_t(p_clips.root_joint) < pose.joints.size()) {
			pose.joints[size_t(p_clips.root_joint)].t = p_rig.bone(size_t(p_clips.root_joint)).rest.t;
		}
		const float w = server_pose_aim_weight(report.aim[p_aim_stance], p_pitch_deg);
		if (w > 0.0f) {
			const bool up = p_pitch_deg > 0.0f;
			const ServerPoseClip role = up ? kAimUp[p_aim_stance] : kAimDown[p_aim_stance];
			apply_additive(pose, up ? *p_up : *p_down, p_clips.get(role).mask, w, p_inputs.aim.aim_order);
		}
		model_pose(p_rig, pose, r_model);
		++report.pose_evaluations;
		return true;
	};

	// Stance rows.
	std::array<LocalPose, kCapsuleStances> up_delta, down_delta;
	std::array<bool, kCapsuleStances> stance_clip{};
	static const char *kStance[kCapsuleStances] = { "stand", "crouch", "prone" };
	for (size_t s = 0; s < kCapsuleStances; ++s) {
		const bool ok = usable_pose(p_rig, p_clips, kIdle[s]) && usable_aim(p_rig, p_clips, kAimUp[s]) && usable_aim(p_rig, p_clips, kAimDown[s]) &&
				aim_delta(p_rig, p_clips.get(kAimUp[s]), up_delta[s], scratch) && aim_delta(p_rig, p_clips.get(kAimDown[s]), down_delta[s], scratch);
		stance_clip[s] = ok;
		for (size_t pb = 0; pb < kCapsulePitchBuckets && ok; ++pb) {
			if (!evaluate(p_clips.get(kIdle[s]), 0.0f, &up_delta[s], &down_delta[s], s, t.pitch_deg[pb], model)) {
				stance_clip[s] = false;
				break;
			}
			place(model, s, &t.steady[t.steady_index(s, pb)]);
		}
		if (!stance_clip[s]) {
			report.notes.push_back(std::string(kStance[s]) + " row: stand-in (idle or aim clips missing, unbound or not additive where required)");
			for (size_t pb = 0; pb < kCapsulePitchBuckets; ++pb) {
				std::copy(p_inputs.standin[s].begin(), p_inputs.standin[s].end(), t.steady.begin() + ptrdiff_t(t.steady_index(s, pb)));
			}
		}
		t.row_source[s] = stance_clip[s] ? core::gameplay::capsule_row_source::kClipDerived : core::gameplay::capsule_row_source::kStandIn;
	}
	// Transition rows: the clip at fraction k / 7 of its length, with the source stance's aim.
	for (size_t r = 0; r < kCapsuleTransitions; ++r) {
		const ServerPoseClip role = ServerPoseClip(size_t(ServerPoseClip::StandToCrouch) + r);
		const size_t from = kTransitionFrom[r];
		const size_t to = kTransitionTo[r];
		bool ok = usable_pose(p_rig, p_clips, role) && stance_clip[from];
		const CrowdClipData &clip = p_clips.get(role);
		for (size_t f = 0; f < kCapsuleTransitionFractions && ok; ++f) {
			const float frame = capsule_transition_fraction(f) * float(clip.tracks.end_frame);
			for (size_t pb = 0; pb < kCapsulePitchBuckets; ++pb) {
				if (!evaluate(clip, frame, &up_delta[from], &down_delta[from], from, t.pitch_deg[pb], model)) {
					ok = false;
					break;
				}
				place(model, from, &t.transition[t.transition_index(r, f, pb)]);
			}
		}
		if (!ok) {
			report.notes.push_back(std::string("transition ") + std::to_string(r) + " (" + kStance[from] + " -> " + kStance[to] +
					"): stand-in (a lerp of the two stance rows)");
			for (size_t f = 0; f < kCapsuleTransitionFractions; ++f) {
				const float w = capsule_transition_fraction(f);
				for (size_t pb = 0; pb < kCapsulePitchBuckets; ++pb) {
					const CapsuleLocal *a = &t.steady[t.steady_index(from, pb)];
					const CapsuleLocal *b = &t.steady[t.steady_index(to, pb)];
					CapsuleLocal *o = &t.transition[t.transition_index(r, f, pb)];
					for (size_t c = 0; c < n; ++c) {
						o[c] = a[c];
						for (size_t k = 0; k < 3; ++k) {
							o[c].a[k] = a[c].a[k] + (b[c].a[k] - a[c].a[k]) * w;
							o[c].b[k] = a[c].b[k] + (b[c].b[k] - a[c].b[k]) * w;
						}
					}
				}
			}
		}
		t.row_source[kCapsuleStances + r] = ok ? core::gameplay::capsule_row_source::kClipDerived : core::gameplay::capsule_row_source::kStandIn;
	}
	// The bound sphere about the feet: every capsule of every entry.
	double bound = 0;
	for (const std::vector<CapsuleLocal> *v : { &t.steady, &t.transition }) {
		for (const CapsuleLocal &e : *v) {
			for (const float *p : { e.a, e.b }) {
				bound = std::max(bound, std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]) + double(e.radius));
			}
		}
	}
	t.bound_radius = float(bound) + 1e-3f;
	t.hash = core::gameplay::capsule_pose_table_hash(t);
	if (!core::gameplay::capsule_pose_table_valid(t)) {
		return core::make_error(where + ": the built table fails its structural checks (a pose far from the feet or a non-finite value)");
	}
	report.row_source = t.row_source;
	report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (r_report != nullptr) {
		*r_report = std::move(report);
	}
	return t;
}

uint64_t capsule_table_source_hash(const Skeleton &p_rig, const ServerPoseClips &p_clips, const CapsuleTableInputs &p_inputs) {
	Fnv h;
	h.add(core::gameplay::kCapsuleTableVersion);
	h.add(uint64_t(p_rig.size()));
	for (const Bone &b : p_rig.bones()) {
		h.str(b.name);
		h.add(b.parent);
		h.add(b.rest);
	}
	h.add(p_clips.root_joint);
	for (size_t k = 0; k < kServerPoseClips; ++k) {
		h.add(p_clips.present[k]);
		if (!p_clips.present[k]) {
			continue;
		}
		const CrowdClipData &c = p_clips.clips[k];
		h.add(c.clip.codec);
		h.add(c.clip.num_keys);
		h.add(c.clip.num_quats);
		h.add(c.clip.num_vec3s);
		h.add(c.clip.num_floats);
		h.add(c.clip.end_frame);
		h.add(c.clip.additive);
		h.vec(c.clip.key_times);
		h.vec(c.clip.quats);
		h.vec(c.clip.vec3s);
		h.vec(c.clip.floats);
		h.vec(c.tracks.rotation_bone);
		h.vec(c.tracks.translation_bone);
		h.add(c.tracks.fps);
		h.add(c.tracks.end_frame);
	}
	h.add(uint64_t(p_inputs.capsules.size()));
	for (const core::gameplay::HitCapsule &c : p_inputs.capsules) {
		h.str(c.bone);
		h.add(c.radius);
		h.add(c.length);
		h.add(c.offset);
		h.add(c.bone_axis);
		h.add(c.valid_hi_lod);
		h.add(c.deactivate_if_behind_wall);
	}
	for (const uint8_t v : p_inputs.property) {
		h.add(v);
	}
	for (const uint8_t v : p_inputs.zone) {
		h.add(v);
	}
	for (const std::vector<CapsuleLocal> &s : p_inputs.standin) {
		h.vec(s);
	}
	h.add(p_inputs.aim.aim_order);
	h.add(p_inputs.aim.aim_up_full_pitch_deg);
	h.add(p_inputs.aim.aim_down_full_pitch_deg);
	return h.value();
}

} // namespace bf3::anim
