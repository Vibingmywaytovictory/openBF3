#include "bf3/audio/patch_instance.h"

#include "bf3/audio/mixer_core.h"

#include "bf3/core/audio/sound_wave.h"
#include "bf3/core/util/finite.h"
#include "patch_state.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
using bf3::core::finite;

bool one_plugin_node(SoundNodeType t) {
	return t == SoundNodeType::Gain || t == SoundNodeType::HighPassIir2 || t == SoundNodeType::LowPassIir2 || t == SoundNodeType::BandPassIir2 ||
			t == SoundNodeType::PeakingIir2 || t == SoundNodeType::Delay;
}

constexpr uint32_t kMaxDelaySeconds = 4;

std::vector<uint64_t> voice_output_masks(const ca::SoundProgram &p_p);

}

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
	st.voice_live.assign(p.voices.size(), 0);
	st.voice_cut.assign(p.voices.size(), 0.0f);
	if (p.outputs.size() <= 64) {
		const std::vector<uint64_t> masks = voice_output_masks(p);
		for (SlotState &sl : st.slots) {
			if (sl.voice < masks.size()) {
				sl.outputs_mask = masks[sl.voice];
			}
		}
	}
	self->slot_views_.resize(st.slots.size());
	return self;
}

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

uint32_t PatchInstance::queued_events() const {
	return state_->event_count + state_->deferred_count;
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
	st.emitter_position = p_emitter.position;
	Geometry &g = st.geometry;
	const Vec3 d = sub(p_emitter.position, p_listener.position);
	g.distance = length(d);
	const Vec3 right = cross(p_listener.forward, p_listener.up);
	if (g.distance > 1e-6f) {
		const Vec3 n{ d.x / g.distance, d.y / g.distance, d.z / g.distance };
		g.azimuth = std::atan2(dot(n, right), dot(n, p_listener.forward));
		g.elevation = std::asin(std::clamp(dot(n, p_listener.up) / std::max(1e-6f, length(p_listener.up)), -1.0f, 1.0f));
		g.relative_velocity = dot(sub(p_emitter.velocity, p_listener.velocity), n);
		const float fl = length(p_emitter.forward);
		if (fl > 1e-6f) {
			const float c = std::clamp(-dot(n, p_emitter.forward) / fl, -1.0f, 1.0f);
			g.cone_angle_deg = std::acos(c) * 180.0f / kPi;
		} else {
			g.cone_angle_deg = 0.0f;
		}
	} else {
		g.azimuth = g.elevation = g.relative_velocity = g.cone_angle_deg = 0.0f;
	}
	g.velocity = length(p_emitter.velocity);
}

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

bool same_coefficients(const BiquadCoefficients &p_a, const BiquadCoefficients &p_b) {
	return p_a.b0 == p_b.b0 && p_a.b1 == p_b.b1 && p_a.b2 == p_b.b2 && p_a.a1 == p_b.a1 && p_a.a2 == p_b.a2;
}

void process_biquad_pair(Biquad (&r_bq)[2], float *r_left, float *r_right, uint32_t p_frames, const BiquadCoefficients &p_target) {
	if (p_frames == 0 || !same_coefficients(r_bq[0].c, r_bq[1].c)) {
		r_bq[0].process(r_left, p_frames, p_target);
		r_bq[1].process(r_right, p_frames, p_target);
		return;
	}
	const BiquadCoefficients identity;
	if (same_coefficients(r_bq[0].c, identity) && same_coefficients(p_target, identity)) {
		r_bq[0].z1 = r_bq[0].z2 = r_bq[1].z1 = r_bq[1].z2 = 0.0f;
		return;
	}
	const BiquadCoefficients &c = r_bq[0].c;
	const bool ramp = !same_coefficients(c, p_target);
	float b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
	float l1 = r_bq[0].z1, l2 = r_bq[0].z2, q1 = r_bq[1].z1, q2 = r_bq[1].z2;
	if (ramp) {
		const float inv = 1.0f / float(p_frames);
		const float db0 = (p_target.b0 - b0) * inv, db1 = (p_target.b1 - b1) * inv, db2 = (p_target.b2 - b2) * inv;
		const float da1 = (p_target.a1 - a1) * inv, da2 = (p_target.a2 - a2) * inv;
		for (uint32_t i = 0; i < p_frames; ++i) {
			b0 += db0;
			b1 += db1;
			b2 += db2;
			a1 += da1;
			a2 += da2;
			const float x = r_left[i], u = r_right[i];
			const float y = b0 * x + l1, w = b0 * u + q1;
			l1 = b1 * x - a1 * y + l2;
			q1 = b1 * u - a1 * w + q2;
			l2 = b2 * x - a2 * y;
			q2 = b2 * u - a2 * w;
			r_left[i] = y;
			r_right[i] = w;
		}
	} else {
		for (uint32_t i = 0; i < p_frames; ++i) {
			const float x = r_left[i], u = r_right[i];
			const float y = b0 * x + l1, w = b0 * u + q1;
			l1 = b1 * x - a1 * y + l2;
			q1 = b1 * u - a1 * w + q2;
			l2 = b2 * x - a2 * y;
			q2 = b2 * u - a2 * w;
			r_left[i] = y;
			r_right[i] = w;
		}
	}
	if (!std::isfinite(l1) || !std::isfinite(l2)) {
		l1 = l2 = 0.0f;
	}
	if (!std::isfinite(q1) || !std::isfinite(q2)) {
		q1 = q2 = 0.0f;
	}
	r_bq[0].z1 = l1;
	r_bq[0].z2 = l2;
	r_bq[1].z1 = q1;
	r_bq[1].z2 = q2;
	r_bq[0].c = p_target;
	r_bq[1].c = p_target;
}

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

}

