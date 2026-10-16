// SoundProgram blob ("OBF3SNDP"), its validation and the cached compile (sound_program.h, design §6.2, §8).
// validate_sound_program is the contract between the compiler and the interpreter: every index the evaluator
// follows without a check is checked here, so a decoded (or hand-built) program that passes is safe to run.

#include "bf3/core/audio/sound_program.h"

#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/cache/disk_cache.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::core::audio {

namespace {

constexpr std::string_view kMagic = "OBF3SNDP";
constexpr std::string_view kDepsMagic = "OBF3SNDD";
constexpr size_t kMaxString = 65536;
constexpr uint32_t kNoNode = UINT32_MAX;

Error invalid(const std::string &p_what) {
	return make_error("sound program: " + p_what);
}

bool range_ok(uint64_t p_begin, uint64_t p_count, uint64_t p_size) {
	return p_begin <= p_size && p_count <= p_size - p_begin;
}

bool option_ok(SoundNodeType p_type, uint8_t p_option) {
	switch (p_type) {
		case SoundNodeType::Sampler:
			return p_option <= uint8_t(SamplerLoop::Unknown);
		case SoundNodeType::SimpleTransform:
			return (p_option & 0x7f) <= uint8_t(SimpleTransformOp::Unknown);
		case SoundNodeType::LogicalExpression:
			return p_option <= uint8_t(LogicalOp::Unknown);
		case SoundNodeType::MultiCrossfaderGroup:
			return p_option <= uint8_t(FaderType::Unknown);
		case SoundNodeType::ConditionGroup:
			return p_option <= uint8_t(ConditionOp::Unknown);
		case SoundNodeType::NumberGenerator:
			return p_option <= uint8_t(NumberGeneratorMode::Unknown);
		case SoundNodeType::Timer:
			return p_option <= uint8_t(TimerMode::Unknown);
		default:
			return p_option == 0;
	}
}

// Ports/floats/plugins a node of p_type must carry (a2 types; others carry none).
Status check_shape(const SoundProgram &p, const SoundNode &n, bool p_child, size_t p_index) {
	const std::string where = std::string(p_child ? "child " : "node ") + std::to_string(p_index);
	if (size_t(n.type) >= size_t(SoundNodeType::Count)) {
		return invalid(where + ": type " + std::to_string(int(n.type)));
	}
	const SoundNodeTypeInfo &info = sound_node_type_info(n.type);
	if (p_child != (info.role == SoundNodeRole::Child)) {
		return invalid(where + ": " + std::string(info.ebx_name) + (p_child ? " is not a child record" : " is a child record"));
	}
	if (!option_ok(n.type, n.option)) {
		return invalid(where + ": option " + std::to_string(n.option));
	}
	if (!range_ok(n.ports_begin, n.ports_count, p.ports.size()) || !range_ok(n.floats_begin, n.floats_count, p.floats.size()) ||
			!range_ok(n.children_begin, n.children_count, p.children.size()) || !range_ok(n.waves_begin, n.waves_count, p.wave_refs.size()) ||
			!range_ok(n.plugins_begin, n.plugins_count, p.plugin_refs.size())) {
		return invalid(where + ": a pool range lies outside its pool");
	}
	if (!info.a2) {
		if (n.ports_count || n.floats_count || n.children_count || n.waves_count || n.plugins_count || n.output != kNoRegister) {
			return invalid(where + ": unimplemented " + std::string(info.ebx_name) + " carries data");
		}
		return Status::success();
	}
	if (n.ports_count != info.ports.size() + port::extra_ports(n.type)) {
		return invalid(where + ": " + std::to_string(n.ports_count) + " ports, " + std::string(info.ebx_name) + " has " +
				std::to_string(info.ports.size() + port::extra_ports(n.type)));
	}
	const bool extra_floats = n.type == SoundNodeType::SimpleValueSelector;
	if (extra_floats ? n.floats_count < info.floats.size() : n.floats_count != info.floats.size()) {
		return invalid(where + ": " + std::to_string(n.floats_count) + " floats");
	}
	const bool outputs = n.type == SoundNodeType::ConeOutput || n.type == SoundNodeType::FlatOutput || n.type == SoundNodeType::DirectOutput;
	if (outputs != (n.output != kNoRegister)) {
		return invalid(where + ": output record mismatch");
	}
	if (outputs && n.plugins_count != port::kOutputPlugins) {
		return invalid(where + ": an output carries " + std::to_string(n.plugins_count) + " plugin refs");
	}
	if (n.type == SoundNodeType::Sampler && (n.waves_count != 1 || n.plugins_count % port::kSamplerSlotPlugins != 0)) {
		return invalid(where + ": sampler wave / slot layout");
	}
	const bool one_plugin = n.type == SoundNodeType::Gain || n.type == SoundNodeType::HighPassIir2 || n.type == SoundNodeType::LowPassIir2 ||
			n.type == SoundNodeType::BandPassIir2 || n.type == SoundNodeType::PeakingIir2 || n.type == SoundNodeType::Delay;
	if (one_plugin && n.plugins_count != 1) {
		return invalid(where + ": plugin ref count");
	}
	if (!outputs && !one_plugin && n.type != SoundNodeType::Sampler && n.plugins_count != 0) {
		return invalid(where + ": unexpected plugin refs");
	}
	if (n.type != SoundNodeType::Sampler && n.type != SoundNodeType::WaveSwitcher && n.waves_count != 0) {
		return invalid(where + ": unexpected wave refs");
	}
	if (p_child && n.children_count != 0) {
		return invalid(where + ": a child record with children");
	}
	if (port::child_type(n.type) == SoundNodeType::Count && n.children_count != 0) {
		return invalid(where + ": unexpected children");
	}
	if (n.type == SoundNodeType::MixerReceiveEntry && n.aux >= p.mixer_inputs.size()) {
		return invalid(where + ": mixer input " + std::to_string(n.aux));
	}
	return Status::success();
}

// ---- writer / reader of every field ---------------------------------------------------------------------------

void put_port(BlobWriter &w, const SoundPort &p) {
	w.u16(p.reg);
	w.f32(p.constant);
}
SoundPort get_port(BlobReader &r) {
	SoundPort p;
	p.reg = r.u16();
	p.constant = r.f32();
	return p;
}
void put_ref(BlobWriter &w, const SoundPluginRef &p) {
	w.u16(p.voice);
	w.u16(p.plugin);
}
SoundPluginRef get_ref(BlobReader &r) {
	SoundPluginRef p;
	p.voice = r.u16();
	p.plugin = r.u16();
	return p;
}
void put_guid(BlobWriter &w, const Guid &g) {
	w.raw(&g, sizeof(Guid));
}
Guid get_guid(BlobReader &r) {
	Guid g;
	std::vector<uint8_t> raw;
	if (r.array<uint8_t>(sizeof(Guid), raw)) {
		std::memcpy(&g, raw.data(), sizeof(Guid));
	}
	return g;
}

void put_node(BlobWriter &w, const SoundNode &n) {
	w.u8(uint8_t(n.type));
	w.u8(n.option);
	w.u8(n.flags);
	w.u32(n.ports_begin);
	w.u16(n.ports_count);
	w.u32(n.floats_begin);
	w.u16(n.floats_count);
	w.u32(n.children_begin);
	w.u16(n.children_count);
	w.u32(n.waves_begin);
	w.u16(n.waves_count);
	w.u32(n.plugins_begin);
	w.u16(n.plugins_count);
	w.u16(n.output);
	w.u32(n.source_index);
	w.u32(n.aux);
}
SoundNode get_node(BlobReader &r) {
	SoundNode n;
	n.type = SoundNodeType(r.u8());
	n.option = r.u8();
	n.flags = r.u8();
	n.ports_begin = r.u32();
	n.ports_count = r.u16();
	n.floats_begin = r.u32();
	n.floats_count = r.u16();
	n.children_begin = r.u32();
	n.children_count = r.u16();
	n.waves_begin = r.u32();
	n.waves_count = r.u16();
	n.plugins_begin = r.u32();
	n.plugins_count = r.u16();
	n.output = r.u16();
	n.source_index = r.u32();
	n.aux = r.u32();
	return n;
}
constexpr size_t kNodeBytes = 3 + 4 * 6 + 2 * 6 + 4 + 4;

template <class T, class Put>
void put_vec(BlobWriter &w, const std::vector<T> &p_v, Put &&p_put) {
	w.u64(p_v.size());
	for (const T &e : p_v) {
		p_put(w, e);
	}
}
template <class T, class Get>
bool get_vec(BlobReader &r, size_t p_min_bytes, size_t p_max, std::vector<T> &r_out, Get &&p_get) {
	const size_t n = r.count(p_min_bytes, p_max);
	if (!r.ok()) {
		return false;
	}
	r_out.clear();
	r_out.reserve(n);
	for (size_t i = 0; i < n && r.ok(); ++i) {
		r_out.push_back(p_get(r));
	}
	return r.ok();
}

} // namespace

