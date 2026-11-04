#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "net_procs_support.h"
#include "test_support.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bf3::test::procs::ChildEnd;
using bf3::test::procs::contains;
using bf3::test::procs::driver_config_from_env;
using bf3::test::procs::kicks_none;
using bf3::test::procs::kLevel;
using bf3::test::procs::kMode;
using bf3::test::procs::line_starts;
using bf3::test::procs::number_after;
using bf3::test::procs::ProcDriver;
using bf3::test::procs::require_clean_exit;
using bf3::test::procs::Server;
using bf3::test::procs::start_server;
using bf3::test::procs::start_tool;
using bf3::test::procs::summary_number;

constexpr double kTickP99Ms = 8.0;
constexpr double kPeerBytesPerSecond = 36000.0;
constexpr double kPrivateMiB = 0.4 * 1024.0;
constexpr uint32_t kLoadbotProcesses = 2;
constexpr double kSettleSeconds = 5.0;
constexpr std::string_view kStatusPrefix = "[dedicated] t=";
constexpr std::string_view kNetPrefix = "[dedicated] net: ";

struct Seat {
	double enet_out = 0.0;
	uint64_t intervals = 0;
	uint64_t over = 0;
	uint64_t max_bytes = 0;
};

struct Report {
	double t = 0.0;
	double p99_ms = 0.0;
	uint32_t clients = 0;
	double private_mib = 0.0;
	bool kicks_none = false;
	std::map<uint32_t, Seat> seats;
};

std::map<uint32_t, Seat> seats_of(std::string_view p_line) {
	std::map<uint32_t, Seat> seats;
	size_t at = 0;
	while ((at = p_line.find("slot ", at)) != std::string_view::npos) {
		const size_t end = std::min(p_line.find("; slot ", at + 5), p_line.size());
		const std::string_view one = p_line.substr(at, end - at);
		const std::optional<double> slot = number_after(one, "slot ");
		const std::optional<double> out = number_after(one, "enet out ");
		const std::optional<double> intervals = number_after(one, "intervals ");
		const std::optional<double> over = number_after(one, " over ");
		const std::optional<double> max = number_after(one, " max ");
		if (slot && out && intervals && over && max) {
			seats[uint32_t(*slot)] = Seat{ *out, uint64_t(*intervals), uint64_t(*over), uint64_t(*max) };
		}
		at = end;
	}
	return seats;
}

std::vector<Report> server_reports(const std::string &p_output) {
	std::vector<Report> reports;
	std::istringstream in(p_output);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind(kStatusPrefix, 0) == 0) {
			Report r;
			r.t = number_after(line, kStatusPrefix).value_or(-1.0);
			r.p99_ms = number_after(line, ") p99 ").value_or(1.0e9);
			r.clients = uint32_t(number_after(line, " clients ").value_or(0.0));
			r.private_mib = number_after(line, "| private ").value_or(1.0e9);
			r.kicks_none = kicks_none(line);
			reports.push_back(std::move(r));
		} else if (line.rfind(kNetPrefix, 0) == 0 && !reports.empty()) {
			reports.back().seats = seats_of(line);
		}
	}
	return reports;
}

struct Window {
	std::vector<Report> reports;
	double start_t = 0.0;
};

std::optional<Window> measure_window(const std::vector<Report> &p_reports, uint32_t p_connections, double p_seconds) {
	const auto seated = std::find_if(p_reports.begin(), p_reports.end(),
			[&](const Report &r) { return r.clients == p_connections && r.seats.size() == p_connections; });
	if (seated == p_reports.end()) {
		return std::nullopt;
	}
	Window w;
	w.start_t = seated->t + kSettleSeconds;
	for (const Report &r : p_reports) {
		if (r.t >= w.start_t && r.t <= w.start_t + p_seconds) {
			w.reports.push_back(r);
		}
	}
	return w;
}

struct Scale {
	std::string server_output;
	std::vector<std::string> fleets;
	std::string server_summary;
	std::optional<Window> window;
};

