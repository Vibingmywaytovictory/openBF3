#pragma once

#include "bf3/audio/dsp.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/util/splitmix64.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bf3::audio::detail {

namespace ca = core::audio;
namespace cp = core::audio::port;

inline constexpr uint32_t kSlotInputFrames = 64;
inline constexpr uint32_t kMaxSubBlock = 512;
inline constexpr uint16_t kNoSegment = 0xffff;

using core::splitmix64;
double uniform01(uint64_t p_seed, uint32_t p_node, uint32_t p_draw);

enum class AdsrPhase : uint8_t { Idle, Attack, Decay, Sustain, Release };

struct NodeState {
	float value = 0.0f;
	double phase = 0.0;
	float release_rate = 0.0f;
	float latched = 0.0f;
	uint32_t draws = 0;
	uint32_t index = 0;
	uint8_t mode = 0;
	uint8_t pulse = 0;
};

struct GroupState {
	float amplitude = 0.0f;
	bool started = false;
};

struct SlotPlay {
	bool active = false;
	ca::WaveId wave = ca::kNoWave;
	uint32_t variation = 0;
	uint16_t segment = 0;
	uint8_t channels = 1;
	uint32_t rate = 48000;
	bool source_done = false;
	uint8_t flush = 0;
	uint32_t frames_out = 0;
	ca::StreamDecoder decoder;
	std::array<float, kSlotInputFrames * ca::kMaxStreamChannels> input{};
	uint32_t input_count = 0, input_pos = 0;
	float history[4][2] = {};
	double frac = 0.0;
};

struct PendingStart {
	bool armed = false;
	uint64_t frame = 0;
	uint16_t segment = 0;
	SamplerTraceKind kind = SamplerTraceKind::Trigger;
	float offset_seconds = 0.0f;
};

struct SlotState {
	SlotPlay play;
	PendingStart pending;
	float fade = 1.0f;
	float fade_step = 0.0f;
	bool fade_kill = false;
	SlotPlay outgoing;
	float outgoing_gain = 0.0f;
	float outgoing_step = 0.0f;
	uint64_t outgoing_frame = 0;
	uint16_t voice = 0xffff;
	uint32_t gain_plugin = UINT32_MAX;
	uint64_t outputs_mask = ~uint64_t(0);
};

struct SamplerState {
	uint32_t node = 0;
	uint32_t slots_begin = 0, slots_count = 0;
	ca::SamplerLoop loop = ca::SamplerLoop::Release;
	bool shuffle = false;
	bool step_connected = false;
	bool active = false;
	bool released = false;
	bool stepped = false;
	float delay_seconds = 0.0f;
	ca::WaveId wave = ca::kNoWave;
	uint32_t variation = 0;
	int32_t first_loop = -1, last_loop = -1;
	uint16_t segments = 0;
	uint8_t last_slot = 0;
	uint16_t last_segment = kNoSegment;
	uint32_t draws = 0;
	bool finished_pulse = false;
	float amplitude = 0.0f, amplitude_prev = 0.0f;
	float pitch = 1.0f;
	bool instant_release = false;
	bool trigger_was_on = false;
};

enum class PluginRole : uint8_t {
	Pass,
	Slot,
	SlotGain,
	Gain,
	HighPass,
	LowPass,
	BandPass,
	Peaking,
	Delay,
	Damping,
	Pan,
	Send,
};

struct PluginState {
	PluginRole role = PluginRole::Pass;
	uint32_t index = UINT32_MAX;
	uint32_t output = UINT32_MAX;
	bool reverb_send = false;
	std::array<float, cp::kMaxPluginAttributes> attributes{};
	uint32_t override_mask = 0;
	std::array<float, cp::kMaxPluginAttributes> overrides{};
	Biquad biquad[2];
	OnePoleLowPass damping[2];
	float gain_prev = 0.0f;
	bool gain_started = false;
	float pan_prev[2] = { 0.0f, 0.0f };
	bool pan_started = false;
	uint32_t delay_begin = 0, delay_frames = 0, delay_write = 0;
	float attribute(uint8_t p_attribute) const {
		return (override_mask >> p_attribute) & 1u ? overrides[p_attribute] : attributes[p_attribute];
	}
};

struct OutputState {
	float curve_gain_db = 0.0f;
	float cone_db = 0.0f;
	float send_gain = 0.0f;
	uint16_t voice = 0xffff;
	bool bypass_headroom = false;
};

struct PendingEvent {
	uint16_t reg = 0;
	uint32_t frame = 0;
	bool stop = false;
};

struct AttributeDrive {
	uint32_t plugin = 0;
	uint8_t attribute = 0;
	ca::SoundPort port;
};

struct Geometry {
	float distance = 0.0f;
	float azimuth = 0.0f;
	float elevation = 0.0f;
	float velocity = 0.0f;
	float relative_velocity = 0.0f;
	float cone_angle_deg = 0.0f;
};

}

namespace bf3::audio {

struct PatchInstance::State {
	std::shared_ptr<const core::audio::SoundProgram> program;
	std::shared_ptr<const core::audio::WaveBank> bank;
	AudioTuning tuning;
	uint64_t seed = 0;
	uint32_t mix_rate = 48000;
	uint64_t clock = 0;
	bool clock_set = false;

