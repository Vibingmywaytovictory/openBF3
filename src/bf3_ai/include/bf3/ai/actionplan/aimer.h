#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <vector>

namespace bf3::ai {

void cubic_roots(float p_c0, float p_c1, float p_c2, float p_c3, std::vector<float> &r_roots);

class Aimer {
public:
	static constexpr float kNoAngle = 666.0f;
	static constexpr float kMaxElevation = 2.984513f;
	static constexpr float kGrenadeFuseSeconds = 2.8f;

	bool set_target_data(
			const Vec3 &p_relative_pos, const Vec3 &p_target_velocity, const Vec3 &p_shooter_velocity, double p_time);
	float impact_time(float p_distance, float p_speed) const;
	void predicted_impact_position(Vec3 p_velocity, float p_time, Vec3 &r_out) const;
	bool static_firing_direction(Vec3 &r_direction, Vec3 &r_target) const;
	bool turn_point_for_grenade(const Vec3 &p_direction, Vec3 &r_point, float &r_time) const;
	bool firing_direction(
			double p_time, float p_accuracy, bool p_high_arc, Vec3 &r_direction, Vec3 &r_target, double *r_impact_time);
	void predicted_target_position(double p_time, Vec3 &r_out) const;
	bool impact_position(Vec3 &r_out, const Vec3 &p_direction, float p_time) const;
	bool nearest_impact_point(const Vec3 &p_direction, Vec3 &r_out, double &r_time) const;
	double vertical_extreme() const { return vertical_extreme_; }
	float angle() const { return angle_; }
	float angle_step() const { return angle_step_; }

	float drag = 0.0f;
	float gravity = 0.0f;
	float speed = 1.0f;
	float time_step = float(sim::kTickSeconds);
	bool grenade = false;
	bool grenade_exact = false;

private:
	double time_ = 0.0;
	Vec3 relative_pos_{ 1.0f, 0.0f, 0.0f };
	Vec3 relative_dir_{ 1.0f, 0.0f, 0.0f };
	Vec3 target_velocity_{};
	Vec3 shooter_velocity_{};
	double vertical_extreme_ = 0.0;
	float angle_ = kNoAngle;
	float angle_step_ = 0.0174533f;
};

inline constexpr float kSoldierAimAccuracy = 0.5f;

bool aim_vector(Aimer &r_aimer, const Vec3 &p_origin, const Vec3 &p_target, const Vec3 &p_target_velocity,
		const Vec3 &p_own_velocity, float p_accuracy, float p_drag, float p_speed, float p_gravity, double p_time,
		Vec3 &r_direction);

}