void PatchInstance::render(uint64_t p_frame, uint32_t p_offset, uint32_t p_frames, BusBuffers &r_buses) {
	State &st = *state_;
	const ca::SoundProgram &p = *st.program;
	const float rate = float(st.mix_rate);
	const bool tapping = st.tap_count > 0;

	const auto touch = [&](uint16_t p_v) {
		if (!st.voice_live[p_v]) {
			std::fill_n(st.voice(p_v, 0) + p_offset, p_frames, 0.0f);
			std::fill_n(st.voice(p_v, 1) + p_offset, p_frames, 0.0f);
			st.voice_live[p_v] = 1;
		}
	};
	std::fill(st.voice_live.begin(), st.voice_live.end(), uint8_t(0));
	if (tapping) {
		for (size_t v = 0; v < p.voices.size(); ++v) {
			touch(uint16_t(v));
		}
	}
	for (uint32_t t = 0; t < st.tap_count; ++t) {
		std::fill_n(st.tap_buffers[t].data(), p_frames, 0.0f);
	}
	st.edge[0] = st.edge[1] = 0.0f;
	if (p_offset == 0) {
		st.head[0] = st.head[1] = 0.0f;
	}
	const float window_db = p.loudness_db - st.tuning.window_top_db - st.tuning.window_headroom_db + st.tuning.master_gain_db;

	const float hdr_window_db = p.loudness_db - st.hdr.top_db - st.hdr.headroom_db + st.tuning.master_gain_db;
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
		bool quiet = false, quiet_known = false;
		for (uint16_t k = 0; k < voice.plugins_count; ++k) {
			const uint32_t g = voice.plugins_begin + k;
			PluginState &ps = st.plugins[g];
			if (ps.role == PluginRole::Slot) {
				quiet = quiet_known = false;
			} else if (!quiet_known) {
				quiet = !tapping && st.voice_quiet(voice, k, ch[0], ch[1], st.voice_live[v] ? p_frames : 0u);
				quiet_known = true;
			}
			if (quiet) {
				st.quiet_plugin(ps, rate, window_db, hdr_window_db);
				continue;
			}
			if (ps.role != PluginRole::Slot) {
				touch(v);
			} else if (const SlotState &sl = st.slots[ps.index];
					sl.play.active || sl.pending.armed || sl.outgoing.active) {
				touch(v);
			}
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
					st.voice_cut[v] *= target;
					break;
				}
				case PluginRole::HighPass:
				case PluginRole::LowPass: {
					const BiquadCoefficients c = BiquadCoefficients::design(ps.role == PluginRole::HighPass ? BiquadShape::HighPass : BiquadShape::LowPass, rate,
							ps.attribute(0), st.tuning.iir2_q, 0.0f);
					process_biquad_pair(ps.biquad, ch[0], ch[1], p_frames, c);
					break;
				}
				case PluginRole::BandPass: {
					const float f = ps.attribute(0), bw = ps.attribute(1);
					const float q = bw > 0.0f ? f / bw : 0.70710678f;
					const BiquadCoefficients c = BiquadCoefficients::design(BiquadShape::BandPass, rate, f, q, 0.0f);
					process_biquad_pair(ps.biquad, ch[0], ch[1], p_frames, c);
					break;
				}
				case PluginRole::Peaking: {
					const BiquadCoefficients c =
							BiquadCoefficients::design(BiquadShape::Peaking, rate, ps.attribute(0), ps.attribute(2), gain_to_db(std::max(0.0f, ps.attribute(1))));
					process_biquad_pair(ps.biquad, ch[0], ch[1], p_frames, c);
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
					break;
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
						break;
					}
					const ca::SoundConnection &c = p.connections[ps.index];
					float target = 1.0f;
					if (ps.output != UINT32_MAX) {
						const OutputState &o = st.outputs[ps.output];
						if (st.hdr.active && p.outputs[ps.output].enable_hdr) {
							target = st.hdr_send_gain(o, hdr_window_db);
						} else {
							target = db_to_gain(o.curve_gain_db + o.cone_db + window_db);
						}
					}
					if (c.to_bus && c.bus == ca::MasterBus::Lfe) {
						target *= db_to_gain(st.tuning.lfe_fold_db);
					}
					if (!c.to_bus && c.voice != v && c.voice < st.voice_cut.size()) {
						st.voice_cut[c.voice] = std::max(st.voice_cut[c.voice], st.voice_cut[v] * target);
					}
					if (c.to_bus && st.voice_cut[v] * target > kAudibleStep) {
						++st.stats.takeover_cuts;
						st.stats.takeover_cut_max = std::max(st.stats.takeover_cut_max, st.voice_cut[v] * target);
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
							touch(c.voice);
							dst = st.voice(c.voice, uint32_t(chn)) + p_offset;
						}
						const bool faded = c.to_bus && (st.cut_fade_left > 0 || st.cut_fade != 1.0f);
						if (dst != nullptr && !faded) {
							add_scaled(dst, ch[chn], p_frames, ps.gain_prev, target);
						}
						if (dst != nullptr && faded) {
							const float step = (target - ps.gain_prev) / float(p_frames);
							float g = ps.gain_prev;
							for (uint32_t i = 0; i < p_frames; ++i) {
								g += step;
								dst[i] += ch[chn][i] * g * st.cut_fade_at(i);
							}
						}
						if (dst != nullptr && c.to_bus) {
							const float g0 = (ps.gain_prev + (target - ps.gain_prev) / float(p_frames)) * st.cut_fade_at(0);
							st.edge[chn] += ch[chn][p_frames - 1] * target * st.cut_fade_at(p_frames - 1);
							if (p_offset == 0) {
								st.head[chn] += ch[chn][0] * g0;
							}
							const float hi = std::max(ps.gain_prev, target), lo = std::min(ps.gain_prev, target);
							if (p_frames < 64 && hi > 0.0f && lo < hi * 0.70794578f) {
								float peak = 0.0f;
								for (uint32_t i = 0; i < p_frames; ++i) {
									peak = std::max(peak, std::fabs(ch[chn][i]));
								}
								const float jump = peak * (hi - lo) / float(p_frames);
								if (jump > 1.0e-3f) {
									++st.abrupt;
									st.abrupt_max = std::max(st.abrupt_max, jump);
								}
							}
						}
					}
					ps.gain_prev = target;
					break;
				}
				case PluginRole::Pass:
					break;
			}
		}
		st.voice_cut[v] = 0.0f;
		tap_now();
	}
	for (uint32_t t = 0; t < st.tap_count; ++t) {
		st.taps[t]->on_voices(st.tap_buffers[t].data(), p_frames, p_frame);
	}
	if (st.cut_fade_left > 0) {
		if (st.cut_fade_left <= p_frames) {
			st.cut_fade = st.cut_fade_target;
			st.cut_fade_left = 0;
			st.cut_fade_step = 0.0f;
		} else {
			st.cut_fade += st.cut_fade_step * float(p_frames);
			st.cut_fade_left -= p_frames;
		}
	}
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

	const float metres = st.geometry.distance;

	uint64_t live = 0;
	for (const SlotState &sl : st.slots) {
		const bool busy = sl.play.active || sl.pending.armed;
		info.voices += busy ? 1 : 0;
		live |= busy ? sl.outputs_mask : 0;
	}
	if (live == 0) {
		live = st.loudness_hint != 0 ? st.loudness_hint : ~uint64_t(0);
	}
	const auto loudest_of = [&](uint64_t p_mask) {
		float loudest = -std::numeric_limits<float>::infinity();
		for (size_t o = 0; o < p.outputs.size(); ++o) {
			const ca::SoundOutput &out = p.outputs[o];
			if ((any_hdr && !out.enable_hdr) || (o < 64 && (p_mask & (uint64_t(1) << o)) == 0)) {
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
		return loudest;
	};
	float loudest = loudest_of(live);
	if (!std::isfinite(loudest) && live != ~uint64_t(0)) {
		loudest = loudest_of(~uint64_t(0));
	}
	info.hdr = any_hdr;
	info.loudness_db = p.loudness_db + (std::isfinite(loudest) ? loudest : 0.0f);
	return info;
}

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
				return true;
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
					target = hdr_send_gain(o, p_hdr_window_db);
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
			break;
	}
}

