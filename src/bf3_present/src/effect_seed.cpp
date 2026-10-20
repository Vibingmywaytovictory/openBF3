#include "bf3/present/effect_seed.h"

#include "bf3/sim/weapons/weapon_rng.h"

namespace bf3::present {

namespace sw = sim::weapons;

uint64_t effect_seed(uint64_t p_match_seed, uint8_t p_slot, uint32_t p_shot_index) {
	return sw::splitmix64(p_match_seed ^ (uint64_t(p_slot) << 32 | uint64_t(p_shot_index)));
}

uint64_t impact_seed(uint64_t p_effect_seed, uint8_t p_pellet, uint32_t p_ordinal) {
	return sw::splitmix64(p_effect_seed ^ (uint64_t(p_pellet) << 40 | uint64_t(p_ordinal)));
}

}
