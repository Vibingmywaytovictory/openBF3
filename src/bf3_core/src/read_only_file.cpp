#include "bf3/core/read_only_file.h"

#include <windows.h>

#include <algorithm>
#include <utility>

namespace bf3::core {

ReadOnlyFile::~ReadOnlyFile() {
	close();
}

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile &&p_other) noexcept :
		handle_(std::exchange(p_other.handle_, nullptr)), size_(std::exchange(p_other.size_, 0)) {}

ReadOnlyFile &ReadOnlyFile::operator=(ReadOnlyFile &&p_other) noexcept {
	if (this != &p_other) {
		close();
		handle_ = std::exchange(p_other.handle_, nullptr);
		size_ = std::exchange(p_other.size_, 0);
	}
	return *this;
}

void ReadOnlyFile::close() {
	if (handle_ != nullptr) {
		CloseHandle(static_cast<HANDLE>(handle_));
		handle_ = nullptr;
		size_ = 0;
	}
}

ReadOnlyFile ReadOnlyFile::open_absolute(const std::filesystem::path &p_path) {
	ReadOnlyFile file;

	HANDLE h = CreateFileW(p_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return file;
	}
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(h, &size)) {
		CloseHandle(h);
		return file;
	}
	file.handle_ = h;
	file.size_ = static_cast<uint64_t>(size.QuadPart);
	return file;
}

int64_t ReadOnlyFile::read_at(uint64_t p_offset, void *r_dst, size_t p_bytes) const {
	if (handle_ == nullptr) {
		return -1;
	}
	uint8_t *dst = static_cast<uint8_t *>(r_dst);
	uint64_t done = 0;
	while (done < p_bytes) {
		const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(p_bytes - done, 1u << 30));
		OVERLAPPED ov{};
		const uint64_t at = p_offset + done;
		ov.Offset = static_cast<DWORD>(at & 0xffffffffu);
		ov.OffsetHigh = static_cast<DWORD>(at >> 32);
		DWORD got = 0;
		if (!ReadFile(static_cast<HANDLE>(handle_), dst + done, chunk, &got, &ov)) {
			if (GetLastError() == ERROR_HANDLE_EOF) {
				break;
			}
			return -1;
		}
		if (got == 0) {
			break;
		}
		done += got;
	}
	return static_cast<int64_t>(done);
}

bool ReadOnlyFile::read_all(std::vector<uint8_t> &r_out) const {
	r_out.resize(static_cast<size_t>(size_));
	const int64_t got = read_at(0, r_out.data(), r_out.size());
	return got == static_cast<int64_t>(size_);
}

}