std::optional<Scale> run_scale(uint32_t p_connections, double p_seconds, bool p_ghost_scope) {
	ProcDriver driver(driver_config_from_env());
	const uint32_t fleet_seconds = uint32_t(p_seconds + 40.0);
	std::vector<std::string> server_args = { "--level=" + std::string(kLevel), "--mode=" + std::string(kMode),
		"--max-players=64", "--kill-limit=100000" };
	if (!p_ghost_scope) {
		server_args.push_back("--ghost-scope=off");
	}
	const std::optional<Server> server = start_server(driver, 0, fleet_seconds + 30u, server_args);
	if (!server) {
		return std::nullopt;
	}
	std::vector<size_t> fleets;
	for (uint32_t i = 0; i < kLoadbotProcesses; ++i) {
		const std::string role = "loadbots" + std::to_string(i);
		const std::optional<size_t> id = start_tool(driver, role,
				{ "loadbots", "127.0.0.1:" + std::to_string(server->port), std::string(kLevel), std::string(kMode),
						"--count=" + std::to_string(p_connections / kLoadbotProcesses), "--script=walkfire",
						"--seconds=" + std::to_string(fleet_seconds), "--name=f" + std::to_string(i) + "b",
						"--seed=" + std::to_string(i + 1) });
		if (!id) {
			return std::nullopt;
		}
		fleets.push_back(*id);
	}
	Scale out;
	for (const size_t f : fleets) {
		CHECK_MESSAGE(line_starts(driver, f, "loadbots: summary", std::chrono::seconds(fleet_seconds + 120)),
				driver.output(f));
		const ChildEnd end = driver.finish(f, 60s);
		require_clean_exit(driver, f, end, "loadbots");
		out.fleets.push_back(driver.output(f));
	}
	CHECK(driver.wait_line(
			server->id, [](std::string_view l) { return contains(l, "[dedicated] summary: "); }, 180s,
			&out.server_summary));
	const ChildEnd server_end = driver.finish(server->id, 60s);
	require_clean_exit(driver, server->id, server_end, "dedicated server");
	out.server_output = driver.output(server->id);
	out.window = measure_window(server_reports(out.server_output), p_connections, p_seconds);
	std::printf("net.procs.scale%u: server cpu %.1f s, peak job %.1f MiB; %s\n", p_connections, server_end.cpu_seconds,
			double(server_end.peak_job_bytes) / (1024.0 * 1024.0), out.server_summary.c_str());
	return out;
}

std::string fleet_summary(const std::string &p_output) {
	std::istringstream in(p_output);
	std::string line;
	while (std::getline(in, line)) {
		if (line.rfind("loadbots: summary", 0) == 0) {
			return line;
		}
	}
	return {};
}

uint64_t fleet_shots(const std::string &p_output) {
	std::istringstream in(p_output);
	std::string line;
	uint64_t shots = 0;
	while (std::getline(in, line)) {
		if (line.rfind("loadbot ", 0) == 0) {
			const uint64_t n = summary_number(line, ", shots ");
			shots += n == UINT64_MAX ? 0u : n;
		}
	}
	return shots;
}

struct Gate {
	double worst_p99 = 0.0;
	double worst_mean = 0.0;
	uint64_t over = 0;
	double worst_private = 0.0;
	uint32_t reports = 0;
	bool clients_held = true;
	bool kicks_none = true;
	std::map<uint32_t, double> mean_out;
	std::string over_at;
};

Gate gate_of(const Window &p_window, uint32_t p_connections) {
	Gate g;
	std::map<uint32_t, double> sum;
	std::map<uint32_t, uint32_t> count;
	const Report *previous = nullptr;
	for (const Report &r : p_window.reports) {
		for (const auto &[slot, seat] : r.seats) {
			const auto was = previous != nullptr ? previous->seats.find(slot) : r.seats.end();
			if (previous != nullptr && was != previous->seats.end() && seat.over > was->second.over) {
				g.over_at += "t=" + std::to_string(int(r.t)) + " s slot " + std::to_string(slot) + " +" +
						std::to_string(seat.over - was->second.over) + " (max " + std::to_string(seat.max_bytes) +
						" B); ";
			}
		}
		previous = &r;
		++g.reports;
		g.worst_p99 = std::max(g.worst_p99, r.p99_ms);
		g.worst_private = std::max(g.worst_private, r.private_mib);
		g.clients_held = g.clients_held && r.clients == p_connections && r.seats.size() == p_connections;
		g.kicks_none = g.kicks_none && r.kicks_none;
		for (const auto &[slot, seat] : r.seats) {
			sum[slot] += seat.enet_out;
			++count[slot];
		}
	}
	for (const auto &[slot, total] : sum) {
		g.mean_out[slot] = total / double(count[slot]);
		g.worst_mean = std::max(g.worst_mean, g.mean_out[slot]);
	}
	if (!p_window.reports.empty()) {
		for (const auto &[slot, last] : p_window.reports.back().seats) {
			const auto first = p_window.reports.front().seats.find(slot);
			g.over += first == p_window.reports.front().seats.end() ? last.over : last.over - first->second.over;
		}
	}
	return g;
}

