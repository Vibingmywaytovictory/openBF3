#include "bf3/core/gameplay/eor_fading.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/level/connections.h"
#include "gameplay_read.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr int32_t kStart = logic_port_id("Start");
constexpr int32_t kOnStarted = logic_port_id("OnStarted");
constexpr int32_t kOnFinished = logic_port_id("OnFinished");
constexpr int32_t kFadeOut = logic_port_id("FadeOut");
constexpr int32_t kFadeTime = logic_port_id("FadeTime");
constexpr int32_t kVisibility = logic_port_id("Visibility");
constexpr int32_t kValue = logic_port_id("Value");

std::optional<EbxInstance> event_target(const level::Connections &p_c, const EbxInstance &p_source, int32_t p_event,
		int32_t p_target_event, std::string_view p_type) {
	for (const level::Connection &c : p_c.event) {
		if (c.source == p_source && c.source_field == p_event && c.target_field == p_target_event &&
				c.target.is_a(p_type)) {
			return c.target;
		}
	}
	return std::nullopt;
}

std::optional<float> seconds_in(const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<double> v = number(p_object, p_field);
	if (!v || !std::isfinite(*v) || *v < 0.0 || *v > 3600000.0) {
		return std::nullopt;
	}
	return float(*v);
}

std::optional<float> fade_time(const level::Connections &p_c, const EbxInstance &p_fade) {
	for (const level::Connection &c : p_c.property) {
		if (c.target == p_fade && c.target_field == kFadeTime && c.source_field == kValue &&
				c.source.is_a("FloatEntityData")) {
			return seconds_in(c.source.data(), "DefaultValue");
		}
	}
	return seconds_in(p_fade.data(), "FadeTime");
}

std::optional<std::array<float, 3>> vec3_field(const EbxStruct &p_object, std::string_view p_field) {
	const std::optional<core::Vec3> v = vec3(p_object, p_field);
	if (!v || !std::isfinite(v->x) || !std::isfinite(v->y) || !std::isfinite(v->z)) {
		return std::nullopt;
	}
	return std::array<float, 3>{ v->x, v->y, v->z };
}

std::optional<EbxInstance> ref_of(const EbxResolver &p_db, const EbxInstance &p_from, std::string_view p_field) {
	const std::optional<EbxValue> v = p_from.get(p_field);
	const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
	return r ? p_db.resolve(p_from.partition(), *r) : std::nullopt;
}

}

Result<std::vector<EorFadingKey>> read_sequence_float_track(
		const EbxResolver &p_db, const EbxInstance &p_sequence, int32_t p_track) {
	const std::optional<EbxValue> tracks = p_sequence.get("PropertyTracks");
	const OptionalEbxArray list = tracks ? tracks->as_array() : OptionalEbxArray();
	if (!list) {
		return make_error("the sequence has no PropertyTracks");
	}
	for (const EbxValue v : *list) {
		const std::optional<EbxRef> r = v.as_ref();
		const std::optional<EbxInstance> track = r ? p_db.resolve(p_sequence.partition(), *r) : std::nullopt;
		if (!track || !track->is_a("FloatPropertyTrackData")) {
			continue;
		}
		const std::optional<EbxValue> id = track->get("Id");
		if (!id || id->as_i32() != std::optional<int32_t>(p_track)) {
			continue;
		}
		const std::optional<EbxValue> times = track->get("Times");
		const std::optional<EbxValue> values = track->get("Values");
		const OptionalEbxArray ta = times ? times->as_array() : OptionalEbxArray();
		const OptionalEbxArray va = values ? values->as_array() : OptionalEbxArray();
		if (!ta || !va || ta->size() != va->size() || ta->size() == 0) {
			return make_error("the float track's times and values differ");
		}
		std::vector<EorFadingKey> keys;
		for (size_t i = 0; i < ta->size(); ++i) {
			const std::optional<EbxValue> t = ta->get(i);
			const std::optional<EbxValue> value = va->get(i);
			const std::optional<double> ms = t ? number(*t) : std::nullopt;
			const std::optional<double> x = value ? number(*value) : std::nullopt;
			if (!ms || !x || !std::isfinite(*ms) || !std::isfinite(*x) ||
					(!keys.empty() && float(*ms / 1000.0) < keys.back().seconds)) {
				return make_error("a float track key is not an increasing number");
			}
			keys.push_back(EorFadingKey{ float(*ms / 1000.0), float(*x) });
		}
		return keys;
	}
	return make_error("no float track " + std::to_string(p_track));
}

float eor_fading_key_at(std::span<const EorFadingKey> p_keys, float p_seconds) {
	if (p_keys.empty()) {
		return 0.0f;
	}
	if (p_seconds <= p_keys.front().seconds) {
		return p_keys.front().value;
	}
	for (size_t i = 1; i < p_keys.size(); ++i) {
		const EorFadingKey &a = p_keys[i - 1];
		const EorFadingKey &b = p_keys[i];
		if (p_seconds <= b.seconds) {
			const float span = b.seconds - a.seconds;
			const float t = span > 0.0f ? (p_seconds - a.seconds) / span : 1.0f;
			return a.value + (b.value - a.value) * t;
		}
	}
	return p_keys.back().value;
}

