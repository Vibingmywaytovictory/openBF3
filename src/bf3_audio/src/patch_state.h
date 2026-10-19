#pragma once
// Internal state of a PatchInstance (patch_instance.h), shared by patch_instance.cpp (creation, API, audio plan),
// patch_nodes.cpp (the control program) and patch_sampler.cpp (the Sampler model). Everything is sized in create();
// the audio-thread paths only index into it.

#include "bf3/audio/dsp.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/wave_bank.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bf3::audio::detail {

namespace ca = core::audio;
namespace cp = core::audio::port;

inline constexpr uint32_t kSlotInputFrames = 64; // decoded frames a slot buffers from its StreamDecoder
inline constexpr uint32_t kMaxSubBlock = 512;
inline constexpr uint16_t kNoSegment = 0xffff;

// Counter-based randomness (patch_instance.h): splitmix64 over (seed, node, draw).
uint64_t splitmix64(uint64_t p_x);
double uniform01(uint64_t p_seed, uint32_t p_node, uint32_t p_draw);

enum class AdsrPhase : uint8_t { Idle, Attack, Decay, Sustain, Release };

struct NodeState {
	float value = 0.0f; // Adsr value; NumberGenerator Y; ParameterFilterLinear value; Timer elapsed; EventGate cool-down left
	double phase = 0.0; // Lfo phase in cycles
	float release_rate = 0.0f; // Adsr: units per second while releasing
	float latched = 0.0f; // MultiCrossfader latched control
	uint32_t draws = 0; // random draws so far
	uint32_t index = 0; // WaveSwitcher index; Sampler: index into samplers
	uint8_t mode = 0; // AdsrPhase; Timer running; ParameterFilterLinear initialised; MultiCrossfader latched
	uint8_t pulse = 0; // Adsr Finished / Timer Tick due at the next evaluation
};

struct GroupState { // per MultiCrossfaderGroup child record
	float amplitude = 0.0f;
	bool started = false;
};

// Where a segment's playback stands on a slot.
struct SlotPlay {
	bool active = false; // decoding (or flushing the resampler) a segment
	ca::WaveId wave = ca::kNoWave;
	uint32_t variation = 0;
	uint16_t segment = 0;
	uint8_t channels = 1;
	uint32_t rate = 48000;
	bool source_done = false; // the segment's stream is exhausted and nothing continues it
	uint8_t flush = 0; // zero frames fed since source_done
	uint32_t frames_out = 0; // output frames since the segment started
	ca::StreamDecoder decoder;
	std::array<float, kSlotInputFrames * ca::kMaxStreamChannels> input{};
	uint32_t input_count = 0, input_pos = 0;
	float history[4][2] = {};
	double frac = 0.0;
};

struct PendingStart {
	bool armed = false;
	uint64_t frame = 0; // absolute MixerClock frame
	uint16_t segment = 0;
	SamplerTraceKind kind = SamplerTraceKind::Trigger;
	float offset_seconds = 0.0f; // Sampler Offset (Trigger only)
};

struct SlotState {
	SlotPlay play;
	PendingStart pending;
	float fade = 1.0f; // takeover / instant-release fade
	float fade_step = 0.0f; // per frame (negative: fading out)
	bool fade_kill = false; // ends the slot when the fade reaches 0
	uint16_t voice = 0xffff; // the slot's voice (SndPlayer ref)
	uint32_t gain_plugin = UINT32_MAX; // global plugin index of the slot's Gai0
	uint64_t outputs_mask = ~uint64_t(0); // PT1 rld [live outputs]: bit o = the slot's voice feeds output o (all: unknown)
};

