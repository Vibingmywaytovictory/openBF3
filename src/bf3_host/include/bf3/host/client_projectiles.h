#pragma once
// The client's cosmetic projectiles (docs/design/m2a.md §8.4; owner cl). Client thread only. Two ProjectileWorlds
// against the client's own CollisionWorld:
//   own world, predicted timeline: every own predicted shot (first prediction only), stepped with the prediction.
//     Its target provider poses the others with sim::weapons::posed_target from net::sample_other over the list that
//     net::rebuild_track builds from the client's ReceivedRecords under the shot's received set, at
//     net::rewind_time_q8(time_start, view_back_q8) of the command that fired the shot: the server's time rule on the
//     same list, so the client tests a target where the server will (up to the counted late_sample shots). Each
//     tested sample's (tick_a, tick_b, kind) and pose go into the E12 log.
//   remote world, render timeline: every remote shot (pellets from the derived seed, pellet_cone and power from the
//     message), spawned when the render time reaches its time and stepped to the render time each client tick,
//     against the others at the render time. A remote grenade's own detonation is suppressed (the server's
//     DetonationWire places it).
// Static contacts become Impact events, soldier contacts SoldierContact events (presentation_events.h).

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/result.h"
#include "bf3/host/lag_comp.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace bf3::host {

struct ClientProjectilesStats {
	uint64_t own_spawned = 0, remote_spawned = 0, refused = 0;
	uint64_t own_soldier_contacts = 0, own_impacts = 0, remote_impacts = 0;
	uint64_t samples_logged = 0; // own-world target samples written to the E12 log
	uint32_t live_own = 0, live_remote = 0;
	// cl, as built.
	uint64_t samples_dropped = 0; // E12 log entries beyond ClientProjectilesConfig::max_samples
	uint64_t own_queries = 0, own_no_view = 0, remote_queries = 0;
	uint64_t remote_soldier_contacts = 0;
	uint64_t detonations_suppressed = 0; // cosmetic detonations (the server's DetonationWire places them)
	uint64_t remote_late = 0; // remote shots spawned after their time had passed the render time (a late datagram)
	uint64_t events_refused = 0; // PresentationEvents::push refused (ring full)
};
static_assert(std::is_trivially_copyable_v<ClientProjectilesStats>);

// One own-shot target sample for E12: what the client's cosmetic test posed (compared with the server's Trace).
struct OwnTestSample {
	uint8_t shooter = 0;
	uint32_t shot_index = 0;
	uint8_t target = 0;
	int64_t time_q8 = 0;
	net::OtherPose pose;
};
static_assert(std::is_trivially_copyable_v<OwnTestSample>);

// Where the own world reads the others' records: cl's ReceivedRecords (the last 32 seqs as they arrived, per slot).
class ReceivedRecordSource {
public:
	virtual ~ReceivedRecordSource() = default;
	// The candidates of p_target under p_view (bit i of the masks: seq view_seq - i), any order.
	virtual size_t candidates(const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const = 0;
	// cl, as built: the server tick of the client's Snapshot of seq p_seq (rebuild_track's view tick; false when the
	// client does not hold that seq), and the slots any record under p_view names (the targets worth sampling).
	virtual bool view_tick(uint16_t p_seq, uint32_t &r_tick) const = 0;
	virtual void slots_in(const ShotView &p_view, std::array<uint64_t, 2> &r_slots) const = 0;
};

// cl, as built (docs/design/m2a-cl.md): the inputs the two worlds need beyond the table. The own world's time rule and
// sampler inputs must be the server's (LagCompConfig: max_rewind_ticks from MatchInfo, the level's pose eye heights,
// ClientSessionConfig::extrapolate_max_ms) for E12's bit equality.
struct ClientProjectilesConfig {
	uint8_t own_slot = 0;
	uint32_t max_rewind_ticks = kDefaultMaxRewindTicks;
	std::array<float, sim::kPoseCount> pose_eye_heights{ 1.6f, 1.0f, 0.3f };
	float extrapolate_max_ms = 250.0f;
	size_t max_samples = 1u << 16; // the E12 log's bound (beyond: dropped and counted)
	size_t max_remote_pending = 1024; // remote shots waiting for the render time (beyond: refused and counted)
	sim::weapons::BallisticsLimits limits;
};

// The effect seed of a shot (docs/design/m2a.md §4.4: splitmix64(match_seed ^ (slot << 32 | shot_index))) and an
// impact's (the effect seed mixed with the pellet and the impact's ordinal within its shot). Both ends of a remote
// shot derive the same values from replicated inputs (E15).
uint64_t effect_seed(uint64_t p_match_seed, uint8_t p_slot, uint32_t p_shot_index);
uint64_t impact_seed(uint64_t p_effect_seed, uint8_t p_pellet, uint32_t p_ordinal);
// A decoded remote shot as the ShotEvent the server fired (seed and spawn time derived, §7.4); false when its def is
// not in the table.
bool shot_event_of(const net::RemoteShot &p_shot, const sim::weapons::WeaponTable &p_table, uint64_t p_match_seed, sim::weapons::ShotEvent &r_event);

class ClientProjectiles {
public:
	static core::Result<std::unique_ptr<ClientProjectiles>> create(std::shared_ptr<const sim::weapons::WeaponTable> p_table,
			std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed);
	// cl, as built: with the own slot, the time rule's clamp and the sampler inputs (the declared create uses the
	// defaults, own slot 0).
	static core::Result<std::unique_ptr<ClientProjectiles>> create(std::shared_ptr<const sim::weapons::WeaponTable> p_table,
			std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed, const ClientProjectilesConfig &p_config);
	~ClientProjectiles();
	ClientProjectiles(const ClientProjectiles &) = delete;
	ClientProjectiles &operator=(const ClientProjectiles &) = delete;

	// Own predicted shots of a tick's first prediction, with the view sample of the command that fired them.
	void spawn_own(const sim::weapons::ShotEvent &p_shot, const ShotView &p_view);
	// Remote shots as decoded (spawned when the render time reaches their time).
	void queue_remote(const net::RemoteShot &p_shot);
	void step_own(sim::Tick p_tick, const sim::CollisionWorld &p_world, const ReceivedRecordSource &p_records, PresentationEvents &r_events);
	void step_remote(double p_render_ticks, const sim::CollisionWorld &p_world, std::span<const net::OtherPose> p_others, PresentationEvents &r_events);
	// The E12 log since the previous call (moved out).
	size_t take_samples(std::span<OwnTestSample> r_out);
	const ClientProjectilesStats &stats() const;

private:
	ClientProjectiles();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host
