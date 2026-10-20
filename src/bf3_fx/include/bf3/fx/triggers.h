#pragma once

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

inline constexpr uint32_t kMaxDecals = 256;
inline constexpr float kDecalFadeSeconds = 30.0f;
inline constexpr float kTracerConvergeM = 30.0f;
inline constexpr uint32_t kMaxPendingStarts = 1024;

inline constexpr uint8_t kSoldierBodyMaterial = 66;
inline constexpr uint8_t kSoldierChestMaterial = 158;
inline constexpr uint8_t kSoldierHeadMaterial = 65;
inline constexpr std::string_view kGrenadeEffectName = "fx_grenade_frag_01";

inline constexpr std::string_view kScorchDecal = "decals/impacts/scorch/decal_scorchmark_m_01";
inline constexpr uint8_t kDefaultBulletMaterial = 1;
inline constexpr uint32_t kDedupeKeys = 16384;
inline constexpr uint32_t kMaxTracers = 256;
inline constexpr uint32_t kMaxAttached = 512;
inline constexpr float kTracerFallbackSpeed = 600.0f;
inline constexpr float kTracerFallbackTtl = 1.5f;

inline constexpr float kFallbackMuzzleRight = 0.15f;
inline constexpr float kFallbackMuzzleUp = -0.12f;
inline constexpr float kFallbackMuzzleForward = 0.55f;

enum class TriggerPurpose : uint8_t {
	Muzzle = 0,
	Shell = 1,
	Tracer = 2,
	ImpactChoice = 3,
	ImpactEffect = 4,
	ImpactSpread = 5,
	Decal = 6,
	Detonation = 7,
};

constexpr uint64_t trigger_seed(uint64_t p_event_seed, TriggerPurpose p_purpose, uint32_t p_index) {
	return sim::weapons::splitmix64(p_event_seed ^ sim::weapons::splitmix64((uint64_t(uint8_t(p_purpose)) << 32) | uint64_t(p_index)));
}

inline constexpr uint32_t kSoldierOrdinal = 0xFFFFFFFFu;
inline uint64_t soldier_seed(uint64_t p_match_seed, uint8_t p_shooter, uint32_t p_shot, uint8_t p_pellet) {
	return host::impact_seed(host::effect_seed(p_match_seed, p_shooter, p_shot), p_pellet, kSoldierOrdinal);
}
inline uint64_t detonation_seed(uint64_t p_match_seed, uint8_t p_shooter, uint32_t p_shot) {
	return trigger_seed(host::effect_seed(p_match_seed, p_shooter, p_shot), TriggerPurpose::Detonation, 0xFFFFFFFFu);
}

struct FireEffect {
	std::string blueprint;
	Transform offset;

	bool update_transform = false;
	bool disable_during_zoom = false;
	bool shell = false;
};

struct WeaponFireEffects {
	std::vector<FireEffect> first_person;
	std::vector<FireEffect> third_person;
	std::string tracer;
	int32_t trace_frequency = 0;
	uint8_t bullet_material = 0;
	bool firearm = true;

	std::string detonation;
	float tracer_speed = 0;
	float tracer_ttl = 0;
	float converge = kTracerConvergeM;
};

struct FireEffectTable {
	std::vector<WeaponFireEffects> defs;
	std::vector<std::string> notes;

	std::vector<std::string> blueprints() const;

	std::string default_detonation;
};
core::Result<FireEffectTable> load_fire_effects(const core::EbxResolver &p_db, std::span<const core::gameplay::WeaponDef> p_defs);

std::vector<std::string> impact_sound_names(const ImpactTable &p_table);

std::vector<std::string> impact_effect_names(const ImpactTable &p_table);

std::vector<std::string> impact_decal_names(const ImpactTable &p_table);

struct MuzzleSample {
	uint8_t slot = 0;
	bool valid = false;
	bool first_person = false;
	Transform transform;
	Vec3 velocity;
};
static_assert(std::is_trivially_copyable_v<MuzzleSample>);

MuzzleSample fallback_muzzle(const host::PresentedSoldier &p_soldier);

Transform effect_frame(const Vec3 &p_origin, const Vec3 &p_axis, const Vec3 &p_forward_hint);

Transform effect_frame_of_weapon(const Transform &p_weapon);
Transform weapon_of_effect_frame(const Transform &p_effect);

MuzzleSample aim_muzzle(uint8_t p_slot, const Vec3 &p_eye, const Vec3 &p_direction, const Vec3 &p_velocity);

Transform fire_effect_offset(const Vec3 &p_offset, const Vec3 &p_rotation_degrees);

struct DecalCue {
	std::string_view decal;
	Transform transform;
	uint64_t seed = 0;
	float start_age = 0.0f;
};

struct DecalTemplate {
	std::string name;
	float size = 0.2f;
	float random_size = 0.0f;
	float rotation = 0.0f;
	float random_rotation = 0.0f;
	float normal_offset = 0.001f;
	float clip_angle = 70.0f;
	uint32_t tiles_x = 1, tiles_y = 1, tile_x = 0, tile_y = 0;
	std::string texture;
};

core::Result<DecalTemplate> load_decal_template(const core::EbxResolver &p_db, std::string_view p_name, std::span<const std::string> p_textures);

