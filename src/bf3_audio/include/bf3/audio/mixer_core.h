#pragma once

#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/hdr_setting.h"
#include "bf3/core/audio/sound_scope.h"
#include "bf3/core/audio/wave_bank.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kBlockFrames = 512;
inline constexpr uint32_t kMixRate = 48000;

struct MixerConfig {
	uint32_t mix_rate = kMixRate;
	uint32_t max_instances = 64;
	uint32_t command_capacity = 1024;
	float output_gain_db = 0.0f;
	uint32_t schedule_lead_blocks = 2;

	std::optional<core::audio::HdrSetting> hdr;
	uint32_t max_voices = 128;
	std::shared_ptr<const core::audio::ScopeTable> scopes;

	uint32_t cut_fade_frames = 512;

	float cap_hold_db = 3.0f;
	uint32_t voice_hold_blocks = 47;

	uint32_t resume_margin = 8;

	float limit_ceiling = 0.966f;
	uint32_t limit_attack_frames = 48;
	float limit_release_ms = 80.0f;
};

using InstanceHandle = uint32_t;
inline constexpr InstanceHandle kNoInstance = 0;

enum class MixerCommandType : uint8_t {
	Start,
	Stop,
	Kill,
	SetParameter,
	PostEvent,
	SetMixerValue,
	SetEmitter,
	SetListener,
	SetBank,
	SetBusGain,
};

struct MixerCommand {
	MixerCommandType type = MixerCommandType::Stop;
	InstanceHandle handle = kNoInstance;
	uint32_t name_hash = 0;
	float value = 0.0f;
	uint64_t at_frame = 0;
	PatchInstance *instance = nullptr;
	const core::audio::WaveBank *bank = nullptr;
	uint64_t epoch = 0;
	EmitterState emitter;
	ListenerState listener;
};

struct MixerStats {
	std::atomic<uint64_t> frames{ 0 };
	std::atomic<uint64_t> blocks{ 0 };
	std::atomic<uint64_t> short_blocks{ 0 };
	std::atomic<uint64_t> late_blocks{ 0 };
	std::atomic<uint64_t> late_commands{ 0 };
	std::atomic<uint64_t> ring_overflows{ 0 };
	std::atomic<uint64_t> starved_voices{ 0 };
	std::atomic<uint64_t> decode_errors{ 0 };
	std::atomic<uint64_t> deferred_events{ 0 };
	std::atomic<uint32_t> active_instances{ 0 };
	std::atomic<uint32_t> peak_instances{ 0 };
	std::atomic<uint64_t> max_block_ns{ 0 };
	std::atomic<uint64_t> max_call_gap_ns{ 0 };
	std::atomic<uint64_t> acked_epoch{ 0 };
	std::atomic<uint64_t> feedback_drops{ 0 };

	std::atomic<uint64_t> mix_ns_last{ 0 };
	std::atomic<uint64_t> mix_ns_max{ 0 };
	std::atomic<uint64_t> mix_ns_hist[16]{};
	std::atomic<uint32_t> live_voices_max{ 0 };
	std::atomic<uint64_t> culled_precull{ 0 };
	std::atomic<uint64_t> culled_scope{ 0 };
	std::atomic<uint64_t> culled_cap{ 0 };
	std::atomic<uint32_t> suspended{ 0 };
	std::atomic<float> window_top_db{ 0.0f };
	std::atomic<float> window_bottom_db{ 0.0f };
};

struct MixerFeedback {
	InstanceHandle handle = kNoInstance;
	uint64_t frame = 0;
	PatchHaptics haptics;
	bool finished = false;
};

class MixTap {
public:
	virtual ~MixTap() = default;
	virtual void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) = 0;
};

