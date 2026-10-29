#include "bf3/ai/knowledge/information.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"

#include <cmath>

namespace bf3::ai {

Information::Information(AiObject *p_object, sim::Tick p_time, const KnowledgeContext &p_context) :
		time_(p_time), object_(p_object), context_(p_context) {}

Information::~Information() {
	physical_.reset();
	cover_.reset();
	mobile_.reset();
	armament_.reset();
	unit_.reset();
	control_info_.reset();
}

int Information::information_sort() const {
	return 3;
}

bool Information::remove_plug_in(PlugInType p_type) {
	switch (p_type) {
		case PlugInType::Physical:
			return physical_ ? (physical_.reset(), true) : false;
		case PlugInType::Cover:
			return cover_ ? (cover_.reset(), true) : false;
		case PlugInType::Mobile:
			return mobile_ ? (mobile_.reset(), true) : false;
		case PlugInType::Unit:
			return unit_ ? (unit_.reset(), true) : false;
		case PlugInType::Armament:
			borrowed_armament_ = nullptr;
			return armament_ ? (armament_.reset(), true) : false;
		case PlugInType::ControlInfo:
			return control_info_ ? (control_info_.reset(), true) : false;
	}
	return false;
}

void Information::set_time_on_plug_ins(sim::Tick p_time) {
	IPIArmament *const arms = armament();
	if (arms != nullptr && (unit_ == nullptr || !unit_->must_borrow_armament()) &&
			(physical_ == nullptr || !physical_->man_down())) {
		arms->set_time(p_time);
	}
	if (control_info_) {
		control_info_->set_time(p_time);
	}
	if (cover_) {
		cover_->set_time(p_time);
	}
	if (mobile_) {
		mobile_->set_time(p_time);
	}
	if (physical_) {
		physical_->set_time(p_time);
	}
	if (unit_) {
		unit_->set_time(p_time);
	}
}

bool Information::set_armament(std::unique_ptr<IPIArmament> p_armament) {
	if (armament() != nullptr) {
		return false;
	}
	armament_ = std::move(p_armament);
	return true;
}

void Information::borrow_armament(Information *p_lender) {
	borrowed_armament_ = p_lender->armament();
}

void Information::reset_armament() {
	borrowed_armament_ = nullptr;
	armament_.reset();
}

IPIArmament *Information::armament() const {
	return borrowed_armament_ != nullptr ? borrowed_armament_ : armament_.get();
}

bool Information::set_control_info(std::unique_ptr<IPIControlInfo> p_control_info) {
	if (control_info_) {
		return false;
	}
	control_info_ = std::move(p_control_info);
	return true;
}

bool Information::set_cover(std::unique_ptr<IPICover> p_cover) {
	if (cover_) {
		return false;
	}
	cover_ = std::move(p_cover);
	return true;
}

bool Information::set_mobile(std::unique_ptr<IPIMobile> p_mobile) {
	if (mobile_) {
		return false;
	}
	mobile_ = std::move(p_mobile);
	return true;
}

bool Information::set_physical(std::unique_ptr<IPIPhysical> p_physical) {
	if (physical_) {
		return false;
	}
	physical_ = std::move(p_physical);
	return true;
}

bool Information::set_unit(std::unique_ptr<IPIUnit> p_unit) {
	if (unit_) {
		return false;
	}
	unit_ = std::move(p_unit);
	return true;
}

const std::string &Information::object_type_name() const {
	return object_->object_type_name();
}

uint32_t Information::object_id() const {
	return object_->object_id();
}

int Information::player_id() const {
	return object_->player_id();
}

bool Information::is_primary() const {
	return object_->is_primary();
}

uint32_t Information::primary_object(int p_side) const {
	const AiObject *const primary = object_->primary_object();
	return primary ? primary->information(p_side) : kNoHandle;
}

uint32_t Information::secondary_object(int &r_iterator, int p_side) const {
	const AiObject *const secondary = object_->secondary_object(r_iterator);
	return secondary ? secondary->information(p_side) : kNoHandle;
}

uint32_t Information::first_secondary_object(int &r_iterator, int p_side) const {
	const AiObject *const secondary = object_->first_secondary_object(r_iterator);
	return secondary ? secondary->information(p_side) : kNoHandle;
}

int Information::first_secondary_object_iterator() const {
	return object_->first_secondary_object_iterator();
}

const std::string &Information::name() const {
	return object_->name();
}

const std::string &Information::ai_name() const {
	return object_->ai_name();
}

bool Information::object_spawner_holding() const {
	return object_->object_spawner_holding();
}

uint32_t Information::object_spawner(int p_side) const {
	const AiObject *const spawner = object_->object_spawner();
	return spawner ? spawner->information(p_side) : kNoHandle;
}

uint32_t Information::handle() const {
	for (int side = 1; side <= 2; ++side) {
		const uint32_t handle = object_->information(side);
		if (context_.information != nullptr && context_.information->information(handle) == this) {
			return handle;
		}
	}
	return kNoHandle;
}

int Information::kit_type() const {
	return object_->kit_type();
}

bool Information::is_root_object() const {
	return object_->is_root_object();
}

SoldierPose Information::soldier_pose() const {
	return object_->soldier_pose();
}

void Information::book_vehicle(int p_player_id) {
	booked_ = true;
	booking_player_id_ = p_player_id;
}

void Information::cancel_book_vehicle() {
	booked_ = false;
	booking_player_id_ = -1;
}

InformationReal::InformationReal(AiObject *p_object, uint32_t p_types, float p_basic_temperature, sim::Tick p_time,
		sim::Tick p_allowed_time_diff, const KnowledgeContext &p_context) :
		Information(p_object, p_time > 2 * p_allowed_time_diff ? p_time - 2 * p_allowed_time_diff : 0, p_context) {
	allowed_time_diff_ = p_allowed_time_diff;
	strength_ = p_basic_temperature;
	basic_temperature_ = p_basic_temperature;
	object_type_flags_ = p_types;
}

int InformationReal::information_sort() const {
	return 0;
}

bool InformationReal::set_time(sim::Tick p_time) {
	if (is_primary() && context_.information != nullptr) {
		int iterator = -1;
		const int own_side = side();
		Information *secondary = context_.information->information(first_secondary_object(iterator, own_side));
		while (secondary != nullptr) {
			secondary->set_time(p_time);
			secondary = context_.information->information(secondary_object(iterator, own_side));
		}
	}
	if (unit() != nullptr && physical() != nullptr) {
		physical()->set_time(p_time);
		unit()->calculate_strength_mod(physical()->health());
	}
	set_time_on_plug_ins(p_time);
	return true;
}

bool InformationReal::set_time(sim::Tick p_time, float) {
	return set_time(p_time);
}

sim::Tick InformationReal::time_stamp() const {
	return context_.now();
}

void InformationReal::add_contact() {}

void InformationReal::remove_contact() {}

void InformationReal::remove_all_contacts() {}

bool InformationReal::has_contact() const {
	return true;
}

float InformationReal::security(sim::Tick) const {
	return 1.0f;
}

float InformationReal::security() const {
	return 1.0f;
}

const Vec3 &InformationReal::position() const {
	return object_->position();
}

float InformationReal::altitude() const {
	return context_.altitude ? context_.altitude->altitude(object_->position()) : 0.0f;
}

float InformationReal::altitude(const Vec3 &p_direction) const {
	if (context_.altitude == nullptr) {
		return 0.0f;
	}
	const float length = float(std::sqrt(double(p_direction.x) * p_direction.x + double(p_direction.y) * p_direction.y +
			double(p_direction.z) * p_direction.z));
	Vec3 probe = object_->position();
	float lowest = context_.altitude->altitude(probe);
	const float step_x = float(p_direction.x * 0.1);
	const float step_y = float(p_direction.y * 0.1);
	const float step_z = float(p_direction.z * 0.1);
	const double step_length = length * 0.1;
	const float step = float(step_length);
	float travelled = step;
	probe.x = float(double(step_x) + probe.x);
	probe.y = float(double(step_y) + probe.y);
	probe.z = float(double(step_z) + probe.z);
	if (step_length < length) {
		double next = 0.0;
		do {
			probe.x = float(double(step_x) + probe.x);
			probe.y = float(double(step_y) + probe.y);
			probe.z = float(double(step_z) + probe.z);
			const float altitude = context_.altitude->altitude(probe);
			if (lowest >= altitude) {
				lowest = altitude;
			}
			next = double(travelled) + step;
			travelled = float(next);
		} while (next < length);
	}
	return lowest;
}

const Transform &InformationReal::transformation() const {
	return object_->transformation();
}

Vec3 InformationReal::facing() const {
	return object_->transformation().forward;
}

int InformationReal::side() const {
	return object_->side();
}

}
