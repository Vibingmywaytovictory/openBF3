#include "bf3/host/logic_screen_effects.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace gp = core::gameplay;

namespace {

constexpr int32_t kFadeIn = gp::logic_port_id("FadeIn");
constexpr int32_t kFadeOut = gp::logic_port_id("FadeOut");
constexpr int32_t kFadeTime = gp::logic_port_id("FadeTime");
constexpr int32_t kEnable = gp::logic_port_id("Enable");
constexpr int32_t kDisable = gp::logic_port_id("Disable");
constexpr int32_t kVisibility = gp::logic_port_id("Visibility");
constexpr int32_t kStart = gp::logic_port_id("Start");
constexpr int32_t kStop = gp::logic_port_id("Stop");
constexpr int32_t kReset = gp::logic_port_id("Reset");
constexpr double kMaxFadeSeconds = 3600.0;

bool bool_of(const core::EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_instance.get(p_field);
	return v && v->as_bool().value_or(false);
}

uint32_t fade_ticks(double p_seconds) {
	return sim::logic::logic_delay_ticks(std::clamp(p_seconds, 0.0, kMaxFadeSeconds));
}

void ramp_to(LogicFadeRamp &r_ramp, float p_to, sim::Tick p_tick, uint32_t p_ticks) {
	r_ramp.from = r_ramp.at(p_tick);
	r_ramp.to = p_to;
	r_ramp.start = p_tick;
	r_ramp.ticks = p_ticks;
}

}

std::optional<core::EbxInstance> logic_node_instance(
		const core::EbxResolver &p_db, const gp::LogicGraph &p_graph, uint32_t p_node) {
	if (p_node >= p_graph.nodes().size()) {
		return std::nullopt;
	}
	const gp::LogicNode &n = p_graph.nodes()[p_node];
	if (n.scope >= p_graph.scopes().size()) {
		return std::nullopt;
	}
	const core::EbxPartition *part = p_db.partition_by_name(p_graph.text(p_graph.scopes()[n.scope].name));
	if (part == nullptr) {
		return std::nullopt;
	}
	return part->instance_by_guid(n.guid);
}

std::optional<double> logic_node_float(const core::EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_instance.get(p_field);
	const std::optional<float> f = v ? v->as_f32() : std::nullopt;
	if (!f || !std::isfinite(*f)) {
		return std::nullopt;
	}
	return double(*f);
}

float LogicFadeRamp::at(sim::Tick p_tick) const {
	if (ticks == 0 || p_tick >= start + ticks) {
		return p_tick >= start ? to : from;
	}
	if (p_tick <= start) {
		return from;
	}
	return from + (to - from) * float(double(p_tick - start) / double(ticks));
}

const LogicEnvironmentState *LogicScreenEffectsFrame::environment(std::string_view p_visual_environment) const {
	for (const LogicEnvironmentState &e : environments) {
		if (e.enabled && e.visual_environment == p_visual_environment) {
			return &e;
		}
	}
	return nullptr;
}

core::Result<LogicScreenEffects> LogicScreenEffects::bind(
		const core::EbxResolver &p_db, const gp::LogicGraph &p_graph) {
	LogicScreenEffects out;
	for (const uint32_t n : p_graph.nodes_of_type(gp::LogicTypeId::Fade)) {
		const std::optional<core::EbxInstance> inst = logic_node_instance(p_db, p_graph, n);
		if (!inst) {
			continue;
		}
		Fade f;
		f.screen = bool_of(*inst, "FadeScreen");
		f.ui = bool_of(*inst, "FadeUI");
		std::optional<double> seconds = logic_node_float(*inst, "FadeTime");
		for (const gp::LogicEdge &e : p_graph.property_edges()) {
			if (e.target != n || e.target_port != kFadeTime || e.source >= p_graph.nodes().size() ||
					p_graph.nodes()[e.source].type != gp::LogicTypeId::Float) {
				continue;
			}
			const std::optional<core::EbxInstance> source = logic_node_instance(p_db, p_graph, e.source);
			if (const std::optional<double> v = source ? logic_node_float(*source, "DefaultValue") : std::nullopt) {
				seconds = v;
				++out.stats_.fade_times_from_properties;
			}
		}
		f.ticks = fade_ticks(seconds.value_or(0.0));
		out.fades_[n] = f;
	}
	for (const uint32_t n : p_graph.nodes_of_type(gp::LogicTypeId::LogicVisualEnvironment)) {
		const std::optional<core::EbxInstance> inst = logic_node_instance(p_db, p_graph, n);
		if (!inst) {
			continue;
		}
		Environment env;
		const std::optional<core::EbxInstance> blueprint = p_db.follow(inst->data(), "VisualEnvironment");
		const std::optional<std::string_view> name =
				blueprint ? p_db.name_of(blueprint->partition().guid()) : std::nullopt;
		env.visual_environment = std::string(name.value_or(""));
		env.visibility = float(logic_node_float(*inst, "Visibility").value_or(1.0));
		for (const gp::LogicEdge &e : p_graph.property_edges()) {
			if (e.target != n || e.target_port != kVisibility || e.source >= p_graph.nodes().size()) {
				continue;
			}
			if (p_graph.nodes()[e.source].type != gp::LogicTypeId::Sequence) {
				env.linked = e.source;
				env.linked_port = e.source_port;
				++out.stats_.linked_visibilities;
				continue;
			}
			const std::optional<core::EbxInstance> sequence = logic_node_instance(p_db, p_graph, e.source);
			if (!sequence) {
				continue;
			}
			core::Result<std::vector<gp::EorFadingKey>> track =
					gp::read_sequence_float_track(p_db, *sequence, e.source_port);
			if (!track) {
				return core::make_error("logic screen effects: visual environment " + env.visual_environment + ": " +
						track.error().message);
			}
			env.sequence = e.source;
			env.track = std::move(*track);
			++out.stats_.tracked_visibilities;
		}
		out.environments_[n] = std::move(env);
	}
	out.stats_.fades = out.fades_.size();
	out.stats_.environments = out.environments_.size();
	return out;
}

