#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "net_procs_support.h"
#include "test_support.h"
#include "win32_text.h"

#include "bf3/present/effect_seed.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bf3::core::detail::path_utf8;
using bf3::test::procs::ChildEnd;
using bf3::test::procs::ChildSpec;
using bf3::test::procs::client_args;
using bf3::test::procs::contains;
using bf3::test::procs::driver_config_from_env;
using bf3::test::procs::godot_client;
using bf3::test::procs::kLevel;
using bf3::test::procs::kMode;
using bf3::test::procs::line_starts;
using bf3::test::procs::need;
using bf3::test::procs::number_after;
using bf3::test::procs::ProcDriver;
using bf3::test::procs::require_clean_exit;
using bf3::test::procs::Server;
using bf3::test::procs::start_server;
using bf3::test::procs::start_tool;
using bf3::test::procs::summary_number;

struct ClientReport {
	uint32_t slot = 0;
	uint64_t match_seed = 0;
	uint32_t lives = 0;
	uint64_t snapshots = 0;
	uint64_t commands = 0;
	float max_walk_m = 0.0f;
	bool connected = false;
	bool complete = false;
	std::map<uint32_t, uint64_t> seen;
	std::map<std::pair<uint32_t, uint32_t>, uint64_t> remote;
	std::multiset<std::tuple<uint32_t, uint64_t>> own;
};

ClientReport read_report(const std::filesystem::path &p_path) {
	ClientReport r;
	std::ifstream in(p_path, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream s(line);
		std::string key;
		s >> key;
		if (key == "slot") {
			s >> r.slot;
		} else if (key == "match_seed") {
			s >> r.match_seed;
		} else if (key == "lives") {
			s >> r.lives;
		} else if (key == "snapshots") {
			s >> r.snapshots;
		} else if (key == "commands") {
			s >> r.commands;
		} else if (key == "max_walk_m") {
			s >> r.max_walk_m;
		} else if (key == "connected") {
			int c = 0;
			s >> c;
			r.connected = c == 1;
		} else if (key == "seen") {
			uint32_t slot = 0;
			uint64_t frames = 0;
			s >> slot >> frames;
			r.seen[slot] = frames;
		} else if (key == "shot") {
			std::string kind;
			uint32_t slot = 0, id = 0;
			uint64_t seed = 0;
			s >> kind >> slot >> id >> seed;
			if (kind == "remote") {
				r.remote[{ slot, id }] = seed;
			} else {
				r.own.insert({ slot, seed });
			}
		} else if (key == "end") {
			r.complete = true;
		}
	}
	return r;
}


}

