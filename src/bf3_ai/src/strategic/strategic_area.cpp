#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/squad/squad_manager.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategic_object.h"
#include "bf3/ai/strategic/strategy_model.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace bf3::ai {

AIStrategicArea::AIStrategicArea(
		const std::string &p_name, uint32_t p_control_point, float p_radius, const StrategicServices &p_services) :
		AIStrategicObject(p_radius, -666.0f), services_(p_services), name_(p_name), control_point_(p_control_point) {
	const int maps = services_.game != nullptr ? std::max(services_.game->number_of_maps(), 1) : 1;
	map_data_.resize(size_t(maps));
	for (std::vector<GroupDistances> &side : groups_) {
		side.resize(size_t(vehicle_group_count()));
	}
}

void AIStrategicArea::internal_constructor_call(const Vec2 &p_centre) {
	position_[0] = p_centre;
	position_[1] = p_centre;
	side_enabled_ = { true, true };
	distances_dirty_ = { true, true };
	const float dx0 = centre_.x - position_[0].x;
	const float dy0 = centre_.y - position_[0].y;
	size_[1] = std::sqrt(dx0 * dx0 + dy0 * dy0);
	const float dx1 = centre_.x - position_[1].x;
	const float dy1 = centre_.y - position_[1].y;
	size_[2] = std::sqrt(dx1 * dx1 + dy1 * dy1);
	enemy_takeover_ = { false, false };
}

void AIStrategicArea::set_area(const Vec2 &p_top_left, const Vec2 &p_centre, float p_height) {
	centre_ = p_top_left;
	height_ = p_height;
	internal_constructor_call(p_centre);
	for (MapData &data : map_data_) {
		data.offset = { p_centre.x, p_height, p_centre.y };
	}
}

bool AIStrategicArea::control_point_position_and_radius(Vec3 &r_position, float &r_radius) const {
	if (control_point_ == kNoControlPoint || services_.game == nullptr) {
		return false;
	}
	bool active = true;
	return services_.game->control_point_info(control_point_, r_position, r_radius, active);
}

void AIStrategicArea::post_load_init() {
	if (from_control_point_ && services_.game != nullptr) {
		Vec3 point{};
		float radius = 0.0f;
		bool active = false;
		services_.game->control_point_info(control_point_, point, radius, active);
		const Vec2 flat{ point.x, point.z };
		centre_ = { flat.x - radius, flat.y - radius };
		height_ = point.y;
		internal_constructor_call(flat);
		for (MapData &data : map_data_) {
			data.offset = point;
		}
		init();
	}
	init_order_positions();
}

void AIStrategicArea::init() {
	if (services_.game == nullptr) {
		return;
	}
	const float altitude = services_.game->terrain_altitude(position_[0].x, position_[0].y);
	if (altitude > height_) {
		height_ = altitude;
	}
}

void AIStrategicArea::init_order_positions() {
	const Vec3 area{ position_[0].x, height_, position_[0].y };
	for (size_t map = 0; map < map_data_.size(); ++map) {
		MapData &data = map_data_[map];
		uint32_t polygon = 0xFFFFFFFFu;
		uint32_t island = 0xFFFFFFFFu;
		Vec3 on = data.offset;
		if (services_.game != nullptr && services_.game->polygon(data.offset, int(map), polygon, island, on)) {
			data.offset = on;
		}
		data.offset = { data.offset.x - area.x, data.offset.y - area.y, data.offset.z - area.z };
		data.island = island;
	}
	order_positions_initialised_ = true;
}

Vec3 AIStrategicArea::order_position(int p_map) const {
	Vec3 position{ position_[0].x, height_, position_[0].y };
	if (p_map >= 0 && size_t(p_map) < map_data_.size()) {
		const MapData &data = map_data_[size_t(p_map)];
		position = { position.x + data.offset.x, position.y + data.offset.y, position.z + data.offset.z };
	}
	return position;
}

float AIStrategicArea::x_extent(int p_side) const {
	const float half = position_[size_t(p_side - 1)].x - centre_.x;
	return half + half;
}