Status validate_sound_program(const SoundProgram &p) {
	if (p.registers > kMaxProgramValues) {
		return invalid(std::to_string(p.registers) + " registers");
	}
	if (p.nodes.size() > kMaxProgramNodes || p.child_nodes.size() > kMaxProgramNodes * kMaxNodeChildren || p.voices.size() > kMaxProgramVoices ||
			p.plugins.size() > kMaxProgramVoices * kMaxVoicePlugins || p.connections.size() > kMaxProgramConnections || p.waves.size() > kMaxProgramWaves ||
			p.outputs.size() > kMaxProgramVoices) {
		return invalid("a structural cap is exceeded");
	}
	auto reg_ok = [&](uint16_t r) { return r == kNoRegister || r < p.registers; };
	for (const SoundPort &port : p.ports) {
		if (!reg_ok(port.reg) || !std::isfinite(port.constant)) {
			return invalid("port register " + std::to_string(port.reg) + " of " + std::to_string(p.registers));
		}
	}
	for (float f : p.floats) {
		if (!std::isfinite(f)) {
			return invalid("non-finite float");
		}
	}
	for (const SoundInput &in : p.inputs) {
		if (in.reg >= p.registers || !std::isfinite(in.default_value)) {
			return invalid("input register " + std::to_string(in.reg));
		}
	}
	for (const SoundMixerInput &m : p.mixer_inputs) {
		if (!reg_ok(m.out)) {
			return invalid("mixer input register");
		}
	}
	for (size_t i = 0; i < p.nodes.size(); ++i) {
		BF3_TRY(check_shape(p, p.nodes[i], false, i));
		const SoundNode &n = p.nodes[i];
		const SoundNodeType ct = port::child_type(n.type);
		for (uint16_t c = 0; c < n.children_count; ++c) {
			const uint32_t ci = p.children[n.children_begin + c];
			if (ci >= p.child_nodes.size() || p.child_nodes[ci].type != ct) {
				return invalid("node " + std::to_string(i) + ": child " + std::to_string(c) + " is not a " + std::string(sound_node_type_info(ct).ebx_name));
			}
		}
		if (n.output != kNoRegister && (n.output >= p.outputs.size() || p.outputs[n.output].node != i)) {
			return invalid("node " + std::to_string(i) + ": output record " + std::to_string(n.output));
		}
	}
	for (size_t i = 0; i < p.child_nodes.size(); ++i) {
		BF3_TRY(check_shape(p, p.child_nodes[i], true, i));
	}
	for (uint16_t w : p.wave_refs) {
		if (w != kNoRegister && w >= p.waves.size()) {
			return invalid("wave ref " + std::to_string(w));
		}
	}
	auto plugin_ok = [&](const SoundPluginRef &r) {
		return !r.valid() || (r.voice < p.voices.size() && r.plugin < p.voices[r.voice].plugins_count);
	};
	for (const SoundPluginRef &r : p.plugin_refs) {
		if (!plugin_ok(r)) {
			return invalid("plugin ref (" + std::to_string(r.voice) + ", " + std::to_string(r.plugin) + ")");
		}
	}
	std::vector<uint8_t> seen(p.voices.size(), 0);
	if (p.voice_order.size() != p.voices.size()) {
		return invalid("voice order size");
	}
	for (uint16_t v : p.voice_order) {
		if (v >= p.voices.size() || seen[v]) {
			return invalid("voice order is not a permutation");
		}
		seen[v] = 1;
	}
	for (size_t i = 1; i < p.voice_order.size(); ++i) {
		if (p.voices[p.voice_order[i - 1]].stage > p.voices[p.voice_order[i]].stage) {
			return invalid("voice order is not by ascending stage");
		}
	}
	for (const SoundVoice &v : p.voices) {
		if (!range_ok(v.plugins_begin, v.plugins_count, p.plugins.size()) || v.plugins_count > kMaxVoicePlugins) {
			return invalid("voice plugin range");
		}
	}
	std::vector<uint8_t> named(p.connections.size(), 0);
	for (const SoundVoicePlugin &pl : p.plugins) {
		if (!range_ok(pl.construct_begin, pl.construct_count, p.construct_params.size())) {
			return invalid("construct param range");
		}
		if (pl.connection != kNoRegister) {
			if (pl.connection >= p.connections.size() || named[pl.connection]) {
				return invalid("connection " + std::to_string(pl.connection) + " out of range or named twice");
			}
			named[pl.connection] = 1;
		}
	}
	for (size_t i = 0; i < named.size(); ++i) {
		if (!named[i]) {
			return invalid("connection " + std::to_string(i) + " is named by no plugin");
		}
	}
	for (const SoundConnection &c : p.connections) {
		if (!c.to_bus && c.voice >= p.voices.size()) {
			return invalid("connection to voice " + std::to_string(c.voice));
		}
		if (size_t(c.bus) > size_t(MasterBus::Unknown)) {
			return invalid("connection bus");
		}
	}
	for (const SoundPluginLink &l : p.links) {
		if ((l.node != kNoNode && l.node >= p.nodes.size()) || !l.plugin.valid() || !plugin_ok(l.plugin) || l.attribute >= port::kMaxPluginAttributes ||
				!reg_ok(l.port.reg) || !std::isfinite(l.port.constant)) {
			return invalid("linked attribute");
		}
	}
	for (size_t i = 0; i < p.outputs.size(); ++i) {
		const SoundOutput &o = p.outputs[i];
		if (o.node >= p.nodes.size() || p.nodes[o.node].output != i) {
			return invalid("output " + std::to_string(i) + " names node " + std::to_string(o.node));
		}
		if (o.attenuation.points.size() > kMaxCurvePoints) {
			return invalid("output curve points");
		}
		for (size_t k = 0; k < o.attenuation.points.size(); ++k) {
			const AudioCurvePoint &pt = o.attenuation.points[k];
			if (!std::isfinite(pt.x) || !std::isfinite(pt.y) || !std::isfinite(pt.k) || (k > 0 && pt.x < o.attenuation.points[k - 1].x)) {
				return invalid("output curve point " + std::to_string(k));
			}
		}
		for (const SoundPluginRef *r : { &o.low_pass, &o.pan, &o.main_send_plugin, &o.reverb_send_plugin, &o.vu }) {
			if (!plugin_ok(*r)) {
				return invalid("output plugin ref");
			}
		}
		if (size_t(o.kind) > size_t(OutputKind::Direct) || size_t(o.main_send) > size_t(MasterBus::Unknown) ||
				size_t(o.reverb_send) > size_t(MasterBus::Unknown) || size_t(o.transform) > size_t(OutputTransformSource::Unknown)) {
			return invalid("output enum");
		}
	}
	if (!std::isfinite(p.loudness_db) || !std::isfinite(p.master_pitch) || !(p.master_pitch > 0.0f) || !std::isfinite(p.radius)) {
		return invalid("patch fields");
	}
	return Status::success();
}

