#pragma once
// The Godot-free mixer core (M2a audio A1; docs/design/m2a-audio.md §6.5, §7). The Godot AudioStreamPlayback
// (bf3_godot audio_stream.h) calls mix() from Godot's audio thread; tests and the offline renderer call it from
// their own thread. Output: interleaved stereo float, 48 kHz.
//
// Block grid: the core renders in its own kBlockFrames grid from frame 0 (MixerClock) and serves mix(n) for any n
// from that grid through a one-block output FIFO. So the output depends only on the commands and their frames, never
// on how the caller sizes its calls (Godot's _mix call size is not assumed), and an offline replay of the same
// commands is sample-identical (gate G8 compares the Godot recording with one). mix() sets FTZ/DAZ in MXCSR for its
// duration and restores it, so denormal handling does not depend on the calling thread.
//
// Handoff (game thread -> audio thread), lock-free and allocation-free on the audio side:
//   - commands: a single-producer single-consumer ring of POD MixerCommands (fixed capacity, set at construction);
//     a full ring refuses the push and counts ring_overflows (the game retries next frame), never blocks;
//   - instances: built on the game thread (PatchInstance::create), passed by pointer in a Start command; finished
//     or stopped ones come back through a second SPSC ring and are destroyed by reclaim() on the game thread;
//   - banks: a new WaveBank (level change) is published by pointer in a SetBank command with an epoch; the game
//     side keeps the shared_ptr until the audio thread's acknowledged epoch passes it (reclaim()).
// Commands carry a target mix frame (MixerClock): a command for a frame inside the next block lands at that
// offset (post_event's frame_in_block), a late one at offset 0 and counts late_commands. Scheduling lead: the game
// stamps a new burst at earliest_frame() = clock() + schedule_lead_blocks x kBlockFrames (tuning, default 2 blocks =
// 21.3 ms), and every later round of the burst at first frame + round x shot interval, so round spacing is exact
// whatever the game's frame jitter, and a command is late only when the game thread stalls for more than the lead.
// Gate G8 requires late_commands = 0.
//
// Ownership: push() of a Start command that returns false (ring full) leaves the PatchInstance with the caller
// (nothing is consumed); the caller retries next frame or destroys it. After a true return the mixer owns it until
// it comes back through the return ring and reclaim() destroys it.
//
// Lifetime: ~MixerCore runs on the game thread after the audio thread stopped calling mix(). The session
// (Bf3LevelSession) holds the owning shared_ptr; it stops the AudioStreamPlayer, waits one AudioServer lock()/
// unlock() so no mix is in flight, and only then releases its reference, so the playback's reference is never the
// last one and nothing is freed on Godot's audio thread (debug builds assert ~MixerCore's thread is the
// constructing thread).
//
// Underrun accounting (gate G8 "no underruns over 30 s"): short_blocks (mix() returned fewer frames than asked),
// late_blocks (a block took longer than its real-time duration to compute, wall time), late_commands,
// starved_voices and ring_overflows. These measure the mixer's compute and ring health, not device xruns (Godot does
// not expose those to GDExtension); max_block_ns and max_call_gap_ns (the longest gap between two mix() calls) are
// reported beside them.

#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/wave_bank.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kBlockFrames = 512; // the core's render grid (also Godot's usual AudioServer buffer_size)
inline constexpr uint32_t kMixRate = 48000; // AudioSystemAsset.SampleRate [verified]; project.godot sets the driver to it

struct MixerConfig {
	uint32_t mix_rate = kMixRate;
	uint32_t max_instances = 64;
	uint32_t command_capacity = 1024; // power of two
	float output_gain_db = 0.0f;
	uint32_t schedule_lead_blocks = 2; // AudioTuning::schedule_lead_blocks
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
	std::atomic<uint64_t> max_block_ns{ 0 }; // report only
	std::atomic<uint64_t> max_call_gap_ns{ 0 }; // report only: longest wall gap between two mix() calls
	std::atomic<uint64_t> acked_epoch{ 0 };
	std::atomic<uint64_t> feedback_drops{ 0 };
};

// Audio thread -> game thread, once per block per instance that has news: haptics (CameraShake / PadRumble) and
// the finished flag. A third SPSC ring; when it is full the new entry is dropped and counted (feedback_drops); the
// game polls it every frame.
struct MixerFeedback {
	InstanceHandle handle = kNoInstance;
	uint64_t frame = 0;
	PatchHaptics haptics;
	bool finished = false;
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
	bool push(const MixerCommand &p_command); // false: ring full (counted); a Start's instance stays the caller's
	uint64_t clock() const; // frames mixed so far (relaxed)
	uint64_t earliest_frame() const; // clock() + schedule_lead_blocks x kBlockFrames: where a new burst is stamped
	void reclaim(); // destroy returned instances, release banks below the acked epoch
	size_t poll_feedback(std::span<MixerFeedback> r_out); // drains up to r_out.size() entries
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
