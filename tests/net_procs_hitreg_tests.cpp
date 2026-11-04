#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "net_procs_support.h"
#include "test_support.h"
#include "win32_text.h"

#include <chrono>
#include <cstdint>
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

constexpr uint32_t kShots = 500;
constexpr uint64_t kMinHits = 100;
constexpr std::string_view kNetsim = "--netsim=lat=50,loss=1";

std::filesystem::path no_damage_config(ProcDriver &r_driver) {
	const std::filesystem::path from = r_driver.config().godot_project / L"config";
	const std::filesystem::path to = r_driver.roots("config").dir;
	std::error_code ec;
	std::filesystem::create_directories(to, ec);
	for (const char *name : { "motor_tuning.cfg", "tdm_tuning.cfg", "weapon_mechanics.cfg" }) {
		std::ifstream in(from / name, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		REQUIRE_MESSAGE(in.good(), path_utf8(from / name));
		std::string body = text.str();
		if (std::string_view(name) == "weapon_mechanics.cfg") {
			const std::string key = "bullet_damage_modifier = 1";
			const size_t at = body.find(key);
			REQUIRE_MESSAGE(at != std::string::npos, key);
			body.replace(at, key.size(), "bullet_damage_modifier = 0");
		}
		std::ofstream out(to / name, std::ios::binary);
		out << body;
		REQUIRE(out.good());
	}
	return to;
}

struct Duel {
	bool skipped = false;
	bool ran = false;
	uint64_t shots = 0;
	uint64_t predicted = 0;
	uint64_t confirmed = 0;
	uint64_t same_zone = 0;
	uint64_t unpredicted = 0;
	uint32_t shooter_code = 0;
	std::string line;
	std::string server_gunplay;
};

Duel run_duel(ProcDriver &r_driver, const std::filesystem::path &p_config, uint32_t p_rewind, std::string_view p_case) {
	Duel d;
	const std::string mechanics = path_utf8(p_config / L"weapon_mechanics.cfg");
	const std::optional<Server> server = start_server(r_driver, 0, 1500,
			{ "--level=" + std::string(kLevel), "--mode=" + std::string(kMode), "--dev", "--mechanics=" + mechanics,
					"--max-rewind-ticks=" + std::to_string(p_rewind) },
			"server");
	if (!server) {
		d.skipped = true;
		return d;
	}
	const std::string server_out = r_driver.output(server->id);
	const size_t gunplay = server_out.find("[dedicated] gunplay: ");
	CHECK_MESSAGE(gunplay != std::string::npos, server_out);
	if (gunplay != std::string::npos) {
		d.server_gunplay = server_out.substr(gunplay, server_out.find('\n', gunplay) - gunplay);
	}
	const std::optional<size_t> target = start_tool(r_driver, "target",
			client_args(r_driver, "target", server->port,
					{ "--script=strafe", "--seconds=1400", "--name=target", std::string(kNetsim),
							"--config-dir=" + path_utf8(p_config) }));
	if (!target || !need(line_starts(r_driver, *target, "client: spawned", 300s), r_driver.output(*target))) {
		r_driver.finish(server->id, 1s);
		return d;
	}
	std::vector<std::string> shooter_args = { std::string(kNetsim), "--hitreg-check=" + std::to_string(kShots),
		"--mechanics=" + mechanics };
	if (p_rewind == 0) {
		shooter_args.push_back("--own-test-rewind-ticks=21");
	}
	bf3::core::Result<size_t> shooter = r_driver.start(godot_client(r_driver, server->port, shooter_args, "shooter"));
	REQUIRE_MESSAGE(shooter.ok(), (shooter.ok() ? std::string() : shooter.error().message));
	const bool reported = r_driver.wait_line(
			*shooter,
			[](std::string_view l) {
				return contains(l, "[hitreg] shots ") || contains(l, "[hitreg] failed") || contains(l, " ended: ");
			},
			1200s, &d.line);
	CHECK_MESSAGE(reported, r_driver.output(*shooter));
	const ChildEnd shooter_end = r_driver.finish(*shooter, 60s);
	d.shooter_code = shooter_end.code;
	r_driver.finish(*target, 1s);
	r_driver.finish(server->id, 1s);
	MESSAGE(p_case << " shooter:\n"
				   << r_driver.output(*shooter) << "\n"
				   << p_case << " target:\n"
				   << r_driver.output(*target));
	if (!reported || !contains(d.line, "[hitreg] shots ")) {
		return d;
	}
	d.shots = summary_number(d.line, "[hitreg] shots ");
	d.predicted = summary_number(d.line, "predicted hits ");
	d.confirmed = summary_number(d.line, "confirmed ");
	d.same_zone = summary_number(d.line, "same zone ");
	d.unpredicted = summary_number(d.line, "unpredicted ");
	MESSAGE(p_case << ": " << d.line << "\n" << d.server_gunplay);
	d.ran = need(d.shots != UINT64_MAX && d.predicted != UINT64_MAX && d.confirmed != UINT64_MAX &&
					d.same_zone != UINT64_MAX && d.unpredicted != UINT64_MAX,
			d.line);
	return d;
}

}

TEST_SUITE("net-procs-hitreg") {
	TEST_CASE("a Godot --connect client shoots a strafing bf3dump client at 100 ms RTT and 1 % loss each: the "
			  "dedicated server confirms 99 % of its predicted hits, 99 % in the same zone; with lag compensation "
			  "off it confirms at most half") {
		ProcDriver driver(driver_config_from_env());
		const std::filesystem::path config = no_damage_config(driver);
		const Duel gate = run_duel(driver, config, 21, "gate");
		if (gate.skipped) {
			return;
		}
		REQUIRE(gate.ran);
		CHECK(gate.shooter_code == 0);
		CHECK(contains(gate.server_gunplay, "rewinds at most 21 ticks"));
		CHECK(gate.shots >= kShots);
		CHECK(gate.predicted >= kMinHits);
		CHECK(double(gate.confirmed) >= 0.99 * double(gate.predicted));
		CHECK(double(gate.same_zone) >= 0.99 * double(gate.confirmed));

		const Duel control = run_duel(driver, config, 0, "control");
		if (control.skipped) {
			return;
		}
		REQUIRE(control.ran);
		CHECK(control.shooter_code == 0);
		CHECK(contains(control.server_gunplay, "rewinds at most 0 ticks"));
		CHECK(control.shots >= kShots);
		CHECK(control.predicted >= kMinHits);
		CHECK(double(control.confirmed) <= 0.5 * double(control.predicted));
	}
}
