#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/map_list.h"
#include "bf3/net/protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bf3::host {

using RotationLoader = std::function<core::Result<std::shared_ptr<const GameServerConfig>>(const LevelEntry &)>;

struct RotationConfig {
	MapList maps;
	std::shared_ptr<const GameServerConfig> first;
	RotationLoader load;
	bool background = true;
	uint16_t intermission_s = 60;
};

struct RotationStats {
	LevelEntry current;
	size_t index = 0;
	uint32_t round_id = 0;
	uint64_t round_overs = 0;
	uint64_t in_place = 0;
	uint64_t level_changes = 0;
	uint64_t load_failures = 0;
	bool changing = false;
	bool loading = false;
	std::string last_error;
};

bool same_level(const LevelEntry &p_a, const LevelEntry &p_b);

class Rotation {
public:
	explicit Rotation(RotationConfig p_config);
	~Rotation();
	Rotation(const Rotation &) = delete;
	Rotation &operator=(const Rotation &) = delete;

	std::unique_ptr<GameServer> after_tick(GameServer &r_server);

	const LevelEntry &current() const { return cursor_.current(); }
	RotationStats stats() const;

private:
	struct Pending {
		LevelEntry entry;
		bool done = false;
		core::Result<std::shared_ptr<const GameServerConfig>> result = core::make_error("not loaded");
	};

	void start_load(const LevelEntry &p_entry);
	void begin_change(GameServer &r_server);
	std::optional<core::Result<std::shared_ptr<const GameServerConfig>>> take_load(const LevelEntry &p_entry);
	net::NextLevel next_identity(const LevelEntry &p_entry) const;
	uint16_t intermission_s() const;
	void join_loader();

	RotationConfig config_;
	MapListCursor cursor_;
	bool announced_ = false;
	bool changing_ = false;
	uint32_t round_id_ = 0;
	std::shared_ptr<const GameServerConfig> current_config_;

	mutable std::mutex mutex_;
	std::thread loader_;
	std::shared_ptr<Pending> pending_;
	RotationStats stats_;
};

}
