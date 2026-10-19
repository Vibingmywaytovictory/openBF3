// host.tdm.server (docs/design/m2b.md §11 E3, E4, E5, E7's server half; docs/design/m2b-ru.md §6; owner ru): the TDM
// rules inside the real GameServer on Metro TDM (load_tdm_level_once: one load per process), stepped back to back on
// the test thread with scripted peers (tests/tdm_fixture.h) and scripted server-side soldiers. No sockets, no
// wall-clock wait; every number is a tick count, so the row is parallel-safe.
//   health*   E3: regen after a fall-damage drop (dev Teleport up), a second drop restarts the delay; spawn immunity 120
//             ticks, ended by Fire and by moving (the abort map), the server equal to a local SoldierMotor over the same
//             MotorParams (what the client predicts with)
//   respawn*  E4: a bot's death -> respawn at death + 300 (+ the revive wait near an assault teammate), the rules' kit;
//             a deploy-screen peer is not spawned by Ready (R10); the M2a hashes of a load without the TDM kits
//   revive*   E5: a scripted enemy kills a bot (and a peer), a peer medic's defibrillator brings it back at the corpse with
//             20 hp as a new life; the kill comes off the enemy count, the medic scores 100; a living target and an
//             enemy medic are refused; the revived bot is not respawned at its old tick
//   hostile*  E7 (server half): SpawnRequest refusals by kind (TdmRules directly) and hostile v3 datagrams (counted)
// The wire parts that need nt's v3 codecs (SpawnRequest accepted through the server, MatchState / PlayerList at the
// peer, Spawned.revived, the DeathEvent stamps on the wire) print PENDING (nt) until the codecs return bytes.
// Exits 77 (skipped) without an install.

#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "tdm_fixture.h"

#include "bf3/host/kits.h"
#include "bf3/sim/collision_world.h"
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

// E4: the M2a hashes of Metro TDM without the TDM kits and the immunity abort (printed by the respawn case; see there).
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

void pending_nt(const char *p_what) {
	std::printf("host.tdm.server: PENDING (nt): %s\n", p_what);
	MESSAGE((std::string("PENDING (nt): ") + p_what));
}

// A scripted server-side soldier (AutoTeam gives it a team and the rules a bot kit, [add]).
struct Script final : host::CommandSource {
	std::function<sim::Command(sim::Tick, const sim::SoldierState &)> fn;
	int slot = -1;
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		sim::Command c = fn ? fn(p_tick, p_state) : sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		c.tick = p_tick;
		return c;
	}
};

// One GameServer with TDM rules on the shared level, peers and scripted soldiers, stepped by hand.
struct Match {
	std::shared_ptr<const host::SharedLevel> level;
	host::GameServer server;
	std::vector<host::OutDatagram> out;
	std::vector<std::unique_ptr<tdm::ScriptedPeer>> peers;
	// A peer's command for the tick about to run (null: idle at its own view); sent only while the peer is alive (a
	// client sends no command before its first own state).
	std::vector<std::function<sim::Command(sim::Tick, const sim::SoldierState &)>> scripts;
	std::unique_ptr<sim::CollisionWorld> world; // local queries (rays, ground, the local motor)
	sim::SoldierMotor motor; // over SharedLevel::motor: what a client predicts with
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
	c.preround_override_s = 0.0f; // --preround=0: StartRound as soon as RoundMinPlayerCount (4) players entered
	return c;
}

