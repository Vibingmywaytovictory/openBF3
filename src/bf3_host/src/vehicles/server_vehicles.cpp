#include "server_vehicles.h"

#include "bf3/sim/logic/conquest_natives.h"
#include "bf3/sim/vehicles/vehicle_input.h"
#include "bf3/sim/vehicles/vehicle_spawning.h"
#include "bf3/sim/weapons/spread_recoil.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

sim::Vec3 rotate(const std::array<float, 4> &p_q, const sim::Vec3 &p_v) {
	const float x = p_q[0], y = p_q[1], z = p_q[2], w = p_q[3];
	const sim::Vec3 t = { 2.0f * (y * p_v[2] - z * p_v[1]), 2.0f * (z * p_v[0] - x * p_v[2]),
		2.0f * (x * p_v[1] - y * p_v[0]) };
	return { p_v[0] + w * t[0] + (y * t[2] - z * t[1]), p_v[1] + w * t[1] + (z * t[0] - x * t[2]),
		p_v[2] + w * t[2] + (x * t[1] - y * t[0]) };
}

}

ServerVehicles::ServerVehicles() {
	use_held_.fill(0);
	seat_held_.fill(0);
}

ServerVehicles::~ServerVehicles() = default;

core::Status ServerVehicles::configure(std::shared_ptr<const LevelVehicles> p_level,
		std::shared_ptr<const sim::StaticWorldShapes> p_shapes, float p_spawn_delay_modifier) {
	level_ = std::move(p_level);
	shapes_ = std::move(p_shapes);
	modifier_ = std::isfinite(p_spawn_delay_modifier) && p_spawn_delay_modifier >= 0.0f ? p_spawn_delay_modifier : 1.0f;
	return rebuild();
}

void ServerVehicles::restart() {
	if (!rebuild().ok()) {
		++stats_.restart_failures;
	}
}

core::Status ServerVehicles::rebuild() {
	world_.reset();
	vehicles_.clear();
	use_held_.fill(0);
	seat_held_.fill(0);
	stats_.live = 0;
	if (level_ == nullptr || shapes_ == nullptr || level_->spawners.empty()) {
		return core::Status::success();
	}
	sim::VehicleWorldOptions o;
	o.max_vehicles = uint32_t(std::min<size_t>(level_->spawners.size(), sim::kMaxVehicleSlots));
	o.tuning = level_->tuning;
	core::Result<std::unique_ptr<sim::VehicleWorld>> w = sim::VehicleWorld::create(shapes_, o);
	if (!w) {
		return core::error_at("server vehicles", w.error());
	}
	world_ = std::move(*w);
	vehicles_.resize(o.max_vehicles);
	for (ServerVehicle &v : vehicles_) {
		v.occupants.fill(net::kNoVehicleOccupant);
	}
	gunnery_.reset(vehicles_.size());
	return core::Status::success();
}

std::optional<uint8_t> ServerVehicles::spawner_team(
		size_t p_spawner, const sim::logic::ConquestNatives *p_conquest) const {
	const LevelVehicleSpawner &s = level_->spawners[p_spawner];
	if (!s.enabled) {
		return std::nullopt;
	}
	if (p_conquest != nullptr) {
		if (const std::optional<uint8_t> gated = p_conquest->vehicle_spawner_team(s.object)) {
			return *gated == 0 ? std::nullopt : gated;
		}
	}
	return s.team;
}

void ServerVehicles::spawn_due(sim::Tick p_tick, const sim::logic::ConquestNatives *p_conquest) {
	if (!on()) {
		return;
	}
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		ServerVehicle &v = vehicles_[i];
		if (v.live) {
			continue;
		}
		const std::optional<uint8_t> team = spawner_team(i, p_conquest);
		if (!team) {
			v.counting = false;
			continue;
		}
		const LevelVehicleSpawner &s = level_->spawners[i];
		if (!v.counting) {
			v.counting = true;
			v.due = p_tick +
					sim::seconds_to_ticks(sim::vehicles::vehicle_spawn_delay(
							v.spawned, s.initial_spawn_delay, s.spawn_delay, modifier_));
		}
		if (p_tick < v.due) {
			continue;
		}
		const core::Status added = world_->add_vehicle(sim::VehicleSlot(i), level_->kinds[s.kind].model, s.placement);
		if (!added.ok()) {
			++stats_.spawn_failures;
			v.counting = false;
			continue;
		}
		v.live = true;
		v.counting = false;
		v.team = *team;
		++v.generation;
		++v.spawned;
		v.command = sim::VehicleCommand();
		v.occupants.fill(net::kNoVehicleOccupant);
		v.seat_commands = {};
		v.armament = sim::vehicles::ArmamentState();
		gunnery_.spawned(i);
		++stats_.spawned;
		++stats_.live;
	}
}

