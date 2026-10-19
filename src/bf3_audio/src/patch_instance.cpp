// PatchInstance (patch_instance.h): creation (every buffer sized here, on the game thread), the game inputs, the
// sub-block split of a mix block, and the audio plan: voices in render order, each running its plugin chain into a
// stereo buffer and sending to later voices or to buses (design §6.4).

#include "bf3/audio/patch_instance.h"

#include "bf3/core/audio/sound_wave.h"
#include "patch_state.h"

#include <algorithm>
#include <cmath>
#include <limits> // M2b au [a3]

namespace bf3::audio {

using namespace detail;
using ca::SoundNode;
using ca::SoundNodeType;
using core::Result;

namespace {

constexpr float kPi = 3.14159265358979323846f;

float dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 sub(const Vec3 &a, const Vec3 &b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
Vec3 cross(const Vec3 &a, const Vec3 &b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
float length(const Vec3 &a) {
	return std::sqrt(dot(a, a));
}
bool finite(const Vec3 &a) {
	return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

bool one_plugin_node(SoundNodeType t) {
	return t == SoundNodeType::Gain || t == SoundNodeType::HighPassIir2 || t == SoundNodeType::LowPassIir2 || t == SoundNodeType::BandPassIir2 ||
			t == SoundNodeType::PeakingIir2 || t == SoundNodeType::Delay;
}

constexpr uint32_t kMaxDelaySeconds = 4;

} // namespace

PatchInstance::~PatchInstance() = default;

Result<std::unique_ptr<PatchInstance>> PatchInstance::create(std::shared_ptr<const ca::SoundProgram> p_program, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, uint64_t p_seed, uint32_t p_mix_rate) {
	if (!p_program) {
		return core::make_error("PatchInstance: no program");
	}
	if (p_mix_rate < 8000 || p_mix_rate > 192000) {
		return core::make_error("PatchInstance: mix rate " + std::to_string(p_mix_rate));
	}
	BF3_TRY(ca::validate_sound_program(*p_program));
	const ca::SoundProgram &p = *p_program;
	std::unique_ptr<PatchInstance> self(new PatchInstance());
	self->state_ = std::make_unique<State>();
	State &st = *self->state_;
	st.program = p_program;
	st.bank = std::move(p_bank);
	st.tuning = p_tuning;
	st.seed = p_seed;
	st.mix_rate = p_mix_rate;

	st.registers.assign(p.registers, 0.0f);
	for (const ca::SoundInput &in : p.inputs) {
		if (in.event) {
			st.event_registers.push_back(in.reg);
		} else {
			st.registers[in.reg] = in.default_value;
		}
	}
	if (p.stop_event != 0) {
		for (const ca::SoundInput &in : p.inputs) {
			if (in.event && in.name_hash == p.stop_event) {
				st.stop_register = in.reg;
			}
		}
	}

	// Waves: program wave -> bank wave and its playable (English) variation range.
	st.wave_ids.assign(p.waves.size(), ca::kNoWave);
	st.wave_variations.assign(p.waves.size(), { 0u, 0u });
	if (st.bank) {
		for (size_t w = 0; w < p.waves.size(); ++w) {
			const std::optional<ca::WaveId> id = st.bank->find(p.waves[w].partition);
			if (!id || !st.bank->resident(*id)) {
				continue;
			}
			const std::optional<std::pair<uint32_t, uint32_t>> range = ca::playable_variations(st.bank->waves()[*id], "English");
			if (!range || range->second == 0) {
				continue;
			}
			st.wave_ids[w] = *id;
			st.wave_variations[w] = *range;
		}
	}
	st.mixer_values.assign(p.mixer_inputs.size(), 0.0f);
	st.groups.assign(p.child_nodes.size(), GroupState{});

	// Plugins: roles by FourCC, then the sampler slots and outputs that claim them.
	st.plugins.assign(p.plugins.size(), PluginState{});
	for (size_t g = 0; g < p.plugins.size(); ++g) {
		PluginState &ps = st.plugins[g];
		const uint32_t id = p.plugins[g].id;
		if (id == ca::fourcc("Gai0")) {
			ps.role = PluginRole::Gain;
			ps.attributes[0] = 1.0f;
		} else if (id == ca::fourcc("HI20")) {
			ps.role = PluginRole::HighPass;
		} else if (id == ca::fourcc("LI20")) {
			ps.role = PluginRole::LowPass;
			ps.attributes[0] = 24000.0f;
		} else if (id == ca::fourcc("BI20")) {
			ps.role = PluginRole::BandPass;
			ps.attributes[0] = 1000.0f;
		} else if (id == ca::fourcc("PI20")) {
			ps.role = PluginRole::Peaking;
			ps.attributes[0] = 1000.0f;
			ps.attributes[1] = 1.0f;
			ps.attributes[2] = 0.70710678f;
		} else if (id == ca::fourcc("Del0")) {
			ps.role = PluginRole::Delay;
		} else if (id == ca::fourcc("LPB0")) {
			ps.role = PluginRole::Damping;
		} else if (id == ca::fourcc("Pn21")) {
			ps.role = PluginRole::Pan;
		} else if ((id == ca::fourcc("Sen0") || id == ca::fourcc("Rou0")) && p.plugins[g].connection != ca::kNoRegister) {
			ps.role = PluginRole::Send;
			ps.index = p.plugins[g].connection;
			const ca::SoundConnection &c = p.connections[ps.index];
			ps.reverb_send = c.to_bus && c.bus == ca::MasterBus::Reverb;
		}
	}
	st.outputs.assign(p.outputs.size(), OutputState{});
	for (size_t o = 0; o < p.outputs.size(); ++o) {
		const ca::SoundOutput &out = p.outputs[o];
		if (const uint32_t g = st.global_plugin(out.main_send_plugin); g != UINT32_MAX && st.plugins[g].role == PluginRole::Send) {
			st.plugins[g].output = uint32_t(o);
			st.outputs[o].voice = out.main_send_plugin.voice;
		}
		if (const uint32_t g = st.global_plugin(out.reverb_send_plugin); g != UINT32_MAX && st.plugins[g].role == PluginRole::Send) {
			st.plugins[g].reverb_send = true;
		}
		if (const uint32_t g = st.global_plugin(out.pan); g != UINT32_MAX) {
			st.plugins[g].output = uint32_t(o);
		}
		if (const uint32_t g = st.global_plugin(out.low_pass); g != UINT32_MAX) {
			st.plugins[g].output = uint32_t(o);
		}
	}

	// Nodes: per-node state, samplers and their slots, attribute drives, delay lines.
	st.nodes.assign(p.nodes.size(), NodeState{});
	size_t slot_count = 0;
	size_t delay_floats = 0;
	for (const SoundNode &n : p.nodes) {
		if (n.type == SoundNodeType::Sampler) {
			slot_count += n.plugins_count / cp::kSamplerSlotPlugins;
		}
	}
	st.slots.assign(slot_count, SlotState{});
	slot_count = 0;
	for (uint32_t ni = 0; ni < p.nodes.size(); ++ni) {
		const SoundNode &n = p.nodes[ni];
		NodeState &s = st.nodes[ni];
		switch (n.type) {
			case SoundNodeType::NumberGenerator:
				s.value = st.flt(n, cp::NumMin);
				break;
			case SoundNodeType::Lfo:
				if (n.flags & ca::kFlagStartAtRandomValue) {
					s.phase = uniform01(p_seed, ni, s.draws++);
				}
				break;
			case SoundNodeType::WaveSwitcher:
				if (n.waves_count > 0) {
					if (n.flags & ca::kFlagRandomStartIndex) {
						s.index = std::min<uint32_t>(n.waves_count - 1, uint32_t(uniform01(p_seed, ni, s.draws++) * n.waves_count));
					} else {
						const float d = std::floor(st.flt(n, cp::WsDefaultIndex));
						s.index = d <= 0.0f ? 0u : std::min<uint32_t>(n.waves_count - 1, uint32_t(d));
					}
				}
				break;
			case SoundNodeType::Sampler: {
				s.index = uint32_t(st.samplers.size());
				SamplerState sm;
				sm.node = ni;
				sm.slots_begin = uint32_t(slot_count);
				sm.slots_count = n.plugins_count / cp::kSamplerSlotPlugins;
				sm.loop = ca::SamplerLoop(n.option);
				sm.shuffle = (n.flags & ca::kFlagShuffleSegments) != 0;
				sm.step_connected = st.port(n, cp::SmpStep).connected();
				sm.last_slot = uint8_t(std::max<uint32_t>(sm.slots_count, 1) - 1);
				for (uint32_t k = 0; k < sm.slots_count; ++k) {
					SlotState &slot = st.slots[slot_count + k];
					const ca::SoundPluginRef player = p.plugin_refs[n.plugins_begin + k * cp::kSamplerSlotPlugins + cp::SlotSndPlayer];
					const ca::SoundPluginRef gain = p.plugin_refs[n.plugins_begin + k * cp::kSamplerSlotPlugins + cp::SlotGain];
					slot.voice = player.valid() ? player.voice : 0xffff;
					if (const uint32_t g = st.global_plugin(player); g != UINT32_MAX) {
						st.plugins[g].role = PluginRole::Slot;
						st.plugins[g].index = uint32_t(slot_count + k);
						st.plugins[g].output = s.index;
					}
					if (const uint32_t g = st.global_plugin(gain); g != UINT32_MAX) {
						st.plugins[g].role = PluginRole::SlotGain;
						st.plugins[g].index = uint32_t(slot_count + k);
						st.plugins[g].output = s.index;
						slot.gain_plugin = g;
					}
				}
				slot_count += sm.slots_count;
				st.samplers.push_back(sm);
				break;
			}
			default:
				break;
		}
		if (one_plugin_node(n.type) && n.plugins_count == 1) {
			const uint32_t g = st.global_plugin(p.plugin_refs[n.plugins_begin]);
			if (g != UINT32_MAX) {
				for (uint8_t a = 0; a < cp::kMaxPluginAttributes; ++a) {
					const uint8_t port = cp::attribute_port(n.type, a);
					if (port != cp::kNoPort) {
						st.drives.push_back(AttributeDrive{ g, a, st.port(n, port) });
					}
				}
				if (n.type == SoundNodeType::Delay && st.plugins[g].role == PluginRole::Delay && st.plugins[g].delay_frames == 0) {
					const float max_s = std::clamp(st.flt(n, cp::DelayMax), 0.0f, float(kMaxDelaySeconds));
					st.plugins[g].delay_frames = uint32_t(max_s * float(p_mix_rate)) + 1;
					st.plugins[g].delay_begin = uint32_t(delay_floats);
					delay_floats += size_t(st.plugins[g].delay_frames) * 2;
				}
			}
		}
	}
	for (const ca::SoundPluginLink &l : p.links) {
		const uint32_t g = st.global_plugin(l.plugin);
		if (g != UINT32_MAX && l.attribute < cp::kMaxPluginAttributes) {
			st.drives.push_back(AttributeDrive{ g, l.attribute, l.port });
		}
	}
	st.delay_pool.assign(delay_floats, 0.0f);
	st.voice_buffers.assign(p.voices.size() * 2 * kMaxSubBlock, 0.0f);
	st.voice_tapped.assign(p.voices.size(), 0);
	st.voice_live.assign(p.voices.size(), 0); // integ/m2b-w1 [lazy zero]
	self->slot_views_.resize(st.slots.size());
	return self;
}

// ---- game inputs --------------------------------------------------------------------------------------------------

void PatchInstance::set_parameter(uint32_t p_name_hash, float p_value) {
	State &st = *state_;
	for (const ca::SoundInput &in : st.program->inputs) {
		if (in.name_hash == p_name_hash && !in.event) {
			st.registers[in.reg] = std::isfinite(p_value) ? p_value : 0.0f;
			return;
		}
	}
	++st.stats.unknown_inputs;
}

void PatchInstance::post_event(uint32_t p_name_hash, uint32_t p_frame_in_block) {
	State &st = *state_;
	for (const ca::SoundInput &in : st.program->inputs) {
		if (in.name_hash == p_name_hash && in.event) {
			const PendingEvent e{ in.reg, std::min(p_frame_in_block, kMaxSubBlock - 1), false };
			if (st.event_count < kMaxEventsPerBlock) {
				st.events[st.event_count++] = e;
			} else {
				++st.stats.deferred_events;
				if (st.deferred_count < kMaxEventsPerBlock) {
					st.deferred[st.deferred_count++] = PendingEvent{ e.reg, 0, false };
				}
			}
			return;
		}
	}
	++st.stats.unknown_inputs;
}

void PatchInstance::set_mixer_value(uint32_t p_name_hash, float p_value) {
	State &st = *state_;
	for (size_t i = 0; i < st.program->mixer_inputs.size(); ++i) {
		if (st.program->mixer_inputs[i].name_hash == p_name_hash) {
			st.mixer_values[i] = std::isfinite(p_value) ? p_value : 0.0f;
		}
	}
}

void PatchInstance::set_geometry(const EmitterState &p_emitter, const ListenerState &p_listener) {
	State &st = *state_;
	if (!finite(p_emitter.position) || !finite(p_listener.position) || !finite(p_emitter.velocity) || !finite(p_listener.velocity)) {
		return;
	}
	st.emitter_position = p_emitter.position; // M2b au [a3]
	Geometry &g = st.geometry;
	const Vec3 d = sub(p_emitter.position, p_listener.position);
	g.distance = length(d);
	const Vec3 right = cross(p_listener.up, p_listener.forward); // +X for +Y up, +Z forward
	if (g.distance > 1e-6f) {
		const Vec3 n{ d.x / g.distance, d.y / g.distance, d.z / g.distance };
		g.azimuth = std::atan2(dot(n, right), dot(n, p_listener.forward));
		g.elevation = std::asin(std::clamp(dot(n, p_listener.up) / std::max(1e-6f, length(p_listener.up)), -1.0f, 1.0f));
		g.relative_velocity = dot(sub(p_emitter.velocity, p_listener.velocity), n);
		const float fl = length(p_emitter.forward);
		if (fl > 1e-6f) {
			const float c = std::clamp(-dot(n, p_emitter.forward) / fl, -1.0f, 1.0f); // emitter -> listener is -n
			g.cone_angle_deg = std::acos(c) * 180.0f / kPi;
		} else {
			g.cone_angle_deg = 0.0f;
		}
	} else {
		g.azimuth = g.elevation = g.relative_velocity = g.cone_angle_deg = 0.0f;
	}
	g.velocity = length(p_emitter.velocity);
}

// ---- processing ---------------------------------------------------------------------------------------------------

namespace {

void sort_events(std::array<PendingEvent, kMaxEventsPerBlock> &r_events, uint32_t p_count) {
	for (uint32_t i = 1; i < p_count; ++i) {
		const PendingEvent e = r_events[i];
		uint32_t j = i;
		while (j > 0 && r_events[j - 1].frame > e.frame) {
			r_events[j] = r_events[j - 1];
			--j;
		}
		r_events[j] = e;
	}
}

// integ/m2b-w1 [biquad pair] (E11, docs/design/m2b.md "M2b integ-w1"): Biquad::process (dsp.cpp) over a voice's two
// channels in one loop, the same arithmetic per channel, so the output is unchanged; the two channels' recurrences
// overlap instead of running one after the other. Channels whose coefficients differ take Biquad::process each.
bool same_coefficients(const BiquadCoefficients &p_a, const BiquadCoefficients &p_b) { // integ/m2b-w1 [biquad pair]
	return p_a.b0 == p_b.b0 && p_a.b1 == p_b.b1 && p_a.b2 == p_b.b2 && p_a.a1 == p_b.a1 && p_a.a2 == p_b.a2; // integ/m2b-w1 [biquad pair]
} // integ/m2b-w1 [biquad pair]

void process_biquad_pair(Biquad (&r_bq)[2], float *r_left, float *r_right, uint32_t p_frames, const BiquadCoefficients &p_target) { // integ/m2b-w1 [biquad pair]
	if (p_frames == 0 || !same_coefficients(r_bq[0].c, r_bq[1].c)) { // integ/m2b-w1 [biquad pair]
		r_bq[0].process(r_left, p_frames, p_target); // integ/m2b-w1 [biquad pair]
		r_bq[1].process(r_right, p_frames, p_target); // integ/m2b-w1 [biquad pair]
		return; // integ/m2b-w1 [biquad pair]
	} // integ/m2b-w1 [biquad pair]
	const BiquadCoefficients identity; // integ/m2b-w1 [biquad pair]
	if (same_coefficients(r_bq[0].c, identity) && same_coefficients(p_target, identity)) { // integ/m2b-w1 [biquad pair]
		r_bq[0].z1 = r_bq[0].z2 = r_bq[1].z1 = r_bq[1].z2 = 0.0f; // integ/m2b-w1 [biquad pair]
		return; // the bit exact pass-through // integ/m2b-w1 [biquad pair]
	} // integ/m2b-w1 [biquad pair]
	const BiquadCoefficients &c = r_bq[0].c; // integ/m2b-w1 [biquad pair]
	const bool ramp = !same_coefficients(c, p_target); // integ/m2b-w1 [biquad pair]
	float b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2; // integ/m2b-w1 [biquad pair]
	float l1 = r_bq[0].z1, l2 = r_bq[0].z2, q1 = r_bq[1].z1, q2 = r_bq[1].z2; // integ/m2b-w1 [biquad pair]
	if (ramp) { // integ/m2b-w1 [biquad pair]
		const float inv = 1.0f / float(p_frames); // integ/m2b-w1 [biquad pair]
		const float db0 = (p_target.b0 - b0) * inv, db1 = (p_target.b1 - b1) * inv, db2 = (p_target.b2 - b2) * inv; // integ/m2b-w1 [biquad pair]
		const float da1 = (p_target.a1 - a1) * inv, da2 = (p_target.a2 - a2) * inv; // integ/m2b-w1 [biquad pair]
		for (uint32_t i = 0; i < p_frames; ++i) { // integ/m2b-w1 [biquad pair]
			b0 += db0; // integ/m2b-w1 [biquad pair]
			b1 += db1; // integ/m2b-w1 [biquad pair]
			b2 += db2; // integ/m2b-w1 [biquad pair]
			a1 += da1; // integ/m2b-w1 [biquad pair]
			a2 += da2; // integ/m2b-w1 [biquad pair]
			const float x = r_left[i], u = r_right[i]; // integ/m2b-w1 [biquad pair]
			const float y = b0 * x + l1, w = b0 * u + q1; // integ/m2b-w1 [biquad pair]
			l1 = b1 * x - a1 * y + l2; // integ/m2b-w1 [biquad pair]
			q1 = b1 * u - a1 * w + q2; // integ/m2b-w1 [biquad pair]
			l2 = b2 * x - a2 * y; // integ/m2b-w1 [biquad pair]
			q2 = b2 * u - a2 * w; // integ/m2b-w1 [biquad pair]
			r_left[i] = y; // integ/m2b-w1 [biquad pair]
			r_right[i] = w; // integ/m2b-w1 [biquad pair]
		} // integ/m2b-w1 [biquad pair]
	} else { // integ/m2b-w1 [biquad pair]
		for (uint32_t i = 0; i < p_frames; ++i) { // integ/m2b-w1 [biquad pair]
			const float x = r_left[i], u = r_right[i]; // integ/m2b-w1 [biquad pair]
			const float y = b0 * x + l1, w = b0 * u + q1; // integ/m2b-w1 [biquad pair]
			l1 = b1 * x - a1 * y + l2; // integ/m2b-w1 [biquad pair]
			q1 = b1 * u - a1 * w + q2; // integ/m2b-w1 [biquad pair]
			l2 = b2 * x - a2 * y; // integ/m2b-w1 [biquad pair]
			q2 = b2 * u - a2 * w; // integ/m2b-w1 [biquad pair]
			r_left[i] = y; // integ/m2b-w1 [biquad pair]
			r_right[i] = w; // integ/m2b-w1 [biquad pair]
		} // integ/m2b-w1 [biquad pair]
	} // integ/m2b-w1 [biquad pair]
	if (!std::isfinite(l1) || !std::isfinite(l2)) { // integ/m2b-w1 [biquad pair]
		l1 = l2 = 0.0f; // integ/m2b-w1 [biquad pair]
	} // integ/m2b-w1 [biquad pair]
	if (!std::isfinite(q1) || !std::isfinite(q2)) { // integ/m2b-w1 [biquad pair]
		q1 = q2 = 0.0f; // integ/m2b-w1 [biquad pair]
	} // integ/m2b-w1 [biquad pair]
	r_bq[0].z1 = l1; // integ/m2b-w1 [biquad pair]
	r_bq[0].z2 = l2; // integ/m2b-w1 [biquad pair]
	r_bq[1].z1 = q1; // integ/m2b-w1 [biquad pair]
	r_bq[1].z2 = q2; // integ/m2b-w1 [biquad pair]
	r_bq[0].c = p_target; // integ/m2b-w1 [biquad pair]
	r_bq[1].c = p_target; // integ/m2b-w1 [biquad pair]
} // integ/m2b-w1 [biquad pair]

void add_scaled(float *r_dst, const float *p_src, uint32_t p_frames, float p_from, float p_to) {
	if (p_from == p_to) {
		for (uint32_t i = 0; i < p_frames; ++i) {
			r_dst[i] += p_src[i] * p_to;
		}
		return;
	}
	const float step = (p_to - p_from) / float(p_frames);
	float g = p_from;
	for (uint32_t i = 0; i < p_frames; ++i) {
		g += step;
		r_dst[i] += p_src[i] * g;
	}
}

} // namespace

void PatchInstance::render(uint64_t p_frame, uint32_t p_offset, uint32_t p_frames, BusBuffers &r_buses) {
	State &st = *state_;
	const ca::SoundProgram &p = *st.program;
	const float rate = float(st.mix_rate);
	const bool tapping = st.tap_count > 0;
	// integ/m2b-w1 [lazy zero] (E11, docs/design/m2b.md "M2b integ-w1"): a voice's sub-block range is zeroed when
	// something first writes or reads it (a busy slot, a plugin that runs, a send into it), not up front; a voice
	// nothing touched holds zeros by definition, so the [quiet] check needs no scan of it. Taps read every voice, so
	// they zero all of them as before.
	const auto touch = [&](uint16_t p_v) { // integ/m2b-w1 [lazy zero]
		if (!st.voice_live[p_v]) { // integ/m2b-w1 [lazy zero]
			std::fill_n(st.voice(p_v, 0) + p_offset, p_frames, 0.0f); // integ/m2b-w1 [lazy zero]
			std::fill_n(st.voice(p_v, 1) + p_offset, p_frames, 0.0f); // integ/m2b-w1 [lazy zero]
			st.voice_live[p_v] = 1; // integ/m2b-w1 [lazy zero]
		} // integ/m2b-w1 [lazy zero]
	}; // integ/m2b-w1 [lazy zero]
	std::fill(st.voice_live.begin(), st.voice_live.end(), uint8_t(0)); // integ/m2b-w1 [lazy zero]
	if (tapping) { // integ/m2b-w1 [lazy zero]
		for (size_t v = 0; v < p.voices.size(); ++v) { // integ/m2b-w1 [lazy zero]
			touch(uint16_t(v)); // integ/m2b-w1 [lazy zero]
		} // integ/m2b-w1 [lazy zero]
	} // integ/m2b-w1 [lazy zero]
	for (uint32_t t = 0; t < st.tap_count; ++t) {
		std::fill_n(st.tap_buffers[t].data(), p_frames, 0.0f);
	}
	st.pt1_edge[0] = st.pt1_edge[1] = 0.0f; // PT1 snd [edge]: the last sub-block's values stay
	if (p_offset == 0) { // PT1 snd [edge]
		st.pt1_head[0] = st.pt1_head[1] = 0.0f; // PT1 snd [edge]
	} // PT1 snd [edge]
	const float window_db = p.loudness_db - st.tuning.window_top_db - st.tuning.window_headroom_db + st.tuning.master_gain_db;
	// M2b au R5 (docs/design/m2b.md §6.1): an EnableHdr output takes the HDR window's gain when the mixer runs one.
	const float hdr_window_db = p.loudness_db - st.hdr.top_db - st.hdr.headroom_db + st.tuning.master_gain_db; // M2b au R5
	const float pan_scale = db_to_gain(st.tuning.pan_law_db) / 0.70710678f;
	for (uint16_t v : p.voice_order) {
		const ca::SoundVoice &voice = p.voices[v];
		float *ch[2] = { st.voice(v, 0) + p_offset, st.voice(v, 1) + p_offset };
		bool tapped = false;
		const auto tap_now = [&] {
			if (tapping && st.voice_tapped[v] && !tapped) {
				for (uint32_t t = 0; t < st.tap_count; ++t) {
					if ((st.voice_tapped[v] >> t) & 1u) {
						float *dst = st.tap_buffers[t].data();
						for (uint32_t i = 0; i < p_frames; ++i) {
							dst[i] += 0.5f * (ch[0][i] + ch[1][i]);
						}
					}
				}
			}
			tapped = true;
		};
		bool quiet = false, quiet_known = false; // M2b au [quiet]
		for (uint16_t k = 0; k < voice.plugins_count; ++k) {
			const uint32_t g = voice.plugins_begin + k;
			PluginState &ps = st.plugins[g];
			if (ps.role == PluginRole::Slot) { // M2b au [quiet]: a slot renders, the plugins after it are checked again
				quiet = quiet_known = false; // M2b au [quiet]
			} else if (!quiet_known) { // M2b au [quiet]
				quiet = !tapping && st.voice_quiet(voice, k, ch[0], ch[1], st.voice_live[v] ? p_frames : 0u); // M2b au [quiet]; integ/m2b-w1 [lazy zero]: no scan of an untouched voice
				quiet_known = true; // M2b au [quiet]
			} // M2b au [quiet]
			if (quiet) { // M2b au [quiet]
				st.quiet_plugin(ps, rate, window_db, hdr_window_db); // M2b au [quiet]
				continue; // M2b au [quiet]
			} // M2b au [quiet]
			if (ps.role != PluginRole::Slot) { // integ/m2b-w1 [lazy zero]
				touch(v); // integ/m2b-w1 [lazy zero]
			} else if (const SlotState &sl = st.slots[ps.index]; sl.play.active || sl.pending.armed) { // integ/m2b-w1 [lazy zero]
				touch(v); // integ/m2b-w1 [lazy zero]: an idle slot writes nothing
			} // integ/m2b-w1 [lazy zero]
			switch (ps.role) {
				case PluginRole::Slot:
					st.render_slot(st.samplers[ps.output], ps.index - st.samplers[ps.output].slots_begin, ch[0], ch[1], p_frame, p_frames);
					break;
				case PluginRole::SlotGain:
				case PluginRole::Gain: {
					float target = ps.role == PluginRole::SlotGain ? st.samplers[ps.output].amplitude : ps.attribute(0);
					target = std::isfinite(target) ? std::clamp(target, 0.0f, 16.0f) : 0.0f;
					if (!ps.gain_started) {
						ps.gain_prev = target;
						ps.gain_started = true;
					}
					apply_gain_ramp(ch[0], p_frames, ps.gain_prev, target);
					apply_gain_ramp(ch[1], p_frames, ps.gain_prev, target);
					ps.gain_prev = target;
					break;
				}
				case PluginRole::HighPass:
				case PluginRole::LowPass: {
					const BiquadCoefficients c = BiquadCoefficients::design(ps.role == PluginRole::HighPass ? BiquadShape::HighPass : BiquadShape::LowPass, rate,
							ps.attribute(0), st.tuning.iir2_q, 0.0f);
					process_biquad_pair(ps.biquad, ch[0], ch[1], p_frames, c); // integ/m2b-w1 [biquad pair]
					break;
				}
				case PluginRole::BandPass: {
					const float f = ps.attribute(0), bw = ps.attribute(1);
					const float q = bw > 0.0f ? f / bw : 0.70710678f; // Bandwidth in Hz [inferred]
					const BiquadCoefficients c = BiquadCoefficients::design(BiquadShape::BandPass, rate, f, q, 0.0f);
					process_biquad_pair(ps.biquad, ch[0], ch[1], p_frames, c); // integ/m2b-w1 [biquad pair]
					break;
				}
				case PluginRole::Peaking: {
					const BiquadCoefficients c =
							BiquadCoefficients::design(BiquadShape::Peaking, rate, ps.attribute(0), ps.attribute(2), gain_to_db(std::max(0.0f, ps.attribute(1))));
					process_biquad_pair(ps.biquad, ch[0], ch[1], p_frames, c); // integ/m2b-w1 [biquad pair]
					break;
				}
				case PluginRole::Delay: {
					if (ps.delay_frames < 2) {
						break;
					}
					float *line = st.delay_pool.data() + ps.delay_begin;
					const uint32_t len = ps.delay_frames;
					const float t = std::isfinite(ps.attribute(0)) ? ps.attribute(0) : 0.0f;
					const uint32_t d = std::clamp<uint32_t>(uint32_t(std::max(0.0f, t) * rate), 1u, len - 1);
					const float fb = std::isfinite(ps.attribute(1)) ? std::clamp(ps.attribute(1), 0.0f, 0.95f) : 0.0f;
					for (uint32_t i = 0; i < p_frames; ++i) {
						const uint32_t r = (ps.delay_write + len - d) % len;
						for (int c = 0; c < 2; ++c) {
							const float out = line[size_t(r) * 2 + c];
							line[size_t(ps.delay_write) * 2 + c] = ch[c][i] + fb * out;
							ch[c][i] = out;
						}
						ps.delay_write = (ps.delay_write + 1) % len;
					}
					break;
				}
				case PluginRole::Damping:
					break; // LPB0: open (no damping model until A3, tuning hf_damping_db_per_metre = 0)
				case PluginRole::Pan: {
					const float spread = ps.output != UINT32_MAX ? p.outputs[ps.output].pan_size : 0.0f;
					const StereoGains pan = equal_power_pan(st.geometry.azimuth, spread);
					const float gl = pan.left * pan_scale, gr = pan.right * pan_scale;
					for (uint32_t i = 0; i < p_frames; ++i) {
						const float m = 0.5f * (ch[0][i] + ch[1][i]);
						ch[0][i] = m * gl;
						ch[1][i] = m * gr;
					}
					break;
				}
				case PluginRole::Send: {
					tap_now();
					if (ps.reverb_send) {
						break; // the Reverb bus is A4
					}
					const ca::SoundConnection &c = p.connections[ps.index];
					float target = 1.0f;
					if (ps.output != UINT32_MAX) {
						const OutputState &o = st.outputs[ps.output];
						if (st.hdr.active && p.outputs[ps.output].enable_hdr) { // M2b au R5
							const float over = std::max(0.0f, p.loudness_db + o.curve_gain_db + o.cone_db - (st.hdr.top_db + st.hdr.overshoot_db)); // M2b au R5
							target = st.hdr.suspended ? 0.0f : db_to_gain(o.curve_gain_db + o.cone_db + hdr_window_db - over); // M2b au R5
						} else { // M2b au R5
							target = db_to_gain(o.curve_gain_db + o.cone_db + window_db);
						} // M2b au R5
					}
					if (c.to_bus && c.bus == ca::MasterBus::Lfe) {
						target *= db_to_gain(st.tuning.lfe_fold_db);
					}
					if (!ps.gain_started) {
						ps.gain_prev = target;
						ps.gain_started = true;
					}
					for (int chn = 0; chn < 2; ++chn) {
						float *dst = nullptr;
						if (c.to_bus) {
							if (size_t(c.bus) < size_t(ca::MasterBus::Unknown) && r_buses.channels[size_t(c.bus)][chn] != nullptr) {
								dst = r_buses.channels[size_t(c.bus)][chn] + p_offset;
							}
						} else if (c.voice != v) {
							touch(c.voice); // integ/m2b-w1 [lazy zero]
							dst = st.voice(c.voice, uint32_t(chn)) + p_offset;
						}
						const bool pt1_faded = c.to_bus && (st.pt1_fade_left > 0 || st.pt1_fade != 1.0f); // PT1 snd [fade]
						if (dst != nullptr && !pt1_faded) { // PT1 snd [fade]: `&& !pt1_faded` added
							add_scaled(dst, ch[chn], p_frames, ps.gain_prev, target);
						}
						if (dst != nullptr && pt1_faded) { // PT1 snd [fade]: add_scaled's ramp times the output fade
							const float step = (target - ps.gain_prev) / float(p_frames); // PT1 snd [fade]
							float g = ps.gain_prev; // PT1 snd [fade]
							for (uint32_t i = 0; i < p_frames; ++i) { // PT1 snd [fade]
								g += step; // PT1 snd [fade]
								dst[i] += ch[chn][i] * g * st.pt1_fade_at(i); // PT1 snd [fade]
							} // PT1 snd [fade]
						} // PT1 snd [fade]
						if (dst != nullptr && c.to_bus) { // PT1 snd [edge]
							const float g0 = (ps.gain_prev + (target - ps.gain_prev) / float(p_frames)) * st.pt1_fade_at(0); // PT1 snd [edge]: the first gain; [fade] x fade
							st.pt1_edge[chn] += ch[chn][p_frames - 1] * target * st.pt1_fade_at(p_frames - 1); // PT1 snd [edge]; [fade] x fade
							if (p_offset == 0) { // PT1 snd [edge]
								st.pt1_head[chn] += ch[chn][0] * g0; // PT1 snd [edge]
							} // PT1 snd [edge]
							const float hi = std::max(ps.gain_prev, target), lo = std::min(ps.gain_prev, target); // PT1 snd [edge]
							if (p_frames < 64 && hi > 0.0f && lo < hi * 0.70794578f) { // PT1 snd [edge]: > 3 dB within < 64 frames
								float peak = 0.0f; // PT1 snd [edge]
								for (uint32_t i = 0; i < p_frames; ++i) { // PT1 snd [edge]
									peak = std::max(peak, std::fabs(ch[chn][i])); // PT1 snd [edge]
								} // PT1 snd [edge]
								const float jump = peak * (hi - lo) / float(p_frames); // PT1 snd [edge]
								if (jump > 1.0e-3f) { // PT1 snd [edge]
									++st.pt1_abrupt; // PT1 snd [edge]
									st.pt1_abrupt_max = std::max(st.pt1_abrupt_max, jump); // PT1 snd [edge]
								} // PT1 snd [edge]
							} // PT1 snd [edge]
						} // PT1 snd [edge]
					}
					ps.gain_prev = target;
					break;
				}
				case PluginRole::Pass:
					break;
			}
		}
		tap_now();
	}
	for (uint32_t t = 0; t < st.tap_count; ++t) {
		st.taps[t]->on_voices(st.tap_buffers[t].data(), p_frames, p_frame);
	}
	if (st.pt1_fade_left > 0) { // PT1 snd [fade]: the fade moves on by the sub-block
		if (st.pt1_fade_left <= p_frames) { // PT1 snd [fade]
			st.pt1_fade = st.pt1_fade_target; // PT1 snd [fade]
			st.pt1_fade_left = 0; // PT1 snd [fade]
			st.pt1_fade_step = 0.0f; // PT1 snd [fade]
		} else { // PT1 snd [fade]
			st.pt1_fade += st.pt1_fade_step * float(p_frames); // PT1 snd [fade]
			st.pt1_fade_left -= p_frames; // PT1 snd [fade]
		} // PT1 snd [fade]
	} // PT1 snd [fade]
}

void PatchInstance::process_block(BusBuffers &r_buses) {
	State &st = *state_;
	const uint32_t n = std::min(r_buses.frames, kMaxSubBlock);
	const uint64_t first = r_buses.first_frame;
	st.clock = first;
	sort_events(st.events, st.event_count);
	uint32_t pos = 0, ei = 0;
	while (pos < n) {
		for (uint16_t r : st.event_registers) {
			st.registers[r] = 0.0f;
		}
		while (ei < st.event_count && std::min(st.events[ei].frame, n - 1) <= pos) {
			st.registers[st.events[ei].reg] = 1.0f;
			st.any_event = true;
			++ei;
		}
		const uint32_t next = ei < st.event_count ? std::max(pos + 1, std::min(st.events[ei].frame, n - 1)) : n;
		const uint32_t len = next - pos;
		st.evaluate_program(first + pos);
		++st.stats.evaluations;
		render(first + pos, pos, len, r_buses);
		st.advance(double(len) / double(st.mix_rate));
		pos = next;
	}
	st.clock = first + n;
	st.event_count = 0;
	for (uint32_t i = 0; i < st.deferred_count; ++i) {
		st.events[st.event_count++] = st.deferred[i];
	}
	st.deferred_count = 0;
}

void PatchInstance::evaluate(double p_seconds) {
	State &st = *state_;
	for (uint16_t r : st.event_registers) {
		st.registers[r] = 0.0f;
	}
	for (uint32_t i = 0; i < st.event_count; ++i) {
		st.registers[st.events[i].reg] = 1.0f;
		st.any_event = true;
	}
	st.event_count = 0;
	st.evaluate_program(st.clock);
	++st.stats.evaluations;
	const double seconds = std::isfinite(p_seconds) ? std::max(0.0, p_seconds) : 0.0;
	st.advance(seconds);
	st.clock += uint64_t(std::llround(seconds * double(st.mix_rate)));
	for (uint32_t i = 0; i < st.deferred_count; ++i) {
		st.events[st.event_count++] = st.deferred[i];
	}
	st.deferred_count = 0;
}

bool PatchInstance::finished() const {
	const State &st = *state_;
	if (st.stats.evaluations == 0) {
		return false;
	}
	if (st.program->stop_event != 0 && !st.stop_seen) {
		return false;
	}
	for (const SamplerState &s : st.samplers) {
		if (s.active || st.sampler_busy(s)) {
			return false;
		}
	}
	const ca::SoundProgram &p = *st.program;
	for (size_t i = 0; i < p.nodes.size(); ++i) {
		const NodeState &s = st.nodes[i];
		if (p.nodes[i].type == SoundNodeType::Adsr) {
			const AdsrPhase phase = AdsrPhase(s.mode);
			if (phase == AdsrPhase::Attack || phase == AdsrPhase::Decay || phase == AdsrPhase::Release) {
				return false;
			}
		} else if (p.nodes[i].type == SoundNodeType::Timer && s.mode && ca::TimerMode(p.nodes[i].option) == ca::TimerMode::SingleShot) {
			return false;
		}
	}
	return true;
}

// ---- inspection and hooks ------------------------------------------------------------------------------------------

float PatchInstance::value(uint16_t p_register) const {
	return p_register < state_->registers.size() ? state_->registers[p_register] : 0.0f;
}

std::span<const float> PatchInstance::registers() const {
	return state_->registers;
}

float PatchInstance::plugin_attribute(ca::SoundPluginRef p_plugin, uint8_t p_attribute) const {
	const State &st = *state_;
	if (!p_plugin.valid() || p_plugin.voice >= st.program->voices.size() || p_plugin.plugin >= st.program->voices[p_plugin.voice].plugins_count ||
			p_attribute >= cp::kMaxPluginAttributes) {
		return 0.0f;
	}
	return st.plugins[st.global_plugin(p_plugin)].attribute(p_attribute);
}

float PatchInstance::crossfader_group_amplitude(uint32_t p_node, uint32_t p_group) const {
	const State &st = *state_;
	if (p_node >= st.program->nodes.size() || p_group >= st.program->nodes[p_node].children_count) {
		return 0.0f;
	}
	return st.groups[st.child_index(st.program->nodes[p_node], p_group)].amplitude;
}

std::span<const SamplerSlotState> PatchInstance::sampler_slots(uint32_t p_node) const {
	const State &st = *state_;
	if (p_node >= st.program->nodes.size() || st.program->nodes[p_node].type != SoundNodeType::Sampler) {
		return {};
	}
	const SamplerState &s = st.samplers[st.nodes[p_node].index];
	for (uint32_t k = 0; k < s.slots_count; ++k) {
		const SlotState &slot = st.slots[s.slots_begin + k];
		SamplerSlotState &view = slot_views_[s.slots_begin + k];
		view.playing = slot.play.active || slot.pending.armed;
		view.wave = slot.play.active ? slot.play.wave : (slot.pending.armed ? s.wave : ca::kNoWave);
		view.variation = slot.play.active ? slot.play.variation : s.variation;
		view.segment = slot.play.active ? slot.play.segment : slot.pending.segment;
		view.frame_in_segment = slot.play.frames_out;
		view.pitch = float(double(s.pitch) * (slot.play.active ? double(slot.play.rate) : double(st.mix_rate)) / double(st.mix_rate));
		view.gain = s.amplitude;
	}
	return std::span<const SamplerSlotState>(slot_views_.data() + s.slots_begin, s.slots_count);
}

float PatchInstance::output_gain_db(uint32_t p_output) const {
	return p_output < state_->outputs.size() ? state_->outputs[p_output].curve_gain_db : 0.0f;
}

PatchHaptics PatchInstance::haptics() const {
	return state_->haptics;
}

void PatchInstance::override_plugin_attribute(ca::SoundPluginRef p_plugin, uint8_t p_attribute, std::optional<float> p_value) {
	State &st = *state_;
	if (!p_plugin.valid() || p_plugin.voice >= st.program->voices.size() || p_plugin.plugin >= st.program->voices[p_plugin.voice].plugins_count ||
			p_attribute >= cp::kMaxPluginAttributes) {
		return;
	}
	PluginState &ps = st.plugins[st.global_plugin(p_plugin)];
	if (p_value && std::isfinite(*p_value)) {
		ps.override_mask |= 1u << p_attribute;
		ps.overrides[p_attribute] = *p_value;
	} else {
		ps.override_mask &= ~(1u << p_attribute);
	}
}

void PatchInstance::set_voice_tap(std::span<const uint16_t> p_voices, VoiceTap *p_tap) {
	State &st = *state_;
	std::fill(st.voice_tapped.begin(), st.voice_tapped.end(), uint8_t(0));
	st.taps.fill(nullptr);
	st.tap_count = 0;
	add_voice_tap(p_voices, p_tap);
}

void PatchInstance::add_voice_tap(std::span<const uint16_t> p_voices, VoiceTap *p_tap) {
	State &st = *state_;
	if (p_tap == nullptr || st.tap_count >= kMaxVoiceTaps) {
		return;
	}
	const uint32_t t = st.tap_count;
	size_t taken = 0;
	for (uint16_t v : p_voices) {
		if (v < st.voice_tapped.size() && taken < kMaxTapVoices) {
			st.voice_tapped[v] |= uint8_t(1u << t);
			++taken;
		}
	}
	if (taken > 0) {
		st.taps[t] = p_tap;
		++st.tap_count;
	}
}

bool PatchInstance::stop_seen() const {
	return state_->program->stop_event == 0 || state_->stop_seen;
}

void PatchInstance::set_sampler_trace(SamplerTrace *r_trace) {
	state_->trace = r_trace;
}

const ca::SoundProgram &PatchInstance::program() const {
	return *state_->program;
}

const PatchInstanceStats &PatchInstance::stats() const {
	return state_->stats;
}

// M2b au [a3] (docs/design/m2b.md §6, docs/design/m2b-au.md): the HDR window and the cull's view of the instance.

void PatchInstance::set_hdr_window(const HdrVoiceWindow &p_window) {
	state_->hdr = p_window;
}

VoiceCullInfo PatchInstance::cull_info() const {
	const State &st = *state_;
	const ca::SoundProgram &p = *st.program;
	VoiceCullInfo info;
	info.distance_m = st.geometry.distance;
	info.position = st.emitter_position;
	bool any_hdr = false;
	for (const ca::SoundOutput &out : p.outputs) {
		any_hdr = any_hdr || out.enable_hdr;
	}
	// The curve at the current distance (as State::evaluate_program computes it), so a voice that has not been
	// evaluated yet (its first block) is measured where it is.
	const float metres = st.geometry.distance;
	float loudest = -std::numeric_limits<float>::infinity();
	for (size_t o = 0; o < p.outputs.size(); ++o) {
		const ca::SoundOutput &out = p.outputs[o];
		if (any_hdr && !out.enable_hdr) {
			continue;
		}
		float d = metres;
		if (metres < out.min_distance) {
			d = metres + (out.min_distance - metres) * st.tuning.min_distance_clamp;
		}
		const float cone = o < st.outputs.size() ? st.outputs[o].cone_db : 0.0f;
		const float at = out.attenuation.evaluate(d * st.tuning.curve_x_metres) + out.gain_db + cone;
		if (std::isfinite(at)) {
			loudest = std::max(loudest, at);
		}
	}
	for (const SlotState &sl : st.slots) {
		info.voices += (sl.play.active || sl.pending.armed) ? 1 : 0;
	}
	info.hdr = any_hdr;
	info.loudness_db = p.loudness_db + (std::isfinite(loudest) ? loudest : 0.0f);
	return info;
}

// M2b au [quiet] (docs/design/m2b-au.md "Quiet voices"): render() walks every voice of the patch every sub-block, and a
// weapon patch has 25-32 voices of which a few play (the 64-shooter load: 3,420 voice chains for 396 busy slots). A
// voice is quiet from plugin p_from on when its buffer holds only zeros there, no later plugin of it is a delay line,
// and every later filter's state is zero: then each later plugin's output is zero whatever it does (a gain, filter
// or pan of zeros with a zero state, a send adding zeros), so render() skips the per-sample work and quiet_plugin
// moves the plugin's state exactly as the full path would (gain ramps to their targets, filter coefficients to their
// designed values). The mix is unchanged; only the sign of a zero sample inside a voice buffer may differ.

bool PatchInstance::State::voice_quiet(const ca::SoundVoice &p_voice, uint16_t p_from, const float *p_left, const float *p_right, uint32_t p_frames) const {
	for (uint16_t k = p_from; k < p_voice.plugins_count; ++k) {
		const PluginState &ps = plugins[p_voice.plugins_begin + k];
		switch (ps.role) {
			case PluginRole::Delay:
				if (ps.delay_frames >= 2) {
					return false;
				}
				break;
			case PluginRole::HighPass:
			case PluginRole::LowPass:
			case PluginRole::BandPass:
			case PluginRole::Peaking:
				if (ps.biquad[0].z1 != 0.0f || ps.biquad[0].z2 != 0.0f || ps.biquad[1].z1 != 0.0f || ps.biquad[1].z2 != 0.0f) {
					return false;
				}
				break;
			case PluginRole::Slot:
				return true; // the slot itself renders (render() re-checks after it)
			default:
				break;
		}
	}
	for (uint32_t i = 0; i < p_frames; ++i) {
		if (p_left[i] != 0.0f || p_right[i] != 0.0f) {
			return false;
		}
	}
	return true;
}

void PatchInstance::State::quiet_plugin(PluginState &r_ps, float p_rate, float p_window_db, float p_hdr_window_db) {
	const ca::SoundProgram &p = *program;
	// Biquad::process on zeros from a zero state: the state stays zero and the coefficients become the target, except
	// when both are the identity (the bit-exact pass-through leaves them).
	const auto biquads = [&](const BiquadCoefficients &p_target) {
		for (Biquad &b : r_ps.biquad) {
			const bool id_now = b.c.b0 == 1.0f && b.c.b1 == 0.0f && b.c.b2 == 0.0f && b.c.a1 == 0.0f && b.c.a2 == 0.0f;
			const bool id_target = p_target.b0 == 1.0f && p_target.b1 == 0.0f && p_target.b2 == 0.0f && p_target.a1 == 0.0f && p_target.a2 == 0.0f;
			if (!(id_now && id_target)) {
				b.c = p_target;
			}
			b.z1 = b.z2 = 0.0f;
		}
	};
	switch (r_ps.role) {
		case PluginRole::SlotGain:
		case PluginRole::Gain: {
			float target = r_ps.role == PluginRole::SlotGain ? samplers[r_ps.output].amplitude : r_ps.attribute(0);
			target = std::isfinite(target) ? std::clamp(target, 0.0f, 16.0f) : 0.0f;
			r_ps.gain_prev = target;
			r_ps.gain_started = true;
			break;
		}
		case PluginRole::HighPass:
		case PluginRole::LowPass:
			biquads(BiquadCoefficients::design(r_ps.role == PluginRole::HighPass ? BiquadShape::HighPass : BiquadShape::LowPass, p_rate, r_ps.attribute(0),
					tuning.iir2_q, 0.0f));
			break;
		case PluginRole::BandPass: {
			const float f = r_ps.attribute(0), bw = r_ps.attribute(1);
			const float q = bw > 0.0f ? f / bw : 0.70710678f;
			biquads(BiquadCoefficients::design(BiquadShape::BandPass, p_rate, f, q, 0.0f));
			break;
		}
		case PluginRole::Peaking:
			biquads(BiquadCoefficients::design(BiquadShape::Peaking, p_rate, r_ps.attribute(0), r_ps.attribute(2), gain_to_db(std::max(0.0f, r_ps.attribute(1)))));
			break;
		case PluginRole::Send: {
			if (r_ps.reverb_send) {
				break;
			}
			const ca::SoundConnection &c = p.connections[r_ps.index];
			float target = 1.0f;
			if (r_ps.output != UINT32_MAX) {
				const OutputState &o = outputs[r_ps.output];
				if (hdr.active && p.outputs[r_ps.output].enable_hdr) {
					const float over = std::max(0.0f, p.loudness_db + o.curve_gain_db + o.cone_db - (hdr.top_db + hdr.overshoot_db));
					target = hdr.suspended ? 0.0f : db_to_gain(o.curve_gain_db + o.cone_db + p_hdr_window_db - over);
				} else {
					target = db_to_gain(o.curve_gain_db + o.cone_db + p_window_db);
				}
			}
			if (c.to_bus && c.bus == ca::MasterBus::Lfe) {
				target *= db_to_gain(tuning.lfe_fold_db);
			}
			r_ps.gain_prev = target;
			r_ps.gain_started = true;
			break;
		}
		default:
			break; // Slot never reaches here; Delay (a zero-length line), Damping, Pan and Pass keep no state
	}
}

// PT1 snd (docs/design/pt1-snd.md): the instance's output at the edges of its last block.

InstanceEdge PatchInstance::pt1_edge() const {
	const State &st = *state_;
	InstanceEdge e;
	e.edge = std::max(std::fabs(st.pt1_edge[0]), std::fabs(st.pt1_edge[1]));
	e.head = std::max(std::fabs(st.pt1_head[0]), std::fabs(st.pt1_head[1]));
	e.abrupt_gain = st.pt1_abrupt;
	e.abrupt_gain_max = st.pt1_abrupt_max;
	return e;
}

void PatchInstance::pt1_fade_to(float p_target, uint32_t p_frames) {
	State &st = *state_;
	const float target = std::isfinite(p_target) ? std::clamp(p_target, 0.0f, 1.0f) : 0.0f;
	st.pt1_fade_target = target;
	if (p_frames == 0) {
		st.pt1_fade = target;
		st.pt1_fade_left = 0;
		st.pt1_fade_step = 0.0f;
		return;
	}
	st.pt1_fade_left = p_frames;
	st.pt1_fade_step = (target - st.pt1_fade) / float(p_frames);
}

void PatchInstance::pt1_set_fade(float p_level) {
	State &st = *state_;
	st.pt1_fade = std::isfinite(p_level) ? std::clamp(p_level, 0.0f, 1.0f) : 0.0f;
	st.pt1_fade_target = st.pt1_fade;
	st.pt1_fade_left = 0;
	st.pt1_fade_step = 0.0f;
}

bool PatchInstance::pt1_fade_done() const {
	return state_->pt1_fade_left == 0;
}

float PatchInstance::pt1_fade_level() const {
	return state_->pt1_fade;
}

} // namespace bf3::audio
