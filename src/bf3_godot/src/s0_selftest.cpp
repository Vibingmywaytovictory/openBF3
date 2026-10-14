#include "s0_selftest.h"

#include "game_session.h"
#include "godot_util.h"

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/sim/nav_link.h"
#include "bf3/sim/physics_world.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace bf3::godot_side {

using namespace godot;

namespace {

std::string fmt(const char *p_fmt, ...) {
	char buf[512];
	va_list args;
	va_start(args, p_fmt);
	vsnprintf(buf, sizeof(buf), p_fmt, args);
	va_end(args);
	return buf;
}

RigidBody3D *find_rigid_body(Node *p_node) {
	if (RigidBody3D *rb = Object::cast_to<RigidBody3D>(p_node)) {
		return rb;
	}
	for (int i = 0; i < p_node->get_child_count(); ++i) {
		if (RigidBody3D *rb = find_rigid_body(p_node->get_child(i))) {
			return rb;
		}
	}
	return nullptr;
}

} // namespace

S0SelfTest::S0SelfTest(GameSession &p_session, double p_seconds) :
		session_(p_session), seconds_(p_seconds > 0.5 ? p_seconds : 0.5) {}

void S0SelfTest::add(const std::string &p_check, Result p_result, const std::string &p_detail) {
	rows_.push_back({ p_check, p_result, p_detail });
	failed_ |= p_result == Result::Fail;
}

