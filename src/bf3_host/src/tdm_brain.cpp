#include "bf3/host/tdm_brain.h"

#include "bf3/ai/main/ai_settings.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim_def.h"
#include "tdm_bot_body.h"
#include "tdm_bot_common.h"
#include "tdm_bot_shared.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace bf3::host {

using namespace tdm_bot;

const char *tdm_bot_activity_name(TdmBotActivity p_activity) {
	switch (p_activity) {
		case TdmBotActivity::Dead:
			return "dead";
		case TdmBotActivity::Restricted:
			return "restricted";
		case TdmBotActivity::Roam:
			return "roam";
		case TdmBotActivity::Hunt:
			return "hunt";
		case TdmBotActivity::Fight:
			return "fight";
		case TdmBotActivity::Retreat:
			return "retreat";
		case TdmBotActivity::RegenWait:
			return "regen_wait";
		case TdmBotActivity::Revive:
			return "revive";
		case TdmBotActivity::Grenade:
			return "grenade";
	}
	return "?";
}

struct TdmBrain::Impl : BotBody {
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) {
		TdmShared &sh = *shared;
		const uint64_t t0 = now_ns();
		sh.begin_tick(p_tick);
		sim::Command c = run(p_tick, p_state);
		sh.brains_ns += now_ns() - t0;
		if (sh.stats != nullptr) {
			sh.stats->last_perception_ns.store(sh.perception_ns, std::memory_order_relaxed);
			sh.stats->last_brains_ns.store(sh.brains_ns, std::memory_order_relaxed);
		}
		if (sh.observer != nullptr) {
			sh.observer->commanded(p_tick, slot, activity, c);
		}
		return c;
	}

	sim::Command run(sim::Tick p_tick, const sim::SoldierState &p_state) {
		TdmShared &sh = *shared;
		sim::Command cmd = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		cmd.flags = 0;
		if (slot == 0xFF || !is_alive(p_state)) {
			targeting.drop();
			set_activity(p_tick, TdmBotActivity::Dead);
			return cmd;
		}
		if (p_state.life != life) {
			new_life(p_tick, p_state);
		}
		const ServerView &view = *sh.host.view;
		if (view.input_restricted() || view.phase() != net::MatchPhase::InRound) {
			restricted(p_tick, cmd);
			return cmd;
		}
		const BotFrame frame = sense(p_tick, p_state, view);
		const TdmBotActivity act = choose_activity(frame);
		set_activity(p_tick, act);

		BotIntent intent = plan(frame, act);
		const bool aligned = turn(frame, intent);
		compose(frame, act, intent, aligned, cmd);
		return cmd;
	}

	TdmBotActivity choose_activity(const BotFrame &p_frame) {
		TdmShared &sh = *shared;
		const TdmBotConfig &c = cfg();
		const BotFrame &f = p_frame;
		if (activity == TdmBotActivity::Retreat || activity == TdmBotActivity::RegenWait) {
			if (retreat.recovered(c, activity == TdmBotActivity::RegenWait, f.tick, f.self.health)) {
				activity = TdmBotActivity::Roam;
			}
		}
		const bool low = f.self.health < c.retreat_below_hp;
		TdmBotActivity act = TdmBotActivity::Roam;
		if (f.revive != nullptr) {
			act = TdmBotActivity::Revive;
		} else if (grenade.active()) {
			act = TdmBotActivity::Grenade;
		} else if (activity == TdmBotActivity::Retreat && !f.visible && retreat.cleared(f.tick)) {
			act = TdmBotActivity::RegenWait;
			retreat.begin_wait(f.tick);
		} else if (activity == TdmBotActivity::Retreat && retreat.running(f.tick) && low) {
			act = TdmBotActivity::Retreat;
		} else if (activity == TdmBotActivity::RegenWait && !f.visible && !f.shot_recent) {
			act = TdmBotActivity::RegenWait;
		} else if (low && (f.visible || f.shot_recent) && sh.nav != nullptr && activity != TdmBotActivity::Retreat && activity != TdmBotActivity::RegenWait &&
				retreat.start(sh, navigation, f.tick, f.self, f.visible ? f.states[size_t(f.target)].position : awareness.shot_from(), rng)) {
			act = TdmBotActivity::Retreat;
		} else if (f.visible) {
			act = TdmBotActivity::Fight;
		} else if (grenade.start(sh, targeting, awareness.perception(), f.tick, f.self, f.states, f.arms.rounds(kSlotGrenade) > 0)) {
			act = TdmBotActivity::Grenade;
		} else if (targeting.blind_until() > f.tick) {
			act = TdmBotActivity::Fight;
		} else {
			act = hunt.wanted(f.tick, awareness.perception(), f.self) ? TdmBotActivity::Hunt : TdmBotActivity::Roam;
		}
		if (act == TdmBotActivity::Retreat && f.visible) {
			retreat.seen(f.tick);
		}
		if (act == TdmBotActivity::Hunt && activity != TdmBotActivity::Hunt) {
			hunt.begin(f.tick);
		}
		return act;
	}

	BotIntent plan(const BotFrame &p_frame, TdmBotActivity p_activity) {
		const BotFrame &f = p_frame;
		BotIntent i = begin_plan(p_activity);
		switch (p_activity) {
			case TdmBotActivity::Fight:
				fight(f, i);
				break;
			case TdmBotActivity::Revive:
				approach_revive(f, i);
				break;
			case TdmBotActivity::Retreat:
				walk_facing(f, i);
				break;
			case TdmBotActivity::RegenWait:
				wait_in_cover(f, i);
				break;
			case TdmBotActivity::Grenade:
				throw_step(f, i);
				break;
			case TdmBotActivity::Hunt:
				hunt_step(f, i);
				break;
			case TdmBotActivity::Roam:
			default:
				roam(f, i);
				break;
		}
		face_shots(f, p_activity, i);
		return i;
	}
};

