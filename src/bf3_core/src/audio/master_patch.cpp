#include "bf3/core/audio/master_patch.h"

#include "bf3/core/audio/hdr_setting.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace bf3::core::audio {

namespace {

constexpr std::array<SpeakerRole, 1> kMono{ SpeakerRole::Mono };
constexpr std::array<SpeakerRole, 2> kStereo{ SpeakerRole::L, SpeakerRole::R };
constexpr std::array<SpeakerRole, 4> kQuad{ SpeakerRole::L, SpeakerRole::R, SpeakerRole::Ls, SpeakerRole::Rs };
constexpr std::array<SpeakerRole, 6> kFive{ SpeakerRole::L, SpeakerRole::C, SpeakerRole::R, SpeakerRole::Ls,
	SpeakerRole::Rs, SpeakerRole::Lfe };
constexpr std::array<SpeakerRole, 8> kSeven{ SpeakerRole::L, SpeakerRole::C, SpeakerRole::R, SpeakerRole::Ls,
	SpeakerRole::Rs, SpeakerRole::Lfe, SpeakerRole::Lb, SpeakerRole::Rb };

constexpr size_t kMaxEvaluationPasses = 32;

Error master_error(const std::string &p_what) {
	return make_error("sound/master: " + p_what);
}

uint32_t djb2_xor(std::string_view p_text) {
	uint32_t h = 5381;
	for (const char c : p_text) {
		h = (h * 33u) ^ uint32_t(uint8_t(c));
	}
	return h;
}

struct Port {
	std::optional<uint32_t> reg;
	float constant = 0.0f;
};

Port read_port(const EbxStruct &p_node, std::string_view p_field) {
	Port p;
	const std::optional<EbxValue> v = p_node.get(p_field);
	const std::optional<EbxStruct> s = v ? v->as_struct() : std::nullopt;
	if (!s) {
		return p;
	}
	const bool connected = s->get("IsConnected") && s->get("IsConnected")->as_bool().value_or(false);
	if (connected && s->get("ValueIndex")) {
		p.reg = uint32_t(std::max<int64_t>(0, s->get("ValueIndex")->as_i64().value_or(0)));
	}
	p.constant = s->get("UnconnectedValue") ? s->get("UnconnectedValue")->as_f32().value_or(0.0f) : 0.0f;
	return p;
}

float read_float(const EbxStruct &p_s, std::string_view p_field, float p_default = 0.0f) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		return p_default;
	}
	if (const std::optional<float> f = v->as_f32()) {
		return *f;
	}
	return float(v->as_f64().value_or(p_default));
}

std::optional<EbxInstance> follow(const EbxResolver &p_db, const EbxPartition &p_part, const EbxValue &p_value) {
	const std::optional<EbxRef> r = p_value.as_ref();
	if (!r || r->is_null()) {
		return std::nullopt;
	}
	return r->kind == EbxRef::Kind::Internal ? p_part.resolve(*r) : p_db.resolve(p_part, *r);
}

struct MasterUnitPort {
	std::string_view port;
	std::string_view field;
	bool equalizer;
	bool compressor;
	bool decibels;
};

constexpr MasterUnitPort kMasterUnitPorts[] = {
	{ "MasterGain", "MasterVolume", false, false, true },
	{ "MainMixGain", "MainMixVolume", false, false, true },
	{ "ReverbGain", "ReverbVolume", false, false, true },
	{ "PostEffectsGain", "PostEffectsGain", false, false, true },
	{ "ParallelDistortionGain", "ParallelDistortionGain", false, false, true },
	{ "DistClipLevel", "DistortionClipLevel", false, false, true },
	{ "MasterLfeGain", "MasterLfeGain", false, false, true },
	{ "MasterDialogGain", "MasterDialogGain", false, false, true },
	{ "FadeTime", "FadeTime", false, false, false },
	{ "CompThreshold", "Threshold", false, true, false },
	{ "CompRatio", "Ratio", false, true, false },
	{ "CompAttack", "Attack", false, true, false },
	{ "CompRelease", "Release", false, true, false },
	{ "LowShelfFreq", "LowShelfFrequency", true, false, false },
	{ "LowShelfGain", "LowShelfGain", true, false, true },
	{ "HighShelfFreq", "HighShelfFrequency", true, false, false },
	{ "HighShelfGain", "HighShelfGain", true, false, true },
	{ "HighPassFreq", "HpCutoffFrequency", true, false, false },
};

