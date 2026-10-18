#pragma once
// VFX3: gameplay events -> FxWorld::start (docs/design/m2a.md §12.2, §8.3, §4.4; owner fx3; W3.0 contract,
// docs/design/m2a-w3.md; as built: docs/design/m2a-fx3.md). Godot-free, main thread: Bf3GameFx
// (src/bf3_godot/src/game_fx.h) owns the FxWorld, the library, these triggers and the decal pool;
// fx_triggers_tests.cpp (fx.triggers.unit) drives them headless, including case "late join" on cl's event streams.
//
// Per frame, in this order: on_muzzles (this frame's muzzles: the own 1p one from the viewmodel, G6, and the 3p ones
// from the crowd, tp), then on_events (the drained PresentationEvents, oldest first), then take the frame's decal and
// sound cues (Bf3GameFx draws the decals, Bf3GameAudio plays the sounds; bf3_fx never links bf3_audio).
//   OwnShot      FireEffects1p of the def at the own 1p muzzle (first_person, the 1p pass); one shell eject per
//                firearm shot (E8 (7)); a tracer when present_flags::kTracer is set, converging from the muzzle onto
//                the aim over the projectile's VisualConvergeDistance (kTracerConvergeM when 0; wp §7.3)
//   RemoteShot   FireEffects3p at the shooter's 3p muzzle of the newest on_muzzles (the aim fallback, counted
//                muzzle_fallbacks, when the crowd has none); tracer from the event's direction
//   Impact       select(the def's bullet material, the event's material) -> one of the cell's effects by seed
//                (trigger_seed ImpactChoice), its spread and mirror, the decal, the impact_sound; not started beyond
//                the effect's CullDistance from the camera (counted; no decal and no sound then either).
//                fx3 as built: PresentEvent::material of an Impact already is the surface's PhysicsMaterialIndex
//                (cl's ClientProjectiles copies ImpactEvent::physics_material, the triangle's pair resolved by the
//                sim's SurfaceMaterialTable), so ImpactTable::surface_material is not applied a second time (the
//                W3.0 wording "surface_material(material)" was wrong; m2a-fx3.md §3).
//   SoldierContact (own) / Hit   the zone's soldier cell (kSoldier*Material; fx_impact_soldier_body_s, no decal):
//                the own shooter's predicted contact plays it; a later Hit of the same (shooter, shot, pellet) is
//                the confirmation (hits_confirmed, not a double start); a Hit nobody predicted plays it; a Hit on
//                the own soldier (kOwnVictim) plays nothing (the 1p camera is inside the capsule)
//   Detonation   the def's DetonationEffect (fx_grenade_frag_01 for the M67) at the event + the scorch decal; a dud
//                plays nothing; the surface's impact needs the surface material, which the wire does not carry
//                (reported, m2a-fx3.md §8)
// Timelines (§8.3): an event's start age is now(timeline) - time in seconds (host::PresentClock), clamped to
// [0, kMaxStartAge]; a negative age (an event ahead of the drawn time: the predicted tick before the camera reaches
// it) or an invalid clock holds the event in a pending list, started when the clock passes it. Immediate events
// start with age 0.
// Duplicates: a start keyed by (kind, slot, id, pellet, ordinal) that already ran is skipped and counted
// (double_starts, gated 0 after replays, E8 (7)); the key memory holds the last kDedupeKeys keys.
// Frames: every Transform here (muzzles, impact frames, decals) is an effect frame in fx2's convention
// (m2a-fx2.md): local +Y (up) = the barrel or the surface normal; for a muzzle, local +Z (forward) = the weapon's up.
// Seeds (§4.4): the event carries its effect seed (shots) or impact seed (impacts) from cl; each start inside one
// event takes trigger_seed(event seed, purpose, index), so every client starts the same variants (E15). Events whose
// seed cl leaves 0 (SoldierContact, Hit, Detonation) take soldier_seed / detonation_seed from the match seed.
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

