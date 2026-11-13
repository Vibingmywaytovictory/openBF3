#include "server_vehicles.h"

#include "bf3/host/gunplay.h"
#include "bf3/sim/logic/conquest_natives.h"
#include "bf3/sim/vehicles/vehicle_input.h"
#include "bf3/sim/vehicles/vehicle_spawning.h"
#include "bf3/sim/weapons/spread_recoil.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

static_assert(sim::weapons::kNoPackDriver == net::kNoVehicleOccupant);

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
	targets_.clear();
	strikes_.clear();
	crew_kills_.clear();
	wrecks_.clear();
	use_held_.fill(0);
	seat_held_.fill(0);
	sent_tick_.reset();
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
	scope_.configure(scope_viewers_, vehicles_.size(), scoped_);
	return core::Status::success();
}

void ServerVehicles::configure_scope(size_t p_viewers, bool p_scoped) {
	scope_viewers_ = p_viewers;
	scoped_ = p_scoped;
	scope_.configure(scope_viewers_, vehicles_.size(), scoped_);
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
		v.health = sim::vehicles::spawned_vehicle_health(level_->kinds[s.kind].damage);
		v.last_attacker = net::kNoVehicleOccupant;
		v.last_def = sim::weapons::kNoWeapon;
		v.wreck_on_wire = false;
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
		if (!v.live || v.health.condition == sim::vehicles::VehicleCondition::Destroyed) {
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
	vehicles_[seat->vehicle].command = sim::vehicles::driver_command(p_command);
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
	kind_property_.assign(level_->kinds.size(), 0);
	kind_explosion_.assign(level_->kinds.size(), std::nullopt);
	kind_blast_property_.assign(level_->kinds.size(), std::nullopt);
	materials_ = p_materials;
	if (p_materials != nullptr) {
		for (size_t k = 0; k < level_->kinds.size(); ++k) {
			const LevelVehicleKind &kind = level_->kinds[k];
			if (kind.material.present) {
				const sim::weapons::SurfacePair pair = p_materials->pair_of(kind.material);
				kind_property_[k] = pair.resolved ? pair.property : uint8_t(0);
			}
			if (kind.explosion.present) {
				core::Result<sim::weapons::ExplosionSimDef> x =
						sim::weapons::make_explosion_sim_def(kind.explosion, *p_materials);
				if (!x) {
					return core::error_at("vehicle " + kind.blueprint, x.error());
				}
				kind_explosion_[k] = *x;
				const sim::weapons::SurfacePair blast = p_materials->pair_of(kind.explosion.material);
				if (kind.explosion.material.present && blast.resolved) {
					kind_blast_property_[k] = blast.property;
				}
			}
		}
	}
	BF3_TRY(gunnery_.configure(*level_, std::move(p_materials), p_mechanics));
	gunnery_.reset(vehicles_.size());
	if (gunnery_.table() != nullptr) {
		gunnery_source_.configure(*gunnery_.table(), level_->weapons);
	}
	return core::Status::success();
}

void ServerVehicles::configure_damage(
		const sim::weapons::WeaponTable &p_soldier_table, std::span<const core::gameplay::WeaponDef> p_soldier_defs) {
	soldier_source_.configure(p_soldier_table, p_soldier_defs);
}

bool ServerVehicles::hidden_crew(size_t p_slot) const {
	const std::optional<VehicleSeat> seat = seat_of(p_slot);
	return seat && !level_->kinds[level_->spawners[seat->vehicle].kind].exposed[seat->seat];
}

sim::Vec3 ServerVehicles::hull_centre(size_t p_vehicle) const {
	return hull(p_vehicle).transform_point(level_->kinds[level_->spawners[p_vehicle].kind].box.centre);
}

uint8_t ServerVehicles::hull_property(size_t p_vehicle) const {
	const uint16_t kind = level_->spawners[p_vehicle].kind;
	return kind < kind_property_.size() ? kind_property_[kind] : uint8_t(0);
}

void ServerVehicles::refresh_targets() {
	targets_.clear();
	pack_vehicles_.clear();
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		const ServerVehicle &v = vehicles_[i];
		if (!v.live) {
			continue;
		}
		sim::vehicles::VehicleTarget t;
		t.target = sim::vehicles::hull_target(level_->kinds[level_->spawners[i].kind].box, hull(i),
				sim::vehicles::kVehicleTargetIdBase + uint32_t(i), hull_property(i));
		t.target.team = v.team;
		for (const uint8_t o : v.occupants) {
			if (o != net::kNoVehicleOccupant && t.crew_count < t.crew.size()) {
				t.crew[t.crew_count++] = o;
			}
		}
		targets_.push_back(t);
		const sim::VehicleState state = world_->state(sim::VehicleSlot(i));
		pack_vehicles_.push_back(
				sim::weapons::PackVehicle{ 0, hull_centre(i), state.body.com_position, state.body.linear_velocity,
						level_->kinds[level_->spawners[i].kind].min_speed_for_mine, v.occupants[0] });
	}
}

