#include "test_scratch.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace bf3::test {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::wstring_view kTrashName = L".trash";
constexpr std::wstring_view kSweepLock = L".sweep.lock";
constexpr std::wstring_view kCmakeTag = L"-cmake-";
constexpr std::wstring_view kLockSuffix = L".lock";
constexpr std::chrono::milliseconds kStartupSweepBudget{ 2000 };

std::atomic<uint32_t> g_next{ 0 };

struct ProcessScratch {
	std::mutex mutex;
	std::vector<fs::path> dirs;
	~ProcessScratch();
};

ProcessScratch &registry() {
	static ProcessScratch r;
	return r;
}

bool digits(std::wstring_view p_text) {
	if (p_text.empty() || p_text.size() > 10) {
		return false;
	}
	for (wchar_t c : p_text) {
		if (c < L'0' || c > L'9') {
			return false;
		}
	}
	return true;
}

bool hex8(std::wstring_view p_text) {
	if (p_text.size() != 8) {
		return false;
	}
	for (wchar_t c : p_text) {
		if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
			return false;
		}
	}
	return true;
}

bool ends_with(std::wstring_view p_text, std::wstring_view p_tail) {
	return p_text.size() >= p_tail.size() && p_text.substr(p_text.size() - p_tail.size()) == p_tail;
}

bool cmake_name(std::wstring_view p_name) {
	const size_t at = p_name.rfind(kCmakeTag);
	return at != std::wstring_view::npos && at > 0 && hex8(p_name.substr(at + kCmakeTag.size()));
}

bool parse_owner_pid(std::wstring_view p_name, uint32_t &r_pid) {
	const size_t last = p_name.rfind(L'-');
	if (last == std::wstring_view::npos || last == 0) {
		return false;
	}
	const size_t mid = p_name.rfind(L'-', last - 1);
	if (mid == std::wstring_view::npos || mid == 0) {
		return false;
	}
	const std::wstring_view pid_text = p_name.substr(mid + 1, last - mid - 1);
	if (!digits(pid_text) || !digits(p_name.substr(last + 1))) {
		return false;
	}
	uint64_t pid = 0;
	for (wchar_t c : pid_text) {
		pid = pid * 10 + uint64_t(c - L'0');
	}
	if (pid > UINT32_MAX) {
		return false;
	}
	r_pid = uint32_t(pid);
	return true;
}

bool missing(const fs::path &p_path) {
	if (GetFileAttributesW(p_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
		return false;
	}
	const DWORD e = GetLastError();
	return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

bool remove_tree(const fs::path &p_dir) {
	for (int attempt = 0; attempt < 5; ++attempt) {
		std::error_code ec;
		fs::remove_all(p_dir, ec);
		if (missing(p_dir)) {
			return true;
		}
		std::this_thread::sleep_for(20ms);
	}
	std::fprintf(stderr, "test scratch: could not remove %ls\n", p_dir.c_str());
	return false;
}

void remove_root_if_empty(const fs::path &p_root) {
	RemoveDirectoryW(p_root.c_str());
}

ProcessScratch::~ProcessScratch() {
	const std::lock_guard<std::mutex> lock(mutex);
	for (const fs::path &d : dirs) {
		remove_tree(d);
	}
	if (!dirs.empty()) {
		remove_root_if_empty(dirs.front().parent_path());
	}
}

ScratchOwner pid_owner(uint32_t p_pid, const fs::path &p_entry) {
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesExW(p_entry.c_str(), GetFileExInfoStandard, &data)) {
		return ScratchOwner::Unknown;
	}
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, p_pid);
	if (h == nullptr) {
		return GetLastError() == ERROR_INVALID_PARAMETER ? ScratchOwner::Dead : ScratchOwner::Alive;
	}
	ScratchOwner owner = ScratchOwner::Alive;
	FILETIME start{};
	FILETIME end{};
	FILETIME kernel{};
	FILETIME user{};
	if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
		owner = ScratchOwner::Dead;
	} else if (GetProcessTimes(h, &start, &end, &kernel, &user) && CompareFileTime(&start, &data.ftCreationTime) > 0) {
		owner = ScratchOwner::Dead;
	}
	CloseHandle(h);
	return owner;
}

