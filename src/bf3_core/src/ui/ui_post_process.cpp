#include "bf3/core/ui/ui_post_process.h"

#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
#include "ui_ebx.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>

namespace bf3::core::ui {

namespace {

std::optional<std::array<float, 3>> vec3_field(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	const std::optional<Vec3> x = v ? v->as_vec3() : std::nullopt;
	return x && finite(*x) ? std::optional<std::array<float, 3>>(to_array(*x)) : std::nullopt;
}

}

Result<UiPostProcessData> read_ui_post_process(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kUiPostProcessCompPartition);
	if (part == nullptr) {
		return make_error("ui post process: no partition " + std::string(kUiPostProcessCompPartition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("UIPostProcessComponentData")) {
		return make_error("ui post process: " + std::string(kUiPostProcessCompPartition) + " is a " +
				std::string(root.type_name()));
	}
	const EbxStruct d = root.data();
	UiPostProcessData out;
	out.enabled = detail::flag_or(d, "Enabled", false);
	const std::array<std::pair<std::string_view, float *>, 9> fields{ {
			{ "RShift", &out.r_shift },
			{ "GShift", &out.g_shift },
			{ "BShift", &out.b_shift },
			{ "Distort", &out.distort },
			{ "XPower", &out.x_power },
			{ "YPower", &out.y_power },
			{ "TimeScale", &out.time_scale },
			{ "Highlight", &out.highlight },
			{ "HighlightThreshold", &out.highlight_threshold },
	} };
	for (const auto &[name, slot] : fields) {
		const std::optional<float> v = detail::f32(d, name);
		if (!v || !std::isfinite(*v)) {
			return make_error("ui post process: no " + std::string(name));
		}
		*slot = *v;
	}
	if (!(out.time_scale > 0.0f)) {
		return make_error("ui post process: TimeScale is not positive");
	}
	const std::optional<EbxRef> noise = detail::ref(d, "NoiseTexture");
	const std::optional<std::string> noise_name = noise ? detail::ref_partition(p_db, *part, *noise) : std::nullopt;
	if (!noise_name || noise_name->empty()) {
		return make_error("ui post process: no NoiseTexture");
	}
	out.noise_texture = *noise_name;
	if (const std::optional<EbxValue> v = d.get("DataSources")) {
		if (const OptionalEbxArray a = v->as_array()) {
			for (size_t i = 0; i < a->size(); ++i) {
				const std::optional<EbxValue> e = a->get(i);
				const std::optional<std::string_view> s = e ? e->as_str() : std::nullopt;
				if (s) {
					out.data_sources.emplace_back(*s);
				}
			}
		}
	}
	return out;
}

float ui_glitch_power(const UiPostProcessData &p_data, float p_start_power, double p_elapsed_s) {
	if (!std::isfinite(p_start_power) || p_start_power <= 0.0f || !std::isfinite(p_elapsed_s) ||
			!(p_data.time_scale > 0.0f)) {
		return 0.0f;
	}
	const double left = double(p_start_power) - std::max(0.0, p_elapsed_s) * double(p_data.time_scale);
	return left > 0.0 ? float(left) : 0.0f;
}

double ui_glitch_seconds(const UiPostProcessData &p_data, float p_start_power) {
	if (!std::isfinite(p_start_power) || p_start_power <= 0.0f || !(p_data.time_scale > 0.0f)) {
		return 0.0;
	}
	return double(p_start_power) / double(p_data.time_scale);
}

Result<MenuGrade> read_menu_visual_environment(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kMenuVisualEnvironmentPartition);
	if (part == nullptr) {
		return make_error("menu grade: no partition " + std::string(kMenuVisualEnvironmentPartition));
	}
	if (!part->primary().is_a("VisualEnvironmentBlueprint")) {
		return make_error("menu grade: " + std::string(kMenuVisualEnvironmentPartition) + " is a " +
				std::string(part->primary().type_name()));
	}
	MenuGrade out;
	for (const EbxInstance &c : part->instances_of("ColorCorrectionComponentData")) {
		const EbxStruct d = c.data();
		if (!detail::flag_or(d, "Enable", false)) {
			continue;
		}
		const std::optional<std::array<float, 3>> b = vec3_field(d, "Brightness");
		const std::optional<std::array<float, 3>> k = vec3_field(d, "Contrast");
		const std::optional<std::array<float, 3>> s = vec3_field(d, "Saturation");
		if (!b || !k || !s) {
			return make_error("menu grade: ColorCorrection without Brightness, Contrast and Saturation");
		}
		out.brightness = *b;
		out.contrast = *k;
		out.saturation = *s;
		out.correction = true;
	}
	for (const EbxInstance &c : part->instances_of("VignetteComponentData")) {
		const EbxStruct d = c.data();
		if (!detail::flag_or(d, "Enable", false)) {
			continue;
		}
		const std::optional<std::array<float, 3>> color = vec3_field(d, "Color");
		const std::optional<std::array<float, 2>> scale = detail::vec2(d, "Scale");
		const std::optional<float> opacity = detail::f32(d, "Opacity");
		const std::optional<float> exponent = detail::f32(d, "Exponent");
		if (!color || !scale || !opacity || !exponent || !std::isfinite(*opacity) || !std::isfinite(*exponent)) {
			return make_error("menu grade: Vignette without Color, Scale, Opacity and Exponent");
		}
		out.vignette_color = *color;
		out.vignette_scale = *scale;
		out.vignette_opacity = *opacity;
		out.vignette_exponent = *exponent;
		out.vignette = true;
	}
	for (const EbxInstance &c : part->instances_of("DofComponentData")) {
		const EbxStruct d = c.data();
		if (!detail::flag_or(d, "Enable", false)) {
			continue;
		}
		out.blur_add = detail::f32_or(d, "BlurAdd", 0.0f);
		out.blur_deviation = detail::f32_or(d, "BlurFilterDeviation", 0.0f);
		out.focus_distance = detail::f32_or(d, "FocusDistance", 0.0f);
		out.near_distance_scale = detail::f32_or(d, "NearDistanceScale", 0.0f);
		out.far_distance_scale = detail::f32_or(d, "FarDistanceScale", 0.0f);
		out.dof = std::isfinite(out.blur_deviation) && out.blur_deviation > 0.0f;
	}
	if (!out.correction) {
		return make_error("menu grade: no enabled ColorCorrectionComponentData");
	}
	return out;
}

std::array<float, 3> menu_grade_rgb(const MenuGrade &p_grade, const std::array<float, 3> &p_rgb, float p_visibility) {
	const float v = std::isfinite(p_visibility) ? std::clamp(p_visibility, 0.0f, 1.0f) : 0.0f;
	const float luma = p_rgb[0] * 0.299f + p_rgb[1] * 0.587f + p_rgb[2] * 0.114f;
	std::array<float, 3> out{};
	for (size_t k = 0; k < 3; ++k) {
		const float s = luma + (p_rgb[k] - luma) * p_grade.saturation[k];
		const float c = std::clamp(((s - 0.5f) * p_grade.contrast[k] + 0.5f) * p_grade.brightness[k], 0.0f, 1.0f);
		out[k] = p_rgb[k] + (c - p_rgb[k]) * v;
	}
	return out;
}

float menu_vignette(const MenuGrade &p_grade, float p_u, float p_v) {
	if (!p_grade.vignette) {
		return 0.0f;
	}
	const float x = (p_u - 0.5f) * p_grade.vignette_scale[0];
	const float y = (p_v - 0.5f) * p_grade.vignette_scale[1];
	const float d = std::sqrt(x * x + y * y);
	return std::pow(std::clamp(d, 0.0f, 1.0f), std::max(p_grade.vignette_exponent, 1e-3f)) *
			std::clamp(p_grade.vignette_opacity, 0.0f, 1.0f);
}

Result<UiDimmerRamp> read_ingame_dimmer(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kIngameDimmerPartition);
	if (part == nullptr) {
		return make_error("ingame dimmer: no partition " + std::string(kIngameDimmerPartition));
	}
	if (!part->primary().is_a("LogicPrefabBlueprint")) {
		return make_error("ingame dimmer: " + std::string(kIngameDimmerPartition) + " is a " +
				std::string(part->primary().type_name()));
	}
	UiDimmerRamp out;
	bool have_in = false;
	bool have_out = false;
	for (const EbxInstance &t : part->instances_of("FloatPropertyTrackData")) {
		const EbxStruct d = t.data();
		DimmerTrack track;
		const std::optional<EbxValue> id = d.get("Id");
		track.event_id = id ? id->as_i32().value_or(0) : 0;
		const std::optional<EbxValue> times = d.get("Times");
		const std::optional<EbxValue> values = d.get("Values");
		const OptionalEbxArray ta = times ? times->as_array() : OptionalEbxArray{};
		const OptionalEbxArray va = values ? values->as_array() : OptionalEbxArray{};
		if (!ta || !va || ta->size() != va->size() || ta->size() < 2 || ta->size() > kMaxDimmerKeys) {
			return make_error(
					"ingame dimmer: track " + std::to_string(track.event_id) + " has no matching Times and Values");
		}
		for (size_t i = 0; i < ta->size(); ++i) {
			const std::optional<EbxValue> tv = ta->get(i);
			const std::optional<EbxValue> vv = va->get(i);
			const std::optional<double> ms = detail::number(tv);
			const std::optional<double> value = detail::number(vv);
			if (!ms || !value || !std::isfinite(*ms) || !std::isfinite(*value) ||
					(!track.keys.empty() && float(*ms) < track.keys.back().ms)) {
				return make_error("ingame dimmer: track " + std::to_string(track.event_id) + " key " +
						std::to_string(i) + " is not a rising time");
			}
			track.keys.push_back(DimmerKey{ float(*ms), float(*value) });
		}
		track.length_ms = track.keys.back().ms;
		if (track.keys.back().value > 0.0f && !have_in) {
			out.in = std::move(track);
			have_in = true;
		} else if (track.keys.back().value < 0.0f && !have_out) {
			out.out = std::move(track);
			have_out = true;
		}
	}
	if (!have_in || !have_out) {
		return make_error("ingame dimmer: no rising and falling FloatPropertyTrackData pair");
	}
	for (const EbxInstance &l : part->instances_of("LogicVisualEnvironmentEntityData")) {
		const std::optional<EbxRef> ve = detail::ref(l.data(), "VisualEnvironment");
		const std::optional<std::string> name = ve ? detail::ref_partition(p_db, *part, *ve) : std::nullopt;
		if (name && !name->empty()) {
			out.visual_environment = *name;
		}
	}
	if (out.visual_environment.empty()) {
		return make_error("ingame dimmer: no LogicVisualEnvironmentEntityData VisualEnvironment");
	}
	return out;
}

