#pragma once
// bf3_fx internal helpers shared by the compiler, the blob decoder and the simulator (docs/design/m2a-vfx1.md §10).

#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"

#include <cstdint>
#include <string>

namespace bf3::fx::detail {

// Structural checks every EffectDef must pass before the simulator sees it (the compiler's output and every decoded
// blob): indices in range (component -> emitter, slot -> evaluator, child -> emitter, gradient ranges), counts within
// the fx_types.h bounds, enum values in range, floats finite and within kMaxAbsParameter, the children graph acyclic
// and at most kMaxChildDepth deep, and max_particles equal to the recomputed bound (effect_particle_bound).
core::Status validate_effect_def(const EffectDef &p_def);

// Sum over the emitter components of particles(e) = MaxCount(e) * (1 + sum over children of particles(c)) (one child
// instance per live parent particle); saturates at kMaxEffectParticles + 1. Requires an acyclic children graph.
uint64_t effect_particle_bound(const EffectDef &p_def);

// The largest possible particle life of an emitter in its own seconds (UpdateAge Lifetime times an upper bound of
// its value), 0 without an UpdateAge slot (docs §6.5).
float emitter_life_bound(const EmitterDef &p_emitter);

// True when every row is within 1e-6 of the identity.
bool is_identity(const Transform &p_transform);

} // namespace bf3::fx::detail
