// HdrSetting reader (hdr_setting.h; docs/design/m2b.md §6.1, docs/design/m2b-au.md). Fields by the data's own
// descriptor names; every instance examined and every curve point is charged before it is read.

#include "bf3/core/audio/hdr_setting.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::core::audio {

namespace {

using gameplay::WorkBudget;

Error hdr_error(const std::string &p_what) {
	return make_error("HdrSetting: " + p_what);
}

// A float field: absent keeps the default; present but not a finite number is an error.
Status read_field(const EbxStruct &p_s, std::string_view p_field, float &r_value) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		return Status::success();
	}
	const std::optional<float> f = v->as_f32();
	const float value = f ? *f : float(v->as_f64().value_or(std::nan("")));
	if (!std::isfinite(value)) {
		return hdr_error(std::string(p_field) + " is not a finite number");
	}
	r_value = value;
	return Status::success();
}

Status read_curve(const EbxStruct &p_s, std::string_view p_field, WorkBudget &r_budget, AudioCurve &r_curve) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	const std::optional<EbxStruct> c = v ? v->as_struct() : std::nullopt;
	if (!c) {
		return Status::success(); // no curve: the release is linear (hdr_window.h)
	}
	BF3_TRY(r_budget.charge_type(*c));
	const std::optional<EbxValue> pts = c->get("Points");
	if (!pts || !pts->as_array()) {
		return Status::success();
	}
	if (pts->as_array()->size() > kMaxCurvePoints) {
		return hdr_error(std::string(p_field) + " has " + std::to_string(pts->as_array()->size()) + " points (at most " + std::to_string(kMaxCurvePoints) + ")");
	}
	r_curve.points.clear();
	for (EbxValue e : *pts->as_array()) {
		BF3_TRY(r_budget.charge(1));
		const std::optional<EbxStruct> ps = e.as_struct();
		if (!ps) {
			return hdr_error(std::string(p_field) + ": a point is not an AudioCurvePoint");
		}
		AudioCurvePoint pt;
		BF3_TRY(read_field(*ps, "X", pt.x));
		BF3_TRY(read_field(*ps, "Y", pt.y));
		BF3_TRY(read_field(*ps, "K", pt.k));
		r_curve.points.push_back(pt);
	}
	std::stable_sort(r_curve.points.begin(), r_curve.points.end(), [](const AudioCurvePoint &a, const AudioCurvePoint &b) { return a.x < b.x; });
	return Status::success();
}

bool in(float p_v, float p_lo, float p_hi) {
	return std::isfinite(p_v) && p_v >= p_lo && p_v <= p_hi;
}

} // namespace

AudioCurve default_hdr_release_curve() {
	// The retail WindowTopReleaseTimeCurve [verified 2026-10-19: bf3dump dump of sound/soundstates on MP_Subway].
	AudioCurve c;
	c.points = {
		{ 0.0020000027f, 0.976f, -0.45407736f },
		{ 0.24000001f, 0.73200005f, -2.167475f },
		{ 0.404f, 0.308f, -2.240487f },
		{ 0.634f, 0.052f, -0.41156512f },
		{ 0.998f, 0.008f, 0.0f },
	};
	return c;
}

HdrSetting retail_hdr_setting() {
	HdrSetting s;
	s.top_release_curve = default_hdr_release_curve();
	return s;
}

Status validate_hdr_setting(const HdrSetting &p) {
	if (!in(p.window_size_db, 0.0f, 200.0f) || !(p.window_size_db > 0.0f)) {
		return hdr_error("WindowSize " + std::to_string(p.window_size_db) + " out of (0, 200]");
	}
	for (float v : { p.window_min_top_db, p.window_min_bottom_db, p.headroom_db, p.allowed_overshoot_db }) {
		if (!in(v, 0.0f, 200.0f)) {
			return hdr_error("a level " + std::to_string(v) + " dB out of [0, 200]");
		}
	}
	for (float v : { p.top_attack_s, p.top_min_release_s, p.top_max_release_s, p.bottom_attack_s, p.bottom_release_s }) {
		if (!in(v, 0.0f, 10.0f)) {
			return hdr_error("a time " + std::to_string(v) + " s out of [0, 10]");
		}
	}
	if (p.top_min_release_s > p.top_max_release_s) {
		return hdr_error("WindowTopMinReleaseTime above WindowTopMaxReleaseTime");
	}
	if (!std::isfinite(p.compress_factor) || !std::isfinite(p.discharge_factor) || !std::isfinite(p.max_allowed_energy)) {
		return hdr_error("CompressFactor / DischargeFactor / MaxAllowedEnergy not finite");
	}
	if (p.top_release_curve.points.size() > kMaxCurvePoints) {
		return hdr_error("release curve with " + std::to_string(p.top_release_curve.points.size()) + " points");
	}
	for (size_t k = 0; k < p.top_release_curve.points.size(); ++k) {
		const AudioCurvePoint &pt = p.top_release_curve.points[k];
		if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.k) || (k > 0 && pt.x < p.top_release_curve.points[k - 1].x)) {
			return hdr_error("release curve point " + std::to_string(k));
		}
	}
	return Status::success();
}

Result<HdrSetting> read_hdr_setting(const EbxResolver &p_db, gameplay::WorkBudget *p_budget) {
	WorkBudget local;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	const EbxPartition *part = p_db.partition_by_name(kSoundStatesPartition);
	if (part == nullptr) {
		return hdr_error(std::string(kSoundStatesPartition) + " is not in the database");
	}
	for (size_t i = 0; i < part->instance_count(); ++i) {
		const std::optional<EbxInstance> inst = part->instance(i);
		if (!inst) {
			continue;
		}
		BF3_TRY(budget.charge_type(inst->data()));
		if (!inst->is_a("HdrSetting")) {
			continue;
		}
		const EbxStruct s = inst->data();
		HdrSetting h;
		BF3_TRY(read_field(s, "WindowSize", h.window_size_db));
		BF3_TRY(read_field(s, "WindowMinTop", h.window_min_top_db));
		BF3_TRY(read_field(s, "WindowMinBottom", h.window_min_bottom_db));
		BF3_TRY(read_field(s, "WindowTopAttackTime", h.top_attack_s));
		BF3_TRY(read_field(s, "WindowTopMinReleaseTime", h.top_min_release_s));
		BF3_TRY(read_field(s, "WindowTopMaxReleaseTime", h.top_max_release_s));
		BF3_TRY(read_curve(s, "WindowTopReleaseTimeCurve", budget, h.top_release_curve));
		BF3_TRY(read_field(s, "WindowBottomAttackTime", h.bottom_attack_s));
		BF3_TRY(read_field(s, "WindowBottomReleaseTime", h.bottom_release_s));
		BF3_TRY(read_field(s, "Headroom", h.headroom_db));
		BF3_TRY(read_field(s, "AllowedOvershoot", h.allowed_overshoot_db));
		BF3_TRY(read_field(s, "CompressFactor", h.compress_factor));
		BF3_TRY(read_field(s, "DischargeFactor", h.discharge_factor));
		BF3_TRY(read_field(s, "MaxAllowedEnergy", h.max_allowed_energy));
		BF3_TRY(validate_hdr_setting(h));
		BF3_TRY(budget.status());
		return h;
	}
	return hdr_error(std::string(kSoundStatesPartition) + " holds no HdrSetting");
}

} // namespace bf3::core::audio
