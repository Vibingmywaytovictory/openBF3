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
#include "bf3/core/audio/hdr_setting.h" // M2b W1.0 (au)
#include "bf3/core/audio/sound_scope.h" // M2b W1.0 (au)
#include "bf3/core/audio/wave_bank.h"

#include <array> // M2b au
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional> // M2b W1.0 (au)
#include <span>
#include <string> // M2b au
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
	// M2b W1.0 (docs/design/m2b.md §6, §2.6 #15; owner au): A3. hdr: the HDR window (hdr_window.h) for kFlagEnableHdr
	// patches (nullopt: today's fixed window for every patch); max_voices: the one voice cap that replaces the router's
	// three limits (R6); scopes: the voice scopes (voice_cull.h; null: no scope step).
	std::optional<core::audio::HdrSetting> hdr; // M2b W1.0 (au)
	uint32_t max_voices = 128; // M2b W1.0 (au)
	std::shared_ptr<const core::audio::ScopeTable> scopes; // M2b W1.0 (au)
	// PT1 snd [fade] (docs/design/pt1-snd.md): a voice the mixer stops while it sounds (a cull drop, a Kill, a loop
	// parked as pending) fades out over this many frames instead of stopping at once, and a parked loop that resumes
	// fades in over them. 0: M2a / A3's cuts. [inferred] default one block (10.7 ms), measured = false: the data has no
	// voice fade time (AudioSystemAsset has none).
	uint32_t pt1_cut_fade_frames = 512; // PT1 snd [fade]
	// PT1 snd [hold] (docs/design/pt1-snd.md): hysteresis at the voice cap. A voice that sounded in the last block ranks
	// pt1_cap_hold_db louder (CullVoice::hold_db), and an instance counts the most sampler voices it had busy over the
	// last pt1_voice_hold_blocks blocks, so the voice at the cap's edge does not park and resume every few blocks when
	// other weapons' busy slots go up and down. [inferred] defaults 3 dB and 47 blocks (0.5 s), measured = false: the
	// data has no voice priority or virtualisation rule. 0 / 0: au's cap.
	float pt1_cap_hold_db = 3.0f; // PT1 snd [hold]
	uint32_t pt1_voice_hold_blocks = 47; // PT1 snd [hold]
	// PT1 snd [hold]: a parked loop resumes only when this many sampler voices stay free after it (CullVoice::
	// resume_margin), at most max_voices / 16. [inferred] default 8 of 128, measured = false.
	uint32_t pt1_resume_margin = 8; // PT1 snd [hold]
	// PT1 snd [limit] (docs/design/pt1-snd.md §3.4): a peak limiter on the stereo output, after output_gain. Windowed
	// rounds reached 2.4 (+7.6 dBFS) in 43 to 176 episodes a round, clipped downstream: the HdrSetting's energy terms
	// (CompressFactor, DischargeFactor, MaxAllowedEnergy) have no known formula (Q9), so nothing in A3 bounds the sum.
	// The block is the lookahead: the gain ramps down over pt1_limit_attack_frames before a peak in the same block, and
	// back up with a pt1_limit_release_ms time constant. A block under the ceiling at unity gain is not touched (the
	// same samples as without it). [inferred] defaults -0.3 dBFS, 48 frames (1 ms), 80 ms, measured = false. A
	// ceiling <= 0 turns it off.
	float pt1_limit_ceiling = 0.966f; // PT1 snd [limit]
	uint32_t pt1_limit_attack_frames = 48; // PT1 snd [limit]
	float pt1_limit_release_ms = 80.0f; // PT1 snd [limit]
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
	std::atomic<uint64_t> decode_errors{ 0 }; // summed over the instances (PatchInstanceStats)
	std::atomic<uint64_t> deferred_events{ 0 };
	std::atomic<uint32_t> active_instances{ 0 };
	std::atomic<uint32_t> peak_instances{ 0 };
	std::atomic<uint64_t> max_block_ns{ 0 }; // report only
	std::atomic<uint64_t> max_call_gap_ns{ 0 }; // report only: longest wall gap between two mix() calls
	std::atomic<uint64_t> acked_epoch{ 0 };
	std::atomic<uint64_t> feedback_drops{ 0 };
	// M2b W1.0 (docs/design/m2b.md §6.3, §2.6 #15; owner au): the A3 profiler (written by the audio thread, read by
	// the game thread, relaxed). The mix CPU time per block (steady_clock around the block's mix, ns) with a 16-bucket
	// histogram (au's bucket edges), the most live voices in one block, the culled voices per step, the suspended
	// voices of the last block and the window edges. blocks (above) already counts the blocks.
	std::atomic<uint64_t> mix_ns_last{ 0 }; // M2b W1.0 (au)
	std::atomic<uint64_t> mix_ns_max{ 0 }; // M2b W1.0 (au)
	std::atomic<uint64_t> mix_ns_hist[16]{}; // M2b W1.0 (au)
	std::atomic<uint32_t> live_voices_max{ 0 }; // M2b W1.0 (au)
	std::atomic<uint64_t> culled_precull{ 0 }; // M2b W1.0 (au)
	std::atomic<uint64_t> culled_scope{ 0 }; // M2b W1.0 (au)
	std::atomic<uint64_t> culled_cap{ 0 }; // M2b W1.0 (au)
	std::atomic<uint32_t> suspended{ 0 }; // M2b W1.0 (au)
	std::atomic<float> window_top_db{ 0.0f }; // M2b W1.0 (au)
	std::atomic<float> window_bottom_db{ 0.0f }; // M2b W1.0 (au)
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