TEST_SUITE("net-procs-connect") {
	TEST_CASE("a bf3dump client and a loadbot process join the dedicated server, play and exit 0") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server = start_server(driver, 2, 40);
		if (!server) {
			return;
		}
		const std::optional<size_t> started = start_tool(driver, "client",
				client_args(driver, "client", server->port, { "--script=walk", "--seconds=20", "--name=walker" }));
		if (!started) {
			return;
		}
		const size_t client = *started;
		std::string spawned;
		if (!need(line_starts(driver, client, "client: spawned", 180s, &spawned), driver.output(client))) {
			return;
		}
		const std::optional<size_t> fleet_started = start_tool(driver, "loadbots",
				{ "loadbots", "127.0.0.1:" + std::to_string(server->port), std::string(kLevel), std::string(kMode),
						"--count=4", "--seconds=8" });
		if (!fleet_started) {
			return;
		}
		const size_t bots = *fleet_started;
		CHECK_MESSAGE(driver.wait_line(
							  server->id,
							  [](std::string_view l) {
								  return l.rfind("[dedicated] t=", 0) == 0 && number_after(l, " clients ") == 5.0;
							  },
							  30s),
				driver.output(server->id));

		std::string fleet;
		CHECK(line_starts(driver, bots, "loadbots: summary", 60s, &fleet));
		const ChildEnd bots_end = driver.finish(bots, 30s);
		std::string summary;
		CHECK(line_starts(driver, client, "client: summary", 60s, &summary));
		const ChildEnd client_end = driver.finish(client, 30s);
		std::string server_summary;
		CHECK(driver.wait_line(
				server->id, [](std::string_view l) { return contains(l, "[dedicated] summary: "); }, 120s,
				&server_summary));
		const ChildEnd server_end = driver.finish(server->id, 60s);

		require_clean_exit(driver, bots, bots_end, "loadbots");
		require_clean_exit(driver, client, client_end, "client");
		require_clean_exit(driver, server->id, server_end, "server");

		CHECK(contains(fleet, "4 bots, matched 4, spawned 4,"));
		CHECK(contains(fleet, "decode errors 0, disconnected 0, refused 0"));
		CHECK(summary_number(fleet, "snapshots ") > 4 * 60);
		CHECK(contains(summary, "connected to the end"));
		CHECK(contains(summary, "install unchanged"));
		CHECK(contains(server_summary, "install unchanged"));
		CHECK(contains(server_summary, "dev commands off"));
		const ClientReport r = read_report(driver.roots("client").dir / L"stats.txt");
		CHECK(r.complete);
		CHECK(r.connected);
		CHECK(r.lives >= 1);
		CHECK(r.snapshots > 300);
		CHECK(r.commands > 300);
		CHECK(r.max_walk_m > 2.0f);
		MESSAGE(spawned << "\n"
						<< summary << "\n"
						<< fleet << "\n"
						<< server_summary << "\nserver cpu " << server_end.cpu_seconds << " s, client cpu "
						<< client_end.cpu_seconds << " s, loadbots cpu " << bots_end.cpu_seconds << " s");
	}
}

TEST_SUITE("net-procs-two-clients") {
	TEST_CASE("two client processes see each other's soldier and the same effect seed for every remote shot") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server = start_server(driver, 2, 55);
		if (!server) {
			return;
		}
		const std::optional<size_t> alpha_started = start_tool(driver, "alpha",
				client_args(driver, "alpha", server->port, { "--script=walkfire", "--seconds=36", "--name=alpha" }));
		if (!alpha_started ||
				!need(line_starts(driver, *alpha_started, "client: spawned", 180s), driver.output(*alpha_started))) {
			return;
		}
		const size_t alpha = *alpha_started;
		const std::optional<size_t> bravo_started = start_tool(driver, "bravo",
				client_args(driver, "bravo", server->port,
						{ "--script=walkfire", "--seconds=20", "--name=bravo", "--seed=2" }));
		if (!bravo_started ||
				!need(line_starts(driver, *bravo_started, "client: spawned", 180s), driver.output(*bravo_started))) {
			return;
		}
		const size_t bravo = *bravo_started;
		std::string bravo_summary, alpha_summary;
		CHECK(line_starts(driver, bravo, "client: summary", 90s, &bravo_summary));
		const ChildEnd bravo_end = driver.finish(bravo, 30s);
		CHECK(line_starts(driver, alpha, "client: summary", 90s, &alpha_summary));
		const ChildEnd alpha_end = driver.finish(alpha, 30s);
		CHECK(driver.wait_line(
				server->id, [](std::string_view l) { return contains(l, "[dedicated] summary: "); }, 120s));
		const ChildEnd server_end = driver.finish(server->id, 60s);
		require_clean_exit(driver, bravo, bravo_end, "bravo");
		require_clean_exit(driver, alpha, alpha_end, "alpha");
		require_clean_exit(driver, server->id, server_end, "server");

		const ClientReport a = read_report(driver.roots("alpha").dir / L"stats.txt");
		const ClientReport b = read_report(driver.roots("bravo").dir / L"stats.txt");
		if (!need(a.complete && b.complete, "a client wrote no complete stats file")) {
			return;
		}
		CHECK(a.slot != b.slot);
		CHECK(a.seen.count(b.slot) == 1);
		CHECK(b.seen.count(a.slot) == 1);
		CHECK(a.match_seed != 0);
		CHECK(a.match_seed == b.match_seed);

		size_t hashed = 0;
		for (const ClientReport *r : { &a, &b }) {
			for (const auto &[key, seed] : r->remote) {
				hashed += seed == bf3::present::effect_seed(r->match_seed, uint8_t(key.first), key.second) ? 1u : 0u;
			}
		}
		CHECK(hashed == a.remote.size() + b.remote.size());

		size_t bot_shots = 0, bot_matched = 0, bot_same = 0;
		size_t from_alpha = 0, from_alpha_owned = 0, from_bravo = 0, from_bravo_owned = 0;
		for (const auto &[key, seed] : b.remote) {
			if (key.first == a.slot) {
				++from_alpha;
				from_alpha_owned += a.own.count({ key.first, seed }) > 0 ? 1u : 0u;
				continue;
			}
			++bot_shots;
			const auto it = a.remote.find(key);
			if (it != a.remote.end()) {
				++bot_matched;
				bot_same += it->second == seed ? 1u : 0u;
			}
		}
		for (const auto &[key, seed] : a.remote) {
			if (key.first == b.slot) {
				++from_bravo;
				from_bravo_owned += b.own.count({ key.first, seed }) > 0 ? 1u : 0u;
			}
		}
		CHECK(b.remote.size() > 20);
		CHECK(bot_matched == bot_shots);
		CHECK(bot_same == bot_matched);
		CHECK(from_alpha > 0);
		CHECK(from_alpha_owned == from_alpha);
		CHECK(from_bravo > 0);
		CHECK(from_bravo_owned == from_bravo);
		MESSAGE("alpha slot " << a.slot << ", bravo slot " << b.slot << "; bravo saw " << bot_shots << " bot shots ("
							  << bot_matched << " also at alpha, " << bot_same << " with the same seed), " << from_alpha
							  << " of alpha's (" << from_alpha_owned << " with alpha's own seed); alpha saw "
							  << from_bravo << " of bravo's (" << from_bravo_owned << " with bravo's own seed)\n"
							  << alpha_summary << "\n"
							  << bravo_summary);
	}
}