ScratchOwner lock_owner(const fs::path &p_lock) {
	HANDLE h = CreateFileW(p_lock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		const DWORD e = GetLastError();
		return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ScratchOwner::Dead : ScratchOwner::Alive;
	}
	OVERLAPPED at{};
	const bool locked = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &at);
	if (locked) {
		UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &at);
	}
	CloseHandle(h);
	return locked ? ScratchOwner::Dead : ScratchOwner::Alive;
}

fs::path with_lock_suffix(const fs::path &p_dir) {
	fs::path lock = p_dir;
	lock += kLockSuffix;
	return lock;
}

void stamp_created_now(const fs::path &p_dir) {
	HANDLE h = CreateFileW(p_dir.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	FILETIME now{};
	GetSystemTimeAsFileTime(&now);
	SetFileTime(h, &now, nullptr, nullptr);
	CloseHandle(h);
}

std::wstring checked_name(std::string_view p_name) {
	std::wstring out;
	for (char c : p_name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
				c == '_' || c == '.';
		if (!ok) {
			std::fprintf(stderr, "test scratch: '%.*s' is not a scratch name ([A-Za-z0-9._-])\n", int(p_name.size()),
					p_name.data());
			std::abort();
		}
		out += wchar_t(c);
	}
	if (out.empty() || out.front() == L'.') {
		std::fprintf(stderr, "test scratch: a scratch name is not empty and does not start with '.'\n");
		std::abort();
	}
	return out;
}

fs::path create_scratch(std::string_view p_name) {
	scratch_startup();
	const std::wstring base = checked_name(p_name) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-";
	const fs::path root = scratch_root();
	for (int attempt = 0; attempt < 64; ++attempt) {
		std::error_code ec;
		fs::create_directories(root, ec);
		const fs::path dir = root / (base + std::to_wstring(g_next++));
		if (CreateDirectoryW(dir.c_str(), nullptr)) {
			stamp_created_now(dir);
			return dir;
		}
		std::this_thread::yield();
	}
	std::fprintf(stderr, "test scratch: could not create a folder under %ls\n", root.c_str());
	std::abort();
}

fs::path claim(const fs::path &p_entry, const fs::path &p_root) {
	const fs::path trash = p_root /
			(std::wstring(kTrashName) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
					std::to_wstring(g_next++));
	if (!MoveFileExW(p_entry.c_str(), trash.c_str(), 0)) {
		return {};
	}
	return trash;
}

bool older_than(const fs::path &p_path, std::chrono::seconds p_age) {
	std::error_code ec;
	const fs::file_time_type t = fs::last_write_time(p_path, ec);
	return !ec && fs::file_time_type::clock::now() - t > p_age;
}

std::vector<fs::path> list(const fs::path &p_dir) {
	std::vector<fs::path> out;
	std::error_code ec;
	fs::directory_iterator it(p_dir, ec);
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
		out.push_back(it->path());
	}
	return out;
}

std::string ascii_name(const fs::path &p_path) {
	const std::wstring name = p_path.filename().native();
	std::string out;
	for (wchar_t c : name) {
		out += c < 0x80 ? char(c) : '?';
	}
	return out;
}

