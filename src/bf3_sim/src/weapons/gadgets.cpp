#include "bf3/sim/weapons/gadgets.h"

#include "bf3/core/ebx.h"
#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/damage.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace bf3::sim::weapons {

namespace {

core::Error gadget_error(std::string_view p_weapon, const std::string &p_what) {
	return core::make_error("gadget " + std::string(p_weapon) + ": " + p_what);
}

std::optional<float> f32_of(const core::EbxStruct &p_s, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_s.get(p_field);
	return v ? v->as_f32() : std::nullopt;
}

std::optional<bool> bool_of(const core::EbxStruct &p_s, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_s.get(p_field);
	return v ? v->as_bool() : std::nullopt;
}

std::optional<int32_t> i32_of(const core::EbxStruct &p_s, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_s.get(p_field);
	return v ? v->as_i32() : std::nullopt;
}

std::optional<core::EbxStruct> struct_of(const core::EbxStruct &p_s, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_s.get(p_field);
	return v ? v->as_struct() : std::nullopt;
}

bool finite_in(float p_v, float p_lo, float p_hi) {
	return std::isfinite(p_v) && p_v >= p_lo && p_v <= p_hi;
}

core::Result<core::EbxInstance> weapon_data(const core::EbxResolver &p_db, std::string_view p_weapon) {
	const core::EbxPartition *part = p_db.partition_by_name(p_weapon);
	if (part == nullptr) {
		return gadget_error(p_weapon, "no such partition");
	}
	const core::EbxInstance blueprint = part->primary();
	if (!blueprint.is_a("SoldierWeaponBlueprint")) {
		return gadget_error(p_weapon, "the primary instance is " + std::string(blueprint.type_name()));
	}
	const std::optional<core::EbxInstance> data = p_db.follow(blueprint.data(), "Object");
	if (!data || !data->is_a("SoldierWeaponData")) {
		return gadget_error(p_weapon, "no SoldierWeaponData");
	}
	return *data;
}

core::Result<core::EbxInstance> firing_function(const core::EbxResolver &p_db, std::string_view p_weapon) {
	core::Result<core::EbxInstance> data = weapon_data(p_db, p_weapon);
	if (!data) {
		return data.error();
	}
	const std::optional<core::EbxInstance> firing = p_db.follow(data->data(), "WeaponFiring");
	if (!firing || !firing->is_a("WeaponFiringData")) {
		return gadget_error(p_weapon, "no WeaponFiringData");
	}
	const std::optional<core::EbxInstance> fire = p_db.follow(firing->data(), "PrimaryFire");
	if (!fire || !fire->is_a("FiringFunctionData")) {
		return gadget_error(p_weapon, "no PrimaryFire FiringFunctionData");
	}
	return *fire;
}

core::Result<SupplyUnitDef> read_supply_unit(
		std::string_view p_weapon, const core::EbxStruct &p_supply, std::string_view p_unit) {
	const std::optional<core::EbxStruct> u = struct_of(p_supply, p_unit);
	if (!u) {
		return gadget_error(p_weapon, "SupplyData has no " + std::string(p_unit));
	}
	SupplyUnitDef d;
	const std::optional<float> radius = f32_of(*u, "Radius");
	const std::optional<float> inc = f32_of(*u, "SupplyIncSpeed");
	const std::optional<float> capacity = f32_of(*u, "SupplyPointsCapacity");
	const std::optional<float> refill = f32_of(*u, "SupplyPointsRefillSpeed");
	const std::optional<bool> infinite = bool_of(*u, "InfiniteCapacity");
	if (!radius || !inc || !capacity || !refill || !infinite) {
		return gadget_error(p_weapon, std::string(p_unit) + " misses a SupplyUnitSphereData field");
	}
	if (!finite_in(*radius, 0.0f, 100.0f) || !finite_in(*inc, 0.0f, 1000.0f) || !finite_in(*capacity, 0.0f, 1e6f) ||
			!finite_in(*refill, 0.0f, 1e6f)) {
		return gadget_error(p_weapon, std::string(p_unit) + " has a value out of range");
	}
	d.radius = *radius;
	d.inc_speed = *inc;
	d.capacity = *capacity;
	d.refill_speed = *refill;
	d.infinite = *infinite;
	return d;
}

bool has_mobile_spawn(const core::EbxResolver &p_db, const core::EbxInstance &p_projectile) {
	const std::optional<core::EbxInstance> vehicle = p_db.follow(p_projectile.data(), "Vehicle");
	return vehicle && !vehicle->partition().instances_of("MobileSpawnComponentData").empty();
}

Vec3 unit_or(const Vec3 &p_v, const Vec3 &p_fallback) {
	const float l = core::vec::length(p_v);
	return l > 1e-6f && std::isfinite(l) ? core::vec::scale(p_v, 1.0f / l) : p_fallback;
}

core::Result<GadgetDef> read_explosion_pack(std::string_view p_weapon, const core::EbxStruct &p_p, GadgetDef p_g) {
	const std::optional<core::EbxStruct> soldier = struct_of(p_p, "SoldierDetonationData");
	if (!soldier) {
		return gadget_error(p_weapon, "ExplosionPackEntityData has no SoldierDetonationData");
	}
	const std::optional<bool> remote = bool_of(p_p, "HasRemoteDetonator");
	const std::optional<bool> vehicle = bool_of(p_p, "HasVehicleDetonation");
	const std::optional<float> vehicle_radius = f32_of(p_p, "VehicleDetonationRadius");
	const std::optional<float> vehicle_delay = f32_of(p_p, "VehicleDetonationActivationDelay");
	const std::optional<bool> on_soldiers = bool_of(*soldier, "HasSoldierDetonation");
	const std::optional<float> soldier_radius = f32_of(*soldier, "Radius");
	const std::optional<float> angle = f32_of(*soldier, "Angle");
	const std::optional<float> min_speed = f32_of(*soldier, "MinSpeedForActivation");
	const std::optional<float> soldier_delay = f32_of(*soldier, "SoldierDetonationActivationDelay");
	const std::optional<bool> use_angle = bool_of(*soldier, "UseAngle");
	const std::optional<int32_t> count = i32_of(p_p, "MaxCount");
	const std::optional<float> incline = f32_of(p_p, "MaxAttachableInclination");
	const std::optional<float> health = f32_of(p_p, "Health");
	const std::optional<bool> destructible = bool_of(p_p, "IsDestructible");
	const std::optional<bool> explosion = bool_of(p_p, "ReceivesExplosionDamage");
	const std::optional<float> ttl = f32_of(p_p, "TimeToLive");
	const std::optional<float> ttl_gone = f32_of(p_p, "TimeToLiveOnPlayerDeath");
	if (!remote || !vehicle || !vehicle_radius || !vehicle_delay || !on_soldiers || !soldier_radius || !angle ||
			!min_speed || !soldier_delay || !use_angle || !count || !incline || !health || !destructible ||
			!explosion || !ttl || !ttl_gone) {
		return gadget_error(p_weapon, "ExplosionPackEntityData misses a field");
	}
	if (!finite_in(*vehicle_radius, 0.0f, 100.0f) || !finite_in(*vehicle_delay, 0.0f, 3600.0f) ||
			!finite_in(*soldier_radius, 0.0f, 100.0f) || !finite_in(*angle, 0.0f, 180.0f) ||
			!finite_in(*min_speed, 0.0f, 1000.0f) || !finite_in(*soldier_delay, 0.0f, 3600.0f) || *count < 1 ||
			*count > 255 || !finite_in(*incline, 0.0f, 180.0f) || !finite_in(*health, 0.0f, 1e6f) ||
			!finite_in(*ttl, 0.0f, 86400.0f) || !finite_in(*ttl_gone, 0.0f, 86400.0f)) {
		return gadget_error(p_weapon, "ExplosionPackEntityData has a value out of range");
	}
	p_g.kind = GadgetKind::ExplosionPack;
	p_g.pack.remote_detonator = *remote;
	p_g.pack.vehicle_detonation = *vehicle;
	p_g.pack.vehicle_radius = *vehicle_radius;
	p_g.pack.vehicle_delay_s = *vehicle_delay;
	p_g.pack.soldier_detonation = *on_soldiers;
	p_g.pack.soldier_radius = *soldier_radius;
	p_g.pack.soldier_angle_deg = *angle;
	p_g.pack.soldier_min_speed = *min_speed;
	p_g.pack.soldier_delay_s = *soldier_delay;
	p_g.pack.use_angle = *use_angle;
	p_g.max_count = uint8_t(*count);
	p_g.max_attach_inclination_deg = *incline;
	p_g.health = *health;
	p_g.destructible = *destructible;
	p_g.receives_explosion_damage = *explosion;
	p_g.time_to_live_s = *ttl;
	p_g.time_to_live_on_owner_gone_s = *ttl_gone;
	return p_g;
}
}

