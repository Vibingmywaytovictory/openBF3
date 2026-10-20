#include "firefight_commands.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/host/firefight_run.h"
#include "bf3/sim/soldier_motor.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bf3dump {

namespace {

using namespace bf3;

constexpr double kPerfGateP99Ms = 8.0;

std::filesystem::path utf8_path(std::string_view p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

bool parse_uint(std::string_view p_text, unsigned &r_value) {
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), r_value);
	return res.ec == std::errc() && res.ptr == p_text.data() + p_text.size();
}

bool parse_u64(std::string_view p_text, uint64_t &r_value) {
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), r_value);
	return res.ec == std::errc() && res.ptr == p_text.data() + p_text.size();
}

bool parse_float(const std::string &p_text, float &r_value) {
	char *end = nullptr;
	r_value = std::strtof(p_text.c_str(), &end);
	return end != nullptr && *end == 0 && std::isfinite(r_value);
}

int finish(const core::InstallReader &p_reader, const core::InstallSnapshot &p_before, int p_rc) {
	const core::SnapshotDiff diff = core::diff_snapshots(p_before, p_reader.snapshot());
	std::printf("install.unchanged %s\n",
			p_before.valid && diff.clean() ? ("yes (" + std::to_string(p_before.files.size()) + " files: sizes and mtimes equal)").c_str() : ("NO: " + diff.summary()).c_str());
	return p_before.valid && diff.clean() ? p_rc : 1;
}

std::string read_file(const std::filesystem::path &p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

}

const char *firefight_usage() {
	return "  firefight <LEVEL> <MODE> [--mode=pairs|free] [--count=N] [--seconds=S] [--warmup=S] [--seed=N] [--observers=N]\n"
		   "      [--slots=N] [--no-walk] [--no-checks] [--bots] [--perf] [--threads=N] [--tuning=<motor_tuning.cfg>]\n"
		   "      [--mechanics=<weapon_mechanics.cfg>] [--cache=<dir>]\n";
}

