#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "net_procs_support.h"
#include "test_support.h"
#include "win32_text.h"

#include <algorithm>
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
#include <vector>

namespace {

using namespace std::chrono_literals;
using bf3::core::detail::path_utf8;
using namespace bf3::test::procs;

constexpr std::string_view kNetsim100ms = "--netsim=lat=50";
constexpr uint32_t kFxBots = 23;
constexpr uint32_t kFxPlaySeconds = 20;
constexpr size_t kFxMinSharedMuzzles = 20;
constexpr size_t kFxMinSharedImpacts = 20;
constexpr uint32_t kJoinBots = 15;
constexpr uint32_t kJoinShooters = 12;
constexpr uint32_t kJoinShooterSeconds = 200;
constexpr uint64_t kJoinAfterKills = 2;

ChildSpec windowed_client(
		ProcDriver &r_driver, uint16_t p_port, std::vector<std::string> p_extra, std::string_view p_role) {
	std::vector<std::string> args = { "--connect=127.0.0.1:" + std::to_string(p_port) };
	args.insert(args.end(), p_extra.begin(), p_extra.end());
	return r_driver.godot_child(p_role, { "--resolution", "1280x720", "res://scenes/metro_tdm.tscn" }, std::move(args));
}

bool windows_skipped(ProcDriver &r_driver) {
	if (const std::string why = r_driver.skip_reason(windowed_client(r_driver, 1, {}, "probe")); !why.empty()) {
		bf3::test::skip_install_case(why.c_str());
		return true;
	}
	return false;
}

std::optional<size_t> start_child(ProcDriver &r_driver, const ChildSpec &p_spec) {
	bf3::core::Result<size_t> id = r_driver.start(p_spec);
	if (!need(id.ok(), id.ok() ? std::string() : id.error().message)) {
		return std::nullopt;
	}
	return *id;
}

std::vector<std::string> split_csv(const std::string &p_line) {
	std::vector<std::string> out;
	std::string cell;
	std::istringstream s(p_line);
	while (std::getline(s, cell, ',')) {
		out.push_back(cell);
	}
	if (!p_line.empty() && p_line.back() == ',') {
		out.emplace_back();
	}
	return out;
}

struct Trace {
	bool read = false;
	size_t rows = 0;
	std::map<std::string, std::set<std::string>> by_key;
};

Trace read_trace(const std::filesystem::path &p_path, std::string_view p_header, const std::vector<size_t> &p_key,
		const std::vector<size_t> &p_value, const std::set<std::string> &p_kinds) {
	Trace t;
	std::ifstream in(p_path, std::ios::binary);
	std::string line;
	if (!std::getline(in, line) || line != p_header) {
		return t;
	}
	t.read = true;
	while (std::getline(in, line)) {
		const std::vector<std::string> cells = split_csv(line);
		if (cells.empty() || p_kinds.count(cells[0]) == 0) {
			continue;
		}
		std::string key, value;
		for (size_t k : p_key) {
			key += (k < cells.size() ? cells[k] : std::string("?")) + ",";
		}
		for (size_t v : p_value) {
			value += (v < cells.size() ? cells[v] : std::string("?")) + ",";
		}
		t.by_key[key].insert(value);
		++t.rows;
	}
	return t;
}

std::string joined(const std::set<std::string> &p_values) {
	std::string out;
	for (const std::string &v : p_values) {
		out += (out.empty() ? "{ " : " | ") + v;
	}
	return out + " }";
}

struct Shared {
	size_t keys = 0;
	size_t differ = 0;
	size_t one_sided = 0;
	std::map<std::string, size_t> by_prefix;
	std::string first_difference;
};

Shared compare(const Trace &p_a, const Trace &p_b, size_t p_prefix_cells) {
	Shared s;
	for (const auto &[key, values] : p_a.by_key) {
		const auto other = p_b.by_key.find(key);
		if (other == p_b.by_key.end()) {
			continue;
		}
		++s.keys;
		size_t at = 0;
		for (size_t i = 0; i < p_prefix_cells; ++i) {
			at = key.find(',', at) + 1;
		}
		++s.by_prefix[key.substr(0, at)];
		if (other->second == values) {
			continue;
		}
		const bool a_in_b = std::includes(other->second.begin(), other->second.end(), values.begin(), values.end());
		const bool b_in_a = std::includes(values.begin(), values.end(), other->second.begin(), other->second.end());
		if (a_in_b || b_in_a) {
			++s.one_sided;
			continue;
		}
		++s.differ;
		if (s.first_difference.empty()) {
			s.first_difference = key + " alpha " + joined(values) + " bravo " + joined(other->second);
		}
	}
	return s;
}

uint64_t round_kills(std::string_view p_line) {
	const uint64_t us = summary_number(p_line, " US ");
	const uint64_t ru = summary_number(p_line, " - RU ");
	return us == UINT64_MAX || ru == UINT64_MAX ? 0 : us + ru;
}

std::string line_with(const std::string &p_text, std::string_view p_needle) {
	std::istringstream lines(p_text);
	for (std::string line; std::getline(lines, line);) {
		if (contains(line, p_needle)) {
			return line;
		}
	}
	return {};
}

}