const char *gadget_kind_name(GadgetKind p_kind) {
	switch (p_kind) {
		case GadgetKind::None:
			return "none";
		case GadgetKind::Supply:
			return "supply";
		case GadgetKind::SpawnBeacon:
			return "spawn beacon";
		case GadgetKind::ExplosionPack:
			return "explosion pack";
	}
	return "?";
}

core::Result<AmmoPickupDef> read_ammo_pickup(const core::EbxResolver &p_db, std::string_view p_weapon) {
	core::Result<core::EbxInstance> fire = firing_function(p_db, p_weapon);
	if (!fire) {
		return fire.error();
	}
	const std::optional<core::EbxStruct> ammo = struct_of(fire->data(), "Ammo");
	if (!ammo) {
		return gadget_error(p_weapon, "FiringFunctionData has no Ammo");
	}
	const std::optional<int32_t> amount = i32_of(*ammo, "AmmoBagPickupAmount");
	const std::optional<float> multiplier = f32_of(*ammo, "AmmoBagPickupDelayMultiplier");
	const std::optional<bool> replenish = bool_of(*ammo, "AutoReplenishMagazine");
	const std::optional<float> delay = f32_of(*ammo, "AutoReplenishDelay");
	if (!amount || !multiplier || !replenish || !delay) {
		return gadget_error(p_weapon, "AmmoConfigData misses a pickup field");
	}
	if (*amount < -1 || *amount > 10000 || !finite_in(*multiplier, 0.0f, 1000.0f) ||
			!finite_in(*delay, 0.0f, 3600.0f)) {
		return gadget_error(p_weapon, "AmmoConfigData has a pickup value out of range");
	}
	AmmoPickupDef d;
	d.amount = *amount;
	d.delay_multiplier = *multiplier;
	d.auto_replenish = *replenish;
	d.auto_replenish_delay = *delay;
	return d;
}

