#include "bot_crew.h"

#include "../tdm_bot_body.h"
#include "../tdm_bot_shared.h"
#include "../tdm_strategy.h"
#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/bot_manager.h"
#include "bf3/host/ai_world.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/vehicles/vehicle_input.h"
#include "server_vehicles.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bf3::host {

namespace {

constexpr float kAltitudeProbeMetres = 200.0f;
constexpr float kNavProbeHeight = 8.0f;
constexpr float kChangeRepathMetres = 5.0f;
constexpr float kRpmToRadiansPerSecond = 2.0f * std::numbers::pi_v<float> / 60.0f;

float ground_altitude(const sim::CollisionWorld &p_world, const sim::Vec3 &p_from, float p_origin_y) {
	sim::StaticHit hit;
	if (!p_world.ray(p_from, { 0.0f, -kAltitudeProbeMetres, 0.0f }, sim::query_mask::kMovement, &hit)) {
		return kAltitudeProbeMetres;
	}
	return p_origin_y - hit.position[1];
}

double seconds(sim::Tick p_tick) {
	return double(p_tick) * double(sim::kTickSeconds);
}

std::optional<sim::Vec3> mission_of(const TdmShared &p_shared, uint8_t p_slot) {
	return p_shared.strategy != nullptr ? p_shared.strategy->mission(p_slot) : std::nullopt;
}

bool live_vehicle(const ServerVehicles &p_vehicles, int p_id) {
	return p_id >= 0 && size_t(p_id) < p_vehicles.vehicles().size() && p_vehicles.vehicles()[size_t(p_id)].live &&
			p_vehicles.vehicles()[size_t(p_id)].health.condition != sim::vehicles::VehicleCondition::Destroyed;
}

}

float change_vehicle_max_speed(const sim::VehicleModel &p_model) {
	const sim::LandDriveModel &land = p_model.land;
	if (land.forward_gears == 0) {
		return 0.0f;
	}
	const float gear = land.forward_ratios[size_t(land.forward_gears) - 1];
	float top = 0.0f;
	for (size_t i = 0; i < p_model.wheel_count; ++i) {
		const sim::VehicleWheelModel &w = p_model.wheels[i];
		const float reduction = gear * w.diff_gear_ratio;
		if (w.driven != 0 && reduction > 0.0f) {
			top = std::max(top, land.engine.rpm_max * kRpmToRadiansPerSecond * w.radius / reduction);
		}
	}
	return top;
}