TEST_SUITE("godot-kick-popup") {
	TEST_CASE("godot.kick_popup: a windowed --connect client with an edited motor_tuning.cfg is kicked by a dedicated "
			  "server, popupgeneric shows the reason, and its QUIT exits 0") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		ProcDriver driver(driver_config_from_env());
		if (windows_skipped(driver)) {
			return;
		}
		const std::filesystem::path tuning = driver.roots("client").dir / L"motor_tuning_edited.cfg";
		std::filesystem::create_directories(tuning.parent_path());
		{
			std::ofstream out(tuning, std::ios::binary);
			out << edited_motor_tuning(OPENBF3_MOTOR_TUNING_CFG);
			REQUIRE(out.good());
		}
		const std::optional<Server> server = start_server(driver, 0, 600);
		if (!server) {
			return;
		}
		const std::optional<size_t> client = start_child(driver,
				windowed_client(driver, server->port, { "--motor-tuning=" + path_utf8(tuning), "--kick-popup-check" },
						"client"));
		REQUIRE(client.has_value());
		std::string popup;
		CHECK_MESSAGE(line_starts(driver, *client, "[match] kick popup: ", 300s, &popup), driver.output(*client));
		CHECK(contains(popup, "title \"YOU HAVE BEEN DISCONNECTED\""));
		CHECK(contains(popup, "text \"install differs: motor\""));
		CHECK(contains(popup, "button \"QUIT\""));
		CHECK_MESSAGE(line_starts(driver, *client, "[match] QUIT from the kick popup", 30s), driver.output(*client));
		const ChildEnd client_end = driver.finish(*client, 60s);
		require_clean_exit(driver, *client, client_end, "client");
		const std::string out = driver.output(*client);
		CHECK(contains(out, "ended: kicked: install differs: motor"));
		CHECK(contains(out, "[level] motor tuning read from " + path_utf8(tuning)));
		std::string kicks;
		CHECK_MESSAGE(driver.wait_line(
							  server->id, [](std::string_view l) { return contains(l, " Motor=1 "); }, 10s, &kicks),
				driver.output(server->id));
		CHECK(contains(kicks, "Protocol=0 Build=0 Full=0 Source=0 Collision=0 Motor=1 "));
		const ChildEnd server_end = driver.finish(server->id, 1s);
		CHECK(server_end.stragglers == 0);
		CHECK(driver.job_processes(server->id) == 0);
		CHECK(bf3::core::diff_snapshots(m.before, m.reader.snapshot()).clean());
		MESSAGE(popup << "\nserver: " << kicks);
	}
}

