#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "net_procs_support.h"
#include "test_support.h"
#include "win32_text.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bf3::core::detail::path_utf8;
using namespace bf3::test::procs;

constexpr uint32_t kGateBots = 29;
constexpr uint32_t kControlBots = 31;
constexpr int kKillLimit = 100;
constexpr double kTeamKdMin = 0.75;
constexpr double kTeamKdMax = 1.33;
constexpr double kSquadMedianMetres = 20.0;
constexpr double kSquadNearPercent = 70.0;
constexpr uint64_t kMinSquadSamples = 2000;
constexpr double kTickP99Ms = 8.0;
constexpr std::string_view kNetsim = "--netsim=lat=50";
constexpr std::string_view kMateName = "mate";
constexpr std::string_view kStatusPrefix = "[dedicated] t=";
constexpr std::string_view kRoundOver = ": round over US ";

std::vector<std::string> lines_of(const std::string &p_text, std::string_view p_prefix) {
	std::vector<std::string> out;
	std::istringstream in(p_text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind(p_prefix, 0) == 0) {
			out.push_back(line);
		}
	}
	return out;
}

std::string squads_line(const std::filesystem::path &p_stats) {
	std::ifstream in(p_stats, std::ios::binary);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind("squads ", 0) == 0) {
			return line.substr(7);
		}
	}
	return {};
}

std::optional<std::string> squad_of(const std::string &p_list, uint32_t p_slot) {
	std::istringstream in(p_list);
	std::string token;
	const std::string key = std::to_string(p_slot) + ":";
	while (in >> token) {
		if (token.rfind(key, 0) == 0) {
			std::string team_squad = token.substr(key.size());
			if (!team_squad.empty() && team_squad.back() == 'L') {
				team_squad.pop_back();
			}
			return team_squad;
		}
	}
	return std::nullopt;
}

struct Cohesion {
	uint64_t samples = 0;
	double median = 0.0;
	double near = 0.0;
};

Cohesion status_cohesion(std::string_view p_line) {
	const size_t at = p_line.find("| squads median ");
	const std::string_view squads = at == std::string_view::npos ? std::string_view() : p_line.substr(at);
	Cohesion c;
	c.median = number_after(squads, "| squads median ").value_or(1.0e9);
	c.near = number_after(squads, " m, near ").value_or(-1.0);
	c.samples = uint64_t(number_after(squads, " % of ").value_or(0.0));
	return c;
}

}