TdmBrain::TdmBrain(std::shared_ptr<TdmShared> p_shared, uint32_t p_index) :
		impl_(std::make_unique<Impl>()) {
	impl_->attach(std::move(p_shared), p_index);
}

TdmBrain::~TdmBrain() = default;

sim::Command TdmBrain::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	if (impl_->shared == nullptr) {
		return sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
	}
	return impl_->command(p_tick, p_state);
}

void TdmBrain::set_slot(uint8_t p_slot) {
	impl_->BotBody::set_slot(p_slot);
}

namespace {

using Key = core::KeyDef<TdmBotConfig>;
using core::KeyFault;
using core::KeyKind;
using core::KeyOutcome;

bool parse_number(std::string_view p_text, double &r_value) {
	if (p_text.empty() || p_text.size() > 32) {
		return false;
	}
	char buf[40];
	std::memcpy(buf, p_text.data(), p_text.size());
	buf[p_text.size()] = '\0';
	char *end = nullptr;
	const double v = std::strtod(buf, &end);
	if (end == buf || *end != '\0' || !std::isfinite(v)) {
		return false;
	}
	r_value = v;
	return true;
}

KeyOutcome bot_number(const Key &p_key, std::string_view p_value, double &r_value) {
	if (!parse_number(p_value, r_value)) {
		return { KeyFault::NotANumber };
	}
	if (r_value < p_key.min || r_value > p_key.max) {
		return { KeyFault::OutOfRange, r_value };
	}
	return {};
}

KeyOutcome bot_float(const Key &p_key, std::string_view p_value, TdmBotConfig &r_config) {
	double v = 0.0;
	const KeyOutcome outcome = bot_number(p_key, p_value, v);
	if (outcome.fault == KeyFault::None) {
		*static_cast<float *>(p_key.field(r_config)) = float(v);
	}
	return outcome;
}

KeyOutcome bot_count(const Key &p_key, std::string_view p_value, TdmBotConfig &r_config) {
	double v = 0.0;
	const KeyOutcome outcome = bot_number(p_key, p_value, v);
	if (outcome.fault == KeyFault::None) {
		*static_cast<uint32_t *>(p_key.field(r_config)) = uint32_t(v);
	}
	return outcome;
}

#define BF3_BOT_FIELD(f) [](TdmBotConfig &c) -> void * { return &c.f; }

const Key kKeys[] = {
	{ "skill_min", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(skill_min), {}, bot_float },
	{ "skill_max", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(skill_max), {}, bot_float },
	{ "reaction_min_s", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(reaction_min_s), {}, bot_float },
	{ "reaction_max_s", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(reaction_max_s), {}, bot_float },
	{ "switch_margin", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(switch_margin), {}, bot_float },
	{ "strafe_min_m", KeyKind::Custom, 0.0f, 50.0f, BF3_BOT_FIELD(strafe_min_m), {}, bot_float },
	{ "strafe_max_m", KeyKind::Custom, 0.0f, 50.0f, BF3_BOT_FIELD(strafe_max_m), {}, bot_float },
	{ "crouch_beyond_m", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(crouch_beyond_m), {}, bot_float },
	{ "retreat_below_hp", KeyKind::Custom, 0.0f, 100.0f, BF3_BOT_FIELD(retreat_below_hp), {}, bot_float },
	{ "grenade_min_m", KeyKind::Custom, 0.0f, 200.0f, BF3_BOT_FIELD(grenade_min_m), {}, bot_float },
	{ "grenade_max_m", KeyKind::Custom, 0.0f, 200.0f, BF3_BOT_FIELD(grenade_max_m), {}, bot_float },
	{ "grenade_every_s", KeyKind::Custom, 1.0f, 3600.0f, BF3_BOT_FIELD(grenade_every_s), {}, bot_float },
	{ "knife_within_m", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(knife_within_m), {}, bot_float },
	{ "heat_cell_m", KeyKind::Custom, 1.0f, 1000.0f, BF3_BOT_FIELD(heat_cell_m), {}, bot_float },
	{ "heat_decay_s", KeyKind::Custom, 0.1f, 3600.0f, BF3_BOT_FIELD(heat_decay_s), {}, bot_float },
	{ "revive_radius_m", KeyKind::Custom, 0.0f, 500.0f, BF3_BOT_FIELD(revive_radius_m), {}, bot_float },
	{ "revive_idle_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(revive_idle_s), {}, bot_float },
	{ "revive_margin_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(revive_margin_s), {}, bot_float },
	{ "w_distance", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_distance), {}, bot_float },
	{ "w_aim", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_aim), {}, bot_float },
	{ "w_damage", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_damage), {}, bot_float },
	{ "w_current", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(w_current), {}, bot_float },
	{ "damage_memory_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(damage_memory_s), {}, bot_float },
	{ "aims_at_me_deg", KeyKind::Custom, 0.0f, 90.0f, BF3_BOT_FIELD(aims_at_me_deg), {}, bot_float },
	{ "engage_strafe_m", KeyKind::Custom, 0.0f, 1000.0f, BF3_BOT_FIELD(engage_strafe_m), {}, bot_float },
	{ "hunt_share", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(hunt_share), {}, bot_float },
	{ "blind_fire_share", KeyKind::Custom, 0.0f, 1.0f, BF3_BOT_FIELD(blind_fire_share), {}, bot_float },
	{ "regen_wait_s", KeyKind::Custom, 0.0f, 600.0f, BF3_BOT_FIELD(regen_wait_s), {}, bot_float },
	{ "regen_until_hp", KeyKind::Custom, 0.0f, 100.0f, BF3_BOT_FIELD(regen_until_hp), {}, bot_float },
	{ "perception.fov_h_deg", KeyKind::Custom, 1.0f, 360.0f, BF3_BOT_FIELD(perception.fov_h_deg), {}, bot_float },
	{ "perception.fov_v_deg", KeyKind::Custom, 1.0f, 180.0f, BF3_BOT_FIELD(perception.fov_v_deg), {}, bot_float },
	{ "perception.max_range_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.max_range_m), {}, bot_float },
	{ "perception.near_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.near_m), {}, bot_float },
	{ "perception.detect_near_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(perception.detect_near_s), {},
			bot_float },
	{ "perception.far_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.far_m), {}, bot_float },
	{ "perception.detect_far_s", KeyKind::Custom, 0.0f, 60.0f, BF3_BOT_FIELD(perception.detect_far_s), {}, bot_float },
	{ "perception.forget_s", KeyKind::Custom, 0.0f, 600.0f, BF3_BOT_FIELD(perception.forget_s), {}, bot_float },
	{ "perception.hearing_radius_m", KeyKind::Custom, 0.0f, 2000.0f, BF3_BOT_FIELD(perception.hearing_radius_m), {},
			bot_float },
	{ "perception.hearing_error_m", KeyKind::Custom, 0.0f, 200.0f, BF3_BOT_FIELD(perception.hearing_error_m), {},
			bot_float },
	{ "perception.head_height_m", KeyKind::Custom, 0.0f, 3.0f, BF3_BOT_FIELD(perception.head_height_m), {}, bot_float },
	{ "perception.chest_height_m", KeyKind::Custom, 0.0f, 3.0f, BF3_BOT_FIELD(perception.chest_height_m), {},
			bot_float },
	{ "aim.e0_deg", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.e0_deg), {}, bot_float },
	{ "aim.e_inf_deg", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.e_inf_deg), {}, bot_float },
	{ "aim.tau_s", KeyKind::Custom, 0.001f, 60.0f, BF3_BOT_FIELD(aim.tau_s), {}, bot_float },
	{ "aim.moving_penalty_deg", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.moving_penalty_deg), {}, bot_float },
	{ "aim.crossing_deg_per_mps", KeyKind::Custom, 0.0f, 45.0f, BF3_BOT_FIELD(aim.crossing_deg_per_mps), {},
			bot_float },
	{ "aim.skill_scale", KeyKind::Custom, 0.0f, 10.0f, BF3_BOT_FIELD(aim.skill_scale), {}, bot_float },
	{ "count", KeyKind::Custom, 0, 64, BF3_BOT_FIELD(count), {}, bot_count },
	{ "burst_min_rounds", KeyKind::Custom, 1, 100, BF3_BOT_FIELD(burst_min_rounds), {}, bot_count },
	{ "burst_max_rounds", KeyKind::Custom, 1, 100, BF3_BOT_FIELD(burst_max_rounds), {}, bot_count },
	{ "perception.update_ticks", KeyKind::Custom, 1, 600, BF3_BOT_FIELD(perception.update_ticks), {}, bot_count },
	{ "perception.max_candidates", KeyKind::Custom, 1, 64, BF3_BOT_FIELD(perception.max_candidates), {}, bot_count },
};

#undef BF3_BOT_FIELD

std::string bot_fault(const core::KeyFaultInfo &p_info) {
	const std::string where = core::key_fault_where(p_info);
	const std::string key(p_info.key);
	switch (p_info.fault) {
		case KeyFault::TooLarge:
			return std::string(p_info.what) + ": " + std::to_string(p_info.count) + " bytes, more than " +
					std::to_string(p_info.limit);
		case KeyFault::NoEquals:
			return where + ": no '='";
		case KeyFault::NotANumber:
			return where + ": '" + key + "' needs a finite number";
		case KeyFault::OutOfRange:
			return where + ": '" + key + "' = " + std::to_string(p_info.number) + " is outside [" +
					std::to_string(p_info.min) + ", " + std::to_string(p_info.max) + "]";
		case KeyFault::UnknownKey:
			return where + ": unknown key '" + key + "'";
		default:
			return core::key_fault_message(p_info);
	}
}

core::KeyOutcome ai_setting(std::string_view p_key, std::string_view p_value, TdmBotConfig &) {
	return ai::check_ai_setting(p_key, p_value);
}

const core::KeyValueGrammar<TdmBotConfig> kGrammar{ "bot_tuning", kKeys, kMaxBotTuningBytes, kMaxBotTuningLines,
	SIZE_MAX, false, '\n', true, true, bot_fault, nullptr, ai_setting };

}

