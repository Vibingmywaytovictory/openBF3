#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

namespace bf3::ai {

IPIPhysicalKnown::IPIPhysicalKnown(Information *p_information) : IPIPhysical(p_information) {}

const AiObjectPhysical &IPIPhysicalKnown::object() const {
	return *information_->ai_object()->physical();
}

void IPIPhysicalKnown::set_time() {
	set_time(information_->context().now());
}

void IPIPhysicalKnown::set_time(sim::Tick) {
	const AiObjectPhysical &physical = object();
	man_down_ = physical.man_down();
	health_ = physical.max_health() > 0.0f ? physical.health() : 0.0f;
	mod_ = 0.5f;
	mod_ = SCurve::calculate(health_) * 0.5f + mod_;
	touching_terrain_ = physical.is_touching_terrain(terrain_position_);
	in_air_ = physical.is_in_air();
	in_water_ = physical.is_in_water();
}

float IPIPhysicalKnown::max_health() const {
	return object().max_health();
}

float IPIPhysicalKnown::radius() const {
	return object().radius();
}

Vec3 IPIPhysicalKnown::sphere_local_offset() const {
	return object().sphere_local_offset();
}

float IPIPhysicalKnown::smallest_radius() const {
	return object().smallest_radius();
}

void IPIPhysicalKnown::sub_spheres_2d(std::vector<Vec2> &r_out) const {
	object().sub_spheres_2d(r_out);
}

float IPIPhysicalKnown::armour(const Vec3 &p_direction) const {
	return object().armour(p_direction);
}

float IPIPhysicalKnown::best_armour() const {
	return object().best_armour();
}

int IPIPhysicalKnown::strength_type() const {
	return object().strength_type();
}

float IPIPhysicalKnown::volume() const {
	return object().volume();
}

float IPIPhysicalKnown::mass() const {
	return object().mass();
}

int IPIPhysicalKnown::material() const {
	return object().material();
}

float IPIPhysicalKnown::man_down_time_left() const {
	return object().man_down_time_left();
}

float IPIPhysicalKnown::max_man_down_time() const {
	return object().max_man_down_time();
}

bool IPIPhysicalKnown::is_touching_terrain(Vec3 &r_position) const {
	r_position = terrain_position_;
	return touching_terrain_;
}

IPIArmamentKnown::IPIArmamentKnown(Information *p_information) : IPIArmament(p_information) {
	refresh();
}

IPIArmamentKnown::~IPIArmamentKnown() {
	WeaponInfoManager *const manager = information_->context().weapons;
	if (manager != nullptr) {
		for (const uint32_t handle : weapons_) {
			manager->free_information(handle);
		}
	}
}

void IPIArmamentKnown::refresh() const {
	if (stale_) {
		const_cast<IPIArmamentKnown *>(this)->update_weapons_internal(false);
		stale_ = false;
	}
}

void IPIArmamentKnown::set_time(sim::Tick) {
	refresh();
	const WeaponInfoManager *const manager = information_->context().weapons;
	if (manager == nullptr) {
		return;
	}
	for (const uint32_t handle : weapons_) {
		if (handle == kNoHandle) {
			continue;
		}
		WeaponInfo *const weapon = manager->information(handle);
		if (weapon != nullptr) {
			weapon->update();
		}
	}
}

uint32_t IPIArmamentKnown::weapon(int p_index) const {
	refresh();
	return weapons_[size_t(p_index)];
}

const std::vector<uint32_t> &IPIArmamentKnown::weapons() const {
	refresh();
	return weapons_;
}

}
