#pragma once

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

	int64_t read_at(uint64_t p_offset, void *r_dst, size_t p_bytes) const;
	bool read_all(std::vector<uint8_t> &r_out) const;
	void close();

private:
	friend class InstallReader;

	static ReadOnlyFile open_absolute(const std::filesystem::path &p_path);

	void *handle_ = nullptr;
	uint64_t size_ = 0;
};

}
