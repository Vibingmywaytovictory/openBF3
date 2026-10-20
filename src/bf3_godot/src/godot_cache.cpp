#include "godot_cache.h"

#include "godot_util.h"

#include <godot_cpp/classes/project_settings.hpp>

#include <chrono>
#include <cwctype>
#include <map>
#include <mutex>

namespace bf3::godot_side {

namespace {

std::mutex g_mutex;
std::map<std::wstring, std::weak_ptr<bf3::core::Cache>> g_open;

std::wstring root_key(const std::filesystem::path &p_root) {
	std::wstring k = std::filesystem::absolute(p_root).lexically_normal().native();
	for (wchar_t &c : k) {
		c = (c == L'/') ? L'\\' : towlower(c);
	}
	while (k.size() > 3 && k.back() == L'\\') {
		k.pop_back();
	}
	return k;
}

}

std::filesystem::path default_cache_root() {
	const std::string root = to_std(godot::ProjectSettings::get_singleton()->globalize_path("user://bf3cache"));
	return std::filesystem::path(std::u8string(root.begin(), root.end())).make_preferred();
}

AcquiredCache acquire_cache(const std::filesystem::path &p_root, const bf3::core::PathGuard &p_guard) {
	AcquiredCache out;
	if (p_root.empty()) {
		out.status = bf3::core::CacheStatus::InvalidArgument;
		out.detail = "no cache root";
		return out;
	}
	const std::wstring key = root_key(p_root);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (std::shared_ptr<bf3::core::Cache> live = g_open[key].lock()) {
		out.cache = std::move(live);
		out.status = bf3::core::CacheStatus::Ok;
		out.shared = true;
		return out;
	}
	const auto t0 = std::chrono::steady_clock::now();
	auto cache = std::make_shared<bf3::core::Cache>();
	bf3::core::CacheOptions options;
	options.root = p_root;
	options.guard = p_guard;
	out.status = cache->open(std::move(options), &out.detail);
	out.open_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (out.status == bf3::core::CacheStatus::Ok) {
		g_open[key] = cache;
		out.cache = std::move(cache);
	}
	return out;
}

}
