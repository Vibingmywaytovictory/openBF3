// PT1 rld: the weapon foley of a kit patch, read from its sound graph (weapon_foley.h; docs/design/pt1-rld.md).
#include "bf3/audio/weapon_foley.h"

#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/ebx.h"
#include "bf3/core/audio/sound_types.h"

#include <algorithm>
#include <cstdio>

namespace bf3::audio {

namespace ca = core::audio;
namespace cp = core::audio::port;

namespace {

// One unit of the dataflow: a Receive / MixerReceive entry on its own (each copies one input), every other node
// together with its child records (a MultiCrossfader's groups write the Start pulses its own Start port fires).
struct Unit {
	const ca::SoundNode *node = nullptr; // the parent (nullptr for a Receive entry: `entry` alone)
	uint32_t node_index = 0; // index into SoundProgram::nodes (the parent's)
	const ca::SoundNode *entry = nullptr; // Receive / MixerReceive entry
};

bool port_ok(const ca::SoundProgram &p_p, const ca::SoundNode &p_n, uint16_t p_i) {
	return p_i < p_n.ports_count && size_t(p_n.ports_begin) + p_i < p_p.ports.size();
}

uint16_t port_reg(const ca::SoundProgram &p_p, const ca::SoundNode &p_n, uint16_t p_i) {
	if (!port_ok(p_p, p_n, p_i)) {
		return ca::kNoRegister;
	}
	const ca::SoundPort &port = p_p.ports[p_n.ports_begin + p_i];
	return port.connected() && port.reg < p_p.registers ? port.reg : ca::kNoRegister;
}

float node_float(const ca::SoundProgram &p_p, const ca::SoundNode &p_n, uint16_t p_i, float p_default) {
	if (p_i >= p_n.floats_count || size_t(p_n.floats_begin) + p_i >= p_p.floats.size()) {
		return p_default;
	}
	return p_p.floats[p_n.floats_begin + p_i];
}

const ca::SoundNode *child_of(const ca::SoundProgram &p_p, const ca::SoundNode &p_n, uint16_t p_c) {
	const size_t at = size_t(p_n.children_begin) + p_c;
	if (p_c >= p_n.children_count || at >= p_p.children.size() || p_p.children[at] >= p_p.child_nodes.size()) {
		return nullptr;
	}
	return &p_p.child_nodes[p_p.children[at]];
}

// The registers a record reads and writes (audio ports excluded: register 0 carries no value).
void record_regs(const ca::SoundProgram &p_p, const ca::SoundNode &p_n, std::vector<uint16_t> &r_reads, std::vector<uint16_t> &r_writes) {
	const uint32_t outs = ca::port::output_ports(p_n.type);
	const uint32_t audio = ca::port::audio_ports(p_n.type);
	for (uint16_t i = 0; i < p_n.ports_count && i < 32; ++i) {
		if ((audio >> i) & 1u) {
			continue;
		}
		const uint16_t r = port_reg(p_p, p_n, i);
		if (r == ca::kNoRegister) {
			continue;
		}
		((outs >> i) & 1u ? r_writes : r_reads).push_back(r);
	}
}

struct Graph {
	std::vector<Unit> units;
	std::vector<std::vector<uint16_t>> reads, writes; // per unit
	std::vector<std::vector<uint32_t>> readers; // per register: the units that read it
	uint16_t distance_reg = ca::kNoRegister; // the Physics node's Distance output
};

Graph build_graph(const ca::SoundProgram &p_p) {
	Graph g;
	for (uint32_t ni = 0; ni < p_p.nodes.size(); ++ni) {
		const ca::SoundNode &n = p_p.nodes[ni];
		if (n.type == ca::SoundNodeType::Receive || n.type == ca::SoundNodeType::MixerReceive) {
			for (uint16_t c = 0; c < n.children_count; ++c) {
				const ca::SoundNode *e = child_of(p_p, n, c);
				if (e == nullptr) {
					continue;
				}
				Unit u;
				u.node_index = ni;
				u.entry = e;
				g.units.push_back(u);
				g.reads.emplace_back();
				g.writes.emplace_back();
				record_regs(p_p, *e, g.reads.back(), g.writes.back());
			}
			continue;
		}
		Unit u;
		u.node = &n;
		u.node_index = ni;
		g.units.push_back(u);
		g.reads.emplace_back();
		g.writes.emplace_back();
		record_regs(p_p, n, g.reads.back(), g.writes.back());
		for (uint16_t c = 0; c < n.children_count; ++c) {
			const ca::SoundNode *e = child_of(p_p, n, c);
			if (e != nullptr) {
				record_regs(p_p, *e, g.reads.back(), g.writes.back());
			}
		}
		if (n.type == ca::SoundNodeType::Physics) {
			g.distance_reg = port_reg(p_p, n, cp::PhysDistance);
		}
	}
	g.readers.resize(p_p.registers);
	for (uint32_t u = 0; u < g.units.size(); ++u) {
		for (uint16_t r : g.reads[u]) {
			if (r < g.readers.size() && (g.readers[r].empty() || g.readers[r].back() != u)) {
				g.readers[r].push_back(u);
			}
		}
	}
	return g;
}

const ca::SoundInput *input_of(const ca::SoundProgram &p_p, uint32_t p_hash, bool p_event) {
	for (const ca::SoundInput &in : p_p.inputs) {
		if (in.name_hash == p_hash && in.event == p_event && in.reg < p_p.registers) {
			return &in;
		}
	}
	return nullptr;
}

// The register a Receive entry copies p_reg into (kNoRegister: none).
std::vector<uint16_t> receive_outs(const ca::SoundProgram &p_p, const Graph &p_g, uint16_t p_reg) {
	std::vector<uint16_t> out;
	for (uint32_t u = 0; u < p_g.units.size(); ++u) {
		const Unit &unit = p_g.units[u];
		if (unit.entry == nullptr || unit.entry->type != ca::SoundNodeType::ReceiveEntry) {
			continue;
		}
		if (port_reg(p_p, *unit.entry, cp::ReceiveSource) == p_reg) {
			const uint16_t o = port_reg(p_p, *unit.entry, cp::ReceiveOut);
			if (o != ca::kNoRegister) {
				out.push_back(o);
			}
		}
	}
	return out;
}

std::string wave_name(const ca::SoundProgram &p_p, uint16_t p_ref) {
	if (p_ref >= p_p.waves.size()) {
		return "(none)";
	}
	const std::string &n = p_p.waves[p_ref].name;
	const size_t slash = n.find_last_of('/');
	return slash == std::string::npos ? n : n.substr(slash + 1);
}

// The waves of the WaveSwitcher that writes p_wave_reg and whose Index reads a register copied from the parameter
// p_param (empty when there is none).
std::vector<std::string> switcher_waves(const ca::SoundProgram &p_p, const Graph &p_g, uint32_t p_param) {
	const ca::SoundInput *in = input_of(p_p, p_param, false);
	if (in == nullptr) {
		return {};
	}
	const std::vector<uint16_t> outs = receive_outs(p_p, p_g, in->reg);
	for (const ca::SoundNode &n : p_p.nodes) {
		if (n.type != ca::SoundNodeType::WaveSwitcher) {
			continue;
		}
		const uint16_t idx = port_reg(p_p, n, cp::WsIndex);
		if (idx == ca::kNoRegister || std::find(outs.begin(), outs.end(), idx) == outs.end()) {
			continue;
		}
		std::vector<std::string> names;
		for (uint16_t w = 0; w < n.waves_count && size_t(n.waves_begin) + w < p_p.wave_refs.size(); ++w) {
			names.push_back(wave_name(p_p, p_p.wave_refs[n.waves_begin + w]));
		}
		return names;
	}
	return {};
}

// An output's OutputName from its hash (the names the weapon patches use), else "#hash".
std::string output_name(uint32_t p_hash) {
	for (const char *n : { "Cone", "Flat", "Handling", "LFE", "Direct", "Interior" }) {
		if (core::ebx_name_hash(n) == p_hash) {
			return n;
		}
	}
	return "#" + std::to_string(p_hash);
}

} // namespace

const char *foley_action_name(FoleyAction p_action) {
	switch (p_action) {
		case FoleyAction::Deploy:
			return "deploy";
		case FoleyAction::Zoom:
			return "zoom";
		case FoleyAction::DryFire:
			return "dry fire";
		case FoleyAction::BipodDeploy:
			return "bipod deploy";
		case FoleyAction::BipodUndeploy:
			return "bipod undeploy";
		case FoleyAction::Light:
			return "light";
		case FoleyAction::FireMode:
			return "fire mode";
	}
	return "?";
}

std::vector<uint16_t> voice_outputs(const ca::SoundProgram &p_p, uint16_t p_voice) {
	std::vector<uint16_t> out;
	if (p_voice >= p_p.voices.size()) {
		return out;
	}
	// Every voice reachable from p_voice through its sends (each voice visited once).
	std::vector<uint8_t> seen(p_p.voices.size(), 0);
	std::vector<uint16_t> stack{ p_voice };
	seen[p_voice] = 1;
	while (!stack.empty()) {
		const uint16_t v = stack.back();
		stack.pop_back();
		const ca::SoundVoice &voice = p_p.voices[v];
		for (uint16_t k = 0; k < voice.plugins_count && size_t(voice.plugins_begin) + k < p_p.plugins.size(); ++k) {
			const uint16_t c = p_p.plugins[voice.plugins_begin + k].connection;
			if (c == ca::kNoRegister || c >= p_p.connections.size() || p_p.connections[c].to_bus) {
				continue;
			}
			const uint16_t to = p_p.connections[c].voice;
			if (to < seen.size() && !seen[to]) {
				seen[to] = 1;
				stack.push_back(to);
			}
		}
	}
	for (uint16_t o = 0; o < p_p.outputs.size(); ++o) {
		const ca::SoundOutput &so = p_p.outputs[o];
		const uint16_t v = so.main_send_plugin.valid() ? so.main_send_plugin.voice : so.low_pass.voice;
		if (v < seen.size() && seen[v]) {
			out.push_back(o);
		}
	}
	return out;
}

FoleyEvent trace_foley_event(const ca::SoundProgram &p_p, uint32_t p_hash) {
	FoleyEvent ev;
	ev.hash = p_hash;
	const ca::SoundInput *in = input_of(p_p, p_hash, true);
	if (in == nullptr) {
		return ev;
	}
	ev.present = true;
	ev.reg = in->reg;
	const Graph g = build_graph(p_p);
	// Forward over the registers from the event's own register: a unit that reads a reached register writes its
	// outputs (a Sampler only through its Trigger: its Finished pulse is the sampler's, not the event's).
	std::vector<uint8_t> reached(p_p.registers, 0), fired(g.units.size(), 0);
	std::vector<uint16_t> work{ in->reg };
	reached[in->reg] = 1;
	float reach = -1.0f;
	bool gated = false;
	while (!work.empty()) {
		const uint16_t r = work.back();
		work.pop_back();
		for (uint32_t u : g.readers[r]) {
			if (fired[u]) {
				continue;
			}
			const Unit &unit = g.units[u];
			const ca::SoundNode &n = unit.entry != nullptr ? *unit.entry : *unit.node;
			if (n.type == ca::SoundNodeType::Sampler) {
				const uint16_t trig = port_reg(p_p, n, cp::SmpTrigger);
				if (trig == ca::kNoRegister || !reached[trig]) {
					continue;
				}
				ev.samplers.push_back(unit.node_index);
			}
			if (n.type == ca::SoundNodeType::MultiCrossfader) {
				const uint16_t start = port_reg(p_p, n, cp::McfStart);
				const uint16_t control = port_reg(p_p, n, cp::McfControl);
				if (start != ca::kNoRegister && reached[start] && control != ca::kNoRegister && control == g.distance_reg) {
					for (uint16_t c = 0; c < n.children_count; ++c) {
						if (const ca::SoundNode *grp = child_of(p_p, n, c)) {
							reach = std::max(reach, node_float(p_p, *grp, cp::GroupFadeEnd, 0.0f));
							gated = true;
						}
					}
				}
			}
			fired[u] = 1;
			for (uint16_t w : g.writes[u]) {
				if (w < reached.size() && !reached[w]) {
					reached[w] = 1;
					work.push_back(w);
				}
			}
		}
	}
	std::sort(ev.samplers.begin(), ev.samplers.end());
	ev.reach_m = gated ? reach : kFoleyUngated;
	// The release: an input event copied straight into a reached sampler's Release port.
	for (uint32_t s : ev.samplers) {
		const ca::SoundNode &n = p_p.nodes[s];
		const uint16_t rel = port_reg(p_p, n, cp::SmpRelease);
		if (rel == ca::kNoRegister || ev.release_event != 0) {
			continue;
		}
		for (const ca::SoundInput &other : p_p.inputs) {
			if (!other.event || other.name_hash == p_hash || other.reg >= p_p.registers) {
				continue;
			}
			const std::vector<uint16_t> outs = receive_outs(p_p, g, other.reg);
			if (std::find(outs.begin(), outs.end(), rel) != outs.end()) {
				ev.release_event = other.name_hash;
				break;
			}
		}
	}
	// The outputs the samplers' voices feed (SndPlayer of every slot: 4 plugin refs per slot).
	for (uint32_t s : ev.samplers) {
		const ca::SoundNode &n = p_p.nodes[s];
		for (uint16_t k = 0; k < n.plugins_count; k += ca::port::kSamplerSlotPlugins) {
			if (size_t(n.plugins_begin) + k >= p_p.plugin_refs.size()) {
				break;
			}
			const ca::SoundPluginRef ref = p_p.plugin_refs[n.plugins_begin + k];
			if (!ref.valid()) {
				continue;
			}
			for (uint16_t o : voice_outputs(p_p, ref.voice)) {
				if (std::find(ev.outputs.begin(), ev.outputs.end(), o) == ev.outputs.end()) {
					ev.outputs.push_back(o);
				}
			}
		}
	}
	std::sort(ev.outputs.begin(), ev.outputs.end());
	return ev;
}

WeaponFoley read_weapon_foley(const ca::SoundProgram &p_p) {
	WeaponFoley f;
	f.reload = trace_foley_event(p_p, kHashReload);
	f.action = trace_foley_event(p_p, kHashAction);
	f.bolt = trace_foley_event(p_p, kHashBolt);
	f.short_reload_param = input_of(p_p, kHashIsShortReload, false) != nullptr;
	f.action_type_param = input_of(p_p, kHashActionType, false) != nullptr;
	const Graph g = build_graph(p_p);
	f.action_waves = switcher_waves(p_p, g, kHashActionType);
	f.reload_waves = switcher_waves(p_p, g, kHashIsShortReload);
	return f;
}

std::string describe_weapon_foley(const ca::SoundProgram &p_p, const WeaponFoley &p_f) {
	const auto one = [&](const char *p_name, const FoleyEvent &p_e) {
		if (!p_e.present) {
			return std::string(p_name) + " -";
		}
		char buf[160];
		if (p_e.reach_m == kFoleyUngated) {
			std::snprintf(buf, sizeof(buf), "%s ungated, %zu sampler(s)", p_name, p_e.samplers.size());
		} else {
			std::snprintf(buf, sizeof(buf), "%s <= %.0f m, %zu sampler(s)", p_name, double(p_e.reach_m), p_e.samplers.size());
		}
		std::string s = buf;
		if (p_e.release_event != 0) {
			s += ", release #" + std::to_string(p_e.release_event);
		}
		for (uint16_t o : p_e.outputs) {
			s += o == p_e.outputs.front() ? ", to " : " + ";
			s += o < p_p.outputs.size() ? output_name(p_p.outputs[o].name_hash) : std::string("?");
		}
		return s;
	};
	std::string s = one("Reload", p_f.reload) + "; " + one("Action", p_f.action) + "; " + one("Bolt", p_f.bolt);
	if (!p_f.reload_waves.empty()) {
		s += "; IsShortReload [";
		for (size_t i = 0; i < p_f.reload_waves.size(); ++i) {
			s += (i ? ", " : "") + p_f.reload_waves[i];
		}
		s += "]";
	}
	if (!p_f.action_waves.empty()) {
		s += "; ActionType [";
		for (size_t i = 0; i < p_f.action_waves.size(); ++i) {
			s += (i ? ", " : "") + p_f.action_waves[i];
		}
		s += "]";
	}
	return s;
}

} // namespace bf3::audio