core::Result<GadgetDef> read_gadget_def(const core::EbxResolver &p_db, std::string_view p_weapon) {
	core::Result<core::EbxInstance> fire = firing_function(p_db, p_weapon);
	if (!fire) {
		return fire.error();
	}
	GadgetDef g;
	g.weapon = std::string(p_weapon);
	core::Result<core::EbxInstance> weapon = weapon_data(p_db, p_weapon);
	if (!weapon) {
		return weapon.error();
	}
	const std::optional<core::EbxInstance> locking = p_db.follow(weapon->data(), "CustomWeaponType");
	if (locking && locking->is_a("LockingWeaponData")) {
		const std::optional<bool> locked = bool_of(locking->data(), "FireOnlyWhenLockedOn");
		if (!locked) {
			return gadget_error(p_weapon, "LockingWeaponData has no FireOnlyWhenLockedOn");
		}
		g.fire_only_when_locked = *locked;
	}
	const std::optional<core::EbxStruct> shot = struct_of(fire->data(), "Shot");
	if (!shot) {
		return gadget_error(p_weapon, "FiringFunctionData has no Shot");
	}
	const std::optional<core::EbxInstance> projectile = p_db.follow(*shot, "ProjectileData");
	if (!projectile) {
		return g;
	}
	g.projectile_type = std::string(projectile->type_name());
	const core::EbxStruct p = projectile->data();
	if (projectile->is_a("SupplySphereEntityData")) {
		const std::optional<core::EbxStruct> supply = struct_of(p, "SupplyData");
		if (!supply) {
			return gadget_error(p_weapon, "SupplySphereEntityData has no SupplyData");
		}
		core::Result<SupplyUnitDef> healing = read_supply_unit(p_weapon, *supply, "Healing");
		if (!healing) {
			return healing.error();
		}
		core::Result<SupplyUnitDef> ammo = read_supply_unit(p_weapon, *supply, "Ammo");
		if (!ammo) {
			return ammo.error();
		}
		const std::optional<bool> team = bool_of(*supply, "TeamSpecific");
		const std::optional<bool> self = bool_of(*supply, "ExcludeSelf");
		const std::optional<bool> soldiers = bool_of(*supply, "SupplySoldiers");
		const std::optional<int32_t> count = i32_of(p, "MaxCount");
		const std::optional<float> incline = f32_of(p, "MaxAttachableInclination");
		const std::optional<float> health = f32_of(p, "Health");
		const std::optional<bool> destructible = bool_of(p, "IsDestructible");
		const std::optional<bool> explosion = bool_of(p, "ReceivesExplosionDamage");
		const std::optional<float> ttl = f32_of(p, "TimeToLive");
		const std::optional<float> ttl_gone = f32_of(p, "TimeToLiveOnPlayerDeath");
		if (!team || !self || !soldiers || !count || !incline || !health || !destructible || !explosion || !ttl ||
				!ttl_gone) {
			return gadget_error(p_weapon, "SupplySphereEntityData misses a field");
		}
		if (*count < 1 || *count > 255 || !finite_in(*incline, 0.0f, 90.0f) || !finite_in(*health, 0.0f, 1e6f) ||
				!finite_in(*ttl, 0.0f, 86400.0f) || !finite_in(*ttl_gone, 0.0f, 86400.0f)) {
			return gadget_error(p_weapon, "SupplySphereEntityData has a value out of range");
		}
		g.kind = GadgetKind::Supply;
		g.healing = *healing;
		g.ammo = *ammo;
		g.team_specific = *team;
		g.exclude_self = *self;
		g.supply_soldiers = *soldiers;
		g.max_count = uint8_t(*count);
		g.max_attach_inclination_deg = *incline;
		g.health = *health;
		g.destructible = *destructible;
		g.receives_explosion_damage = *explosion;
		g.time_to_live_s = *ttl;
		g.time_to_live_on_owner_gone_s = *ttl_gone;
		return g;
	}
	if (projectile->is_a("VehicleProjectileEntityData") && has_mobile_spawn(p_db, *projectile)) {
		core::Result<core::EbxInstance> data = weapon_data(p_db, p_weapon);
		if (!data) {
			return data.error();
		}
		const std::optional<core::EbxInstance> custom = p_db.follow(data->data(), "CustomWeaponType");
		if (!custom || !custom->is_a("VehicleDeployWeaponData")) {
			return gadget_error(p_weapon, "a vehicle projectile with no VehicleDeployWeaponData");
		}
		const std::optional<float> ray = f32_of(custom->data(), "DeployAreaGroundRayLength");
		const std::optional<float> flat = f32_of(custom->data(), "DeployAreaGroundFlatness");
		const std::optional<int32_t> count = i32_of(p, "MaxCount");
		if (!ray || !flat || !count) {
			return gadget_error(p_weapon, "the spawn beacon misses a field");
		}
		if (!finite_in(*ray, 0.0f, 100.0f) || !finite_in(*flat, 0.0f, 1.0f) || *count < 1 || *count > 255) {
			return gadget_error(p_weapon, "the spawn beacon has a value out of range");
		}
		g.kind = GadgetKind::SpawnBeacon;
		g.ground_ray_m = *ray;
		g.ground_flatness = *flat;
		g.max_count = uint8_t(*count);
		return g;
	}
	if (projectile->is_a("ExplosionPackEntityData")) {
		return read_explosion_pack(p_weapon, p, std::move(g));
	}
	return g;
}

