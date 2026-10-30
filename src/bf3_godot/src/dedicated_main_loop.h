#pragma once

#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/host/server_session.h"

#include <godot_cpp/classes/main_loop.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bf3::godot_side {

struct DedicatedArgs {
	std::string level;
	std::string mode;
	std::string maplist;
	std::string cache_root;
	std::string bind_ip = "127.0.0.1";
	uint32_t port = 0;
	std::string port_file;
	double run_seconds = 0.0;
	uint32_t stop_after_kills = 0;
	float intermission_s = -1.0f;
	float load_stall_s = -1.0f;
	bf3::host::ServerRules rules;
};

class DedicatedMainLoop : public godot::MainLoop {
	GDCLASS(DedicatedMainLoop, godot::MainLoop)

public:
	DedicatedMainLoop() = default;
	~DedicatedMainLoop() override;

	void _initialize() override;
	bool _process(double p_delta) override;
	bool _physics_process(double p_delta) override;
	void _finalize() override;

protected:
	static void _bind_methods() {}

private:
	bool parse_args(std::string &r_error);
	bool start(std::string &r_error);
	void report(const bf3::host::ServerSessionStats &p_stats);

	DedicatedArgs args_;
	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	std::shared_ptr<bf3::core::Cache> cache_;
	std::unique_ptr<bf3::host::ServerSession> session_;
	double elapsed_ = 0.0;
	double report_timer_ = 0.0;
	double load_seconds_ = 0.0;
	int32_t kills_ = 0;
	uint32_t stuck_ = 0;
	uint64_t private_bytes_ = 0;
	uint64_t peak_private_bytes_ = 0;
	int64_t resources_after_load_ = 0;
	uint64_t level_changes_seen_ = 0;
	const char *stopped_by_ = "the process";
	bool failed_ = false;
};

}
