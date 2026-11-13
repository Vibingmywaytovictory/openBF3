#pragma once

#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::sim::spotting {

enum class SpotState : uint8_t { None = 0, Active = 1, Passive = 2, Radar = 3, Unspottable = 4 };
inline constexpr uint8_t kSpotStateCount = 5;

enum class SpotType : uint8_t { Active = 1, Passive = 2 };

inline constexpr size_t kMaxSpotTargets = 8;
inline constexpr size_t kRayHits = 5;
inline constexpr float kSeatedConeWidening = 1.5f;
inline constexpr float kLowAmmoCheckSeconds = 2.0f;
inline constexpr float kRadarSweepSpottedTime = 1.0f;
inline constexpr uint8_t kNoSpotter = 0xFF;

struct SpotterDef {
	float fov_deg = 0;
	float team_order_fov_deg = 0;
	float distance = 0;
	float passive_interval = 0;
	float passive_required = 0;
	float cooldown_history_time = 0;
	uint32_t cooldown_allowed = 0;
	bool one_spotted = false;
	float order_need_ammo_percent = 0;
	float order_heal_percent = 0;
	float order_repair_percent = 0;
	float pickup_order_distance = 0;
	float heal_order_distance = 0;
	float ammo_order_distance = 0;
	float repair_order_distance = 0;
};

struct TargetDef {
	float active_time = 0;
	float passive_time = 0;
	float active_multiplier = 1;
	float passive_multiplier = 1;
	float on_fire_multiplier = 1;
};

struct SpottingDef {
	bool spotter_present = false;
	bool target_present = false;
	SpotterDef spotter;
	TargetDef target;
};

core::Result<SpottingDef> make_spotting_def(const core::gameplay::SoldierDef &p_soldier);

struct SpotTarget {
	SpotState state = SpotState::None;
	float remaining = 0;
	uint8_t spotter = kNoSpotter;

	bool operator==(const SpotTarget &) const = default;
};
static_assert(std::is_trivially_copyable_v<SpotTarget>);

bool apply_active(SpotTarget &r_target, const TargetDef &p_def, uint8_t p_spotter);
bool apply_passive(SpotTarget &r_target, const TargetDef &p_def, uint8_t p_spotter, bool p_firing);
bool apply_radar(SpotTarget &r_target, const TargetDef &p_def, uint8_t p_spotter, float p_radar_time);
bool step_target(SpotTarget &r_target, float p_dt);

bool in_spot_cone(
		const Vec3 &p_eye, const Vec3 &p_view, const Vec3 &p_centre, float p_half_angle_deg, float p_distance);
float crosshair_cosine(const Vec3 &p_eye, const Vec3 &p_view, const Vec3 &p_centre);

class SpotCooldown {
public:
	bool press(double p_clock, const SpotterDef &p_def);
	bool ready(double p_clock, const SpotterDef &p_def) const;
	size_t held() const { return history_.size(); }
	void clear() { history_.clear(); }

private:
	std::vector<double> history_;
};

struct RayHit {
	bool spottable = false;
	uint8_t slot = 0;
	uint8_t team = 0;
};

std::optional<uint8_t> first_enemy_hit(std::span<const RayHit> p_hits, uint8_t p_own_team);

struct Candidate {
	uint8_t slot = 0;
	float cosine = 0;
};

class PotentialList {
public:
	bool add(uint8_t p_slot, float p_cosine);
	void trim_to_nearest_crosshair();
	std::span<const Candidate> entries() const { return { entries_.data(), count_ }; }
	bool empty() const { return count_ == 0; }
	void clear() { count_ = 0; }

private:
	std::array<Candidate, kMaxSpotTargets> entries_{};
	size_t count_ = 0;
};

class PassiveScan {
public:
	bool scan_due(float p_dt, bool p_pressed, bool p_blocked, const SpotterDef &p_def);
	bool send_due(bool p_list_empty, bool p_active_pending, const SpotterDef &p_def);
	float accumulator() const { return accumulator_; }
	float timer() const { return timer_; }

private:
	float timer_ = 0;
	float accumulator_ = 0;
	bool primed_ = false;
};

}
