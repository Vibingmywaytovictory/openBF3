#include "net_procs_driver.h"

#include "test_support.h"
#include "win32_text.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

namespace bf3::test::procs {

namespace {

using core::detail::env_value;
using core::detail::path_utf8;
using core::detail::widen;

constexpr std::chrono::milliseconds kSettleTimeout{ 2000 };
constexpr std::chrono::milliseconds kKillTimeout{ 10000 };
constexpr std::chrono::milliseconds kJobPoll{ 1 };

std::wstring quote_arg(const std::wstring &p_arg) {
	if (!p_arg.empty() && p_arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		return p_arg;
	}
	std::wstring out = L"\"";
	for (size_t i = 0;; ++i) {
		size_t backslashes = 0;
		while (i < p_arg.size() && p_arg[i] == L'\\') {
			++i;
			++backslashes;
		}
		if (i == p_arg.size()) {
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (p_arg[i] == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
		} else {
			out.append(backslashes, L'\\');
		}
		out.push_back(p_arg[i]);
	}
	out.push_back(L'"');
	return out;
}

bool valid_role(std::string_view p_role) {
	return !p_role.empty() && std::all_of(p_role.begin(), p_role.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
	});
}

struct GodotArgs {
	std::vector<std::string> engine;
	std::vector<std::string> user;
};

GodotArgs split_godot_args(const std::vector<std::string> &p_args) {
	GodotArgs out;
	const auto dash = std::find(p_args.begin(), p_args.end(), std::string("--"));
	out.engine.assign(p_args.begin(), dash);
	if (dash != p_args.end()) {
		out.user.assign(dash + 1, p_args.end());
	}
	return out;
}

bool godot_windowed(const ChildSpec &p_spec) {
	const GodotArgs split = split_godot_args(p_spec.args);
	return std::find(split.engine.begin(), split.engine.end(), std::string("--headless")) == split.engine.end();
}

size_t count_prefixed(const std::vector<std::string> &p_args, std::string_view p_prefix) {
	return static_cast<size_t>(std::count_if(
			p_args.begin(), p_args.end(), [&](const std::string &a) { return a.rfind(p_prefix, 0) == 0; }));
}

bool has_exactly(const std::vector<std::string> &p_args, std::string_view p_prefix, const std::string &p_whole) {
	return count_prefixed(p_args, p_prefix) == 1 && std::find(p_args.begin(), p_args.end(), p_whole) != p_args.end();
}

bool has_pair(const std::vector<std::string> &p_args, std::string_view p_flag, const std::string &p_value) {
	size_t seen = 0;
	bool matched = false;
	for (size_t i = 0; i < p_args.size(); ++i) {
		if (p_args[i] == p_flag) {
			++seen;
			matched = i + 1 < p_args.size() && p_args[i + 1] == p_value;
		}
	}
	return seen == 1 && matched;
}

JOBOBJECT_BASIC_ACCOUNTING_INFORMATION job_accounting(HANDLE p_job) {
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
	QueryInformationJobObject(p_job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr);
	return info;
}

bool wait_job_empty(HANDLE p_job, std::chrono::milliseconds p_timeout) {
	return wait_until([&] { return job_accounting(p_job).ActiveProcesses == 0; }, p_timeout, kJobPoll);
}

}

struct ProcDriver::Child {
	std::string role;
	HANDLE process = nullptr;
	HANDLE job = nullptr;
	HANDLE read_end = nullptr;
	uint32_t pid = 0;
	std::thread reader;
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::vector<std::string> lines;
	std::string partial;
	std::string text;
	size_t cursor = 0;
	bool eof = false;
	bool finished = false;
	ChildEnd end;

	void read_output() {
		char buf[4096];
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(read_end, buf, sizeof(buf), &got, nullptr) || got == 0) {
				break;
			}
			std::lock_guard lock(mutex);
			text.append(buf, got);
			partial.append(buf, got);
			for (size_t nl = partial.find('\n'); nl != std::string::npos; nl = partial.find('\n')) {
				std::string line = partial.substr(0, nl);
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				lines.push_back(std::move(line));
				partial.erase(0, nl + 1);
			}
			changed.notify_all();
		}
		std::lock_guard lock(mutex);
		if (!partial.empty()) {
			if (partial.back() == '\r') {
				partial.pop_back();
			}
			lines.push_back(std::move(partial));
			partial.clear();
		}
		eof = true;
		changed.notify_all();
	}

	void kill_and_join() {
		if (job != nullptr) {
			TerminateJobObject(job, kKilledExitCode);
			WaitForSingleObject(process, static_cast<DWORD>(kKillTimeout.count()));
			wait_job_empty(job, kKillTimeout);
		}
		if (reader.joinable()) {
			reader.join();
		}
	}

	~Child() {
		kill_and_join();
		for (HANDLE h : { process, job, read_end }) {
			if (h != nullptr) {
				CloseHandle(h);
			}
		}
	}
};

