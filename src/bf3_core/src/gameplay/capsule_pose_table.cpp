#include "bf3/core/gameplay/capsule_pose_table.h"

#include "bf3/core/cache/blob_io.h"

#include <cmath>
#include <cstring>

namespace bf3::core::gameplay {

namespace {

constexpr std::string_view kMagic = "BF3CAPS1";

class Fnv {
public:
	template <typename T>
	void add(const T &p_v) {
		static_assert(std::is_trivially_copyable_v<T>);
		unsigned char b[sizeof(T)];
		std::memcpy(b, &p_v, sizeof(T));
		for (const unsigned char c : b) {
			h_ ^= c;
			h_ *= 0x100000001B3ull;
		}
	}
	uint64_t value() const { return h_; }

private:
	uint64_t h_ = 0xCBF29CE484222325ull;
};

void hash_capsule(Fnv &r_h, const CapsuleLocal &p_c) {
	for (const float v : p_c.a) {
		r_h.add(v);
	}
	for (const float v : p_c.b) {
		r_h.add(v);
	}
	r_h.add(p_c.radius);
	r_h.add(p_c.zone);
	r_h.add(p_c.capsule);
	r_h.add(p_c.property);
	r_h.add(p_c.flags);
}

bool finite_capsule(const CapsuleLocal &p_c) {
	for (size_t k = 0; k < 3; ++k) {
		if (!std::isfinite(p_c.a[k]) || !std::isfinite(p_c.b[k]) || std::fabs(p_c.a[k]) > 100.0f || std::fabs(p_c.b[k]) > 100.0f) {
			return false;
		}
	}
	return std::isfinite(p_c.radius) && p_c.radius > 0.0f && p_c.radius <= 2.0f && p_c.zone < 4;
}

}

bool capsule_transition_of(uint8_t p_from, uint8_t p_to, CapsuleTransition &r_transition) {
	if (p_from == p_to || p_from >= kCapsuleStances || p_to >= kCapsuleStances) {
		return false;
	}

	static constexpr uint8_t kRow[kCapsuleStances][kCapsuleStances] = { { 0xFF, 0, 1 }, { 2, 0xFF, 3 }, { 4, 5, 0xFF } };
	r_transition = CapsuleTransition(kRow[p_from][p_to]);
	return true;
}

uint64_t capsule_pose_table_hash(const CapsulePoseTable &p_table) {
	Fnv h;
	h.add(p_table.version);
	h.add(p_table.capsule_count);
	for (const float p : p_table.pitch_deg) {
		h.add(p);
	}
	h.add(uint64_t(p_table.steady.size()));
	for (const CapsuleLocal &c : p_table.steady) {
		hash_capsule(h, c);
	}
	h.add(uint64_t(p_table.transition.size()));
	for (const CapsuleLocal &c : p_table.transition) {
		hash_capsule(h, c);
	}
	for (const uint8_t s : p_table.row_source) {
		h.add(s);
	}
	h.add(p_table.bound_radius);
	return h.value();
}

bool capsule_pose_table_valid(const CapsulePoseTable &p_table) {
	if (p_table.version != kCapsuleTableVersion || p_table.capsule_count == 0 || p_table.capsule_count > kMaxTableCapsules) {
		return false;
	}
	const size_t n = p_table.capsule_count;
	if (p_table.steady.size() != kCapsuleStances * kCapsulePitchBuckets * n ||
			p_table.transition.size() != kCapsuleTransitions * kCapsuleTransitionFractions * kCapsulePitchBuckets * n) {
		return false;
	}
	for (size_t i = 0; i < kCapsulePitchBuckets; ++i) {
		if (!std::isfinite(p_table.pitch_deg[i]) || std::fabs(p_table.pitch_deg[i]) > 90.0f || (i > 0 && !(p_table.pitch_deg[i] > p_table.pitch_deg[i - 1]))) {
			return false;
		}
	}
	for (const uint8_t s : p_table.row_source) {
		if (s > capsule_row_source::kStandIn) {
			return false;
		}
	}
	if (!std::isfinite(p_table.bound_radius) || !(p_table.bound_radius > 0.0f) || p_table.bound_radius > 10.0f) {
		return false;
	}
	for (const CapsuleLocal &c : p_table.steady) {
		if (!finite_capsule(c)) {
			return false;
		}
	}
	for (const CapsuleLocal &c : p_table.transition) {
		if (!finite_capsule(c)) {
			return false;
		}
	}
	return true;
}

std::vector<uint8_t> encode_capsule_pose_table(const CapsulePoseTable &p_table) {
	BlobWriter w;
	w.magic(kMagic, kCapsuleTableVersion);
	w.u32(p_table.version);
	w.u64(p_table.hash);
	w.u8(p_table.capsule_count);
	for (const float p : p_table.pitch_deg) {
		w.f32(p);
	}
	w.u32(uint32_t(p_table.steady.size()));
	w.array(p_table.steady);
	w.u32(uint32_t(p_table.transition.size()));
	w.array(p_table.transition);
	for (const uint8_t s : p_table.row_source) {
		w.u8(s);
	}
	w.f32(p_table.bound_radius);
	return w.take();
}

bool decode_capsule_pose_table(std::span<const uint8_t> p_blob, CapsulePoseTable &r_table, std::string *r_error) {
	auto fail = [&](const std::string &p_why) {
		if (r_error != nullptr) {
			*r_error = "capsule pose table: " + p_why;
		}
		r_table = CapsulePoseTable();
		return false;
	};
	BlobReader r(p_blob);
	if (r.magic(kMagic) != kCapsuleTableVersion) {
		return fail("not a version " + std::to_string(kCapsuleTableVersion) + " blob");
	}
	CapsulePoseTable t;
	t.version = r.u32();
	t.hash = r.u64();
	t.capsule_count = r.u8();
	for (float &p : t.pitch_deg) {
		p = r.f32();
	}
	const size_t max_steady = kCapsuleStances * kCapsulePitchBuckets * kMaxTableCapsules;
	const size_t max_transition = kCapsuleTransitions * kCapsuleTransitionFractions * kCapsulePitchBuckets * kMaxTableCapsules;
	const uint32_t ns = r.u32();
	if (ns > max_steady) {
		return fail("steady count " + std::to_string(ns));
	}
	r.array(ns, t.steady);
	const uint32_t nt = r.u32();
	if (nt > max_transition) {
		return fail("transition count " + std::to_string(nt));
	}
	r.array(nt, t.transition);
	for (uint8_t &s : t.row_source) {
		s = r.u8();
	}
	t.bound_radius = r.f32();
	if (!r.ok()) {
		return fail(r.error("blob").message);
	}
	if (!r.at_end()) {
		return fail("trailing bytes");
	}
	if (!capsule_pose_table_valid(t)) {
		return fail("the table fails its structural checks");
	}
	if (capsule_pose_table_hash(t) != t.hash) {
		return fail("hash mismatch");
	}
	r_table = std::move(t);
	return true;
}

}
