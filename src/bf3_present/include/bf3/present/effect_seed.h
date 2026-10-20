#pragma once

#include <cstdint>

namespace bf3::present {

uint64_t effect_seed(uint64_t p_match_seed, uint8_t p_slot, uint32_t p_shot_index);
uint64_t impact_seed(uint64_t p_effect_seed, uint8_t p_pellet, uint32_t p_ordinal);

}
