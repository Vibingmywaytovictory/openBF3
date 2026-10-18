#pragma once
// VFX3: gameplay events -> FxWorld::start (docs/design/m2a.md §12.2, §8.3, §4.4; owner fx3; W3.0 contract,
// docs/design/m2a-w3.md). Godot-free, main thread: Bf3GameFx (src/bf3_godot/src/game_fx.h) owns the FxWorld, the
// library, these triggers and the decal pool; fx_triggers_tests.cpp (fx.triggers.unit) drives them headless,
// including case "late join" on cl's recorded event streams.
//
// Per frame, in this order: on_muzzles (this frame's muzzles: the own 1p one from the viewmodel, G6, and the 3p ones
// from the crowd, tp), then on_events (the drained PresentationEvents, oldest first), then take the frame's decal and
// sound cues (Bf3GameFx draws the decals, Bf3GameAudio plays the sounds; bf3_fx never links bf3_audio).
//   OwnShot      FireEffects1p of the def at the own 1p muzzle (first_person, the 1p pass); one shell eject per
//                firearm shot (E8 (7)); a tracer when present_flags::kTracer is set, converging from the muzzle onto
//                the aim over kTracerConvergeM (wp §7.3)
//   RemoteShot   FireEffects3p at the shooter's 3p muzzle of the newest on_muzzles (fallback_muzzle when the crowd
//                has none); tracer from the event's direction
//   Impact       ImpactTable::surface_material(material) -> select(the def's bullet material, surface) -> one of the
//                cell's effects by seed (trigger_seed ImpactChoice), its spread and mirror, the decal, the
//                impact_sound; not started beyond the effect's CullDistance from the camera (counted)
//   SoldierContact (own) / Hit   the zone's soldier cell (kSoldier*Material; fx_impact_soldier_body_s, no decal)
//   Detonation   fx_grenade_frag_01 at the event + the surface's impact + the scorch decal
// Timelines (§8.3): an event's start age is now(timeline) - time in seconds (host::PresentClock), clamped to
// [0, kMaxStartAge]; a negative age (an event ahead of the drawn time: the predicted tick before the camera reaches
// it) is held in a pending list and started when the clock passes it. Immediate events start with age 0.
// Duplicates: a start keyed by (kind, slot, id, pellet, ordinal) that already ran is skipped and counted
// (double_starts, gated 0 after replays, E8 (7)).
// Seeds (§4.4): the event carries its effect seed (shots) or impact seed (impacts) from cl; each start inside one
// event takes trigger_seed(event seed, purpose, index), so every client starts the same variants (E15).
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/fx/effect_library.h"
#include "bf3/fx/fx_types.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/simulator.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/presentation_events.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::fx {

inline constexpr uint32_t kMaxDecals = 256; // the Godot Decal pool (§12.2), oldest replaced
inline constexpr float kDecalFadeSeconds = 30.0f; // [inferred]
inline constexpr float kTracerConvergeM = 30.0f; // wp §7.3
inline constexpr uint32_t kMaxPendingStarts = 1024; // events waiting for their timeline (beyond: dropped, counted)
// SoldierContact / Hit select the impact table's cell of the hit zone's surface material (Body 66, Chest 158, Head 65:
// fx_impact_soldier_body_s, spread 90 / 90 / 0, no decal [verified: fx G5, fx_harness.cpp]). The Detonation effect by
// its short name (fx3 resolves the partition with find_partition, effect_compiler.h).
inline constexpr uint8_t kSoldierBodyMaterial = 66;
inline constexpr uint8_t kSoldierChestMaterial = 158;
inline constexpr uint8_t kSoldierHeadMaterial = 65;
inline constexpr std::string_view kGrenadeEffectName = "fx_grenade_frag_01";

// Which start inside one event a seed is for.
enum class TriggerPurpose : uint8_t {
	Muzzle = 0, // FireEffects1p / 3p entry `index`
	Shell = 1,
	Tracer = 2,
	ImpactChoice = 3, // which of the cell's effects
	ImpactEffect = 4, // the chosen effect's FxWorld start
	ImpactSpread = 5, // the spread / mirror draw
	Decal = 6, // decal rotation
	Detonation = 7,
};

// §4.4 + W3.0: the seed of start `index` of `purpose` inside an event whose seed is p_event_seed.
constexpr uint64_t trigger_seed(uint64_t p_event_seed, TriggerPurpose p_purpose, uint32_t p_index) {
	return sim::weapons::splitmix64(p_event_seed ^ sim::weapons::splitmix64((uint64_t(uint8_t(p_purpose)) << 32) | uint64_t(p_index)));
}

// One FireEffects1p / FireEffects3p entry of a weapon (an effect blueprint and its offset from the muzzle socket).
struct FireEffect {
	std::string blueprint; // EffectBlueprint partition, lowercase
	Transform offset; // relative to the muzzle (fx_demo: the shell ejection sits 0.55 m behind it at +Y = barrel)
};

