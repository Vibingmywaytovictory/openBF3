#include <doctest/doctest.h>

#include "bf3/ai/bot/bot_main.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/percentile.h"
#include "bf3/host/game_server.h"
#include "bf3/host/squads.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/sim/command.h"

#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

constexpr size_t kSlots = 64;
constexpr float kMedianMetres = 20.0f;
constexpr float kNearMetres = 30.0f;
constexpr double kNearShare = 0.70;
constexpr double kReachShare = 0.90;
constexpr sim::Tick kReachTicks = 60 * 60;

bool alive(const sim::SoldierState *p_state) {
	return p_state != nullptr && (p_state->flags & sim::soldier_flags::kAlive) != 0;
}

float metres(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct ActivityLog final : host::TdmBotObserver {
	std::array<host::TdmBotActivity, kSlots> activity{};

	void commanded(sim::Tick, uint8_t p_bot, host::TdmBotActivity p_activity, const sim::Command &) override {
		if (p_bot < kSlots) {
			activity[p_bot] = p_activity;
		}
	}
};

tt::TdmRunConfig bots_only(uint64_t p_seed, int p_kill_limit) {
	tt::TdmRunConfig c;
	c.seed = p_seed;
	c.bots = 32;
	c.peer = false;
	c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = false;
	c.rules.revive = true;
	c.rules.kill_limit_override = p_kill_limit;
	c.rules.preround_override_s = 0.0f;
	c.rules.max_players = 32;
	c.after_round_ticks = 0;
	c.hashes = false;
	return c;
}

struct Cohesion {
	std::vector<float> distances;
	uint64_t samples = 0, near = 0;
	uint32_t bots = 0, unsquadded = 0, oversized = 0;
	uint64_t squad_spawns = 0, spawns = 0;
	tt::TdmRunResult result;
	std::string error;
};

Cohesion run_cohesion(const tt::TdmLevel &p_level, uint64_t p_seed) {
	Cohesion out;
	tt::TdmRunConfig c = bots_only(p_seed, 50);
	const auto log = std::make_shared<ActivityLog>();
	c.configure_bots = [log](host::TdmBotConfig &r_bots) {
		r_bots.tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		r_bots.observer = log;
	};
	std::array<uint8_t, kSlots> seen{};
	std::array<uint32_t, kSlots> origin_life{};
	std::array<sim::Vec3, kSlots> origin{};
	c.on_tick = [&](sim::Tick, host::GameServer &r_server, tt::ScriptedPeer *) {
		const host::TdmRules *rules = r_server.rules();
		if (rules == nullptr || rules->phase() != net::MatchPhase::InRound) {
			return;
		}
		const host::SquadTable &squads = rules->squads();
		for (const net::PlayerRow &row : rules->players()) {
			if (!row.bot || row.slot >= kSlots || seen[row.slot] != 0) {
				continue;
			}
			seen[row.slot] = 1;
			++out.bots;
			const uint8_t squad = squads.squad_of(row.slot);
			out.unsquadded += squad == 0 ? 1u : 0u;
			out.oversized += squads.members(squads.team_of(row.slot), squad).size() > squads.squad_size() ? 1u : 0u;
		}
		for (uint8_t team = 1; team <= host::kSquadTeams; ++team) {
			for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
				const std::span<const uint8_t> members = squads.members(team, q);
				if (members.size() < 2) {
					continue;
				}
				const uint8_t leader = members.front();
				const sim::SoldierState *lead = r_server.soldier(leader);
				if (!alive(lead)) {
					continue;
				}
				if (origin_life[leader] != lead->life + 1u) {
					origin_life[leader] = lead->life + 1u;
					origin[leader] = lead->position;
				}
				if (metres(lead->position, origin[leader]) <= kNearMetres) {
					continue;
				}
				for (size_t i = 1; i < members.size(); ++i) {
					const sim::SoldierState *member = r_server.soldier(members[i]);
					if (!alive(member) || log->activity[members[i]] == host::TdmBotActivity::Fight) {
						continue;
					}
					const float d = metres(member->position, lead->position);
					out.distances.push_back(d);
					++out.samples;
					out.near += d <= kNearMetres ? 1u : 0u;
				}
			}
		}
	};
	core::Result<tt::TdmRunResult> r = tt::run_tdm(p_level, c);
	if (!r) {
		out.error = r.error().message;
		return out;
	}
	out.result = std::move(*r);
	out.squad_spawns = out.result.rules.squad_spawns;
	out.spawns = out.result.rules.spawns_scored + out.result.rules.spawn_fallbacks + out.squad_spawns;
	return out;
}

struct UrgencyLog final : host::TdmBotObserver {
	std::array<host::TdmBotActivity, kSlots> activity{};
	std::array<bool, kSlots> holding{};
	std::array<sim::Vec3, kSlots> position{}, target{};
	float far_m = 0.0f;
	uint64_t samples = 0, move_to_first = 0;

