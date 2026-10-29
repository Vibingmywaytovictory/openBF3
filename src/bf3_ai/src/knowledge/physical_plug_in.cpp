#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/knowledge/plug_ins.h"

namespace bf3::ai {

IPIPhysicalReal::IPIPhysicalReal(Information *p_information) : IPIPhysical(p_information) {}

const AiObjectPhysical &IPIPhysicalReal::object() const {
	return *information_->ai_object()->physical();
}

void IPIPhysicalReal::set_time() {
	set_time(information_->context().now());
}

void IPIPhysicalReal::set_time(sim::Tick) {
	const AiObjectPhysical &physical = object();
	const float health = physical.max_health() <= 0.0f ? 0.0f : physical.health();
	mod_ = 0.5f;
	mod_ = float(double(SCurve::calculate(health)) * 0.5 + mod_);
}

float IPIPhysicalReal::health() const {
	const AiObjectPhysical &physical = object();
	return physical.health() / physical.max_health();
}

void IPIPhysicalReal::set_health(float) {}

float IPIPhysicalReal::max_health() const {
	return object().max_health();
}

float IPIPhysicalReal::mod() {
	set_time();
	return mod_;
}

float IPIPhysicalReal::radius() const {
	return object().radius();
}

Vec3 IPIPhysicalReal::sphere_local_offset() const {
	return object().sphere_local_offset();
}

float IPIPhysicalReal::smallest_radius() const {
	return object().smallest_radius();
}

void IPIPhysicalReal::sub_spheres_2d(std::vector<Vec2> &r_out) const {
	object().sub_spheres_2d(r_out);
}

float IPIPhysicalReal::armour(const Vec3 &p_direction) const {
	return object().armour(p_direction);
}

float IPIPhysicalReal::best_armour() const {
	return object().best_armour();
}

int IPIPhysicalReal::strength_type() const {
	return object().strength_type();
}

float IPIPhysicalReal::volume() const {
	return object().volume();
}

float IPIPhysicalReal::mass() const {
	return object().mass();
}

int IPIPhysicalReal::material() const {
	return object().material();
}

float IPIPhysicalReal::man_down_time_left() const {
	return object().man_down_time_left();
}

float IPIPhysicalReal::max_man_down_time() const {
	return object().max_man_down_time();
}

bool IPIPhysicalReal::man_down() const {
	return object().man_down();
}

bool IPIPhysicalReal::is_touching_terrain(Vec3 &r_position) const {
	return object().is_touching_terrain(r_position);
}

bool IPIPhysicalReal::is_in_air() const {
	return object().is_in_air();
}

bool IPIPhysicalReal::is_in_water() const {
	return object().is_in_water();
}

}
