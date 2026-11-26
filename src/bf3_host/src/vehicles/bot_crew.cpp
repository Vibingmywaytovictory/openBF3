#include "bot_crew.h"

#include "../bot_weapon_templates.h"
#include "../tdm_bot_body.h"
#include "../tdm_bot_shared.h"
#include "../tdm_bot_weapons.h"
#include "../tdm_bot_weapons_vehicles.h"
#include "../tdm_strategy.h"
#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/bot_manager.h"
#include "bf3/ai/strategic/strategic_object.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/host/ai_world.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
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

constexpr float kDegreesPerRadian = 180.0f / std::numbers::pi_v<float>;

void seat_aim(const LevelVehicleKind &p_kind, uint8_t p_seat, ai::ChangeSeat &r_seat) {
	if (p_kind.armament == nullptr) {
		return;
	}
	for (const sim::vehicles::ArmamentTurret &turret : p_kind.armament->turrets) {
		if (turret.seat != int(p_seat)) {
			continue;
		}
		if (turret.aim == sim::vehicles::TurretAimAxis::Yaw) {
			r_seat.aim_yaw_deg = std::atan2(turret.frame.forward[0], turret.frame.forward[2]) * kDegreesPerRadian;
			if (turret.rule.constrained) {
				r_seat.yaw_min_deg = turret.rule.min_degrees;
				r_seat.yaw_max_deg = turret.rule.max_degrees;
			}
		} else if (turret.aim == sim::vehicles::TurretAimAxis::Pitch && turret.rule.constrained) {
			r_seat.pitch_min_deg = turret.rule.min_degrees;
			r_seat.pitch_max_deg = turret.rule.max_degrees;
		}
	}
}

ai::ChangeSensed sensed_row(const TdmShared &p_shared, const ServerVehicles &p_vehicles,
		std::span<const ai::ChangeVehicle> p_scene, uint8_t p_slot, const sim::Vec3 &p_at) {
	ai::ChangeSensed s;
	s.position = p_at;
	s.sided = p_shared.host.view->team(p_slot) != 0;
	const std::optional<VehicleSeat> seat = p_vehicles.seat_of(p_slot);
	if (seat && size_t(seat->vehicle) < p_scene.size()) {
		const ai::ChangeVehicle &v = p_scene[size_t(seat->vehicle)];
		s.strength_type = v.strength_type;
		s.strengths = v.seat[std::min<size_t>(seat->seat, ai::kMaxChangeSeats - 1)].strengths;
	} else {
		s.strengths = change_soldier_strengths(p_shared, p_slot);
	}
	return s;
}

std::vector<ai::Vec3> neighbour_offsets(const TdmShared &p_shared, int p_side, uint32_t p_bot_id) {
	std::vector<ai::Vec3> out;
	const TdmStrategy *strategy = p_shared.strategy.get();
	const ai::Sai *sai = strategy != nullptr ? strategy->sai(p_side) : nullptr;
	const HostAiWorld *world = strategy != nullptr ? strategy->world() : nullptr;
	if (sai == nullptr || world == nullptr) {
		return out;
	}
	const int ordered = sai->bot_ordered_pos(int(p_bot_id));
	if (ordered == -1 && sai->bot_current_pos(int(p_bot_id)) != -1) {
		return out;
	}
	const ai::AIStrategicObject *object = world->strategic().strategic_object(ordered);
	const auto *area = dynamic_cast<const ai::AIStrategicArea *>(object);
	if (area == nullptr) {
		return out;
	}
	const ai::Vec2 from = area->position(p_side);
	for (const ai::AIStrategicObject *neighbour : area->neighbours()) {
		if (neighbour->status(p_side) == ai::StrategicStatus::Owned) {
			continue;
		}
		const ai::Vec2 to = neighbour->position(p_side);
		out.push_back({ to.x - from.x, 0.0f, to.y - from.y });
	}
	return out;
}

std::vector<ai::ChangeSensed> change_known(const TdmShared &p_shared, const ServerVehicles &p_vehicles,
		std::span<const ai::ChangeVehicle> p_scene, int p_side) {
	std::vector<ai::ChangeSensed> out;
	const HostAiWorld *world = p_shared.strategy != nullptr ? p_shared.strategy->world() : nullptr;
	if (world == nullptr) {
		return out;
	}
	const size_t slots = p_shared.host.view->states().size();
	for (size_t slot = 0; slot < slots; ++slot) {
		if (const ai::Vec3 *at = world->known_enemy(p_side, uint8_t(slot))) {
			out.push_back(sensed_row(p_shared, p_vehicles, p_scene, uint8_t(slot), *at));
		}
	}
	return out;
}

