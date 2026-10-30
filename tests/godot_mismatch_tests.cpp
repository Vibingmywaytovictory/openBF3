#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "test_support.h"
#include "win32_text.h"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using bf3::core::detail::path_utf8;
using bf3::test::procs::ChildEnd;
using bf3::test::procs::ChildSpec;
using bf3::test::procs::driver_config_from_env;
using bf3::test::procs::ProcDriver;

constexpr std::string_view kServerHostOn = "[dedicated] ServerHost on 127.0.0.1:";
constexpr std::string_view kKicked = "ended: kicked: install differs: motor";

std::string edited_motor_tuning() {
	std::ifstream in(OPENBF3_MOTOR_TUNING_CFG, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	std::string out;
	std::string line;
	bool edited = false;
	while (std::getline(text, line)) {
		if (line.rfind("gravity = ", 0) == 0) {
			line = "gravity = 9.5";
			edited = true;
		}
		out += line + "\n";
	}
	REQUIRE_MESSAGE(edited, OPENBF3_MOTOR_TUNING_CFG << " has no gravity line");
	return out;
}

}

TEST_SUITE("godot-mismatch") {
	TEST_CASE("godot.mismatch: a --connect client with an edited motor_tuning.cfg is kicked Motor by a dedicated "
			  "server") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		ProcDriver driver(driver_config_from_env());
		const ChildSpec server_spec = driver.godot_child("server", { "--headless", "--main-loop", "DedicatedMainLoop" },
				{ "--level=MP_Subway", "--mode=TeamDeathMatch0", "--bots=0", "--port=0", "--dedicated-seconds=600" });
		if (const std::string why = driver.skip_reason(server_spec); !why.empty()) {
			bf3::test::skip_install_case(why.c_str());
			return;
		}
		const std::filesystem::path tuning = driver.roots("client").dir / L"motor_tuning_edited.cfg";
		std::filesystem::create_directories(tuning.parent_path());
		{
			std::ofstream out(tuning, std::ios::binary);
			out << edited_motor_tuning();
			REQUIRE(out.good());
		}

		bf3::core::Result<size_t> server = driver.start(server_spec);
		REQUIRE_MESSAGE(server.ok(), (server.ok() ? std::string() : server.error().message));
		std::string line;
		REQUIRE_MESSAGE(
				driver.wait_line(
						*server, [](std::string_view l) { return l.rfind(kServerHostOn, 0) == 0; }, 300s, &line),
				driver.output(*server));
		uint32_t port = 0;
		const std::string_view digits = std::string_view(line).substr(kServerHostOn.size());
		std::from_chars(digits.data(), digits.data() + digits.size(), port);
		REQUIRE(port != 0);

		const ChildSpec client_spec = driver.godot_child("client", { "--headless", "res://scenes/metro_tdm.tscn" },
				{ "--connect=127.0.0.1:" + std::to_string(port), "--motor-tuning=" + path_utf8(tuning) });
		bf3::core::Result<size_t> client = driver.start(client_spec);
		REQUIRE_MESSAGE(client.ok(), (client.ok() ? std::string() : client.error().message));
		CHECK_MESSAGE(driver.wait_line(
							  *client, [](std::string_view l) { return l.find(kKicked) != std::string_view::npos; },
							  300s, &line),
				driver.output(*client));
		const ChildEnd client_end = driver.finish(*client, 60s);
		CHECK(client_end.exited);
		CHECK(client_end.code == 1);
		CHECK(client_end.stragglers == 0);
		const std::string client_out = driver.output(*client);
		CHECK(client_out.find("[level] motor tuning read from " + path_utf8(tuning)) != std::string::npos);
		CHECK(client_out.find("[level] install unchanged: yes") != std::string::npos);

		CHECK_MESSAGE(driver.wait_line(
							  *server, [](std::string_view l) { return l.find(" Motor=1 ") != std::string_view::npos; },
							  10s, &line),
				driver.output(*server));
		CHECK(line.find("Protocol=0 Build=0 Full=0 Source=0 Collision=0 Motor=1 ") != std::string::npos);
		MESSAGE("client: " << kKicked << "; server: " << line);
		const ChildEnd server_end = driver.finish(*server, 1s);
		CHECK(server_end.stragglers == 0);
		CHECK(driver.job_processes(*server) == 0);
		CHECK(bf3::core::diff_snapshots(m.before, m.reader.snapshot()).clean());
	}
}