namespace {

double level_change_load_s(std::string_view p_line) {
	const size_t at = p_line.find("loaded in ");
	if (at == std::string_view::npos) {
		return -1.0;
	}
	const std::string tail(p_line.substr(at + 10));
	return std::strtod(tail.c_str(), nullptr);
}

}

namespace {

constexpr double kReturnMargin = 1.25;

std::vector<std::string> lines_with(const std::string &p_text, std::string_view p_needle) {
	std::vector<std::string> out;
	std::istringstream lines(p_text);
	for (std::string line; std::getline(lines, line);) {
		if (contains(line, p_needle)) {
			out.push_back(line);
		}
	}
	return out;
}

}

TEST_SUITE("net-procs-godot-connect") {
	TEST_CASE("a Godot --connect client with no --level or --mode loads the level and mode the server names, joins "
			  "under its --name and prints the server's players and bots, not the listen game's defaults") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server =
				start_server(driver, 0, 900, { "--level=MP_011", "--mode=TeamDeathMatch0" });
		if (!server) {
			return;
		}
		bf3::core::Result<size_t> client =
				driver.start(godot_client(driver, server->port, { "--quit-after=1", "--name=Ann" }));
		REQUIRE_MESSAGE(client.ok(), (client.ok() ? std::string() : client.error().message));
		std::string line;
		CHECK_MESSAGE(
				driver.wait_line(
						*client,
						[](std::string_view l) { return contains(l, "[level] done: ") || contains(l, " ended: "); },
						600s, &line),
				driver.output(*client));
		CHECK(contains(line, "[level] done: "));
		const ChildEnd client_end = driver.finish(*client, 60s);
		const std::string out = driver.output(*client);
		CHECK_MESSAGE(client_end.code == 0, out);
		CHECK(client_end.stragglers == 0);
		CHECK(contains(out, "[level] MP_011 TeamDeathMatch0"));
		CHECK(contains(out, "[level] install unchanged: yes"));
		CHECK(contains(out, "[tdm] 1 player(s): Ann (you, slot "));
		CHECK(contains(out, "); 0 bot(s)"));
		CHECK(contains(out, "TDM rules, bots, limits and tuning from the server"));
		CHECK(contains(out, "respawn and bots from the server"));
		CHECK_FALSE(contains(out, " TDM bots, "));
		CHECK_FALSE(contains(out, ", bots 31, "));
		driver.finish(server->id, 1s);
		MESSAGE(line);
	}
}