int cmd_firefight(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const std::vector<std::string> &p_args) {
	host::FirefightRunOptions o;
	o.hashes = false;
	host::FirefightLoadOptions lo;
	std::vector<std::string> positional;
	std::filesystem::path cache_root;
	std::string tuning_text;
	bool perf = false, per_bot = false;
	uint32_t slots = 0;
	float seconds = 60.0f, warmup = 0.0f;
	bool seconds_set = false, warmup_set = false, count_set = false, mode_set = false, observers_set = false;
	for (const std::string &s : p_args) {
		unsigned v = 0;
		uint64_t v64 = 0;
		float f = 0.0f;
		if (s == "--mode=pairs" || s == "--mode=free") {
			o.mode = s == "--mode=pairs" ? host::FirefightMode::Pairs : host::FirefightMode::Free;
			mode_set = true;
		} else if (s.rfind("--count=", 0) == 0 && parse_uint(std::string_view(s).substr(8), v) && v >= 1 && v <= 120) {
			o.count = v;
			count_set = true;
		} else if (s.rfind("--seconds=", 0) == 0 && parse_float(s.substr(10), f) && f > 0.0f && f <= 3600.0f) {
			seconds = f;
			seconds_set = true;
		} else if (s.rfind("--warmup=", 0) == 0 && parse_float(s.substr(9), f) && f >= 0.0f && f <= 600.0f) {
			warmup = f;
			warmup_set = true;
		} else if (s.rfind("--seed=", 0) == 0 && parse_u64(std::string_view(s).substr(7), v64)) {
			o.seed = v64;
		} else if (s.rfind("--observers=", 0) == 0 && parse_uint(std::string_view(s).substr(12), v) && v <= 8) {
			o.observers = v;
			observers_set = true;
		} else if (s.rfind("--slots=", 0) == 0 && parse_uint(std::string_view(s).substr(8), v) && v >= 1 && v <= 128) {
			slots = v;
		} else if (s.rfind("--threads=", 0) == 0 && parse_uint(std::string_view(s).substr(10), v)) {
			lo.threads = v;
		} else if (s == "--no-walk") {
			o.walk = false;
		} else if (s == "--no-checks") {
			o.check_every = 0;
		} else if (s == "--bots") {
			per_bot = true;
		} else if (s == "--perf") {
			perf = true;
		} else if (s.rfind("--tuning=", 0) == 0) {
			tuning_text = read_file(utf8_path(std::string_view(s).substr(9)));
			if (tuning_text.empty()) {
				std::fprintf(stderr, "firefight: cannot read %s\n", s.c_str() + 9);
				return 2;
			}
		} else if (s.rfind("--mechanics=", 0) == 0) {
			o.mechanics_cfg = s.substr(12);
		} else if (s.rfind("--cache=", 0) == 0) {
			cache_root = utf8_path(std::string_view(s).substr(8));
		} else if (s.rfind("--", 0) == 0) {
			std::fprintf(stderr, "firefight: bad option %s\n%s", s.c_str(), firefight_usage());
			return 2;
		} else {
			positional.push_back(s);
		}
	}
	if (positional.size() != 2) {
		std::fprintf(stderr, "%s", firefight_usage());
		return 2;
	}
	if (perf) {

		if (!mode_set) {
			o.mode = host::FirefightMode::Pairs;
		}
		if (!count_set) {
			o.count = 64;
		}
		if (!observers_set) {
			o.observers = 1;
		}
		if (!warmup_set) {
			warmup = 10.0f;
		}
		if (!seconds_set) {
			seconds = 120.0f;
		}
		if (o.mode == host::FirefightMode::Pairs) {
			o.walk = false;
		}
		o.check_every = 0;
	}
	o.max_slots = slots != 0 ? slots : std::max<uint32_t>(64u, o.count + o.observers);
	o.ticks = uint32_t(std::lround(double(seconds) * double(sim::kSimHz)));
	o.warmup_ticks = uint32_t(std::lround(double(warmup) * double(sim::kSimHz)));
	if (o.mechanics_cfg.empty()) {

		std::error_code ec;
		if (std::filesystem::exists("bf3-opensource/config/weapon_mechanics.cfg", ec)) {
			o.mechanics_cfg = "bf3-opensource/config/weapon_mechanics.cfg";
		}
		std::printf("mechanics: %s\n", o.mechanics_cfg.empty() ? "config/weapon_mechanics.cfg (pass --mechanics=<weapon_mechanics.cfg>)" : o.mechanics_cfg.c_str());
	}
	if (tuning_text.empty()) {

		tuning_text = read_file("bf3-opensource/config/motor_tuning.cfg");
		std::printf("tuning: %s\n", tuning_text.empty() ? "defaults (pass --tuning=<motor_tuning.cfg>)" : "bf3-opensource/config/motor_tuning.cfg");
	}
	if (!tuning_text.empty()) {
		core::Result<sim::MotorTuning> t = sim::parse_motor_tuning(tuning_text);
		if (!t) {
			std::fprintf(stderr, "firefight: tuning: %s\n", t.error().message.c_str());
			return 2;
		}
		lo.tuning = *t;
	}
	const core::InstallSnapshot before = p_reader.snapshot();
	core::Cache cache;
	if (!cache_root.empty()) {
		core::CacheOptions co;
		std::error_code ec;
		co.root = std::filesystem::absolute(cache_root, ec);
		co.guard = p_reader.write_guard();
		std::string detail;
		if (ec || cache.open(std::move(co), &detail) != core::CacheStatus::Ok) {
			std::fprintf(stderr, "firefight: cache: %s\n", detail.c_str());
			return 1;
		}
		lo.cache = &cache;
	}
	core::Result<host::FirefightLevel> level = host::load_firefight_level(p_install, positional[0], positional[1], lo);
	cache.close();
	if (!level) {
		std::fprintf(stderr, "firefight: %s\n", level.error().message.c_str());
		return finish(p_reader, before, 1);
	}
	std::printf("load: database %.2f s, shared level (gunplay) %.2f s, bot level %.2f s (nav %s)\n", level->seconds_database, level->seconds_shared,
			level->seconds_bots, level->nav_cache_hit ? "from the cache" : "baked");
	core::Result<host::FirefightReport> r = host::run_firefight(level->shared, level->bots, o);
	if (!r) {
		std::fprintf(stderr, "firefight: %s\n", r.error().message.c_str());
		return finish(p_reader, before, 1);
	}
	std::printf("%s\n", host::firefight_report_text(*r).c_str());
	if (per_bot) {
		std::printf("slot\tteam\tkit\tshots\thits\tkills\tdeaths\tlives\tmetres\twindows\tstuck\tmin_window_m\n");
		for (const host::FirefightBotRow &b : r->bots) {
			std::printf("%u\t%u\t%u\t%llu\t%llu\t%u\t%u\t%u\t%.1f\t%u\t%u\t%.2f\n", unsigned(b.slot), unsigned(b.team), unsigned(b.kit),
					(unsigned long long)b.shots, (unsigned long long)b.hits, b.kills, b.deaths, b.lives, b.metres, b.windows, b.stuck_windows,
					double(b.min_window_metres));
		}
	}
	if (perf) {
		const double gun = r->weapons_ms.mean + r->projectiles_ms.mean + r->rewind_ms.mean;
		std::printf("E13: tick p99 %.3f ms %s %.1f ms; weapons + projectiles + rewind mean %.3f ms (%.1f %% of the mean tick)\n", r->tick_ms.p99,
				r->tick_ms.p99 <= kPerfGateP99Ms ? "<=" : ">", kPerfGateP99Ms, gun, r->tick_ms.mean > 0 ? 100.0 * gun / r->tick_ms.mean : 0.0);
	}
	return finish(p_reader, before, 0);
}

}