std::vector<uint8_t> encode_sound_program(const SoundProgram &p) {
	BlobWriter w;
	w.magic(kMagic, kSoundProgramFormatVersion);
	w.str(p.name);
	put_guid(w, p.partition);
	w.f32(p.loudness_db);
	w.f32(p.radius);
	w.f32(p.master_pitch);
	w.f32(p.doppler_factor);
	w.f32(p.ai_loudness);
	w.boolean(p.is_looping);
	w.u32(p.start_event);
	w.u32(p.stop_event);
	w.u32(p.registers);
	put_vec(w, p.inputs, [](BlobWriter &b, const SoundInput &e) {
		b.u32(e.name_hash);
		b.u16(e.reg);
		b.f32(e.default_value);
		b.boolean(e.event);
	});
	put_vec(w, p.nodes, put_node);
	put_vec(w, p.child_nodes, put_node);
	put_vec(w, p.children, [](BlobWriter &b, uint32_t e) { b.u32(e); });
	put_vec(w, p.ports, put_port);
	put_vec(w, p.floats, [](BlobWriter &b, float e) { b.f32(e); });
	put_vec(w, p.wave_refs, [](BlobWriter &b, uint16_t e) { b.u16(e); });
	put_vec(w, p.plugin_refs, put_ref);
	put_vec(w, p.waves, [](BlobWriter &b, const SoundWaveRef &e) {
		put_guid(b, e.partition);
		b.str(e.name);
	});
	put_vec(w, p.mixer_inputs, [](BlobWriter &b, const SoundMixerInput &e) {
		put_guid(b, e.mixer);
		b.u32(e.name_hash);
		b.u16(e.out);
	});
	put_vec(w, p.outputs, [](BlobWriter &b, const SoundOutput &o) {
		b.u32(o.node);
		b.u8(uint8_t(o.kind));
		b.u32(o.name_hash);
		put_vec(b, o.attenuation.points, [](BlobWriter &c, const AudioCurvePoint &pt) {
			c.f32(pt.x);
			c.f32(pt.y);
			c.f32(pt.k);
		});
		for (float f : { o.gain_db, o.min_distance, o.hf_damping_distance, o.hf_damping_obstruction, o.hf_damping_occlusion, o.hf_damping_angle,
					 o.reverb_gain_db, o.pan_size, o.angle, o.outside_gain_db, o.direction[0], o.direction[1], o.direction[2] }) {
			b.f32(f);
		}
		b.u8(uint8_t(o.main_send));
		b.u8(uint8_t(o.reverb_send));
		for (const SoundPluginRef *r : { &o.low_pass, &o.pan, &o.main_send_plugin, &o.reverb_send_plugin, &o.vu }) {
			put_ref(b, *r);
		}
		b.boolean(o.enable_hdr);
		b.u8(uint8_t(o.transform));
	});
	put_vec(w, p.voices, [](BlobWriter &b, const SoundVoice &v) {
		b.u8(uint8_t(v.stage));
		b.u32(v.plugins_begin);
		b.u16(v.plugins_count);
	});
	put_vec(w, p.voice_order, [](BlobWriter &b, uint16_t e) { b.u16(e); });
	put_vec(w, p.plugins, [](BlobWriter &b, const SoundVoicePlugin &e) {
		b.u32(e.id);
		b.u8(e.channels);
		b.u16(e.connection);
		b.u32(e.construct_begin);
		b.u16(e.construct_count);
		b.u32(e.attribute_mask);
	});
	put_vec(w, p.construct_params, [](BlobWriter &b, float e) { b.f32(e); });
	put_vec(w, p.connections, [](BlobWriter &b, const SoundConnection &e) {
		b.boolean(e.to_bus);
		b.u16(e.voice);
		b.u8(uint8_t(e.bus));
	});
	put_vec(w, p.links, [](BlobWriter &b, const SoundPluginLink &e) {
		b.u32(e.node);
		put_ref(b, e.plugin);
		b.u8(e.attribute);
		b.boolean(e.allow_extreme);
		put_port(b, e.port);
	});
	return w.take();
}

