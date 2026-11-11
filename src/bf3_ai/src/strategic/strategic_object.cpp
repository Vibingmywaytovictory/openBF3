#include "bf3/ai/strategic/strategic_object.h"

#include "bf3/ai/main/ai_random.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

EnemyStatistics::EnemyStatistics(uint32_t p_size, float p_weight) :
		weight_(p_weight), strength_(p_size, 0.0f), count_(p_size, 0.0f) {}

void EnemyStatistics::sample(const float *p_strength, const int *p_count) {
	const float samples = float(samples_);
	const float total = samples + weight_;
	for (size_t i = 0; i < strength_.size(); ++i) {
		strength_[i] = samples / total * strength_[i];
		strength_[i] = weight_ / total * p_strength[i] + strength_[i];
		count_[i] = samples / total * count_[i];
		count_[i] = weight_ / total * float(p_count[i]) + count_[i];
	}
	++samples_;
}

void EnemyStatistics::copy(const EnemyStatistics &p_other) {
	for (size_t i = 0; i < strength_.size() && i < p_other.strength_.size(); ++i) {
		strength_[i] = p_other.strength_[i];
		count_[i] = p_other.count_[i];
	}
	samples_ = p_other.samples_;
}

void EnemyStatistics::add(const EnemyStatistics &p_other) {
	for (size_t i = 0; i < strength_.size() && i < p_other.strength_.size(); ++i) {
		strength_[i] += p_other.strength_[i];
		count_[i] += p_other.count_[i];
	}
	samples_ += p_other.samples_;
}

void EnemyStatistics::clear() {
	std::fill(strength_.begin(), strength_.end(), 0.0f);
	std::fill(count_.begin(), count_.end(), 0.0f);
	samples_ = 0;
}

void SampleHelper::add_sample(const float *p_input, int p_index, float *r_strength, int *r_count) const {
	for (uint32_t i = 0; i < size_; ++i) {
		r_strength[i] = p_input[i] + r_strength[i];
	}
	++r_count[p_index];
}

void SampleHelper::clear(float *r_strength, int *r_count) const {
	for (uint32_t i = 0; i < size_; ++i) {
		r_strength[i] = 0.0f;
		r_count[i] = 0;
	}
}

AIStrategicObject::AIStrategicObject(float p_radius, float p_height) : height_(p_height) {
	statistics_[0] = std::make_unique<EnemyStatistics>(6);
	statistics_[1] = std::make_unique<EnemyStatistics>(6);
	statistics_[2] = std::make_unique<EnemyStatistics>(2);
	statistics_[3] = std::make_unique<EnemyStatistics>(2);
	size_ = { p_radius, 0.0f, 0.0f };
	security_sum_ = { p_radius, p_radius, p_radius, p_radius };
}

bool AIStrategicObject::register_available_attacker(int p_unit, int p_side) {
	available_attackers_[size_t(p_side - 1)].insert(p_unit);
	return true;
}

bool AIStrategicObject::register_available_defender(int p_unit, int p_side) {
	available_defenders_[size_t(p_side - 1)].insert(p_unit);
	return true;
}

bool AIStrategicObject::unregister_available_attacker(std::set<int>::iterator &r_it, int p_side) {
	std::set<int> &units = available_attackers_[size_t(p_side - 1)];
	if (r_it == units.end()) {
		return false;
	}
	r_it = units.erase(r_it);
	return true;
}

bool AIStrategicObject::unregister_available_defender(std::set<int>::iterator &r_it, int p_side) {
	std::set<int> &units = available_defenders_[size_t(p_side - 1)];
	if (r_it == units.end()) {
		return false;
	}
	r_it = units.erase(r_it);
	return true;
}

bool AIStrategicObject::unregister_available_attacker(int p_unit, int p_side) {
	std::set<int>::iterator it = available_attackers_[size_t(p_side - 1)].find(p_unit);
	return unregister_available_attacker(it, p_side);
}

bool AIStrategicObject::unregister_available_defender(int p_unit, int p_side) {
	std::set<int>::iterator it = available_defenders_[size_t(p_side - 1)].find(p_unit);
	return unregister_available_defender(it, p_side);
}

int AIStrategicObject::unregister_all_available_attackers(int p_side) {
	const int count = int(available_attackers_[size_t(p_side - 1)].size());
	available_attackers_[size_t(p_side - 1)].clear();
	return count;
}