DriverConfig driver_config_from_env() {
	DriverConfig c;
	c.row = env_value("OPENBF3_PROC_ROW");
	c.root = widen(env_value("OPENBF3_PROC_ROOT"));
	c.godot_exe = widen(env_value("OPENBF3_GODOT"));
	if (c.godot_exe.empty()) {
		c.godot_exe = widen(env_value("OPENBF3_PROC_GODOT_EXE"));
	}
	c.godot_project = widen(env_value("OPENBF3_PROC_GODOT_PROJECT"));
	c.bf3dump = widen(env_value("OPENBF3_PROC_BF3DUMP"));
	c.window_row = env_value("OPENBF3_PROC_WINDOW_ROW") == "1";
	c.windowed_allowed = env_value("OPENBF3_GODOT_WINDOWED") == "1";
	c.user_active_flag = widen(env_value("OPENBF3_USER_ACTIVE_FLAG"));
	for (std::filesystem::path *p : { &c.root, &c.godot_exe, &c.godot_project, &c.bf3dump, &c.user_active_flag }) {
		p->make_preferred();
	}
	return c;
}

std::wstring command_line(const std::filesystem::path &p_exe, const std::vector<std::string> &p_args) {
	std::wstring line = quote_arg(p_exe.native());
	for (const std::string &a : p_args) {
		line += L' ';
		line += quote_arg(widen(a));
	}
	return line;
}

ProcDriver::ProcDriver(DriverConfig p_config) : config_(std::move(p_config)) {}

ProcDriver::~ProcDriver() = default;

ChildRoots ProcDriver::roots(std::string_view p_role) const {
	ChildRoots r;
	r.dir = config_.root / widen(config_.row + "-" + std::string(p_role));
	r.settings_file = r.dir / L"settings.cfg";
	r.log_file = r.dir / L"godot.log";
	r.cache = r.dir / L"cache";
	return r;
}

ChildSpec ProcDriver::godot_child(
		std::string_view p_role, std::vector<std::string> p_engine_args, std::vector<std::string> p_user_args) const {
	const ChildRoots r = roots(p_role);
	ChildSpec spec;
	spec.role = std::string(p_role);
	spec.kind = ChildKind::Godot;
	spec.exe = config_.godot_exe;
	spec.args = { "--path", path_utf8(config_.godot_project) };
	spec.args.insert(spec.args.end(), p_engine_args.begin(), p_engine_args.end());
	spec.args.insert(spec.args.end(), { "--log-file", path_utf8(r.log_file), "--" });
	spec.args.insert(spec.args.end(), p_user_args.begin(), p_user_args.end());
	spec.args.push_back("--bf3-cache=" + path_utf8(r.cache));
	spec.args.push_back("--settings-file=" + path_utf8(r.settings_file));
	return spec;
}

ChildSpec ProcDriver::tool_child(
		std::string_view p_role, std::filesystem::path p_exe, std::vector<std::string> p_args) const {
	ChildSpec spec;
	spec.role = std::string(p_role);
	spec.kind = ChildKind::Tool;
	spec.exe = std::move(p_exe);
	spec.args = std::move(p_args);
	return spec;
}

std::string ProcDriver::skip_reason(const ChildSpec &p_spec) const {
	if (p_spec.kind != ChildKind::Godot) {
		return {};
	}
	std::error_code ec;
	if (config_.godot_exe.empty() || !std::filesystem::exists(config_.godot_exe, ec)) {
		return "no Godot console executable (OPENBF3_GODOT unset and none found next to the checkout at configure "
			   "time)";
	}
	if (config_.godot_project.empty() || !std::filesystem::exists(config_.godot_project / L"project.godot", ec)) {
		return "no Godot project: the Godot rows are off in this configuration";
	}
	if (godot_windowed(p_spec)) {
		if (!config_.windowed_allowed) {
			return "a windowed Godot child; it runs only with OPENBF3_GODOT_WINDOWED=1 (the merge pass's window phase)";
		}
		if (!config_.user_active_flag.empty() && std::filesystem::exists(config_.user_active_flag, ec)) {
			return "the user is active (" + path_utf8(config_.user_active_flag) +
					" exists); windowed children wait for the user-away tenure";
		}
	}
	return {};
}

