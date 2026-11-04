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