TEST_SUITE("net-procs-follow") {
	TEST_CASE("a Godot --connect client follows a dedicated server through MP_Subway, MP_011, MP_003 and back to "
			  "MP_Subway: no kick, one connection, and private bytes and objects on the return within the margin of "
			  "the first "
			  "visit") {
		ProcDriver driver(driver_config_from_env());
		const std::filesystem::path maplist = driver.roots("server").dir / L"maplist.cfg";
		std::error_code ec;
		std::filesystem::create_directories(maplist.parent_path(), ec);
		{
			std::ofstream out(maplist, std::ios::binary);
			out << "MP_Subway TeamDeathMatch0 1\nMP_011 TeamDeathMatch0 1\nMP_003 TeamDeathMatch0 1\n";
		}
		const std::optional<Server> server = start_server(driver, 8, 1700,
				{ "--maplist=" + path_utf8(maplist), "--kill-limit=5", "--intermission-s=5", "--stop-after-levels=4" });
		if (!server) {
			return;
		}
		bf3::core::Result<size_t> client =
				driver.start(godot_client(driver, server->port, { "--match-levels=4", "--quit-after=2" }));
		REQUIRE_MESSAGE(client.ok(), (client.ok() ? std::string() : client.error().message));
		std::string line;
		CHECK_MESSAGE(
				driver.wait_line(
						*client,
						[](std::string_view l) { return contains(l, "[match] level 4 ") || contains(l, " ended: "); },
						1500s, &line),
				driver.output(*client));
		CHECK(contains(line, "[match] level 4 MP_Subway TeamDeathMatch0 done: "));
		const ChildEnd client_end = driver.finish(*client, 60s);
		const std::string out = driver.output(*client);
		CHECK_MESSAGE(client_end.code == 0, out);
		CHECK(client_end.stragglers == 0);
		CHECK_FALSE(contains(out, " ended: "));
		CHECK(contains(out, "[match] level change 1: MP_011 TeamDeathMatch0 round 1"));
		CHECK(contains(out, "[match] level change 2: MP_003 TeamDeathMatch0 round 2"));
		CHECK(contains(out, "[match] level change 3: MP_Subway TeamDeathMatch0 round 3"));
		CHECK(contains(out, "[level] install unchanged: yes"));
		const std::vector<std::string> done = lines_with(out, "[level] done: ");
		CHECK(done.size() == 4u);
		const std::vector<std::string> levels = lines_with(out, " done: private ");
		REQUIRE(levels.size() == 4u);
		const uint64_t first_private = summary_number(levels[0], "done: private ");
		const uint64_t last_private = summary_number(levels[3], "done: private ");
		CHECK(first_private != UINT64_MAX);
		CHECK(double(last_private) <= double(first_private) * kReturnMargin);
		const std::string server_out = driver.output(server->id);
		CHECK_FALSE(contains(server_out, "LoadTimeout=1"));
		CHECK_FALSE(contains(server_out, "LoadStalled=1"));
		CHECK(contains(server_out, "level change 3: MP_Subway TeamDeathMatch0 round 3,"));
		CHECK(contains(server_out, "carried 1 clients"));
		for (const std::string &level_line : levels) {
			CHECK(summary_number(level_line, "orphan nodes ") == 0u);
		}
		const uint64_t first_objects = summary_number(levels[0], "objects ");
		CHECK(first_objects != UINT64_MAX);
		CHECK(summary_number(levels[3], "objects ") <= first_objects);
		driver.finish(server->id, 1s);
		for (const std::string &l : levels) {
			MESSAGE(l);
		}
	}
}

