#include "bf3/core/cache/blob_io.h"

namespace bf3::core {

void BlobWriter::put(const void *p_bytes, size_t p_size) {
	if (p_size == 0) {
		return;
	}
	const size_t at = bytes_.size();
	bytes_.resize(at + p_size);
	std::memcpy(bytes_.data() + at, p_bytes, p_size);
}

void BlobWriter::magic(std::string_view p_magic, uint32_t p_version) {
	char m[kBlobMagicSize] = {};
	std::memcpy(m, p_magic.data(), p_magic.size() < kBlobMagicSize ? p_magic.size() : kBlobMagicSize);
	put(m, kBlobMagicSize);
	u32(p_version);
}

void BlobWriter::str(std::string_view p_text) {
	u32(static_cast<uint32_t>(p_text.size()));
	put(p_text.data(), p_text.size());
}

void BlobWriter::align(size_t p_alignment) {
	while (p_alignment > 1 && bytes_.size() % p_alignment != 0) {
		bytes_.push_back(0);
	}
}

void BlobReader::fail(std::string p_why) {
	if (ok_) {
		ok_ = false;
		fail_at_ = at_;
		why_ = std::move(p_why);
	}
}

bool BlobReader::take(void *r_out, size_t p_size) {
	if (!ok_) {
		std::memset(r_out, 0, p_size);
		return false;
	}
	if (p_size > remaining()) {
		fail("truncated (" + std::to_string(p_size) + " bytes wanted, " + std::to_string(remaining()) + " left)");
		std::memset(r_out, 0, p_size);
		return false;
	}
	std::memcpy(r_out, bytes_.data() + at_, p_size);
	at_ += p_size;
	return true;
}

uint32_t BlobReader::magic(std::string_view p_magic) {
	char m[kBlobMagicSize] = {};
	if (!take(m, kBlobMagicSize)) {
		return 0;
	}
	char want[kBlobMagicSize] = {};
	std::memcpy(want, p_magic.data(), p_magic.size() < kBlobMagicSize ? p_magic.size() : kBlobMagicSize);
	if (std::memcmp(m, want, kBlobMagicSize) != 0) {
		at_ = 0;
		fail("bad magic (not a " + std::string(p_magic) + " blob)");
		return 0;
	}
	return u32();
}

uint8_t BlobReader::u8() {
	uint8_t v = 0;
	take(&v, 1);
	return v;
}

uint16_t BlobReader::u16() {
	uint16_t v = 0;
	take(&v, 2);
	return v;
}

uint32_t BlobReader::u32() {
	uint32_t v = 0;
	take(&v, 4);
	return v;
}

int32_t BlobReader::i32() {
	int32_t v = 0;
	take(&v, 4);
	return v;
}

uint64_t BlobReader::u64() {
	uint64_t v = 0;
	take(&v, 8);
	return v;
}

float BlobReader::f32() {
	float v = 0;
	take(&v, 4);
	return v;
}

bool BlobReader::boolean() {
	const uint8_t v = u8();
	if (v > 1) {
		fail("boolean byte " + std::to_string(v));
		return false;
	}
	return v == 1;
}

std::string BlobReader::str(size_t p_max_length) {
	const uint32_t n = u32();
	if (!ok_) {
		return {};
	}
	if (n > p_max_length) {
		fail("string of " + std::to_string(n) + " bytes (limit " + std::to_string(p_max_length) + ")");
		return {};
	}
	if (n > remaining()) {
		fail("string of " + std::to_string(n) + " bytes past the end");
		return {};
	}
	std::string out(reinterpret_cast<const char *>(bytes_.data() + at_), n);
	at_ += n;
	return out;
}

void BlobReader::align(size_t p_alignment) {
	if (!ok_ || p_alignment <= 1) {
		return;
	}
	const size_t pad = (p_alignment - at_ % p_alignment) % p_alignment;
	if (pad > remaining()) {
		fail("alignment padding past the end");
		return;
	}
	for (size_t i = 0; i < pad; ++i) {
		if (bytes_[at_ + i] != 0) {
			fail("non-zero alignment padding");
			return;
		}
	}
	at_ += pad;
}

size_t BlobReader::count(size_t p_min_element_bytes, size_t p_max) {
	const uint64_t n = u64();
	if (!ok_) {
		return 0;
	}
	const size_t element = p_min_element_bytes == 0 ? 1 : p_min_element_bytes;
	if (n > p_max || n > remaining() / element) {
		fail("count " + std::to_string(n) + " larger than the blob can hold");
		return 0;
	}
	return static_cast<size_t>(n);
}

Error BlobReader::error(std::string_view p_what) const {
	return make_error(std::string(p_what) + ": " + (ok_ ? std::string("ok") : why_) + " at byte " + std::to_string(fail_at_));
}

}
