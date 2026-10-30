#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "test_support.h"

#include "bf3/core/util/elapsed.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using bf3::test::procs::ChildEnd;
using bf3::test::procs::ChildSpec;
using bf3::test::procs::contains;
using bf3::test::procs::driver_config_from_env;
using bf3::test::procs::kKilledExitCode;
using bf3::test::procs::ProcDriver;

constexpr std::string_view kGrandchildPid = "grandchild pid=";

std::filesystem::path self_exe() {
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (n < path.size()) {
			path.resize(n);
			return path;
		}
		path.resize(path.size() * 2);
	}
}

std::vector<std::string> child_case(const char *p_case) {
	return { "--test-suite=net-procs-harness-children", std::string("--test-case=") + p_case, "--no-skip" };
}

uint32_t grandchild_pid(ProcDriver &r_driver, size_t p_child) {
	std::string line;
	if (!r_driver.wait_line(p_child, [](std::string_view l) { return l.rfind(kGrandchildPid, 0) == 0; }, 30s, &line)) {
		return 0;
	}
	uint32_t pid = 0;
	const std::string_view digits = std::string_view(line).substr(kGrandchildPid.size());
	std::from_chars(digits.data(), digits.data() + digits.size(), pid);
	return pid;
}

void announce_grandchild(const char *p_role, bool p_die) {
	ProcDriver driver(driver_config_from_env());
	bf3::core::Result<size_t> grand =
			driver.start(driver.tool_child(p_role, self_exe(), child_case("idle grandchild")));
	REQUIRE_MESSAGE(grand.ok(), (grand.ok() ? std::string() : grand.error().message));
	std::printf("%.*s%u\n", static_cast<int>(kGrandchildPid.size()), kGrandchildPid.data(), driver.pid(*grand));
	std::fflush(stdout);
	if (p_die) {
		TerminateProcess(GetCurrentProcess(), 3);
	}
	std::this_thread::sleep_for(120s);
	FAIL("the hung child was not killed at its timeout");
}

}

