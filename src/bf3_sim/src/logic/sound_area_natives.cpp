#include "bf3/sim/logic/sound_area_natives.h"

#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bf3::sim::logic {

namespace gp = core::gameplay;

namespace {

struct Source {
	uint32_t node = gp::kNoLogicIndex;
	int32_t port = 0;
};

struct MixerInputFeed {
	size_t input = 0;
	Source source;
};

struct MixerOutputFeed {
	size_t output = 0;
	int32_t port = 0;
	float published = 0.0f;
	bool sent = false;
};

struct EntityFeeds {
	uint32_t node = gp::kNoLogicIndex;
	uint32_t mixer = 0;
	std::vector<MixerInputFeed> inputs;
	std::vector<MixerOutputFeed> outputs;
};

std::optional<float> number(const std::optional<LogicValue> &p_v) {
	if (!p_v) {
		return std::nullopt;
	}
	switch (p_v->kind) {
		case LogicValue::Kind::Float:
			return std::isfinite(p_v->f) ? std::optional<float>(float(p_v->f)) : std::nullopt;
		case LogicValue::Kind::Int:
			return float(p_v->i);
		case LogicValue::Kind::Bool:
			return p_v->b ? 1.0f : 0.0f;
		default:
			return std::nullopt;
	}
}

LogicValue float_value(float p_v) {
	LogicValue v;
	v.kind = LogicValue::Kind::Float;
	v.f = double(p_v);
	return v;
}

}

struct SoundAreaNatives::State {
	std::shared_ptr<const LogicGraph> graph;
	gp::SoundAreaData data;
	std::vector<SoundAreaValue> values;
	std::vector<std::vector<Source>> multipliers;
	std::vector<EntityFeeds> entities;
	std::vector<std::vector<float>> inputs;
	std::vector<std::vector<float>> outputs;
	std::vector<std::vector<float>> registers;
	std::vector<std::vector<std::vector<float>>> contributions;
	bool listening = false;
	core::Vec3 listener{};
	SoundAreaNativeStats stats;
};

core::Result<SoundAreaNatives> SoundAreaNatives::create(
		const gp::LogicWorld &p_world, std::shared_ptr<const LogicGraph> p_graph) {
	if (p_graph == nullptr) {
		return core::make_error("sound area natives: no graph");
	}
	core::Result<gp::SoundAreaData> data = gp::read_sound_areas(p_world, *p_graph);
	if (!data) {
		return data.error();
	}
	auto s = std::make_unique<State>();
	s->graph = std::move(p_graph);
	s->data = std::move(*data);
	const LogicGraph &g = *s->graph;
	s->values.resize(s->data.areas.size());
	s->multipliers.resize(s->data.areas.size());
	for (size_t i = 0; i < s->data.areas.size(); ++i) {
		s->values[i].node = s->data.areas[i].node;
		s->values[i].multiplier = s->data.areas[i].proximity_multiplier;
	}
	for (const gp::LogicEdge &e : g.property_edges()) {
		for (size_t i = 0; i < s->data.areas.size(); ++i) {
			const gp::AreaDef &a = s->data.areas[i];
			if (e.target == a.node && a.type == gp::LogicTypeId::SoundArea &&
					e.target_port == gp::kSoundAreaProximityMultiplier) {
				s->multipliers[i].push_back(Source{ e.source, e.source_port });
			}
		}
	}
	for (const gp::MixerEntityDef &m : s->data.mixer_entities) {
		const core::audio::MixerAsset &asset = s->data.mixers[m.mixer];
		EntityFeeds f;
		f.node = m.node;
		f.mixer = m.mixer;
		for (const gp::LogicEdge &e : g.property_edges()) {
			if (e.target != m.node) {
				continue;
			}
			if (const std::optional<size_t> in = asset.input_of(uint32_t(e.target_port))) {
				f.inputs.push_back(MixerInputFeed{ *in, Source{ e.source, e.source_port } });
			}
		}
		for (const gp::LogicEdge &e : g.properties_from(m.node)) {
			const std::optional<size_t> out = asset.output_of(uint32_t(e.source_port));
			if (out && std::none_of(f.outputs.begin(), f.outputs.end(), [&](const MixerOutputFeed &o) {
					return o.port == e.source_port;
				})) {
				f.outputs.push_back(MixerOutputFeed{ *out, e.source_port });
			}
		}
		s->entities.push_back(std::move(f));
	}
	for (const core::audio::MixerAsset &asset : s->data.mixers) {
		s->inputs.emplace_back(asset.inputs.size(), 0.0f);
		s->outputs.emplace_back(asset.outputs.size(), 0.0f);
		s->registers.emplace_back(asset.registers, 0.0f);
		s->contributions.emplace_back(asset.inputs.size());
	}
	s->stats.areas = s->data.areas.size();
	s->stats.mixers = s->data.mixers.size();
	s->stats.mixer_entities = s->data.mixer_entities.size();
	s->stats.unshaped = s->data.unshaped;
	SoundAreaNatives out(std::move(s));
	out.reset();
	return out;
}