void ServerVehicles::kill_crew(size_t p_vehicle, uint8_t p_attacker, uint16_t p_def, sim::weapons::SimTime p_time,
		std::vector<sim::weapons::DamageEvent> &r_damage) {
	for (const uint8_t o : vehicles_[p_vehicle].occupants) {
		if (o == net::kNoVehicleOccupant) {
			continue;
		}
		sim::weapons::DamageEvent e;
		e.time = p_time;
		e.attacker_slot = p_attacker != net::kNoVehicleOccupant ? p_attacker : o;
		e.target = o;
		e.def = p_def;
		e.kind = sim::weapons::DamageKind::Direct;
		e.base = sim::weapons::kMaxDamage;
		e.amount = sim::weapons::kMaxDamage;
		r_damage.push_back(e);
		++stats_.occupant_kills;
	}
}

void ServerVehicles::explode(size_t p_vehicle, sim::Tick p_tick) {
	ServerVehicle &v = vehicles_[p_vehicle];
	const uint16_t kind = level_->spawners[p_vehicle].kind;
	if (kind < kind_explosion_.size() && kind_explosion_[kind]) {
		wrecks_.push_back(WreckBlast{ p_vehicle, v.last_attacker, p_tick, hull_centre(p_vehicle) });
	}
	v.wreck = world_->state(sim::VehicleSlot(p_vehicle));
	v.wreck_on_wire = true;
	v.exploded = p_tick;
	if (!world_->remove_vehicle(sim::VehicleSlot(p_vehicle)).ok()) {
		++stats_.remove_failures;
	}
	v.live = false;
	v.counting = false;
	v.command = sim::VehicleCommand();
	v.occupants.fill(net::kNoVehicleOccupant);
	v.seat_commands = {};
	gunnery_.spawned(p_vehicle);
	++stats_.exploded;
	if (stats_.live > 0) {
		--stats_.live;
	}
}

