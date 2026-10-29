#pragma once

#include "bf3/core/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::test::procs {

constexpr uint32_t kKilledExitCode = 124;

enum class ChildKind : uint8_t {
	Tool,
	Godot,
};

struct ChildRoots {
	std::filesystem::path dir;
	std::filesystem::path settings_file;
	std::filesystem::path log_file;
	std::filesystem::path cache;
};

struct ChildSpec {
	std::string role;
	ChildKind kind = ChildKind::Tool;
	std::filesystem::path exe;
	std::vector<std::string> args;
	std::vector<std::string> settings;
};

struct ChildEnd {
	bool exited = false;
	bool killed = false;
	uint32_t code = 0;
	uint32_t stragglers = 0;
	double cpu_seconds = 0.0;
	uint64_t peak_job_bytes = 0;
};

struct DriverConfig {
	std::string row;
	std::filesystem::path root;
	std::filesystem::path godot_exe;
	std::filesystem::path godot_project;
	std::filesystem::path bf3dump;
	bool window_row = false;
	bool windowed_allowed = false;
	std::filesystem::path user_active_flag;
};

DriverConfig driver_config_from_env();
std::wstring command_line(const std::filesystem::path &p_exe, const std::vector<std::string> &p_args);

class ProcDriver {
public:
	explicit ProcDriver(DriverConfig p_config);
	~ProcDriver();
	ProcDriver(const ProcDriver &) = delete;
	ProcDriver &operator=(const ProcDriver &) = delete;

	const DriverConfig &config() const { return config_; }
	ChildRoots roots(std::string_view p_role) const;
	ChildSpec godot_child(std::string_view p_role, std::vector<std::string> p_engine_args,
			std::vector<std::string> p_user_args) const;
	ChildSpec tool_child(std::string_view p_role, std::filesystem::path p_exe, std::vector<std::string> p_args) const;
	std::string skip_reason(const ChildSpec &p_spec) const;
	core::Result<size_t> start(const ChildSpec &p_spec);
	bool wait_line(size_t p_child, const std::function<bool(std::string_view)> &p_match,
			std::chrono::milliseconds p_timeout, std::string *r_line = nullptr);
	ChildEnd finish(size_t p_child, std::chrono::milliseconds p_timeout);
	uint32_t job_processes(size_t p_child) const;
	std::vector<uint32_t> job_pids(size_t p_child) const;
	uint32_t pid(size_t p_child) const;
	std::string output(size_t p_child) const;

private:
	struct Child;
	core::Status check_spec(const ChildSpec &p_spec) const;

	DriverConfig config_;
	std::vector<std::unique_ptr<Child>> children_;
};

}