struct Graph {
	const EbxResolver &db;
	const EbxPartition &part;
	std::vector<EbxInstance> nodes;
	std::vector<float> regs;
	std::vector<uint8_t> written;
	uint32_t preset_reg = UINT32_MAX;
	uint32_t speaker_reg = UINT32_MAX;
	uint32_t stereo_hash = djb2_xor("StereoMode");

	float value(const Port &p_port) const {
		return p_port.reg && *p_port.reg < regs.size() ? regs[*p_port.reg] : p_port.constant;
	}
	bool set(uint32_t p_reg, float p_value) {
		if (p_reg >= regs.size()) {
			return false;
		}
		const bool changed = !written[p_reg] || regs[p_reg] != p_value;
		regs[p_reg] = p_value;
		written[p_reg] = 1;
		return changed;
	}
};

Result<bool> evaluate_node(Graph &r_g, const EbxInstance &p_node, uint32_t p_preset, uint32_t p_stereo_mode,
		float p_low_pass_hz, uint32_t p_low_pass_reg) {
	const EbxStruct n = p_node.data();
	const std::string_view type = p_node.type_name();
	bool changed = false;
	const auto entries = [&](std::string_view p_field) {
		std::vector<EbxInstance> out;
		const std::optional<EbxValue> v = n.get(p_field);
		if (v && v->as_array()) {
			for (EbxValue e : *v->as_array()) {
				if (std::optional<EbxInstance> i = follow(r_g.db, r_g.part, e)) {
					out.push_back(*i);
				}
			}
		}
		return out;
	};
	if (type == "ReceiveNodeData") {
		for (const EbxInstance &e : entries("Entries")) {
			const Port out = read_port(e.data(), "Out");
			const std::optional<EbxValue> src = e.get("Source");
			const std::optional<EbxInstance> param = src ? follow(r_g.db, r_g.part, *src) : std::nullopt;
			if (!out.reg || !param) {
				continue;
			}
			float v = read_float(param->data(), "DefaultValue");
			const uint32_t hash = param->get("NameHash") ? uint32_t(param->get("NameHash")->as_u32().value_or(0)) : 0u;
			if (*out.reg == r_g.preset_reg) {
				v = float(p_preset);
			} else if (hash == r_g.stereo_hash) {
				v = float(p_stereo_mode);
			} else if (*out.reg == p_low_pass_reg) {
				v = p_low_pass_hz;
			}
			changed = r_g.set(*out.reg, v) || changed;
		}
		return changed;
	}
	if (type == "MixerReceiveNodeData") {
		for (const EbxInstance &e : entries("Entries")) {
			const Port out = read_port(e.data(), "Out");
			const std::optional<EbxValue> src = e.get("Source");
			const std::optional<EbxInstance> param = src ? follow(r_g.db, r_g.part, *src) : std::nullopt;
			if (!out.reg) {
				continue;
			}
			if (!param) {
				return master_error("a MixerReceive entry names no mixer output in the database");
			}
			changed = r_g.set(*out.reg, read_float(param->data(), "DefaultValue")) || changed;
		}
		return changed;
	}
	if (type == "MasterUnitNodeData") {
		const std::optional<EbxValue> settings = n.get("Settings");
		if (!settings || !settings->as_array() || settings->as_array()->empty()) {
			return master_error("MasterUnit has no Settings");
		}
		const size_t count = settings->as_array()->size();
		const size_t index = std::min<size_t>(
				size_t(std::max(0.0f, std::round(r_g.value(read_port(n, "SettingsIndex"))))), count - 1);
		const std::optional<EbxInstance> s = follow(r_g.db, r_g.part, *settings->as_array()->get(index));
		if (!s) {
			return master_error("MasterUnit Settings[" + std::to_string(index) + "] is not in the partition");
		}
		const std::optional<EbxValue> eq = s->get("Equalizer");
		const std::optional<EbxValue> comp = s->get("Compressor");
		const std::optional<EbxInstance> eqi = eq ? follow(r_g.db, r_g.part, *eq) : std::nullopt;
		const std::optional<EbxInstance> compi = comp ? follow(r_g.db, r_g.part, *comp) : std::nullopt;
		for (const MasterUnitPort &p : kMasterUnitPorts) {
			const Port port = read_port(n, p.port);
			if (!port.reg) {
				continue;
			}
			const std::optional<EbxInstance> &from = p.equalizer ? eqi : p.compressor ? compi : s;
			if (!from) {
				return master_error(
						std::string("MasterUnit ") + std::string(p.port) + ": its settings record is missing");
			}
			const float raw = read_float(from->data(), p.field);
			changed = r_g.set(*port.reg, p.decibels ? std::pow(10.0f, raw / 20.0f) : raw) || changed;
		}
		return changed;
	}
	if (type == "SimpleValueSelectorNodeData") {
		const Port out = read_port(n, "Out");
		const std::optional<EbxValue> values = n.get("OutValues");
		if (!out.reg || !values || !values->as_array() || values->as_array()->empty()) {
			return false;
		}
		const size_t count = values->as_array()->size();
		const size_t index =
				std::min<size_t>(size_t(std::max(0.0f, std::round(r_g.value(read_port(n, "Index"))))), count - 1);
		const std::optional<EbxValue> v = values->as_array()->get(index);
		return r_g.set(*out.reg, v ? v->as_f32().value_or(0.0f) : 0.0f);
	}
	if (type == "SimpleTransformNodeData") {
		const Port out = read_port(n, "Z");
		if (!out.reg) {
			return false;
		}
		const float x = r_g.value(read_port(n, "X")), y = r_g.value(read_port(n, "Y"));
		const std::string_view op =
				n.get("Operation") ? n.get("Operation")->enum_name().value_or("") : std::string_view();
		float z = 0.0f;
		if (op == "SimpleTransformOperation_Multiply") {
			z = x * y;
		} else if (op == "SimpleTransformOperation_Add") {
			z = x + y;
		} else if (op == "SimpleTransformOperation_Subtract") {
			z = x - y;
		} else {
			return master_error("SimpleTransform operation " + std::string(op) + " is not evaluated for the master");
		}
		return r_g.set(*out.reg, z);
	}
	if (type == "ScaleClampNodeData") {
		const Port out = read_port(n, "Out");
		if (!out.reg) {
			return false;
		}
		const float in = r_g.value(read_port(n, "In")), in_min = r_g.value(read_port(n, "InMin")),
					in_max = r_g.value(read_port(n, "InMax"));
		const float out_min = r_g.value(read_port(n, "OutMin")), out_max = r_g.value(read_port(n, "OutMax"));
		const float t = in_max != in_min ? std::clamp((in - in_min) / (in_max - in_min), 0.0f, 1.0f) : 0.0f;
		return r_g.set(*out.reg, out_min + (out_max - out_min) * t);
	}
	return false;
}

