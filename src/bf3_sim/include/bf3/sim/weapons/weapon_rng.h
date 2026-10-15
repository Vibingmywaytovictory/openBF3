#pragma once
// Counter-based random numbers for shots (docs/design/m2a-weapons.md §5.4; plan §3.2 "counter-based RNG", D7).
//
// No generator state is kept or sent: every random number is a pure function of (match seed, player slot,
// shot index) and of what it is for, so the server, the shooter's predicting client and a remote client that
// gets a ShotEvent all draw the same spread, pellet and recoil numbers, in any order, and a replay after a
// reconciliation draws them again unchanged. shot_index counts every shot of a player slot over the whole match
// (SoldierWeaponsState::next_shot_index: never reset on respawn or weapon switch), so the slot and the index alone
// name a shot uniquely. The hash is SplitMix64 (Steele, Lea, Flood 2014; public-domain reference constants),
// written out here, integer-only and therefore identical on every machine (plan R4).

#include <cstdint>

namespace bf3::sim::weapons {

constexpr uint64_t splitmix64(uint64_t p_x) {
	uint64_t z = p_x + 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

// The per-shot seed: hash(match seed, player slot, shot index). Carried on ShotEvent::seed.
constexpr uint64_t shot_seed(uint64_t p_match_seed, uint32_t p_player_slot, uint32_t p_shot_index) {
	return splitmix64(splitmix64(p_match_seed ^ (uint64_t(p_player_slot) << 32)) ^ uint64_t(p_shot_index));
}

// What a number is for. Values are part of the contract (changing one changes every shot): append only.
enum class RngPurpose : uint8_t {
	SpreadRadius = 0, // the spread disc's radius draw
	SpreadAngle = 1, // its azimuth
	PelletRadius = 2, // per pellet, inside the pellet cone
	PelletAngle = 3,
	RecoilHorizontal = 4, // where in [horizontal min, max] this shot's yaw kick falls
	FuseVariation = 5, // BulletEntityData.DetonationTimeVariation (0 on the M2a kits)
};

// The p_lane-th number of p_purpose for one shot (p_lane = pellet index, 0 for per-shot numbers).
constexpr uint64_t shot_random(uint64_t p_shot_seed, RngPurpose p_purpose, uint32_t p_lane = 0) {
	return splitmix64(p_shot_seed ^ splitmix64((uint64_t(uint8_t(p_purpose)) << 32) | uint64_t(p_lane)));
}

// A uniform float in [0, 1) from the top 24 bits (every value exactly representable).
constexpr float unit_float(uint64_t p_bits) {
	return float(uint32_t(p_bits >> 40)) * (1.0f / 16777216.0f);
}

} // namespace bf3::sim::weapons
