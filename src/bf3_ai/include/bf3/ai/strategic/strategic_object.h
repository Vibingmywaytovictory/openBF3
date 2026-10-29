#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace bf3::ai {

class AIInformationGrid;
class InformationManager;
class Sai;
class SquadManager;

enum class StrategicStatus : uint8_t { Owned = 0, Hostile = 1, Neutral = 2, Unknown = 3 };

inline constexpr uint32_t kNoControlPoint = 0xFFFFFFFFu;

class EnemyStatistics {
public:
	explicit EnemyStatistics(uint32_t p_size, float p_weight = 1.0f);

	void sample(const float *p_strength, const int *p_count);
	void copy(const EnemyStatistics &p_other);
	void add(const EnemyStatistics &p_other);
	void clear();

	uint32_t size() const { return uint32_t(strength_.size()); }
	float strength(uint32_t p_index) const { return strength_[p_index]; }
	float count(uint32_t p_index) const { return count_[p_index]; }
	int samples() const { return samples_; }

private:
	float weight_ = 1.0f;
	std::vector<float> strength_;
	std::vector<float> count_;
	int samples_ = 0;
};

class SampleHelper {
public:
	explicit SampleHelper(uint32_t p_size) : size_(p_size) {}
	void add_sample(const float *p_input, int p_index, float *r_strength, int *r_count) const;
	void clear(float *r_strength, int *r_count) const;

private:
	uint32_t size_ = 0;
};

class StrategicGame {
public:
	virtual ~StrategicGame() = default;
	virtual bool control_point_info(uint32_t p_id, Vec3 &r_position, float &r_radius, bool &r_active) const = 0;
	virtual int owner_of_control_point(uint32_t p_id) const = 0;
	virtual bool control_point_takeable(uint32_t p_id) const = 0;
	virtual float control_point_take_over_counter(uint32_t p_id) const = 0;
	virtual float terrain_altitude(float p_x, float p_z) const = 0;
	virtual int number_of_maps() const = 0;
	virtual bool polygon(
			const Vec3 &p_position, int p_map, uint32_t &r_polygon, uint32_t &r_island, Vec3 &r_on) const = 0;
};

struct StrategicServices {
	const StrategicGame *game = nullptr;
	const InformationManager *information = nullptr;
	const SquadManager *squads = nullptr;
	std::array<const AIInformationGrid *, 2> grids{};
};

class AIStrategicObject {
public:
	enum Type : int {
		Flank = 1,
		Centre = 2,
		Base = 3,
		Close = 4,
		Remote = 5,
		AirSpawner = 6,
		LandSpawner = 7,
		NavalSpawner = 8,
		SoldierSpawner = 9,
		StrongPoint = 10,
		ChokePoint = 11,
		Bridge = 12,
		AirField = 13,
		SupplyPoint = 14,
		Route = 15,
		ControlPoint = 16,
		North = 17,
		West = 18,
		South = 19,
		East = 20,
		Front = 22,
		Safe = 23,
		Enemy = 24,
		NeutralType = 26
	};

	AIStrategicObject(float p_radius, float p_height);
	virtual ~AIStrategicObject() = default;
	AIStrategicObject(const AIStrategicObject &) = delete;
	AIStrategicObject &operator=(const AIStrategicObject &) = delete;

	virtual int type() const { return 3; }
	virtual const Vec2 &position(int p_side) const { return position_[size_t(p_side - 1)]; }
	virtual void set_position(int p_side, const Vec2 &p_position) { position_[size_t(p_side - 1)] = p_position; }
	virtual Vec3 order_position(int p_map) const = 0;
	virtual void update(int p_side, const Sai &p_sai, bool &r_changed) = 0;
	virtual bool has_neighbour_with_status(StrategicStatus p_status, int p_side) const = 0;
	virtual bool has_neighbouring_object_with_status(StrategicStatus p_status, int p_side) const = 0;
	virtual int object_type_flags() const { return 0x8000000; }
	virtual bool has_object_type_flag(Type p_type) const;
	virtual float gravity(const Vec3 &p_position, int p_side) const;
	virtual float gravity(const Vec2 &p_position, int p_side) const;
	virtual float x_extent(int p_side) const { return size_[size_t(p_side)] + size_[size_t(p_side)]; }
	virtual float z_extent(int p_side) const { return size_[size_t(p_side)] + size_[size_t(p_side)]; }
	virtual void set_as_dirty(int p_side) { distances_dirty_[size_t(p_side - 1)] = true; }

