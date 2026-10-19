#pragma once
// The TDM bots (docs/design/m2b.md §5, §2.6 #1, #13; declared in W1.0, owner bb). They replace M2a's Free-mode
// FirefightBrains for TDM behind the same CommandSource seam; FirefightBrain stays for the M2a rows.
//
// How they get into the server (#1): GameServerConfig::tdm_bots (a shared TdmBotConfig, null = none). After every M2a
// source (target bots, walkers, S1's bots, the firefight bots), GameServer::init calls add_tdm_bots(*this, config)
// (the named insertion "M2b ru [tdm bots]" in game_server.cpp). It runs after sv_init created the rules, so each
// add_local_soldier gets its team from TdmRules::on_connect ([add], team 0 = AutoTeam's choice). Slots >= 64 are
// spectators with rules on and are never filled (§3.2). Server thread only, serial in slot order.
//
// A brain (bb fills the bodies, §5.1-§5.7): perception (perception.h) every update_ticks staggered by slot; target
// choice (w_d / distance + w_aim + w_dmg + w_cur, switch at a 25 % better score); a reaction delay before the first
// shot; the aim model (aim_model.h); WalkerBrain movement on S1's nav (the last known enemy or heard shot, a contact
// heat grid, a random main-component point; strafe, crouch, retreat below 30 hp); M2a's fire logic (bursts by range,
// reload, sidearm, the M67, the knife); the revive of §5.6 through ServerView::downed() and weapon slot 2. It reads its
// kit through ServerView::kit(slot) (the rules draw it, §3.2) and idles while ServerView::input_restricted() or the
// phase says so. Seeds: bot seed = match_seed ^ slot, each life draws from splitmix64(bot_seed ^ life) (§5.7).

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/sim/bots/aim_model.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/sim_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace bf3::host {

// What the brains counted (server thread writes, any thread reads; relaxed).
struct TdmBotStats {
	std::atomic<uint32_t> bots{ 0 };
	std::atomic<uint64_t> perception_updates{ 0 };
	std::atomic<uint64_t> rays{ 0 }; // perception rays cast
	std::atomic<uint64_t> detections{ 0 }; // enemies reaching detection 1
	std::atomic<uint64_t> heard{ 0 }; // shots heard
	std::atomic<uint64_t> retargets{ 0 };
	std::atomic<uint64_t> bursts{ 0 };
	std::atomic<uint64_t> blind_fire{ 0 }; // bursts at a last-known position (E2 counts them apart)
	std::atomic<uint64_t> grenades{ 0 };
	std::atomic<uint64_t> revive_attempts{ 0 };
	std::atomic<uint64_t> revive_shots{ 0 }; // defib shots fired at a corpse
	std::atomic<uint64_t> retreats{ 0 };
	std::atomic<uint64_t> repaths{ 0 };
	// Per tick (the last tick's, wall nanoseconds on the server thread; E8 reads them through the fixture).
	std::atomic<uint64_t> last_perception_ns{ 0 };
	std::atomic<uint64_t> last_brains_ns{ 0 };
};

struct TdmBotConfig {
	uint32_t count = 31; // 16 v 16 with one human (J6); capped by the free slots below 64
	sim::bots::PerceptionParams perception;
	sim::bots::AimParams aim;
	// Skill per bot from its seed (§5.3, J5).
	float skill_min = 0.3f;
	float skill_max = 0.8f;
	// Reaction delay before the first shot on a new target (§5.2), uniform by skill.
	float reaction_min_s = 0.25f;
	float reaction_max_s = 0.6f;
	float switch_margin = 0.25f; // a new target must score this much better
	// Movement and fighting (§5.4, §5.5).
	float strafe_min_m = 2.0f, strafe_max_m = 6.0f;
	float crouch_beyond_m = 40.0f; // for skill > 0.5
	float retreat_below_hp = 30.0f;
	float grenade_min_m = 15.0f, grenade_max_m = 35.0f, grenade_every_s = 20.0f;
	float knife_within_m = 2.0f;
	float heat_cell_m = 16.0f, heat_decay_s = 30.0f;
	// Revive (§5.6).
	float revive_radius_m = 30.0f;
	float revive_idle_s = 2.0f; // not engaged for this long
	float revive_margin_s = 1.0f; // until_tick - now must exceed the travel time + this
	// config/bot_tuning.cfg (bb): read once by add_tdm_bots over these defaults; empty = the defaults above.
	std::string tuning_cfg;
	std::shared_ptr<TdmBotStats> stats; // null = none
};

// The brains of one GameServer share this (bb; tdm_brain.cpp).
struct TdmShared;

// One bot (bb). Holds the GameServer for view() and gunplay().
class TdmBrain final : public CommandSource {
public:
	TdmBrain(std::shared_ptr<TdmShared> p_shared, uint32_t p_index);
	~TdmBrain() override;
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;
	void set_slot(uint8_t p_slot); // add_tdm_bots tells the brain its slot after add_local_soldier

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// The #1 hook (body bb). W1.0's stub succeeds without GameServerConfig::tdm_bots and refuses with one ("W1.0 stub"),
// so a run that asks for TDM bots before bb lands fails loudly instead of running without them.
core::Status add_tdm_bots(GameServer &r_server, const GameServerConfig &p_config);

} // namespace bf3::host
