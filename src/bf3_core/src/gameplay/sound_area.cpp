#include "bf3/core/gameplay/sound_area.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace bf3::core::gameplay {

namespace {

constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kTiltTolerance = 1e-3f;

struct Plane2 {
	float u = 0.0f, v = 0.0f;
};

float segment_distance(const Plane2 &p_p, const Plane2 &p_a, const Plane2 &p_b) {
	const float ex = p_b.u - p_a.u, ey = p_b.v - p_a.v;
	const float len2 = ex * ex + ey * ey;
	float t = len2 > 0.0f ? ((p_p.u - p_a.u) * ex + (p_p.v - p_a.v) * ey) / len2 : 0.0f;
	t = std::clamp(t, 0.0f, 1.0f);
	const float dx = p_p.u - (p_a.u + t * ex), dy = p_p.v - (p_a.v + t * ey);
	return std::sqrt(dx * dx + dy * dy);
}

bool inside_polygon(const Plane2 &p_p, std::span<const Plane2> p_poly) {
	bool in = false;
	for (size_t i = 0, j = p_poly.size() - 1; i < p_poly.size(); j = i++) {
		const Plane2 &a = p_poly[i], &b = p_poly[j];
		if ((a.v > p_p.v) != (b.v > p_p.v) && p_p.u < (b.u - a.u) * (p_p.v - a.v) / (b.v - a.v) + a.u) {
			in = !in;
		}
	}
	return in;
}

std::optional<double> number(const std::optional<PropertyValue> &p_v) {
	return p_v ? p_v->as_f64() : std::nullopt;
}

std::optional<AreaFadeCurve> fade_curve(std::string_view p_name) {
	if (p_name == "FadeCurveType_SineAmplitude") {
		return AreaFadeCurve::SineAmplitude;
	}
	if (p_name == "FadeCurveType_LinearAmplitude") {
		return AreaFadeCurve::LinearAmplitude;
	}
	if (p_name == "FadeCurveType_LinearDecibel") {
		return AreaFadeCurve::LinearDecibel;
	}
	return std::nullopt;
}

struct Reader {
	const LogicWorld &world;
	const LogicGraph &graph;
	SoundAreaData out;
	std::map<std::pair<uint32_t, Guid>, uint32_t> objects;
	std::map<uint32_t, level::LevelTransform> scope_world;

	Error error(const std::string &p_what) const { return make_error("sound areas: " + p_what); }

	std::optional<EbxInstance> instance(uint32_t p_node) const {
		const LogicNode &n = graph.nodes()[p_node];
		const auto it = objects.find({ n.scope, n.guid });
		if (it == objects.end()) {
			return std::nullopt;
		}
		return world.objects()[it->second].entity;
	}

	level::LevelTransform scope_transform(uint32_t p_scope) {
		if (const auto it = scope_world.find(p_scope); it != scope_world.end()) {
			return it->second;
		}
		level::LevelTransform t = level::LevelTransform::identity();
		const LogicScope &s = world.scopes()[p_scope];
		if (s.parent && s.reference) {
			const level::LevelTransform parent = scope_transform(*s.parent);
			level::LevelTransform local = level::LevelTransform::identity();
			const std::optional<PropertyValue> fed =
					world.property(*s.parent, *s.reference, field_id("BlueprintTransform"));
			if (fed && fed->kind == PropertyValue::Kind::Transform) {
				local = level::LevelTransform::from_rows(fed->rows);
			} else if (const std::optional<EbxValue> own = s.reference->get("BlueprintTransform")) {
				if (const std::optional<level::Rows> rows = own->as_transform()) {
					local = level::LevelTransform::from_rows(*rows);
				}
			}
			t = local.in_parent(parent);
		}
		scope_world.emplace(p_scope, t);
		return t;
	}