// ---- fx3 additions ----
inline constexpr std::string_view kScorchDecal = "decals/impacts/scorch/decal_scorchmark_m_01"; // [verified: bf3dump, Metro]
inline constexpr uint8_t kDefaultBulletMaterial = 1; // "Bullet", the 5.56's PhysicsMaterialIndex (impact_table.h)
inline constexpr uint32_t kDedupeKeys = 16384; // the duplicate-start memory (FIFO)
inline constexpr uint32_t kMaxTracers = 256; // live tracers (beyond: the oldest is stopped, counted)
inline constexpr uint32_t kMaxAttached = 512; // live UpdateTransform muzzle effects following their muzzle
inline constexpr float kTracerFallbackSpeed = 600.0f; // m/s when the def has no muzzle speed [inferred]
inline constexpr float kTracerFallbackTtl = 1.5f; // s, the 5.56 bullet's TimeToLive [verified: bf3dump]
// The aim fallback muzzle (fallback_muzzle, a RemoteShot without a crowd muzzle, an OwnShot without a viewmodel):
// the weapon offset from the eye along the view, right / up / forward in metres [inferred].
inline constexpr float kFallbackMuzzleRight = 0.15f;
inline constexpr float kFallbackMuzzleUp = -0.12f;
inline constexpr float kFallbackMuzzleForward = 0.55f;

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
// fx3: the event seed of a soldier cell (SoldierContact and Hit carry none; both kinds give the same seed, so the
// variant does not depend on which of the two played it) and of a Detonation.
inline constexpr uint32_t kSoldierOrdinal = 0xFFFFFFFFu;
inline uint64_t soldier_seed(uint64_t p_match_seed, uint8_t p_shooter, uint32_t p_shot, uint8_t p_pellet) {
	return host::impact_seed(host::effect_seed(p_match_seed, p_shooter, p_shot), p_pellet, kSoldierOrdinal);
}
inline uint64_t detonation_seed(uint64_t p_match_seed, uint8_t p_shooter, uint32_t p_shot) {
	return trigger_seed(host::effect_seed(p_match_seed, p_shooter, p_shot), TriggerPurpose::Detonation, 0xFFFFFFFFu);
}

// One FireEffects1p / FireEffects3p entry of a weapon: an effect blueprint and its offset in the weapon's frame (the
// rig's Wep_Root, viewmodel_muzzle.h G6; the M16A3's muzzle entries sit at 0.99 m, the shell ejection 0.55 m behind).
struct FireEffect {
	std::string blueprint; // EffectBlueprint partition, lowercase
	Transform offset; // in the weapon frame (Frostbite axes, as the data stores it)
	// ---- fx3 additions (FireEffectData) ----
	bool update_transform = false; // UpdateTransform: the effect follows the muzzle while it lives
	bool disable_during_zoom = false; // DisableDuringZoom (carried; M2a has no zoom state here)
	bool shell = false; // a shell ejection (the blueprint lives under fx/weapons/shells/): the Shell seed and count
};

struct WeaponFireEffects {
	std::vector<FireEffect> first_person; // FireEffects1p
	std::vector<FireEffect> third_person; // FireEffects3p
	std::string tracer; // the projectile's trace effect ("" none)
	int32_t trace_frequency = 0; // WeaponDef::trace_frequency (cl sets kTracer from it)
	uint8_t bullet_material = 0; // the projectile's PhysicsMaterialIndex (ImpactTable::select's first index)
	bool firearm = true; // false: knife / grenade (no shell, no muzzle)
	// ---- fx3 additions ----
	std::string detonation; // the projectile's Explosion.DetonationEffect ("" none)
	float tracer_speed = 0; // FireFunction::initial_speed z, m/s
	float tracer_ttl = 0; // ProjectileDef::time_to_live, s
	float converge = kTracerConvergeM; // ProjectileDef::visual_converge_distance (kTracerConvergeM when 0)
};

// Every def of the match, by WeaponTable def id (GunplayLevel::defs order). Reads the weapons' firing data from the
// install through p_db; a def whose effects cannot be read keeps empty lists and a note (never fatal).
struct FireEffectTable {
	std::vector<WeaponFireEffects> defs;
	std::vector<std::string> notes;
	// Every blueprint the table names (Bf3GameFx preloads them with the impact table's).
	std::vector<std::string> blueprints() const;
	// fx3: the detonation effect for a Detonation whose def has none (the first def's whose short name is
	// kGrenadeEffectName; "" when no def names it).
	std::string default_detonation;
};
core::Result<FireEffectTable> load_fire_effects(const core::EbxResolver &p_db, std::span<const core::gameplay::WeaponDef> p_defs);

// Every ImpactCell::impact_sound of the table, deduplicated and sorted (au3 compiles them: ImpactSoundCache).
std::vector<std::string> impact_sound_names(const ImpactTable &p_table);
// Every effect blueprint the table's cells name, deduplicated and sorted (the preload list).
std::vector<std::string> impact_effect_names(const ImpactTable &p_table);
// fx3: every decal the table's cells name plus kScorchDecal, deduplicated and sorted (Bf3GameFx's decal textures).
std::vector<std::string> impact_decal_names(const ImpactTable &p_table);

