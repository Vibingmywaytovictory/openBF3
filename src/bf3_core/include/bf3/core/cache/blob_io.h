#pragma once
// Little-endian blob writer and bounds-checked reader for the converters' cache payloads (plan §2.4 step 3:
// "render meshes -> Godot surface blobs", "resolved WeaponDef ... blobs"). A blob starts with an 8-byte magic
// and a u32 format version, chosen by the converter; everything after is the converter's own layout.
//
// BlobReader never reads past its span and never allocates more than the bytes still unread can describe:
// every count is checked against the remaining bytes (count x the element's encoded size) before a vector is
// sized, and every string against its own limit. A failed read is sticky (ok() stays false, later reads return
// zeros), so a decoder can read a whole record and check ok() once; the first failure's offset and reason are
// kept for the error message.

#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::core {

inline constexpr size_t kBlobMagicSize = 8;

class BlobWriter {
public:
	void magic(std::string_view p_magic, uint32_t p_version); // p_magic: exactly 8 characters
	void u8(uint8_t p_v) { bytes_.push_back(p_v); }
	void u16(uint16_t p_v) { put(&p_v, 2); }
	void u32(uint32_t p_v) { put(&p_v, 4); }
	void i32(int32_t p_v) { put(&p_v, 4); }
	void u64(uint64_t p_v) { put(&p_v, 8); }
	void f32(float p_v) { put(&p_v, 4); }
	void boolean(bool p_v) { u8(p_v ? 1 : 0); }
	void str(std::string_view p_text); // u32 length + bytes
	void raw(const void *p_bytes, size_t p_size) { put(p_bytes, p_size); }
	// Pads with zeros to a multiple of p_alignment bytes from the blob start.
	void align(size_t p_alignment);
	template <class T>
	void array(const std::vector<T> &p_values) { // raw little-endian elements, no count
		static_assert(std::is_trivially_copyable_v<T>);
		put(p_values.data(), p_values.size() * sizeof(T));
	}

	size_t size() const { return bytes_.size(); }
	std::vector<uint8_t> take() { return std::move(bytes_); }

private:
	void put(const void *p_bytes, size_t p_size);
	std::vector<uint8_t> bytes_;
};

class BlobReader {
public:
	explicit BlobReader(std::span<const uint8_t> p_bytes) :
			bytes_(p_bytes) {}

	// Checks the magic and returns the version (0 after a failure).
	uint32_t magic(std::string_view p_magic);
	uint8_t u8();
	uint16_t u16();
	uint32_t u32();
	int32_t i32();
	uint64_t u64();
	float f32();
	bool boolean(); // 0 or 1; anything else fails
	// u32 length (at most p_max_length) + bytes.
	std::string str(size_t p_max_length);
	void align(size_t p_alignment);
	// p_count raw elements; fails (nothing allocated) when fewer than p_count * sizeof(T) bytes remain.
	template <class T>
	bool array(size_t p_count, std::vector<T> &r_values) {
		static_assert(std::is_trivially_copyable_v<T>);
		r_values.clear();
		if (!ok_) {
			return false;
		}
		if (p_count > remaining() / sizeof(T)) {
			fail("array of " + std::to_string(p_count) + " x " + std::to_string(sizeof(T)) + " bytes past the end");
			return false;
		}
		r_values.resize(p_count);
		std::memcpy(r_values.data(), bytes_.data() + at_, p_count * sizeof(T));
		at_ += p_count * sizeof(T);
		return true;
	}
	// A count about to size a container of elements that take at least p_min_element_bytes each in the blob:
	// fails when the remaining bytes cannot hold that many (so a hostile count cannot allocate).
	size_t count(size_t p_min_element_bytes, size_t p_max = SIZE_MAX);

	void fail(std::string p_why);
	bool ok() const { return ok_; }
	size_t offset() const { return at_; }
	size_t remaining() const { return bytes_.size() - at_; }
	bool at_end() const { return at_ == bytes_.size(); }
	// "<p_what>: <reason> at byte <offset>", for a failed reader.
	Error error(std::string_view p_what) const;

private:
	bool take(void *r_out, size_t p_size);

	std::span<const uint8_t> bytes_;
	size_t at_ = 0;
	bool ok_ = true;
	size_t fail_at_ = 0;
	std::string why_;
};

} // namespace bf3::core
