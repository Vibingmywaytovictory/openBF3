#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kMaxEventsPerBlock = 32;
inline constexpr uint32_t kMaxBusChannels = 2;

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};
struct EmitterState {
	Vec3 position;
	Vec3 velocity;
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
};
struct ListenerState {
	Vec3 position;
	Vec3 velocity;
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
	Vec3 up{ 0.0f, 1.0f, 0.0f };
};

struct BusBuffers {
	float *channels[size_t(core::audio::MasterBus::Unknown)][kMaxBusChannels] = {};
	uint32_t frames = 0;
	uint64_t first_frame = 0;
};

inline constexpr size_t kMaxTapVoices = 16;
inline constexpr size_t kMaxVoiceTaps = 4;
class VoiceTap {
public:
	virtual ~VoiceTap() = default;
	virtual void on_voices(const float *p_mono, uint32_t p_frames, uint64_t p_first_frame) = 0;
};

enum class SamplerTraceKind : uint8_t { Trigger, Step, Advance, Tail, End };
struct SamplerTraceEntry {
	uint64_t frame = 0;
	uint32_t node = 0;
	uint8_t slot = 0;
	uint16_t segment = 0;
	SamplerTraceKind kind = SamplerTraceKind::Trigger;
	float pitch = 1.0f;

	uint64_t prev_end = 0;
	core::audio::WaveId wave = core::audio::kNoWave;
	uint32_t variation = 0;
};

struct SamplerTrace {
	std::vector<SamplerTraceEntry> entries;
	uint32_t count = 0;
	uint64_t dropped = 0;
};

struct SamplerSlotState {
	bool playing = false;
	core::audio::WaveId wave = core::audio::kNoWave;
	uint32_t variation = 0;
	uint16_t segment = 0;
	uint32_t frame_in_segment = 0;
	float pitch = 1.0f;
	float gain = 0.0f;
};

struct PatchHaptics {
	float shake_pitch = 0.0f, shake_roll = 0.0f, shake_yaw = 0.0f;
	float rumble_low = 0.0f, rumble_high = 0.0f;
};

struct PatchInstanceStats {
	uint64_t evaluations = 0;
	uint64_t deferred_events = 0;
	uint64_t starved_slots = 0;
	uint64_t decode_errors = 0;
	uint64_t sampler_triggers = 0;
	uint64_t unknown_inputs = 0;
};

struct HdrVoiceWindow {
	bool active = false;
	bool suspended = false;
	float top_db = 0.0f;
	float headroom_db = 0.0f;
	float overshoot_db = 0.0f;

	float alone_top_db = std::numeric_limits<float>::infinity();
};

struct VoiceCullInfo {
	float loudness_db = 0.0f;
	bool hdr = false;
	float distance_m = 0.0f;
	Vec3 position;
	uint32_t voices = 0;
};

struct InstanceEdge {
	float edge = 0.0f;
	float head = 0.0f;
	uint64_t abrupt_gain = 0;
	float abrupt_gain_max = 0.0f;
};

class PatchInstance {
public:

	static core::Result<std::unique_ptr<PatchInstance>> create(std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, uint64_t p_seed, uint32_t p_mix_rate);
	~PatchInstance();

	void set_parameter(uint32_t p_name_hash, float p_value);
	void post_event(uint32_t p_name_hash, uint32_t p_frame_in_block);
	void set_mixer_value(uint32_t p_name_hash, float p_value);
	void set_geometry(const EmitterState &p_emitter, const ListenerState &p_listener);

	void process_block(BusBuffers &r_buses);

	void evaluate(double p_seconds);

	bool finished() const;

	float value(uint16_t p_register) const;
	std::span<const float> registers() const;

	float plugin_attribute(core::audio::SoundPluginRef p_plugin, uint8_t p_attribute) const;

	float crossfader_group_amplitude(uint32_t p_node, uint32_t p_group) const;
	std::span<const SamplerSlotState> sampler_slots(uint32_t p_node) const;

	float output_gain_db(uint32_t p_output) const;
	PatchHaptics haptics() const;

	void override_plugin_attribute(core::audio::SoundPluginRef p_plugin, uint8_t p_attribute, std::optional<float> p_value);

	void set_voice_tap(std::span<const uint16_t> p_voices, VoiceTap *p_tap);
	void add_voice_tap(std::span<const uint16_t> p_voices, VoiceTap *p_tap);
	bool stop_seen() const;
	void set_sampler_trace(SamplerTrace *r_trace);
	const core::audio::SoundProgram &program() const;
	const PatchInstanceStats &stats() const;
	void set_hdr_window(const HdrVoiceWindow &p_window);
	VoiceCullInfo cull_info() const;

	InstanceEdge pt1_edge() const;

	void pt1_fade_to(float p_target, uint32_t p_frames);
	void pt1_set_fade(float p_level);
	bool pt1_fade_done() const;
	float pt1_fade_level() const;

	void set_loudness_outputs(std::span<const uint16_t> p_outputs);

	void skip_stop_event();

	PatchInstance(const PatchInstance &) = delete;
	PatchInstance &operator=(const PatchInstance &) = delete;

	struct State;

private:
	PatchInstance() = default;
	void render(uint64_t p_frame, uint32_t p_offset, uint32_t p_frames, BusBuffers &r_buses);
	std::unique_ptr<State> state_;
	mutable std::vector<SamplerSlotState> slot_views_;
};

}