Result<SoundProgram> decode_sound_program(std::span<const uint8_t> p_blob) {
	BlobReader r(p_blob);
	if (r.magic(kMagic) != kSoundProgramFormatVersion) {
		return r.ok() ? invalid("format version") : r.error("sound program");
	}
	SoundProgram p;
	p.name = r.str(kMaxString);
	p.partition = get_guid(r);
	p.loudness_db = r.f32();
	p.radius = r.f32();
	p.master_pitch = r.f32();
	p.doppler_factor = r.f32();
	p.ai_loudness = r.f32();
	p.is_looping = r.boolean();
	p.start_event = r.u32();
	p.stop_event = r.u32();
	p.registers = r.u32();
	get_vec(r, 11, kMaxProgramValues, p.inputs, [](BlobReader &b) {
		SoundInput e;
		e.name_hash = b.u32();
		e.reg = b.u16();
		e.default_value = b.f32();
		e.event = b.boolean();
		return e;
	});
	get_vec(r, kNodeBytes, kMaxProgramNodes, p.nodes, get_node);
	get_vec(r, kNodeBytes, kMaxProgramNodes * kMaxNodeChildren, p.child_nodes, get_node);
	get_vec(r, 4, SIZE_MAX, p.children, [](BlobReader &b) { return b.u32(); });
	get_vec(r, 6, SIZE_MAX, p.ports, get_port);
	get_vec(r, 4, SIZE_MAX, p.floats, [](BlobReader &b) { return b.f32(); });
	get_vec(r, 2, SIZE_MAX, p.wave_refs, [](BlobReader &b) { return b.u16(); });
	get_vec(r, 4, SIZE_MAX, p.plugin_refs, get_ref);
	get_vec(r, 20, kMaxProgramWaves, p.waves, [](BlobReader &b) {
		SoundWaveRef e;
		e.partition = get_guid(b);
		e.name = b.str(kMaxString);
		return e;
	});
	get_vec(r, 22, SIZE_MAX, p.mixer_inputs, [](BlobReader &b) {
		SoundMixerInput e;
		e.mixer = get_guid(b);
		e.name_hash = b.u32();
		e.out = b.u16();
		return e;
	});
	get_vec(r, 4 + 1 + 4 + 8 + 13 * 4 + 2 + 20 + 2, kMaxProgramVoices, p.outputs, [](BlobReader &b) {
		SoundOutput o;
		o.node = b.u32();
		o.kind = OutputKind(b.u8());
		o.name_hash = b.u32();
		get_vec(b, 12, kMaxCurvePoints, o.attenuation.points, [](BlobReader &c) {
			AudioCurvePoint pt;
			pt.x = c.f32();
			pt.y = c.f32();
			pt.k = c.f32();
			return pt;
		});
		for (float *f : { &o.gain_db, &o.min_distance, &o.hf_damping_distance, &o.hf_damping_obstruction, &o.hf_damping_occlusion, &o.hf_damping_angle,
					 &o.reverb_gain_db, &o.pan_size, &o.angle, &o.outside_gain_db, &o.direction[0], &o.direction[1], &o.direction[2] }) {
			*f = b.f32();
		}
		o.main_send = MasterBus(b.u8());
		o.reverb_send = MasterBus(b.u8());
		for (SoundPluginRef *ref : { &o.low_pass, &o.pan, &o.main_send_plugin, &o.reverb_send_plugin, &o.vu }) {
			*ref = get_ref(b);
		}
		o.enable_hdr = b.boolean();
		o.transform = OutputTransformSource(b.u8());
		return o;
	});
	get_vec(r, 7, kMaxProgramVoices, p.voices, [](BlobReader &b) {
		SoundVoice v;
		v.stage = int8_t(b.u8());
		v.plugins_begin = b.u32();
		v.plugins_count = b.u16();
		return v;
	});
	get_vec(r, 2, kMaxProgramVoices, p.voice_order, [](BlobReader &b) { return b.u16(); });
	get_vec(r, 17, kMaxProgramVoices * kMaxVoicePlugins, p.plugins, [](BlobReader &b) {
		SoundVoicePlugin e;
		e.id = b.u32();
		e.channels = b.u8();
		e.connection = b.u16();
		e.construct_begin = b.u32();
		e.construct_count = b.u16();
		e.attribute_mask = b.u32();
		return e;
	});
	get_vec(r, 4, kMaxProgramVoices * kMaxVoicePlugins, p.construct_params, [](BlobReader &b) { return b.f32(); });
	get_vec(r, 4, kMaxProgramConnections, p.connections, [](BlobReader &b) {
		SoundConnection e;
		e.to_bus = b.boolean();
		e.voice = b.u16();
		e.bus = MasterBus(b.u8());
		return e;
	});
	get_vec(r, 16, kMaxProgramVoices * kMaxVoicePlugins, p.links, [](BlobReader &b) {
		SoundPluginLink e;
		e.node = b.u32();
		e.plugin = get_ref(b);
		e.attribute = b.u8();
		e.allow_extreme = b.boolean();
		e.port = get_port(b);
		return e;
	});
	if (!r.ok()) {
		return r.error("sound program");
	}
	if (!r.at_end()) {
		return invalid(std::to_string(r.remaining()) + " trailing bytes");
	}
	BF3_TRY(validate_sound_program(p));
	return p;
}