std::vector<std::pair<uint32_t, int32_t>> LogicScreenEffects::watch_ports() const {
	std::vector<std::pair<uint32_t, int32_t>> out;
	for (const auto &[node, fade] : fades_) {
		out.emplace_back(node, kFadeIn);
		out.emplace_back(node, kFadeOut);
	}
	for (const auto &[node, env] : environments_) {
		out.emplace_back(node, kEnable);
		out.emplace_back(node, kDisable);
		if (env.sequence != gp::kNoLogicIndex) {
			out.emplace_back(env.sequence, kStart);
			out.emplace_back(env.sequence, kStop);
			out.emplace_back(env.sequence, kReset);
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::optional<uint32_t> LogicScreenEffects::event(uint32_t p_node, int32_t p_port, sim::Tick p_tick) {
	if (const auto f = fades_.find(p_node); f != fades_.end() && (p_port == kFadeIn || p_port == kFadeOut)) {
		const float to = p_port == kFadeOut ? 1.0f : 0.0f;
		if (f->second.screen) {
			ramp_to(screen_, to, p_tick, f->second.ticks);
		}
		if (f->second.ui) {
			ramp_to(ui_, to, p_tick, f->second.ticks);
		}
		++stats_.fade_events;
		return f->second.ticks;
	}
	if (const auto e = environments_.find(p_node);
			e != environments_.end() && (p_port == kEnable || p_port == kDisable)) {
		e->second.enabled = p_port == kEnable;
		++stats_.environment_events;
	}
	for (const auto &[node, env] : environments_) {
		if (env.sequence != p_node) {
			continue;
		}
		if (p_port == kStart) {
			sequence_starts_[p_node] = p_tick;
			++stats_.sequence_starts;
		} else if (p_port == kStop || p_port == kReset) {
			sequence_starts_.erase(p_node);
		}
		break;
	}
	return std::nullopt;
}

void LogicScreenEffects::reset() {
	screen_ = LogicFadeRamp{};
	ui_ = LogicFadeRamp{};
	sequence_starts_.clear();
	for (auto &[node, env] : environments_) {
		env.enabled = false;
	}
}

LogicScreenEffectsFrame LogicScreenEffects::frame(sim::Tick p_tick, const sim::logic::LogicRuntime *p_runtime) const {
	LogicScreenEffectsFrame out;
	out.screen_faded = screen_.at(p_tick);
	out.ui_faded = ui_.at(p_tick);
	for (const auto &[node, env] : environments_) {
		LogicEnvironmentState s;
		s.node = node;
		s.visual_environment = env.visual_environment;
		s.enabled = env.enabled;
		s.visibility = env.visibility;
		if (const auto started = sequence_starts_.find(env.sequence); started != sequence_starts_.end()) {
			const sim::Tick since = p_tick >= started->second ? p_tick - started->second : 0;
			s.visibility = gp::eor_fading_key_at(env.track, float(double(since) / double(sim::kSimHz)));
			s.source = LogicVisibilitySource::Sequence;
		} else if (env.linked != gp::kNoLogicIndex) {
			const std::optional<gp::LogicValue> v =
					p_runtime != nullptr ? p_runtime->output(env.linked, env.linked_port) : std::nullopt;
			if (v && v->kind == gp::LogicValue::Kind::Float && std::isfinite(v->f)) {
				s.visibility = float(v->f);
				s.source = LogicVisibilitySource::Runtime;
			} else {
				s.source = LogicVisibilitySource::Unresolved;
			}
		}
		out.environments.push_back(std::move(s));
	}
	return out;
}

}
