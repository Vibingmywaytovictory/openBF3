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
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/sim_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bf3::host {

struct BotLevel; // bot_fill.h
class Gunplay; // gunplay.h
class ServerView; // server_view.h

// bb (as built, docs/design/m2b-bb.md): what a brain is doing in a tick (the observer's activity callback; E2 leaves
// the Restricted, RegenWait and Revive ticks out of the stuck windows).
enum class TdmBotActivity : uint8_t { Dead = 0, Restricted = 1, Roam = 2, Hunt = 3, Fight = 4, Retreat = 5, RegenWait = 6, Revive = 7, Grenade = 8 };
inline constexpr size_t kTdmBotActivityCount = 9;
const char *tdm_bot_activity_name(TdmBotActivity p_activity);

// bb (as built): a test's look into the brains (E2's independent checks). Called on the server thread inside
// GameServer::tick (the command-source step), in slot order; p_view is the server's view of that step.
class TdmBotObserver {
public:
	virtual ~TdmBotObserver() = default;
	// p_bot's perception raised p_target to detection 1 this tick, from p_eye (E2: a clear eye-to-head or eye-to-chest
	// ray must exist at that tick).
	virtual void detected(const ServerView &p_view, sim::Tick p_tick, uint8_t p_bot, uint8_t p_target, const sim::Vec3 &p_eye) {
		(void)p_view, (void)p_tick, (void)p_bot, (void)p_target, (void)p_eye;
	}
	// p_bot's command holds the trigger: aimed at p_target whose last clear ray was at p_last_visible, or blind fire at
	// a last-known position (p_blind; then p_target may be -1). Not called for the defibrillator.
	virtual void fired(const ServerView &p_view, sim::Tick p_tick, uint8_t p_bot, int p_target, sim::Tick p_last_visible, bool p_blind) {
		(void)p_view, (void)p_tick, (void)p_bot, (void)p_target, (void)p_last_visible, (void)p_blind;
	}
	virtual void activity(sim::Tick p_tick, uint8_t p_bot, TdmBotActivity p_activity) {
		(void)p_tick, (void)p_bot, (void)p_activity;
	}
};

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
	// bb (as built).
	std::atomic<uint64_t> reloads{ 0 };
	std::atomic<uint64_t> sidearm_switches{ 0 };
	std::atomic<uint64_t> knife_attacks{ 0 };
	std::atomic<uint64_t> revive_aborts{ 0 }; // shot at, the corpse gone or the window over
	std::atomic<uint64_t> walker_resets{ 0 }; // a new movement goal handed to the walker
	std::atomic<uint64_t> heat_goals{ 0 }; // roam goals drawn from the contact heat grid
	std::atomic<uint32_t> walk_unavailable{ 0 }; // 1: no BotLevel (GameServerConfig::bots): the bots stand and fight
	std::atomic<uint64_t> activity_ticks[kTdmBotActivityCount]{}; // bot-ticks per TdmBotActivity
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
	uint32_t burst_min_rounds = 3, burst_max_rounds = 6; // bb: §5.5 bursts (max close, min at 60 m, single shots beyond)
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
	// bb (as built): target choice (§5.2): w_distance / distance + w_aim (it aims at me) + w_damage (it shot at me in
	// the last damage_memory_s) + w_current (the current target) [inferred].
	float w_distance = 10.0f, w_aim = 0.5f, w_damage = 1.0f, w_current = 0.3f;
	float damage_memory_s = 3.0f;
	float aims_at_me_deg = 10.0f; // an enemy view within this of the line to me "aims at me"
	float engage_strafe_m = 25.0f; // closer: strafe; farther: push towards the target (§5.4)
	float hunt_share = 0.85f; // walker legs that go to the brain's goal (else S1's random nav points)
	float blind_fire_share = 0.5f; // a lost target draws one blind burst at its last-known position with this chance
	float regen_wait_s = 15.0f; // the longest regen wait after a retreat
	float regen_until_hp = 90.0f;
	// config/bot_tuning.cfg (bb): read once by add_tdm_bots over these defaults; empty = the defaults above.
	std::string tuning_cfg;
	std::shared_ptr<TdmBotStats> stats; // null = none
	std::shared_ptr<TdmBotObserver> observer; // bb: null = none (tests)
};

// bb: config/bot_tuning.cfg ("key = value", '#' comments, one key per line; every key names a TdmBotConfig field,
// perception.* and aim.* the nested params). Unknown keys, non-finite or out-of-range numbers and files above
// kMaxBotTuningBytes fail; the keys present overwrite r_config, the rest keep their values.
inline constexpr size_t kMaxBotTuningBytes = 64 * 1024;
core::Status parse_bot_tuning(std::string_view p_text, TdmBotConfig &r_config);
core::Status load_bot_tuning(const std::string &p_path, TdmBotConfig &r_config);

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

// The #1 hook (body bb). Succeeds without GameServerConfig::tdm_bots (or with count 0). With rules on
// (GameServerConfig::rules.mode == Tdm) each bot gets team 0 (AutoTeam's choice, [add]) and its kit from
// ServerView::kit; without rules the bots take M2a's firefight teams 1 + ((i + i / 4) % 2) and gunplay's kit
// (slot % 4), so the brains also run on M2a levels and the synthetic floor. At most the free slots below
// net::kMaxPlayerRows (64) are filled (slots >= 64 are spectators with rules on). Fails without a level, or when the
// tuning file does not parse. Walking needs GameServerConfig::bots' BotLevel (else the bots stand and fight, counted).
core::Status add_tdm_bots(GameServer &r_server, const GameServerConfig &p_config);

// bb (as built): the test seam for brains over any ServerView (fakes in host.tdm.bots.unit). One TdmShared serves the
// brains of one server; make_tdm_shared fails without a view.
struct TdmBrainHost {
	const ServerView *view = nullptr;
	const Gunplay *gunplay = nullptr; // null: no weapons (the brain moves and aims only)
	std::shared_ptr<const BotLevel> nav; // null: no walking
	sim::bots::WalkerParams walker;
	float eye_heights[3] = { 1.6f, 1.0f, 0.3f }; // MotorParams::poses[].eye_height
	uint64_t match_seed = 0;
	bool rules = false; // kits from ServerView::kit (else Gunplay::kit)
	sim::Vec3 heat_origin{ -512.0f, 0.0f, -512.0f }; // the contact heat grid's (x, z) corner
};
core::Result<std::shared_ptr<TdmShared>> make_tdm_shared(const TdmBrainHost &p_host, std::shared_ptr<const TdmBotConfig> p_config);

} // namespace bf3::host