Vec3 shot_frame_vector(const Vec3 &p_direction, const Vec3 &p_local) {
	const Vec3 forward = unit_or(p_direction, Vec3{ 0.0f, 0.0f, 1.0f });
	const Vec3 right = unit_or(core::vec::cross(Vec3{ 0.0f, 1.0f, 0.0f }, forward), Vec3{ 1.0f, 0.0f, 0.0f });
	const Vec3 up = core::vec::cross(forward, right);
	Vec3 out{};
	for (size_t i = 0; i < 3; ++i) {
		out[i] = right[i] * p_local[0] + up[i] * p_local[1] + forward[i] * p_local[2];
	}
	return out;
}

GadgetField::GadgetField(std::vector<GadgetDef> p_defs) : defs_(std::move(p_defs)) {}

const GadgetDef *GadgetField::gadget_of(uint16_t p_def) const {
	if (p_def == kNoWeapon) {
		return nullptr;
	}
	for (const GadgetDef &g : defs_) {
		if (g.def == p_def && g.kind != GadgetKind::None) {
			return &g;
		}
	}
	return nullptr;
}

const Deployable *GadgetField::find(uint16_t p_id) const {
	for (const Deployable &d : live_) {
		if (d.id == p_id) {
			return &d;
		}
	}
	return nullptr;
}

uint16_t GadgetField::next_id() {
	for (;;) {
		const uint16_t id = next_id_;
		next_id_ = uint16_t(next_id_ == 0xFFFEu ? 1u : next_id_ + 1u);
		if (find(id) == nullptr) {
			return id;
		}
	}
}

void GadgetField::erase_at(size_t p_index) {
	live_.erase(live_.begin() + std::ptrdiff_t(p_index));
	++stats_.removed;
	++revision_;
}