Result<MasterStepKind> step_kind(uint32_t p_id) {
	static const std::pair<uint32_t, MasterStepKind> kKinds[] = {
		{ fourcc("Sub0"), MasterStepKind::Pass },
		{ fourcc("Uma0"), MasterStepKind::Pass },
		{ fourcc("Dac0"), MasterStepKind::Pass },
		{ fourcc("Gai0"), MasterStepKind::Gain },
		{ fourcc("HI20"), MasterStepKind::HighPass },
		{ fourcc("LI20"), MasterStepKind::LowPass },
		{ fourcc("HS20"), MasterStepKind::HighShelf },
		{ fourcc("LS20"), MasterStepKind::LowShelf },
		{ fourcc("PI20"), MasterStepKind::Peaking },
		{ fourcc("Co10"), MasterStepKind::Compressor },
		{ fourcc("DCl0"), MasterStepKind::Clip },
		{ fourcc("Sen0"), MasterStepKind::Send },
		{ fourcc("Rou0"), MasterStepKind::Route },
		{ fourcc("MtP0"), MasterStepKind::MatrixPan },
		{ fourcc("RIR1"), MasterStepKind::Silence },
	};
	for (const auto &[id, kind] : kKinds) {
		if (id == p_id) {
			return kind;
		}
	}
	return master_error("plugin " + std::to_string(p_id) + " has no master evaluation");
}

