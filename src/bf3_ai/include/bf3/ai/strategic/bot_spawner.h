#pragma once

#include "bf3/ai/squad/squad_world.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace bf3::ai {

class AIInformationGrid;
class AIRandom;
class InformationManager;
class SquadManager;

inline constexpr int kKitTypeCount = 13;
inline constexpr int kKitNone = 13;
inline constexpr int kKitEngineer = 4;
inline constexpr int kKitMedic = 5;
inline constexpr int kKitAntiTank = 6;

class SpawnObjective {
public:
	virtual ~SpawnObjective() = default;
	virtual int index() const = 0;
	virtual Vec2 position(int p_side) const = 0;
	virtual float strategic_value(int p_side) const = 0;
	virtual Vec2 centre() const = 0;
};

class SpawnBot {
public:
	virtual ~SpawnBot() = default;
	virtual uint32_t bot_id() const = 0;
	virtual int side() const = 0;
	virtual const SquadPlayer &player() const = 0;
	virtual int spawn_point() const = 0;
	virtual const SquadMember *squad_member() const = 0;
	virtual int spawn_to_strategic_area_id() const = 0;
	virtual void set_spawn_target(int p_group, int p_kit) = 0;
	virtual void set_spawn_target(int p_area, int p_group, int p_kit) = 0;
};

class SpawnGame {
public:
	virtual ~SpawnGame() = default;
	virtual const std::vector<int> &spawn_point_list(int p_side) const = 0;
	virtual Vec3 spawn_point_position(int p_group) const = 0;
	virtual bool is_eligible_spawnpoint(int p_side, int p_group) const = 0;
	virtual int control_point_id(int p_group) const = 0;
	virtual const std::vector<int> &kit_templates() const = 0;
	virtual bool is_valid_position(const Vec3 &p_position) const = 0;
	virtual const SpawnObjective *strategic_object(int p_id) const = 0;
	virtual const SpawnObjective *closest_object(const Vec3 &p_position, int p_side) const = 0;
	virtual void add_spawn_to_point(int p_side, int p_group) = 0;
	virtual const std::map<int, std::pair<int, int>> &dead_bots(int p_side) const = 0;
	virtual const SpawnBot *bot_from_id(uint32_t p_id) const = 0;
	virtual const AIInformationGrid *information_grid(int p_side) const = 0;
};

struct SpawnServices {
	SpawnGame *game = nullptr;
	const SquadManager *squads = nullptr;
	AIRandom *random = nullptr;
	const InformationManager *information = nullptr;
};

struct SpawnChoice {
	int group = 0;
	float strategic_value = 0.0f;
	int information_count = 0;
	float free_strength = 0.0f;
	const SpawnObjective *object = nullptr;
	float distance = 0.0f;
	float weight = 0.0f;
};

class BotSpawner {
public:
	explicit BotSpawner(const SpawnServices &p_services) : services_(p_services) {}

	bool find_suitable_spawn_group(int p_strategic_object_id, SpawnBot &r_bot);
	int find_suitable_spawn_group(int p_side, const SpawnObjective *&r_object, SpawnBot &r_bot);
	int find_suitable_kit(const SpawnObjective *p_object, int p_side, SpawnBot &r_bot);
	int random_kit();

	const std::vector<SpawnChoice> &point_weights() const { return point_weights_; }

private:
	double draw();

	SpawnServices services_;
	std::vector<SpawnChoice> point_weights_;
};

}
