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

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::host {

// m2a.md §16 Q1 (taken 2026-10-16): 350 ms = 21 ticks, so "150/2" (~270 ms lag) is fully compensated; beyond it
// the server tests at the clamped time and counts rewind_clamped.
inline constexpr uint32_t kDefaultMaxRewindTicks = 21;
inline constexpr uint32_t kHistorySnapshots = 48; // 1.6 s of 30 Hz snapshots: the 32-seq received window plus the view lag (m2a-lc.md)
inline constexpr uint32_t kSentRingEntries = 64; // per client, by seq
inline constexpr uint32_t kShotViewRing = 128; // per shooter slot, by shot index
inline constexpr uint32_t kPoseCacheEntries = 2048; // open addressing, cleared in begin_tick
inline constexpr uint32_t kViewSeqWindow = 32; // view_seq8 resolves within 32 seqs of the packet's newest_snapshot
inline constexpr double kMaxViewExtrapolateMs = 250.0; // quality Held / Extrapolated beyond this: Refused

struct LagCompConfig {
	uint32_t history_snapshots = kHistorySnapshots;
	uint32_t max_rewind_ticks = kDefaultMaxRewindTicks;
	bool enabled = true; // false = every shot at the present (E12's control run sets max_rewind_ticks 0 instead)
	// lc additions (as built, docs/design/m2a-lc.md). sample_other's other inputs, which must equal the client's for
	// the E12 bit equality: the level's pose eye heights (MotorParams::poses[k].eye_height, as PresentationFrame's
	// pose_eye_heights) and ClientSessionConfig::extrapolate_max_ms.
	std::array<float, sim::kPoseCount> pose_eye_heights{ 1.6f, 1.0f, 0.3f };
	float extrapolate_max_ms = 250.0f;
	bool pose_cache = true; // false: every pose computed (sim.lagcomp.unit's cache-off arm; the answers are equal)
	bool time_provider = true; // the provider's own wall time per tick (LagCompStats::tick_ms_p99, E13)
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
	// lc additions (as built).
	uint64_t ticks = 0; // provider ticks (begin_tick calls)
	uint64_t present_queries = 0; // queries answered from the present set (bots, quality None, refused, no view)
	uint64_t rewound_queries = 0;
	uint64_t no_view = 0; // queries whose shot had no stored view (answered at the present)
	uint64_t culled = 0; // targets the swept-bound broad phase removed
	uint64_t generation_skips = 0; // candidates sampled from another life than the present one
	uint64_t history_misses = 0; // candidate records no longer in the shared store
	uint64_t answered = 0; // HitTargets returned in all
	double tick_ms_max = 0;
	double tick_ms_mean = 0;
};

// One posed candidate of a rewound query (sim.lagcomp.unit, the E12 trace rows; RewindProvider::set_trace).
struct RewindTraceRow {
	uint32_t shooter = 0;
	uint32_t shot_index = 0;
	uint8_t pellet = 0;
	uint8_t target = 0;
	int64_t time_q8 = 0;
	net::OtherPose pose;
	bool cached = false;
};

// A target's swept bound over the history window (m2a.md §6.4): the AABB of its recorded feet over the shared store,
// the largest recorded speed and the store's tick span.
struct SweptBounds {
	bool any = false;
	float lo[3]{};
	float hi[3]{};
	float max_speed = 0;
	uint32_t oldest_tick = 0, newest_tick = 0;
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

	// lc additions (as built).
	const LagCompConfig &config() const;
	uint32_t max_slots() const;
	SweptBounds bounds(uint8_t p_slot) const; // maintained in record_tick
	// The tick a seq of that client's sent ring stands for (false: not in the ring).
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

	// The present set, the swept bounds' SoA and the cache clear, once per tick before ProjectileWorld::step.
	void begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present);
	void note_shot(uint8_t p_slot, uint32_t p_shot_index, const ShotView &p_view);
	// A scratch list of the candidates' HitTargets in slot order, valid until the next call (hit_target.h contract).
	std::span<const sim::weapons::HitTarget> targets_for(const sim::weapons::TargetQuery &p_query) const override;
	const LagCompStats &stats() const;

	// lc additions (as built).
	// begin_tick with the teams per slot (HitTarget::team; a missing entry is team 0). A present soldier is one with
	// life != 0 and mode != Dead; spawn_protected = protect_ticks > 0. The shooter's client slot is its player slot.
	void begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present, std::span<const uint8_t> p_teams);
	// Closes the tick's timing (otherwise the next begin_tick does).
	void end_tick();
	// A shot whose view LagHistory::shot_view refused: tested at the present, counted Refused.
	void note_refused(uint8_t p_slot, uint32_t p_shot_index);
	// The view stored for a shot (false: none, or it was refused).
	bool shot_view_of(uint8_t p_slot, uint32_t p_shot_index, ShotView &r_view) const;
	// Every posed candidate of every rewound query is appended here while set (null: off). The caller bounds it.
	void set_trace(std::vector<RewindTraceRow> *r_rows);
	// The present set (posed in begin_tick) of a slot; null when not present.
	const sim::weapons::HitTarget *present_target(uint8_t p_slot) const;
	double last_tick_ms() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host