TEST_SUITE("net-procs-harness") {
	TEST_CASE("a child without its own roots is refused and nothing starts") {
		ProcDriver driver(driver_config_from_env());
		bf3::core::Result<size_t> elsewhere = driver.start(driver.tool_child(
				"refused", driver.config().bf3dump, { "prepare", "MP_Subway", "TDM", "--cache=C:/elsewhere" }));
		REQUIRE_FALSE(elsewhere.ok());
		CHECK(contains(elsewhere.error().message, "own cache root"));

		ChildSpec godot = driver.godot_child("refused", { "--headless", "--quit" }, {});
		godot.args.pop_back();
		bf3::core::Result<size_t> no_settings = driver.start(godot);
		REQUIRE_FALSE(no_settings.ok());
		CHECK(contains(no_settings.error().message, "--settings-file"));

		ChildSpec shared = driver.godot_child("refused", { "--headless", "--quit" }, { "--bf3-cache=user://bf3cache" });
		bf3::core::Result<size_t> user_cache = driver.start(shared);
		REQUIRE_FALSE(user_cache.ok());
		CHECK(contains(user_cache.error().message, "user://"));

		bf3::core::Result<size_t> window = driver.start(driver.godot_child("refused", { "--quit" }, {}));
		REQUIRE_FALSE(window.ok());
		CHECK(contains(window.error().message, "window row"));
		CHECK_FALSE(std::filesystem::exists(driver.roots("refused").dir));
	}

	TEST_CASE("a bf3dump child and a headless Godot child exit 0 with their own roots") {
		ProcDriver driver(driver_config_from_env());
		const ChildSpec godot_spec = driver.godot_child("godot", { "--headless", "--quit" }, {});
		if (const std::string why = driver.skip_reason(godot_spec); !why.empty()) {
			bf3::test::skip_install_case(why.c_str());
			return;
		}
		const auto t0 = std::chrono::steady_clock::now();
		bf3::core::Result<size_t> tool =
				driver.start(driver.tool_child("bf3dump", driver.config().bf3dump, { "numcheck" }));
		REQUIRE_MESSAGE(tool.ok(), (tool.ok() ? std::string() : tool.error().message));
		bf3::core::Result<size_t> godot = driver.start(godot_spec);
		REQUIRE_MESSAGE(godot.ok(), (godot.ok() ? std::string() : godot.error().message));

		CHECK(driver.wait_line(*tool, [](std::string_view l) { return l == "numcheck: PASS"; }, 30s));
		CHECK(driver.wait_line(*godot, [](std::string_view l) { return l.rfind("Godot Engine v", 0) == 0; }, 60s));
		const ChildEnd t = driver.finish(*tool, 30s);
		const ChildEnd g = driver.finish(*godot, 60s);
		CHECK(t.exited);
		CHECK(t.code == 0);
		CHECK(t.stragglers == 0);
		CHECK(g.exited);
		CHECK(g.code == 0);
		CHECK(g.stragglers == 0);
		CHECK(driver.job_processes(*tool) == 0);
		CHECK(driver.job_processes(*godot) == 0);
		const std::filesystem::path log = driver.roots("godot").log_file;
		CHECK(std::filesystem::exists(log));
		CHECK(std::filesystem::is_directory(driver.roots("godot").cache));
		CHECK_FALSE(contains(driver.output(*godot), "user://settings.cfg"));
		MESSAGE("bf3dump cpu " << t.cpu_seconds << " s; godot cpu " << g.cpu_seconds << " s, peak job "
							   << (g.peak_job_bytes >> 20) << " MiB; both done in " << bf3::core::seconds_since(t0)
							   << " s");
		if (!g.exited || g.code != 0) {
			MESSAGE(driver.output(*godot));
		}
	}

	TEST_CASE("a dedicated server child runs the server project, which loads only the server extension") {
		ProcDriver driver(driver_config_from_env());
		const ChildSpec spec = driver.server_child("server", { "--port=none" });
		if (const std::string why = driver.skip_reason(spec); !why.empty()) {
			bf3::test::skip_install_case(why.c_str());
			return;
		}
		bf3::core::Result<size_t> server = driver.start(spec);
		REQUIRE_MESSAGE(server.ok(), (server.ok() ? std::string() : server.error().message));
		std::string extensions;
		CHECK(driver.wait_line(
				*server, [](std::string_view l) { return l.rfind("[dedicated] extensions ", 0) == 0; }, 60s,
				&extensions));
		CHECK(extensions == "[dedicated] extensions res://openbf3_server.gdextension, classes DedicatedMainLoop");
		CHECK(driver.wait_line(
				*server, [](std::string_view l) { return l.rfind("[dedicated] failed: --port=none", 0) == 0; }, 60s));
		const ChildEnd end = driver.finish(*server, 60s);
		CHECK(end.exited);
		CHECK(end.stragglers == 0);
		CHECK(driver.job_processes(*server) == 0);
	}

	TEST_CASE("a hung child is killed at its timeout with its grandchild and leaves no process in its job") {
		ProcDriver driver(driver_config_from_env());
		bf3::core::Result<size_t> hung = driver.start(driver.tool_child("hung", self_exe(), child_case("hung child")));
		REQUIRE_MESSAGE(hung.ok(), (hung.ok() ? std::string() : hung.error().message));
		const uint32_t grand_pid = grandchild_pid(driver, *hung);
		REQUIRE(grand_pid != 0);
		HANDLE grand = OpenProcess(SYNCHRONIZE, FALSE, grand_pid);
		REQUIRE(grand != nullptr);
		const std::vector<uint32_t> in_job = driver.job_pids(*hung);
		CHECK(std::find(in_job.begin(), in_job.end(), driver.pid(*hung)) != in_job.end());
		CHECK(std::find(in_job.begin(), in_job.end(), grand_pid) != in_job.end());
		const auto t0 = std::chrono::steady_clock::now();
		const ChildEnd end = driver.finish(*hung, 1500ms);
		CHECK(bf3::core::seconds_since(t0) >= 1.48);
		CHECK(end.killed);
		CHECK_FALSE(end.exited);
		CHECK(end.code == kKilledExitCode);
		CHECK(WaitForSingleObject(grand, 5000) == WAIT_OBJECT_0);
		CHECK(driver.job_processes(*hung) == 0);
		CloseHandle(grand);
	}

	TEST_CASE("a child that dies takes the grandchild it started with it") {
		ProcDriver driver(driver_config_from_env());
		bf3::core::Result<size_t> dying =
				driver.start(driver.tool_child("dying", self_exe(), child_case("dying parent")));
		REQUIRE_MESSAGE(dying.ok(), (dying.ok() ? std::string() : dying.error().message));
		const uint32_t grand_pid = grandchild_pid(driver, *dying);
		REQUIRE(grand_pid != 0);
		HANDLE grand = OpenProcess(SYNCHRONIZE, FALSE, grand_pid);
		REQUIRE(grand != nullptr);
		const ChildEnd end = driver.finish(*dying, 30s);
		CHECK(end.exited);
		CHECK(end.code == 3);
		CHECK(end.stragglers == 0);
		CHECK(WaitForSingleObject(grand, 5000) == WAIT_OBJECT_0);
		CloseHandle(grand);
	}
}

TEST_SUITE("net-procs-harness-children" * doctest::skip()) {
	TEST_CASE("hung child") {
		announce_grandchild("hung_grandchild", false);
	}

	TEST_CASE("dying parent") {
		announce_grandchild("dying_grandchild", true);
	}

	TEST_CASE("idle grandchild") {
		std::this_thread::sleep_for(120s);
		FAIL("the grandchild outlived its parent's job");
	}
}
