#pragma once

#include "bf3/core/result.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/vehicles/vehicle_obstacles.h"
#include "bf3/sim/weapons/gadgets.h"
#include "vehicle_gunnery.h"
#include "vehicle_hits.h"
#include "vehicle_scope.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::sim::logic {
class ConquestNatives;
}

namespace bf3::host {

class Gunplay;

struct ServerVehicleStats {
	uint32_t live = 0;
	uint64_t spawned = 0;
	uint64_t entries = 0;
	uint64_t exits = 0;
	uint64_t seat_switches = 0;
	uint64_t step_failures = 0;
	uint64_t spawn_failures = 0;
	uint64_t restart_failures = 0;
	uint64_t hits = 0;
	uint64_t absorbed = 0;
	uint64_t repairs = 0;
	uint64_t disabled = 0;
	uint64_t destroyed = 0;
	uint64_t exploded = 0;
	uint64_t occupant_kills = 0;
	uint64_t hidden_crew_dropped = 0;
	uint64_t remove_failures = 0;
	uint64_t wreck_blasts = 0;
	uint64_t wreck_blast_soldier_hits = 0;
	uint64_t wreck_blast_vehicle_hits = 0;
	uint64_t unlocated = 0;
	uint64_t pushes = 0;
	uint64_t obstacle_failures = 0;
	uint64_t run_overs = 0;
};

inline constexpr sim::Tick kWreckWireTicks = 6;

struct ServerVehicle {
	bool live = false;
	uint8_t team = 0;
	uint8_t generation = 0;
	uint32_t spawned = 0;
	sim::Tick due = 0;
	bool counting = false;
	std::array<uint8_t, net::kMaxVehicleSeats> occupants{};
	sim::VehicleCommand command;
	std::array<sim::Command, net::kMaxVehicleSeats> seat_commands{};
	sim::vehicles::ArmamentState armament;
	sim::vehicles::VehicleHealth health;
	uint8_t last_attacker = net::kNoVehicleOccupant;
	uint16_t last_def = sim::weapons::kNoWeapon;
	bool wreck_on_wire = false;
	sim::Tick exploded = 0;
	std::array<uint8_t, net::kMaxVehicleSeats> seen{};
	sim::Tick possessed = 0;
	bool possessed_ever = false;
	sim::VehicleState wreck{};
};

struct VehicleSeat {
	uint16_t vehicle = 0;
	uint8_t seat = 0;
};

uint16_t wire_health(const sim::vehicles::VehicleDamageRule &p_rule, const sim::vehicles::VehicleHealth &p_health);
bool vehicle_burning(const sim::vehicles::VehicleDamageRule &p_rule, const sim::vehicles::VehicleHealth &p_health);
float wreck_blast_amount(const sim::weapons::SurfaceMaterialTable *p_materials, std::optional<uint8_t> p_blast_property,
		uint8_t p_hull_property, float p_base);
sim::VehicleCommand disabled_command(
		const sim::VehicleCommand &p_command, std::span<const core::gameplay::VehicleInputModifierDef> p_modifiers);

class ServerVehicles {
public:
	ServerVehicles();
	~ServerVehicles();

	core::Status configure(std::shared_ptr<const LevelVehicles> p_level,
			std::shared_ptr<const sim::StaticWorldShapes> p_shapes, float p_spawn_delay_modifier);
	bool on() const { return world_ != nullptr; }
	void restart();

	void spawn_due(sim::Tick p_tick, const sim::logic::ConquestNatives *p_conquest);
	std::optional<sim::Vec3> use(size_t p_slot, const sim::Command &p_command, const sim::SoldierState &p_soldier);
	void drive(size_t p_slot, const sim::Command &p_command);
	void leave(size_t p_slot);
	core::Status step(sim::Tick p_tick, uint64_t p_weapon_seed);
	core::Status configure_weapons(std::shared_ptr<const sim::weapons::SurfaceMaterialTable> p_materials,
			const sim::weapons::Mechanics &p_mechanics);
	void resolve_weapons(sim::Tick p_tick, const sim::CollisionWorld &p_world,
			const sim::weapons::HitTargetProvider &p_targets, std::vector<sim::weapons::DamageEvent> &r_damage);
	void configure_damage(const sim::weapons::WeaponTable &p_soldier_table,
			std::span<const core::gameplay::WeaponDef> p_soldier_defs);
	void step_health(sim::Tick p_tick, std::span<const sim::Vec3> p_soldiers);
	void take_hits(Gunplay &r_gunplay, std::vector<sim::weapons::DamageEvent> &r_damage);
	void run_over(sim::Tick p_tick, size_t p_slot, std::span<const sim::ObstacleContact> p_contacts);
	const sim::VehicleObstacles &obstacles() const { return obstacles_; }
	std::span<const sim::vehicles::VehicleTarget> targets() const { return targets_; }
	std::span<const sim::weapons::PackVehicle> pack_vehicles() const { return pack_vehicles_; }
	std::span<const sim::weapons::LockCandidate> lock_candidates() const { return lock_candidates_; }
	bool hidden_crew(size_t p_slot) const;
	const VehicleGunnery &gunnery() const { return gunnery_; }
	core::Transform hull(size_t p_vehicle) const;
	std::optional<float> health(size_t p_vehicle) const;

