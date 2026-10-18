// M2a bt (docs/design/m2a.md §1 E13, E15, §10.2, §14.1; docs/design/m2a-bt.md): the firefight bots on the real
// GameServer, headless, on Metro TDM (MP_Subway / TeamDeathMatch0; read-only on the install, a per-process scratch
// cache root under the temp directory, the shipped motor tuning). Three suites, one ctest row each:
//   host-gunplay-firefight  host.gunplay.firefight (functional, untimed): 64 Free bots for 60 s plus one idle observer
//                           peer: both teams kill, every bot fires, none is stuck, none inside geometry or below the
//                           ground, no NaN, no precondition failure, no team kill; the peer (a human slot) is engaged.
//   host-gunplay-replay     host.gunplay.replay (E15, functional): two 3,600-tick firefights with one seed give equal
//                           per-tick world hashes, a third with another seed differs.
//   host-gunplay-perf       host.gunplay.perf (E13, timing, RUN_SERIAL): 64 Pairs bots and one observer peer, 10 s
//                           warm-up, 120 s window, server tick p99 <= 8.0 ms (report only under asan).
// Every suite ends with its install-unchanged case.
#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/host/firefight_run.h"
#include "bf3/sim/soldier_motor.h"
#include "test_support.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define OPENBF3_GETPID _getpid
#else
#include <unistd.h>
#define OPENBF3_GETPID getpid
#endif

namespace {

using namespace bf3;

inline constexpr double kGateP99Ms = 8.0; // E13 (S1's G1 gate)
inline constexpr uint64_t kSeed = 0xB07B07ull;

std::string read_text(const std::filesystem::path &p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

struct Fixture {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	std::filesystem::path cache_root;
	host::FirefightLevel level;
};

// Metro with gunplay and S1's bot level, once per process (the three rows are three processes).
Fixture *fixture() {
	static std::unique_ptr<Fixture> s_fixture;
	static bool s_tried = false;
	if (s_tried) {
		if (s_fixture == nullptr) {
			bf3::test::skip_install_case("no BF3 install or the load failed (see the first case)");
		}
		return s_fixture.get();
	}
	s_tried = true;
	auto f = std::make_unique<Fixture>();
	f->reader = std::make_unique<core::InstallReader>();
	if (!f->reader->locate({})) {
		bf3::test::skip_install_case(f->reader->describe().c_str());
		return nullptr;
	}
	f->before = f->reader->snapshot();
	core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(*f->reader);
	REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	if (!opened) {
		return nullptr;
	}
	f->install = *opened;
	std::error_code ec;
	f->cache_root = std::filesystem::temp_directory_path(ec) / ("openbf3_host_gunplay_perf_" + std::to_string(OPENBF3_GETPID()));
	std::filesystem::remove_all(f->cache_root, ec);
	core::Cache cache;
	core::CacheOptions co;
	co.root = f->cache_root;
	co.guard = f->reader->write_guard();
	std::string detail;
	REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
	const std::string tuning_text = read_text(OPENBF3_MOTOR_TUNING_CFG);
	core::Result<sim::MotorTuning> tuning = sim::parse_motor_tuning(tuning_text);
	REQUIRE_MESSAGE(tuning.ok(), (tuning.ok() ? std::string() : tuning.error().message));
	if (!tuning) {
		return nullptr;
	}
	host::FirefightLoadOptions lo;
	lo.cache = &cache;
	lo.tuning = *tuning;
	const auto start = std::chrono::steady_clock::now();
	core::Result<host::FirefightLevel> level = host::load_firefight_level(f->install, "MP_Subway", "TeamDeathMatch0", lo);
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	if (!level) {
		return nullptr;
	}
	cache.close();
	f->level = std::move(*level);
	std::printf("firefight fixture: Metro TDM with gunplay and the bot level in %.2f s (database %.2f, shared %.2f, bots %.2f; %zu spawns, "
				"%zu bot spawn points)\n",
			std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), f->level.seconds_database, f->level.seconds_shared,
			f->level.seconds_bots, f->level.shared->spawns.size(), f->level.bots->spawns.points.size());
	s_fixture = std::move(f);
	return s_fixture.get();
}

void check_install_unchanged() {
	Fixture *f = fixture();
	if (f == nullptr) {
		return;
	}
	const core::SnapshotDiff diff = core::diff_snapshots(f->before, f->reader->snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
	std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), f->before.files.size());
	std::error_code ec;
	std::filesystem::remove_all(f->cache_root, ec);
}