void S0SelfTest::begin() {
	UtilityFunctions::print(to_godot(fmt("[s0-selftest] start, running the server and Godot physics for %.1f s", seconds_)));

	// Engine and build.
	const Dictionary v = Engine::get_singleton()->get_version_info();
	const int major = int(v["major"]);
	const int minor = int(v["minor"]);
	const bool single = OS::get_singleton()->has_feature("single");
	add("engine.version", (major == 4 && minor == 8) ? Result::Pass : Result::Fail,
			engine_summary() + fmt(" | real_t %d B | debug build %s", int(sizeof(real_t)), OS::get_singleton()->is_debug_build() ? "yes" : "no"));
	add("engine.precision", (single && sizeof(real_t) == 4) ? Result::Pass : Result::Fail,
			single ? "engine single, extension single" : "engine/extension precision mismatch");
	const String engine_name = ProjectSettings::get_singleton()->get_setting("physics/3d/physics_engine");
	add("godot.physics_engine", to_std(engine_name) == "Jolt Physics" ? Result::Pass : Result::Fail, to_std(engine_name));

	// Install: located, layout, read-only reads, write guard.
	const bf3::core::InstallReader &install = session_.install();
	if (!install.status().located) {
		// No install at the default path: skip. An explicit override that points nowhere: fail.
		const bool explicit_path = install.status().source != "default";
		add("install.locate", explicit_path ? Result::Fail : Result::Skip, install.describe() + " (set OPENBF3_INSTALL or --bf3-install=)");
	} else {
		add("install.locate", install.is_mounted() ? Result::Pass : Result::Fail, install.describe());
		bf3::core::ReadOnlyFile f = install.open("Data/layout.toc");
		std::vector<uint8_t> bytes;
		const bool read_ok = f.is_open() && f.read_all(bytes) && bytes.size() >= 4;
		const bool magic_ok = read_ok && bytes[0] == 0x00 && bytes[1] == 0xD1 && bytes[2] == 0xCE && bytes[3] <= 0x01;
		add("install.read", magic_ok ? Result::Pass : Result::Fail,
				fmt("Data/layout.toc %llu B read via GENERIC_READ handle, header %02X %02X %02X %02X", (unsigned long long)bytes.size(),
						read_ok ? bytes[0] : 0, read_ok ? bytes[1] : 0, read_ok ? bytes[2] : 0, read_ok ? bytes[3] : 0));
		const bf3::core::PathGuard guard = install.write_guard();
		const std::filesystem::path root = install.root();
		int refused = 0;
		int tried = 0;
		auto expect_refused = [&](const std::filesystem::path &p) {
			tried++;
			refused += guard.allows_write(p) ? 0 : 1;
		};
		expect_refused(root / L"Data" / L"Win32" / L"new.cache");
		expect_refused(root / L"Update" / L"Patch" / L"Data" / L"x.bin");
		std::wstring fwd = root.native();
		for (wchar_t &c : fwd) {
			c = c == L'\\' ? L'/' : c;
		}
		expect_refused(fwd + L"/Data/x.bin");
		wchar_t shortp[MAX_PATH];
		const DWORD n = GetShortPathNameW(root.c_str(), shortp, MAX_PATH);
		if (n > 0 && n < MAX_PATH) {
			expect_refused(std::wstring(shortp) + L"\\Data\\x.bin");
		}
		const std::string cache = to_std(ProjectSettings::get_singleton()->globalize_path("user://bf3cache/probe.bin"));
		const bool cache_ok = guard.allows_write(std::filesystem::path(std::u8string(cache.begin(), cache.end())));
		add("install.write_guard", (refused == tried && cache_ok) ? Result::Pass : Result::Fail,
				fmt("%d/%d install aliases refused (incl. 8.3 + '/'), user://bf3cache allowed: %s", refused, tried, cache_ok ? "yes" : "no"));
		const bf3::core::InstallSnapshot &snap = session_.install_snapshot_at_start();
		add("install.snapshot_before", snap.valid ? Result::Pass : Result::Fail,
				fmt("%zu files (%zu toc, %zu sb, %zu cas.cat), %.1f GiB, scan %.1f ms", snap.files.size(), snap.toc_count, snap.sb_count,
						snap.cat_count, double(snap.total_bytes) / (1024.0 * 1024.0 * 1024.0), snap.scan_ms));
	}

	// Recast/Detour linked.
	const bf3::sim::NavLinkResult nav = bf3::sim::nav_link_check();
	add("recast.link", nav.ok ? Result::Pass : Result::Fail, fmt("heightfield %dx%d, %d spans, Detour alloc %s", nav.grid_w, nav.grid_h, nav.spans,
																	 nav.detour_alloc ? "ok" : "failed"));

	// Our Jolt, in the same process as Godot's Jolt: two worlds must agree bit for bit.
	{
		bf3::sim::WorldConfig cfg;
		bf3::sim::PhysicsWorld a;
		bf3::sim::PhysicsWorld b;
		cfg.job_threads = 2;
		a.init(cfg);
		cfg.job_threads = 0;
		b.init(cfg);
		for (int i = 0; i < 300; ++i) {
			a.step();
			b.step();
		}
		const bool same = a.state_hash() == b.state_hash();
		add("jolt.determinism", same ? Result::Pass : Result::Fail,
				fmt("own Jolt %s, 300 steps, 2 threads vs single-threaded: hash %016llx %s %016llx", bf3::sim::PhysicsWorld::jolt_version(),
						(unsigned long long)a.state_hash(), same ? "==" : "!=", (unsigned long long)b.state_hash()));
	}

	// ENet: two hosts of our upstream ENet in this process.
	const bf3::net::PingStats ping = bf3::net::loopback_ping(200);
	add("enet.loopback_ping", ping.ok ? Result::Pass : Result::Fail,
			ping.ok ? fmt("ENet %s, 127.0.0.1:%u, %d/%d pongs, RTT mean %.1f us, p50 %.1f us, p99 %.1f us, max %.1f us", bf3::net::enet_version().c_str(),
							  ping.port, ping.received, ping.sent, ping.mean_us, ping.p50_us, ping.p99_us, ping.max_us)
					: ping.error);

	// Godot's own physics: the scene's RigidBody3D.
	if (SceneTree *tree = session_.get_tree()) {
		Node *scene = tree->get_current_scene();
		body_ = find_rigid_body(scene ? scene : static_cast<Node *>(tree->get_root()));
	}
	if (body_ != nullptr) {
		body_y0_ = body_->get_global_position().y;
	}
	const bf3::host::ServerStats s = session_.server_stats();
	server_ticks_at_begin_ = s.ticks;
	server_hash_at_begin_ = s.state_hash;
	window_start_ns_ = bf3::net::monotonic_ns();
}

void S0SelfTest::physics_tick() {
	physics_ticks_++;
	if (body_ != nullptr) {
		body_min_y_ = std::min(body_min_y_, body_->get_global_position().y);
	}
}

bool S0SelfTest::process(double p_delta) {
	if (finished_) {
		return true;
	}
	elapsed_ += p_delta;
	frames_++;
	if (elapsed_ < seconds_) {
		return false;
	}
	finish();
	finished_ = true;
	return true;
}