TEST_SUITE("net-procs-listen-maplist") {
	TEST_CASE("a Godot game given --maplist runs its own rotating server and follows it from MP_Subway to MP_011") {
		ProcDriver driver(driver_config_from_env());
		const std::filesystem::path maplist = driver.roots("game").dir / L"maplist.cfg";
		std::error_code ec;
		std::filesystem::create_directories(maplist.parent_path(), ec);
		{
			std::ofstream out(maplist, std::ios::binary);
			out << "MP_Subway TeamDeathMatch0 1\nMP_011 TeamDeathMatch0 1\n";
		}
		const ChildSpec spec = driver.godot_child("game", { "--headless", "res://scenes/metro_tdm.tscn" },
				{ "--maplist=" + path_utf8(maplist), "--bots=8", "--kill-limit=5", "--preround=0", "--intermission-s=5",
						"--match-levels=2", "--quit-after=2" });
		if (const std::string why = driver.skip_reason(spec); !why.empty()) {
			bf3::test::skip_install_case(why.c_str());
			return;
		}
		bf3::core::Result<size_t> game = driver.start(spec);
		REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
		std::string line;
		CHECK_MESSAGE(
				driver.wait_line(
						*game,
						[](std::string_view l) { return contains(l, "[match] level 2 ") || contains(l, " ended: "); },
						1200s, &line),
				driver.output(*game));
		CHECK(contains(line, "[match] level 2 MP_011 TeamDeathMatch0 done: "));
		const ChildEnd end = driver.finish(*game, 60s);
		const std::string out = driver.output(*game);
		CHECK_MESSAGE(end.code == 0, out);
		CHECK(end.stragglers == 0);
		CHECK(contains(out, "[match] listen server rotates MP_Subway TeamDeathMatch0 x1, MP_011 TeamDeathMatch0 x1"));
		CHECK(contains(out, "[match] level change 1: MP_011 TeamDeathMatch0 round 1, load deadline 65535 s"));
		CHECK(contains(out, "[level] install unchanged: yes"));
		MESSAGE(line);
	}
}

TEST_SUITE("net-procs-kick") {
	TEST_CASE("a Godot --connect client kicked at its Hello reports the reason with no level loaded and exits 1") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server = start_server(driver, 0, 300);
		if (!server) {
			return;
		}
		bf3::core::Result<size_t> client =
				driver.start(godot_client(driver, server->port, { "--game-build=1", "--quit-after=3" }));
		REQUIRE_MESSAGE(client.ok(), (client.ok() ? std::string() : client.error().message));
		std::string line;
		CHECK_MESSAGE(driver.wait_line(
							  *client, [](std::string_view l) { return contains(l, " ended: kicked: "); }, 120s, &line),
				driver.output(*client));
		const ChildEnd client_end = driver.finish(*client, 60s);
		CHECK(client_end.code == 1);
		CHECK(client_end.stragglers == 0);
		const std::string out = driver.output(*client);
		CHECK_FALSE(contains(out, "[level] done: "));
		CHECK_FALSE(contains(out, "[level] MP_"));
		CHECK_MESSAGE(driver.wait_line(
							  server->id, [](std::string_view l) { return contains(l, " Build=1 "); }, 30s),
				driver.output(server->id));
		driver.finish(server->id, 1s);
		MESSAGE(line);
	}
}