	std::optional<VehicleSeat> seat_of(size_t p_slot) const;
	bool seated(size_t p_slot) const { return seat_of(p_slot).has_value(); }
	std::optional<sim::Vec3> seat_position(size_t p_slot) const;
	std::optional<sim::Vec3> exit_position(size_t p_slot) const;

	void configure_scope(size_t p_viewers, bool p_scoped);
	void forget_viewer(size_t p_slot) { scope_.forget(p_slot); }
	std::vector<net::VehicleStates> wire_for(size_t p_slot, const GhostViewer &p_viewer, sim::Tick p_tick);
	const VehicleScopeStats &scope_stats() const { return scope_.stats(); }
	std::span<const ServerVehicle> vehicles() const { return vehicles_; }
	const LevelVehicles *level() const { return level_.get(); }
	const sim::VehicleWorld *world() const { return world_.get(); }
	const ServerVehicleStats &stats() const { return stats_; }

private:
	std::optional<uint8_t> spawner_team(size_t p_spawner, const sim::logic::ConquestNatives *p_conquest) const;
	void vacate(const VehicleSeat &p_seat);
	void exit(const VehicleSeat &p_seat);
	void switch_seat(size_t p_slot, std::optional<uint8_t> p_wanted);
	core::Status rebuild();
	void strike(const VehicleStrike &p_strike, Gunplay &r_gunplay);
	void kill_crew(size_t p_vehicle, uint8_t p_attacker, uint16_t p_def, sim::weapons::SimTime p_time,
			std::vector<sim::weapons::DamageEvent> &r_damage);
	void explode(size_t p_vehicle, sim::Tick p_tick);
	float wreck_amount(const sim::weapons::DamageEvent &p_event, uint8_t p_hull_property) const;
	void push(const sim::weapons::ProjectileStepOutput &p_out, const sim::weapons::WeaponTable *p_table);
	void refresh_targets();
	void refresh_obstacles();
	net::VehicleWire wire_of(size_t p_vehicle, const sim::VehicleState &p_state) const;
	sim::Vec3 hull_centre(size_t p_vehicle) const;
	uint8_t hull_property(size_t p_vehicle) const;
	void gather(std::span<const sim::weapons::DamageEvent> p_events, const VehicleDamageSource &p_source,
			const sim::weapons::ProjectileStepOutput &p_out, bool p_soldier_weapon);

	std::shared_ptr<const LevelVehicles> level_;
	std::shared_ptr<const sim::StaticWorldShapes> shapes_;
	float modifier_ = 1.0f;
	std::unique_ptr<sim::VehicleWorld> world_;
	std::vector<ServerVehicle> vehicles_;
	std::array<uint8_t, net::kMaxSlots> use_held_{};
	std::array<uint8_t, net::kMaxSlots> seat_held_{};
	ServerVehicleStats stats_;
	VehicleGunnery gunnery_;
	VehicleDamageSource gunnery_source_;
	VehicleDamageSource soldier_source_;
	std::vector<uint8_t> kind_property_;
	std::vector<sim::vehicles::VehicleTarget> targets_;
	std::vector<sim::weapons::PackVehicle> pack_vehicles_;
	std::vector<sim::weapons::LockCandidate> lock_candidates_;
	std::vector<VehicleStrike> strikes_;
	std::vector<sim::weapons::DamageEvent> crew_kills_;
	std::vector<sim::weapons::DamageEvent> run_overs_;
	sim::VehicleObstacles obstacles_;
	std::vector<sim::weapons::DamageEvent> taken_;
	VehicleScope scope_;
	size_t scope_viewers_ = 0;
	bool scoped_ = true;
	std::vector<sim::VehicleState> sent_states_;
	std::optional<sim::Tick> sent_tick_;
	struct WreckBlast {
		size_t vehicle = 0;
		uint8_t attacker = net::kNoVehicleOccupant;
		sim::Tick tick = 0;
		sim::Vec3 at{};
	};
	std::vector<WreckBlast> wrecks_;
	std::shared_ptr<const sim::weapons::SurfaceMaterialTable> materials_;
	std::vector<std::optional<sim::weapons::ExplosionSimDef>> kind_explosion_;
	std::vector<std::optional<uint8_t>> kind_blast_property_;
};

}