std::vector<ai::ChangeVehicle> change_vehicles(
		const TdmShared &p_shared, const ServerVehicles &p_vehicles, uint8_t p_slot, bool p_ground) {
	const ServerView &view = *p_shared.host.view;
	const std::span<const ServerVehicle> live = p_vehicles.vehicles();
	std::vector<ai::ChangeVehicle> out(live.size());
	const std::optional<sim::Vec3> mine = mission_of(p_shared, p_slot);
	for (size_t i = 0; i < live.size(); ++i) {
		const ServerVehicle &v = live[i];
		ai::ChangeVehicle &c = out[i];
		if (!v.live || v.health.condition == sim::vehicles::VehicleCondition::Destroyed) {
			c.on_nav = false;
			continue;
		}
		const LevelVehicleKind &kind = p_vehicles.level()->kinds[p_vehicles.level()->spawners[i].kind];
		const sim::VehicleState s = p_vehicles.world()->state(sim::VehicleSlot(i));
		c.kind = ai::ChangeUnitKind::Ground;
		c.side = v.team;
		c.position = s.body.com_position;
		c.up = sim::vehicles::hull_up_y(s.body.rotation);
		c.radius = std::sqrt(kind.box.half[0] * kind.box.half[0] + kind.box.half[1] * kind.box.half[1] +
				kind.box.half[2] * kind.box.half[2]);
		c.max_speed = change_vehicle_max_speed(*kind.model);
		const float full = sim::vehicles::full_vehicle_health(kind.damage);
		c.health = full > 0.0f ? p_vehicles.health(i).value_or(0.0f) / full : 0.0f;
		c.seats = uint8_t(std::min<size_t>(kind.seats, ai::kMaxChangeSeats));
		for (uint8_t k = 0; k < c.seats; ++k) {
			ai::ChangeSeat &seat = c.seat[k];
			const uint8_t occupant = v.occupants[k];
			if (occupant != net::kNoVehicleOccupant) {
				seat.occupant = occupant;
				seat.occupant_bot = view.bot(occupant);
				seat.same_order = mission_of(p_shared, occupant) == mine;
			}
			const uint8_t booked =
					i < p_shared.vehicle_bookings.size() ? p_shared.vehicle_bookings[i][k] : net::kNoVehicleOccupant;
			seat.booked_by = booked != net::kNoVehicleOccupant ? int(booked) : -1;
			seat.possessed_s = v.player_left[k] ? seconds(*v.player_left[k]) : ai::kNeverPossessedSeconds;
		}
		if (!p_ground) {
			continue;
		}
		c.altitude = ground_altitude(view.world(), s.body.com_position, s.body.origin[1]);
		c.on_nav = false;
		if (p_shared.nav != nullptr && p_shared.nav->query != nullptr) {
			sim::NavPolyRef poly = 0;
			sim::Vec3 near{};
			const sim::Vec3 extent{ 2.0f * c.radius, kNavProbeHeight, 2.0f * c.radius };
			if (p_shared.nav->query->nearest(s.body.com_position, extent, &poly, &near) && poly != 0) {
				c.on_nav = true;
				c.nav_gap = std::hypot(near[0] - s.body.com_position[0], near[2] - s.body.com_position[2]);
			}
		}
	}
	return out;
}

ai::ChangeBot change_bot(const TdmShared &p_shared, const ServerVehicles &p_vehicles, uint8_t p_slot,
		const sim::SoldierState &p_state, bool p_stuck) {
	const ServerView &view = *p_shared.host.view;
	ai::ChangeBot b;
	b.player_id = p_slot;
	b.side = view.team(p_slot);
	b.position = p_state.position;
	b.max_speed = p_shared.host.soldier_top_speed;
	b.radius = p_shared.nav != nullptr && p_shared.nav->level != nullptr && p_shared.nav->level->nav != nullptr
			? p_shared.nav->level->nav->params().agent_radius
			: 0.0f;
	const float full = p_shared.host.gunplay != nullptr ? p_shared.host.gunplay->level().max_health : 0.0f;
	b.health = full > 0.0f ? p_state.health / full : 0.0f;
	b.now_s = seconds(view.tick());
	b.bail_value = p_shared.ai != nullptr ? p_shared.ai->main().bot_manager().vehicle_bail_value() : b.bail_value;
	b.stuck = p_stuck;
	if (const std::optional<VehicleSeat> seat = p_vehicles.seat_of(p_slot)) {
		b.vehicle = seat->vehicle;
		b.seat = seat->seat;
	}
	return b;
}

void BotCrew::new_life(TdmShared &r_shared, uint8_t p_slot) {
	book(r_shared, p_slot, std::nullopt);
	use_down_ = false;
}

void BotCrew::book(TdmShared &r_shared, uint8_t p_slot, std::optional<ai::ChangeTarget> p_target) {
	if (target_ && !target_->soldier() && size_t(target_->vehicle) < r_shared.vehicle_bookings.size()) {
		uint8_t &held = r_shared.vehicle_bookings[size_t(target_->vehicle)][target_->seat];
		if (held == p_slot) {
			held = net::kNoVehicleOccupant;
		}
	}
	target_ = p_target;
	if (target_ && !target_->soldier()) {
		if (r_shared.vehicle_bookings.size() <= size_t(target_->vehicle)) {
			std::array<uint8_t, net::kMaxVehicleSeats> none;
			none.fill(net::kNoVehicleOccupant);
			r_shared.vehicle_bookings.resize(size_t(target_->vehicle) + 1, none);
		}
		r_shared.vehicle_bookings[size_t(target_->vehicle)][target_->seat] = p_slot;
	}
}

