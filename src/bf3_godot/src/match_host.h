#pragma once

#include "bf3/core/install_reader.h"
#include "bf3/host/client_session.h"
#include "bf3/host/map_list.h"
#include "bf3/host/server_session.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bf3::godot_side {

class Bf3LevelSession;
class GameSettings;
class NativeKick;

struct MatchLevel {
	std::string level;
	std::string mode;
	uint8_t rules_flags = 0;
	bool rules_known = false;
	uint32_t index = 1;
};

class Bf3MatchHost : public godot::Node {
	GDCLASS(Bf3MatchHost, godot::Node)

public:
	Bf3MatchHost();
	~Bf3MatchHost() override;

	static Bf3MatchHost *current();

	void _ready() override;
	void _process(double p_delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &p_event) override;
	void _exit_tree() override;

	bool following() const { return following_; }
	std::optional<MatchLevel> first_level() const;
	std::shared_ptr<bf3::host::ClientSession> client() const;
	bf3::host::ClientSessionConfig client_config() const;
	bool last_level() const { return level_index_ >= levels_to_play_; }

	std::shared_ptr<GameSettings> settings() const;
	void keep_settings(std::shared_ptr<GameSettings> p_settings);
	void level_done(const Bf3LevelSession &p_session);

protected:
	static void _bind_methods();

private:
	enum class Phase : uint8_t { Lobby, Playing, Swapping };

	bool parse_connect();
	bool prepare_listen_server();
	void connect_client(const std::string &p_host, uint16_t p_port);
	void run_listen_server();
	void watch_link();
	void watch_changes();
	void make_session(const MatchLevel &p_level);
	void free_session();
	void end_match(const std::string &p_reason);
	Bf3LevelSession *session() const;
	void persistent_ui();

	bool following_ = false;
	bool headless_ = false;
	bool ended_ = false;
	std::string kick_text_;
	bool timed_out_kick_ = false;
	Phase phase_ = Phase::Lobby;
	std::string connect_host_ = "127.0.0.1";
	uint16_t connect_port_ = 0;
	std::string netsim_ = "off";
	uint64_t seed_ = 1;
	std::optional<uint64_t> game_build_;
	std::optional<MatchLevel> first_level_;
	uint32_t level_index_ = 1;
	uint32_t levels_to_play_ = 1;
	std::optional<uint32_t> handled_round_;
	std::optional<uint8_t> rules_flags_;
	MatchLevel pending_;
	bool scene_game_ = false;
	bool scene_tdm_ = false;

	std::thread worker_;
	std::atomic<bool> stop_{ false };
	mutable std::mutex mutex_;
	std::shared_ptr<bf3::host::ClientSession> client_;
	std::string worker_error_;
	bf3::host::ServerSessionConfig server_config_;
	std::unique_ptr<bf3::host::ServerSession> server_;
	bf3::core::InstallReader install_;
	std::shared_ptr<GameSettings> settings_;
	std::unique_ptr<NativeKick> kick_;
};

}
