#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "test_support.h"
#include "win32_text.h"

#include "bf3/present/effect_seed.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
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
using bf3::test::procs::contains;
using bf3::test::procs::driver_config_from_env;
using bf3::test::procs::ProcDriver;

constexpr std::string_view kLevel = "MP_Subway";
constexpr std::string_view kMode = "TeamDeathMatch0";
constexpr std::string_view kServerLine = "[dedicated] ServerHost on 127.0.0.1:";

bool need(bool p_ok, const std::string &p_why) {
	CHECK_MESSAGE(p_ok, p_why);
	return p_ok;
}

struct Server {
	size_t id = 0;
	uint16_t port = 0;
};

std::optional<Server> start_server(ProcDriver &r_driver, uint32_t p_bots, uint32_t p_seconds) {
	const ChildSpec spec = r_driver.godot_child("server", { "--headless", "--main-loop", "DedicatedMainLoop" },
			{ "--level=" + std::string(kLevel), "--mode=" + std::string(kMode), "--bots=" + std::to_string(p_bots),
					"--preround=0", "--dedicated-seconds=" + std::to_string(p_seconds) });
	if (const std::string why = r_driver.skip_reason(spec); !why.empty()) {
		bf3::test::skip_install_case(why.c_str());
		return std::nullopt;
	}
	bf3::core::Result<size_t> id = r_driver.start(spec);
	if (!need(id.ok(), id.ok() ? std::string() : id.error().message)) {
		return std::nullopt;
	}
	std::string line;
	const bool printed = r_driver.wait_line(
			*id, [](std::string_view l) { return contains(l, kServerLine) || contains(l, "[dedicated] failed: "); },
			300s, &line);
	if (printed && contains(line, "no usable install")) {
		r_driver.finish(*id, 30s);
		bf3::test::skip_install_case(line.c_str());
		return std::nullopt;
	}
	if (!need(printed && contains(line, kServerLine), r_driver.output(*id))) {
		return std::nullopt;
	}
	const std::string_view digits = std::string_view(line).substr(line.find(kServerLine) + kServerLine.size());
	uint32_t port = 0;
	std::from_chars(digits.data(), digits.data() + digits.size(), port);
	if (!need(port > 0 && port < 65536, line)) {
		return std::nullopt;
	}
	return Server{ *id, uint16_t(port) };
}

std::vector<std::string> client_args(
		ProcDriver &r_driver, std::string_view p_role, uint16_t p_port, std::vector<std::string> p_extra) {
	const bf3::test::procs::ChildRoots r = r_driver.roots(p_role);
	std::error_code ec;
	std::filesystem::create_directories(r.cache, ec);
	std::vector<std::string> args = { "client", "127.0.0.1:" + std::to_string(p_port), std::string(kLevel),
		std::string(kMode), "--cache=" + path_utf8(r.cache), "--stats-file=" + path_utf8(r.dir / L"stats.txt") };
	args.insert(args.end(), p_extra.begin(), p_extra.end());
	return args;
}

std::optional<size_t> start_tool(ProcDriver &r_driver, std::string_view p_role, std::vector<std::string> p_args) {
	bf3::core::Result<size_t> id =
			r_driver.start(r_driver.tool_child(p_role, r_driver.config().bf3dump, std::move(p_args)));
	if (!need(id.ok(), id.ok() ? std::string() : id.error().message)) {
		return std::nullopt;
	}
	return *id;
}

bool line_starts(ProcDriver &r_driver, size_t p_child, std::string_view p_prefix, std::chrono::milliseconds p_timeout,
		std::string *r_line = nullptr) {
	return r_driver.wait_line(
			p_child, [&](std::string_view l) { return l.rfind(p_prefix, 0) == 0; }, p_timeout, r_line);
}

void require_clean_exit(ProcDriver &r_driver, size_t p_child, const ChildEnd &p_end, const char *p_what) {
	CHECK_MESSAGE(p_end.exited, p_what);
	CHECK_MESSAGE(p_end.code == 0, p_what << " exited " << p_end.code << ":\n" << r_driver.output(p_child));
	CHECK(p_end.stragglers == 0);
	CHECK(r_driver.job_processes(p_child) == 0);
}

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

uint64_t summary_number(std::string_view p_line, std::string_view p_key) {
	const size_t at = p_line.find(p_key);
	if (at == std::string_view::npos) {
		return UINT64_MAX;
	}
	const std::string_view tail = p_line.substr(at + p_key.size());
	uint64_t v = 0;
	const auto res = std::from_chars(tail.data(), tail.data() + tail.size(), v);
	return res.ec == std::errc() ? v : UINT64_MAX;
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
							  server->id, [](std::string_view l) { return contains(l, " clients 5 bots "); }, 30s),
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