std::string pick_decal_texture(std::string_view p_decal, std::span<const std::string> p_textures);

struct DecalPlacement {
	float size = 0.2f;
	float rotation = 0.0f;
};
DecalPlacement decal_placement(const DecalTemplate &p_template, uint64_t p_seed);

core::Result<std::vector<uint8_t>> decode_bc1(std::span<const uint8_t> p_blocks, uint32_t p_width, uint32_t p_height);

inline constexpr float kMagentaKeyedShare = 0.2f;
float magenta_share(std::span<const uint8_t> p_rgba);

void unkey_magenta(std::span<uint8_t> r_rgba);

std::vector<uint8_t> rgba_mip_chain(std::span<const uint8_t> p_level0, uint32_t p_width, uint32_t p_height, uint32_t *r_levels);

core::Result<std::vector<uint8_t>> decode_bc3(std::span<const uint8_t> p_blocks, uint32_t p_width, uint32_t p_height);

float transparent_share(std::span<const uint8_t> p_rgba);

inline constexpr float kMinDecalTransparentShare = 0.05f;

enum class DecalPixelFormat : uint8_t {
	Bc1,
	Bc3,
	Rgba8,
	Other,
};
enum class DecalTextureKind : uint8_t {
	Alpha,
	MagentaKeyed,
	Opaque,
	Unsupported,
};
const char *decal_texture_kind_name(DecalTextureKind p_kind);
struct DecalTexturePrep {
	DecalTextureKind kind = DecalTextureKind::Unsupported;
	float magenta = 0.0f;
	float transparent = 0.0f;
	std::vector<uint8_t> rgba_chain;
	uint32_t levels = 0;
	bool usable() const { return kind == DecalTextureKind::Alpha || kind == DecalTextureKind::MagentaKeyed; }
};

core::Result<DecalTexturePrep> prepare_decal_texture(DecalPixelFormat p_format, std::span<const uint8_t> p_data, uint32_t p_width, uint32_t p_height);

struct ImpactSoundCue {
	std::string_view patch;
	Vec3 position;
	Vec3 normal{ 0, 1, 0 };
	uint64_t seed = 0;
	int64_t time = 0;
	host::PresentTimeline timeline = host::PresentTimeline::Render;
};

struct EffectSoundCue {
	std::string_view patch;
	uint64_t effect = 0;
	uint32_t component = 0;
	Vec3 position;
	bool start = true;
};

void effect_sound_cues(std::span<const FxEvent> p_events, std::vector<EffectSoundCue> &r_out);

struct FxTriggerConfig {
	uint8_t tier = kTierHigh;
	uint64_t match_seed = 0;
	uint8_t own_slot = 0xFF;
	bool impacts = true;
	bool decals = true;
	bool tracers = true;
	bool shells = true;
};

struct FxTriggerStats {
	uint64_t events = 0;
	uint64_t muzzle_starts = 0;
	uint64_t muzzle_fallbacks = 0;
	uint64_t shells = 0;
	uint64_t tracers = 0;
	uint64_t impacts = 0;
	uint64_t impacts_culled = 0;
	uint64_t impacts_empty = 0;
	uint64_t soldier_hits = 0;
	uint64_t detonations = 0;
	uint64_t decals = 0;
	uint64_t sounds = 0;
	uint64_t start_failures = 0;
	uint64_t compile_failures = 0;
	uint64_t double_starts = 0;
	uint64_t pending_dropped = 0;

	uint64_t own_shots = 0;
	uint64_t remote_shots = 0;
	uint64_t firearm_shots = 0;
	uint64_t tracer_shots = 0;
	uint64_t hits_confirmed = 0;
	uint64_t duds = 0;
	uint64_t rejected = 0;
	uint64_t unknown_defs = 0;
	uint64_t tracer_steals = 0;
	uint64_t attached_updates = 0;
	uint64_t pending_now = 0;
	uint64_t culled = 0;
};
static_assert(std::is_trivially_copyable_v<FxTriggerStats>);

struct TriggerStart {
	host::PresentKind kind = host::PresentKind::OwnShot;
	TriggerPurpose purpose = TriggerPurpose::Muzzle;
	uint32_t index = 0;
	uint8_t slot = 0;
	uint32_t id = 0;
	uint8_t pellet = 0;
	uint64_t seed = 0;
	std::shared_ptr<const EffectDef> def;
	Transform transform;
	float start_age = 0;
	bool first_person = false;
	bool started = false;
	EffectHandle handle;
};

class FxTriggers {
public:
	FxTriggers(FxWorld &r_world, EffectLibrary &r_library, std::shared_ptr<const FireEffectTable> p_fire, const FxTriggerConfig &p_config = {});
	~FxTriggers();
	FxTriggers(const FxTriggers &) = delete;
	FxTriggers &operator=(const FxTriggers &) = delete;

	void set_config(const FxTriggerConfig &p_config);

	void on_muzzles(std::span<const MuzzleSample> p_muzzles);

	void on_events(std::span<const host::PresentEvent> p_events, const host::PresentClock &p_now, const FxView &p_view);

	std::span<const DecalCue> decals() const;
	std::span<const ImpactSoundCue> sounds() const;
	const FxTriggerStats &stats() const;

	std::span<const TriggerStart> starts() const;
	size_t pending() const;
	size_t live_tracers() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
