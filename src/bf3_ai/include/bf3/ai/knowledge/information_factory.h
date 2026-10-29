#pragma once

#include "bf3/ai/knowledge/handle_pool.h"
#include "bf3/ai/knowledge/information.h"

#include <memory>

namespace bf3::ai {

class WeaponInfo;

class InformationManager {
public:
	HandlePool<Information> &pool() { return pool_; }
	const HandlePool<Information> &pool() const { return pool_; }
	Information *information(uint32_t p_handle) const { return pool_.get(p_handle); }
	bool free_information(uint32_t p_handle) { return pool_.release(p_handle); }

private:
	HandlePool<Information> pool_;
};

class InformationFactory {
public:
	explicit InformationFactory(const KnowledgeContext &p_context);

	uint32_t request_information_real(uint32_t p_handle, AiObject *p_object, uint32_t p_types,
			float p_basic_temperature, sim::Tick p_time, sim::Tick p_allowed_time_diff) const;
	bool request_information_plug_in(uint32_t p_handle, PlugInRequest p_type, bool p_real) const;
	bool request_information_plug_in(uint32_t p_source, uint32_t p_target, PlugInRequest p_type, bool p_real) const;
	bool update_information_plug_in(uint32_t p_handle, PlugInRequest p_type) const;
	uint32_t request_weapon_info(uint32_t p_handle, AiWeapon *p_weapon, bool p_real) const;
	bool check_if_info_exists(uint32_t p_handle) const;
	bool check_if_weapon_info_exists(uint32_t p_handle) const;
	uint32_t register_information(uint32_t p_handle, std::unique_ptr<Information> p_information) const;
	uint32_t register_weapon(uint32_t p_handle, std::unique_ptr<WeaponInfo> p_weapon) const;
	const KnowledgeContext &context() const { return context_; }

private:
	KnowledgeContext context_;
};

}