core::Status ProcDriver::check_spec(const ChildSpec &p_spec) const {
	if (config_.row.empty() || config_.root.empty()) {
		return core::make_error("run through ctest: openbf3_proc_row sets OPENBF3_PROC_ROW and OPENBF3_PROC_ROOT");
	}
	if (!valid_role(p_spec.role)) {
		return core::make_error("child role '" + p_spec.role + "' is not [a-z0-9_]+");
	}
	for (const auto &c : children_) {
		if (c->role == p_spec.role && !c->finished) {
			return core::make_error(p_spec.role + ": a child of this role is still running; its roots are in use");
		}
	}
	for (const std::string &a : p_spec.args) {
		if (a.find("user://") != std::string::npos) {
			return core::make_error(
					p_spec.role + ": '" + a + "' names user://, which every worktree and the owner share");
		}
	}
	const ChildRoots r = roots(p_spec.role);
	if (p_spec.kind == ChildKind::Tool) {
		for (std::string_view flag : { std::string_view("--cache="), std::string_view("--bf3-cache=") }) {
			for (const std::string &a : p_spec.args) {
				if (a.rfind(flag, 0) == 0 && a != std::string(flag) + path_utf8(r.cache)) {
					return core::make_error(
							p_spec.role + ": '" + a + "' is not the child's own cache root " + path_utf8(r.cache));
				}
			}
		}
		return core::Status::success();
	}
	const GodotArgs split = split_godot_args(p_spec.args);
	const bool own_roots = has_pair(split.engine, "--path", path_utf8(config_.godot_project)) &&
			has_pair(split.engine, "--log-file", path_utf8(r.log_file)) &&
			has_exactly(split.user, "--bf3-cache=", "--bf3-cache=" + path_utf8(r.cache)) &&
			has_exactly(split.user, "--settings-file=", "--settings-file=" + path_utf8(r.settings_file)) &&
			count_prefixed(split.user, "--log-file") == 0 && count_prefixed(split.engine, "--bf3-cache") == 0 &&
			count_prefixed(split.engine, "--settings-file") == 0;
	if (!own_roots) {
		return core::make_error(p_spec.role + ": a Godot child needs --path " + path_utf8(config_.godot_project) +
				" and its own --log-file, --bf3-cache and --settings-file under " + path_utf8(r.dir) +
				" (ProcDriver::godot_child)");
	}
	if (godot_windowed(p_spec) && !config_.window_row) {
		return core::make_error(
				p_spec.role + ": a windowed Godot child needs a window row (openbf3_proc_row LABEL window)");
	}
	return core::Status::success();
}