// ---- cache (the WeaponDef two-entry scheme) -------------------------------------------------------------------

namespace {

std::vector<uint8_t> encode_deps(const std::vector<std::string> &p_deps) {
	BlobWriter w;
	w.magic(kDepsMagic, kSoundProgramFormatVersion);
	w.u64(p_deps.size());
	for (const std::string &d : p_deps) {
		w.str(d);
	}
	return w.take();
}

Result<std::vector<std::string>> decode_deps(std::span<const uint8_t> p_blob) {
	BlobReader r(p_blob);
	if (r.magic(kDepsMagic) != kSoundProgramFormatVersion) {
		return r.error("sound program deps");
	}
	std::vector<std::string> out;
	const size_t n = r.count(4, 4096);
	for (size_t i = 0; i < n && r.ok(); ++i) {
		out.push_back(r.str(kMaxString));
	}
	if (!r.ok()) {
		return r.error("sound program deps");
	}
	if (!r.at_end()) {
		return make_error("sound program deps: trailing bytes");
	}
	return out;
}

std::string params_of(std::string_view p_patch, const CompileOptions &p_options) {
	return "patch=" + std::string(p_patch) + ";allow=" + (p_options.allow_unimplemented ? "1" : "0");
}

std::optional<CacheKey> deps_key(const SoundProgramSources &p_sources, std::string_view p_patch, const CompileOptions &p_options) {
	if (!p_sources.digest) {
		return std::nullopt;
	}
	const std::optional<Sha1> root = p_sources.digest(p_patch);
	if (!root) {
		return std::nullopt;
	}
	CacheKeyInputs in;
	in.kind = kSoundProgramDepsCacheKind;
	in.sources.push_back(source_digest(*root));
	in.converter_version = kSoundProgramConverterVersion;
	in.engine = std::string(cache_engine_id());
	in.format_version = kSoundProgramFormatVersion;
	in.params = params_of(p_patch, p_options);
	return make_cache_key(in);
}

std::optional<CacheKey> program_key(const SoundProgramSources &p_sources, std::string_view p_patch, const CompileOptions &p_options,
		const std::vector<std::string> &p_deps) {
	if (!p_sources.digest) {
		return std::nullopt;
	}
	CacheKeyInputs in;
	in.kind = kSoundProgramCacheKind;
	const std::optional<Sha1> root = p_sources.digest(p_patch);
	if (!root) {
		return std::nullopt;
	}
	in.sources.push_back(source_digest(*root));
	for (const std::string &d : p_deps) {
		const std::optional<Sha1> sha1 = p_sources.digest(d);
		in.sources.push_back(sha1 ? source_digest(*sha1) : Sha1Digest{});
	}
	in.converter_version = kSoundProgramConverterVersion;
	in.engine = std::string(cache_engine_id());
	in.format_version = kSoundProgramFormatVersion;
	in.params = params_of(p_patch, p_options) + ";deps=" + sha256_of(encode_deps(p_deps)).hex();
	return make_cache_key(in);
}

} // namespace