TEST_SUITE("net-procs-rotation") {
	TEST_CASE("a dedicated server walks MP_Subway, MP_011, MP_003 and back to MP_Subway with two loadbots processes: "
			  "the one that stalls on level 2 is kicked LoadStalled, the other follows every change on its slot, and "
			  "the warm return to MP_Subway loads in 2 s or less") {
		ProcDriver driver(driver_config_from_env());
		const std::filesystem::path maplist = driver.roots("server").dir / L"maplist.cfg";
		std::error_code ec;
		std::filesystem::create_directories(maplist.parent_path(), ec);
		{
			std::ofstream out(maplist, std::ios::binary);
			out << "MP_Subway TeamDeathMatch0 1\nMP_011 TeamDeathMatch0 1\nMP_003 TeamDeathMatch0 1\n";
		}
		const std::optional<Server> server = start_server(driver, 8, 1700,
				{ "--maplist=" + path_utf8(maplist), "--kill-limit=5", "--intermission-s=5", "--load-stall-s=5",
						"--stop-after-levels=4" });
		if (!server) {
			return;
		}
		const std::string at = "127.0.0.1:" + std::to_string(server->port);
		const std::optional<size_t> steady_started = start_tool(driver, "steady",
				{ "loadbots", at, std::string(kLevel), std::string(kMode), "--count=1", "--levels=4", "--name=steady",
						"--seconds=1650" });
		const std::optional<size_t> staller_started = start_tool(driver, "staller",
				{ "loadbots", at, std::string(kLevel), std::string(kMode), "--count=1", "--stall-level=2",
						"--name=staller", "--seconds=1650" });
		if (!steady_started || !staller_started) {
			return;
		}
		const size_t steady = *steady_started;
		const size_t staller = *staller_started;
		std::string staller_summary;
		CHECK_MESSAGE(
				line_starts(driver, staller, "loadbots: summary", 1200s, &staller_summary), driver.output(staller));
		const ChildEnd staller_end = driver.finish(staller, 30s);
		std::string steady_summary;
		CHECK_MESSAGE(line_starts(driver, steady, "loadbots: summary", 1500s, &steady_summary), driver.output(steady));
		const ChildEnd steady_end = driver.finish(steady, 30s);
		std::string server_summary;
		CHECK(driver.wait_line(
				server->id, [](std::string_view l) { return contains(l, "[dedicated] summary: "); }, 300s,
				&server_summary));
		const ChildEnd server_end = driver.finish(server->id, 60s);
		require_clean_exit(driver, staller, staller_end, "staller");
		require_clean_exit(driver, steady, steady_end, "steady");
		require_clean_exit(driver, server->id, server_end, "server");

		const std::string staller_out = driver.output(staller);
		const std::string steady_out = driver.output(steady);
		const std::string server_out = driver.output(server->id);
		CHECK(contains(staller_out, "levels 2, level changes 1, slot changes 0, round 1, stalled, kicked LoadStalled"));
		CHECK(contains(staller_summary, "stalled 1, kicked 1"));
		CHECK(contains(steady_out, "levels 4, level changes 3, slot changes 0, round 3"));
		CHECK(contains(steady_summary, "decode errors 0, disconnected 0, refused 0"));
		CHECK(contains(steady_summary, "level changes 3, stalled 0, kicked 0"));
		std::vector<std::string> changes;
		std::istringstream lines(server_out);
		for (std::string line; std::getline(lines, line);) {
			if (contains(line, "[dedicated] level change ")) {
				changes.push_back(line);
			}
		}
		REQUIRE(changes.size() >= 3u);
		CHECK(contains(changes[0], "level change 1: MP_011 TeamDeathMatch0 round 1,"));
		CHECK(contains(changes[0], "carried 2 clients"));
		CHECK(contains(changes[1], "level change 2: MP_003 TeamDeathMatch0 round 2,"));
		CHECK(contains(changes[1], "carried 1 clients"));
		CHECK(contains(changes[2], "level change 3: MP_Subway TeamDeathMatch0 round 3,"));
		CHECK(contains(changes[2], "carried 1 clients"));
		const double warm_s = level_change_load_s(changes[2]);
		CHECK(warm_s >= 0.0);
		CHECK(warm_s <= 2.0);
		CHECK(contains(server_out, "LoadStalled=1"));
		CHECK(contains(server_summary, "install unchanged"));
		CHECK(contains(server_summary, "stopped by --stop-after-levels"));
		CHECK(contains(server_summary, "summary: MP_Subway TeamDeathMatch0 loaded in "));
		const uint64_t kills = summary_number(server_summary, ", kills ");
		CHECK(kills != UINT64_MAX);
		CHECK(kills >= 15u);
		MESSAGE(changes[0] << "\n"
						   << changes[1] << "\n"
						   << changes[2] << "\n"
						   << staller_summary << "\n"
						   << steady_summary << "\n"
						   << server_summary);
	}
}