DeployOutcome GadgetField::deploy(const GadgetThrow &p_throw, const CollisionWorld *p_world, uint16_t *r_id) {
	const GadgetDef *g = gadget_of(p_throw.def);
	if (g == nullptr || !core::finite(p_throw.origin) || !core::finite(p_throw.direction)) {
		return DeployOutcome::NotGadget;
	}
	Deployable d;
	d.def = p_throw.def;
	d.gadget = uint8_t(g - defs_.data());
	d.owner = p_throw.owner;
	d.team = p_throw.team;
	d.deployed = p_throw.tick;
	d.position = p_throw.origin;
	d.healing_points = g->healing.capacity;
	d.ammo_points = g->ammo.capacity;
	d.health = g->health;
	if (g->kind == GadgetKind::SpawnBeacon) {
		StaticHit hit;
		if (p_world == nullptr ||
				!p_world->ray(d.position, Vec3{ 0.0f, -g->ground_ray_m, 0.0f }, query_mask::kProjectile, &hit)) {
			++stats_.no_ground;
			return DeployOutcome::NoGround;
		}
		if (hit.normal[1] < g->ground_flatness) {
			++stats_.steep;
			return DeployOutcome::Steep;
		}
		d.position = hit.position;
		d.resting = true;
	} else {
		d.velocity = shot_frame_vector(p_throw.direction, p_throw.launch_velocity);
		d.facing = unit_or(Vec3{ p_throw.direction[0], 0.0f, p_throw.direction[2] }, Vec3{ 0.0f, 0.0f, 1.0f });
	}
	if (live_.size() >= kMaxDeployables) {
		++stats_.full;
		return DeployOutcome::Full;
	}
	size_t owned = 0;
	for (const Deployable &o : live_) {
		owned += o.owner == d.owner && o.def == d.def && !o.owner_gone ? 1u : 0u;
	}
	for (size_t i = 0; owned >= g->max_count && i < live_.size();) {
		if (live_[i].owner == d.owner && live_[i].def == d.def && !live_[i].owner_gone) {
			erase_at(i);
			--owned;
			++stats_.replaced;
		} else {
			++i;
		}
	}
	d.id = next_id();
	live_.push_back(d);
	++revision_;
	if (r_id != nullptr) {
		*r_id = d.id;
	}
	if (d.resting) {
		++stats_.placed;
		return DeployOutcome::Placed;
	}
	++stats_.thrown;
	return DeployOutcome::Thrown;
}

void GadgetField::step(const CollisionWorld &p_world) {
	for (size_t i = 0; i < live_.size();) {
		Deployable &d = live_[i];
		if (d.resting) {
			++i;
			continue;
		}
		const GadgetDef &g = defs_[d.gadget];
		const float attach_cos = std::cos(g.max_attach_inclination_deg * std::numbers::pi_v<float> / 180.0f);
		const float dt = kTickSeconds;
		const Vec3 next{ d.position[0] + d.velocity[0] * dt,
			d.position[1] + d.velocity[1] * dt - 0.5f * kGadgetGravity * dt * dt, d.position[2] + d.velocity[2] * dt };
		const Vec3 delta{ next[0] - d.position[0], next[1] - d.position[1], next[2] - d.position[2] };
		d.velocity[1] -= kGadgetGravity * dt;
		StaticHit hit;
		if (p_world.ray(d.position, delta, query_mask::kProjectile, &hit)) {
			if (hit.normal[1] >= attach_cos) {
				d.position = hit.position;
				d.normal = hit.normal;
				d.velocity = {};
				d.resting = true;
				++stats_.landed;
				++revision_;
			} else {
				d.position = core::vec::add(hit.position, core::vec::scale(hit.normal, kGadgetSlideLift));
				d.velocity = core::vec::sub(
						d.velocity, core::vec::scale(hit.normal, core::vec::dot(d.velocity, hit.normal)));
				++stats_.slides;
			}
		} else {
			d.position = next;
		}
		d.flight = d.resting ? d.flight : uint16_t(std::min<uint32_t>(d.flight + 1u, 0xFFFFu));
		++i;
	}
	for (size_t i = 0; i < live_.size();) {
		const Deployable &d = live_[i];
		if (!d.resting && (!core::finite(d.position) || d.flight >= kGadgetFlightTicks)) {
			++stats_.lost;
			erase_at(i);
		} else {
			++i;
		}
	}
}

bool supplies(const GadgetDef &p_gadget, const Deployable &p_bag, const SupplyReceiver &p_receiver, bool p_healing) {
	const SupplyUnitDef &u = p_healing ? p_gadget.healing : p_gadget.ammo;
	const float points = p_healing ? p_bag.healing_points : p_bag.ammo_points;
	return p_gadget.kind == GadgetKind::Supply && p_bag.resting && p_gadget.supply_soldiers && u.active() &&
			!(p_gadget.team_specific && p_bag.team != p_receiver.team) &&
			!(p_gadget.exclude_self && !p_bag.owner_gone && p_bag.owner == p_receiver.slot) &&
			(u.infinite || points > 0.0f) &&
			core::vec::length(core::vec::sub(p_bag.position, p_receiver.position)) <= u.radius;
}

