#pragma once

#include "bf3/ai/knowledge/information.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bf3::ai {

class WeaponInfo;

class IPIPhysical : public InformationPlugIn {
public:
	PlugInType type() const override { return PlugInType::Physical; }
	virtual float health() const = 0;
	virtual void set_health(float p_health) = 0;
	virtual float max_health() const = 0;
	virtual float mod() = 0;
	virtual float radius() const = 0;
	virtual Vec3 sphere_local_offset() const = 0;
	virtual float smallest_radius() const = 0;
	virtual void sub_spheres_2d(std::vector<Vec2> &r_out) const = 0;
	virtual float armour(const Vec3 &p_direction) const = 0;
	virtual float best_armour() const = 0;
	virtual int strength_type() const = 0;
	virtual float volume() const = 0;
	virtual float mass() const = 0;
	virtual int material() const = 0;
	virtual float man_down_time_left() const = 0;
	virtual float max_man_down_time() const = 0;
	virtual bool man_down() const = 0;
	virtual bool is_touching_terrain(Vec3 &r_position) const = 0;
	virtual bool is_in_air() const = 0;
	virtual bool is_in_water() const = 0;

protected:
	using InformationPlugIn::InformationPlugIn;
};

class IPIPhysicalReal final : public IPIPhysical {
public:
	explicit IPIPhysicalReal(Information *p_information);
	void set_time() override;
	void set_time(sim::Tick p_time) override;
	float health() const override;
	void set_health(float p_health) override;
	float max_health() const override;
	float mod() override;
	float radius() const override;
	Vec3 sphere_local_offset() const override;
	float smallest_radius() const override;
	void sub_spheres_2d(std::vector<Vec2> &r_out) const override;
	float armour(const Vec3 &p_direction) const override;
	float best_armour() const override;
	int strength_type() const override;
	float volume() const override;
	float mass() const override;
	int material() const override;
	float man_down_time_left() const override;
	float max_man_down_time() const override;
	bool man_down() const override;
	bool is_touching_terrain(Vec3 &r_position) const override;
	bool is_in_air() const override;
	bool is_in_water() const override;

private:
	const AiObjectPhysical &object() const;
	float mod_ = 0.0f;
};

class IPIUnit final : public InformationPlugIn {
public:
	explicit IPIUnit(Information *p_information);
	PlugInType type() const override { return PlugInType::Unit; }
	void set_time() override;
	void set_time(sim::Tick p_time) override;

	void calculate_strengths();
	float ammo();
	float ammo(sim::Tick p_time);
	bool calculate_strength_mod(float p_health);
	uint32_t occupier(int p_side) const;
	uint32_t occupied_object(int p_side) const;
	int occupying_player_id() const;
	int battle_strength_count() const { return kBattleStrengthCount; }
	int strategic_strength_count() const { return kStrategicStrengthCount; }
	int equipment_type() const;
	bool is_occupied() const;
	bool use_no_pathfinding_to_get_to_object() const;
	bool has_exposed_soldier() const;
	bool always_target_soldier() const;
	float cull_distance() const;
	float cull_distance_sqr() const;
	float entry_radius() const;
	sim::Tick last_player_possession_tick() const;
	int team_lock() const;
	bool is_team_locked(int p_side) const;
	int missile_lock_status() const;
	void set_missile_lock_status(int p_status);
	bool must_borrow_armament() const;
	const Vec3 &entry_position_of_non_mobile() const;
	const float *battle_strengths() const { return battle_strengths_; }
	const float *strategic_strengths() const { return strategic_strengths_; }
	float strength_mod() const { return strength_mod_; }

private:
	AiObjectUnit &object() const;
	float battle_strengths_[kBattleStrengthCount] = {};
	float strategic_strengths_[kStrategicStrengthCount] = {};
	float strength_mod_ = 1.0f;
	float ammo_ = 1.0f;
	std::optional<sim::Tick> ammo_time_;
};

class IPICover final : public InformationPlugIn {
public:
	explicit IPICover(Information *p_information);
	PlugInType type() const override { return PlugInType::Cover; }
	void set_time() override;
	void set_time(sim::Tick p_time) override;

	std::vector<Vec2> local_outline() const;
	const std::vector<Vec2> &outline() const { return outline_; }
	float volume() const;
	float cover_value() const;
	float height() const;
	float transparency() const;

private:
	std::vector<Vec2> outline_;
};

class IPIControlInfo final : public InformationPlugIn {
public:
	explicit IPIControlInfo(Information *p_information) : InformationPlugIn(p_information) {}
	PlugInType type() const override { return PlugInType::ControlInfo; }
	void set_time() override {}
	void set_time(sim::Tick) override {}
};

class IPIMobile : public InformationPlugIn {
public:
	struct ValidPos {
		uint64_t polygon = ~uint64_t(0);
		uint32_t map = kNoHandle;
		Vec3 position{};
	};