std::optional<VehicleSeat> ServerVehicles::seat_of(size_t p_slot) const {
	if (p_slot >= net::kMaxSlots) {
		return std::nullopt;
	}
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		const ServerVehicle &v = vehicles_[i];
		if (!v.live) {
			continue;
		}
		for (size_t k = 0; k < v.occupants.size(); ++k) {
			if (v.occupants[k] == p_slot) {
				return VehicleSeat{ uint16_t(i), uint8_t(k) };
			}
		}
	}
	return std::nullopt;
}

void ServerVehicles::vacate(const VehicleSeat &p_seat) {
	ServerVehicle &v = vehicles_[p_seat.vehicle];
	v.occupants[p_seat.seat] = net::kNoVehicleOccupant;
	if (p_seat.seat == 0) {
		v.command = sim::VehicleCommand();
	}
}

void ServerVehicles::exit(const VehicleSeat &p_seat) {
	vacate(p_seat);
	++stats_.exits;
}

std::optional<sim::Vec3> ServerVehicles::use(
		size_t p_slot, const sim::Command &p_command, const sim::SoldierState &p_soldier) {
	if (!on() || p_slot >= net::kMaxSlots) {
		return std::nullopt;
	}
	const bool held = (p_command.buttons & sim::buttons::kUse) != 0;
	const bool pressed = held && use_held_[p_slot] == 0;
	use_held_[p_slot] = held ? 1 : 0;
	switch_seat(p_slot, sim::vehicles::seat_request(p_command.buttons));
	if (!pressed) {
		return std::nullopt;
	}
	if (const std::optional<VehicleSeat> seat = seat_of(p_slot)) {
		const std::optional<sim::Vec3> out = exit_position(p_slot);
		exit(*seat);
		return out;
	}
	float best = INFINITY;
	std::optional<VehicleSeat> chosen;
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		const ServerVehicle &v = vehicles_[i];
		if (!v.live) {
			continue;
		}
		const LevelVehicleKind &kind = level_->kinds[level_->spawners[i].kind];
		const sim::Vec3 centre = world_->state(sim::VehicleSlot(i)).body.com_position;
		const float dx = centre[0] - p_soldier.position[0], dy = centre[1] - p_soldier.position[1],
					dz = centre[2] - p_soldier.position[2];
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (!(distance <= kind.entry_radius) || distance >= best) {
			continue;
		}
		for (uint8_t k = 0; k < kind.seats; ++k) {
			if (v.occupants[k] == net::kNoVehicleOccupant) {
				best = distance;
				chosen = VehicleSeat{ uint16_t(i), k };
				break;
			}
		}
	}
	if (chosen) {
		vehicles_[chosen->vehicle].occupants[chosen->seat] = uint8_t(p_slot);
		++stats_.entries;
	}
	return std::nullopt;
}

void ServerVehicles::switch_seat(size_t p_slot, std::optional<uint8_t> p_wanted) {
	const uint8_t key = p_wanted ? uint8_t(*p_wanted + 1u) : uint8_t(0);
	const bool pressed = key != 0 && key != seat_held_[p_slot];
	seat_held_[p_slot] = key;
	const std::optional<VehicleSeat> seat = pressed ? seat_of(p_slot) : std::nullopt;
	if (!seat || *p_wanted == seat->seat) {
		return;
	}
	ServerVehicle &v = vehicles_[seat->vehicle];
	if (*p_wanted >= level_->kinds[level_->spawners[seat->vehicle].kind].seats ||
			v.occupants[*p_wanted] != net::kNoVehicleOccupant) {
		return;
	}
	vacate(*seat);
	v.occupants[*p_wanted] = uint8_t(p_slot);
	++stats_.seat_switches;
}

void ServerVehicles::drive(size_t p_slot, const sim::Command &p_command) {
	const std::optional<VehicleSeat> seat = seat_of(p_slot);
	if (!seat) {
		return;
	}
	vehicles_[seat->vehicle].seat_commands[seat->seat] = p_command;
	if (seat->seat != 0) {
		return;
	}
	sim::VehicleCommand &c = vehicles_[seat->vehicle].command;
	c.tick = p_command.tick;
	c.sequence = p_command.sequence;
	c.throttle = p_command.move_forward;
	c.brake = 0;
	c.steer = p_command.move_right;
	c.buttons = sim::vehicles::handbrake_held(p_command.buttons) ? sim::vehicle_buttons::kHandbrake : uint8_t(0);
	c.flags = sim::vehicle_flags::kDriven;
}

void ServerVehicles::leave(size_t p_slot) {
	if (const std::optional<VehicleSeat> seat = seat_of(p_slot)) {
		exit(*seat);
	}
	if (p_slot < use_held_.size()) {
		use_held_[p_slot] = 0;
		seat_held_[p_slot] = 0;
	}
}

core::Status ServerVehicles::configure_weapons(std::shared_ptr<const sim::weapons::SurfaceMaterialTable> p_materials,
		const sim::weapons::Mechanics &p_mechanics) {
	if (level_ == nullptr) {
		return core::Status::success();
	}
	BF3_TRY(gunnery_.configure(*level_, std::move(p_materials), p_mechanics));
	gunnery_.reset(vehicles_.size());
	return core::Status::success();
}

