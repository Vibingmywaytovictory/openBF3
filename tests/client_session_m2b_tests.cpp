#include "gunplay_fixture.h"
#include "weapons_support.h"

#include "bf3/host/client_session.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

using namespace bf3;
namespace sw = bf3::sim::weapons;
namespace tw = bf3::test::weapons;
namespace tg = bf3::test::gunplay;

namespace {

constexpr uint8_t kOwnSlot = 2;

size_t duplicates(const std::vector<uint32_t> &p_ids) {
	std::set<uint32_t> seen;
	size_t n = 0;
	for (const uint32_t id : p_ids) {
		n += seen.insert(id).second ? 0u : 1u;
	}
	return n;
}

net::PlayerRow row(uint8_t p_slot, uint8_t p_team, const char *p_name) {
	net::PlayerRow r;
	r.slot = p_slot;
	r.team = p_team;
	r.alive = true;
	net::set_player_name(r, p_name);
	return r;
}

}

TEST_SUITE("host-client-m2b") {
	TEST_CASE("host.client.m2b: OwnShotIds presents every own shot once; lookups follow the newest emission") {
		host::OwnShotIds ids;
		CHECK(ids.lookup(7) == 7);
		std::vector<uint32_t> presented;
		for (uint32_t i = 100; i < 105; ++i) {
			presented.push_back(ids.emit(i));
		}
		CHECK(presented == std::vector<uint32_t>{ 100, 101, 102, 103, 104 });
		CHECK(ids.resets() == 0);

		for (uint32_t i = 101; i < 104; ++i) {
			presented.push_back(ids.emit(i));
		}
		CHECK(ids.resets() == 1);
		CHECK(presented[5] == 105);
		CHECK(presented[7] == 107);
		CHECK(duplicates(presented) == 0);
		CHECK(ids.lookup(101) == 105);
		CHECK(ids.lookup(100) == 100);
		CHECK(ids.lookup(110) == 114);
		CHECK(ids.lookup(50) == 50);

		for (uint32_t k = 0; k < host::OwnShotIds::kSegments + 4; ++k) {
			presented.push_back(ids.emit(200 + k * 3));
			presented.push_back(ids.emit(199 + k * 3));
		}
		CHECK(duplicates(presented) == 0);
		CHECK(ids.segments() == host::OwnShotIds::kSegments);

		host::OwnShotIds many;
		for (uint32_t i = 0; i < 1000; ++i) {
			CHECK(many.emit(i) == i);
		}
		CHECK(many.lookup(3) == 3);
	}

	TEST_CASE("host.client.m2b: present_own_shot_id rewrites the own shots' ids only") {
		host::OwnShotIds ids;
		for (uint32_t i = 10; i < 13; ++i) {
			(void)ids.emit(i);
		}
		(void)ids.emit(10);
		const auto ev = [](host::PresentKind p_kind, uint8_t p_slot, uint32_t p_id) {
			host::PresentEvent e;
			e.kind = p_kind;
			e.slot = p_slot;
			e.id = p_id;
			e.def = 3;
			return e;
		};
		host::PresentEvent shot = ev(host::PresentKind::OwnShot, kOwnSlot, 11);
		CHECK(host::present_own_shot_id(ids, kOwnSlot, shot));
		CHECK(shot.id == 14);
		host::PresentEvent impact = ev(host::PresentKind::Impact, kOwnSlot, 11);
		CHECK(host::present_own_shot_id(ids, kOwnSlot, impact));
		CHECK(impact.id == 14);
		host::PresentEvent remote_impact = ev(host::PresentKind::Impact, kOwnSlot, 11);
		remote_impact.timeline = host::PresentTimeline::Render;
		CHECK_FALSE(host::present_own_shot_id(ids, kOwnSlot, remote_impact));
		host::PresentEvent hit = ev(host::PresentKind::Hit, kOwnSlot, 10);
		hit.flags = host::present_flags::kOwnShooter;
		CHECK(host::present_own_shot_id(ids, kOwnSlot, hit));
		CHECK(hit.id == 13);
		host::PresentEvent hit_on_me = ev(host::PresentKind::Hit, 5, 10);
		hit_on_me.flags = host::present_flags::kOwnVictim;
		CHECK_FALSE(host::present_own_shot_id(ids, kOwnSlot, hit_on_me));
		CHECK(hit_on_me.id == 10);
		host::PresentEvent kill = ev(host::PresentKind::Death, kOwnSlot, 10);
		kill.flags = host::present_flags::kOwnShooter;
		CHECK(host::present_own_shot_id(ids, kOwnSlot, kill));
		CHECK(kill.id == 13);
		host::PresentEvent fall = ev(host::PresentKind::Death, kOwnSlot, 10);
		fall.flags = host::present_flags::kOwnShooter | host::present_flags::kOwnVictim;
		fall.def = sw::kNoWeapon;
		CHECK_FALSE(host::present_own_shot_id(ids, kOwnSlot, fall));
		host::PresentEvent boom = ev(host::PresentKind::Detonation, kOwnSlot, 10);
		CHECK(host::present_own_shot_id(ids, kOwnSlot, boom));
		CHECK(boom.id == 13);
		host::PresentEvent spawn = ev(host::PresentKind::Spawn, kOwnSlot, 10);
		CHECK_FALSE(host::present_own_shot_id(ids, kOwnSlot, spawn));
		host::PresentEvent revive = ev(host::PresentKind::Revive, kOwnSlot, 10);
		CHECK_FALSE(host::present_own_shot_id(ids, kOwnSlot, revive));
		host::PresentEvent remote = ev(host::PresentKind::RemoteShot, 5, 10);
		CHECK_FALSE(host::present_own_shot_id(ids, kOwnSlot, remote));
	}

	TEST_CASE("host.client.m2b: the shot-id reuse on the client path (death, replay, respawn) and its fix") {
		const std::shared_ptr<const host::SharedLevel> level = tg::synthetic_floor();
		REQUIRE(level != nullptr);
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		sim::SoldierMotor motor;
		REQUIRE(motor.init(**world, level->motor).ok());
		const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();
		const uint16_t rifle = tw::id_of(*table, "weapons/m16a4/u_m16a4");
		const uint16_t pistol = tw::id_of(*table, "weapons/m9/u_m9");
		REQUIRE(rifle != sw::kNoWeapon);
		REQUIRE(pistol != sw::kNoWeapon);
		struct Sink final : sim::WeaponEmitSink {
			std::vector<uint32_t> shots;
			void emitted(const sim::Command &, const sw::WeaponTickOutput &p_o, const sim::WeaponPredictionEntry &) override {
				for (size_t i = 0; i < p_o.shot_count && i < p_o.shots.size(); ++i) {
					shots.push_back(p_o.shots[i].shot_index);
				}
			}
		} sink;
		sim::PredictionRing ring(256);
		sim::WeaponLane lane(256, table, level->motor, 0x5EEDull);
		lane.set_sink(&sink);
		ring.attach_weapons(&lane);
		const std::array<uint16_t, 2> loadout{ rifle, pistol };

		const auto spawn = [&](uint32_t p_life, sim::Tick p_tick) {
			const sim::SettleResult s = sim::settle_spawn(motor, **world, level->spawns[0], p_life, p_tick);
			REQUIRE(s.ok);
			sim::WeaponPredictionEntry w;
			if (const sim::WeaponPredictionEntry *prev = lane.newest_at_or_below(p_tick); prev != nullptr) {
				w.state = prev->state;
			}
			w.state.player_slot = kOwnSlot;
			REQUIRE(sw::spawn_soldier_weapons(w.state, *table, loadout, 0, p_tick).ok());
			w.tick = p_tick;
			w.spawn_tick = p_tick;
			lane.set_emitted_through(p_tick);
			lane.put(w);
			ring.put({ sim::hold_command(s.state), s.state, false });
		};

		const auto predict = [&](sim::Tick p_from, sim::Tick p_to) {
			sim::SoldierState state = ring.find(p_from - 1u)->state;
			for (sim::Tick t = p_from; t <= p_to; ++t) {
				sim::Command c;
				c.tick = t;
				c.sequence = t;
				c.yaw = state.yaw;
				c.buttons = sim::buttons::kFire;
				const sim::SoldierState before = state;
				motor.step(state, c);
				const sim::WeaponPredictionEntry *b = lane.find(t - 1u);
				REQUIRE(b != nullptr);
				lane.put(lane.step(*b, c, before, state));
				ring.put({ c, state, true });
			}
		};
		spawn(1, 50);
		predict(51, 140);
		const size_t life1 = sink.shots.size();
		REQUIRE(life1 >= 10);

		const uint32_t server_next = lane.find(100)->state.next_shot_index;
		sim::SoldierState dead = ring.find(100)->state;
		dead.mode = sim::MoveMode::Dead;
		dead.health = 0;
		(void)sim::catch_up(motor, dead, ring, 141);
		CHECK(sink.shots.size() == life1);
		CHECK(lane.find(140)->state.next_shot_index == server_next);
		size_t never_fired = 0;
		for (const uint32_t id : sink.shots) {
			never_fired += id >= server_next ? 1u : 0u;
		}
		REQUIRE(never_fired > 0);

		spawn(2, 200);
		predict(201, 260);
		const std::vector<uint32_t> real = sink.shots;
		REQUIRE(real.size() > life1);
		CHECK(real[life1] == server_next);
		const size_t reused = duplicates(real);
		CHECK(reused > 0);

		host::OwnShotIds ids;
		std::vector<uint32_t> presented;
		for (const uint32_t id : real) {
			presented.push_back(ids.emit(id));
		}
		CHECK(duplicates(presented) == 0);
		CHECK(ids.resets() == 1);
		const uint32_t first_new = presented[life1];
		CHECK(first_new == *std::max_element(presented.begin(), presented.begin() + std::ptrdiff_t(life1)) + 1u);

		CHECK(ids.lookup(real[life1]) == first_new);
		CHECK(ids.lookup(real.back()) == presented.back());
		CHECK(ids.lookup(real[0]) == presented[0]);
		std::printf("host.client.m2b: life 1 emitted %zu shots, %zu after the server's death; life 2 reused %zu ids; presented: 0 "
					"duplicates, 1 segment reset\n",
				life1, never_fired, reused);
	}

	TEST_CASE("host.client.m2b: PlayerList into the client's table: full lists replace, changed rows update, removed slots go") {
		host::PlayerTable table;
		std::array<net::PlayerRow, net::kMaxPlayerRows> rows{};
		CHECK(host::player_rows(table, rows) == 0);
		net::PlayerList full;
		full.full = true;
		full.rows = { row(1, 1, "a"), row(7, 2, "b"), row(63, 1, "c") };
		host::apply_player_list(table, full);
		REQUIRE(host::player_rows(table, rows) == 3);
		CHECK(rows[0].slot == 1);
		CHECK(rows[1].slot == 7);
		CHECK(rows[2].slot == 63);
		CHECK(rows[3].slot == 0);
		CHECK(rows[3].name_bytes == 0);
		net::PlayerList delta;
		delta.rows = { row(7, 2, "b2"), row(9, 1, "d") };
		delta.rows[0].kills = 5;
		delta.removed = { 1 };
		host::apply_player_list(table, delta);
		REQUIRE(host::player_rows(table, rows) == 3);
		CHECK(rows[0].slot == 7);
		CHECK(rows[0].kills == 5);
		CHECK(net::player_name(rows[0]) == "b2");
		CHECK(rows[1].slot == 9);
		CHECK(rows[2].slot == 63);
		net::PlayerList again;
		again.full = true;
		again.rows = { row(4, 1, "e") };
		again.removed = { 4 };
		host::apply_player_list(table, again);
		REQUIRE(host::player_rows(table, rows) == 1);
		CHECK(rows[0].slot == 4);
		net::PlayerList hostile;
		hostile.rows = { row(200, 1, "x") };
		hostile.removed = { 250 };
		host::apply_player_list(table, hostile);
		CHECK(host::player_rows(table, rows) == 1);
	}

	TEST_CASE("host.client.m2b: own_unspawned_at, rules_refusal and the restriction on predicted commands") {
		net::MatchState m;
		CHECK_FALSE(host::own_unspawned_at(true, m, 10, 500));
		m.unspawn_tick = 400;
		CHECK_FALSE(host::own_unspawned_at(false, m, 10, 500));
		CHECK_FALSE(host::own_unspawned_at(true, m, 10, 399));
		CHECK(host::own_unspawned_at(true, m, 10, 400));
		CHECK(host::own_unspawned_at(true, m, 10, 900));
		CHECK_FALSE(host::own_unspawned_at(true, m, 400, 900));
		net::MatchInfo mi;
		CHECK(host::rules_refusal(mi, false, 0) == nullptr);
		mi.rules_flags = net::rules_flags::kTdm | net::rules_flags::kDeployScreen;
		CHECK(host::rules_refusal(mi, false, 0) != nullptr);
		CHECK(host::rules_refusal(mi, true, 77) == nullptr);
		mi.logic_digest = 77;
		CHECK(host::rules_refusal(mi, true, 77) == nullptr);
		CHECK(host::rules_refusal(mi, true, 78) != nullptr);

		const uint32_t metro = net::restrict_bits::kYaw | net::restrict_bits::kPitch | net::restrict_bits::kJump | net::restrict_bits::kProne |
				net::restrict_bits::kZoom | net::restrict_bits::kReload;
		sim::Command c;
		c.tick = 99;
		c.move_forward = 127;
		c.move_right = -5;
		c.buttons = sim::buttons::kFire | sim::buttons::kJump | sim::buttons::kSprint;
		const sim::Command r = net::restrict_command(c, 100, metro);
		CHECK(r.move_forward == 0);
		CHECK(r.move_right == 0);
		CHECK(r.buttons == sim::buttons::kJump);
		c.tick = 100;
		CHECK(net::restrict_command(c, 100, metro).buttons == c.buttons);
	}
}
