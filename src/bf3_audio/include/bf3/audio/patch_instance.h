#pragma once
// One playing instance of a compiled sound patch (M2a audio A2; docs/design/m2a-audio.md §6.2-§6.4): the per-instance
// register file and node state over a shared, immutable bf3::core::audio::SoundProgram, the control evaluation, and
// the rendering of its voice graph into bus buffers.
//
// Threads: created and destroyed on the game thread (create allocates every buffer the instance will use); handed
// to the mixer through its command ring and from then on touched only by the audio thread, where nothing allocates,
// locks or throws (design §7).
//
// Control rate: one evaluation per sub-block. A mix block (512 frames) is split at the frame offsets of the events
// posted for it; at each split the pending events pulse (their registers read 1 for exactly one evaluation, then 0)
// and the program runs once in its topological order. Timers, Adsr, Lfo and ParameterFilterLinear advance by the
// sub-block's duration. So an event lands on its frame (sample-accurate onsets, gate G7: 75 ms = 3600 frames at
// 48 kHz +- 1 frame), and the control cost is bounded by (events per block + 1) x program size.
//
// Randomness (NumberGenerator, Lfo StartAtRandomValue, WaveSwitcher IsRandom, Sampler variation choice and
// ShuffleSegments): a counter-based generator per node, seeded from (instance seed, node index, draw count), so a
// given seed and event sequence renders bit-identically (gate G7 determinism), on any thread.
//
// AreaType (MixerReceive from sound/mixers/areatype_mixer): A2 feeds 0 (design §6.3, plan M2a A2); set_mixer_value
// is the seam A4 drives.

#include "bf3/audio/audio_config.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kMaxEventsPerBlock = 32; // further events in one block are deferred to the next one
inline constexpr uint32_t kMaxBusChannels = 2; // A1/A2 render every bus folded to stereo

// Positions in metres, Frostbite axes (+Y up, +Z forward). Sent by the game per frame; the instance derives the
// Physics node's Distance, Azimuth, ElevationAngle, Velocity and RelativeVelocity from them.
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

// Stereo bus buffers for one sub-block, indexed by core::audio::MasterBus; the mixer core sums and folds them.
struct BusBuffers {
	float *channels[size_t(core::audio::MasterBus::Unknown)][kMaxBusChannels] = {};
	uint32_t frames = 0;
};

// What the instance is doing, for tests, the oracle gate and the debug overlay. Values are those of the last
// evaluation.
struct SamplerSlotState {
	bool playing = false;
	core::audio::WaveId wave = core::audio::kNoWave;
	uint32_t variation = 0;
	uint16_t segment = 0;
	uint32_t frame_in_segment = 0;
	float pitch = 1.0f; // final playback-rate factor (BasePitch x Pitch x MasterPitch)
	float gain = 0.0f; // linear, after Gai0
};

// CameraShake / PadRumble node outputs (gap audio §11.1: they feed the game, not the mix). Summed over the nodes.
struct PatchHaptics {
	float shake_pitch = 0.0f, shake_roll = 0.0f, shake_yaw = 0.0f;
	float rumble_low = 0.0f, rumble_high = 0.0f;
};

struct PatchInstanceStats {
	uint64_t evaluations = 0;
	uint64_t deferred_events = 0; // events past kMaxEventsPerBlock
	uint64_t starved_slots = 0; // a sampler slot whose wave is not resident (counted, silent)
	uint64_t decode_errors = 0; // a stream that failed mid-play (counted, slot stops)
};

class PatchInstance {
public:
	// Binds the program's waves to p_bank (a wave the bank lacks plays silence and counts starved_slots) and the
	// game inputs to their defaults. Fails only when the program and bank are inconsistent (a wave index outside
	// the program), never on missing waves.
	static core::Result<std::unique_ptr<PatchInstance>> create(std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, uint64_t p_seed, uint32_t p_mix_rate);
	~PatchInstance();

	// Game inputs (AudioGraphParameter / AudioGraphEvent by NameHash). Unknown hashes are ignored and counted.
	void set_parameter(uint32_t p_name_hash, float p_value);
	void post_event(uint32_t p_name_hash, uint32_t p_frame_in_block);
	void set_mixer_value(uint32_t p_name_hash, float p_value); // MixerReceive sources (AreaType)
	void set_geometry(const EmitterState &p_emitter, const ListenerState &p_listener);

	// Evaluates and renders one block of p_buses.frames frames (<= 512), adding into p_buses.
	void process_block(BusBuffers &r_buses);
	// Control only, no audio (oracle gate G5, unit tests): evaluates once with the posted events at frame 0 and
	// advances the control clock by p_seconds.
	void evaluate(double p_seconds);

	// Done: no slot playing, no Adsr or Timer running, and the stop event (or none) seen.
	bool finished() const;

	float value(uint16_t p_register) const;
	std::span<const float> registers() const;
	// The value a LinkedPluginAttribute currently drives (e.g. the HighPassIir2 frequency on the M16A3's voice 14).
	float plugin_attribute(core::audio::SoundPluginRef p_plugin, uint8_t p_attribute) const;
	// The amplitude a MultiCrossfader group currently applies (gate G5 bands).
	float crossfader_group_amplitude(uint32_t p_node, uint32_t p_group) const;
	std::span<const SamplerSlotState> sampler_slots(uint32_t p_node) const;
	// The attenuation (dB) an output node currently applies from its curve and Gain.
	float output_gain_db(uint32_t p_output) const;
	PatchHaptics haptics() const;
	// Test hook (gate G7 HPF check): pin a plugin attribute to p_value instead of what the program drives
	// (nullopt restores it). Not used by the game.
	void override_plugin_attribute(core::audio::SoundPluginRef p_plugin, uint8_t p_attribute, std::optional<float> p_value);
	const core::audio::SoundProgram &program() const;
	const PatchInstanceStats &stats() const;

	PatchInstance(const PatchInstance &) = delete;
	PatchInstance &operator=(const PatchInstance &) = delete;

private:
	PatchInstance() = default;
	struct State;
	std::unique_ptr<State> state_;
};

} // namespace bf3::audio