float AIStrategicArea::z_extent(int p_side) const {
	const float half = position_[size_t(p_side - 1)].y - centre_.y;
	return half + half;
}

float AIStrategicArea::update_temperature(const Strategy &p_strategy, int p_side) {
	const size_t first = size_t(p_side - 1);
	const size_t second = size_t(p_side + 1);
	const std::array<float, kStrategicObjectTypes> &modifiers = p_strategy.strategic_objects_modifiers(status(p_side));
	const auto scale = [&](float p_factor) {
		security_sum_[first] *= p_factor;
		security_sum_[second] *= p_factor;
	};
	for (int flag = 0; flag <= int(East); ++flag) {
		if (has_object_type_flag(Type(flag))) {
			scale(modifiers[size_t(flag)]);
		}
	}
	switch (status(p_side)) {
		case StrategicStatus::Owned:
			scale(modifiers[has_neighbour_with_status(StrategicStatus::Hostile, p_side) ? Front : Safe]);
			break;
		case StrategicStatus::Hostile:
			scale(modifiers[Enemy]);
			break;
		case StrategicStatus::Neutral:
			scale(modifiers[NeutralType]);
			break;
		case StrategicStatus::Unknown:
			break;
	}
	scale(p_strategy.specific_strategic_object_modifier(index_));
	return security_sum_[first];
}

int AIStrategicArea::changed_attack_defence(int p_side) const {
	return status_changed_ && status(p_side) != StrategicStatus::Owned ? 1 : 0;
}

float AIStrategicArea::calculate_defense_need(int p_side) const {
	if (status(p_side) != StrategicStatus::Owned) {
		return 0.0f;
	}
	const size_t s = size_t(p_side - 1);
	float need = (2.0f - SCurve::calculate(need_curve_input_[s])) * (enemy_battle_strength_[s] + security_sum_[s]);
	if (has_neighbour_with_status(StrategicStatus::Hostile, p_side)) {
		need *= 1.5f;
	} else if (has_neighbour_with_status(StrategicStatus::Neutral, p_side)) {
		need *= 1.25f;
	}
	return need;
}

float AIStrategicArea::calculate_attack_value(int p_side) const {
	const size_t s = size_t(p_side - 1);
	float value = 0.0f;
	if (status(p_side) != StrategicStatus::Owned && has_neighbour_with_status(StrategicStatus::Owned, p_side)) {
		value = (1.0f + need_curve_input_[s]) * security_sum_[s];
		if (!has_neighbour_with_status(StrategicStatus::Hostile, p_side)) {
			value *= has_neighbour_with_status(StrategicStatus::Neutral, p_side) ? 1.5f : 1.25f;
		}
		const float x = position_[0].x;
		const float z = position_[0].y;
		const Vec3 at{ x, services_.game != nullptr ? services_.game->terrain_altitude(x, z) : height_, z };
		const float distance =
				services_.squads != nullptr ? services_.squads->distance_to_closest_squad(at, p_side) : FLT_MAX;
		value = distance > 1.0f && 100000.0f > distance ? 1.0f / distance * 100.0f * value : 0.0f;
	}
	return value - calculate_attack_cost(p_side);
}

float AIStrategicArea::calculate_attack_cost(int p_side) const {
	const size_t s = size_t(p_side - 1);
	const EnemyStatistics &statistics = *statistics_[s];
	const float strength = statistics.size() > 1 ? statistics.strength(1) : -1.0f;
	return strength * (1.5f - 0.5f * need_curve_input_[s]) * (1.0f + friendly_ratio_[s]);
}

bool AIStrategicArea::is_neighbour(const AIStrategicObject *p_object) const {
	return std::find(neighbours_.begin(), neighbours_.end(), p_object) != neighbours_.end();
}

bool AIStrategicArea::is_neighbouring_object(const AIStrategicObject *p_object) const {
	return std::find(neighbouring_objects_.begin(), neighbouring_objects_.end(), p_object) !=
			neighbouring_objects_.end();
}

