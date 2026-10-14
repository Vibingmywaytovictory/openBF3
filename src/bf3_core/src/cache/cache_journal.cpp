#include "cache_journal.h"

#include "bf3/core/cache/cache_key.h"

#include <cstring>

namespace bf3::core::cache_detail {

namespace {

constexpr char kMagic[8] = { 'O', 'B', 'F', '3', 'J', 'R', 'N', 'L' };
constexpr uint32_t kVersion = 1;

enum RecordType : uint8_t {
	kPut = 1,
	kTouch = 2,
	kRemove = 3,
	kPin = 4,
	kClose = 5,
};

void put_u8(std::vector<uint8_t> &r_out, uint8_t p_v) {
	r_out.push_back(p_v);
}

void put_u32(std::vector<uint8_t> &r_out, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_v >> (8 * i)));
	}
}

void put_u64(std::vector<uint8_t> &r_out, uint64_t p_v) {
	for (int i = 0; i < 8; ++i) {
		r_out.push_back(uint8_t(p_v >> (8 * i)));
	}
}

void put_key(std::vector<uint8_t> &r_out, const Sha256Digest &p_key) {
	r_out.insert(r_out.end(), p_key.bytes.begin(), p_key.bytes.end());
}

// Frames a body: length, CRC, body.
void frame(std::vector<uint8_t> &r_out, const std::vector<uint8_t> &p_body) {
	put_u32(r_out, static_cast<uint32_t>(p_body.size()));
	put_u32(r_out, crc32(p_body));
	r_out.insert(r_out.end(), p_body.begin(), p_body.end());
}

// Bounds-checked little-endian reader over one record body.
struct Reader {
	std::span<const uint8_t> bytes;
	size_t at = 0;
	bool ok = true;

	bool need(size_t p_n) {
		if (!ok || p_n > bytes.size() - at) {
			ok = false;
		}
		return ok;
	}
	uint8_t u8() { return need(1) ? bytes[at++] : 0; }
	uint64_t u64() {
		if (!need(8)) {
			return 0;
		}
		uint64_t v = 0;
		for (int i = 7; i >= 0; --i) {
			v = (v << 8) | bytes[at + size_t(i)];
		}
		at += 8;
		return v;
	}
	Sha256Digest key() {
		Sha256Digest k;
		if (need(32)) {
			std::memcpy(k.bytes.data(), bytes.data() + at, 32);
			at += 32;
		}
		return k;
	}
	std::string str(size_t p_n) {
		if (!need(p_n)) {
			return {};
		}
		std::string s(reinterpret_cast<const char *>(bytes.data() + at), p_n);
		at += p_n;
		return s;
	}
	bool done() const { return ok && at == bytes.size(); }
};

uint32_t load_u32(const uint8_t *p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

} // namespace

void journal_header(std::vector<uint8_t> &r_out) {
	r_out.insert(r_out.end(), kMagic, kMagic + 8);
	put_u32(r_out, kVersion);
	put_u32(r_out, 0);
}

void journal_put(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, std::string_view p_kind, uint64_t p_payload_size, uint64_t p_tick) {
	std::vector<uint8_t> b;
	put_u8(b, kPut);
	put_key(b, p_key);
	put_u64(b, p_payload_size);
	put_u64(b, p_tick);
	put_u8(b, static_cast<uint8_t>(p_kind.size()));
	b.insert(b.end(), p_kind.begin(), p_kind.end());
	frame(r_out, b);
}

void journal_touch(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, uint64_t p_tick) {
	std::vector<uint8_t> b;
	put_u8(b, kTouch);
	put_key(b, p_key);
	put_u64(b, p_tick);
	frame(r_out, b);
}

void journal_remove(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, RemoveReason p_reason) {
	std::vector<uint8_t> b;
	put_u8(b, kRemove);
	put_key(b, p_key);
	put_u8(b, static_cast<uint8_t>(p_reason));
	frame(r_out, b);
}

void journal_pin(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, bool p_pinned) {
	std::vector<uint8_t> b;
	put_u8(b, kPin);
	put_key(b, p_key);
	put_u8(b, p_pinned ? 1 : 0);
	frame(r_out, b);
}

void journal_close(std::vector<uint8_t> &r_out, uint64_t p_tick) {
	std::vector<uint8_t> b;
	put_u8(b, kClose);
	put_u64(b, p_tick);
	frame(r_out, b);
}

JournalReplay replay_journal(std::span<const uint8_t> p_bytes) {
	JournalReplay r;
	if (p_bytes.size() < kJournalHeaderSize || std::memcmp(p_bytes.data(), kMagic, 8) != 0 || load_u32(p_bytes.data() + 8) != kVersion) {
		r.torn_bytes = p_bytes.size();
		return r;
	}
	r.header_ok = true;
	size_t at = kJournalHeaderSize;
	for (;;) {
		if (p_bytes.size() - at < 8) {
			break;
		}
		const uint32_t len = load_u32(p_bytes.data() + at);
		const uint32_t crc = load_u32(p_bytes.data() + at + 4);
		if (len == 0 || len > kJournalMaxBody || len > p_bytes.size() - at - 8) {
			break;
		}
		const std::span<const uint8_t> body = p_bytes.subspan(at + 8, len);
		if (crc32(body) != crc) {
			break;
		}
		Reader rd{ body };
		const uint8_t type = rd.u8();
		bool good = true;
		bool closing = false;
		switch (type) {
			case kPut: {
				const Sha256Digest key = rd.key();
				JournalEntry e;
				e.payload_size = rd.u64();
				e.tick = rd.u64();
				e.kind = rd.str(rd.u8());
				good = rd.done() && is_valid_cache_kind(e.kind);
				if (good) {
					auto it = r.entries.find(key);
					e.pinned = it != r.entries.end() && it->second.pinned;
					r.max_tick = e.tick > r.max_tick ? e.tick : r.max_tick;
					r.entries[key] = std::move(e);
				}
			} break;
			case kTouch: {
				const Sha256Digest key = rd.key();
				const uint64_t tick = rd.u64();
				good = rd.done();
				if (good) {
					auto it = r.entries.find(key);
					if (it != r.entries.end() && tick > it->second.tick) {
						it->second.tick = tick;
					}
					r.max_tick = tick > r.max_tick ? tick : r.max_tick;
				}
			} break;
			case kRemove: {
				const Sha256Digest key = rd.key();
				rd.u8();
				good = rd.done();
				if (good) {
					r.entries.erase(key);
				}
			} break;
			case kPin: {
				const Sha256Digest key = rd.key();
				const bool pinned = rd.u8() != 0;
				good = rd.done();
				if (good) {
					auto it = r.entries.find(key);
					if (it != r.entries.end()) {
						it->second.pinned = pinned;
					}
				}
			} break;
			case kClose: {
				const uint64_t tick = rd.u64();
				good = rd.done();
				r.max_tick = tick > r.max_tick ? tick : r.max_tick;
				closing = true;
			} break;
			default:
				good = false;
		}
		if (!good) {
			break; // a CRC-valid but malformed record: treat as the end
		}
		r.clean_close = closing;
		++r.records;
		at += 8 + len;
	}
	r.valid_bytes = at;
	r.torn_bytes = p_bytes.size() - at;
	return r;
}

} // namespace bf3::core::cache_detail