	int number_of_assigned_units(int p_side) const { return int(assigned_units_[size_t(p_side - 1)].size()); }
	std::set<int> &assigned_units(int p_side) { return assigned_units_[size_t(p_side - 1)]; }
	bool is_attack(int p_side) const { return attack_[size_t(p_side - 1)]; }
	void set_attack(int p_side, bool p_attack) { attack_[size_t(p_side - 1)] = p_attack; }
	EnemyStatistics &battle_statistics(int p_side) const { return *statistics_[size_t(p_side - 1)]; }
	EnemyStatistics &strategic_statistics(int p_side) const { return *statistics_[size_t(p_side + 1)]; }
	StrategicStatus status(int p_side) const { return status_[size_t(p_side - 1)]; }
	void set_status(int p_side, StrategicStatus p_status) { status_[size_t(p_side - 1)] = p_status; }
	int index() const { return index_; }
	int category() const { return category_; }
	float height() const { return height_; }
	bool distances_dirty(int p_side) const { return distances_dirty_[size_t(p_side - 1)]; }
	float temperature(int p_side) const { return temperature_[size_t(p_side - 1)]; }
	void set_temperature(int p_side, float p_temperature) { temperature_[size_t(p_side - 1)] = p_temperature; }
	float friendly_ratio(int p_side) const { return friendly_ratio_[size_t(p_side - 1)]; }
	float friendly_battle_strength(int p_side) const { return friendly_battle_strength_[size_t(p_side - 1)]; }
	float enemy_battle_strength(int p_side) const { return enemy_battle_strength_[size_t(p_side - 1)]; }
	int resource_need(int p_side) const { return resource_need_[size_t(p_side - 1)]; }
	int assigned_squad_strength(int p_side) const { return assigned_squad_strength_[size_t(p_side - 1)]; }
	float security_sum(int p_side) const { return security_sum_[size_t(p_side - 1)]; }

protected:
	friend class AIStrategicObjectManager;

	std::array<std::unique_ptr<EnemyStatistics>, 4> statistics_;
	std::array<std::set<int>, 2> assigned_units_;
	std::array<StrategicStatus, 4> status_{ StrategicStatus::Neutral, StrategicStatus::Neutral,
		StrategicStatus::Neutral, StrategicStatus::Neutral };
	std::array<Vec2, 2> position_{};
	float height_ = -666.0f;
	std::array<float, 3> size_{};
	std::array<float, 4> security_sum_{};
	std::array<float, 2> need_curve_input_{};
	std::array<float, 2> friendly_ratio_{};
	std::array<float, 2> friendly_battle_strength_{};
	std::array<float, 2> enemy_battle_strength_{};
	std::array<int, 2> resource_need_{};
	std::array<int, 2> assigned_squad_strength_{};
	std::array<bool, 2> attack_{ true, true };
	std::array<bool, 2> distances_dirty_{ true, true };
	std::array<float, 2> temperature_{};
	int index_ = -1;
	int category_ = -1;
};

class AIStrategicArea final : public AIStrategicObject {
public:
	struct MapData {
		Vec3 offset{};
		uint32_t island = 0xFFFFFFFFu;
	};

	AIStrategicArea(
			const std::string &p_name, uint32_t p_control_point, float p_radius, const StrategicServices &p_services);

	void post_load_init();
	void init();
	void init_order_positions();
	bool control_point_position_and_radius(Vec3 &r_position, float &r_radius) const;
	float control_point_take_over_counter() const;
	bool is_area_takeable() const;

	int type() const override { return 0; }
	Vec3 order_position(int p_map) const override;
	void update(int p_side, const Sai &p_sai, bool &r_changed) override;
	bool has_neighbour_with_status(StrategicStatus p_status, int p_side) const override;
	bool has_neighbouring_object_with_status(StrategicStatus p_status, int p_side) const override;
	int object_type_flags() const override { return object_type_flags_; }
	bool has_object_type_flag(Type p_type) const override { return ((object_type_flags_ >> int(p_type)) & 1) != 0; }
	float gravity(const Vec3 &p_position, int p_side) const override;
	float gravity(const Vec2 &p_position, int p_side) const override;
	float gravity(float p_distance, int p_side) const;
	float x_extent(int p_side) const override;
	float z_extent(int p_side) const override;