std::vector<ai::ChangeSensed> change_sensed(const TdmShared &p_shared, const ServerVehicles &p_vehicles,
		std::span<const ai::ChangeVehicle> p_scene, const sim::bots::Perception &p_perception,
		const std::optional<tdm_bot::SeenVehicle> &p_vehicle) {
	std::vector<ai::ChangeSensed> out;
	for (const sim::bots::PerceivedEnemy &e : p_perception.enemies()) {
		out.push_back(sensed_row(p_shared, p_vehicles, p_scene, e.slot, e.last_position));
	}
	if (!p_vehicle || p_vehicle->crew_count == 0) {
		return out;
	}
	const std::span<const uint8_t> crew(p_vehicle->crew.data(), p_vehicle->crew_count);
	const bool perceived = std::any_of(p_perception.enemies().begin(), p_perception.enemies().end(),
			[&](const sim::bots::PerceivedEnemy &p_enemy) {
				return std::find(crew.begin(), crew.end(), p_enemy.slot) != crew.end();
			});
	if (!perceived) {
		out.push_back(sensed_row(p_shared, p_vehicles, p_scene, crew.front(), p_vehicle->centre));
	}
	return out;
}

float counter_scale(const TdmShared &p_shared, KitRole p_kit, uint8_t p_slot, ai::CombatStrengthType p_type) {
	float sum = 0.0f;
	int count = 0;
	for (const auto &team : p_shared.host.gunplay->level().kits.loadouts) {
		for (const KitLoadout &kit : team) {
			if (kit.kit != uint8_t(p_kit)) {
				continue;
			}
			sum += float(p_shared.weapons->of(kit.defs[p_slot], 0).strength(p_type));
			++count;
		}
	}
	return sum > 0.0f ? p_shared.host.soldier_top_speed * float(count) / sum : 0.0f;
}

}

ai::ChangeStrengths change_strength_scales(const TdmShared &p_shared) {
	ai::ChangeStrengths out{};
	if (p_shared.host.gunplay == nullptr || p_shared.weapons == nullptr || !(p_shared.host.soldier_top_speed > 0.0f)) {
		return out;
	}
	const float infantry = counter_scale(p_shared, KitRole::Assault, kKitSlots[0], ai::CombatStrengthType::Infantry);
	for (size_t i = 0; i < out.size(); ++i) {
		const float armour = i == size_t(ai::CombatStrengthType::Infantry)
				? 0.0f
				: counter_scale(p_shared, KitRole::Engineer, uint8_t(core::gameplay::WeaponSlot::Gadget1),
						  ai::CombatStrengthType(i));
		out[i] = armour > 0.0f ? armour : infantry;
	}
	return out;
}

ai::ChangeStrengths change_strengths(const TdmShared &p_shared, const ai::ChangeStrengths &p_per_minute) {
	const ai::ChangeStrengths scale = change_strength_scales(p_shared);
	ai::ChangeStrengths out{};
	for (size_t i = 0; i < out.size(); ++i) {
		out[i] = p_per_minute[i] * scale[i];
	}
	return out;
}