std::optional<host::FirefightReport> run(const host::FirefightRunOptions &p_options, const char *p_name) {
	Fixture *f = fixture();
	if (f == nullptr) {
		return std::nullopt;
	}
	host::FirefightRunOptions o = p_options;
	o.mechanics_cfg = OPENBF3_MECHANICS_CFG; // the tests do not run in the project directory
	core::Result<host::FirefightReport> r = host::run_firefight(f->level.shared, f->level.bots, o);
	CHECK_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	if (!r) {
		return std::nullopt;
	}
	std::printf("---- %s ----\n%s\n", p_name, host::firefight_report_text(*r).c_str());
	std::fflush(stdout);
	return std::move(*r);
}

// The checks every firefight run must pass (a broken world, a broken brain or a broken server).
void check_sane(const host::FirefightReport &p_r) {
	CHECK(p_r.checks > 0);
	CHECK_MESSAGE(p_r.inside_geometry == 0, "soldier capsules inside the static world: ", p_r.inside_geometry);
	CHECK_MESSAGE(p_r.below_ground == 0, "soldiers with no ground below: ", p_r.below_ground);
	CHECK_MESSAGE(p_r.non_finite == 0, "non-finite soldier states: ", p_r.non_finite);
	CHECK(p_r.step_precondition_failures == 0);
	CHECK(p_r.weapon_precondition_failures == 0);
	CHECK(p_r.team_kills == 0);
	CHECK(p_r.kills[0] == 0);
}

} // namespace

TEST_SUITE("host-gunplay-firefight") {
	TEST_CASE("64 bots fight for 60 s") {
		host::FirefightRunOptions o;
		o.mode = host::FirefightMode::Free;
		o.count = 64;
		o.walk = true;
		o.seed = kSeed;
		o.ticks = 60 * sim::kSimHz;
		o.observers = 1; // a human slot (held at its spawn): bots must engage it like any enemy
		o.hashes = false;
		o.check_every = 30;
		std::optional<host::FirefightReport> r = run(o, "host.gunplay.firefight: Free, 64 bots, 60 s");
		if (!r) {
			return;
		}
		CHECK(r->bots.size() == 64);
		CHECK(r->counters.walk_unavailable == 0);
		check_sane(*r);
		CHECK_MESSAGE(r->kills[1] > 0, "team 1 scored no kill");
		CHECK_MESSAGE(r->kills[2] > 0, "team 2 scored no kill");
		CHECK_MESSAGE(r->bots_fired == r->bots.size(), (r->bots.size() - r->bots_fired), " bots never fired");
		for (const host::FirefightBotRow &b : r->bots) {
			CHECK_MESSAGE(b.shots > 0, "slot ", int(b.slot), " never fired");
			CHECK_MESSAGE(b.stuck_windows == 0, "slot ", int(b.slot), " stuck in ", b.stuck_windows, " of ", b.windows, " 20 s windows (min ",
					b.min_window_metres, " m)");
		}
		CHECK(r->windows > 0);
		CHECK(r->bots_stuck == 0);
		CHECK(r->peer_slots == 1);
		CHECK_MESSAGE(r->peer_hits > 0, "the bots never hit the observer peer (a human slot)");
		// Most bots should have hit someone (not only blind fire); reported, gated loosely.
		CHECK_MESSAGE(r->bots_hit_enemy * 2 >= r->bots.size(), "only ", r->bots_hit_enemy, " bots hit an enemy");
	}

	TEST_CASE("install unchanged") {
		check_install_unchanged();
	}
}

