#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::cache_detail {

inline constexpr uint32_t kErrReparsePoint = 0x20000001;
inline constexpr uint32_t kErrHardLinked = 0x20000002;
inline constexpr uint32_t kErrNotRegular = 0x20000003;
inline constexpr uint32_t kErrNtUnavailable = 0x20000004;

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
	bool is_directory() const { return (attributes & 0x10) != 0; }
	bool is_reparse() const { return (attributes & 0x400) != 0; }
};

struct DirEntry {
	std::wstring name;
	uint64_t size = 0;
	uint32_t attributes = 0;
	bool is_directory() const { return (attributes & 0x10) != 0; }
	bool is_reparse() const { return (attributes & 0x400) != 0; }
};

bool valid_component(std::wstring_view p_name);

Handle open_dir_path(const std::filesystem::path &p_path, uint32_t &r_err);

Handle open_child_dir(const Handle &p_parent, std::wstring_view p_name, bool p_create, uint32_t &r_err);

Handle create_child_file(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

Handle open_child_file_read(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

Handle open_child_any(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

Handle open_child_append(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

Handle open_child_lock(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

bool query_info(const Handle &p_handle, FileInfo &r_info, uint32_t &r_err);

std::optional<std::filesystem::path> final_path(const Handle &p_handle);

bool write_all(const Handle &p_file, const void *p_data, size_t p_size, uint32_t &r_err);
bool flush(const Handle &p_file, uint32_t &r_err);

bool read_prefix(const Handle &p_file, void *r_dst, size_t p_size, uint32_t &r_err);
bool read_all(const Handle &p_file, std::vector<uint8_t> &r_out, uint64_t p_max, uint32_t &r_err);

bool rename_into(const Handle &p_file, const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err);

bool delete_by_handle(const Handle &p_handle, uint32_t &r_err);

bool list_dir(const Handle &p_dir, std::vector<DirEntry> &r_out, uint32_t &r_err);

const void *map_read(const Handle &p_file, uint64_t p_size, uint32_t &r_err);
void unmap(const void *p_view);

uint32_t current_process_id();

}