ai::ChangeStrengths change_soldier_strengths(const TdmShared &p_shared, uint8_t p_slot) {
	ai::ChangeStrengths out{};
	const tdm_bot::BotArms arms(p_shared.host.gunplay, p_slot);
	for (uint8_t s = 0; s < sim::weapons::kMaxWeaponSlots; ++s) {
		if (arms.def(s) == nullptr) {
			continue;
		}
		const ai::WeaponTemplate &weapon = tdm_bot::slot_template(p_shared, arms, s);
		for (size_t i = 0; i < ai::kChangeStrengthCount; ++i) {
			out[i] = std::max(out[i], float(weapon.strengths[i]));
		}
	}
	return change_strengths(p_shared, out);
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
		for (const uint8_t occupant : v.occupants) {
			if (occupant != net::kNoVehicleOccupant) {
				c.side = view.team(occupant);
				break;
			}
		}
		c.position = s.body.com_position;
		c.up = sim::vehicles::hull_up_y(s.body.rotation);
		const core::Transform hull = sim::vehicles::hull_transform(s.body.rotation, s.body.com_position);
		c.right = hull.right;
		c.hull_up = hull.up;
		c.forward = hull.forward;
		c.radius = std::sqrt(kind.box.half[0] * kind.box.half[0] + kind.box.half[1] * kind.box.half[1] +
				kind.box.half[2] * kind.box.half[2]);
		c.max_speed = change_vehicle_max_speed(*kind.model);
		c.strength_type = int(vehicle_strength_type(kind));
		const float full = sim::vehicles::full_vehicle_health(kind.damage);
		c.health = full > 0.0f ? p_vehicles.health(i).value_or(0.0f) / full : 0.0f;
		c.seats = uint8_t(std::min<size_t>(kind.seats, ai::kMaxChangeSeats));
		const float basic = p_shared.host.gunplay != nullptr
				? vehicle_basic_strength(kind, p_shared.host.gunplay->level().max_health)
				: 0.0f;
		for (uint8_t k = 0; k < c.seats; ++k) {
			ai::ChangeSeat &seat = c.seat[k];
			seat.basic = basic;
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
			if (p_shared.weapons != nullptr) {
				const SeatArms &arms = p_shared.weapons->seat(p_vehicles.level()->spawners[i].kind, k);
				seat.armed = arms.armed;
				seat.min_range = arms.min_range;
				seat.max_range = arms.max_range;
				seat.strengths = change_strengths(p_shared, arms.strengths);
			}
			seat_aim(kind, k, seat);
			seat.exposed = kind.exposed[k];
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
	b.strengths = change_soldier_strengths(p_shared, p_slot);
	b.basic = ai::kSoldierBasicStrength;
	if (const ai::Sai *sai = p_shared.strategy != nullptr ? p_shared.strategy->sai(b.side) : nullptr) {
		b.enemy_strengths = change_strengths(p_shared, sai->enemy_strengths());
		b.enemy_types = sai->enemy_counts();
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

ai::ChangeChoice BotCrew::evaluate(TdmShared &r_shared, uint8_t p_slot, uint32_t p_bot_id,
		const sim::SoldierState &p_state, bool p_stuck, const sim::bots::Perception &p_perception,
		const std::optional<tdm_bot::SeenVehicle> &p_vehicle) {
	evaluated_.reset();
	const ServerVehicles *const vehicles = r_shared.host.view->server_vehicles();
	if (vehicles == nullptr || !vehicles->on()) {
		return {};
	}
	const std::vector<ai::ChangeVehicle> scene = change_vehicles(r_shared, *vehicles, p_slot, true);
	const std::vector<ai::ChangeSensed> sensed = change_sensed(r_shared, *vehicles, scene, p_perception, p_vehicle);
	ai::ChangeBot bot = change_bot(r_shared, *vehicles, p_slot, p_state, p_stuck);
	const std::vector<ai::ChangeSensed> known = change_known(r_shared, *vehicles, scene, bot.side);
	const std::vector<ai::Vec3> neighbours = neighbour_offsets(r_shared, bot.side, p_bot_id);
	bot.sensed = sensed;
	bot.known = known;
	bot.neighbour_offsets = neighbours;
	const ai::ChangeChoice choice = ai::choose_change(bot, scene);
	if (bot.vehicle < 0) {
		r_shared.count(&TdmBotStats::vehicle_foot_decisions);
		r_shared.count(&TdmBotStats::vehicle_foot_stay_milli, uint64_t(std::llround(double(choice.current) * 1000.0)));
	}
	if (choice.change) {
		r_shared.count(&TdmBotStats::vehicle_choices);
		r_shared.count(&TdmBotStats::vehicle_change_milli, uint64_t(std::llround(double(choice.urgency) * 1000.0)));
	}
	evaluated_ = choice;
	return choice;
}

void BotCrew::settle(TdmShared &r_shared, uint8_t p_slot, bool p_chosen, float p_chosen_urgency) {
	if (!evaluated_) {
		return;
	}
	const ai::ChangeChoice choice = *evaluated_;
	evaluated_.reset();
	if (choice.change) {
		r_shared.count(&TdmBotStats::vehicle_winner_milli, uint64_t(std::llround(double(p_chosen_urgency) * 1000.0)));
	}
	if (p_chosen) {
		r_shared.count(&TdmBotStats::vehicle_change_won);
	}
	book(r_shared, p_slot,
			p_chosen && choice.change && choice.urgency > 0.0f ? std::optional(choice.target) : std::nullopt);
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
			r_body.navigation.approach(sh, p_frame.tick, order.goal, 0.0f, kChangeRepathMetres);
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
