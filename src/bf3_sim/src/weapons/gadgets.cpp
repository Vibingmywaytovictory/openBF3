#include "bf3/sim/weapons/gadgets.h"

#include "bf3/core/ebx.h"
#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

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

}

const char *gadget_kind_name(GadgetKind p_kind) {
	switch (p_kind) {
		case GadgetKind::None:
			return "none";
		case GadgetKind::Supply:
			return "supply";
		case GadgetKind::SpawnBeacon:
			return "spawn beacon";
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
		if (!team || !self || !soldiers || !count || !incline) {
			return gadget_error(p_weapon, "SupplySphereEntityData misses a field");
		}
		if (*count < 1 || *count > 255 || !finite_in(*incline, 0.0f, 90.0f)) {
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
	}
	if (live_.size() >= kMaxDeployables) {
		++stats_.full;
		return DeployOutcome::Full;
	}
	size_t owned = 0;
	for (const Deployable &o : live_) {
		owned += o.owner == d.owner && o.def == d.def ? 1u : 0u;
	}
	for (size_t i = 0; owned >= g->max_count && i < live_.size();) {
		if (live_[i].owner == d.owner && live_[i].def == d.def) {
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
				const float points = healing ? d.healing_points : d.ammo_points;
				if (g.kind != GadgetKind::Supply || !d.resting || !g.supply_soldiers || !u.active() ||
						(g.team_specific && d.team != r.team) || (g.exclude_self && d.owner == r.slot) ||
						(!u.infinite && points <= 0.0f) ||
						core::vec::length(core::vec::sub(d.position, r.position)) > u.radius) {
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
		if (live_[i].owner == p_owner) {
			erase_at(i);
			++removed;
		} else {
			++i;
		}
	}
	return removed;
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
