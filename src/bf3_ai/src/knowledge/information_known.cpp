#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"

#include <cmath>

namespace bf3::ai {

InformationKnown::InformationKnown(AiObject *p_object, uint32_t p_types, sim::Tick p_time, float p_degeneration,
		float p_basic_temperature, sim::Tick p_allowed_time_diff, int p_owner_side, const KnowledgeContext &p_context) :
		Information(p_object, 0, p_context), last_update_(p_time), degeneration_(p_degeneration),
		owner_side_(p_owner_side) {
	allowed_time_diff_ = p_allowed_time_diff;
	strength_ = p_basic_temperature;
	basic_temperature_ = p_basic_temperature;
	object_type_flags_ = p_types;
	side_ = object_->side();
	store_last_seen();
}

void InformationKnown::store_last_seen() {
	last_position_ = object_->position();
	last_transformation_ = object_->transformation();
}

template <class F> void InformationKnown::for_each_secondary(F &&p_visit) {
	if (!is_primary() || context_.information == nullptr) {
		return;
	}
	int iterator = -1;
	const int own_side = side();
	Information *secondary = context_.information->information(first_secondary_object(iterator, own_side));
	while (secondary != nullptr) {
		p_visit(*secondary);
		secondary = context_.information->information(secondary_object(iterator, own_side));
	}
}

int InformationKnown::information_sort() const {
	return 1;
}

bool InformationKnown::set_time() {
	return set_time(context_.now());
}

bool InformationKnown::set_time(sim::Tick p_time) {
	const double tick_seconds = context_.tick_seconds();
	const int64_t never = tick_seconds > 0.0 ? -int64_t(std::llround(1.0 / tick_seconds)) : -1;
	const int64_t stamp = stamped_ ? int64_t(time_) : never;
	if (!(int64_t(p_time) > stamp + int64_t(allowed_time_diff_))) {
		last_update_ = p_time;
		return false;
	}
	if (contacts_ == 0) {
		store_last_seen();
		if (mobile() != nullptr) {
			mobile()->set_lost_contact();
		}
	}
	if (mobile() != nullptr) {
		mobile()->set_time(p_time);
	}
	for_each_secondary([p_time](Information &r_secondary) { r_secondary.set_time(p_time); });
	side_ = object_->side();
	if (unit() != nullptr && physical() != nullptr) {
		physical()->set_time(p_time);
		unit()->calculate_strength_mod(physical()->health());
	}
	set_time_on_plug_ins(p_time);
	time_ = p_time;
	stamped_ = true;
	if (is_primary() && context_.environment != nullptr) {
		context_.environment->add_and_update_position(
				owner_side_, unit() != nullptr, object_->information(owner_side_));
	}
	last_update_ = p_time;
	return true;
}

bool InformationKnown::set_time(sim::Tick p_time, float p_security) {
	float target = 0.0f;
	if (!(0.0f > p_security)) {
		target = p_security > 1.0f ? 1.0f : p_security;
	}
	if (!(security() < target)) {
		return false;
	}
	set_time(p_time);
	if (target == 0.0f) {
		last_update_ = 0;
		return true;
	}
	const double tick_seconds = context_.tick_seconds();
	const double back_seconds = 1.0 / (double(target) * degeneration_);
	const int64_t back_ticks = tick_seconds > 0.0 ? int64_t(std::llround(back_seconds / tick_seconds)) : 0;
	const int64_t backdated = int64_t(p_time) - back_ticks;
	stamped_ = false;
	return set_time(sim::Tick(backdated > 0 ? backdated : 0));
}

sim::Tick InformationKnown::time_stamp() const {
	return last_update_;
}

void InformationKnown::add_contact() {
	++contacts_;
	if (mobile() != nullptr) {
		mobile()->set_has_contact();
	}
	for_each_secondary([](Information &r_secondary) { r_secondary.add_contact(); });
}

void InformationKnown::remove_contact() {
	if (contacts_ > 0 && --contacts_ == 0) {
		store_last_seen();
		if (mobile() != nullptr) {
			mobile()->set_lost_contact();
		}
	}
	for_each_secondary([](Information &r_secondary) { r_secondary.remove_contact(); });
}

void InformationKnown::remove_all_contacts() {
	if (contacts_ > 0) {
		contacts_ = 0;
		store_last_seen();
		if (mobile() != nullptr) {
			mobile()->set_lost_contact();
		}
	}
	for_each_secondary([](Information &r_secondary) { r_secondary.remove_all_contacts(); });
}

bool InformationKnown::has_contact() const {
	return contacts_ > 0;
}

float InformationKnown::security(sim::Tick p_time) const {
	const float age = float((double(p_time) - double(last_update_)) * context_.tick_seconds());
	return 1.0f - SCurve::calculate(age / degeneration_);
}

float InformationKnown::security() const {
	if (contacts_ > 0) {
		return 1.0f;
	}
	return security(context_.now());
}

const Vec3 &InformationKnown::position() const {
	return contacts_ != 0 ? object_->position() : last_position_;
}

float InformationKnown::altitude() const {
	return context_.altitude ? context_.altitude->altitude(position()) : 0.0f;
}

float InformationKnown::altitude(const Vec3 &p_direction) const {
	return context_.altitude ? lowest_altitude_along(*context_.altitude, position(), p_direction) : 0.0f;
}

const Transform &InformationKnown::transformation() const {
	return contacts_ != 0 ? object_->transformation() : last_transformation_;
}

Vec3 InformationKnown::facing() const {
	return last_transformation_.forward;
}

int InformationKnown::side() const {
	return side_;
}

}
