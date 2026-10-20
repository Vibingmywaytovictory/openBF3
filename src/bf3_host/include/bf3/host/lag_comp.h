#pragma once

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/hit_target.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::host {

inline constexpr uint32_t kDefaultMaxRewindTicks = 21;
inline constexpr uint32_t kHistorySnapshots = 48;
inline constexpr uint32_t kSentRingEntries = 64;
inline constexpr uint32_t kShotViewRing = 128;
inline constexpr uint32_t kPoseCacheEntries = 2048;
inline constexpr uint32_t kViewSeqWindow = 32;
inline constexpr double kMaxViewExtrapolateMs = 250.0;

struct LagCompConfig {
	uint32_t history_snapshots = kHistorySnapshots;
	uint32_t max_rewind_ticks = kDefaultMaxRewindTicks;
	bool enabled = true;

	std::array<float, sim::kPoseCount> pose_eye_heights{ 1.6f, 1.0f, 0.3f };
	float extrapolate_max_ms = 250.0f;
	bool pose_cache = true;
	bool time_provider = true;
};

enum class RewindVerdict : uint8_t { Present = 0, Rewound = 1, Clamped = 2, Refused = 3 };

struct LagCompStats {
	uint64_t queries = 0;
	uint64_t candidates = 0;
	uint64_t pose_cache_hits = 0;
	uint64_t pose_cache_full = 0;
	uint64_t present = 0, rewound = 0, clamped = 0, refused = 0;
	uint64_t acks_ignored = 0;
	double tick_ms_p99 = 0;

	uint64_t ticks = 0;
	uint64_t present_queries = 0;
	uint64_t rewound_queries = 0;
	uint64_t no_view = 0;
	uint64_t culled = 0;
	uint64_t generation_skips = 0;
	uint64_t history_misses = 0;
	uint64_t answered = 0;
	double tick_ms_max = 0;
	double tick_ms_mean = 0;
};

struct RewindTraceRow {
	uint32_t shooter = 0;
	uint32_t shot_index = 0;
	uint8_t pellet = 0;
	uint8_t target = 0;
	int64_t time_q8 = 0;
	net::OtherPose pose;
	bool cached = false;
};

struct SweptBounds {
	bool any = false;
	float lo[3]{};
	float hi[3]{};
	float max_speed = 0;
	uint32_t oldest_tick = 0, newest_tick = 0;
};
static_assert(std::is_trivially_copyable_v<LagCompStats>);

struct AckState {
	uint16_t newest = 0;
	uint64_t main_bits = 0;
	uint64_t others_bits = 0;
};
static_assert(std::is_trivially_copyable_v<AckState>);

struct ShotView {
	int32_t view_back_q8 = 0;
	uint8_t quality = 0;
	uint8_t pad = 0;
	uint16_t view_seq = 0;
	uint32_t main_bits = 0;
	uint32_t others_bits = 0;
};
static_assert(std::is_trivially_copyable_v<ShotView>);

class LagHistory {
public:
	LagHistory(const LagCompConfig &p_config, uint32_t p_max_slots);
	~LagHistory();
	LagHistory(const LagHistory &) = delete;
	LagHistory &operator=(const LagHistory &) = delete;

	void record_tick(uint32_t p_snapshot_tick, std::span<const net::OtherSoldier> p_records);

	void sent(uint8_t p_client_slot, uint16_t p_seq, uint32_t p_snapshot_tick, uint64_t p_main_mask, uint64_t p_others_mask);

	void ack(uint8_t p_client_slot, uint16_t p_newest, uint32_t p_snapshot_ack_bits, uint32_t p_others_ack_bits);

	AckState ack_state(uint8_t p_client_slot) const;

	bool shot_view(uint8_t p_client_slot, const AckState &p_at_arrival, uint8_t p_view_seq8, int32_t p_view_back_q8, uint8_t p_quality,
			ShotView &r_view) const;

	size_t track(uint8_t p_client_slot, const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther, net::kTrackSamples> r_list) const;
	net::OtherPose sample(std::span<const net::TimedOther> p_list, int64_t p_time_q8) const;
	void client_reset(uint8_t p_client_slot);
	const LagCompStats &stats() const;

	const LagCompConfig &config() const;
	uint32_t max_slots() const;
	SweptBounds bounds(uint8_t p_slot) const;

	bool sent_tick(uint8_t p_client_slot, uint16_t p_seq, uint32_t &r_tick) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

class RewindProvider final : public sim::weapons::HitTargetProvider {
public:
	RewindProvider(const LagHistory &p_history, const core::gameplay::CapsulePoseTable &p_capsules, const LagCompConfig &p_config);
	~RewindProvider() override;
	RewindProvider(const RewindProvider &) = delete;
	RewindProvider &operator=(const RewindProvider &) = delete;

	void begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present);
	void note_shot(uint8_t p_slot, uint32_t p_shot_index, const ShotView &p_view);

	std::span<const sim::weapons::HitTarget> targets_for(const sim::weapons::TargetQuery &p_query) const override;
	const LagCompStats &stats() const;

	void begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present, std::span<const uint8_t> p_teams);

	void end_tick();

	void note_refused(uint8_t p_slot, uint32_t p_shot_index);

	bool shot_view_of(uint8_t p_slot, uint32_t p_shot_index, ShotView &r_view) const;

	void set_trace(std::vector<RewindTraceRow> *r_rows);

	const sim::weapons::HitTarget *present_target(uint8_t p_slot) const;
	double last_tick_ms() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
