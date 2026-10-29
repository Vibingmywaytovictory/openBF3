#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"

#include <cmath>

namespace bf3::ai {

namespace {

double tick_seconds(const KnowledgeContext &p_context) {
	return p_context.timer != nullptr ? p_context.timer->tick_seconds() : 0.0;
}

}

IPIMobile::IPIMobile(Information *p_information) : InformationPlugIn(p_information) {}

const AiObjectMobile &IPIMobile::object() const {
	return *information_->ai_object()->mobile();
}

void IPIMobile::set_time() {
	set_time(information_->context().now());
}

int IPIMobile::vehicle_number() const {
	return object().vehicle_number();
}

float IPIMobile::max_speed() const {
	return object().max_speed();
}

float IPIMobile::turn_radius() const {
	return object().turn_radius();
}

float IPIMobile::drag() const {
	return object().drag();
}

float IPIMobile::cover_search_radius() const {
	return object().cover_search_radius();
}

float IPIMobile::sound_sphere_radius_max() const {
	return object().sound_sphere_radius_max();
}

float IPIMobile::sound_sphere_radius_min() const {
	return object().sound_sphere_radius_min();
}

float IPIMobile::hearing_probability_max() const {
	return object().hearing_probability_max();
}

float IPIMobile::hearing_probability_min() const {
	return object().hearing_probability_min();
}

bool IPIMobile::is_turnable() const {
	return object().is_turnable();
}

float IPIMobile::avoid_collision_look_ahead() const {
	return object().avoid_collision_look_ahead();
}

float IPIMobile::sprint_stamina() const {
	return object().sprint_stamina();
}

const IPIMobile::ValidPos &IPIMobile::last_valid_position() const {
	return last_valid_position(vehicle_number());
}

const IPIMobile::ValidPos &IPIMobile::last_valid_position(int p_vehicle_number) const {
	if (information_->context().infantry_map == p_vehicle_number) {
		return last_valid_infantry_pos_;
	}
	return use_first_vehicle_valid_pos_ ? last_valid_vehicle_pos_a_ : last_valid_vehicle_pos_b_;
}

void IPIMobile::set_last_valid_infantry_pos_internal(const ValidPos &p_position) {
	last_valid_infantry_pos_ = p_position;
}

void IPIMobile::set_last_valid_vehicle_pos_internal(const ValidPos &p_position, uint8_t p_material) {
	switch (p_material) {
		case 3:
			last_valid_vehicle_pos_b_ = p_position;
			last_valid_vehicle_pos_a_ = p_position;
			use_first_vehicle_valid_pos_ = true;
			break;
		case 4:
			last_valid_vehicle_pos_b_ = p_position;
			use_first_vehicle_valid_pos_ = false;
			break;
		default:
			last_valid_vehicle_pos_a_ = p_position;
			use_first_vehicle_valid_pos_ = true;
			break;
	}
}

void IPIMobile::set_last_valid_vehicle_pos_internal(
		const ValidPos &p_first, const ValidPos &p_second, bool p_use_first) {
	last_valid_vehicle_pos_b_ = p_second;
	last_valid_vehicle_pos_a_ = p_first;
	use_first_vehicle_valid_pos_ = p_use_first;
}

void IPIMobile::set_last_valid_positions(
		const ValidPos &p_infantry, const ValidPos &p_first_vehicle, const ValidPos &p_second_vehicle) {
	last_valid_infantry_pos_ = p_infantry;
	last_valid_vehicle_pos_a_ = p_first_vehicle;
	last_valid_vehicle_pos_b_ = p_second_vehicle;
}

IPIMobileReal::IPIMobileReal(Information *p_information) : IPIMobile(p_information) {}

void IPIMobileReal::set_time(sim::Tick) {}

void IPIMobileReal::set_has_contact() {}

void IPIMobileReal::set_lost_contact() {}

const Vec3 &IPIMobileReal::speed() const {
	return object().speed();
}

const Vec3 &IPIMobileReal::rotational_speed() const {
	return object().rotational_speed();
}

float IPIMobileReal::speed_magnitude() const {
	const Vec3 &v = speed();
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float IPIMobileReal::sound_sphere_radius() const {
	return object().sound_sphere_radius();
}

void IPIMobileReal::position_estimate(Vec3 &r_position) const {
	r_position = information_->position();
}

void IPIMobileReal::position_estimate(sim::Tick p_time, Vec3 &r_position) const {
	r_position = information_->position();
	const KnowledgeContext &context = information_->context();
	const Vec3 &v = speed();
	const double difference = (double(p_time) - double(context.now())) * tick_seconds(context);
	const double scaled_x = double(v.x) * difference;
	const float scaled_y = float(double(v.y) * difference);
	const float scaled_z = float(double(v.z) * difference);
	r_position.x = float(scaled_x + double(r_position.x));
	r_position.y = float(double(scaled_y) + r_position.y);
	r_position.z = float(double(scaled_z) + r_position.z);
}

uint32_t IPIMobileReal::sector() const {
	const AiObject *const sector = object().sector();
	return sector ? sector->information(information_->side()) : kNoHandle;
}

int IPIMobileReal::last_passed_portal() const {
	return object().last_passed_portal();
}

void IPIMobileReal::set_last_valid_infantry_pos(const ValidPos &p_position) {
	set_last_valid_infantry_pos_internal(p_position);
}

void IPIMobileReal::set_last_valid_vehicle_pos(const ValidPos &p_position, uint8_t p_material) {
	set_last_valid_vehicle_pos_internal(p_position, p_material);
}

IPIMobileKnown::IPIMobileKnown(Information *p_information) : IPIMobile(p_information) {
	has_contact_ = true;
	speed_ = object().speed();
	has_contact_ = false;
	steal_last_valid();
	time_ = p_information->time_stamp();
}

void IPIMobileKnown::set_time(sim::Tick p_time) {
	time_ = p_time;
	const AiObject *const sector = object().sector();
	sector_ = sector ? sector->information(information_->side()) : kNoHandle;
	if (!has_contact_) {
		steal_last_valid();
	}
}

void IPIMobileKnown::set_has_contact() {
	if (!has_contact_) {
		steal_last_valid();
	}
	has_contact_ = true;
}

void IPIMobileKnown::set_lost_contact() {
	has_contact_ = false;
	speed_ = object().speed();
	IPIMobile::set_time();
}

const Vec3 &IPIMobileKnown::speed() const {
	return has_contact_ ? object().speed() : speed_;
}

const Vec3 &IPIMobileKnown::rotational_speed() const {
	return has_contact_ ? object().rotational_speed() : rotational_speed_;
}

float IPIMobileKnown::speed_magnitude() const {
	const Vec3 &v = speed();
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float IPIMobileKnown::sound_sphere_radius() const {
	return object().sound_sphere_radius();
}

void IPIMobileKnown::position_estimate(Vec3 &r_position) const {
	if (has_contact_) {
		r_position = information_->position();
	} else {
		position_estimate(information_->context().now(), r_position);
	}
}

void IPIMobileKnown::position_estimate(sim::Tick p_time, Vec3 &r_position) const {
	const float security = information_->security(p_time);
	const double wide_scale =
			double(security) * (double(time_) - double(p_time)) * tick_seconds(information_->context());
	const float rounded_scale = float(wide_scale);
	r_position.x = float(double(speed_.x) * wide_scale);
	r_position.y = speed_.y * rounded_scale;
	r_position.z = speed_.z * rounded_scale;
	const Vec3 &known = information_->position();
	r_position.x += known.x;
	r_position.y += known.y;
	r_position.z += known.z;
}

uint32_t IPIMobileKnown::sector() const {
	return sector_;
}

int IPIMobileKnown::last_passed_portal() const {
	return last_passed_portal_;
}

void IPIMobileKnown::set_last_valid_infantry_pos(const ValidPos &p_position) {
	if (has_contact_) {
		set_last_valid_infantry_pos_internal(p_position);
	}
}

void IPIMobileKnown::set_last_valid_vehicle_pos(const ValidPos &p_position, uint8_t p_material) {
	if (has_contact_) {
		set_last_valid_vehicle_pos_internal(p_position, p_material);
	}
}

void IPIMobileKnown::steal_last_valid() {
	const AiObject *const object = information_->ai_object();
	const InformationManager *const manager = information_->context().information;
	const Information *const real = manager ? manager->information(object->information(object->side())) : nullptr;
	const IPIMobile *const mobile = real ? real->mobile() : nullptr;
	if (mobile == nullptr || mobile == this) {
		return;
	}
	set_last_valid_infantry_pos_internal(mobile->last_valid_infantry_pos_);
	set_last_valid_vehicle_pos_internal(
			mobile->last_valid_vehicle_pos_a_, mobile->last_valid_vehicle_pos_b_, mobile->use_first_vehicle_valid_pos_);
}

}