TEST_SUITE("godot-net-fx-same") {
	TEST_CASE("godot.net.fx_same: two windowed clients at 100 ms RTT start the same muzzle, tracer and impact effects "
			  "with the same seeds, and the same weapon sounds, for the bots' bursts") {
		ProcDriver driver(driver_config_from_env());
		if (windows_skipped(driver)) {
			return;
		}
		const std::optional<Server> server = start_server(driver, kFxBots, 600);
		if (!server) {
			return;
		}
		const std::vector<std::string> roles = { "alpha", "bravo" };
		std::vector<size_t> clients;
		for (const std::string &role : roles) {
			const ChildRoots r = driver.roots(role);
			std::filesystem::create_directories(r.dir);
			const std::optional<size_t> id = start_child(driver,
					windowed_client(driver, server->port,
							{ std::string(kNetsim100ms), "--fx-trace=" + path_utf8(r.dir / L"fx.csv"),
									"--audio-trace=" + path_utf8(r.dir / L"audio.csv"),
									"--quit-after=" + std::to_string(kFxPlaySeconds), "--auto-deploy" },
							role));
			REQUIRE(id.has_value());
			clients.push_back(*id);
		}
		std::vector<std::string> exits;
		for (size_t i = 0; i < clients.size(); ++i) {
			CHECK_MESSAGE(driver.wait_line(
								  clients[i],
								  [](std::string_view l) {
									  return contains(l, "[game] fx trace: ") || contains(l, " ended: ");
								  },
								  300s),
					driver.output(clients[i]));
			const ChildEnd end = driver.finish(clients[i], 60s);
			require_clean_exit(driver, clients[i], end, roles[i].c_str());
			const std::string out = driver.output(clients[i]);
			CHECK(contains(out, "[level] install unchanged: yes"));
			exits.push_back(line_with(out, "[game] fx trace: ") + "\n" + line_with(out, "[game] audio trace: "));
		}
		driver.finish(server->id, 1s);

		const std::set<std::string> fx_kinds = { "RemoteShot", "Impact", "Hit", "SoldierContact" };
		const std::string fx_header = "kind,purpose,slot,id,pellet,seed,blueprint,index";
		const Trace fa =
				read_trace(driver.roots("alpha").dir / L"fx.csv", fx_header, { 0, 1, 2, 3, 4, 7 }, { 5, 6 }, fx_kinds);
		const Trace fb =
				read_trace(driver.roots("bravo").dir / L"fx.csv", fx_header, { 0, 1, 2, 3, 4, 7 }, { 5, 6 }, fx_kinds);
		const std::set<std::string> sound_kinds = { "RemoteBurst", "RemoteOneShot", "StateBurst", "Detonation" };
		const std::string sound_header = "kind,slot,def,key,seed";
		const Trace sa =
				read_trace(driver.roots("alpha").dir / L"audio.csv", sound_header, { 0, 1, 2, 3 }, { 4 }, sound_kinds);
		const Trace sb =
				read_trace(driver.roots("bravo").dir / L"audio.csv", sound_header, { 0, 1, 2, 3 }, { 4 }, sound_kinds);
		REQUIRE(fa.read);
		REQUIRE(fb.read);
		REQUIRE(sa.read);
		REQUIRE(sb.read);
		Shared fx = compare(fa, fb, 2);
		const Shared sounds = compare(sa, sb, 1);
		std::string by_purpose;
		for (const auto &[prefix, n] : fx.by_prefix) {
			by_purpose += " " + prefix + std::to_string(n);
		}
		std::string by_kind;
		for (const auto &[prefix, n] : sounds.by_prefix) {
			by_kind += " " + prefix + std::to_string(n);
		}
		MESSAGE("fx rows alpha " << fa.rows << " bravo " << fb.rows << ", shared keys " << fx.keys << " (" << by_purpose
								 << " ), differing " << fx.differ << ", starts only one client made " << fx.one_sided
								 << "; weapon sound rows alpha " << sa.rows << " bravo " << sb.rows << ", shared keys "
								 << sounds.keys << " (" << by_kind << " ), differing " << sounds.differ << "\n"
								 << exits[0] << "\n"
								 << exits[1]);
		CHECK_MESSAGE(fx.differ == 0, fx.first_difference);
		CHECK_MESSAGE(sounds.differ == 0, sounds.first_difference);
		CHECK(fx.by_prefix["RemoteShot,Muzzle,"] >= kFxMinSharedMuzzles);
		CHECK(fx.by_prefix["RemoteShot,Tracer,"] >= 1);
		CHECK(fx.by_prefix["Impact,ImpactEffect,"] >= kFxMinSharedImpacts);
		CHECK(sounds.keys >= 1);
	}
}