int8_t role_index(std::span<const SpeakerRole> p_layout, SpeakerRole p_role) {
	for (size_t i = 0; i < p_layout.size(); ++i) {
		if (p_layout[i] == p_role) {
			return int8_t(i);
		}
	}
	return -1;
}

}

std::span<const SpeakerRole> speaker_layout(uint32_t p_channels) {
	switch (p_channels) {
		case 1:
			return kMono;
		case 2:
			return kStereo;
		case 4:
			return kQuad;
		case 6:
			return kFive;
		case 8:
			return kSeven;
		default:
			return {};
	}
}

const MasterMixPlan *MasterMixData::plan(uint32_t p_stereo_mode, uint32_t p_preset) const {
	const std::vector<MasterMixPlan> &v = plans[std::min<uint32_t>(p_stereo_mode, 1)];
	return p_preset < v.size() ? &v[p_preset] : nullptr;
}

Result<float> read_sound_state_low_pass(const EbxResolver &p_db, std::string_view p_state) {
	const EbxPartition *part = p_db.partition_by_name(kSoundStatesPartition);
	if (part == nullptr) {
		return make_error(std::string(kSoundStatesPartition) + " is not in the database");
	}
	for (const EbxInstance &i : part->instances_of("SoundState")) {
		if (i.get("Name") && i.get("Name")->as_str().value_or("") == p_state) {
			const float hz = read_float(i.data(), "LpCutoffFrequency");
			if (!std::isfinite(hz) || hz <= 0.0f) {
				return make_error("sound state " + std::string(p_state) + ": LpCutoffFrequency " + std::to_string(hz));
			}
			return hz;
		}
	}
	return make_error(std::string(kSoundStatesPartition) + " has no sound state " + std::string(p_state));
}

