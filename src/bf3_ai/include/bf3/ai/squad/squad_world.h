#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace bf3::ai {

class InformationManager;
class SquadMember;

inline float distance_sq(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x;
	const float dy = p_a.y - p_b.y;
	const float dz = p_a.z - p_b.z;
	return dx * dx + dy * dy + dz * dz;
}
inline float distance(const Vec3 &p_a, const Vec3 &p_b) {
	return std::sqrt(distance_sq(p_a, p_b));
}

class SquadPlayer {
public:
	virtual ~SquadPlayer() = default;
	virtual int id() const = 0;
	virtual bool is_ai() const = 0;
	virtual bool is_alive() const = 0;
	virtual bool is_man_down() const = 0;
	virtual int side() const = 0;
	virtual int spawn_group() const = 0;
	virtual int spawn_kit() const = 0;
	virtual AiObject *soldier() const = 0;
	virtual AiObject *current_vehicle() const = 0;
};

enum class SquadGoalKind : uint8_t { None, Slot, SquadTarget };

struct SquadGoal {
	SquadGoalKind kind = SquadGoalKind::None;
	Vec3 position{};
	float max_speed = 0.0f;
	bool wait_for_members = false;
};

class SquadBot {
public:
	virtual ~SquadBot() = default;
	virtual void set_squad_member(SquadMember *p_member) = 0;
	virtual bool join_squad(int p_squad) = 0;
	virtual void set_squad_goal(const SquadGoal &p_goal) = 0;
};

class SquadService {
public:
	virtual ~SquadService() = default;
	virtual bool join_squad(int p_squad, const SquadPlayer &p_player) = 0;
	virtual bool exit_squad(int p_squad, const SquadPlayer &p_player) = 0;
	virtual bool is_squad_leader(int p_squad, const SquadPlayer &p_player) const = 0;
	virtual void request_new_commander_order(int p_squad, const SquadPlayer &p_player) = 0;
};

class SquadArea {
public:
	virtual ~SquadArea() = default;
	virtual int index() const = 0;
	virtual int status(int p_side) const = 0;
	virtual bool is_takeable() const = 0;
	virtual bool control_point(Vec3 &r_position, float &r_radius) const = 0;
};

class SquadStrategy {
public:
	virtual ~SquadStrategy() = default;
	virtual bool is_player_commander(int p_side) const = 0;
	virtual bool own_all_takeable_control_points(int p_side) const = 0;
	virtual bool have_any_owned_and_takeable_control_points(int p_side) const = 0;
};

class SquadWorld {
public:
	virtual ~SquadWorld() = default;
	virtual int max_number_of_squads() const = 0;
	virtual int max_number_of_squad_members() const = 0;
	virtual sim::Tick game_tick() const = 0;
	virtual SquadPlayer *player(int p_player_id) const = 0;
	virtual std::vector<SquadPlayer *> all_players() const = 0;
	virtual SquadBot *bot_from_player_id(int p_player_id) const = 0;
	virtual bool is_valid_position(const Vec3 &p_position) const = 0;
};

struct FormationTuning {
	float line_spacing_m = 1.0f;
	float moving_speed_mps = 0.5f;
	bool trail_while_moving = true;
	float max_wait_s = 10.0f;
};

struct SquadServices {
	SquadWorld *world = nullptr;
	SquadService *squads = nullptr;
	const SquadStrategy *strategy = nullptr;
	const InformationManager *information = nullptr;
	FormationTuning formation;
};

}