// A muzzle this frame, world space (Frostbite axes, +Y up). The own soldier's is first_person (the viewmodel's
// muzzle joint, G6: viewmodel_muzzle_transform); the others' come from the crowd (tp: CrowdEvaluator::muzzle_model x
// the soldier's root).
// fx3 as built: the transform sits at the person's reference entry, the def's first FireEffects1p (1p) or
// FireEffects3p (3p) entry (the muzzle flash, 0.99 m along the M16A3), as tp's muzzle_offset and G6 with that entry's
// offset give it; the other entries (the shell 0.55 m behind) are placed relative to it in the weapon frame.
struct MuzzleSample {
	uint8_t slot = 0;
	bool valid = false;
	bool first_person = false;
	Transform transform; // the effect frame, fx2's convention (m2a-fx2.md): local +Y (up) = the barrel
	Vec3 velocity;
};
static_assert(std::is_trivially_copyable_v<MuzzleSample>);
// The stand-in muzzle while the crowd has none for a soldier (§15 fx3 row): eye height + the weapon offset, along
// the soldier's view (yaw, pitch).
MuzzleSample fallback_muzzle(const host::PresentedSoldier &p_soldier);

// ---- fx3 additions: frames ----
// An effect frame whose local +Y is p_axis (normalised; +Y when zero) at p_origin; p_forward_hint (projected
// perpendicular to the axis) picks local +Z, so right = up x forward (fx_demo.cpp's frame_y).
Transform effect_frame(const Vec3 &p_origin, const Vec3 &p_axis, const Vec3 &p_forward_hint);
// The weapon frame (Frostbite axes: x right, y up, z = the barrel) as an effect frame: up = z, forward = y.
Transform effect_frame_of_weapon(const Transform &p_weapon);
Transform weapon_of_effect_frame(const Transform &p_effect);
// The aim fallback: the eye at p_eye looking along p_direction (world up for the roll), moved by the
// kFallbackMuzzle* offset.
MuzzleSample aim_muzzle(uint8_t p_slot, const Vec3 &p_eye, const Vec3 &p_direction, const Vec3 &p_velocity);
// FireEffectData's Offset (metres) and Rotation (degrees, x then y then z [inferred]; zero on the M2a kits) as a
// Transform in the weapon frame.
Transform fire_effect_offset(const Vec3 &p_offset, const Vec3 &p_rotation_degrees);

// A decal to place (Bf3GameFx's pool).
struct DecalCue {
	std::string_view decal; // ImpactCell::decal (DecalTemplateData partition; valid while the table lives)
	Transform transform; // on the surface, the effect frame convention: local +Y (up) = the surface normal
	uint64_t seed = 0; // trigger_seed(.., Decal, 0): rotation
	float start_age = 0.0f;
};
// fx3: a DecalTemplateData as the decal pool draws it. Size + U x RandomSize metres square, rotated Rotation +-
// RandomRotation degrees about the normal [inferred: the field names; retail Size 0.2, RandomSize 0.16, RandomRotation
// 180 on the concrete hit]. The texture is the diffuse TextureAsset beside it (pick_decal_texture [inferred: the
// template names no texture; its ShaderGraph binds them]).
struct DecalTemplate {
	std::string name;
	float size = 0.2f;
	float random_size = 0.0f;
	float rotation = 0.0f; // degrees
	float random_rotation = 0.0f; // degrees
	float normal_offset = 0.001f;
	float clip_angle = 70.0f; // degrees (carried; Godot's Decal has no clip angle)
	uint32_t tiles_x = 1, tiles_y = 1, tile_x = 0, tile_y = 0; // AtlasTile (a tiled decal draws its whole texture: counted)
	std::string texture; // "" none found
};
// Reads p_name's DecalTemplateData (bounded reads; an error for a missing or malformed partition). p_textures: every
// TextureAsset partition name of the level (lowercase).
core::Result<DecalTemplate> load_decal_template(const core::EbxResolver &p_db, std::string_view p_name, std::span<const std::string> p_textures);
// The diffuse texture of a decal [inferred]: "<decal>_d" when it exists, else the "_d" / "_c" texture in the decal's
// folder with the longest common name prefix (not a "glow" one), else "". decal_glass_s_01 -> decal_glass_01_d,
// decal_metalarmor_s_01 -> decal_metal_s_01_d, decal_scorchmark_m_01 -> decal_scorch_01_c.
std::string pick_decal_texture(std::string_view p_decal, std::span<const std::string> p_textures);
// One placement's square size (metres) and rotation (radians) from the cue's seed.
struct DecalPlacement {
	float size = 0.2f;
	float rotation = 0.0f;
};
DecalPlacement decal_placement(const DecalTemplate &p_template, uint64_t p_seed);

