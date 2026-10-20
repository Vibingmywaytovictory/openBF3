#include "bf3/core/ids.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

namespace bf3::core {

namespace {

int hex_value(char p_digit) {
	if (p_digit >= '0' && p_digit <= '9') {
		return p_digit - '0';
	}
	if (p_digit >= 'a' && p_digit <= 'f') {
		return p_digit - 'a' + 10;
	}
	if (p_digit >= 'A' && p_digit <= 'F') {
		return p_digit - 'A' + 10;
	}
	return -1;
}

std::string_view trim(std::string_view p_text) {
	while (!p_text.empty() && (p_text.front() == ' ' || p_text.front() == '\t' || p_text.front() == '\r' || p_text.front() == '\n')) {
		p_text.remove_prefix(1);
	}
	while (!p_text.empty() && (p_text.back() == ' ' || p_text.back() == '\t' || p_text.back() == '\r' || p_text.back() == '\n')) {
		p_text.remove_suffix(1);
	}
	return p_text;
}

char lower(char p_c) {
	return (p_c >= 'A' && p_c <= 'Z') ? static_cast<char>(p_c + ('a' - 'A')) : p_c;
}

}

uint64_t hash_seed() {

	static const uint64_t seed = [] {
		std::random_device device;
		uint64_t s = (static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device());
		s ^= hash_mix(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
		s ^= hash_mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&device)));
		return hash_mix(s);
	}();
	return seed;
}

Guid Guid::from_bytes(const uint8_t *p_bytes) {
	Guid g;
	std::memcpy(g.bytes.data(), p_bytes, 16);
	return g;
}

Guid Guid::from_big_endian_fields(const uint8_t *p_bytes) {
	Guid g = from_bytes(p_bytes);
	std::reverse(g.bytes.begin(), g.bytes.begin() + 4);
	std::reverse(g.bytes.begin() + 4, g.bytes.begin() + 6);
	std::reverse(g.bytes.begin() + 6, g.bytes.begin() + 8);
	return g;
}

std::optional<Guid> Guid::parse(std::string_view p_text) {
	std::string_view t = trim(p_text);
	while (!t.empty() && t.front() == '{') {
		t.remove_prefix(1);
	}
	while (!t.empty() && t.back() == '}') {
		t.remove_suffix(1);
	}
	if (t.size() != 36 || t[8] != '-' || t[13] != '-' || t[18] != '-' || t[23] != '-') {
		return std::nullopt;
	}
	uint8_t big[16];
	size_t n = 0;
	int high = -1;
	for (char c : t) {
		if (c == '-') {
			continue;
		}
		const int v = hex_value(c);
		if (v < 0 || n >= 16) {
			return std::nullopt;
		}
		if (high < 0) {
			high = v;
		} else {
			big[n++] = static_cast<uint8_t>((high << 4) | v);
			high = -1;
		}
	}
	if (n != 16 || high >= 0) {
		return std::nullopt;
	}
	return from_big_endian_fields(big);
}

bool Guid::is_zero() const {
	return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
}

std::string Guid::to_string() const {
	const uint8_t *b = bytes.data();
	char text[40];
	std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1], b[0],
			b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return text;
}

Sha1 Sha1::from_bytes(const uint8_t *p_bytes) {
	Sha1 s;
	std::memcpy(s.bytes.data(), p_bytes, 20);
	return s;
}

std::optional<Sha1> Sha1::parse(std::string_view p_text) {
	const std::string_view t = trim(p_text);
	if (t.size() != 40) {
		return std::nullopt;
	}
	Sha1 s;
	for (size_t i = 0; i < 20; ++i) {
		const int hi = hex_value(t[2 * i]);
		const int lo = hex_value(t[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return std::nullopt;
		}
		s.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return s;
}

std::string Sha1::to_string() const {
	return hex_lower(bytes.data(), bytes.size());
}

uint32_t asset_name_hash(std::string_view p_name) {
	uint32_t hash = 5381;
	for (char c : p_name) {
		hash = (hash * 33u) ^ static_cast<uint8_t>(lower(c));
	}
	return hash;
}

std::string to_lower_ascii(std::string_view p_text) {
	std::string out(p_text);
	for (char &c : out) {
		c = lower(c);
	}
	return out;
}

bool iequals_ascii(std::string_view p_a, std::string_view p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.size(); ++i) {
		if (lower(p_a[i]) != lower(p_b[i])) {
			return false;
		}
	}
	return true;
}

std::string hex_lower(const uint8_t *p_bytes, size_t p_count) {
	static const char digits[] = "0123456789abcdef";
	std::string out(p_count * 2, '0');
	for (size_t i = 0; i < p_count; ++i) {
		out[2 * i] = digits[p_bytes[i] >> 4];
		out[2 * i + 1] = digits[p_bytes[i] & 15];
	}
	return out;
}

}