core::Transform ServerVehicles::hull(size_t p_vehicle) const {
	const sim::VehicleState s = world_->state(sim::VehicleSlot(p_vehicle));
	return sim::vehicles::hull_transform(s.body.rotation, s.body.origin);
}

void ServerVehicles::resolve_weapons(sim::Tick p_tick, const sim::CollisionWorld &p_world,
		const sim::weapons::HitTargetProvider &p_targets, std::vector<sim::weapons::DamageEvent> &r_damage) {
	gunnery_.step(p_tick, p_world, p_targets, r_damage);
}

core::Status ServerVehicles::step(sim::Tick p_tick, uint64_t p_weapon_seed) {
	if (!on()) {
		return core::Status::success();
	}
	std::vector<sim::VehicleSlot> slots;
	std::vector<sim::VehicleCommand> commands;
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		if (vehicles_[i].live) {
			slots.push_back(sim::VehicleSlot(i));
			commands.push_back(vehicles_[i].command);
		}
	}
	if (slots.empty()) {
		return core::Status::success();
	}
	const core::Status s = world_->step(slots, commands);
	if (!s.ok()) {
		++stats_.step_failures;
		return s;
	}
	for (const sim::VehicleSlot slot : slots) {
		ServerVehicle &v = vehicles_[slot];
		const LevelVehicleKind &kind = level_->kinds[level_->spawners[slot].kind];
		if (kind.armament == nullptr) {
			continue;
		}
		const core::Transform h = hull(slot);
		if (p_tick % sim::vehicles::kTurretTickDivisor == 0) {
			std::array<sim::vehicles::SeatAim, net::kMaxVehicleSeats> aims{};
			for (size_t k = 0; k < aims.size(); ++k) {
				aims[k].active = v.occupants[k] != net::kNoVehicleOccupant;
				aims[k].direction = sim::weapons::aim_direction(v.seat_commands[k].yaw, v.seat_commands[k].pitch, {});
			}
			sim::vehicles::step_armament(*kind.armament, v.armament, h, aims);
		}
		gunnery_.fire(p_tick, slot, kind, v.armament, h, v.occupants, v.seat_commands, p_weapon_seed);
	}
	return s;
}

std::optional<sim::Vec3> ServerVehicles::seat_position(size_t p_slot) const {
	const std::optional<VehicleSeat> seat = seat_of(p_slot);
	if (!seat) {
		return std::nullopt;
	}
	const sim::VehicleState s = world_->state(sim::VehicleSlot(seat->vehicle));
	const LevelVehicleKind &kind = level_->kinds[level_->spawners[seat->vehicle].kind];
	const core::Vec3 &at = kind.seat_defs[seat->seat].transform.trans;
	const sim::Vec3 offset = rotate(s.body.rotation, sim::Vec3{ at[0], at[1], at[2] });
	return sim::Vec3{ s.body.origin[0] + offset[0], s.body.origin[1] + offset[1], s.body.origin[2] + offset[2] };
}

std::optional<sim::Vec3> ServerVehicles::exit_position(size_t p_slot) const {
	const std::optional<VehicleSeat> seat = seat_of(p_slot);
	if (!seat) {
		return std::nullopt;
	}
	const sim::VehicleState s = world_->state(sim::VehicleSlot(seat->vehicle));
	const LevelVehicleKind &kind = level_->kinds[level_->spawners[seat->vehicle].kind];
	const std::optional<core::Vec3> at = vehicle_exit_point(kind, seat->seat);
	if (!at) {
		return seat_position(p_slot);
	}
	const sim::Vec3 offset = rotate(s.body.rotation, *at);
	return sim::Vec3{ s.body.origin[0] + offset[0], s.body.origin[1] + offset[1], s.body.origin[2] + offset[2] };
}

std::vector<net::VehicleStates> ServerVehicles::wire(sim::Tick p_tick) const {
	std::vector<net::VehicleStates> out;
	if (!on()) {
		return out;
	}
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		const ServerVehicle &v = vehicles_[i];
		if (!v.live) {
			continue;
		}
		if (out.empty() || out.back().vehicles.size() >= net::kMaxVehiclesPerDatagram) {
			out.emplace_back();
			out.back().server_tick = p_tick;
		}
		net::VehicleWire w;
		w.spawner = uint16_t(i);
		w.generation = v.generation;
		w.team = v.team;
		w.seat_count = level_->kinds[level_->spawners[i].kind].seats;
		w.occupants = v.occupants;
		w.state = world_->state(sim::VehicleSlot(i));
		const LevelVehicleKind &kind = level_->kinds[level_->spawners[i].kind];
		if (kind.armament != nullptr) {
			w.turret_count = uint8_t(std::min(
					{ kind.armament->turrets.size(), net::kMaxVehicleTurrets, sim::vehicles::kMaxArmamentTurrets }));
			for (size_t k = 0; k < w.turret_count; ++k) {
				w.turret_angles[k] = v.armament.bodies[k].angle;
			}
		}
		out.back().vehicles.push_back(w);
	}
	return out;
}

}
