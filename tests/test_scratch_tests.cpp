#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "net_procs_driver.h"
#include "test_scratch.h"
#include "test_support.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using bf3::test::ScratchDir;
using bf3::test::ScratchOwner;
using bf3::test::procs::ChildEnd;
using bf3::test::procs::driver_config_from_env;
using bf3::test::procs::ProcDriver;

constexpr std::string_view kHeldLine = "scratch held=";
constexpr std::string_view kCmakeLine = "cmake scratch=";
constexpr uint32_t kGonePid = 0xFFFFFFF0u;

fs::path self_exe() {
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

std::string pid_text(uint32_t p_pid) {
	return std::to_string(p_pid);
}

std::string own_pid() {
	return pid_text(GetCurrentProcessId());
}

void write_file(const fs::path &p_path, std::string_view p_text) {
	std::ofstream out(p_path, std::ios::binary);
	out.write(p_text.data(), std::streamsize(p_text.size()));
}

bool set_times(const fs::path &p_path, std::chrono::hours p_ago) {
	HANDLE h =
			CreateFileW(p_path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	FILETIME now{};
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER t{};
	t.LowPart = now.dwLowDateTime;
	t.HighPart = now.dwHighDateTime;
	t.QuadPart -= uint64_t(std::chrono::duration_cast<std::chrono::seconds>(p_ago).count()) * 10'000'000ull;
	FILETIME then{};
	then.dwLowDateTime = t.LowPart;
	then.dwHighDateTime = t.HighPart;
	const bool ok = SetFileTime(h, &then, nullptr, &then) != 0;
	CloseHandle(h);
	return ok;
}

fs::path make_dir(const fs::path &p_path) {
	fs::create_directories(p_path / "sub");
	write_file(p_path / "sub" / "file.bin", "data");
	return p_path;
}

bool contains_path(const std::vector<fs::path> &p_paths, const fs::path &p_path) {
	return std::find(p_paths.begin(), p_paths.end(), p_path) != p_paths.end();
}

std::vector<std::string> child_case(const char *p_case) {
	return { "--test-suite=test-scratch-children", std::string("--test-case=") + p_case, "--no-skip" };
}

fs::path line_path(std::string_view p_line, std::string_view p_prefix) {
	const std::string_view text = p_line.substr(p_prefix.size());
	return fs::path(std::u8string(text.begin(), text.end()));
}

std::string utf8(const fs::path &p_path) {
	const std::u8string s = p_path.u8string();
	return std::string(s.begin(), s.end());
}

fs::path wait_path(ProcDriver &r_driver, size_t p_child, std::string_view p_prefix) {
	std::string line;
	if (!r_driver.wait_line(p_child, [&](std::string_view l) { return l.rfind(p_prefix, 0) == 0; }, 60s, &line)) {
		return {};
	}
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
		line.pop_back();
	}
	return line_path(line, p_prefix);
}

fs::path move_into(const fs::path &p_entry, const fs::path &p_root) {
	const fs::path target = p_root / p_entry.filename();
	REQUIRE(MoveFileExW(p_entry.c_str(), target.c_str(), 0));
	return target;
}

struct SweepHold {
	HANDLE lock = INVALID_HANDLE_VALUE;
	SweepHold() {
		const fs::path root = bf3::test::scratch_root();
		REQUIRE(bf3::test::wait_until(
				[&] {
					std::error_code ec;
					fs::create_directories(root, ec);
					lock = CreateFileW((root / ".sweep.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
							OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
					return lock != INVALID_HANDLE_VALUE;
				},
				10s, 20ms));
	}
	~SweepHold() {
		if (lock != INVALID_HANDLE_VALUE) {
			CloseHandle(lock);
		}
	}
	SweepHold(const SweepHold &) = delete;
	SweepHold &operator=(const SweepHold &) = delete;
};

struct OpenUntilExit {
	HANDLE file = INVALID_HANDLE_VALUE;
	~OpenUntilExit() {
		if (file != INVALID_HANDLE_VALUE) {
			CloseHandle(file);
		}
	}
};

void announce_held(const char *p_name) {
	const fs::path held = bf3::test::process_scratch(p_name);
	make_dir(held);
	static OpenUntilExit open;
	open.file = CreateFileW((held / "sub" / "file.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(open.file != INVALID_HANDLE_VALUE);
	std::printf("%.*s%s\n", int(kHeldLine.size()), kHeldLine.data(), utf8(held).c_str());
	std::fflush(stdout);
}

}

TEST_SUITE("test-scratch") {
	TEST_CASE("a ScratchDir is <temp>\\openbf3-tests\\<name>-<pid>-<n>, unique, and removed with its contents at scope "
			  "end") {
		fs::path first;
		fs::path second;
		{
			ScratchDir a("unit-scope");
			ScratchDir b("unit-scope");
			first = a.path();
			second = b.path();
			CHECK(first.parent_path() == bf3::test::scratch_root());
			CHECK(bf3::test::scratch_root() == bf3::test::temp_dir() / "openbf3-tests");
			CHECK(utf8(first.filename()).rfind("unit-scope-" + own_pid() + "-", 0) == 0);
			CHECK(first != second);
			CHECK(fs::is_directory(first));
			make_dir(first);
			ScratchDir moved = std::move(b);
			CHECK(moved.path() == second);
			CHECK(b.path().empty());
			CHECK(fs::is_directory(second));
		}
		CHECK_FALSE(fs::exists(first));
		CHECK_FALSE(fs::exists(second));
		const fs::path held = bf3::test::process_scratch("unit-held");
		CHECK(fs::is_directory(held));
		CHECK(bf3::test::scratch_owner(held) == ScratchOwner::Alive);
	}

	TEST_CASE("a folder's owner is alive for a running process that started before it, dead for a gone pid or a pid "
			  "that started after it, and a CMake folder is alive while its lock is held") {
		ScratchDir fake("unit-owner");
		const fs::path root = fake.path() / "openbf3-tests";
		const fs::path live = make_dir(root / ("live-" + own_pid() + "-0"));
		const fs::path gone = make_dir(root / ("gone-" + pid_text(kGonePid) + "-0"));
		const fs::path reused = make_dir(root / ("reused-" + own_pid() + "-1"));
		REQUIRE(set_times(reused, 24h * 30));
		CHECK(bf3::test::scratch_owner(live) == ScratchOwner::Alive);
		CHECK(bf3::test::scratch_owner(gone) == ScratchOwner::Dead);
		CHECK(bf3::test::scratch_owner(reused) == ScratchOwner::Dead);
		CHECK(bf3::test::scratch_owner(make_dir(root / "no-owner")) == ScratchOwner::Unknown);
		CHECK(bf3::test::scratch_owner(make_dir(root / "x-cmake-12345678-")) == ScratchOwner::Unknown);

		const fs::path cmake_dir = make_dir(root / "godot-m1-cmake-0123abcd");
		CHECK(bf3::test::scratch_owner(cmake_dir) == ScratchOwner::Dead);
		const fs::path lock = root / "godot-m1-cmake-0123abcd.lock";
		write_file(lock, "");
		HANDLE h = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		REQUIRE(h != INVALID_HANDLE_VALUE);
		OVERLAPPED at{};
		REQUIRE(LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &at));
		CHECK(bf3::test::scratch_owner(cmake_dir) == ScratchOwner::Alive);
		CHECK(bf3::test::scratch_owner(lock) == ScratchOwner::Alive);
		UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &at);
		CloseHandle(h);
		CHECK(bf3::test::scratch_owner(cmake_dir) == ScratchOwner::Dead);
		CHECK(bf3::test::scratch_owner(lock) == ScratchOwner::Dead);
	}

	TEST_CASE(
			"a sweep removes dead owners' folders and legacy leftovers older than 6 hours, keeps live and young ones, "
			"and only one process sweeps at a time") {
		ScratchDir fake("unit-sweep");
		const fs::path temp = fake.path();
		const fs::path root = temp / "openbf3-tests";
		const fs::path live = make_dir(root / ("live-" + own_pid() + "-0"));
		const fs::path gone = make_dir(root / ("gone-" + pid_text(kGonePid) + "-3"));
		const fs::path reused = make_dir(root / ("reused-" + own_pid() + "-1"));
		REQUIRE(set_times(reused, 24h * 30));
		const fs::path trash = make_dir(root / (".trash-" + pid_text(kGonePid) + "-7"));
		const fs::path cmake_dead = make_dir(root / "m1-prepare-cmake-89abcdef");
		write_file(root / "m1-prepare-cmake-89abcdef.lock", "");
		const fs::path orphan_lock = root / "tc-cost-cmake-00112233.lock";
		write_file(orphan_lock, "");
		const fs::path unknown_old = make_dir(root / "unnamed-old");
		REQUIRE(set_times(unknown_old, 7h));
		const fs::path unknown_young = make_dir(root / "unnamed-young");
		const fs::path legacy_old = make_dir(temp / "openbf3-cl-316962746298100");
		REQUIRE(set_times(legacy_old, 7h));
		const fs::path legacy_old_underscore = make_dir(temp / "openbf3_host_gunplay_perf_17528");
		REQUIRE(set_times(legacy_old_underscore, 7h));
		const fs::path legacy_cmake_old = make_dir(temp / "o3p-0a1b2c3d");
		REQUIRE(set_times(legacy_cmake_old, 7h));
		const fs::path legacy_tick_old = make_dir(temp / "o3t-17528-320331515");
		REQUIRE(set_times(legacy_tick_old, 7h));
		const fs::path legacy_young = make_dir(temp / "openbf3-ai-script-306298858428200");
		const fs::path other_old = make_dir(temp / "o3p-not-ours");
		REQUIRE(set_times(other_old, 7h));
		const fs::path not_ours = make_dir(temp / "someone-else");
		REQUIRE(set_times(not_ours, 7h));

		{
			HANDLE busy = CreateFileW((root / ".sweep.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
					OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			REQUIRE(busy != INVALID_HANDLE_VALUE);
			const bf3::test::SweepReport skipped = bf3::test::sweep_scratch(temp, bf3::test::kLegacyScratchAge, 10s);
			CHECK_FALSE(skipped.ran);
			CHECK(fs::exists(gone));
			CloseHandle(busy);
		}

		const bf3::test::SweepReport r = bf3::test::sweep_scratch(temp, bf3::test::kLegacyScratchAge, 10s);
		CHECK(r.ran);
		for (const fs::path &p : { gone, reused, trash, cmake_dead, unknown_old, legacy_old, legacy_old_underscore,
					 legacy_cmake_old, legacy_tick_old }) {
			INFO(utf8(p));
			CHECK_FALSE(fs::exists(p));
			CHECK(contains_path(r.removed, p));
		}
		CHECK_FALSE(fs::exists(root / "m1-prepare-cmake-89abcdef.lock"));
		CHECK_FALSE(fs::exists(orphan_lock));
		for (const fs::path &p : { live, unknown_young, legacy_young, other_old, not_ours }) {
			INFO(utf8(p));
			CHECK(fs::exists(p));
			CHECK_FALSE(contains_path(r.removed, p));
		}
		CHECK(contains_path(r.kept, live));
		CHECK_FALSE(fs::exists(root / ".sweep.lock"));
		std::vector<fs::path> left;
		for (const fs::directory_entry &e : fs::directory_iterator(root)) {
			left.push_back(e.path());
		}
		CHECK(left.size() == 2);
	}

	TEST_CASE("a sweep stops claiming folders when its time budget is spent and leaves the rest for the next one") {
		ScratchDir fake("unit-budget");
		const fs::path root = fake.path() / "openbf3-tests";
		const fs::path gone = make_dir(root / ("gone-" + pid_text(kGonePid) + "-1"));
		const bf3::test::SweepReport spent = bf3::test::sweep_scratch(fake.path(), bf3::test::kLegacyScratchAge, -1ms);
		CHECK(spent.ran);
		CHECK(spent.removed.empty());
		CHECK(contains_path(spent.kept, gone));
		CHECK(fs::exists(gone));
		const bf3::test::SweepReport next = bf3::test::sweep_scratch(fake.path(), bf3::test::kLegacyScratchAge, 10s);
		CHECK(contains_path(next.removed, gone));
		CHECK_FALSE(fs::exists(gone));
	}
}

TEST_SUITE("test-scratch-procs") {
	TEST_CASE("a process-held folder is removed at normal exit, after statics that kept a file open in it") {
		ProcDriver driver(driver_config_from_env());
		bf3::core::Result<size_t> child =
				driver.start(driver.tool_child("held", self_exe(), child_case("held until exit")));
		REQUIRE_MESSAGE(child.ok(), (child.ok() ? std::string() : child.error().message));
		const fs::path held = wait_path(driver, *child, kHeldLine);
		REQUIRE_FALSE(held.empty());
		const ChildEnd end = driver.finish(*child, 30s);
		CHECK(end.exited);
		CHECK(end.code == 0);
		INFO(driver.output(*child));
		CHECK(held.parent_path() == bf3::test::scratch_root());
		CHECK_FALSE(fs::exists(held));
	}

	TEST_CASE("a crashed process leaves its folder, which reads dead and is removed by the next sweep") {
		ProcDriver driver(driver_config_from_env());
		bf3::core::Result<size_t> child =
				driver.start(driver.tool_child("crash", self_exe(), child_case("held then crash")));
		REQUIRE_MESSAGE(child.ok(), (child.ok() ? std::string() : child.error().message));
		const fs::path held = wait_path(driver, *child, kHeldLine);
		REQUIRE_FALSE(held.empty());
		ScratchDir fake("procs-crash");
		fs::create_directories(fake.path() / "openbf3-tests");
		SweepHold no_other_sweep;
		const ChildEnd end = driver.finish(*child, 30s);
		CHECK(end.exited);
		CHECK(end.code == 3);
		REQUIRE(fs::is_directory(held));
		CHECK(bf3::test::scratch_owner(held) == ScratchOwner::Dead);
		const fs::path moved = move_into(held, fake.path() / "openbf3-tests");
		const bf3::test::SweepReport r = bf3::test::sweep_scratch(fake.path(), bf3::test::kLegacyScratchAge, 10s);
		CHECK(contains_path(r.removed, moved));
		CHECK_FALSE(fs::exists(moved));
	}

	TEST_CASE("a CMake script's folder reads alive while the script runs, dead once it is killed, and a finished "
			  "script removes its own") {
		ProcDriver driver(driver_config_from_env());
		bf3::core::Result<size_t> hold = driver.start(driver.tool_child(
				"cmake_hold", OPENBF3_CMAKE_COMMAND, { "-DMODE=hold", "-P", OPENBF3_SCRATCH_CHILD_CMAKE }));
		REQUIRE_MESSAGE(hold.ok(), (hold.ok() ? std::string() : hold.error().message));
		const fs::path dir = wait_path(driver, *hold, kCmakeLine);
		REQUIRE_FALSE(dir.empty());
		CHECK(fs::equivalent(dir.parent_path(), bf3::test::scratch_root()));
		CHECK(utf8(dir.filename()).rfind("scratch-child-cmake-", 0) == 0);
		CHECK(bf3::test::scratch_owner(dir) == ScratchOwner::Alive);
		ScratchDir fake("procs-cmake");
		fs::create_directories(fake.path() / "openbf3-tests");
		SweepHold no_other_sweep;
		const ChildEnd killed = driver.finish(*hold, 500ms);
		CHECK(killed.killed);
		CHECK(bf3::test::wait_until([&] { return bf3::test::scratch_owner(dir) == ScratchOwner::Dead; }, 10s, 20ms));
		CHECK(fs::is_directory(dir));
		fs::path lock = dir;
		lock += ".lock";
		const fs::path moved = move_into(dir, fake.path() / "openbf3-tests");
		const fs::path moved_lock = move_into(lock, fake.path() / "openbf3-tests");
		const bf3::test::SweepReport r = bf3::test::sweep_scratch(fake.path(), bf3::test::kLegacyScratchAge, 10s);
		CHECK(contains_path(r.removed, moved));
		CHECK_FALSE(fs::exists(moved));
		CHECK_FALSE(fs::exists(moved_lock));

		bf3::core::Result<size_t> clean = driver.start(driver.tool_child(
				"cmake_clean", OPENBF3_CMAKE_COMMAND, { "-DMODE=clean", "-P", OPENBF3_SCRATCH_CHILD_CMAKE }));
		REQUIRE_MESSAGE(clean.ok(), (clean.ok() ? std::string() : clean.error().message));
		const fs::path cleaned = wait_path(driver, *clean, kCmakeLine);
		REQUIRE_FALSE(cleaned.empty());
		const ChildEnd done = driver.finish(*clean, 60s);
		CHECK(done.exited);
		CHECK(done.code == 0);
		fs::path cleaned_lock = cleaned;
		cleaned_lock += ".lock";
		CHECK_FALSE(fs::exists(cleaned));
		CHECK_FALSE(fs::exists(cleaned_lock));
	}
}

TEST_SUITE("test-scratch-children" * doctest::skip()) {
	TEST_CASE("held until exit") {
		announce_held("procs-exit");
	}

	TEST_CASE("held then crash") {
		announce_held("procs-crash");
		TerminateProcess(GetCurrentProcess(), 3);
	}
}
