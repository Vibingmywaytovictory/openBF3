#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

namespace bf3::ai {

IPIArmament::IPIArmament(Information *p_information) : InformationPlugIn(p_information) {}

AiObjectArmament &IPIArmament::object() const {
	return *information_->ai_object()->armament();
}

void IPIArmament::set_time() {
	set_time(information_->context().now());
}

void IPIArmament::set_time(sim::Tick) {}

int IPIArmament::active_weapon() const {
	return object().active_weapon();
}

void IPIArmament::set_active_weapon(int p_weapon, int p_side) {
	object().set_active_weapon(p_weapon, p_side);
}

int IPIArmament::weapon_id_from_activation_id(int p_activation, int p_fire) const {
	return object().weapon_id_from_activation_id(p_activation, p_fire);
}

WeaponInfo *IPIArmament::last_fired_weapon() const {
	const AiObjectArmament &armament = object();
	const AiWeapon *const weapon = armament.last_fired_weapon();
	const WeaponInfoManager *const manager = information_->context().weapons;
	if (weapon == nullptr || manager == nullptr) {
		return nullptr;
	}
	const int index = armament.weapon_id_from_activation_id(weapon->weapon_activate(), weapon->weapon_fire());
	if (index < 0 || size_t(index) >= weapons_.size()) {
		return nullptr;
	}
	return manager->information(weapons_[size_t(index)]);
}

int IPIArmament::is_anti_aircraft() const {
	return object().is_anti_aircraft();
}

void IPIArmament::rearm(WeaponInfo &r_weapon, float p_amount, int p_side) {
	r_weapon.rearm_amount_ += p_amount;
	if (object().rearm(r_weapon.weapon_, r_weapon.rearm_amount_, p_side)) {
		r_weapon.rearm_amount_ = 0.0f;
	}
}

bool IPIArmament::projectile_position(Vec3 &r_out) const {
	return object().projectile_position(r_out);
}

bool IPIArmament::projectile_speed(Vec3 &r_out) const {
	return object().projectile_speed(r_out);
}

void IPIArmament::update_weapon_internal(int p_index, bool p_real) {
	const std::vector<AiWeapon *> &engine_weapons = object().weapons();
	if (engine_weapons.size() != weapons_.size()) {
		update_weapons_internal(p_real);
		return;
	}
	const KnowledgeContext &context = information_->context();
	const size_t index = size_t(p_index);
	if (context.weapons != nullptr) {
		context.weapons->free_information(weapons_[index]);
	}
	weapons_[index] = kNoHandle;
	AiWeapon *const weapon = engine_weapons[index];
	if (weapon != nullptr && context.factory != nullptr) {
		weapons_[index] = context.factory->request_weapon_info(weapons_[index], weapon, p_real);
	}
}

void IPIArmament::update_weapons_internal(bool p_real) {
	const KnowledgeContext &context = information_->context();
	for (uint32_t &handle : weapons_) {
		if (context.weapons != nullptr) {
			context.weapons->free_information(handle);
		}
		handle = kNoHandle;
	}
	const std::vector<AiWeapon *> &engine_weapons = object().weapons();
	weapons_.resize(engine_weapons.size(), kNoHandle);
	for (size_t i = 0; i < engine_weapons.size(); ++i) {
		AiWeapon *const weapon = engine_weapons[i];
		if (weapon != nullptr && context.factory != nullptr) {
			weapons_[i] = context.factory->request_weapon_info(weapons_[i], weapon, p_real);
		}
	}
}

IPIArmamentReal::IPIArmamentReal(Information *p_information) : IPIArmament(p_information) {
	update_weapons();
}

IPIArmamentReal::~IPIArmamentReal() {
	WeaponInfoManager *const manager = information_->context().weapons;
	if (manager != nullptr) {
		for (const uint32_t handle : weapons_) {
			manager->free_information(handle);
		}
	}
}

uint32_t IPIArmamentReal::weapon(int p_index) const {
	return weapons_[size_t(p_index)];
}

const std::vector<uint32_t> &IPIArmamentReal::weapons() const {
	return weapons_;
}

void IPIArmamentReal::update_weapons() {
	update_weapons_internal(true);
}

}