void ServerVehicles::step_health(sim::Tick p_tick, std::span<const sim::Vec3> p_soldiers) {
	if (!on()) {
		return;
	}
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		ServerVehicle &v = vehicles_[i];
		if (!v.live) {
			continue;
		}
		const LevelVehicleSpawner &spawner = level_->spawners[i];
		const LevelVehicleKind &kind = level_->kinds[spawner.kind];
		const sim::VehicleState s = world_->state(sim::VehicleSlot(i));
		sim::vehicles::VehicleTickInput in;
		in.dt = sim::kTickSeconds;
		const std::array<float, 4> &q = s.body.rotation;
		in.up_y = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
		in.abandon_applies = spawner.apply_damage_to_abandoned;
		in.time_until_abandoned = spawner.time_until_abandoned;
		in.time_until_abandoned_is_destroyed = spawner.time_until_abandoned_is_destroyed;
		const sim::Vec3 &home = spawner.placement.position;
		const float hx = s.body.origin[0] - home[0], hy = s.body.origin[1] - home[1], hz = s.body.origin[2] - home[2];
		bool kept = hx * hx + hy * hy + hz * hz <= spawner.respawn_range * spawner.respawn_range ||
				std::any_of(
						v.occupants.begin(), v.occupants.end(), [](uint8_t o) { return o != net::kNoVehicleOccupant; });
		const float keep = spawner.keep_alive_radius * spawner.keep_alive_radius;
		for (size_t k = 0; k < p_soldiers.size() && !kept; ++k) {
			const float dx = p_soldiers[k][0] - s.body.com_position[0];
			const float dy = p_soldiers[k][1] - s.body.com_position[1];
			const float dz = p_soldiers[k][2] - s.body.com_position[2];
			kept = dx * dx + dy * dy + dz * dz <= keep;
		}
		in.abandoned = !kept;
		const sim::vehicles::VehicleTickOutcome o = sim::vehicles::step_vehicle_health(kind.damage, v.health, in);
		if ((o.events & sim::vehicles::vehicle_events::kDisabled) != 0) {
			++stats_.disabled;
		}
		if ((o.events & sim::vehicles::vehicle_events::kDestroyed) != 0) {
			++stats_.destroyed;
			const bool rolled = (o.events & sim::vehicles::vehicle_events::kUpsideDownHit) != 0;
			kill_crew(i, rolled ? v.occupants[0] : v.last_attacker, rolled ? sim::weapons::kNoWeapon : v.last_def,
					sim::weapons::tick_start(p_tick), crew_kills_);
		}
		if ((o.events & sim::vehicles::vehicle_events::kExploded) != 0) {
			explode(i, p_tick);
		}
	}
	refresh_targets();
}

void ServerVehicles::strike(const VehicleStrike &p_strike, Gunplay &r_gunplay) {
	const sim::weapons::DamageEvent &e = p_strike.event;
	const size_t index = vehicle_of_target(e.target);
	if (index >= vehicles_.size() || !vehicles_[index].live) {
		return;
	}
	ServerVehicle &v = vehicles_[index];
	const LevelVehicleKind &kind = level_->kinds[level_->spawners[index].kind];
	const core::Transform h = hull(index);
	sim::vehicles::VehicleHit hit;
	hit.damage = p_strike.amount;
	if (p_strike.located) {
		const sim::vehicles::HullHit at =
				sim::vehicles::hull_hit(kind.box, sim::vehicles::to_hull(h, p_strike.position),
						p_strike.directional ? sim::vehicles::to_hull_direction(h, p_strike.direction) : sim::Vec3{});
		hit.zone = at.zone;
		hit.impact_degrees = at.impact_degrees;
		hit.zone_angle = at.zone_angle;
		hit.zone_degrees = at.zone_degrees;
	}
	const uint8_t attacker = uint8_t(std::min<uint32_t>(e.attacker_slot, 255u));
	sim::vehicles::DamageParties parties;
	parties.vehicle_team = v.team;
	parties.attacker_is_player = e.attacker_slot < net::kMaxSlots;
	parties.attacker_team = parties.attacker_is_player ? r_gunplay.team(attacker) : uint8_t(0);
	parties.attacker_is_occupant = std::find(v.occupants.begin(), v.occupants.end(), attacker) != v.occupants.end();
	parties.human_occupant =
			std::any_of(v.occupants.begin(), v.occupants.end(), [](uint8_t o) { return o != net::kNoVehicleOccupant; });
	parties.friendly_fire = r_gunplay.config().friendly_fire;
	hit.modifier = sim::vehicles::damage_modifier(kind.damage.armor_multiplier, parties);
	const float repair = p_strike.soldier_weapon ? soldier_source_.repair(e.def) : 0.0f;
	if (repair > 0.0f && parties.attacker_is_player && (v.team == 0 || v.team == parties.attacker_team)) {
		hit.damage = -repair;
		hit.modifiers = false;
		++stats_.repairs;
	}
	const sim::vehicles::VehicleDamageOutcome o = sim::vehicles::apply_vehicle_damage(kind.damage, v.health, hit);
	if (!o.applied) {
		return;
	}
	++stats_.hits;
	stats_.absorbed += o.absorbed ? 1u : 0u;
	if (o.damage > 0.0f && parties.attacker_is_player) {
		v.last_attacker = attacker;
		v.last_def = p_strike.soldier_weapon ? e.def : sim::weapons::kNoWeapon;
	}
	if ((o.events & sim::vehicles::vehicle_events::kDisabled) != 0) {
		++stats_.disabled;
	}
	if ((o.events & sim::vehicles::vehicle_events::kDestroyed) != 0) {
		++stats_.destroyed;
		kill_crew(index, v.last_attacker, v.last_def, e.time, crew_kills_);
	}
}

