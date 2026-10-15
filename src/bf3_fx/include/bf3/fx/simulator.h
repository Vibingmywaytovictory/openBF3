#pragma once
// The CPU particle simulator (docs/design/m2a-vfx1.md §6.3-§6.8, §7; plan §3.14 "simulate live emitters on worker
// jobs", D19 option (c)). Godot-free; owns no thread. One FxWorld holds every live effect instance of a client; the
// caller steps it once per frame through the phased API below, so the per-emitter work can run on any job system
// (Godot's WorkerThreadPool in VFX2, bf3::core::parallel_for in tools and tests).
//
//   start(def, EffectStart) ......... an instance: MaxInstanceCount pool per def (steal the oldest when
//                                     ResetInstanceWhenStarted, else refuse), SpawnProbability drawn per component
//   set_transform / set_params / stop / clear   owner thread, between frames (never during a frame)
//   begin_frame(dt, view) -> k ....... owner: clamps dt to max_dt, splits it into k = ceil(dt / max_substep) equal
//                                     sub-steps (k = 1 at any frame rate above 4 Hz), clears the last frame's events
//   per sub-step, k times:
//     begin_step() -> n .............. owner, serial over every live emitter instance in list order (below): advance
//                                     the effect and emitter clocks, start due components, refresh child emitter
//                                     transforms, run the Emitter stage (SpawnRate accumulator, burst), ADMIT spawns
//                                     against MaxCount and the world budget, size each item's output range
//     run_items(first, count) ........ any threads, disjoint ranges: one item = one live emitter instance (spawn its
//                                     admitted particles, update all, write vertices and lights into its own range)
//     end_step() ..................... owner: child-emitter starts and stops, retired instances, compaction
//   outputs() / particles() / lights() / events() are valid after the frame's k-th end_step until the next
//   begin_frame. step(dt, view, lanes) does the whole frame.
//
// Admission (the only place a spawn can be refused; docs §6.7). With B = max_particles - (live particles after the
// last compaction), begin_step visits the live emitter instances in list order and for each computes due = the
// spawns its SpawnRate stage yields this sub-step, then grants
//     n = min(due, MaxCount - live_i, B),   spawns_capped += due - min(due, MaxCount - live_i),
//     spawns_dropped += min(due, MaxCount - live_i) - n,   B -= n
// and sizes the item's particle range to live_i + n (its light range to the same for ActAsPointLight emitters).
// Capped and dropped spawns are discarded, never queued. So the ranges of one sub-step sum to at most max_particles,
// and which spawns are refused depends only on the list order, never on threads or item partition.
// List order: emitter instances are appended when created (a component start in begin_step, a child start in
// end_step, both in a fixed order) and removed stably when retired, so it is a function of the start / stop / dt
// sequence alone.
//
// Children (docs §6.5): a child instance's emitter transform is set in begin_step from its parent particle's state
// as of the last end_step (position; orientation from the velocity); run_items never reads another item's state.
//
// Determinism: every random draw is keyed per fx_types.h (start seed -> per-component emitter instance seed
// fx_emitter_seed, child seed fx_child_seed, particle draws fx_key(instance seed, serial, slot, lane)); serials count
// the granted spawns of an instance. The same defs, starts and dt sequence give byte-identical particles() and
// lights() on 1 or N threads and with any item partition (fixed or variable dt alike); outputs() compare equal
// field by field (EmitterOutput has padding; the world zero-fills each record before writing it, but tests compare
// fields, not bytes).
// Bounds: all particle, light and instance storage is allocated at construction from FxWorldConfig and never grows;
// a start that does not fit is refused, an emitter instance past max_emitter_instances is not created, and a spawn
// past the budget is dropped, all counted. A particle whose state becomes non-finite is killed and counted (the
// renderer never sees a NaN).
//
// Client only: a dedicated server never constructs an FxWorld (plan §2.6, VFX4 makes it a link-time rule).

#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"
#include "bf3/fx/fx_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::fx {

// ---- per-particle output (the VFX2 renderer's input) -------------------------------------------------------