struct SamplerState {
	uint32_t node = 0;
	uint32_t slots_begin = 0, slots_count = 0; // into State::slots
	ca::SamplerLoop loop = ca::SamplerLoop::Release;
	bool shuffle = false;
	bool step_connected = false;
	bool active = false; // triggered and not yet ended
	bool released = false;
	bool stepped = false; // latched at Trigger
	float delay_seconds = 0.0f; // latched at Trigger
	ca::WaveId wave = ca::kNoWave;
	uint32_t variation = 0;
	int32_t first_loop = -1, last_loop = -1; // clamped to the variation; -1: no loop
	uint16_t segments = 0;
	uint8_t last_slot = 0;
	uint16_t last_segment = kNoSegment;
	uint32_t draws = 0;
	bool finished_pulse = false;
	float amplitude = 0.0f, amplitude_prev = 0.0f; // Gai0 of every slot
	float pitch = 1.0f; // BasePitch x Pitch x MasterPitch
	bool instant_release = false;
};

enum class PluginRole : uint8_t {
	Pass, // Rsp0, Pau0, Sub0, VuM0 and unknown FourCCs
	Slot, // SnP1 of a sampler slot (index = slot)
	SlotGain, // Gai0 of a sampler slot (index = slot)
	Gain, // Gai0 driven by a Gain node / link (attribute 0, linear)
	HighPass,
	LowPass,
	BandPass,
	Peaking,
	Delay,
	Damping, // LPB0
	Pan, // Pn21 (index = output, or UINT32_MAX)
	Send, // Sen0 / Rou0 (index = connection)
};

struct PluginState {
	PluginRole role = PluginRole::Pass;
	uint32_t index = UINT32_MAX;
	uint32_t output = UINT32_MAX; // Send: the output whose main send this is; Damping/Pan: their output
	bool reverb_send = false; // dropped until A4
	std::array<float, cp::kMaxPluginAttributes> attributes{};
	uint32_t override_mask = 0;
	std::array<float, cp::kMaxPluginAttributes> overrides{};
	Biquad biquad[2];
	OnePoleLowPass damping[2];
	float gain_prev = 0.0f; // Gain / Send ramps
	bool gain_started = false;
	uint32_t delay_begin = 0, delay_frames = 0, delay_write = 0; // into State::delay_pool (stereo interleaved)
	float attribute(uint8_t p_attribute) const {
		return (override_mask >> p_attribute) & 1u ? overrides[p_attribute] : attributes[p_attribute];
	}
};

struct OutputState {
	float curve_gain_db = 0.0f; // curve + Gain (output_gain_db)
	float cone_db = 0.0f;
	float send_gain = 0.0f; // linear, the main send of the output voice
	uint16_t voice = 0xffff;
	bool bypass_headroom = false; // PT1 rld [bypass headroom]: the output node's BypassHeadroom port, read each evaluation
};

struct PendingEvent {
	uint16_t reg = 0;
	uint32_t frame = 0;
	bool stop = false;
};

// A plugin attribute driven by a port: the audio node's own attribute ports, then LinkedPluginAttributes (later
// drives of the same attribute win).
struct AttributeDrive {
	uint32_t plugin = 0;
	uint8_t attribute = 0;
	ca::SoundPort port;
};

struct Geometry {
	float distance = 0.0f;
	float azimuth = 0.0f; // radians, positive right
	float elevation = 0.0f; // radians
	float velocity = 0.0f;
	float relative_velocity = 0.0f; // positive: separating
	float cone_angle_deg = 0.0f; // angle between the emitter's forward and the listener
};

} // namespace bf3::audio::detail

namespace bf3::audio {

struct PatchInstance::State {
	std::shared_ptr<const core::audio::SoundProgram> program;
	std::shared_ptr<const core::audio::WaveBank> bank;
	AudioTuning tuning;
	uint64_t seed = 0;
	uint32_t mix_rate = 48000;
	uint64_t clock = 0; // MixerClock frame of the next evaluation (control clock)
	bool clock_set = false; // process_block takes the clock from the first BusBuffers::first_frame

