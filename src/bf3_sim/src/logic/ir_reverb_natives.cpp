#include "bf3/sim/logic/ir_reverb_natives.h"

#include "bf3/sim/logic/sound_area_natives.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <utility>

namespace bf3::sim::logic {

namespace gp = core::gameplay;

namespace {

core::Error ir_error(const std::string &p_what) {
	return core::make_error("ir reverbs: " + p_what);
}

}

float IrReverbDef::amplitude(float p_volume) const {
	const float v = std::isfinite(p_volume) ? std::clamp(p_volume, 0.0f, 1.0f) : 0.0f;
	const float a = core::level::area_fade(curve, 1.0f - v, 1.0f) * std::pow(10.0f, gain_db / 20.0f);
	return std::isfinite(a) ? a : 0.0f;
}

struct IrReverbNatives::State {
	std::shared_ptr<const LogicGraph> graph;
	std::vector<IrReverbDef> zones;
	std::vector<IrReverbValue> values;
	bool updated = false;
	IrReverbNativeStats stats;
};

core::Result<IrReverbNatives> IrReverbNatives::create(
		const gp::LogicWorld &p_world, std::shared_ptr<const LogicGraph> p_graph) {
	if (p_graph == nullptr) {
		return ir_error("no graph");
	}
	const LogicGraph &g = *p_graph;
	if (g.scopes().size() != p_world.scopes().size()) {
		return ir_error("the graph has " + std::to_string(g.scopes().size()) + " scopes, the world " +
				std::to_string(p_world.scopes().size()));
	}
	std::map<std::pair<uint32_t, core::Guid>, uint32_t> objects;
	for (uint32_t i = 0; i < p_world.objects().size(); ++i) {
		objects.emplace(std::make_pair(p_world.objects()[i].scope, p_world.objects()[i].entity.guid()), i);
	}
	auto s = std::make_unique<State>();
	for (const uint32_t n : g.nodes_of_type(gp::LogicTypeId::IrReverb)) {
		const gp::LogicNode &node = g.nodes()[n];
		const auto it = objects.find({ node.scope, node.guid });
		if (it == objects.end()) {
			return ir_error("node " + std::to_string(n) + " has no world object");
		}
		const core::EbxInstance &inst = p_world.objects()[it->second].entity;
		IrReverbDef d;
		d.node = n;
		const std::optional<gp::PropertyValue> gain = p_world.effective(node.scope, inst, "Gain");
		d.gain_db = float(gain ? gain->as_f64().value_or(0.0) : 0.0);
		const std::optional<gp::PropertyValue> volume = p_world.effective(node.scope, inst, "Volume");
		d.volume = float(volume ? volume->as_f64().value_or(0.0) : 0.0);
		const std::optional<gp::PropertyValue> curve = p_world.effective(node.scope, inst, "FadeCurve");
		const std::optional<std::string_view> curve_name = curve ? curve->enum_name() : std::nullopt;
		const std::optional<core::level::AreaFadeCurve> c =
				core::level::parse_area_fade_curve(curve_name.value_or(std::string_view()));
		if (!c) {
			return ir_error(
					"node " + std::to_string(n) + " has the fade curve '" + std::string(curve_name.value_or("")) + "'");
		}
		d.curve = *c;
		if (!std::isfinite(d.gain_db) || !std::isfinite(d.volume)) {
			return ir_error("node " + std::to_string(n) + " has a non-finite Gain or Volume");
		}
		const std::optional<core::EbxValue> field = inst.get("ImpulseResponse");
		const std::optional<core::EbxRef> ref = field ? field->as_ref() : std::nullopt;
		const std::optional<core::EbxInstance> asset =
				ref && !ref->is_null() ? p_world.db().resolve(inst.partition(), *ref) : std::nullopt;
		if (asset && asset->is_a("ImpulseResponseAsset")) {
			d.impulse = asset->partition().guid();
			const std::optional<core::EbxValue> name = asset->get("Name");
			d.impulse_name = std::string(name ? name->as_str().value_or("") : std::string_view());
		} else {
			++s->stats.without_impulse;
		}
		for (const gp::LogicEdge &e : g.property_edges()) {
			if (e.target == n && e.target_port == kIrReverbVolume) {
				d.sources.push_back(IrReverbSource{ e.source, e.source_port });
			}
		}
		s->stats.unfed += d.sources.empty() ? 1 : 0;
		s->zones.push_back(std::move(d));
	}
	s->values.resize(s->zones.size());
	s->stats.zones = s->zones.size();
	s->graph = std::move(p_graph);
	IrReverbNatives out(std::move(s));
	out.reset();
	return out;
}

IrReverbNatives::IrReverbNatives(std::unique_ptr<State> p_state) : state_(std::move(p_state)) {}
IrReverbNatives::IrReverbNatives(IrReverbNatives &&) noexcept = default;
IrReverbNatives &IrReverbNatives::operator=(IrReverbNatives &&) noexcept = default;
IrReverbNatives::~IrReverbNatives() = default;

void IrReverbNatives::reset() {
	State &s = *state_;
	for (size_t i = 0; i < s.zones.size(); ++i) {
		s.values[i].volume = s.zones[i].volume;
		s.values[i].amplitude = s.zones[i].amplitude(s.zones[i].volume);
	}
	s.updated = false;
}

void IrReverbNatives::update(const LogicRuntime &p_runtime) {
	State &s = *state_;
	++s.stats.updates;
	for (size_t i = 0; i < s.zones.size(); ++i) {
		const IrReverbDef &d = s.zones[i];
		std::optional<float> volume;
		for (const IrReverbSource &src : d.sources) {
			if (const std::optional<float> v = logic_number(p_runtime.output(src.node, src.port))) {
				volume = std::max(volume.value_or(*v), *v);
			}
		}
		s.values[i].volume = volume.value_or(d.volume);
		s.values[i].amplitude = d.amplitude(s.values[i].volume);
	}
	s.updated = true;
}

bool IrReverbNatives::updated() const {
	return state_->updated;
}

std::span<const IrReverbDef> IrReverbNatives::zones() const {
	return state_->zones;
}

std::span<const IrReverbValue> IrReverbNatives::values() const {
	return state_->values;
}

const IrReverbNativeStats &IrReverbNatives::stats() const {
	return state_->stats;
}

}
