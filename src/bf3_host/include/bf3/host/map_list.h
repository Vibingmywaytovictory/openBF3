#pragma once

#include "bf3/core/result.h"
#include "bf3/host/server_level.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::host {

inline constexpr size_t kMaxMapListBytes = 16 * 1024;
inline constexpr size_t kMaxMapListLines = 256;
inline constexpr size_t kMaxMapListEntries = 64;
inline constexpr size_t kMaxMapListName = 64;
inline constexpr uint32_t kMaxMapListRounds = 100;

struct MapList {
	std::vector<LevelEntry> entries;

	bool operator==(const MapList &) const = default;
};

core::Result<MapList> parse_map_list(std::string_view p_text);
core::Result<MapList> load_map_list(const std::string &p_path);
std::string format_map_list(const MapList &p_list);

class MapListCursor {
public:
	explicit MapListCursor(MapList p_list);

	const MapList &list() const { return list_; }
	size_t index() const { return index_; }
	uint32_t round() const { return round_; }
	const LevelEntry &current() const { return list_.entries[index_]; }
	const LevelEntry &next_level() const;
	bool round_over();

private:
	MapList list_;
	size_t index_ = 0;
	uint32_t round_ = 0;
};

}