// PT1 snd (docs/design/pt1-snd.md): voice cut diagnostics, the measurement behind the playtest's "sound glitching".
// A cut is a moment an instance's contribution to the mix changes without a ramp: it stops being rendered (a cull
// drop, a Kill, a loop parked as pending, its natural finish) or starts (a Start, a pending loop resumed). Its step is
// the |sample| the instance's sends wrote at the frame before the cut (PatchInstance::pt1_edge().edge), or the first
// frame after a start (.head): the discontinuity the cut leaves in the mix. Suspend / Unsuspend are the HDR window's
// transitions (the sends ramp over one sub-block). Fade: a cut that pt1's fade turned into a ramp (its step is the
// level the fade starts from). CallGap: a mix() call that came more than kPt1LateCallMs after the previous one
// (Godot's audio thread late; step = the gap in ms). LateBlock: a block that took longer than its real-time length to
// render (MixerStats::late_blocks; step = its wall time in ms), with its frame, to set against the main thread's hitches.
enum class MixerCutKind : uint8_t { Drop = 0, Pend, Kill, Finish, Start, Resume, Suspend, Unsuspend, Fade, CallGap, LateBlock, Count };
inline constexpr size_t kMixerCutKinds = size_t(MixerCutKind::Count);
inline constexpr float kPt1AudibleStep = 1.0e-3f; // -60 dBFS
inline constexpr float kPt1LoudStep = 1.0e-2f; // -40 dBFS
inline constexpr double kPt1LateCallMs = 25.0; // a mix() call gap above it counts (Godot calls about every 10.7 ms)
struct MixerCutEvent {
	uint64_t frame = 0; // MixerClock frame of the block the cut lands in
	InstanceHandle handle = kNoInstance;
	MixerCutKind kind = MixerCutKind::Drop;
	float step = 0.0f;
	float loudness_db = 0.0f; // the cull's loudness at the listener (0 for CallGap)
	float distance_m = 0.0f;
	char program[48] = {}; // the program's name, truncated
};
struct MixerCutStats {
	std::atomic<uint64_t> events[kMixerCutKinds]{}; // every cut
	std::atomic<uint64_t> audible[kMixerCutKinds]{}; // step > kPt1AudibleStep
	std::atomic<uint64_t> loud[kMixerCutKinds]{}; // step > kPt1LoudStep
	std::atomic<float> step_max[kMixerCutKinds]{};
	std::atomic<uint64_t> abrupt_gain{ 0 }; // PatchInstance InstanceEdge::abrupt_gain, summed over the instances
	std::atomic<float> abrupt_gain_max{ 0.0f };
	std::atomic<uint64_t> calls{ 0 }; // mix() calls
	std::atomic<uint64_t> call_frames_min{ ~0ull }, call_frames_max{ 0 };
	std::atomic<uint64_t> gap_hist[8]{}; // mix() call gaps: < 5, 10, 15, 20, 30, 50, 100 ms, >= 100 ms
	std::atomic<uint64_t> event_drops{ 0 }; // the event ring was full
	std::atomic<uint64_t> overs_in{ 0 }, overs_out{ 0 }; // PT1 snd [limit]: frames above 1.0 before / after the limiter
	std::atomic<uint64_t> limit_blocks{ 0 }; // PT1 snd [limit]: blocks with a gain below 1
	std::atomic<uint64_t> limit_edge_steps{ 0 }; // PT1 snd [limit]: a peak in a block's first frames, faster than the attack
	std::atomic<float> limit_min_gain{ 1.0f }; // PT1 snd [limit]
};
inline constexpr std::array<double, 7> kPt1GapEdgesMs = { 5.0, 10.0, 15.0, 20.0, 30.0, 50.0, 100.0 };
const char *mixer_cut_kind_name(MixerCutKind p_kind);

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
	// PT1 snd (docs/design/pt1-snd.md): the cut counters (any thread, relaxed); with recording on (set before the audio
	// thread starts), every cut is also queued as a MixerCutEvent for pt1_poll_cuts (game thread).
	const MixerCutStats &pt1_cut_stats() const; // PT1 snd
	void pt1_record_cuts(bool p_on); // PT1 snd
	size_t pt1_poll_cuts(std::span<MixerCutEvent> r_out); // PT1 snd

	MixerCore(const MixerCore &) = delete;
	MixerCore &operator=(const MixerCore &) = delete;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	MixerStats stats_;
};

