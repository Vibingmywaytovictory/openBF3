#pragma once

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <optional>

namespace bf3::sim::weapons {

using core::gameplay::Stance;
using core::gameplay::Zoom;

inline constexpr float kSwaySeconds = 1.0f / 30.0f;
inline constexpr SimTime kSwayUnits = 2 * kTimeUnitsPerTick;
inline constexpr float kSwayAirHold = 0.5f;
inline constexpr float kSwayMovingSpeed = 0.5f;
inline constexpr float kSwayVerticalLow = 0.9f;
inline constexpr double kSwayRecoveryScale = 0.01;
inline constexpr uint32_t kNoSwaySeed = 0xFFFFFFFFu;
inline constexpr uint8_t kSwayFirstShotCount = 1;
inline constexpr uint8_t kSwayShotCountCap = 2;

constexpr uint32_t sway_tick(SimTime p_time) {
	return uint32_t(p_time / kSwayUnits);
}
constexpr SimTime sway_tick_start(uint32_t p_tick) {
	return SimTime(p_tick) * kSwayUnits;
}
constexpr SimTime sway_tick_end(uint32_t p_tick) {
	return SimTime(p_tick + 1u) * kSwayUnits;
}
constexpr bool starts_sway_tick(Tick p_tick) {
	return tick_start(p_tick) % kSwayUnits == 0;
}

struct SwayContext {
	Stance stance = Stance::Stand;
	Zoom zoom = Zoom::NoZoom;
	SwayMove move = SwayMove::Base;
	float move_fraction = 1.0f;
	float suppression = 0.0f;
	bool supported = false;
	bool light_on = false;

	constexpr bool operator==(const SwayContext &) const = default;
};

namespace sway_flags {
inline constexpr uint8_t kFired = 1u << 0;
inline constexpr uint8_t kTriggerHeld = 1u << 1;
}

struct GunSwayState {
	float dispersion = 0;
	float deviation_pitch = 0;
	float deviation_yaw = 0;
	float recoil_pitch = 0;
	float recoil_yaw = 0;
	float since_shot = 0;
	float since_air = kSwayAirHold;
	float spring_pitch = 0;
	float spring_yaw = 0;
	float spring_pitch_rate = 0;
	float spring_yaw_rate = 0;
	uint32_t rng = 1;
	uint32_t last_seed = kNoSwaySeed;
	uint8_t shots = 0;
	uint8_t flags = 0;

	constexpr bool operator==(const GunSwayState &) const = default;
};

struct SwayRecord {
	Dispersion dispersion;
	bool modifiers = true;
};

std::optional<SwayRecord> sway_record(const WeaponSimDef &p_def, const SwayContext &p_context, float p_since_air);

Dispersion effective_dispersion(const WeaponSimDef &p_def, const SwayContext &p_context);

void sway_fire_start(GunSwayState &r_sway);
void sway_fire_stop(GunSwayState &r_sway);
void sway_shot(GunSwayState &r_sway, uint32_t p_seed);
void sway_update(GunSwayState &r_sway, const WeaponSimDef &p_def, const SwayContext &p_context);

struct RecoilOffset {
	float pitch = 0;
	float yaw = 0;

	constexpr bool operator==(const RecoilOffset &) const = default;
};

float gameplay_deviation_scale(const WeaponSimDef &p_def, Zoom p_zoom);

RecoilOffset gameplay_recoil(const WeaponSimDef &p_def, const GunSwayState &p_sway, Zoom p_zoom);
RecoilOffset camera_recoil(const WeaponSimDef &p_def, const GunSwayState &p_sway, Zoom p_zoom);

Vec3 sway_rotate(const Vec3 &p_local, float p_pitch, float p_yaw);

Vec3 shoot_direction(uint16_t p_yaw, int16_t p_pitch, const WeaponSimDef &p_def, const GunSwayState &p_sway,
		Zoom p_zoom, bool p_dispersion);

Vec3 sample_cone(const Vec3 &p_aim, float p_spread_degrees, float p_u_radius, float p_u_angle);

Vec3 aim_direction(uint16_t p_yaw, int16_t p_pitch, const RecoilOffset &p_offset);

struct SwayWait {
	SimTime time = 0;
	float carry = 0;
};

float sway_carry_at(SimTime p_time);

SwayWait sway_wait(SimTime p_from, float p_carry, float p_seconds);
}
