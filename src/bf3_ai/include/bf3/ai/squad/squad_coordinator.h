#pragma once

#include "bf3/ai/squad/squad_formation.h"
#include "bf3/ai/squad/squad_member.h"
#include "bf3/ai/squad/squad_world.h"

#include <list>
#include <memory>
#include <vector>

namespace bf3::ai {

enum class SquadOrder : int { Attack = 0, Defend = 1, Move = 2, None = 4 };

enum class SquadMissionStatus : int { None = 0, Issued = 1, Accepted = 2 };

inline constexpr sim::Tick kSquadFireRecordTicks = sim::seconds_to_ticks(10.0f);
inline constexpr sim::Tick kSquadRepairRequestTicks = sim::seconds_to_ticks(60.0f);
inline constexpr sim::Tick kSquadNeedOrdersTicks = sim::seconds_to_ticks(20.0f);

class SquadCoordinator {
public:
	struct SquadMemberFire {
		sim::Tick expires = 0;
		int player_id = -1;
		Vec3 position{};
	};

	SquadCoordinator(const SquadServices &p_services, int p_side, int p_squad_index);
	SquadCoordinator(const SquadCoordinator &) = delete;
	SquadCoordinator &operator=(const SquadCoordinator &) = delete;

	void update(sim::Tick p_tick);
	void update_squad_position();
	void update_shots_fired();
	void update_repair_request(sim::Tick p_tick);
	void update_need_orders_message(sim::Tick p_tick);
	bool is_squad_taking_flag() const;
	void update_squad_paths();
	int side() const { return side_; }
	SquadMissionStatus mission_status() const { return mission_status_; }
	SquadOrder order() const { return order_; }
	bool is_squad_assigned_to_area(int p_area_index) const;
	int strategic_strength(int p_type) const;
	void squad_position(Vec3 &r_position) const { r_position = squad_position_; }
	SquadMember *squad_leader() const;
	int number_of_squad_members() const { return int(members_.size()); }
	int squad_id() const { return squad_index_; }
	bool squad_has_position() const;
	SquadFormation &formation() { return *formation_; }
	const SquadFormation &formation() const { return *formation_; }
	const Vec3 &squad_goto_position() const { return goto_position_; }
	void add_squad_member(SquadPlayer &r_player);
	bool remove_squad_member(int p_player_id);
	void update_squad_leader();
	void un_assign_squad();
	void assign_squad(const SquadArea *p_area, const Vec3 &p_goto, SquadOrder p_order, SquadMissionStatus p_status);
	SquadMember *squad_member(int p_index) const { return members_[size_t(p_index)].get(); }
	SquadMember *squad_member(int p_player_id, int &r_index) const;
	bool has_member_with_kit(int p_kit) const;
	bool is_squad_assigned_to_any_area() const { return area_ != nullptr; }
	const SquadArea *squad_strategic_area_assignment() const { return area_; }
	void reset_need_order_message_timer(sim::Tick p_tick) { need_order_message_tick_ = p_tick; }
	sim::Tick need_order_message_tick() const { return need_order_message_tick_; }
	sim::Tick last_update() const { return now_; }
	void shots_fired(int p_player_id);
	const std::list<SquadMemberFire> &fire_records() const { return fires_; }
	void request_repair(const Vec3 &p_position, sim::Tick p_tick);
	void confirm_repair_request() { repair_confirmed_ = repair_requested_; }
	bool is_repair_requested() const { return repair_requested_; }
	bool have_confirmed_repair_request() const { return repair_confirmed_; }
	const Vec3 &repair_request_position() const { return repair_position_; }
	const SquadServices &services() const { return services_; }

private:
	void on_leader_changed();

	SquadServices services_;
	std::vector<std::unique_ptr<SquadMember>> members_;
	std::list<SquadMemberFire> fires_;
	int squad_index_ = 0;
	int side_ = 0;
	const SquadArea *area_ = nullptr;
	Vec3 goto_position_{};
	SquadOrder order_ = SquadOrder::None;
	int leader_index_ = -1;
	std::unique_ptr<SquadFormation> formation_;
	Vec3 squad_position_{};
	SquadMissionStatus mission_status_ = SquadMissionStatus::None;
	sim::Tick now_ = 0;
	bool repair_requested_ = false;
	Vec3 repair_position_{};
	sim::Tick repair_deadline_ = 0;
	bool repair_confirmed_ = false;
	sim::Tick need_order_message_tick_ = 0;
};

}
