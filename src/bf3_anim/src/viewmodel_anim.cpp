#include "bf3/anim/viewmodel_anim.h"

#include "bf3/anim/layers.h"
#include "bf3/core/math.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>

namespace bf3::anim {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kMaxFrameSeconds = 0.25f;

float clamp01(float p_w) {
	return p_w > 0.0f ? (p_w < 1.0f ? p_w : 1.0f) : 0.0f;
}

float finite_or(float p_v, float p_fallback) {
	return std::isfinite(p_v) ? p_v : p_fallback;
}

Quat quat_axis_angle(float p_x, float p_y, float p_z, float p_radians) {
	const float s = std::sin(p_radians * 0.5f);
	return Quat{ p_x * s, p_y * s, p_z * s, std::cos(p_radians * 0.5f) };
}

Quat camera_rotation(float p_pitch_rad, float p_yaw_rad, float p_roll_rad) {
	return quat_normalized(
			quat_mul(quat_axis_angle(0, 1, 0, p_yaw_rad), quat_mul(quat_axis_angle(1, 0, 0, p_pitch_rad), quat_axis_angle(0, 0, 1, p_roll_rad))));
}

float hump(float p_u) {
	if (!(p_u > 0.0f) || p_u >= 1.0f) {
		return 0.0f;
	}
	return std::sin(float(kPi) * p_u);
}

}

const char *vm_clip_name(VmClip p_clip) {
	switch (p_clip) {
		case VmClip::Deploy:
			return "Deploy";
		case VmClip::Idle:
			return "Idle";
		case VmClip::Fire:
			return "Fire";
		case VmClip::FireSingle:
			return "FireSingle";
		case VmClip::Reload:
			return "Reload";
		case VmClip::ReloadEmpty:
			return "ReloadEmpty";
		case VmClip::Bolt:
			return "Bolt";
		case VmClip::ZoomIn:
			return "ZoomIn";
		case VmClip::ZoomOut:
			return "ZoomOut";
		case VmClip::ZoomIdle:
			return "ZoomIdle";
		case VmClip::Throw:
			return "Throw";
		case VmClip::Knife:
			return "Knife";
		case VmClip::Sprint:
			return "Sprint";
		case VmClip::ThrowStart:
			return "ThrowStart";
		case VmClip::ThrowHold:
			return "ThrowHold";
		case VmClip::Count:
			break;
	}
	return "?";
}

const char *vm_cue_name(VmCue::Kind p_kind) {
	switch (p_kind) {
		case VmCue::Kind::Shot:
			return "Shot";
		case VmCue::Kind::DeployStarted:
			return "DeployStarted";
		case VmCue::Kind::DeployFinished:
			return "DeployFinished";
		case VmCue::Kind::ReloadStarted:
			return "ReloadStarted";
		case VmCue::Kind::ReloadCommitted:
			return "ReloadCommitted";
		case VmCue::Kind::ReloadFinished:
			return "ReloadFinished";
		case VmCue::Kind::ReloadCancelled:
			return "ReloadCancelled";
		case VmCue::Kind::BoltStarted:
			return "BoltStarted";
		case VmCue::Kind::Thrown:
			return "Thrown";
		case VmCue::Kind::HoldStarted:
			return "HoldStarted";
	}
	return "?";
}

void step_spring(const SpringParams &p_params, float p_target, float p_dt, SpringState &r_state) {

	const double dt = std::clamp(double(finite_or(p_dt, 0.0f)), 0.0, double(kMaxFrameSeconds));
	const double target = double(finite_or(p_target, 0.0f));
	if (!std::isfinite(r_state.x) || !std::isfinite(r_state.v)) {
		r_state = SpringState{ float(target), 0.0f };
	}
	if (dt <= 0.0) {
		return;
	}
	const double k = std::max(0.0, double(finite_or(p_params.stiffness, 0.0f)));
	const double c = std::max(0.0, double(finite_or(p_params.damping, 0.0f)));
	const double y0 = double(r_state.x) - target;
	const double v0 = double(r_state.v);
	double y = y0, v = v0;
	if (k <= 1e-9) {
		if (c > 1e-9) {
			const double e = std::exp(-c * dt);
			y = y0 + v0 * (1.0 - e) / c;
			v = v0 * e;
		} else {
			y = y0 + v0 * dt;
		}
	} else {
		const double w = std::sqrt(k);
		const double zeta = c / (2.0 * w);
		if (std::fabs(zeta - 1.0) < 1e-3) {
			const double e = std::exp(-w * dt);
			const double b = v0 + w * y0;
			y = (y0 + b * dt) * e;
			v = (v0 - w * b * dt) * e;
		} else if (zeta < 1.0) {
			const double a = zeta * w;
			const double wd = w * std::sqrt(1.0 - zeta * zeta);
			const double e = std::exp(-a * dt);
			const double cs = std::cos(wd * dt), sn = std::sin(wd * dt);
			y = e * (y0 * cs + (v0 + a * y0) / wd * sn);
			v = e * (v0 * cs - (a * v0 + k * y0) / wd * sn);
		} else {
			const double r = w * std::sqrt(zeta * zeta - 1.0);
			const double r1 = -zeta * w + r;
			const double r2 = -zeta * w - r;
			const double c2 = (v0 - r1 * y0) / (r2 - r1);
			const double c1 = y0 - c2;
			const double e1 = std::exp(r1 * dt), e2 = std::exp(r2 * dt);
			y = c1 * e1 + c2 * e2;
			v = r1 * c1 * e1 + r2 * c2 * e2;
		}
	}
	r_state.x = float(target + y);
	r_state.v = float(v);
	if (!std::isfinite(r_state.x) || !std::isfinite(r_state.v)) {
		r_state = SpringState{ float(target), 0.0f };
	}
}

