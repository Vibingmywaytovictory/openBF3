// soldier_state_hash: FNV-1a 64 over every SoldierState field in declaration order (docs/design/m1.md §6.2, §6.10).
// Fields are hashed one by one, never the struct's bytes, so padding cannot make two equal states differ.
#include "bf3/sim/soldier_state.h"

#include <cstring>

namespace bf3::sim {

namespace {

class Fnv64 {
public:
	template <typename T>
	void add(const T &p_value) {
		static_assert(std::is_trivially_copyable_v<T>);
		unsigned char bytes[sizeof(T)];
		std::memcpy(bytes, &p_value, sizeof(T));
		for (const unsigned char b : bytes) {
			hash_ ^= b;
			hash_ *= 0x100000001B3ull;
		}
	}
	void add3(const Vec3 &p_v) {
		add(p_v[0]);
		add(p_v[1]);
		add(p_v[2]);
	}
	uint64_t value() const { return hash_; }

private:
	uint64_t hash_ = 0xCBF29CE484222325ull;
};

} // namespace

uint64_t soldier_state_hash(const SoldierState &p_state) {
	Fnv64 h;
	h.add(p_state.tick);
	h.add(p_state.life);
	h.add3(p_state.position);
	h.add3(p_state.velocity);
	h.add3(p_state.ground_normal);
	h.add(p_state.yaw);
	h.add(p_state.pitch);
	h.add(p_state.pose);
	h.add(p_state.target_pose);
	h.add(p_state.transition_ticks);
	h.add(p_state.transition_total);
	h.add(p_state.mode);
	h.add(p_state.reserved0);
	h.add(p_state.flags);
	h.add(p_state.events);
	h.add(p_state.jump_penalty_ticks);
	h.add(p_state.air_ticks);
	h.add(p_state.protect_ticks);
	h.add(p_state.reserved1);
	h.add(p_state.health);
	h.add(p_state.impact_speed);
	h.add(p_state.damage);
	return h.value();
}

} // namespace bf3::sim