struct WeaponFireEffects {
	std::vector<FireEffect> first_person; // FireEffects1p
	std::vector<FireEffect> third_person; // FireEffects3p
	std::string tracer; // the projectile's trace effect ("" none)
	int32_t trace_frequency = 0; // WeaponDef::trace_frequency (cl sets kTracer from it)
	uint8_t bullet_material = 0; // the projectile's PhysicsMaterialIndex (ImpactTable::select's first index)
	bool firearm = true; // false: knife / grenade (no shell, no muzzle)
};

// Every def of the match, by WeaponTable def id (GunplayLevel::defs order). Reads the weapons' firing data from the
// install through p_db; a def whose effects cannot be read keeps empty lists and a note (never fatal).
struct FireEffectTable {
	std::vector<WeaponFireEffects> defs;
	std::vector<std::string> notes;
	// Every blueprint the table names (Bf3GameFx preloads them with the impact table's).
	std::vector<std::string> blueprints() const;
};
core::Result<FireEffectTable> load_fire_effects(const core::EbxResolver &p_db, std::span<const core::gameplay::WeaponDef> p_defs);

// Every ImpactCell::impact_sound of the table, deduplicated and sorted (au3 compiles them: ImpactSoundCache).
std::vector<std::string> impact_sound_names(const ImpactTable &p_table);
// Every effect blueprint the table's cells name, deduplicated and sorted (the preload list).
std::vector<std::string> impact_effect_names(const ImpactTable &p_table);

// A muzzle this frame, world space (Frostbite axes, +Y up). The own soldier's is first_person (the viewmodel's
// muzzle joint, G6: viewmodel_muzzle_transform); the others' come from the crowd (tp: CrowdEvaluator::muzzle_model x
// the soldier's root).
struct MuzzleSample {
	uint8_t slot = 0;
	bool valid = false;
	bool first_person = false;
	Transform transform; // forward = the barrel direction
	Vec3 velocity;
};
static_assert(std::is_trivially_copyable_v<MuzzleSample>);
// The stand-in muzzle while the crowd has none for a soldier (§15 fx3 row): eye height + the weapon offset, along
// the soldier's view (yaw, pitch).
MuzzleSample fallback_muzzle(const host::PresentedSoldier &p_soldier);

// A decal to place (Bf3GameFx's pool).
struct DecalCue {
	std::string_view decal; // ImpactCell::decal (DecalTemplateData partition; valid while the table lives)
	Transform transform; // on the surface, forward = the normal
	uint64_t seed = 0; // trigger_seed(.., Decal, 0): rotation
	float start_age = 0.0f;
};
// An impact sound to play (au3: ImpactAudio::play_impact).
struct ImpactSoundCue {
	std::string_view patch; // ImpactCell::impact_sound (valid while the table lives)
	Vec3 position;
	Vec3 normal{ 0, 1, 0 };
	uint64_t seed = 0; // the event's impact seed
	int64_t time = 0; // PresentEvent::time
	host::PresentTimeline timeline = host::PresentTimeline::Render;
};

struct FxTriggerConfig {
	uint8_t tier = kTierHigh;
	uint64_t match_seed = 0; // PresentationFrame::match_seed (Detonation and soldier-cell seeds)
	uint8_t own_slot = 0xFF;
	bool impacts = true;
	bool decals = true;
	bool tracers = true;
	bool shells = true;
};

// Plain counters (the E8 report).
struct FxTriggerStats {
	uint64_t events = 0;
	uint64_t muzzle_starts = 0;
	uint64_t muzzle_fallbacks = 0; // a RemoteShot without a crowd muzzle (fallback_muzzle)
	uint64_t shells = 0;
	uint64_t tracers = 0;
	uint64_t impacts = 0; // cells started
	uint64_t impacts_culled = 0; // beyond the effect's CullDistance
	uint64_t impacts_empty = 0; // no cell / empty cell
	uint64_t soldier_hits = 0;
	uint64_t detonations = 0;
	uint64_t decals = 0;
	uint64_t sounds = 0; // impact sound cues
	uint64_t start_failures = 0; // FxWorld::start refused (admission, MaxInstanceCount): counted
	uint64_t compile_failures = 0; // EffectLibrary::get failed for a named blueprint
	uint64_t double_starts = 0; // gated 0 (E8 (7))
	uint64_t pending_dropped = 0;
};
static_assert(std::is_trivially_copyable_v<FxTriggerStats>);

class FxTriggers {
public:
	FxTriggers(FxWorld &r_world, EffectLibrary &r_library, std::shared_ptr<const FireEffectTable> p_fire, const FxTriggerConfig &p_config = {});
	~FxTriggers();
	FxTriggers(const FxTriggers &) = delete;
	FxTriggers &operator=(const FxTriggers &) = delete;

	void set_config(const FxTriggerConfig &p_config);
	// This frame's muzzles (replaces the previous frame's).
	void on_muzzles(std::span<const MuzzleSample> p_muzzles);
	// This frame's drained events, oldest first; p_now: the frame's clocks; p_view: the camera (cull distances).
	void on_events(std::span<const host::PresentEvent> p_events, const host::PresentClock &p_now, const FxView &p_view);
	// The decal and sound cues of the last on_events (valid until the next call).
	std::span<const DecalCue> decals() const;
	std::span<const ImpactSoundCue> sounds() const;
	const FxTriggerStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::fx
