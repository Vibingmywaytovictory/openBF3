#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "pipe_transport.h"
#include "tdm_fixture.h"
#include "vehicles/client_vehicles.h"
#include "vehicles/server_vehicles.h"
#include "vehicles/vehicle_gunnery.h"
#include "vehicles/vehicle_hits.h"
#include "vehicles/vehicle_scope.h"

#include "bf3/core/gameplay/eor_fading.h"
#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/level/eor_camera.h"
#include "bf3/core/math.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/text.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/client_session.h"
#include "bf3/host/conquest_wire.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/load_bots.h"
#include "bf3/host/loadout.h"
#include "bf3/host/server_level.h"
#include "bf3/host/spotter.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/logic/conquest_data.h"
#include "bf3/sim/logic/conquest_natives.h"
#include "bf3/sim/logic/round_natives.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/vehicles/land_rules.h"
#include "bf3/sim/vehicles/vehicle_input.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_install.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace bf3;
namespace tdm = bf3::test::tdm;
namespace tg = bf3::test::gunplay;
namespace sw = bf3::sim::weapons;
namespace tp = bf3::test::pipe;

namespace {

constexpr uint64_t kM2aKitHash = 0x31b45dc7cc3f4271ull;
constexpr uint64_t kM2aWeaponHash = 0x0a9b0425100867c2ull;
constexpr uint64_t kM2aMotorHash = 0x5030b00de56f1ee8ull;

using tdm::distance;
using tdm::is_alive;

sim::Vec3 add(const sim::Vec3 &p_a, float p_x, float p_y, float p_z) {
	return { p_a[0] + p_x, p_a[1] + p_y, p_a[2] + p_z };
}

struct Script final : host::CommandSource {
	std::function<sim::Command(sim::Tick, const sim::SoldierState &)> fn;
	int slot = -1;
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		sim::Command c = fn ? fn(p_tick, p_state) : sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		c.tick = p_tick;
		return c;
	}
};

struct Match {
	std::shared_ptr<const host::SharedLevel> level;
	host::GameServer server;
	std::vector<host::OutDatagram> out;
	std::vector<std::unique_ptr<tdm::ScriptedPeer>> peers;

	std::vector<std::function<sim::Command(sim::Tick, const sim::SoldierState &)>> scripts;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	uint32_t next_debug = 1;

	const host::TdmRules &rules() const { return *server.rules(); }
	sim::Tick now() const { return server.current_tick(); }
	const sim::SoldierState *state(int p_slot) const { return p_slot >= 0 ? server.soldier(uint32_t(p_slot)) : nullptr; }

	void tick() {
		const sim::Tick next = server.current_tick() + 1u;
		for (size_t i = 0; i < peers.size(); ++i) {
			const sim::SoldierState *own = state(peers[i]->slot());
			if (!is_alive(own)) {
				continue;
			}
			peers[i]->command(scripts[i] ? scripts[i](next, *own) : sim::idle_command(next, own->yaw, own->pitch, own->target_pose));
		}
		out.clear();
		server.tick(out);
		for (auto &p : peers) {
			p->receive(out);
		}
	}
	void ticks(uint32_t p_count) {
		for (uint32_t i = 0; i < p_count; ++i) {
			tick();
		}
	}
	template <typename F>
	bool until(F p_done, uint32_t p_max) {
		for (uint32_t i = 0; i < p_max; ++i) {
			if (p_done()) {
				return true;
			}
			tick();
		}
		return p_done();
	}
	tdm::ScriptedPeer &join(const char *p_name) {
		peers.push_back(std::make_unique<tdm::ScriptedPeer>(server, uint32_t(peers.size() + 1u), p_name));
		scripts.emplace_back();
		tdm::ScriptedPeer &p = *peers.back();
		p.hello();
		tick();
		return p;
	}
	Script *bot() {
		auto s = std::make_unique<Script>();
		Script *raw = s.get();
		raw->slot = server.add_local_soldier(std::move(s), 1);
		REQUIRE(raw->slot >= 0);
		return raw;
	}
	void teleport(tdm::ScriptedPeer &r_peer, const sim::Vec3 &p_at, uint16_t p_yaw = 0) {
		net::DebugRequest q;
		q.kind = net::DebugKind::Teleport;
		q.id = next_debug++;
		q.position = p_at;
		q.yaw = p_yaw;
		r_peer.debug(q);
	}
	void spawn_at(tdm::ScriptedPeer &r_peer, uint32_t p_index) {
		net::DebugRequest q;
		q.kind = net::DebugKind::SpawnAt;
		q.id = next_debug++;
		q.index = p_index;
		r_peer.debug(q);
	}
	void place(int p_slot, const sim::Vec3 &p_at, uint16_t p_yaw = 0) {
		const core::Status st = server.place_soldier(uint32_t(p_slot), p_at, p_yaw, sim::Pose::Stand);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	}
	sim::Vec3 eye(const sim::SoldierState &p_state) const { return add(p_state.position, 0.0f, motor.eye_height(p_state), 0.0f); }
	const net::PlayerRow *row(int p_slot) const {
		for (const net::PlayerRow &r : rules().players()) {
			if (int(r.slot) == p_slot) {
				return &r;
			}
		}
		return nullptr;
	}
};

host::MatchRulesConfig tdm_rules_config(bool p_deploy) {
	host::MatchRulesConfig c;
	c.mode = host::MatchRulesConfig::Mode::Tdm;
	c.deploy_screen = p_deploy;
	c.revive = true;
	c.preround_override_s = 0.0f;
	return c;
}

std::unique_ptr<Match> start_match(const tdm::TdmLevel &p_level, bool p_deploy, uint64_t p_seed = 7,
		int p_kill_limit = 0, int p_min_players = -1,
		const std::function<void(host::GameServerConfig &)> &p_configure = {}) {
	auto m = std::make_unique<Match>();
	m->level = p_level.shared;
	host::GameServerConfig cfg;
	cfg.level = p_level.shared;
	cfg.max_slots = 16;
	cfg.dev_commands = true;
	cfg.seed = p_seed;
	cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
	cfg.gunplay.player_kit = 0;
	cfg.rules = tdm_rules_config(p_deploy);
	cfg.rules.kill_limit_override = p_kill_limit;
	cfg.rules.min_players_override = p_min_players;
	if (p_configure) {
		p_configure(cfg);
	}
	const core::Status st = m->server.init(cfg);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	REQUIRE(m->server.rules() != nullptr);
	core::Result<std::unique_ptr<sim::CollisionWorld>> w = sim::CollisionWorld::create(p_level.shared->static_world);
	REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
	m->world = std::move(*w);
	const core::Status mi = m->motor.init(*m->world, p_level.shared->motor);
	REQUIRE_MESSAGE(mi.ok(), (mi.ok() ? std::string() : mi.error().message));
	return m;
}

bool wait_in_round(Match &r_match) {
	return r_match.until([&] { return r_match.rules().phase() == net::MatchPhase::InRound; }, 240);
}

std::optional<sim::Vec3> floor_at(const sim::CollisionWorld &p_world, const sim::Vec3 &p_at) {
	sim::StaticHit hit;
	if (!p_world.ground_below(add(p_at, 0.0f, 1.0f, 0.0f), 3.0f, sim::query_mask::kMovement, &hit) || hit.normal[1] < 0.9f) {
		return std::nullopt;
	}
	return hit.position;
}

bool clear(const sim::CollisionWorld &p_world, const sim::Vec3 &p_from, const sim::Vec3 &p_to) {
	const sim::Vec3 d{ p_to[0] - p_from[0], p_to[1] - p_from[1], p_to[2] - p_from[2] };
	return !p_world.ray(p_from, d, sim::query_mask::kProjectile) && !p_world.ray(p_from, d, sim::query_mask::kMovement);
}

struct Patch {
	sim::Vec3 victim{}, shooter{}, medic{}, enemy_medic{};
	uint32_t spawn = 0;
};
std::optional<Patch> find_patch(const Match &p_match, float p_sky_m) {
	const sim::CollisionWorld &w = *p_match.world;
	for (const sim::SpawnPoint &sp : p_match.level->spawns) {
		const std::optional<sim::Vec3> v = floor_at(w, sp.position);
		if (!v) {
			continue;
		}
		const std::optional<sim::Vec3> e = floor_at(w, add(*v, 2.5f, 0.0f, 0.0f));
		const std::optional<sim::Vec3> a = floor_at(w, add(*v, 0.0f, 0.0f, 0.9f));
		const std::optional<sim::Vec3> x = floor_at(w, add(*v, 0.0f, 0.0f, -0.9f));
		if (!e || !a || !x) {
			continue;
		}
		bool ok = true;
		for (const sim::Vec3 &p : { *v, *e, *a, *x }) {
			ok = ok && std::fabs(p[1] - (*v)[1]) < 0.15f && p_match.motor.fits_standing(add(p, 0.0f, 0.05f, 0.0f));
		}
		ok = ok && clear(w, add(*e, 0.0f, 1.6f, 0.0f), add(*v, 0.0f, 1.2f, 0.0f)) && clear(w, add(*a, 0.0f, 1.0f, 0.0f), add(*v, 0.0f, 0.3f, 0.0f)) &&
				clear(w, add(*x, 0.0f, 1.0f, 0.0f), add(*v, 0.0f, 0.3f, 0.0f)) && clear(w, add(*v, 0.0f, 2.0f, 0.0f), add(*v, 0.0f, p_sky_m, 0.0f));
		if (ok) {
			return Patch{ *v, *e, *a, *x, sp.index };
		}
	}
	return std::nullopt;
}

float pick_drop(const Match &p_match, const sim::Vec3 &p_floor, bool p_lethal, float p_min_damage, float p_max_damage,
		float p_max_height) {
	for (float h = 6.0f; h <= p_max_height; h += 1.0f) {
		sim::SoldierState s = p_match.motor.spawn_state(add(p_floor, 0.0f, h, 0.0f), 0, 1, 0, false);
		for (int i = 0; i < 900; ++i) {
			p_match.motor.step(s, sim::idle_command(s.tick + 1u, s.yaw, s.pitch, s.target_pose));
			if ((s.events & sim::soldier_events::kLanded) != 0) {
				break;
			}
		}
		if (p_lethal ? s.health <= 0.0f : (s.damage > p_min_damage && s.damage < p_max_damage && s.health > 0.0f)) {
			return h;
		}
	}
	return 0.0f;
}

struct Drop {
	sim::Vec3 floor{}, beside{};
	float height = 0;
	uint32_t spawn = 0;
};
std::optional<Drop> find_drop(const Match &p_match, bool p_lethal, float p_min_damage = 10.0f,
		float p_max_damage = 60.0f, bool p_under_roof = false) {
	constexpr float kHeadroom = 2.5f;
	const sim::CollisionWorld &w = *p_match.world;
	for (const sim::SpawnPoint &sp : p_match.level->spawns) {
		sim::StaticHit hit;
		float max_height = 78.0f;
		if (p_under_roof) {
			const std::optional<sim::Vec3> f = floor_at(w, sp.position);
			if (!f) {
				continue;
			}
			hit.position = *f;
			sim::StaticHit roof;
			if (w.ray(add(*f, 0.0f, 0.5f, 0.0f), sim::Vec3{ 0.0f, max_height, 0.0f }, sim::query_mask::kMovement,
						&roof)) {
				max_height = roof.fraction * max_height + 0.5f - kHeadroom;
			}
		} else if (!w.ground_below(add(sp.position, 0.0f, 80.0f, 0.0f), 160.0f, sim::query_mask::kMovement, &hit) ||
				hit.normal[1] < 0.9f) {
			continue;
		}
		const sim::Vec3 floor = hit.position;
		const std::optional<sim::Vec3> beside = floor_at(w, add(floor, 2.0f, 0.0f, 0.0f));
		if (!beside || std::fabs((*beside)[1] - floor[1]) > 0.15f || !p_match.motor.fits_standing(add(floor, 0.0f, 0.05f, 0.0f)) ||
				!p_match.motor.fits_standing(add(*beside, 0.0f, 0.05f, 0.0f))) {
			continue;
		}
		const float h = pick_drop(p_match, floor, p_lethal, p_min_damage, p_max_damage, max_height);
		if (h > 0.0f) {
			return Drop{ floor, *beside, h, sp.index };
		}
	}
	return std::nullopt;
}

uint32_t select_and_fire(uint8_t p_slot, bool p_fire) {
	return tg::select_slot_field(p_slot) | (p_fire ? sw::weapon_buttons::kFire : 0u);
}

sim::Command aim_command(const Match &p_match, sim::Tick p_tick, const sim::SoldierState &p_state, const sim::Vec3 &p_target, sim::Pose p_pose,
		uint32_t p_buttons) {
	float yaw = 0, pitch = 0;
	tg::aim_at(p_match.eye(p_state), p_target, yaw, pitch);
	sim::Command c = sim::idle_command(p_tick, sim::quantize_yaw(yaw), sim::quantize_pitch(pitch), p_pose);
	c.buttons = p_buttons;
	return c;
}

const net::DeathEvent *last_death(const tdm::ScriptedPeer &p_peer, int p_victim) {
	for (auto c = p_peer.combat.rbegin(); c != p_peer.combat.rend(); ++c) {
		for (auto d = c->deaths.rbegin(); d != c->deaths.rend(); ++d) {
			if (int(d->victim) == p_victim) {
				return &*d;
			}
		}
	}
	return nullptr;
}

const net::ReviveEvent *last_revive(const tdm::ScriptedPeer &p_peer, int p_revived) {
	for (auto c = p_peer.combat.rbegin(); c != p_peer.combat.rend(); ++c) {
		for (auto r = c->revives.rbegin(); r != c->revives.rend(); ++r) {
			if (int(r->revived) == p_revived) {
				return &*r;
			}
		}
	}
	return nullptr;
}

size_t player_rows_differ(const tdm::ScriptedPeer &p_peer, const host::TdmRules &p_rules) {
	const std::span<const net::PlayerRow> want = p_rules.players();
	size_t differ = want.size() > p_peer.players.size() ? want.size() - p_peer.players.size() : p_peer.players.size() - want.size();
	for (const net::PlayerRow &w : want) {
		auto at = std::find_if(p_peer.players.begin(), p_peer.players.end(), [&](const net::PlayerRow &r) { return r.slot == w.slot; });
		if (at == p_peer.players.end()) {
			continue;
		}
		const bool same = at->team == w.team && at->kit == w.kit && at->bot == w.bot && at->alive == w.alive && at->kills == w.kills &&
				at->deaths == w.deaths && at->score == w.score && net::player_name(*at) == net::player_name(w);
		differ += same ? 0u : 1u;
	}
	return differ;
}

std::function<void(host::GameServerConfig &)> squad_server(const tdm::TdmLevel &p_level) {
	return [&p_level](host::GameServerConfig &r_cfg) {
		r_cfg.max_slots = 40;
		r_cfg.dev_commands = false;
		r_cfg.rules.max_players = 32;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = p_level.bots;
		fill->count = 0;
		r_cfg.bots = fill;
	};
}

void squad_request(
		Match &r_match, const tdm::ScriptedPeer &p_peer, net::SquadOp p_op, uint8_t p_squad = 0, uint8_t p_target = 0) {
	net::SquadRequest q;
	q.op = p_op;
	q.squad = p_squad;
	q.target = p_target;
	r_match.server.on_receive(p_peer.peer(), net::kChannelReliable, net::encode_squad_request(q));
	r_match.ticks(net::kPlayerListIntervalTicks + 2u);
}

const net::PlayerRow *seen_row(const tdm::ScriptedPeer &p_peer, int p_slot) {
	for (const net::PlayerRow &r : p_peer.players) {
		if (int(r.slot) == p_slot) {
			return &r;
		}
	}
	return nullptr;
}

struct SquadCensus {
	std::array<std::array<uint32_t, net::kMaxSquads + 1>, 3> members{};
	std::array<std::array<uint32_t, net::kMaxSquads + 1>, 3> leaders{};
	std::array<std::array<int, net::kMaxSquads + 1>, 3> leader_slot{};
	std::array<uint32_t, 3> team{};
};

SquadCensus squad_census(std::span<const net::PlayerRow> p_rows) {
	SquadCensus c;
	for (auto &t : c.leader_slot) {
		t.fill(-1);
	}
	for (const net::PlayerRow &r : p_rows) {
		if (r.team > 2 || r.squad > net::kMaxSquads) {
			continue;
		}
		++c.team[r.team];
		++c.members[r.team][r.squad];
		if (r.leader) {
			++c.leaders[r.team][r.squad];
			c.leader_slot[r.team][r.squad] = r.slot;
		}
	}
	return c;
}
int squad_index(const tdm::ScriptedPeer &p_peer, int p_of, int p_mate) {
	const net::PlayerRow *own = seen_row(p_peer, p_of);
	if (own == nullptr || own->squad == 0) {
		return -1;
	}
	std::vector<int> slots;
	for (const net::PlayerRow &r : p_peer.players) {
		if (r.team == own->team && r.squad == own->squad) {
			slots.push_back(r.slot);
		}
	}
	std::sort(slots.begin(), slots.end());
	const auto at = std::find(slots.begin(), slots.end(), p_mate);
	return at != slots.end() ? int(at - slots.begin()) : -1;
}
}

TEST_SUITE("host-tdm-server") {

	TEST_CASE("tuning: the shipped tdm_tuning.cfg, hostile text, 3,000 mutations") {
		const std::string text = tg::read_text(OPENBF3_TDM_TUNING_CFG);
		REQUIRE(!text.empty());
		core::Result<host::TdmTuning> shipped = host::parse_tdm_tuning(text);
		REQUIRE_MESSAGE(shipped.ok(), (shipped.ok() ? std::string() : shipped.error().message));
		const host::TdmTuning defaults;
		CHECK(shipped->revive_wait_s == defaults.revive_wait_s);
		CHECK(shipped->defib_range_m == defaults.defib_range_m);
		CHECK(shipped->bot_kit_weights == defaults.bot_kit_weights);
		CHECK(shipped->immunity_abort_map.size() == 3u);
		CHECK_FALSE(shipped->measured);
		const char *hostile[] = {
			"revive_wait_s",
			"nonsense = 1",
			"revive_wait_s = nan",
			"revive_wait_s = inf",
			"revive_wait_s = -1",
			"revive_wait_s = 1e30",
			"score_kill = 99999999999999999999",
			"bot_kit_weights = 1 2 3",
			"bot_kit_weights = 0 0 0 0",
			"bot_kit_weights = 1 2 3 4 5",
			"bot_kit_weights = 1 x 3 4",
			"immunity_abort_map = 7",
			"immunity_abort_map = 7:teleport",
			"immunity_abort_map = x:fire",
			"revive_takes_kill = maybe",
			"spawn_los_rays = 65",
		};
		for (const char *h : hostile) {
			CAPTURE(h);
			core::Result<host::TdmTuning> r = host::parse_tdm_tuning(h);
			CHECK_FALSE(r.ok());
			if (!r.ok()) {
				CHECK(r.error().message.find("line 1") != std::string::npos);
			}
		}
		std::string many = "immunity_abort_map =";
		for (size_t i = 0; i <= host::kMaxImmunityAbortEntries; ++i) {
			many += " " + std::to_string(i) + ":fire";
		}
		CHECK_FALSE(host::parse_tdm_tuning(many).ok());
		CHECK_FALSE(host::parse_tdm_tuning(std::string(host::kMaxTdmTuningBytes + 1, '#')).ok());
		CHECK_FALSE(host::parse_tdm_tuning(std::string(host::kMaxTdmTuningLines, '\n')).ok());
		CHECK(host::parse_tdm_tuning(std::string(host::kMaxTdmTuningLines - 1, '\n')).ok());

		uint64_t rng = 20261019u;
		size_t ok = 0;
		for (int i = 0; i < 3000; ++i) {
			std::string m = text;
			for (int k = 0; k < 4; ++k) {
				rng = sw::splitmix64(rng);
				const size_t at = size_t(rng % m.size());
				switch ((rng >> 32) % 3u) {
					case 0:
						m[at] = char(rng >> 40);
						break;
					case 1:
						m.erase(at, 1 + size_t((rng >> 48) % 8u));
						break;
					default:
						m.insert(at, 1, "=:# -.0123456789en\n"[(rng >> 40) % 19u]);
						break;
				}
				if (m.empty()) {
					m = "#";
				}
			}
			ok += host::parse_tdm_tuning(m).ok() ? 1u : 0u;
		}
		std::printf("tdm tuning: %zu of 3000 mutations still parse\n", ok);
	}

	TEST_CASE("health: regen after damage, a new hit restarts the delay; spawn immunity and its abort inputs") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const core::gameplay::SoldierDef &soldier = level->shared->gunplay->soldier;
		const sim::MotorParams &mp = level->shared->motor;
		std::printf("E3: SoldierDef regeneration_delay %.3f s, regeneration_rate %.3f hp/s, post_revive_health %.1f, immortal_time_after_spawn %.2f s; "
					"abort buttons 0x%x, abort move %d\n",
				soldier.regeneration_delay, soldier.regeneration_rate, soldier.post_revive_health, soldier.immortal_time_after_spawn,
				unsigned(mp.protect_abort_buttons), int(mp.protect_abort_move));
		const uint32_t delay = soldier.regeneration_delay > 0 ? uint32_t(std::lround(soldier.regeneration_delay * float(sim::kSimHz))) : 600u;
		const float per_tick = (soldier.regeneration_rate > 0 ? soldier.regeneration_rate : 5.0f) / float(sim::kSimHz);
		CHECK(delay == 600u);
		CHECK(per_tick == doctest::Approx(1.0f / 12.0f));
		CHECK((mp.protect_abort_buttons & sim::buttons::kFire) != 0);
		CHECK(mp.protect_abort_move);

		std::unique_ptr<Match> m = start_match(*level, false);
		for (int i = 0; i < 3; ++i) {
			m->bot();
		}
		tdm::ScriptedPeer &p = m->join("health");
		REQUIRE(wait_in_round(*m));
		p.ready();
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 10));
		const int slot = p.slot();

		const uint32_t immortal = uint32_t(std::lround(soldier.immortal_time_after_spawn * float(sim::kSimHz)));
		CHECK(immortal == 120u);
		for (int run = 0; run < 3; ++run) {
			if (run > 0) {
				const uint32_t life = m->state(slot)->life;
				m->spawn_at(p, m->level->spawns.size() > 3 ? uint32_t(3 + run) : 0u);
				REQUIRE(m->until([&] { return m->state(slot)->life != life; }, 5));
			} else {
				REQUIRE(m->state(slot)->life >= 1u);
			}
			const sim::SoldierState spawned = *m->state(slot);
			const sim::Tick t0 = spawned.tick;
			CHECK(spawned.protect_ticks > 0);
			const sim::Tick abort_at = run == 0 ? 0 : t0 + 30u;
			const auto script = [&, run, abort_at](sim::Tick p_tick, const sim::SoldierState &p_state) {
				sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
				if (run > 0 && p_tick == abort_at) {
					if (run == 1) {
						c.buttons = sw::weapon_buttons::kFire;
					} else {
						c.move_forward = 127;
					}
				}
				return c;
			};
			m->scripts[0] = script;
			sim::SoldierState local = spawned;
			sim::Tick server_end = 0, local_end = 0;
			uint32_t mismatches = 0;
			for (uint32_t i = 0; i < immortal + 10u; ++i) {
				m->tick();
				const sim::SoldierState &s = *m->state(slot);
				const sim::Command c = script(local.tick + 1u, local);
				m->motor.step(local, c);
				mismatches += s.protect_ticks != local.protect_ticks ? 1u : 0u;
				if (server_end == 0 && s.protect_ticks == 0) {
					server_end = s.tick;
				}
				if (local_end == 0 && local.protect_ticks == 0) {
					local_end = local.tick;
				}
			}
			m->scripts[0] = nullptr;
			const char *what = run == 0 ? "idle" : (run == 1 ? "Fire at +30" : "move at +30");
			std::printf("E3 immunity (%s): spawn tick %u, protect %u, ends at +%u on the server, +%u in the local motor, %u tick mismatches\n", what,
					unsigned(t0), unsigned(spawned.protect_ticks), unsigned(server_end - t0), unsigned(local_end - t0), unsigned(mismatches));
			CHECK(mismatches == 0u);
			CHECK(server_end == local_end);
			if (run == 0) {
				CHECK(server_end - t0 == immortal);
			} else {
				CHECK(server_end == abort_at);
			}
		}

		const std::optional<Drop> place = find_drop(*m, false);
		REQUIRE(place.has_value());
		const float h = place->height;
		std::printf("E3 regen: drop %.0f m onto the top surface over spawn point %u (%.1f %.1f %.1f)\n", h, unsigned(place->spawn), place->floor[0],
				place->floor[1], place->floor[2]);
		const auto drop = [&]() -> sim::Tick {
			m->teleport(p, add(place->floor, 0.0f, h, 0.0f));
			sim::Tick hit = 0;
			m->until(
					[&] {
						const sim::SoldierState *s = m->state(slot);
						if (s != nullptr && s->damage > 0.0f) {
							hit = s->tick;
						}
						return hit != 0;
					},
					600);
			return hit;
		};
		const sim::Tick t1 = drop();
		REQUIRE(t1 != 0);
		REQUIRE(is_alive(m->state(slot)));
		const float h1 = m->state(slot)->health;
		CHECK(h1 < 100.0f);

		sim::Tick first_regen = 0;
		float before = h1;
		uint32_t early = 0;
		std::vector<float> steps;
		while (m->now() < t1 + delay + 60u) {
			m->tick();
			const float hp = m->state(slot)->health;
			if (hp > before + 1e-6f) {
				if (first_regen == 0) {
					first_regen = m->now();
				}
				steps.push_back(hp - before);
			}
			if (m->now() < t1 + delay - 1u && hp != h1) {
				++early;
			}
			before = hp;
		}
		std::printf("E3 regen: damage at tick %u (%.2f hp left), first regen at +%u, %zu regen steps (first %.5f hp)\n", unsigned(t1), h1,
				unsigned(first_regen - t1), steps.size(), steps.empty() ? 0.0 : double(steps.front()));
		CHECK(early == 0u);
		CHECK(first_regen >= t1 + delay - 1u);
		CHECK(first_regen <= t1 + delay + 1u);
		REQUIRE(!steps.empty());
		for (const float s : steps) {
			CHECK(s == doctest::Approx(per_tick).epsilon(0.001));
		}

		const sim::Tick t2 = drop();
		REQUIRE(t2 != 0);
		REQUIRE(is_alive(m->state(slot)));
		const float h2 = m->state(slot)->health;
		sim::Tick second_regen = 0;
		before = h2;
		while (m->now() < t2 + delay + 10u) {
			m->tick();
			const float hp = m->state(slot)->health;
			if (second_regen == 0 && hp > before + 1e-6f) {
				second_regen = m->now();
			}
			before = hp;
		}
		std::printf("E3 regen: second drop at tick %u (%.2f hp left), regen again at +%u\n", unsigned(t2), h2, unsigned(second_regen - t2));
		CHECK(second_regen >= t2 + delay - 1u);
		CHECK(second_regen <= t2 + delay + 1u);
		CHECK(m->rules().stats().regen_ticks > 0u);
	}

	TEST_CASE("respawn: bot respawn ticks and kits, R10, the M2a hashes") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, false);
		Script *b0 = m->bot();
		Script *b1 = m->bot();
		Script *b2 = m->bot();
		tdm::ScriptedPeer &p = m->join("medic");
		p.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 10));
		CHECK(m->rules().team_of(uint8_t(b0->slot)) == 1);
		CHECK(m->rules().team_of(uint8_t(b1->slot)) == 2);
		CHECK(m->rules().team_of(uint8_t(b2->slot)) == 1);
		CHECK(m->rules().team_of(uint8_t(p.slot())) == 2);
		for (const Script *b : { b0, b1, b2 }) {
			CHECK(m->server.gunplay()->kit(uint8_t(b->slot)) == m->rules().kit_of(uint8_t(b->slot)));
		}
		const std::optional<Drop> patch = find_drop(*m, true);
		REQUIRE(patch.has_value());
		std::printf("E4: a lethal drop of %.0f m onto the top surface over spawn point %u\n", patch->height, unsigned(patch->spawn));

		m->teleport(p, m->level->spawns[(patch->spawn + 50u) % m->level->spawns.size()].position);
		m->ticks(30);
		const auto kill = [&](int p_slot) -> std::pair<sim::Tick, sim::Tick> {
			const uint32_t life = m->state(p_slot)->life;
			m->place(p_slot, add(patch->floor, 0.0f, patch->height, 0.0f));
			sim::Tick died = 0, back = 0;
			m->until(
					[&] {
						const sim::SoldierState *s = m->state(p_slot);
						if (died == 0 && !is_alive(s)) {
							died = m->now();
						}
						if (died != 0 && is_alive(s) && s->life != life) {
							back = m->now();
						}
						return back != 0;
					},
					1200);
			return { died, back };
		};
		const float medic_distance = distance(m->state(p.slot())->position, patch->floor);
		const auto [d1, r1] = kill(b1->slot);
		const uint32_t e1 = m->rules().earliest_respawn(uint8_t(b1->slot));
		std::printf("E4 bot slot %d (medic %.0f m away): died at %u, earliest_respawn %u (+%u), respawned at %u (+%u), kit %u (rules %u)\n", b1->slot,
				medic_distance, unsigned(d1), unsigned(e1), unsigned(e1 - d1), unsigned(r1), unsigned(r1 - d1),
				unsigned(m->server.gunplay()->kit(uint8_t(b1->slot))), unsigned(m->rules().kit_of(uint8_t(b1->slot))));
		REQUIRE(d1 != 0);
		REQUIRE(r1 != 0);
		CHECK(medic_distance > 20.0f);
		CHECK(e1 == d1 + 300u);
		CHECK(r1 == e1);
		CHECK(m->server.gunplay()->kit(uint8_t(b1->slot)) == m->rules().kit_of(uint8_t(b1->slot)));

		m->teleport(p, add(patch->beside, 0.0f, 0.05f, 0.0f));
		m->ticks(30);
		const auto [d2, r2] = kill(b1->slot);
		const uint32_t e2 = m->rules().earliest_respawn(uint8_t(b1->slot));
		std::printf("E4 bot slot %d next to the medic (%.1f m): died at %u, earliest_respawn +%u, respawned at +%u\n", b1->slot,
				distance(m->state(p.slot())->position, patch->floor), unsigned(d2), unsigned(e2 - d2), unsigned(r2 - d2));
		CHECK(e2 == d2 + 480u);
		CHECK(r2 == e2);

		const auto [d3, r3] = kill(b0->slot);
		std::printf("E4 bot slot %d (enemy of the medic) next to it: respawned at +%u\n", b0->slot, unsigned(r3 - d3));
		CHECK(r3 == d3 + 300u);
		CHECK(m->server.gunplay()->kit(uint8_t(b0->slot)) == m->rules().kit_of(uint8_t(b0->slot)));
		CHECK(m->rules().stats().runtime_faults == 0u);

		std::unique_ptr<Match> d = start_match(*level, true);
		for (int i = 0; i < 3; ++i) {
			d->bot();
		}
		tdm::ScriptedPeer &q = d->join("deploy");
		q.ready();
		REQUIRE(wait_in_round(*d));
		d->ticks(120);
		CHECK(q.slot() >= 0);
		CHECK_FALSE(is_alive(d->state(q.slot())));
		CHECK(q.spawned.empty());
		REQUIRE(!net::encode_spawn_request(net::SpawnRequest{ 2, 0 }).empty());

		const auto request_and_wait = [&](uint8_t p_kit) -> std::pair<sim::Tick, sim::Tick> {
			const size_t before = q.spawned.size();
			const sim::Tick request_tick = d->now() + 1u;
			q.spawn_request(p_kit);
			sim::Tick arrived = 0;
			for (int i = 0; i < 5 && arrived == 0; ++i) {
				d->tick();
				arrived = q.spawned.size() > before ? d->now() : 0;
			}
			return { request_tick, arrived };
		};
		const auto [rq1, at1] = request_and_wait(2);
		std::printf("E4 deploy: SpawnRequest kit 2 for tick %u, Spawned in tick %u's output (+%u)\n", unsigned(rq1), unsigned(at1), unsigned(at1 - rq1));
		REQUIRE(at1 != 0);
		CHECK(at1 <= rq1 + 1u);
		CHECK(q.spawned.back().kit == 2);
		CHECK_FALSE(q.spawned.back().revived);
		CHECK(is_alive(d->state(q.slot())));
		CHECK(d->server.gunplay()->kit(uint8_t(q.slot())) == 2);

		const std::optional<Drop> lethal = find_drop(*d, true);
		REQUIRE(lethal.has_value());
		d->ticks(130);
		const uint32_t qlife = d->state(q.slot())->life;
		d->teleport(q, add(lethal->floor, 0.0f, lethal->height, 0.0f));
		sim::Tick qdied = 0;
		REQUIRE(d->until(
				[&] {
					if (!is_alive(d->state(q.slot()))) {
						qdied = d->now();
					}
					return qdied != 0;
				},
				900));
		d->tick();
		const uint32_t q_earliest = d->rules().earliest_respawn(uint8_t(q.slot()));
		const net::DeathEvent *qd = last_death(q, q.slot());
		REQUIRE(qd != nullptr);
		std::printf("E4 deploy: the peer died at %u; DeathEvent at the peer: respawn_tick %u (+%u), revive_until %u (+%u), killer_health %.2f; rules "
					"earliest_respawn %u\n",
				unsigned(qdied), unsigned(qd->respawn_tick), unsigned(qd->respawn_tick - qdied), unsigned(qd->revive_until),
				unsigned(qd->revive_until - qdied), qd->killer_health, unsigned(q_earliest));
		CHECK(qd->respawn_tick == qdied + 300u);
		CHECK(qd->respawn_tick == q_earliest);
		CHECK(qd->revive_until == d->rules().revive_until(uint8_t(q.slot())));
		CHECK(qd->killer_health == 0.0f);
		CHECK_FALSE(is_alive(d->state(q.slot())));
		while (d->now() + 1u < q_earliest - 1u) {
			d->tick();
		}
		const uint64_t early0 = d->rules().stats().refused[size_t(host::SpawnRefusal::TooEarly)];
		const size_t spawned_before = q.spawned.size();
		q.spawn_request(3);
		d->tick();
		CHECK(d->rules().stats().refused[size_t(host::SpawnRefusal::TooEarly)] == early0 + 1u);
		CHECK(q.spawned.size() == spawned_before);
		CHECK_FALSE(is_alive(d->state(q.slot())));
		const auto [rq2, at2] = request_and_wait(3);
		std::printf("E4 deploy: request for %u refused (too early), for %u accepted, Spawned in tick %u's output, kit %u, life %u -> %u\n",
				unsigned(rq2 - 1u), unsigned(rq2), unsigned(at2), unsigned(q.spawned.empty() ? 0 : q.spawned.back().kit), unsigned(qlife),
				unsigned(d->state(q.slot())->life));
		CHECK(rq2 == q_earliest);
		REQUIRE(at2 != 0);
		CHECK(at2 <= rq2 + 1u);
		CHECK(q.spawned.back().kit == 3);
		CHECK(d->server.gunplay()->kit(uint8_t(q.slot())) == 3);
		CHECK(d->state(q.slot())->life == qlife + 1u);

		d->ticks(net::kPlayerListIntervalTicks + 2u);
		REQUIRE(q.match_state.has_value());
		CHECK(net::encode_match_state(*q.match_state) == net::encode_match_state(d->rules().match_state()));
		const size_t differ = player_rows_differ(q, d->rules());
		std::printf("E4 deploy: %zu MatchStates and %zu player rows at the peer, %zu rows differ from the rules', %llu decode errors\n", q.match_states.size(),
				q.players.size(), differ, (unsigned long long)q.decode_errors);
		CHECK(differ == 0u);
		CHECK(q.players.size() == 4u);
		CHECK(q.decode_errors == 0u);

		d->peers.push_back(std::make_unique<tdm::ScriptedPeer>(d->server, 99u, "v2"));
		d->scripts.emplace_back();
		tdm::ScriptedPeer &v2 = *d->peers.back();
		net::Hello old;
		old.protocol = 2;
		old.name = "v2";
		d->server.on_receive(v2.peer(), net::kChannelReliable, net::encode_hello(old));
		d->tick();
		std::printf("E4/E7: a v2 Hello: %s\n", v2.kick ? ("Kick \"" + v2.kick->reason + "\"").c_str() : "no Kick");
		CHECK(v2.kick.has_value());
		CHECK_FALSE(v2.match_info.has_value());
		CHECK(d->rules().stats().runtime_faults == 0u);

		const auto start = std::chrono::steady_clock::now();
		core::Result<std::shared_ptr<const host::GunplayLevel>> m2a = host::load_gunplay_level(level->install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE_MESSAGE(m2a.ok(), (m2a.ok() ? std::string() : m2a.error().message));
		core::Result<sw::Mechanics> mech = host::load_mechanics_cfg(OPENBF3_MECHANICS_CFG);
		REQUIRE(mech.ok());
		core::Result<std::shared_ptr<const sw::WeaponTable>> t_m2a = host::make_match_table(**m2a, *mech, false, false);
		core::Result<std::shared_ptr<const sw::WeaponTable>> t_tdm = host::make_match_table(*level->shared->gunplay, *mech, false, false);
		REQUIRE(t_m2a.ok());
		REQUIRE(t_tdm.ok());
		sim::MotorParams plain = level->shared->motor;
		plain.protect_abort_buttons = 0;
		plain.protect_abort_move = false;
		const uint64_t kit_m2a = host::kit_table_hash((*m2a)->kits), kit_tdm = host::kit_table_hash(level->shared->gunplay->kits);
		const uint64_t weapon_m2a = host::match_weapon_hash(**t_m2a, (*m2a)->capsule_table);
		const uint64_t weapon_tdm = host::match_weapon_hash(**t_tdm, level->shared->gunplay->capsule_table);
		const uint64_t motor_m2a = sim::motor_params_hash(plain), motor_tdm = sim::motor_params_hash(level->shared->motor);
		std::printf("E4 hashes (M2a load %.2f s): kit_table_hash M2a %016" PRIx64 " TDM %016" PRIx64 "; match_weapon_hash M2a %016" PRIx64 " TDM %016" PRIx64
					"; motor_params_hash M2a %016" PRIx64 " TDM %016" PRIx64 "\n",
				tdm::detail::seconds_since(start), kit_m2a, kit_tdm, weapon_m2a, weapon_tdm, motor_m2a, motor_tdm);
		CHECK((*m2a)->kits.defib_def == sw::kNoWeapon);
		CHECK(level->shared->gunplay->kits.defib_def != sw::kNoWeapon);
		CHECK(kit_m2a != kit_tdm);
		CHECK(weapon_m2a != weapon_tdm);
		CHECK(motor_m2a != motor_tdm);

		CHECK(kit_m2a == kM2aKitHash);
		CHECK(weapon_m2a == kM2aWeaponHash);
		CHECK(motor_m2a == kM2aMotorHash);
	}

	TEST_CASE("revive: a bot and a peer come back at the corpse; refusals; no stale respawn") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const float revive_health = level->shared->gunplay->soldier.post_revive_health > 0 ? level->shared->gunplay->soldier.post_revive_health : 20.0f;
		CHECK(revive_health == doctest::Approx(20.0f));
		const uint16_t defib = level->shared->gunplay->kits.defib_def;
		REQUIRE(defib != sw::kNoWeapon);
		REQUIRE(level->shared->scoring != nullptr);
		CHECK(level->shared->scoring->score("ID_SCORE_REVIVE_SQUAD") == std::optional<double>(110.0));
		CHECK(level->shared->tdm_tuning->score_revive_squad == 110);
		CHECK(level->shared->tdm_tuning->score_squad_wipe == 10);
		CHECK(level->shared->tdm_tuning->score_spawn_squad == 10);
		for (int victim_peer = 0; victim_peer < 2; ++victim_peer) {
			CAPTURE(victim_peer);
			std::unique_ptr<Match> m = start_match(*level, false, 11u + uint64_t(victim_peer));

			tdm::ScriptedPeer &medic = m->join("medic");
			medic.ready();
			m->tick();
			Script *shooter = m->bot();
			Script *victim_bot = nullptr;
			tdm::ScriptedPeer *victim_p = nullptr;
			if (victim_peer != 0) {
				victim_p = &m->join("victim");
				victim_p->ready();
				m->tick();
			} else {
				victim_bot = m->bot();
			}
			tdm::ScriptedPeer &enemy = m->join("enemy medic");
			enemy.ready();
			REQUIRE(wait_in_round(*m));
			m->ticks(2);
			const int victim = victim_bot != nullptr ? victim_bot->slot : victim_p->slot();
			REQUIRE(m->rules().team_of(uint8_t(medic.slot())) == 1);
			REQUIRE(m->rules().team_of(uint8_t(victim)) == 1);
			REQUIRE(m->rules().team_of(uint8_t(shooter->slot)) == 2);
			REQUIRE(m->rules().team_of(uint8_t(enemy.slot())) == 2);
			REQUIRE(m->server.gunplay()->kit(uint8_t(medic.slot())) == 0);
			REQUIRE(m->server.gunplay()->kit(uint8_t(enemy.slot())) == 0);
			std::optional<Patch> patch = find_patch(*m, 3.0f);
			REQUIRE(patch.has_value());

			if (victim_bot != nullptr) {
				m->place(victim, add(patch->victim, 0.0f, 0.05f, 0.0f));
			} else {
				m->teleport(*victim_p, add(patch->victim, 0.0f, 0.05f, 0.0f));
			}
			m->place(shooter->slot, add(patch->shooter, 0.0f, 0.05f, 0.0f));
			m->teleport(medic, add(patch->medic, 0.0f, 0.05f, 0.0f));
			m->teleport(enemy, add(patch->enemy_medic, 0.0f, 0.05f, 0.0f));
			m->ticks(40);
			const sim::Vec3 corpse_centre = add(patch->victim, 0.0f, 0.3f, 0.0f);

			bool medic_fire = false, enemy_fire = false, shooter_fire = false;
			const auto medic_script = [&](bool *p_fire) {
				return [&m, p_fire, corpse_centre](sim::Tick p_tick, const sim::SoldierState &p_state) {
					const bool press = *p_fire && (p_tick % 60u) < 50u;
					return aim_command(*m, p_tick, p_state, corpse_centre, sim::Pose::Crouch, select_and_fire(host::kKitGadgetSlot, press));
				};
			};
			m->scripts[0] = medic_script(&medic_fire);
			m->scripts[victim_p != nullptr ? 2 : 1] = medic_script(&enemy_fire);
			shooter->fn = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
				const sim::SoldierState *v = m->state(victim);
				const sim::Vec3 chest = v != nullptr ? add(v->position, 0.0f, 1.2f, 0.0f) : patch->victim;
				return aim_command(*m, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, shooter_fire && (p_tick % 6u) < 3u));
			};
			m->ticks(90);
			CHECK(m->server.gunplay()->weapons(uint8_t(medic.slot())).active == host::kKitGadgetSlot);

			const uint64_t refused0 = m->rules().stats().defib_refused;
			const uint64_t shots0 = m->rules().stats().defib_shots;
			medic_fire = true;
			m->ticks(130);
			medic_fire = false;
			const host::RulesStats s1 = m->rules().stats();
			std::printf("E5 (%s victim): living target: %" PRIu64 " defib shots, %" PRIu64 " refused, %" PRIu64 " misses\n", victim_peer ? "peer" : "bot",
					s1.defib_shots - shots0, s1.defib_refused - refused0, s1.defib_misses);
			CHECK(s1.defib_shots > shots0);
			CHECK(s1.defib_refused > refused0);
			CHECK(s1.revives == 0u);
			CHECK(is_alive(m->state(victim)));
			const uint32_t life = m->state(victim)->life;
			const uint8_t kit_before = m->server.gunplay()->kit(uint8_t(victim));
			const uint8_t team_before = m->server.gunplay()->team(uint8_t(victim));

			const int32_t team2_before = m->rules().match_state().kills[1];
			shooter_fire = true;
			sim::Tick died = 0;
			REQUIRE(m->until(
					[&] {
						if (!is_alive(m->state(victim))) {
							died = m->now();
						}
						return died != 0;
					},
					900));
			shooter_fire = false;
			const sim::Vec3 corpse = m->state(victim)->position;
			m->place(shooter->slot, m->level->spawns[(patch->spawn + 50u) % m->level->spawns.size()].position);
			m->tick();
			const int32_t team2_kill = m->rules().match_state().kills[1];
			const uint32_t respawn_at = m->rules().earliest_respawn(uint8_t(victim));
			const uint32_t revive_until = m->rules().revive_until(uint8_t(victim));
			std::printf("E5 (%s victim): died at %u at (%.2f %.2f %.2f), team 2 kills %d -> %d, respawn +%u, revive until +%u, downed %zu\n",
					victim_peer ? "peer" : "bot", unsigned(died), corpse[0], corpse[1], corpse[2], team2_before, team2_kill, unsigned(respawn_at - died),
					unsigned(revive_until - died), m->rules().downed().size());
			CHECK(team2_kill == team2_before + 1);
			CHECK(m->rules().downed().size() == 1u);
			CHECK(respawn_at == died + (victim_peer ? 300u : 480u));
			const uint8_t kit_while_down = m->rules().kit_of(uint8_t(victim));

			const uint64_t refused1 = m->rules().stats().defib_refused;
			enemy_fire = true;
			m->ticks(70);
			enemy_fire = false;
			CHECK(m->rules().stats().defib_refused > refused1);
			CHECK(m->rules().stats().revives == 0u);
			CHECK_FALSE(is_alive(m->state(victim)));

			const int32_t medic_score = m->row(medic.slot()) != nullptr ? m->row(medic.slot())->score : 0;
			const size_t spawned_before = victim_p != nullptr ? victim_p->spawned.size() : 0;
			medic_fire = true;
			sim::Tick revived = 0;
			m->until(
					[&] {
						if (is_alive(m->state(victim))) {
							revived = m->now();
						}
						return revived != 0;
					},
					std::max<uint32_t>(1u, respawn_at > m->now() + 5u ? respawn_at - m->now() - 5u : 1u));
			medic_fire = false;
			REQUIRE(revived != 0);
			const sim::SoldierState back = *m->state(victim);
			m->ticks(2);
			const host::RulesStats s2 = m->rules().stats();
			std::printf("E5 (%s victim): revived at +%u, life %u -> %u, %.2f hp, %.3f m from the corpse, revives %" PRIu64 ", medic score %d -> %d, team 2 "
						"kills %d\n",
					victim_peer ? "peer" : "bot", unsigned(revived - died), unsigned(life), unsigned(back.life), back.health, distance(back.position, corpse),
					s2.revives, medic_score, m->row(medic.slot()) != nullptr ? m->row(medic.slot())->score : -1, m->rules().match_state().kills[1]);
			CHECK(s2.revives == 1u);
			CHECK(back.life == life + 1u);
			CHECK(back.health == doctest::Approx(revive_health));
			CHECK(back.protect_ticks == 0u);
			CHECK(distance(back.position, corpse) < 0.25f);
			CHECK(revived < respawn_at);
			CHECK(m->rules().match_state().kills[1] == team2_kill - 1);
			REQUIRE(m->row(medic.slot()) != nullptr);
			REQUIRE(m->row(victim) != nullptr);
			CHECK(m->row(medic.slot())->squad != 0);
			CHECK(m->row(medic.slot())->squad == m->row(victim)->squad);
			CHECK(m->row(medic.slot())->score == medic_score + 110);
			CHECK(m->rules().downed().empty());

			std::printf("E5 (%s victim): kit %u -> %u (next-life draw while down %u), team %u -> %u\n", victim_peer ? "peer" : "bot",
					unsigned(kit_before), unsigned(m->server.gunplay()->kit(uint8_t(victim))), unsigned(kit_while_down), unsigned(team_before),
					unsigned(m->server.gunplay()->team(uint8_t(victim))));
			CHECK(m->server.gunplay()->kit(uint8_t(victim)) == kit_before);
			CHECK(m->server.gunplay()->team(uint8_t(victim)) == team_before);
			if (victim_bot != nullptr) {
				CHECK(m->rules().kit_of(uint8_t(victim)) == kit_before);
			}
			if (victim_p != nullptr) {
				REQUIRE(victim_p->spawned.size() == spawned_before + 1u);
				const net::Spawned &sp = victim_p->spawned.back();
				CHECK(sp.state.life == back.life);
				CHECK(sp.spawn_index == 0u);
				CHECK(distance(sp.state.position, corpse) < 0.25f);
				CHECK(sp.revived);
			}

			const net::DeathEvent *de = last_death(medic, victim);
			const net::ReviveEvent *re = last_revive(medic, victim);
			REQUIRE(de != nullptr);
			REQUIRE(re != nullptr);
			std::printf("E5 (%s victim): at the medic's peer: DeathEvent respawn_tick +%u, revive_until +%u, killer_health %.2f; ReviveEvent reviver %u "
						"revived %u tick %u (revived at %u)\n",
					victim_peer ? "peer" : "bot", unsigned(de->respawn_tick - died), unsigned(de->revive_until - died), de->killer_health,
					unsigned(re->reviver), unsigned(re->revived), unsigned(re->tick), unsigned(revived));
			CHECK(de->respawn_tick == respawn_at);
			CHECK(de->revive_until == revive_until);
			CHECK(de->killer_health > 0.0f);
			CHECK(int(re->reviver) == medic.slot());
			CHECK(re->tick == back.tick);

			const uint64_t refused2 = s2.defib_refused;
			medic_fire = true;
			while (m->now() < respawn_at + 30u) {
				m->tick();
			}
			medic_fire = false;
			CHECK(m->state(victim)->life == back.life);
			CHECK(is_alive(m->state(victim)));
			CHECK(m->rules().stats().revives == 1u);
			CHECK(m->rules().stats().defib_refused > refused2);
			CHECK(m->rules().stats().runtime_faults == 0u);
		}
	}

	TEST_CASE("revive: deploy screen, after the man-down window and after the victim deployed") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 21u);

		tdm::ScriptedPeer &medic = m->join("medic");
		medic.ready();
		Script *shooter = m->bot();
		tdm::ScriptedPeer &victim_p = m->join("victim");
		victim_p.ready();
		m->bot();
		REQUIRE(wait_in_round(*m));
		medic.spawn_request(0);
		victim_p.spawn_request(2);
		m->ticks(3);
		const int victim = victim_p.slot();
		REQUIRE(is_alive(m->state(medic.slot())));
		REQUIRE(is_alive(m->state(victim)));
		REQUIRE(m->rules().team_of(uint8_t(medic.slot())) == 1);
		REQUIRE(m->rules().team_of(uint8_t(victim)) == 1);
		REQUIRE(m->rules().team_of(uint8_t(shooter->slot)) == 2);
		REQUIRE(m->server.gunplay()->kit(uint8_t(medic.slot())) == 0);
		const std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		const sim::Vec3 corpse_centre = add(patch->victim, 0.0f, 0.3f, 0.0f);
		bool medic_fire = false, shooter_fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const bool press = medic_fire && (p_tick % 60u) < 50u;
			return aim_command(*m, p_tick, p_state, corpse_centre, sim::Pose::Crouch, select_and_fire(host::kKitGadgetSlot, press));
		};
		shooter->fn = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::SoldierState *v = m->state(victim);
			const sim::Vec3 chest = v != nullptr ? add(v->position, 0.0f, 1.2f, 0.0f) : patch->victim;
			return aim_command(*m, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, shooter_fire && (p_tick % 6u) < 3u));
		};
		const sim::Vec3 away = m->level->spawns[(patch->spawn + 50u) % m->level->spawns.size()].position;

		struct Kill {
			sim::Tick died = 0;
			uint32_t respawn_at = 0, revive_until = 0, life = 0;
		};
		const auto kill = [&]() -> Kill {
			m->teleport(victim_p, add(patch->victim, 0.0f, 0.05f, 0.0f));
			m->teleport(medic, add(patch->medic, 0.0f, 0.05f, 0.0f));
			m->place(shooter->slot, add(patch->shooter, 0.0f, 0.05f, 0.0f));
			m->ticks(90);
			Kill k;
			k.life = m->state(victim)->life;
			shooter_fire = true;
			m->until(
					[&] {
						if (!is_alive(m->state(victim))) {
							k.died = m->now();
						}
						return k.died != 0;
					},
					900);
			shooter_fire = false;
			m->place(shooter->slot, away);
			m->tick();
			k.respawn_at = m->rules().earliest_respawn(uint8_t(victim));
			k.revive_until = m->rules().revive_until(uint8_t(victim));
			return k;
		};

		const Kill k1 = kill();
		REQUIRE(k1.died != 0);
		std::printf("E5 deploy: died at %u, respawn +%u, revive until +%u, downed %zu\n", unsigned(k1.died), unsigned(k1.respawn_at - k1.died),
				unsigned(k1.revive_until - k1.died), m->rules().downed().size());
		CHECK(k1.revive_until > k1.died);
		CHECK(m->rules().downed().size() == 1u);
		while (m->now() < k1.revive_until + 5u) {
			m->tick();
		}
		const host::RulesStats s0 = m->rules().stats();
		medic_fire = true;
		m->ticks(130);
		medic_fire = false;
		const host::RulesStats s1 = m->rules().stats();
		std::printf("E5 deploy: after the window: %" PRIu64 " defib shots, %" PRIu64 " refused, %" PRIu64 " misses, revives %" PRIu64 ", downed %zu\n",
				s1.defib_shots - s0.defib_shots, s1.defib_refused - s0.defib_refused, s1.defib_misses - s0.defib_misses, s1.revives,
				m->rules().downed().size());
		CHECK(s1.defib_shots > s0.defib_shots);
		CHECK(s1.revives == 0u);
		CHECK_FALSE(is_alive(m->state(victim)));
		CHECK(m->state(victim)->life == k1.life);
		const size_t spawned1 = victim_p.spawned.size();
		victim_p.spawn_request(2);
		m->ticks(2);
		REQUIRE(victim_p.spawned.size() == spawned1 + 1u);
		CHECK_FALSE(victim_p.spawned.back().revived);
		CHECK(m->state(victim)->life == k1.life + 1u);

		m->ticks(130);
		const Kill k2 = kill();
		REQUIRE(k2.died != 0);
		CHECK(k2.respawn_at < k2.revive_until);
		while (m->now() + 1u < k2.respawn_at) {
			m->tick();
		}
		const uint64_t accepted = m->rules().stats().spawns_accepted;
		victim_p.spawn_request(1);
		CHECK(m->rules().stats().spawns_accepted == accepted + 1u);
		CHECK(m->rules().revive_until(uint8_t(victim)) == 0u);
		CHECK(m->rules().downed().empty());
		medic_fire = true;
		m->ticks(130);
		medic_fire = false;
		std::printf("E5 deploy: deployed at %u (death + %u): revive_until %u, downed %zu, revives %" PRIu64 ", life %u -> %u, revived %d\n",
				unsigned(k2.respawn_at), unsigned(k2.respawn_at - k2.died), unsigned(m->rules().revive_until(uint8_t(victim))), m->rules().downed().size(),
				m->rules().stats().revives, unsigned(k2.life), unsigned(m->state(victim)->life), victim_p.spawned.back().revived ? 1 : 0);
		CHECK(m->rules().stats().revives == 0u);
		CHECK(m->state(victim)->life == k2.life + 1u);
		CHECK(victim_p.spawned.back().kit == 1);
		CHECK_FALSE(victim_p.spawned.back().revived);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("scoring: the install's scoring table rides on the shared level; the TDM scores come from it") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->shared->scoring != nullptr);
		REQUIRE(level->shared->tdm_tuning != nullptr);
		const core::gameplay::ScoringTable &scoring = *level->shared->scoring;
		CHECK(scoring.score("ID_SCORE_KILL") == std::optional<double>(100.0));
		CHECK(scoring.score("ID_SCORE_HEADSHOT") == std::optional<double>(10.0));
		CHECK(scoring.score("ID_SCORE_AVENGER") == std::optional<double>(10.0));
		CHECK(scoring.score("ID_SCORE_SAVIOUR") == std::optional<double>(20.0));
		CHECK(level->shared->tdm_tuning->score_kill == 100);
		CHECK(level->shared->tdm_tuning->score_headshot == 10);
	}

	TEST_CASE("end of round: the uiendofroundlogic timeline rides on the shared level") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->shared->eor != nullptr);
		const core::gameplay::EorTimeline &t = *level->shared->eor;
		std::printf("end of round: fade %.2f s, scoreboard after %.2f s, end-of-round request after %.2f s, PreEorTime "
					"%.2f s, EorTime %.2f s, restart "
					"after %.2f s\n",
				double(t.fade_s), double(t.scoreboard_delay_s), double(t.request_eor_delay_s), double(t.pre_eor_s),
				double(t.eor_s), double(t.restart_delay_s));
		CHECK(t.fade_s == 1.0f);
		CHECK(t.scoreboard_delay_s == 4.0f);
		CHECK(t.request_eor_delay_s == 9.0f);
		CHECK(t.pre_eor_s == 12.0f);
		CHECK(t.eor_s == 48.0f);
		CHECK(t.restart_delay_s == 60.0f);
		CHECK(t.pre_eor_s + t.eor_s == t.restart_delay_s);
		CHECK(t.fade_s + t.request_eor_delay_s <= t.pre_eor_s);
	}

	TEST_CASE("end of round: mp_eor_fading's white flash and screen fade, and the showroom's ShowTopPlayers camera") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->shared->eor_fading != nullptr);
		const core::gameplay::EorFading &f = *level->shared->eor_fading;
		std::printf("eor fading: flash %.2f s, UI fade %.2f s, screen fade %.2f s, brightness %.1f, saturation %.1f\n",
				double(f.flash_s), double(f.ui_fade_s), double(f.screen_fade_s), double(f.brightness[0]),
				double(f.saturation[0]));
		CHECK(f.flash_s == 0.75f);
		REQUIRE(f.visibility.size() == 2);
		CHECK(f.visibility[0].seconds == 0.0f);
		CHECK(f.visibility[0].value == 0.0f);
		CHECK(f.visibility[1].seconds == 0.75f);
		CHECK(f.visibility[1].value == 1.0f);
		CHECK(f.ui_fade_s == 0.33f);
		CHECK(f.screen_fade_s == 0.5f);
		CHECK(f.brightness == std::array<float, 3>{ 20.0f, 20.0f, 20.0f });
		CHECK(f.contrast == std::array<float, 3>{ 1.0f, 1.0f, 1.0f });
		CHECK(f.saturation == std::array<float, 3>{ 0.0f, 0.0f, 0.0f });
		CHECK(f.visual_environment == "fx/visualenviroments/fullscreen/ve_whitescreen");
		CHECK(f.visibility_at(0.375f) == doctest::Approx(0.5f));
		CHECK(f.visibility_at(1.0f) == 1.0f);
		CHECK(f.visibility_at(1.25f) == 0.0f);
		CHECK(f.screen_black_at(0.5f) == 0.0f);
		CHECK(f.screen_black_at(1.0f) == doctest::Approx(0.5f));
		CHECK(f.screen_black_at(2.0f) == 1.0f);
		CHECK(f.ui_alpha_at(0.33f) == 0.0f);

		REQUIRE(level->shared->eor_camera.has_value());
		const core::level::EorCamera &c = *level->shared->eor_camera;
		std::printf("eor camera: object %u at %.4f %.4f %.4f, forward %.3f %.3f %.3f, fov %.1f, priority %d\n",
				unsigned(c.object), double(c.transform.trans[0]), double(c.transform.trans[1]),
				double(c.transform.trans[2]), double(c.transform.forward[0]), double(c.transform.forward[1]),
				double(c.transform.forward[2]), double(c.fov_deg), int(c.priority));
		CHECK(c.fov_deg == 23.0f);
		CHECK(c.priority == 3);
		CHECK(std::abs(c.transform.trans[0] - (-15.04f)) < 0.005f);
		CHECK(std::abs(c.transform.trans[1] - (-277.07f)) < 0.005f);
		CHECK(std::abs(c.transform.trans[2] - (-7.28f)) < 0.005f);
	}

	TEST_CASE("deploy list: the TDM setup's two character spawners, one per team, both RANDOM SPAWN POINT") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::vector<int> teams;
		for (const core::level::CharacterSpawnerDef &s : level->shared->spawners) {
			if (s.enabled && !s.location_text_sid.empty()) {
				teams.push_back(s.team);
				CHECK(s.location_text_sid == "ID_H_MP_DM_RANDOMSPAWN");
			}
		}
		std::sort(teams.begin(), teams.end());
		CHECK(teams == std::vector<int>{ 1, 2 });
	}

	TEST_CASE("round start: the data's RoundMinPlayerCount holds one peer in the pre-round; the min-players override starts it") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->shared->logic_natives != nullptr);
		const int32_t data_min = level->shared->logic_natives->round_min_players;
		std::printf("round start: RoundMinPlayerCount %d\n", int(data_min));
		CHECK(data_min == 4);

		std::unique_ptr<Match> held = start_match(*level, false);
		held->join("alone");
		CHECK_FALSE(wait_in_round(*held));
		CHECK(held->rules().phase() == net::MatchPhase::PreRound);
		CHECK(held->rules().match_state().players_needed == uint16_t(data_min - 1));
		CHECK(held->rules().match_state().countdown_end_tick == 0);

		std::unique_ptr<Match> started = start_match(*level, false, 7, 0, 1);
		started->join("alone");
		CHECK(wait_in_round(*started));
		CHECK(started->rules().phase() == net::MatchPhase::InRound);
		CHECK(started->rules().match_state().players_needed == 0);
	}

	TEST_CASE("kill limit: kills before the round over lands count to the KillCounter's limit, not past it") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		constexpr int kLimit = 2;
		std::unique_ptr<Match> m = start_match(*level, false, 7, kLimit, 1);
		std::vector<Script *> bots;
		for (int i = 0; i < 6; ++i) {
			bots.push_back(m->bot());
		}
		REQUIRE(wait_in_round(*m));
		int killer = -1;
		std::vector<int> victims;
		for (const Script *b : bots) {
			const uint8_t team = m->rules().team_of(uint8_t(b->slot));
			if (team == 1 && killer < 0) {
				killer = b->slot;
			} else if (team == 2) {
				victims.push_back(b->slot);
			}
		}
		REQUIRE(killer >= 0);
		REQUIRE(victims.size() == 3u);
		for (const int v : victims) {
			net::DeathEvent e;
			e.victim = uint8_t(v);
			e.killer = uint8_t(killer);
			m->server.rules()->death(e);
		}
		REQUIRE(m->until([&] { return m->rules().phase() == net::MatchPhase::RoundOver; }, 60));
		const net::MatchState &ms = m->rules().match_state();
		std::printf("kill limit: 3 kills by team 1 in one tick at limit %d: round over US %d - RU %d, winner %u\n",
				kLimit, int(ms.kills[0]), int(ms.kills[1]), unsigned(ms.winner));
		CHECK(ms.kills[0] == kLimit);
		CHECK(ms.kills[1] == 0);
		CHECK(ms.winner == 1);
		CHECK_FALSE(m->rules().faulted());
	}

	TEST_CASE("hostile: SpawnRequest refusals and v3 datagrams") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}

		{
			core::Result<std::unique_ptr<host::TdmRules>> created = host::TdmRules::create(*level->shared, nullptr, tdm_rules_config(true), 5, 128);
			REQUIRE_MESSAGE(created.ok(), (created.ok() ? std::string() : created.error().message));
			host::TdmRules &r = **created;
			using host::SpawnRefusal;
			CHECK(r.on_connect(70, false, "spectator") == 0);
			CHECK(r.request_spawn(70, 0, 0, 10) == SpawnRefusal::Spectator);
			const uint8_t team = r.on_connect(0, false, "human");
			CHECK((team == 1 || team == 2));
			CHECK(r.on_connect(1, true, "") != 0);
			CHECK(r.request_spawn(0, host::kKitCount, 0, 10) == SpawnRefusal::BadKit);
			CHECK(r.request_spawn(0, 0, 1, 11) == SpawnRefusal::SquadMate);
			CHECK(r.request_spawn(0, 1, 0, 12) == SpawnRefusal::None);
			CHECK(r.request_spawn(0, 1, 0, 12) == SpawnRefusal::RateLimited);
			CHECK(r.request_spawn(0, 1, 0, 13) == SpawnRefusal::Alive);
			CHECK(r.request_spawn(1, 1, 0, 14) == SpawnRefusal::Phase);
			r.on_spawned(0, 20, false);
			CHECK(r.kit_of(0) == 1);
			CHECK(r.request_spawn(0, 1, 0, 21) == SpawnRefusal::Alive);
			CHECK_FALSE(r.respawn_tick(0, 100).has_value());
			net::DeathEvent e;
			e.victim = 0;
			e.killer = 0;
			r.death(e);
			const uint32_t earliest = r.earliest_respawn(0);
			CHECK(earliest >= 300u);
			CHECK(r.request_spawn(0, 2, 0, earliest - 1u) == SpawnRefusal::TooEarly);
			CHECK(r.request_spawn(0, 2, 0, earliest) == SpawnRefusal::None);
			CHECK(r.kit_of(0) == 2);
			const host::RulesStats &st = r.stats();
			std::printf("E7 refusals: requests %" PRIu64 ", accepted %" PRIu64 ", alive %" PRIu64 ", too early %" PRIu64 ", bad kit %" PRIu64 ", phase %" PRIu64
						", rate %" PRIu64 ", spectator %" PRIu64 "\n",
					st.spawn_requests, st.spawns_accepted, st.refused[1], st.refused[2], st.refused[3], st.refused[4], st.refused[5], st.refused[6]);
			CHECK(st.spawn_requests == 10u);
			CHECK(st.spawns_accepted == 2u);
			CHECK(st.refused[size_t(SpawnRefusal::Alive)] == 2u);
			CHECK(st.refused[size_t(SpawnRefusal::TooEarly)] == 1u);
			CHECK(st.refused[size_t(SpawnRefusal::BadKit)] == 1u);
			CHECK(st.refused[size_t(SpawnRefusal::SquadMate)] == 1u);
			CHECK(st.refused[size_t(SpawnRefusal::Phase)] == 1u);
			CHECK(st.refused[size_t(SpawnRefusal::RateLimited)] == 1u);
			CHECK(st.refused[size_t(SpawnRefusal::Spectator)] == 1u);
			CHECK_FALSE(r.faulted());
		}

		std::unique_ptr<Match> m = start_match(*level, true);
		for (int i = 0; i < 3; ++i) {
			m->bot();
		}
		tdm::ScriptedPeer &p = m->join("hostile");
		p.ready();
		REQUIRE(wait_in_round(*m));
		const uint64_t bad0 = m->server.stats().bad_datagrams;
		uint64_t sent = 0;
		uint64_t rng = 0x9e3779b97f4a7c15ull;
		const auto next = [&] {
			rng = sw::splitmix64(rng);
			return rng;
		};
		const std::vector<uint8_t> valid = net::encode_spawn_request(net::SpawnRequest{ 1, 0 });
		REQUIRE(!valid.empty());
		for (int round = 0; round < 60; ++round) {
			for (size_t cut = 1; cut < valid.size(); ++cut) {
				m->server.on_receive(p.peer(), net::kChannelReliable, std::span<const uint8_t>(valid.data(), cut));
				++sent;
			}
			std::vector<uint8_t> junk(1 + next() % 40);
			for (uint8_t &b : junk) {
				b = uint8_t(next());
			}
			junk[0] = uint8_t(net::MessageType::SpawnRequest);
			m->server.on_receive(p.peer(), net::kChannelReliable, junk);
			junk[0] = uint8_t(net::MessageType::MatchState);
			m->server.on_receive(p.peer(), net::kChannelReliable, junk);
			junk[0] = uint8_t(net::MessageType::PlayerList);
			m->server.on_receive(p.peer(), net::kChannelReliable, junk);
			sent += 3;
			m->tick();
		}
		const uint64_t bad = m->server.stats().bad_datagrams - bad0;
		std::printf("E7 hostile datagrams: %" PRIu64 " sent, %" PRIu64 " counted bad, %" PRIu64 " spawn requests reached the rules, the peer %s\n", sent, bad,
				m->rules().stats().spawn_requests, is_alive(m->state(p.slot())) ? "alive" : "not spawned");
		CHECK(bad + m->rules().stats().spawn_requests == sent);
		CHECK(m->rules().stats().runtime_faults == 0u);
		CHECK(m->server.stats().step_precondition_failures == 0u);

		std::unique_ptr<Match> o = start_match(*level, true, 31u, 1);
		Script *killer = o->bot();
		Script *target = o->bot();
		o->bot();
		tdm::ScriptedPeer &late = o->join("late");
		late.ready();
		REQUIRE(wait_in_round(*o));
		o->ticks(130);
		const std::optional<Patch> patch = find_patch(*o, 3.0f);
		REQUIRE(patch.has_value());
		o->place(target->slot, add(patch->victim, 0.0f, 0.05f, 0.0f));
		o->place(killer->slot, add(patch->shooter, 0.0f, 0.05f, 0.0f));
		killer->fn = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::SoldierState *v = o->state(target->slot);
			const sim::Vec3 chest = v != nullptr ? add(v->position, 0.0f, 1.2f, 0.0f) : patch->victim;
			return aim_command(*o, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, (p_tick % 6u) < 3u));
		};
		REQUIRE(o->until([&] { return o->rules().phase() == net::MatchPhase::RoundOver; }, 900));
		killer->fn = nullptr;
		const uint64_t phase0 = o->rules().stats().refused[size_t(host::SpawnRefusal::Phase)];
		late.spawn_request(0);
		o->ticks(2);
		std::printf("E7 RoundOver: kills %d / %d, a SpawnRequest refused (phase %" PRIu64 " -> %" PRIu64 "), the peer %s\n", o->rules().match_state().kills[0],
				o->rules().match_state().kills[1], phase0, o->rules().stats().refused[size_t(host::SpawnRefusal::Phase)],
				is_alive(o->state(late.slot())) ? "spawned" : "not spawned");
		CHECK(o->rules().stats().refused[size_t(host::SpawnRefusal::Phase)] == phase0 + 1u);
		CHECK(late.spawned.empty());
		CHECK_FALSE(is_alive(o->state(late.slot())));
	}

	TEST_CASE("loadout: the install catalog holds every unlock of each kit slot; resolve_loadout keeps defaults and "
			  "refuses foreign items") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		REQUIRE_FALSE(gl.loadouts.empty());
		CHECK(level->shared->level_name_sid == "ID_M_LEVELNAME_MP_SUBWAY");
		CHECK(gl.loadouts.kit(1, 0)->label_sid_hash == core::ui::text_hash("ID_M_ASSAULT"));
		CHECK(gl.loadouts.kit(2, 3)->label_sid_hash == core::ui::text_hash("ID_M_RECON"));
		size_t defaults = 0;
		for (const auto &team : gl.kits.loadouts) {
			for (const host::KitLoadout &l : team) {
				for (const uint16_t d : l.defs) {
					if (d != sw::kNoWeapon) {
						defaults = std::max<size_t>(defaults, size_t(d) + 1u);
					}
				}
			}
		}
		const host::LoadoutKit *us_assault = gl.loadouts.kit(1, uint8_t(host::KitRole::Assault));
		REQUIRE(us_assault != nullptr);
		CHECK(us_assault->slots[0].size() == 28);
		CHECK(us_assault->slots[1].size() == 18);
		CHECK(us_assault->slots[6].size() == 1);
		CHECK(us_assault->slots[7].size() == 2);
		CHECK(us_assault->defaults[0] == 2580429694u);
		const host::LoadoutItem *m16 = us_assault->find(0, 2580429694u);
		REQUIRE(m16 != nullptr);
		CHECK(m16->unlock == "weapons/m16a4/u_m16a4");
		CHECK(m16->def == gl.kits.loadout(1, 0).defs[0]);
		CHECK(m16->accessories[0].size() == 13);
		CHECK(m16->accessories[1].size() == 3);
		CHECK(m16->accessories[2].size() == 6);
		CHECK(m16->accessories[3].size() == 1);
		CHECK(m16->default_accessories == std::array<uint32_t, 4>{ 3010458987u, 975928659u, 1580481064u, 2341043160u });
		for (size_t a = 0; a < host::kLoadoutAccessories; ++a) {
			CHECK(m16->accessory_unlocks[a].size() == m16->accessories[a].size());
		}
		CHECK(host::loadout_primary_item(gl.loadouts, 1, m16->def) == m16);
		CHECK(host::loadout_primary_item(gl.loadouts, 1, sw::kNoWeapon) == nullptr);
		const host::LoadoutAccessorySet m16_plain = host::loadout_accessory_set(*m16, {});
		CHECK(m16_plain.listed.size() == 23);
		CHECK(m16_plain.active ==
				std::vector<std::string>{ "weapons/common/nooptics", "weapons/common/nosecondaryrail",
						"weapons/common/nosecondaryaccessory", "weapons/common/nocamo" });
		CHECK(m16_plain.picked == std::array<uint32_t, 4>{});
		CHECK(host::loadout_accessory_set(*m16, m16->default_accessories).active ==
				std::vector<std::string>{ "weapons/m16a4/u_m16a4_acog", "weapons/m16a4/u_m16a4_foregrip",
						"weapons/m16a4/u_m16a4_heavybarrel", "weapons/common/nocamo" });
		const host::LoadoutAccessorySet m16_optic =
				host::loadout_accessory_set(*m16, { m16->accessories[0].front(), 0, 12345u, 0 });
		CHECK(m16_optic.active.front() == m16->accessory_unlocks[0].front());
		CHECK(m16_optic.active.size() == 4);
		CHECK(m16_optic.picked == std::array<uint32_t, 4>{ m16->accessories[0].front(), 0, 0, 0 });
		CHECK(us_assault->category_sid_hashes[0] == core::ui::text_hash("ID_M_SOLDIER_PRIMARY"));
		CHECK(host::loadout_item_category_sid(*us_assault, host::LoadoutKitItem::Gadget1) ==
				core::ui::text_hash("ID_M_SOLDIER_GADGET1"));
		CHECK(host::loadout_item_category_sid(*us_assault, host::LoadoutKitItem::Gadget2) ==
				core::ui::text_hash("ID_M_SOLDIER_GADGET2"));
		CHECK(host::loadout_item_slots(*us_assault, host::LoadoutKitItem::Gadget2) == std::vector<uint8_t>{ 2, 5 });
		CHECK(host::loadout_item_options(*us_assault, host::LoadoutKitItem::Gadget1).size() == 1);
		CHECK(host::loadout_item_options(*us_assault, host::LoadoutKitItem::Gadget2).size() == 12);
		CHECK(us_assault->specializations.size() == 15);
		CHECK(us_assault->default_specialization != 0);
		CHECK(us_assault->specialization_sid_hash != 0);
		const host::LoadoutKit *us_support = gl.loadouts.kit(1, uint8_t(host::KitRole::Support));
		REQUIRE(us_support != nullptr);
		CHECK(host::loadout_item_slots(*us_support, host::LoadoutKitItem::Gadget1) == std::vector<uint8_t>{ 4 });
		CHECK(host::loadout_item_slots(*us_support, host::LoadoutKitItem::Gadget2) == std::vector<uint8_t>{ 2, 5 });
		CHECK(host::loadout_item_category_sid(*us_support, host::LoadoutKitItem::Gadget1) ==
				core::ui::text_hash("ID_M_SOLDIER_GADGET1"));
		const host::LoadoutKit *us_engineer = gl.loadouts.kit(1, uint8_t(host::KitRole::Engineer));
		REQUIRE(us_engineer != nullptr);
		CHECK(host::loadout_item_slots(*us_engineer, host::LoadoutKitItem::Gadget1) == std::vector<uint8_t>{ 2 });
		CHECK(host::loadout_item_slots(*us_engineer, host::LoadoutKitItem::Gadget2) == std::vector<uint8_t>{ 5 });
		size_t simulated_items = 0, missing_defs = 0;
		for (const auto &team : gl.loadouts.kits) {
			for (const host::LoadoutKit &k : team) {
				for (const uint8_t s : host::kKitSlots) {
					for (const host::LoadoutItem &i : k.slots[s]) {
						++simulated_items;
						missing_defs +=
								i.def < gl.defs.size() && gl.defs[i.def].unlock_identifier == i.identifier ? 0u : 1u;
					}
				}
			}
		}
		CHECK(missing_defs == 0);

		const host::Loadout plain = host::default_loadout(gl.loadouts, gl.kits, 1, 0);
		CHECK(plain.defs == gl.kits.loadout(1, 0).defs);
		CHECK_FALSE(plain.chosen);
		core::Result<host::Loadout> echo =
				host::resolve_loadout(gl.loadouts, gl.kits, 1, 0, host::LoadoutChoice{ plain.choice.items, {}, 0 });
		REQUIRE(echo.ok());
		CHECK(echo->defs == plain.defs);
		host::LoadoutChoice pick;
		const host::LoadoutItem &other = us_assault->slots[0].back();
		pick.items[0] = other.identifier;
		pick.accessories[0] = other.accessories[0].empty() ? 0u : other.accessories[0].front();
		core::Result<host::Loadout> picked = host::resolve_loadout(gl.loadouts, gl.kits, 1, 0, pick);
		REQUIRE_MESSAGE(picked.ok(), (picked.ok() ? std::string() : picked.error().message));
		CHECK(picked->defs[0] == other.def);
		CHECK(picked->defs[1] == plain.defs[1]);
		CHECK(picked->chosen);
		host::LoadoutChoice wrong_slot;
		wrong_slot.items[0] = us_assault->slots[1].front().identifier;
		CHECK_FALSE(host::resolve_loadout(gl.loadouts, gl.kits, 1, 0, wrong_slot).ok());
		CHECK(host::resolve_loadout(gl.loadouts, gl.kits, 1, 0, host::LoadoutChoice{ {}, { 3011272554u, 0, 0, 0 }, 0 })
						.ok());
		CHECK_FALSE(host::resolve_loadout(
				gl.loadouts, gl.kits, 1, 0, host::LoadoutChoice{ {}, { 0, 3011272554u, 0, 0 }, 0 })
						.ok());
		CHECK_FALSE(host::resolve_loadout(gl.loadouts, gl.kits, 1, 0, host::LoadoutChoice{ {}, {}, 12345u }).ok());
		CHECK_FALSE(host::resolve_loadout(gl.loadouts, gl.kits, 3, 0, pick).ok());
		CHECK_FALSE(host::resolve_loadout(gl.loadouts, gl.kits, 1, 4, pick).ok());
		std::printf("loadout catalog: %zu items (%zu in simulated slots), %zu weapon defs (%zu defaults), US assault "
					"28 primaries, M16A4 13/3/6/1 "
					"accessories, hash %016" PRIx64 "\n",
				gl.loadouts.items, simulated_items, gl.defs.size(), defaults, gl.loadouts.hash);
	}

	TEST_CASE("loadout: a LoadoutRequest picks the kit items, the next spawn carries them, a disallowed item is "
			  "refused, bots keep theirs") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		std::unique_ptr<Match> m = start_match(*level, true, 43u);
		std::vector<int> bots;
		for (int i = 0; i < 3; ++i) {
			bots.push_back(m->bot()->slot);
		}
		tdm::ScriptedPeer &p = m->join("picker");
		p.ready();
		tdm::ScriptedPeer &watcher = m->join("watcher");
		watcher.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t team = m->rules().team_of(uint8_t(p.slot()));
		REQUIRE((team == 1 || team == 2));
		const host::LoadoutKit *kit = gl.loadouts.kit(team, 0);
		REQUIRE(kit != nullptr);
		const host::KitLoadout &plain = gl.kits.loadout(team, 0);
		const host::LoadoutItem *primary = nullptr;
		for (const host::LoadoutItem &i : kit->slots[0]) {
			if (primary == nullptr && i.def != plain.defs[0] && !i.accessories[0].empty()) {
				primary = &i;
			}
		}
		const host::LoadoutItem *knife = nullptr;
		for (const host::LoadoutItem &i : kit->slots[7]) {
			if (knife == nullptr && i.def != plain.defs[7]) {
				knife = &i;
			}
		}
		REQUIRE(primary != nullptr);
		REQUIRE(knife != nullptr);
		REQUIRE_FALSE(kit->specializations.empty());
		net::LoadoutRequest request;
		request.kit = 0;
		request.items[0] = primary->identifier;
		request.items[7] = knife->identifier;
		request.accessories[0] = primary->accessories[0].front();
		request.specialization = kit->specializations.front();
		const host::GunplayStats g0 = m->server.stats().gunplay;
		const uint64_t bad0 = m->server.stats().bad_datagrams;
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		net::LoadoutRequest wrong_slot = request;
		wrong_slot.items[0] = kit->slots[1].front().identifier;
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_loadout_request(wrong_slot));
		net::LoadoutRequest wrong_accessory = request;
		wrong_accessory.accessories[0] = 12345u;
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_loadout_request(wrong_accessory));
		std::vector<uint8_t> long_request = net::encode_loadout_request(request);
		long_request.push_back(0);
		m->server.on_receive(p.peer(), net::kChannelReliable, long_request);
		m->ticks(1);
		const host::GunplayStats g1 = m->server.stats().gunplay;
		CHECK(g1.loadout_select == g0.loadout_select + 1u);
		CHECK(g1.loadout_refused == g0.loadout_refused + 3u);
		CHECK(m->server.stats().bad_datagrams == bad0 + 1u);

		const size_t spawned0 = p.spawned.size();
		p.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		m->ticks(2);
		const host::Gunplay &g = *m->server.gunplay();
		const sw::SoldierWeaponsState &w = g.weapons(uint8_t(p.slot()));
		CHECK(w.slots[0].def == primary->def);
		CHECK(w.slots[1].def == plain.defs[1]);
		CHECK(w.slots[6].def == plain.defs[6]);
		CHECK(w.slots[7].def == knife->def);
		const host::Loadout &carried = g.loadout(uint8_t(p.slot()));
		CHECK(carried.chosen);
		CHECK(carried.choice.items[0] == primary->identifier);
		CHECK(carried.choice.items[7] == knife->identifier);
		CHECK(carried.choice.items[1] == kit->defaults[1]);
		CHECK(carried.choice.accessories[0] == primary->accessories[0].front());
		CHECK(carried.choice.specialization == kit->specializations.front());
		REQUIRE(p.spawned.size() > spawned0);
		CHECK(p.spawned.back().defs == carried.defs);
		CHECK(p.spawned.back().defs[0] == primary->def);
		const auto gear_of = [&](const tdm::ScriptedPeer &p_peer) {
			std::optional<net::SoldierGear> out;
			for (const net::SoldierGear &gr : p_peer.gear) {
				out = gr.slot == uint8_t(p.slot()) ? std::optional<net::SoldierGear>(gr) : out;
			}
			return out;
		};
		const net::SoldierGear want{ uint8_t(p.slot()), primary->def, carried.choice.accessories };
		CHECK(gear_of(p) == want);
		CHECK(gear_of(watcher) == want);
		tdm::ScriptedPeer &late = m->join("late");
		late.ready();
		m->ticks(2);
		CHECK(gear_of(late) == want);
		size_t bot_gears = 0;
		for (const net::SoldierGear &gr : late.gear) {
			bot_gears += std::find(bots.begin(), bots.end(), int(gr.slot)) != bots.end() &&
							gr.accessories == std::array<uint32_t, net::kLoadoutAccessories>{}
					? 1u
					: 0u;
		}
		CHECK(bot_gears > 0);
		size_t bots_checked = 0;
		for (const int b : bots) {
			if (!g.armed(uint8_t(b))) {
				continue;
			}
			++bots_checked;
			CHECK(g.weapons(uint8_t(b)).slots[0].def == gl.kits.loadout(g.team(uint8_t(b)), g.kit(uint8_t(b))).defs[0]);
			CHECK_FALSE(g.loadout(uint8_t(b)).chosen);
		}
		CHECK(bots_checked > 0);
		CHECK(m->server.stats().gunplay.loadout_dropped == g0.loadout_dropped);
		std::printf("loadout: the peer spawned with %s (def %u) and %s (def %u), 1 request taken, 3 refused, %zu bots "
					"on their kit defaults\n",
				primary->unlock.c_str(), unsigned(primary->def), knife->unlock.c_str(), unsigned(knife->def),
				bots_checked);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("suicide: a SuicideRequest kills the peer's soldier as its own killer; refused while dead; a bad one is "
			  "counted") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 41u);
		for (int i = 0; i < 3; ++i) {
			m->bot();
		}
		tdm::ScriptedPeer &p = m->join("quitter");
		p.ready();
		REQUIRE(wait_in_round(*m));
		p.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		m->ticks(30);
		REQUIRE(is_alive(m->state(p.slot())));
		const host::RulesStats s0 = m->rules().stats();
		const uint64_t bad0 = m->server.stats().bad_datagrams;
		const size_t combat0 = p.combat.size();
		const std::vector<uint8_t> request = net::encode_suicide_request();
		CHECK(request.size() == 1u);
		m->server.on_receive(p.peer(), net::kChannelReliable, request);
		m->ticks(3);
		CHECK_FALSE(is_alive(m->state(p.slot())));
		size_t own = 0;
		for (size_t i = combat0; i < p.combat.size(); ++i) {
			for (const net::DeathEvent &d : p.combat[i].deaths) {
				own += d.victim == uint8_t(p.slot()) && d.killer == uint8_t(p.slot()) ? 1u : 0u;
			}
		}
		CHECK(own == 1u);
		CHECK(m->rules().stats().suicides == s0.suicides + 1u);
		m->server.on_receive(p.peer(), net::kChannelReliable, request);
		m->ticks(2);
		CHECK(m->rules().stats().suicides == s0.suicides + 1u);
		CHECK(m->rules().stats().suicides_refused == s0.suicides_refused + 1u);
		std::vector<uint8_t> extra = request;
		extra.push_back(0);
		m->server.on_receive(p.peer(), net::kChannelReliable, extra);
		CHECK(m->server.stats().bad_datagrams == bad0 + 1u);
		std::printf("suicide: the peer died as its own killer, %" PRIu64 " suicide, %" PRIu64 " refused while dead\n",
				m->rules().stats().suicides - s0.suicides, m->rules().stats().suicides_refused - s0.suicides_refused);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("revive: a Revive debug request brings the dead peer back at its corpse as its own reviver; refused "
			  "while alive") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 43u);
		for (int i = 0; i < 3; ++i) {
			m->bot();
		}
		tdm::ScriptedPeer &p = m->join("lazarus");
		p.ready();
		REQUIRE(wait_in_round(*m));
		p.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		m->ticks(30);
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_suicide_request());
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(p.slot())));
		const sim::SoldierState dead = *m->state(p.slot());
		REQUIRE(m->row(p.slot()) != nullptr);
		const int32_t score = m->row(p.slot())->score;
		const host::RulesStats s0 = m->rules().stats();
		net::DebugRequest q;
		q.kind = net::DebugKind::Revive;
		q.id = m->next_debug++;
		p.debug(q);
		m->ticks(3);
		REQUIRE(is_alive(m->state(p.slot())));
		const sim::SoldierState back = *m->state(p.slot());
		const net::ReviveEvent *re = last_revive(p, p.slot());
		REQUIRE(re != nullptr);
		std::printf("revive request: life %u -> %u, %.2f hp, %.3f m from the corpse, revives +%" PRIu64
					", reviver %u\n",
				unsigned(dead.life), unsigned(back.life), back.health, distance(back.position, dead.position),
				m->rules().stats().revives - s0.revives, unsigned(re->reviver));
		CHECK(back.life == dead.life + 1u);
		CHECK(distance(back.position, dead.position) < 0.25f);
		CHECK(m->rules().stats().revives == s0.revives + 1u);
		CHECK(int(re->reviver) == p.slot());
		CHECK(m->row(p.slot())->score == score);
		CHECK(m->rules().downed().empty());
		q.id = m->next_debug++;
		p.debug(q);
		m->ticks(3);
		CHECK(m->state(p.slot())->life == back.life);
		CHECK(m->rules().stats().revives == s0.revives + 1u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("restart: the in-place restart unspawns the deploy-screen peer's old life until it deploys on its "
			  "rotated team's kit, not its old team's pick; the bots respawn on theirs") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		std::unique_ptr<Match> o = start_match(*level, true, 31u, 1);
		Script *killer = o->bot();
		Script *target = o->bot();
		const int third = o->bot()->slot;
		tdm::ScriptedPeer &p = o->join("rotated");
		p.ready();
		REQUIRE(wait_in_round(*o));
		const uint8_t team1 = o->rules().team_of(uint8_t(p.slot()));
		REQUIRE((team1 == 1 || team1 == 2));
		const host::LoadoutKit *own_kit = gl.loadouts.kit(team1, 0);
		const host::LoadoutKit *other_kit = gl.loadouts.kit(uint8_t(3 - team1), 0);
		REQUIRE(own_kit != nullptr);
		REQUIRE(other_kit != nullptr);
		const host::LoadoutItem *shared = nullptr;
		for (const host::LoadoutItem &i : own_kit->slots[0]) {
			const bool on_both = std::any_of(other_kit->slots[0].begin(), other_kit->slots[0].end(),
					[&](const host::LoadoutItem &p_o) { return p_o.identifier == i.identifier; });
			if (shared == nullptr && on_both && i.def != gl.kits.loadout(team1, 0).defs[0] &&
					i.def != gl.kits.loadout(uint8_t(3 - team1), 0).defs[0]) {
				shared = &i;
			}
		}
		REQUIRE(shared != nullptr);
		net::LoadoutRequest pick;
		pick.kit = 0;
		pick.items[0] = shared->identifier;
		o->server.on_receive(p.peer(), net::kChannelReliable, net::encode_loadout_request(pick));
		o->ticks(1);
		p.spawn_request(0);
		REQUIRE(o->until([&] { return !p.spawned.empty() && is_alive(o->state(p.slot())); }, 240));
		const net::Spawned first = p.spawned.back();
		CHECK(first.defs[0] == shared->def);
		const uint64_t dropped0 = o->server.stats().gunplay.loadout_dropped;
		const std::array<int, 3> bots{ killer->slot, target->slot, third };
		o->ticks(130);
		const std::optional<Patch> patch = find_patch(*o, 3.0f);
		REQUIRE(patch.has_value());
		o->place(target->slot, add(patch->victim, 0.0f, 0.05f, 0.0f));
		o->place(killer->slot, add(patch->shooter, 0.0f, 0.05f, 0.0f));
		killer->fn = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::SoldierState *v = o->state(target->slot);
			const sim::Vec3 chest = v != nullptr ? add(v->position, 0.0f, 1.2f, 0.0f) : patch->victim;
			return aim_command(*o, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, (p_tick % 6u) < 3u));
		};
		REQUIRE(o->until([&] { return o->rules().phase() == net::MatchPhase::RoundOver; }, 900));
		killer->fn = nullptr;
		std::array<uint8_t, 3> bot_teams{};
		for (size_t i = 0; i < bots.size(); ++i) {
			bot_teams[i] = o->rules().team_of(uint8_t(bots[i]));
		}
		const std::vector<net::PlayerRow> round1(o->rules().players().begin(), o->rules().players().end());
		REQUIRE(o->until(
				[&] {
					const net::MatchPhase ph = o->rules().phase();
					return ph != net::MatchPhase::RoundOver && ph != net::MatchPhase::Restarting;
				},
				6000));
		const sim::Tick restart = o->now();
		o->ticks(30);
		size_t kept = 0;
		for (const net::PlayerRow &was : round1) {
			const net::PlayerRow *now = o->row(was.slot);
			REQUIRE(now != nullptr);
			CAPTURE(int(was.slot));
			CHECK(was.squad != 0);
			CHECK(now->team == uint8_t(3 - was.team));
			CHECK(now->squad == was.squad);
			CHECK(now->leader == was.leader);
			kept += now->squad == was.squad && now->leader == was.leader && was.squad != 0 ? 1u : 0u;
		}
		std::printf("restart: %zu of %zu players keep their squad and lead on the rotated team\n", kept, round1.size());
		REQUIRE(p.match_state.has_value());
		const bool old_unspawned = host::own_unspawned_at(true, *p.match_state, first.state.tick, o->now());
		std::printf("restart: at tick %u, unspawn_tick %u, the peer's old life (spawned at %u on team %u) %s, the "
					"server soldier %s\n",
				unsigned(restart), unsigned(p.match_state->unspawn_tick), unsigned(first.state.tick),
				unsigned(first.team), old_unspawned ? "unspawned" : "still alive on the client",
				is_alive(o->state(p.slot())) ? "alive" : "not spawned");
		CHECK(old_unspawned);
		CHECK_FALSE(is_alive(o->state(p.slot())));
		const host::Gunplay &g = *o->server.gunplay();
		size_t bots_checked = 0;
		for (size_t i = 0; i < bots.size(); ++i) {
			const uint8_t b = uint8_t(bots[i]);
			if (!is_alive(o->state(b))) {
				continue;
			}
			++bots_checked;
			CHECK(g.team(b) == uint8_t(3 - bot_teams[i]));
			CHECK(g.weapons(b).slots[0].def == gl.kits.loadout(g.team(b), g.kit(b)).defs[0]);
		}
		CHECK(bots_checked > 0);
		const size_t spawned0 = p.spawned.size();
		p.spawn_request(0);
		REQUIRE(o->until([&] { return p.spawned.size() > spawned0 && is_alive(o->state(p.slot())); }, 240));
		const net::Spawned next = p.spawned.back();
		const host::KitLoadout &kit = gl.kits.loadout(next.team, next.kit);
		std::printf("restart: the peer deployed on team %u (round 1: %u), defs %u %u %u, the kit's %u %u %u\n",
				unsigned(next.team), unsigned(first.team), unsigned(next.defs[0]), unsigned(next.defs[1]),
				unsigned(next.defs[2]), unsigned(kit.defs[0]), unsigned(kit.defs[1]), unsigned(kit.defs[2]));
		CHECK(next.team == uint8_t(3 - first.team));
		CHECK(next.team == o->rules().team_of(uint8_t(p.slot())));
		CHECK(g.weapons(uint8_t(p.slot())).slots[0].def == kit.defs[0]);
		CHECK(next.defs[0] == kit.defs[0]);
		std::printf("restart: the round-1 pick %s (def %u, on both teams' lists) %s on team %u\n",
				shared->unlock.c_str(), unsigned(shared->def),
				g.loadout(uint8_t(p.slot())).chosen ? "carried over" : "stays with its team", unsigned(next.team));
		CHECK_FALSE(g.loadout(uint8_t(p.slot())).chosen);
		CHECK(o->server.stats().gunplay.loadout_dropped == dropped0);
		CHECK_FALSE(host::own_unspawned_at(true, *p.match_state, next.state.tick, o->now()));
		CHECK(o->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("squads: 32 players make four squads of four per team with the first joiner leading; a full squad "
			  "refuses a join; kick, "
			  "leave and create; the longest-serving member takes over; SwitchTeam only keeps the teams within 1 (dev "
			  "commands off)") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::printf("squads: %s %s squad size %u\n", level->shared->level.c_str(), level->shared->mode.c_str(),
				unsigned(level->shared->squad_size));
		CHECK(level->shared->squad_size == 4);
		std::unique_ptr<Match> m = start_match(*level, false, 51u, 0, -1, squad_server(*level));
		tdm::ScriptedPeer &alpha = m->join("alpha");
		tdm::ScriptedPeer &bravo = m->join("bravo");
		tdm::ScriptedPeer &charlie = m->join("charlie");
		alpha.ready();
		bravo.ready();
		charlie.ready();
		for (int i = 0; i < 29; ++i) {
			m->bot();
		}
		m->ticks(net::kPlayerListIntervalTicks + 2u);
		REQUIRE(m->rules().players().size() == 32u);
		REQUIRE(m->rules().team_of(uint8_t(alpha.slot())) == 1);
		REQUIRE(m->rules().team_of(uint8_t(bravo.slot())) == 2);
		REQUIRE(m->rules().team_of(uint8_t(charlie.slot())) == 1);

		const SquadCensus server = squad_census(m->rules().players());
		const SquadCensus wire = squad_census(alpha.players);
		for (uint8_t t = 1; t <= 2; ++t) {
			std::printf("squads: team %u: %u players, squads 1-4 hold %u %u %u %u, no squad %u, leaders %d %d %d %d\n",
					unsigned(t), server.team[t], server.members[t][1], server.members[t][2], server.members[t][3],
					server.members[t][4], server.members[t][0], server.leader_slot[t][1], server.leader_slot[t][2],
					server.leader_slot[t][3], server.leader_slot[t][4]);
			CHECK(server.team[t] == 16u);
			CHECK(server.members[t][0] == 0u);
			for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
				CAPTURE(q);
				CHECK(server.members[t][q] == (q <= 4 ? 4u : 0u));
				CHECK(server.leaders[t][q] == (q <= 4 ? 1u : 0u));
				CHECK(wire.members[t][q] == server.members[t][q]);
				CHECK(wire.leader_slot[t][q] == server.leader_slot[t][q]);
			}
			for (uint8_t q = 1; q <= 4; ++q) {
				int first = -1;
				for (const net::PlayerRow &r : m->rules().players()) {
					if (r.team == t && r.squad == q && (first < 0 || int(r.slot) < first)) {
						first = int(r.slot);
					}
				}
				CHECK(server.leader_slot[t][q] == first);
			}
		}
		const auto squad_of = [&](const tdm::ScriptedPeer &p_peer) {
			return m->row(p_peer.slot()) != nullptr ? int(m->row(p_peer.slot())->squad) : -1;
		};
		const auto leads = [&](const tdm::ScriptedPeer &p_peer) {
			return m->row(p_peer.slot()) != nullptr && m->row(p_peer.slot())->leader;
		};
		REQUIRE(squad_of(alpha) == 1);
		REQUIRE(squad_of(charlie) == 1);
		if (squad_of(alpha) != 1 || squad_of(charlie) != 1) {
			return;
		}
		CHECK(leads(alpha));

		squad_request(*m, charlie, net::SquadOp::Join, 2);
		CHECK(squad_of(charlie) == 1);
		squad_request(*m, charlie, net::SquadOp::Kick, 0, uint8_t(alpha.slot()));
		CHECK(squad_of(alpha) == 1);
		CHECK(leads(alpha));
		squad_request(*m, alpha, net::SquadOp::Kick, 0, uint8_t(charlie.slot()));
		CHECK(squad_of(charlie) == 0);
		CHECK_FALSE(leads(charlie));
		squad_request(*m, charlie, net::SquadOp::Join, 1);
		CHECK(squad_of(charlie) == 1);
		CHECK_FALSE(leads(charlie));

		int heir = -1;
		for (const net::PlayerRow &r : m->rules().players()) {
			if (r.team == 1 && r.squad == 1 && int(r.slot) != alpha.slot() && int(r.slot) != charlie.slot() &&
					(heir < 0 || int(r.slot) < heir)) {
				heir = int(r.slot);
			}
		}
		REQUIRE(heir > charlie.slot());
		if (heir <= charlie.slot()) {
			return;
		}
		squad_request(*m, alpha, net::SquadOp::Leave);
		CHECK(squad_of(alpha) == 0);
		CHECK_FALSE(leads(alpha));
		CHECK_FALSE(leads(charlie));
		REQUIRE(m->row(heir) != nullptr);
		CHECK(m->row(heir)->leader);
		REQUIRE(seen_row(alpha, heir) != nullptr);
		CHECK(seen_row(alpha, heir)->leader);
		squad_request(*m, alpha, net::SquadOp::Create);
		CHECK(squad_of(alpha) == 5);
		CHECK(leads(alpha));

		squad_request(*m, bravo, net::SquadOp::SwitchTeam);
		CHECK(m->rules().team_of(uint8_t(bravo.slot())) == 2);
		CHECK(squad_census(m->rules().players()).team[1] == 16u);
		m->server.on_receive(charlie.peer(), net::kChannelReliable, net::encode_bye());
		m->ticks(net::kPlayerListIntervalTicks + 2u);
		CHECK(squad_census(m->rules().players()).members[1][1] == 2u);
		REQUIRE(m->row(heir) != nullptr);
		CHECK(m->row(heir)->leader);
		squad_request(*m, bravo, net::SquadOp::SwitchTeam);
		CHECK(m->rules().team_of(uint8_t(bravo.slot())) == 1);
		CHECK(squad_of(bravo) == 1);
		CHECK_FALSE(leads(bravo));
		const SquadCensus after = squad_census(m->rules().players());
		CHECK(after.team[1] == 16u);
		CHECK(after.team[2] == 15u);
		CHECK(m->until(
				[&] { return is_alive(m->state(bravo.slot())) && m->server.view().team(uint8_t(bravo.slot())) == 1; },
				600));
		const net::PlayerRow *bravo_seen = seen_row(alpha, bravo.slot());
		REQUIRE(bravo_seen != nullptr);
		CHECK(bravo_seen->team == 1);
		CHECK(bravo_seen->squad == 1);
		std::printf("squads: after the requests alpha leads squad %d, slot %d took over squad 1, bravo switched to "
					"team %u squad %u (teams %u / %u)\n",
				squad_of(alpha), heir, unsigned(bravo_seen->team), unsigned(bravo_seen->squad), after.team[1],
				after.team[2]);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("squads: a SpawnRequest on a living squad mate deploys at the mate's nav position and scores SPAWN_SQUAD "
			  "for the mate; the "
			  "requester itself, a missing index and a dead mate are refused") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		REQUIRE(level->bots->nav != nullptr);
		REQUIRE(level->shared->scoring != nullptr);
		CHECK(level->shared->scoring->score("ID_SCORE_SPAWN_SQUAD") == std::optional<double>(10.0));
		std::unique_ptr<Match> m = start_match(*level, true, 53u, 0, -1, squad_server(*level));
		tdm::ScriptedPeer &alpha = m->join("alpha");
		tdm::ScriptedPeer &bravo = m->join("bravo");
		tdm::ScriptedPeer &charlie = m->join("charlie");
		alpha.ready();
		bravo.ready();
		charlie.ready();
		m->bot();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->rules().team_of(uint8_t(alpha.slot())) == 1);
		REQUIRE(m->rules().team_of(uint8_t(charlie.slot())) == 1);
		charlie.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(charlie.slot())); }, 240));
		m->ticks(net::kPlayerListIntervalTicks + 2u);
		const int mate_index = squad_index(alpha, alpha.slot(), charlie.slot());
		const int self_index = squad_index(alpha, alpha.slot(), alpha.slot());
		std::printf("squad spawn: alpha slot %d squad %d, charlie slot %d at index %d, alpha at index %d\n",
				alpha.slot(), seen_row(alpha, alpha.slot()) != nullptr ? int(seen_row(alpha, alpha.slot())->squad) : -1,
				charlie.slot(), mate_index, self_index);
		CHECK(self_index == 0);
		CHECK(mate_index == 1);
		const uint8_t self_choice = 1, mate_choice = 2;
		REQUIRE(m->row(charlie.slot()) != nullptr);
		const int32_t mate_score = m->row(charlie.slot())->score;

		alpha.spawn_request(0, self_choice);
		m->ticks(2);
		alpha.spawn_request(0, uint8_t(1 + 4));
		m->ticks(2);
		CHECK_FALSE(is_alive(m->state(alpha.slot())));
		CHECK(alpha.spawned.empty());

		core::Result<std::unique_ptr<sim::NavQuery>> nav = sim::NavQuery::create(level->bots->nav);
		REQUIRE_MESSAGE(nav.ok(), (nav.ok() ? std::string() : nav.error().message));
		const sim::Vec3 mate_at = m->state(charlie.slot())->position;
		sim::NavPolyRef poly = 0;
		sim::Vec3 nav_at{};
		REQUIRE((*nav)->nearest(mate_at, sim::Vec3{ 1.0f, 2.0f, 1.0f }, &poly, &nav_at));
		alpha.spawn_request(0, mate_choice);
		REQUIRE(m->until([&] { return is_alive(m->state(alpha.slot())); }, 10));
		REQUIRE(alpha.spawned.size() == 1u);
		if (alpha.spawned.size() != 1u) {
			return;
		}
		const sim::Vec3 deployed = alpha.spawned.back().state.position;
		std::printf("squad spawn: the mate at (%.2f %.2f %.2f), its nav point (%.2f %.2f %.2f), alpha deployed at "
					"(%.2f %.2f %.2f): %.3f m from the nav "
					"point\n",
				mate_at[0], mate_at[1], mate_at[2], nav_at[0], nav_at[1], nav_at[2], deployed[0], deployed[1],
				deployed[2], distance(deployed, nav_at));
		CHECK(distance(deployed, nav_at) <= 1.0f);
		m->ticks(net::kPlayerListIntervalTicks + 2u);
		REQUIRE(m->row(charlie.slot()) != nullptr);
		CHECK(m->row(charlie.slot())->score == mate_score + 10);
		REQUIRE(seen_row(alpha, charlie.slot()) != nullptr);
		CHECK(seen_row(alpha, charlie.slot())->score == mate_score + 10);

		const std::vector<uint8_t> suicide = net::encode_suicide_request();
		m->server.on_receive(charlie.peer(), net::kChannelReliable, suicide);
		m->server.on_receive(alpha.peer(), net::kChannelReliable, suicide);
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(charlie.slot())));
		REQUIRE_FALSE(is_alive(m->state(alpha.slot())));
		const uint32_t earliest = m->rules().earliest_respawn(uint8_t(alpha.slot()));
		REQUIRE(m->until([&] { return m->now() + 1u >= earliest; }, 900));
		m->ticks(2);
		alpha.spawn_request(0, mate_choice);
		m->ticks(3);
		CHECK_FALSE(is_alive(m->state(alpha.slot())));
		CHECK(alpha.spawned.size() == 1u);
		alpha.spawn_request(0, 0);
		CHECK(m->until([&] { return is_alive(m->state(alpha.slot())); }, 10));
		CHECK(alpha.spawned.size() == 2u);
		const host::RulesStats &st = m->rules().stats();
		std::printf("squad spawn: %llu squad spawns (%llu on the nav mesh), %llu refused for the mate\n",
				(unsigned long long)st.squad_spawns, (unsigned long long)st.squad_spawns_on_nav,
				(unsigned long long)st.refused[size_t(host::SpawnRefusal::SquadMate)]);
		CHECK(st.squad_spawns == 1u);
		CHECK(st.squad_spawns_on_nav == 1u);
		CHECK(st.refused[size_t(host::SpawnRefusal::SquadMate)] == 3u);
		CHECK(st.runtime_faults == 0u);
	}

	TEST_CASE("squads: the kill that leaves an enemy squad of two with no living member scores SQUAD_WIPE; a kill with "
			  "a living mate does "
			  "not") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->shared->scoring != nullptr);
		CHECK(level->shared->scoring->score("ID_SCORE_SQUAD_WIPE") == std::optional<double>(10.0));
		const int32_t kill_score = level->shared->tdm_tuning->score_kill;
		const int32_t headshot_score = level->shared->tdm_tuning->score_headshot;
		std::unique_ptr<Match> m = start_match(*level, true, 57u, 0, -1, squad_server(*level));
		tdm::ScriptedPeer &lone = m->join("lone");
		lone.ready();
		Script *killer = m->bot();
		Script *target = m->bot();
		m->bot();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->rules().team_of(uint8_t(lone.slot())) == 1);
		REQUIRE(m->rules().team_of(uint8_t(target->slot)) == 1);
		REQUIRE(m->rules().team_of(uint8_t(killer->slot)) == 2);
		lone.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(lone.slot())); }, 240));
		m->ticks(net::kPlayerListIntervalTicks + 2u);
		REQUIRE(m->row(lone.slot()) != nullptr);
		REQUIRE(m->row(target->slot) != nullptr);
		CHECK(m->row(lone.slot())->squad != 0);
		CHECK(m->row(lone.slot())->squad == m->row(target->slot)->squad);
		const std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		bool fire = false;
		killer->fn = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::SoldierState *v = m->state(target->slot);
			const sim::Vec3 chest = v != nullptr ? add(v->position, 0.0f, 1.2f, 0.0f) : patch->victim;
			return aim_command(
					*m, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, fire && (p_tick % 6u) < 3u));
		};
		const auto kill_target = [&](int32_t &r_delta, bool &r_headshot) {
			if (!m->until([&] { return is_alive(m->state(target->slot)); }, 1200)) {
				return false;
			}
			m->place(target->slot, add(patch->victim, 0.0f, 0.05f, 0.0f));
			m->place(killer->slot, add(patch->shooter, 0.0f, 0.05f, 0.0f));
			m->ticks(30);
			const int32_t before = m->row(killer->slot)->score;
			fire = true;
			const bool died = m->until([&] { return !is_alive(m->state(target->slot)); }, 900);
			fire = false;
			m->ticks(2);
			const net::DeathEvent *d = last_death(lone, target->slot);
			r_delta = m->row(killer->slot)->score - before;
			r_headshot = d != nullptr && d->headshot;
			return died && d != nullptr && int(d->killer) == killer->slot;
		};
		int32_t delta = 0;
		bool headshot = false;
		REQUIRE(kill_target(delta, headshot));
		std::printf("squad wipe: a kill while the mate lives scores %d (headshot %d)\n", int(delta), int(headshot));
		CHECK(delta == kill_score + (headshot ? headshot_score : 0));
		m->server.on_receive(lone.peer(), net::kChannelReliable, net::encode_suicide_request());
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(lone.slot())));
		REQUIRE(kill_target(delta, headshot));
		std::printf(
				"squad wipe: the kill of the last living member scores %d (headshot %d)\n", int(delta), int(headshot));
		CHECK(delta == kill_score + (headshot ? headshot_score : 0) + 10);
		CHECK(m->rules().stats().squad_wipes == 1u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("squads: a squad order goes to its team's peers as a SquadOrder event, a late joiner of that team gets "
			  "the standing order, "
			  "the other team gets none; an empty squad takes no order") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, false, 59u, 0, -1, squad_server(*level));
		tdm::ScriptedPeer &alpha = m->join("alpha");
		tdm::ScriptedPeer &bravo = m->join("bravo");
		alpha.ready();
		bravo.ready();
		m->bot();
		m->bot();
		m->ticks(2);
		REQUIRE(m->rules().team_of(uint8_t(alpha.slot())) == 1);
		REQUIRE(m->rules().team_of(uint8_t(bravo.slot())) == 2);
		host::SquadOrderState attack;
		attack.kind = net::SquadOrderKind::Attack;
		attack.area = 3;
		attack.position = { 12.5f, 1.0f, -40.25f };
		CHECK_FALSE(m->server.set_squad_order(1, 7, attack));
		CHECK_FALSE(m->server.set_squad_order(3, 1, attack));
		host::SquadOrderState bad = attack;
		bad.position[0] = std::nanf("");
		CHECK_FALSE(m->server.set_squad_order(1, 1, bad));
		REQUIRE(m->server.set_squad_order(1, 1, attack));
		m->ticks(2);
		REQUIRE(alpha.squad_orders.size() == 1u);
		const net::SquadOrder &got = alpha.squad_orders.back();
		CHECK(got.team == 1);
		CHECK(got.squad == 1);
		CHECK(got.kind == net::SquadOrderKind::Attack);
		CHECK(got.area == 3);
		CHECK(got.position == attack.position);
		CHECK(bravo.squad_orders.empty());
		m->ticks(30);
		CHECK(alpha.squad_orders.size() == 1u);

		tdm::ScriptedPeer &charlie = m->join("charlie");
		REQUIRE(m->rules().team_of(uint8_t(charlie.slot())) == 1);
		REQUIRE(charlie.squad_orders.size() == 1u);
		CHECK(charlie.squad_orders.back().kind == net::SquadOrderKind::Attack);
		CHECK(charlie.squad_orders.back().squad == 1);

		REQUIRE(m->server.set_squad_order(1, 1, host::SquadOrderState()));
		m->ticks(2);
		REQUIRE(alpha.squad_orders.size() == 2u);
		CHECK(alpha.squad_orders.back().kind == net::SquadOrderKind::None);
		CHECK(alpha.squad_orders.back().area == net::kNoSquadArea);
		CHECK(bravo.squad_orders.empty());
		std::printf("squad orders: alpha %zu, bravo %zu, the late joiner %zu\n", alpha.squad_orders.size(),
				bravo.squad_orders.size(), charlie.squad_orders.size());
	}

	TEST_CASE("install unchanged") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		tdm::check_install_unchanged(*level);
	}
}

namespace {

const sw::GadgetDef *gadget_named(const host::GunplayLevel &p_level, std::string_view p_weapon) {
	for (const sw::GadgetDef &g : p_level.gadgets) {
		if (g.weapon == p_weapon) {
			return &g;
		}
	}
	return nullptr;
}

std::optional<sim::Tick> rest_tick(Match &r_match, size_t p_count, uint32_t p_max) {
	for (uint32_t i = 0; i < p_max; ++i) {
		size_t resting = 0;
		for (const sw::Deployable &d : r_match.server.gunplay()->deployables()) {
			resting += d.resting ? 1u : 0u;
		}
		if (resting >= p_count) {
			return r_match.now();
		}
		r_match.tick();
	}
	return std::nullopt;
}

std::function<void(host::GameServerConfig &)> gadget_bot_server(
		const tdm::TdmLevel &p_level, uint8_t p_kit, std::shared_ptr<host::TdmBotStats> p_stats, bool p_squads = true) {
	return [&p_level, p_kit, p_stats, p_squads](host::GameServerConfig &r_cfg) {
		auto shared = std::make_shared<host::SharedLevel>(*p_level.shared);
		auto tuning = p_level.shared->tdm_tuning != nullptr
				? std::make_shared<host::TdmTuning>(*p_level.shared->tdm_tuning)
				: std::make_shared<host::TdmTuning>();
		tuning->bot_kit_weights = {};
		tuning->bot_kit_weights[p_kit] = 1;
		shared->tdm_tuning = tuning;
		r_cfg.level = shared;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = p_level.bots;
		fill->count = 0;
		r_cfg.bots = fill;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bf3::test::use_shipped_bot_ai(*bots);
		bots->count = 1;
		bots->stats = p_stats;
		bots->squads = p_squads;
		r_cfg.tdm_bots = bots;
	};
}

struct GadgetBotMatch {
	int bot = -1;
	tdm::ScriptedPeer *mate = nullptr;
	size_t mate_script = 0;
};

GadgetBotMatch gadget_bot_match(Match &r_match) {
	GadgetBotMatch g;
	for (const char *name : { "alpha", "bravo", "charlie" }) {
		r_match.join(name).ready();
	}
	REQUIRE(wait_in_round(r_match));
	for (const net::PlayerRow &r : r_match.rules().players()) {
		if (r.bot) {
			g.bot = int(r.slot);
		}
	}
	REQUIRE(g.bot >= 0);
	if (g.bot < 0) {
		return g;
	}
	const uint8_t team = r_match.rules().team_of(uint8_t(g.bot));
	tdm::ScriptedPeer *mate = nullptr;
	for (size_t i = 0; i < r_match.peers.size() && mate == nullptr; ++i) {
		if (r_match.rules().team_of(uint8_t(r_match.peers[i]->slot())) == team) {
			mate = r_match.peers[i].get();
			g.mate_script = i;
		}
	}
	REQUIRE(mate != nullptr);
	if (mate == nullptr) {
		return g;
	}
	mate->spawn_request(0);
	const bool alive =
			r_match.until([&] { return is_alive(r_match.state(g.bot)) && is_alive(r_match.state(mate->slot())); }, 900);
	REQUIRE(alive);
	g.mate = alive ? mate : nullptr;
	return g;
}

int supply_slot_of(const Match &p_match, int p_slot, bool p_healing) {
	const host::Gunplay &gp = *p_match.server.gunplay();
	const sw::SoldierWeaponsState &w = gp.weapons(uint8_t(p_slot));
	for (const uint8_t s : host::kGadgetSlots) {
		for (const sw::GadgetDef &d : gp.level().gadgets) {
			if (d.def == w.slots[s].def && (p_healing ? d.healing : d.ammo).active()) {
				return s;
			}
		}
	}
	return -1;
}

std::optional<sim::Vec3> flat_lane(const Match &p_match, std::array<float, 2> &r_face) {
	for (size_t s = 0; s < p_match.level->spawns.size(); ++s) {
		const std::optional<sim::Vec3> p = floor_at(*p_match.world, p_match.level->spawns[s].position);
		for (int k = 0; k < 8 && p; ++k) {
			const float a = float(k) * sim::kTwoPi / 8.0f;
			const std::array<float, 2> f{ std::sin(a), std::cos(a) };
			bool lane = true;
			for (float d = -5.0f; d <= 9.0f && lane; d += 0.5f) {
				const std::optional<sim::Vec3> q = floor_at(*p_match.world, add(*p, d * f[0], 0.0f, d * f[1]));
				lane = q && std::fabs((*q)[1] - (*p)[1]) < 0.15f &&
						p_match.motor.fits_standing(add(*q, 0.0f, 0.05f, 0.0f)) &&
						clear(*p_match.world, add(*p, 0.0f, 1.0f, 0.0f), add(*q, 0.0f, 1.0f, 0.0f));
			}
			if (lane) {
				r_face = f;
				return p;
			}
		}
	}
	return std::nullopt;
}

const sw::Deployable *resting_bag(const Match &p_match, int p_owner, bool p_healing) {
	const host::Gunplay &gp = *p_match.server.gunplay();
	const std::span<const sw::GadgetDef> defs = gp.level().gadgets;
	for (const sw::Deployable &d : gp.deployables()) {
		if (int(d.owner) == p_owner && d.resting && d.gadget < defs.size() &&
				(p_healing ? defs[d.gadget].healing : defs[d.gadget].ammo).active()) {
			return &d;
		}
	}
	return nullptr;
}

std::optional<sim::Vec3> ring_spot(const Match &p_match, const sim::Vec3 &p_centre, float p_min, float p_max,
		const std::optional<sim::Vec3> &p_apart) {
	for (float r = p_min; r <= p_max; r += 1.0f) {
		for (int k = 0; k < 16; ++k) {
			const float a = float(k) * sim::kTwoPi / 16.0f;
			const std::optional<sim::Vec3> f =
					floor_at(*p_match.world, add(p_centre, r * std::cos(a), 0.0f, r * std::sin(a)));
			if (f && std::fabs((*f)[1] - p_centre[1]) < 0.3f &&
					p_match.motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f)) &&
					clear(*p_match.world, add(p_centre, 0.0f, 1.2f, 0.0f), add(*f, 0.0f, 1.2f, 0.0f)) &&
					(!p_apart || core::vec::horizontal_distance(*f, *p_apart) >= 8.0f)) {
				return f;
			}
		}
	}
	return std::nullopt;
}

}

TEST_SUITE("host-gunplay-gadgets") {
	TEST_CASE("gadgets: Metro TDM's medkit, ammo bag and radio beacon carry the install's supply and deploy data and "
			  "are the "
			  "kits' default slot 4 and 5 weapons; C4, claymore and M15 carry their ExplosionPackEntityData triggers") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &g = *level->shared->gunplay;
		std::printf("gadgets: %zu gadget defs, %zu ammo pickups for %zu weapon defs\n", g.gadgets.size(),
				g.ammo_pickups.size(), g.defs.size());
		CHECK(g.gadgets.size() == 9u);
		CHECK(g.ammo_pickups.size() == g.defs.size());
		const sw::GadgetDef *c4 = gadget_named(g, "weapons/gadgets/c4/c4");
		const sw::GadgetDef *claymore = gadget_named(g, "weapons/gadgets/claymore/claymore");
		const sw::GadgetDef *m15 = gadget_named(g, "weapons/gadgets/m15/m15");
		REQUIRE(c4 != nullptr);
		REQUIRE(claymore != nullptr);
		REQUIRE(m15 != nullptr);
		for (const sw::GadgetDef *x : { c4, claymore, m15 }) {
			CHECK(x->kind == sw::GadgetKind::ExplosionPack);
			CHECK(x->destructible);
			CHECK(x->receives_explosion_damage);
			CHECK(x->time_to_live_s == 0.0f);
		}
		CHECK(c4->pack.remote_detonator);
		CHECK_FALSE(c4->pack.vehicle_detonation);
		CHECK_FALSE(c4->pack.soldier_detonation);
		CHECK(c4->max_count == 6u);
		CHECK(c4->max_attach_inclination_deg == 180.0f);
		CHECK(c4->health == 15.0f);
		CHECK(c4->time_to_live_on_owner_gone_s == 5.0f);
		CHECK_FALSE(claymore->pack.remote_detonator);
		CHECK(claymore->pack.soldier_detonation);
		CHECK(claymore->pack.use_angle);
		CHECK(claymore->pack.soldier_angle_deg == 60.0f);
		CHECK(claymore->pack.soldier_radius == 1.5f);
		CHECK(claymore->pack.soldier_min_speed == doctest::Approx(3.8f));
		CHECK(claymore->pack.soldier_delay_s == 3.0f);
		CHECK(claymore->pack.vehicle_detonation);
		CHECK(claymore->pack.vehicle_radius == 3.0f);
		CHECK(claymore->pack.vehicle_delay_s == 4.0f);
		CHECK(claymore->max_count == 2u);
		CHECK(claymore->max_attach_inclination_deg == 45.0f);
		CHECK(claymore->time_to_live_on_owner_gone_s == 1.0f);
		CHECK_FALSE(m15->pack.soldier_detonation);
		CHECK(m15->pack.vehicle_detonation);
		CHECK(m15->pack.vehicle_radius == 3.0f);
		CHECK(m15->pack.vehicle_delay_s == 4.0f);
		CHECK(m15->max_count == 6u);
		CHECK(m15->max_attach_inclination_deg == 15.0f);
		CHECK(m15->time_to_live_on_owner_gone_s == 0.0f);
		const sw::GadgetDef *medkit = gadget_named(g, "weapons/gadgets/medicbag/medicbag");
		const sw::GadgetDef *ammo = gadget_named(g, "weapons/gadgets/ammobag/ammobag");
		const sw::GadgetDef *beacon = gadget_named(g, "weapons/gadgets/radiobeacon/radio_beacon");
		REQUIRE(medkit != nullptr);
		REQUIRE(ammo != nullptr);
		REQUIRE(beacon != nullptr);
		CHECK(medkit->kind == sw::GadgetKind::Supply);
		CHECK(medkit->healing == sw::SupplyUnitDef{ 3.5f, 12.0f, 1.0f, 0.0f, true });
		CHECK_FALSE(medkit->ammo.active());
		CHECK(ammo->ammo == sw::SupplyUnitDef{ 3.5f, 1.0f, 7.0f, 0.0f, true });
		CHECK_FALSE(ammo->healing.active());
		for (const sw::GadgetDef *s : { medkit, ammo }) {
			CHECK(s->max_count == 1u);
			CHECK(s->max_attach_inclination_deg == 25.0f);
			CHECK_FALSE(s->team_specific);
			CHECK_FALSE(s->exclude_self);
			CHECK(s->supply_soldiers);
			CHECK(s->health == 50.0f);
			CHECK(s->destructible);
			CHECK(s->receives_explosion_damage);
			CHECK(s->time_to_live_s == 0.0f);
			CHECK(s->time_to_live_on_owner_gone_s == 0.0f);
		}
		CHECK(beacon->kind == sw::GadgetKind::SpawnBeacon);
		CHECK(beacon->ground_ray_m == doctest::Approx(3.1f));
		CHECK(beacon->ground_flatness == doctest::Approx(0.5f));
		CHECK(beacon->max_count == 1u);
		REQUIRE(medkit->def < g.ammo_pickups.size());
		REQUIRE(beacon->def < g.ammo_pickups.size());
		CHECK(g.ammo_pickups[medkit->def] == sw::AmmoPickupDef{ -1, 1.0f, true, 1.0f });
		CHECK(g.ammo_pickups[beacon->def] == sw::AmmoPickupDef{ -1, 2.5f, false, 5.0f });
		std::string replenished;
		size_t replenishing = 0;
		for (size_t d = 0; d < g.ammo_pickups.size(); ++d) {
			if (g.ammo_pickups[d].auto_replenish) {
				++replenishing;
				replenished += " " + g.defs[d].variant;
			}
		}
		std::printf("gadgets: %zu auto-replenishing defs:%s\n", replenishing, replenished.c_str());
		CHECK(replenishing == 2u);
		for (uint8_t team = 1; team <= 2; ++team) {
			CHECK(host::default_loadout(g.loadouts, g.kits, team, 0).defs[4] == medkit->def);
			CHECK(host::default_loadout(g.loadouts, g.kits, team, 2).defs[4] == ammo->def);
			CHECK(host::default_loadout(g.loadouts, g.kits, team, 3).defs[5] == beacon->def);
			const host::KitLoadout &engineer = g.kits.loadout(team, 1);
			CHECK(engineer.unlocks[5] == "weapons/gadgets/repairtool/u_repairtool");
			CHECK(engineer.defs[5] != sw::kNoWeapon);
			CHECK(host::default_loadout(g.loadouts, g.kits, team, 1).defs[5] == engineer.defs[5]);
			CHECK(std::none_of(g.gadgets.begin(), g.gadgets.end(),
					[&](const sw::GadgetDef &p_gadget) { return p_gadget.def == engineer.defs[5]; }));
			CHECK(g.kits.loadout(team, 3).defs[5] == beacon->def);
		}
	}

	TEST_CASE("gadgets: the recon's SOFLAM, T-UGS and MAV are placed vehicles; the SOFLAM carries its LaserPainterData "
			  "controller and the T-UGS and MAV their RadarSweepComponentData") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &g = *level->shared->gunplay;
		const sw::GadgetDef *soflam = gadget_named(g, "weapons/gadgets/soflam/soflam_pda");
		const sw::GadgetDef *tugs = gadget_named(g, "weapons/gadgets/t-ugs/t-ugs");
		const sw::GadgetDef *mav = gadget_named(g, "weapons/gadgets/mav/mav_pda");
		REQUIRE(soflam != nullptr);
		REQUIRE(tugs != nullptr);
		REQUIRE(mav != nullptr);
		CHECK(soflam->kind == sw::GadgetKind::LaserDesignator);
		CHECK(tugs->kind == sw::GadgetKind::RadarSweep);
		CHECK(mav->kind == sw::GadgetKind::RadarSweep);
		for (const sw::GadgetDef *p : { soflam, tugs, mav }) {
			CHECK(sw::placed_on_ground(p->kind));
			CHECK(p->ground_ray_m == doctest::Approx(3.1f));
			CHECK(p->ground_flatness == doctest::Approx(0.5f));
			CHECK(p->max_count == 1u);
			REQUIRE(p->def < g.defs.size());
		}
		const sw::LockingControllerSimDef &laser = soflam->painter.controller;
		REQUIRE(laser.present);
		CHECK(laser.lock_type(0) == core::gameplay::LockType::Always);
		CHECK(laser.zoom_lock_count == 1u);
		CHECK(laser.lock_time == sw::seconds_to_units(2.0));
		CHECK(laser.release_time == sw::seconds_to_units(1.0));
		CHECK(laser.release_on_new_target_time == sw::seconds_to_units(1.5));
		CHECK(laser.sample_rate == sw::seconds_to_units(0.06));
		CHECK(laser.ray_length == 500.0f);
		CHECK(laser.acceptance_angle_deg == 15.0f);
		CHECK(laser.lock_on_empty_vehicles);
		CHECK(laser.visible_only);
		CHECK(soflam->painter.painted_time == sw::seconds_to_units(0.5));
		std::printf("soflam: laser %.3f %.3f %.3f m from the placed origin\n", double(soflam->painter.origin[0]),
				double(soflam->painter.origin[1]), double(soflam->painter.origin[2]));
		CHECK(soflam->painter.origin[1] > 0.4f);
		CHECK(soflam->painter.origin[1] < 0.7f);
		CHECK(tugs->sweep.radius == 30.0f);
		CHECK(tugs->sweep.interval == sw::seconds_to_units(2.0));
		CHECK(tugs->sweep.speed_threshold == doctest::Approx(2.05f));
		CHECK(mav->sweep.radius == 30.0f);
		CHECK(mav->sweep.interval == sw::seconds_to_units(3.0));
		CHECK(mav->sweep.speed_threshold == doctest::Approx(2.05f));
		for (uint8_t team = 1; team <= host::kKitTeams; ++team) {
			const host::LoadoutKit *recon = g.loadouts.kit(team, uint8_t(host::KitRole::Recon));
			REQUIRE(recon != nullptr);
			const size_t gadget1 = size_t(core::gameplay::WeaponSlot::Gadget1);
			for (const sw::GadgetDef *p : { soflam, tugs, mav }) {
				CHECK(std::any_of(recon->slots[gadget1].begin(), recon->slots[gadget1].end(),
						[&](const host::LoadoutItem &i) { return i.def == p->def; }));
			}
		}
	}

	TEST_CASE("gadgets: a thrown medkit heals a fallen soldier at its SupplyIncSpeed of 12 HP/s before regeneration "
			  "starts") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 71u, 0, 1);
		tdm::ScriptedPeer &medic = m->join("medic");
		tdm::ScriptedPeer &patient = m->join("patient");
		medic.ready();
		patient.ready();
		REQUIRE(wait_in_round(*m));
		medic.spawn_request(0);
		patient.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(medic.slot())) && is_alive(m->state(patient.slot())); }, 600));
		const std::optional<Drop> drop = find_drop(*m, false);
		REQUIRE(drop.has_value());
		m->teleport(medic, add(drop->beside, 0.0f, 0.05f, 0.0f));
		m->teleport(patient, add(drop->floor, 0.0f, drop->height, 0.0f));
		REQUIRE(m->until(
				[&] {
					const sim::SoldierState *s = m->state(patient.slot());
					return s != nullptr && s->health < 99.0f && s->mode == sim::MoveMode::OnGround;
				},
				600));
		m->ticks(5);
		REQUIRE(is_alive(m->state(patient.slot())));
		const float hurt = m->state(patient.slot())->health;
		const uint16_t medkit = m->server.gunplay()->weapons(uint8_t(medic.slot())).slots[4].def;
		std::printf(
				"medkit: patient at %.1f HP after the fall, the medic's slot 4 holds def %u\n", hurt, unsigned(medkit));
		CHECK(medkit != sw::kNoWeapon);
		bool fire = false;
		const sim::Vec3 target = add(drop->floor, 0.0f, 0.1f, 0.0f);
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(
					*m, p_tick, p_state, target, sim::Pose::Stand, select_and_fire(4, fire && (p_tick % 30u) < 10u));
		};
		m->ticks(40);
		fire = true;
		const std::optional<sim::Tick> rested = rest_tick(*m, 1, 300);
		fire = false;
		REQUIRE(rested.has_value());
		const float at_rest = m->state(patient.slot())->health;
		const uint32_t window = 90;
		m->ticks(window);
		const float after = m->state(patient.slot())->health;
		const host::GadgetStats &st = m->server.gunplay()->gadget_stats();
		std::printf("medkit: rested at tick %u, health %.2f -> %.2f over %u ticks (%.2f HP/s), %llu deploys, %llu heal "
					"ticks\n",
				unsigned(*rested), at_rest, after, window, (after - at_rest) * 60.0f / float(window),
				(unsigned long long)st.deploys, (unsigned long long)st.heal_ticks);
		CHECK(after - at_rest == doctest::Approx(12.0f * float(window) / 60.0f).epsilon(0.01));
		CHECK(st.deploys >= 1u);
		CHECK(m->rules().stats().regen_ticks == 0u);
	}

	TEST_CASE("gadgets: an ammo bag refills a magazine of the primary's reserve per second, up to the spawn reserve") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 73u, 0, 1);
		tdm::ScriptedPeer &support = m->join("support");
		tdm::ScriptedPeer &rifle = m->join("rifle");
		support.ready();
		rifle.ready();
		REQUIRE(wait_in_round(*m));
		support.spawn_request(2);
		rifle.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(support.slot())) && is_alive(m->state(rifle.slot())); }, 600));
		std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		m->teleport(support, add(patch->medic, 0.0f, 0.05f, 0.0f));
		m->teleport(rifle, add(patch->victim, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		const sw::WeaponSlotState &primary = m->server.gunplay()->weapons(uint8_t(rifle.slot())).slots[0];
		const int32_t full = primary.reserve;
		const int32_t magazine = m->server.gunplay()->table().def(primary.def, primary.variant).magazine_capacity;
		bool shoot = true;
		m->scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::Vec3 sky = add(p_state.position, 0.0f, 30.0f, 1.0f);
			return aim_command(
					*m, p_tick, p_state, sky, sim::Pose::Stand, select_and_fire(0, shoot && (p_tick % 4u) < 2u));
		};
		REQUIRE(m->until([&] { return primary.reserve <= full - magazine; }, 1200));
		shoot = false;
		m->ticks(240);
		const int32_t low = primary.reserve;
		REQUIRE(low < full);
		CHECK(m->server.gunplay()->weapons(uint8_t(support.slot())).slots[4].def != sw::kNoWeapon);
		bool fire = false;
		const sim::Vec3 target = add(patch->victim, 0.0f, 0.1f, 0.0f);
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(
					*m, p_tick, p_state, target, sim::Pose::Stand, select_and_fire(4, fire && (p_tick % 30u) < 10u));
		};
		m->ticks(40);
		fire = true;
		const std::optional<sim::Tick> rested = rest_tick(*m, 1, 300);
		fire = false;
		REQUIRE(rested.has_value());
		const int32_t at_rest = primary.reserve;
		sim::Tick first_refill = 0;
		REQUIRE(m->until(
				[&] {
					if (first_refill == 0 && primary.reserve > at_rest) {
						first_refill = m->now();
					}
					return primary.reserve >= full;
				},
				600));
		std::printf("ammo bag: reserve %d of %d (magazine %d) before the bag, the first refill %u ticks after it "
					"rested, full at %u\n",
				low, full, magazine, unsigned(first_refill - *rested), unsigned(m->now() - *rested));
		CHECK(at_rest == low);
		CHECK(first_refill - *rested >= 58u);
		CHECK(first_refill - *rested <= 62u);
		CHECK(primary.reserve == full);
		CHECK(m->server.gunplay()->gadget_stats().rounds_refilled >= uint64_t(full - low));
	}

	TEST_CASE("gadgets: an enemy grenade dropped on a medkit destroys it (Health 50, ReceivesExplosionDamage); "
			  "it stays until then") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 75u, 0, 1);
		tdm::ScriptedPeer &medic = m->join("medic");
		tdm::ScriptedPeer &enemy = m->join("enemy");
		medic.ready();
		enemy.ready();
		REQUIRE(wait_in_round(*m));
		medic.spawn_request(0);
		enemy.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(medic.slot())) && is_alive(m->state(enemy.slot())); }, 600));
		REQUIRE(m->rules().team_of(uint8_t(medic.slot())) != m->rules().team_of(uint8_t(enemy.slot())));
		std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		m->teleport(medic, add(patch->shooter, 0.0f, 0.05f, 0.0f));
		m->teleport(enemy, add(patch->victim, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		bool fire = false;
		const sim::Vec3 target = add(patch->victim, 0.0f, 0.1f, 0.0f);
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(
					*m, p_tick, p_state, target, sim::Pose::Stand, select_and_fire(4, fire && (p_tick % 30u) < 10u));
		};
		m->ticks(40);
		fire = true;
		const std::optional<sim::Tick> rested = rest_tick(*m, 1, 300);
		fire = false;
		REQUIRE(rested.has_value());
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		const sim::Vec3 bag = m->server.gunplay()->deployables()[0].position;
		m->ticks(600);
		CHECK(m->server.gunplay()->deployables().size() == 1u);
		bool pull = false;
		const sim::Vec3 feet = add(bag, 0.0f, 0.0f, 0.3f);
		m->scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, feet, sim::Pose::Stand, select_and_fire(6, pull));
		};
		m->ticks(60);
		pull = true;
		m->ticks(30);
		pull = false;
		size_t detonations = 0;
		REQUIRE(m->until(
				[&] {
					detonations = 0;
					for (const net::CombatEvents &c : enemy.combat) {
						detonations += c.detonations.size();
					}
					return detonations > 0u;
				},
				600));
		m->ticks(2);
		const sw::GadgetFieldStats &st = m->server.gunplay()->gadget_stats().field;
		std::printf("grenade: %zu detonation(s) by the medkit at (%.1f, %.1f, %.1f); %zu deployables left, %llu "
					"damaged, %llu destroyed\n",
				detonations, bag[0], bag[1], bag[2], m->server.gunplay()->deployables().size(),
				(unsigned long long)st.damaged, (unsigned long long)st.destroyed);
		CHECK(m->server.gunplay()->deployables().empty());
		CHECK(st.destroyed == 1u);
	}

	TEST_CASE("gadgets: a support's C4 thrown at an enemy's feet rests there, goes off on its owner's detonator (Zoom "
			  "with the C4 in hand) and kills him through the shared blast, with the kill and the C4 on the thrower") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const auto flat_distance = [](const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
			return std::hypot(p_a[0] - p_b[0], p_a[2] - p_b[2]);
		};
		std::unique_ptr<Match> m = start_match(*level, true, 91u, 0, 1);
		tdm::ScriptedPeer &sapper = m->join("sapper");
		tdm::ScriptedPeer &enemy = m->join("enemy");
		sapper.ready();
		enemy.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t support = uint8_t(host::KitRole::Support);
		const uint8_t team = m->rules().team_of(uint8_t(sapper.slot()));
		const host::LoadoutKit *kit = gl.loadouts.kit(team, support);
		REQUIRE(kit != nullptr);
		const size_t gadget2 = size_t(core::gameplay::WeaponSlot::Gadget2);
		const auto c4 = std::find_if(kit->slots[gadget2].begin(), kit->slots[gadget2].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/c4/u_c4"; });
		REQUIRE(c4 != kit->slots[gadget2].end());
		net::LoadoutRequest request;
		request.kit = support;
		request.items[gadget2] = c4->identifier;
		m->server.on_receive(sapper.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		sapper.spawn_request(support);
		enemy.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(sapper.slot())) && is_alive(m->state(enemy.slot())); }, 600));
		REQUIRE(m->rules().team_of(uint8_t(sapper.slot())) != m->rules().team_of(uint8_t(enemy.slot())));
		REQUIRE(m->server.gunplay()->weapons(uint8_t(sapper.slot())).slots[gadget2].def == c4->def);
		std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		std::optional<sim::Vec3> far;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
			if (!far && f && flat_distance(*f, patch->victim) > 25.0f &&
					m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				far = f;
			}
		}
		REQUIRE(far.has_value());
		m->teleport(sapper, add(patch->shooter, 0.0f, 0.05f, 0.0f));
		m->teleport(enemy, add(patch->victim, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		bool fire = false, detonate = false;
		const sim::Vec3 feet = add(patch->victim, 0.0f, 0.1f, 0.0f);
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			uint32_t b = select_and_fire(uint8_t(gadget2), fire && (p_tick % 30u) < 10u);
			b |= detonate ? sim::buttons::kZoom : 0u;
			return aim_command(*m, p_tick, p_state, feet, sim::Pose::Stand, b);
		};
		m->ticks(60);
		fire = true;
		const std::optional<sim::Tick> rested = rest_tick(*m, 1, 300);
		fire = false;
		REQUIRE(rested.has_value());
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		const sw::Deployable placed = m->server.gunplay()->deployables()[0];
		CHECK(placed.def == c4->def);
		CHECK(flat_distance(placed.position, patch->victim) < 1.5f);
		m->teleport(sapper, add(*far, 0.0f, 0.05f, 0.0f));
		m->ticks(120);
		REQUIRE(is_alive(m->state(enemy.slot())));
		CHECK(m->server.gunplay()->deployables().size() == 1u);
		const int kills_before = int(m->row(sapper.slot())->kills);
		detonate = true;
		REQUIRE(m->until([&] { return !is_alive(m->state(enemy.slot())); }, 60));
		detonate = false;
		m->ticks(2);
		CHECK(m->server.gunplay()->deployables().empty());
		const host::GadgetStats &gs = m->server.gunplay()->gadget_stats();
		CHECK(gs.remote_detonations == 1u);
		CHECK(gs.pack_detonations == 1u);
		const net::DeathEvent *death = last_death(sapper, enemy.slot());
		REQUIRE(death != nullptr);
		CHECK(int(death->killer) == sapper.slot());
		CHECK(death->def == c4->def);
		CHECK(int(m->row(sapper.slot())->kills) == kills_before + 1);
		CHECK(is_alive(m->state(sapper.slot())));
		size_t detonations = 0;
		for (const net::CombatEvents &c : enemy.combat) {
			for (const net::DetonationWire &d : c.detonations) {
				detonations += d.def == c4->def ? 1u : 0u;
			}
		}
		CHECK(detonations == 1u);
		std::printf(
				"C4: thrown at (%.1f, %.1f, %.1f), %.2f m from the enemy's feet, detonated from %.0f m: %llu remote, "
				"%llu pack detonations\n",
				placed.position[0], placed.position[1], placed.position[2],
				flat_distance(placed.position, patch->victim), flat_distance(*far, patch->victim),
				(unsigned long long)gs.remote_detonations, (unsigned long long)gs.pack_detonations);
	}

	TEST_CASE("gadgets: a support's claymore lets its owner sprint through its cone and goes off on an enemy "
			  "sprinting at it (R2-G4 C: the team skip, the strict speed and the half-angle), the kill on the owner") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		std::unique_ptr<Match> m = start_match(*level, true, 93u, 0, 1);
		tdm::ScriptedPeer &sapper = m->join("sapper");
		tdm::ScriptedPeer &enemy = m->join("enemy");
		sapper.ready();
		enemy.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t support = uint8_t(host::KitRole::Support);
		const uint8_t team = m->rules().team_of(uint8_t(sapper.slot()));
		const host::LoadoutKit *kit = gl.loadouts.kit(team, support);
		REQUIRE(kit != nullptr);
		const size_t gadget2 = size_t(core::gameplay::WeaponSlot::Gadget2);
		const auto claymore = std::find_if(kit->slots[gadget2].begin(), kit->slots[gadget2].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/claymore/u_claymore"; });
		REQUIRE(claymore != kit->slots[gadget2].end());
		net::LoadoutRequest request;
		request.kit = support;
		request.items[gadget2] = claymore->identifier;
		m->server.on_receive(sapper.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		sapper.spawn_request(support);
		enemy.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(sapper.slot())) && is_alive(m->state(enemy.slot())); }, 600));
		REQUIRE(m->rules().team_of(uint8_t(sapper.slot())) != m->rules().team_of(uint8_t(enemy.slot())));
		REQUIRE(m->server.gunplay()->weapons(uint8_t(sapper.slot())).slots[gadget2].def == claymore->def);

		std::array<float, 2> face{};
		const std::optional<sim::Vec3> spot = flat_lane(*m, face);
		REQUIRE(spot.has_value());
		const auto at = [&](float p_d) { return add(*spot, p_d * face[0], 0.05f, p_d * face[1]); };
		m->teleport(sapper, at(0.0f));
		m->teleport(enemy, at(-30.0f));
		m->ticks(20);
		bool lay = false;
		bool run = false;
		const auto runner = [&](sim::Command p_command) {
			if (run) {
				p_command.yaw = sim::quantize_yaw(std::atan2(-face[0], -face[1]));
				p_command.pitch = 0;
				p_command.move_forward = 127;
				p_command.buttons |= sim::buttons::kSprint;
			}
			return p_command;
		};
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return runner(aim_command(*m, p_tick, p_state, at(1.0f), sim::Pose::Stand,
					select_and_fire(uint8_t(gadget2), lay && (p_tick % 30u) < 10u)));
		};
		m->scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return runner(sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose));
		};
		m->ticks(60);
		lay = true;
		const std::optional<sim::Tick> rested = rest_tick(*m, 1, 300);
		lay = false;
		REQUIRE(rested.has_value());
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		const sw::Deployable placed = m->server.gunplay()->deployables()[0];
		CHECK(placed.def == claymore->def);
		const float along = (placed.position[0] - (*spot)[0]) * face[0] + (placed.position[2] - (*spot)[2]) * face[1];
		const float facing = std::atan2(placed.facing[0] * face[1] - placed.facing[2] * face[0],
				placed.facing[0] * face[0] + placed.facing[2] * face[1]);
		std::printf("claymore: rests %.2f m along the lane, facing %.1f deg off it\n", double(along),
				double(facing / core::kDegToRadF));
		REQUIRE(std::fabs(facing) < 10.0f * core::kDegToRadF);
		const auto stats = [&] { return m->server.gunplay()->gadget_stats(); };
		sim::Tick trip_tick = 0, blast_tick = 0;
		const auto sprint_through = [&](tdm::ScriptedPeer &r_peer) {
			m->teleport(r_peer, at(along + 7.0f));
			m->ticks(20);
			run = true;
			float fastest = 0.0f;
			m->until(
					[&] {
						trip_tick = trip_tick == 0 && stats().field.soldier_trips > 0u ? m->now() : trip_tick;
						blast_tick = blast_tick == 0 && stats().pack_detonations > 0u ? m->now() : blast_tick;
						const sim::SoldierState *s = m->state(r_peer.slot());
						if (s == nullptr || !is_alive(s)) {
							return true;
						}
						const float d =
								(s->position[0] - (*spot)[0]) * face[0] + (s->position[2] - (*spot)[2]) * face[1];
						if (std::fabs(d - along) < 1.5f) {
							fastest = std::max(fastest, std::hypot(s->velocity[0], s->velocity[2]));
						}
						return d < along - 3.0f || stats().pack_detonations > 0u;
					},
					240);
			run = false;
			return fastest;
		};
		m->ticks(sim::seconds_to_ticks(3.5f));
		m->teleport(enemy, at(-30.0f));
		const float own_speed = sprint_through(sapper);
		std::printf("claymore: its owner sprinted through it at %.2f m/s: %llu soldier trips\n", double(own_speed),
				(unsigned long long)stats().field.soldier_trips);
		CHECK(own_speed > 3.8f);
		CHECK(stats().field.soldier_trips == 0u);
		CHECK(stats().pack_detonations == 0u);
		CHECK(m->server.gunplay()->deployables().size() == 1u);
		m->teleport(sapper, at(-30.0f));
		m->ticks(20);
		const int kills_before = int(m->row(sapper.slot())->kills);
		const float enemy_speed = sprint_through(enemy);
		m->ticks(5);
		std::printf("claymore: the enemy sprinted at it at %.2f m/s: %llu soldier trips, %llu pack detonations, enemy "
					"%s\n",
				double(enemy_speed), (unsigned long long)stats().field.soldier_trips,
				(unsigned long long)stats().pack_detonations, is_alive(m->state(enemy.slot())) ? "alive" : "dead");
		CHECK(enemy_speed > 3.8f);
		CHECK(stats().field.soldier_trips == 1u);
		CHECK(stats().pack_detonations == 1u);
		CHECK(m->server.gunplay()->deployables().empty());
		CHECK_FALSE(is_alive(m->state(enemy.slot())));
		const net::DeathEvent *death = last_death(sapper, enemy.slot());
		REQUIRE(death != nullptr);
		CHECK(int(death->killer) == sapper.slot());
		CHECK(death->def == claymore->def);
		CHECK(int(m->row(sapper.slot())->kills) == kills_before + 1);
		const sw::SimTime delay = m->server.gunplay()->table().defs[claymore->def].projectile.explosion.spawn_delay;
		std::printf("claymore: tripped at tick %u, its explosion %u ticks later (SpawnDelay %.2f s)\n",
				unsigned(trip_tick), unsigned(blast_tick - trip_tick), sw::units_to_seconds(delay));
		CHECK(delay == sw::seconds_to_units(0.15));
		CHECK(blast_tick - trip_tick == sim::seconds_to_ticks(0.15f));
	}

	TEST_CASE("gadgets: a recon's radio beacon is a spawn point for its squad: a mate's beacon SpawnRequest deploys at "
			  "it; with no beacon, outside the squad or on the other team it is refused") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		std::unique_ptr<Match> m = start_match(*level, true, 79u, 0, -1, squad_server(*level));
		tdm::ScriptedPeer &alpha = m->join("alpha");
		tdm::ScriptedPeer &bravo = m->join("bravo");
		tdm::ScriptedPeer &charlie = m->join("charlie");
		alpha.ready();
		bravo.ready();
		charlie.ready();
		m->bot();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->rules().team_of(uint8_t(alpha.slot())) == m->rules().team_of(uint8_t(charlie.slot())));
		REQUIRE(m->rules().team_of(uint8_t(alpha.slot())) != m->rules().team_of(uint8_t(bravo.slot())));
		alpha.spawn_request(3);
		REQUIRE(m->until([&] { return is_alive(m->state(alpha.slot())); }, 600));
		m->ticks(net::kPlayerListIntervalTicks + 2u);
		REQUIRE(squad_index(alpha, alpha.slot(), charlie.slot()) >= 0);
		charlie.spawn_request(1, net::kSpawnChoiceBeacon);
		m->ticks(3);
		CHECK_FALSE(is_alive(m->state(charlie.slot())));
		CHECK(m->rules().stats().refused[size_t(host::SpawnRefusal::SquadMate)] == 1u);

		CHECK(m->server.gunplay()->weapons(uint8_t(alpha.slot())).slots[5].def != sw::kNoWeapon);
		bool fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, sim::quantize_pitch(-0.3f), sim::Pose::Stand);
			c.buttons = select_and_fire(5, fire && (p_tick % 30u) < 10u);
			return c;
		};
		m->ticks(60);
		fire = true;
		const std::optional<sim::Tick> placed = rest_tick(*m, 1, 240);
		fire = false;
		REQUIRE(placed.has_value());
		const sim::Vec3 beacon = m->server.gunplay()->deployables()[0].position;
		const sim::Vec3 owner = m->state(alpha.slot())->position;
		std::printf("beacon: placed at (%.2f %.2f %.2f), %.2f m from its owner\n", beacon[0], beacon[1], beacon[2],
				distance(beacon, owner));
		CHECK(distance(beacon, owner) < 1.5f);
		m->ticks(2);
		const sw::Deployable &placed_beacon = m->server.gunplay()->deployables()[0];
		for (const tdm::ScriptedPeer *p : { &alpha, &bravo, &charlie }) {
			REQUIRE(p->deployables.size() == 1u);
			const net::Deployables sent = p->deployables.empty() ? net::Deployables() : p->deployables[0];
			CHECK(sent.removed.empty());
			REQUIRE(sent.added.size() == 1u);
			if (sent.added.empty()) {
				continue;
			}
			CHECK(sent.added[0].id == placed_beacon.id);
			CHECK(sent.added[0].def == placed_beacon.def);
			CHECK(sent.added[0].owner == uint8_t(alpha.slot()));
			CHECK(sent.added[0].team == m->rules().team_of(uint8_t(alpha.slot())));
			CHECK(distance(sent.added[0].position, beacon) < 0.01f);
		}
		REQUIRE(m->row(alpha.slot()) != nullptr);
		const uint8_t squad = m->row(alpha.slot())->squad;
		squad_request(*m, charlie, net::SquadOp::Leave);
		REQUIRE(m->row(charlie.slot())->squad == 0);
		m->ticks(1);
		charlie.spawn_request(1, net::kSpawnChoiceBeacon);
		m->ticks(3);
		CHECK_FALSE(is_alive(m->state(charlie.slot())));
		CHECK(m->rules().stats().refused[size_t(host::SpawnRefusal::SquadMate)] == 2u);
		bravo.spawn_request(1, net::kSpawnChoiceBeacon);
		m->ticks(3);
		CHECK_FALSE(is_alive(m->state(bravo.slot())));
		CHECK(m->rules().stats().refused[size_t(host::SpawnRefusal::SquadMate)] == 3u);
		squad_request(*m, charlie, net::SquadOp::Join, squad);
		REQUIRE(m->row(charlie.slot())->squad == squad);
		m->ticks(1);
		charlie.spawn_request(1, net::kSpawnChoiceBeacon);
		REQUIRE(m->until([&] { return is_alive(m->state(charlie.slot())); }, 10));
		REQUIRE(charlie.spawned.size() == 1u);
		const sim::Vec3 deployed = charlie.spawned.back().state.position;
		std::printf("beacon: charlie deployed at (%.2f %.2f %.2f), %.2f m from the beacon\n", deployed[0], deployed[1],
				deployed[2], distance(deployed, beacon));
		CHECK(distance(deployed, beacon) <= 1.5f);
		CHECK(m->rules().stats().beacon_spawns == 1u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
	TEST_CASE("gadgets: a medic bot walks to a hurt mate and throws its medkit at the mate's feet, which heals the "
			  "mate, and a loadbots client decodes the Deployables the server sends the mate") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(*level, true, 83u, 0, 1, gadget_bot_server(*level, 0, stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		const std::optional<Drop> drop = find_drop(*m, false, 25.0f, 60.0f, true);
		REQUIRE(drop.has_value());
		if (g.mate == nullptr || !drop.has_value()) {
			return;
		}
		REQUIRE(m->server.gunplay()->kit(uint8_t(g.bot)) == 0u);
		REQUIRE(supply_slot_of(*m, g.bot, true) >= 0);
		m->place(g.bot, add(drop->beside, 0.0f, 0.05f, 0.0f));
		m->teleport(*g.mate, add(drop->floor, 0.0f, drop->height, 0.0f));
		REQUIRE(m->until(
				[&] {
					const sim::SoldierState *s = m->state(g.mate->slot());
					return s != nullptr && s->health < 80.0f && s->mode == sim::MoveMode::OnGround;
				},
				600));
		const sim::Tick landed = m->now();
		const float hurt = m->state(g.mate->slot())->health;
		const sim::Vec3 feet = m->state(g.mate->slot())->position;
		const sw::Deployable *bag = nullptr;
		REQUIRE(m->until(
				[&] {
					bag = resting_bag(*m, g.bot, true);
					return bag != nullptr;
				},
				900));
		if (bag == nullptr) {
			return;
		}
		const sim::Tick rested = m->now();
		const float at_rest = m->state(g.mate->slot())->health;
		const float off = distance(bag->position, feet);
		const uint64_t heal_before = m->server.gunplay()->gadget_stats().heal_ticks;
		std::vector<std::vector<uint8_t>> to_mate;
		const auto keep_deployables = [&] {
			for (const host::OutDatagram &d : m->out) {
				if (d.peer == g.mate->peer() && net::peek_type(d.bytes) == net::MessageType::Deployables) {
					to_mate.push_back(d.bytes);
				}
			}
		};
		keep_deployables();
		for (int i = 0; i < 60; ++i) {
			m->tick();
			keep_deployables();
		}
		const float after = m->state(g.mate->slot())->health;
		std::printf(
				"medic bot: mate at %.1f HP, medkit at rest %u ticks after the landing, %.2f m from the mate's feet, "
				"mate %.1f -> %.1f HP in 1 s; %zu Deployables to the mate\n",
				hurt, unsigned(rested - landed), off, at_rest, after, to_mate.size());
		CHECK(off <= 3.5f);
		CHECK(m->server.gunplay()->gadget_stats().heal_ticks - heal_before >= 50u);
		CHECK(after - at_rest >= 6.0f);
		CHECK(stats->medkit_throws.load() >= 1u);
		REQUIRE(!to_mate.empty());

		auto pipe = std::make_shared<tp::Pipe>();
		pipe->connect(0);
		{
			std::lock_guard lock(pipe->mutex);
			for (const std::vector<uint8_t> &bytes : to_mate) {
				pipe->to_client[0].push_back(tp::Pipe::Sent{ 0, net::kChannelReliable, bytes });
			}
		}
		std::vector<std::unique_ptr<host::ClientTransport>> link;
		link.push_back(std::make_unique<tp::PipeClientTransport>(pipe, 0));
		host::LoadBotFleetConfig fc;
		fc.count = 1;
		fc.bot.level = level->shared->level;
		fc.bot.mode = level->shared->mode;
		host::LoadBotFleet fleet;
		const core::Status started = fleet.start(fc, std::move(link), net::monotonic_ns);
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		const auto handled = [&] {
			const host::LoadBotFleetStats f = fleet.stats();
			return f.bots.size() == 1u && f.bots[0].reliable_messages + f.bots[0].decode_errors >= to_mate.size();
		};
		CHECK(bf3::test::wait_until(handled, std::chrono::seconds(10), std::chrono::milliseconds(16)));
		fleet.stop();
		const host::LoadBotStats loadbot = fleet.stats().bots.at(0);
		std::printf("medic bot: a loadbots client decodes %llu of the %zu Deployables, %llu decode errors\n",
				(unsigned long long)loadbot.reliable_messages, to_mate.size(),
				(unsigned long long)loadbot.decode_errors);
		CHECK(loadbot.decode_errors == 0u);
		CHECK(loadbot.reliable_messages == to_mate.size());
	}

	TEST_CASE("gadgets: a support bot throws its ammo bag for a mate who emptied a magazine of its reserve, which "
			  "refills it") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(*level, true, 89u, 0, 1, gadget_bot_server(*level, 2, stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		const std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		if (g.mate == nullptr || !patch.has_value()) {
			return;
		}
		REQUIRE(m->server.gunplay()->kit(uint8_t(g.bot)) == 2u);
		REQUIRE(supply_slot_of(*m, g.bot, false) >= 0);
		m->teleport(*g.mate, add(patch->victim, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		const sw::WeaponSlotState &primary = m->server.gunplay()->weapons(uint8_t(g.mate->slot())).slots[0];
		const int32_t full = primary.reserve;
		const int32_t magazine = m->server.gunplay()->table().def(primary.def, primary.variant).magazine_capacity;
		bool shoot = true;
		m->scripts[g.mate_script] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::Vec3 sky = add(p_state.position, 0.0f, 30.0f, 1.0f);
			return aim_command(
					*m, p_tick, p_state, sky, sim::Pose::Stand, select_and_fire(0, shoot && (p_tick % 4u) < 2u));
		};
		REQUIRE(m->until([&] { return primary.reserve <= full - magazine; }, 1200));
		shoot = false;
		m->ticks(120);
		const int32_t low = primary.reserve;
		m->place(g.bot, add(patch->shooter, 0.0f, 0.05f, 0.0f));
		const sim::Tick placed = m->now();
		const sw::Deployable *bag = nullptr;
		REQUIRE(m->until(
				[&] {
					bag = resting_bag(*m, g.bot, false);
					return bag != nullptr;
				},
				900));
		if (bag == nullptr) {
			return;
		}
		const float off = distance(bag->position, m->state(g.mate->slot())->position);
		const sim::Tick rested = m->now();
		CHECK(m->until([&] { return primary.reserve > low; }, 180));
		std::printf("support bot: mate reserve %d of %d (magazine %d), ammo bag at rest %u ticks after the bot was "
					"placed, %.2f m from the mate, the reserve at %d %u ticks after the rest\n",
				low, full, magazine, unsigned(rested - placed), off, primary.reserve, unsigned(m->now() - rested));
		CHECK(off <= 3.5f);
		CHECK(m->server.gunplay()->gadget_stats().rounds_refilled > 0u);
		CHECK(stats->ammo_throws.load() >= 1u);
	}

	TEST_CASE(
			"gadgets: a bot with no squad order shot below 50 HP calls for a medic once the enemy is gone, walks to a "
			"medkit 8 to 15 m away and stays in it until healed") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(*level, true, 97u, 0, 1, gadget_bot_server(*level, 1, stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		if (g.mate == nullptr) {
			return;
		}
		size_t enemy_script = 0;
		for (size_t i = 0; i < m->peers.size(); ++i) {
			enemy_script = m->peers[i].get() != g.mate ? i : enemy_script;
		}
		tdm::ScriptedPeer &enemy = *m->peers[enemy_script];
		std::optional<sim::Vec3> centre, spot, shooter, away;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
			if (!f || !m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				continue;
			}
			const std::optional<sim::Vec3> s = ring_spot(*m, *f, 8.0f, 15.0f, std::nullopt);
			const std::optional<sim::Vec3> e = s ? ring_spot(*m, *f, 8.0f, 15.0f, s) : std::nullopt;
			if (s && e) {
				centre = f;
				spot = s;
				shooter = e;
				break;
			}
		}
		REQUIRE(centre.has_value());
		if (!centre.has_value()) {
			return;
		}
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
			if (f && core::vec::horizontal_distance(*f, *centre) >= 60.0f &&
					m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				away = f;
				break;
			}
		}
		REQUIRE(away.has_value());
		if (!away.has_value()) {
			return;
		}
		REQUIRE(m->server.gunplay()->kit(uint8_t(g.bot)) == 1u);
		REQUIRE(supply_slot_of(*m, g.bot, true) < 0);
		REQUIRE(supply_slot_of(*m, g.mate->slot(), true) >= 0);
		m->place(g.bot, add(*centre, 0.0f, 0.05f, 0.0f));
		m->teleport(*g.mate, add(*away, 0.0f, 0.05f, 0.0f));
		enemy.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(enemy.slot())); }, 600));
		m->teleport(enemy, add(*shooter, 0.0f, 0.05f, 0.0f));
		m->ticks(5);
		bool shoot = true;
		m->scripts[enemy_script] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::SoldierState *b = m->state(g.bot);
			const sim::Vec3 chest = b != nullptr ? add(b->position, 0.0f, 1.2f, 0.0f) : p_state.position;
			return aim_command(*m, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, shoot));
		};
		REQUIRE(m->until(
				[&] {
					const sim::SoldierState *b = m->state(g.bot);
					return b != nullptr && b->health < 50.0f;
				},
				600));
		shoot = false;
		m->scripts[enemy_script] = {};
		m->teleport(enemy, add(*away, 0.0f, 0.05f, 0.0f));
		REQUIRE(is_alive(m->state(g.bot)));
		if (!is_alive(m->state(g.bot))) {
			return;
		}
		const std::optional<sim::Vec3> hurt_floor = floor_at(*m->world, m->state(g.bot)->position);
		spot = hurt_floor ? ring_spot(*m, *hurt_floor, 8.0f, 15.0f, std::nullopt) : std::nullopt;
		REQUIRE(spot.has_value());
		if (!spot.has_value()) {
			return;
		}
		m->teleport(*g.mate, add(*spot, 0.0f, 0.05f, 0.0f));
		bool fire = false;
		const sim::Vec3 target = add(*spot, 0.6f, 0.1f, 0.0f);
		m->scripts[g.mate_script] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(
					*m, p_tick, p_state, target, sim::Pose::Stand, select_and_fire(4, fire && (p_tick % 30u) < 10u));
		};
		m->ticks(40);
		fire = true;
		const sw::Deployable *bag = nullptr;
		REQUIRE(m->until(
				[&] {
					bag = resting_bag(*m, g.mate->slot(), true);
					return bag != nullptr;
				},
				300));
		fire = false;
		if (bag == nullptr) {
			return;
		}
		const sim::Vec3 at = bag->position;
		m->scripts[g.mate_script] = {};
		m->teleport(*g.mate, add(*away, 0.0f, 0.05f, 0.0f));
		const float hurt = m->state(g.bot)->health;
		const float start = distance(m->state(g.bot)->position, at);
		const sim::Tick shot = m->now();
		sim::Tick entered = 0, inside_since = 0;
		const bool healed = m->until(
				[&] {
					const bool inside = distance(m->state(g.bot)->position, at) <= 3.5f;
					entered = inside && entered == 0 ? m->now() : entered;
					inside_since = inside ? (inside_since != 0 ? inside_since : m->now()) : 0;
					return inside_since != 0 && m->now() - inside_since >= 60u && m->state(g.bot)->health >= 80.0f;
				},
				1200);
		std::printf(
				"calling bot: %.1f HP when the medkit came to rest, %.1f m from it, first in it %u ticks later, in it "
				"since %u ticks, %.1f HP at %u ticks\n",
				hurt, start, entered != 0 ? unsigned(entered - shot) : 0u,
				inside_since != 0 ? unsigned(inside_since - shot) : 0u, m->state(g.bot)->health,
				unsigned(m->now() - shot));
		CHECK(start > 3.5f);
		CHECK(healed);
		CHECK(stats->medic_calls.load() >= 1u);
		CHECK(stats->medic_orders.load() >= 1u);
	}
}

TEST_SUITE("host-tdm-server") {
	TEST_CASE("squad spawn: a bot whose squad mate stands on a nav island off the bots' main component spawns on the "
			  "main component, not stranded beside the mate") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		core::Result<std::unique_ptr<sim::NavQuery>> nav = sim::NavQuery::create(level->bots->nav);
		REQUIRE_MESSAGE(nav.ok(), (nav.ok() ? std::string() : nav.error().message));
		if (!nav.ok()) {
			return;
		}
		const auto component = [&](const sim::Vec3 &p_at) {
			sim::NavPolyRef poly = 0;
			sim::Vec3 at{};
			return (*nav)->nearest(p_at, host::kSquadSpawnNavExtents, &poly, &at) && poly != 0
					? sim::nav_poly_component((*nav)->world(), level->bots->components, poly)
					: UINT32_MAX;
		};
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(*level, true, 101u, 0, 1, gadget_bot_server(*level, 1, stats));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		if (g.mate == nullptr) {
			return;
		}
		REQUIRE(m->rules().squads().same_squad(uint8_t(g.bot), uint8_t(g.mate->slot())));
		std::optional<sim::Vec3> island, centre, shooter, away;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
			const uint32_t c = f ? component(*f) : UINT32_MAX;
			if (f && c != UINT32_MAX && c != level->bots->main_component &&
					m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				island = f;
				break;
			}
		}
		REQUIRE(island.has_value());
		if (!island.has_value()) {
			return;
		}
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
			if (!f || core::vec::horizontal_distance(*f, *island) < 60.0f ||
					component(*f) != level->bots->main_component ||
					!m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				continue;
			}
			const std::optional<sim::Vec3> s = ring_spot(*m, *f, 8.0f, 15.0f, std::nullopt);
			if (!centre && s) {
				centre = f;
				shooter = s;
			} else if (centre && core::vec::horizontal_distance(*f, *centre) >= 60.0f) {
				away = f;
				break;
			}
		}
		REQUIRE(centre.has_value());
		REQUIRE(away.has_value());
		if (!centre.has_value() || !away.has_value()) {
			return;
		}
		size_t enemy_script = 0;
		for (size_t i = 0; i < m->peers.size(); ++i) {
			enemy_script = m->peers[i].get() != g.mate ? i : enemy_script;
		}
		tdm::ScriptedPeer &enemy = *m->peers[enemy_script];
		m->teleport(*g.mate, add(*island, 0.0f, 0.05f, 0.0f));
		m->place(g.bot, add(*centre, 0.0f, 0.05f, 0.0f));
		enemy.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(enemy.slot())); }, 600));
		m->teleport(enemy, add(*shooter, 0.0f, 0.05f, 0.0f));
		m->ticks(5);
		REQUIRE(is_alive(m->state(g.bot)));
		const uint32_t life = m->state(g.bot)->life;
		m->scripts[enemy_script] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			const sim::SoldierState *b = m->state(g.bot);
			const sim::Vec3 chest = b != nullptr ? add(b->position, 0.0f, 1.2f, 0.0f) : p_state.position;
			return aim_command(*m, p_tick, p_state, chest, sim::Pose::Stand, select_and_fire(0, true));
		};
		REQUIRE(m->until([&] { return !is_alive(m->state(g.bot)); }, 900));
		m->scripts[enemy_script] = {};
		if (is_alive(m->state(enemy.slot()))) {
			m->teleport(enemy, add(*away, 0.0f, 0.05f, 0.0f));
		}
		const uint64_t squad_before = m->rules().stats().squad_spawns;
		const bool back = m->until(
				[&] {
					const sim::SoldierState *b = m->state(g.bot);
					return is_alive(b) && b->life != life;
				},
				1800);
		REQUIRE(back);
		if (!back) {
			return;
		}
		const sim::Vec3 at = m->state(g.bot)->position;
		std::printf(
				"island squad spawn: mate on nav component %u at (%.1f %.1f %.1f), the bots' main component %u; the "
				"bot respawned at (%.1f %.1f %.1f) on component %u, %.1f m from the mate, %llu squad spawns\n",
				component(*island), (*island)[0], (*island)[1], (*island)[2], level->bots->main_component, at[0], at[1],
				at[2], component(at), core::vec::horizontal_distance(at, *island),
				(unsigned long long)(m->rules().stats().squad_spawns - squad_before));
		CHECK(component(at) == level->bots->main_component);
		CHECK(m->rules().stats().squad_spawns == squad_before);
	}

	TEST_CASE("squad spawn: a bot's spawn on its chosen mate follows M-CQ3 rule 4 alone: a mate hit a tick ago with an "
			  "enemy beside him takes it, a dead one does not and no other mate stands in, OnlySquadLeaderSpawn") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		for (const bool leader_only : { false, true }) {
			std::unique_ptr<Match> m = start_match(*level, true, 71u, 0, 2,
					[&](host::GameServerConfig &r_config) { r_config.rules.only_squad_leader_spawn = leader_only; });
			std::vector<tdm::ScriptedPeer *> peers;
			for (const char *name : { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot" }) {
				peers.push_back(&m->join(name));
				peers.back()->ready();
			}
			REQUIRE(wait_in_round(*m));
			for (tdm::ScriptedPeer *q : peers) {
				q->spawn_request(1, 0);
			}
			REQUIRE(m->until(
					[&] {
						return std::all_of(peers.begin(), peers.end(),
								[&](tdm::ScriptedPeer *q) { return is_alive(m->state(q->slot())); });
					},
					240));
			m->ticks(20);
			host::TdmRules &rules = *m->server.rules();
			const host::SquadTable &table = rules.squads();
			const uint8_t first = uint8_t(peers.front()->slot());
			int leader = -1, enemy = -1;
			tdm::ScriptedPeer *enemy_peer = nullptr;
			std::vector<int> members;
			for (tdm::ScriptedPeer *q : peers) {
				const uint8_t s = uint8_t(q->slot());
				if (table.team_of(s) != table.team_of(first)) {
					enemy = s;
					enemy_peer = q;
				} else if ((table.same_squad(s, first) || s == first) && table.leader(s)) {
					leader = s;
				} else if (table.same_squad(s, first) || s == first) {
					members.push_back(s);
				}
			}
			REQUIRE(leader >= 0);
			REQUIRE(enemy >= 0);
			REQUIRE(members.size() >= 2u);
			const uint8_t asker = uint8_t(members[0]);
			const auto choice_of = [&](int p_mate) {
				for (uint8_t i = 0; i < table.squad_size(); ++i) {
					if (table.mate(asker, i) == p_mate) {
						return uint8_t(i + 1u);
					}
				}
				return uint8_t(0);
			};
			const auto under_fire = [&](int p_mate) {
				const sim::Vec3 at = m->state(p_mate)->position;
				const std::optional<sim::Vec3> beside = floor_at(*m->world, add(at, 2.0f, 0.0f, 0.0f));
				m->teleport(*enemy_peer, add(beside.value_or(at), 0.0f, 0.05f, 0.0f));
				m->ticks(2);
				CHECK(distance(m->state(enemy)->position, m->state(p_mate)->position) < 5.0f);
				net::HitEvent hit;
				hit.victim = uint8_t(p_mate);
				hit.shooter = uint8_t(enemy);
				hit.damage = 10.0f;
				rules.hit(hit);
			};
			const auto take = [&](int p_mate, sim::Vec3 &r_at) {
				std::vector<sim::SoldierState> live(16);
				for (size_t i = 0; i < live.size(); ++i) {
					if (const sim::SoldierState *st = m->state(int(i))) {
						live[i] = *st;
					}
				}
				rules.set_spawn_choice(asker, choice_of(p_mate));
				uint16_t yaw = 0;
				return rules.take_squad_spawn(asker, live, r_at, yaw);
			};
			sim::Vec3 at{};
			under_fire(members[1]);
			const bool on_member = take(members[1], at);
			const float from_member = distance(at, m->state(members[1])->position);
			under_fire(leader);
			const bool on_leader = take(leader, at);
			const float from_leader = distance(at, m->state(leader)->position);
			const auto dying = std::find_if(
					peers.begin(), peers.end(), [&](tdm::ScriptedPeer *q) { return q->slot() == members[1]; });
			REQUIRE(dying != peers.end());
			m->server.on_receive((*dying)->peer(), net::kChannelReliable, net::encode_suicide_request());
			m->ticks(3);
			REQUIRE_FALSE(is_alive(m->state(members[1])));
			const bool on_dead = take(members[1], at);
			std::printf(
					"squad spawn (OnlySquadLeaderSpawn %d): on a member under fire %d (%.1f m), on the leader under "
					"fire %d (%.1f m), on a dead member %d\n",
					int(leader_only), int(on_member), from_member, int(on_leader), from_leader, int(on_dead));
			CHECK(on_member == !leader_only);
			CHECK(on_leader);
			CHECK(from_leader < 5.0f);
			CHECK_FALSE(on_dead);
			if (!leader_only) {
				CHECK(from_member < 5.0f);
			}
		}
	}

	TEST_CASE("nav: a bot standing 1.1 m off the nav mesh at Metro's (-189.1, 64.2, -542.7), where an E2 bot stood "
			  "40 s, walks back onto the mesh") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		REQUIRE(level->bots != nullptr);
		core::Result<std::unique_ptr<sim::NavQuery>> nav = sim::NavQuery::create(level->bots->nav);
		REQUIRE_MESSAGE(nav.ok(), (nav.ok() ? std::string() : nav.error().message));
		if (!nav.ok()) {
			return;
		}
		const auto off_mesh = [&](const sim::Vec3 &p_at) {
			sim::NavPolyRef poly = 0;
			sim::Vec3 at{};
			return (*nav)->nearest(p_at, { 3.0f, 2.5f, 3.0f }, &poly, &at) && poly != 0
					? core::vec::horizontal_distance(p_at, at)
					: 1e9f;
		};
		const sim::Vec3 wedge{ -189.07f, 64.28f, -542.67f };
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(*level, true, 103u, 0, 1, gadget_bot_server(*level, 0, stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		if (g.mate == nullptr) {
			return;
		}
		std::optional<sim::Vec3> away;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
			if (f && core::vec::horizontal_distance(*f, wedge) >= 60.0f &&
					m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				away = f;
				break;
			}
		}
		REQUIRE(away.has_value());
		if (!away.has_value()) {
			return;
		}
		m->teleport(*g.mate, add(*away, 0.0f, 0.05f, 0.0f));
		m->place(g.bot, wedge);
		m->ticks(5);
		REQUIRE(is_alive(m->state(g.bot)));
		const sim::Vec3 start = m->state(g.bot)->position;
		const float start_off = off_mesh(start);
		CHECK(start_off > 1.0f);
		float off = start_off, moved = 0.0f;
		const sim::Tick placed = m->now();
		const bool back = m->until(
				[&] {
					const sim::SoldierState *b = m->state(g.bot);
					if (!is_alive(b)) {
						return false;
					}
					off = off_mesh(b->position);
					moved = core::vec::horizontal_distance(b->position, start);
					return off <= 0.5f && moved >= 1.0f;
				},
				600);
		std::printf("wedge: bot placed at (%.2f %.2f %.2f), %.2f m off the mesh; after %u ticks %.2f m off, moved "
					"%.2f m, %llu wedge exits\n",
				start[0], start[1], start[2], start_off, unsigned(m->now() - placed), off, moved,
				(unsigned long long)stats->wedge_exits.load());
		CHECK(back);
		CHECK(stats->wedge_exits.load() >= 1u);
	}
}

namespace {

std::optional<net::SpotRow> last_spot_row(const tdm::ScriptedPeer &p_peer, int p_slot) {
	std::optional<net::SpotRow> out;
	for (const net::SpotStates &m : p_peer.spot_states) {
		for (const net::SpotRow &r : m.rows) {
			if (int(r.slot) == p_slot) {
				out = r;
			}
		}
	}
	return out;
}

void send_spot(Match &r_match, const tdm::ScriptedPeer &p_from, uint8_t p_type, std::vector<uint8_t> p_targets) {
	r_match.server.on_receive(
			p_from.peer(), net::kChannelReliable, net::encode_spot_request({ p_type, std::move(p_targets) }));
}

void send_order(Match &r_match, const tdm::ScriptedPeer &p_from, sim::commo::OrderType p_type, uint8_t p_target) {
	r_match.server.on_receive(p_from.peer(), net::kChannelReliable, net::encode_order_request({ p_type, p_target }));
}

}

TEST_SUITE("host-spotting") {
	TEST_CASE("spotting: a spot request reaches every client as the target's state and spotter, Active lasts the "
			  "soldier's ActiveSpottedTime on the server tick, and an order reaches only its receiver") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, false, 61u, 0, 1);
		tdm::ScriptedPeer &alpha = m->join("alpha");
		tdm::ScriptedPeer &bravo = m->join("bravo");
		tdm::ScriptedPeer &charlie = m->join("charlie");
		alpha.ready();
		bravo.ready();
		charlie.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until(
				[&] {
					return is_alive(m->state(alpha.slot())) && is_alive(m->state(bravo.slot())) &&
							is_alive(m->state(charlie.slot()));
				},
				60));
		const uint8_t a = uint8_t(alpha.slot());
		const uint8_t b = uint8_t(bravo.slot());
		const uint8_t c = uint8_t(charlie.slot());
		REQUIRE(m->rules().team_of(a) == m->rules().team_of(c));
		REQUIRE(m->rules().team_of(a) != m->rules().team_of(b));
		for (const tdm::ScriptedPeer *p : { &alpha, &bravo, &charlie }) {
			REQUIRE_FALSE(p->spot_states.empty());
			CHECK(p->spot_states.front().full);
			CHECK(last_spot_row(*p, b).value_or(net::SpotRow{}).state == sim::spotting::SpotState::None);
		}

		const sim::spotting::SpottingDef def = *sim::spotting::make_spotting_def(level->shared->gunplay->soldier);
		CHECK(def.target.active_time == 7.0f);
		host::SpotSubject body;
		REQUIRE(host::place_spot_body(body, level->shared->gunplay->soldier, sim::Pose::Stand, { 0.0f, 0.0f, 0.0f }));
		CHECK(body.body.radius == doctest::Approx(0.35f));
		CHECK(body.body.b[1] == doctest::Approx(1.8f - 0.35f));
		REQUIRE(host::place_spot_body(body, level->shared->gunplay->soldier, sim::Pose::Crouch, { 0.0f, 0.0f, 0.0f }));
		CHECK(body.body.b[1] == doctest::Approx(1.15f - 0.35f));
		REQUIRE(host::place_spot_body(body, level->shared->gunplay->soldier, sim::Pose::Prone, { 0.0f, 0.0f, 0.0f }));
		CHECK(body.body.b[1] == doctest::Approx(0.35f));
		CHECK(body.centre[1] == doctest::Approx(0.35f));
		send_spot(*m, alpha, uint8_t(sim::spotting::SpotType::Active), { b });
		const sim::Tick applied = m->now();
		m->tick();
		for (const tdm::ScriptedPeer *p : { &alpha, &bravo, &charlie }) {
			const std::optional<net::SpotRow> row = last_spot_row(*p, b);
			REQUIRE(row.has_value());
			CHECK(row->state == sim::spotting::SpotState::Active);
			CHECK(row->spotter == a);
		}
		CHECK(m->server.stats().spotting.spots_applied == 1u);
		send_spot(*m, charlie, uint8_t(sim::spotting::SpotType::Active), { b });
		send_spot(*m, alpha, 3, { b });
		CHECK(m->server.stats().spotting.unhandled_spot_types == 1u);
		CHECK(m->server.spot_row(b).spotter == a);

		const bool expired = m->until(
				[&] {
					return last_spot_row(charlie, b).value_or(net::SpotRow{}).state == sim::spotting::SpotState::None;
				},
				2 * 7 * sim::kSimHz);
		REQUIRE(expired);
		const sim::Tick lasted = m->now() - applied;
		std::printf("spotting: Active from tick %u for %u ticks (%.3f s at %u Hz), %llu state messages\n", applied,
				lasted, double(lasted) / double(sim::kSimHz), sim::kSimHz,
				(unsigned long long)m->server.stats().spotting.state_messages);
		CHECK(lasted >= 7u * sim::kSimHz);
		CHECK(lasted <= 7u * sim::kSimHz + 3u);
		CHECK(last_spot_row(alpha, b).value_or(net::SpotRow{}).state == sim::spotting::SpotState::None);
		CHECK(last_spot_row(bravo, b).value_or(net::SpotRow{}).state == sim::spotting::SpotState::None);

		send_spot(*m, alpha, uint8_t(sim::spotting::SpotType::Passive), { b });
		m->tick();
		CHECK(last_spot_row(bravo, b).value_or(net::SpotRow{}).state == sim::spotting::SpotState::Passive);

		const size_t before_a = alpha.order_notices.size();
		const size_t before_b = bravo.order_notices.size();
		send_order(*m, alpha, sim::commo::OrderType::RequestHeal, c);
		m->tick();
		REQUIRE(charlie.order_notices.size() == 1u);
		CHECK(charlie.order_notices.back().order == sim::commo::OrderType::RequestHeal);
		CHECK(charlie.order_notices.back().issuer == a);
		CHECK(alpha.order_notices.size() == before_a);
		CHECK(bravo.order_notices.size() == before_b);
		send_order(*m, alpha, sim::commo::OrderType::RequestHeal, c);
		m->tick();
		CHECK(charlie.order_notices.size() == 2u);
		send_order(*m, alpha, sim::commo::OrderType::GoGoGo, net::kNoOrderTarget);
		send_order(*m, alpha, sim::commo::OrderType::RequestAmmo, a);
		m->tick();
		CHECK(charlie.order_notices.size() == 2u);
		CHECK(alpha.order_notices.size() == before_a);
		CHECK(m->server.stats().spotting.orders_stored == 2u);
		CHECK(m->server.stats().spotting.orders_refused == 1u);
		CHECK(m->server.stats().bad_datagrams == 0u);
	}
}

namespace {

const sim::logic::ConquestNatives &conquest_of(const Match &p_match) {
	const sim::logic::ConquestNatives *c = p_match.rules().natives().conquest();
	REQUIRE(c != nullptr);
	return *c;
}

bool on_open_point(const host::SharedLevel &p_level, const sim::logic::ConquestNatives &p_conquest, uint8_t p_team,
		const sim::Vec3 &p_at, float p_radius) {
	for (size_t i = 0; i < p_level.spawns.size(); ++i) {
		const sim::SpawnPoint &sp = p_level.spawns[i];
		const float dx = sp.position[0] - p_at[0], dz = sp.position[2] - p_at[2];
		if ((sp.team == 0 || sp.team == int(p_team)) && dx * dx + dz * dz <= p_radius * p_radius &&
				p_conquest.spawn_point_open(p_level.spawn_spawners[i], p_team)) {
			return true;
		}
	}
	return false;
}

size_t open_points(const host::SharedLevel &p_level, const sim::logic::ConquestNatives &p_conquest, uint8_t p_team) {
	size_t n = 0;
	for (size_t i = 0; i < p_level.spawns.size(); ++i) {
		n += (p_level.spawns[i].team == 0 || p_level.spawns[i].team == int(p_team)) &&
						p_conquest.spawn_point_open(p_level.spawn_spawners[i], p_team)
				? 1u
				: 0u;
	}
	return n;
}

sim::Vec3 ground_at(const sim::CollisionWorld &p_world, float p_x, float p_z, float p_from_y) {
	sim::StaticHit hit;
	REQUIRE(p_world.ground_below({ p_x, p_from_y, p_z }, 2000.0f, sim::query_mask::kMovement, &hit));
	return add(hit.position, 0.0f, 0.05f, 0.0f);
}

}

TEST_SUITE("host-tdm-conquest") {
	TEST_CASE(
			"conquest: MP_007 ConquestSmall0 on the GameServer: HQ spawns, a soldier on a flag takes it and opens its "
			"spawn points for his team only, a human deserter dies as his own killer") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::SharedLevel &shared = *level->shared;
		REQUIRE(shared.spawn_spawners.size() == shared.spawns.size());
		std::unique_ptr<Match> m = start_match(*level, false, 51u, 0, 2);
		std::vector<Script *> bots;
		for (int i = 0; i < 4; ++i) {
			bots.push_back(m->bot());
		}
		tdm::ScriptedPeer &p = m->join("deserter");
		p.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until(
				[&] {
					for (const Script *b : bots) {
						if (!is_alive(m->state(b->slot))) {
							return false;
						}
					}
					return is_alive(m->state(p.slot()));
				},
				600));
		const sim::logic::ConquestNatives &cq = conquest_of(*m);
		for (const uint8_t t : { uint8_t(1), uint8_t(2) }) {
			CHECK(cq.tickets(t) == cq.initial_tickets(t));
		}
		size_t on_hq = 0;
		for (const Script *b : bots) {
			const uint8_t t = m->rules().team_of(uint8_t(b->slot));
			on_hq += on_open_point(shared, cq, t, m->state(b->slot)->position, 10.0f) ? 1u : 0u;
		}
		const uint8_t peer_team = m->rules().team_of(uint8_t(p.slot()));
		on_hq += on_open_point(shared, cq, peer_team, m->state(p.slot())->position, 10.0f) ? 1u : 0u;
		const size_t open1 = open_points(shared, cq, 1), open2 = open_points(shared, cq, 2);
		std::printf(
				"conquest: %zu of %zu soldiers spawned on an open point of their team; open points US %zu RU %zu of "
				"%zu; tickets US %lld RU %lld\n",
				on_hq, bots.size() + 1u, open1, open2, shared.spawns.size(), (long long)cq.tickets(1),
				(long long)cq.tickets(2));
		CHECK(on_hq == bots.size() + 1u);
		REQUIRE_FALSE(p.conquest_states.empty());
		const auto wire_matches = [&](const net::ConquestState &p_wire) {
			bool same = p_wire.point_count == cq.points().size();
			for (uint8_t t = 1; t <= 2; ++t) {
				same = same && p_wire.tickets[t - 1u] == cq.tickets(t) &&
						p_wire.initial_tickets[t - 1u] == cq.initial_tickets(t) &&
						p_wire.bleeding[t - 1u] == cq.bleeding(t);
			}
			for (size_t i = 0; same && i < cq.points().size(); ++i) {
				const sim::logic::ConquestPointView &v = cq.points()[i];
				same = p_wire.points[i].team == v.team && p_wire.points[i].controlled == v.controlled &&
						std::abs(int(p_wire.points[i].progress) - int(std::lround(v.progress * 100.0f))) <= 300;
			}
			return same;
		};
		CHECK(wire_matches(p.conquest_states.back()));

		Script *taker = nullptr;
		for (Script *b : bots) {
			if (m->rules().team_of(uint8_t(b->slot)) == 1) {
				taker = b;
				break;
			}
		}
		REQUIRE(taker != nullptr);
		const sim::logic::ConquestPointView *flag = nullptr;
		for (const sim::logic::ConquestPointView &v : cq.points()) {
			if (!v.hq && v.team == 0) {
				flag = &v;
				break;
			}
		}
		REQUIRE(flag != nullptr);
		const uint32_t flag_node = flag->node;
		const size_t flag_index = size_t(flag - cq.points().data());
		const sim::Vec3 flag_at = flag->position;
		m->place(taker->slot, ground_at(*m->world, flag_at[0], flag_at[2], flag_at[1] + 50.0f));
		const sim::Tick placed = m->now();
		const size_t states_before = p.conquest_states.size();
		const auto flag_team = [&] {
			for (const sim::logic::ConquestPointView &v : cq.points()) {
				if (v.node == flag_node) {
					return v.controlled ? v.team : uint8_t(0);
				}
			}
			return uint8_t(0xFF);
		};
		REQUIRE(m->until([&] { return flag_team() == 1; }, 1500));
		const sim::Tick taken = m->now();
		const size_t open1_after = open_points(shared, cq, 1), open2_after = open_points(shared, cq, 2);
		std::printf(
				"conquest: the US soldier on flag node %u took it in %u ticks; open points US %zu -> %zu, RU %zu -> "
				"%zu; captures %llu\n",
				unsigned(flag_node), unsigned(taken - placed), open1, open1_after, open2, open2_after,
				(unsigned long long)cq.stats().captures);
		CHECK(is_alive(m->state(taker->slot)));
		CHECK(taken - placed >= 1150u);
		CHECK(open1_after > open1);
		CHECK(open2_after == open2);
		CHECK(cq.stats().captures == 1u);
		m->ticks(1);
		const size_t capture_states = p.conquest_states.size() - states_before;
		const net::ConquestPointWire &taken_wire = p.conquest_states.back().points[flag_index];
		std::printf("conquest: the client got %zu ConquestState messages over the %u-tick capture; the flag on the "
					"wire: team %u, controlled %d, progress %u\n",
				capture_states, unsigned(taken - placed), unsigned(taken_wire.team), int(taken_wire.controlled),
				unsigned(taken_wire.progress));
		CHECK(wire_matches(p.conquest_states.back()));
		CHECK(taken_wire.team == 1u);
		CHECK(taken_wire.controlled);
		CHECK(taken_wire.progress == net::kConquestProgressMax);
		CHECK(capture_states >= 3u);
		CHECK(capture_states <= (taken - placed) / host::kConquestProgressIntervalTicks + 8u);

		const sim::logic::ConquestParams &params = *shared.logic_natives->conquest;
		const sim::logic::ConquestAreaData *area = nullptr;
		for (const sim::logic::ConquestAreaData &a : params.areas) {
			if (!a.params.team_specific || a.params.team == peer_team) {
				area = &a;
				break;
			}
		}
		REQUIRE(area != nullptr);
		REQUIRE(area->polygon.size() >= 3u);
		float max_x = area->polygon[0][0], min_z = area->polygon[0][1], max_z = area->polygon[0][1];
		for (const std::array<float, 2> &v : area->polygon) {
			max_x = std::max(max_x, v[0]);
			min_z = std::min(min_z, v[1]);
			max_z = std::max(max_z, v[1]);
		}
		const float out_x = max_x + 40.0f, out_z = 0.5f * (min_z + max_z);
		REQUIRE_FALSE(sim::logic::polygon_contains(area->polygon, out_x, out_z));
		const sim::SoldierState before = *m->state(p.slot());
		const uint64_t suicides0 = m->rules().stats().suicides;
		const size_t combat0 = p.combat.size();
		m->teleport(p, ground_at(*m->world, out_x, out_z, before.position[1] + 300.0f));
		const sim::Tick left = m->now();
		const size_t deserter_states = p.conquest_states.size();
		m->ticks(2);
		REQUIRE(p.conquest_states.size() > deserter_states);
		const sim::Tick return_by = p.conquest_states.back().return_by_tick;
		const sim::Tick expected_return = left + uint32_t(area->params.time_to_return) * sim::kSimHz;
		std::printf("conquest: the deserter's return tick on the wire %u, left at %u, TimeToReturn %d s\n",
				unsigned(return_by), unsigned(left), int(area->params.time_to_return));
		CHECK(return_by + 3u >= expected_return);
		CHECK(return_by <= expected_return + 3u);
		REQUIRE(m->until(
				[&] { return !is_alive(m->state(p.slot())); }, uint32_t(area->params.time_to_return) * 60u + 120u));
		const sim::Tick died = m->now();
		const auto own_deaths = [&] {
			size_t n = 0;
			for (size_t i = combat0; i < p.combat.size(); ++i) {
				for (const net::DeathEvent &d : p.combat[i].deaths) {
					n += d.victim == uint8_t(p.slot()) && d.killer == uint8_t(p.slot()) ? 1u : 0u;
				}
			}
			return n;
		};
		CHECK(m->until([&] { return own_deaths() != 0; }, 10));
		const size_t own = own_deaths();
		std::printf(
				"conquest: the human outside the combat area died after %u ticks (TimeToReturn %d s); deserter kills "
				"%llu, strikes %llu, suicides +%llu\n",
				unsigned(died - left), int(area->params.time_to_return), (unsigned long long)cq.stats().deserter_kills,
				(unsigned long long)cq.stats().strikes, (unsigned long long)(m->rules().stats().suicides - suicides0));
		CHECK(died - left >= uint32_t(area->params.time_to_return) * 60u - 2u);
		CHECK(own == 1u);
		const bool struck_on_wire = std::any_of(p.conquest_states.begin() + std::ptrdiff_t(deserter_states),
				p.conquest_states.end(), [&](const net::ConquestState &c) { return c.is_struck(uint8_t(p.slot())); });
		m->ticks(1);
		CHECK(struck_on_wire);
		CHECK(p.conquest_states.back().return_by_tick == 0u);
		CHECK_FALSE(p.conquest_states.back().is_struck(uint8_t(p.slot())));
		CHECK(cq.stats().deserter_kills == 1u);
		CHECK(m->rules().stats().suicides == suicides0 + 1u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("conquest: the dedicated server's Conquest settings reach the rules: GameModeCounterModifier, "
			  "IsDesertingAllowed, DisableTicketBleed, SuperFastBleed") {
		host::ServerRules rules;
		CHECK(rules.conquest.game_mode_counter_modifier == 1.0);
		CHECK_FALSE(rules.conquest.deserting_allowed);
		CHECK_FALSE(rules.conquest.bleed_disabled);
		CHECK(rules.conquest.bleed_scale == 1.0);
		rules.conquest.game_mode_counter_modifier = 2.0;
		rules.conquest.deserting_allowed = true;
		rules.conquest.bleed_disabled = true;
		rules.conquest.bleed_scale = 100.0;
		const host::GameServerConfig c = host::tdm_game_server_config(host::ServerLevel{}, rules);
		CHECK(c.rules.conquest.game_mode_counter_modifier == 2.0);
		CHECK(c.rules.conquest.deserting_allowed);
		CHECK(c.rules.conquest.bleed_disabled);
		CHECK(c.rules.conquest.bleed_scale == 100.0);
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, false, 59u, 0, 2,
				[&](host::GameServerConfig &r_config) { r_config.rules.conquest = c.rules.conquest; });
		const sim::logic::ConquestNatives &cq = conquest_of(*m);
		std::printf("conquest: with GameModeCounterModifier 2 the teams start with %lld and %lld tickets\n",
				(long long)cq.initial_tickets(1), (long long)cq.initial_tickets(2));
		CHECK(cq.initial_tickets(1) == 300);
		CHECK(cq.initial_tickets(2) == 300);
	}

	TEST_CASE("conquest: a flag spawn choice deploys on the chosen flag once the team holds it; a closed flag is "
			  "refused and a flag lost in the deploy's tick leaves no open point, both answered on the wire") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::SharedLevel &shared = *level->shared;
		std::unique_ptr<Match> m = start_match(*level, true, 61u, 0, 2);
		std::vector<Script *> bots;
		for (int i = 0; i < 3; ++i) {
			bots.push_back(m->bot());
		}
		tdm::ScriptedPeer &p = m->join("flagger");
		p.ready();
		REQUIRE(wait_in_round(*m));
		const sim::logic::ConquestNatives &cq = conquest_of(*m);
		const uint8_t team = m->rules().team_of(uint8_t(p.slot()));
		Script *mate = nullptr;
		Script *enemy = nullptr;
		for (Script *b : bots) {
			(m->rules().team_of(uint8_t(b->slot)) == team ? mate : enemy) = b;
		}
		REQUIRE(mate != nullptr);
		REQUIRE(enemy != nullptr);
		REQUIRE(m->until([&] { return is_alive(m->state(mate->slot)) && is_alive(m->state(enemy->slot)); }, 600));
		size_t flag = SIZE_MAX;
		for (size_t i = 0; i < cq.points().size() && flag == SIZE_MAX; ++i) {
			flag = !cq.points()[i].hq && cq.points()[i].team == 0 ? i : SIZE_MAX;
		}
		REQUIRE(flag != SIZE_MAX);
		const uint8_t choice = uint8_t(net::kSpawnChoiceFlag + flag);
		const sim::Vec3 flag_at = cq.points()[flag].position;
		CHECK_FALSE(m->rules().flag_open(flag, team));
		p.spawn_request(1, choice);
		m->ticks(2);
		REQUIRE_FALSE(p.spawn_answers.empty());
		CHECK(p.spawn_answers.back().kind == net::SpawnAnswerKind::FlagClosed);
		CHECK(p.spawn_answers.back().spawn_choice == choice);
		CHECK_FALSE(is_alive(m->state(p.slot())));
		CHECK(m->rules().stats().refused[size_t(host::SpawnRefusal::FlagClosed)] == 1u);

		m->place(mate->slot, ground_at(*m->world, flag_at[0], flag_at[2], flag_at[1] + 50.0f));
		REQUIRE(m->until([&] { return m->rules().flag_open(flag, team); }, 1500));
		p.spawn_request(1, choice);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		const sim::Vec3 at = m->state(p.slot())->position;
		size_t nearest = SIZE_MAX;
		float best = 1e9f;
		for (size_t i = 0; i < shared.spawns.size(); ++i) {
			const float d = distance(shared.spawns[i].position, at);
			if (d < best) {
				best = d;
				nearest = i;
			}
		}
		REQUIRE(nearest != SIZE_MAX);
		std::printf("conquest: the peer deployed on flag %zu at (%.1f %.1f %.1f), %.1f m from spawn point %zu, %.0f m "
					"from the flag\n",
				flag, at[0], at[1], at[2], best, nearest, distance(at, flag_at));
		CHECK(best < 2.0f);
		CHECK(m->rules().spawn_point_in_flag(flag, shared.spawns[nearest]));
		CHECK(m->rules().stats().flag_spawns == 1u);

		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_suicide_request());
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(p.slot())));
		const uint32_t earliest = m->rules().earliest_respawn(uint8_t(p.slot()));
		m->place(mate->slot, ground_at(*m->world, flag_at[0] + 400.0f, flag_at[2], flag_at[1] + 300.0f));
		m->place(enemy->slot, ground_at(*m->world, flag_at[0], flag_at[2], flag_at[1] + 50.0f));
		const auto flag_progress = [&] { return cq.points()[flag].progress; };
		REQUIRE(m->until(
				[&] { return m->now() >= earliest && flag_progress() > 0.0f && flag_progress() < 0.09f; }, 1500));
		REQUIRE(m->rules().flag_open(flag, team));
		const size_t answers = p.spawn_answers.size();
		p.spawn_request(1, choice);
		m->ticks(2);
		std::printf("conquest: the flag fell to team %u in the deploy's tick; answers %zu -> %zu, alive %d\n",
				unsigned(cq.points()[flag].team), answers, p.spawn_answers.size(), int(is_alive(m->state(p.slot()))));
		CHECK_FALSE(m->rules().flag_open(flag, team));
		CHECK_FALSE(is_alive(m->state(p.slot())));
		REQUIRE(p.spawn_answers.size() == answers + 1u);
		CHECK(p.spawn_answers.back().kind == net::SpawnAnswerKind::NoSpawnPoint);
		CHECK(p.spawn_answers.back().spawn_choice == choice);
		p.spawn_request(1, 0);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("conquest: OnlySquadLeaderSpawn refuses a squad spawn between two members and allows one on the leader "
			  "(M-CQ3 rule 4)") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 67u, 0, 2,
				[](host::GameServerConfig &r_config) { r_config.rules.only_squad_leader_spawn = true; });
		std::vector<tdm::ScriptedPeer *> peers;
		for (const char *name : { "alpha", "bravo", "charlie", "delta", "echo" }) {
			peers.push_back(&m->join(name));
			peers.back()->ready();
		}
		REQUIRE(wait_in_round(*m));
		REQUIRE(peers.front()->match_info.has_value());
		CHECK((peers.front()->match_info->rules_flags & net::rules_flags::kOnlySquadLeaderSpawn) != 0);
		for (tdm::ScriptedPeer *q : peers) {
			q->spawn_request(1, 0);
		}
		REQUIRE(m->until(
				[&] {
					return std::all_of(peers.begin(), peers.end(),
							[&](tdm::ScriptedPeer *q) { return is_alive(m->state(q->slot())); });
				},
				240));
		m->ticks(20);
		const net::PlayerRow *first = m->row(peers.front()->slot());
		REQUIRE(first != nullptr);
		tdm::ScriptedPeer *leader = nullptr;
		std::vector<tdm::ScriptedPeer *> members;
		for (tdm::ScriptedPeer *q : peers) {
			const net::PlayerRow *r = m->row(q->slot());
			REQUIRE(r != nullptr);
			if (r->team != first->team || r->squad != first->squad) {
				continue;
			}
			if (r->leader) {
				leader = q;
			} else {
				members.push_back(q);
			}
		}
		REQUIRE(leader != nullptr);
		REQUIRE(members.size() >= 2u);
		tdm::ScriptedPeer &dead = *members[0];
		tdm::ScriptedPeer &member = *members[1];
		m->server.on_receive(dead.peer(), net::kChannelReliable, net::encode_suicide_request());
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(dead.slot())));
		const uint32_t earliest = m->rules().earliest_respawn(uint8_t(dead.slot()));
		REQUIRE(m->until([&] { return m->now() >= earliest; }, 1200));
		const std::vector<net::PlayerRow> rows = dead.players;
		const uint8_t on_member = host::squad_spawn_choice(rows, uint8_t(dead.slot()), uint8_t(member.slot()));
		const uint8_t on_leader = host::squad_spawn_choice(rows, uint8_t(dead.slot()), uint8_t(leader->slot()));
		REQUIRE(on_member != 0);
		REQUIRE(on_leader != 0);
		dead.spawn_request(1, on_member);
		m->ticks(2);
		REQUIRE_FALSE(dead.spawn_answers.empty());
		CHECK(dead.spawn_answers.back().kind == net::SpawnAnswerKind::NotSquadLeader);
		CHECK_FALSE(is_alive(m->state(dead.slot())));
		CHECK(m->rules().stats().refused[size_t(host::SpawnRefusal::NotSquadLeader)] == 1u);
		dead.spawn_request(1, on_leader);
		REQUIRE(m->until([&] { return is_alive(m->state(dead.slot())); }, 240));
		const float from_leader = distance(m->state(dead.slot())->position, m->state(leader->slot())->position);
		std::printf("conquest: OnlySquadLeaderSpawn: a member's spawn on a member refused, on the leader at %.1f m\n",
				from_leader);
		CHECK(from_leader < 5.0f);
		CHECK(m->rules().stats().squad_spawns == 1u);
	}

	TEST_CASE("conquest: a revive costs no ticket, a redeploy costs one (M-CQ2 rule 4)") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, true, 53u, 0, 2);
		for (int i = 0; i < 3; ++i) {
			m->bot();
		}
		tdm::ScriptedPeer &p = m->join("lazarus");
		p.ready();
		REQUIRE(wait_in_round(*m));
		p.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		m->ticks(30);
		const sim::logic::ConquestNatives &cq = conquest_of(*m);
		const uint8_t team = m->rules().team_of(uint8_t(p.slot()));
		const int64_t spawned = cq.tickets(team);
		CHECK(spawned == cq.initial_tickets(team));
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_suicide_request());
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(p.slot())));
		const int64_t dead = cq.tickets(team);
		net::DebugRequest q;
		q.kind = net::DebugKind::Revive;
		q.id = m->next_debug++;
		p.debug(q);
		m->ticks(3);
		REQUIRE(is_alive(m->state(p.slot())));
		REQUIRE(p.spawned.back().revived);
		const int64_t revived = cq.tickets(team);
		m->ticks(30);
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_suicide_request());
		m->ticks(3);
		REQUIRE_FALSE(is_alive(m->state(p.slot())));
		const uint32_t earliest = m->rules().earliest_respawn(uint8_t(p.slot()));
		REQUIRE(m->until([&] { return m->now() >= earliest; }, 1200));
		p.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		REQUIRE_FALSE(p.spawned.back().revived);
		const int64_t redeployed = cq.tickets(team);
		std::printf("conquest: team %u tickets: spawned %lld, dead %lld, revived %lld, redeployed %lld; respawn costs "
					"%llu\n",
				unsigned(team), (long long)spawned, (long long)dead, (long long)revived, (long long)redeployed,
				(unsigned long long)cq.stats().ticket_respawn_costs);
		CHECK(dead == spawned);
		CHECK(revived == dead);
		CHECK(redeployed == revived - 1);
		CHECK(cq.stats().ticket_respawn_costs == 1u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE(
			"conquest: with an enemy on every open spawn point the Spawner's fallback still spawns on an open point") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::SharedLevel &shared = *level->shared;
		std::unique_ptr<Match> m = start_match(*level, true, 57u, 0, 2);
		std::vector<Script *> bots;
		for (int i = 0; i < 14; ++i) {
			bots.push_back(m->bot());
		}
		tdm::ScriptedPeer &p = m->join("cornered");
		p.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until(
				[&] {
					for (const Script *b : bots) {
						if (!is_alive(m->state(b->slot))) {
							return false;
						}
					}
					return true;
				},
				600));
		const sim::logic::ConquestNatives &cq = conquest_of(*m);
		const uint8_t team = m->rules().team_of(uint8_t(p.slot()));
		std::vector<Script *> enemies;
		for (Script *b : bots) {
			if (m->rules().team_of(uint8_t(b->slot)) != team) {
				enemies.push_back(b);
			}
		}
		std::vector<sim::Vec3> guards;
		size_t open = 0;
		for (size_t i = 0; i < shared.spawns.size(); ++i) {
			const sim::SpawnPoint &sp = shared.spawns[i];
			if ((sp.team != 0 && sp.team != int(team)) || !cq.spawn_point_open(shared.spawn_spawners[i], team)) {
				continue;
			}
			++open;
			const bool covered = std::any_of(
					guards.begin(), guards.end(), [&](const sim::Vec3 &g) { return distance(g, sp.position) < 9.5f; });
			if (!covered) {
				REQUIRE(guards.size() < enemies.size());
				const sim::Vec3 at = ground_at(*m->world, sp.position[0], sp.position[2], sp.position[1] + 2.0f);
				m->place(enemies[guards.size()]->slot, at);
				guards.push_back(at);
			}
		}
		m->ticks(2);
		for (size_t g = 0; g < guards.size(); ++g) {
			REQUIRE(is_alive(m->state(enemies[g]->slot)));
		}
		const uint64_t fallbacks0 = m->rules().stats().spawn_fallbacks;
		p.spawn_request(1);
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 240));
		const sim::Vec3 at = m->state(p.slot())->position;
		const bool on_open = on_open_point(shared, cq, team, at, 1.0f);
		std::printf(
				"conquest: %zu open points of team %u guarded by %zu enemies; the peer spawned at (%.1f %.1f %.1f), on "
				"an open point %d; scored fallbacks +%llu\n",
				open, unsigned(team), guards.size(), at[0], at[1], at[2], int(on_open),
				(unsigned long long)(m->rules().stats().spawn_fallbacks - fallbacks0));
		CHECK(m->rules().stats().spawn_fallbacks > fallbacks0);
		CHECK(on_open);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

namespace {

using tdm::VehicleWatch;

float horizontal(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dz * dz);
}

std::optional<uint16_t> tank_cannon(const host::LevelVehicles &p_lv) {
	std::optional<uint16_t> out;
	for (size_t i = 0; i < p_lv.weapons.size(); ++i) {
		out = p_lv.weapons[i].variant == "vehicles/common/weapondata/mbt_cannon_firing" ? uint16_t(i) : out;
	}
	return out;
}

bool carries(const host::LevelVehicleKind &p_kind, uint16_t p_weapon) {
	return std::find(p_kind.weapon_defs.begin(), p_kind.weapon_defs.end(), p_weapon) != p_kind.weapon_defs.end();
}

struct PairCandidate {
	uint16_t spawner = 0;
	sim::Vec3 at{};
};

struct TankPair {
	uint16_t tank = 0;
	uint16_t target = 0;
	float range = INFINITY;
};

std::optional<TankPair> nearest_tank_pair(const host::LevelVehicles &p_lv, uint16_t p_cannon,
		const std::vector<PairCandidate> &p_candidates,
		const std::function<bool(const host::LevelVehicleKind &)> &p_target,
		const std::function<bool(const PairCandidate &, const PairCandidate &)> &p_clear) {
	std::optional<TankPair> out;
	for (const PairCandidate &t : p_candidates) {
		if (!carries(p_lv.kinds[p_lv.spawners[t.spawner].kind], p_cannon)) {
			continue;
		}
		for (const PairCandidate &j : p_candidates) {
			const float range = horizontal(t.at, j.at);
			if (j.spawner == t.spawner || !p_target(p_lv.kinds[p_lv.spawners[j.spawner].kind]) || range < 15.0f ||
					range > 120.0f || (out && range >= out->range) || !p_clear(t, j)) {
				continue;
			}
			out = TankPair{ t.spawner, j.spawner, range };
		}
	}
	return out;
}

std::array<float, 2> heading_of(const std::array<float, 4> &p_rotation) {
	const std::array<float, 4> &q = p_rotation;
	const float x = 2.0f * (q[0] * q[2] + q[3] * q[1]), z = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
	const float n = std::sqrt(x * x + z * z);
	return { x / n, z / n };
}

float speed_of(const sim::Vec3 &p_velocity) {
	const sim::Vec3 &v = p_velocity;
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

bool parked_upright(const std::array<float, 4> &p_rotation, const sim::Vec3 &p_velocity) {
	return tdm::up_y_of(p_rotation) > 0.98f && speed_of(p_velocity) < 0.05f;
}

float clear_run(const sim::CollisionWorld &p_world, const sim::Vec3 &p_from, const std::array<float, 4> &p_rotation,
		std::span<const sim::Vec3> p_vehicles, float p_length) {
	const std::array<float, 2> f = heading_of(p_rotation);
	for (const sim::Vec3 &o : p_vehicles) {
		const float dx = o[0] - p_from[0], dz = o[2] - p_from[2];
		const float along = dx * f[0] + dz * f[1];
		if (along > 1.0f && std::fabs(dx * f[1] - dz * f[0]) < 4.0f) {
			p_length = std::min(p_length, std::max(along - 5.0f, 0.0f));
		}
	}
	sim::StaticHit hit;
	if (!p_world.ground_below(p_from, 10.0f, sim::query_mask::kCollision, &hit)) {
		return 0.0f;
	}
	float ground = hit.position[1];
	for (float d = 1.0f; d <= p_length; d += 1.0f) {
		for (const float lateral : { -1.3f, 0.0f, 1.3f }) {
			for (const float height : { 0.7f, 1.6f }) {
				const sim::Vec3 a{ p_from[0] + f[0] * (d - 1.0f) + f[1] * lateral, ground + height,
					p_from[2] + f[1] * (d - 1.0f) - f[0] * lateral };
				if (p_world.ray_any(a, { f[0], 0.0f, f[1] }, sim::query_mask::kCollision)) {
					return d - 1.0f;
				}
			}
		}
		const sim::Vec3 next{ p_from[0] + f[0] * d, ground + 2.0f, p_from[2] + f[1] * d };
		if (!p_world.ground_below(next, 4.0f, sim::query_mask::kCollision, &hit) ||
				std::fabs(hit.position[1] - ground) > 0.3f) {
			return d - 1.0f;
		}
		ground = hit.position[1];
	}
	return p_length;
}

}

TEST_SUITE("host-vehicles") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: the Growlers and VDV Buggies spawn from the Conquest spawners their "
			  "gates open, a soldier "
			  "enters one with the use key, drives it on the BF3 gearbox and engine rules, the state goes out at full "
			  "precision, and he gets out") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::SharedLevel &shared = *level->shared;
		REQUIRE(shared.vehicles != nullptr);
		const host::LevelVehicles &lv = *shared.vehicles;
		for (const std::string &n : lv.notes) {
			std::printf("vehicles: %s\n", n.c_str());
		}
		std::optional<uint16_t> jeep_kind;
		for (size_t k = 0; k < lv.kinds.size(); ++k) {
			std::printf(
					"vehicles: kind %zu %s, %u seats, entry radius %.2f m, gearbox type %d mode %d, %u forward gears\n",
					k, lv.kinds[k].blueprint.c_str(), unsigned(lv.kinds[k].seats), double(lv.kinds[k].entry_radius),
					int(lv.kinds[k].model->land.gearbox_type), int(lv.kinds[k].model->land.gearbox_mode),
					unsigned(lv.kinds[k].model->land.forward_gears));
			if (lv.kinds[k].blueprint == "vehicles/growleritv/growleritv") {
				jeep_kind = uint16_t(k);
			}
		}
		REQUIRE(jeep_kind.has_value());
		size_t growlers = 0;
		for (const host::LevelVehicleSpawner &s : lv.spawners) {
			growlers += s.kind == *jeep_kind ? 1u : 0u;
		}
		std::printf("vehicles: %zu vehicle spawners, %zu of them Growlers\n", lv.spawners.size(), growlers);
		CHECK(lv.kinds.size() == 4);
		CHECK(lv.spawners.size() == 12);
		CHECK(growlers == 3);

		std::unique_ptr<Match> m = start_match(*level, false, 61u, 0, 1);
		tdm::ScriptedPeer &p = m->join("driver");
		p.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 600));
		const sim::logic::ConquestNatives &cq = conquest_of(*m);

		VehicleWatch watch;
		float latest_delay = 0.0f;
		for (const host::LevelVehicleSpawner &s : lv.spawners) {
			latest_delay = std::max(latest_delay, s.initial_spawn_delay);
		}
		const uint32_t wait_ticks = sim::seconds_to_ticks(latest_delay) + 120u;
		size_t gated_shut = 0, gated_seen = 0;
		for (uint32_t t = 0; t < wait_ticks; ++t) {
			m->tick();
			watch.take(m->out, p.peer());
			for (size_t i = 0; i < lv.spawners.size(); ++i) {
				const std::optional<uint8_t> gate = cq.vehicle_spawner_team(lv.spawners[i].object);
				if (gate && *gate == 0) {
					++gated_shut;
					gated_seen += watch.of(uint16_t(i)) != nullptr ? 1u : 0u;
				}
			}
		}
		CHECK(watch.bad == 0);
		CHECK(watch.messages > 0);
		CHECK(gated_seen == 0);
		size_t live_growlers = 0, upright = 0;
		const net::VehicleWire *target = nullptr;
		float target_clear = 0.0f;
		for (const net::VehicleWire &w : watch.latest) {
			REQUIRE(w.spawner < lv.spawners.size());
			const host::LevelVehicleSpawner &s = lv.spawners[w.spawner];
			CHECK(w.generation == 1);
			CHECK(w.state.wheel_count == lv.kinds[s.kind].model->wheel_count);
			CHECK(horizontal(w.state.body.com_position, s.placement.position) < 3.0f);
			const bool at_rest = parked_upright(w.state.body.rotation, w.state.body.linear_velocity);
			upright += at_rest ? 1u : 0u;
			if (s.kind == *jeep_kind && at_rest) {
				++live_growlers;
				std::vector<sim::Vec3> others;
				for (const net::VehicleWire &o : watch.latest) {
					others.push_back(o.state.body.com_position);
				}
				const float clear =
						clear_run(*m->world, w.state.body.com_position, w.state.body.rotation, others, 120.0f);
				std::printf("vehicles: Growler of spawner %u, parked upright, has %.0f m clear ahead\n",
						unsigned(w.spawner), double(clear));
				if (clear > target_clear) {
					target = &w;
					target_clear = clear;
				}
			}
		}
		std::printf("vehicles: %zu vehicles live after %u ticks (%zu Growlers, %zu vehicles parked upright at rest); "
					"flag-gated spawner ticks shut %zu\n",
				watch.latest.size(), wait_ticks, live_growlers, upright, gated_shut);
		REQUIRE(target != nullptr);
		REQUIRE(target_clear >= 30.0f);
		const uint16_t spawner = target->spawner;
		const sim::Vec3 parked = target->state.body.com_position;

		const float radius = lv.kinds[*jeep_kind].entry_radius;
		REQUIRE(radius > 0.5f);
		const sim::Vec3 beside = ground_at(*m->world, parked[0] + 0.6f * radius, parked[2], parked[1] + 5.0f);
		m->teleport(p, beside);
		REQUIRE(m->until([&] { return horizontal(m->state(p.slot())->position, beside) < 0.5f; }, 30));
		const sim::Tick use_at = m->now() + 2u;
		int8_t forward = 0;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			if (p_tick == use_at) {
				c.buttons |= sim::buttons::kUse;
			}
			c.move_forward = forward;
			return c;
		};
		const uint8_t slot = uint8_t(p.slot());
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, p.peer());
					const net::VehicleWire *w = watch.of(spawner);
					return w != nullptr && w->occupants[0] == slot;
				},
				60));
		const sim::SoldierState *seated = m->state(p.slot());
		REQUIRE(seated != nullptr);
		CHECK(horizontal(seated->position, watch.of(spawner)->state.body.com_position) < 3.0f);

		const sim::LandDriveModel &land = lv.kinds[*jeep_kind].model->land;
		const auto upshift_speed = [&](int32_t p_gear) {
			if (p_gear < 1) {
				return 0.0f;
			}
			return p_gear <= land.forward_speed_count ? land.forward_speeds[size_t(p_gear - 1)] / 3.6f : 1.0e9f;
		};
		const net::VehicleWire *w = watch.of(spawner);
		const sim::Vec3 start = w->state.body.com_position;
		const std::array<float, 2> line = heading_of(w->state.body.rotation);
		const auto along = [&](const sim::Vec3 &p_at) {
			return (p_at[0] - start[0]) * line[0] + (p_at[2] - start[2]) * line[1];
		};
		const auto off_line = [&](const sim::Vec3 &p_at) {
			return std::fabs((p_at[0] - start[0]) * line[1] - (p_at[2] - start[2]) * line[0]);
		};
		uint64_t seen = watch.messages;
		float previous_speed = speed_of(w->state.body.linear_velocity), lowest_up = 1.0f, widest = 0.0f;
		int32_t gear = w->state.driver.gearbox.gear;
		const int32_t first_gear = gear;
		CHECK(first_gear >= 0);
		CHECK(first_gear <= 1);
		std::vector<std::pair<int32_t, float>> upshifts;
		forward = 127;
		uint32_t throttle_ticks = 0;
		for (; gear < 3 && throttle_ticks < 600; ++throttle_ticks) {
			m->tick();
			watch.take(m->out, p.peer());
			w = watch.of(spawner);
			REQUIRE(w != nullptr);
			if (watch.messages == seen) {
				continue;
			}
			seen = watch.messages;
			const float speed = speed_of(w->state.body.linear_velocity);
			const int32_t now_gear = w->state.driver.gearbox.gear;
			CHECK(w->occupants[0] == slot);
			CHECK_MESSAGE(
					speed >= previous_speed - 0.3f, "tick " << m->now() << ": " << previous_speed << " -> " << speed);
			CHECK(now_gear >= gear);
			CHECK(now_gear <= gear + 1);
			if (now_gear > gear) {
				upshifts.emplace_back(now_gear, speed);
				CHECK_MESSAGE(speed >= upshift_speed(gear), "into gear " << now_gear << " at " << speed << " m/s");
			}
			CHECK_MESSAGE(speed <= upshift_speed(now_gear) + 0.5f, "gear " << now_gear << " at " << speed << " m/s");
			CHECK(w->state.driver.engine_rpm >= 0.0f);
			CHECK(w->state.driver.engine_rpm <= land.engine.rpm_max);
			lowest_up = std::min(lowest_up, tdm::up_y_of(w->state.body.rotation));
			widest = std::max(widest, off_line(w->state.body.com_position));
			previous_speed = speed;
			gear = now_gear;
		}
		const float top_speed = previous_speed;
		const float run = along(w->state.body.com_position);
		for (const auto &[g, s] : upshifts) {
			std::printf("vehicles: into gear %d at %.1f km/h\n", int(g), double(3.6f * s));
		}
		std::printf("vehicles: the Growler reached gear %d at %.1f km/h in %.2f s and %.1f m, %.2f m off its line, "
					"up axis y >= %.3f, rpm %.0f\n",
				int(gear), double(3.6f * top_speed), double(float(throttle_ticks) / 60.0f), double(run), double(widest),
				double(lowest_up), double(w->state.driver.engine_rpm));
		REQUIRE(gear == 3);
		CHECK(upshifts.size() == size_t(3 - first_gear));
		CHECK(run < target_clear - 15.0f);
		CHECK(widest < 1.5f);
		CHECK(lowest_up > 0.95f);
		CHECK(horizontal(m->state(p.slot())->position, w->state.body.com_position) < 3.0f);
		CHECK(is_alive(m->state(p.slot())));

		const auto forward_speed = [&](const sim::Vec3 &p_velocity) {
			return p_velocity[0] * line[0] + p_velocity[2] * line[1];
		};
		const float turn_speed = land.opposite_dir_gear_change_max_speed / 3.6f;
		previous_speed = forward_speed(w->state.body.linear_velocity);
		forward = -127;
		uint32_t brake_ticks = 0;
		for (; previous_speed > 0.3f && brake_ticks < 600; ++brake_ticks) {
			m->tick();
			watch.take(m->out, p.peer());
			w = watch.of(spawner);
			if (watch.messages == seen) {
				continue;
			}
			seen = watch.messages;
			const float speed = forward_speed(w->state.body.linear_velocity);
			const int32_t now_gear = w->state.driver.gearbox.gear;
			CHECK_MESSAGE(
					speed <= previous_speed + 0.05f, "tick " << m->now() << ": " << previous_speed << " -> " << speed);
			CHECK(now_gear <= gear);
			CHECK(now_gear >= -1);
			CHECK((now_gear >= 1 || speed < turn_speed + 0.5f));
			lowest_up = std::min(lowest_up, tdm::up_y_of(w->state.body.rotation));
			widest = std::max(widest, off_line(w->state.body.com_position));
			previous_speed = speed;
			gear = now_gear;
		}
		const float deceleration = (top_speed - previous_speed) / (float(brake_ticks) / 60.0f);
		std::printf(
				"vehicles: the back pedal brought it from %.1f to %.1f km/h in %.2f s (%.1f m/s^2), gear %d, %.1f m "
				"along its line, %.2f m off it, up axis y >= %.3f\n",
				double(3.6f * top_speed), double(3.6f * previous_speed), double(float(brake_ticks) / 60.0f),
				double(deceleration), int(gear), double(along(w->state.body.com_position)), double(widest),
				double(lowest_up));
		CHECK(previous_speed <= 0.3f);
		CHECK(deceleration > 4.0f);
		CHECK(along(w->state.body.com_position) < target_clear);
		CHECK(widest < 1.5f);
		CHECK(lowest_up > 0.95f);
		forward = 0;
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, p.peer());
					return speed_of(watch.of(spawner)->state.body.linear_velocity) < 0.1f;
				},
				120));
		CHECK(tdm::up_y_of(watch.of(spawner)->state.body.rotation) > 0.95f);
		const sim::Tick exit_at = m->now() + 2u;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			if (p_tick == exit_at) {
				c.buttons |= sim::buttons::kUse;
			}
			return c;
		};
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, p.peer());
					const net::VehicleWire *w = watch.of(spawner);
					return w != nullptr && w->occupants[0] == net::kNoVehicleOccupant;
				},
				60));
		m->ticks(60);
		watch.take(m->out, p.peer());
		const sim::SoldierState *out = m->state(p.slot());
		REQUIRE(out != nullptr);
		CHECK(is_alive(out));
		const float away = horizontal(out->position, watch.of(spawner)->state.body.com_position);
		std::printf("vehicles: out of the Growler %.2f m from its centre\n", double(away));
		CHECK(away > 1.0f);
		CHECK(away < 6.0f);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-vehicles-scope") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: a soldier's client gets each parked vehicle's state once every 15 "
			  "snapshots, an asleep vehicle with a driver in at its distance tier, and the vehicle he drives every "
			  "snapshot") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::unique_ptr<Match> m = start_match(*level, false, 66u, 0, 1);
		tdm::ScriptedPeer &p = m->join("viewer");
		tdm::ScriptedPeer &o = m->join("observer");
		p.ready();
		o.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())) && is_alive(m->state(o.slot())); }, 600));
		VehicleWatch watch;
		std::map<uint16_t, uint32_t> updates, observed;
		uint32_t snapshots = 0;
		const auto run = [&](uint32_t p_ticks) {
			for (uint32_t t = 0; t < p_ticks; ++t) {
				m->tick();
				snapshots += m->now() % 2u == 0u ? 1u : 0u;
				for (const host::OutDatagram &d : m->out) {
					if ((d.peer != p.peer() && d.peer != o.peer()) ||
							net::peek_type(d.bytes) != net::MessageType::VehicleStates) {
						continue;
					}
					core::Result<net::VehicleStates> s = net::decode_vehicle_states(d.bytes);
					REQUIRE(s.ok());
					for (const net::VehicleWire &w : s->vehicles) {
						++(d.peer == p.peer() ? updates : observed)[w.spawner];
					}
				}
				watch.take(m->out, p.peer());
			}
		};
		run(sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 240u);
		updates.clear();
		snapshots = 0;
		run(300);
		size_t parked = 0;
		uint32_t most = 0, fewest = UINT32_MAX;
		for (const net::VehicleWire &w : watch.latest) {
			if (w.state.body.active != 0) {
				continue;
			}
			++parked;
			most = std::max(most, updates[w.spawner]);
			fewest = std::min(fewest, updates[w.spawner]);
		}
		std::printf("vehicles: over %u snapshots the viewer got %zu parked vehicles' states %u to %u times each\n",
				unsigned(snapshots), parked, unsigned(fewest), unsigned(most));
		REQUIRE(parked >= 6u);
		CHECK(most <= snapshots / host::kRestingVehicleSnapshots + 1u);
		CHECK(fewest >= snapshots / host::kRestingVehicleSnapshots - 1u);

		const net::VehicleWire *target = nullptr;
		for (const net::VehicleWire &w : watch.latest) {
			if (lv.kinds[lv.spawners[w.spawner].kind].blueprint == "vehicles/growleritv/growleritv" &&
					parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
				target = &w;
				break;
			}
		}
		REQUIRE(target != nullptr);
		const uint16_t spawner = target->spawner;
		const sim::Vec3 com = target->state.body.com_position;
		const float radius = lv.kinds[lv.spawners[spawner].kind].entry_radius;
		m->teleport(p, ground_at(*m->world, com[0] + 0.6f * radius, com[2], com[1] + 5.0f));
		m->ticks(30);
		const sim::Tick use_at = m->now() + 2u;
		int8_t forward = 0;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			if (p_tick == use_at) {
				c.buttons |= sim::buttons::kUse;
			}
			c.move_forward = forward;
			return c;
		};
		const uint8_t slot = uint8_t(p.slot());
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, p.peer());
					const net::VehicleWire *w = watch.of(spawner);
					return w != nullptr && w->occupants[0] == slot;
				},
				60));
		const sim::Vec3 crewed = watch.of(spawner)->state.body.com_position;
		m->teleport(o, ground_at(*m->world, crewed[0] - 0.6f * radius, crewed[2] + 6.0f, crewed[1] + 5.0f));
		m->ticks(30);
		observed.clear();
		snapshots = 0;
		run(300);
		const net::VehicleWire *at_rest = watch.of(spawner);
		REQUIRE(at_rest != nullptr);
		std::printf("vehicles: the Growler at rest with its driver in (asleep %d) reached the observer %u times in %u "
					"snapshots\n",
				int(at_rest->state.body.active == 0), unsigned(observed[spawner]), unsigned(snapshots));
		CHECK(at_rest->occupants[0] == slot);
		CHECK(at_rest->state.body.active == 0);
		CHECK(observed[spawner] + 1u >= snapshots / 2u);
		forward = 127;
		updates.clear();
		snapshots = 0;
		run(120);
		std::printf("vehicles: driving, the viewer got his Growler's state %u times in %u snapshots\n",
				unsigned(updates[spawner]), unsigned(snapshots));
		CHECK(updates[spawner] == snapshots);
		CHECK(watch.bad == 0);
	}
}

TEST_SUITE("host-vehicles-net") {
	TEST_CASE("vehicles: two clients on MP_007 ConquestSmall0: the second client's ClientVehicles shows the first one "
			  "driving a Growler") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::unique_ptr<Match> m = start_match(*level, false, 62u, 0, 1);
		tdm::ScriptedPeer &driver = m->join("driver");
		tdm::ScriptedPeer &watcher = m->join("watcher");
		driver.ready();
		watcher.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until([&] { return is_alive(m->state(driver.slot())) && is_alive(m->state(watcher.slot())); }, 600));

		host::ClientVehicles seen;
		auto frame = std::make_unique<host::PresentationFrame>();
		const double render_delay_ticks =
				double(host::ClientSessionConfig{}.interp_min_ms) / (1000.0 * double(sim::kTickSeconds));
		const auto feed = [&] {
			for (const host::OutDatagram &d : m->out) {
				if (d.peer == watcher.peer() && net::peek_type(d.bytes) == net::MessageType::VehicleStates) {
					seen.receive(d.bytes);
				}
			}
			frame->render_ticks_at_publish = double(m->now()) - render_delay_ticks;
			seen.publish(*frame);
		};
		const auto growler = [&]() -> const host::PresentedVehicle * {
			std::vector<sim::Vec3> others;
			for (uint8_t i = 0; i < frame->vehicle_count; ++i) {
				others.push_back(frame->vehicles[i].com_position);
			}
			const host::PresentedVehicle *best = nullptr;
			float best_clear = 0.0f;
			for (uint8_t i = 0; i < frame->vehicle_count; ++i) {
				const host::PresentedVehicle &v = frame->vehicles[i];
				if (v.spawner < lv.spawners.size() && parked_upright(v.rotation, v.linear_velocity) &&
						lv.kinds[lv.spawners[v.spawner].kind].blueprint == "vehicles/growleritv/growleritv") {
					const float clear = clear_run(*m->world, v.com_position, v.rotation, others, 30.0f);
					if (clear > best_clear) {
						best = &v;
						best_clear = clear;
					}
				}
			}
			return best_clear >= 30.0f ? best : nullptr;
		};
		float latest_delay = 0.0f;
		for (const host::LevelVehicleSpawner &s : lv.spawners) {
			latest_delay = std::max(latest_delay, s.initial_spawn_delay);
		}
		REQUIRE(m->until(
				[&] {
					feed();
					return frame->vehicle_count == lv.spawners.size();
				},
				sim::seconds_to_ticks(latest_delay) + 120u));
		m->until(
				[&] {
					feed();
					return false;
				},
				120);
		REQUIRE(growler() != nullptr);
		const host::PresentedVehicle parked = *growler();
		const float radius = lv.kinds[lv.spawners[parked.spawner].kind].entry_radius;
		const sim::Vec3 beside = ground_at(*m->world, parked.com_position[0] + 0.6f * radius, parked.com_position[2],
				parked.com_position[1] + 5.0f);
		m->teleport(driver, beside);
		REQUIRE(m->until([&] { return horizontal(m->state(driver.slot())->position, beside) < 0.5f; }, 30));
		const sim::Tick use_at = m->now() + 2u;
		int8_t forward = 0;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			if (p_tick == use_at) {
				c.buttons |= sim::buttons::kUse;
			}
			c.move_forward = forward;
			return c;
		};
		const uint8_t slot = uint8_t(driver.slot());
		const auto driven = [&]() -> const host::PresentedVehicle * {
			for (uint8_t i = 0; i < frame->vehicle_count; ++i) {
				if (frame->vehicles[i].spawner == parked.spawner) {
					return &frame->vehicles[i];
				}
			}
			return nullptr;
		};
		REQUIRE(m->until(
				[&] {
					feed();
					return driven() != nullptr && driven()->occupants[0] == slot;
				},
				60));
		forward = 127;
		m->until(
				[&] {
					feed();
					return false;
				},
				120);
		const host::PresentedVehicle *after = driven();
		REQUIRE(after != nullptr);
		const std::array<float, 2> line = heading_of(parked.rotation);
		const float dx = after->com_position[0] - parked.com_position[0],
					dz = after->com_position[2] - parked.com_position[2];
		const float moved = dx * line[0] + dz * line[1];
		const float off_line = std::fabs(dx * line[1] - dz * line[0]);
		const float kmh = 3.6f *
				std::sqrt(after->linear_velocity[0] * after->linear_velocity[0] +
						after->linear_velocity[2] * after->linear_velocity[2]);
		std::printf("vehicles: the watcher saw the driver's Growler move %.1f m along its line (%.2f m off it) in 2 s, "
					"%.1f km/h, gear %d, rpm %.0f, server tick %u\n",
				double(moved), double(off_line), double(kmh), int(after->gear), double(after->engine_rpm),
				unsigned(after->tick));
		CHECK(moved > 8.0f);
		CHECK(off_line < 1.5f);
		CHECK(kmh > 25.0f);
		CHECK(after->gear >= 2);
		CHECK(after->occupants[0] == slot);
		CHECK(after->tick + 4u >= m->now());
		REQUIRE(after->window_count >= 2);
		const host::PresentedVehicleSample &early = after->window[after->window_count - 2];
		const host::PresentedVehicleSample &late = after->window[after->window_count - 1];
		REQUIRE(early.tick < late.tick);
		const double mid_tick = 0.5 * double(early.tick + late.tick);
		const host::PresentedVehiclePose mid = host::vehicle_pose_at(*after, mid_tick);
		CHECK(mid.com_position[0] ==
				doctest::Approx(0.5f * (early.pose.com_position[0] + late.pose.com_position[0])).epsilon(1e-4));
		CHECK(host::vehicle_pose_at(*after, double(early.tick)).com_position[2] ==
				doctest::Approx(early.pose.com_position[2]));
		const host::PresentedVehiclePose ahead = host::vehicle_pose_at(*after, double(after->tick) + 100.0);
		const float ahead_x = after->com_position[0] +
				after->linear_velocity[0] * float(host::kVehicleExtrapolateTicks) * sim::kTickSeconds;
		CHECK(ahead.com_position[0] == doctest::Approx(ahead_x).epsilon(1e-4));
		REQUIRE(after->wheel_count > 0);
		CHECK(std::fabs(after->wheel_spin[0]) > 1.0f);
		CHECK(ahead.wheel_angle[0] ==
				doctest::Approx(after->wheel_angle[0] +
						after->wheel_spin[0] * float(host::kVehicleExtrapolateTicks) * sim::kTickSeconds)
						.epsilon(1e-4));
		uint32_t still = 0, interpolated = 0;
		float drawn_before = 0.0f;
		for (uint32_t t = 0; t < 30; ++t) {
			m->tick();
			feed();
			const host::PresentedVehicle *v = driven();
			REQUIRE(v != nullptr);
			const double render = frame->render_ticks_at_publish;
			const host::PresentedVehiclePose pose = host::vehicle_pose_at(*v, render);
			const float drawn = pose.com_position[0] * line[0] + pose.com_position[2] * line[1];
			bool on_sample = std::fabs(render - double(v->tick)) < 1e-6;
			for (uint8_t k = 0; k < v->window_count; ++k) {
				on_sample = on_sample || std::fabs(render - double(v->window[k].tick)) < 1e-6;
			}
			interpolated +=
					!on_sample && render < double(v->tick) && v->window_count > 0 && render > double(v->window[0].tick)
					? 1u
					: 0u;
			if (t > 0 && std::fabs(drawn - drawn_before) < 1e-4f) {
				++still;
			}
			drawn_before = drawn;
		}
		std::printf("vehicles: at the client's render time (%.1f ticks behind the server) the watcher drew the Growler "
					"between two received states on %u of 30 ticks and left it still on %u of 29 steps\n",
				render_delay_ticks, unsigned(interpolated), unsigned(still));
		CHECK(still == 0u);
		CHECK(interpolated >= 10u);
		host::ClientStats stats;
		seen.stats(stats);
		CHECK(stats.vehicle_messages > 0);
		CHECK(stats.bad_vehicle_messages == 0);
	}
}

TEST_SUITE("host-vehicles-seats") {
	TEST_CASE("vehicles: a seated soldier moves between the Growler's seats with the seat keys in entry order, a taken "
			  "seat refuses him, and the held handbrake bit holds the handbrake") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::unique_ptr<Match> m = start_match(*level, false, 63u, 0, 1);
		tdm::ScriptedPeer &p = m->join("rider");
		tdm::ScriptedPeer &q = m->join("passenger");
		p.ready();
		q.ready();
		REQUIRE(wait_in_round(*m));
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())) && is_alive(m->state(q.slot())); }, 600));
		VehicleWatch watch;
		float latest_delay = 0.0f;
		for (const host::LevelVehicleSpawner &s : lv.spawners) {
			latest_delay = std::max(latest_delay, s.initial_spawn_delay);
		}
		for (uint32_t t = 0; t < sim::seconds_to_ticks(latest_delay) + 120u; ++t) {
			m->tick();
			watch.take(m->out, p.peer());
		}
		const net::VehicleWire *target = nullptr;
		for (const net::VehicleWire &w : watch.latest) {
			const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[w.spawner].kind];
			if (kind.blueprint == "vehicles/growleritv/growleritv" &&
					parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
				target = &w;
				break;
			}
		}
		REQUIRE(target != nullptr);
		const uint16_t spawner = target->spawner;
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[spawner].kind];
		REQUIRE(kind.seats >= 3);
		const sim::Vec3 parked = target->state.body.com_position;
		const sim::Vec3 beside =
				ground_at(*m->world, parked[0] + 0.6f * kind.entry_radius, parked[2], parked[1] + 5.0f);
		m->teleport(p, beside);
		m->teleport(q, beside);
		REQUIRE(m->until([&] { return horizontal(m->state(p.slot())->position, beside) < 0.5f; }, 30));
		uint32_t rider = 0, passenger = 0;
		int8_t passenger_forward = 0;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			c.buttons |= rider;
			return c;
		};
		m->scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			c.buttons |= passenger;
			c.move_forward = passenger_forward;
			return c;
		};
		const uint8_t ps = uint8_t(p.slot()), qs = uint8_t(q.slot());
		const auto seats_are = [&](std::initializer_list<std::pair<uint8_t, uint8_t>> p_want) {
			watch.take(m->out, p.peer());
			const net::VehicleWire *w = watch.of(spawner);
			if (w == nullptr) {
				return false;
			}
			for (const auto &[seat, slot] : p_want) {
				if (w->occupants[seat] != slot) {
					return false;
				}
			}
			return true;
		};
		const auto run = [&](uint32_t p_ticks) {
			for (uint32_t t = 0; t < p_ticks; ++t) {
				m->tick();
				watch.take(m->out, p.peer());
			}
		};
		const auto press = [&](uint32_t &r_buttons, uint32_t p_buttons) {
			r_buttons = p_buttons;
			run(4);
			r_buttons = 0;
			run(4);
		};
		press(rider, sim::buttons::kUse);
		REQUIRE(m->until([&] { return seats_are({ { 0, ps } }); }, 60));
		press(passenger, sim::buttons::kUse);
		REQUIRE(m->until([&] { return seats_are({ { 0, ps }, { 1, qs } }); }, 60));

		press(rider, sim::vehicles::with_seat_request(0, uint8_t(1)));
		run(10);
		CHECK_MESSAGE(seats_are({ { 0, ps }, { 1, qs } }), "the gunner seat is taken: the driver stays");
		press(rider, sim::vehicles::with_seat_request(0, uint8_t(2)));
		CHECK(m->until([&] { return seats_are({ { 0, net::kNoVehicleOccupant }, { 1, qs }, { 2, ps } }); }, 30));
		press(passenger, sim::vehicles::with_seat_request(0, uint8_t(0)));
		CHECK(m->until([&] { return seats_are({ { 0, qs }, { 1, net::kNoVehicleOccupant }, { 2, ps } }); }, 30));
		press(rider, sim::vehicles::with_seat_request(0, uint8_t(kind.seats)));
		run(10);
		CHECK_MESSAGE(seats_are({ { 0, qs }, { 2, ps } }), "a seat key past the vehicle's seats does nothing");

		const sim::VehicleModel &model = *kind.model;
		const auto wheel_spin = [&] {
			float spin = 0.0f;
			const net::VehicleWire *w = watch.of(spawner);
			for (uint8_t i = 0; i < model.wheel_count && i < w->state.wheel_count; ++i) {
				spin += std::fabs(w->state.wheels[i].angular_velocity);
			}
			return spin;
		};
		passenger_forward = 127;
		passenger = sim::vehicles::seat_buttons::kHandbrake;
		run(20);
		const float held = wheel_spin();
		passenger = 0;
		run(20);
		const float free = wheel_spin();
		passenger_forward = 0;
		std::printf("vehicles: the wheels spin %.2f rad/s in all with the handbrake bit held under throttle, %.2f "
					"without\n",
				double(held), double(free));
		CHECK(held < 0.25f * free);
		std::printf(
				"vehicles: the Growler of spawner %u has %u seats; driver to seat 3, passenger to the driver seat\n",
				unsigned(spawner), unsigned(kind.seats));

		run(60);
		const auto rotate = [](const std::array<float, 4> &p_q, const core::Vec3 &p_v) {
			const float x = p_q[0], y = p_q[1], z = p_q[2], w = p_q[3];
			const float tx = 2.0f * (y * p_v[2] - z * p_v[1]), ty = 2.0f * (z * p_v[0] - x * p_v[2]),
						tz = 2.0f * (x * p_v[1] - y * p_v[0]);
			return sim::Vec3{ p_v[0] + w * tx + (y * tz - z * ty), p_v[1] + w * ty + (z * tx - x * tz),
				p_v[2] + w * tz + (x * ty - y * tx) };
		};
		const auto in_vehicle = [&](const core::Vec3 &p_local) {
			const net::VehicleWire *w = watch.of(spawner);
			const sim::Vec3 com = rotate(w->state.body.rotation, model.center_of_mass);
			const sim::Vec3 at = rotate(w->state.body.rotation, p_local);
			return sim::Vec3{ w->state.body.com_position[0] - com[0] + at[0],
				w->state.body.com_position[1] - com[1] + at[1], w->state.body.com_position[2] - com[2] + at[2] };
		};
		const auto distance = [](const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
			return std::sqrt((p_a[0] - p_b[0]) * (p_a[0] - p_b[0]) + (p_a[1] - p_b[1]) * (p_a[1] - p_b[1]) +
					(p_a[2] - p_b[2]) * (p_a[2] - p_b[2]));
		};
		REQUIRE(kind.seat_defs.size() >= 3);
		const sim::Vec3 driver_seat = in_vehicle(kind.seat_defs[0].transform.trans);
		const sim::Vec3 rear_seat = in_vehicle(kind.seat_defs[2].transform.trans);
		const float driver_off = distance(m->state(q.slot())->position, driver_seat);
		const float rear_off = distance(m->state(p.slot())->position, rear_seat);
		const std::optional<core::Vec3> exit_local = host::vehicle_exit_point(kind, 2);
		REQUIRE(exit_local.has_value());
		const sim::Vec3 exit_at = in_vehicle(*exit_local);
		press(rider, sim::buttons::kUse);
		REQUIRE(m->until([&] { return seats_are({ { 2, net::kNoVehicleOccupant } }); }, 30));
		const float exit_off = horizontal(m->state(p.slot())->position, exit_at);
		std::printf("vehicles: seated soldiers ride %.3f m (driver) and %.3f m (seat 3) from body * seat frame; the "
					"rider got out %.3f m from seat 3's nearest exit point (%.2f %.2f %.2f)\n",
				double(driver_off), double(rear_off), double(exit_off), double((*exit_local)[0]),
				double((*exit_local)[1]), double((*exit_local)[2]));
		CHECK(driver_off < 0.05f);
		CHECK(rear_off < 0.05f);
		CHECK(exit_off < 0.25f);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("vehicles: SharedLoadOptions::vehicle_tuning reaches MP_007 Conquest's level vehicles") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		core::Cache cache;
		core::CacheOptions co;
		co.root = level->cache_root;
		co.guard = level->reader->write_guard();
		std::string detail;
		REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
		host::SharedLoadOptions options;
		options.cache = &cache;
		options.tuning = tg::shipped_motor_tuning();
		options.tdm = true;
		options.tdm_tuning_cfg = tdm::detail::default_tdm_tuning_cfg();
		options.vehicle_tuning.hand_brake_torque = 2.0f * sim::VehicleTuning{}.hand_brake_torque;
		core::Result<host::LevelDatabase> db =
				host::open_level_database(level->install, "MP_007", "ConquestSmall0", 0, nullptr);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
		core::Result<std::shared_ptr<const host::SharedLevel>> shared = host::load_shared_level(*db, options, nullptr);
		REQUIRE_MESSAGE(shared.ok(), (shared.ok() ? std::string() : shared.error().message));
		cache.close();
		REQUIRE((*shared)->vehicles != nullptr);
		const uint64_t given = sim::vehicle_tuning_hash(options.vehicle_tuning);
		const uint64_t loaded = sim::vehicle_tuning_hash((*shared)->vehicles->tuning);
		const uint64_t defaults = sim::vehicle_tuning_hash(sim::VehicleTuning{});
		std::printf("vehicles: a load given hand_brake_torque %.0f carries tuning %016llx (given %016llx, defaults "
					"%016llx)\n",
				double(options.vehicle_tuning.hand_brake_torque), (unsigned long long)loaded, (unsigned long long)given,
				(unsigned long long)defaults);
		CHECK(loaded == given);
		CHECK(loaded != defaults);
		CHECK((*shared)->vehicles->tuning.hand_brake_torque ==
				doctest::Approx(options.vehicle_tuning.hand_brake_torque));
	}
}

TEST_SUITE("host-vehicles-armor") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: a soldier drives a tank on its tracks, its turret follows his aim "
			  "on the turret rules and its cannon's shell kills an enemy soldier off its bow") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		for (const std::string &n : lv.notes) {
			std::printf("armor: %s\n", n.c_str());
		}
		for (size_t i = 0; i < lv.weapons.size(); ++i) {
			std::printf("armor: weapon %zu %s, rate %.0f, magazine %d\n", i, lv.weapons[i].variant.c_str(),
					double(lv.weapons[i].fire.rate_of_fire), int(lv.weapons[i].fire.magazine_capacity));
		}
		const std::optional<uint16_t> cannon_def = tank_cannon(lv);
		REQUIRE(cannon_def.has_value());
		std::vector<uint16_t> tank_kinds;
		for (size_t k = 0; k < lv.kinds.size(); ++k) {
			const host::LevelVehicleKind &kind = lv.kinds[k];
			std::printf("armor: kind %zu %s, %u wheels, tracked %d, %zu turrets, %zu weapons\n", k,
					kind.blueprint.c_str(), unsigned(kind.model->wheel_count), int(host::land_tracked(*kind.model)),
					kind.armament ? kind.armament->turrets.size() : size_t(0),
					kind.armament ? kind.armament->weapons.size() : size_t(0));
			if (host::land_tracked(*kind.model) && kind.armament != nullptr && carries(kind, *cannon_def)) {
				tank_kinds.push_back(uint16_t(k));
			}
		}
		REQUIRE(!tank_kinds.empty());

		std::unique_ptr<Match> m = start_match(*level, false, 61u, 0, 1);
		tdm::ScriptedPeer &p = m->join("driver");
		p.ready();
		REQUIRE(wait_in_round(*m));
		std::vector<Script *> bots{ m->bot(), m->bot() };
		REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 600));
		const uint8_t team = m->rules().team_of(uint8_t(p.slot()));
		Script *enemy = nullptr;
		for (Script *b : bots) {
			enemy = m->rules().team_of(uint8_t(b->slot)) != team ? b : enemy;
		}
		REQUIRE(enemy != nullptr);
		REQUIRE(m->until([&] { return is_alive(m->state(enemy->slot)); }, 600));

		VehicleWatch watch;
		const net::VehicleWire *tank = nullptr;
		float tank_clear = 0.0f;
		for (uint32_t t = 0; t < 900 && tank == nullptr; ++t) {
			m->tick();
			watch.take(m->out, p.peer());
			if (t < 120) {
				continue;
			}
			std::vector<sim::Vec3> others;
			for (const net::VehicleWire &o : watch.latest) {
				others.push_back(o.state.body.com_position);
			}
			for (const net::VehicleWire &w : watch.latest) {
				const uint16_t kind = lv.spawners[w.spawner].kind;
				if (std::find(tank_kinds.begin(), tank_kinds.end(), kind) == tank_kinds.end() ||
						!parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
					continue;
				}
				const float clear =
						clear_run(*m->world, w.state.body.com_position, w.state.body.rotation, others, 60.0f);
				if (clear > tank_clear) {
					tank = &w;
					tank_clear = clear;
				}
			}
		}
		REQUIRE(tank != nullptr);
		const uint16_t spawner = tank->spawner;
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[spawner].kind];
		std::printf("armor: %s of spawner %u parked upright with %.0f m clear ahead\n", kind.blueprint.c_str(),
				unsigned(spawner), double(tank_clear));
		REQUIRE(tank_clear >= 12.0f);
		CHECK(tank->state.wheel_count == kind.model->wheel_count);

		const sim::Vec3 parked = tank->state.body.com_position;
		const sim::Vec3 beside =
				ground_at(*m->world, parked[0] + 0.6f * kind.entry_radius, parked[2], parked[1] + 5.0f);
		m->teleport(p, beside);
		REQUIRE(m->until([&] { return horizontal(m->state(p.slot())->position, beside) < 0.5f; }, 30));
		const sim::Tick use_at = m->now() + 2u;
		int8_t forward = 0;
		uint16_t aim_yaw = m->state(p.slot())->yaw;
		int16_t aim_pitch = 0;
		bool fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, aim_yaw, aim_pitch, p_state.target_pose);
			if (p_tick == use_at) {
				c.buttons |= sim::buttons::kUse;
			}
			if (fire) {
				c.buttons |= sim::buttons::kFire;
			}
			c.move_forward = forward;
			return c;
		};
		const uint8_t slot = uint8_t(p.slot());
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, p.peer());
					const net::VehicleWire *w = watch.of(spawner);
					return w != nullptr && w->occupants[0] == slot;
				},
				60));

		const sim::Vec3 start = watch.of(spawner)->state.body.com_position;
		const std::array<float, 2> line = heading_of(watch.of(spawner)->state.body.rotation);
		forward = 127;
		m->ticks(120);
		watch.take(m->out, p.peer());
		const net::VehicleWire *w = watch.of(spawner);
		REQUIRE(w != nullptr);
		const float run = (w->state.body.com_position[0] - start[0]) * line[0] +
				(w->state.body.com_position[2] - start[2]) * line[1];
		std::printf("armor: 2 s of throttle took it %.1f m along its bow at %.1f km/h, gear %d, rpm %.0f, up y %.3f\n",
				double(run), double(3.6f * speed_of(w->state.body.linear_velocity)), int(w->state.driver.gearbox.gear),
				double(w->state.driver.engine_rpm), double(tdm::up_y_of(w->state.body.rotation)));
		CHECK(run > 1.0f);
		CHECK(run < tank_clear);
		CHECK(tdm::up_y_of(w->state.body.rotation) > 0.95f);
		forward = -127;
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, p.peer());
					const sim::Vec3 &v = watch.of(spawner)->state.body.linear_velocity;
					return v[0] * line[0] + v[2] * line[1] <= 0.2f;
				},
				240));
		forward = 0;
		m->ticks(30);
		watch.take(m->out, p.peer());
		w = watch.of(spawner);
		const std::array<float, 2> bow = heading_of(w->state.body.rotation);
		const sim::Vec3 hull_at = w->state.body.com_position;

		constexpr float kOffBow = 0.7f;
		const float side_x = bow[0] * std::cos(kOffBow) + bow[1] * std::sin(kOffBow);
		const float side_z = -bow[0] * std::sin(kOffBow) + bow[1] * std::cos(kOffBow);
		const sim::Vec3 target_at =
				ground_at(*m->world, hull_at[0] + 25.0f * side_x, hull_at[2] + 25.0f * side_z, hull_at[1] + 30.0f);
		m->place(enemy->slot, target_at);
		m->ticks(2);
		const sim::SoldierState *victim = m->state(enemy->slot);
		REQUIRE(is_alive(victim));
		const float health_before = victim->health;
		const sim::Vec3 chest = add(victim->position, 0.0f, 1.2f, 0.0f);
		const sim::Vec3 gun = add(hull_at, 0.0f, 1.1f, 0.0f);
		const float dx = chest[0] - gun[0], dy = chest[1] - gun[1], dz = chest[2] - gun[2];
		aim_yaw = sim::quantize_yaw(std::atan2(dx, dz));
		aim_pitch = sim::quantize_pitch(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
		m->ticks(120);
		REQUIRE(is_alive(m->state(enemy->slot)));
		watch.take(m->out, p.peer());
		w = watch.of(spawner);
		REQUIRE(w != nullptr);
		REQUIRE(w->turret_count == kind.armament->turrets.size());
		std::optional<size_t> gun_yaw;
		for (size_t k = 0; k < w->turret_count; ++k) {
			const sim::vehicles::ArmamentTurret &t = kind.armament->turrets[k];
			std::printf("armor: turret body %zu of seat %d at %.1f deg\n", k, int(t.seat),
					double(w->turret_angles[k] / core::kDegToRadF));
			if (t.seat == 0 && t.parent < 0 && t.aim == sim::vehicles::TurretAimAxis::Yaw) {
				gun_yaw = k;
			}
		}
		REQUIRE(gun_yaw.has_value());
		CHECK(std::abs(std::abs(w->turret_angles[*gun_yaw]) - kOffBow) < 2.0f * core::kDegToRadF);
		const int kills_before = m->row(int(slot)) != nullptr ? int(m->row(int(slot))->kills) : 0;
		const uint64_t shots_before = m->server.stats().vehicle_shots;
		fire = true;
		m->ticks(4);
		fire = false;
		const bool killed = m->until([&] { return !is_alive(m->state(enemy->slot)); }, 90);
		std::printf("armor: vehicle shots %llu -> %llu\n", static_cast<unsigned long long>(shots_before),
				static_cast<unsigned long long>(m->server.stats().vehicle_shots));
		CHECK(m->server.stats().vehicle_shots == shots_before + 1u);
		const sim::SoldierState *after = m->state(enemy->slot);
		std::printf("armor: the enemy %.1f m off the bow at %.0f deg: health %.0f -> %.0f, killed %d, kills %d -> %d\n",
				double(horizontal(target_at, hull_at)), double(kOffBow / core::kDegToRadF), double(health_before),
				double(after != nullptr ? after->health : -1.0f), int(killed), kills_before,
				m->row(int(slot)) != nullptr ? int(m->row(int(slot))->kills) : -1);
		CHECK(killed);
		REQUIRE(m->row(int(slot)) != nullptr);
		CHECK(int(m->row(int(slot))->kills) == kills_before + 1);
		CHECK(is_alive(m->state(p.slot())));
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

namespace {

struct VehicleSight {
	bool sent = false;
	bool present = false;
	uint8_t generation = 0;
	sim::Vec3 at{};
	uint16_t health = 0;
	uint8_t condition = 0;
	bool wreck = false;
	bool pending = false;
};

bool destroyed_unexploded(const host::GameServer &p_server, uint16_t p_spawner) {
	const std::optional<float> health = p_server.vehicle_health(p_spawner);
	return health.has_value() && *health <= 0.0f;
}

VehicleSight sighted(std::span<const host::OutDatagram> p_out, uint32_t p_peer, uint16_t p_spawner) {
	VehicleSight s;
	for (const host::OutDatagram &d : p_out) {
		if (d.peer != p_peer || net::peek_type(d.bytes) != net::MessageType::VehicleStates) {
			continue;
		}
		core::Result<net::VehicleStates> m = net::decode_vehicle_states(d.bytes);
		if (!m) {
			continue;
		}
		s.sent = true;
		for (const net::VehicleWire &w : m->vehicles) {
			if (w.spawner == p_spawner && w.condition == uint8_t(sim::vehicles::VehicleCondition::Destroyed)) {
				s.wreck = s.wreck || w.exploded;
				s.pending = s.pending || !w.exploded;
			} else if (w.spawner == p_spawner) {
				s.present = true;
				s.generation = w.generation;
				s.at = w.state.body.com_position;
				s.health = w.health;
				s.condition = w.condition;
			}
		}
	}
	return s;
}

bool clear_line(const sim::CollisionWorld &p_world, const sim::Vec3 &p_from, const sim::Vec3 &p_to,
		std::span<const net::VehicleWire> p_vehicles, uint16_t p_skip_a, uint16_t p_skip_b) {
	const sim::Vec3 d{ p_to[0] - p_from[0], p_to[1] - p_from[1], p_to[2] - p_from[2] };
	if (p_world.ray_any(p_from, d, sim::query_mask::kCollision)) {
		return false;
	}
	const float l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	for (const net::VehicleWire &w : p_vehicles) {
		if (w.spawner == p_skip_a || w.spawner == p_skip_b) {
			continue;
		}
		const sim::Vec3 &c = w.state.body.com_position;
		const float s = std::clamp(
				((c[0] - p_from[0]) * d[0] + (c[1] - p_from[1]) * d[1] + (c[2] - p_from[2]) * d[2]) / l2, 0.0f, 1.0f);
		const sim::Vec3 q{ p_from[0] + s * d[0], p_from[1] + s * d[1], p_from[2] + s * d[2] };
		if (horizontal(q, c) < 5.0f) {
			return false;
		}
	}
	return true;
}

float soldier_health(const sim::SoldierState *p_state) {
	return p_state != nullptr && is_alive(p_state) ? p_state->health : 0.0f;
}

struct WreckRun {
	bool ran = false;
	uint32_t shells = 0;
	uint64_t hits = 0;
	uint64_t crew_kills = 0;
	bool driver_dead = false;
	bool driver_untouched_until_wrecked = true;
	int kills_gained = 0;
	int driver_deaths_gained = 0;
	bool gunner_alive = false;
	uint32_t gone_for = 0;
	uint32_t delay_ticks = 0;
	uint8_t generation_before = 0;
	uint8_t generation_after = 0;
	float respawn_offset = 0;
	uint64_t runtime_faults = 0;
	uint16_t first_health = 0;
	uint16_t lowest_health = net::kVehicleHealthFull;
	uint16_t disabled_at = 0;
	uint32_t disabled_samples = 0;
	uint32_t misread_samples = 0;
	uint16_t respawn_health = 0;
	uint8_t respawn_condition = 0xFF;
	uint32_t wreck_samples = 0;
	uint32_t pending_samples = 0;
	uint32_t destroyed_sends = 0;
	uint32_t pending_after_wreck = 0;
	uint64_t wreck_blasts = 0;
	uint64_t wreck_soldier_hits = 0;
	uint64_t wreck_vehicle_hits = 0;
	uint32_t blast_ticks = 0;
	sim::Tick blast_tick = 0;
	sim::Tick first_wreck_tick = 0;
	sim::Tick last_pending_tick = 0;
	float bystander_before = 0;
	float bystander_after = 0;
	bool mate_placed = false;
	float mate_before = 0;
	float mate_after = 0;
	uint64_t unlocated = 0;
	bool neighbour_placed = false;
	float neighbour_gap = 0;
	uint16_t neighbour_before = 0;
	uint16_t neighbour_after = 0;
	sim::Tick neighbour_before_tick = 0;
	sim::Tick neighbour_after_tick = 0;
	uint16_t tank_before = 0;
	uint16_t tank_after = 0;
	sim::Tick tank_after_tick = 0;
};

struct BlastSides {
	uint16_t before = 0;
	uint16_t after = 0;
	sim::Tick before_tick = 0;
	sim::Tick after_tick = 0;
};

void watch_sides(
		BlastSides &r_sides, const VehicleSight &p_seen, sim::Tick p_now, uint32_t p_blasts, sim::Tick p_blast) {
	if (!p_seen.present) {
		return;
	}
	if (p_blasts == 0) {
		r_sides.before = p_seen.health;
		r_sides.before_tick = p_now;
	} else if (p_now > p_blast && r_sides.after_tick == 0) {
		r_sides.after = p_seen.health;
		r_sides.after_tick = p_now;
	}
}

struct NeighbourLevel {
	tdm::TdmLevel level;
	uint16_t target = 0;
	uint16_t neighbour = 0;
	float gap = 0;
};

NeighbourLevel with_vehicle_beside(
		const tdm::TdmLevel &p_level, const std::function<bool(const host::LevelVehicleKind &)> &p_target) {
	const host::LevelVehicles &lv = *p_level.shared->vehicles;
	const std::optional<uint16_t> cannon_def = tank_cannon(lv);
	REQUIRE(cannon_def.has_value());
	std::vector<PairCandidate> placed;
	for (size_t s = 0; s < lv.spawners.size(); ++s) {
		placed.push_back({ uint16_t(s), lv.spawners[s].placement.position });
	}
	const std::optional<TankPair> pair = nearest_tank_pair(
			lv, *cannon_def, placed, p_target, [](const PairCandidate &, const PairCandidate &) { return true; });
	REQUIRE(pair.has_value());
	const uint16_t target = pair->target, gunner = pair->tank;
	const sim::Vec3 at = lv.spawners[target].placement.position;
	std::optional<uint16_t> neighbour;
	float nearest = INFINITY;
	for (size_t n = 0; n < lv.spawners.size(); ++n) {
		const float d = horizontal(lv.spawners[n].placement.position, at);
		if (n != target && !carries(lv.kinds[lv.spawners[n].kind], *cannon_def) && d < nearest) {
			nearest = d;
			neighbour = uint16_t(n);
		}
	}
	REQUIRE(neighbour.has_value());
	const core::gameplay::Explosion &x = lv.kinds[lv.spawners[target].kind].explosion;
	REQUIRE(x.present);
	const float gap = 0.5f * (x.inner_blast_radius + x.blast_radius) + 1.0f;
	const sim::Vec3 from = lv.spawners[gunner].placement.position;
	const float dx = at[0] - from[0], dz = at[2] - from[2], len = horizontal(at, from);
	core::Result<std::unique_ptr<sim::CollisionWorld>> w = sim::CollisionWorld::create(p_level.shared->static_world);
	REQUIRE(w.ok());
	const sim::Vec3 old = lv.spawners[*neighbour].placement.position;
	const float clearance = old[1] - ground_at(**w, old[0], old[2], old[1] + 5.0f)[1];
	const float nx = at[0] - dz / len * gap, nz = at[2] + dx / len * gap;
	auto vehicles = std::make_shared<host::LevelVehicles>(lv);
	host::LevelVehicleSpawner &moved = vehicles->spawners[*neighbour];
	moved.placement.position = { nx, ground_at(**w, nx, nz, at[1] + 5.0f)[1] + clearance, nz };
	moved.placement.rotation = lv.spawners[target].placement.rotation;
	auto shared = std::make_shared<host::SharedLevel>(*p_level.shared);
	shared->vehicles = vehicles;
	NeighbourLevel out;
	out.level.shared = shared;
	out.level.bots = p_level.bots;
	out.level.cache_root = p_level.cache_root;
	out.target = target;
	out.neighbour = *neighbour;
	out.gap = gap;
	std::printf("damage: the %s of spawner %u moved %.1f m beside the %s of spawner %u, off the line from the tank "
				"of spawner %u\n",
			lv.kinds[moved.kind].blueprint.c_str(), unsigned(*neighbour), double(gap),
			lv.kinds[lv.spawners[target].kind].blueprint.c_str(), unsigned(target), unsigned(gunner));
	return out;
}

WreckRun wreck_and_respawn(tdm::TdmLevel &r_level, const std::function<bool(const host::LevelVehicleKind &)> &p_target,
		bool p_teammate_driver = false, std::optional<uint16_t> p_neighbour = std::nullopt) {
	WreckRun run;
	const host::LevelVehicles &lv = *r_level.shared->vehicles;
	const std::optional<uint16_t> cannon_def = tank_cannon(lv);
	REQUIRE(cannon_def.has_value());

	std::unique_ptr<Match> m = start_match(
			r_level, false, 61u, 0, 1, [](host::GameServerConfig &r_config) { r_config.ghost_scope = false; });
	tdm::ScriptedPeer &p = m->join("gunner");
	p.ready();
	REQUIRE(wait_in_round(*m));
	std::vector<Script *> bots{ m->bot(), m->bot(), m->bot() };
	REQUIRE(m->until([&] { return is_alive(m->state(p.slot())); }, 600));
	const uint8_t team = m->rules().team_of(uint8_t(p.slot()));
	Script *enemy = nullptr;
	for (Script *b : bots) {
		enemy = (m->rules().team_of(uint8_t(b->slot)) == team) == p_teammate_driver ? b : enemy;
	}
	REQUIRE(enemy != nullptr);
	Script *bystander = nullptr;
	Script *mate = nullptr;
	for (Script *b : bots) {
		if (b != enemy && m->rules().team_of(uint8_t(b->slot)) != team) {
			bystander = b;
		} else if (b != enemy) {
			mate = b;
		}
	}
	REQUIRE(bystander != nullptr);
	REQUIRE(m->until([&] { return is_alive(m->state(enemy->slot)) && is_alive(m->state(bystander->slot)); }, 600));

	VehicleWatch watch;
	m->ticks(150);
	for (uint32_t t = 0; t < 30; ++t) {
		m->tick();
		watch.take(m->out, p.peer());
	}
	std::vector<PairCandidate> parked;
	for (const net::VehicleWire &w : watch.latest) {
		if (parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
			parked.push_back({ w.spawner, w.state.body.com_position });
		}
	}
	const std::optional<TankPair> pair = nearest_tank_pair(
			lv, *cannon_def, parked, p_target, [&](const PairCandidate &p_t, const PairCandidate &p_j) {
				return clear_line(*m->world, add(p_t.at, 0.0f, 1.1f, 0.0f), add(p_j.at, 0.0f, 0.3f, 0.0f), watch.latest,
						p_t.spawner, p_j.spawner);
			});
	REQUIRE(pair.has_value());
	const uint16_t tank_spawner = pair->tank, target_spawner = pair->target;
	const float best = pair->range;
	const net::VehicleWire *target = watch.of(target_spawner);
	const host::LevelVehicleKind &tank_kind = lv.kinds[lv.spawners[tank_spawner].kind];
	const host::LevelVehicleKind &target_kind = lv.kinds[lv.spawners[target_spawner].kind];
	const float spawn_delay = lv.spawners[target_spawner].spawn_delay;
	std::printf("damage: %s of spawner %u aims at the %s of spawner %u (SpawnDelay %.0f s) %.1f m away\n",
			tank_kind.blueprint.c_str(), unsigned(tank_spawner), target_kind.blueprint.c_str(),
			unsigned(target_spawner), double(spawn_delay), double(best));
	const sim::Vec3 target_at = target->state.body.com_position;
	run.generation_before = target->generation;
	run.first_health = target->health;
	const float full = sim::vehicles::full_vehicle_health(target_kind.damage);
	run.disabled_at =
			uint16_t(std::ceil(target_kind.damage.disabled_threshold / full * float(net::kVehicleHealthFull)));

	const sim::Tick enemy_use = m->now() + 3u;
	enemy->fn = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
		sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		if (p_tick == enemy_use) {
			c.buttons |= sim::buttons::kUse;
		}
		return c;
	};
	m->place(enemy->slot,
			ground_at(*m->world, target_at[0] + 0.5f * target_kind.entry_radius, target_at[2], target_at[1] + 5.0f));
	const uint8_t enemy_slot = uint8_t(enemy->slot);
	REQUIRE(m->until(
			[&] {
				watch.take(m->out, p.peer());
				const net::VehicleWire *w = watch.of(target_spawner);
				return w != nullptr && w->occupants[0] == enemy_slot;
			},
			60));

	const sim::Vec3 tank_at = watch.of(tank_spawner)->state.body.com_position;
	m->teleport(p, ground_at(*m->world, tank_at[0] + 0.6f * tank_kind.entry_radius, tank_at[2], tank_at[1] + 5.0f));
	REQUIRE(m->until([&] { return horizontal(m->state(p.slot())->position, tank_at) < tank_kind.entry_radius; }, 30));
	const sim::Tick use_at = m->now() + 2u;
	const sim::Vec3 gun = add(tank_at, 0.0f, 1.1f, 0.0f);
	const sim::Vec3 aim = add(target_at, 0.0f, 0.3f, 0.0f);
	const float dx = aim[0] - gun[0], dy = aim[1] - gun[1], dz = aim[2] - gun[2];
	const uint16_t aim_yaw = sim::quantize_yaw(std::atan2(dx, dz));
	const int16_t aim_pitch = sim::quantize_pitch(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
	bool fire = false;
	m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
		sim::Command c = sim::idle_command(p_tick, aim_yaw, aim_pitch, p_state.target_pose);
		if (p_tick == use_at) {
			c.buttons |= sim::buttons::kUse;
		}
		if (fire) {
			c.buttons |= sim::buttons::kFire;
		}
		return c;
	};
	const uint8_t slot = uint8_t(p.slot());
	REQUIRE(m->until(
			[&] {
				watch.take(m->out, p.peer());
				const net::VehicleWire *w = watch.of(tank_spawner);
				return w != nullptr && w->occupants[0] == slot;
			},
			60));
	const float away_x = target_at[0] - tank_at[0], away_z = target_at[2] - tank_at[2];
	const float away_len = std::max(1e-3f, horizontal(target_at, tank_at));
	const float dir_x = away_x / away_len, dir_z = away_z / away_len;
	REQUIRE(target_kind.explosion.present);
	const float stand = target_kind.explosion.inner_blast_radius +
			0.5f * (target_kind.explosion.blast_radius - target_kind.explosion.inner_blast_radius);
	const auto beside = [&](float p_side) {
		return ground_at(*m->world, target_at[0] + dir_x * stand - dir_z * p_side,
				target_at[2] + dir_z * stand + dir_x * p_side, target_at[1] + 5.0f);
	};
	m->place(bystander->slot, beside(1.5f));
	if (mate != nullptr && is_alive(m->state(mate->slot))) {
		m->place(mate->slot, beside(-1.5f));
		run.mate_placed = true;
	}
	if (p_neighbour) {
		const net::VehicleWire *n = watch.of(*p_neighbour);
		REQUIRE(n != nullptr);
		run.neighbour_gap = horizontal(n->state.body.com_position, target_at);
		run.neighbour_placed = parked_upright(n->state.body.rotation, n->state.body.linear_velocity) &&
				run.neighbour_gap < target_kind.explosion.blast_radius;
		std::printf("damage: the neighbour of spawner %u stands %.2f m from the target, upright %d\n",
				unsigned(*p_neighbour), double(run.neighbour_gap),
				int(parked_upright(n->state.body.rotation, n->state.body.linear_velocity)));
		REQUIRE(run.neighbour_placed);
	}
	std::printf("damage: the %s's explosion: BlastDamage %.0f, %.1f..%.1f m, PreExplosionTime %.2f s, "
				"CriticalDamageTime %.2f s; bystanders %.1f m behind it\n",
			target_kind.blueprint.c_str(), double(target_kind.explosion.blast_damage),
			double(target_kind.explosion.inner_blast_radius), double(target_kind.explosion.blast_radius),
			double(target_kind.damage.pre_explosion_time), double(target_kind.damage.critical_damage_time),
			double(stand));
	m->ticks(150);
	const int kills_before = m->row(int(slot)) != nullptr ? int(m->row(int(slot))->kills) : 0;
	REQUIRE(m->row(enemy->slot) != nullptr);
	const int driver_deaths_before = int(m->row(enemy->slot)->deaths);
	const uint64_t hits_before = m->server.stats().vehicle_hits;
	const uint64_t shots_before = m->server.stats().vehicle_shots;
	const uint64_t crew_before = m->server.stats().vehicle_crew_kills;
	const uint64_t unlocated_before = m->server.stats().vehicle_unlocated_hits;
	BlastSides neighbour_sides, tank_sides;
	REQUIRE(is_alive(m->state(enemy->slot)));
	const float driver_health = m->state(enemy->slot)->health;

	const auto tick_watching_blast = [&] {
		const uint64_t blasts_now = m->server.stats().vehicle_wreck_blasts;
		const float by_before = soldier_health(m->state(bystander->slot));
		const float mt_before = mate != nullptr ? soldier_health(m->state(mate->slot)) : 0.0f;
		m->tick();
		if (m->server.stats().vehicle_wreck_blasts != blasts_now) {
			++run.blast_ticks;
			run.blast_tick = m->now();
			run.bystander_before = by_before;
			run.bystander_after = soldier_health(m->state(bystander->slot));
			run.mate_before = mt_before;
			run.mate_after = mate != nullptr ? soldier_health(m->state(mate->slot)) : 0.0f;
		}
		if (p_neighbour) {
			watch_sides(neighbour_sides, sighted(m->out, p.peer(), *p_neighbour), m->now(), run.blast_ticks,
					run.blast_tick);
		}
		watch_sides(tank_sides, sighted(m->out, p.peer(), tank_spawner), m->now(), run.blast_ticks, run.blast_tick);
	};
	std::optional<sim::Tick> gone;
	for (int shell = 0; shell < 10 && !gone; ++shell) {
		fire = true;
		m->tick();
		m->tick();
		fire = false;
		for (uint32_t t = 0; t < 360 && !gone; ++t) {
			const uint64_t crew_now = m->server.stats().vehicle_crew_kills;
			tick_watching_blast();
			const sim::SoldierState *driver = m->state(enemy->slot);
			if (crew_now == crew_before && m->server.stats().vehicle_crew_kills == crew_before &&
					(driver == nullptr || driver->health != driver_health)) {
				run.driver_untouched_until_wrecked = false;
			}
			const VehicleSight now = sighted(m->out, p.peer(), target_spawner);
			if (now.sent && !now.present) {
				gone = m->now();
			}
			run.pending_after_wreck += now.pending && run.wreck_samples > 0 ? 1u : 0u;
			run.first_wreck_tick = now.wreck && run.wreck_samples == 0 ? m->now() : run.first_wreck_tick;
			run.last_pending_tick = now.pending ? m->now() : run.last_pending_tick;
			run.wreck_samples += now.wreck ? 1u : 0u;
			run.pending_samples += now.pending ? 1u : 0u;
			run.destroyed_sends += now.sent && destroyed_unexploded(m->server, target_spawner) ? 1u : 0u;
			if (now.present) {
				run.lowest_health = std::min(run.lowest_health, now.health);
				const uint8_t expected = now.health == 0 ? uint8_t(sim::vehicles::VehicleCondition::Destroyed)
						: now.health <= run.disabled_at	 ? uint8_t(sim::vehicles::VehicleCondition::Disabled)
														 : uint8_t(sim::vehicles::VehicleCondition::Healthy);
				run.disabled_samples += now.condition == uint8_t(sim::vehicles::VehicleCondition::Disabled) ? 1u : 0u;
				run.misread_samples += now.condition != expected && now.health != 0 ? 1u : 0u;
			}
		}
		std::printf("damage: after shell %d: %llu shots, %llu vehicle hits, the driver %s\n", shell + 1,
				static_cast<unsigned long long>(m->server.stats().vehicle_shots - shots_before),
				static_cast<unsigned long long>(m->server.stats().vehicle_hits - hits_before),
				is_alive(m->state(enemy->slot)) ? "alive" : "dead");
	}
	REQUIRE(gone.has_value());
	run.shells = uint32_t(m->server.stats().vehicle_shots - shots_before);
	run.hits = m->server.stats().vehicle_hits - hits_before;
	run.crew_kills = m->server.stats().vehicle_crew_kills - crew_before;
	run.driver_dead = !is_alive(m->state(enemy->slot));
	REQUIRE(m->row(int(slot)) != nullptr);
	run.kills_gained = int(m->row(int(slot))->kills) - kills_before;
	REQUIRE(m->row(enemy->slot) != nullptr);
	run.driver_deaths_gained = int(m->row(enemy->slot)->deaths) - driver_deaths_before;
	run.gunner_alive = is_alive(m->state(p.slot()));

	run.delay_ticks = sim::seconds_to_ticks(spawn_delay);
	std::optional<sim::Tick> back;
	VehicleSight seen;
	for (uint32_t t = 0; t < run.delay_ticks + 120u && !back; ++t) {
		tick_watching_blast();
		seen = sighted(m->out, p.peer(), target_spawner);
		run.pending_after_wreck += seen.pending && run.wreck_samples > 0 ? 1u : 0u;
		run.first_wreck_tick = seen.wreck && run.wreck_samples == 0 ? m->now() : run.first_wreck_tick;
		run.last_pending_tick = seen.pending ? m->now() : run.last_pending_tick;
		run.wreck_samples += seen.wreck ? 1u : 0u;
		run.pending_samples += seen.pending ? 1u : 0u;
		run.destroyed_sends += seen.sent && destroyed_unexploded(m->server, target_spawner) ? 1u : 0u;
		if (seen.present) {
			back = m->now();
		}
	}
	REQUIRE(back.has_value());
	run.gone_for = *back - *gone;
	run.generation_after = seen.generation;
	run.respawn_health = seen.health;
	run.respawn_condition = seen.condition;
	run.wreck_blasts = m->server.stats().vehicle_wreck_blasts;
	run.wreck_soldier_hits = m->server.stats().vehicle_wreck_blast_soldier_hits;
	run.wreck_vehicle_hits = m->server.stats().vehicle_wreck_blast_vehicle_hits;
	run.unlocated = m->server.stats().vehicle_unlocated_hits - unlocated_before;
	run.neighbour_before = neighbour_sides.before;
	run.neighbour_after = neighbour_sides.after;
	run.neighbour_before_tick = neighbour_sides.before_tick;
	run.neighbour_after_tick = neighbour_sides.after_tick;
	run.tank_before = tank_sides.before;
	run.tank_after = tank_sides.after;
	run.tank_after_tick = tank_sides.after_tick;
	std::printf("damage: around the blast at tick %u: the gunner's tank %u -> %u (tick %u), the neighbour %u (tick "
				"%u) -> %u (tick %u); %llu unlocated vehicle hits\n",
			unsigned(run.blast_tick), unsigned(run.tank_before), unsigned(run.tank_after),
			unsigned(run.tank_after_tick), unsigned(run.neighbour_before), unsigned(run.neighbour_before_tick),
			unsigned(run.neighbour_after), unsigned(run.neighbour_after_tick),
			static_cast<unsigned long long>(run.unlocated));
	run.respawn_offset = horizontal(seen.at, target_at);
	std::printf("damage: the %s left the wire at tick %u and came back at tick %u (%u ticks, SpawnDelay %u ticks), "
				"generation %u -> %u, %.2f m from its first spawn\n",
			target_kind.blueprint.c_str(), unsigned(*gone), unsigned(*back), unsigned(run.gone_for),
			unsigned(run.delay_ticks), unsigned(run.generation_before), unsigned(run.generation_after),
			double(run.respawn_offset));
	run.runtime_faults = m->rules().stats().runtime_faults;
	run.ran = true;
	return run;
}

void check_blast_relation(const tdm::TdmLevel &p_level) {
	const host::LevelVehicles &lv = *p_level.shared->vehicles;
	REQUIRE(p_level.shared->gunplay != nullptr);
	REQUIRE(p_level.shared->gunplay->materials != nullptr);
	const sim::weapons::SurfaceMaterialTable &materials = *p_level.shared->gunplay->materials;
	host::VehicleGunnery gunnery;
	REQUIRE(gunnery.configure(lv, p_level.shared->gunplay->materials, sim::weapons::Mechanics()).ok());
	REQUIRE(gunnery.table() != nullptr);
	host::VehicleDamageSource source;
	source.configure(*gunnery.table(), lv.weapons);
	const std::optional<uint16_t> cannon = tank_cannon(lv);
	REQUIRE(cannon.has_value());
	const sim::weapons::SurfacePair blast = materials.pair_of(lv.weapons[*cannon].projectile.explosion.material);
	REQUIRE(blast.resolved);
	size_t thresholds = 0;
	for (const host::LevelVehicleKind &kind : lv.kinds) {
		const sim::weapons::SurfacePair hull = materials.pair_of(kind.material);
		REQUIRE(hull.resolved);
		const core::gameplay::DamageRelation rel = materials.relation(blast.property, hull.property);
		sim::weapons::DamageEvent e;
		e.kind = sim::weapons::DamageKind::Blast;
		e.def = *cannon;
		for (const float base : { 100.0f, 25.0f }) {
			e.base = base;
			const float scaled = base * (rel.present ? rel.protection_multiplier : 1.0f);
			const float expected = !rel.present || scaled >= rel.protection_threshold ? scaled : 0.0f;
			std::printf("damage: a %.0f tank shell blast on the %s: relation %d, x%.3f, threshold %.1f -> %.2f\n",
					double(base), kind.blueprint.c_str(), int(rel.present), double(rel.protection_multiplier),
					double(rel.protection_threshold), double(source.amount(e, hull.property)));
			CHECK(source.amount(e, hull.property) == doctest::Approx(expected));
		}
		thresholds += rel.present && rel.protection_threshold > 0.0f ? 1u : 0u;
	}
	CHECK(thresholds > 0u);
	size_t wreck_relations = 0;
	for (const host::LevelVehicleKind &wreck : lv.kinds) {
		if (!wreck.explosion.present) {
			continue;
		}
		const sim::weapons::SurfacePair wreck_blast = materials.pair_of(wreck.explosion.material);
		REQUIRE(wreck_blast.resolved);
		for (const host::LevelVehicleKind &kind : lv.kinds) {
			const uint8_t hull = materials.pair_of(kind.material).property;
			const core::gameplay::DamageRelation rel = materials.relation(wreck_blast.property, hull);
			const float scaled = 100.0f * (rel.present ? rel.protection_multiplier : 1.0f);
			const float expected = !rel.present || scaled >= rel.protection_threshold ? scaled : 0.0f;
			CHECK(host::wreck_blast_amount(&materials, wreck_blast.property, hull, 100.0f) ==
					doctest::Approx(expected));
			CHECK(host::wreck_blast_amount(nullptr, wreck_blast.property, hull, 100.0f) == 100.0f);
			wreck_relations += rel.present ? 1u : 0u;
			std::printf("damage: the %s's explosion on the %s: 100 -> %.2f\n", wreck.blueprint.c_str(),
					kind.blueprint.c_str(),
					double(host::wreck_blast_amount(&materials, wreck_blast.property, hull, 100.0f)));
		}
	}
	CHECK(wreck_relations > 0u);
}

void check_respawn(const WreckRun &p_run) {
	const uint32_t divisor = host::GameServerConfig().snapshot_divisor;
	CHECK(p_run.gone_for >= p_run.delay_ticks);
	CHECK(p_run.gone_for <= p_run.delay_ticks + divisor);
	CHECK(p_run.generation_after == uint8_t(p_run.generation_before + 1u));
	CHECK(p_run.respawn_offset < 1.0f);
	CHECK(p_run.gunner_alive);
	CHECK(p_run.runtime_faults == 0u);
	std::printf("damage: wire health %u at the start, lowest %u while on the wire, disabled at %u, %u disabled "
				"samples, %u misread, %u after the respawn\n",
			unsigned(p_run.first_health), unsigned(p_run.lowest_health), unsigned(p_run.disabled_at),
			unsigned(p_run.disabled_samples), unsigned(p_run.misread_samples), unsigned(p_run.respawn_health));
	CHECK(p_run.first_health == net::kVehicleHealthFull);
	CHECK(p_run.lowest_health < net::kVehicleHealthFull);
	CHECK(p_run.misread_samples == 0u);
	CHECK(p_run.respawn_health == net::kVehicleHealthFull);
	CHECK(p_run.respawn_condition == uint8_t(sim::vehicles::VehicleCondition::Healthy));
	std::printf("damage: %u wreck samples on the wire from tick %u, %u destroyed before it exploded (the last at "
				"tick %u; %u sends while the server held it destroyed and unexploded), %u of those after; the blast "
				"at tick %u\n",
			unsigned(p_run.wreck_samples), unsigned(p_run.first_wreck_tick), unsigned(p_run.pending_samples),
			unsigned(p_run.last_pending_tick), unsigned(p_run.destroyed_sends), unsigned(p_run.pending_after_wreck),
			unsigned(p_run.blast_tick));
	CHECK(p_run.wreck_samples >= 2u);
	CHECK(p_run.pending_samples == p_run.destroyed_sends);
	CHECK(p_run.pending_after_wreck == 0u);
	CHECK(p_run.first_wreck_tick >= p_run.blast_tick);
	std::printf("damage: %u wreck blast, %u soldier hits, %u vehicle hits; the enemy bystander %.1f -> %.1f, the "
				"gunner's teammate %.1f -> %.1f\n",
			unsigned(p_run.wreck_blasts), unsigned(p_run.wreck_soldier_hits), unsigned(p_run.wreck_vehicle_hits),
			double(p_run.bystander_before), double(p_run.bystander_after), double(p_run.mate_before),
			double(p_run.mate_after));
	CHECK(p_run.wreck_blasts == 1u);
	CHECK(p_run.blast_ticks == 1u);
	CHECK(p_run.wreck_soldier_hits >= 1u);
	CHECK(p_run.bystander_before == doctest::Approx(100.0f));
	CHECK(p_run.bystander_after < p_run.bystander_before);
	CHECK(p_run.bystander_after > 0.0f);
	if (p_run.mate_placed) {
		CHECK(p_run.wreck_soldier_hits >= 2u);
		CHECK(p_run.mate_before == doctest::Approx(100.0f));
		CHECK(p_run.mate_after == p_run.mate_before);
	}
	CHECK(p_run.unlocated == 0u);
	CHECK(p_run.tank_after_tick > p_run.blast_tick);
	CHECK(p_run.tank_after == p_run.tank_before);
	if (p_run.neighbour_placed) {
		CHECK(p_run.wreck_vehicle_hits >= 1u);
		CHECK(p_run.neighbour_before_tick < p_run.blast_tick);
		CHECK(p_run.neighbour_after_tick > p_run.blast_tick);
		CHECK(p_run.neighbour_after < p_run.neighbour_before);
	}
}
}

TEST_SUITE("host-vehicles-unit") {
	TEST_CASE("vehicles: the wire carries a vehicle's health in thousandths of full health rounded up, its condition "
			  "and burning once its decay delay has run; the client presents them") {
		sim::vehicles::VehicleDamageRule rule;
		rule.zones[0].max_health = 500;
		rule.zones[1].max_health = 500;
		rule.share_damage = true;
		rule.disabled_threshold = 300;
		rule.decay_delay = 7;
		rule.decay_rate = 15;
		sim::vehicles::VehicleHealth h = sim::vehicles::spawned_vehicle_health(rule);
		CHECK(host::wire_health(rule, h) == net::kVehicleHealthFull);
		h.health = { 218.6f, 218.6f, 0.0f, 0.0f };
		CHECK(host::wire_health(rule, h) == 438);
		h.condition = sim::vehicles::VehicleCondition::Disabled;
		h.decay_timer = 6.9f;
		CHECK_FALSE(host::vehicle_burning(rule, h));
		h.decay_timer = 7.0f;
		CHECK(host::vehicle_burning(rule, h));
		h.condition = sim::vehicles::VehicleCondition::Destroyed;
		h.health = {};
		CHECK(host::wire_health(rule, h) == 0);
		CHECK_FALSE(host::vehicle_burning(rule, h));

		net::VehicleWire w;
		w.state.wheel_count = 4;
		w.health = 438;
		w.condition = uint8_t(sim::vehicles::VehicleCondition::Disabled);
		w.burning = true;
		const host::PresentedVehicle v = host::present_vehicle(w, 40);
		CHECK(v.health == doctest::Approx(0.438f));
		CHECK(v.condition == sim::vehicles::VehicleCondition::Disabled);
		CHECK(v.burning);
		CHECK_FALSE(v.exploded);
		w.health = 0;
		w.condition = uint8_t(sim::vehicles::VehicleCondition::Destroyed);
		w.burning = false;
		CHECK_FALSE(host::present_vehicle(w, 41).exploded);
		w.exploded = true;
		CHECK(host::present_vehicle(w, 42).exploded);
	}

	TEST_CASE("vehicles: the client hides a vehicle and starts its explosion once per life on the exploded mark, "
			  "not on Destroyed") {
		host::VehicleWrecks wrecks;
		net::VehicleWire w;
		w.spawner = 7;
		w.generation = 3;
		const host::VehicleWreckSight live = wrecks.see(host::present_vehicle(w, 10));
		CHECK_FALSE(live.hidden);
		CHECK_FALSE(live.explosion);
		w.health = 0;
		w.condition = uint8_t(sim::vehicles::VehicleCondition::Destroyed);
		const host::VehicleWreckSight destroyed = wrecks.see(host::present_vehicle(w, 11));
		CHECK_FALSE(destroyed.hidden);
		CHECK_FALSE(destroyed.explosion);
		w.exploded = true;
		const host::VehicleWreckSight first = wrecks.see(host::present_vehicle(w, 12));
		CHECK(first.hidden);
		CHECK(first.explosion);
		for (sim::Tick t = 13; t < 18; ++t) {
			const host::VehicleWreckSight again = wrecks.see(host::present_vehicle(w, t));
			CHECK(again.hidden);
			CHECK_FALSE(again.explosion);
		}
		w.spawner = 8;
		CHECK(wrecks.see(host::present_vehicle(w, 18)).explosion);
		w.spawner = 7;
		w.generation = 4;
		w.health = net::kVehicleHealthFull;
		w.condition = uint8_t(sim::vehicles::VehicleCondition::Healthy);
		w.exploded = false;
		CHECK_FALSE(wrecks.see(host::present_vehicle(w, 600)).hidden);
		w.health = 0;
		w.condition = uint8_t(sim::vehicles::VehicleCondition::Destroyed);
		w.exploded = true;
		CHECK(wrecks.see(host::present_vehicle(w, 900)).explosion);
		CHECK_FALSE(wrecks.see(host::present_vehicle(w, 901)).explosion);
	}

	TEST_CASE("vehicles: a disabled vehicle's driver input goes through the data's input modifiers: EIAThrottle on "
			  "the forward pedal, EIABrake on the back pedal and the brake, EIAYaw on the steer") {
		const std::vector<core::gameplay::VehicleInputModifierDef> tank{ { "EIAThrottle", 0.68f, 0.0f },
			{ "EIABrake", 0.68f, 0.0f }, { "EIAYaw", 0.65f, 0.0f }, { "EIASprint", 0.0f, 0.0f } };
		sim::VehicleCommand c;
		c.throttle = 127;
		c.steer = -127;
		c.brake = 255;
		c.buttons = sim::vehicle_buttons::kHandbrake;
		const sim::VehicleCommand d = host::disabled_command(c, tank);
		CHECK(int(d.throttle) == 86);
		CHECK(int(d.steer) == -83);
		CHECK(int(d.brake) == 173);
		CHECK(d.buttons == c.buttons);
		c.throttle = -100;
		CHECK(int(host::disabled_command(c, tank).throttle) == -68);
		CHECK(host::disabled_command(c, {}) == c);
	}

	TEST_CASE("vehicles: the client draws turret angles between two received states the short way round") {
		net::VehicleWire w;
		w.turret_count = 2;
		w.turret_angles[0] = 3.0f;
		w.turret_angles[1] = 0.2f;
		const host::PresentedVehicle early = host::present_vehicle(w, 10);
		w.turret_angles[0] = -3.0f;
		w.turret_angles[1] = -0.2f;
		host::PresentedVehicle late = host::present_vehicle(w, 20);
		late.window[0] = early.window[0];
		late.window_count = 1;
		const host::PresentedVehiclePose mid = host::vehicle_pose_at(late, 15.0);
		CHECK(std::fabs(std::remainder(mid.turret_angle[0] - 3.14159265f, 6.28318531f)) < 1e-4f);
		CHECK(mid.turret_angle[1] == doctest::Approx(0.0f).epsilon(1e-5));
		CHECK(host::vehicle_pose_at(late, 12.5).turret_angle[1] == doctest::Approx(0.1f));
		CHECK(host::vehicle_pose_at(late, 30.0).turret_angle[0] == doctest::Approx(-3.0f));
		CHECK(host::vehicle_pose_at(late, 5.0).turret_angle[0] == doctest::Approx(3.0f));
	}
}

TEST_SUITE("host-vehicles-damage") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: tank shells wreck a Growler by the R-G3 damage rules, the splash "
			  "kills its exposed enemy driver with the kill on the gunner row, the wreck explodes and its spawner "
			  "brings it back after its SpawnDelay") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		const host::LevelVehicleKind *growler = nullptr;
		for (const host::LevelVehicleKind &kind : lv.kinds) {
			std::printf("damage: %s, health %.0f, disabled at %.0f, box half %.2f %.2f %.2f, material %d\n",
					kind.blueprint.c_str(), double(sim::vehicles::full_vehicle_health(kind.damage)),
					double(kind.damage.disabled_threshold), double(kind.box.half[0]), double(kind.box.half[1]),
					double(kind.box.half[2]), int(kind.material.resolved));
			CHECK(kind.material.resolved);
			growler = kind.blueprint == "vehicles/growleritv/growleritv" ? &kind : growler;
		}
		REQUIRE(growler != nullptr);
		CHECK(sim::vehicles::full_vehicle_health(growler->damage) == doctest::Approx(500.0f));
		CHECK(growler->damage.disabled_threshold == doctest::Approx(300.0f));
		CHECK(growler->damage.regeneration_delay == doctest::Approx(12.0f));
		CHECK(growler->damage.regeneration_rate == doctest::Approx(32.0f));
		CHECK(growler->damage.decay_delay == doctest::Approx(7.0f));
		CHECK(growler->damage.decay_rate == doctest::Approx(15.0f));
		CHECK(growler->damage.share_damage);
		CHECK(growler->exposed[0]);
		for (const host::LevelVehicleSpawner &sp : lv.spawners) {
			CAPTURE(lv.kinds[sp.kind].blueprint);
			const bool quick = sp.time_until_abandoned < 7.5f;
			CHECK(sp.apply_damage_to_abandoned);
			CHECK(sp.respawn_range == doctest::Approx(15.0f));
			CHECK(sp.time_until_abandoned == doctest::Approx(quick ? 5.0f : 10.0f));
			CHECK(sp.time_until_abandoned_is_destroyed == doctest::Approx(quick ? 5.0f : 10.0f));
			CHECK(sp.keep_alive_radius == doctest::Approx(quick ? 15.0f : 20.0f));
			CHECK((!quick || host::land_jeep(*lv.kinds[sp.kind].model)));
		}
		check_blast_relation(*level);

		const auto growler_target = [](const host::LevelVehicleKind &p_kind) {
			return p_kind.blueprint == "vehicles/growleritv/growleritv";
		};
		NeighbourLevel beside = with_vehicle_beside(*level, growler_target);
		const WreckRun run = wreck_and_respawn(beside.level, growler_target, false, beside.neighbour);
		REQUIRE(run.ran);
		CHECK(run.neighbour_placed);
		CHECK(run.shells >= 2u);
		CHECK(run.hits >= run.shells);
		CHECK(run.driver_dead);
		CHECK(run.kills_gained == 1);
		CHECK(run.mate_placed);
		check_respawn(run);
	}

	TEST_CASE("vehicles: MP_007 ConquestSmall0: an enemy tank driver sits in a hidden seat, no hit on his tank "
			  "reaches him, and when the shells wreck it he dies at once with the kill on the gunner row; the "
			  "tank comes back after its SpawnDelay") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const WreckRun run = wreck_and_respawn(*level, [](const host::LevelVehicleKind &p_kind) {
			return p_kind.blueprint == "vehicles/m1a2/m1abrams";
		});
		REQUIRE(run.ran);
		CHECK(run.shells >= 3u);
		CHECK(run.driver_untouched_until_wrecked);
		CHECK(run.crew_kills == 1u);
		CHECK(run.driver_dead);
		CHECK(run.kills_gained == 1);
		CHECK(run.mate_placed);
		check_respawn(run);
	}
}

TEST_SUITE("host-vehicles-damage") {
	TEST_CASE(
			"vehicles: MP_007 ConquestSmall0: an RPG-7 rocket and an MBT shell on each hull in percent of its health, "
			"direct hit and blast at the point of impact, through the material relations (R-G4) and the R-G3 chain") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		const sw::SurfaceMaterialTable &materials = *level->shared->gunplay->materials;
		core::Result<sw::InstallWeapons> install = sw::open_install_weapons(level->install);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
		core::Result<core::gameplay::WeaponDef> rpg_def = install->catalog->resolve("weapons/rpg7/u_rpg7");
		REQUIRE_MESSAGE(rpg_def.ok(), (rpg_def.ok() ? std::string() : rpg_def.error().message));
		const std::vector<core::gameplay::WeaponDef> soldier_defs{ *rpg_def };
		core::Result<std::shared_ptr<const sw::WeaponTable>> made =
				sw::make_weapon_table(soldier_defs, level->shared->gunplay->materials);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		const sw::WeaponTable &soldier_table = **made;
		const std::optional<uint16_t> rpg = uint16_t(0);
		host::VehicleDamageSource soldier;
		soldier.configure(soldier_table, soldier_defs);
		host::VehicleGunnery gunnery;
		REQUIRE(gunnery.configure(lv, level->shared->gunplay->materials, sw::Mechanics()).ok());
		REQUIRE(gunnery.table() != nullptr);
		host::VehicleDamageSource shells;
		shells.configure(*gunnery.table(), lv.weapons);
		const std::optional<uint16_t> cannon = tank_cannon(lv);
		REQUIRE(cannon.has_value());
		struct Round {
			const char *name;
			const host::VehicleDamageSource *source;
			const sw::WeaponTable *table;
			const core::gameplay::WeaponDef *def;
			uint16_t index;
		};
		const Round rounds[] = {
			{ "RPG-7", &soldier, &soldier_table, &soldier_defs[*rpg], *rpg },
			{ "MBT shell", &shells, gunnery.table(), &lv.weapons[*cannon], *cannon },
		};
		struct Percent {
			std::string_view blueprint;
			std::string_view round;
			float full, shield, direct, blast, first, second;
		};
		constexpr Percent kPercent[] = {
			{ "vehicles/t90/t90", "RPG-7", 1000.0f, 175.0f, 202.5f, 0.0f, 0.0f, 20.25f },
			{ "vehicles/t90/t90", "MBT shell", 1000.0f, 175.0f, 270.0f, 0.0f, 0.0f, 27.0f },
			{ "vehicles/m1a2/m1abrams", "RPG-7", 1000.0f, 175.0f, 202.5f, 0.0f, 0.0f, 20.25f },
			{ "vehicles/m1a2/m1abrams", "MBT shell", 1000.0f, 175.0f, 270.0f, 0.0f, 0.0f, 27.0f },
			{ "vehicles/growleritv/growleritv", "RPG-7", 500.0f, 0.0f, 225.0f, 112.0f, 67.4f, 32.6f },
			{ "vehicles/growleritv/growleritv", "MBT shell", 500.0f, 0.0f, 300.0f, 100.0f, 80.0f, 20.0f },
			{ "vehicles/vdv_buggy/vdv_buggy", "RPG-7", 500.0f, 0.0f, 225.0f, 112.0f, 67.4f, 32.6f },
			{ "vehicles/vdv_buggy/vdv_buggy", "MBT shell", 500.0f, 0.0f, 300.0f, 100.0f, 80.0f, 20.0f },
		};
		size_t pinned = 0;
		size_t hulls = 0;
		for (const host::LevelVehicleKind &kind : lv.kinds) {
			const sw::SurfacePair hull = materials.pair_of(kind.material);
			REQUIRE(hull.resolved);
			const float full = sim::vehicles::full_vehicle_health(kind.damage);
			REQUIRE(full > 0.0f);
			const sim::vehicles::HealthZoneRule &rear = kind.damage.zones[size_t(sim::vehicles::HullZone::Rear)];
			REQUIRE(rear.max_health > 0.0f);
			sim::vehicles::DamageParties enemy;
			enemy.vehicle_team = 1;
			enemy.attacker_team = 2;
			enemy.attacker_is_player = true;
			const float modifier = sim::vehicles::damage_modifier(kind.damage.armor_multiplier, enemy);
			CHECK(modifier == doctest::Approx(1.0f / kind.damage.armor_multiplier));
			const float square = sim::vehicles::angle_of_impact_factor(kind.damage.angle_of_impact, 0.0f);
			std::printf("damage: %s MinSpeedForMineActivation %.2f m/s\n", kind.blueprint.c_str(),
					double(kind.min_speed_for_mine));
			CHECK(kind.min_speed_for_mine == doctest::Approx(0.1f));
			for (const Round &r : rounds) {
				const sw::ProjectileSimDef &p = r.table->defs[r.index].projectile;
				REQUIRE(p.explosion.present);
				const auto relation_damage = [&](float p_base, uint8_t p_property) {
					const core::gameplay::DamageRelation rel = materials.relation(p_property, hull.property);
					return rel.present ? sim::vehicles::material_relation_damage(
												 p_base, rel.protection_multiplier, rel.protection_threshold)
									   : p_base;
				};
				sw::DamageEvent direct;
				direct.kind = sw::DamageKind::Direct;
				direct.def = r.index;
				direct.base = p.kind == core::gameplay::ProjectileKind::Missile ? p.direct_damage
																				: float(sw::falloff_damage(p, 0.0));
				direct.penetration = 1.0f;
				sw::DamageEvent blast = direct;
				blast.kind = sw::DamageKind::Blast;
				blast.base = float(sw::blast_damage(p.explosion, 0.0, sw::BlastFalloff::Linear));
				const sw::SurfacePair blast_pair = materials.pair_of(r.def->projectile.explosion.material);
				REQUIRE(blast_pair.resolved);
				const float want_direct = relation_damage(direct.base, p.property);
				const float want_blast = relation_damage(blast.base, blast_pair.property);
				CHECK(r.source->amount(direct, hull.property) == doctest::Approx(want_direct));
				CHECK(r.source->amount(blast, hull.property) == doctest::Approx(want_blast));
				sim::vehicles::VehicleHealth h = sim::vehicles::spawned_vehicle_health(kind.damage);
				const size_t rear_index = size_t(sim::vehicles::HullZone::Rear);
				float shield = h.shield[rear_index];
				float zone_lost = 0.0f;
				std::array<float, 2> percent{};
				for (size_t round = 0; round < percent.size(); ++round) {
					const float before = sim::vehicles::vehicle_health(kind.damage, h);
					for (const float amount : { want_direct, want_blast }) {
						sim::vehicles::VehicleHit hit;
						hit.damage = amount;
						hit.zone = sim::vehicles::HullZone::Rear;
						hit.modifier = modifier;
						sim::vehicles::apply_vehicle_damage(kind.damage, h, hit);
						const float d = amount * square * modifier;
						if (!(d > 0.0f)) {
							continue;
						}
						if (rear.max_shield > 0.0f && shield >= rear.max_shield && d >= rear.max_shield) {
							shield = std::max(0.0f, shield - d);
						} else {
							zone_lost += d;
						}
					}
					percent[round] = 100.0f * (before - sim::vehicles::vehicle_health(kind.damage, h)) / full;
				}
				const float lost = full - sim::vehicles::vehicle_health(kind.damage, h);
				const float want_lost = std::min(full,
						kind.damage.share_damage ? zone_lost * full / rear.max_health
												 : std::min(zone_lost, rear.max_health) / rear.max_health * full);
				CHECK(lost == doctest::Approx(want_lost).epsilon(1e-4));
				std::printf(
						"damage: %s on the %s (health %.0f, armour x%.2f, rear shield %.0f): direct %.1f (base %.0f), "
						"blast %.1f (base %.0f) from the rear: the first round %.2f %% of its health, the second %.2f "
						"%%\n",
						r.name, kind.blueprint.c_str(), double(full), double(kind.damage.armor_multiplier),
						double(rear.max_shield), double(want_direct), double(direct.base), double(want_blast),
						double(blast.base), double(percent[0]), double(percent[1]));
				const Percent *want = nullptr;
				for (const Percent &p : kPercent) {
					want = p.blueprint == kind.blueprint && p.round == r.name ? &p : want;
				}
				REQUIRE_MESSAGE(want != nullptr, kind.blueprint << " " << r.name);
				CHECK(full == doctest::Approx(want->full));
				CHECK(rear.max_shield == doctest::Approx(want->shield));
				CHECK(want_direct == doctest::Approx(want->direct));
				CHECK(want_blast == doctest::Approx(want->blast));
				CHECK(percent[0] == doctest::Approx(want->first));
				CHECK(percent[1] == doctest::Approx(want->second));
				++pinned;
			}
			++hulls;
		}
		CHECK(hulls == 4u);
		CHECK(pinned == std::size(kPercent));
	}
}

TEST_SUITE("host-vehicles-damage") {
	TEST_CASE("vehicles: the repair tool (wcPowerTool, StartDamage 6, 600 rpm) is the soldier source's repair amount, "
			  "a repair of 6 per shot with no modifiers that refills the zone and then the shield (R3-G3 E, R-G3 7); "
			  "other weapons repair nothing") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		core::Result<sw::InstallWeapons> install = sw::open_install_weapons(level->install);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
		std::vector<core::gameplay::WeaponDef> defs;
		for (const char *v : { "weapons/gadgets/repairtool/u_repairtool", "weapons/rpg7/u_rpg7" }) {
			core::Result<core::gameplay::WeaponDef> d = install->catalog->resolve(v);
			REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
			defs.push_back(*d);
		}
		core::Result<std::shared_ptr<const sw::WeaponTable>> table =
				sw::make_weapon_table(defs, level->shared->gunplay->materials);
		REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
		REQUIRE((*table)->defs[0].weapon_class == host::kRepairToolClass);
		CHECK(sw::sim_effective_rpm((*table)->defs[0]) == doctest::Approx(600.0));
		host::VehicleDamageSource source;
		source.configure(**table, defs);
		CHECK(source.repair(0) == 6.0f);
		CHECK(source.repair(1) == 0.0f);
		CHECK(source.repair(7) == 0.0f);
		const host::LevelVehicles &lv = *level->shared->vehicles;
		const host::LevelVehicleKind *tank = nullptr;
		for (const host::LevelVehicleKind &k : lv.kinds) {
			tank = k.blueprint == "vehicles/m1a2/m1abrams" ? &k : tank;
		}
		REQUIRE(tank != nullptr);
		const sim::vehicles::HealthZoneRule &rear = tank->damage.zones[size_t(sim::vehicles::HullZone::Rear)];
		REQUIRE(rear.max_shield > 0.0f);
		sim::vehicles::VehicleHealth h = sim::vehicles::spawned_vehicle_health(tank->damage);
		const size_t ri = size_t(sim::vehicles::HullZone::Rear);
		h.shield[ri] = 0.0f;
		sim::vehicles::VehicleHit hit;
		hit.zone = sim::vehicles::HullZone::Rear;
		hit.damage = 100.0f;
		REQUIRE(sim::vehicles::apply_vehicle_damage(tank->damage, h, hit).applied);
		const float damaged = sim::vehicles::vehicle_health(tank->damage, h);
		hit.damage = -source.repair(0);
		hit.modifiers = false;
		hit.modifier = 0.0f;
		for (int shot = 0; shot < 10; ++shot) {
			sim::vehicles::apply_vehicle_damage(tank->damage, h, hit);
		}
		const float full = sim::vehicles::full_vehicle_health(tank->damage);
		CHECK(sim::vehicles::vehicle_health(tank->damage, h) ==
				doctest::Approx(std::min(full, damaged + 60.0f * full / rear.max_health)));
		for (int shot = 0; shot < 400; ++shot) {
			sim::vehicles::apply_vehicle_damage(tank->damage, h, hit);
		}
		CHECK(h.health[ri] == rear.max_health);
		CHECK(h.shield[ri] == rear.max_shield);
		std::printf("repair: %s rear %.0f + shield %.0f, 10 repair tool shots (1 s) give %.1f health\n",
				tank->blueprint.c_str(), double(rear.max_health), double(rear.max_shield),
				double(60.0f * full / rear.max_health));
	}
}

TEST_SUITE("host-vehicles-damage-team") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: a teammate's shells wreck a friendly Growler with friendly fire off; "
			  "the splash spares its driver, the wreck still kills him, credited to himself, not to the gunner") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const WreckRun run = wreck_and_respawn(
				*level,
				[](const host::LevelVehicleKind &p_kind) {
					return p_kind.blueprint == "vehicles/growleritv/growleritv";
				},
				true);
		REQUIRE(run.ran);
		CHECK(run.shells >= 2u);
		CHECK(run.driver_untouched_until_wrecked);
		CHECK(run.crew_kills == 1u);
		CHECK(run.driver_dead);
		CHECK(run.driver_deaths_gained == 1);
		CHECK(run.kills_gained == 0);
		check_respawn(run);
	}
}

namespace {

sim::Vec3 beside_hull(const Match &p_match, const net::VehicleWire &p_vehicle, float p_side) {
	const std::array<float, 2> f = heading_of(p_vehicle.state.body.rotation);
	const sim::Vec3 &c = p_vehicle.state.body.com_position;
	return ground_at(*p_match.world, c[0] + p_side * f[1], c[2] - p_side * f[0], c[1] + 5.0f);
}

}

TEST_SUITE("host-vehicles-damage-team") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: an engineer's repair tool repairs a vehicle of his team and is a "
			  "plain hit on one of the enemy's (R3-G3 E)") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::unique_ptr<Match> m = start_match(*level, true, 113u, 0, 1);
		tdm::ScriptedPeer &mechanic = m->join("mechanic");
		mechanic.ready();
		REQUIRE(wait_in_round(*m));
		mechanic.spawn_request(uint8_t(host::KitRole::Engineer));
		REQUIRE(m->until([&] { return is_alive(m->state(mechanic.slot())); }, 900));
		const uint8_t team = m->rules().team_of(uint8_t(mechanic.slot()));
		const size_t tool_slot = size_t(core::gameplay::WeaponSlot::Gadget2);
		const uint16_t tool = m->server.gunplay()->weapons(uint8_t(mechanic.slot())).slots[tool_slot].def;
		REQUIRE(tool < gl.defs.size());
		REQUIRE(gl.defs[tool].variant == "weapons/gadgets/repairtool/u_repairtool");
		VehicleWatch watch;
		std::optional<uint16_t> ours, theirs;
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, mechanic.peer());
					for (const net::VehicleWire &w : watch.latest) {
						if (!parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
							continue;
						}
						ours = !ours && w.team == team ? std::optional<uint16_t>(w.spawner) : ours;
						theirs = !theirs && w.team != team && w.team != 0 ? std::optional<uint16_t>(w.spawner) : theirs;
					}
					return ours.has_value() && theirs.has_value();
				},
				sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u));
		bool fire = false;
		sim::Vec3 aim{};
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, aim, sim::Pose::Stand, select_and_fire(uint8_t(tool_slot), fire));
		};
		const auto repair_run = [&](uint16_t p_spawner) {
			m->teleport(mechanic, beside_hull(*m, *watch.of(p_spawner), 2.2f));
			m->ticks(30);
			watch.take(m->out, mechanic.peer());
			aim = add(watch.of(p_spawner)->state.body.com_position, 0.0f, 0.3f, 0.0f);
			m->ticks(30);
			const uint64_t repairs = m->server.stats().vehicle_repairs, hits = m->server.stats().vehicle_hits;
			fire = true;
			m->ticks(60);
			fire = false;
			m->ticks(10);
			return std::pair<uint64_t, uint64_t>{ m->server.stats().vehicle_repairs - repairs,
				m->server.stats().vehicle_hits - hits };
		};
		const std::pair<uint64_t, uint64_t> own = repair_run(*ours);
		const std::pair<uint64_t, uint64_t> enemy = repair_run(*theirs);
		std::printf("repair: a second of the tool on his team's %s: %llu repairs, %llu hits; on the enemy's %s: %llu "
					"repairs, %llu hits\n",
				lv.kinds[lv.spawners[*ours].kind].blueprint.c_str(), (unsigned long long)own.first,
				(unsigned long long)own.second, lv.kinds[lv.spawners[*theirs].kind].blueprint.c_str(),
				(unsigned long long)enemy.first, (unsigned long long)enemy.second);
		CHECK(own.first >= 5u);
		CHECK(own.second == own.first);
		CHECK(enemy.first == 0u);
		CHECK(enemy.second >= 5u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("vehicles: MP_007 ConquestSmall0: an engineer's M15 stays quiet under his mate driving an enemy hull "
			  "and goes off under an enemy driver backing it over the mine (R2-G4 C: the driver's team counts), "
			  "wrecking it with the kill on the engineer") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::unique_ptr<Match> m = start_match(*level, true, 113u, 0, 1);
		tdm::ScriptedPeer &sapper = m->join("sapper");
		tdm::ScriptedPeer &enemy = m->join("enemy");
		sapper.ready();
		enemy.ready();
		REQUIRE(wait_in_round(*m));
		std::vector<Script *> bots{ m->bot(), m->bot() };
		const uint8_t engineer = uint8_t(host::KitRole::Engineer);
		const uint8_t team = m->rules().team_of(uint8_t(sapper.slot()));
		const uint8_t other = m->rules().team_of(uint8_t(enemy.slot()));
		REQUIRE(team != other);
		Script *mate = nullptr;
		for (Script *b : bots) {
			mate = m->rules().team_of(uint8_t(b->slot)) == team ? b : mate;
		}
		REQUIRE(mate != nullptr);
		const host::LoadoutKit *kit = gl.loadouts.kit(team, engineer);
		REQUIRE(kit != nullptr);
		const size_t mine_slot = size_t(core::gameplay::WeaponSlot::Gadget2);
		const auto m15 = std::find_if(kit->slots[mine_slot].begin(), kit->slots[mine_slot].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/m15/u_m15"; });
		REQUIRE(m15 != kit->slots[mine_slot].end());
		net::LoadoutRequest request;
		request.kit = engineer;
		request.items[mine_slot] = m15->identifier;
		m->server.on_receive(sapper.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		sapper.spawn_request(engineer);
		enemy.spawn_request(0);
		REQUIRE(m->until(
				[&] {
					return is_alive(m->state(sapper.slot())) && is_alive(m->state(enemy.slot())) &&
							is_alive(m->state(mate->slot));
				},
				900));
		REQUIRE(m->server.gunplay()->weapons(uint8_t(sapper.slot())).slots[mine_slot].def == m15->def);

		VehicleWatch watch;
		std::optional<uint16_t> hull;
		float hull_clear = 0.0f;
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, sapper.peer());
					std::vector<sim::Vec3> others;
					for (const net::VehicleWire &o : watch.latest) {
						others.push_back(o.state.body.com_position);
					}
					for (const net::VehicleWire &w : watch.latest) {
						if (w.team != other || host::land_tracked(*lv.kinds[lv.spawners[w.spawner].kind].model) ||
								!parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
							continue;
						}
						const float c =
								clear_run(*m->world, w.state.body.com_position, w.state.body.rotation, others, 40.0f);
						if (c > hull_clear) {
							hull = w.spawner;
							hull_clear = c;
						}
					}
					return hull_clear >= 35.0f;
				},
				sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u));
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[*hull].kind];
		const net::VehicleWire parked = *watch.of(*hull);
		const sim::Vec3 start = parked.state.body.com_position;
		const std::array<float, 2> line = heading_of(parked.state.body.rotation);
		const auto along = [&](const sim::Vec3 &p_at) {
			return (p_at[0] - start[0]) * line[0] + (p_at[2] - start[2]) * line[1];
		};
		const sim::Vec3 spot =
				ground_at(*m->world, start[0] + 10.0f * line[0], start[2] + 10.0f * line[1], start[1] + 5.0f);
		m->teleport(sapper, add(spot, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		bool lay = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, add(spot, line[0], 0.1f, line[1]), sim::Pose::Stand,
					select_and_fire(uint8_t(mine_slot), lay && (p_tick % 30u) < 10u));
		};
		m->ticks(60);
		lay = true;
		const std::optional<sim::Tick> rested = rest_tick(*m, 1, 300);
		lay = false;
		REQUIRE(rested.has_value());
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		const sw::Deployable mine = m->server.gunplay()->deployables()[0];
		CHECK(mine.def == m15->def);
		const float off_line =
				std::fabs((mine.position[0] - start[0]) * line[1] - (mine.position[2] - start[2]) * line[0]);
		std::printf("M15: on the line of the %s of spawner %u (team %u, %.0f m clear), %.1f m along it and %.2f m "
					"off it; the sapper is on team %u\n",
				kind.blueprint.c_str(), unsigned(*hull), unsigned(other), double(hull_clear),
				double(along(mine.position)), double(off_line), unsigned(team));
		REQUIRE(off_line < 1.5f);
		m->teleport(sapper, beside_hull(*m, parked, -25.0f));

		sim::Tick use_at = 0;
		int8_t throttle = 0;
		const auto driver = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			c.buttons |= p_tick == use_at ? sim::buttons::kUse : 0u;
			c.move_forward = throttle;
			return c;
		};
		const auto take_wheel = [&](int p_slot) {
			throttle = 0;
			const sim::Vec3 at = beside_hull(*m, *watch.of(*hull), 2.0f);
			REQUIRE(m->until([&] { return horizontal(m->state(p_slot)->position, at) < 0.5f; }, 30));
			use_at = m->now() + 3u;
			REQUIRE(m->until(
					[&] {
						watch.take(m->out, sapper.peer());
						return watch.of(*hull)->occupants[0] == uint8_t(p_slot);
					},
					60));
		};
		const auto stats = [&] { return m->server.gunplay()->gadget_stats(); };
		m->ticks(sim::seconds_to_ticks(4.5f));
		m->place(mate->slot, beside_hull(*m, *watch.of(*hull), 2.0f));
		mate->fn = driver;
		take_wheel(mate->slot);
		throttle = 127;
		CHECK(m->until(
				[&] {
					watch.take(m->out, sapper.peer());
					return along(watch.of(*hull)->state.body.com_position) > 20.0f;
				},
				600));
		throttle = -127;
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, sapper.peer());
					const sim::Vec3 &v = watch.of(*hull)->state.body.linear_velocity;
					return v[0] * line[0] + v[2] * line[1] <= 0.2f;
				},
				300));
		throttle = 0;
		std::printf("M15: the mate drove the %s %.1f m along its line over the mine: %llu vehicle trips\n",
				kind.blueprint.c_str(), double(along(watch.of(*hull)->state.body.com_position)),
				(unsigned long long)stats().field.vehicle_trips);
		CHECK(stats().field.vehicle_trips == 0u);
		CHECK(stats().pack_detonations == 0u);
		CHECK(m->server.gunplay()->deployables().size() == 1u);
		use_at = m->now() + 3u;
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, sapper.peer());
					return watch.of(*hull)->occupants[0] == net::kNoVehicleOccupant;
				},
				60));
		m->place(mate->slot, beside_hull(*m, *watch.of(*hull), 30.0f));

		const uint64_t hits = m->server.stats().vehicle_hits;
		const uint64_t destroyed = m->server.stats().vehicles_destroyed;
		m->teleport(enemy, beside_hull(*m, *watch.of(*hull), 2.0f));
		m->scripts[1] = driver;
		take_wheel(enemy.slot());
		throttle = -127;
		CHECK(m->until([&] { return stats().pack_detonations > 0u; }, 600));
		throttle = 0;
		m->ticks(5);
		size_t detonations = 0;
		for (const net::CombatEvents &c : enemy.combat) {
			for (const net::DetonationWire &d : c.detonations) {
				detonations += d.def == m15->def ? 1u : 0u;
			}
		}
		std::printf("M15: the enemy backed the %s over it: %llu vehicle trips, %llu pack detonations, %llu hull hits, "
					"%zu detonations on his wire\n",
				kind.blueprint.c_str(), (unsigned long long)stats().field.vehicle_trips,
				(unsigned long long)stats().pack_detonations,
				(unsigned long long)(m->server.stats().vehicle_hits - hits), detonations);
		CHECK(stats().field.vehicle_trips == 1u);
		CHECK(stats().pack_detonations == 1u);
		CHECK(m->server.gunplay()->deployables().empty());
		CHECK(m->server.stats().vehicle_hits > hits);
		CHECK(detonations == 1u);
		CHECK(m->server.stats().vehicles_destroyed == destroyed + 1u);
		CHECK_FALSE(is_alive(m->state(enemy.slot())));
		const net::DeathEvent *death = last_death(enemy, enemy.slot());
		REQUIRE(death != nullptr);
		CHECK(int(death->killer) == sapper.slot());
		CHECK(death->def == m15->def);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

namespace {

struct LauncherWatch final : host::GunplayObserver {
	std::vector<sw::DamageEvent> hits;
	std::vector<sw::DetonationEvent> detonations;
	std::vector<sw::ShotEvent> shots;

	void weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sw::SoldierWeaponsState &p_after,
			const sw::WeaponTickOutput &p_out) override {
		(void)p_slot, (void)p_command, (void)p_after;
		shots.insert(shots.end(), p_out.shots.begin(), p_out.shots.begin() + p_out.shot_count);
	}
	void projectiles(
			uint32_t p_tick, const sw::ProjectileStepOutput &p_out, std::span<const sw::Projectile> p_live) override {
		(void)p_tick, (void)p_live;
		detonations.insert(detonations.end(), p_out.detonations.begin(), p_out.detonations.end());
	}
	void damage(uint32_t p_tick, const sw::DamageEvent &p_event, bool p_applied, float p_health_after) override {
		(void)p_tick, (void)p_health_after;
		if (p_applied) {
			hits.push_back(p_event);
		}
	}
};

}

TEST_SUITE("host-vehicles-damage-team") {
	TEST_CASE(
			"gadgets: MP_007 ConquestSmall0: an engineer's kit-default launcher in slot 2 fires in a match; of two "
			"rockets into an enemy MBT's rear the first is spent on the full rear shield and pushes the parked hull by "
			"its impulse over its mass, and the second takes 22.275 % of its health (R-G3, R-G4)") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles &lv = *level->shared->vehicles;
		std::unique_ptr<Match> m = start_match(*level, true, 113u, 0, 1);
		tdm::ScriptedPeer &gunner = m->join("gunner");
		gunner.ready();
		REQUIRE(wait_in_round(*m));
		gunner.spawn_request(uint8_t(host::KitRole::Engineer));
		REQUIRE(m->until([&] { return is_alive(m->state(gunner.slot())); }, 900));
		const uint8_t team = m->rules().team_of(uint8_t(gunner.slot()));
		const uint8_t launcher_slot = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		const uint16_t launcher = m->server.gunplay()->weapons(uint8_t(gunner.slot())).slots[launcher_slot].def;
		REQUIRE(launcher < gl.defs.size());
		CHECK(gl.defs[launcher].variant == (team == 1 ? "weapons/smaw/u_smaw" : "weapons/rpg7/u_rpg7"));
		const auto mbt = [&](const net::VehicleWire &p_w) {
			const std::string &b = lv.kinds[lv.spawners[p_w.spawner].kind].blueprint;
			return b == "vehicles/t90/t90" || b == "vehicles/m1a2/m1abrams";
		};
		VehicleWatch watch;
		std::optional<uint16_t> tank;
		sim::Vec3 stand{}, aim{};
		REQUIRE(m->until(
				[&] {
					watch.take(m->out, gunner.peer());
					for (const net::VehicleWire &w : watch.latest) {
						if (tank || w.team == team || w.team == 0 || !mbt(w) ||
								!parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
							continue;
						}
						const std::array<float, 2> f = heading_of(w.state.body.rotation);
						const sim::Vec3 &c = w.state.body.com_position;
						for (float d = 40.0f; !tank && d >= 30.0f; d -= 2.0f) {
							const sim::Vec3 at = ground_at(*m->world, c[0] - d * f[0], c[2] - d * f[1], c[1] + 5.0f);
							const host::LevelVehicleKind &k = lv.kinds[lv.spawners[w.spawner].kind];
							const core::Transform turn = sim::vehicles::hull_transform(w.state.body.rotation, {});
							const sim::Vec3 origin = core::vec::sub(c, turn.transform_vector(k.model->center_of_mass));
							const sim::Vec3 centre = sim::vehicles::hull_transform(w.state.body.rotation, origin)
															 .transform_point(k.box.centre);
							if (std::fabs(at[1] - c[1]) < 3.0f && m->motor.fits_standing(at) &&
									clear(*m->world, add(at, 0.0f, 1.6f, 0.0f), centre)) {
								tank = w.spawner;
								stand = at;
								aim = centre;
							}
						}
					}
					return tank.has_value();
				},
				sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u));
		const float full = sim::vehicles::full_vehicle_health(lv.kinds[lv.spawners[*tank].kind].damage);
		REQUIRE(m->server.vehicle_health(*tank) == full);
		m->teleport(gunner, stand);
		m->ticks(20);
		bool fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, aim, sim::Pose::Stand,
					select_and_fire(launcher_slot, fire && (p_tick % 30u) < 10u) | sim::buttons::kZoom);
		};
		m->ticks(90);
		std::optional<sim::VehicleState> before, after;
		std::optional<sw::DetonationEvent> blast;
		const auto rocket = [&] {
			const uint64_t shots = m->server.gunplay()->stats().shots, hits = m->server.stats().vehicle_hits;
			fire = true;
			const bool fired = m->until([&] { return m->server.gunplay()->stats().shots > shots; }, 600);
			fire = false;
			before = m->server.vehicle_state(*tank);
			const bool hit = m->until(
					[&] {
						if (m->server.stats().vehicle_hits > hits) {
							return true;
						}
						before = m->server.vehicle_state(*tank);
						return false;
					},
					120);
			after = m->server.vehicle_state(*tank);
			blast.reset();
			for (const sw::DetonationEvent &d : m->server.gunplay()->step_output().detonations) {
				if (d.def == launcher && d.player_slot == gunner.slot()) {
					blast = d;
				}
			}
			m->ticks(1);
			return std::pair<bool, float>{ fired && hit, m->server.vehicle_health(*tank).value_or(-1.0f) };
		};
		const uint64_t pushes = m->server.stats().vehicle_pushes;
		const std::pair<bool, float> first = rocket();
		REQUIRE(before.has_value());
		REQUIRE(after.has_value());
		REQUIRE(blast.has_value());
		const sw::ProjectileSimDef &round = m->server.gunplay()->table().def(launcher, blast->variant).projectile;
		const sw::ExplosionSimDef &x = blast->dud ? round.dud : round.explosion;
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[*tank].kind];
		const core::Transform hull = sim::vehicles::hull_transform(after->body.rotation, after->body.origin);
		sw::ImpulseBox box;
		box.centre = hull.transform_point(kind.box.centre);
		box.axes = hull.axes();
		box.half = kind.box.half;
		const sim::Vec3 toward = core::vec::sub(box.centre, blast->position);
		const sim::Vec3 dv = core::vec::sub(after->body.linear_velocity, before->body.linear_velocity);
		const float mass = kind.model->mass;
		const sim::Vec3 expected =
				core::vec::scale(sw::explosion_impulse(x, blast->position, box.centre, box), 1.0f / mass);
		std::printf(
				"launcher: the first rocket's blast %.2f m from the hull-box centre (BlastImpulse %.0f, radius %.1f, "
				"ShockwaveImpulse %.0f) pushes the %.0f kg hull %s by (%.5f %.5f %.5f) m/s, expected (%.5f %.5f "
				"%.5f)\n",
				double(core::vec::length(toward)), double(x.impulse), double(x.radius), double(x.shockwave_impulse),
				double(mass), before->body.active != 0 ? "awake" : "asleep", double(dv[0]), double(dv[1]),
				double(dv[2]), double(expected[0]), double(expected[1]), double(expected[2]));
		CHECK(before->body.active == 0);
		CHECK(core::vec::length(before->body.linear_velocity) == 0.0f);
		CHECK(core::vec::length(expected) > 0.0f);
		CHECK(core::vec::dot(core::vec::normalize_or(dv, {}), core::vec::normalize_or(toward, {})) ==
				doctest::Approx(1.0f));
		CHECK(core::vec::length(dv) * mass ==
				doctest::Approx(sw::blast_impulse(x, double(core::vec::length(toward))) *
						double(sw::impulse_size_factor(box, toward))));
		for (size_t i = 0; i < 3; ++i) {
			CHECK(dv[i] == doctest::Approx(expected[i]));
		}
		CHECK(after->body.angular_velocity == before->body.angular_velocity);
		const std::pair<bool, float> second = rocket();
		std::printf("launcher: %s from %.1f m behind the %s of spawner %u (health %.0f): after the first rocket %.2f, "
					"after the second %.2f (%.2f %% of its health)\n",
				gl.defs[launcher].variant.c_str(), double(horizontal(stand, aim)),
				lv.kinds[lv.spawners[*tank].kind].blueprint.c_str(), unsigned(*tank), double(full),
				double(first.second), double(second.second), double(100.0f * (first.second - second.second) / full));
		const sim::vehicles::HealthZoneRule &rear =
				lv.kinds[lv.spawners[*tank].kind].damage.zones[size_t(sim::vehicles::HullZone::Rear)];
		const float square = sim::vehicles::zone_angle_factor(rear, 0.0f);
		std::printf("launcher: the rear zone's MinDamageAngle %.1f and DamageAngleMultiplier %.2f give a straight hit "
					"x%.3f on the rule row's 20.25 %%\n",
				double(rear.min_damage_angle), double(rear.damage_angle_multiplier), double(square));
		CHECK(first.first);
		CHECK(second.first);
		CHECK(first.second == doctest::Approx(full));
		CHECK(100.0f * (first.second - second.second) / full == doctest::Approx(20.25f * square));
		CHECK(100.0f * (first.second - second.second) / full == doctest::Approx(22.275f));
		CHECK(m->server.stats().vehicle_pushes >= pushes + 2u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-gunplay-gadgets") {
	TEST_CASE("gadgets: an assault's slot 2 M320 picked in CUSTOMIZE fires each of its rounds in a Metro match: HE a "
			  "dud before TimeToArm and a direct hit after, buckshot 12 pellets of falloff damage, smoke a 0-damage "
			  "blast whose ShockwaveDamage floor the soldier zones' thresholds cut, the low-velocity grenade going off "
			  "at its TimeToLive") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		auto watch = std::make_shared<LauncherWatch>();
		std::unique_ptr<Match> m = start_match(
				*level, true, 91u, 0, 1, [&](host::GameServerConfig &r_config) { r_config.gunplay.observer = watch; });
		std::vector<tdm::ScriptedPeer *> peers;
		for (const char *name : { "p1", "p2", "p3", "p4", "p5", "p6", "p7" }) {
			peers.push_back(&m->join(name));
			peers.back()->ready();
		}
		REQUIRE(wait_in_round(*m));
		std::array<std::vector<tdm::ScriptedPeer *>, 3> by_team;
		for (tdm::ScriptedPeer *p : peers) {
			const uint8_t t = m->rules().team_of(uint8_t(p->slot()));
			REQUIRE((t == 1 || t == 2));
			by_team[t].push_back(p);
		}
		const uint8_t team = by_team[1].size() >= 4 ? 1 : 2;
		REQUIRE(by_team[team].size() >= 4u);
		REQUIRE(by_team[3 - team].size() >= 3u);
		const uint8_t assault = uint8_t(host::KitRole::Assault);
		const size_t gadget1 = size_t(core::gameplay::WeaponSlot::Gadget1);
		const host::LoadoutKit *kit = gl.loadouts.kit(team, assault);
		REQUIRE(kit != nullptr);
		const char *const rounds[] = { "weapons/gadgets/m320/u_m320_he", "weapons/gadgets/m320/u_m320_shg",
			"weapons/gadgets/m320/u_m320_smk", "weapons/gadgets/m320/u_m320_lvg" };
		std::array<uint16_t, 4> defs{};
		for (size_t r = 0; r < 4; ++r) {
			const auto item = std::find_if(kit->slots[gadget1].begin(), kit->slots[gadget1].end(),
					[&](const host::LoadoutItem &i) { return i.unlock == rounds[r]; });
			REQUIRE(item != kit->slots[gadget1].end());
			REQUIRE(item->def < gl.defs.size());
			defs[r] = item->def;
			net::LoadoutRequest request;
			request.kit = assault;
			request.items[gadget1] = item->identifier;
			m->server.on_receive(by_team[team][r]->peer(), net::kChannelReliable, net::encode_loadout_request(request));
		}
		m->ticks(1);
		for (tdm::ScriptedPeer *p : peers) {
			p->spawn_request(assault);
		}
		REQUIRE(m->until(
				[&] {
					return std::all_of(peers.begin(), peers.end(),
							[&](tdm::ScriptedPeer *p) { return is_alive(m->state(p->slot())); });
				},
				900));
		for (size_t r = 0; r < 4; ++r) {
			CHECK(m->server.gunplay()->weapons(uint8_t(by_team[team][r]->slot())).slots[gadget1].def == defs[r]);
		}
		const sw::WeaponTable &table = m->server.gunplay()->table();
		std::optional<std::pair<sim::Vec3, sim::Vec3>> lane;
		std::vector<sim::Vec3> floors;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			if (const std::optional<sim::Vec3> f = floor_at(*m->world, sp.position);
					f && m->motor.fits_standing(add(*f, 0.0f, 0.05f, 0.0f))) {
				floors.push_back(*f);
			}
		}
		for (size_t a = 0; a < floors.size() && !lane; ++a) {
			for (size_t b = 0; b < floors.size() && !lane; ++b) {
				const float d = horizontal(floors[a], floors[b]);
				if (d >= 26.0f && d <= 40.0f && std::fabs(floors[a][1] - floors[b][1]) < 1.0f &&
						clear(*m->world, add(floors[a], 0.0f, 1.6f, 0.0f), add(floors[b], 0.0f, 1.2f, 0.0f)) &&
						clear(*m->world, add(floors[a], 0.0f, 1.6f, 0.0f), add(floors[b], 0.0f, 0.1f, 0.0f))) {
					lane = std::pair<sim::Vec3, sim::Vec3>{ floors[a], floors[b] };
				}
			}
		}
		REQUIRE(lane.has_value());
		const sim::Vec3 from = lane->first, to = lane->second;
		const float run = horizontal(from, to);
		const sim::Vec3 toward{ (from[0] - to[0]) / run, 0.0f, (from[2] - to[2]) / run };
		std::optional<sim::Vec3> aside;
		for (const sim::Vec3 &f : floors) {
			if (!aside && horizontal(f, from) > 30.0f && horizontal(f, to) > 30.0f) {
				aside = f;
			}
		}
		REQUIRE(aside.has_value());
		for (tdm::ScriptedPeer *p : peers) {
			m->teleport(*p, add(*aside, 0.0f, 0.05f, 0.0f));
		}
		m->ticks(10);
		bool fire = false;
		sim::Vec3 aim{};
		size_t shooter = 0;
		for (size_t r = 0; r < 4; ++r) {
			const size_t i = size_t(std::find(peers.begin(), peers.end(), by_team[team][r]) - peers.begin());
			m->scripts[i] = [&, r](sim::Tick p_tick, const sim::SoldierState &p_state) {
				return aim_command(*m, p_tick, p_state, aim, sim::Pose::Stand,
						select_and_fire(uint8_t(gadget1), shooter == r && fire && (p_tick % 30u) < 10u) |
								(shooter == r ? sim::buttons::kZoom : 0u));
			};
		}
		const auto shoot = [&](size_t p_round, tdm::ScriptedPeer &r_victim, const sim::Vec3 &p_at,
								   const sim::Vec3 &p_aim, uint32_t p_after) {
			shooter = p_round;
			m->teleport(*by_team[team][p_round], add(p_at, 0.0f, 0.05f, 0.0f));
			m->teleport(r_victim, add(to, 0.0f, 0.05f, 0.0f));
			aim = p_aim;
			m->ticks(90);
			watch->hits.clear();
			watch->detonations.clear();
			watch->shots.clear();
			fire = true;
			const bool fired = m->until([&] { return !watch->shots.empty(); }, 120);
			fire = false;
			m->ticks(p_after);
			m->teleport(*by_team[team][p_round], add(*aside, 0.0f, 0.05f, 0.0f));
			if (is_alive(m->state(r_victim.slot()))) {
				m->teleport(r_victim, add(*aside, 1.0f, 0.05f, 0.0f));
			}
			m->ticks(5);
			return fired;
		};
		const auto victim_hits = [&](const tdm::ScriptedPeer &p_victim, sw::DamageKind p_kind) {
			std::vector<sw::DamageEvent> out;
			for (const sw::DamageEvent &e : watch->hits) {
				if (int(e.target) == p_victim.slot() && e.kind == p_kind) {
					out.push_back(e);
				}
			}
			return out;
		};
		tdm::ScriptedPeer &v_ground = *by_team[3 - team][0];
		tdm::ScriptedPeer &v_buck = *by_team[3 - team][1];
		tdm::ScriptedPeer &v_direct = *by_team[3 - team][2];
		const sim::Vec3 chest = add(to, 0.0f, 1.2f, 0.0f);

		const sw::ProjectileSimDef &he = table.defs[defs[0]].projectile;
		const sim::Vec3 near = add(to, 5.0f * toward[0], 0.0f, 5.0f * toward[2]);
		REQUIRE(shoot(0, v_ground, near, add(to, 1.5f * toward[0], 0.05f, 1.5f * toward[2]), 60));
		REQUIRE(watch->detonations.size() == 1u);
		CHECK(watch->detonations[0].def == defs[0]);
		CHECK(watch->detonations[0].dud);
		const std::vector<sw::DamageEvent> dud = victim_hits(v_ground, sw::DamageKind::DudBlast);
		REQUIRE(dud.size() == 1u);
		CHECK(dud[0].base == doctest::Approx(sw::blast_damage(he.dud, dud[0].distance, table.mechanics.blast_falloff)));
		std::printf("M320 HE from 5 m: a dud, %.1f damage at %.2f m (dud BlastDamage %.0f, inner %.2f m)\n",
				double(dud[0].amount), double(dud[0].distance), double(he.dud.blast_damage),
				double(he.dud.inner_radius));

		const int deaths_direct = int(m->row(v_direct.slot())->deaths);
		const float flight = run / he.launch_velocity[2];
		const float lead = -0.5f * he.gravity * flight * flight - he.launch_velocity[1] * flight;
		REQUIRE(shoot(0, v_direct, from, add(chest, 0.0f, lead, 0.0f), 90));
		REQUIRE(watch->detonations.size() == 1u);
		CHECK_FALSE(watch->detonations[0].dud);
		const std::vector<sw::DamageEvent> direct = victim_hits(v_direct, sw::DamageKind::Direct);
		REQUIRE(direct.size() == 1u);
		CHECK(direct[0].def == defs[0]);
		CHECK(direct[0].base == doctest::Approx(sw::falloff_damage(he, direct[0].distance)));
		CHECK(int(m->row(v_direct.slot())->deaths) == deaths_direct + 1);
		std::printf("M320 HE from %.1f m: armed, a direct hit of %.1f (StartDamage %.0f x %.2f)\n", double(run),
				double(direct[0].amount), double(direct[0].base), double(direct[0].multiplier));

		const sw::WeaponSimDef &shg = table.defs[defs[1]];
		REQUIRE(shoot(1, v_buck, from, chest, 30));
		REQUIRE(watch->shots.size() == 1u);
		CHECK(watch->shots[0].pellets == 12u);
		CHECK(shg.pellets == 12);
		CHECK(watch->detonations.empty());
		const std::vector<sw::DamageEvent> pellets = victim_hits(v_buck, sw::DamageKind::Bullet);
		REQUIRE_FALSE(pellets.empty());
		for (const sw::DamageEvent &e : pellets) {
			CHECK(e.def == defs[1]);
			CHECK(e.base == doctest::Approx(sw::falloff_damage(shg.projectile, e.distance)));
		}
		std::printf(
				"M320 buckshot from %.1f m: %zu of 12 pellets hit, %.2f base each (%.0f -> %.1f over %.0f..%.0f m)\n",
				double(run), pellets.size(), double(pellets[0].base), double(shg.projectile.start_damage),
				double(shg.projectile.end_damage), double(shg.projectile.falloff_start),
				double(shg.projectile.falloff_end));

		const sw::ProjectileSimDef &smk = table.defs[defs[2]].projectile;
		REQUIRE(shoot(2, v_ground, from, add(to, 2.0f * toward[0], 0.05f, 2.0f * toward[2]), 90));
		REQUIRE(watch->detonations.size() == 1u);
		CHECK(watch->detonations[0].def == defs[2]);
		CHECK_FALSE(watch->detonations[0].dud);
		CHECK(smk.explosion.blast_damage == 0.0f);
		bool spared = true;
		for (size_t z = 0; z < sw::kHitZoneCount; ++z) {
			spared = spared &&
					sim::vehicles::material_relation_damage(smk.explosion.shockwave_damage,
							smk.explosion.zone_multiplier[z], smk.explosion.zone_threshold[z]) == 0.0f;
		}
		CHECK(spared);
		CHECK(victim_hits(v_ground, sw::DamageKind::Blast).empty());
		CHECK(victim_hits(v_ground, sw::DamageKind::Direct).empty());
		std::printf("M320 smoke: detonated, ShockwaveDamage %.0f out to %.0f m is under the soldier zones' "
					"thresholds (%.0f body, %.0f legs): no damage\n",
				double(smk.explosion.shockwave_damage), double(smk.explosion.gather_radius()),
				double(smk.explosion.zone_threshold[size_t(sw::HitZone::Body)]),
				double(smk.explosion.zone_threshold[size_t(sw::HitZone::Legs)]));

		const sw::ProjectileSimDef &lvg = table.defs[defs[3]].projectile;
		CHECK(lvg.kind == core::gameplay::ProjectileKind::Grenade);
		REQUIRE(shoot(3, v_ground, from, add(from, -8.0f * toward[0], -1.0f, -8.0f * toward[2]),
				uint32_t(sim::seconds_to_ticks(float(sw::units_to_seconds(lvg.time_to_live)))) + 30u));
		REQUIRE(watch->detonations.size() == 1u);
		CHECK(watch->detonations[0].def == defs[3]);
		CHECK(watch->detonations[0].timeout);
		CHECK(watch->detonations[0].time - watch->shots[0].spawn_time == lvg.time_to_live);
		std::printf("M320 low-velocity grenade: bounced, went off at its TimeToLive %.2f s\n",
				sw::units_to_seconds(lvg.time_to_live));
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-gunplay-gadgets") {
	TEST_CASE("gadgets: a C4 outlives its owner's death by its TimeToLiveOnPlayerDeath (5 s) and goes without a "
			  "blast (R2-G4 C: the owner-gone timer)") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		std::unique_ptr<Match> m = start_match(*level, true, 95u, 0, 1);
		tdm::ScriptedPeer &sapper = m->join("sapper");
		sapper.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t support = uint8_t(host::KitRole::Support);
		const uint8_t team = m->rules().team_of(uint8_t(sapper.slot()));
		const host::LoadoutKit *kit = gl.loadouts.kit(team, support);
		REQUIRE(kit != nullptr);
		const size_t gadget2 = size_t(core::gameplay::WeaponSlot::Gadget2);
		const auto c4 = std::find_if(kit->slots[gadget2].begin(), kit->slots[gadget2].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/c4/u_c4"; });
		REQUIRE(c4 != kit->slots[gadget2].end());
		const sw::GadgetDef *pack = gadget_named(gl, "weapons/gadgets/c4/c4");
		REQUIRE(pack != nullptr);
		net::LoadoutRequest request;
		request.kit = support;
		request.items[gadget2] = c4->identifier;
		m->server.on_receive(sapper.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		sapper.spawn_request(support);
		REQUIRE(m->until([&] { return is_alive(m->state(sapper.slot())); }, 600));
		const std::optional<Drop> drop = find_drop(*m, true);
		REQUIRE(drop.has_value());
		const std::optional<Patch> patch = find_patch(*m, 3.0f);
		REQUIRE(patch.has_value());
		m->teleport(sapper, add(patch->shooter, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		bool fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, add(patch->victim, 0.0f, 0.1f, 0.0f), sim::Pose::Stand,
					select_and_fire(uint8_t(gadget2), fire && (p_tick % 30u) < 10u));
		};
		m->ticks(60);
		fire = true;
		REQUIRE(rest_tick(*m, 1, 300).has_value());
		fire = false;
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		m->teleport(sapper, add(drop->floor, 0.0f, drop->height, 0.0f));
		REQUIRE(m->until([&] { return !is_alive(m->state(sapper.slot())); }, 600));
		const sim::Tick died = m->now();
		m->ticks(2);
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		const sw::Deployable orphan = m->server.gunplay()->deployables()[0];
		CHECK(orphan.owner_gone);
		CHECK_FALSE(orphan.owner_left);
		CHECK(pack->time_to_live_on_owner_gone_s == 5.0f);
		REQUIRE(m->until([&] { return m->server.gunplay()->deployables().empty(); }, sim::seconds_to_ticks(6.0f)));
		const float lived = float(m->now() - died) * sim::kTickSeconds;
		std::printf("C4: its owner died at tick %u; it stayed %.2f s and went without a blast\n", unsigned(died),
				double(lived));
		CHECK(lived >= 5.0f);
		CHECK(lived < 5.0f + 1.5f * sim::kTickSeconds);
		CHECK(m->server.gunplay()->gadget_stats().pack_detonations == 0u);
	}

	TEST_CASE("gadgets: a claymore whose owner left before it went off kills with no kill to the player who took his "
			  "slot") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		std::unique_ptr<Match> m = start_match(*level, true, 97u, 0, 1);
		tdm::ScriptedPeer &sapper = m->join("sapper");
		tdm::ScriptedPeer &enemy = m->join("enemy");
		sapper.ready();
		enemy.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t support = uint8_t(host::KitRole::Support);
		const uint8_t team = m->rules().team_of(uint8_t(sapper.slot()));
		const host::LoadoutKit *kit = gl.loadouts.kit(team, support);
		REQUIRE(kit != nullptr);
		const size_t gadget2 = size_t(core::gameplay::WeaponSlot::Gadget2);
		const auto claymore = std::find_if(kit->slots[gadget2].begin(), kit->slots[gadget2].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/claymore/u_claymore"; });
		REQUIRE(claymore != kit->slots[gadget2].end());
		net::LoadoutRequest request;
		request.kit = support;
		request.items[gadget2] = claymore->identifier;
		m->server.on_receive(sapper.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		sapper.spawn_request(support);
		enemy.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(sapper.slot())) && is_alive(m->state(enemy.slot())); }, 600));
		REQUIRE(m->rules().team_of(uint8_t(enemy.slot())) != team);
		std::array<float, 2> face{};
		const std::optional<sim::Vec3> spot = flat_lane(*m, face);
		REQUIRE(spot.has_value());
		const auto at = [&](float p_d) { return add(*spot, p_d * face[0], 0.05f, p_d * face[1]); };
		m->teleport(sapper, at(0.0f));
		m->teleport(enemy, at(-30.0f));
		m->ticks(20);
		bool lay = false, run = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, at(1.0f), sim::Pose::Stand,
					select_and_fire(uint8_t(gadget2), lay && (p_tick % 30u) < 10u));
		};
		m->scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			if (run) {
				c.yaw = sim::quantize_yaw(std::atan2(-face[0], -face[1]));
				c.pitch = 0;
				c.move_forward = 127;
				c.buttons |= sim::buttons::kSprint;
			}
			return c;
		};
		m->ticks(60);
		lay = true;
		REQUIRE(rest_tick(*m, 1, 300).has_value());
		lay = false;
		REQUIRE(m->server.gunplay()->deployables().size() == 1u);
		const sw::Deployable placed = m->server.gunplay()->deployables()[0];
		const float along = (placed.position[0] - (*spot)[0]) * face[0] + (placed.position[2] - (*spot)[2]) * face[1];
		m->teleport(sapper, at(-30.0f));
		m->ticks(sim::seconds_to_ticks(3.5f));
		m->teleport(enemy, at(along + 6.0f));
		m->ticks(20);
		const int slot = sapper.slot();
		run = true;
		REQUIRE(m->until(
				[&] {
					const sim::SoldierState *s = m->state(enemy.slot());
					return s != nullptr &&
							(s->position[0] - (*spot)[0]) * face[0] + (s->position[2] - (*spot)[2]) * face[1] <
							along + 3.5f;
				},
				120));
		m->server.on_disconnect(sapper.peer());
		tdm::ScriptedPeer &heir = m->join("heir");
		heir.ready();
		REQUIRE(heir.slot() == slot);
		REQUIRE(m->until([&] { return m->server.gunplay()->gadget_stats().pack_detonations > 0u; }, 60));
		run = false;
		m->ticks(5);
		CHECK(m->server.gunplay()->deployables().empty());
		CHECK_FALSE(is_alive(m->state(enemy.slot())));
		const net::DeathEvent *death = last_death(enemy, enemy.slot());
		REQUIRE(death != nullptr);
		std::printf("claymore: its owner left, the slot went to a new player, and it killed the enemy: killer %d, "
					"victim %d\n",
				int(death->killer), enemy.slot());
		CHECK(int(death->killer) == enemy.slot());
		CHECK(death->def == claymore->def);
		CHECK(int(m->row(heir.slot())->kills) == 0);
	}
}

namespace {

struct TankStand {
	uint16_t spawner = 0;
	sim::Vec3 stand{};
	sim::Vec3 centre{};
};

sim::Vec3 hull_box_centre(const host::LevelVehicles &p_lv, const net::VehicleWire &p_wire) {
	const host::LevelVehicleKind &k = p_lv.kinds[p_lv.spawners[p_wire.spawner].kind];
	const core::Transform turn = sim::vehicles::hull_transform(p_wire.state.body.rotation, {});
	const sim::Vec3 origin =
			core::vec::sub(p_wire.state.body.com_position, turn.transform_vector(k.model->center_of_mass));
	return sim::vehicles::hull_transform(p_wire.state.body.rotation, origin).transform_point(k.box.centre);
}

std::optional<TankStand> tank_with_stand(
		Match &r_match, VehicleWatch &r_watch, uint32_t p_peer, const std::function<bool(uint8_t)> &p_team) {
	const host::LevelVehicles &lv = *r_match.level->vehicles;
	std::optional<TankStand> out;
	r_match.until(
			[&] {
				r_watch.take(r_match.out, p_peer);
				for (const net::VehicleWire &w : r_watch.latest) {
					const std::string &b = lv.kinds[lv.spawners[w.spawner].kind].blueprint;
					if (out || !p_team(w.team) || (b != "vehicles/t90/t90" && b != "vehicles/m1a2/m1abrams") ||
							!parked_upright(w.state.body.rotation, w.state.body.linear_velocity) ||
							!(r_match.server.vehicle_health(w.spawner).value_or(0.0f) > 0.0f)) {
						continue;
					}
					const std::array<float, 2> f = heading_of(w.state.body.rotation);
					const sim::Vec3 &c = w.state.body.com_position;
					const sim::Vec3 centre = hull_box_centre(lv, w);
					for (float d = 40.0f; !out && d >= 30.0f; d -= 2.0f) {
						const sim::Vec3 at = ground_at(*r_match.world, c[0] - d * f[0], c[2] - d * f[1], c[1] + 5.0f);
						if (std::fabs(at[1] - c[1]) < 3.0f && r_match.motor.fits_standing(at) &&
								clear(*r_match.world, add(at, 0.0f, 1.6f, 0.0f), centre)) {
							out = TankStand{ w.spawner, at, centre };
						}
					}
				}
				return out.has_value();
			},
			sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u);
	return out;
}

std::optional<sim::Vec3> stand_in_view(const Match &p_match, const sim::Vec3 &p_centre, uint16_t p_yaw) {
	for (const float turn : { 0.0f, 0.4f, -0.4f, 0.8f, -0.8f, 1.1f, -1.1f }) {
		const float yaw = sim::yaw_radians(p_yaw) + turn;
		const float fx = std::sin(yaw), fz = std::cos(yaw);
		for (float d = 25.0f; d <= 40.0f; d += 1.0f) {
			sim::StaticHit hit;
			if (!p_match.world->ground_below({ p_centre[0] - d * fx, p_centre[1] + 5.0f, p_centre[2] - d * fz }, 20.0f,
						sim::query_mask::kMovement, &hit)) {
				continue;
			}
			const sim::Vec3 at = add(hit.position, 0.0f, 0.05f, 0.0f);
			if (std::fabs(at[1] - p_centre[1]) < 4.0f && p_match.motor.fits_standing(at) &&
					clear(*p_match.world, add(at, 0.0f, 1.6f, 0.0f), p_centre)) {
				return at;
			}
		}
	}
	return std::nullopt;
}

uint16_t facing(const sim::Vec3 &p_from, const sim::Vec3 &p_to) {
	return sim::quantize_yaw(std::atan2(p_to[0] - p_from[0], p_to[2] - p_from[2]));
}

size_t enemy_script(const Match &p_match, int p_bot) {
	const uint8_t team = p_match.rules().team_of(uint8_t(p_bot));
	for (size_t i = 0; i < p_match.peers.size(); ++i) {
		if (p_match.rules().team_of(uint8_t(p_match.peers[i]->slot())) != team) {
			return i;
		}
	}
	return p_match.peers.size();
}

bool seat_driver(Match &r_match, VehicleWatch &r_watch, uint32_t p_watch_peer, size_t p_script, uint16_t p_spawner) {
	tdm::ScriptedPeer &driver = *r_match.peers[p_script];
	r_match.teleport(driver, beside_hull(r_match, *r_watch.of(p_spawner), 2.2f));
	r_match.ticks(5);
	const sim::Tick use_at = r_match.now() + 3u;
	r_match.scripts[p_script] = [use_at](sim::Tick p_tick, const sim::SoldierState &p_state) {
		sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		c.buttons |= p_tick == use_at ? sim::buttons::kUse : 0u;
		return c;
	};
	return r_match.until(
			[&] {
				r_watch.take(r_match.out, p_watch_peer);
				return r_watch.of(p_spawner)->occupants[0] == uint8_t(driver.slot());
			},
			60);
}

int32_t slot_rounds(const Match &p_match, int p_slot, uint8_t p_weapon_slot) {
	const sw::WeaponSlotState &s = p_match.server.gunplay()->weapons(uint8_t(p_slot)).slots[p_weapon_slot];
	return s.magazine + s.reserve;
}

const std::string &blueprint_of(const Match &p_match, uint16_t p_spawner) {
	const host::LevelVehicles &lv = *p_match.level->vehicles;
	return lv.kinds[lv.spawners[p_spawner].kind].blueprint;
}

}

TEST_SUITE("host-bots-gadgets") {
	TEST_CASE("bot gadgets: MP_007 ConquestSmall0: an engineer bot that sees an MBT with an enemy driver takes its "
			  "kit launcher by WeaponChoice over the install's damage against the hull, fires it and takes the "
			  "tank's health down") {
		tdm::TdmLevel *level = tdm::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(
				*level, true, 113u, 0, 1, gadget_bot_server(*level, uint8_t(host::KitRole::Engineer), stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		const size_t enemy = enemy_script(*m, g.bot);
		REQUIRE(enemy < m->peers.size());
		m->peers[enemy]->spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(m->peers[enemy]->slot())); }, 900));
		const uint8_t launcher_slot = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		const uint16_t launcher = m->server.gunplay()->weapons(uint8_t(g.bot)).slots[launcher_slot].def;
		REQUIRE(launcher < gl.defs.size());
		CHECK((gl.defs[launcher].variant == "weapons/smaw/u_smaw" ||
				gl.defs[launcher].variant == "weapons/rpg7/u_rpg7"));
		VehicleWatch watch;
		const uint32_t watcher = g.mate->peer();
		const std::optional<TankStand> tank =
				tank_with_stand(*m, watch, watcher, [](uint8_t p_team) { return p_team != 0; });
		REQUIRE(tank.has_value());
		REQUIRE(seat_driver(*m, watch, watcher, enemy, tank->spawner));
		const float full = m->server.vehicle_health(tank->spawner).value_or(0.0f);
		REQUIRE(full > 0.0f);
		const uint16_t view = m->state(g.bot)->yaw;
		const std::optional<sim::Vec3> stand = stand_in_view(*m, tank->centre, view);
		REQUIRE(stand.has_value());
		m->place(g.bot, *stand, view);
		const int32_t rounds = slot_rounds(*m, g.bot, launcher_slot);
		const sim::Tick from = m->now();
		const bool damaged =
				m->until([&] { return m->server.vehicle_health(tank->spawner).value_or(full) < full; }, 1800);
		const float after = m->server.vehicle_health(tank->spawner).value_or(-1.0f);
		std::printf("bot gadgets: the engineer bot's %s from %.1f m at the %s of spawner %u with an enemy driver: "
					"%d rockets in %.1f s, health %.0f -> %.0f\n",
				gl.defs[launcher].variant.c_str(), double(horizontal(*stand, tank->centre)),
				blueprint_of(*m, tank->spawner).c_str(), unsigned(tank->spawner),
				int(rounds - slot_rounds(*m, g.bot, launcher_slot)), double(m->now() - from) / double(sim::kSimHz),
				double(full), double(after));
		CHECK(damaged);
		CHECK(after < full);
		CHECK(slot_rounds(*m, g.bot, launcher_slot) < rounds);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("bot gadgets: MP_007 ConquestSmall0: an engineer bot repairs his team's MBT that a human mate drives "
			  "after an enemy's rockets took it under 99 % (BBMedicAssist's engineer path)") {
		tdm::TdmLevel *level = tdm::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(
				*level, true, 117u, 0, 1, gadget_bot_server(*level, uint8_t(host::KitRole::Engineer), stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		const size_t enemy = enemy_script(*m, g.bot);
		REQUIRE(enemy < m->peers.size());
		tdm::ScriptedPeer &gunner = *m->peers[enemy];
		gunner.spawn_request(uint8_t(host::KitRole::Engineer));
		REQUIRE(m->until([&] { return is_alive(m->state(gunner.slot())); }, 900));
		const uint8_t team = m->rules().team_of(uint8_t(g.bot));
		VehicleWatch watch;
		const uint32_t watcher = g.mate->peer();
		const std::optional<TankStand> tank =
				tank_with_stand(*m, watch, watcher, [team](uint8_t p_team) { return p_team == team; });
		REQUIRE(tank.has_value());
		const sim::Vec3 hull = watch.of(tank->spawner)->state.body.com_position;
		std::optional<sim::Vec3> far;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			if (!far || horizontal(sp.position, hull) > horizontal(*far, hull)) {
				far = sp.position;
			}
		}
		REQUIRE(far.has_value());
		REQUIRE(horizontal(*far, hull) > 150.0f);
		m->place(g.bot, add(*far, 0.0f, 0.05f, 0.0f));
		REQUIRE(seat_driver(*m, watch, watcher, g.mate_script, tank->spawner));
		const float full = m->server.vehicle_health(tank->spawner).value_or(0.0f);
		REQUIRE(full > 0.0f);
		const uint8_t launcher_slot = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		m->teleport(gunner, tank->stand);
		bool fire = false;
		m->scripts[enemy] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, tank->centre, sim::Pose::Stand,
					select_and_fire(launcher_slot, fire && (p_tick % 30u) < 10u) | sim::buttons::kZoom);
		};
		m->ticks(90);
		fire = true;
		REQUIRE(m->until([&] { return m->server.vehicle_health(tank->spawner).value_or(full) < 0.99f * full; }, 1200));
		fire = false;
		m->scripts[enemy] = {};
		m->teleport(gunner, add(*far, 0.0f, 0.05f, 0.0f));
		m->ticks(30);
		const float damaged = m->server.vehicle_health(tank->spawner).value_or(-1.0f);
		const uint64_t repairs = m->server.stats().vehicle_repairs;
		const sim::Vec3 near = beside_hull(*m, *watch.of(tank->spawner), 15.0f);
		m->place(g.bot, near, facing(near, tank->centre));
		const sim::Tick from = m->now();
		const bool repaired =
				m->until([&] { return m->server.vehicle_health(tank->spawner).value_or(0.0f) >= 0.99f * full; }, 1800);
		const float after = m->server.vehicle_health(tank->spawner).value_or(-1.0f);
		std::printf("bot gadgets: the engineer bot from 15 m beside his team's %s of spawner %u (a human driver): "
					"health %.0f of %.0f -> %.0f in %.1f s, %llu repair hits\n",
				blueprint_of(*m, tank->spawner).c_str(), unsigned(tank->spawner), double(damaged), double(full),
				double(after), double(m->now() - from) / double(sim::kSimHz),
				(unsigned long long)(m->server.stats().vehicle_repairs - repairs));
		CHECK(damaged < 0.99f * full);
		CHECK(repaired);
		CHECK(after > damaged);
		CHECK(m->server.stats().vehicle_repairs > repairs);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-bots-gadgets") {
	TEST_CASE("bot gadgets: MP_007 ConquestSmall0: a support bot given C4 walks up to an MBT with an enemy driver, "
			  "throws the charge at its foot, backs off 9 m and sets it off with the detonator, taking the tank's "
			  "health down, and sets off his last charge too (BBPFireInfantery's placed-explosive branch)") {
		tdm::TdmLevel *level = tdm::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		auto stats = std::make_shared<host::TdmBotStats>();
		const uint8_t support = uint8_t(host::KitRole::Support);
		std::unique_ptr<Match> m =
				start_match(*level, true, 119u, 0, 1, gadget_bot_server(*level, support, stats, false));
		int bot = -1;
		for (int k = 0; k < 10 && bot < 0; ++k) {
			for (const net::PlayerRow &r : m->rules().players()) {
				bot = r.bot && m->rules().team_of(r.slot) != 0 ? int(r.slot) : bot;
			}
			if (bot < 0) {
				m->tick();
			}
		}
		REQUIRE(bot >= 0);
		const size_t gadget2 = size_t(core::gameplay::WeaponSlot::Gadget2);
		const host::LoadoutKit *kit = gl.loadouts.kit(m->rules().team_of(uint8_t(bot)), support);
		REQUIRE(kit != nullptr);
		const auto c4 = std::find_if(kit->slots[gadget2].begin(), kit->slots[gadget2].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/c4/u_c4"; });
		REQUIRE(c4 != kit->slots[gadget2].end());
		net::LoadoutRequest request;
		request.kit = support;
		request.items[gadget2] = c4->identifier;
		m->server.request_loadout(uint8_t(bot), request);
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		REQUIRE(g.bot == bot);
		const uint32_t life = m->state(bot)->life;
		m->place(bot, add(m->state(bot)->position, 0.0f, 200.0f, 0.0f));
		REQUIRE(m->until([&] { return is_alive(m->state(bot)) && m->state(bot)->life != life; }, 3600));
		REQUIRE(m->server.gunplay()->weapons(uint8_t(bot)).slots[gadget2].def == c4->def);
		const size_t enemy = enemy_script(*m, g.bot);
		REQUIRE(enemy < m->peers.size());
		m->peers[enemy]->spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(m->peers[enemy]->slot())); }, 900));
		VehicleWatch watch;
		const uint32_t watcher = g.mate->peer();
		const std::optional<TankStand> tank =
				tank_with_stand(*m, watch, watcher, [](uint8_t p_team) { return p_team != 0; });
		REQUIRE(tank.has_value());
		REQUIRE(seat_driver(*m, watch, watcher, enemy, tank->spawner));
		const float full = m->server.vehicle_health(tank->spawner).value_or(0.0f);
		REQUIRE(full > 0.0f);
		const uint16_t view = m->state(g.bot)->yaw;
		const std::optional<sim::Vec3> stand = stand_in_view(*m, tank->centre, view);
		REQUIRE(stand.has_value());
		m->place(g.bot, *stand, view);
		const int32_t rounds = slot_rounds(*m, g.bot, uint8_t(gadget2));
		const sim::Tick from = m->now();
		const host::GadgetStats before = m->server.gunplay()->gadget_stats();
		const uint64_t hits = m->server.stats().vehicle_hits;
		float nearest = INFINITY;
		const bool damaged = m->until(
				[&] {
					nearest = std::min(nearest, horizontal(m->state(g.bot)->position, tank->centre));
					return m->server.vehicle_health(tank->spawner).value_or(full) < full;
				},
				3600);
		const float after = m->server.vehicle_health(tank->spawner).value_or(-1.0f);
		const host::GadgetStats &gs = m->server.gunplay()->gadget_stats();
		std::printf("bot gadgets: the support bot's C4 at the %s of spawner %u with an enemy driver, from %.1f m: "
					"came within %.1f m of the hull centre, %d charges used, %llu remote and %llu pack detonations "
					"in %.1f s, health %.0f -> %.0f, the bot %s\n",
				blueprint_of(*m, tank->spawner).c_str(), unsigned(tank->spawner),
				double(horizontal(*stand, tank->centre)), double(nearest),
				int(rounds - slot_rounds(*m, g.bot, uint8_t(gadget2))),
				(unsigned long long)(gs.remote_detonations - before.remote_detonations),
				(unsigned long long)(gs.pack_detonations - before.pack_detonations),
				double(m->now() - from) / double(sim::kSimHz), double(full), double(after),
				is_alive(m->state(g.bot)) ? "alive" : "dead");
		std::printf("bot gadgets: %llu hull hits\n", (unsigned long long)(m->server.stats().vehicle_hits - hits));
		CHECK(damaged);
		CHECK(after < full);
		CHECK(slot_rounds(*m, g.bot, uint8_t(gadget2)) < rounds);
		CHECK(gs.remote_detonations > before.remote_detonations);
		CHECK(is_alive(m->state(g.bot)));
		const auto charges_out = [&] {
			size_t n = 0;
			for (const sw::Deployable &d : m->server.gunplay()->deployables()) {
				n += d.owner == uint8_t(g.bot) && d.def == c4->def && !d.owner_gone ? 1u : 0u;
			}
			return n;
		};
		const auto spent = [&] { return slot_rounds(*m, g.bot, uint8_t(gadget2)) == 0 && charges_out() == 0; };
		const int driver = m->peers[enemy]->slot();
		uint16_t spawner = tank->spawner;
		const auto crewed = [&] {
			watch.take(m->out, watcher);
			const net::VehicleWire *w = watch.of(spawner);
			return w != nullptr && w->occupants[0] == uint8_t(driver) && is_alive(m->state(driver));
		};
		int tanks = 1;
		while (m->until([&] { return spent() || !crewed(); }, 3600) && !spent() && tanks < 3) {
			if (slot_rounds(*m, g.bot, uint8_t(gadget2)) == 0) {
				m->until([&] { return charges_out() == 0; }, 120);
				break;
			}
			REQUIRE(m->until(
					[&] {
						if (!is_alive(m->state(driver)) && m->now() % 30u == 0) {
							m->peers[enemy]->spawn_request(0);
						}
						return is_alive(m->state(driver));
					},
					1800));
			const std::optional<TankStand> next =
					tank_with_stand(*m, watch, watcher, [](uint8_t p_team) { return p_team != 0; });
			REQUIRE(next.has_value());
			REQUIRE(seat_driver(*m, watch, watcher, enemy, next->spawner));
			spawner = next->spawner;
			const uint16_t yaw = m->state(g.bot)->yaw;
			const std::optional<sim::Vec3> again = stand_in_view(*m, next->centre, yaw);
			REQUIRE(again.has_value());
			m->place(g.bot, *again, yaw);
			++tanks;
		}
		std::printf("bot gadgets: the support bot's %d charges at %d tanks: %llu set off with the detonator, %zu left "
					"lying, %d not thrown, %.1f s\n",
				int(rounds), tanks, (unsigned long long)(gs.remote_detonations - before.remote_detonations),
				charges_out(), int(slot_rounds(*m, g.bot, uint8_t(gadget2))),
				double(m->now() - from) / double(sim::kSimHz));
		CHECK(spent());
		CHECK(gs.remote_detonations - before.remote_detonations == uint64_t(rounds));
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-bots-gadgets") {
	TEST_CASE("bot gadgets: MP_007 ConquestSmall0: an assault bot, whose kit scores nothing against an MBT's hull, "
			  "keeps firing at the enemy driver it sees as a soldier") {
		tdm::TdmLevel *level = tdm::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		auto stats = std::make_shared<host::TdmBotStats>();
		std::unique_ptr<Match> m = start_match(
				*level, true, 113u, 0, 1, gadget_bot_server(*level, uint8_t(host::KitRole::Assault), stats, false));
		const GadgetBotMatch g = gadget_bot_match(*m);
		REQUIRE(g.mate != nullptr);
		const size_t enemy = enemy_script(*m, g.bot);
		REQUIRE(enemy < m->peers.size());
		m->peers[enemy]->spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(m->peers[enemy]->slot())); }, 900));
		VehicleWatch watch;
		const uint32_t watcher = g.mate->peer();
		const std::optional<TankStand> tank =
				tank_with_stand(*m, watch, watcher, [](uint8_t p_team) { return p_team != 0; });
		REQUIRE(tank.has_value());
		REQUIRE(seat_driver(*m, watch, watcher, enemy, tank->spawner));
		const uint16_t view = m->state(g.bot)->yaw;
		const std::optional<sim::Vec3> stand = stand_in_view(*m, tank->centre, view);
		REQUIRE(stand.has_value());
		m->place(g.bot, *stand, view);
		const uint64_t bursts = stats->bursts.load();
		m->ticks(600);
		std::printf("bot gadgets: the assault bot %.1f m from the %s of spawner %u with an enemy driver: %llu bursts "
					"in 10 s\n",
				double(horizontal(*stand, tank->centre)), blueprint_of(*m, tank->spawner).c_str(),
				unsigned(tank->spawner), (unsigned long long)(stats->bursts.load() - bursts));
		CHECK(stats->bursts.load() > bursts);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

namespace {

struct ParkedJeep {
	std::unique_ptr<Match> m;
	tdm::ScriptedPeer *driver = nullptr;
	tdm::ScriptedPeer *victim = nullptr;
	VehicleWatch watch;
	uint16_t hull = 0;
	const host::LevelVehicleKind *kind = nullptr;
	net::VehicleWire parked;
	std::array<float, 2> line{};
	uint16_t toward_hull = 0;

	float along(const sim::Vec3 &p_at) const {
		const sim::Vec3 &o = parked.state.body.com_position;
		return (p_at[0] - o[0]) * line[0] + (p_at[2] - o[2]) * line[1];
	}
	sim::Vec3 ahead(float p_metres) const {
		const sim::Vec3 &o = parked.state.body.com_position;
		return add(ground_at(*m->world, o[0] + p_metres * line[0], o[2] + p_metres * line[1], o[1] + 5.0f), 0.0f, 0.05f,
				0.0f);
	}
};

void park_jeep(tdm::TdmLevel &r_level, ParkedJeep &r_out, bool p_tracked = false) {
	const host::LevelVehicles &lv = *r_level.shared->vehicles;
	r_out.m = start_match(r_level, true, 131u, 0, 1);
	Match &m = *r_out.m;
	r_out.driver = &m.join("driver");
	r_out.victim = &m.join("victim");
	r_out.driver->ready();
	r_out.victim->ready();
	REQUIRE(wait_in_round(m));
	const uint8_t team = m.rules().team_of(uint8_t(r_out.driver->slot()));
	REQUIRE(team != m.rules().team_of(uint8_t(r_out.victim->slot())));
	r_out.driver->spawn_request(0);
	r_out.victim->spawn_request(0);
	REQUIRE(m.until(
			[&] { return is_alive(m.state(r_out.driver->slot())) && is_alive(m.state(r_out.victim->slot())); }, 900));
	std::optional<uint16_t> hull;
	float hull_clear = 0.0f;
	REQUIRE(m.until(
			[&] {
				r_out.watch.take(m.out, r_out.driver->peer());
				std::vector<sim::Vec3> others;
				for (const net::VehicleWire &o : r_out.watch.latest) {
					others.push_back(o.state.body.com_position);
				}
				for (const net::VehicleWire &w : r_out.watch.latest) {
					if (w.team != team ||
							host::land_tracked(*lv.kinds[lv.spawners[w.spawner].kind].model) != p_tracked ||
							!parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
						continue;
					}
					const float c =
							clear_run(*m.world, w.state.body.com_position, w.state.body.rotation, others, 40.0f);
					if (c > hull_clear) {
						hull = w.spawner;
						hull_clear = c;
					}
				}
				return hull_clear >= 35.0f;
			},
			sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u));
	r_out.hull = *hull;
	r_out.kind = &lv.kinds[lv.spawners[*hull].kind];
	r_out.parked = *r_out.watch.of(*hull);
	r_out.line = heading_of(r_out.parked.state.body.rotation);
	r_out.toward_hull = sim::quantize_yaw(std::atan2(-r_out.line[0], -r_out.line[1]));
	std::printf("contact: the %s of spawner %u, %.0f m clear\n", r_out.kind->blueprint.c_str(), unsigned(*hull),
			double(hull_clear));
}

void run_over_case(tdm::TdmLevel &r_level, bool p_tracked) {
	ParkedJeep j;
	park_jeep(r_level, j, p_tracked);
	Match &m = *j.m;
	std::printf("contact: kill speed %.1f m/s, normal ignored %d\n", double(j.kind->run_over.kill_speed),
			int(j.kind->run_over.ignore_normal));
	CHECK(j.kind->run_over.kill_speed == doctest::Approx(5.0f));
	CHECK_FALSE(j.kind->run_over.ignore_normal);
	CHECK(host::land_tracked(*j.kind->model) == p_tracked);
	m.teleport(*j.victim, j.ahead(20.0f), j.toward_hull);
	sim::Tick use_at = 0;
	int8_t throttle = 0;
	m.scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
		sim::Command c = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		c.buttons |= p_tick == use_at ? sim::buttons::kUse : 0u;
		c.move_forward = throttle;
		return c;
	};
	const sim::Vec3 door = beside_hull(m, j.parked, 2.0f);
	m.teleport(*j.driver, door);
	REQUIRE(m.until([&] { return horizontal(m.state(j.driver->slot())->position, door) < 0.5f; }, 30));
	use_at = m.now() + 3u;
	REQUIRE(m.until(
			[&] {
				j.watch.take(m.out, j.driver->peer());
				return j.watch.of(j.hull)->occupants[0] == uint8_t(j.driver->slot());
			},
			60));
	REQUIRE(is_alive(m.state(j.victim->slot())));
	throttle = 127;
	float speed_at_death = 0.0f;
	m.until(
			[&] {
				j.watch.take(m.out, j.driver->peer());
				const net::VehicleWire *w = j.watch.of(j.hull);
				if (!is_alive(m.state(j.victim->slot()))) {
					speed_at_death = speed_of(w->state.body.linear_velocity);
					return true;
				}
				return j.along(w->state.body.com_position) > 30.0f;
			},
			600);
	throttle = 0;
	m.ticks(10);
	const net::DeathEvent *death = last_death(*j.driver, j.victim->slot());
	std::printf("contact: the driver ran into the victim 20 m ahead at %.1f m/s, killer %d (driver %d)\n",
			double(speed_at_death), death != nullptr ? int(death->killer) : -1, j.driver->slot());
	CHECK_FALSE(is_alive(m.state(j.victim->slot())));
	REQUIRE(death != nullptr);
	CHECK(int(death->killer) == j.driver->slot());
	CHECK(speed_at_death > j.kind->run_over.kill_speed);
	CHECK(is_alive(m.state(j.driver->slot())));
	CHECK(m.rules().stats().runtime_faults == 0u);
}
}

TEST_SUITE("host-vehicles-contact") {
	TEST_CASE("vehicles: MP_007 ConquestSmall0: a soldier walking into a parked Growler stops at its hull unhurt") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		ParkedJeep j;
		park_jeep(*level, j);
		Match &m = *j.m;
		m.teleport(*j.victim, j.ahead(8.0f), j.toward_hull);
		m.ticks(20);
		m.scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = sim::idle_command(p_tick, j.toward_hull, p_state.pitch, p_state.target_pose);
			c.move_forward = 127;
			return c;
		};
		float nearest = INFINITY;
		for (uint32_t t = 0; t < 240; ++t) {
			m.tick();
			nearest = std::min(nearest, j.along(m.state(j.victim->slot())->position));
		}
		m.scripts[1] = {};
		m.ticks(10);
		const sim::SoldierState *walked = m.state(j.victim->slot());
		std::printf("contact: walking 4 s into the parked hull from 8 m ahead, the victim got %.2f m from its centre "
					"along its line, health %.1f\n",
				double(nearest), double(walked->health));
		CHECK(nearest > 1.5f);
		CHECK(is_alive(walked));
		CHECK(walked->health == doctest::Approx(m.motor.params().max_health));
		CHECK(m.rules().stats().runtime_faults == 0u);
	}

	TEST_CASE("vehicles: MP_007 ConquestSmall0: a Growler driven into an enemy above its "
			  "KillSoldierCollisionSpeedThreshold runs him over, credited to its driver") {
		if (tdm::TdmLevel *level = tdm::load_conquest_level_once()) {
			run_over_case(*level, false);
		}
	}

	TEST_CASE("vehicles: MP_007 ConquestSmall0: every land hull's run-over kills through its material relation "
			  "against the soldier's body above its KillSoldierCollisionSpeedThreshold") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		const sim::weapons::SurfaceMaterialTable &materials = *level->shared->gunplay->materials;
		size_t tracked = 0, wheeled = 0;
		for (const host::LevelVehicleKind &k : lv.kinds) {
			if (k.model == nullptr || !(host::land_tracked(*k.model) || host::land_jeep(*k.model))) {
				continue;
			}
			const sim::weapons::SurfacePair pair = materials.pair_of(k.material);
			const core::gameplay::DamageRelation relation =
					materials.relation(pair.property, materials.zone_property(sw::HitZone::Body));
			const float damage = sim::vehicles::run_over_damage(k.run_over, k.run_over.kill_speed + 0.01f,
					relation.collision_multiplier, relation.collision_threshold);
			std::printf("run-over data: %s: kill speed %.1f m/s, normal ignored %d, hull property %u, relation %d "
						"(multiplier %.3f, threshold %.1f), a kill does %.1f\n",
					k.blueprint.c_str(), double(k.run_over.kill_speed), int(k.run_over.ignore_normal),
					unsigned(pair.property), int(relation.present), double(relation.collision_multiplier),
					double(relation.collision_threshold), double(damage));
			CHECK_MESSAGE(k.material.present, k.blueprint);
			CHECK_MESSAGE(pair.resolved, k.blueprint);
			CHECK_MESSAGE(relation.present, k.blueprint);
			CHECK_MESSAGE(k.run_over.kill_speed > 0.0f, k.blueprint);
			CHECK_MESSAGE(damage >= level->shared->motor.max_health, k.blueprint);
			tracked += host::land_tracked(*k.model) ? 1u : 0u;
			wheeled += host::land_jeep(*k.model) ? 1u : 0u;
		}
		CHECK(tracked >= 2u);
		CHECK(wheeled >= 1u);
	}

	TEST_CASE("vehicles: MP_007 ConquestSmall0: a tank driven into an enemy above its "
			  "KillSoldierCollisionSpeedThreshold runs him over, credited to its driver") {
		if (tdm::TdmLevel *level = tdm::load_conquest_level_once()) {
			run_over_case(*level, true);
		}
	}
}

namespace {

struct GuidedWatch final : host::GunplayObserver {
	uint32_t shot_index = UINT32_MAX;
	uint16_t def = sw::kNoWeapon;
	std::vector<std::pair<uint32_t, sw::Projectile>> flight;
	std::vector<sw::ImpactEvent> impacts;
	std::vector<sw::DetonationEvent> detonations;
	std::vector<sw::DamageEvent> hits;
	std::vector<sw::ShotEvent> shots;

	void weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sw::SoldierWeaponsState &p_after,
			const sw::WeaponTickOutput &p_out) override {
		(void)p_slot, (void)p_command, (void)p_after;
		for (uint8_t i = 0; i < p_out.shot_count; ++i) {
			if (p_out.shots[i].def == def) {
				shots.push_back(p_out.shots[i]);
			}
		}
	}

	void projectiles(
			uint32_t p_tick, const sw::ProjectileStepOutput &p_out, std::span<const sw::Projectile> p_live) override {
		for (const sw::Projectile &p : p_live) {
			if (p.def == def && (shot_index == UINT32_MAX || p.shot_index == shot_index)) {
				shot_index = p.shot_index;
				flight.emplace_back(p_tick, p);
			}
		}
		for (const sw::ImpactEvent &e : p_out.impacts) {
			if (e.def == def) {
				impacts.push_back(e);
			}
		}
		for (const sw::DetonationEvent &d : p_out.detonations) {
			if (d.def == def) {
				detonations.push_back(d);
			}
		}
	}
	void damage(uint32_t p_tick, const sw::DamageEvent &p_event, bool p_applied, float p_health_after) override {
		(void)p_tick, (void)p_health_after;
		if (p_applied && p_event.def == def) {
			hits.push_back(p_event);
		}
	}
};

}

namespace bf3::test::tdm {

std::optional<LockStand> mbt_lock_stand(const host::SharedLevel &p_level, const sim::CollisionWorld &p_world,
		const sim::SoldierMotor &p_motor, std::span<const net::VehicleWire> p_vehicles, uint8_t p_team) {
	const host::LevelVehicles &lv = *p_level.vehicles;
	const auto mbt = [&](const net::VehicleWire &p_w) {
		const std::string &b = lv.kinds[lv.spawners[p_w.spawner].kind].blueprint;
		return b == "vehicles/t90/t90" || b == "vehicles/m1a2/m1abrams";
	};
	const sim::logic::ConquestParams &conquest = *p_level.logic_natives->conquest;
	const auto in_play = [&](const sim::Vec3 &p_at) {
		for (const sim::logic::ConquestAreaData &a : conquest.areas) {
			if ((!a.params.team_specific || a.params.team == p_team) &&
					!sim::logic::polygon_contains(a.polygon, p_at[0], p_at[2])) {
				return false;
			}
		}
		return true;
	};
	for (const net::VehicleWire &w : p_vehicles) {
		if (w.team == p_team || w.team == 0 || !mbt(w) ||
				!parked_upright(w.state.body.rotation, w.state.body.linear_velocity)) {
			continue;
		}
		const host::LevelVehicleKind &k = lv.kinds[lv.spawners[w.spawner].kind];
		const sim::Vec3 &c = w.state.body.com_position;
		const core::Transform turn = sim::vehicles::hull_transform(w.state.body.rotation, {});
		const sim::Vec3 origin = core::vec::sub(c, turn.transform_vector(k.model->center_of_mass));
		const sim::Vec3 centre =
				sim::vehicles::hull_transform(w.state.body.rotation, origin).transform_point(k.box.centre);
		for (int a = 0; a < 16; ++a) {
			const float yaw = float(a) * 3.14159265f / 8.0f;
			for (float d = 60.0f; d <= 240.0f; d += 10.0f) {
				const sim::Vec3 at =
						ground_at(p_world, c[0] + d * std::sin(yaw), c[2] + d * std::cos(yaw), c[1] + 60.0f);
				if (std::fabs(at[1] - c[1]) < 6.0f && in_play(at) && p_motor.fits_standing(at) &&
						clear(p_world, add(at, 0.0f, 1.6f, 0.0f), centre) &&
						clear(p_world, add(centre, 0.0f, 2.0f, 0.0f), add(centre, 0.0f, 45.0f, 0.0f)) &&
						clear(p_world, add(at, 0.0f, 1.6f, 0.0f), add(at, 0.0f, 55.0f, 0.0f))) {
					return LockStand{ w.spawner, at, centre };
				}
			}
		}
	}
	return std::nullopt;
}

}

TEST_SUITE("host-gadgets-guided") {
	TEST_CASE(
			"gadgets: MP_007 ConquestSmall0: an engineer's FGM-148 picked in CUSTOMIZE fires only once locked; zoomed "
			"on a parked enemy MBT its radar controller (zoom level 1, LockTime 2.25 s, SampleRate 0.06 s) locks, "
			"and the missile flies the R2-G4 top attack (static point, target + 35 m, homing inside 15 m) into the "
			"tank from above") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles &lv = *level->shared->vehicles;
		auto watch = std::make_shared<GuidedWatch>();
		std::unique_ptr<Match> m = start_match(
				*level, true, 131u, 0, 1, [&](host::GameServerConfig &r_config) { r_config.gunplay.observer = watch; });
		tdm::ScriptedPeer &gunner = m->join("gunner");
		gunner.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t team = m->rules().team_of(uint8_t(gunner.slot()));
		const uint8_t engineer = uint8_t(host::KitRole::Engineer);
		const uint8_t launcher_slot = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		const host::LoadoutKit *kit = gl.loadouts.kit(team, engineer);
		REQUIRE(kit != nullptr);
		const auto item = std::find_if(kit->slots[launcher_slot].begin(), kit->slots[launcher_slot].end(),
				[&](const host::LoadoutItem &i) { return i.unlock == "weapons/fgm148/u_fgm148"; });
		REQUIRE(item != kit->slots[launcher_slot].end());
		REQUIRE(item->def < gl.defs.size());
		const uint16_t javelin = item->def;
		watch->def = javelin;
		const sw::WeaponSimDef &def = m->server.gunplay()->table().def(javelin, 0);
		REQUIRE(def.locking.present);
		CHECK(def.locking.fire_only_when_locked);
		CHECK(def.locking.homing);
		CHECK(def.locking.primary.lock_type(1) == core::gameplay::LockType::LaserPainted);
		CHECK(def.locking.secondary.lock_type(1) == core::gameplay::LockType::Radar);
		CHECK(def.locking.secondary.lock_type(0) == core::gameplay::LockType::Never);
		CHECK(def.projectile.guidance.use_static);
		CHECK(def.projectile.guidance.use_target);
		net::LoadoutRequest request;
		request.kit = engineer;
		request.items[launcher_slot] = item->identifier;
		m->server.on_receive(gunner.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		gunner.spawn_request(engineer);
		REQUIRE(m->until([&] { return is_alive(m->state(gunner.slot())); }, 900));
		REQUIRE(m->server.gunplay()->weapons(uint8_t(gunner.slot())).slots[launcher_slot].def == javelin);
		VehicleWatch vehicles;
		std::optional<tdm::LockStand> found;
		REQUIRE(m->until(
				[&] {
					vehicles.take(m->out, gunner.peer());
					found = tdm::mbt_lock_stand(*level->shared, *m->world, m->motor, vehicles.latest, team);
					return found.has_value();
				},
				sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u));
		const uint16_t tank = found->spawner;
		const sim::Vec3 stand = found->stand, aim = found->aim;
		const float full = sim::vehicles::full_vehicle_health(lv.kinds[lv.spawners[tank].kind].damage);
		REQUIRE(m->server.vehicle_health(tank) == full);
		const uint32_t tank_id = sim::vehicles::kVehicleTargetIdBase + uint32_t(tank);
		m->teleport(gunner, stand);
		m->ticks(20);
		bool zoom = false, fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, aim, sim::Pose::Stand,
					select_and_fire(launcher_slot, fire) | (zoom ? sim::buttons::kZoom : 0u));
		};
		m->ticks(150);
		const uint64_t shots = m->server.gunplay()->stats().shots;
		fire = true;
		m->ticks(60);
		fire = false;
		CHECK(m->server.gunplay()->stats().shots == shots);
		CHECK_FALSE(m->server.gunplay()->lock_state(uint8_t(gunner.slot())).locked());
		zoom = true;
		const sim::Tick zoomed = m->now();
		REQUIRE(m->until([&] { return m->server.gunplay()->lock_state(uint8_t(gunner.slot())).locked(); }, 600));
		const sim::Tick lock_ticks = m->now() - zoomed;
		const sw::WeaponLockState lock = m->server.gunplay()->lock_state(uint8_t(gunner.slot()));
		std::printf("javelin: zoomed %.1f m from the %s of spawner %u, locked after %u ticks (%.3f s), primary %s, "
					"secondary target 0x%x\n",
				double(horizontal(stand, aim)), lv.kinds[lv.spawners[tank].kind].blueprint.c_str(), unsigned(tank),
				unsigned(lock_ticks), double(lock_ticks) / double(sim::kSimHz),
				lock.primary.locked ? "locked" : "not locked", unsigned(lock.secondary.target));
		CHECK(lock.secondary.locked);
		CHECK_FALSE(lock.primary.locked);
		CHECK(lock.target() == tank_id);
		m->ticks(1);
		REQUIRE(gunner.last_snapshot.has_value());
		REQUIRE(gunner.last_snapshot->own_lock.has_value());
		CHECK(gunner.last_snapshot->own_lock->locked);
		CHECK(gunner.last_snapshot->own_lock->target == tank);
		CHECK(gunner.last_snapshot->own_lock->amount == 255);
		CHECK(double(lock_ticks) / double(sim::kSimHz) >= 2.25);
		CHECK(double(lock_ticks) / double(sim::kSimHz) <= 2.25 + 2.0 * 0.0667 + 1.0 / 60.0);
		const uint64_t launches = m->server.gunplay()->gadget_stats().guided_launches;
		const uint64_t hits = m->server.stats().vehicle_hits;
		fire = true;
		REQUIRE(m->until([&] { return m->server.gunplay()->stats().shots > shots; }, 120));
		fire = false;
		REQUIRE(watch->shots.size() == 1u);
		CHECK(watch->shots.front().lock_target == tank_id);
		CHECK(net::remote_shot_of(watch->shots.front()).lock_target == tank_id);
		CHECK(m->server.gunplay()->gadget_stats().guided_launches == launches + 1u);
		REQUIRE(m->until([&] { return !watch->detonations.empty(); }, 15 * 60));
		m->ticks(2);
		REQUIRE_FALSE(watch->flight.empty());
		float peak = -1e9f;
		std::array<uint32_t, 7> state_ticks{};
		for (const auto &[tick, p] : watch->flight) {
			peak = std::max(peak, float(p.position[1]));
			++state_ticks[size_t(p.guide.state)];
			if (tick % 15u == 0u) {
				std::printf("javelin: tick %u state %u at (%.1f %.1f %.1f) speed %.1f, %.1f m from the centre\n",
						unsigned(tick), unsigned(p.guide.state), p.position[0], p.position[1], p.position[2],
						std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] +
								p.velocity[2] * p.velocity[2]),
						double(core::vec::length(core::vec::sub(
								sim::Vec3{ float(p.position[0]), float(p.position[1]), float(p.position[2]) }, aim))));
			}
		}
		const sw::DetonationEvent &blast = watch->detonations.front();
		std::printf("javelin: peak %.1f m above the hull centre; ticks in states 2/3/4: %u/%u/%u; detonation %.2f m "
					"from the centre, %.2f m above it\n",
				double(peak - aim[1]), unsigned(state_ticks[2]), unsigned(state_ticks[3]), unsigned(state_ticks[4]),
				double(core::vec::length(core::vec::sub(blast.position, aim))), double(blast.position[1] - aim[1]));
		CHECK(state_ticks[size_t(sw::MissileState::StaticPoint)] > 0u);
		CHECK(state_ticks[size_t(sw::MissileState::TargetOffset)] > 0u);
		CHECK(state_ticks[size_t(sw::MissileState::Homing)] > 0u);
		CHECK(peak - aim[1] >= 30.0f);
		CHECK_FALSE(blast.dud);
		REQUIRE_FALSE(watch->impacts.empty());
		CHECK(watch->impacts.back().velocity[1] < 0.0f);
		CHECK(m->server.stats().vehicle_hits > hits);
		const float first = m->server.vehicle_health(tank).value_or(-1.0f);
		REQUIRE(m->until(
				[&] {
					const sw::WeaponSlotState &w =
							m->server.gunplay()->weapons(uint8_t(gunner.slot())).slots[launcher_slot];
					return w.magazine > 0 && w.phase == sw::WeaponPhase::Ready &&
							m->server.gunplay()->lock_state(uint8_t(gunner.slot())).locked();
				},
				600));
		const uint64_t second_shots = m->server.gunplay()->stats().shots;
		watch->detonations.clear();
		watch->shot_index = UINT32_MAX;
		fire = true;
		REQUIRE(m->until([&] { return m->server.gunplay()->stats().shots > second_shots; }, 120));
		fire = false;
		REQUIRE(m->until([&] { return !watch->detonations.empty(); }, 15 * 60));
		m->ticks(2);
		const float second = m->server.vehicle_health(tank).value_or(-1.0f);
		std::printf("javelin: health %.2f -> %.2f after the first missile -> %.2f after the second (%.2f %%)\n",
				double(full), double(first), double(second), double(100.0f * (first - second) / full));
		CHECK(first == doctest::Approx(full));
		CHECK(second < first);
		CHECK(m->server.gunplay()->gadget_stats().guided_launches == launches + 2u);
		REQUIRE(m->until([&] { return m->server.gunplay()->lock_state(uint8_t(gunner.slot())).locked(); }, 600));
		m->server.on_receive(gunner.peer(), net::kChannelReliable, net::encode_suicide_request());
		REQUIRE(m->until([&] { return !is_alive(m->state(gunner.slot())); }, 60));
		m->ticks(2);
		const sw::WeaponLockState dead = m->server.gunplay()->lock_state(uint8_t(gunner.slot()));
		CHECK_FALSE(dead.locked());
		CHECK(dead.primary.target == sw::kNoLockTarget);
		CHECK(dead.secondary.target == sw::kNoLockTarget);
		REQUIRE(gunner.last_snapshot.has_value());
		CHECK_FALSE(gunner.last_snapshot->own_lock.has_value());
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-gadgets-soflam") {
	TEST_CASE("gadgets: MP_007 ConquestSmall0: a recon's SOFLAM placed facing a parked enemy MBT paints it through its "
			  "LockAlways controller, and an engineer's FGM-148 then locks it zoomed through its LockOnLaserPainted "
			  "LockingController (LockTime 1.5 s) before its radar one could (2.25 s); the missile homes on it") {
		tdm::TdmLevel *level = tdm::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles &lv = *level->shared->vehicles;
		auto watch = std::make_shared<GuidedWatch>();
		std::unique_ptr<Match> m = start_match(
				*level, true, 137u, 0, 1, [&](host::GameServerConfig &r_config) { r_config.gunplay.observer = watch; });
		tdm::ScriptedPeer &p = m->join("spotter");
		p.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t slot = uint8_t(p.slot());
		const uint8_t team = m->rules().team_of(slot);
		const uint8_t gadget1 = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		const auto item_of = [&](host::KitRole p_kit, std::string_view p_unlock) -> const host::LoadoutItem * {
			const host::LoadoutKit *kit = gl.loadouts.kit(team, uint8_t(p_kit));
			REQUIRE(kit != nullptr);
			const auto it = std::find_if(kit->slots[gadget1].begin(), kit->slots[gadget1].end(),
					[&](const host::LoadoutItem &i) { return i.unlock == p_unlock; });
			REQUIRE(it != kit->slots[gadget1].end());
			REQUIRE(it->def < gl.defs.size());
			return &*it;
		};
		const host::LoadoutItem *soflam = item_of(host::KitRole::Recon, "weapons/gadgets/soflam/u_soflam");
		const host::LoadoutItem *javelin = item_of(host::KitRole::Engineer, "weapons/fgm148/u_fgm148");
		const sw::GadgetDef *painter = nullptr;
		for (const sw::GadgetDef &g : gl.gadgets) {
			painter = g.def == soflam->def ? &g : painter;
		}
		REQUIRE(painter != nullptr);
		REQUIRE(painter->kind == sw::GadgetKind::LaserDesignator);
		const auto spawn_as = [&](host::KitRole p_kit, const host::LoadoutItem &p_item) {
			net::LoadoutRequest request;
			request.kit = uint8_t(p_kit);
			request.items[gadget1] = p_item.identifier;
			m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_loadout_request(request));
			m->ticks(1);
			REQUIRE(m->until([&] { return m->now() >= m->rules().earliest_respawn(slot); }, 900));
			p.spawn_request(uint8_t(p_kit));
			REQUIRE(m->until([&] { return is_alive(m->state(slot)); }, 600));
			REQUIRE(m->server.gunplay()->weapons(slot).slots[gadget1].def == p_item.def);
		};
		spawn_as(host::KitRole::Recon, *soflam);
		VehicleWatch vehicles;
		std::optional<tdm::LockStand> found;
		REQUIRE(m->until(
				[&] {
					vehicles.take(m->out, p.peer());
					found = tdm::mbt_lock_stand(*level->shared, *m->world, m->motor, vehicles.latest, team);
					return found.has_value();
				},
				sim::seconds_to_ticks(tdm::latest_initial_delay(lv)) + 600u));
		const uint32_t tank_id = sim::vehicles::kVehicleTargetIdBase + uint32_t(found->spawner);
		const sim::Vec3 stand = found->stand, aim = found->aim;
		m->teleport(p, stand);
		m->ticks(20);
		bool zoom = false, fire = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, aim, sim::Pose::Stand,
					select_and_fire(gadget1, fire) | (zoom ? sim::buttons::kZoom : 0u));
		};
		m->ticks(90);
		const uint64_t deploys = m->server.gunplay()->gadget_stats().deploys;
		fire = true;
		REQUIRE(m->until([&] { return m->server.gunplay()->gadget_stats().deploys > deploys; }, 120));
		fire = false;
		const auto placed =
				std::find_if(m->server.gunplay()->deployables().begin(), m->server.gunplay()->deployables().end(),
						[&](const sw::Deployable &d) { return d.def == soflam->def && d.owner == slot; });
		REQUIRE(placed != m->server.gunplay()->deployables().end());
		CHECK(placed->resting);
		const sim::Tick deployed = m->now();
		REQUIRE(m->until([&] { return m->server.gunplay()->gadget_stats().painted_candidates > 0; }, 600));
		const sim::Tick paint_ticks = m->now() - deployed;
		std::printf("soflam: placed %.1f m from the %s of spawner %u, painting after %u ticks (%.3f s)\n",
				double(horizontal(placed->position, aim)), lv.kinds[lv.spawners[found->spawner].kind].blueprint.c_str(),
				unsigned(found->spawner), unsigned(paint_ticks), double(paint_ticks) / double(sim::kSimHz));
		CHECK(paint_ticks >= 152u);
		CHECK(paint_ticks <= 154u);
		m->server.on_receive(p.peer(), net::kChannelReliable, net::encode_suicide_request());
		REQUIRE(m->until([&] { return !is_alive(m->state(slot)); }, 60));
		m->scripts[0] = nullptr;
		spawn_as(host::KitRole::Engineer, *javelin);
		m->teleport(p, stand);
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, aim, sim::Pose::Stand,
					select_and_fire(gadget1, fire) | (zoom ? sim::buttons::kZoom : 0u));
		};
		m->ticks(150);
		REQUIRE(m->server.gunplay()->deployables().size() > 0u);
		CHECK_FALSE(m->server.gunplay()->lock_state(slot).locked());
		zoom = true;
		const sim::Tick zoomed = m->now();
		REQUIRE(m->until([&] { return m->server.gunplay()->lock_state(slot).locked(); }, 600));
		const sim::Tick lock_ticks = m->now() - zoomed;
		const sw::WeaponLockState lock = m->server.gunplay()->lock_state(slot);
		std::printf("soflam: the javelin locked after %u ticks (%.3f s), laser %s, radar %s, %llu painted candidate "
					"ticks\n",
				unsigned(lock_ticks), double(lock_ticks) / double(sim::kSimHz), lock.primary.locked ? "locked" : "not",
				lock.secondary.locked ? "locked" : "not",
				(unsigned long long)m->server.gunplay()->gadget_stats().painted_candidates);
		CHECK(lock.primary.locked);
		CHECK(lock.primary.target == tank_id);
		CHECK_FALSE(lock.secondary.locked);
		CHECK(double(lock_ticks) / double(sim::kSimHz) >= 1.5);
		CHECK(double(lock_ticks) / double(sim::kSimHz) < 2.25);
		const uint64_t shots = m->server.gunplay()->stats().shots;
		const uint64_t launches = m->server.gunplay()->gadget_stats().guided_launches;
		watch->def = javelin->def;
		fire = true;
		REQUIRE(m->until([&] { return m->server.gunplay()->stats().shots > shots; }, 120));
		fire = false;
		REQUIRE(watch->shots.size() == 1u);
		CHECK(watch->shots.front().lock_target == tank_id);
		CHECK(m->server.gunplay()->gadget_stats().guided_launches == launches + 1u);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}

TEST_SUITE("host-gadgets-sweep") {
	TEST_CASE("gadgets: Metro TDM: a recon's T-UGS placed on a clear lane radar-spots (R2-E1 E) the enemy who walks "
			  "past it within 30 m, not while he stands still; the spot names the recon and ends after he stops") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		std::unique_ptr<Match> m = start_match(*level, true, 139u, 0, 1);
		tdm::ScriptedPeer &recon = m->join("recon");
		tdm::ScriptedPeer &enemy = m->join("walker");
		recon.ready();
		enemy.ready();
		REQUIRE(wait_in_round(*m));
		const uint8_t rs = uint8_t(recon.slot()), es = uint8_t(enemy.slot());
		const uint8_t team = m->rules().team_of(rs);
		REQUIRE(m->rules().team_of(es) != team);
		const uint8_t gadget1 = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		const host::LoadoutKit *kit = gl.loadouts.kit(team, uint8_t(host::KitRole::Recon));
		REQUIRE(kit != nullptr);
		const auto tugs = std::find_if(kit->slots[gadget1].begin(), kit->slots[gadget1].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/gadgets/t-ugs/u_ugs"; });
		REQUIRE(tugs != kit->slots[gadget1].end());
		REQUIRE(tugs->def < gl.defs.size());
		net::LoadoutRequest request;
		request.kit = uint8_t(host::KitRole::Recon);
		request.items[gadget1] = tugs->identifier;
		m->server.on_receive(recon.peer(), net::kChannelReliable, net::encode_loadout_request(request));
		m->ticks(1);
		recon.spawn_request(uint8_t(host::KitRole::Recon));
		enemy.spawn_request(0);
		REQUIRE(m->until([&] { return is_alive(m->state(rs)) && is_alive(m->state(es)); }, 900));
		REQUIRE(m->server.gunplay()->weapons(rs).slots[gadget1].def == tugs->def);
		const sim::CollisionWorld &w = *m->world;
		std::optional<std::array<sim::Vec3, 3>> lane;
		for (const sim::SpawnPoint &sp : m->level->spawns) {
			const std::optional<sim::Vec3> centre = floor_at(w, sp.position);
			for (int k = 0; centre && k < 8 && !lane; ++k) {
				const float a = float(k) * 0.785398f;
				const sim::Vec3 dir{ std::sin(a), 0.0f, std::cos(a) };
				const std::optional<sim::Vec3> from = floor_at(w, add(*centre, dir[0] * 8.0f, 0.0f, dir[2] * 8.0f));
				const std::optional<sim::Vec3> to = floor_at(w, add(*centre, -dir[0] * 8.0f, 0.0f, -dir[2] * 8.0f));
				const std::optional<sim::Vec3> back = floor_at(w, add(*centre, dir[0] * 1.5f, 0.0f, dir[2] * 1.5f));
				if (from && to && back && std::abs((*from)[1] - (*centre)[1]) < 0.3f &&
						std::abs((*to)[1] - (*centre)[1]) < 0.3f &&
						clear(w, add(*from, 0, 1.2f, 0), add(*to, 0, 1.2f, 0)) &&
						clear(w, add(*from, 0, 0.3f, 0), add(*to, 0, 0.3f, 0))) {
					lane = std::array<sim::Vec3, 3>{ *centre, *from, *to };
				}
			}
			if (lane) {
				break;
			}
		}
		REQUIRE(lane.has_value());
		const sim::Vec3 centre = (*lane)[0], from = (*lane)[1], to = (*lane)[2];
		const sim::Vec3 dir = core::vec::normalize(core::vec::sub(from, centre));
		m->teleport(recon, add(centre, -dir[0] * 1.5f, 0.05f, -dir[2] * 1.5f));
		m->teleport(enemy, add(from, 0.0f, 0.05f, 0.0f));
		m->ticks(20);
		bool fire = false, walk = false;
		m->scripts[0] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			return aim_command(*m, p_tick, p_state, centre, sim::Pose::Stand, select_and_fire(gadget1, fire));
		};
		m->scripts[1] = [&](sim::Tick p_tick, const sim::SoldierState &p_state) {
			sim::Command c = aim_command(*m, p_tick, p_state, add(to, 0.0f, 1.6f, 0.0f), sim::Pose::Stand, 0u);
			c.move_forward = walk && horizontal(p_state.position, to) > 1.0f ? int8_t(127) : int8_t(0);
			return c;
		};
		m->ticks(90);
		const uint64_t deploys = m->server.gunplay()->gadget_stats().deploys;
		fire = true;
		REQUIRE(m->until([&] { return m->server.gunplay()->gadget_stats().deploys > deploys; }, 120));
		fire = false;
		const auto placed =
				std::find_if(m->server.gunplay()->deployables().begin(), m->server.gunplay()->deployables().end(),
						[&](const sw::Deployable &d) { return d.def == tugs->def && d.owner == rs; });
		REQUIRE(placed != m->server.gunplay()->deployables().end());
		REQUIRE(placed->resting);
		const sim::Vec3 sensor = placed->position;
		const uint64_t sweeps0 = m->server.gunplay()->gadget_stats().radar_sweeps;
		for (int i = 0; i < 150; ++i) {
			m->ticks(1);
			CHECK(m->server.spot_row(es).state == sim::spotting::SpotState::None);
		}
		CHECK(m->server.gunplay()->gadget_stats().radar_sweeps > sweeps0);
		walk = true;
		const sim::Tick started = m->now();
		float fastest = 0.0f;
		REQUIRE(m->until(
				[&] {
					fastest = std::max(fastest, core::vec::length(m->state(es)->velocity));
					return m->server.spot_row(es).state == sim::spotting::SpotState::Radar;
				},
				300));
		const sim::Tick spotted = m->now() - started;
		const float range = core::vec::length(core::vec::sub(m->state(es)->position, sensor));
		std::printf("t-ugs: the walker was radar-spotted %u ticks after he set off, %.1f m from the sensor at up to "
					"%.2f m/s\n",
				unsigned(spotted), double(range), double(fastest));
		CHECK(m->server.spot_row(es).spotter == rs);
		CHECK(range <= 30.0f);
		CHECK(m->server.spot_row(rs).state == sim::spotting::SpotState::None);
		CHECK(m->server.stats().spotting.radar_spots > 0u);
		REQUIRE(m->until([&] { return horizontal(m->state(es)->position, to) <= 1.0f; }, 600));
		walk = false;
		const sim::Tick stopped = m->now();
		REQUIRE(m->until([&] { return m->server.spot_row(es).state == sim::spotting::SpotState::None; }, 300));
		std::printf("t-ugs: the spot ended %u ticks after he stopped\n", unsigned(m->now() - stopped));
		m->ticks(150);
		CHECK(m->server.spot_row(es).state == sim::spotting::SpotState::None);
		CHECK(m->rules().stats().runtime_faults == 0u);
	}
}