float dimmer_track_value(const DimmerTrack &p_track, double p_elapsed_ms) {
	if (p_track.keys.empty()) {
		return 0.0f;
	}
	const double t = std::isfinite(p_elapsed_ms) ? p_elapsed_ms : 0.0;
	if (t <= double(p_track.keys.front().ms)) {
		return p_track.keys.front().value;
	}
	for (size_t i = 1; i < p_track.keys.size(); ++i) {
		const DimmerKey &a = p_track.keys[i - 1];
		const DimmerKey &b = p_track.keys[i];
		if (t <= double(b.ms)) {
			const double span = double(b.ms) - double(a.ms);
			const double f = span > 0.0 ? (t - double(a.ms)) / span : 1.0;
			return float(double(a.value) + (double(b.value) - double(a.value)) * f);
		}
	}
	return p_track.keys.back().value;
}

void UiDimmer::fade_in(double p_now_s) {
	in_started_ = true;
	in_at_s_ = p_now_s;
	out_started_ = false;
	++fade_ins_;
}

void UiDimmer::fade_out(double p_now_s) {
	if (!in_started_) {
		return;
	}
	out_started_ = true;
	out_at_s_ = p_now_s;
	++fade_outs_;
}

float UiDimmer::visibility(double p_now_s) const {
	if (!enabled(p_now_s)) {
		return 0.0f;
	}
	float v = dimmer_track_value(ramp_.in, (p_now_s - in_at_s_) * 1000.0);
	if (out_started_) {
		v += dimmer_track_value(ramp_.out, (p_now_s - out_at_s_) * 1000.0);
	}
	return std::clamp(v, 0.0f, 1.0f);
}