	// Control program
	std::vector<float> registers;
	std::vector<uint16_t> event_registers; // registers of AudioGraphEvents (pulses)
	std::vector<detail::NodeState> nodes;
	std::vector<detail::GroupState> groups; // by child_nodes index
	std::vector<detail::SamplerState> samplers;
	std::vector<detail::SlotState> slots;
	std::vector<core::audio::WaveId> wave_ids; // program wave index -> bank wave (kNoWave: not in the bank)
	std::vector<std::pair<uint32_t, uint32_t>> wave_variations; // program wave index -> playable variation range (English)
	std::vector<float> mixer_values; // by program mixer_inputs index
	detail::Geometry geometry;
	PatchHaptics haptics;
	uint16_t stop_register = core::audio::kNoRegister;
	bool stop_seen = false;
	bool any_event = false;

	// Events of the block being processed, and those deferred to the next one
	std::array<detail::PendingEvent, kMaxEventsPerBlock> events{};
	uint32_t event_count = 0;
	std::array<detail::PendingEvent, kMaxEventsPerBlock> deferred{};
	uint32_t deferred_count = 0;

	// Audio plan
	std::vector<detail::PluginState> plugins; // by global plugin index (voice.plugins_begin + plugin)
	std::vector<detail::OutputState> outputs;
	std::vector<detail::AttributeDrive> drives;
	std::vector<float> voice_buffers; // per voice: 2 planar channels of kMaxSubBlock
	std::vector<float> delay_pool;
	std::vector<uint8_t> voice_tapped; // per voice: bit t = tap t takes it
	std::vector<uint8_t> voice_live; // integ/m2b-w1 [lazy zero]: per voice, 1 once render() zeroed its sub-block range
	std::array<std::array<float, detail::kMaxSubBlock>, kMaxVoiceTaps> tap_buffers{};
	std::array<VoiceTap *, kMaxVoiceTaps> taps{};
	uint32_t tap_count = 0;
	SamplerTrace *trace = nullptr;

	PatchInstanceStats stats;
	HdrVoiceWindow hdr; // M2b au [a3]: set by the mixer per block (patch_instance.h)
	Vec3 emitter_position; // M2b au [a3]: the last set_geometry's emitter (the cull's DuplicateSpawn distance)
	uint64_t loudness_hint = 0; // PT1 rld [live outputs]: set_loudness_outputs (0: none)
	float hdr_send_gain(const detail::OutputState &p_o, float p_hdr_window_db) const; // PT1 rld [bypass headroom]: an EnableHdr output's send gain
	// M2b au [quiet] (patch_instance.cpp, docs/design/m2b-au.md "Quiet voices"): a voice whose buffer is silent and whose
	// remaining plugins are at rest skips their per-sample work; quiet_plugin keeps their ramp and coefficient state
	// exactly where the full path would leave it, so the render is unchanged.
	bool voice_quiet(const core::audio::SoundVoice &p_voice, uint16_t p_from, const float *p_left, const float *p_right, uint32_t p_frames) const; // M2b au [quiet]
	void quiet_plugin(detail::PluginState &r_ps, float p_rate, float p_window_db, float p_hdr_window_db); // M2b au [quiet]

	float *voice(uint16_t p_voice, uint32_t p_channel) { return voice_buffers.data() + (size_t(p_voice) * 2 + p_channel) * detail::kMaxSubBlock; }
	float read(const core::audio::SoundPort &p_port) const { return p_port.reg == core::audio::kNoRegister ? p_port.constant : registers[p_port.reg]; }
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

	// patch_nodes.cpp: one evaluation of the control program at absolute frame p_frame; then the plugin attributes.
	void evaluate_program(uint64_t p_frame);
	// Advances the time-based nodes by p_seconds after an evaluation.
	void advance(double p_seconds);
	// patch_sampler.cpp
	void sampler_events(uint32_t p_node_index, const core::audio::SoundNode &p_node, uint64_t p_frame);
	void render_slot(detail::SamplerState &r_sampler, uint32_t p_slot, float *r_left, float *r_right, uint64_t p_first_frame, uint32_t p_frames);
	bool sampler_busy(const detail::SamplerState &p_sampler) const;
};

} // namespace bf3::audio
