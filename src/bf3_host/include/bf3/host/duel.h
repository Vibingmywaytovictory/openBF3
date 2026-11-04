#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace bf3::host {

inline constexpr float kDuelLaneHalf = 5.0f;
inline constexpr float kDuelAimHeight = 1.3f;
inline constexpr float kDuelLaneTurn = 4.0f;
inline constexpr double kDuelLegSeconds = 3.0;
inline constexpr uint32_t kDuelFireTicks = 12;
inline constexpr double kDuelSpawnTimeoutSeconds = 180.0;
inline constexpr double kDuelTeleportResendSeconds = 2.0;
inline constexpr double kDuelTeleportTimeoutSeconds = 20.0;
inline constexpr float kDuelAtSiteMetres = 1.0f;
inline constexpr float kDuelLaneNearMetres = 6.0f;
inline constexpr float kDuelMovingMetresPerSecond = 1.0f;
inline constexpr double kDuelSettleSeconds = 1.5;
inline constexpr double kDuelSettleTimeoutSeconds = 30.0;
inline constexpr double kDuelTailSeconds = 2.0;

struct DuelSite {
	sim::Vec3 shooter{};
	uint16_t shooter_yaw = 0;
	sim::Vec3 target{};
	uint16_t target_yaw = 0;
	uint32_t spawn_index = 0;
	float distance = 0;
	uint32_t candidates = 0;
};

std::optional<DuelSite> find_duel_site(
		const sim::CollisionWorld &p_world, const SharedLevel &p_level, float p_distance, float p_half);

core::Result<DuelSite> duel_site(const SharedLevel &p_level);

sim::Vec3 present_own_eye(const PresentationFrame &p_frame);

struct ViewAim {
	float yaw = 0.0f;
	float pitch = 0.0f;
};

ViewAim aim_at(const sim::Vec3 &p_eye, const sim::Vec3 &p_point);

class DuelStrafe {
public:
	void set_lane(const sim::Vec3 &p_origin, float p_yaw0);
	void step(const sim::SoldierState &p_own, double p_t, InputSample &r_input);

private:
	uint64_t lane_generation_ = 0;
	sim::Vec3 lane_origin_{};
	float lane_yaw0_ = 0.0f;
	bool init_ = false;
	uint32_t life_ = 0;
	uint64_t generation_ = 0;
	sim::Vec3 origin_{};
	float yaw0_ = 0.0f;
	int dir_ = 1;
	double leg_start_ = 0.0;
};

struct DuelShooterIo {
	std::function<void()> deploy;
	std::function<void(const net::DebugRequest &)> debug;
	std::function<void(float, float)> view;
	std::function<void()> fire;
	std::function<void(bool)> hold_zoom;
};

enum class DuelPhase : uint8_t { Spawn, Teleport, Settle, Fire, Tail, Done, Failed };

class DuelShooter {
public:
	DuelShooter(const DuelSite &p_site, uint32_t p_shots);

	DuelPhase frame(double p_now_s, uint64_t p_now_ns, const PresentationFrame &p_frame, bool p_alive,
			const DuelShooterIo &p_io);
	DuelPhase phase() const { return phase_; }
	uint64_t shots() const { return shots_; }
	const std::string &failure() const { return failure_; }
	std::string report(const ClientStats &p_stats) const;

private:
	void fail(const std::string &p_why);
	void enter(DuelPhase p_phase, double p_now_s);

	DuelSite site_;
	uint32_t want_shots_ = 0;
	DuelPhase phase_ = DuelPhase::Spawn;
	double phase_s_ = -1.0;
	double first_s_ = -1.0;
	double sent_s_ = -1.0;
	double settled_s_ = -1.0;
	double deployed_s_ = -1.0;
	uint32_t life_ = 0;
	uint32_t teleports_ = 0;
	uint64_t base_shots_ = 0;
	uint64_t shots_ = 0;
	sim::Tick last_press_ = 0;
	float seen_from_lane_ = -1.0f;
	float seen_speed_ = 0.0f;
	bool zoomed_ = false;
	std::string failure_;
};

}