void ServerVehicles::gather(std::span<const sim::weapons::DamageEvent> p_events, const VehicleDamageSource &p_source,
		const sim::weapons::ProjectileStepOutput &p_out, bool p_soldier_weapon) {
	for (const sim::weapons::DamageEvent &e : p_events) {
		const size_t index = vehicle_of_target(e.target);
		if (index >= vehicles_.size() || !vehicles_[index].live) {
			continue;
		}
		VehicleStrike s;
		s.event = e;
		s.soldier_weapon = p_soldier_weapon;
		s.amount = !p_soldier_weapon && e.shot_index >= kWreckShotIndexBase ? wreck_amount(e, hull_property(index))
																			: p_source.amount(e, hull_property(index));
		locate_strike(s, p_out, hull_centre(index));
		stats_.unlocated += s.located ? 0u : 1u;
		strikes_.push_back(s);
	}
}

void ServerVehicles::take_hits(Gunplay &r_gunplay, std::vector<sim::weapons::DamageEvent> &r_damage) {
	if (!on()) {
		return;
	}
	strikes_.clear();
	gather(gunnery_.vehicle_damage(), gunnery_source_, gunnery_.step_output(), false);
	taken_.clear();
	r_gunplay.take_damage_if(
			[](const sim::weapons::DamageEvent &e) { return e.target >= sim::vehicles::kVehicleTargetIdBase; }, taken_);
	gather(taken_, soldier_source_, r_gunplay.step_output(), true);
	const auto hidden = [&](const sim::weapons::DamageEvent &e) {
		return e.target < net::kMaxSlots && hidden_crew(e.target);
	};
	taken_.clear();
	r_gunplay.take_damage_if(hidden, taken_);
	const size_t before = r_damage.size();
	std::erase_if(r_damage, hidden);
	stats_.hidden_crew_dropped += taken_.size() + (before - r_damage.size());
	std::sort(strikes_.begin(), strikes_.end(), strike_before);
	for (const VehicleStrike &s : strikes_) {
		strike(s, r_gunplay);
	}
	const bool friendly_fire = r_gunplay.config().friendly_fire;
	for (sim::weapons::DamageEvent &e : crew_kills_) {
		const uint8_t attacker = uint8_t(std::min<uint32_t>(e.attacker_slot, 255u));
		const uint8_t team = r_gunplay.team(attacker);
		if (!friendly_fire && e.attacker_slot != e.target && team != 0 && team == r_gunplay.team(uint8_t(e.target))) {
			e.attacker_slot = e.target;
			e.def = sim::weapons::kNoWeapon;
		}
	}
	r_damage.insert(r_damage.end(), crew_kills_.begin(), crew_kills_.end());
	crew_kills_.clear();
}

core::Transform ServerVehicles::hull(size_t p_vehicle) const {
	const sim::VehicleState s = world_->state(sim::VehicleSlot(p_vehicle));
	return sim::vehicles::hull_transform(s.body.rotation, s.body.origin);
}