TEST_SUITE("godot-rotation") {
	TEST_CASE("two Godot --connect clients, one windowed on an emptied cache and one headless on a kept cache, follow "
			  "a dedicated server through MP_Subway, MP_011 and MP_003 with the data's end-of-round intermission: each "
			  "prepares the next level before the level change, neither is kicked, and each logs its load seconds and "
			  "cache bytes per level") {
		ProcDriver driver(driver_config_from_env());
		if (const std::string why = driver.skip_reason(godot_client(driver, 1, {}, "cold", true)); !why.empty()) {
			bf3::test::skip_install_case(why.c_str());
			return;
		}
		const std::filesystem::path maplist = driver.roots("server").dir / L"maplist.cfg";
		std::error_code ec;
		std::filesystem::create_directories(maplist.parent_path(), ec);
		{
			std::ofstream out(maplist, std::ios::binary);
			out << "MP_Subway TeamDeathMatch0 1\nMP_011 TeamDeathMatch0 1\nMP_003 TeamDeathMatch0 1\n";
		}
		std::filesystem::remove_all(driver.roots("cold").cache, ec);
		const std::optional<Server> server = start_server(
				driver, 8, 1700, { "--maplist=" + path_utf8(maplist), "--kill-limit=5", "--stop-after-levels=3" });
		if (!server) {
			return;
		}
		std::map<std::string, size_t> clients;
		for (const char *role : { "cold", "warm" }) {
			bf3::core::Result<size_t> started = driver.start(godot_client(driver, server->port,
					{ "--match-levels=3", "--quit-after=2" }, role, std::string_view(role) == "cold"));
			REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
			clients[role] = *started;
		}
		for (const auto &[role, id] : clients) {
			std::string line;
			CHECK_MESSAGE(driver.wait_line(
								  id,
								  [](std::string_view l) {
									  return contains(l, "[match] level 3 ") || contains(l, " ended: ");
								  },
								  1500s, &line),
					role << ":\n"
						 << driver.output(id));
			CHECK_MESSAGE(contains(line, "[match] level 3 MP_003 TeamDeathMatch0 done: "), role << ": " << line);
		}
		for (const auto &[role, id] : clients) {
			const ChildEnd end = driver.finish(id, 60s);
			const std::string out = driver.output(id);
			CHECK_MESSAGE(end.code == 0, role << ":\n" << out);
			CHECK(end.stragglers == 0);
			CHECK_FALSE(contains(out, " ended: "));
			CHECK(contains(out, "[level] install unchanged: yes"));
			for (const char *level : { "MP_011", "MP_003" }) {
				const std::vector<std::string> prepared =
						lines_with(out, std::string("[match] prepare ") + level + " TeamDeathMatch0: ");
				CHECK_MESSAGE(prepared.size() == 1u, role << " " << level << ":\n" << out);
				for (const std::string &p : prepared) {
					CHECK_MESSAGE(contains(p, ": ready in "), role << ": " << p);
					CHECK_MESSAGE(contains(p, "finished before the level change"), role << ": " << p);
					MESSAGE(role << ": " << p);
				}
			}
			const std::vector<std::string> levels = lines_with(out, " done: private ");
			CHECK(levels.size() == 3u);
			for (const std::string &l : levels) {
				CHECK_MESSAGE(contains(l, ", load "), role << ": " << l);
				CHECK_MESSAGE(summary_number(l, ", cache ") != UINT64_MAX, role << ": " << l);
				MESSAGE(role << ": " << l);
			}
			if (role == "cold" && !levels.empty()) {
				CHECK_MESSAGE(contains(levels.front(), "(cold, target 15 s)"), levels.front());
			}
		}
		const std::string server_out = driver.output(server->id);
		const std::vector<std::string> kicks = lines_with(server_out, "| kicks ");
		CHECK_FALSE(kicks.empty());
		for (const std::string &k : kicks) {
			CHECK_MESSAGE(contains(k, " LoadTimeout=0 LoadStalled=0 |"), k);
		}
		CHECK(contains(server_out, "level change 1: MP_011 TeamDeathMatch0 round 1,"));
		CHECK(contains(server_out, "level change 2: MP_003 TeamDeathMatch0 round 2,"));
		CHECK(lines_with(server_out, "carried 2 clients").size() == 2u);
		driver.finish(server->id, 1s);
	}
}