// One live particle, world space, 80 bytes. How a particle is drawn is VFX2's choice (camera-facing quads for SAQ,
// quads turned about their velocity for DAQ, quads in the emitter frame for WAQ/Quad, meshes oriented by axis); the
// simulator only fills the fields.
struct ParticleVertex {
	Vec3 position; // metres, world (LocalSpace emitters are transformed at output)
	float rotation = 0; // radians (SpawnRotation, UpdateRotation; unit [inferred] docs §6.4)
	Vec3 size; // x, y quad extents in metres; z for meshes (UpdateSizeZ)
	float frame = 0; // flipbook frame: integer part = frame index in [0, frame_count), fraction = blend to the next
	Vec4 color; // HDR linear rgb (UpdateColor), a = UpdateTransparency
	Vec3 axis; // DAQ: unit velocity; ParticleMesh: orientation Euler degrees; else 0
	float age = 0; // normalised particle age t in [0, 1]
	float alpha_min = 0; // UpdateAlphaLevelMin value at t (0 when absent)
	float alpha_max = 1; // UpdateAlphaLevelMax value at t (1 when absent)
	float alpha_scale = 1; // UpdateAlphaLevelScale value at t (1 when absent); the exponent is per emitter
	uint32_t serial = 0; // spawn serial within its emitter instance (stable sort tie-break)
};
static_assert(sizeof(ParticleVertex) == 80);

// One light per live particle of an ActAsPointLight emitter (VFX3 merges them per emitter instance into the pool).
struct ParticleLight {
	Vec3 position; // world, PointLightPivot applied
	float radius = 0; // PointLightRadius (does not scale with particle size [inferred], open Q5)
	Vec3 color; // PointLightColor * intensity, HDR linear
	float intensity = 0; // clamp(cubic(t)) * U[random min, max]
};
static_assert(sizeof(ParticleLight) == 32);

// ---- handles, inputs ---------------------------------------------------------------------------------------

struct EffectHandle {
	uint32_t slot = 0;
	uint32_t generation = 0; // 0 = invalid
	bool valid() const { return generation != 0; }
	bool operator==(const EffectHandle &) const = default;
};

// The instance-supplied EmitterParameter channels (carried; VFX1 evaluates every evaluator as if None, docs §6.6).
struct EmitterParams {
	float distance = 0;
	Vec3 vec;
	float vec_average = 0;
	float param1 = 0, param2 = 0, param3 = 0;
};

struct EffectStart {
	Transform transform; // the effect in world space (muzzle socket, impact frame, placement)
	Vec3 velocity; // the spawn source's velocity (InheritSpeedAndDirectionFromEmitter, DistributeOverDistance)
	uint64_t seed = 0; // = hash(event id) in VFX3/VFX4 (shotSeq, detonation id, slot transition)
	uint8_t tier = kTierHigh; // the client's quality tier (one bit of TierBits)
	bool first_person = false; // drawn in the viewmodel pass (VFX2); the simulation is the same
	// Seconds the effect has already been running (a late joiner seeing a running loop, plan §7.10 item 10; VFX4).
	// Clamped to [0, kMaxStartAge]. VFX1 applies it to the clocks only: elapsed StartDelays count as passed, a
	// component whose finite window ended before start_age is not started, and a continuous or repeating emitter
	// starts with its clock at start_age (no particles prewarmed; the prewarm rule is VFX4's, docs §12).
	float start_age = 0;
	EmitterParams params;
};

struct FxView {
	Vec3 camera_position;
	Vec3 camera_forward{ 0, 0, 1 };
};

enum class StopMode : uint8_t {
	Stop, // emitters stop spawning; particles live out (KillParticlesWithEmitter emitters drop theirs)
	Kill, // every particle and light of the instance vanishes at the next step
};

struct FxWorldConfig {
	uint32_t max_effects = 4096; // live effect instances
	uint32_t max_emitter_instances = 16384; // live emitter instances (children included)
	uint32_t max_particles = 1u << 16; // live particles over the world (the global spawn budget; plan §7.10 item 7: 16k in a firefight)
	float max_substep = 0.25f; // a longer dt is split into equal sub-steps of at most this
	float max_dt = 2.0f; // a longer dt is clamped (and counted)
	uint8_t tier = kTierHigh;
};

