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
// Seeds: the game derives p_seed from replicated state so every client renders the same sound for the same shot:
// splitmix64(entity id << 32 | the weapon's burst sequence number) for weapons, the effect instance id for one-shot
// effect sounds (design §7.2). Tests use fixed seeds.
//
// AreaType (MixerReceive from sound/mixers/areatype_mixer): A2 feeds 0 (design §6.3, plan M2a A2); set_mixer_value
// is the seam A4 drives.
//
// Sampler model (design §6.3; [inferred], open question 4). A variation's segments split into an intro
// [0, FirstLoopSegmentIndex), the loop [FirstLoop, LastLoop] and a tail (LastLoop, end) (the M16A3's hifi_lmg_m4_wave:
// 1 / 12 / 1 segments; corebassclose_lmg_08_wave has no tail; lfe_wave is one segment that is its own loop).
//   - Trigger starts segment 0 on a free slot, Delay seconds after the event frame (Delay is latched at Trigger).
//   - Stepped mode (Step connected and EnableStep >= 0.5 at the event): each Step starts the next loop segment
//     (in order, or shuffled) on the other slot at the Step frame + the latched Delay; a slot whose segment ends
//     goes idle (no automatic advance), so the rhythm is exactly the Step rhythm.
//   - Unstepped mode (Step unconnected, or EnableStep < 0.5): LtRelease plays the loop segments back to back on the
//     same slot (next segment starts on the frame the previous one ends) until Release; LtForward does the same and
//     ignores Release until Stop/kill; LtNone plays the segments once, in order.
//   - Release (LtRelease): the playing segment finishes, then the tail plays (stepped and unstepped alike; with an
//     idle slot it starts at the Release frame); no tail = the sampler ends with its segment. LtInstantRelease fades
//     out over release_fade_ms.
//   - Pitch = BasePitch x Pitch port x MasterPitch, read per sub-block; the M16A3's pitch register v30 = v28 x v29
//     where v28 is drawn once per burst (its NumberGenerator is triggered by the gated Fire, not by Step).
// DeMux: output index = floor(Value) clamped to [0, outputs - 1] (WrapValue false) or modulo outputs (true)
// [inferred]. On the M16A3 that makes the EnableStep parameter the fire mode: v119 = SimpleValueSelector(#1365968171
// + EnableStep) in [0, 1.9, ...]; with 0 the gated Fire is routed to the release register v100 in the same evaluation
// (single shot: Trigger and Release together), with 1.9 the stop event is routed to v100 through
// ParameterFilterLinear(v119, release 0.3/s) -> DeMux, so EnableStep must stay at the weapon's fire mode for the life
// of the instance (dropping it to 0 lets the filtered index fall below 1 after 3 s and the stop would no longer
// release the loops). weapon_sound.h sets it once from the fire mode.

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
	uint64_t first_frame = 0; // MixerClock frame of the buffer's first frame (taps and the sampler trace use it)
};

// Analysis tap on a set of voices (gates G7/G8, design §9): after each sub-block, the outputs of the listed voices
// (each voice's plugin chain output, before its sends), summed to mono, are handed to on_voices on the audio
// thread (no blocking, no allocation). The M16A3 "core" tap is voices 26-29 (the stepped corebassdistant and
// corebassclose samplers), the "noise" tap voice 14 (the submix behind the distance HPF v46).
inline constexpr size_t kMaxTapVoices = 16;
inline constexpr size_t kMaxVoiceTaps = 4; // taps per instance (the M16A3 check uses two: core and noise)
class VoiceTap {
public:
	virtual ~VoiceTap() = default;
	virtual void on_voices(const float *p_mono, uint32_t p_frames, uint64_t p_first_frame) = 0;
};

// One sampler segment event, for the model checks of gate G7 (the unstepped noise loop plays back to back; Steps
// start on their frames; the tail follows the release).
enum class SamplerTraceKind : uint8_t { Trigger, Step, Advance, Tail, End };
struct SamplerTraceEntry {
	uint64_t frame = 0; // MixerClock frame the segment starts (End: the frame the slot went idle)
	uint32_t node = 0; // index into SoundProgram::nodes
	uint8_t slot = 0;
	uint16_t segment = 0;
	SamplerTraceKind kind = SamplerTraceKind::Trigger;
	float pitch = 1.0f;
	// Advance / Tail on the same slot: the frame the previous segment's last output frame was followed by this
	// segment's first (equal to `frame`: the resampler crosses the boundary without a gap); 0 otherwise.
	uint64_t prev_end = 0;
	core::audio::WaveId wave = core::audio::kNoWave; // the bank wave and variation the segment belongs to
	uint32_t variation = 0;
};
// Caller-owned trace storage: entries is sized before use and never grows; the audio side writes entries[count++]
// and counts dropped once it is full.
struct SamplerTrace {
	std::vector<SamplerTraceEntry> entries;
	uint32_t count = 0;
	uint64_t dropped = 0;
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
	uint64_t sampler_triggers = 0; // Sampler Trigger events taken (the mixer returns a never-stopped one-shot once it has played)
	uint64_t unknown_inputs = 0; // set_parameter / post_event with a hash the program does not have
};