void S0SelfTest::finish() {
	const bf3::host::ServerStats s = session_.server_stats();
	const bf3::host::LoopbackClientStats c = session_.client_stats();
	const uint64_t ticks = s.ticks - server_ticks_at_begin_;
	const double window_s = double(bf3::net::monotonic_ns() - window_start_ns_) / 1e9; // wall clock, not frame deltas
	const double expected = 60.0 * window_s;
	const bool rate_ok = s.running && std::fabs(double(ticks) - expected) <= 0.05 * expected + 1.0 && s.step_errors == 0;
	add("server.tick_rate", rate_ok ? Result::Pass : Result::Fail,
			fmt("std::thread ServerHost: %llu ticks in %.3f s wall (expected %.1f), %.2f Hz since start, overruns %llu, step errors %llu",
					(unsigned long long)ticks, window_s, expected, s.measured_hz, (unsigned long long)s.overruns, (unsigned long long)s.step_errors));
	add("server.step_time", s.p99_step_ms < 5.0 ? Result::Pass : Result::Fail,
			fmt("Jolt step last %.3f ms, p99 %.3f ms, max %.3f ms; whole tick %.3f ms", s.last_step_ms, s.p99_step_ms, s.max_step_ms, s.last_tick_ms));
	const bool settled = s.lowest_body_y > 0.3f && s.lowest_body_y < 0.7f && s.state_hash != server_hash_at_begin_;
	add("server.jolt_world", settled ? Result::Pass : Result::Fail,
			fmt("%d dynamic bodies fell onto the ground: lowest y %.3f m, %d still awake, state hash changed", s.dynamic_bodies, s.lowest_body_y,
					s.active_bodies));
	add("server.enet_loopback", (c.connected && c.pongs_received > 0 && s.peers >= 1) ? Result::Pass : Result::Fail,
			fmt("LoopbackClient thread -> ServerHost: %llu/%llu pongs, RTT last %.2f ms, mean %.2f ms, max %.2f ms (server echoes once per tick), ENet RTT %u ms",
					(unsigned long long)c.pongs_received, (unsigned long long)c.pings_sent, c.last_rtt_ms, c.mean_rtt_ms, c.max_rtt_ms, c.enet_rtt_ms));

	if (body_ == nullptr) {
		add("godot.rigidbody", Result::Fail, "no RigidBody3D in the scene");
	} else {
		const float y1 = body_->get_global_position().y;
		const float v = body_->get_linear_velocity().length();
		const bool fell = body_y0_ - y1 > 1.0f && v < 0.2f;
		add("godot.rigidbody", fell ? Result::Pass : Result::Fail,
				fmt("%s (Godot Jolt): y %.2f -> %.2f m, |v| %.3f m/s, sleeping %s, %llu physics ticks", to_std(String(body_->get_name())).c_str(),
						body_y0_, y1, v, body_->is_sleeping() ? "yes" : "no", (unsigned long long)physics_ticks_));
	}
	const bool both = rate_ok && body_ != nullptr && physics_ticks_ > static_cast<uint64_t>(0.8 * 60.0 * elapsed_);
	add("jolt.coexistence", both ? Result::Pass : Result::Fail,
			fmt("own Jolt %s (server thread, %llu ticks) and Godot's Jolt (main thread, %llu physics ticks) stepped concurrently, %llu frames",
					bf3::sim::PhysicsWorld::jolt_version(), (unsigned long long)ticks, (unsigned long long)physics_ticks_, (unsigned long long)frames_));

	const bf3::core::InstallReader &install = session_.install();
	if (install.is_mounted()) {
		const bf3::core::InstallSnapshot after = install.snapshot();
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(session_.install_snapshot_at_start(), after);
		add("install.unchanged", d.clean() ? Result::Pass : Result::Fail,
				fmt("%zu files re-stamped after the session: ", after.files.size()) + d.summary());
	} else {
		add("install.unchanged", Result::Skip, "no install");
	}
	print_table();
}

void S0SelfTest::print_table() {
	size_t w = 5;
	for (const Row &r : rows_) {
		w = std::max(w, r.check.size());
	}
	auto pad = [&](const std::string &s) { return s + std::string(w - s.size(), ' '); };
	UtilityFunctions::print("");
	UtilityFunctions::print(to_godot("| " + pad("check") + " | result | detail"));
	UtilityFunctions::print(to_godot("|-" + std::string(w, '-') + "-|--------|-------"));
	int pass = 0;
	int fail = 0;
	int skip = 0;
	for (const Row &r : rows_) {
		const char *res = r.result == Result::Pass ? "PASS  " : r.result == Result::Fail ? "FAIL  " : r.result == Result::Skip ? "SKIP  " : "INFO  ";
		pass += r.result == Result::Pass;
		fail += r.result == Result::Fail;
		skip += r.result == Result::Skip;
		UtilityFunctions::print(to_godot("| " + pad(r.check) + " | " + res + " | " + r.detail));
	}
	UtilityFunctions::print(to_godot(fmt("S0 SELFTEST: %s (%d pass, %d fail, %d skip) -> exit code %d", fail ? "FAIL" : "PASS", pass, fail, skip,
			fail ? 1 : 0)));
}

} // namespace bf3::godot_side