struct FxStats {
	uint32_t live_effects = 0;
	uint32_t live_emitters = 0;
	uint32_t live_particles = 0;
	uint32_t live_lights = 0;
	uint64_t steps = 0;
	uint64_t substeps = 0;
	uint64_t dt_clamped = 0;
	uint64_t spawned = 0;
	uint64_t died = 0;
	uint64_t spawns_capped = 0; // spawn attempts past an emitter's MaxCount (the cap BF3 data relies on)
	uint64_t spawns_dropped = 0; // spawns past the world's particle budget (admission, header comment)
	uint64_t emitter_starts_dropped = 0; // component or child emitter instances past max_emitter_instances
	uint64_t nonfinite_kills = 0; // particles killed because a state value became NaN or infinite
	uint64_t stolen_instances = 0; // MaxInstanceCount pool steals (ResetInstanceWhenStarted)
	uint64_t refused_starts = 0; // starts that did not fit (pool without reset, world full, def too large)
};

// One live emitter instance's output after the frame's last end_step(). Has padding: compare field by field.
struct EmitterOutput {
	EffectHandle effect;
	const EffectDef *def = nullptr; // valid while the instance lives (the world holds the shared_ptr)
	uint32_t emitter = 0; // index into def->emitters
	uint32_t component = 0; // EffectComponentDef index; for a child emitter, its root component
	bool first_person = false;
	uint32_t first_particle = 0, particle_count = 0; // into FxWorld::particles()
	uint32_t first_light = 0, light_count = 0; // into FxWorld::lights()
	Transform emitter_transform; // current world transform of the emitter
};

// Sound and visual-environment components (the audio track's and VFX5's seams): emitted when a component starts or
// its effect ends; the owner forwards them (A-track SoundPatchAsset voices, VisualEnvironment blending).
struct FxEvent {
	enum class Kind : uint8_t { SoundStart, SoundStop, VisualEnvironmentStart, VisualEnvironmentStop };
	Kind kind = Kind::SoundStart;
	EffectHandle effect;
	uint32_t component = 0;
	const EffectDef *def = nullptr;
	Transform transform;
};

// Whether start() runs component p_component of p_def for a start with p_start_seed at p_tier: the tier masks of the
// effect and the component, then one SpawnProbability draw (fx_types.h key layout). FxWorld::start uses exactly this,
// so a harness can count the expected per-start component fractions independently of the particles it observes.
inline bool fx_component_selected(const EffectDef &p_def, uint32_t p_component, uint64_t p_start_seed, uint8_t p_tier) {
	if (p_component >= p_def.components.size()) {
		return false;
	}
	const EffectComponentDef &c = p_def.components[p_component];
	if ((p_def.tiers & p_tier) == 0 || (c.tiers & p_tier) == 0) {
		return false;
	}
	return fx_unit(fx_key(p_start_seed, kKeyComponentDraw, p_component, 0)) < c.spawn_probability;
}

class FxWorld {
public:
	explicit FxWorld(const FxWorldConfig &p_config = {});
	~FxWorld();
	FxWorld(const FxWorld &) = delete;
	FxWorld &operator=(const FxWorld &) = delete;

	core::Result<EffectHandle> start(std::shared_ptr<const EffectDef> p_def, const EffectStart &p_start);
	void stop(EffectHandle p_effect, StopMode p_mode = StopMode::Stop);
	// Kills every instance, empties the pools and resets the stats (handles from before stay invalid: generations keep
	// counting); keeps the storage. Harness runs use it between independent starts (docs §7.7).
	void clear();
	void set_transform(EffectHandle p_effect, const Transform &p_transform, const Vec3 &p_velocity);
	void set_params(EffectHandle p_effect, const EmitterParams &p_params);
	// False once the instance has finished (every emitter done and every particle dead) or was stolen/killed.
	bool alive(EffectHandle p_effect) const;

	// The phases (header comment). p_dt in seconds; a non-finite or non-positive dt is a frame of 0 sub-steps.
	uint32_t begin_frame(float p_dt, const FxView &p_view);
	// Returns 0 and does nothing once the frame's sub-steps are done.
	size_t begin_step();
	void run_items(size_t p_first, size_t p_count);
	void end_step();
	// begin_frame, then per sub-step begin_step + run_items on p_lanes threads (bf3::core::parallel_for; 1 = the
	// calling thread) + end_step.
	void step(float p_dt, const FxView &p_view, unsigned p_lanes = 1);

	// Valid after the frame's last end_step until the next begin_frame.
	std::span<const EmitterOutput> outputs() const;
	std::span<const ParticleVertex> particles() const;
	std::span<const ParticleLight> lights() const;
	std::span<const FxEvent> events() const;
	const FxStats &stats() const;
	const FxWorldConfig &config() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::fx
