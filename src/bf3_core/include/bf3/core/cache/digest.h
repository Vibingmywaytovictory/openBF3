#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bf3::core {

template <size_t N>
struct Digest {
	static constexpr size_t kSize = N;
	std::array<uint8_t, N> bytes{};

	auto operator<=>(const Digest &) const = default;
	bool operator==(const Digest &) const = default;

	std::string hex() const {
		static constexpr char kDigits[] = "0123456789abcdef";
		std::string out(N * 2, '0');
		for (size_t i = 0; i < N; ++i) {
			out[i * 2] = kDigits[bytes[i] >> 4];
			out[i * 2 + 1] = kDigits[bytes[i] & 15];
		}
		return out;
	}

	static std::optional<Digest> from_hex(std::string_view p_hex) {
		if (p_hex.size() != N * 2) {
			return std::nullopt;
		}
		Digest out;
		for (size_t i = 0; i < N * 2; ++i) {
			const char c = p_hex[i];
			int v = -1;
			if (c >= '0' && c <= '9') {
				v = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				v = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				v = c - 'A' + 10;
			}
			if (v < 0) {
				return std::nullopt;
			}
			out.bytes[i / 2] = static_cast<uint8_t>(out.bytes[i / 2] | (i % 2 == 0 ? v << 4 : v));
		}
		return out;
	}
};

using Sha1Digest = Digest<20>;
using Sha256Digest = Digest<32>;

struct DigestHash {
	template <size_t N>
	size_t operator()(const Digest<N> &p_digest) const {
		static_assert(N >= sizeof(size_t));
		size_t v;
		std::memcpy(&v, p_digest.bytes.data(), sizeof v);
		return v;
	}
};

enum class HashAlgorithm {
	Sha1,
	Sha256,
};

class Hasher {
public:
	explicit Hasher(HashAlgorithm p_algorithm);
	~Hasher();
	Hasher(Hasher &&p_other) noexcept;
	Hasher &operator=(Hasher &&p_other) noexcept;
	Hasher(const Hasher &) = delete;
	Hasher &operator=(const Hasher &) = delete;

	bool ok() const { return state_ != nullptr && !failed_; }
	size_t digest_size() const { return algorithm_ == HashAlgorithm::Sha1 ? 20 : 32; }
	void update(std::span<const uint8_t> p_bytes);

	bool finish(std::span<uint8_t> r_out);

private:
	void release();

	HashAlgorithm algorithm_;
	void *state_ = nullptr;
	bool failed_ = false;
};

Sha1Digest sha1_of(std::span<const uint8_t> p_bytes);
Sha256Digest sha256_of(std::span<const uint8_t> p_bytes);

uint32_t crc32(std::span<const uint8_t> p_bytes, uint32_t p_seed = 0);

}
