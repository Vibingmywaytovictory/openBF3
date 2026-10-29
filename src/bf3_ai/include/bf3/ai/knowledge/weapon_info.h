#pragma once

#include "bf3/ai/knowledge/handle_pool.h"

#include <memory>
#include <string>

namespace bf3::ai {

class IPIArmament;

class WeaponInfo {
public:
	explicit WeaponInfo(AiWeapon *p_weapon);
	virtual ~WeaponInfo() = default;

	virtual bool update();
	virtual int shots_in_mag(int p_magazine) const = 0;
	virtual int ammo() const = 0;
	virtual int current_ammo() const = 0;
	virtual int number_of_mags() const = 0;
	virtual bool is_ready_to_fire() const = 0;
	virtual sim::Tick time_of_firing() const = 0;
	virtual float heat() const = 0;
	virtual bool uses_heat() const = 0;
	virtual float security() const = 0;
	virtual float security(sim::Tick p_time) const = 0;

	int fire_rate() const;
	float reload_time() const;
	const Transform &absolute_transformation() const;
	const Vec3 &position() const;
	const Transform &projectile_origin() const;
	bool has_auto_reload() const;
	bool has_heat() const;
	int weapon_type() const;
	int weapon_guidance_type() const;
	bool burst() const;
	bool indirect() const;
	int projectile_material() const;
	int max_ammo_in_mag() const;
	float drag() const;
	float gravity_modifier() const;
	float exit_velocity() const;
	float min_range() const;
	float max_range() const;
	float optimal_range_percentage() const;
	bool fires_through_transparent() const;
	SoldierPose firing_pose() const;
	float allowed_deviation() const;
	float explosion_radius() const;
	const int *strengths() const;
	int strength(int p_index) const;
	int weapon_activate() const;
	int weapon_fire() const;
	int weapon_zoom() const;
	int weapon_alt_fire() const;
	bool is_zoomed() const;
	int weapon_state() const;
	int max_ammo() const;
	bool is_thrown() const;
	bool use_aimer_only() const;
	float sound_sphere_radius() const;
	bool set_fake_fire(bool p_fake) const;
	float replenish_radius() const;
	void set_bot_skill(float p_skill, sim::Tick p_since, float p_scale);
	bool set_target(uint32_t p_target_id);
	bool has_target_locked() const;
	AiObject *locked_target() const;
	const std::string &name() const;
	AiWeapon *weapon() const { return weapon_; }
	float rearm_amount() const { return rearm_amount_; }

protected:
	friend class IPIArmament;
	AiWeapon *weapon_ = nullptr;
	float rearm_amount_ = 0.0f;
};

class WeaponInfoReal final : public WeaponInfo {
public:
	explicit WeaponInfoReal(AiWeapon *p_weapon) : WeaponInfo(p_weapon) {}
	bool update() override;
	int shots_in_mag(int p_magazine) const override;
	int ammo() const override;
	int current_ammo() const override;
	int number_of_mags() const override;
	bool is_ready_to_fire() const override;
	sim::Tick time_of_firing() const override;
	float heat() const override;
	bool uses_heat() const override;
	float security() const override;
	float security(sim::Tick p_time) const override;
};

class WeaponInfoManager {
public:
	uint32_t register_information(std::unique_ptr<WeaponInfo> p_info);
	WeaponInfo *information(uint32_t p_handle) const;
	bool set_information(uint32_t p_handle, std::unique_ptr<WeaponInfo> p_info);
	void free_information(uint32_t p_handle);
	bool change_information(uint32_t p_handle, std::unique_ptr<WeaponInfo> p_info);
	bool swap_information(uint32_t p_a, uint32_t p_b);
	void reset();
	size_t live_count() const { return pool_.live_count(); }

private:
	HandlePool<WeaponInfo> pool_;
};

}
