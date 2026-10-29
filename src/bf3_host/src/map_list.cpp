#include "bf3/host/map_list.h"

#include "bf3/core/util/key_values.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace bf3::host {

namespace {

bool name_char(char p_c) {
	return (p_c >= 'A' && p_c <= 'Z') || (p_c >= 'a' && p_c <= 'z') || (p_c >= '0' && p_c <= '9') || p_c == '_';
}

bool valid_name(std::string_view p_name) {
	return !p_name.empty() && p_name.size() <= kMaxMapListName && std::all_of(p_name.begin(), p_name.end(), name_char);
}

core::KeyOutcome map_row(std::string_view p_item, size_t, MapList &r_list) {
	std::array<std::string_view, 3> words{};
	size_t n = 0;
	for (std::string_view rest = core::trim(p_item); !rest.empty(); rest = core::trim(rest)) {
		if (n == words.size()) {
			return { core::KeyFault::BadValue, 0.0, "expected `level mode [rounds]`, got more words" };
		}
		const size_t end = std::min(rest.find_first_of(" \t"), rest.size());
		words[n++] = rest.substr(0, end);
		rest.remove_prefix(end);
	}
	if (n < 2) {
		return { core::KeyFault::BadValue, 0.0, "expected `level mode [rounds]`" };
	}
	if (!valid_name(words[0]) || !valid_name(words[1])) {
		return { core::KeyFault::BadValue, 0.0, "a level or mode name is not 1..64 letters, digits or underscores" };
	}
	if (r_list.entries.size() >= kMaxMapListEntries) {
		return { core::KeyFault::BadValue, 0.0, "more than 64 entries" };
	}
	LevelEntry e;
	e.level = std::string(words[0]);
	e.mode = std::string(words[1]);
	if (n == 3) {
		const core::KeyOutcome rounds = core::parse_u32_value(words[2], 1.0, double(kMaxMapListRounds), e.rounds);
		if (rounds.fault != core::KeyFault::None) {
			return { core::KeyFault::BadValue, 0.0, "rounds is not a whole number 1..100" };
		}
	}
	r_list.entries.push_back(std::move(e));
	return {};
}

constexpr core::KeyValueGrammar<MapList> kGrammar{
	.what = "map list",
	.keys = {},
	.max_bytes = kMaxMapListBytes,
	.max_lines = kMaxMapListLines,
	.max_line_bytes = 256,
	.bare = map_row,
};

}

core::Result<MapList> parse_map_list(std::string_view p_text) {
	core::Result<MapList> list = core::parse_key_values(p_text, kGrammar);
	if (list && list->entries.empty()) {
		return core::make_error("map list: no entry (each line is `level mode [rounds]`)");
	}
	return list;
}

core::Result<MapList> load_map_list(const std::string &p_path) {
	const std::filesystem::path path(std::u8string(p_path.begin(), p_path.end()));
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(path, ec);
	if (ec) {
		return core::make_error("map list " + p_path + ": " + ec.message());
	}
	if (size > kMaxMapListBytes) {
		return core::make_error("map list " + p_path + ": " + std::to_string(size) + " bytes (limit " +
				std::to_string(kMaxMapListBytes) + ")");
	}
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return core::make_error("map list: cannot open " + p_path);
	}
	std::string text(size_t(size), '\0');
	in.read(text.data(), std::streamsize(size));
	text.resize(size_t(in.gcount()));
	core::Result<MapList> list = parse_map_list(text);
	if (!list) {
		return core::error_at(p_path, list.error());
	}
	return list;
}

std::string format_map_list(const MapList &p_list) {
	std::string out;
	for (const LevelEntry &e : p_list.entries) {
		out += e.level + " " + e.mode + " " + std::to_string(e.rounds) + "\n";
	}
	return out;
}

MapListCursor::MapListCursor(MapList p_list) : list_(std::move(p_list)) {}

const LevelEntry &MapListCursor::next_level() const {
	if (round_ + 1 < list_.entries[index_].rounds) {
		return list_.entries[index_];
	}
	return list_.entries[(index_ + 1) % list_.entries.size()];
}

bool MapListCursor::round_over() {
	if (++round_ < list_.entries[index_].rounds) {
		return false;
	}
	round_ = 0;
	const LevelEntry &before = list_.entries[index_];
	index_ = (index_ + 1) % list_.entries.size();
	return !core::iequals_ascii(before.level, current().level) || !core::iequals_ascii(before.mode, current().mode);
}

}