void ServerVehicles::resolve_weapons(sim::Tick p_tick, const sim::CollisionWorld &p_world,
		const sim::weapons::HitTargetProvider &p_targets, std::vector<sim::weapons::DamageEvent> &r_damage) {
	const sim::vehicles::JoinedTargets joined(p_targets, targets_);
	gunnery_.step(p_tick, p_world, joined, r_damage);
	for (const WreckBlast &w : wrecks_) {
		const uint16_t kind = level_->spawners[w.vehicle].kind;
		sim::weapons::DetonationEvent source;
		source.player_slot = net::kNoVehicleOccupant;
		source.shot_index = kWreckShotIndexBase + uint32_t(w.vehicle);
		source.time = sim::weapons::tick_start(w.tick);
		source.position = w.at;
		source.normal = sim::Vec3{ 0.0f, 1.0f, 0.0f };
		const VehicleBlastHits hits =
				gunnery_.explode(*kind_explosion_[kind], source, w.attacker, p_world, joined, r_damage);
		stats_.wreck_blasts += hits.ran ? 1u : 0u;
		stats_.wreck_blast_soldier_hits += hits.soldiers;
		stats_.wreck_blast_vehicle_hits += hits.vehicles;
	}
	wrecks_.clear();
}

float ServerVehicles::wreck_amount(const sim::weapons::DamageEvent &p_event, uint8_t p_hull_property) const {
	const size_t vehicle = p_event.shot_index - kWreckShotIndexBase;
	if (vehicle >= level_->spawners.size()) {
		return p_event.base;
	}
	return wreck_blast_amount(
			materials_.get(), kind_blast_property_[level_->spawners[vehicle].kind], p_hull_property, p_event.base);
}