// M2b au (docs/design/m2b.md §6.3, docs/design/m2b-au.md "Profiler"): the A3 profiler. render_block times every
// block (steady_clock around the whole block: commands, cull, window, rendering, the tap) and files the share of the
// block's real-time duration (512 frames at 48 kHz = 10.67 ms) into MixerStats::mix_ns_hist: bucket i < 15 holds the
// blocks with share < kMixHistEdges[i] (and >= the edge before it), bucket 15 those at or above the last edge (a late
// block). E11 reads the p99 from the histogram: an upper bound, exact at the edges (0.10 is one).
inline constexpr size_t kMixHistBuckets = 16;
inline constexpr std::array<float, kMixHistBuckets - 1> kMixHistEdges = { 0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.08f, 0.10f, 0.12f, 0.15f, 0.20f, 0.30f,
	0.50f, 0.75f, 1.00f };
// Audio thread (render_block): one block's mix time against its real-time duration.
void record_mix_ns(MixerStats &r_stats, uint64_t p_ns, uint64_t p_block_ns);
// The upper edge of the histogram bucket that holds the p_q quantile of the mix share (0.99: p99); +inf when that is
// the last bucket; 0 before the first block.
double mix_share_quantile(const MixerStats &p_stats, double p_q);
// A plain snapshot of the A3 counters (any thread, relaxed reads) and its one-line readout.
struct MixerProfile {
	uint64_t blocks = 0, block_ns = 0, mix_ns_last = 0, mix_ns_max = 0;
	double share_p50 = 0.0, share_p99 = 0.0; // histogram upper bounds (mix_share_quantile)
	// live_instances: the instances rendered in the last block; live_voices_max: the most sampler voices the cull kept in
	// one block (voice_cull.h: the cap counts sampler voices).
	uint32_t live_instances = 0, live_voices_max = 0, suspended = 0;
	uint64_t culled_precull = 0, culled_scope = 0, culled_cap = 0;
	float window_top_db = 0.0f, window_bottom_db = 0.0f; // 0 / 0 without an HdrSetting
};
MixerProfile read_mixer_profile(const MixerStats &p_stats, uint32_t p_mix_rate = kMixRate);
std::string format_mixer_profile(const MixerProfile &p_profile);

// PT1 snd (docs/design/pt1-snd.md): a one-line readout of MixerCutStats (the counts per kind, the audible and loud
// ones, the largest steps, the abrupt gain ramps and the mix() call gaps).
std::string format_mixer_cuts(const MixerCutStats &p_stats);

} // namespace bf3::audio
