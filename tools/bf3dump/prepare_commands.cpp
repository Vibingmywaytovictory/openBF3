// bf3dump prepare (plan §3.12 "Prepare levels": CLI `bf3dump prepare`; M0 "per-item streaming converters").
#include "prepare_commands.h"

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/render/level_prepare.h"

#include <windows.h>
// psapi after windows.h
#include <psapi.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string_view>

using namespace bf3::core;

namespace bf3dump {

namespace {

std::filesystem::path utf8_path(std::string_view p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string path_utf8(const std::filesystem::path &p_path) {
	const std::u8string s = p_path.u8string();
	return std::string(s.begin(), s.end());
}

// Godot's OS_Windows::get_user_data_dir for a project without a custom user dir:
// %APPDATA%/Godot/app_userdata/<project name>; the project is named "bf3-opensource" (project.godot).
std::filesystem::path default_cache_root() {
	wchar_t buf[32768];
	const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, DWORD(std::size(buf)));
	if (n == 0 || n >= std::size(buf)) {
		return {};
	}
	return std::filesystem::path(std::wstring(buf, n)) / L"Godot" / L"app_userdata" / L"bf3-opensource" / L"bf3cache";
}

double mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

void print_totals(const char *p_what, const bf3::render::PrepareTotals &p_t) {
	const uint64_t misses = p_t.converted + p_t.reused + p_t.uncached;
	std::printf("%-8s %llu items: %llu hits, %llu misses (%llu converted, %llu reused, %llu uncached), %llu failed, %llu dropped corrupt; "
				"%.1f MiB written, %.1f MiB read from the cache; %.3f s\n",
			p_what, static_cast<unsigned long long>(p_t.items), static_cast<unsigned long long>(p_t.hits), static_cast<unsigned long long>(misses),
			static_cast<unsigned long long>(p_t.converted), static_cast<unsigned long long>(p_t.reused), static_cast<unsigned long long>(p_t.uncached),
			static_cast<unsigned long long>(p_t.failed), static_cast<unsigned long long>(p_t.dropped_corrupt), mib(p_t.bytes_written), mib(p_t.bytes_hit),
			p_t.seconds);
}

} // namespace

int cmd_prepare(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	std::vector<std::string> positional;
	std::filesystem::path root;
	bf3::render::PrepareOptions options;
	for (const std::string &a : p_args) {
		if (a.rfind("--cache=", 0) == 0) {
			root = utf8_path(std::string_view(a).substr(8));
		} else if (a.rfind("--threads=", 0) == 0) {
			unsigned n = 0;
			const std::string_view v = std::string_view(a).substr(10);
			const auto res = std::from_chars(v.data(), v.data() + v.size(), n);
			if (res.ec != std::errc() || res.ptr != v.data() + v.size() || n == 0 || n > 256) {
				std::fprintf(stderr, "prepare: --threads wants 1..256: %s\n", a.c_str());
				return 2;
			}
			options.threads = n;
		} else if (a == "--no-weapons") {
			options.weapons = false;
		} else if (a == "--no-meshes") {
			options.meshes = false;
		} else if (a.rfind("--", 0) == 0) {
			std::fprintf(stderr, "prepare: unknown option %s\n", a.c_str());
			return 2;
		} else {
			positional.push_back(a);
		}
	}
	if (positional.size() != 2) {
		std::fprintf(stderr, "usage: bf3dump prepare <LEVEL> <MODE> [--cache=<dir>] [--threads=N] [--no-weapons] [--no-meshes]\n");
		return 2;
	}
	if (root.empty()) {
		root = default_cache_root();
		if (root.empty()) {
			std::fprintf(stderr, "prepare: APPDATA is not set; pass --cache=<dir>\n");
			return 2;
		}
	}
	std::error_code ec;
	root = std::filesystem::absolute(root, ec);
	if (ec) {
		std::fprintf(stderr, "prepare: bad cache root\n");
		return 2;
	}

	Cache cache;
	CacheOptions cache_options;
	cache_options.root = root;
	cache_options.guard = p_reader.write_guard(); // refuses the install (and anything below it)
	std::string detail;
	const CacheStatus opened = cache.open(std::move(cache_options), &detail);
	if (opened != CacheStatus::Ok) {
		std::fprintf(stderr, "prepare: cache %s: %s %s\n", path_utf8(root).c_str(), cache_status_name(opened), detail.c_str());
		return 1;
	}
	const CacheStats before = cache.stats();
	std::printf("prepare %s %s -> %s (engine %s)\n", positional[0].c_str(), positional[1].c_str(), path_utf8(cache.root()).c_str(),
			std::string(cache_engine_id()).c_str());
	std::printf("cache    open: %llu entries, %.1f MiB%s%s\n", static_cast<unsigned long long>(before.entries), mib(before.bytes),
			before.recovered_unclean ? ", recovered an unclean close" : "", before.corrupt_dropped ? ", dropped damaged entries" : "");

	Result<bf3::render::PrepareReport> report = bf3::render::prepare_level(p_install, positional[0], positional[1], &cache, options);
	if (!report) {
		std::fprintf(stderr, "prepare %s %s: %s\n", positional[0].c_str(), positional[1].c_str(), report.error().message.c_str());
		cache.close();
		return 1;
	}
	const bf3::render::PrepareReport &r = *report;
	std::printf("level    %zu partitions, load + traverse %.3f s; census %.3f s: %zu placements, %zu scene meshes, %zu census failures\n", r.partitions,
			r.seconds_load, r.seconds_census, r.placements, r.scene_meshes, r.census_failures);
	print_totals("meshes", r.meshes);
	std::printf("         %llu vertices, %llu triangles over the prepared LODs\n", static_cast<unsigned long long>(r.meshes.vertices),
			static_cast<unsigned long long>(r.meshes.triangles));
	print_totals("weapons", r.weapons);
	for (const auto &[item, error] : r.failures) {
		std::printf("issue    %s: %s\n", item.c_str(), error.c_str());
	}
	const CacheStats after = cache.stats();
	cache.close();
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
	std::printf("cache    %llu -> %llu entries, %.1f -> %.1f MiB; %llu puts, %llu hits, %llu misses, %llu dedup reused, %llu damaged dropped\n",
			static_cast<unsigned long long>(before.entries), static_cast<unsigned long long>(after.entries), mib(before.bytes), mib(after.bytes),
			static_cast<unsigned long long>(after.puts - before.puts), static_cast<unsigned long long>(after.hits - before.hits),
			static_cast<unsigned long long>(after.misses - before.misses), static_cast<unsigned long long>(after.dedup_reused - before.dedup_reused),
			static_cast<unsigned long long>(after.corrupt_dropped));
	std::printf("total    %.3f s; peak working set %.1f MiB, peak private bytes %.1f MiB\n", r.seconds_total, mib(pmc.PeakWorkingSetSize), mib(pmc.PeakPagefileUsage));
	const bool failed = r.meshes.failed > 0 || r.weapons.failed > 0;
	return failed ? 1 : 0;
}

} // namespace bf3dump