float wreck_blast_amount(const sim::weapons::SurfaceMaterialTable *p_materials, std::optional<uint8_t> p_blast_property,
		uint8_t p_hull_property, float p_base) {
	if (p_materials == nullptr || !p_blast_property) {
		return p_base;
	}
	const core::gameplay::DamageRelation rel = p_materials->relation(*p_blast_property, p_hull_property);
	return rel.present
			? sim::vehicles::material_relation_damage(p_base, rel.protection_multiplier, rel.protection_threshold)
			: p_base;
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
			const sim::vehicles::VehicleCondition condition = vehicles_[i].health.condition;
			commands.push_back(condition == sim::vehicles::VehicleCondition::Destroyed ? sim::VehicleCommand()
							: condition == sim::vehicles::VehicleCondition::Disabled
							? disabled_command(
									  vehicles_[i].command, level_->kinds[level_->spawners[i].kind].disabled_input)
							: vehicles_[i].command);
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
		if (kind.armament == nullptr || v.health.condition == sim::vehicles::VehicleCondition::Destroyed) {
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
	return vehicle_seat_point(kind, seat->seat, s.body.rotation, s.body.origin);
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

uint16_t wire_health(const sim::vehicles::VehicleDamageRule &p_rule, const sim::vehicles::VehicleHealth &p_health) {
	const float full = sim::vehicles::full_vehicle_health(p_rule);
	if (!(full > 0.0f)) {
		return net::kVehicleHealthFull;
	}
	const float ratio = std::clamp(sim::vehicles::vehicle_health(p_rule, p_health) / full, 0.0f, 1.0f);
	return uint16_t(std::ceil(ratio * float(net::kVehicleHealthFull)));
}

sim::VehicleCommand disabled_command(
		const sim::VehicleCommand &p_command, std::span<const core::gameplay::VehicleInputModifierDef> p_modifiers) {
	const auto scaled = [&](std::string_view p_action, float p_axis, float p_full) {
		return std::lround(core::gameplay::apply_input_modifiers(p_modifiers, p_action, p_axis / p_full) * p_full);
	};
	sim::VehicleCommand c = p_command;
	const float throttle = float(std::max<int>(p_command.throttle, -127));
	c.throttle =
			int8_t(throttle >= 0.0f ? scaled("EIAThrottle", throttle, 127.0f) : -scaled("EIABrake", -throttle, 127.0f));
	c.brake = uint8_t(std::clamp<long>(scaled("EIABrake", float(p_command.brake), 255.0f), 0, 255));
	c.steer = int8_t(scaled("EIAYaw", float(std::max<int>(p_command.steer, -127)), 127.0f));
	return c;
}

bool vehicle_burning(const sim::vehicles::VehicleDamageRule &p_rule, const sim::vehicles::VehicleHealth &p_health) {
	return p_health.condition == sim::vehicles::VehicleCondition::Disabled && p_rule.decay_rate > 0.0f &&
			p_health.decay_timer >= p_rule.decay_delay;
}

net::VehicleWire ServerVehicles::wire_of(size_t p_vehicle, const sim::VehicleState &p_state) const {
	const ServerVehicle &v = vehicles_[p_vehicle];
	const LevelVehicleKind &kind = level_->kinds[level_->spawners[p_vehicle].kind];
	net::VehicleWire w;
	w.spawner = uint16_t(p_vehicle);
	w.generation = v.generation;
	w.team = v.team;
	w.seat_count = kind.seats;
	w.occupants = v.occupants;
	w.state = p_state;
	if (kind.armament != nullptr) {
		w.turret_count = uint8_t(std::min(
				{ kind.armament->turrets.size(), net::kMaxVehicleTurrets, sim::vehicles::kMaxArmamentTurrets }));
		for (size_t k = 0; k < w.turret_count; ++k) {
			w.turret_angles[k] = v.armament.bodies[k].angle;
		}
	}
	w.health = wire_health(kind.damage, v.health);
	w.condition = uint8_t(v.health.condition);
	w.burning = vehicle_burning(kind.damage, v.health);
	w.exploded = v.health.exploded;
	return w;
}

namespace {

void pack(std::vector<net::VehicleStates> &r_out, sim::Tick p_tick, net::VehicleWire &&p_wire) {
	if (r_out.empty() || r_out.back().vehicles.size() >= net::kMaxVehiclesPerDatagram) {
		r_out.emplace_back();
		r_out.back().server_tick = p_tick;
	}
	r_out.back().vehicles.push_back(std::move(p_wire));
}

}

std::vector<net::VehicleStates> ServerVehicles::wire_for(size_t p_slot, const GhostViewer &p_viewer, sim::Tick p_tick) {
	std::vector<net::VehicleStates> out;
	if (!on()) {
		return out;
	}
	if (sent_tick_ != p_tick) {
		sent_tick_ = p_tick;
		sent_states_.assign(vehicles_.size(), sim::VehicleState());
		for (size_t i = 0; i < vehicles_.size(); ++i) {
			if (vehicles_[i].live) {
				sent_states_[i] = world_->state(sim::VehicleSlot(i));
			} else if (vehicles_[i].wreck_on_wire) {
				sent_states_[i] = vehicles_[i].wreck;
			}
		}
	}
	for (size_t i = 0; i < vehicles_.size(); ++i) {
		const ServerVehicle &v = vehicles_[i];
		if (!v.live && !(v.wreck_on_wire && p_tick < v.exploded + kWreckWireTicks)) {
			continue;
		}
		const sim::VehicleState &s = sent_states_[i];
		VehicleScopeCandidate c;
		c.team = v.team;
		c.position = s.body.com_position;
		c.occupied_by_viewer = std::find(v.occupants.begin(), v.occupants.end(), uint8_t(p_slot)) != v.occupants.end();
		c.resting = s.body.active == 0 && std::all_of(v.occupants.begin(), v.occupants.end(), [](uint8_t o) {
			return o == net::kNoVehicleOccupant;
		});
		c.generation = v.generation;
		if (!v.live || scope_.due(p_slot, p_viewer, i, c)) {
			pack(out, p_tick, wire_of(i, s));
		}
	}
	return out;
}

}