	PlugInType type() const override { return PlugInType::Mobile; }
	using InformationPlugIn::set_time;
	void set_time() override;
	virtual void set_has_contact() = 0;
	virtual void set_lost_contact() = 0;
	virtual const Vec3 &speed() const = 0;
	virtual const Vec3 &rotational_speed() const = 0;
	virtual float speed_magnitude() const = 0;
	virtual float sound_sphere_radius() const = 0;
	virtual void position_estimate(Vec3 &r_position) const = 0;
	virtual void position_estimate(sim::Tick p_time, Vec3 &r_position) const = 0;
	virtual uint32_t sector() const = 0;
	virtual int last_passed_portal() const = 0;
	virtual void set_last_valid_infantry_pos(const ValidPos &p_position) = 0;
	virtual void set_last_valid_vehicle_pos(const ValidPos &p_position, uint8_t p_material) = 0;

	int vehicle_number() const;
	float max_speed() const;
	float turn_radius() const;
	float drag() const;
	float cover_search_radius() const;
	float sound_sphere_radius_max() const;
	float sound_sphere_radius_min() const;
	float hearing_probability_max() const;
	float hearing_probability_min() const;
	bool is_turnable() const;
	float avoid_collision_look_ahead() const;
	float sprint_stamina() const;
	const ValidPos &last_valid_position() const;
	const ValidPos &last_valid_position(int p_vehicle_number) const;
	void set_last_valid_infantry_pos_internal(const ValidPos &p_position);
	void set_last_valid_vehicle_pos_internal(const ValidPos &p_position, uint8_t p_material);
	void set_last_valid_vehicle_pos_internal(const ValidPos &p_first, const ValidPos &p_second, bool p_use_first);
	void set_last_valid_positions(
			const ValidPos &p_infantry, const ValidPos &p_first_vehicle, const ValidPos &p_second_vehicle);

protected:
	explicit IPIMobile(Information *p_information);
	const AiObjectMobile &object() const;

	ValidPos last_valid_infantry_pos_;
	ValidPos last_valid_vehicle_pos_a_;
	ValidPos last_valid_vehicle_pos_b_;
	bool use_first_vehicle_valid_pos_ = true;

	friend class IPIMobileKnown;
};

class IPIMobileReal final : public IPIMobile {
public:
	explicit IPIMobileReal(Information *p_information);
	using IPIMobile::set_time;
	void set_time(sim::Tick p_time) override;
	void set_has_contact() override;
	void set_lost_contact() override;
	const Vec3 &speed() const override;
	const Vec3 &rotational_speed() const override;
	float speed_magnitude() const override;
	float sound_sphere_radius() const override;
	void position_estimate(Vec3 &r_position) const override;
	void position_estimate(sim::Tick p_time, Vec3 &r_position) const override;
	uint32_t sector() const override;
	int last_passed_portal() const override;
	void set_last_valid_infantry_pos(const ValidPos &p_position) override;
	void set_last_valid_vehicle_pos(const ValidPos &p_position, uint8_t p_material) override;
};

class IPIMobileKnown final : public IPIMobile {
public:
	explicit IPIMobileKnown(Information *p_information);
	using IPIMobile::set_time;
	void set_time(sim::Tick p_time) override;
	void set_has_contact() override;
	void set_lost_contact() override;
	const Vec3 &speed() const override;
	const Vec3 &rotational_speed() const override;
	float speed_magnitude() const override;
	float sound_sphere_radius() const override;
	void position_estimate(Vec3 &r_position) const override;
	void position_estimate(sim::Tick p_time, Vec3 &r_position) const override;
	uint32_t sector() const override;
	int last_passed_portal() const override;
	void set_last_valid_infantry_pos(const ValidPos &p_position) override;
	void set_last_valid_vehicle_pos(const ValidPos &p_position, uint8_t p_material) override;
	bool contact() const { return has_contact_; }

private:
	void steal_last_valid();
	Vec3 speed_{};
	Vec3 rotational_speed_{};
	sim::Tick time_ = 0;
	uint32_t sector_ = kNoHandle;
	int last_passed_portal_ = -1;
	bool has_contact_ = false;
};

class IPIArmament : public InformationPlugIn {
public:
	PlugInType type() const override { return PlugInType::Armament; }
	void set_time() override;
	void set_time(sim::Tick p_time) override;
	virtual int active_weapon() const;
	virtual void set_active_weapon(int p_weapon, int p_side);
	virtual uint32_t weapon(int p_index) const = 0;
	virtual const std::vector<uint32_t> &weapons() const = 0;
	virtual int weapon_id_from_activation_id(int p_activation, int p_fire) const;
	virtual WeaponInfo *last_fired_weapon() const;
	virtual int is_anti_aircraft() const;
	virtual void update_weapons() = 0;
	virtual void rearm(WeaponInfo &r_weapon, float p_amount, int p_side);
	virtual bool projectile_position(Vec3 &r_out) const;
	virtual bool projectile_speed(Vec3 &r_out) const;

protected:
	explicit IPIArmament(Information *p_information);
	AiObjectArmament &object() const;
	void update_weapon_internal(int p_index, bool p_real);
	void update_weapons_internal(bool p_real);

	std::vector<uint32_t> weapons_;
};

class IPIArmamentReal final : public IPIArmament {
public:
	explicit IPIArmamentReal(Information *p_information);
	~IPIArmamentReal() override;
	uint32_t weapon(int p_index) const override;
	const std::vector<uint32_t> &weapons() const override;
	void update_weapons() override;
};

}