core::Status parse_bot_tuning(std::string_view p_text, TdmBotConfig &r_config) {
	core::Result<TdmBotConfig> parsed = core::parse_key_values(p_text, kGrammar, r_config);
	if (!parsed) {
		return parsed.error();
	}
	r_config = std::move(*parsed);
	return core::Status::success();
}

core::Status load_bot_tuning(const std::string &p_path, TdmBotConfig &r_config) {
	std::ifstream f(p_path, std::ios::binary);
	if (!f) {
		return core::make_error("bot_tuning: cannot open " + p_path);
	}
	std::string text;
	text.resize(kMaxBotTuningBytes + 1);
	f.read(text.data(), std::streamsize(text.size()));
	text.resize(size_t(f.gcount()));
	const core::Status st = parse_bot_tuning(text, r_config);
	if (!st.ok()) {
		return core::error_at(p_path, st.error());
	}
	return core::Status::success();
}

core::Status add_tdm_bots(GameServer &r_server, const GameServerConfig &p_config) {
	if (p_config.tdm_bots == nullptr || p_config.tdm_bots->count == 0) {
		return core::Status::success();
	}
	if (p_config.level == nullptr) {
		return core::make_error("tdm bots: no level");
	}
	std::shared_ptr<const TdmBotConfig> config = p_config.tdm_bots;
	if (!config->tuning_cfg.empty()) {
		auto tuned = std::make_shared<TdmBotConfig>(*config);
		BF3_TRY(load_bot_tuning(config->tuning_cfg, *tuned));
		tuned->count = config->count;
		config = tuned;
	}
	TdmBrainHost host;
	host.view = &r_server.view();
	host.gunplay = r_server.gunplay();
	host.match_seed = r_server.gunplay() != nullptr ? r_server.gunplay()->match_seed() : p_config.seed;
	host.rules = p_config.rules.mode == MatchRulesConfig::Mode::Tdm;
	for (size_t i = 0; i < 3; ++i) {
		if (p_config.level->motor.poses[i].eye_height > 0.0f) {
			host.eye_heights[i] = p_config.level->motor.poses[i].eye_height;
		}
	}
	if (p_config.bots != nullptr && p_config.bots->level != nullptr) {
		host.nav = p_config.bots->level;
		host.walker = p_config.bots->walker;
	}

	if (!p_config.level->spawns.empty()) {
		double sx = 0.0, sz = 0.0;
		for (const sim::SpawnPoint &p : p_config.level->spawns) {
			sx += p.position[0];
			sz += p.position[2];
		}
		const float half = 0.5f * float(kHeatCells) * std::max(config->heat_cell_m, 1.0f);
		const float cx = float(sx / double(p_config.level->spawns.size())), cz = float(sz / double(p_config.level->spawns.size()));
		if (std::isfinite(cx) && std::isfinite(cz)) {
			host.heat_origin = { cx - half, 0.0f, cz - half };
		}
	}
	core::Result<std::shared_ptr<TdmShared>> made = make_tdm_shared(host, config);
	if (!made) {
		return made.error();
	}
	std::shared_ptr<TdmShared> shared = std::move(*made);

	const GameServerStats st = r_server.stats();
	const size_t limit = std::min<size_t>({ st.slots.size(), size_t(p_config.max_slots), size_t(net::kMaxPlayerRows) });
	uint32_t free_below = 0;
	for (size_t s = 0; s < limit; ++s) {
		free_below += st.slots[s].used ? 0u : 1u;
	}
	if (st.slots.empty()) {
		free_below = std::min<uint32_t>(p_config.max_slots, uint32_t(net::kMaxPlayerRows));
	}
	const uint32_t count = std::min(config->count, free_below);
	for (uint32_t i = 0; i < count; ++i) {
		auto brain = std::make_unique<TdmBrain>(shared, i);
		TdmBrain *raw = brain.get();

		const int team = host.rules ? 0 : 1 + int((i + i / 4u) % 2u);
		const int slot = r_server.add_local_bot(std::move(brain), team);
		if (slot < 0) {
			return core::make_error("tdm bots: no slot for bot " + std::to_string(i) + " of " + std::to_string(count));
		}
		raw->set_slot(uint8_t(slot));
		if (shared->stats != nullptr) {
			shared->stats->bots.fetch_add(1, std::memory_order_relaxed);
		}
	}
	return core::Status::success();
}

}
