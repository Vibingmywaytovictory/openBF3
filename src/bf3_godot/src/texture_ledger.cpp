#include "texture_ledger.h"

#include "video_memory.h"

#include "bf3/core/util/format.h"
#include "bf3/core/util/text.h"

#include <map>
#include <mutex>
#include <utility>

namespace bf3::godot_side {

namespace {

struct Tally {
	uint64_t textures = 0;
	uint64_t bytes = 0;
};

std::mutex &ledger_mutex() {
	static std::mutex m;
	return m;
}

std::map<std::string, Tally> &ledger() {
	static std::map<std::string, Tally> l;
	return l;
}

}

void ledger_texture(std::string_view p_path, std::string_view p_name, uint64_t p_bytes) {
	const std::string lower = core::to_lower_ascii(p_name);
	const size_t slash = lower.find('/');
	const std::string key = std::string(p_path) + " " + (slash == std::string::npos ? lower : lower.substr(0, slash));
	std::lock_guard lock(ledger_mutex());
	Tally &t = ledger()[key];
	++t.textures;
	t.bytes += p_bytes;
}

std::string texture_ledger_text() {
	std::lock_guard lock(ledger_mutex());
	uint64_t total = 0;
	std::string parts;
	for (const auto &[key, t] : ledger()) {
		total += t.bytes;
		parts += core::fmt("; %s %llu textures %.1f MiB", key.c_str(), static_cast<unsigned long long>(t.textures),
				video_memory_mib(t.bytes));
	}
	return core::fmt("[memory] textures created %.1f MiB", video_memory_mib(total)) + parts;
}

}