std::unique_ptr<Match> start_match(const tdm::TdmLevel &p_level, bool p_deploy, uint64_t p_seed = 7) {
	auto m = std::make_unique<Match>();
	m->level = p_level.shared;
	host::GameServerConfig cfg;
	cfg.level = p_level.shared;
	cfg.max_slots = 16;
	cfg.dev_commands = true;
	cfg.seed = p_seed;
	cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
	cfg.gunplay.player_kit = 0; // a peer without KitSelect is assault (the medic)
	cfg.rules = tdm_rules_config(p_deploy);
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

// The floor under p_at (within 3 m below a point 1 m above it), or nullopt.
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

// An open patch: the victim at a spawn point's floor, the shooter 2.5 m along +X, the medics 0.9 m along +Z / -Z, every
// one on flat floor, standing room, the rays between them clear, and open sky above the victim (a drop needs it).
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

// The height of a drop onto p_floor that a local motor says lands with 10 .. 60 damage (non-lethal) or kills
// (p_lethal), or 0.
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

// A drop: the top walkable surface above a spawn point (nothing above it within 80 m), standing room on it and 2 m
// beside it, and the height a local motor says lands non-lethally (or lethally).
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

// A command at p_state aiming its eye at p_target.
sim::Command aim_command(const Match &p_match, sim::Tick p_tick, const sim::SoldierState &p_state, const sim::Vec3 &p_target, sim::Pose p_pose,
		uint32_t p_buttons) {
	float yaw = 0, pitch = 0;
	tg::aim_at(p_match.eye(p_state), p_target, yaw, pitch);
	sim::Command c = sim::idle_command(p_tick, sim::quantize_yaw(yaw), sim::quantize_pitch(pitch), p_pose);
	c.buttons = p_buttons;
	return c;
}

} // namespace

