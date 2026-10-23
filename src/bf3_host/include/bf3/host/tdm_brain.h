#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/sim/bots/aim_model.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/sim_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bf3::host {

struct BotLevel;
class Gunplay;
class ServerView;

enum class TdmBotActivity : uint8_t { Dead = 0, Restricted = 1, Roam = 2, Hunt = 3, Fight = 4, Retreat = 5, RegenWait = 6, Revive = 7, Grenade = 8 };
inline constexpr size_t kTdmBotActivityCount = 9;
const char *tdm_bot_activity_name(TdmBotActivity p_activity);

class TdmBotObserver {
public:
	virtual ~TdmBotObserver() = default;

	virtual void detected(const ServerView &p_view, sim::Tick p_tick, uint8_t p_bot, uint8_t p_target, const sim::Vec3 &p_eye) {
		(void)p_view, (void)p_tick, (void)p_bot, (void)p_target, (void)p_eye;
	}

	virtual void fired(const ServerView &p_view, sim::Tick p_tick, uint8_t p_bot, int p_target, sim::Tick p_last_visible, bool p_blind) {
		(void)p_view, (void)p_tick, (void)p_bot, (void)p_target, (void)p_last_visible, (void)p_blind;
	}
	virtual void activity(sim::Tick p_tick, uint8_t p_bot, TdmBotActivity p_activity) {
		(void)p_tick, (void)p_bot, (void)p_activity;
	}
	virtual void commanded(sim::Tick p_tick, uint8_t p_bot, TdmBotActivity p_activity, const sim::Command &p_command) {
		(void)p_tick, (void)p_bot, (void)p_activity, (void)p_command;
	}
};

struct TdmBotStats {
	std::atomic<uint32_t> bots{ 0 };
	std::atomic<uint64_t> perception_updates{ 0 };
	std::atomic<uint64_t> rays{ 0 };
	std::atomic<uint64_t> detections{ 0 };
	std::atomic<uint64_t> heard{ 0 };
	std::atomic<uint64_t> retargets{ 0 };
	std::atomic<uint64_t> bursts{ 0 };
	std::atomic<uint64_t> blind_fire{ 0 };
	std::atomic<uint64_t> grenades{ 0 };
	std::atomic<uint64_t> revive_attempts{ 0 };
	std::atomic<uint64_t> revive_shots{ 0 };
	std::atomic<uint64_t> retreats{ 0 };
	std::atomic<uint64_t> repaths{ 0 };

	std::atomic<uint64_t> reloads{ 0 };
	std::atomic<uint64_t> sidearm_switches{ 0 };
	std::atomic<uint64_t> knife_attacks{ 0 };
	std::atomic<uint64_t> revive_aborts{ 0 };
	std::atomic<uint64_t> walker_resets{ 0 };
	std::atomic<uint64_t> heat_goals{ 0 };
	std::atomic<uint32_t> walk_unavailable{ 0 };
	std::atomic<uint64_t> off_main_ticks{ 0 };
	std::atomic<uint64_t> hunts_given_up{ 0 };
	std::atomic<uint64_t> activity_ticks[kTdmBotActivityCount]{};

	std::atomic<uint64_t> last_perception_ns{ 0 };
	std::atomic<uint64_t> last_brains_ns{ 0 };
};

struct TdmBotConfig {
	uint32_t count = 31;
	sim::bots::PerceptionParams perception;
	sim::bots::AimParams aim;

	float skill_min = 0.3f;
	float skill_max = 0.8f;

	float reaction_min_s = 0.25f;
	float reaction_max_s = 0.6f;
	float switch_margin = 0.25f;
	uint32_t burst_min_rounds = 3, burst_max_rounds = 6;

	float strafe_min_m = 2.0f, strafe_max_m = 6.0f;
	float crouch_beyond_m = 40.0f;
	float retreat_below_hp = 30.0f;
	float grenade_min_m = 15.0f, grenade_max_m = 35.0f, grenade_every_s = 20.0f;
	float knife_within_m = 2.0f;
	float heat_cell_m = 16.0f, heat_decay_s = 30.0f;

	float revive_radius_m = 30.0f;
	float revive_idle_s = 2.0f;
	float revive_margin_s = 1.0f;

	float w_distance = 10.0f, w_aim = 0.5f, w_damage = 1.0f, w_current = 0.3f;
	float damage_memory_s = 3.0f;
	float aims_at_me_deg = 10.0f;
	float engage_strafe_m = 25.0f;
	float hunt_share = 0.85f;
	float blind_fire_share = 0.5f;
	float regen_wait_s = 15.0f;
	float regen_until_hp = 90.0f;

	std::string tuning_cfg;
	std::shared_ptr<TdmBotStats> stats;
	std::shared_ptr<TdmBotObserver> observer;
};

inline constexpr size_t kMaxBotTuningBytes = 64 * 1024;
inline constexpr size_t kMaxBotTuningLines = 1024;
core::Status parse_bot_tuning(std::string_view p_text, TdmBotConfig &r_config);
core::Status load_bot_tuning(const std::string &p_path, TdmBotConfig &r_config);

struct TdmShared;

class TdmBrain final : public CommandSource {
public:
	TdmBrain(std::shared_ptr<TdmShared> p_shared, uint32_t p_index);
	~TdmBrain() override;
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;
	void set_slot(uint8_t p_slot);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

core::Status add_tdm_bots(GameServer &r_server, const GameServerConfig &p_config);

struct TdmBrainHost {
	const ServerView *view = nullptr;
	const Gunplay *gunplay = nullptr;
	std::shared_ptr<const BotLevel> nav;
	sim::bots::WalkerParams walker;
	float eye_heights[3] = { 1.6f, 1.0f, 0.3f };
	uint64_t match_seed = 0;
	bool rules = false;
	sim::Vec3 heat_origin{ -512.0f, 0.0f, -512.0f };
};
core::Result<std::shared_ptr<TdmShared>> make_tdm_shared(const TdmBrainHost &p_host, std::shared_ptr<const TdmBotConfig> p_config);

}