SoundAreaNatives::SoundAreaNatives(std::unique_ptr<State> p_state) : state_(std::move(p_state)) {}
SoundAreaNatives::SoundAreaNatives(SoundAreaNatives &&) noexcept = default;
SoundAreaNatives &SoundAreaNatives::operator=(SoundAreaNatives &&) noexcept = default;
SoundAreaNatives::~SoundAreaNatives() = default;

void SoundAreaNatives::reset() {
	State &s = *state_;
	for (size_t i = 0; i < s.values.size(); ++i) {
		s.values[i].proximity = 0.0f;
		s.values[i].multiplier = s.data.areas[i].proximity_multiplier;
		s.values[i].published = false;
	}
	for (EntityFeeds &f : s.entities) {
		for (MixerOutputFeed &o : f.outputs) {
			o.sent = false;
		}
	}
	for (size_t m = 0; m < s.data.mixers.size(); ++m) {
		for (size_t i = 0; i < s.inputs[m].size(); ++i) {
			s.inputs[m][i] = s.data.mixers[m].inputs[i].default_value;
		}
		s.data.mixers[m].evaluate(s.inputs[m], s.registers[m], s.outputs[m]);
	}
}

core::Status SoundAreaNatives::listen(const core::Vec3 &p_listener, LogicRuntime &r_runtime) {
	State &s = *state_;
	if (!core::finite(p_listener)) {
		return core::make_error("sound area natives: the listener is not finite");
	}
	s.listening = true;
	s.listener = p_listener;
	++s.stats.listens;
	for (size_t i = 0; i < s.data.areas.size(); ++i) {
		const gp::AreaDef &a = s.data.areas[i];
		SoundAreaValue &v = s.values[i];
		for (const Source &src : s.multipliers[i]) {
			if (const std::optional<float> m = number(r_runtime.output(src.node, src.port))) {
				v.multiplier = *m;
			}
		}
		const float p = a.proximity(p_listener, v.multiplier);
		if (v.published && p == v.proximity) {
			continue;
		}
		BF3_TRY(r_runtime.force_property(a.node, a.out_port, float_value(p)));
		v.proximity = p;
		v.published = true;
		++s.stats.area_publishes;
	}
	return core::Status::success();
}

core::Status SoundAreaNatives::settle(LogicRuntime &r_runtime) {
	State &s = *state_;
	++s.stats.settles;
	for (std::vector<std::vector<float>> &per : s.contributions) {
		for (std::vector<float> &c : per) {
			c.clear();
		}
	}
	for (const EntityFeeds &f : s.entities) {
		for (const MixerInputFeed &in : f.inputs) {
			if (const std::optional<float> v = number(r_runtime.output(in.source.node, in.source.port))) {
				s.contributions[f.mixer][in.input].push_back(*v);
			}
		}
	}
	for (size_t m = 0; m < s.data.mixers.size(); ++m) {
		const core::audio::MixerAsset &asset = s.data.mixers[m];
		for (size_t i = 0; i < asset.inputs.size(); ++i) {
			s.inputs[m][i] = asset.accumulate(i, s.contributions[m][i]);
		}
		asset.evaluate(s.inputs[m], s.registers[m], s.outputs[m]);
	}
	for (EntityFeeds &f : s.entities) {
		for (MixerOutputFeed &o : f.outputs) {
			const float v = s.outputs[f.mixer][o.output];
			if (o.sent && v == o.published) {
				continue;
			}
			BF3_TRY(r_runtime.force_property(f.node, o.port, float_value(v)));
			o.published = v;
			o.sent = true;
			++s.stats.mixer_publishes;
		}
	}
	return core::Status::success();
}

bool SoundAreaNatives::listening() const {
	return state_->listening;
}

core::Vec3 SoundAreaNatives::listener() const {
	return state_->listener;
}

std::optional<float> SoundAreaNatives::mixer_value(uint32_t p_output_hash) const {
	const State &s = *state_;
	for (size_t m = 0; m < s.data.mixers.size(); ++m) {
		if (const std::optional<size_t> o = s.data.mixers[m].output_of(p_output_hash)) {
			return s.outputs[m][*o];
		}
	}
	return std::nullopt;
}

float SoundAreaNatives::area_type() const {
	return mixer_value(kMixerAreaType).value_or(0.0f);
}

std::span<const SoundAreaValue> SoundAreaNatives::areas() const {
	return state_->values;
}

std::span<const float> SoundAreaNatives::mixer_inputs(size_t p_mixer) const {
	return p_mixer < state_->inputs.size() ? std::span<const float>(state_->inputs[p_mixer]) : std::span<const float>();
}

std::span<const float> SoundAreaNatives::mixer_outputs(size_t p_mixer) const {
	return p_mixer < state_->outputs.size() ? std::span<const float>(state_->outputs[p_mixer])
											: std::span<const float>();
}

const gp::SoundAreaData &SoundAreaNatives::data() const {
	return state_->data;
}

const SoundAreaNativeStats &SoundAreaNatives::stats() const {
	return state_->stats;
}

}
