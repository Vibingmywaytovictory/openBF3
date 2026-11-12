#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "net_procs_support.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace bf3::test::procs;

constexpr std::string_view kCaspian = "MP_007";
constexpr std::string_view kConquestSmall = "ConquestSmall0";
constexpr uint32_t kSlots = 64;
constexpr uint32_t kServerSeconds = 90;
constexpr uint32_t kClientPlaySeconds = 30;
constexpr double kServerBudgetMiB = 0.4 * 1024.0;
constexpr double kClientVideoBudgetMiB = 2.5 * 1024.0;
constexpr double kClientMemoryBudgetMiB = 1.5 * 1024.0;
constexpr std::string_view kStatusPrefix = "[dedicated] t=";
constexpr std::string_view kSummary = "[dedicated] summary: ";
constexpr std::string_view kMemoryLine = "[memory] ";
constexpr std::string_view kMemoryReport = " samples after the level was done: ";

std::vector<std::string> server_args() {
	return { "--level=" + std::string(kCaspian), "--mode=" + std::string(kConquestSmall),
		"--max-players=" + std::to_string(kSlots) };
}

std::string strip_cr(std::string p_line) {
	if (!p_line.empty() && p_line.back() == '\r') {
		p_line.pop_back();
	}
	return p_line;
}

}

TEST_SUITE("net-procs-memory-server") {
	TEST_CASE("the dedicated server on MP_007 ConquestSmall0 with all 64 slots bots stays within 0.4 GiB of private "
			  "bytes on every status line of 90 s of play") {
		ProcDriver driver(driver_config_from_env());
		const std::optional<Server> server = start_server(driver, kSlots, kServerSeconds, server_args());
		if (!server) {
			return;
		}
		std::string summary;
		CHECK_MESSAGE(driver.wait_line(
							  server->id, [](std::string_view l) { return contains(l, kSummary); },
							  std::chrono::seconds(kServerSeconds + 300), &summary),
				driver.output(server->id));
		const ChildEnd end = driver.finish(server->id, 60s);
		require_clean_exit(driver, server->id, end, "dedicated server");
		std::istringstream in(driver.output(server->id));
		std::string line;
		double worst_private = 0.0;
		double worst_peak = 0.0;
		uint32_t status_lines = 0;
		uint32_t full_lines = 0;
		while (std::getline(in, line)) {
			line = strip_cr(line);
			if (line.rfind(kStatusPrefix, 0) != 0) {
				continue;
			}
			++status_lines;
			worst_private = std::max(worst_private, number_after(line, "| private ").value_or(1.0e9));
			worst_peak = std::max(worst_peak, number_after(line, " MiB (peak ").value_or(1.0e9));
			full_lines += number_after(line, " bots ").value_or(0.0) == double(kSlots) ? 1u : 0u;
		}
		const double summary_peak = number_after(summary, " MiB (peak ").value_or(1.0e9);
		std::printf("net.procs.memory_server: %u status lines, %u with %u bots; worst private %.1f MiB, worst peak "
					"commit %.1f MiB, summary peak %.1f MiB, job peak %.1f MiB, cpu %.1f s (budget %.1f MiB)\n%s\n",
				status_lines, full_lines, kSlots, worst_private, worst_peak, summary_peak,
				double(end.peak_job_bytes) / (1024.0 * 1024.0), end.cpu_seconds, kServerBudgetMiB, summary.c_str());
		CHECK(full_lines >= 60);
		CHECK(contains(summary, "install unchanged"));
		CHECK(worst_private <= kServerBudgetMiB);
	}
}

TEST_SUITE("godot-memory-client") {
	TEST_CASE("a fullscreen game client on MP_007 ConquestSmall0 against a dedicated server with 63 bots, deployed "
			  "for 30 s: video memory at most 2.5 GiB and private memory, less the driver's write-combined mappings of "
			  "video memory, at most 1.5 GiB in play") {
		ProcDriver driver(driver_config_from_env());
		const auto client_spec = [&](uint16_t p_port) {
			return driver.godot_child("client", { "--fullscreen", "res://scenes/metro_tdm.tscn" },
					{ "--connect=127.0.0.1:" + std::to_string(p_port), "--auto-deploy",
							"--quit-after=" + std::to_string(kClientPlaySeconds), "--memory-report" });
		};
		if (const std::string why = driver.skip_reason(client_spec(1)); !why.empty()) {
			bf3::test::skip_install_case(why.c_str());
			return;
		}
		const std::optional<Server> server = start_server(driver, kSlots - 1, 900, server_args());
		if (!server) {
			return;
		}
		bf3::core::Result<size_t> client = driver.start(client_spec(server->port));
		if (!need(client.ok(), client.ok() ? std::string() : client.error().message)) {
			driver.finish(server->id, 1s);
			return;
		}
		std::string report;
		const bool reported = driver.wait_line(
				*client, [](std::string_view l) { return contains(l, kMemoryReport) || contains(l, " ended: "); }, 600s,
				&report);
		const ChildEnd end = driver.finish(*client, 60s);
		driver.finish(server->id, 1s);
		const std::string out = driver.output(*client);
		REQUIRE_MESSAGE(reported, out);
		require_clean_exit(driver, *client, end, "game client");
		report = strip_cr(report.substr(report.find(kMemoryLine)));
		const double video = number_after(report, "video local peak ").value_or(1.0e9);
		const double commit = number_after(report, "process peak commit ").value_or(1.0e9);
		const double play = number_after(report, "cpu private peak ").value_or(1.0e9);
		const double load = number_after(report, " MiB in play, ").value_or(1.0e9);
		const double working = number_after(report, "working set peak ").value_or(1.0e9);
		std::printf("godot.memory_client: video local peak %.1f MiB (budget %.1f); cpu private %.1f MiB in play "
					"(budget %.1f), %.1f MiB in the load; peak commit %.1f MiB, working set %.1f MiB, job peak %.1f "
					"MiB\n%s\n",
				video, kClientVideoBudgetMiB, play, kClientMemoryBudgetMiB, load, commit, working,
				double(end.peak_job_bytes) / (1024.0 * 1024.0), report.c_str());
		CHECK(contains(out, "[level] install unchanged: yes"));
		CHECK(number_after(report, "[memory] ").value_or(0.0) >= 60.0);
		CHECK(video > 0.0);
		CHECK(video <= kClientVideoBudgetMiB);
		CHECK(play > 0.0);
		CHECK(play <= kClientMemoryBudgetMiB);
	}
}
