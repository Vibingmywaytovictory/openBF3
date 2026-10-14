// SHA-1 / SHA-256 through Windows CNG (BCrypt). The algorithm pseudo-handles (Windows 10+) need
// no provider setup and are safe to use from any thread; CNG allocates the hash object itself.
#include "bf3/core/cache/digest.h"

#include <windows.h>

#include <bcrypt.h>

#include <utility>

namespace bf3::core {

namespace {

BCRYPT_ALG_HANDLE algorithm_handle(HashAlgorithm p_algorithm) {
	return p_algorithm == HashAlgorithm::Sha1 ? BCRYPT_SHA1_ALG_HANDLE : BCRYPT_SHA256_ALG_HANDLE;
}

} // namespace

Hasher::Hasher(HashAlgorithm p_algorithm) :
		algorithm_(p_algorithm) {
	BCRYPT_HASH_HANDLE h = nullptr;
	if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm_handle(p_algorithm), &h, nullptr, 0, nullptr, 0, 0))) {
		state_ = h;
	}
}

Hasher::~Hasher() {
	release();
}

Hasher::Hasher(Hasher &&p_other) noexcept :
		algorithm_(p_other.algorithm_), state_(std::exchange(p_other.state_, nullptr)), failed_(p_other.failed_) {}

Hasher &Hasher::operator=(Hasher &&p_other) noexcept {
	if (this != &p_other) {
		release();
		algorithm_ = p_other.algorithm_;
		state_ = std::exchange(p_other.state_, nullptr);
		failed_ = p_other.failed_;
	}
	return *this;
}

void Hasher::release() {
	if (state_ != nullptr) {
		BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(state_));
		state_ = nullptr;
	}
}

void Hasher::update(std::span<const uint8_t> p_bytes) {
	if (!ok()) {
		return;
	}
	// BCryptHashData takes a ULONG length: feed large spans in 1 GiB pieces.
	constexpr size_t kPiece = size_t(1) << 30;
	size_t done = 0;
	while (done < p_bytes.size()) {
		const size_t n = p_bytes.size() - done < kPiece ? p_bytes.size() - done : kPiece;
		if (!BCRYPT_SUCCESS(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(state_), const_cast<PUCHAR>(p_bytes.data() + done),
					static_cast<ULONG>(n), 0))) {
			failed_ = true;
			return;
		}
		done += n;
	}
}

bool Hasher::finish(std::span<uint8_t> r_out) {
	if (!ok() || r_out.size() != digest_size()) {
		release();
		return false;
	}
	const bool good = BCRYPT_SUCCESS(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(state_), r_out.data(), static_cast<ULONG>(r_out.size()), 0));
	release();
	return good;
}

Sha1Digest sha1_of(std::span<const uint8_t> p_bytes) {
	Hasher h(HashAlgorithm::Sha1);
	h.update(p_bytes);
	Sha1Digest out;
	if (!h.finish(out.bytes)) {
		out = {};
	}
	return out;
}

Sha256Digest sha256_of(std::span<const uint8_t> p_bytes) {
	Hasher h(HashAlgorithm::Sha256);
	h.update(p_bytes);
	Sha256Digest out;
	if (!h.finish(out.bytes)) {
		out = {};
	}
	return out;
}

} // namespace bf3::core
