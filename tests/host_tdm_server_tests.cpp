#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "tdm_fixture.h"

#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/host/kits.h"
#include "bf3/host/loadout.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/logic/round_natives.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3;
namespace tdm = bf3::test::tdm;
namespace tg = bf3::test::gunplay;
namespace sw = bf3::sim::weapons;

namespace {

constexpr uint64_t kM2aKitHash = 0x31b45dc7cc3f4271ull;
constexpr uint64_t kM2aWeaponHash = 0xb8fd23d8ccfc4c63ull;
constexpr uint64_t kM2aMotorHash = 0x6d75dd5f51d95090ull;

bool is_alive(const sim::SoldierState *p_state) {
	return p_state != nullptr && (p_state->flags & sim::soldier_flags::kAlive) != 0;
}

float distance(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

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

std::unique_ptr<Match> start_match(const tdm::TdmLevel &p_level, bool p_deploy, uint64_t p_seed = 7, int p_kill_limit = 0, int p_min_players = -1) {
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

float pick_drop(const Match &p_match, const sim::Vec3 &p_floor, bool p_lethal) {
	for (float h = 6.0f; h <= 78.0f; h += 1.0f) {
		sim::SoldierState s = p_match.motor.spawn_state(add(p_floor, 0.0f, h, 0.0f), 0, 1, 0, false);
		for (int i = 0; i < 900; ++i) {
			p_match.motor.step(s, sim::idle_command(s.tick + 1u, s.yaw, s.pitch, s.target_pose));
			if ((s.events & sim::soldier_events::kLanded) != 0) {
				break;
			}
		}
		if (p_lethal ? s.health <= 0.0f : (s.damage > 10.0f && s.damage < 60.0f && s.health > 0.0f)) {
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
std::optional<Drop> find_drop(const Match &p_match, bool p_lethal) {
	const sim::CollisionWorld &w = *p_match.world;
	for (const sim::SpawnPoint &sp : p_match.level->spawns) {
		sim::StaticHit hit;
		if (!w.ground_below(add(sp.position, 0.0f, 80.0f, 0.0f), 160.0f, sim::query_mask::kMovement, &hit) || hit.normal[1] < 0.9f) {
			continue;
		}
		const sim::Vec3 floor = hit.position;
		const std::optional<sim::Vec3> beside = floor_at(w, add(floor, 2.0f, 0.0f, 0.0f));
		if (!beside || std::fabs((*beside)[1] - floor[1]) > 0.15f || !p_match.motor.fits_standing(add(floor, 0.0f, 0.05f, 0.0f)) ||
				!p_match.motor.fits_standing(add(*beside, 0.0f, 0.05f, 0.0f))) {
			continue;
		}
		const float h = pick_drop(p_match, floor, p_lethal);
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
			CHECK(m->row(medic.slot())->score == medic_score + 100);
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
			CHECK(r.request_spawn(0, 0, 1, 11) == SpawnRefusal::BadKit);
			CHECK(r.request_spawn(0, 1, 0, 12) == SpawnRefusal::None);
			CHECK(r.request_spawn(0, 1, 0, 12) == SpawnRefusal::RateLimited);
			CHECK(r.request_spawn(0, 1, 0, 13) == SpawnRefusal::Alive);
			CHECK(r.request_spawn(1, 1, 0, 14) == SpawnRefusal::Phase);
			r.on_spawned(0, 20);
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
			CHECK(st.refused[size_t(SpawnRefusal::BadKit)] == 2u);
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
		CHECK(us_assault->category_sid_hashes[0] == core::ui::text_hash("ID_M_SOLDIER_PRIMARY"));
		CHECK(host::loadout_item_category_sid(*us_assault, host::LoadoutKitItem::Gadget1) ==
				core::ui::text_hash("ID_WEAPON_CATEGORYGADGET1"));
		CHECK(host::loadout_item_category_sid(*us_assault, host::LoadoutKitItem::Gadget2) ==
				core::ui::text_hash("ID_M_SOLDIER_GADGET1"));
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

	TEST_CASE("install unchanged") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		tdm::check_install_unchanged(*level);
	}
}
