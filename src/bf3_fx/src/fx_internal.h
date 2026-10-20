#pragma once

#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"

#include <cstdint>
#include <string>

namespace bf3::fx::detail {

core::Status validate_effect_def(const EffectDef &p_def);

uint64_t effect_particle_bound(const EffectDef &p_def);

float emitter_life_bound(const EmitterDef &p_emitter);

bool is_identity(const Transform &p_transform);

}
