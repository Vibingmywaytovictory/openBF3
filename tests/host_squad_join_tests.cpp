#include <doctest/doctest.h>

#include "bf3/host/squad_join.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace bf3;

namespace {

constexpr uint8_t kOwn = 7;
constexpr uint8_t kMate = 3;

net::PlayerRow row(uint8_t p_slot, uint8_t p_team, uint8_t p_squad, bool p_leader, const char *p_name, bool p_alive) {
	net::PlayerRow r;
	r.slot = p_slot;
	r.team = p_team;
	r.squad = p_squad;
	r.leader = p_leader;
	r.alive = p_alive;
	r.name_bytes = uint8_t(std::strlen(p_name));
	std::memcpy(r.name.data(), p_name, r.name_bytes);
	return r;
}

struct Script {
	std::vector<net::SquadRequest> requests;
	std::vector<uint8_t> deploys;
	bool deploy_ok = true;
	host::SquadJoinIo io() {
		host::SquadJoinIo out;
		out.request = [this](const net::SquadRequest &p_q) { requests.push_back(p_q); };
		out.deploy_on = [this](uint8_t p_slot) {
			deploys.push_back(p_slot);
			return deploy_ok;
		};
		return out;
	}
};

host::SquadJoinView view(const std::vector<net::PlayerRow> &p_rows) {
	host::SquadJoinView v;
	v.players = p_rows;
	v.own_slot = kOwn;
	v.can_deploy = true;
	v.mate_position = sim::Vec3{ 10.0f, 0.0f, 10.0f };
	return v;
}

}

TEST_SUITE("host-squad-join-unit") {
	TEST_CASE("the scripted client switches to the mate's team, joins its squad, deploys on it and reports the squads "
			  "it holds at round over") {
		host::SquadJoin join("mate", 120.0);
		Script s;
		std::vector<net::PlayerRow> rows = { row(kMate, 2, 5, true, "mate", true),
			row(kOwn, 1, 1, false, "game", false), row(0, 1, 1, true, "bot", true) };
		CHECK(join.frame(0.0, view(rows), s.io()) == host::SquadJoinPhase::Switching);
		CHECK(join.frame(0.5, view(rows), s.io()) == host::SquadJoinPhase::Switching);
		REQUIRE(s.requests.size() == 1);
		CHECK(s.requests[0].op == net::SquadOp::SwitchTeam);
		CHECK(join.frame(1.0, view(rows), s.io()) == host::SquadJoinPhase::Switching);
		CHECK(s.requests.size() == 2);

		rows[1].team = 2;
		rows[1].squad = 2;
		CHECK(join.frame(1.5, view(rows), s.io()) == host::SquadJoinPhase::Joining);
		CHECK(s.requests.size() == 2);
		CHECK(join.frame(2.0, view(rows), s.io()) == host::SquadJoinPhase::Joining);
		REQUIRE(s.requests.size() == 3);
		CHECK(s.requests[2].op == net::SquadOp::Join);
		CHECK(s.requests[2].squad == 5);

		rows[1].squad = 5;
		rows[0].alive = false;
		CHECK(join.frame(2.1, view(rows), s.io()) == host::SquadJoinPhase::Deploying);
		CHECK(s.deploys.empty());
		rows[0].alive = true;
		CHECK(join.frame(2.2, view(rows), s.io()) == host::SquadJoinPhase::Deploying);
		CHECK(s.deploys == std::vector<uint8_t>{ kMate });

		host::SquadJoinView alive = view(rows);
		alive.own_alive = true;
		alive.own_life = 1;
		alive.own_position = sim::Vec3{ 11.0f, 0.0f, 10.5f };
		CHECK(join.frame(2.3, alive, s.io()) == host::SquadJoinPhase::Playing);
		CHECK(join.spawns() == 1);
		CHECK(join.mate_spawns() == 1);

		host::SquadJoinView over = alive;
		over.round_over = true;
		CHECK(join.frame(80.0, over, s.io()) == host::SquadJoinPhase::Done);
		CHECK(join.squads() == "0:1/1L 3:2/5L 7:2/5");
		CHECK(join.failure().empty());
		const std::string report = join.report();
		CHECK_MESSAGE(report.find("squads 0:1/1L 3:2/5L 7:2/5") != std::string::npos, report);
		CHECK_MESSAGE(report.find("on the mate 1") != std::string::npos, report);
	}

	TEST_CASE("a spawn far from the mate, or one without a deploy on it, does not count; a round over before a spawn "
			  "on the mate fails") {
		host::SquadJoin join("mate", 60.0);
		Script s;
		std::vector<net::PlayerRow> rows = { row(kMate, 2, 5, true, "mate", true),
			row(kOwn, 2, 5, false, "game", true) };
		host::SquadJoinView v = view(rows);
		v.own_alive = true;
		v.own_life = 1;
		v.own_position = sim::Vec3{ 10.0f, 0.0f, 10.0f };
		CHECK(join.frame(0.0, v, s.io()) == host::SquadJoinPhase::Deploying);
		CHECK(join.mate_spawns() == 0);

		v.own_alive = false;
		s.deploy_ok = false;
		CHECK(join.frame(1.0, v, s.io()) == host::SquadJoinPhase::Deploying);
		v.own_alive = true;
		v.own_life = 2;
		CHECK(join.frame(2.0, v, s.io()) == host::SquadJoinPhase::Deploying);
		CHECK(join.mate_spawns() == 0);

		v.own_alive = false;
		s.deploy_ok = true;
		CHECK(join.frame(3.0, v, s.io()) == host::SquadJoinPhase::Deploying);
		v.own_alive = true;
		v.own_life = 3;
		v.own_position = sim::Vec3{ 40.0f, 0.0f, 10.0f };
		CHECK(join.frame(4.0, v, s.io()) == host::SquadJoinPhase::Deploying);
		CHECK(join.spawns() == 3);
		CHECK(join.mate_spawns() == 0);
		CHECK_MESSAGE(join.report().find("on its spawn row 1;") != std::string::npos, join.report());

		v.round_over = true;
		CHECK(join.frame(5.0, v, s.io()) == host::SquadJoinPhase::Failed);
		CHECK(join.failure().find("before a spawn on the mate") != std::string::npos);
	}

	TEST_CASE("no mate in the list times out") {
		host::SquadJoin join("mate", 10.0);
		Script s;
		const std::vector<net::PlayerRow> rows = { row(kOwn, 1, 1, true, "game", false) };
		CHECK(join.frame(0.0, view(rows), s.io()) == host::SquadJoinPhase::Waiting);
		CHECK(join.frame(10.5, view(rows), s.io()) == host::SquadJoinPhase::Failed);
		CHECK(join.failure().find("timed out after 10 s waiting for the mate") != std::string::npos);
		CHECK(s.requests.empty());
	}

	TEST_CASE("the player list text names the humans by the server's rows in slot order, marks the own one and "
			  "counts the bots") {
		std::vector<net::PlayerRow> rows = { row(25, 2, 1, false, "Bob", true), row(3, 1, 2, true, "bot", true),
			row(24, 1, 1, true, "Ann", true), row(4, 2, 1, true, "bot", false) };
		rows[1].bot = true;
		rows[3].bot = true;
		CHECK(host::players_text(rows, 24) == "2 player(s): Ann (you, slot 24), Bob (slot 25); 2 bot(s)");
		CHECK(host::players_text({}, 0) == "0 player(s): none; 0 bot(s)");
	}
}
