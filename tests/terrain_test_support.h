#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/superbundle_mount.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bf3::test {

struct LoadedTerrainTree {
	core::res::TerrainStreamingTree tree;
	std::string name;
};

inline bool load_terrain_tree(const core::LevelMount &p_mount, const std::string &p_asset, LoadedTerrainTree &r_out) {
	auto terrain = p_mount.read_res(p_asset);
	if (!terrain || !core::res::terrain_streaming_tree_name(terrain->second, r_out.name).ok()) {
		return false;
	}
	auto bytes = p_mount.read_res(r_out.name);
	return bytes && core::res::TerrainStreamingTree::parse(bytes->second, r_out.tree).ok();
}

class TerrainChunks {
public:
	explicit TerrainChunks(const core::LevelMount &p_mount) : mount_(p_mount) {}

	core::res::TerrainChunkFetch fetch() {
		return [this](const core::res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
			auto found = chunks_.find(p_id.bytes);
			if (found == chunks_.end()) {
				core::Result<std::vector<uint8_t>> c = mount_.read_chunk(core::Guid::from_bytes(p_id.bytes.data()));
				if (!c) {
					return std::nullopt;
				}
				found = chunks_.emplace(p_id.bytes, std::move(*c)).first;
			}
			return found->second;
		};
	}

private:
	const core::LevelMount &mount_;
	std::map<std::array<uint8_t, 16>, std::vector<uint8_t>> chunks_;
};

}
