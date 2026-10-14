#pragma once
// Handle-relative file operations for the cache (internal). Every name below the cache root is a
// single path component opened relative to an already verified directory handle and never follows
// a reparse point, so a junction or symlink swapped into the tree cannot redirect an operation.
// Windows implementation: cache_fs_win32.cpp (NtCreateFile and NtSetInformationFile with a
// RootDirectory handle, FILE_OPEN_REPARSE_POINT, FileRenameInformationEx, FileDispositionInfoEx).
// A POSIX port maps onto openat(O_NOFOLLOW|O_DIRECTORY)/renameat/unlinkat.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::cache_detail {

// Our own error codes, in the customer range of Win32 error codes (bit 29).
inline constexpr uint32_t kErrReparsePoint = 0x20000001; // a link where a plain file/dir was expected
inline constexpr uint32_t kErrHardLinked = 0x20000002; // the target has more than one name
inline constexpr uint32_t kErrNotRegular = 0x20000003; // a directory where a file was expected
inline constexpr uint32_t kErrNtUnavailable = 0x20000004; // ntdll entry points missing

class Handle {
public:
	Handle() = default;
	explicit Handle(void *p_handle) :
			handle_(p_handle) {}
	~Handle() { reset(); }
	Handle(Handle &&p_other) noexcept :
			handle_(p_other.handle_) { p_other.handle_ = nullptr; }
	Handle &operator=(Handle &&p_other) noexcept {
		if (this != &p_other) {
			reset();
			handle_ = p_other.handle_;
			p_other.handle_ = nullptr;
		}
		return *this;
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;

	bool valid() const { return handle_ != nullptr; }
	void *get() const { return handle_; }
	void reset();

private:
	void *handle_ = nullptr;
};

struct FileInfo {
	uint64_t size = 0;
	uint32_t links = 0;
	uint32_t attributes = 0;
	bool is_directory() const { return (attributes & 0x10) != 0; } // FILE_ATTRIBUTE_DIRECTORY
	bool is_reparse() const { return (attributes & 0x400) != 0; } // FILE_ATTRIBUTE_REPARSE_POINT
};

struct DirEntry {
	std::wstring name;
	uint64_t size = 0;
	uint32_t attributes = 0;
	bool is_directory() const { return (attributes & 0x10) != 0; }
	bool is_reparse() const { return (attributes & 0x400) != 0; }
};

// True for one plain path component: no separators, no ':' (streams), no "."/"..", no trailing
// dot or space, no control or wildcard characters.
bool valid_component(std::wstring_view p_name);

// Opens an existing directory by absolute path, following links (only for the cache root and its
// ancestors, which may legitimately be redirected, e.g. a moved AppData).
Handle open_dir_path(const std::filesystem::path &p_path, uint32_t &r_err);
// Opens (or creates) a child directory; refuses reparse points (kErrReparsePoint). The handle does
// not share delete access, so the directory cannot be renamed or moved while it is held.
Handle open_child_dir(const Handle &p_parent, std::wstring_view p_name, bool p_create, uint32_t &r_err);
// Creates a new file for writing (fails if the name exists). Access includes DELETE for rename.
Handle create_child_file(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);
// Opens an existing regular file for reading (share read + delete); refuses reparse points.
Handle open_child_file_read(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);
// Opens an existing child (file or directory) for attribute queries and deletion, not following links.
Handle open_child_any(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);
// Opens (creating) a file for appending; p_share_write false = others may only read.
Handle open_child_append(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);
// Opens (creating) an exclusive lock file: no sharing at all (ERROR_SHARING_VIOLATION when held).
Handle open_child_lock(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

bool query_info(const Handle &p_handle, FileInfo &r_info, uint32_t &r_err);
// Final path of an open handle (links resolved, "\\?\" stripped), as GetFinalPathNameByHandleW.
std::optional<std::filesystem::path> final_path(const Handle &p_handle);

bool write_all(const Handle &p_file, const void *p_data, size_t p_size, uint32_t &r_err);
bool flush(const Handle &p_file, uint32_t &r_err);
// Reads exactly p_size bytes at offset 0.
bool read_prefix(const Handle &p_file, void *r_dst, size_t p_size, uint32_t &r_err);
bool read_all(const Handle &p_file, std::vector<uint8_t> &r_out, uint64_t p_max, uint32_t &r_err);
// Renames the open file to p_name inside p_dir (a single component relative to the directory
// handle, never a path), replacing an existing file; POSIX semantics where the volume has them.
// Calls NtSetInformationFile directly: kernel32's SetFileInformationByHandle rejects a non-NULL
// RootDirectory, and with a NULL one it resolves a bare name against the process's current
// directory, both of which would defeat the handle-relative design.
bool rename_into(const Handle &p_file, const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);
// Deletes the open file or empty directory (POSIX semantics when supported: the name goes now,
// open mappings stay readable).
bool delete_by_handle(const Handle &p_handle, uint32_t &r_err);

bool list_dir(const Handle &p_dir, std::vector<DirEntry> &r_out, uint32_t &r_err);

// Read-only mapping of a whole file (p_size > 0). The view outlives the handle.
const void *map_read(const Handle &p_file, uint64_t p_size, uint32_t &r_err);
void unmap(const void *p_view);

uint32_t current_process_id();

} // namespace bf3::core::cache_detail