bool spring_at_rest(const SpringParams &p_params, float p_target, const SpringState &p_state) {
	const float e = p_params.settle_epsilon;
	return std::fabs(p_state.x - p_target) <= e && std::fabs(p_state.v) <= e;
}

float spring_impulse_for_peak(const SpringParams &p_params, float p_peak) {
	const float omega = std::sqrt(std::max(0.0f, p_params.stiffness));
	return p_peak * omega * float(std::exp(1.0));
}

namespace {

enum class KeyType : uint8_t { Float, Bool };

struct KeyDef {
	const char *name;
	KeyType type;
	float min;
	float max;
	void *(*field)(ViewmodelTuning &);
};

#define BF3_VM_FIELD(f) [](ViewmodelTuning &t) -> void * { return &t.f; }
#define BF3_VM_SPRING(s) \
	{ #s ".stiffness", KeyType::Float, 0.1f, 10000.0f, BF3_VM_FIELD(s.stiffness) }, { #s ".damping", KeyType::Float, 0.0f, 1000.0f, BF3_VM_FIELD(s.damping) }, \
			{ #s ".settle_epsilon", KeyType::Float, 1e-6f, 1.0f, BF3_VM_FIELD(s.settle_epsilon) }, \
			{ #s ".settle_seconds", KeyType::Float, 0.01f, 10.0f, BF3_VM_FIELD(s.settle_seconds) }

const KeyDef kKeys[] = {
	BF3_VM_SPRING(sway),
	{ "sway_scale", KeyType::Float, 0.0f, 1.0f, BF3_VM_FIELD(sway_scale) },
	{ "sway_max", KeyType::Float, 0.0f, 1.0f, BF3_VM_FIELD(sway_max) },
	BF3_VM_SPRING(bob),
	{ "bob_amplitude", KeyType::Float, 0.0f, 1.0f, BF3_VM_FIELD(bob_amplitude) },
	{ "bob_frequency", KeyType::Float, 0.0f, 20.0f, BF3_VM_FIELD(bob_frequency) },
	{ "bob_ads_scale", KeyType::Float, 0.0f, 1.0f, BF3_VM_FIELD(bob_ads_scale) },
	BF3_VM_SPRING(kick),
	{ "kick_back", KeyType::Float, 0.0f, 1.0f, BF3_VM_FIELD(kick_back) },
	{ "kick_pitch_deg", KeyType::Float, 0.0f, 45.0f, BF3_VM_FIELD(kick_pitch_deg) },
	{ "kick_ads_scale", KeyType::Float, 0.0f, 1.0f, BF3_VM_FIELD(kick_ads_scale) },
	BF3_VM_SPRING(camera_recoil),
	{ "camera_kick_pitch_deg", KeyType::Float, 0.0f, 20.0f, BF3_VM_FIELD(camera_kick_pitch_deg) },
	{ "ads_in_seconds", KeyType::Float, 0.01f, 5.0f, BF3_VM_FIELD(ads_in_seconds) },
	{ "ads_out_seconds", KeyType::Float, 0.01f, 5.0f, BF3_VM_FIELD(ads_out_seconds) },
	{ "fov_transition_specific", KeyType::Bool, 0, 0, BF3_VM_FIELD(fov_transition_specific) },
	{ "sprint_blend_seconds", KeyType::Float, 0.01f, 5.0f, BF3_VM_FIELD(sprint_blend_seconds) },
	{ "deploy_drop", KeyType::Float, 0.0f, 2.0f, BF3_VM_FIELD(deploy_drop) },
	{ "reload_drop", KeyType::Float, 0.0f, 2.0f, BF3_VM_FIELD(reload_drop) },
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef BF3_VM_SPRING
#undef BF3_VM_FIELD

using bf3::core::trim;

std::string shown(std::string_view p_text) {

	std::string out;
	for (size_t i = 0; i < p_text.size() && i < 40; ++i) {
		const char c = p_text[i];
		out += (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	if (p_text.size() > 40) {
		out += "...";
	}
	return out;
}

std::string number_text(float p_value) {
	char buffer[64];
	const std::to_chars_result r = std::to_chars(buffer, buffer + sizeof(buffer), p_value);
	return std::string(buffer, r.ptr);
}

}

core::Result<ViewmodelTuning> parse_viewmodel_tuning(std::string_view p_text) {
	ViewmodelTuning t;
	bool seen[kKeyCount] = {};
	size_t line_no = 0;
	size_t at = 0;
	while (at <= p_text.size()) {
		const size_t end = std::min(p_text.find('\n', at), p_text.size());
		std::string_view line = p_text.substr(at, end - at);
		at = end + 1;
		++line_no;
		if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
			line = line.substr(0, hash);
		}
		line = trim(line);
		if (line.empty()) {
			if (end == p_text.size()) {
				break;
			}
			continue;
		}
		const std::string where = "viewmodel tuning line " + std::to_string(line_no);
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos) {
			return core::make_error(where + ": expected `key = value`, got `" + shown(line) + "`");
		}
		const std::string_view key = trim(line.substr(0, eq));
		const std::string_view value = trim(line.substr(eq + 1));
		size_t index = kKeyCount;
		for (size_t k = 0; k < kKeyCount; ++k) {
			if (key == kKeys[k].name) {
				index = k;
				break;
			}
		}
		if (index == kKeyCount) {
			return core::make_error(where + ": unknown key `" + shown(key) + "`");
		}
		if (seen[index]) {
			return core::make_error(where + ": `" + std::string(key) + "` given twice");
		}
		seen[index] = true;
		const KeyDef &k = kKeys[index];
		void *field = k.field(t);
		switch (k.type) {
			case KeyType::Float: {
				float v = 0;
				const std::from_chars_result r = std::from_chars(value.data(), value.data() + value.size(), v);
				if (value.empty() || r.ec != std::errc() || r.ptr != value.data() + value.size() || !std::isfinite(v)) {
					return core::make_error(where + ": `" + std::string(key) + "` needs a number, got `" + shown(value) + "`");
				}
				if (!(v >= k.min && v <= k.max)) {
					return core::make_error(where + ": `" + std::string(key) + "` = " + shown(value) + " is outside " + number_text(k.min) + ".." +
							number_text(k.max));
				}
				*static_cast<float *>(field) = v;
				break;
			}
			case KeyType::Bool:
				if (value == "true") {
					*static_cast<bool *>(field) = true;
				} else if (value == "false") {
					*static_cast<bool *>(field) = false;
				} else {
					return core::make_error(where + ": `" + std::string(key) + "` needs true or false, got `" + shown(value) + "`");
				}
				break;
		}
		if (end == p_text.size()) {
			break;
		}
	}
	return t;
}

std::string format_viewmodel_tuning(const ViewmodelTuning &p_tuning) {
	ViewmodelTuning t = p_tuning;
	std::string out;
	for (const KeyDef &k : kKeys) {
		out += k.name;
		out += " = ";
		void *field = k.field(t);
		switch (k.type) {
			case KeyType::Float:
				out += number_text(*static_cast<float *>(field));
				break;
			case KeyType::Bool:
				out += *static_cast<bool *>(field) ? "true" : "false";
				break;
		}
		out += '\n';
	}
	return out;
}

float smoothstep01(float p_x) {
	const float x = clamp01(p_x);
	return x * x * (3.0f - 2.0f * x);
}

bool ads_allowed(uint8_t p_phase, bool p_sprinting) {
	return p_phase != kVmPhaseDeploying && !p_sprinting;
}

void AdsCamera::reset() {
	fov_progress_ = 0;
	blend_progress_ = 0;
	last_ = AdsFrame{};
}

namespace {

float advance_progress(float p_progress, bool p_target, float p_in_seconds, float p_out_seconds, float p_dt) {
	const float seconds = p_target ? p_in_seconds : p_out_seconds;
	const float rate = seconds > 1e-4f ? 1.0f / seconds : 1e4f;
	float p = p_progress + (p_target ? 1.0f : -1.0f) * rate * p_dt;
	if (p > 1.0f - 1e-5f) {
		p = 1.0f;
	}
	if (p < 1e-5f) {
		p = 0.0f;
	}
	return p;
}

}

AdsParams with_hip_fov(const AdsParams &p_params, float p_hip_fov_deg) {
	AdsParams p = p_params;
	if (std::isfinite(p_hip_fov_deg) && p_hip_fov_deg > 0.0f) {
		p.hip_fov = p_hip_fov_deg;
		if (!p.can_zoom) {
			p.zoom_fov = p_hip_fov_deg;
		}
	}
	return p;
}

AdsFrame AdsCamera::step(const AdsParams &p_params, bool p_zoomed, float p_dt) {
	const float dt = std::clamp(finite_or(p_dt, 0.0f), 0.0f, kMaxFrameSeconds);
	const bool zoomed = p_zoomed && p_params.can_zoom;
	fov_progress_ = advance_progress(fov_progress_, zoomed, p_params.fov_in_seconds, p_params.fov_out_seconds, dt);
	blend_progress_ = advance_progress(blend_progress_, zoomed, p_params.zoom_in_seconds, p_params.zoom_out_seconds, dt);
	AdsFrame f;
	f.zoomed = zoomed;
	f.fov_progress = fov_progress_;
	f.blend = smoothstep01(blend_progress_);
	const float fov_ease = smoothstep01(fov_progress_);
	f.world_fov = p_params.hip_fov + (p_params.zoom_fov - p_params.hip_fov) * fov_ease;
	f.viewmodel_fov = p_params.zoom_render_fov > 0.0f ? p_params.render_fov + (p_params.zoom_render_fov - p_params.render_fov) * f.blend : p_params.render_fov;
	f.look_scale = p_params.look_speed_hip + (p_params.look_speed_zoom - p_params.look_speed_hip) * fov_ease;
	last_ = f;
	return f;
}

ViewAngles camera_view_angles(double p_view_yaw, double p_view_pitch, float p_recoil_pitch_deg, float p_recoil_yaw_deg, float p_kick_pitch_deg,
		float p_kick_yaw_deg) {
	constexpr double kPitchLimit = 89.99 * core::kDegToRad;
	ViewAngles a;
	a.yaw = p_view_yaw + (double(p_recoil_yaw_deg) + double(p_kick_yaw_deg)) * core::kDegToRad;
	a.pitch = std::clamp(p_view_pitch + (double(p_recoil_pitch_deg) + double(p_kick_pitch_deg)) * core::kDegToRad, -kPitchLimit, kPitchLimit);
	return a;
}

namespace {

struct Action {
	bool active = false;
	VmClip role = VmClip::Idle;
	double start = 0;
	float seconds = 0;
	bool procedural = false;
	bool cut_in = false;
	double fade_out_start = -1;
	float fade_from = 1;
	float offset = 0;
	float loop = 0;
	bool flow_release = false;
};

}

struct VmDeltaLoco;
struct VmDeltaLocoDeleter {
	void operator()(VmDeltaLoco *p_loco) const;
};

bool vm_loco_apply(VmDeltaLoco &r_loco, const ViewmodelFrameInput &p_input, float p_sprint_w, float p_ads, float p_dt, float p_blend_seconds, LocalPose &r_pose,
		ViewmodelOutput &r_out);
void vm_loco_reset(VmDeltaLoco &r_loco);

struct ViewmodelAnimator::Impl {
	const Skeleton &rig;
	std::array<VmClipRef, kVmClips> clips;
	ViewmodelTuning tuning;

	LocalPose rest, idle, zoom, sprint, base, layer;
	core::ant::ClipPose scratch;

	Action cur, prev;
	double time = 0;
	bool have_time = false;

	float last_ads = 0;
	int ads_dir = 0;
	bool ads_clip_ok = false;

	float sprint_w = 0;
	double bob_phase = 0;
	uint32_t shots = 0;

	SpringState sway_x, sway_y, bob_amp, kick_back, kick_pitch, cam_pitch, cam_yaw;
	float sway_tx = 0, sway_ty = 0, bob_target = 0;
	std::unique_ptr<VmDeltaLoco, VmDeltaLocoDeleter> delta_loco;

	VmThrowFlow flow;
	bool has_flow = false;
	bool next_pending = false;
	double release_due = -1;
	double last_release_cue = -1;
	bool switch_back = false;
	double release_end = -1;

	Impl(const Skeleton &p_rig, std::span<const VmClipRef, kVmClips> p_clips, const ViewmodelTuning &p_tuning) :
			rig(p_rig), tuning(p_tuning) {
		for (size_t i = 0; i < kVmClips; ++i) {
			clips[i] = p_clips[i];
		}
		rest = rest_pose(rig);
		idle = rest;
		if (has_clip(VmClip::Idle)) {
			apply_clip_at(VmClip::Idle, 0.0f, idle);
		}
		zoom = idle;
		if (has_clip(VmClip::ZoomIdle)) {
			apply_clip_at(VmClip::ZoomIdle, 0.0f, zoom);
		}
		sprint = idle;
		if (has_clip(VmClip::Sprint)) {
			apply_clip_at(VmClip::Sprint, 0.0f, sprint);
		}
	}

	const VmClipRef &ref(VmClip p_role) const { return clips[size_t(p_role)]; }
	bool has_clip(VmClip p_role) const {
		const VmClipRef &r = ref(p_role);
		return r.clip != nullptr && r.tracks != nullptr && r.clip->consistent();
	}
	float role_seconds(VmClip p_role) const {
		const VmClipRef &r = ref(p_role);
		if (r.seconds > 0.0f) {
			return r.seconds;
		}
		return r.tracks ? r.tracks->duration_seconds() : 0.0f;
	}

	void apply_clip_at(VmClip p_role, float p_seconds, LocalPose &r_pose) {
		const VmClipRef &r = ref(p_role);
		if (r.clip == nullptr || r.tracks == nullptr) {
			return;
		}
		const float fps = r.tracks->fps > 0.0f ? r.tracks->fps : 30.0f;
		float frame = std::max(0.0f, finite_or(p_seconds, 0.0f)) * fps;
		frame = std::min(frame, float(r.tracks->end_frame));
		if (!core::ant::sample_clip(*r.clip, frame, scratch)) {
			return;
		}
		apply_clip_pose(*r.tracks, scratch, r_pose);
	}
	void apply_clip_frame(VmClip p_role, float p_frame, LocalPose &r_pose) {
		const VmClipRef &r = ref(p_role);
		if (r.clip == nullptr || r.tracks == nullptr) {
			return;
		}
		const float frame = std::clamp(finite_or(p_frame, 0.0f), 0.0f, float(r.tracks->end_frame));
		if (!core::ant::sample_clip(*r.clip, frame, scratch)) {
			return;
		}
		apply_clip_pose(*r.tracks, scratch, r_pose);
	}

	void start_action(VmClip p_role, double p_time, bool p_cut_in) {
		if (p_cut_in) {
			prev = Action{};
		} else if (cur.active) {
			prev = cur;
			if (prev.fade_out_start < 0) {
				prev.fade_out_start = p_time;
				prev.fade_from = weight_of(cur, p_time);
			}
		}
		cur = Action{};
		cur.active = true;
		cur.role = p_role;
		cur.start = p_time;
		cur.procedural = !has_clip(p_role);
		cur.seconds = role_seconds(p_role);
		cur.cut_in = p_cut_in;
	}

	void fade_out(double p_time) {
		if (cur.active && cur.fade_out_start < 0) {
			cur.fade_from = weight_of(cur, p_time);
			cur.fade_out_start = p_time;
		}
	}

	float weight_of(const Action &p_a, double p_t) const {
		if (!p_a.active) {
			return 0.0f;
		}
		const float x = kViewmodelCrossfadeSeconds;
		const float t = float(p_t - p_a.start);
		float w = p_a.cut_in ? 1.0f : clamp01(t / x + (t >= 0 ? 1e-6f : 0.0f));
		const float span = p_a.loop > 0.0f ? std::numeric_limits<float>::max() : p_a.seconds;
		if (span > 0.0f && !p_a.procedural) {
			w = std::min(w, clamp01((span - t) / x));
		} else if (p_a.seconds > 0.0f) {
			w = t < p_a.seconds ? w : 0.0f;
		} else {
			w = 0.0f;
		}
		if (p_a.fade_out_start >= 0) {
			const float u = float(p_t - p_a.fade_out_start);
			w = std::min(w, p_a.fade_from * clamp01(1.0f - u / x));
		}
		return w;
	}

	float clip_time(const Action &p_a, double p_t) const {
		const float t = p_a.offset + float(p_t - p_a.start);
		if (p_a.loop > 0.0f) {
			return std::fmod(std::max(0.0f, t), p_a.loop);
		}
		return t;
	}

	bool finished(const Action &p_a, double p_t) const {
		if (!p_a.active) {
			return true;
		}
		const double t = p_t - p_a.start;
		if (p_a.fade_out_start >= 0 && p_t - p_a.fade_out_start >= kViewmodelCrossfadeSeconds) {
			return true;
		}
		if (p_a.loop > 0.0f) {
			return false;
		}
		return t >= double(p_a.seconds);
	}

	void action_pose(const Action &p_a, double p_t, LocalPose &r_pose) {
		if (!p_a.procedural) {
			apply_clip_at(p_a.role, clip_time(p_a, p_t), r_pose);
		}
	}

	void procedural_offset(const Action &p_a, double p_t, float p_w, float &r_dy, float &r_dz, float &r_pitch, float &r_roll) {
		if (!p_a.active || !p_a.procedural || p_a.seconds <= 0.0f || p_w <= 0.0f) {
			return;
		}
		const float u = float(p_t - p_a.start) / p_a.seconds;
		switch (p_a.role) {
			case VmClip::Deploy: {
				const float k = 1.0f - smoothstep01(u);
				r_dy -= tuning.deploy_drop * k * p_w;
				r_pitch -= 35.0f * core::kDegToRadF * k * p_w;
				break;
			}
			case VmClip::Reload:
			case VmClip::ReloadEmpty:
			case VmClip::Bolt: {
				const float h = hump(u);
				r_dy -= tuning.reload_drop * h * p_w;
				r_roll += 25.0f * core::kDegToRadF * h * p_w;
				r_pitch += 10.0f * core::kDegToRadF * h * p_w;
				break;
			}
			case VmClip::Throw: {
				const float h = hump(u);
				r_pitch += 40.0f * core::kDegToRadF * h * p_w;
				r_dz -= 0.10f * h * p_w;
				r_dy += 0.05f * h * p_w;
				break;
			}
			case VmClip::Knife: {
				const float h = hump(u);
				r_dz -= 0.18f * h * p_w;
				r_pitch -= 10.0f * core::kDegToRadF * h * p_w;
				break;
			}
			default:
				break;
		}
	}

	void start_release(double p_time, float p_offset) {
		start_action(VmClip::Throw, p_time, false);
		cur.offset = std::clamp(p_offset, 0.0f, cur.seconds);
		cur.seconds = std::max(0.0f, cur.seconds - cur.offset);
		cur.flow_release = true;
		release_due = -1;
	}

	void release(double p_time) {
		if (p_time == last_release_cue) {
			return;
		}
		last_release_cue = p_time;
		if (cur.active && cur.role == VmClip::ThrowStart) {
			const double from = cur.start + double(flow.release_from_s);
			if (p_time < from) {
				release_due = from;
			} else {
				start_release(p_time, flow.release_in_s);
			}
			return;
		}
		start_release(p_time, 0.0f);
	}

	void advance_flow(double p_time) {
		if (!has_flow) {
			return;
		}
		for (int guard = 0; guard < 4; ++guard) {
			if (release_due >= 0 && p_time >= release_due) {
				start_release(release_due, flow.release_in_s);
				continue;
			}
			if (cur.active && cur.role == VmClip::ThrowStart && release_due < 0 &&
					p_time >= cur.start + double(flow.hold_from_s)) {
				start_action(VmClip::ThrowHold, cur.start + double(flow.hold_from_s), false);
				cur.loop = flow.hold_loop_s;
				continue;
			}
			if (cur.active && cur.flow_release && cur.fade_out_start < 0) {
				const double end = cur.start + double(flow.release_end_s - cur.offset);
				if (p_time >= end) {
					release_end = end;
					if (switch_back && flow.skip_undeploy) {
						next_pending = false;
						cur.flow_release = false;
					} else if (next_pending) {
						next_pending = false;
						start_action(VmClip::Deploy, end, false);
					} else {
						fade_out(end);
					}
					continue;
				}
			}
			break;
		}
	}

	void on_cue(const VmCue &p_cue, const ViewmodelFrameInput &p_input) {
		const double t = p_cue.time_s;
		advance_flow(t);
		if (has_flow) {
			switch (p_cue.kind) {
				case VmCue::Kind::HoldStarted:
					next_pending = false;
					release_due = -1;
					start_action(VmClip::ThrowStart, t, false);
					return;
				case VmCue::Kind::Shot:
					++shots;
					release(t);
					return;
				case VmCue::Kind::Thrown:
					release(t);
					return;
				case VmCue::Kind::ReloadStarted:
					if (cur.active && cur.flow_release && cur.fade_out_start < 0) {
						next_pending = true;
					} else {
						start_action(VmClip::Deploy, t, false);
					}
					return;
				case VmCue::Kind::DeployStarted:
					next_pending = false;
					release_due = -1;
					break;
				default:
					break;
			}
		}
		switch (p_cue.kind) {
			case VmCue::Kind::Shot: {
				++shots;
				const float ads = clamp01(p_input.ads);
				if (ref(VmClip::Knife).seconds > 0.0f || has_clip(VmClip::Knife)) {
					start_action(VmClip::Knife, t, false);
					break;
				}
				if (ref(VmClip::Throw).seconds > 0.0f || has_clip(VmClip::Throw)) {

					if (!(cur.active && cur.role == VmClip::Throw && std::fabs(cur.start - t) < 1e-6)) {
						start_action(VmClip::Throw, t, false);
					}
					break;
				}
				const bool fire_clip = has_clip(VmClip::FireSingle) || has_clip(VmClip::Fire);
				const float kick_scale = fire_clip ? ads * tuning.kick_ads_scale : 1.0f + (tuning.kick_ads_scale - 1.0f) * ads;
				kick_back.v += spring_impulse_for_peak(tuning.kick, tuning.kick_back * kick_scale);
				kick_pitch.v += spring_impulse_for_peak(tuning.kick, tuning.kick_pitch_deg * kick_scale);
				cam_pitch.v += spring_impulse_for_peak(tuning.camera_recoil, tuning.camera_kick_pitch_deg);

				const float yaw_sign = ((shots * 2654435761u) >> 31) ? 1.0f : -1.0f;
				cam_yaw.v += spring_impulse_for_peak(tuning.camera_recoil, 0.25f * tuning.camera_kick_pitch_deg * yaw_sign);
				if (fire_clip) {
					start_action(has_clip(VmClip::FireSingle) ? VmClip::FireSingle : VmClip::Fire, t, false);
				} else if (cur.active && cur.role != VmClip::Fire) {

					fade_out(t);
				}
				break;
			}
			case VmCue::Kind::DeployStarted:
				start_action(VmClip::Deploy, t, true);
				break;
			case VmCue::Kind::ReloadStarted:
				start_action(p_cue.empty_reload && (has_clip(VmClip::ReloadEmpty) || ref(VmClip::ReloadEmpty).seconds > 0.0f) ? VmClip::ReloadEmpty
																															  : VmClip::Reload,
						t, false);
				break;
			case VmCue::Kind::ReloadCancelled:
				if (cur.active && (cur.role == VmClip::Reload || cur.role == VmClip::ReloadEmpty)) {
					fade_out(t);
				}
				break;
			case VmCue::Kind::BoltStarted:
				start_action(VmClip::Bolt, t, false);
				break;
			case VmCue::Kind::Thrown:
				if (!(cur.active && cur.role == VmClip::Throw && std::fabs(cur.start - t) < 1e-6)) {
					start_action(VmClip::Throw, t, false);
				}
				break;
			case VmCue::Kind::DeployFinished:
			case VmCue::Kind::ReloadCommitted:
			case VmCue::Kind::ReloadFinished:
			case VmCue::Kind::HoldStarted:
				break;
		}
	}

	void step(const ViewmodelFrameInput &p_input, std::span<const VmCue> p_cues, LocalPose &r_pose, ViewmodelOutput &r_out) {
		const double t = p_input.time_s;
		const float dt = std::clamp(finite_or(p_input.dt, 0.0f), 0.0f, kMaxFrameSeconds);
		switch_back = p_input.switch_back;
		for (const VmCue &c : p_cues) {
			on_cue(c, p_input);
		}
		advance_flow(t);
		time = t;
		have_time = true;
		if (prev.active && (finished(prev, t) || weight_of(prev, t) <= 0.0f)) {
			prev = Action{};
		}
		if (cur.active && finished(cur, t)) {
			cur = Action{};
		}

		const float ads = clamp01(p_input.ads);
		int dir = ads > last_ads + 1e-6f ? 1 : (ads < last_ads - 1e-6f ? -1 : 0);
		if (dir != 0 && dir != ads_dir) {

			ads_clip_ok = (dir > 0 && last_ads <= 1e-6f) || (dir < 0 && last_ads >= 1.0f - 1e-6f);
		}
		if (dir == 0 && (ads <= 0.0f || ads >= 1.0f)) {
			ads_dir = 0;
		} else if (dir != 0) {
			ads_dir = dir;
		}
		last_ads = ads;
		base = idle;
		uint32_t base_clip = uint32_t(VmClip::Idle);
		const VmClip zoom_role = ads_dir > 0 ? VmClip::ZoomIn : VmClip::ZoomOut;
		const VmClipRef &zr = ref(zoom_role);
		if (ads_dir != 0 && ads > 0.0f && ads < 1.0f) {
			base_clip = uint32_t(zoom_role);
			if (ads_clip_ok && has_clip(zoom_role) && !zr.stand_in) {

				const float progress = ads_dir > 0 ? ads : 1.0f - ads;
				apply_clip_frame(zoom_role, progress * float(zr.tracks->end_frame), base);
			} else {
				blend_poses(idle, zoom, ads, base);
			}
		} else if (ads >= 1.0f) {
			base = zoom;
			base_clip = uint32_t(VmClip::ZoomIdle);
		} else if (ads > 0.0f) {
			blend_poses(idle, zoom, ads, base);
		}
		const float sprint_rate = 1.0f / std::max(0.01f, tuning.sprint_blend_seconds);
		sprint_w = clamp01(sprint_w + (p_input.sprinting ? 1.0f : -1.0f) * sprint_rate * dt);
		const bool sprint_clip = has_clip(VmClip::Sprint);
		if (sprint_w > 0.0f && sprint_clip) {
			layer = base;
			apply_clip_at(VmClip::Sprint, 0.0f, layer);
			blend_poses(base, layer, smoothstep01(sprint_w), base);
		}
		if (sprint_w > 0.5f) {
			base_clip = uint32_t(VmClip::Sprint);
		}

		r_pose = base;
		float dy = 0, dz = 0, pitch = 0, roll = 0, yaw = 0;
		for (Action *a : { &prev, &cur }) {
			if (!a->active) {
				continue;
			}
			float w = weight_of(*a, t);
			if (a->role == VmClip::Fire || a->role == VmClip::FireSingle) {
				w *= 1.0f - ads;
			}
			if (w <= 0.0f) {
				continue;
			}
			if (!a->procedural) {
				layer = r_pose;
				action_pose(*a, t, layer);
				blend_poses(r_pose, layer, w, r_pose);
			} else {
				procedural_offset(*a, t, w, dy, dz, pitch, roll);
			}
		}
		if (!sprint_clip && sprint_w > 0.0f) {
			const float s = smoothstep01(sprint_w);
			dy -= 0.05f * s;
			yaw += 25.0f * core::kDegToRadF * s;
			pitch -= 10.0f * core::kDegToRadF * s;
		}

		const bool loco_applied = delta_loco && vm_loco_apply(*delta_loco, p_input, sprint_w, ads, dt, tuning.sprint_blend_seconds, r_pose, r_out);

		const float speed = std::max(0.0f, finite_or(p_input.speed, 0.0f));
		sway_tx = std::clamp(finite_or(p_input.view_yaw_rate, 0.0f) * tuning.sway_scale, -tuning.sway_max, tuning.sway_max);
		sway_ty = std::clamp(-finite_or(p_input.view_pitch_rate, 0.0f) * tuning.sway_scale, -tuning.sway_max, tuning.sway_max);
		step_spring(tuning.sway, sway_tx, dt, sway_x);
		step_spring(tuning.sway, sway_ty, dt, sway_y);
		const float move = p_input.grounded ? std::min(speed / 4.0f, 1.5f) : 0.0f;
		bob_target = loco_applied ? 0.0f : tuning.bob_amplitude * move * (1.0f + (tuning.bob_ads_scale - 1.0f) * ads);
		step_spring(tuning.bob, bob_target, dt, bob_amp);
		bob_phase += 2.0 * kPi * double(tuning.bob_frequency) * double(std::max(move, 0.0f)) * double(dt);
		bob_phase = std::fmod(bob_phase, 2.0 * kPi);
		step_spring(tuning.kick, 0.0f, dt, kick_back);
		step_spring(tuning.kick, 0.0f, dt, kick_pitch);
		step_spring(tuning.camera_recoil, 0.0f, dt, cam_pitch);
		step_spring(tuning.camera_recoil, 0.0f, dt, cam_yaw);

		const float bx = bob_amp.x * float(std::sin(bob_phase));
		const float by = bob_amp.x * 0.5f * float(std::sin(2.0 * bob_phase));
		Xform off;
		off.t = Vec3{ sway_x.x + bx, sway_y.x + by + dy, kick_back.x + dz };
		off.q = camera_rotation(pitch + kick_pitch.x * core::kDegToRadF, yaw, roll);
		r_out.weapon_offset = off;
		r_out.camera_recoil_pitch_deg = cam_pitch.x;
		r_out.camera_recoil_yaw_deg = cam_yaw.x;
		r_out.springs_at_rest = spring_at_rest(tuning.sway, sway_tx, sway_x) && spring_at_rest(tuning.sway, sway_ty, sway_y) &&
				spring_at_rest(tuning.bob, bob_target, bob_amp) && spring_at_rest(tuning.kick, 0.0f, kick_back) &&
				spring_at_rest(tuning.kick, 0.0f, kick_pitch) && spring_at_rest(tuning.camera_recoil, 0.0f, cam_pitch) &&
				spring_at_rest(tuning.camera_recoil, 0.0f, cam_yaw);
		r_out.release_end_s = release_end;
		if (cur.active) {
			r_out.action_clip = uint32_t(cur.role);
			r_out.action_time_s = clip_time(cur, t);
			r_out.action_start_s = cur.start;
			r_out.action_weight = weight_of(cur, t) * ((cur.role == VmClip::Fire || cur.role == VmClip::FireSingle) ? 1.0f - ads : 1.0f);
			r_out.action_procedural = cur.procedural;
		} else {
			r_out.action_clip = uint32_t(VmClip::Idle);
			r_out.action_time_s = 0;
			r_out.action_start_s = 0;
			r_out.action_weight = 0;
			r_out.action_procedural = false;
		}
		r_out.base_clip = base_clip;
		r_out.sprint_weight = sprint_w;
		r_out.shots = shots;
		r_out.kick_back = kick_back.x;
	}

	void reset() {
		cur = Action{};
		prev = Action{};
		have_time = false;
		time = 0;
		last_ads = 0;
		ads_dir = 0;
		ads_clip_ok = false;
		sprint_w = 0;
		bob_phase = 0;
		shots = 0;
		sway_x = sway_y = bob_amp = kick_back = kick_pitch = cam_pitch = cam_yaw = SpringState{};
		sway_tx = sway_ty = bob_target = 0;
		next_pending = false;
		release_due = -1;
		last_release_cue = -1;
		release_end = -1;
		if (delta_loco) {
			vm_loco_reset(*delta_loco);
		}
	}
};

ViewmodelAnimator::ViewmodelAnimator(const Skeleton &p_rig, std::span<const VmClipRef, kVmClips> p_clips, const ViewmodelTuning &p_tuning) :
		impl_(std::make_unique<Impl>(p_rig, p_clips, p_tuning)) {}

ViewmodelAnimator::~ViewmodelAnimator() = default;

void ViewmodelAnimator::step(const ViewmodelFrameInput &p_input, std::span<const VmCue> p_cues, LocalPose &r_pose, ViewmodelOutput &r_out) {
	impl_->step(p_input, p_cues, r_pose, r_out);
}

void ViewmodelAnimator::reset() {
	impl_->reset();
}

const ViewmodelTuning &ViewmodelAnimator::tuning() const {
	return impl_->tuning;
}

std::array<SpringState, 7> ViewmodelAnimator::springs() const {
	return { impl_->sway_x, impl_->sway_y, impl_->bob_amp, impl_->kick_back, impl_->kick_pitch, impl_->cam_pitch, impl_->cam_yaw };
}

}

namespace bf3::anim {

struct VmDeltaLoco {
	const Skeleton *rig = nullptr;
	VmLocomotion loco;
	BoneMask run_mask, sprint_mask;
	bool run_ok = false, sprint_ok = false;
	float run_frame = 0.0f, sprint_frame = 0.0f;
	float move_w = 0.0f;
	LocalPose delta;
	core::ant::ClipPose scratch;
};

void VmDeltaLocoDeleter::operator()(VmDeltaLoco *p_loco) const {
	delete p_loco;
}

namespace {

bool vm_loco_usable(const Skeleton &p_rig, VmDeltaClip &r_d, BoneMask &r_mask) {
	const VmClipRef &c = r_d.clip;
	if (c.clip == nullptr || c.tracks == nullptr || !c.clip->consistent() || c.tracks->end_frame == 0) {
		return false;
	}
	const float end = float(c.tracks->end_frame);
	if (!std::isfinite(r_d.start_frame) || !std::isfinite(r_d.end_frame)) {
		r_d.start_frame = 0.0f;
		r_d.end_frame = end;
	}
	r_d.start_frame = std::clamp(r_d.start_frame, 0.0f, end);
	r_d.end_frame = std::clamp(r_d.end_frame, 0.0f, end);
	if (!(r_d.end_frame > r_d.start_frame)) {
		r_d.start_frame = 0.0f;
		r_d.end_frame = end;
	}
	r_mask = mask_from_tracks(p_rig, *c.tracks);
	return r_mask.size() == p_rig.size();
}

float vm_loco_advance(float p_frame, float p_frames, const VmDeltaClip &p_d) {
	const float len = p_d.end_frame - p_d.start_frame;
	if (!(len > 0.0f) || !std::isfinite(p_frames)) {
		return p_d.start_frame;
	}
	float f = (std::isfinite(p_frame) ? p_frame : p_d.start_frame) - p_d.start_frame + std::max(0.0f, p_frames);
	f -= len * std::floor(f / len);
	if (!(f >= 0.0f && f < len)) {
		f = 0.0f;
	}
	return p_d.start_frame + f;
}

}

bool vm_loco_apply(VmDeltaLoco &r_loco, const ViewmodelFrameInput &p_input, float p_sprint_w, float p_ads, float p_dt, float p_blend_seconds, LocalPose &r_pose,
		ViewmodelOutput &r_out) {
	if (!r_loco.run_ok && !r_loco.sprint_ok) {
		return false;
	}
	const float speed = std::isfinite(p_input.speed) ? std::max(0.0f, p_input.speed) : 0.0f;
	const bool moving = p_input.grounded && speed > kVmMovingSpeed;
	const float ramp = p_blend_seconds > 0.0f ? p_dt / p_blend_seconds : 1.0f;
	r_loco.move_w = std::clamp(r_loco.move_w + (moving ? ramp : -ramp), 0.0f, 1.0f);
	const float sprint_w = std::clamp(std::isfinite(p_sprint_w) ? p_sprint_w : 0.0f, 0.0f, 1.0f);
	const float ads = std::clamp(std::isfinite(p_ads) ? p_ads : 0.0f, 0.0f, 1.0f);
	const float run_w = r_loco.run_ok ? r_loco.move_w * (1.0f - sprint_w) * (1.0f - ads) : 0.0f;
	const float spr_w = r_loco.sprint_ok ? sprint_w : 0.0f;
	if (r_loco.run_ok) {
		const float fps = r_loco.loco.run.clip.tracks->fps > 0.0f ? r_loco.loco.run.clip.tracks->fps : 30.0f;
		r_loco.run_frame = vm_loco_advance(r_loco.run_frame, p_dt * fps * speed / kVmRunSpeedUnit, r_loco.loco.run);
	}
	if (r_loco.sprint_ok) {
		const float fps = r_loco.loco.sprint.clip.tracks->fps > 0.0f ? r_loco.loco.sprint.clip.tracks->fps : 30.0f;
		r_loco.sprint_frame = vm_loco_advance(r_loco.sprint_frame, sprint_w > 0.0f ? p_dt * fps : 0.0f, r_loco.loco.sprint);
	}
	auto add = [&](const VmDeltaClip &p_d, float p_frame, const BoneMask &p_mask, float p_w) {
		if (!(p_w > 0.0f)) {
			return;
		}
		r_loco.delta = identity_pose(*r_loco.rig);
		if (!core::ant::sample_clip(*p_d.clip.clip, p_frame, r_loco.scratch)) {
			return;
		}
		apply_clip_pose(*p_d.clip.tracks, r_loco.scratch, r_loco.delta);
		apply_additive(r_pose, r_loco.delta, p_mask, p_w, AdditiveOrder::LocalPost);
	};
	add(r_loco.loco.run, r_loco.run_frame, r_loco.run_mask, run_w);
	add(r_loco.loco.sprint, r_loco.sprint_frame, r_loco.sprint_mask, spr_w);
	r_out.loco_run_weight = run_w;
	r_out.loco_sprint_weight = spr_w;
	r_out.loco_run_frame = r_loco.run_frame;
	r_out.loco_sprint_frame = r_loco.sprint_frame;
	return true;
}

void vm_loco_reset(VmDeltaLoco &r_loco) {
	r_loco.run_frame = r_loco.loco.run.start_frame;
	r_loco.sprint_frame = r_loco.loco.sprint.start_frame;
	r_loco.move_w = 0.0f;
}

bool VmThrowFlow::valid() const {
	auto ok = [](float p_v) { return std::isfinite(p_v) && p_v >= 0.0f; };
	return ok(release_from_s) && ok(release_until_s) && ok(release_in_s) && ok(hold_from_s) && ok(release_end_s) &&
			std::isfinite(hold_loop_s) && hold_loop_s > 0.0f && release_until_s >= release_from_s &&
			release_end_s > release_in_s;
}

void ViewmodelAnimator::set_throw_flow(const VmThrowFlow &p_flow) {
	Impl &m = *impl_;
	m.flow = p_flow;
	m.has_flow = p_flow.valid() && m.has_clip(VmClip::ThrowStart) && m.has_clip(VmClip::ThrowHold) &&
			m.has_clip(VmClip::Throw);
	m.next_pending = false;
	m.release_due = -1;
	m.last_release_cue = -1;
	m.release_end = -1;
}

void ViewmodelAnimator::set_locomotion(const VmLocomotion &p_locomotion) {
	Impl &m = *impl_;
	std::unique_ptr<VmDeltaLoco, VmDeltaLocoDeleter> l(new VmDeltaLoco());
	l->rig = &m.rig;
	l->loco = p_locomotion;
	l->run_ok = vm_loco_usable(m.rig, l->loco.run, l->run_mask);
	l->sprint_ok = vm_loco_usable(m.rig, l->loco.sprint, l->sprint_mask);
	vm_loco_reset(*l);
	m.delta_loco = l->run_ok || l->sprint_ok ? std::move(l) : nullptr;
}

}