TEST_SUITE("host-tdm-server") {
	// E3: regen and spawn immunity.
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
		CHECK(delay == 600u); // E3: 10 s
		CHECK(per_tick == doctest::Approx(1.0f / 12.0f)); // E3: 5 hp/s
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

		// Immunity, three lives: idle (runs out), Fire at +30, a move at +30. Each life comes from SpawnAt (a spawn with
		// protection, settle included); the local motor steps the server's state of the spawn tick with the same commands.
		const uint32_t immortal = uint32_t(std::lround(soldier.immortal_time_after_spawn * float(sim::kSimHz)));
		CHECK(immortal == 120u); // E3: 2 s
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

		// Regen: a fall-damage drop (dev Teleport up over open floor), then a second one during the regen.
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
		// No regen before t1 + delay, then per_tick per tick.
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
		// A second drop in the regen restarts the delay.
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

	// E4: death and respawn.
	TEST_CASE("respawn: bot respawn ticks and kits, R10, the M2a hashes") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::unique_ptr<Match> m = start_match(*level, false);
		Script *b0 = m->bot(); // team 1
		Script *b1 = m->bot(); // team 2
		Script *b2 = m->bot(); // team 1
		tdm::ScriptedPeer &p = m->join("medic"); // team 2, assault
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
		// The medic far away first (> 20 m: no revive wait), then next to the drop.
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
		CHECK(e1 == d1 + 300u); // PlayerKilledDelay 5 s
		CHECK(r1 == e1);
		CHECK(m->server.gunplay()->kit(uint8_t(b1->slot)) == m->rules().kit_of(uint8_t(b1->slot)));
		// Next to a living assault teammate (the medic, team 2): the bot waits for a revive (revive_wait_s 8 s).
		m->teleport(p, add(patch->beside, 0.0f, 0.05f, 0.0f));
		m->ticks(30);
		const auto [d2, r2] = kill(b1->slot);
		const uint32_t e2 = m->rules().earliest_respawn(uint8_t(b1->slot));
		std::printf("E4 bot slot %d next to the medic (%.1f m): died at %u, earliest_respawn +%u, respawned at +%u\n", b1->slot,
				distance(m->state(p.slot())->position, patch->floor), unsigned(d2), unsigned(e2 - d2), unsigned(r2 - d2));
		CHECK(e2 == d2 + 480u);
		CHECK(r2 == e2);
		// An enemy of the medic dies next to it: no wait.
		const auto [d3, r3] = kill(b0->slot);
		std::printf("E4 bot slot %d (enemy of the medic) next to it: respawned at +%u\n", b0->slot, unsigned(r3 - d3));
		CHECK(r3 == d3 + 300u);
		CHECK(m->server.gunplay()->kit(uint8_t(b0->slot)) == m->rules().kit_of(uint8_t(b0->slot)));
		CHECK(m->rules().stats().runtime_faults == 0u);

		// R10: with the deploy screen a peer's Ready spawns nothing; a SpawnRequest (nt's codec) does.
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
		if (!net::encode_spawn_request(net::SpawnRequest{ 2, 0 }).empty()) {
			q.spawn_request(2);
			d->tick();
			d->tick();
			REQUIRE(!q.spawned.empty());
			CHECK(q.spawned.back().kit == 2);
			CHECK(is_alive(d->state(q.slot())));
		} else {
			pending_nt("a deploy-screen SpawnRequest through the server spawns the peer with its kit within 1 tick");
		}

		// The M2a hashes: a load without the TDM kits (the 3-argument load_gunplay_level) and the motor without the
		// immunity abort give M2a's values; the TDM load's differ only by the defibrillator and the abort.
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
		// The M2a values, recorded from this branch's M2a path (docs/design/m2b-ru.md §7: equal to main's while
		// load_gunplay_level's 3-argument form, kits.cpp's M2a table and the motor are main's).
		CHECK(kit_m2a == kM2aKitHash);
		CHECK(weapon_m2a == kM2aWeaponHash);
		CHECK(motor_m2a == kM2aMotorHash);
	}

	// E5: revive.
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
			// Join order sets AutoTeam's teams: medic 1, shooter 2, victim 1, enemy medic 2.
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
			// Positions: the victim at the patch, the shooter 2.5 m away facing it, both medics beside the victim.
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
			// The medics crouch with the defibrillator out, aimed at the corpse's centre; *_fire says when they fire.
			bool medic_fire = false, enemy_fire = false, shooter_fire = false;
			const auto medic_script = [&](bool *p_fire) {
				return [&m, p_fire, corpse_centre](sim::Tick p_tick, const sim::SoldierState &p_state) {
					const bool press = *p_fire && (p_tick % 60u) < 50u; // held 50 ticks, released 10 (semi or held)
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
			m->ticks(90); // the switch to the gadget slot, the crouch
			CHECK(m->server.gunplay()->weapons(uint8_t(medic.slot())).active == host::kKitGadgetSlot);
			// 1. A living target: the medic's paddles reach the living victim -> refused, nothing happens.
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
			// 2. The kill.
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
			m->place(shooter->slot, m->level->spawns[(patch->spawn + 50u) % m->level->spawns.size()].position); // away
			m->tick();
			const int32_t team2_kill = m->rules().match_state().kills[1];
			const uint32_t respawn_at = m->rules().earliest_respawn(uint8_t(victim));
			const uint32_t revive_until = m->rules().revive_until(uint8_t(victim));
			std::printf("E5 (%s victim): died at %u at (%.2f %.2f %.2f), team 2 kills %d -> %d, respawn +%u, revive until +%u, downed %zu\n",
					victim_peer ? "peer" : "bot", unsigned(died), corpse[0], corpse[1], corpse[2], team2_before, team2_kill, unsigned(respawn_at - died),
					unsigned(revive_until - died), m->rules().downed().size());
			CHECK(team2_kill == team2_before + 1);
			CHECK(m->rules().downed().size() == 1u);
			CHECK(respawn_at == died + (victim_peer ? 300u : 480u)); // a bot next to a living assault teammate waits for the revive
			// 3. The enemy medic's paddles on the corpse: refused.
			const uint64_t refused1 = m->rules().stats().defib_refused;
			enemy_fire = true;
			m->ticks(70);
			enemy_fire = false;
			CHECK(m->rules().stats().defib_refused > refused1);
			CHECK(m->rules().stats().revives == 0u);
			CHECK_FALSE(is_alive(m->state(victim)));
			// 4. The medic revives.
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
			m->ticks(2); // the rows and MatchState are the rules' step's: the revive's score and the kill taken back show next tick
			const host::RulesStats s2 = m->rules().stats();
			std::printf("E5 (%s victim): revived at +%u, life %u -> %u, %.2f hp, %.3f m from the corpse, revives %" PRIu64 ", medic score %d -> %d, team 2 "
						"kills %d\n",
					victim_peer ? "peer" : "bot", unsigned(revived - died), unsigned(life), unsigned(back.life), back.health, distance(back.position, corpse),
					s2.revives, medic_score, m->row(medic.slot()) != nullptr ? m->row(medic.slot())->score : -1, m->rules().match_state().kills[1]);
			CHECK(s2.revives == 1u);
			CHECK(back.life == life + 1u);
			CHECK(back.health == doctest::Approx(revive_health));
			CHECK(back.protect_ticks == 0u); // a revive is not a spawn: no immunity
			CHECK(distance(back.position, corpse) < 0.25f);
			CHECK(revived < respawn_at);
			CHECK(m->rules().match_state().kills[1] == team2_kill - 1); // revive_takes_kill
			REQUIRE(m->row(medic.slot()) != nullptr);
			CHECK(m->row(medic.slot())->score == medic_score + 100);
			CHECK(m->rules().downed().empty());
			if (victim_p != nullptr) {
				REQUIRE(victim_p->spawned.size() == spawned_before + 1u);
				const net::Spawned &sp = victim_p->spawned.back();
				CHECK(sp.state.life == back.life);
				CHECK(sp.spawn_index == 0u);
				CHECK(distance(sp.state.position, corpse) < 0.25f);
				if constexpr (net::kProtocolVersion >= 3) {
					CHECK(sp.revived);
				} else {
					pending_nt("Spawned.revived on the wire (the v2 Spawned codec has no revived bit)");
				}
			}
			// 5. The revived soldier is not respawned at its old respawn tick, and a defib at the living one is refused.
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
		pending_nt("after the man-down window and after the victim deployed (needs a deploy-screen peer: SpawnRequest)");
	}

	// E7 (server half): SpawnRequest refusals by kind, and hostile v3 datagrams at the server.
	TEST_CASE("hostile: SpawnRequest refusals and v3 datagrams") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		// The refusals, on TdmRules directly (128 slots: 64 players + spectators).
		{
			core::Result<std::unique_ptr<host::TdmRules>> created = host::TdmRules::create(*level->shared, nullptr, tdm_rules_config(true), 5, 128);
			REQUIRE_MESSAGE(created.ok(), (created.ok() ? std::string() : created.error().message));
			host::TdmRules &r = **created;
			using host::SpawnRefusal;
			CHECK(r.on_connect(70, false, "spectator") == 0); // slots >= 64 are spectators
			CHECK(r.request_spawn(70, 0, 0, 10) == SpawnRefusal::Spectator);
			const uint8_t team = r.on_connect(0, false, "human");
			CHECK((team == 1 || team == 2));
			CHECK(r.on_connect(1, true, "") != 0);
			CHECK(r.request_spawn(0, host::kKitCount, 0, 10) == SpawnRefusal::BadKit);
			CHECK(r.request_spawn(0, 0, 1, 11) == SpawnRefusal::BadKit); // spawn_choice != 0 (M4's flags)
			CHECK(r.request_spawn(0, 1, 0, 12) == SpawnRefusal::None);
			CHECK(r.request_spawn(0, 1, 0, 12) == SpawnRefusal::RateLimited); // twice in a tick
			CHECK(r.request_spawn(0, 1, 0, 13) == SpawnRefusal::Alive); // already pending
			CHECK(r.request_spawn(1, 1, 0, 14) == SpawnRefusal::Phase); // a bot has no deploy screen
			r.on_spawned(0, 20);
			CHECK(r.kit_of(0) == 1);
			CHECK(r.request_spawn(0, 1, 0, 21) == SpawnRefusal::Alive);
			CHECK_FALSE(r.respawn_tick(0, 100).has_value()); // a deploy-screen human waits for its SpawnRequest
			net::DeathEvent e;
			e.victim = 0;
			e.killer = 0; // a fall
			r.death(e); // at the rules' own tick (no step ran): it asks the policy again
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
		// Hostile datagrams: SpawnRequest garbage (every truncation of a valid one, random bodies), and the server-only v3
		// messages sent by a client. Counted as bad datagrams, the tick runs on, no other soldier changes.
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
		std::vector<uint8_t> valid = net::encode_spawn_request(net::SpawnRequest{ 1, 0 });
		const bool codec = !valid.empty();
		if (!codec) {
			valid = { uint8_t(net::MessageType::SpawnRequest), 1, 0 };
		}
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
		CHECK(bad >= sent - (codec ? 60u : 0u)); // every one but (with nt's codec) a random body that happens to decode
		CHECK(m->rules().stats().runtime_faults == 0u);
		CHECK(m->server.stats().step_precondition_failures == 0u);
		if (!codec) {
			pending_nt("hostile SpawnRequests that decode (the refusals through the server's [recv]); a v2 client refused with Kick");
		}
	}

	TEST_CASE("install unchanged") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		tdm::check_install_unchanged(*level);
	}
}
