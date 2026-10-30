#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/sim/sim_types.h"

#include <cstddef>
#include <cstdint>

namespace bf3::ai {

enum class InfantryActivity : uint8_t { Idle, Roam, Hunt, Fight, Retreat, RegenWait, Revive, Grenade };
inline constexpr size_t kInfantryActivityCount = 8;
const char *infantry_activity_name(InfantryActivity p_activity);

struct InfantrySenses {
	bool enemy_visible = false;
	bool blind_fire = false;
	bool shot_at = false;
	bool low_health = false;
	bool revive_target = false;
	bool grenade_running = false;
	bool hunt_target = false;
	bool hunt_near = false;
	bool navigable = false;

	bool operator==(const InfantrySenses &) const = default;
};

enum class MoveGoal : uint8_t { Roam, Hunt, Cover, Squad };
enum class ObjectGoal : uint8_t { Target, Corpse };
enum class PoseRequest : uint8_t { Crouch, NearCorpse };
enum class TriggerKind : uint8_t { Aimed, Blind, Grenade, Defib };
enum class SenseFocus : uint8_t { Target, BlindPoint, Threat, ShotOrigin, Around };

class InfantryBody {
public:
	virtual ~InfantryBody() = default;

	virtual sim::Tick tick() const = 0;
	virtual Vec3 position() const = 0;
	virtual void see() = 0;
	virtual void update_sensing() = 0;
	virtual const InfantrySenses &senses() const = 0;

	virtual bool can_throw_grenade() const = 0;
	virtual bool throw_grenade() = 0;
	virtual bool take_cover() = 0;
	virtual void wait_in_cover() = 0;
	virtual bool cover_recovered(bool p_waiting) const = 0;
	virtual bool cover_cleared() const = 0;
	virtual bool cover_running() const = 0;

	virtual void begin_plan(InfantryActivity p_activity) = 0;
	virtual bool move_to(MoveGoal p_goal, const Vec3 &p_point) = 0;
	virtual bool move_to_object(ObjectGoal p_object) = 0;
	virtual bool soldier_pose(PoseRequest p_pose) = 0;
	virtual bool trigger(TriggerKind p_trigger) = 0;
	virtual bool sense(SenseFocus p_focus) = 0;
	virtual void end_plan() = 0;
};

}