int AIStrategicArea::tactical_status(int p_side) const {
	return p_side == 1 ? tactical_status_[0] : (p_side == 2 ? tactical_status_[1] : int(control_point_));
}

void AIStrategicArea::set_as_dirty(int p_side) {
	distances_dirty_[size_t(p_side - 1)] = true;
	for (GroupDistances &group : groups_[size_t(p_side - 1)]) {
		group.dirty = true;
	}
}

void AIStrategicArea::validate_distances(int p_side) {
	if (!distances_dirty_[size_t(p_side - 1)]) {
		return;
	}
	distances_[size_t(p_side - 1)].clear();
	for (int group = 0; group < vehicle_group_count(); ++group) {
		validate_distances(p_side, group);
		insert_unit_distances(p_side, group);
	}
}

void AIStrategicArea::validate_distances(int p_side, int p_group) {
	GroupDistances &group = groups_[size_t(p_side - 1)][size_t(p_group)];
	if (!group.dirty) {
		return;
	}
	group.dirty = false;
	group.sorted.clear();
	group.routes.clear();
	group.costs.clear();
	find_distances(p_side, group, 1 << p_group);
}

void AIStrategicArea::find_distances(int p_side, GroupDistances &r_group, int p_group_mask) const {
	struct Search {
		const AIStrategicObject *object;
		const AIStrategicObject *previous;
		float cost;
	};
	if (!allows_some_vehicle_groups(p_group_mask)) {
		return;
	}
	std::list<Search> open{ { this, this, 0.0f } };
	r_group.routes.emplace(this, std::list<const AIStrategicObject *>{});
	while (!open.empty()) {
		const Search at = open.front();
		open.pop_front();
		const auto found = r_group.costs.find(at.object);
		if ((found != r_group.costs.end() && !(found->second > at.cost)) ||
				!at.object->allows_some_vehicle_groups(p_group_mask)) {
			continue;
		}
		if (at.object->type() == 0) {
			for (const AIStrategicObject *next : static_cast<const AIStrategicArea *>(at.object)->neighbours_) {
				const Vec2 &from = at.object->position(p_side);
				const Vec2 &to = next->position(p_side);
				const float dx = to.x - from.x;
				const float dy = to.y - from.y;
				float distance = std::sqrt(dx * dx + dy * dy);
				if (next->status(p_side) == StrategicStatus::Neutral) {
					distance *= 1.1f;
				} else if (next->status(p_side) == StrategicStatus::Hostile) {
					distance *= 5.0f;
				}
				open.push_back({ next, at.object, at.cost + distance });
			}
		}
		r_group.costs[at.object] = at.cost;
		std::list<const AIStrategicObject *> route = r_group.routes[at.previous];
		route.push_front(at.object);
		r_group.routes[at.object] = std::move(route);
	}
	for (const auto &[object, cost] : r_group.costs) {
		r_group.sorted.emplace_back(cost, object);
	}
	r_group.sorted.sort();
}

void AIStrategicArea::insert_unit_distances(int p_side, int p_group) {
	std::list<AreaDistanceInfo> &merged = distances_[size_t(p_side - 1)];
	const std::list<std::pair<float, const AIStrategicObject *>> &sorted =
			groups_[size_t(p_side - 1)][size_t(p_group)].sorted;
	auto at = merged.begin();
	auto it = sorted.begin();
	for (; it != sorted.end(); ++it) {
		while (at != merged.end() && it->first > at->distance) {
			++at;
		}
		if (at == merged.end()) {
			break;
		}
		if (at->distance == it->first) {
			at->vehicle_groups |= 1 << p_group;
		} else {
			merged.insert(at, AreaDistanceInfo{ 1 << p_group, it->second, it->first });
		}
	}
	for (; it != sorted.end(); ++it) {
		merged.push_back(AreaDistanceInfo{ 1 << p_group, it->second, it->first });
	}
}