	Result<std::optional<AreaShape>> shape(uint32_t p_shape_node) {
		const std::optional<EbxInstance> inst = instance(p_shape_node);
		if (!inst) {
			return std::optional<AreaShape>();
		}
		const level::LevelTransform frame = scope_transform(graph.nodes()[p_shape_node].scope);
		AreaShape s;
		if (graph.nodes()[p_shape_node].type == LogicTypeId::Obb) {
			s.kind = AreaShape::Kind::Box;
			const std::optional<EbxValue> transform = inst->get("Transform");
			const std::optional<level::Rows> rows = transform ? transform->as_transform() : std::nullopt;
			const std::optional<EbxValue> half = inst->get("HalfExtents");
			const std::optional<Vec3> extents = half ? half->as_vec3() : std::nullopt;
			if (!rows || !extents) {
				return error("an OBBData has no Transform or HalfExtents");
			}
			s.box = level::LevelTransform::from_rows(*rows).in_parent(frame);
			s.half_extents = *extents;
			return std::optional<AreaShape>(std::move(s));
		}
		s.kind = AreaShape::Kind::Prism;
		const std::optional<EbxValue> points = inst->get("Points");
		const OptionalEbxArray array = points ? points->as_array() : OptionalEbxArray();
		if (!array || array->size() < 3 || array->size() > kMaxAreaShapePoints) {
			return error("a VolumeVectorShapeData has " + std::to_string(array ? array->size() : 0) + " points");
		}
		for (const EbxValue p : *array) {
			const std::optional<Vec3> v = p.as_vec3();
			if (!v) {
				return error("a VolumeVectorShapeData point is not a Vec3");
			}
			s.polygon.push_back(frame.transform_point(*v));
		}
		Vec3 up{ 0.0f, 1.0f, 0.0f };
		const std::optional<EbxValue> normals = inst->get("Normals");
		const OptionalEbxArray narray = normals ? normals->as_array() : OptionalEbxArray();
		bool first = true;
		if (narray) {
			for (const EbxValue n : *narray) {
				const std::optional<Vec3> v = n.as_vec3();
				if (!v || vec::length(*v) <= 0.0f) {
					continue;
				}
				const Vec3 unit = vec::normalize(*v);
				if (first) {
					up = unit;
					first = false;
				} else if (vec::length(vec::sub(unit, up)) > kTiltTolerance) {
					++out.tilted_normals;
					break;
				}
			}
		}
		s.up = vec::normalize(frame.rotate(up));
		const std::optional<EbxValue> height = inst->get("Height");
		s.height = float(height ? height->as_f64().value_or(0.0) : 0.0) * frame.scale;
		const std::optional<EbxValue> closed = inst->get("IsClosed");
		if (closed && !closed->as_bool().value_or(true)) {
			++out.open_paths;
		}
		const std::optional<EbxValue> tension = inst->get("Tension");
		if (tension && tension->as_f64().value_or(0.0) != 0.0) {
			++out.curved_paths;
		}
		return std::optional<AreaShape>(std::move(s));
	}

	Status area(uint32_t p_node, LogicTypeId p_type) {
		const std::optional<EbxInstance> inst = instance(p_node);
		if (!inst) {
			return error("node " + std::to_string(p_node) + " has no world object");
		}
		const uint32_t scope = graph.nodes()[p_node].scope;
		AreaDef a;
		a.node = p_node;
		a.type = p_type;
		if (p_type == LogicTypeId::AreaProximity) {
			a.out_port = kAreaProximityOut;
			a.curve = AreaFadeCurve::LinearAmplitude;
			a.fade_width = float(number(world.effective(scope, *inst, "ProximityDistance")).value_or(0.0));
		} else {
			a.out_port = kSoundAreaProximity;
			a.fade_width = float(number(world.effective(scope, *inst, "FadeWidth")).value_or(0.0));
			const std::optional<EbxValue> multiplier = inst->get("ProximityMultiplier");
			a.proximity_multiplier = float(multiplier ? multiplier->as_f64().value_or(1.0) : 1.0);
			const std::optional<PropertyValue> curve = world.effective(scope, *inst, "FadeCurve");
			const std::optional<std::string_view> name = curve ? curve->enum_name() : std::nullopt;
			const std::optional<AreaFadeCurve> c = name ? fade_curve(*name) : std::nullopt;
			if (!c) {
				return error("a SoundAreaEntityData has the fade curve '" + std::string(name.value_or("")) + "'");
			}
			a.curve = *c;
		}
		if (!std::isfinite(a.fade_width) || a.fade_width < 0.0f || !std::isfinite(a.proximity_multiplier)) {
			return error("node " + std::to_string(p_node) + " has a non-finite or negative fade");
		}
		for (const LogicEdge &e : graph.links_from(p_node)) {
			if (e.source_port != kAreaShapeLink || e.target >= graph.nodes().size()) {
				continue;
			}
			const LogicTypeId t = graph.nodes()[e.target].type;
			if (t != LogicTypeId::VolumeVectorShape && t != LogicTypeId::Obb) {
				continue;
			}
			Result<std::optional<AreaShape>> s = shape(e.target);
			if (!s) {
				return s.error();
			}
			if (*s) {
				a.shape = std::move(**s);
				a.shape_node = e.target;
				break;
			}
		}
		if (a.shape_node == kNoLogicIndex) {
			++out.unshaped;
			return Status::success();
		}
		out.areas.push_back(std::move(a));
		return Status::success();
	}

