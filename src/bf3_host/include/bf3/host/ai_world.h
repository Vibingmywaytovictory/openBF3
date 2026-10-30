#pragma once

#include "bf3/ai/ai_world.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"
#include "bf3/sim/bots/perception.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::host {

class ServerView;
class TdmRules;

struct AiWorldConfig {
	std::shared_ptr<const BotLevel> level;
	sim::bots::PerceptionParams perception;
	float eye_heights[3] = { 1.6f, 1.0f, 0.3f };
	float gravity = 9.81f;
	uint32_t grid_dimension = 32;
	uint64_t seed = 1;
	std::shared_ptr<const sim::StaticWorldShapes> static_world;
};

float area_heat(std::span<const float> p_heat, int p_cells, const sim::Vec3 &p_origin, float p_cell_m,
		const sim::Vec3 &p_centre, float p_radius);

class HostAiWorld final : public ai::AiTimer,
						  public ai::AiPhysics,
						  public ai::AiTerrain,
						  public ai::AiObjectEnvironment,
						  public ai::AiCollisionHandler,
						  public ai::AiGraphCreator,
						  public ai::StrategicGame {
public:
	static core::Result<std::unique_ptr<HostAiWorld>> create(const AiWorldConfig &p_config);
	~HostAiWorld() override;
	HostAiWorld(const HostAiWorld &) = delete;
	HostAiWorld &operator=(const HostAiWorld &) = delete;

	ai::AiWorld world();

	void add_bot(uint8_t p_slot);
	void step(const ServerView &p_view, std::span<const sim::bots::HeardShot> p_shots,
			std::span<const uint8_t> p_firing, sim::Tick p_tick);
	void set_area_temperature(size_t p_area, float p_temperature);

	const ai::InformationManager &information() const;
	const ai::AIInformationGrid &grid(int p_side) const;
	uint32_t information_of(int p_side, uint8_t p_slot) const;
	const sim::bots::Perception *perception(uint8_t p_slot) const;
	std::span<const uint8_t> bots() const;
	ai::AIStrategicObjectManager &strategic();
	const ai::AIStrategicObjectManager &strategic() const;
	ai::AIStrategicArea *area(size_t p_index) const;
	uint32_t perception_ticks() const;

	sim::Tick game_tick() const override;
	double tick_seconds() const override;
	double update_time() const override;
	float gravity() const override;
	float altitude(const ai::Vec3 &p_position) const override;
	float height(float p_x, float p_z) const override;
	ai::Vec2 world_min() const override;
	ai::Vec2 world_max() const override;
	bool inside_combat_area(const ai::Vec3 &p_position) const override;
	void world_player_ids(std::vector<int> &r_out) const override;
	ai::AiObject *player_object(int p_player_id) const override;
	bool collide_line_with_world(const ai::Vec3 &p_from, const ai::Vec3 &p_to, ai::Vec3 *r_hit) const override;
	std::span<const ai::AiShot> recent_shots() const override;
	const ai::NavClusterMap *create_graph() const override;
	bool control_point_info(uint32_t p_id, ai::Vec3 &r_position, float &r_radius, bool &r_active) const override;
	int owner_of_control_point(uint32_t p_id) const override;
	bool control_point_takeable(uint32_t p_id) const override;
	float control_point_take_over_counter(uint32_t p_id) const override;
	float terrain_altitude(float p_x, float p_z) const override;
	int number_of_maps() const override;
	bool polygon(const ai::Vec3 &p_position, int p_map, uint32_t &r_polygon, uint32_t &r_island,
			ai::Vec3 &r_on) const override;
	int number_of_control_points(int p_side) const;

	struct Impl;

private:
	explicit HostAiWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

class RulesSquadService final : public ai::SquadService {
public:
	explicit RulesSquadService(TdmRules &r_rules) : rules_(r_rules) {}

	bool join_squad(int p_squad, const ai::SquadPlayer &p_player) override;
	bool exit_squad(int p_squad, const ai::SquadPlayer &p_player) override;
	bool is_squad_leader(int p_squad, const ai::SquadPlayer &p_player) const override;
	void request_new_commander_order(int p_squad, const ai::SquadPlayer &p_player) override;

private:
	TdmRules &rules_;
};

}