void GadgetField::supply(
		std::span<const SupplyReceiver> p_receivers, float p_seconds, std::span<SupplyGrant> r_grants) {
	const size_t n = std::min(p_receivers.size(), r_grants.size());
	for (size_t i = 0; i < n; ++i) {
		r_grants[i] = SupplyGrant();
	}
	if (!(p_seconds > 0.0f) || !std::isfinite(p_seconds)) {
		return;
	}
	for (Deployable &d : live_) {
		const GadgetDef &g = defs_[d.gadget];
		for (SupplyUnitDef const *u : { &g.healing, &g.ammo }) {
			float &points = u == &g.healing ? d.healing_points : d.ammo_points;
			if (!u->infinite && u->refill_speed > 0.0f) {
				points = std::min(u->capacity, points + u->refill_speed * p_seconds);
			}
		}
	}
	for (size_t i = 0; i < n; ++i) {
		const SupplyReceiver &r = p_receivers[i];
		if (!r.alive || !core::finite(r.position)) {
			continue;
		}
		for (const bool healing : { true, false }) {
			Deployable *best = nullptr;
			float best_rate = 0;
			for (Deployable &d : live_) {
				const GadgetDef &g = defs_[d.gadget];
				const SupplyUnitDef &u = healing ? g.healing : g.ammo;
				if (!supplies(g, d, r, healing)) {
					continue;
				}
				if (u.inc_speed > best_rate) {
					best_rate = u.inc_speed;
					best = &d;
				}
			}
			if (best == nullptr) {
				continue;
			}
			const GadgetDef &g = defs_[best->gadget];
			const SupplyUnitDef &u = healing ? g.healing : g.ammo;
			float amount = best_rate * p_seconds;
			if (!u.infinite) {
				float &points = healing ? best->healing_points : best->ammo_points;
				amount = std::min(amount, points);
				points -= amount;
			}
			(healing ? r_grants[i].health : r_grants[i].ammo) += amount;
		}
	}
}

size_t GadgetField::remove_owner(uint8_t p_owner) {
	size_t removed = 0;
	for (size_t i = 0; i < live_.size();) {
		if (live_[i].owner == p_owner && !live_[i].owner_gone) {
			erase_at(i);
			++removed;
		} else {
			++i;
		}
	}
	return removed;
}

size_t GadgetField::owner_gone(uint8_t p_owner, Tick p_tick) {
	size_t removed = 0;
	for (size_t i = 0; i < live_.size();) {
		Deployable &d = live_[i];
		if (d.owner != p_owner || d.owner_gone) {
			++i;
		} else if (defs_[d.gadget].kind == GadgetKind::SpawnBeacon) {
			erase_at(i);
			++removed;
		} else {
			d.owner_gone = true;
			d.owner_gone_tick = p_tick;
			++stats_.orphaned;
			++i;
		}
	}
	return removed;
}

size_t GadgetField::expire(Tick p_tick) {
	size_t expired = 0;
	for (size_t i = 0; i < live_.size();) {
		const Deployable &d = live_[i];
		const GadgetDef &g = defs_[d.gadget];
		const bool aged = g.time_to_live_s > 0.0f && float(p_tick - d.deployed) * kTickSeconds > g.time_to_live_s;
		const bool orphaned = d.owner_gone && g.time_to_live_on_owner_gone_s > 0.0f &&
				float(p_tick - d.owner_gone_tick) * kTickSeconds > g.time_to_live_on_owner_gone_s;
		if (p_tick >= d.deployed && (aged || orphaned)) {
			erase_at(i);
			++stats_.expired;
			++expired;
		} else {
			++i;
		}
	}
	return expired;
}

bool GadgetField::damage(uint16_t p_id, float p_amount, bool p_explosion) {
	for (size_t i = 0; i < live_.size(); ++i) {
		Deployable &d = live_[i];
		if (d.id != p_id) {
			continue;
		}
		const GadgetDef &g = defs_[d.gadget];
		if (!g.destructible || (p_explosion && !g.receives_explosion_damage) || !(p_amount > 0.0f) ||
				!std::isfinite(p_amount)) {
			return false;
		}
		if (d.detonating) {
			return false;
		}
		d.health -= p_amount;
		++stats_.damaged;
		if (d.health > 0.0f) {
			return false;
		}
		++stats_.destroyed;
		if (g.kind == GadgetKind::ExplosionPack) {
			d.detonating = true;
		} else {
			erase_at(i);
		}
		return true;
	}
	return false;
}