// fx3: BF3's BC1 impact decal textures (decal_metal_s_01_d, decal_brick_s_01_d, decal_stone_s_01_d,
// decal_wood_s_01_d, decal_s_01_d on Metro) carry no alpha: they are colour-keyed on magenta [verified: decompressed,
// the background is pure (255, 0, 255), the hole's halo blends towards it, means (0.90, 0.01, 0.90); the BC3 decals
// (concrete, glass, scorch) carry a real alpha]. How BF3's decal shader uses the key is [inferred]: each texel is read
// as a x colour + (1 - a) x magenta with a grey-ish colour, so 1 - a = min(R, B) - G.
// The BC1 blocks of a w x h level as RGBA8 (BC1's 3-colour mode: index 3 transparent black). An error when the bytes
// are fewer than the blocks need or the size is 0 or above 4096.
core::Result<std::vector<uint8_t>> decode_bc1(std::span<const uint8_t> p_blocks, uint32_t p_width, uint32_t p_height);
// The share of texels that are the key (R, B >= 230, G <= 25): keyed when >= kMagentaKeyedShare.
inline constexpr float kMagentaKeyedShare = 0.2f;
float magenta_share(std::span<const uint8_t> p_rgba);
// Un-blends every texel in place: a = 1 - clamp(min(R, B) - G), colour = (R - k, G, B - k) / a (k = 1 - a); a texel
// with a < 1/255 becomes (0, 0, 0, 0).
void unkey_magenta(std::span<uint8_t> r_rgba);
// The full mip chain of an RGBA8 level (each level halves, at least 1 texel; alpha-weighted 2 x 2 box): level 0 first,
// tightly packed; r_levels counts the levels (Godot's chain length). Empty for a size of 0 or too few bytes.
std::vector<uint8_t> rgba_mip_chain(std::span<const uint8_t> p_level0, uint32_t p_width, uint32_t p_height, uint32_t *r_levels);

// An impact sound to play (au3: ImpactAudio::play_impact).
struct ImpactSoundCue {
	std::string_view patch; // ImpactCell::impact_sound (valid while the table lives)
	Vec3 position;
	Vec3 normal{ 0, 1, 0 };
	uint64_t seed = 0; // the event's impact seed
	int64_t time = 0; // PresentEvent::time
	host::PresentTimeline timeline = host::PresentTimeline::Render;
};
// fx3: an effect's sound component starting or stopping (FxEvent SoundStart / SoundStop; au3:
// audio::EffectSoundRequest, ImpactAudio::on_effect_sound).
struct EffectSoundCue {
	std::string_view patch; // EffectComponentDef::asset (valid while the def lives: the library keeps it)
	uint64_t effect = 0; // the EffectHandle packed: slot << 32 | generation
	uint32_t component = 0;
	Vec3 position;
	bool start = true; // false: SoundStop
};
// The sound cues of one FxWorld frame's events (Visual environment events are skipped); appends to r_out.
void effect_sound_cues(std::span<const FxEvent> p_events, std::vector<EffectSoundCue> &r_out);

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
	// ---- fx3 additions ----
	uint64_t own_shots = 0; // OwnShot events processed
	uint64_t remote_shots = 0;
	uint64_t firearm_shots = 0; // shots of a firearm def (E8 (7): shells == firearm_shots with shells on)
	uint64_t tracer_shots = 0; // shots with kTracer set and a tracer effect
	uint64_t hits_confirmed = 0; // a Hit after the own predicted SoldierContact of that pellet
	uint64_t duds = 0;
	uint64_t rejected = 0; // events with a non-finite position / direction or an unknown kind
	uint64_t unknown_defs = 0; // a shot / impact / detonation whose def is not in the table
	uint64_t tracer_steals = 0; // a tracer stopped early for a new one (kMaxTracers)
	uint64_t attached_updates = 0; // UpdateTransform muzzle effects moved with their muzzle
	uint64_t pending_now = 0; // events held this frame
	uint64_t culled = 0; // starts of any purpose beyond their def's CullDistance (impacts_culled counts the impacts' cells)
};
static_assert(std::is_trivially_copyable_v<FxTriggerStats>);

// fx3: one FxWorld start of the last on_events (tests, E8 (3) / (5) / (7) counting, the late-join variant row).
struct TriggerStart {
	host::PresentKind kind = host::PresentKind::OwnShot;
	TriggerPurpose purpose = TriggerPurpose::Muzzle;
	uint32_t index = 0; // the entry / start index of the purpose
	uint8_t slot = 0;
	uint32_t id = 0;
	uint8_t pellet = 0;
	uint64_t seed = 0; // EffectStart::seed
	std::shared_ptr<const EffectDef> def;
	Transform transform;
	float start_age = 0;
	bool first_person = false;
	bool started = false; // false: culled or refused by FxWorld::start
	EffectHandle handle;
};

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
	// ---- fx3 additions ----
	std::span<const TriggerStart> starts() const; // the last on_events' starts, in order
	size_t pending() const;
	size_t live_tracers() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::fx
