#pragma once
// DedicatedMainLoop (plan §2.6): the entry of a dedicated server under `godot --headless`.
//   godot --headless --path <project> --main-loop DedicatedMainLoop -- [--dedicated-seconds=N]
//         [--port=N] [--bind=127.0.0.1] [--bf3-install=<dir>]
// No scene tree, no rendering: it mounts the install read-only and runs ServerHost until
// N seconds have passed (0 = until the process is stopped), printing stats once per second.

#include "bf3/core/install_reader.h"
#include "bf3/host/server_host.h"

#include <godot_cpp/classes/main_loop.hpp>

#include <memory>
#include <string>

namespace bf3::godot_side {

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
	bf3::core::InstallReader install_;
	std::unique_ptr<bf3::host::ServerHost> server_;
	std::string bind_ip_ = "127.0.0.1";
	double run_seconds_ = 0.0;
	double elapsed_ = 0.0;
	double report_timer_ = 0.0;
	bool failed_ = false;
};

} // namespace bf3::godot_side