size_t GadgetField::blast(const ExplosionSimDef &p_explosion, BlastFalloff p_falloff, const Vec3 &p_at,
		const Vec3 &p_normal, const CollisionWorld *p_occluders, uint8_t p_spared_team) {
	if (!p_explosion.present || !core::finite(p_at)) {
		return 0;
	}
	const Vec3 eye = core::vec::add(p_at, core::vec::scale(p_normal, kBlastEyeLift));
	std::vector<std::pair<uint16_t, float>> hits;
	for (const Deployable &d : live_) {
		if (p_spared_team != 0 && d.team == p_spared_team) {
			continue;
		}
		const float amount =
				float(blast_damage(p_explosion, core::vec::length(core::vec::sub(d.position, p_at)), p_falloff));
		if (!(amount > 0.0f)) {
			continue;
		}
		const Vec3 centre = core::vec::add(d.position, Vec3{ 0.0f, kGadgetSlideLift, 0.0f });
		if (p_occluders != nullptr && !p_explosion.disable_occlusion &&
				p_occluders->ray(eye, core::vec::sub(centre, eye), query_mask::kCollision, nullptr)) {
			continue;
		}
		hits.emplace_back(d.id, amount);
	}
	size_t destroyed = 0;
	for (const auto &[id, amount] : hits) {
		destroyed += damage(id, amount, true) ? 1u : 0u;
	}
	return destroyed;
}

bool soldier_trips(const ExplosionPackDef &p_pack, const Deployable &p_mine, const PackSoldier &p_soldier) {
	if (p_soldier.team == p_mine.team || !(p_soldier.speed > p_pack.soldier_min_speed)) {
		return false;
	}
	const Vec3 offset = core::vec::sub(p_soldier.position, p_mine.position);
	const float distance = core::vec::length(offset);
	if (!(distance <= p_pack.soldier_radius)) {
		return false;
	}
	if (!p_pack.use_angle || distance < kPackCloseTrigger) {
		return true;
	}
	const Vec3 axis = unit_or(Vec3{ p_mine.facing[0], 0.0f, p_mine.facing[2] }, Vec3{ 0.0f, 0.0f, 1.0f });
	const float cosine = std::clamp(core::vec::dot(core::vec::scale(offset, 1.0f / distance), axis), -1.0f, 1.0f);
	return std::acos(cosine) < p_pack.soldier_angle_deg * std::numbers::pi_v<float> / 180.0f;
}

bool vehicle_trips(const ExplosionPackDef &p_pack, const Deployable &p_mine, const PackVehicle &p_vehicle) {
	if (p_vehicle.driver_team != 0 && p_vehicle.driver_team == p_mine.team) {
		return false;
	}
	if (!(core::vec::length(core::vec::sub(p_mine.position, p_vehicle.centre)) <= p_pack.vehicle_radius)) {
		return false;
	}
	const float speed = core::vec::length(p_vehicle.velocity);
	if (!(speed > p_vehicle.min_speed_for_mine)) {
		return false;
	}
	const Vec3 toward = unit_or(core::vec::sub(p_mine.position, p_vehicle.position), Vec3{});
	return core::vec::dot(core::vec::scale(p_vehicle.velocity, 1.0f / speed), toward) > 0.0f;
}

void GadgetField::arm_packs(
		std::span<const PackSoldier> p_soldiers, std::span<const PackVehicle> p_vehicles, float p_dt) {
	for (Deployable &d : live_) {
		const GadgetDef &g = defs_[d.gadget];
		if (g.kind != GadgetKind::ExplosionPack || !d.resting || d.detonating) {
			continue;
		}
		d.armed_s += p_dt;
		d.vehicle_check_s += p_dt;
		d.soldier_check_s += p_dt;
		const ExplosionPackDef &k = g.pack;
		if (double(d.vehicle_check_s) > kPackCheckInterval) {
			d.vehicle_check_s = 0.0f;
			if (k.vehicle_detonation && k.vehicle_delay_s < d.armed_s && k.vehicle_radius > 0.0f) {
				for (const PackVehicle &v : p_vehicles) {
					if (vehicle_trips(k, d, v)) {
						d.detonating = true;
						++stats_.vehicle_trips;
						break;
					}
				}
			}
		}
		if (double(d.soldier_check_s) > kPackCheckInterval) {
			d.soldier_check_s = 0.0f;
			if (!d.detonating && k.soldier_detonation && k.soldier_delay_s < d.armed_s && k.soldier_radius > 0.0f) {
				for (const PackSoldier &s : p_soldiers) {
					if (soldier_trips(k, d, s)) {
						d.detonating = true;
						++stats_.soldier_trips;
						break;
					}
				}
			}
		}
	}
}

