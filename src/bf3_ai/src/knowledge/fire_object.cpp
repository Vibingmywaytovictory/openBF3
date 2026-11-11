#include "bf3/ai/knowledge/fire_object.h"

#include "bf3/ai/knowledge/information_factory.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

FireObject::FireObject(const Vec3 &p_origin, const Vec3 &p_target,
		std::span<const float, kBattleStrengthCount> p_strengths, bool p_direct, sim::Tick p_time, float p_decay) :
		direct_(p_direct), origin_(p_origin), target_(p_target), time_(p_time), decay_(p_decay) {
	std::copy(p_strengths.begin(), p_strengths.end(), strengths_.begin());
}

bool FireObject::is_within_radius(const Vec3 &p_position, float p_radius) const {
	const Vec3 direction{ target_.x - origin_.x, target_.y - origin_.y, target_.z - origin_.z };
	Vec2 normal{ direction.z, -direction.x };
	const float length_sq = normal.x * normal.x + normal.y * normal.y;
	if (std::fabs(1.0f - length_sq) >= 1.1920929e-07f) {
		if (std::fabs(length_sq) >= 1.4210855e-14f) {
			const float inverse = 1.0f / std::sqrt(length_sq);
			normal.x = normal.x * inverse;
			normal.y = inverse * normal.y;
		} else {
			normal = Vec2{};
		}
	}
	const Vec2 offset{ p_position.x - origin_.x, p_position.z - origin_.z };
	return std::fabs(offset.x * normal.x + offset.y * normal.y) <= p_radius;
}

float FireObject::battle_strength(int p_type, const AiTimer &p_timer) const {
	const float age = float(double(p_timer.game_tick() - time_) * p_timer.tick_seconds());
	return strengths_[size_t(p_type)] / (age * decay_ + 1.0f);
}

bool DangerObject::operator==(const DangerObject &p_other) const {
	if (is_information() && !p_other.is_information()) {
		return false;
	}
	if (is_fire_object() && !p_other.is_fire_object()) {
		return false;
	}
	const void *const own = pointer();
	return own != nullptr && own == p_other.pointer();
}

bool FireDanger::position(Vec3 &r_position) const {
	if (fire_ == nullptr) {
		return false;
	}
	r_position = fire_->origin();
	return true;
}

bool InfoDanger::position(Vec3 &r_position) const {
	const Information *const information = information_->information(handle_);
	if (information == nullptr) {
		return false;
	}
	r_position = information->position();
	return true;
}

const void *InfoDanger::pointer() const {
	if (handle_ == kNoHandle) {
		return nullptr;
	}
	return information_->information(handle_);
}

}