TEST_SUITE("godot-net-late-join") {
	TEST_CASE("godot.net.late_join: a windowed client joining a running round at 100 ms RTT has the fire loops at "
			  "once and starts no effect or sound timed before its first snapshot; one joining in RoundOver gets the "
			  "end-of-round screen") {
		ProcDriver driver(driver_config_from_env());
		if (windows_skipped(driver)) {
			return;
		}
		const std::string level = "--level=" + std::string(kLevel);
		const std::string mode = "--mode=" + std::string(kMode);
		const std::optional<Server> over =
				start_server(driver, kJoinBots, 600, { level, mode, "--kill-limit=1", "--intermission-s=120" }, "over");
		if (!over) {
			return;
		}
		const std::optional<Server> round =
				start_server(driver, kJoinBots, 600, { level, mode, "--kill-limit=500", "--ghost-scope=off" }, "round");
		if (!round) {
			return;
		}
		const std::optional<size_t> shooters = start_tool(driver, "shooters",
				{ "loadbots", "127.0.0.1:" + std::to_string(round->port), std::string(kLevel), std::string(kMode),
						"--count=" + std::to_string(kJoinShooters), "--script=relayfire",
						"--seconds=" + std::to_string(kJoinShooterSeconds), "--name=lj", "--seed=1" });
		REQUIRE(shooters.has_value());
		const std::string all_spawned = " spawned " + std::to_string(kJoinShooters) + " ";
		CHECK_MESSAGE(driver.wait_line(
							  *shooters, [&](std::string_view l) { return contains(l, all_spawned); }, 120s),
				driver.output(*shooters));
		std::string fighting;
		CHECK_MESSAGE(driver.wait_line(
							  round->id,
							  [](std::string_view l) {
								  return contains(l, ": in round US ") && round_kills(l) >= kJoinAfterKills;
							  },
							  120s, &fighting),
				driver.output(round->id));
		const std::optional<size_t> joiner = start_child(driver,
				windowed_client(driver, round->port,
						{ std::string(kNetsim100ms), "--late-join-check=in-round", "--auto-deploy" }, "joiner"));
		REQUIRE(joiner.has_value());
		std::string over_line;
		CHECK_MESSAGE(
				driver.wait_line(
						over->id, [](std::string_view l) { return contains(l, ": round over US "); }, 120s, &over_line),
				driver.output(over->id));
		const std::optional<size_t> late = start_child(driver,
				windowed_client(driver, over->port, { std::string(kNetsim100ms), "--late-join-check=round-over" },
						"round_over"));
		REQUIRE(late.has_value());
		std::string in_round, in_over;
		CHECK_MESSAGE(line_starts(driver, *joiner, "[late_join] LATE JOIN in-round: ", 300s, &in_round),
				driver.output(*joiner));
		CHECK_MESSAGE(
				line_starts(driver, *late, "[late_join] LATE JOIN round-over: ", 300s, &in_over), driver.output(*late));
		CHECK(contains(in_round, "LATE JOIN in-round: PASS"));
		CHECK(contains(in_over, "LATE JOIN round-over: PASS"));
		for (const size_t id : { *joiner, *late }) {
			const ChildEnd end = driver.finish(id, 60s);
			require_clean_exit(driver, id, end, "client");
			CHECK(contains(driver.output(id), "[level] install unchanged: yes"));
		}
		driver.finish(*shooters, 1s);
		driver.finish(round->id, 1s);
		driver.finish(over->id, 1s);
		MESSAGE(in_round << "\n" << in_over << "\nround over server: " << over_line);
	}
}
