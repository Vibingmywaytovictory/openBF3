#pragma once
// The Godot-free mixer core (M2a audio A1; docs/design/m2a-audio.md §6.5, §7). The Godot AudioStreamPlayback
// (bf3_godot audio_stream.h) calls mix() from Godot's audio thread; tests and the offline renderer call it from
// their own thread. Output: interleaved stereo float, 48 kHz, in blocks of kBlockFrames.
//
// Handoff (game thread -> audio thread), lock-free and allocation-free on the audio side:
//   - commands: a single-producer single-consumer ring of POD MixerCommands (fixed capacity, set at construction);
//     a full ring refuses the push and counts ring_overflows (the game retries next frame), never blocks;
//   - instances: built on the game thread (PatchInstance::create), passed by pointer in a Start command; finished
//     or stopped ones come back through a second SPSC ring and are destroyed by reclaim() on the game thread;
//   - banks: a new WaveBank (level change) is published by pointer in a SetBank command with an epoch; the game
//     side keeps the shared_ptr until the audio thread's acknowledged epoch passes it (reclaim()).
// Commands carry a target mix frame (MixerClock): a command for a frame inside the next block lands at that
// offset (post_event's frame_in_block), a late one at offset 0 and counts late_commands.
//
// Underrun accounting (gate G8 "no underruns over 30 s"): short_blocks (mix() returned fewer frames than asked),
// late_blocks (a block took longer than its real-time duration to compute), starved_voices and ring_overflows.

#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/wave_bank.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kBlockFrames = 512; // Godot's mix block (AudioServer buffer_size)
inline constexpr uint32_t kMixRate = 48000; // AudioSystemAsset.SampleRate [verified]; project.godot sets the driver to it

struct MixerConfig {
	uint32_t mix_rate = kMixRate;
	uint32_t max_instances = 64;
	uint32_t command_capacity = 1024; // power of two
	float output_gain_db = 0.0f;
};

using InstanceHandle = uint32_t; // allocated by the game side, never reused within a mixer's lifetime
inline constexpr InstanceHandle kNoInstance = 0;

enum class MixerCommandType : uint8_t {
	Start, // instance (owned pointer), handle
	Stop, // handle: post the program's stop event, finish when silent
	Kill, // handle: drop now (short fade)
	SetParameter, // handle, name_hash, value
	PostEvent, // handle, name_hash, at_frame
	SetMixerValue, // name_hash, value: every instance (AreaType)
	SetEmitter, // handle, emitter
	SetListener, // listener
	SetBank, // bank pointer, epoch
};

struct MixerCommand {
	MixerCommandType type = MixerCommandType::Stop;
	InstanceHandle handle = kNoInstance;
	uint32_t name_hash = 0;
	float value = 0.0f;
	uint64_t at_frame = 0; // MixerClock frame; 0 = as soon as possible
	PatchInstance *instance = nullptr; // Start: ownership moves to the mixer
	const core::audio::WaveBank *bank = nullptr; // SetBank
	uint64_t epoch = 0;
	EmitterState emitter;
	ListenerState listener;
};

struct MixerStats {
	std::atomic<uint64_t> frames{ 0 }; // MixerClock: frames mixed since construction
	std::atomic<uint64_t> blocks{ 0 };
	std::atomic<uint64_t> short_blocks{ 0 };
	std::atomic<uint64_t> late_blocks{ 0 };
	std::atomic<uint64_t> late_commands{ 0 };
	std::atomic<uint64_t> ring_overflows{ 0 };
	std::atomic<uint64_t> starved_voices{ 0 };
	std::atomic<uint32_t> active_instances{ 0 };
	std::atomic<uint32_t> peak_instances{ 0 };
	std::atomic<uint64_t> max_block_ns{ 0 };
	std::atomic<uint64_t> acked_epoch{ 0 };
};

// Receives every mixed block on the audio thread (WAV recorder, analysis). Must not block or allocate.
class MixTap {
public:
	virtual ~MixTap() = default;
	virtual void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) = 0;
};

class MixerCore {
public:
	explicit MixerCore(const MixerConfig &p_config);
	~MixerCore(); // destroys the instances it still owns; call from the game thread after the audio thread stopped

	// Game thread.
	InstanceHandle next_handle();
	bool push(const MixerCommand &p_command); // false: ring full (counted)
	uint64_t clock() const; // frames mixed so far (relaxed)
	void reclaim(); // destroy returned instances, release banks below the acked epoch
	const MixerStats &stats() const { return stats_; }

	// Audio thread. Fills p_frames frames (any count; split internally into kBlockFrames blocks); always fills the
	// whole buffer (silence when idle) and returns p_frames.
	uint32_t mix(float *r_interleaved_stereo, uint32_t p_frames);
	void set_tap(MixTap *p_tap); // set before the audio thread starts or while it is stopped

	MixerCore(const MixerCore &) = delete;
	MixerCore &operator=(const MixerCore &) = delete;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	MixerStats stats_;
};

} // namespace bf3::audio