TEST_SUITE("host-gunplay-replay") {
	TEST_CASE("equal seeds replay tick for tick, another seed differs") {
		if (fixture() == nullptr) {
			return;
		}
		// Three independent servers (own GameServer, CollisionWorld, brains) on three threads over the shared const level.
		host::FirefightRunOptions o;
		o.mode = host::FirefightMode::Free;
		o.count = 64;
		o.walk = true;
		o.seed = kSeed;
		o.ticks = 3600;
		o.hashes = true;
		o.check_every = 60;
		o.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		host::FirefightRunOptions other = o;
		other.seed = kSeed + 1;
		Fixture *f = fixture();
		std::vector<core::Result<host::FirefightReport>> results;
		results.reserve(3);
		for (int i = 0; i < 3; ++i) {
			results.emplace_back(core::make_error("not run"));
		}
		const host::FirefightRunOptions *opts[3] = { &o, &o, &other };
		{
			std::vector<std::thread> threads;
			for (int i = 0; i < 3; ++i) {
				threads.emplace_back([&, i]() { results[i] = host::run_firefight(f->level.shared, f->level.bots, *opts[i]); });
			}
			for (std::thread &t : threads) {
				t.join();
			}
		}
		for (int i = 0; i < 3; ++i) {
			REQUIRE_MESSAGE(results[i].ok(), (results[i].ok() ? std::string() : results[i].error().message));
			if (!results[i]) {
				return;
			}
		}
		const host::FirefightReport &a = *results[0], &b = *results[1], &c = *results[2];
		std::printf("---- host.gunplay.replay run A (seed %llu) ----\n%s\n", (unsigned long long)o.seed, host::firefight_report_text(a).c_str());
		std::printf("---- run B (seed %llu) ----\n%s\n", (unsigned long long)o.seed, host::firefight_report_text(b).c_str());
		std::printf("---- run C (seed %llu) ----\n%s\n", (unsigned long long)other.seed, host::firefight_report_text(c).c_str());
		REQUIRE(a.tick_hashes.size() == 3600);
		REQUIRE(b.tick_hashes.size() == 3600);
		REQUIRE(c.tick_hashes.size() == 3600);
		size_t first_ab = SIZE_MAX, differ_ab = 0, differ_ac = 0;
		for (size_t t = 0; t < 3600; ++t) {
			if (a.tick_hashes[t] != b.tick_hashes[t]) {
				++differ_ab;
				first_ab = std::min(first_ab, t);
			}
			if (a.tick_hashes[t] != c.tick_hashes[t]) {
				++differ_ac;
			}
		}
		std::printf("replay: equal seeds differ on %zu of 3600 ticks (first %lld); the other seed differs on %zu ticks; run hashes 0x%016llx "
					"0x%016llx 0x%016llx\n",
				differ_ab, first_ab == SIZE_MAX ? -1ll : (long long)first_ab, differ_ac, (unsigned long long)a.run_hash, (unsigned long long)b.run_hash,
				(unsigned long long)c.run_hash);
		CHECK_MESSAGE(differ_ab == 0, "equal seeds diverge at window tick ", first_ab);
		CHECK(a.run_hash == b.run_hash);
		CHECK(a.match_seed == b.match_seed);
		CHECK(a.match_seed != c.match_seed);
		CHECK(differ_ac > 0);
		CHECK(a.run_hash != c.run_hash);
		// The replayed run is a real fight, not a frozen one.
		CHECK(a.kills[1] + a.kills[2] > 0);
		CHECK(a.bots_fired > 0);
		check_sane(a);
		check_sane(c);
	}

	TEST_CASE("install unchanged") {
		check_install_unchanged();
	}
}

TEST_SUITE("host-gunplay-perf") {
	TEST_CASE("E13: 64 firing bots, 120 s, tick p99") {
		host::FirefightRunOptions o;
		o.mode = host::FirefightMode::Pairs;
		o.count = 64;
		o.walk = false;
		o.seed = kSeed;
		o.observers = 1;
		o.warmup_ticks = 10 * sim::kSimHz;
		o.ticks = 120 * sim::kSimHz;
		o.hashes = false;
		o.check_every = 0; // measured ticks only (the firefight row checks the world)
		std::optional<host::FirefightReport> r = run(o, "host.gunplay.perf (E13): Pairs, 64 bots, 1 observer, 10 s warm-up, 120 s");
		if (!r) {
			return;
		}
		CHECK(r->bots.size() == 64);
		CHECK(r->ticks == 7200);
		CHECK(r->step_precondition_failures == 0);
		CHECK(r->weapon_precondition_failures == 0);
		// The bots really fire through the window (E13 measures firing bots).
		CHECK_MESSAGE(r->bots_fired == r->bots.size(), (r->bots.size() - r->bots_fired), " bots never fired");
		const double gun = r->weapons_ms.mean + r->projectiles_ms.mean + r->rewind_ms.mean;
		std::printf("E13: tick p99 %.3f ms (gate %.1f ms); weapons + projectiles + rewind mean %.3f ms = %.1f %% of the mean tick %.3f ms\n",
				r->tick_ms.p99, kGateP99Ms, gun, r->tick_ms.mean > 0 ? 100.0 * gun / r->tick_ms.mean : 0.0, r->tick_ms.mean);
#if !defined(OPENBF3_TIMING_REPORT_ONLY)
		CHECK_MESSAGE(r->tick_ms.p99 <= kGateP99Ms, "E13: tick p99 ", r->tick_ms.p99, " ms > ", kGateP99Ms, " ms");
#endif
	}

	TEST_CASE("install unchanged") {
		check_install_unchanged();
	}
}
