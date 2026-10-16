#pragma once
// Lag compensation on the server (plan D7; docs/design/m2a.md §6.3, §6.4, §6.6; owner lc). Server thread only.
//
// A human's shot is tested against the targets as that client drew them: at the shot's rewound time (the command's
// view sample, net::rewind_time_q8), from only the records that client had received, through the same sampler
// (net::sample_other) and the same track rules (net::rebuild_track) the client uses. LagHistory keeps three stores:
//   1. records per snapshot tick, shared: the quantised OtherSoldier of every live soldier, once per tick
//      (net::quantize_other takes no receiver, and every client's snapshot of a tick is built in one call);
//   2. a sent ring per client: 64 entries by that client's seq, {seq, tick, main_mask, others_mask} (M1 numbers
//      snapshots per client and splits Snapshot / SnapshotOthers nearest-first per receiver);
//   3. acks per client: main_received / others_received per sent-ring entry, set from every command packet's
//      newest_snapshot / snapshot_ack_bits / others_ack_bits (conventions: protocol.h, m2a.md §7.2), never cleared
//      while the entry lives; an ack for a seq the ring does not hold, or an others bit for a seq that sent no
//      SnapshotOthers, is ignored and counted.
// RewindProvider answers ProjectileWorld's TargetQuery: the broad phase tests the query's segment (TargetQuery::from,
// to, radius; filled by ProjectileWorld, R4) against each target's swept bound over the history window, then poses
// only the candidates (posed_target) at the rewound sample, through a pose cache keyed by everything sample_other
// depends on (target, generation, time q8, tick_a, tick_b, kind), so the answers do not depend on the cache.
// Verdicts per shot (after IW4L's ShotSampleProvenance, studied, own code): Present (bot, or quality None), Rewound,
// Clamped (beyond max_rewind_ticks), Refused (malformed view sample: tested at the present).

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/hit_target.h"

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace bf3::host {

// m2a.md §16 Q1 (taken 2026-10-16): 350 ms = 21 ticks, so "150/2" (~270 ms lag) is fully compensated; beyond it
// the server tests at the clamped time and counts rewind_clamped.
inline constexpr uint32_t kDefaultMaxRewindTicks = 21;
inline constexpr uint32_t kHistorySnapshots = 24; // 800 ms of 30 Hz snapshots
inline constexpr uint32_t kSentRingEntries = 64; // per client, by seq
inline constexpr uint32_t kShotViewRing = 128; // per shooter slot, by shot index
inline constexpr uint32_t kPoseCacheEntries = 2048; // open addressing, cleared in begin_tick
inline constexpr uint32_t kViewSeqWindow = 32; // view_seq8 resolves within 32 seqs of the packet's newest_snapshot
inline constexpr double kMaxViewExtrapolateMs = 250.0; // quality Held / Extrapolated beyond this: Refused

struct LagCompConfig {
	uint32_t history_snapshots = kHistorySnapshots;
	uint32_t max_rewind_ticks = kDefaultMaxRewindTicks;
	bool enabled = true; // false = every shot at the present (E12's control run sets max_rewind_ticks 0 instead)
};

enum class RewindVerdict : uint8_t { Present = 0, Rewound = 1, Clamped = 2, Refused = 3 };

struct LagCompStats {
	uint64_t queries = 0;
	uint64_t candidates = 0;
	uint64_t pose_cache_hits = 0;
	uint64_t pose_cache_full = 0; // posed without being cached (the cache was full)
	uint64_t present = 0, rewound = 0, clamped = 0, refused = 0; // shot verdicts
	uint64_t acks_ignored = 0; // ack bits for seqs or parts never sent to that client
	double tick_ms_p99 = 0; // the provider's own time per tick (E13)
};
static_assert(std::is_trivially_copyable_v<LagCompStats>);

struct AckState {
	uint16_t newest = 0; // the newest acked Snapshot seq
	uint64_t main_bits = 0; // bit i: the Snapshot of seq newest - i arrived (bit 0 = newest itself)
	uint64_t others_bits = 0; // bit i: the SnapshotOthers of seq newest - i arrived
};
static_assert(std::is_trivially_copyable_v<AckState>);

// Per shot index, 16 B: a snapshot of the shooter's received set when its command first arrived (§6.3).
struct ShotView {
	int32_t view_back_q8 = 0; // Command::view_back_q8 (server ticks x 256)
	uint8_t quality = 0; // sim::ViewQuality
	uint8_t pad = 0;
	uint16_t view_seq = 0; // the full seq resolved from Command::view_seq8
	uint32_t main_bits = 0; // bit i: the Snapshot of seq view_seq - i was acked (bit 0 = view_seq itself)
	uint32_t others_bits = 0; // bit i: the SnapshotOthers of seq view_seq - i was acked
};
static_assert(std::is_trivially_copyable_v<ShotView>);

class LagHistory {
public:
	LagHistory(const LagCompConfig &p_config, uint32_t p_max_slots);
	~LagHistory();
	LagHistory(const LagHistory &) = delete;
	LagHistory &operator=(const LagHistory &) = delete;

	// Shared, once per snapshot tick (§4.1 step 7): every live soldier's record; receiver-independent.
	void record_tick(uint32_t p_snapshot_tick, std::span<const net::OtherSoldier> p_records);
	// Per client, after its datagrams of this tick are encoded (§4.1 step 8): that client's seq and split.
	void sent(uint8_t p_client_slot, uint16_t p_seq, uint32_t p_snapshot_tick, uint64_t p_main_mask, uint64_t p_others_mask);
	// Per command packet (§4.1 step 1); conventions of §7.2; unknown seqs / parts ignored and counted.
	void ack(uint8_t p_client_slot, uint16_t p_newest, uint32_t p_snapshot_ack_bits, uint32_t p_others_ack_bits);
	// At a command's first arrival: the slot's cumulative acks, kept with the buffered command (§6.3).
	AckState ack_state(uint8_t p_client_slot) const;
	// For a firing command: resolve view_seq8 against the stored acks and fill the shot view; false = Refused.
	bool shot_view(uint8_t p_client_slot, const AckState &p_at_arrival, uint8_t p_view_seq8, int32_t p_view_back_q8, uint8_t p_quality,
			ShotView &r_view) const;
	// The candidates of p_target under the shot view, then net::rebuild_track (oldest first, <= 8).
	size_t track(uint8_t p_client_slot, const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther, net::kTrackSamples> r_list) const;
	net::OtherPose sample(std::span<const net::TimedOther> p_list, int64_t p_time_q8) const; // net::sample_other
	void client_reset(uint8_t p_client_slot); // connect / disconnect: the sent ring and acks of that slot
	const LagCompStats &stats() const;

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

	// The present set, the swept bounds' SoA and the cache clear, once per tick before ProjectileWorld::step.
	void begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present);
	void note_shot(uint8_t p_slot, uint32_t p_shot_index, const ShotView &p_view);
	// A scratch list of the candidates' HitTargets in slot order, valid until the next call (hit_target.h contract).
	std::span<const sim::weapons::HitTarget> targets_for(const sim::weapons::TargetQuery &p_query) const override;
	const LagCompStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host
