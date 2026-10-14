#pragma once
// A file of the BF3 install opened for reading only (plan §2.1). Only InstallReader can open one:
// it is the single place that turns install-relative names into paths.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace bf3::core {

class ReadOnlyFile {
public:
	ReadOnlyFile() = default;
	~ReadOnlyFile();
	ReadOnlyFile(ReadOnlyFile &&p_other) noexcept;
	ReadOnlyFile &operator=(ReadOnlyFile &&p_other) noexcept;
	ReadOnlyFile(const ReadOnlyFile &) = delete;
	ReadOnlyFile &operator=(const ReadOnlyFile &) = delete;

	bool is_open() const { return handle_ != nullptr; }
	uint64_t size() const { return size_; }

	// Positional read (does not depend on a shared file pointer, so several threads may read one
	// file). Returns the number of bytes read, or -1 on error.
	int64_t read_at(uint64_t p_offset, void *r_dst, size_t p_bytes) const;
	bool read_all(std::vector<uint8_t> &r_out) const;
	void close();

private:
	friend class InstallReader;
	// CreateFileW(GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING).
	static ReadOnlyFile open_absolute(const std::filesystem::path &p_path);

	void *handle_ = nullptr;
	uint64_t size_ = 0;
};

} // namespace bf3::core