InstanceEdge PatchInstance::edge() const {
	const State &st = *state_;
	InstanceEdge e;
	e.edge = std::max(std::fabs(st.edge[0]), std::fabs(st.edge[1]));
	e.head = std::max(std::fabs(st.head[0]), std::fabs(st.head[1]));
	e.abrupt_gain = st.abrupt;
	e.abrupt_gain_max = st.abrupt_max;
	return e;
}

void PatchInstance::fade_to(float p_target, uint32_t p_frames) {
	State &st = *state_;
	const float target = std::isfinite(p_target) ? std::clamp(p_target, 0.0f, 1.0f) : 0.0f;
	st.cut_fade_target = target;
	if (p_frames == 0) {
		st.cut_fade = target;
		st.cut_fade_left = 0;
		st.cut_fade_step = 0.0f;
		return;
	}
	st.cut_fade_left = p_frames;
	st.cut_fade_step = (target - st.cut_fade) / float(p_frames);
}

void PatchInstance::set_fade(float p_level) {
	State &st = *state_;
	st.cut_fade = std::isfinite(p_level) ? std::clamp(p_level, 0.0f, 1.0f) : 0.0f;
	st.cut_fade_target = st.cut_fade;
	st.cut_fade_left = 0;
	st.cut_fade_step = 0.0f;
}