namespace {

Status read_grade(
		const EbxResolver &p_db, const EbxInstance &p_environment, const std::string &p_where, EorFading &r_out) {
	const std::optional<EbxInstance> blueprint = ref_of(p_db, p_environment, "VisualEnvironment");
	const std::optional<EbxInstance> ve = blueprint ? ref_of(p_db, *blueprint, "Object") : std::nullopt;
	const std::optional<EbxValue> components = ve ? ve->get("Components") : std::nullopt;
	const OptionalEbxArray list = components ? components->as_array() : OptionalEbxArray();
	if (!list) {
		return make_error(p_where + ": the visual environment has no components");
	}
	for (const EbxValue v : *list) {
		const std::optional<EbxRef> r = v.as_ref();
		const std::optional<EbxInstance> cc = r ? p_db.resolve(ve->partition(), *r) : std::nullopt;
		if (!cc || !cc->is_a("ColorCorrectionComponentData")) {
			continue;
		}
		const std::optional<std::array<float, 3>> b = vec3_field(cc->data(), "Brightness");
		const std::optional<std::array<float, 3>> k = vec3_field(cc->data(), "Contrast");
		const std::optional<std::array<float, 3>> s = vec3_field(cc->data(), "Saturation");
		if (!b || !k || !s) {
			return make_error(p_where + ": the colour correction lacks Brightness, Contrast or Saturation");
		}
		r_out.brightness = *b;
		r_out.contrast = *k;
		r_out.saturation = *s;
		r_out.visual_environment = std::string(p_db.name_of(blueprint->partition().guid()).value_or(""));
		return Status::success();
	}
	return make_error(p_where + ": the visual environment has no colour correction");
}

}

float EorFading::visibility_at(float p_seconds) const {
	if (visibility.empty() || p_seconds < 0.0f || p_seconds >= flash_s + screen_fade_s) {
		return 0.0f;
	}
	return eor_fading_key_at(visibility, p_seconds);
}

float EorFading::screen_black_at(float p_seconds) const {
	if (p_seconds < flash_s) {
		return 0.0f;
	}
	if (screen_fade_s <= 0.0f) {
		return 1.0f;
	}
	return std::clamp((p_seconds - flash_s) / screen_fade_s, 0.0f, 1.0f);
}

float EorFading::ui_alpha_at(float p_seconds) const {
	if (p_seconds < 0.0f) {
		return 1.0f;
	}
	if (ui_fade_s <= 0.0f) {
		return 0.0f;
	}
	return 1.0f - std::clamp(p_seconds / ui_fade_s, 0.0f, 1.0f);
}

Result<EorFading> read_eor_fading(const EbxResolver &p_db, level::WorkBudget &r_budget) {
	const std::string where = "eor fading " + std::string(kEorFadingPartition);
	const EbxPartition *partition = p_db.partition_by_name(kEorFadingPartition);
	if (partition == nullptr) {
		return make_error(where + ": partition not found");
	}
	const EbxInstance blueprint = partition->primary();
	if (!blueprint.is_a("LogicPrefabBlueprint")) {
		return make_error(where + ": the primary instance is a " + std::string(blueprint.type_name()));
	}
	const std::optional<EbxInstance> descriptor = ref_of(p_db, blueprint, "Descriptor");
	if (!descriptor) {
		return make_error(where + ": no Descriptor");
	}
	Result<level::Connections> connections = level::Connections::of(p_db, blueprint, r_budget);
	BF3_TRY(connections);
	const level::Connections &c = *connections;
	const std::optional<EbxInstance> sequence =
			event_target(c, *descriptor, logic_port_id(kEorFadingStartEvent), kStart, "SequenceEntityData");
	if (!sequence) {
		return make_error(where + ": StartFade starts no sequence");
	}
	EorFading out;
	const std::optional<float> length_ms = seconds_in(sequence->data(), "SequenceLength");
	if (!length_ms || *length_ms <= 0.0f) {
		return make_error(where + ": the sequence has no SequenceLength");
	}
	out.flash_s = *length_ms / 1000.0f;
	std::optional<EbxInstance> environment;
	int32_t track = 0;
	for (const level::Connection &p : c.property) {
		if (p.source == *sequence && p.target_field == kVisibility &&
				p.target.is_a("LogicVisualEnvironmentEntityData")) {
			environment = p.target;
			track = p.source_field;
		}
	}
	if (!environment) {
		return make_error(where + ": the sequence drives no visual environment's Visibility");
	}
	Result<std::vector<EorFadingKey>> keys = read_sequence_float_track(p_db, *sequence, track);
	if (!keys) {
		return make_error(where + ": " + keys.error().message);
	}
	out.visibility = std::move(*keys);
	BF3_TRY(read_grade(p_db, *environment, where, out));
	const std::optional<EbxInstance> ui_fade = event_target(c, *sequence, kOnStarted, kFadeOut, "FadeEntityData");
	const std::optional<EbxInstance> screen_fade = event_target(c, *sequence, kOnFinished, kFadeOut, "FadeEntityData");
	if (!ui_fade || !screen_fade) {
		return make_error(where + ": the sequence starts no UI fade or ends in no screen fade");
	}
	const std::optional<float> ui_s = fade_time(c, *ui_fade);
	const std::optional<float> screen_s = fade_time(c, *screen_fade);
	if (!ui_s || !screen_s || boolean(ui_fade->data(), "FadeUI") != std::optional<bool>(true) ||
			boolean(screen_fade->data(), "FadeScreen") != std::optional<bool>(true)) {
		return make_error(where + ": the fades lack a FadeTime, or do not fade the UI and then the screen");
	}
	out.ui_fade_s = *ui_s;
	out.screen_fade_s = *screen_s;
	return out;
}

}