void check_scale(uint32_t p_connections, double p_seconds) {
	const std::optional<Scale> s = run_scale(p_connections, p_seconds, true);
	if (!s) {
		return;
	}
	for (const std::string &f : s->fleets) {
		const std::string summary = fleet_summary(f);
		CHECK_MESSAGE(contains(summary, "decode errors 0, disconnected 0, refused 0"), summary);
		CHECK_MESSAGE(contains(summary, "stalled 0, kicked 0"), summary);
		CHECK(summary_number(summary, "spawned ") == p_connections / kLoadbotProcesses);
		CHECK(fleet_shots(f) > 0);
	}
	REQUIRE_MESSAGE(
			s->window.has_value(), "the server never reported ", p_connections, " seated clients:\n", s->server_output);
	const Gate g = gate_of(*s->window, p_connections);
	std::printf("net.procs.scale%u: %u reports from t=%.1f s: worst 10 s tick p99 %.3f ms, worst client mean %.0f B/s, "
				"intervals over 1,200 B %llu, worst private %.1f MiB, clients held %s, kicks %s\n",
			p_connections, g.reports, s->window->start_t, g.worst_p99, g.worst_mean, (unsigned long long)g.over,
			g.worst_private, g.clients_held ? "yes" : "no", g.kicks_none ? "none" : "some");
	std::fflush(stdout);
	CHECK(double(g.reports) >= p_seconds - 2.0);
	CHECK(g.clients_held);
	CHECK(g.kicks_none);
	CHECK(g.worst_p99 < kTickP99Ms);
	CHECK(g.mean_out.size() == p_connections);
	for (const auto &[slot, mean] : g.mean_out) {
		CHECK_MESSAGE(mean <= kPeerBytesPerSecond, "slot ", slot, ": ", mean, " B/s");
	}
	CHECK_MESSAGE(g.over == 0, g.over_at);
	CHECK(g.worst_private <= kPrivateMiB);
	CHECK(contains(s->server_summary, "install unchanged"));
}

}

TEST_SUITE("net-procs-scale32") {
	TEST_CASE("32 real connections from two loadbots processes walking and firing for 120 s on the dedicated Metro TDM "
			  "server: tick p99 < 8 ms, every client within 36 KB/s, no interval over 1,200 B, no kick") {
		check_scale(32, 120.0);
	}
}

TEST_SUITE("net-procs-scale64") {
	TEST_CASE("64 real connections from two loadbots processes walking and firing for 120 s on the dedicated Metro TDM "
			  "server: tick p99 < 8 ms, every client within 36 KB/s, no interval over 1,200 B, no kick") {
		check_scale(64, 120.0);
	}
}

TEST_SUITE("net-procs-scale-control") {
	TEST_CASE("the control: 64 connections to a server with --ghost-scope=off exceed 36 KB/s per client") {
		const std::optional<Scale> s = run_scale(64, 30.0, false);
		if (!s) {
			return;
		}
		CHECK(contains(s->server_output, "ghost scope off (--ghost-scope=off)"));
		REQUIRE_MESSAGE(s->window.has_value(), s->server_output);
		const Gate g = gate_of(*s->window, 64);
		double sum = 0.0;
		for (const auto &[slot, mean] : g.mean_out) {
			sum += mean;
		}
		const double mean = g.mean_out.empty() ? 0.0 : sum / double(g.mean_out.size());
		std::printf("net.procs.scale_control: mean client %.0f B/s, worst %.0f B/s, intervals over 1,200 B %llu\n",
				mean, g.worst_mean, (unsigned long long)g.over);
		CHECK(mean > kPeerBytesPerSecond);
		CHECK(g.over > 0);
	}
}