bool is_dir(const fs::path &p_path) {
	const DWORD a = GetFileAttributesW(p_path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}

fs::path temp_dir() {
	wchar_t buffer[MAX_PATH + 1] = {};
	const DWORD n = GetTempPathW(MAX_PATH + 1, buffer);
	if (n == 0 || n > MAX_PATH) {
		std::fprintf(stderr, "test scratch: no temp directory\n");
		std::abort();
	}
	return fs::path(buffer).parent_path();
}

fs::path scratch_root() {
	return temp_dir() / kScratchRootName;
}

ScratchDir::ScratchDir(std::string_view p_name) : path_(create_scratch(p_name)) {}

ScratchDir::~ScratchDir() {
	if (!path_.empty()) {
		remove_tree(path_);
		remove_root_if_empty(path_.parent_path());
	}
}

ScratchDir::ScratchDir(ScratchDir &&p_other) noexcept : path_(std::move(p_other.path_)) {
	p_other.path_.clear();
}

ScratchDir &ScratchDir::operator=(ScratchDir &&p_other) noexcept {
	if (this != &p_other) {
		if (!path_.empty()) {
			remove_tree(path_);
		}
		path_ = std::move(p_other.path_);
		p_other.path_.clear();
	}
	return *this;
}

fs::path process_scratch(std::string_view p_name) {
	const fs::path dir = create_scratch(p_name);
	ProcessScratch &r = registry();
	const std::lock_guard<std::mutex> lock(r.mutex);
	r.dirs.push_back(dir);
	return dir;
}

ScratchOwner scratch_owner(const fs::path &p_entry) {
	const std::wstring name = p_entry.filename().native();
	if (cmake_name(name)) {
		return lock_owner(with_lock_suffix(p_entry));
	}
	if (ends_with(name, kLockSuffix) &&
			cmake_name(std::wstring_view(name).substr(0, name.size() - kLockSuffix.size()))) {
		return lock_owner(p_entry);
	}
	uint32_t pid = 0;
	if (parse_owner_pid(name, pid)) {
		return pid_owner(pid, p_entry);
	}
	return ScratchOwner::Unknown;
}

bool is_legacy_scratch_name(std::string_view p_name) {
	const bool prefixed = p_name.rfind("openbf3-", 0) == 0 || p_name.rfind("openbf3_", 0) == 0;
	if (prefixed) {
		return p_name != kScratchRootName;
	}
	if (p_name.size() < 5 || p_name.rfind("o3", 0) != 0 || p_name[2] < 'a' || p_name[2] > 'z' || p_name[3] != '-') {
		return false;
	}
	for (char c : p_name.substr(4)) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) {
			return false;
		}
	}
	return true;
}

SweepReport sweep_scratch(
		const fs::path &p_temp, std::chrono::seconds p_legacy_age, std::chrono::milliseconds p_budget) {
	SweepReport report;
	const fs::path root = p_temp / kScratchRootName;
	std::error_code ec;
	fs::create_directories(root, ec);
	HANDLE lock = CreateFileW((root / kSweepLock).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (lock == INVALID_HANDLE_VALUE) {
		return report;
	}
	report.ran = true;
	const auto deadline = std::chrono::steady_clock::now() + p_budget;
	auto take = [&](const fs::path &p_entry) {
		if (std::chrono::steady_clock::now() > deadline) {
			report.kept.push_back(p_entry);
			return;
		}
		const fs::path trash = claim(p_entry, root);
		if (trash.empty()) {
			report.kept.push_back(p_entry);
			return;
		}
		remove_tree(trash);
		if (cmake_name(p_entry.filename().native())) {
			DeleteFileW(with_lock_suffix(p_entry).c_str());
		}
		report.removed.push_back(p_entry);
	};
	std::vector<fs::path> dead;
	for (const fs::path &entry : list(root)) {
		const std::wstring name = entry.filename().native();
		if (name == kSweepLock) {
			continue;
		}
		if (!is_dir(entry)) {
			if (ends_with(name, kLockSuffix) && missing(fs::path(entry).replace_extension()) &&
					scratch_owner(entry) == ScratchOwner::Dead) {
				DeleteFileW(entry.c_str());
			}
			continue;
		}
		const ScratchOwner owner = scratch_owner(entry);
		if (owner == ScratchOwner::Dead || (owner == ScratchOwner::Unknown && older_than(entry, p_legacy_age))) {
			dead.push_back(entry);
		} else {
			report.kept.push_back(entry);
		}
	}
	for (const fs::path &entry : dead) {
		take(entry);
	}
	for (const fs::path &entry : list(p_temp)) {
		if (is_dir(entry) && is_legacy_scratch_name(ascii_name(entry)) && older_than(entry, p_legacy_age)) {
			take(entry);
		}
	}
	CloseHandle(lock);
	remove_root_if_empty(root);
	return report;
}

void scratch_startup() {
	static std::once_flag once;
	std::call_once(once, [] {
		registry();
		sweep_scratch(temp_dir(), kLegacyScratchAge, kStartupSweepBudget);
	});
}

}
