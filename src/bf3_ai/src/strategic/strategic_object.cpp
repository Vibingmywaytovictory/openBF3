#include "bf3/ai/strategic/strategic_object.h"

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

bool AIStrategicObject::has_object_type_flag(Type p_type) const {
	(void)p_type;
	return false;
}

float AIStrategicObject::gravity(const Vec3 &p_position, int p_side) const {
	(void)p_position, (void)p_side;
	return 0.0f;
}

float AIStrategicObject::gravity(const Vec2 &p_position, int p_side) const {
	(void)p_position, (void)p_side;
	return 0.0f;
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
