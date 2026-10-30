#pragma once

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/squad/squad_coordinator.h"
#include "bf3/ai/squad/squad_world.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategic_object.h"
#include "bf3/core/util/key_values.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::ai {

class SquadManager;

inline constexpr std::string_view kStrategyHunt = "Hunt";
inline constexpr std::string_view kStrategySpread = "Spread";
inline constexpr std::string_view kNoStrategies = "No Strategies";

struct StrategyTuning {
	uint32_t sai_period_ticks = 30;
	uint32_t squad_updates_per_tick = 2;
	float hunt_heat = 2.0f;
	float hunt_keep = 0.5f;
	float squad_spawn_draw = 0.0f;
	std::array<float, kInfantryBehaviourCount> order{ { 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 0.4f, 1.0f } };

	bool operator==(const StrategyTuning &) const = default;
};

inline constexpr std::string_view kStrategySettingsPrefix = "sai.";
bool is_strategy_key(std::string_view p_key);
core::KeyOutcome parse_strategy_setting(std::string_view p_key, std::string_view p_value, StrategyTuning &r_tuning);
std::string format_strategy_tuning(const StrategyTuning &p_tuning);

class Strategy {
public:
	explicit Strategy(std::string p_name) : name_(std::move(p_name)) {}
	const std::string &name() const { return name_; }

private:
	std::string name_;
};

class StrategyManager {
public:
	StrategyManager();
	virtual ~StrategyManager();
	StrategyManager(const StrategyManager &) = delete;
	StrategyManager &operator=(const StrategyManager &) = delete;

	virtual void reset();

	Strategy *active_strategy(int p_side) const;
	std::string active_strategy_name(int p_side) const;
	Strategy *forced_strategy(int p_side) const;
	Strategy *strategy(const std::string &p_name) const;
	void create_strategy(const std::string &p_name);
	void set_active_strategy(int p_side, const std::string &p_name);
	void force_strategy(const std::string &p_name, int p_side);
	uint64_t changes(int p_side) const { return changes_[size_t(p_side - 1)]; }

private:
	std::array<std::string, 2> active_name_;
	std::array<Strategy *, 2> active_{};
	std::map<std::string, std::unique_ptr<Strategy>> strategies_;
	std::array<Strategy *, 2> forced_{};
	std::array<uint64_t, 2> changes_{};
};

class StrategyHeat {
public:
	virtual ~StrategyHeat() = default;
	virtual float area_heat(const AIStrategicArea &p_area) const = 0;
};

class StrategicSquadArea final : public SquadArea {
public:
	explicit StrategicSquadArea(const AIStrategicArea &p_area) : area_(&p_area) {}

	int index() const override { return area_->index(); }
	int status(int p_side) const override { return int(area_->status(p_side)); }
	bool is_takeable() const override { return area_->is_area_takeable(); }
	bool control_point(Vec3 &r_position, float &r_radius) const override {
		return area_->control_point_position_and_radius(r_position, r_radius);
	}
	const AIStrategicArea &area() const { return *area_; }

private:
	const AIStrategicArea *area_;
};

struct StrategyStats {
	uint64_t missions = 0;
	uint64_t hunt_missions = 0;
	uint64_t spread_missions = 0;
	uint64_t attack_missions = 0;
	uint64_t defend_missions = 0;
	uint64_t released = 0;
};

class StrategyDecisions final : public SaiDecisions {
public:
	StrategyDecisions(StrategyManager &r_manager, SquadManager &r_squads, AIStrategicObjectManager &r_objects,
			const StrategyHeat *p_heat, const StrategyTuning &p_tuning);

	void choose_strategy(Sai &r_sai) override;
	void modify_strategic_objects_temperatures(Sai &r_sai) override;
	void update_bot_positions(Sai &r_sai) override;
	void update_distribution(Sai &r_sai) override;
	void distribute_resources(Sai &r_sai) override;
	void distribute_defence(Sai &r_sai) override;
	void distribute_attack(Sai &r_sai) override;
	void distribute_with_no_areas(Sai &r_sai) override;
	int release_squads_from_object(Sai &r_sai, int p_object, int p_side) override;

	size_t area_count() const { return areas_.size(); }
	const StrategicSquadArea &area(size_t p_index) const { return areas_[p_index]; }
	int squad_area(int p_side, int p_squad) const;
	int hunted_area(int p_side) const { return hunted_[size_t(p_side - 1)]; }
	const StrategyStats &stats() const { return stats_; }

private:
	bool free_for_mission(const SquadCoordinator &p_squad) const;
	bool led_by_bot(const SquadCoordinator &p_squad) const;
	bool squad_position(const SquadCoordinator &p_squad, Vec3 &r_position) const;
	int nearest_area(const Vec3 &p_position, const std::vector<uint8_t> *p_skip) const;
	int least_visited_area(int p_side, const Vec3 &p_position, const std::vector<uint8_t> &p_skip) const;
	void send(SquadCoordinator &r_squad, size_t p_area, SquadOrder p_order);
	float heat(int p_side, size_t p_area) const;

	StrategyManager &manager_;
	SquadManager &squads_;
	const StrategyHeat *heat_;
	StrategyTuning tuning_;
	std::vector<StrategicSquadArea> areas_;
	std::vector<AIStrategicArea *> objects_;
	std::array<std::vector<int>, 2> squad_areas_;
	std::array<std::vector<uint8_t>, 2> assigned_;
	std::array<std::vector<uint32_t>, 2> visited_;
	std::array<uint32_t, 2> visit_cycle_{};
	std::array<int, 2> hunted_{ -1, -1 };
	StrategyStats stats_;
};

}
