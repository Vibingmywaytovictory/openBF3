#pragma once

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
	uint64_t samples_logged = 0;
	uint32_t live_own = 0, live_remote = 0;

	uint64_t samples_dropped = 0;
	uint64_t own_queries = 0, own_no_view = 0, remote_queries = 0;
	uint64_t remote_soldier_contacts = 0;
	uint64_t detonations_suppressed = 0;
	uint64_t remote_late = 0;
	uint64_t events_refused = 0;
};
static_assert(std::is_trivially_copyable_v<ClientProjectilesStats>);

struct OwnTestSample {
	uint8_t shooter = 0;
	uint32_t shot_index = 0;
	uint8_t target = 0;
	int64_t time_q8 = 0;
	net::OtherPose pose;
};
static_assert(std::is_trivially_copyable_v<OwnTestSample>);

class ReceivedRecordSource {
public:
	virtual ~ReceivedRecordSource() = default;

	virtual size_t candidates(const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const = 0;

	virtual bool view_tick(uint16_t p_seq, uint32_t &r_tick) const = 0;
	virtual void slots_in(const ShotView &p_view, std::array<uint64_t, 2> &r_slots) const = 0;
};

struct ClientProjectilesConfig {
	uint8_t own_slot = 0;
	uint32_t max_rewind_ticks = kDefaultMaxRewindTicks;
	std::array<float, sim::kPoseCount> pose_eye_heights{ 1.6f, 1.0f, 0.3f };
	float extrapolate_max_ms = 250.0f;
	size_t max_samples = 1u << 16;
	size_t max_remote_pending = 1024;
	sim::weapons::BallisticsLimits limits;
};

uint64_t effect_seed(uint64_t p_match_seed, uint8_t p_slot, uint32_t p_shot_index);
uint64_t impact_seed(uint64_t p_effect_seed, uint8_t p_pellet, uint32_t p_ordinal);

bool shot_event_of(const net::RemoteShot &p_shot, const sim::weapons::WeaponTable &p_table, uint64_t p_match_seed, sim::weapons::ShotEvent &r_event);

class ClientProjectiles {
public:
	static core::Result<std::unique_ptr<ClientProjectiles>> create(std::shared_ptr<const sim::weapons::WeaponTable> p_table,
			std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed);

	static core::Result<std::unique_ptr<ClientProjectiles>> create(std::shared_ptr<const sim::weapons::WeaponTable> p_table,
			std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed, const ClientProjectilesConfig &p_config);
	~ClientProjectiles();
	ClientProjectiles(const ClientProjectiles &) = delete;
	ClientProjectiles &operator=(const ClientProjectiles &) = delete;

	void spawn_own(const sim::weapons::ShotEvent &p_shot, const ShotView &p_view);

	void queue_remote(const net::RemoteShot &p_shot);
	void step_own(sim::Tick p_tick, const sim::CollisionWorld &p_world, const ReceivedRecordSource &p_records, PresentationEvents &r_events);
	void step_remote(double p_render_ticks, const sim::CollisionWorld &p_world, std::span<const net::OtherPose> p_others, PresentationEvents &r_events);

	size_t take_samples(std::span<OwnTestSample> r_out);
	const ClientProjectilesStats &stats() const;

private:
	ClientProjectiles();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
