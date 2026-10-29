#include "bf3/ai/knowledge/information_factory.h"

#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

namespace bf3::ai {

InformationFactory::InformationFactory(const KnowledgeContext &p_context) : context_(p_context) {
	context_.factory = this;
}

uint32_t InformationFactory::request_information_real(uint32_t p_handle, AiObject *p_object, uint32_t p_types,
		float p_basic_temperature, sim::Tick p_time, sim::Tick p_allowed_time_diff) const {
	return register_information(p_handle,
			std::make_unique<InformationReal>(
					p_object, p_types, p_basic_temperature, p_time, p_allowed_time_diff, context_));
}

bool InformationFactory::request_information_plug_in(uint32_t p_handle, PlugInRequest p_type, bool p_real) const {
	return request_information_plug_in(p_handle, p_handle, p_type, p_real);
}

bool InformationFactory::request_information_plug_in(
		uint32_t p_source, uint32_t p_target, PlugInRequest p_type, bool p_real) const {
	if (context_.information == nullptr) {
		return false;
	}
	Information *const source = context_.information->information(p_source);
	Information *const target = context_.information->information(p_target);
	if (source == nullptr || target == nullptr) {
		return false;
	}
	switch (p_type) {
		case PlugInRequest::Armament:
			if (!p_real) {
				return false;
			}
			target->set_armament(std::make_unique<IPIArmamentReal>(source));
			return true;
		case PlugInRequest::ControlInfo:
			target->set_control_info(std::make_unique<IPIControlInfo>(source));
			return true;
		case PlugInRequest::Cover:
			target->set_cover(std::make_unique<IPICover>(source));
			return true;
		case PlugInRequest::Mobile:
			if (p_real) {
				target->set_mobile(std::make_unique<IPIMobileReal>(source));
			} else {
				target->set_mobile(std::make_unique<IPIMobileKnown>(source));
			}
			return true;
		case PlugInRequest::Physical:
			if (!p_real) {
				return false;
			}
			target->set_physical(std::make_unique<IPIPhysicalReal>(source));
			return true;
		case PlugInRequest::Unit:
			target->set_unit(std::make_unique<IPIUnit>(source));
			return true;
	}
	return false;
}

bool InformationFactory::update_information_plug_in(uint32_t p_handle, PlugInRequest p_type) const {
	if (p_type != PlugInRequest::Armament || context_.information == nullptr) {
		return false;
	}
	Information *const information = context_.information->information(p_handle);
	if (information == nullptr || information->armament() == nullptr) {
		return false;
	}
	information->armament()->update_weapons();
	return true;
}

uint32_t InformationFactory::request_weapon_info(uint32_t p_handle, AiWeapon *p_weapon, bool p_real) const {
	if (!p_real) {
		return kNoHandle;
	}
	return register_weapon(p_handle, std::make_unique<WeaponInfoReal>(p_weapon));
}

bool InformationFactory::check_if_info_exists(uint32_t p_handle) const {
	return context_.information != nullptr && context_.information->information(p_handle) != nullptr;
}

bool InformationFactory::check_if_weapon_info_exists(uint32_t p_handle) const {
	return context_.weapons != nullptr && context_.weapons->information(p_handle) != nullptr;
}

uint32_t InformationFactory::register_information(uint32_t p_handle, std::unique_ptr<Information> p_information) const {
	if (context_.information == nullptr) {
		return kNoHandle;
	}
	if (p_handle == kNoHandle) {
		return context_.information->pool().acquire(std::move(p_information));
	}
	context_.information->pool().set(p_handle, std::move(p_information));
	return p_handle;
}

uint32_t InformationFactory::register_weapon(uint32_t p_handle, std::unique_ptr<WeaponInfo> p_weapon) const {
	if (context_.weapons == nullptr) {
		return kNoHandle;
	}
	if (p_handle == kNoHandle) {
		return context_.weapons->register_information(std::move(p_weapon));
	}
	context_.weapons->set_information(p_handle, std::move(p_weapon));
	return p_handle;
}

}