Result<CachedSoundProgram> compile_sound_patch_cached(Cache *p_cache, const SoundProgramSources &p_sources, std::string_view p_patch,
		const CompileOptions &p_options) {
	if (p_sources.db == nullptr) {
		return make_error("compile_sound_patch_cached: no resolver");
	}
	CachedSoundProgram out;
	const bool use_cache = p_cache != nullptr && p_cache->is_open();
	const std::optional<CacheKey> dkey = use_cache ? deps_key(p_sources, p_patch, p_options) : std::nullopt;
	std::optional<std::vector<std::string>> cached_deps;
	bool deps_dropped = false;
	if (dkey) {
		GetResult got = p_cache->get(*dkey);
		if (got.status == CacheStatus::Ok) {
			Result<std::vector<std::string>> d = decode_deps(got.entry.payload());
			if (d) {
				cached_deps = std::move(*d);
			} else {
				got.entry.reset();
				p_cache->remove(*dkey);
				deps_dropped = true;
			}
		} else if (got.dropped_corrupt) {
			deps_dropped = true;
		}
	}
	if (cached_deps) {
		if (const std::optional<CacheKey> key = program_key(p_sources, p_patch, p_options, *cached_deps)) {
			GetResult got = p_cache->get(*key);
			if (got.status == CacheStatus::Ok) {
				Result<SoundProgram> prog = decode_sound_program(got.entry.payload());
				if (prog) {
					out.program = std::move(*prog);
					out.item.outcome = CacheOutcome::Hit;
					out.item.payload_bytes = got.entry.payload().size();
					return out;
				}
				got.entry.reset();
				p_cache->remove(*key);
				out.item.dropped_corrupt = true;
			} else if (got.dropped_corrupt) {
				out.item.dropped_corrupt = true;
			}
		}
	}
	const EbxPartition *partition = p_sources.db->partition_by_name(p_patch);
	if (partition == nullptr) {
		return make_error("sound patch " + std::string(p_patch) + ": no such partition");
	}
	gameplay::WorkBudget budget;
	CompileStats stats;
	Result<SoundProgram> prog = compile_sound_patch(*p_sources.db, *partition, p_options, budget, &stats);
	if (!prog) {
		return prog.error();
	}
	const std::vector<uint8_t> blob = encode_sound_program(*prog);
	out.item.payload_bytes = blob.size();
	out.item.outcome = CacheOutcome::Uncached;
	out.item.dropped_corrupt = out.item.dropped_corrupt || deps_dropped;
	out.program = std::move(*prog);
	if (!use_cache) {
		return out;
	}
	const std::optional<CacheKey> key = program_key(p_sources, p_patch, p_options, stats.dependencies);
	if (!dkey || !key) {
		out.item.cache_error = "no manifest SHA1 for a source partition";
		return out;
	}
	if (!cached_deps || *cached_deps != stats.dependencies) {
		if (cached_deps) {
			p_cache->remove(*dkey);
		}
		const PutResult put = p_cache->put(*dkey, encode_deps(stats.dependencies));
		if (put.status != CacheStatus::Ok) {
			out.item.cache_error = std::string("put deps: ") + cache_status_name(put.status);
			return out;
		}
	}
	const PutResult put = p_cache->put(*key, blob);
	if (put.status == CacheStatus::Ok) {
		out.item.outcome = put.written ? CacheOutcome::Converted : CacheOutcome::Reused;
	} else {
		out.item.cache_error = std::string("put: ") + cache_status_name(put.status);
	}
	return out;
}

} // namespace bf3::core::audio