// M2b au [a3] (docs/design/m2b.md §6.1, R5; docs/design/m2b-au.md): the HDR window one mixer block applies to the
// instance's EnableHdr outputs (MixerCore sets it before process_block when MixerConfig::hdr is set). Inactive: the
// fixed window of AudioTuning (window_top_db, window_headroom_db) for every output, as in M2a.
struct HdrVoiceWindow { // M2b au [a3]
	bool active = false;
	bool suspended = false; // below the window's bottom: the HDR sends ramp to silence, the instance keeps advancing
	float top_db = 0.0f;
	float headroom_db = 0.0f;
	float overshoot_db = 0.0f; // AllowedOvershoot: an output louder than top + this sounds at top + this
};
// M2b au [a3]: what the voice cull reads of an instance (voice_cull.h): the loudness at the listener = the patch's
// Loudness + the loudest output's distance curve at the current geometry + Gain + its last cone attenuation, over the
// EnableHdr outputs (hdr true) or, without one, over every output (hdr false).
struct VoiceCullInfo { // M2b au [a3]
	float loudness_db = 0.0f;
	bool hdr = false;
	float distance_m = 0.0f;
	Vec3 position;
	uint32_t voices = 0; // busy sampler slots (playing, or armed to start)
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
	// Test hook (gates G7/G8 HPF checks): pin a plugin attribute to p_value instead of what the program drives
	// (nullopt restores it). Not used by the game. Call it on the thread that owns the instance: before the Start
	// command hands it to the mixer, or offline.
	void override_plugin_attribute(core::audio::SoundPluginRef p_plugin, uint8_t p_attribute, std::optional<float> p_value);
	// Analysis hooks (gates G7/G8), same threading rule. p_voices is copied (at most kMaxTapVoices). p_tap and
	// r_trace are caller-owned and must outlive the instance's time in the mixer; the caller reads the trace once
	// the instance is finished (it outlives the instance, which reclaim() destroys).
	void set_voice_tap(std::span<const uint16_t> p_voices, VoiceTap *p_tap); // replaces every tap with this one
	void add_voice_tap(std::span<const uint16_t> p_voices, VoiceTap *p_tap); // up to kMaxVoiceTaps; more are ignored
	bool stop_seen() const; // the program's stop event has pulsed (true when it has none)
	void set_sampler_trace(SamplerTrace *r_trace);
	const core::audio::SoundProgram &program() const;
	const PatchInstanceStats &stats() const;
	void set_hdr_window(const HdrVoiceWindow &p_window); // M2b au [a3]: audio thread, before process_block
	VoiceCullInfo cull_info() const; // M2b au [a3]: audio thread
	// PT1 rld [live outputs] (docs/design/pt1-rld.md): the outputs (indices into SoundProgram::outputs) the cull counts
	// while no sampler slot is busy yet (the first block after a start). Empty: every output, as before. Call it before
	// the Start command hands the instance to the mixer.
	void set_loudness_outputs(std::span<const uint16_t> p_outputs);
	// PT1 rld [no stop] (docs/design/pt1-rld.md): treat the program's stop event as seen: the instance ends when its
	// samplers are idle, and the mixer's Stop posts nothing. For one-shots started by another event (Reload, Action,
	// Bolt) of a weapon patch, whose stop event is the trigger release: on a single-fire patch it fires a round.
	// Before the Start command, like set_loudness_outputs.
	void skip_stop_event();

	PatchInstance(const PatchInstance &) = delete;
	PatchInstance &operator=(const PatchInstance &) = delete;

	struct State; // patch_state.h (internal)

private:
	PatchInstance() = default;
	void render(uint64_t p_frame, uint32_t p_offset, uint32_t p_frames, BusBuffers &r_buses);
	std::unique_ptr<State> state_;
	mutable std::vector<SamplerSlotState> slot_views_; // sampler_slots() storage, sized in create()
};

} // namespace bf3::audio