void BotCrew::decide(
		TdmShared &r_shared, uint8_t p_slot, const sim::SoldierState &p_state, bool p_stuck, float p_chosen_urgency) {
	const ServerVehicles *const vehicles = r_shared.host.view->server_vehicles();
	if (vehicles == nullptr || !vehicles->on() || !r_shared.config->change_unmerged) {
		return;
	}
	const std::vector<ai::ChangeVehicle> scene = change_vehicles(r_shared, *vehicles, p_slot, true);
	const ai::ChangeBot bot = change_bot(r_shared, *vehicles, p_slot, p_state, p_stuck);
	const ai::ChangeChoice choice = ai::choose_change(bot, scene);
	if (bot.vehicle < 0) {
		r_shared.count(&TdmBotStats::vehicle_foot_decisions);
		r_shared.count(&TdmBotStats::vehicle_foot_stay_milli, uint64_t(std::llround(double(choice.current) * 1000.0)));
	}
	if (choice.change) {
		r_shared.count(&TdmBotStats::vehicle_choices);
		r_shared.count(&TdmBotStats::vehicle_change_milli, uint64_t(std::llround(double(choice.urgency) * 1000.0)));
		r_shared.count(&TdmBotStats::vehicle_winner_milli, uint64_t(std::llround(double(p_chosen_urgency) * 1000.0)));
	}
	book(r_shared, p_slot, choice.change && choice.urgency > 0.0f ? std::optional(choice.target) : std::nullopt);
}

bool BotCrew::press_use() {
	use_down_ = !use_down_;
	return use_down_;
}

bool BotCrew::command(tdm_bot::BotBody &r_body, const tdm_bot::BotFrame &p_frame, sim::Command &r_cmd) {
	TdmShared &sh = *r_body.shared;
	const ServerVehicles *const vehicles = sh.host.view->server_vehicles();
	if (vehicles == nullptr || !vehicles->on()) {
		return false;
	}
	const bool seated = vehicles->seated(r_body.slot);
	if (!target_) {
		return seated;
	}
	const std::vector<ai::ChangeVehicle> scene = change_vehicles(sh, *vehicles, r_body.slot, false);
	if (!target_->soldier() && !live_vehicle(*vehicles, target_->vehicle)) {
		book(sh, r_body.slot, std::nullopt);
		return seated;
	}
	const ai::ChangeBot bot = change_bot(sh, *vehicles, r_body.slot, p_frame.self, false);
	const ai::ChangeOrder order = ai::change_plan_step(bot, scene, *target_);
	switch (order.step) {
		case ai::ChangeStep::None:
			book(sh, r_body.slot, std::nullopt);
			return seated;
		case ai::ChangeStep::Hold:
			return true;
		case ai::ChangeStep::SelectSeat:
			r_cmd.buttons = sim::vehicles::with_seat_request(r_cmd.buttons, order.seat);
			return true;
		case ai::ChangeStep::GetOut:
			if (press_use()) {
				r_cmd.buttons |= sim::buttons::kUse;
			}
			return true;
		case ai::ChangeStep::Approach:
		case ai::ChangeStep::Enter: {
			tdm_bot::BotIntent i;
			r_body.navigation.set_goal(sh, p_frame.tick, order.goal, 0.0f, true, kChangeRepathMetres);
			r_body.walk_facing(p_frame, i);
			const bool aligned = r_body.turn(p_frame, i);
			r_body.compose(p_frame, r_body.activity, i, aligned, r_cmd);
			if (order.step == ai::ChangeStep::Enter && press_use()) {
				r_cmd.buttons |= sim::buttons::kUse;
			}
			return true;
		}
	}
	return seated;
}

}