bool PatchInstance::fade_done() const {
	return state_->cut_fade_left == 0;
}

float PatchInstance::fade_level() const {
	return state_->cut_fade;
}

namespace {

std::vector<uint64_t> voice_output_masks(const ca::SoundProgram &p_p) {
	const size_t nv = p_p.voices.size();
	std::vector<uint64_t> m(nv, 0);
	for (size_t o = 0; o < p_p.outputs.size() && o < 64; ++o) {
		const ca::SoundOutput &so = p_p.outputs[o];
		const uint16_t v = so.main_send_plugin.valid() ? so.main_send_plugin.voice : so.low_pass.voice;
		if (v < nv) {
			m[v] |= uint64_t(1) << o;
		}
	}
	for (size_t pass = 0; pass < nv; ++pass) {
		bool changed = false;
		for (size_t v = 0; v < nv; ++v) {
			const ca::SoundVoice &voice = p_p.voices[v];
			for (uint32_t k = 0; k < voice.plugins_count && size_t(voice.plugins_begin) + k < p_p.plugins.size(); ++k) {
				const uint16_t c = p_p.plugins[voice.plugins_begin + k].connection;
				if (c == ca::kNoRegister || c >= p_p.connections.size() || p_p.connections[c].to_bus) {
					continue;
				}
				const uint16_t to = p_p.connections[c].voice;
				if (to < nv && (m[v] | m[to]) != m[v]) {
					m[v] |= m[to];
					changed = true;
				}
			}
		}
		if (!changed) {
			break;
		}
	}
	return m;
}

}

void PatchInstance::set_loudness_outputs(std::span<const uint16_t> p_outputs) {
	uint64_t m = 0;
	for (const uint16_t o : p_outputs) {
		m |= o < 64 ? uint64_t(1) << o : 0;
	}
	state_->loudness_hint = m;
}

void PatchInstance::skip_stop_event() {
	state_->stop_seen = true;
}

float PatchInstance::State::hdr_send_gain(const detail::OutputState &p_o, float p_hdr_window_db) const {
	const ca::SoundProgram &p = *program;
	const float level = p.loudness_db + p_o.curve_gain_db + p_o.cone_db;
	float top = hdr.top_db;
	if (p_o.bypass_headroom && std::isfinite(hdr.alone_top_db)) {
		top = std::min(hdr.top_db, hdr.alone_top_db);
	} else if (hdr.suspended) {
		return 0.0f;
	}
	const float window_db = p_hdr_window_db + (hdr.top_db - top);
	const float over = std::max(0.0f, level - (top + hdr.overshoot_db));
	return db_to_gain(p_o.curve_gain_db + p_o.cone_db + window_db - over);
}

}