TEST_SUITE("net-procs-bots-squads") {
	TEST_CASE("the dedicated Metro TDM server with 29 bots, a bf3dump client that leads a squad of its own and a "
			  "headless game client that joins it, both at 100 ms RTT: a round to 100 kills, team K/D in [0.75, "
			  "1.33], squad cohesion as host.tdm.squads, both clients agree on every squad and leader, the game "
			  "client deploys on its mate, no kick, tick p99 < 8 ms") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server = start_server(driver, kGateBots, 2400,
				{ "--level=" + std::string(kLevel), "--mode=" + std::string(kMode),
						"--kill-limit=" + std::to_string(kKillLimit) });
		if (!server) {
			return;
		}
		const std::optional<size_t> mate = start_tool(driver, "mate",
				client_args(driver, "mate", server->port,
						{ "--script=still", "--squad=create", "--until=round-over", "--seconds=2300",
								"--name=" + std::string(kMateName), std::string(kNetsim) }));
		if (!mate || !need(line_starts(driver, *mate, "client: playing", 300s), driver.output(*mate))) {
			driver.finish(server->id, 1s);
			return;
		}
		bf3::core::Result<size_t> game = driver.start(godot_client(
				driver, server->port, { std::string(kNetsim), "--squad-check=" + std::string(kMateName) }, "game"));
		REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
		std::string report;
		const bool reported = driver.wait_line(
				*game, [](std::string_view l) { return contains(l, "[squads] ") || contains(l, " ended: "); }, 2200s,
				&report);
		CHECK_MESSAGE(reported, driver.output(*game));
		const ChildEnd game_end = driver.finish(*game, 60s);
		CHECK_MESSAGE(line_starts(driver, *mate, "client: summary", 300s), driver.output(*mate));
		const ChildEnd mate_end = driver.finish(*mate, 60s);
		require_clean_exit(driver, *mate, mate_end, "bf3dump client");
		driver.wait_line(
				server->id,
				[](std::string_view l) { return l.rfind(kStatusPrefix, 0) == 0 && contains(l, kRoundOver); }, 10s);
		const std::string server_out = driver.output(server->id);
		driver.finish(server->id, 1s);
		MESSAGE("game client: " << report);
		if (!need(contains(report, "[squads] done: "), driver.output(*game))) {
			return;
		}
		CHECK(game_end.code == 0);

		std::string game_squads = report.substr(report.rfind("; squads ") + 9);
		while (!game_squads.empty() && (game_squads.back() == '\r' || game_squads.back() == ' ')) {
			game_squads.pop_back();
		}
		const std::string mate_squads = squads_line(driver.roots("mate").dir / L"stats.txt");
		CHECK_MESSAGE(!mate_squads.empty(), driver.output(*mate));
		CHECK(game_squads == mate_squads);
		const uint64_t mate_slot = summary_number(report, ": mate " + std::string(kMateName) + " slot ");
		const uint64_t own_slot = summary_number(report, ", own slot ");
		if (!need(mate_slot != UINT64_MAX, report) || !need(own_slot != UINT64_MAX, report)) {
			return;
		}
		const std::optional<std::string> mate_squad = squad_of(game_squads, uint32_t(mate_slot));
		const std::optional<std::string> own_squad = squad_of(game_squads, uint32_t(own_slot));
		if (!need(mate_squad.has_value(), game_squads) || !need(own_squad.has_value(), game_squads)) {
			return;
		}
		CHECK(*mate_squad == *own_squad);
		CHECK(mate_squad->find("/0") == std::string::npos);
		const uint64_t mate_spawns = summary_number(report, ", on the mate ");
		CHECK(mate_spawns != UINT64_MAX);
		CHECK(mate_spawns >= 1u);

		const std::vector<std::string> status = lines_of(server_out, kStatusPrefix);
		const auto seated = std::find_if(status.begin(), status.end(),
				[](const std::string &l) { return number_after(l, " clients ").value_or(0.0) >= 2.0; });
		const auto over = std::find_if(
				status.begin(), status.end(), [](const std::string &l) { return contains(l, kRoundOver); });
		if (!need(seated != status.end(), server_out) || !need(over != status.end(), server_out)) {
			return;
		}
		double worst_p99 = 0.0;
		std::string_view worst_line;
		bool kicks = true;
		for (auto it = seated; it != status.end() && it <= over; ++it) {
			const double p99 = number_after(*it, ") p99 ").value_or(1.0e9);
			if (p99 >= worst_p99) {
				worst_p99 = p99;
				worst_line = *it;
			}
		}
		for (const std::string &l : status) {
			kicks = kicks && kicks_none(l);
		}
		const std::string_view score = std::string_view(*over).substr(over->find(kRoundOver) + kRoundOver.size() - 3);
		const double us = number_after(score, "US ").value_or(0.0);
		const double ru = number_after(score, " - RU ").value_or(0.0);
		const double limit = number_after(score.substr(score.find(" - RU ")), " of ").value_or(0.0);
		const Cohesion c = status_cohesion(*over);
		const uint64_t spawns_on_mates = summary_number(*over, "spawns on a mate ");
		std::printf("net.procs.bots_squads: US %.0f - RU %.0f of %.0f, team K/D %.3f; squads median %.0f m, near "
					"%.1f %% of %llu; spawns on a mate %llu; worst 10 s tick p99 %.3f ms; kicks %s\n%s\n",
				us, ru, limit, ru > 0.0 ? us / ru : 0.0, c.median, c.near, (unsigned long long)c.samples,
				(unsigned long long)spawns_on_mates, worst_p99, kicks ? "none" : "some", over->c_str());
		std::fflush(stdout);
		CHECK(limit == double(kKillLimit));
		CHECK(std::max(us, ru) == double(kKillLimit));
		if (!need(ru > 0.0, *over)) {
			return;
		}
		CHECK(us / ru >= kTeamKdMin);
		CHECK(us / ru <= kTeamKdMax);
		CHECK(c.samples >= kMinSquadSamples);
		CHECK(c.median <= kSquadMedianMetres);
		CHECK(c.near >= kSquadNearPercent);
		CHECK(spawns_on_mates != UINT64_MAX);
		CHECK(spawns_on_mates >= 1u);
		CHECK(kicks);
		CHECK_MESSAGE(worst_p99 < kTickP99Ms, worst_line);
	}

	TEST_CASE("the control: the same server with --bot-squads=off fails the cohesion threshold") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server = start_server(driver, kControlBots, 900,
				{ "--level=" + std::string(kLevel), "--mode=" + std::string(kMode), "--bot-squads=off",
						"--stop-after-kills=5", "--stop-after-squad-samples=" + std::to_string(kMinSquadSamples) });
		if (!server) {
			return;
		}
		std::string summary;
		CHECK(driver.wait_line(
				server->id, [](std::string_view l) { return contains(l, "[dedicated] summary: "); }, 960s, &summary));
		const ChildEnd end = driver.finish(server->id, 60s);
		require_clean_exit(driver, server->id, end, "dedicated server");
		const std::string out = driver.output(server->id);
		std::printf("net.procs.bots_squads control: %s\n", summary.c_str());
		std::fflush(stdout);
		CHECK(contains(out, "bot squads off (--bot-squads=off)"));
		CHECK(contains(summary, "stopped by --stop-after-kills"));
		CHECK(summary_number(summary, "squad samples ") >= kMinSquadSamples);
		const double median = number_after(summary, "squad median ").value_or(0.0);
		const double near = number_after(summary, "squad near ").value_or(100.0);
		CHECK((median > kSquadMedianMetres || near < kSquadNearPercent));
	}
}
