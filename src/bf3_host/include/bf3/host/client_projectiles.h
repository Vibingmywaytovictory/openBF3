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
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

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
};

class ClientProjectiles {
public:
	static core::Result<std::unique_ptr<ClientProjectiles>> create(std::shared_ptr<const sim::weapons::WeaponTable> p_table,
			std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed);
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
