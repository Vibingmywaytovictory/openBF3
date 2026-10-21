#pragma once

#include "bf3/net/bit_stream.h"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace bf3::net {

template <class M, class T>
concept MaybeConst = std::same_as<std::remove_const_t<M>, T>;

template <class T>
concept WireScalar = std::integral<T> || std::is_enum_v<T>;

template <WireScalar T>
constexpr uint64_t wire_raw(T p_value) {
	if constexpr (std::is_enum_v<T>) {
		return uint64_t(static_cast<std::underlying_type_t<T>>(p_value));
	} else {
		return uint64_t(p_value);
	}
}

class BitWriteArchive {
public:
	static constexpr bool kReading = false;

	explicit BitWriteArchive(BitWriter &r_writer) :
			w_(r_writer) {}

	template <WireScalar T>
	void bits(const T &p_value, unsigned p_bits) {
		w_.write_bits(wire_raw(p_value), p_bits);
	}
	void varint(uint64_t p_value) { w_.write_varint(p_value); }
	void f32(float p_value) { w_.write_f32(p_value); }

	template <class Encode>
	auto encode(Encode &&p_encode) {
		return p_encode();
	}
	template <class T, class Decode>
	void store(const T &, Decode &&) {}
	template <class T, class Encode, class Decode>
	void coded(const T &p_value, unsigned p_bits, Encode &&p_encode, Decode &&) {
		bits(p_encode(p_value), p_bits);
	}
	template <class O, class Body>
	void optional(const O &p_value, Body &&p_body) {
		const bool present = p_value.has_value();
		bits(present, 1);
		if (present) {
			p_body(*p_value);
		}
	}
	template <class V>
	size_t resize(const V &, size_t p_count) {
		return p_count;
	}

	template <class Pred>
	void check(const char *, Pred &&) {}
	template <class Pred>
	void require(const char *, Pred &&) {}
	template <class Pred>
	bool valid(Pred &&) {
		return true;
	}
	bool fits(size_t) const { return true; }
	bool ok() const { return !w_.overflowed(); }
	size_t bit_count() const { return w_.bit_count(); }

private:
	BitWriter &w_;
};

class BitCounter {
public:
	static constexpr bool kReading = false;

	template <WireScalar T>
	constexpr void bits(const T &, unsigned p_bits) {
		bits_ += p_bits;
	}
	constexpr void varint(uint64_t p_value) {
		do {
			bits_ += 8;
			p_value >>= 7;
		} while (p_value != 0);
	}
	constexpr void f32(float) { bits_ += 32; }

	template <class Encode>
	constexpr auto encode(Encode &&p_encode) {
		return p_encode();
	}
	template <class T, class Decode>
	constexpr void store(const T &, Decode &&) {}
	template <class T, class Encode, class Decode>
	constexpr void coded(const T &, unsigned p_bits, Encode &&, Decode &&) {
		bits_ += p_bits;
	}
	template <class O, class Body>
	constexpr void optional(const O &p_value, Body &&p_body) {
		bits_ += 1;
		if (p_value.has_value()) {
			p_body(*p_value);
		}
	}
	template <class V>
	constexpr size_t resize(const V &, size_t p_count) {
		return p_count;
	}

	template <class Pred>
	constexpr void check(const char *, Pred &&) {}
	template <class Pred>
	constexpr void require(const char *, Pred &&) {}
	template <class Pred>
	constexpr bool valid(Pred &&) {
		return true;
	}
	constexpr bool fits(size_t) const { return true; }
	constexpr bool ok() const { return true; }
	constexpr size_t bit_count() const { return bits_; }

private:
	size_t bits_ = 0;
};

class BitReadArchive {
public:
	static constexpr bool kReading = true;

	explicit BitReadArchive(BitReader &r_reader) :
			r_(r_reader), total_bits_(r_reader.bits_left()) {}

	template <WireScalar T>
	void bits(T &r_value, unsigned p_bits) {
		assert((std::is_same_v<T, bool> ? p_bits == 1 : p_bits <= 8 * sizeof(T)));
		if constexpr (std::is_same_v<T, bool>) {
			r_value = r_.read_bits(p_bits) != 0;
		} else if constexpr (std::is_enum_v<T>) {
			static_assert(std::is_unsigned_v<std::underlying_type_t<T>>);
			r_value = T(std::underlying_type_t<T>(r_.read_bits(p_bits)));
		} else if constexpr (std::is_signed_v<T>) {
			r_value = T(r_.read_signed(p_bits));
		} else {
			r_value = T(r_.read_bits(p_bits));
		}
	}
	void varint(uint64_t &r_value) { r_value = r_.read_varint(); }
	void f32(float &r_value) { r_value = r_.read_f32(); }

	template <class Encode>
	auto encode(Encode &&) {
		return std::remove_cvref_t<decltype(std::declval<Encode &>()())>{};
	}
	template <class T, class Decode>
	void store(T &r_value, Decode &&p_decode) {
		r_value = p_decode();
	}
	template <class T, class Encode, class Decode>
	void coded(T &r_value, unsigned p_bits, Encode &&, Decode &&p_decode) {
		std::remove_cvref_t<decltype(std::declval<Encode &>()(r_value))> code{};
		bits(code, p_bits);
		r_value = p_decode(code);
	}
	template <class O, class Body>
	void optional(O &r_value, Body &&p_body) {
		bool present = false;
		bits(present, 1);
		if (!present) {
			r_value.reset();
			return;
		}
		r_value.emplace();
		p_body(*r_value);
	}
	template <class V>
	size_t resize(V &r_values, size_t p_count) {
		if (!ok()) {
			return 0;
		}
		r_values.resize(p_count);
		return p_count;
	}

	template <class Pred>
	void check(const char *p_why, Pred &&p_pred) {
		if (why_ != nullptr) {
			return;
		}
		if (r_.failed()) {
			why_ = kTruncated;
		} else if (!p_pred()) {
			why_ = p_why;
		}
	}
	template <class Pred>
	void require(const char *p_why, Pred &&p_pred) {
		if (why_ == nullptr && (r_.failed() || !p_pred())) {
			why_ = p_why;
		}
	}
	template <class Pred>
	bool valid(Pred &&p_pred) {
		return p_pred();
	}
	bool fits(size_t p_bits) const { return p_bits <= r_.bits_left(); }
	bool ok() const { return why_ == nullptr && !r_.failed(); }
	size_t bit_count() const { return total_bits_ - r_.bits_left(); }

	const char *error() const { return why_ != nullptr ? why_ : r_.failed() ? kTruncated : nullptr; }

private:
	static constexpr const char *kTruncated = "truncated";

	BitReader &r_;
	size_t total_bits_ = 0;
	const char *why_ = nullptr;
};

template <class Serialize>
constexpr size_t count_bits(Serialize &&p_serialize) {
	BitCounter counter;
	p_serialize(counter);
	return counter.bit_count();
}

}