	void commanded(sim::Tick, uint8_t p_bot, host::TdmBotActivity p_activity, const sim::Command &) override {
		if (p_bot < kSlots) {
			activity[p_bot] = p_activity;
		}
	}
	void urgencies(sim::Tick, uint8_t p_bot, const ai::BotMain &p_bot_main) override {
		if (p_bot >= kSlots || !holding[p_bot] || activity[p_bot] == host::TdmBotActivity::Fight ||
				metres(position[p_bot], target[p_bot]) < far_m) {
			return;
		}
		const float move_to = p_bot_main.weighted_urgency(ai::InfantryBehaviour::MoveTo);
		++samples;
		move_to_first += move_to > p_bot_main.weighted_urgency(ai::InfantryBehaviour::Idle) &&
						move_to > p_bot_main.weighted_urgency(ai::InfantryBehaviour::Random)
				? 1u
				: 0u;
	}
};

struct Mission {
	uint8_t team = 0, squad = 0, slot = 0;
	uint32_t life = 0;
	sim::Tick start = 0;
	host::SquadOrderState order;
	bool open = true, reached = false, died = false, superseded = false, late = false;
};

struct Orders {
	std::vector<Mission> missions;
	uint64_t orders = 0;
	uint64_t urgency_samples = 0, move_to_first = 0;
	tt::TdmRunResult result;
	std::string error;
};

bool same_order(const host::SquadOrderState &p_a, const host::SquadOrderState &p_b) {
	return p_a.kind == p_b.kind && p_a.area == p_b.area && p_a.position[0] == p_b.position[0] &&
			p_a.position[1] == p_b.position[1] && p_a.position[2] == p_b.position[2];
}

Orders run_orders(const tt::TdmLevel &p_level, uint64_t p_seed, float p_arrive_m, float p_far_m) {
	Orders out;
	tt::TdmRunConfig c = bots_only(p_seed, 50);
	const auto log = std::make_shared<UrgencyLog>();
	log->far_m = p_far_m;
	c.configure_bots = [log](host::TdmBotConfig &r_bots) {
		r_bots.tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		r_bots.observer = log;
	};
	std::array<std::array<host::SquadOrderState, net::kMaxSquads>, host::kSquadTeams> last{};
	c.on_tick = [&](sim::Tick p_tick, host::GameServer &r_server, tt::ScriptedPeer *) {
		const host::TdmRules *rules = r_server.rules();
		if (rules == nullptr) {
			return;
		}
		const host::SquadTable &squads = rules->squads();
		log->holding.fill(false);
		for (Mission &m : out.missions) {
			if (!m.open) {
				continue;
			}
			const sim::SoldierState *s = r_server.soldier(m.slot);
			const host::SquadOrderState &now = squads.order(m.team, m.squad);
			if (!alive(s) || s->life != m.life || squads.squad_of(m.slot) != m.squad) {
				m.died = true;
			} else if (metres(s->position, m.order.position) <= p_arrive_m) {
				m.reached = true;
			} else if (now.kind != net::SquadOrderKind::None && !same_order(now, m.order)) {
				m.superseded = true;
			} else if (p_tick - m.start > kReachTicks) {
				m.late = true;
			}
			m.open = !(m.died || m.reached || m.superseded || m.late);
			if (m.open && same_order(now, m.order)) {
				log->holding[m.slot] = true;
				log->position[m.slot] = s->position;
				log->target[m.slot] = m.order.position;
			}
		}
		for (uint8_t team = 1; team <= host::kSquadTeams; ++team) {
			for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
				const host::SquadOrderState &now = squads.order(team, q);
				host::SquadOrderState &was = last[team - 1u][q - 1u];
				if (same_order(now, was)) {
					continue;
				}
				was = now;
				if (now.kind == net::SquadOrderKind::None || rules->phase() != net::MatchPhase::InRound) {
					continue;
				}
				++out.orders;
				const std::span<const uint8_t> members = squads.members(team, q);
				for (size_t i = 1; i < members.size(); ++i) {
					const sim::SoldierState *s = r_server.soldier(members[i]);
					if (!alive(s)) {
						continue;
					}
					Mission m;
					m.team = team;
					m.squad = q;
					m.slot = members[i];
					m.life = s->life;
					m.start = p_tick;
					m.order = now;
					out.missions.push_back(m);
				}
			}
		}
	};
	core::Result<tt::TdmRunResult> r = tt::run_tdm(p_level, c);
	if (!r) {
		out.error = r.error().message;
		return out;
	}
	out.result = std::move(*r);
	out.urgency_samples = log->samples;
	out.move_to_first = log->move_to_first;
	return out;
}

}