const std::list<const AIStrategicObject *> *AIStrategicArea::route(
		int p_side, int p_group, const AIStrategicObject *p_to) const {
	if (p_group < 0 || p_group >= vehicle_group_count()) {
		return nullptr;
	}
	const GroupDistances &group = groups_[size_t(p_side - 1)][size_t(p_group)];
	const auto found = group.routes.find(p_to);
	return found != group.routes.end() ? &found->second : nullptr;
}

bool AIStrategicArea::is_inside(const Vec2 &p_point) const {
	const Vec2 &centre = position_[0];
	const Vec2 maximum{ centre.x - centre_.x + centre.x, centre.y - centre_.y + centre.y };
	return centre_.x <= p_point.x && p_point.x <= maximum.x && centre_.y <= p_point.y && p_point.y <= maximum.y;
}

float AIStrategicArea::control_point_take_over_counter() const {
	if (control_point_ != kNoControlPoint && services_.game != nullptr) {
		return services_.game->control_point_take_over_counter(control_point_);
	}
	return 1.0f;
}

bool AIStrategicArea::is_area_takeable() const {
	return control_point_ != kNoControlPoint && services_.game != nullptr &&
			services_.game->control_point_takeable(control_point_);
}

void AIStrategicArea::update(int p_side, const Sai &p_sai, bool &r_changed) {
	const size_t s = size_t(p_side - 1);
	assigned_squad_strength_[s] =
			services_.squads != nullptr ? services_.squads->squad_strengths(p_side, index_, attack_[s] ? 0 : 1) : 0;
	const Vec2 upper{ (position_[s].x - centre_.x) + position_[s].x, (position_[s].y - centre_.y) + position_[s].y };
	float total = 0.0f;
	float without_units = 0.0f;
	float friendly = 0.0f;
	float enemy = 0.0f;
	float ratio = 0.0f;
	int friendly_units = 0;
	int enemy_units = 0;
	const SampleHelper battle_samples(6);
	const SampleHelper strategic_samples(2);
	float battle[6];
	float strategic[2];
	int battle_counts[6];
	int strategic_counts[2];
	battle_samples.clear(battle, battle_counts);
	strategic_samples.clear(strategic, strategic_counts);
	resource_need_[s] = 0;
	std::vector<uint32_t> handles;
	const AIInformationGrid *grid = services_.grids[s];
	if (grid != nullptr) {
		grid->information_within_rectangle(handles, centre_, upper, nullptr);
	}
	const auto sample = [&](const Information &p_information, bool p_root) {
		const float security = p_information.security();
		const float weighted = security * p_information.strength();
		total += weighted;
		const IPIUnit *unit = p_information.unit();
		if (unit == nullptr) {
			without_units += weighted;
			return;
		}
		const float *values = unit->battle_strengths();
		const float strength = values[0] + values[1] + values[2] + values[3] + values[4] + values[5];
		if (!unit->is_occupied()) {
			return;
		}
		const bool counted = !p_root || (p_information.object_type_flags() & 0x10u) == 0;
		if (p_sai.is_friend(p_information.side())) {
			friendly_units += counted ? 1 : 0;
			friendly = security * strength + friendly;
		} else if (p_sai.is_enemy(p_information.side())) {
			enemy_units += counted ? 1 : 0;
			enemy = security * strength + enemy;
			const IPIPhysical *physical = p_information.physical();
			const int type = physical != nullptr ? std::clamp(physical->strength_type(), 0, 5) : 0;
			battle_samples.add_sample(values, type, battle, battle_counts);
			const int owned = status_[s] != StrategicStatus::Owned ? 1 : 0;
			strategic_samples.add_sample(unit->strategic_strengths(), owned, strategic, strategic_counts);
			resource_need_[s] += int(unit->strategic_strengths()[status_[s] == StrategicStatus::Owned ? 1 : 0]);
		}
	};
	if (services_.information != nullptr) {
		for (const uint32_t handle : handles) {
			const Information *root = services_.information->information(handle);
			if (root == nullptr) {
				continue;
			}
			sample(*root, true);
			int secondary = -1;
			uint32_t next = root->first_secondary_object(secondary, p_side);
			while (const Information *current = services_.information->information(next)) {
				sample(*current, false);
				next = root->secondary_object(secondary, p_side);
			}
		}
	}
	if (friendly + enemy > 0.0f) {
		ratio = friendly / (friendly + enemy);
	}
	const StrategicStatus old_status = status_[s];
	security_sum_[s] = total + size_[0];
	security_sum_[s + 2] = without_units + size_[0];
	friendly_ratio_[s] = ratio;
	enemy_battle_strength_[s] = enemy;
	friendly_battle_strength_[s] = friendly;
	if (control_point_ != kNoControlPoint && services_.game != nullptr) {
		const int owner = services_.game->owner_of_control_point(control_point_);
		status_[s] = owner == p_side ? StrategicStatus::Owned
				: owner == 0		 ? StrategicStatus::Neutral
									 : StrategicStatus::Hostile;
	} else if (friendly_units > 0 || enemy_units > 0) {
		if (side_enabled_[s] && friendly_units > 0 && enemy_units == 0) {
			status_[s] = StrategicStatus::Owned;
		} else if (side_enabled_[p_side == 1 ? 1 : 0] && enemy_units > 0 && friendly_units == 0) {
			status_[s] = StrategicStatus::Hostile;
		}
	}
	if (control_point_ != kNoControlPoint) {
		if (old_status == StrategicStatus::Owned && status_[s] == StrategicStatus::Neutral) {
			r_changed = true;
		} else {
			enemy_takeover_[s] = false;
			if (status_[s] == StrategicStatus::Owned && control_point_take_over_counter() < 0.9f) {
				enemy_takeover_[s] = true;
				status_[s] = StrategicStatus::Neutral;
			}
			r_changed = old_status == StrategicStatus::Owned ? status_[s] != StrategicStatus::Owned
															 : status_[s] == StrategicStatus::Owned;
		}
	} else {
		r_changed = (old_status == StrategicStatus::Owned && status_[s] == StrategicStatus::Hostile) ||
				(old_status == StrategicStatus::Hostile && status_[s] == StrategicStatus::Owned);
	}
	status_changed_ = r_changed;
	statistics_[s]->sample(battle, battle_counts);
	statistics_[s + 2]->sample(strategic, strategic_counts);
	const float count = float(resource_need_[s]);
	const float floored = count > 1.0f ? count : 1.0f;
	const bool owned = status_[s] == StrategicStatus::Owned;
	const float input = owned ? need_curve_input_[s] : friendly_ratio_[s];
	double need = (2.0 - double(SCurve::calculate(input))) * floored;
	if (!owned) {
		need *= 1.25;
	}
	resource_need_[s] = int(need);
}

