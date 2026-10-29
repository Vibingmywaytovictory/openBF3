#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/nav/nav_clusters.h"
#include "bf3/ai/squad/squad_world.h"
#include "bf3/ai/strategic/bot_spawner.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategic_object.h"
#include "bf3/ai/vehicle/aerial_height_map.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::ai {

class AiPhysics {
public:
	virtual ~AiPhysics() = default;
	virtual float gravity() const = 0;
};

class AiTerrain : public AltitudeSource, public AerialHeightSource {
public:
	virtual Vec2 world_min() const = 0;
	virtual Vec2 world_max() const = 0;
	virtual bool inside_combat_area(const Vec3 &p_position) const = 0;
};

class AiObjectEnvironment {
public:
	virtual ~AiObjectEnvironment() = default;
	virtual void world_player_ids(std::vector<int> &r_out) const = 0;
	virtual AiObject *player_object(int p_player_id) const = 0;
	virtual bool collide_line_with_world(const Vec3 &p_from, const Vec3 &p_to, Vec3 *r_hit) const = 0;
};

struct AiShot {
	int player_id = -1;
	Vec3 origin{};
	sim::Tick tick = 0;
};

class AiCollisionHandler {
public:
	virtual ~AiCollisionHandler() = default;
	virtual std::span<const AiShot> recent_shots() const = 0;
};

class AiGraphCreator {
public:
	virtual ~AiGraphCreator() = default;
	virtual const NavClusterMap *create_graph() const = 0;
};

struct AiWorld {
	AiGame *game = nullptr;
	const AiTimer *timer = nullptr;
	const AiPhysics *physics = nullptr;
	const AiTerrain *terrain = nullptr;
	AiObjectEnvironment *objects = nullptr;
	const AiCollisionHandler *collisions = nullptr;
	const AiGraphCreator *graphs = nullptr;
	const StrategicGame *strategic = nullptr;
	const SaiGame *sai = nullptr;
	SquadWorld *squads = nullptr;
	SpawnGame *spawns = nullptr;
};

}
