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

	Result<std::optional<AreaVolume>> shape(uint32_t p_shape_node) {
		const std::optional<EbxInstance> inst = instance(p_shape_node);
		if (!inst) {
			return std::optional<AreaVolume>();
		}
		level::LevelTransform frame = scope_transform(graph.nodes()[p_shape_node].scope);
		const std::optional<EbxValue> own = inst->get("Transform");
		if (const std::optional<level::Rows> rows = own ? own->as_transform() : std::nullopt) {
			frame = level::LevelTransform::from_rows(*rows).in_parent(frame);
		}
		Result<AreaVolume> v = level::read_area_volume(*inst, frame);
		if (!v) {
			return error(v.error().message);
		}
		out.tilted_normals += v->tilted ? 1 : 0;
		out.open_paths += v->open ? 1 : 0;
		out.curved_paths += v->curved ? 1 : 0;
		return std::optional<AreaVolume>(std::move(*v));
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
			Result<std::optional<AreaVolume>> s = shape(e.target);
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

float AreaDef::proximity(const Vec3 &p_listener, float p_multiplier) const {
	const float p = area_fade(curve, shape.distance(p_listener), fade_width) * p_multiplier;
	return std::isfinite(p) ? p : 0.0f;
}

Result<SoundAreaData> read_sound_areas(const LogicWorld &p_world, const LogicGraph &p_graph) {
	Reader r{ p_world, p_graph, {}, {}, {} };
	return r.run();
}

}