bool UiDimmer::enabled(double p_now_s) const {
	if (!in_started_) {
		return false;
	}
	return !(out_started_ && (p_now_s - out_at_s_) * 1000.0 >= double(ramp_.out.length_ms));
}

Result<std::vector<PostProcessAction>> read_post_process_actions(
		const EbxResolver &p_db, std::string_view p_graph_partition) {
	const EbxPartition *part = p_db.partition_by_name(p_graph_partition);
	if (part == nullptr) {
		return make_error("post process actions: no partition " + std::string(p_graph_partition));
	}
	std::vector<PostProcessAction> out;
	for (const EbxInstance &a : part->instances_of("ActionNode")) {
		const EbxStruct d = a.data();
		const std::optional<EbxValue> key = d.get("ActionKey");
		if (!key || key->as_i32().value_or(0) != kPostProcessEffectActionKey) {
			continue;
		}
		PostProcessAction action;
		const std::optional<EbxValue> name = d.get("Name");
		const std::optional<std::string_view> name_text = name ? name->as_str() : std::nullopt;
		action.name = name_text ? std::string(*name_text) : std::string();
		const std::optional<EbxValue> params = d.get("Params");
		const OptionalEbxArray pa = params ? params->as_array() : OptionalEbxArray{};
		const std::optional<EbxValue> first = pa && !pa->empty() ? pa->get(0) : std::nullopt;
		const std::optional<std::string_view> text = first ? first->as_str() : std::nullopt;
		float power = 0.0f;
		const std::from_chars_result r = text ? std::from_chars(text->data(), text->data() + text->size(), power)
											  : std::from_chars_result{ nullptr, std::errc::invalid_argument };
		if (r.ec != std::errc() || r.ptr != text->data() + text->size() || !std::isfinite(power) || power < 0.0f) {
			return make_error("post process actions: " + std::string(p_graph_partition) + " " + action.name +
					" has no numeric power");
		}
		action.power = power;
		out.push_back(std::move(action));
	}
	return out;
}

}