	const std::string &name() const { return name_; }
	uint32_t control_point() const { return control_point_; }
	const Vec2 &centre() const { return centre_; }
	const std::vector<MapData> &map_data() const { return map_data_; }
	const std::vector<AIStrategicObject *> &neighbours() const { return neighbours_; }
	const std::vector<AIStrategicObject *> &neighbouring_objects() const { return neighbouring_objects_; }
	void add_neighbour(AIStrategicObject *p_object) { neighbours_.push_back(p_object); }
	void add_neighbouring_object(AIStrategicObject *p_object) { neighbouring_objects_.push_back(p_object); }
	void set_object_type_flags(int p_flags) { object_type_flags_ = p_flags; }
	void set_from_control_point(bool p_from) { from_control_point_ = p_from; }
	void set_area(const Vec2 &p_top_left, const Vec2 &p_centre, float p_height);
	bool from_control_point() const { return from_control_point_; }
	bool order_positions_initialised() const { return order_positions_initialised_; }
	bool status_changed() const { return status_changed_; }
	bool enemy_takeover(int p_side) const { return enemy_takeover_[size_t(p_side - 1)]; }

private:
	void internal_constructor_call(const Vec2 &p_centre);

	StrategicServices services_;
	Vec2 centre_{};
	std::vector<MapData> map_data_;
	std::vector<AIStrategicObject *> neighbours_;
	std::vector<AIStrategicObject *> neighbouring_objects_;
	std::string name_;
	int object_type_flags_ = 0;
	uint32_t control_point_ = kNoControlPoint;
	std::array<bool, 2> side_enabled_{ true, true };
	bool status_changed_ = false;
	std::array<bool, 2> enemy_takeover_{};
	bool from_control_point_ = true;
	bool order_positions_initialised_ = false;
};

class SAWayPoint {
public:
	SAWayPoint(const Vec3 &p_position, int p_map) : position_(p_position), map_(p_map) {}
	void init(const StrategicGame &p_game);

	const Vec3 &position() const { return position_; }
	uint32_t polygon() const { return polygon_; }
	uint32_t island() const { return island_; }
	int map() const { return map_; }

private:
	Vec3 position_{};
	uint32_t island_ = 0xFFFFFFFFu;
	uint32_t polygon_ = 0xFFFFFFFFu;
	int map_ = 0;
};

class AIStrategicObjectManager {
public:
	explicit AIStrategicObjectManager(const StrategicServices &p_services) : services_(p_services) {}

	const std::vector<AIStrategicObject *> &strategic_objects() const { return objects_; }
	int number_of_strategic_objects() const { return int(objects_.size()); }
	AIStrategicObject *strategic_object(int p_index) const;
	AIStrategicArea *strategic_area(const std::string &p_name) const;
	const std::map<std::string, std::unique_ptr<AIStrategicArea>> &strategic_areas() const { return areas_; }
	AIStrategicArea *active_area() const { return active_; }
	AIStrategicObject *closest_object(const Vec3 &p_position, int p_side) const;

	void create_new_category(const std::string &p_name);
	int number_of_categories() const { return int(category_names_.size()); }
	int category_from_name(const std::string &p_name) const;
	const std::vector<AIStrategicObject *> &strategic_objects_of_category(int p_category) const;

	bool create_from_control_point(
			const std::string &p_name, uint32_t p_control_point, float p_radius, const std::string &p_category);
	bool create(const std::string &p_name, const Vec2 &p_top_left, const Vec2 &p_centre, float p_height, float p_radius,
			const std::string &p_category);
	bool add_two_way_neighbour(const std::string &p_a, const std::string &p_b);
	void add_way_point(const Vec3 &p_position, int p_map, const std::string &p_from, const std::string &p_to);
	const std::vector<std::unique_ptr<SAWayPoint>> *way_points(int p_from, int p_to) const;

	void init_areas();
	void post_load_init_areas();
	bool areas_initialised() const { return areas_initialised_; }

private:
	void add_strategic_object(AIStrategicObject *p_object);

	StrategicServices services_;
	std::vector<AIStrategicObject *> objects_;
	std::vector<std::vector<AIStrategicObject *>> objects_by_category_;
	std::map<std::string, std::unique_ptr<AIStrategicArea>> areas_;
	AIStrategicArea *active_ = nullptr;
	std::map<std::string, int> categories_;
	std::vector<std::string> category_names_;
	std::map<std::pair<int, int>, std::vector<std::unique_ptr<SAWayPoint>>> way_points_;
	bool areas_initialised_ = false;
};

}
