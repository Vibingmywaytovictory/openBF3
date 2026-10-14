#include "dedicated_main_loop.h"

#include "godot_util.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <cstdlib>

namespace bf3::godot_side {

using namespace godot;

DedicatedMainLoop::~DedicatedMainLoop() {
	if (server_) {
		server_->stop();
	}
}

void DedicatedMainLoop::_initialize() {
	const std::string secs = user_arg_value("dedicated-seconds");
	run_seconds_ = secs.empty() ? 0.0 : std::atof(secs.c_str());
	const std::string bind = user_arg_value("bind");
	if (!bind.empty()) {
		bind_ip_ = bind;
	}
	const std::string port = user_arg_value("port");

	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	install_.locate(opts);
	UtilityFunctions::print(to_godot("[dedicated] engine " + engine_summary()));
	UtilityFunctions::print(to_godot("[dedicated] install " + install_.describe()));

	bf3::host::ServerConfig cfg;
	cfg.bind_ip = bind_ip_.c_str();
	cfg.port = static_cast<uint16_t>(port.empty() ? 0 : std::atoi(port.c_str()));
	server_ = std::make_unique<bf3::host::ServerHost>();
	std::string err;
	if (!server_->start(cfg, &err)) {
		UtilityFunctions::push_error(to_godot("[dedicated] ServerHost failed: " + err));
		failed_ = true;
		return;
	}
	UtilityFunctions::print(to_godot("[dedicated] ServerHost on " + bind_ip_ + ":" + std::to_string(server_->port()) + " at " +
			std::to_string(cfg.tick_hz) + " Hz" + (run_seconds_ > 0.0 ? ", for " + secs + " s" : ", until stopped")));
}

bool DedicatedMainLoop::_physics_process(double p_delta) {
	(void)p_delta;
	return false;
}

bool DedicatedMainLoop::_process(double p_delta) {
	if (failed_) {
		return true;
	}
	elapsed_ += p_delta;
	report_timer_ += p_delta;
	if (report_timer_ >= 1.0) {
		report_timer_ = 0.0;
		const bf3::host::ServerStats s = server_->stats();
		char buf[256];
		std::snprintf(buf, sizeof(buf), "[dedicated] t=%.1f s tick %llu (%.2f Hz) step p99 %.3f ms peers %u", elapsed_,
				static_cast<unsigned long long>(s.ticks), s.measured_hz, s.p99_step_ms, s.peers);
		UtilityFunctions::print(buf);
	}
	return run_seconds_ > 0.0 && elapsed_ >= run_seconds_;
}

void DedicatedMainLoop::_finalize() {
	if (server_) {
		const bf3::host::ServerStats s = server_->stats();
		server_->stop();
		char buf[256];
		std::snprintf(buf, sizeof(buf), "[dedicated] stopped after %.1f s: %llu ticks (%.2f Hz), overruns %llu, thread joined", elapsed_,
				static_cast<unsigned long long>(s.ticks), s.measured_hz, static_cast<unsigned long long>(s.overruns));
		UtilityFunctions::print(buf);
		server_.reset();
	}
}

} // namespace bf3::godot_side
