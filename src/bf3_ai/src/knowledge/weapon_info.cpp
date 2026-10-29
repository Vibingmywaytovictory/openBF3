#include "bf3/ai/knowledge/weapon_info.h"

namespace bf3::ai {

WeaponInfo::WeaponInfo(AiWeapon *p_weapon) : weapon_(p_weapon) {}

bool WeaponInfo::update() {
	return true;
}

int WeaponInfo::fire_rate() const {
	return weapon_->fire_rate();
}

float WeaponInfo::reload_time() const {
	return weapon_->reload_time();
}

const Transform &WeaponInfo::absolute_transformation() const {
	return weapon_->absolute_transformation();
}

const Vec3 &WeaponInfo::position() const {
	return weapon_->position();
}

const Transform &WeaponInfo::projectile_origin() const {
	return weapon_->projectile_origin();
}

bool WeaponInfo::has_auto_reload() const {
	return weapon_->has_auto_reload();
}

bool WeaponInfo::has_heat() const {
	return weapon_->has_heat();
}

int WeaponInfo::weapon_type() const {
	return weapon_->weapon_type();
}

int WeaponInfo::weapon_guidance_type() const {
	return weapon_->weapon_guiding_system();
}

bool WeaponInfo::burst() const {
	return weapon_->burst();
}

bool WeaponInfo::indirect() const {
	return weapon_->indirect();
}

int WeaponInfo::projectile_material() const {
	return weapon_->projectile_material();
}

int WeaponInfo::max_ammo_in_mag() const {
	return weapon_->max_ammo_in_mag();
}

float WeaponInfo::drag() const {
	return weapon_->drag();
}

float WeaponInfo::gravity_modifier() const {
	return weapon_->gravity_modifier();
}

float WeaponInfo::exit_velocity() const {
	return weapon_->exit_velocity();
}

float WeaponInfo::min_range() const {
	return weapon_->min_range();
}

float WeaponInfo::max_range() const {
	return weapon_->max_range();
}

float WeaponInfo::optimal_range_percentage() const {
	return weapon_->optimal_range_percentage();
}

bool WeaponInfo::fires_through_transparent() const {
	return weapon_->fires_through_transparent();
}

SoldierPose WeaponInfo::firing_pose() const {
	return weapon_->firing_pose();
}

float WeaponInfo::allowed_deviation() const {
	return weapon_->allowed_deviation();
}

float WeaponInfo::explosion_radius() const {
	return weapon_->explosion_radius();
}

const int *WeaponInfo::strengths() const {
	return weapon_->strengths();
}

int WeaponInfo::weapon_activate() const {
	return weapon_->weapon_activate();
}

int WeaponInfo::weapon_fire() const {
	return weapon_->weapon_fire();
}

int WeaponInfo::weapon_zoom() const {
	return weapon_->weapon_zoom();
}

int WeaponInfo::weapon_alt_fire() const {
	return weapon_->weapon_alt_fire();
}

bool WeaponInfo::is_zoomed() const {
	return weapon_->is_zoomed();
}

int WeaponInfo::weapon_state() const {
	return weapon_->weapon_state();
}

int WeaponInfo::max_ammo() const {
	return weapon_->max_ammo();
}

bool WeaponInfo::is_thrown() const {
	return weapon_->is_thrown();
}

bool WeaponInfo::use_aimer_only() const {
	return weapon_->use_aimer_only();
}

float WeaponInfo::sound_sphere_radius() const {
	return weapon_->sound_sphere_radius();
}

float WeaponInfo::replenish_radius() const {
	return weapon_->replenish_radius();
}

bool WeaponInfo::has_target_locked() const {
	return weapon_->has_target_locked();
}

AiObject *WeaponInfo::locked_target() const {
	return weapon_->locked_target();
}

const std::string &WeaponInfo::name() const {
	return weapon_->name();
}

int WeaponInfo::strength(int p_index) const {
	return weapon_->strength(p_index);
}

bool WeaponInfo::set_fake_fire(bool p_fake) const {
	return weapon_->set_fake_fire(p_fake);
}

void WeaponInfo::set_bot_skill(float p_skill, sim::Tick p_since, float p_scale) {
	weapon_->set_bot_skill(p_skill, p_since, p_scale);
}

bool WeaponInfo::set_target(uint32_t p_target_id) {
	return weapon_->set_target(p_target_id);
}

bool WeaponInfoReal::update() {
	return true;
}

int WeaponInfoReal::shots_in_mag(int p_magazine) const {
	return weapon_->ammo_in_mag(p_magazine);
}

int WeaponInfoReal::ammo() const {
	return weapon_->ammo();
}

int WeaponInfoReal::current_ammo() const {
	return weapon_->current_ammo();
}

int WeaponInfoReal::number_of_mags() const {
	return weapon_->number_of_mags();
}

bool WeaponInfoReal::is_ready_to_fire() const {
	return weapon_->is_ready_to_fire();
}

sim::Tick WeaponInfoReal::time_of_firing() const {
	return weapon_->time_of_firing();
}

float WeaponInfoReal::heat() const {
	return weapon_->heat();
}

bool WeaponInfoReal::uses_heat() const {
	return weapon_->uses_heat();
}

float WeaponInfoReal::security() const {
	return 1.0f;
}

float WeaponInfoReal::security(sim::Tick) const {
	return 1.0f;
}

uint32_t WeaponInfoManager::register_information(std::unique_ptr<WeaponInfo> p_info) {
	return pool_.acquire(std::move(p_info));
}

WeaponInfo *WeaponInfoManager::information(uint32_t p_handle) const {
	return pool_.get(p_handle);
}

bool WeaponInfoManager::set_information(uint32_t p_handle, std::unique_ptr<WeaponInfo> p_info) {
	return pool_.set(p_handle, std::move(p_info));
}

void WeaponInfoManager::free_information(uint32_t p_handle) {
	pool_.release(p_handle);
}

bool WeaponInfoManager::change_information(uint32_t p_handle, std::unique_ptr<WeaponInfo> p_info) {
	return pool_.set(p_handle, std::move(p_info));
}

bool WeaponInfoManager::swap_information(uint32_t p_a, uint32_t p_b) {
	return pool_.swap(p_a, p_b);
}

void WeaponInfoManager::reset() {
	pool_.reset();
}

}