bool AIStrategicArea::has_neighbour_with_status(StrategicStatus p_status, int p_side) const {
	for (const AIStrategicObject *neighbour : neighbours_) {
		if (neighbour->status(p_side) == p_status) {
			return true;
		}
	}
	return false;
}

bool AIStrategicArea::has_neighbouring_object_with_status(StrategicStatus p_status, int p_side) const {
	for (const AIStrategicObject *neighbour : neighbouring_objects_) {
		if (neighbour->status(p_side) == p_status) {
			return true;
		}
	}
	return false;
}

float AIStrategicArea::gravity(const Vec3 &p_position, int p_side) const {
	return gravity(Vec2{ p_position.x, p_position.z }, p_side);
}

float AIStrategicArea::gravity(const Vec2 &p_position, int p_side) const {
	const double dx = double(p_position.x) - position_[size_t(p_side - 1)].x;
	const double dz = double(p_position.y) - position_[size_t(p_side - 1)].y;
	return gravity(float(std::sqrt(dx * dx + dz * dz)), p_side);
}

float AIStrategicArea::gravity(float p_distance, int p_side) const {
	if (p_distance <= 1.0f) {
		return size_[size_t(p_side)];
	}
	const double wide = p_distance;
	return float(size_[size_t(p_side)] / (wide * wide));
}

}