	Status mixer(uint32_t p_node) {
		const std::optional<EbxInstance> inst = instance(p_node);
		const std::optional<EbxValue> field = inst ? inst->get("Mixer") : std::nullopt;
		const std::optional<EbxRef> ref = field ? field->as_ref() : std::nullopt;
		const std::optional<EbxInstance> asset =
				ref && !ref->is_null() ? world.db().resolve(inst->partition(), *ref) : std::nullopt;
		if (!asset) {
			++out.mixers_without_asset;
			return Status::success();
		}
		const Guid partition = asset->partition().guid();
		uint32_t index = 0;
		while (index < out.mixers.size() && out.mixers[index].partition != partition) {
			++index;
		}
		if (index == out.mixers.size()) {
			Result<audio::MixerAsset> m = audio::read_mixer_asset(world.db(), asset->partition());
			if (!m) {
				return error(m.error().message);
			}
			out.mixers.push_back(std::move(*m));
		}
		out.mixer_entities.push_back(MixerEntityDef{ p_node, index });
		return Status::success();
	}

	Result<SoundAreaData> run() {
		if (graph.scopes().size() != world.scopes().size()) {
			return error("the graph has " + std::to_string(graph.scopes().size()) + " scopes, the world " +
					std::to_string(world.scopes().size()));
		}
		for (uint32_t i = 0; i < world.objects().size(); ++i) {
			const LogicObject &o = world.objects()[i];
			objects.emplace(std::make_pair(o.scope, o.entity.guid()), i);
		}
		for (const LogicTypeId t : { LogicTypeId::SoundArea, LogicTypeId::AreaProximity }) {
			for (const uint32_t n : graph.nodes_of_type(t)) {
				BF3_TRY(area(n, t));
			}
		}
		for (const uint32_t n : graph.nodes_of_type(LogicTypeId::Mixer)) {
			BF3_TRY(mixer(n));
		}
		return std::move(out);
	}
};

}

float area_fade(AreaFadeCurve p_curve, float p_distance, float p_width) {
	if (!(p_distance > 0.0f)) {
		return 1.0f;
	}
	if (!(p_width > 0.0f) || p_distance >= p_width) {
		return 0.0f;
	}
	const float t = p_distance / p_width;
	switch (p_curve) {
		case AreaFadeCurve::SineAmplitude:
			return std::cos(t * kHalfPi);
		case AreaFadeCurve::LinearAmplitude:
			return 1.0f - t;
		case AreaFadeCurve::LinearDecibel:
			return std::pow(10.0f, kAreaFadeDecibelFloor * t / 20.0f);
	}
	return 0.0f;
}

float AreaShape::distance(const Vec3 &p_point) const {
	if (kind == Kind::Box) {
		const Vec3 d = vec::sub(p_point, box.trans);
		float sum = 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			Vec3 unit{ 0.0f, 0.0f, 0.0f };
			unit[axis] = 1.0f;
			const Vec3 world_axis = box.transform_vector(unit);
			const float len = vec::length(world_axis);
			if (!(len > 0.0f)) {
				return std::numeric_limits<float>::infinity();
			}
			const float along = std::fabs(vec::dot(d, world_axis) / len);
			const float excess = std::max(0.0f, along - half_extents[axis] * len);
			sum += excess * excess;
		}
		return std::sqrt(sum);
	}
	if (polygon.size() < 3) {
		return std::numeric_limits<float>::infinity();
	}
	const Vec3 helper = std::fabs(up.y) < 0.9f ? Vec3{ 0.0f, 1.0f, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
	const Vec3 u = vec::normalize(vec::cross(helper, up));
	const Vec3 v = vec::cross(up, u);
	std::vector<Plane2> flat;
	flat.reserve(polygon.size());
	float base = std::numeric_limits<float>::infinity();
	float top = -std::numeric_limits<float>::infinity();
	for (const Vec3 &p : polygon) {
		flat.push_back(Plane2{ vec::dot(p, u), vec::dot(p, v) });
		const float h = vec::dot(p, up);
		base = std::min(base, h);
		top = std::max(top, h);
	}
	top += height;
	const Plane2 q{ vec::dot(p_point, u), vec::dot(p_point, v) };
	float horizontal = 0.0f;
	if (!inside_polygon(q, flat)) {
		horizontal = std::numeric_limits<float>::infinity();
		for (size_t i = 0, j = flat.size() - 1; i < flat.size(); j = i++) {
			horizontal = std::min(horizontal, segment_distance(q, flat[j], flat[i]));
		}
	}
	const float h = vec::dot(p_point, up);
	const float vertical = std::max({ 0.0f, base - h, h - top });
	return std::sqrt(horizontal * horizontal + vertical * vertical);
}

float AreaDef::proximity(const Vec3 &p_listener, float p_multiplier) const {
	const float p = area_fade(curve, shape.distance(p_listener), fade_width) * p_multiplier;
	return std::isfinite(p) ? p : 0.0f;
}

Result<SoundAreaData> read_sound_areas(const LogicWorld &p_world, const LogicGraph &p_graph) {
	Reader r{ p_world, p_graph, {}, {}, {} };
	return r.run();
}

}
