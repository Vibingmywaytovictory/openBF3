#pragma once

#include "bf3/ai/ai_world.h"
#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/communication_hub.h"
#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/squads.h"
#include "bf3/sim/bots/perception.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
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
	uint8_t squad_size = kDefaultSquadSize;
	uint64_t seed = 1;
	std::shared_ptr<const sim::StaticWorldShapes> static_world;
	const sim::CollisionWorld *world = nullptr;
};

float area_heat(std::span<const float> p_heat, int p_cells, const sim::Vec3 &p_origin, float p_cell_m,
		const sim::Vec3 &p_centre, float p_radius);

class HostAiWorld final : public ai::AiTimer,
						  public ai::AiPhysics,
						  public ai::AiTerrain,
						  public ai::AiObjectEnvironment,
						  public ai::AiCollisionHandler,
						  public ai::AiGraphCreator,
						  public ai::StrategicGame,
						  public ai::SquadWorld {
public:
	static core::Result<std::unique_ptr<HostAiWorld>> create(const AiWorldConfig &p_config);
	~HostAiWorld() override;
	HostAiWorld(const HostAiWorld &) = delete;
	HostAiWorld &operator=(const HostAiWorld &) = delete;

	ai::AiWorld world();

	void add_bot(uint8_t p_slot, bool p_perceive = true);
	void step(const ServerView &p_view, std::span<const sim::bots::HeardShot> p_shots,
			std::span<const uint8_t> p_firing, sim::Tick p_tick);
	void set_area_temperature(size_t p_area, float p_temperature);
	void set_control_point(size_t p_area, int p_owner, float p_take_over);

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
	int max_number_of_squads() const override;
	int max_number_of_squad_members() const override;
	ai::SquadPlayer *player(int p_player_id) const override;
	std::vector<ai::SquadPlayer *> all_players() const override;
	ai::SquadBot *bot_from_player_id(int p_player_id) const override;
	bool is_valid_position(const ai::Vec3 &p_position) const override;
	void set_squad_bot(uint8_t p_slot, ai::SquadBot *p_bot);

	struct Impl;

private:
	explicit HostAiWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct BotSeatState {
	const sim::SoldierState *state = nullptr;
	bool alive = false;
};

class BotSeat {
public:
	virtual ~BotSeat() = default;
	virtual BotSeatState prepare(sim::Tick p_tick, bool p_playing) = 0;
	virtual void acted(sim::Tick p_tick) = 0;
};

class HostAiBots final : public ai::AiTimer, public ai::AiGame, public ai::BotFactory, public ai::RadioComm {
public:
	using MedicCall = std::function<std::optional<uint8_t>(uint8_t p_caller)>;

	HostAiBots(const ServerView &r_view, float p_view_distance, float p_call_seconds, MedicCall p_medic_call,
			const ai::AiSettings &p_settings = {});
	~HostAiBots() override;
	HostAiBots(const HostAiBots &) = delete;
	HostAiBots &operator=(const HostAiBots &) = delete;

	ai::BotMain *admit(
			uint8_t p_slot, ai::InfantryBody &r_body, const ai::BehaviourTuning &p_tuning, BotSeat *p_seat = nullptr);
	void remove(uint8_t p_slot);
	void observe(uint8_t p_slot, const sim::SoldierState &p_state, bool p_alive, sim::Tick p_tick);
	void step(sim::Tick p_tick);
	const ai::AIMain &main() const;

	sim::Tick game_tick() const override;
	double tick_seconds() const override;
	double update_time() const override;

	bool is_playing() const override;
	int max_number_of_players() const override;
	bool is_valid_player_id(int p_player_id) const override;
	int player_side(int p_player_id) const override;
	bool player_is_ai(int p_player_id) const override;
	bool add_bot(int &r_player_id, int &r_side, std::string &r_name) override;
	void terminate_player(int p_player_id) override;
	void give_damage(ai::AiObject *p_target, int p_from_player_id, int p_side, float p_damage) override;
	float recommended_view_distance() const override;
	void respawn_bot(int p_player_id) override;
	void set_bot_name_unused(const std::string &p_name) override;
	ai::AiObject *player_vehicle(int p_player_id) const override;
	ai::AiObject *player_default_vehicle(int p_player_id) const override;
	void occupied_objects(std::vector<std::pair<int, ai::AiObject *>> &r_out) const override;
	uint32_t force_information(ai::AiObject &r_object, int p_side, bool p_known) override;
	bool player_camera(int p_player_id, ai::Transform &r_camera) const override;
	void enemy_spotted_by_ai(int p_spotter, const ai::AiObject &p_spotted) override;
	int player_score(int p_player_id) const override;
	void set_player_side(int p_player_id, int p_side) override;

	std::unique_ptr<ai::Bot> create_bot(
			int p_player_id, uint32_t p_bot_id, const float *p_personality, int p_side) override;

	void send_radio_message(int p_sender, int p_side, const ai::RadioMessage &p_message) override;

	struct Impl;

private:
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