TEST_SUITE("host-tdm-squads") {
	TEST_CASE("bots-only TDM, seeds 1-3: every bot in a squad, members keep up with a leader who left its deploy "
			  "point, outside a fight, bots spawn on squad mates") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const std::vector<Cohesion> runs =
				core::parallel_map<Cohesion>(3, [&](size_t i) { return run_cohesion(*level, uint64_t(i + 1)); }, 3);
		for (size_t i = 0; i < runs.size(); ++i) {
			const Cohesion &run = runs[i];
			REQUIRE_MESSAGE(run.error.empty(), run.error);
			const float median = core::nearest_rank(run.distances, 0.5);
			const double near = run.samples > 0 ? double(run.near) / double(run.samples) : 0.0;
			std::printf("host.tdm.squads seed %zu: %u bots, %u without a squad, %u in an oversized squad; %llu member "
						"ticks outside a fight with the leader away from where it deployed, "
						"median %.1f m to the leader, %.1f %% within %.0f m; spawns %llu, on a squad mate %llu; kills "
						"%d / %d in %u ticks\n",
					i + 1, run.bots, run.unsquadded, run.oversized, (unsigned long long)run.samples, median,
					100.0 * near, kNearMetres, (unsigned long long)run.spawns, (unsigned long long)run.squad_spawns,
					run.result.kills[1], run.result.kills[2], run.result.ticks);
			CHECK(run.result.failure.empty());
			CHECK(run.result.round_over);
			CHECK(run.bots == 32u);
			CHECK(run.unsquadded == 0u);
			CHECK(run.oversized == 0u);
			CHECK(run.samples > 0u);
			CHECK(median <= kMedianMetres);
			CHECK(near >= kNearShare);
			CHECK(run.squad_spawns > 0u);
		}
	}
	TEST_CASE("host-tdm-squads: install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}

TEST_SUITE("host-tdm-orders") {
	TEST_CASE("bots-only TDM, seeds 4-6: members of a squad given a mission reach its arrive radius within 60 s or die "
			  "trying") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		host::TdmBotConfig tuning;
		REQUIRE(host::load_bot_tuning(OPENBF3_BOT_TUNING_CFG, tuning).ok());
		const float arrive = tuning.behaviours.mission_arrive_m;
		const float far = tuning.behaviours.squad_far_m;
		const std::vector<Orders> runs = core::parallel_map<Orders>(
				3, [&](size_t i) { return run_orders(*level, uint64_t(i + 4), arrive, far); }, 3);
		uint64_t pooled_judged = 0, pooled_kept = 0;
		for (size_t i = 0; i < runs.size(); ++i) {
			const Orders &run = runs[i];
			REQUIRE_MESSAGE(run.error.empty(), run.error);
			uint64_t reached = 0, died = 0, superseded = 0, late = 0, open = 0;
			for (const Mission &m : run.missions) {
				reached += m.reached ? 1u : 0u;
				died += m.died ? 1u : 0u;
				superseded += m.superseded ? 1u : 0u;
				late += m.late ? 1u : 0u;
				open += m.open ? 1u : 0u;
			}
			const uint64_t judged = reached + died + late;
			const double share = judged > 0 ? double(reached + died) / double(judged) : 0.0;
			pooled_judged += judged;
			pooled_kept += reached + died;
			std::printf("host.tdm.orders seed %zu: %llu orders passed down, %zu member missions: %llu reached %.0f m, "
						"%llu died trying, %llu late, "
						"%llu superseded before 60 s, %llu open at the end; %.1f %% reached or died; "
						"MoveTo above Idle and Random in %llu of %llu decisions %.0f m or more from the target; kills "
						"%d / %d\n",
					i + 4, (unsigned long long)run.orders, run.missions.size(), (unsigned long long)reached, arrive,
					(unsigned long long)died, (unsigned long long)late, (unsigned long long)superseded,
					(unsigned long long)open, 100.0 * share, (unsigned long long)run.move_to_first,
					(unsigned long long)run.urgency_samples, far, run.result.kills[1], run.result.kills[2]);
			CHECK(run.result.failure.empty());
			CHECK(run.orders > 0u);
			CHECK(judged > 0u);
			CHECK(run.urgency_samples > 0u);
			CHECK(run.move_to_first == run.urgency_samples);
		}
		const double pooled = pooled_judged > 0 ? double(pooled_kept) / double(pooled_judged) : 0.0;
		std::printf("host.tdm.orders pooled: %llu of %llu judged member missions reached or died trying, %.1f %%\n",
				(unsigned long long)pooled_kept, (unsigned long long)pooled_judged, 100.0 * pooled);
		CHECK(pooled >= kReachShare);
	}
	TEST_CASE("host-tdm-orders: install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}