size_t GadgetField::remote_detonate(uint8_t p_owner, uint16_t p_def) {
	size_t n = 0;
	for (Deployable &d : live_) {
		const GadgetDef &g = defs_[d.gadget];
		if (g.kind == GadgetKind::ExplosionPack && g.pack.remote_detonator && d.def == p_def && d.owner == p_owner &&
				!d.owner_gone && !d.detonating) {
			d.detonating = true;
			++n;
		}
	}
	stats_.remote += n;
	return n;
}

std::vector<Deployable> GadgetField::take_detonating() {
	std::vector<Deployable> out;
	for (size_t i = 0; i < live_.size();) {
		if (live_[i].detonating) {
			out.push_back(live_[i]);
			erase_at(i);
			++stats_.detonated;
		} else {
			++i;
		}
	}
	return out;
}

bool GadgetField::remove(uint16_t p_id) {
	for (size_t i = 0; i < live_.size(); ++i) {
		if (live_[i].id == p_id) {
			erase_at(i);
			return true;
		}
	}
	return false;
}

void GadgetField::clear() {
	if (!live_.empty()) {
		stats_.removed += live_.size();
		live_.clear();
		++revision_;
	}
}

void arm_ammo_refill(AmmoRefillState &r_state, const SoldierWeaponsState &p_weapons) {
	r_state = AmmoRefillState();
	for (size_t s = 0; s < kMaxWeaponSlots; ++s) {
		r_state.caps[s] = p_weapons.slots[s].def != kNoWeapon ? p_weapons.slots[s].reserve : 0;
	}
}

int32_t refill_ammo(SoldierWeaponsState &r_weapons, const WeaponTable &p_table,
		std::span<const AmmoPickupDef> p_pickups, float p_points, AmmoRefillState &r_state) {
	int32_t added = 0;
	for (size_t s = 0; s < kMaxWeaponSlots; ++s) {
		WeaponSlotState &x = r_weapons.slots[s];
		const int32_t cap = r_state.caps[s];
		if (x.def == kNoWeapon || x.def >= p_table.defs.size() || x.def >= p_pickups.size() || x.reserve >= cap) {
			r_state.progress[s] = 0;
			continue;
		}
		const WeaponSimDef &d = p_table.def(x.def, x.variant);
		const AmmoPickupDef &pick = p_pickups[x.def];
		if (d.unlimited_ammo || !(pick.delay_multiplier > 0.0f) || !(p_points > 0.0f)) {
			continue;
		}
		const int32_t amount = pick.amount < 0 ? d.magazine_capacity : pick.amount;
		if (amount <= 0) {
			continue;
		}
		r_state.progress[s] += p_points;
		while (r_state.progress[s] >= pick.delay_multiplier && x.reserve < cap) {
			const int32_t take = std::min(amount, cap - x.reserve);
			x.reserve += take;
			added += take;
			r_state.progress[s] -= pick.delay_multiplier;
		}
		if (x.reserve >= cap) {
			r_state.progress[s] = 0;
		}
	}
	return added;
}

int32_t replenish_ammo(SoldierWeaponsState &r_weapons, const WeaponTable &p_table,
		std::span<const AmmoPickupDef> p_pickups, Tick p_tick, AmmoRefillState &r_state) {
	int32_t added = 0;
	for (size_t s = 0; s < kMaxWeaponSlots; ++s) {
		WeaponSlotState &x = r_weapons.slots[s];
		const int32_t cap = r_state.caps[s];
		if (x.def == kNoWeapon || x.def >= p_table.defs.size() || x.def >= p_pickups.size() ||
				!p_pickups[x.def].auto_replenish || x.reserve >= cap) {
			r_state.low_since[s] = 0;
			continue;
		}
		if (r_state.low_since[s] == 0) {
			r_state.low_since[s] = std::max<Tick>(p_tick, 1u);
			continue;
		}
		if (p_tick - r_state.low_since[s] >= seconds_to_ticks(p_pickups[x.def].auto_replenish_delay)) {
			added += cap - x.reserve;
			x.reserve = cap;
			r_state.low_since[s] = 0;
		}
	}
	return added;
}

}