Result<MasterMixData> read_master_mix(
		const EbxResolver &p_db, float p_low_pass_hz, uint32_t p_default_preset, uint32_t p_default_stereo_mode) {
	const EbxPartition *part = p_db.partition_by_name(kMasterPatchPartition);
	if (part == nullptr) {
		return master_error("not in the database");
	}
	CompileOptions options;
	options.allow_unimplemented = true;
	gameplay::WorkBudget budget;
	Result<SoundProgram> compiled = compile_sound_patch(p_db, *part, options, budget);
	if (!compiled) {
		return compiled.error();
	}
	const SoundProgram &prog = *compiled;
	const EbxInstance primary = part->primary();
	if (!primary.is_a("SoundMasterPatchAsset")) {
		return master_error("the primary instance is " + std::string(primary.type_name()));
	}
	const std::optional<EbxValue> gv = primary.get("Graph");
	const std::optional<EbxInstance> graph = gv ? follow(p_db, *part, *gv) : std::nullopt;
	if (!graph) {
		return master_error("no SoundGraphData");
	}
	Graph g{ p_db, *part, {}, {}, {} };
	const std::optional<EbxValue> nodes = graph->get("Nodes");
	if (nodes && nodes->as_array()) {
		for (EbxValue nv : *nodes->as_array()) {
			if (std::optional<EbxInstance> n = follow(p_db, *part, nv)) {
				g.nodes.push_back(*n);
			}
		}
	}
	std::vector<MasterBus> voice_bus(prog.voices.size(), MasterBus::Unknown);
	uint32_t dac_speakers_reg = UINT32_MAX;
	float dac_speakers_constant = 2.0f;
	MasterMixData out;
	for (const EbxInstance &n : g.nodes) {
		if (n.type_name() == "MasterUnitNodeData") {
			const Port p = read_port(n.data(), "SettingsIndex");
			g.preset_reg = p.reg.value_or(UINT32_MAX);
			const std::optional<EbxValue> settings = n.get("Settings");
			if (settings && settings->as_array()) {
				for (EbxValue s : *settings->as_array()) {
					const std::optional<EbxInstance> si = follow(p_db, *part, s);
					out.presets.push_back(si && si->get("Name") ? std::string(si->get("Name")->as_str().value_or(""))
																: std::string());
				}
			}
		} else if (n.type_name() == "DacNodeData") {
			const Port p = read_port(n.data(), "SpeakerCount");
			dac_speakers_reg = p.reg.value_or(UINT32_MAX);
			dac_speakers_constant = p.constant;
		} else if (n.type_name() == "BusNodeData") {
			const std::optional<EbxValue> name = n.get("BusName");
			const std::optional<EbxValue> plugin = n.get("SubmixPlugin");
			const std::optional<EbxStruct> ps = plugin ? plugin->as_struct() : std::nullopt;
			const int64_t voice = ps && ps->get("VoiceIndex") ? ps->get("VoiceIndex")->as_i64().value_or(-1) : -1;
			if (name && voice >= 0 && size_t(voice) < voice_bus.size()) {
				voice_bus[size_t(voice)] = master_bus_of_name(name->as_str().value_or(""));
			}
		}
	}
	if (out.presets.empty()) {
		return master_error("no MasterUnit presets");
	}
	uint32_t low_pass_reg = UINT32_MAX;
	for (const SoundPluginLink &l : prog.links) {
		if (l.port.connected() && l.plugin.voice < prog.voices.size() &&
				l.plugin.plugin < prog.voices[l.plugin.voice].plugins_count &&
				prog.plugins[prog.voices[l.plugin.voice].plugins_begin + l.plugin.plugin].id == fourcc("LI20") &&
				l.attribute == 0) {
			for (const EbxInstance &n : g.nodes) {
				if (n.type_name() != "ReceiveNodeData") {
					continue;
				}
				const std::optional<EbxValue> e = n.get("Entries");
				if (e && e->as_array()) {
					for (EbxValue ev : *e->as_array()) {
						const std::optional<EbxInstance> ei = follow(p_db, *part, ev);
						if (ei && read_port(ei->data(), "Out").reg == std::optional<uint32_t>(l.port.reg)) {
							low_pass_reg = l.port.reg;
						}
					}
				}
			}
		}
	}
	const std::optional<EbxValue> info = graph->get("Info");
	const std::optional<EbxValue> conns = info ? info->get("Connections") : std::nullopt;
	std::vector<std::array<int64_t, 3>> route_params(prog.connections.size(), { -1, -1, -1 });
	if (conns && conns->as_array()) {
		for (size_t c = 0; c < conns->as_array()->size() && c < route_params.size(); ++c) {
			const std::optional<EbxValue> cv = conns->as_array()->get(c);
			const std::optional<EbxValue> params = cv ? cv->get("Parameters") : std::nullopt;
			if (params && params->as_array()) {
				for (EbxValue pv : *params->as_array()) {
					const int64_t index = pv.get("Index") ? pv.get("Index")->as_i64().value_or(0) : 0;
					if (index >= 1 && index <= 3) {
						route_params[c][size_t(index - 1)] = int64_t(
								std::lround(pv.get("Value") ? pv.get("Value")->as_f32().value_or(-1.0f) : -1.0f));
					}
				}
			}
		}
	}
	out.default_preset = std::min<uint32_t>(p_default_preset, uint32_t(out.presets.size() - 1));
	out.default_stereo_mode = std::min<uint32_t>(p_default_stereo_mode, 1);
	out.low_pass_hz = p_low_pass_hz;
	for (uint32_t stereo = 0; stereo < 2; ++stereo) {
		for (uint32_t preset = 0; preset < out.presets.size(); ++preset) {
			g.regs.assign(std::max<size_t>(prog.registers, 1), 0.0f);
			g.written.assign(g.regs.size(), 0);
			for (const SoundInput &in : prog.inputs) {
				if (in.reg < g.regs.size()) {
					g.regs[in.reg] = in.default_value;
				}
			}
			bool settled = false;
			for (size_t pass = 0; pass < kMaxEvaluationPasses && !settled; ++pass) {
				settled = true;
				for (const EbxInstance &n : g.nodes) {
					Result<bool> c = evaluate_node(g, n, preset, stereo, p_low_pass_hz, low_pass_reg);
					if (!c) {
						return c.error();
					}
					settled = settled && !*c;
				}
			}
			if (!settled) {
				return master_error("the control values do not settle");
			}
			MasterMixPlan plan;
			plan.preset = out.presets[preset];
			plan.speakers = uint32_t(
					std::lround(dac_speakers_reg < g.regs.size() ? g.regs[dac_speakers_reg] : dac_speakers_constant));
			plan.voices.resize(prog.voices.size());
			for (size_t v = 0; v < prog.voices.size(); ++v) {
				const SoundVoice &sv = prog.voices[v];
				MasterVoice &mv = plan.voices[v];
				mv.stage = sv.stage;
				mv.bus = voice_bus[v];
				uint32_t channels = sv.plugins_count > 0 ? prog.plugins[sv.plugins_begin].channels : 0u;
				mv.channels = uint8_t(channels);
				for (uint16_t k = 0; k < sv.plugins_count; ++k) {
					const SoundVoicePlugin &pl = prog.plugins[sv.plugins_begin + k];
					Result<MasterStepKind> kind = step_kind(pl.id);
					if (!kind) {
						return kind.error();
					}
					MasterStep step;
					step.kind = *kind;
					step.map.fill(-1);
					if (step.kind == MasterStepKind::Gain) {
						step.values[0] = 1.0f;
					}
					for (const SoundPluginLink &l : prog.links) {
						if (l.plugin.voice == v && l.plugin.plugin == k && l.attribute < kMasterAttributes) {
							step.values[l.attribute] = l.port.connected() && l.port.reg < g.regs.size()
									? g.regs[l.port.reg]
									: l.port.constant;
						}
					}
					if (step.kind == MasterStepKind::Send || step.kind == MasterStepKind::Route) {
						if (pl.connection == kNoRegister || pl.connection >= prog.connections.size() ||
								prog.connections[pl.connection].to_bus) {
							return master_error("voice " + std::to_string(v) + " plugin " + std::to_string(k) +
									" sends to no voice");
						}
						const uint16_t to = prog.connections[pl.connection].voice;
						if (to >= prog.voices.size() || prog.voices[to].plugins_count == 0) {
							return master_error("voice " + std::to_string(v) + " sends to voice " + std::to_string(to));
						}
						step.to_voice = to;
						const std::span<const SpeakerRole> from_layout = speaker_layout(channels);
						const std::span<const SpeakerRole> to_layout =
								speaker_layout(prog.plugins[prog.voices[to].plugins_begin].channels);
						if (step.kind == MasterStepKind::Send) {
							step.values[0] = 1.0f;
							for (const SpeakerRole r : from_layout) {
								step.map[size_t(r)] = role_index(to_layout, r) >= 0 ? int8_t(r) : int8_t(-1);
							}
						} else {
							const std::array<int64_t, 3> &rp = route_params[pl.connection];
							for (int64_t i = 0; i < rp[2]; ++i) {
								const int64_t s = rp[0] + i, d = rp[1] + i;
								if (s >= 0 && size_t(s) < from_layout.size() && d >= 0 &&
										size_t(d) < to_layout.size()) {
									step.map[size_t(from_layout[size_t(s)])] = int8_t(to_layout[size_t(d)]);
								}
							}
						}
					}
					if (step.kind == MasterStepKind::MatrixPan) {
						const std::span<const SpeakerRole> roles = speaker_layout(8);
						for (size_t a = 0; a < roles.size(); ++a) {
							step.map[size_t(roles[a])] = int8_t(a);
						}
					}
					if (pl.id == fourcc("Dac0")) {
						plan.output_voice = uint16_t(v);
					}
					channels = pl.channels > 0 ? pl.channels : channels;
					mv.steps.push_back(step);
				}
			}
			if (plan.output_voice == kNoRegister) {
				return master_error("no Dac plugin");
			}
			plan.order = prog.voice_order;
			out.plans[stereo].push_back(std::move(plan));
		}
	}
	return out;
}

}