int AIStrategicObject::unregister_all_available_defenders(int p_side) {
	const int count = int(available_defenders_[size_t(p_side - 1)].size());
	available_defenders_[size_t(p_side - 1)].clear();
	return count;
}

int AIStrategicObject::number_of_available_bots(bool p_attackers, int p_side) const {
	return int((p_attackers ? available_attackers_ : available_defenders_)[size_t(p_side - 1)].size());
}

bool AIStrategicObject::is_present_unit(int p_unit, int p_side) const {
	return present_units_[size_t(p_side - 1)].contains(p_unit);
}

bool AIStrategicObject::set_as_present_unit(int p_unit, int p_side) {
	return present_units_[size_t(p_side - 1)].insert(p_unit).second;
}

void AIStrategicObject::remove_as_present_unit(std::set<int>::iterator &r_it, int p_side) {
	r_it = present_units_[size_t(p_side - 1)].erase(r_it);
}

bool AIStrategicObject::remove_as_present_unit(int p_unit, int p_side) {
	return present_units_[size_t(p_side - 1)].erase(p_unit) != 0;
}

void AIStrategicObject::set_side(int p_side) {
	if (p_side == 1) {
		status_ = { StrategicStatus::Owned, StrategicStatus::Hostile, StrategicStatus::Owned,
			StrategicStatus::Hostile };
	} else if (p_side == 2) {
		status_ = { StrategicStatus::Hostile, StrategicStatus::Owned, StrategicStatus::Hostile,
			StrategicStatus::Owned };
	} else {
		status_[2] = StrategicStatus::Neutral;
		status_[3] = StrategicStatus::Neutral;
	}
}

int AIStrategicObject::side() const {
	if (status_[2] == StrategicStatus::Owned) {
		return 1;
	}
	return status_[3] == StrategicStatus::Owned ? 2 : 0;
}

void AIStrategicObject::reset_status() {
	status_[0] = status_[2];
	status_[1] = status_[3];
}

void AIStrategicObject::add_allowed_vehicle_group(int p_group) {
	if (allowed_vehicle_groups_ == -1) {
		allowed_vehicle_groups_ = 0;
	}
	allowed_vehicle_groups_ |= 1 << p_group;
}

float AIStrategicObject::assigned_part_of_wanted_strength(int p_side) const {
	const int need = resource_need_[size_t(p_side - 1)];
	if (need == 0) {
		return 1.0f;
	}
	return float(assigned_squad_strength_[size_t(p_side - 1)] / need);
}

void AIStrategicObject::randomize_pos(int p_side, float p_scale, AIRandom &r_random, Vec3 &r_position) const {
	const float draw_x = r_random.next_float();
	const float dx = x_extent(p_side) * draw_x * p_scale - x_extent(p_side) * 0.5f;
	const float draw_z = r_random.next_float();
	const float dz = draw_z * z_extent(p_side) * p_scale - z_extent(p_side) * 0.5f;
	r_position = { dx + position_[size_t(p_side - 1)].x, height_, dz + position_[size_t(p_side - 1)].y };
}

void AIStrategicObject::randomize_controlpoint_pos(
		float p_scale, float p_spread, AIRandom &r_random, Vec3 &r_position) const {
	const double turn = double(r_random.next()) * double(2.32830644e-10f);
	const double angle = (turn + turn) * double(3.14159274f);
	const double draw = double(r_random.next()) * double(2.32830644e-10f);
	const float distance = float((0.5 + draw * (double(p_spread) - 0.5)) * p_scale);
	const double s = std::sin(float(angle));
	const double c = std::cos(float(angle));
	r_position.x = float(s * distance + r_position.x);
	r_position.z = float(c * distance + r_position.z);
}

void SAWayPoint::init(const StrategicGame &p_game) {
	Vec3 on{};
	p_game.polygon(position_, map_, polygon_, island_, on);
}

AIStrategicObject *AIStrategicObjectManager::strategic_object(int p_index) const {
	if (p_index < 0 || p_index >= int(objects_.size())) {
		return nullptr;
	}
	return objects_[size_t(p_index)];
}

AIStrategicArea *AIStrategicObjectManager::strategic_area(const std::string &p_name) const {
	const auto found = areas_.find(p_name);
	return found == areas_.end() ? nullptr : found->second.get();
}

