#pragma once

#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_foley.h"
#include "bf3/audio/weapon_sound.h"

#include <cstdint>
#include <type_traits>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kHashShotFired = kHashStep;
inline constexpr uint32_t kHashFireStopped = 1920007028u;
inline constexpr uint32_t kHashReloadEnd = 1864262363u;
inline constexpr uint32_t kHashLockOn = 2904570447u;
inline constexpr uint32_t kHashWeaponGone = 3520090073u;
inline constexpr uint32_t kHashFireLogicType = 1365968171u;
inline constexpr uint32_t kHashOverheatLevel = 1519867045u;
inline constexpr uint32_t kHashLockAmount = 1911182178u;
inline constexpr uint32_t kHashZoomLevel = 3650803780u;
inline constexpr uint32_t kHashOwnerSameTeam = 3576779520u;
inline constexpr uint32_t kHashOwnerLocal = 4198762218u;
inline constexpr uint32_t kHashWeaponFlag = 636551541u;

inline constexpr uint8_t kInitialFireLogicType = 2;
inline constexpr float kDefaultWeaponFlag = 1.0f;
inline constexpr uint8_t kFireModeAction = 6;
inline constexpr uint8_t kZoomAction = 1;

struct WeaponSoundOwner {
	bool first_person = false;
	bool same_team = false;
	bool local = false;
};

enum class WeaponSoundOpKind : uint8_t { Post, Set, Release, Detach };

struct WeaponSoundOp {
	WeaponSoundOpKind kind = WeaponSoundOpKind::Post;
	uint32_t hash = 0;
	float value = 0.0f;
	bool operator==(const WeaponSoundOp &) const = default;
};
static_assert(std::is_trivially_copyable_v<WeaponSoundOp>);

constexpr uint32_t vehicle_crew_key(uint16_t p_spawner, uint8_t p_generation) {
	return (uint32_t(p_spawner) << 8) | uint32_t(p_generation);
}

struct VehicleCrewSeat {
	uint8_t slot = 0xFF;
	uint8_t team = 0;
	uint32_t vehicle = 0;
};
static_assert(std::is_trivially_copyable_v<VehicleCrewSeat>);

struct VehicleWeaponPose {
	uint32_t vehicle = 0;
	uint16_t def = 0xFFFF;
	Vec3 position;
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
};
static_assert(std::is_trivially_copyable_v<VehicleWeaponPose>);

struct VehicleWeaponOp {
	uint64_t update = 0;
	uint8_t slot = 0xFF;
	uint16_t def = 0xFFFF;
	WeaponSoundOp op;
};
static_assert(std::is_trivially_copyable_v<VehicleWeaponOp>);

class WeaponSoundState {
public:
	void fire_start(const WeaponSoundOwner &p_owner);
	void shot();
	void fire_end();
	void trigger_release();
	void reload_start(bool p_first_person, bool p_short_reload);
	void reload_end();
	void bolt();
	void action(uint8_t p_type, float p_zoom, uint8_t p_fire_logic);
	void lock_on(const WeaponSoundOwner &p_owner, float p_amount);

	void set_fire_logic(uint8_t p_type);
	void set_overheat(float p_heat);
	void set_lock_amount(float p_amount);

	bool wants_handle() const;
	void update(bool p_playing, std::vector<WeaponSoundOp> &r_out);
	void destroyed(bool p_playing, std::vector<WeaponSoundOp> &r_out);
	void inputs(std::vector<WeaponSoundOp> &r_out) const;

	uint32_t queued_shots() const { return shots_; }
	bool firing() const { return firing_; }
	bool ending() const { return end_; }
	const WeaponSoundOwner &owner() const { return owner_; }

private:
	bool fire_ = false;
	bool reload_ = false;
	bool reload_end_ = false;
	bool action_ = false;
	bool bolt_ = false;
	bool lock_ = false;
	bool firing_ = false;
	bool end_ = false;
	bool shot_mark_ = false;
	uint32_t shots_ = 0;
	WeaponSoundOwner owner_;
	float short_reload_ = 0.0f;
	float action_type_ = 0.0f;
	float zoom_ = 0.0f;
	float fire_logic_ = float(kInitialFireLogicType);
	float overheat_ = 0.0f;
	float lock_amount_ = 0.0f;
};

}