core::Result<size_t> ProcDriver::start(const ChildSpec &p_spec) {
	if (core::Status checked = check_spec(p_spec); !checked.ok()) {
		return checked.error();
	}
	if (const std::string why = skip_reason(p_spec); !why.empty()) {
		return core::make_error(p_spec.role + ": skipped: " + why);
	}
	std::error_code ec;
	if (p_spec.exe.empty() || !std::filesystem::exists(p_spec.exe, ec)) {
		return core::make_error(p_spec.role + ": no executable at '" + path_utf8(p_spec.exe) + "'");
	}
	const ChildRoots r = roots(p_spec.role);
	std::filesystem::create_directories(r.dir, ec);
	if (ec) {
		return core::make_error(p_spec.role + ": cannot create " + path_utf8(r.dir) + ": " + ec.message());
	}
	if (p_spec.kind == ChildKind::Godot) {
		std::filesystem::remove(r.settings_file, ec);
		std::filesystem::remove(r.log_file, ec);
		std::filesystem::create_directories(r.cache, ec);
		if (!p_spec.settings.empty()) {
			std::ofstream seed(r.settings_file, std::ios::binary);
			for (const std::string &line : p_spec.settings) {
				seed << line << '\n';
			}
			if (!seed) {
				return core::make_error(p_spec.role + ": cannot write " + path_utf8(r.settings_file));
			}
		}
	}

	auto child = std::make_unique<Child>();
	child->role = p_spec.role;
	SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
	HANDLE write_end = nullptr;
	if (!CreatePipe(&child->read_end, &write_end, &inherit, 0)) {
		return core::make_error(p_spec.role + ": CreatePipe failed (" + std::to_string(GetLastError()) + ")");
	}
	SetHandleInformation(child->read_end, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul =
			CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
	child->job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	const bool job_ok = child->job != nullptr &&
			SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

	SIZE_T attr_bytes = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_bytes);
	std::vector<unsigned char> attr_storage(attr_bytes);
	auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
	HANDLE inherited[2] = { write_end, nul };
	const bool attrs_ok = nul != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_bytes) &&
			UpdateProcThreadAttribute(
					attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = nul;
	si.StartupInfo.hStdOutput = write_end;
	si.StartupInfo.hStdError = write_end;
	si.lpAttributeList = attrs;
	std::wstring line = command_line(p_spec.exe, p_spec.args);
	PROCESS_INFORMATION pi{};
	const bool created = job_ok && attrs_ok &&
			CreateProcessW(p_spec.exe.c_str(), line.data(), nullptr, nullptr, TRUE,
					CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, r.dir.c_str(),
					&si.StartupInfo, &pi);
	const DWORD create_error = GetLastError();
	if (attrs_ok) {
		DeleteProcThreadAttributeList(attrs);
	}
	CloseHandle(write_end);
	if (nul != INVALID_HANDLE_VALUE) {
		CloseHandle(nul);
	}
	if (!created) {
		return core::make_error(
				p_spec.role + ": cannot start " + path_utf8(p_spec.exe) + " (" + std::to_string(create_error) + ")");
	}
	child->process = pi.hProcess;
	child->pid = pi.dwProcessId;
	if (!AssignProcessToJobObject(child->job, pi.hProcess)) {
		const DWORD assign_error = GetLastError();
		TerminateProcess(pi.hProcess, kKilledExitCode);
		CloseHandle(pi.hThread);
		return core::make_error(
				p_spec.role + ": cannot put the child in its job (" + std::to_string(assign_error) + ")");
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	Child *raw = child.get();
	child->reader = std::thread([raw] { raw->read_output(); });
	children_.push_back(std::move(child));
	return children_.size() - 1;
}

bool ProcDriver::wait_line(size_t p_child, const std::function<bool(std::string_view)> &p_match,
		std::chrono::milliseconds p_timeout, std::string *r_line) {
	Child &c = *children_.at(p_child);
	const auto deadline = std::chrono::steady_clock::now() + p_timeout;
	std::unique_lock lock(c.mutex);
	for (;;) {
		while (c.cursor < c.lines.size()) {
			const std::string &line = c.lines[c.cursor++];
			if (p_match(line)) {
				if (r_line != nullptr) {
					*r_line = line;
				}
				return true;
			}
		}
		if (c.eof) {
			return false;
		}
		if (c.changed.wait_until(lock, deadline) == std::cv_status::timeout && c.cursor >= c.lines.size()) {
			return false;
		}
	}
}

ChildEnd ProcDriver::finish(size_t p_child, std::chrono::milliseconds p_timeout) {
	Child &c = *children_.at(p_child);
	if (c.finished) {
		return c.end;
	}
	ChildEnd end;
	if (WaitForSingleObject(c.process, static_cast<DWORD>(p_timeout.count())) != WAIT_OBJECT_0) {
		TerminateJobObject(c.job, kKilledExitCode);
		WaitForSingleObject(c.process, static_cast<DWORD>(kKillTimeout.count()));
		end.killed = true;
	} else if (!wait_job_empty(c.job, kSettleTimeout)) {
		end.stragglers = job_accounting(c.job).ActiveProcesses;
	}
	c.kill_and_join();
	DWORD code = 0;
	GetExitCodeProcess(c.process, &code);
	end.code = code;
	end.exited = !end.killed;
	const JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc = job_accounting(c.job);
	end.cpu_seconds = static_cast<double>(acc.TotalUserTime.QuadPart + acc.TotalKernelTime.QuadPart) * 1e-7;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	if (QueryInformationJobObject(c.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr)) {
		end.peak_job_bytes = limits.PeakJobMemoryUsed;
	}
	c.end = end;
	c.finished = true;
	return end;
}

uint32_t ProcDriver::job_processes(size_t p_child) const {
	return job_accounting(children_.at(p_child)->job).ActiveProcesses;
}

std::vector<uint32_t> ProcDriver::job_pids(size_t p_child) const {
	HANDLE job = children_.at(p_child)->job;
	std::vector<ULONG_PTR> storage(2 + 64);
	for (;;) {
		auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(storage.data());
		const DWORD bytes = static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR));
		if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, bytes, nullptr) ||
				GetLastError() != ERROR_MORE_DATA) {
			std::vector<uint32_t> pids;
			for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
				pids.push_back(static_cast<uint32_t>(list->ProcessIdList[i]));
			}
			return pids;
		}
		storage.resize(2 + static_cast<size_t>(list->NumberOfAssignedProcesses) + 64);
	}
}

uint32_t ProcDriver::pid(size_t p_child) const {
	return children_.at(p_child)->pid;
}

std::string ProcDriver::output(size_t p_child) const {
	const Child &c = *children_.at(p_child);
	std::lock_guard lock(c.mutex);
	return c.text;
}

}