enum class MixerCutKind : uint8_t { Drop = 0, Pend, Kill, Finish, Start, Resume, Suspend, Unsuspend, Fade, CallGap, LateBlock, Count };
inline constexpr size_t kMixerCutKinds = size_t(MixerCutKind::Count);
inline constexpr float kAudibleStep = 1.0e-3f;
inline constexpr float kLoudStep = 1.0e-2f;
inline constexpr double kLateCallMs = 25.0;
struct MixerCutEvent {
	uint64_t frame = 0;
	InstanceHandle handle = kNoInstance;
	MixerCutKind kind = MixerCutKind::Drop;
	float step = 0.0f;
	float loudness_db = 0.0f;
	float distance_m = 0.0f;
	char program[48] = {};
};
struct MixerCutStats {
	std::atomic<uint64_t> events[kMixerCutKinds]{};
	std::atomic<uint64_t> audible[kMixerCutKinds]{};
	std::atomic<uint64_t> loud[kMixerCutKinds]{};
	std::atomic<float> step_max[kMixerCutKinds]{};
	std::atomic<uint64_t> abrupt_gain{ 0 };
	std::atomic<float> abrupt_gain_max{ 0.0f };
	std::atomic<uint64_t> calls{ 0 };
	std::atomic<uint64_t> call_frames_min{ ~0ull }, call_frames_max{ 0 };
	std::atomic<uint64_t> gap_hist[8]{};
	std::atomic<uint64_t> event_drops{ 0 };
	std::atomic<uint64_t> overs_in{ 0 }, overs_out{ 0 };
	std::atomic<uint64_t> limit_blocks{ 0 };
	std::atomic<uint64_t> limit_edge_steps{ 0 };
	std::atomic<float> limit_min_gain{ 1.0f };
};
inline constexpr std::array<double, 7> kGapEdgesMs = { 5.0, 10.0, 15.0, 20.0, 30.0, 50.0, 100.0 };
const char *mixer_cut_kind_name(MixerCutKind p_kind);

class MixerCore {
public:
	explicit MixerCore(const MixerConfig &p_config);
	~MixerCore();

	InstanceHandle next_handle();
	bool push(const MixerCommand &p_command);
	uint64_t clock() const;
	uint64_t earliest_frame() const;
	void reclaim();
	size_t poll_feedback(std::span<MixerFeedback> r_out);
	const MixerStats &stats() const { return stats_; }

	uint32_t mix(float *r_interleaved_stereo, uint32_t p_frames);
	void set_tap(MixTap *p_tap);
	float bus_gain(core::audio::MasterBus p_bus) const;

	const MixerCutStats &cut_stats() const;
	void record_cuts(bool p_on);
	size_t poll_cuts(std::span<MixerCutEvent> r_out);

	MixerCore(const MixerCore &) = delete;
	MixerCore &operator=(const MixerCore &) = delete;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	MixerStats stats_;
};

inline constexpr size_t kMixHistBuckets = 16;
inline constexpr std::array<float, kMixHistBuckets - 1> kMixHistEdges = { 0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.08f, 0.10f, 0.12f, 0.15f, 0.20f, 0.30f,
	0.50f, 0.75f, 1.00f };

void record_mix_ns(MixerStats &r_stats, uint64_t p_ns, uint64_t p_block_ns);

double mix_share_quantile(const MixerStats &p_stats, double p_q);

struct MixerProfile {
	uint64_t blocks = 0, block_ns = 0, mix_ns_last = 0, mix_ns_max = 0;
	double share_p50 = 0.0, share_p99 = 0.0;

	uint32_t live_instances = 0, live_voices_max = 0, suspended = 0;
	uint64_t culled_precull = 0, culled_scope = 0, culled_cap = 0;
	float window_top_db = 0.0f, window_bottom_db = 0.0f;
};
MixerProfile read_mixer_profile(const MixerStats &p_stats, uint32_t p_mix_rate = kMixRate);
std::string format_mixer_profile(const MixerProfile &p_profile);

std::string format_mixer_cuts(const MixerCutStats &p_stats);

}