AIStrategicObject *AIStrategicObjectManager::closest_object(const Vec3 &p_position, int p_side) const {
	AIStrategicObject *closest = nullptr;
	float best = -1.0f;
	for (AIStrategicObject *object : objects_) {
		const float gravity = object->gravity(p_position, p_side);
		if (gravity > best) {
			best = gravity;
			closest = object;
		}
	}
	return closest;
}

void AIStrategicObjectManager::create_new_category(const std::string &p_name) {
	if (categories_.contains(p_name)) {
		return;
	}
	categories_[p_name] = int(category_names_.size());
	category_names_.push_back(p_name);
	objects_by_category_.emplace_back(objects_.size(), nullptr);
}

int AIStrategicObjectManager::category_from_name(const std::string &p_name) const {
	const auto found = categories_.find(p_name);
	return found == categories_.end() ? -1 : found->second;
}

const std::vector<AIStrategicObject *> &AIStrategicObjectManager::strategic_objects_of_category(int p_category) const {
	return objects_by_category_[size_t(p_category)];
}

void AIStrategicObjectManager::add_strategic_object(AIStrategicObject *p_object) {
	const int index = int(objects_.size());
	objects_.push_back(p_object);
	p_object->index_ = index;
	for (int category = 0; category < int(objects_by_category_.size()); ++category) {
		objects_by_category_[size_t(category)].push_back(category == p_object->category_ ? p_object : nullptr);
	}
}

bool AIStrategicObjectManager::create_from_control_point(
		const std::string &p_name, uint32_t p_control_point, float p_radius, const std::string &p_category) {
	if (areas_.contains(p_name)) {
		active_ = nullptr;
		return false;
	}
	auto area = std::make_unique<AIStrategicArea>(p_name, p_control_point, p_radius, services_);
	area->category_ = number_of_categories() <= 0 ? 0 : category_from_name(p_category);
	AIStrategicArea *raw = area.get();
	areas_[p_name] = std::move(area);
	add_strategic_object(raw);
	active_ = raw;
	return true;
}

bool AIStrategicObjectManager::create(const std::string &p_name, const Vec2 &p_top_left, const Vec2 &p_centre,
		float p_height, float p_radius, const std::string &p_category) {
	if (!create_from_control_point(p_name, kNoControlPoint, p_radius, p_category)) {
		return false;
	}
	active_->set_from_control_point(false);
	active_->set_area(p_top_left, p_centre, p_height);
	return true;
}

bool AIStrategicObjectManager::add_two_way_neighbour(const std::string &p_a, const std::string &p_b) {
	AIStrategicArea *a = strategic_area(p_a);
	AIStrategicArea *b = strategic_area(p_b);
	if (a == nullptr || b == nullptr || a == b) {
		return false;
	}
	a->add_neighbour(b);
	b->add_neighbour(a);
	return true;
}

void AIStrategicObjectManager::add_way_point(
		const Vec3 &p_position, int p_map, const std::string &p_from, const std::string &p_to) {
	const AIStrategicArea *from = strategic_area(p_from);
	const AIStrategicArea *to = strategic_area(p_to);
	if (from == nullptr || to == nullptr) {
		return;
	}
	const int low = std::min(from->index(), to->index());
	const int high = std::max(from->index(), to->index());
	way_points_[{ low, high }].push_back(std::make_unique<SAWayPoint>(p_position, p_map));
}

const std::vector<std::unique_ptr<SAWayPoint>> *AIStrategicObjectManager::way_points(int p_from, int p_to) const {
	const auto found = way_points_.find({ std::min(p_from, p_to), std::max(p_from, p_to) });
	return found == way_points_.end() ? nullptr : &found->second;
}

void AIStrategicObjectManager::init_areas() {
	for (auto &[name, area] : areas_) {
		if (!area->from_control_point()) {
			area->init();
		}
	}
}

void AIStrategicObjectManager::post_load_init_areas() {
	for (auto &[name, area] : areas_) {
		area->post_load_init();
		for (const AIStrategicObject *neighbour : area->neighbours()) {
			const int low = std::min(area->index(), neighbour->index());
			const int high = std::max(area->index(), neighbour->index());
			way_points_.try_emplace({ low, high });
		}
	}
	if (services_.game != nullptr) {
		for (auto &[pair, points] : way_points_) {
			for (const std::unique_ptr<SAWayPoint> &point : points) {
				point->init(*services_.game);
			}
		}
	}
	areas_initialised_ = true;
}

}