	std::vector<float> registers;
	std::vector<float> previous;
	bool evaluated = false;
	std::vector<uint16_t> event_registers;
	std::vector<detail::NodeState> nodes;
	std::vector<detail::GroupState> groups;
	std::vector<detail::SamplerState> samplers;
	std::vector<detail::SlotState> slots;
	std::vector<core::audio::WaveId> wave_ids;
	std::vector<std::pair<uint32_t, uint32_t>> wave_variations;
	std::vector<float> mixer_values;
	detail::Geometry geometry;
	PatchHaptics haptics;
	uint16_t stop_register = core::audio::kNoRegister;
	bool stop_seen = false;
	bool any_event = false;

	std::array<detail::PendingEvent, kMaxEventsPerBlock> events{};
	uint32_t event_count = 0;
	std::array<detail::PendingEvent, kMaxEventsPerBlock> deferred{};
	uint32_t deferred_count = 0;

	std::vector<detail::PluginState> plugins;
	std::vector<detail::OutputState> outputs;
	std::vector<detail::AttributeDrive> drives;
	std::vector<float> voice_buffers;
	std::vector<float> delay_pool;
	std::vector<uint8_t> voice_tapped;
	std::vector<uint8_t> voice_live;
	std::vector<float> voice_cut;
	std::vector<float> voice_pan_step;
	std::array<std::array<float, detail::kMaxSubBlock>, kMaxVoiceTaps> tap_buffers{};
	std::array<VoiceTap *, kMaxVoiceTaps> taps{};
	uint32_t tap_count = 0;
	SamplerTrace *trace = nullptr;

	PatchInstanceStats stats;
	HdrVoiceWindow hdr;
	Vec3 emitter_position;
	uint64_t loudness_hint = 0;
	float hdr_send_gain(const detail::OutputState &p_o, float p_hdr_window_db) const;

	bool voice_quiet(const core::audio::SoundVoice &p_voice, uint16_t p_from, const float *p_left, const float *p_right, uint32_t p_frames) const;
	void quiet_plugin(detail::PluginState &r_ps, float p_rate, float p_window_db, float p_hdr_window_db);

	float edge[2] = { 0.0f, 0.0f };
	float head[2] = { 0.0f, 0.0f };
	uint64_t abrupt = 0;
	float abrupt_max = 0.0f;

	float cut_fade = 1.0f;
	float cut_fade_step = 0.0f;
	float cut_fade_target = 1.0f;
	uint32_t cut_fade_left = 0;
	float cut_fade_at(uint32_t p_i) const {
		return p_i < cut_fade_left ? cut_fade + cut_fade_step * float(p_i + 1) : cut_fade_target;
	}

	float *voice(uint16_t p_voice, uint32_t p_channel) { return voice_buffers.data() + (size_t(p_voice) * 2 + p_channel) * detail::kMaxSubBlock; }
	float read(const core::audio::SoundPort &p_port) const { return p_port.reg == core::audio::kNoRegister ? p_port.constant : registers[p_port.reg]; }
	float read_previous(const core::audio::SoundPort &p_port) const {
		if (!evaluated) {
			return 0.0f;
		}
		return p_port.reg == core::audio::kNoRegister ? p_port.constant : previous[p_port.reg];
	}
	bool rose(const core::audio::SoundPort &p_port) const {
		return read(p_port) >= 0.5f && !(read_previous(p_port) >= 0.5f);
	}
	bool events_repeat(uint32_t p_from, uint32_t p_until_frame) const;
	void write(const core::audio::SoundPort &p_port, float p_value) {
		if (p_port.reg != core::audio::kNoRegister) {
			registers[p_port.reg] = std::isfinite(p_value) ? p_value : 0.0f;
		}
	}
	const core::audio::SoundPort &port(const core::audio::SoundNode &p_node, uint32_t p_index) const { return program->ports[p_node.ports_begin + p_index]; }
	float flt(const core::audio::SoundNode &p_node, uint32_t p_index) const { return program->floats[p_node.floats_begin + p_index]; }
	const core::audio::SoundNode &child(const core::audio::SoundNode &p_node, uint32_t p_index) const {
		return program->child_nodes[program->children[p_node.children_begin + p_index]];
	}
	uint32_t child_index(const core::audio::SoundNode &p_node, uint32_t p_index) const { return program->children[p_node.children_begin + p_index]; }
	uint32_t global_plugin(core::audio::SoundPluginRef p_ref) const {
		return p_ref.valid() ? program->voices[p_ref.voice].plugins_begin + p_ref.plugin : UINT32_MAX;
	}
	void trace_entry(uint64_t p_frame, uint32_t p_node, uint8_t p_slot, uint16_t p_segment, SamplerTraceKind p_kind, float p_pitch, uint64_t p_prev_end);

	void evaluate_program(uint64_t p_frame);

	void advance(double p_seconds);

	void sampler_events(uint32_t p_node_index, const core::audio::SoundNode &p_node, uint64_t p_frame);
	void render_slot(detail::SamplerState &r_sampler, uint32_t p_slot, float *r_left, float *r_right, uint64_t p_first_frame, uint32_t p_frames);
	bool sampler_busy(const detail::SamplerState &p_sampler) const;
};

}