TEST_SUITE("net-procs-master") {
	TEST_CASE("a dedicated server registers with a master process, bf3dump servers lists it, and a headless Godot "
			  "client finds and joins it by name") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<size_t> master = start_tool(driver, "master", { "master", "--port=0", "--seconds=900" });
		if (!master) {
			return;
		}
		constexpr std::string_view kListening = "master: listening on 127.0.0.1:";
		std::string listening;
		REQUIRE_MESSAGE(line_starts(driver, *master, kListening, 30s, &listening), driver.output(*master));
		const std::string_view digits = std::string_view(listening).substr(kListening.size());
		uint32_t master_port = 0;
		std::from_chars(digits.data(), digits.data() + digits.size(), master_port);
		REQUIRE_MESSAGE((master_port > 0 && master_port < 65536), listening);
		const std::string at = "127.0.0.1:" + std::to_string(master_port);
		const std::optional<Server> server = start_server(driver, 0, 900,
				{ "--level=MP_011", "--mode=TeamDeathMatch0", "--master=" + at, "--name=procs-master" });
		if (!server) {
			driver.finish(*master, 1s);
			return;
		}
		CHECK_MESSAGE(driver.wait_line(
							  server->id,
							  [&](std::string_view l) {
								  return contains(l, "[dedicated] master " + at + ": registered as procs-master");
							  },
							  60s),
				driver.output(server->id));
		const std::optional<size_t> listing = start_tool(driver, "servers", { "servers", at });
		REQUIRE(listing.has_value());
		const ChildEnd listed = driver.finish(*listing, 60s);
		const std::string list_out = driver.output(*listing);
		require_clean_exit(driver, *listing, listed, "bf3dump servers");
		CHECK_MESSAGE(contains(list_out, "servers: 1 on " + at), list_out);
		CHECK_MESSAGE(contains(list_out,
							  "server: procs-master | 127.0.0.1:" + std::to_string(server->port) +
									  " | MP_011 TeamDeathMatch0 | players 0, bots 0, max 32 | protocol "),
				list_out);
		CHECK_MESSAGE(contains(list_out, " (this build) | "), list_out);
		bf3::core::Result<size_t> client = driver.start(driver.godot_child("client",
				{ "--headless", "res://scenes/metro_tdm.tscn" }, { "--connect=procs-master@" + at, "--quit-after=1" }));
		REQUIRE_MESSAGE(client.ok(), (client.ok() ? std::string() : client.error().message));
		std::string line;
		CHECK_MESSAGE(driver.wait_line(
							  *client,
							  [](std::string_view l) {
								  return contains(l, "[level] done: ") || contains(l, "[level] --connect=");
							  },
							  600s, &line),
				driver.output(*client));
		CHECK(contains(line, "[level] done: "));
		const ChildEnd client_end = driver.finish(*client, 60s);
		const std::string out = driver.output(*client);
		CHECK_MESSAGE(client_end.code == 0, out);
		CHECK(client_end.stragglers == 0);
		CHECK(contains(out, "the master lists procs-master at 127.0.0.1:" + std::to_string(server->port)));
		CHECK(contains(out, "[level] MP_011 TeamDeathMatch0"));
		CHECK(contains(out, "[level] install unchanged: yes"));
		driver.finish(server->id, 1s);
		const ChildEnd master_end = driver.finish(*master, 1s);
		CHECK(master_end.stragglers == 0);
		const std::vector<std::string> counts = lines_with(driver.output(*master), "master: 1 servers | ");
		REQUIRE_MESSAGE(!counts.empty(), driver.output(*master));
		CHECK_MESSAGE(contains(counts.back(), "registered 1,"), counts.back());
		CHECK_MESSAGE(contains(counts.back(), "queries 2 | refused 0 ("), counts.back());
		MESSAGE(line);
	}
}
