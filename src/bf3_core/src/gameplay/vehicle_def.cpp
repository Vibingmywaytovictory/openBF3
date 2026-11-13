#include "bf3/core/gameplay/vehicle_def.h"

#include "bf3/core/ids.h"
#include "bf3/core/level/level_transform.h"
#include "ebx_fields.h"
#include "gameplay_read.h"
#include "weapon_readers.h"

#include <algorithm>
#include <limits>

namespace bf3::core::gameplay {

using namespace detail;

namespace {

constexpr size_t kNodePasses = 8;

enum class Role : uint8_t { Other, Chassis, Seat, Turret, Weapon, Camera, ExitPoint, Mesh };

struct Node {
	EbxInstance data;
	int parent = -1;
	Role role = Role::Other;
	level::LevelTransform frame;
	level::LevelTransform turret_frame;
};

core::Transform model_transform(const level::LevelTransform &p_t) {
	return { p_t.right, p_t.up, p_t.forward, p_t.trans };
}

Role role_of(const EbxResolver &p_db, const EbxInstance &p_component) {
	if (p_component.get("VehicleConfig")) {
		return Role::Chassis;
	}
	if (p_component.is_a("EntryComponentData")) {
		return Role::Seat;
	}
	if (p_component.is_a("ChildComponentData")) {
		const std::optional<EbxInstance> body = p_db.follow(p_component.data(), "MovingBody");
		if (body && body->is_a("ChildRotationBodyData")) {
			return Role::Turret;
		}
	}
	if (p_component.is_a("WeaponComponentData")) {
		return Role::Weapon;
	}
	if (p_component.is_a("CameraComponentData")) {
		return Role::Camera;
	}
	if (p_component.is_a("VehicleExitPointComponentData")) {
		return Role::ExitPoint;
	}
	if (p_component.is_a("MeshComponentData")) {
		return Role::Mesh;
	}
	return Role::Other;
}

class Layout {
public:
	std::vector<Node> nodes;
	bool bad_transform = false;

	void walk(WorkBudget &r_budget, const EbxResolver &p_db, const EbxInstance &p_container, int p_parent,
			size_t p_depth, const level::LevelTransform &p_frame = level::LevelTransform::identity()) {
		if (p_depth > kMaxVehicleComponentDepth) {
			return;
		}
		for (const EbxInstance &component : follow_all(r_budget, p_db, p_container.data(), "Components")) {
			if (nodes.size() >= kMaxVehicleComponents) {
				return;
			}
			if (!active_component(component)) {
				continue;
			}
			const int at = static_cast<int>(nodes.size());
			const level::LevelTransform parent =
					p_parent >= 0 ? nodes[static_cast<size_t>(p_parent)].frame : level::LevelTransform::identity();
			const Result<level::LevelTransform> local = component_transform(component);
			bad_transform = bad_transform || !local;
			const level::LevelTransform here = local ? *local : level::LevelTransform::identity();
			const Role role = role_of(p_db, component);
			nodes.push_back({ component, p_parent, role, here.in_parent(parent), here.in_parent(p_frame) });
			walk(r_budget, p_db, component, at, p_depth + 1,
					role == Role::Turret ? level::LevelTransform::identity() : nodes.back().turret_frame);
		}
	}

	void index_children() {
		first_seat_.assign(nodes.size() + 1, -1);
		first_turret_.assign(nodes.size() + 1, -1);
		for (size_t i = 0; i < nodes.size(); ++i) {
			const size_t slot = static_cast<size_t>(nodes[i].parent + 1);
			if (nodes[i].role == Role::Seat && first_seat_[slot] < 0) {
				first_seat_[slot] = static_cast<int>(i);
			} else if (nodes[i].role == Role::Turret && first_turret_[slot] < 0) {
				first_turret_[slot] = static_cast<int>(i);
			}
		}
	}

	int enclosing(int p_at, Role p_role) const {
		for (int up = nodes[p_at].parent; up >= 0; up = nodes[up].parent) {
			if (nodes[up].role == p_role) {
				return up;
			}
		}
		return -1;
	}

	int innermost(int p_at) const {
		int current = p_at;
		for (size_t guard = 0; guard <= kMaxVehicleComponentDepth + 1; ++guard) {
			const int child = first_turret_[static_cast<size_t>(current + 1)];
			if (child < 0) {
				break;
			}
			current = child;
		}
		return current;
	}

	int aiming(int p_at) const {
		const int up = enclosing(p_at, Role::Turret);
		if (up >= 0) {
			return up;
		}
		const int beside = first_turret_[static_cast<size_t>(nodes[p_at].parent + 1)];
		return beside >= 0 ? innermost(beside) : -1;
	}

	int seat_of(int p_at) const {
		int current = p_at;
		for (size_t guard = 0; current >= 0 && guard <= kMaxVehicleComponentDepth + 1; ++guard) {
			if (nodes[current].role == Role::Seat) {
				return current;
			}
			const int parent = nodes[current].parent;
			if (const int beside = first_seat_[static_cast<size_t>(parent + 1)]; beside >= 0) {
				return beside;
			}
			current = parent;
		}
		return -1;
	}

	int camera_seat(int p_at) const {
		if (const int seat = enclosing(p_at, Role::Seat); seat >= 0) {
			return seat;
		}
		const int turret = enclosing(p_at, Role::Turret);
		return turret >= 0 ? seat_of(turret) : -1;
	}

private:
	std::vector<int> first_seat_;
	std::vector<int> first_turret_;
};

std::vector<int> valid_stances(WorkBudget &r_budget, const Layout &p_layout, int p_at) {
	std::vector<int> out;
	for (int up = p_layout.nodes[static_cast<size_t>(p_at)].parent; up >= 0;
			up = p_layout.nodes[static_cast<size_t>(up)].parent) {
		const EbxInstance &d = p_layout.nodes[static_cast<size_t>(up)].data;
		if (!d.is_a("StanceFilterComponentData")) {
			continue;
		}
		if (const OptionalEbxArray stances = array_of(r_budget, d.data().get("ValidStances"))) {
			for (const EbxValue v : *stances) {
				if (const std::optional<double> n = number(v)) {
					out.push_back(static_cast<int>(*n));
				}
			}
		}
		break;
	}
	return out;
}

VehicleHealthZoneDef read_zone(const EbxStruct &p_entity, std::string_view p_field) {
	VehicleHealthZoneDef out;
	const std::optional<EbxValue> v = p_entity.get(p_field);
	const std::optional<EbxStruct> z = v ? v->as_struct() : std::nullopt;
	if (!z) {
		return out;
	}
	out.max_health = static_cast<float>(number_or(*z, "MaxHealth", 0));
	out.max_shield_health = static_cast<float>(number_or(*z, "MaxShieldHealth", 0));
	out.min_damage_angle = static_cast<float>(number_or(*z, "MinDamageAngle", 0));
	out.damage_angle_multiplier = static_cast<float>(number_or(*z, "DamageAngleMultiplier", 1));
	out.use_damage_angle = boolean(*z, "UseDamageAngleCalculation").value_or(false);
	return out;
}

Result<VehicleCameraDef> read_camera(WorkBudget &r_budget, const EbxResolver &p_db, const EbxInstance &p_component) {
	VehicleCameraDef out;
	const EbxStruct c = p_component.data();
	out.field_of_view = static_cast<float>(number_or(c, "FieldOfView", 0));
	out.first_person = boolean(c, "IsFirstPerson").value_or(false);
	if (const std::optional<EbxInstance> camera = p_db.follow(c, "Camera"); camera && r_budget.take_type(camera->data())) {
		out.camera_type = copy_text(r_budget, camera->type_name());
		out.camera = partition_name(r_budget, p_db, *camera);
		if (camera->is_a("StaticCameraData")) {
			const EbxStruct s = camera->data();
			out.left_yaw = static_cast<float>(number_or(s, "LeftYawAngle", 0));
			out.right_yaw = static_cast<float>(number_or(s, "RightYawAngle", 0));
			out.up_pitch = static_cast<float>(number_or(s, "UpPitchAngle", 0));
			out.down_pitch = static_cast<float>(number_or(s, "DownPitchAngle", 0));
		} else if (camera->is_a("ChaseCameraData")) {
			Result<ChaseCameraDef> chase = read_chase_camera(p_db, camera->data());
			if (!chase) {
				return chase.error();
			}
			out.chase = *chase;
		}
	}
	if (const std::optional<EbxInstance> alternate = p_db.follow(c, "AlternateView"); alternate && r_budget.take_type(alternate->data())) {
		if (const std::optional<double> fov = number(alternate->data(), "FieldOfView")) {
			out.zoom_field_of_view = static_cast<float>(*fov);
		}
		out.zoom_fov_transition_time = static_cast<float>(number_or(alternate->data(), "FovTransitionTime", 0));
		out.zoom_fade_to_black = boolean(alternate->data(), "FadeToBlack").value_or(false);
		out.zoom_fade_out = static_cast<float>(number_or(alternate->data(), "FadeOutDuration", 0));
		out.zoom_black = static_cast<float>(number_or(alternate->data(), "BlackDuration", 0));
		out.zoom_fade_in = static_cast<float>(number_or(alternate->data(), "FadeInDuration", 0));
	}
	return out;
}
}

float VehicleDef::max_zone_health() const {
	return std::max({ front.max_health, rear.max_health, left.max_health, right.max_health });
}

const VehicleSeatDef *VehicleDef::driver() const {
	for (const VehicleSeatDef &s : seats) {
		if (s.seat_type == "EST_Driver") {
			return &s;
		}
	}
	return nullptr;
}

Result<VehicleDef> read_vehicle_def(const EbxResolver &p_db, std::string_view p_blueprint, WorkBudget *p_budget) {
	WorkBudget own;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : own;
	const std::string name = to_lower_ascii(p_blueprint);
	const EbxPartition *partition = p_db.partition_by_name(name);
	if (!partition) {
		return make_error("vehicle: no partition `" + name + "`");
	}
	const EbxInstance blueprint = partition->primary();
	BF3_TRY(budget.charge_type(blueprint.data()));
	const std::optional<EbxInstance> entity = p_db.follow(blueprint.data(), "Object");
	if (!entity || !entity->is_a("VehicleEntityData")) {
		return make_error("vehicle: `" + name + "` has no VehicleEntityData (primary " + std::string(blueprint.type_name()) + ")");
	}
	BF3_TRY(budget.charge_type(entity->data()));
	VehicleDef out;
	out.blueprint = name;
	const EbxStruct e = entity->data();
	out.name_sid = text(budget, e, "NameSid");
	out.minimap_icon = text(budget, e, "HudData.MinimapIcon");
	out.controllable_type = text(budget, e, "ControllableType");
	if (const std::optional<EbxInstance> mesh = p_db.follow(e, "Mesh"); mesh && budget.take_type(mesh->data())) {
		out.mesh = partition_name(budget, p_db, *mesh);
	}
	out.front = read_zone(e, "FrontHealthZone");
	out.rear = read_zone(e, "RearHealthZone");
	out.left = read_zone(e, "LeftHealthZone");
	out.right = read_zone(e, "RightHealthZone");
	out.health_zones_share_damage = boolean(e, "HealthZonesShareDamage").value_or(false);
	out.disabled_damage_threshold = static_cast<float>(number_or(e, "DisabledDamageThreshold", 0));
	out.pre_destruction_damage_threshold = static_cast<float>(number_or(e, "PreDestructionDamageThreshold", 0));
	out.emergency_repair_health = static_cast<float>(number_or(e, "EmergencyRepairHealth", 0));
	out.regeneration_delay = static_cast<float>(number_or(e, "RegenerationDelay", 0));
	out.regeneration_rate = static_cast<float>(number_or(e, "RegenerationRate", 0));
	out.regeneration_delay_multiplier = static_cast<float>(number_or(e, "RegenerationDelayMultiplier", 1));
	out.regeneration_rate_multiplier = static_cast<float>(number_or(e, "RegenerationRateMultiplier", 1));
	out.critical_damage_time = static_cast<float>(number_or(e, "CriticalDamageTime", 0));
	out.pre_explosion_time = static_cast<float>(number_or(e, "PreExplosionTime", 0));
	out.angle_of_impact.enabled = boolean(e, "AngleOfImpact.Enabled").value_or(false);
	out.angle_of_impact.zone12_delimiter = static_cast<float>(number_or(e, "AngleOfImpact.Zone12Delimiter", 0));
	out.angle_of_impact.zone23_delimiter = static_cast<float>(number_or(e, "AngleOfImpact.Zone23Delimiter", 0));
	out.angle_of_impact.multiplier = { static_cast<float>(number_or(e, "AngleOfImpact.Zone1Multiplier", 1)),
		static_cast<float>(number_or(e, "AngleOfImpact.Zone2Multiplier", 1)),
		static_cast<float>(number_or(e, "AngleOfImpact.Zone3Multiplier", 1)) };
	if (e.get("MaterialPair")) {
		Fields fields(p_db);
		out.material = fields.material_pair(e, "MaterialPair");
	}
	out.decay_delay = static_cast<float>(number_or(e, "DecayDelay", 0));
	out.decay_rate = static_cast<float>(number_or(e, "DecayRate", 0));
	out.armor_multiplier = static_cast<float>(number_or(e, "ArmorMultiplier", 1));
	out.low_health_threshold = static_cast<float>(number_or(e, "LowHealthThreshold", 0));
	out.velocity_damage_threshold = static_cast<float>(number_or(e, "VelocityDamageThreshold", 0));
	out.velocity_damage_magnifier = static_cast<float>(number_or(e, "VelocityDamageMagnifier", 0));
	out.water_damage = static_cast<float>(number_or(e, "WaterDamage", 0));
	out.upside_down_angle = static_cast<float>(number_or(e, "UpsideDownAngle", 0));
	out.upside_down_damage = static_cast<float>(number_or(e, "UpsideDownDamage", 0));
	out.upside_down_damage_delay = static_cast<float>(number_or(e, "UpsideDownDamageDelay", 0));
	out.kill_soldier_collision_speed_threshold = static_cast<float>(number_or(e, "KillSoldierCollisionSpeedThreshold", 0));
	out.ignore_soldier_collision_normal = boolean(e, "IgnoreSoldierCollisionNormal").value_or(false);
	out.min_speed_for_mine_activation = static_cast<float>(number_or(e, "MinSpeedForMineActivation", 0));
	out.use_prediction = boolean(e, "UsePrediction").value_or(false);
	Result<std::vector<VehicleInputModifierDef>> disabled = read_disabled_input(p_db, blueprint, *entity, budget);
	if (!disabled) {
		return make_error("vehicle " + name + ": disabled input: " + disabled.error().message);
	}
	out.disabled_input = std::move(*disabled);
	Result<VehicleDamageVisualsDef> visuals = read_vehicle_damage_visuals(p_db, blueprint, *entity, budget);
	if (!visuals) {
		return make_error("vehicle " + name + ": damage visuals: " + visuals.error().message);
	}
	out.damage_visuals = std::move(*visuals);
	if (const std::optional<EbxInstance> x = p_db.follow(e, "Explosion");
			x && x->is_a("ExplosionEntityData") && budget.take_type(x->data())) {
		Fields fields(p_db);
		read_explosion(fields, *x, out.explosion);
		if (!fields.ok()) {
			return make_error("vehicle " + name + ": Explosion: " + fields.error().message);
		}
		if (const std::optional<EbxInstance> fx = p_db.follow(x->data(), "DetonationEffect");
				fx && budget.take_type(fx->data())) {
			out.explosion_effect = partition_name(budget, p_db, *fx);
		}
	}

	Layout layout;
	layout.walk(budget, p_db, *entity, -1, 0);
	if (layout.bad_transform) {
		return make_error("vehicle " + name + ": non-finite or out-of-range component Transform");
	}
	out.components = layout.nodes.size();

	BF3_TRY(budget.charge(kNodePasses * layout.nodes.size()));
	layout.index_children();

	for (const Node &node : layout.nodes) {
		if (node.role != Role::Chassis) {
			continue;
		}
		if (const std::optional<EbxInstance> config = p_db.follow(node.data.data(), "VehicleConfig"); config && budget.take_type(config->data())) {
			const EbxStruct c = config->data();
			out.body_mass = static_cast<float>(number_or(c, "BodyMass", 0));
			out.gravity_modifier = static_cast<float>(number_or(c, "GravityModifier", 1));
			out.center_of_mass = vec3(c, "CenterOfMass").value_or(core::Vec3{});
			out.inertia_modifier = vec3(c, "InertiaModifier").value_or(core::Vec3{ 1, 1, 1 });
			break;
		}
	}

	std::vector<int> seat_nodes;
	std::vector<double> order(layout.nodes.size(), 0);
	for (size_t i = 0; i < layout.nodes.size(); ++i) {
		if (layout.nodes[i].role == Role::Seat) {
			seat_nodes.push_back(static_cast<int>(i));
			const std::optional<double> n = number(layout.nodes[i].data.data(), "EntryOrderNumber");
			order[i] = n ? *n : std::numeric_limits<double>::max();
		}
	}
	std::stable_sort(seat_nodes.begin(), seat_nodes.end(), [&](int a, int b) { return order[static_cast<size_t>(a)] < order[static_cast<size_t>(b)]; });
	std::vector<int> seat_at(layout.nodes.size(), -1);
	for (size_t i = 0; i < seat_nodes.size(); ++i) {
		seat_at[static_cast<size_t>(seat_nodes[i])] = static_cast<int>(i);
	}
	const auto seat_index = [&](int p_node) { return p_node < 0 ? -1 : seat_at[static_cast<size_t>(p_node)]; };
	for (const int node : seat_nodes) {
		const EbxInstance &d = layout.nodes[node].data;
		VehicleSeatDef seat;
		seat.entry_order = static_cast<int>(number_or(d.data(), "EntryOrderNumber", 0));
		seat.seat_type = text(budget, d.data(), "HudData.SeatType");
		seat.component_type = copy_text(budget, d.type_name());
		seat.entry_radius = static_cast<float>(number_or(d.data(), "EntryRadius", 0));
		seat.shows_soldier = boolean(d.data(), "ShowSoldierInEntry").value_or(false);
		seat.own_weapon = d.is_a("SoldierEntryComponentData");
		seat.transform = model_transform(layout.nodes[static_cast<size_t>(node)].frame);
		seat.stance_count = static_cast<int>(number_or(d.data(), "NumberOfStances", 1));
		out.seats.push_back(std::move(seat));
	}

	std::vector<int> turret_nodes;
	std::vector<int> turret_at(layout.nodes.size(), -1);
	for (size_t i = 0; i < layout.nodes.size(); ++i) {
		if (layout.nodes[i].role == Role::Turret) {
			turret_at[i] = static_cast<int>(turret_nodes.size());
			turret_nodes.push_back(static_cast<int>(i));
		}
	}
	const auto turret_index = [&](int p_node) { return p_node < 0 ? -1 : turret_at[static_cast<size_t>(p_node)]; };
	for (const int node : turret_nodes) {
		VehicleTurretDef t;
		t.parent = turret_index(layout.enclosing(node, Role::Turret));
		t.seat = seat_index(layout.seat_of(node));
		if (const std::optional<EbxInstance> body = p_db.follow(layout.nodes[node].data.data(), "MovingBody"); body && budget.take_type(body->data())) {
			const EbxStruct b = body->data();
			t.rotation_axis = static_cast<int>(number_or(b, "RotationAxis", 1));
			t.rotation_input = text(budget, b, "RotationInput");
			t.constrained = boolean(b, "UseAngularConstraint").value_or(false);
			t.min_angle = static_cast<float>(number_or(b, "AngularConstraintMin", 0));
			t.max_angle = static_cast<float>(number_or(b, "AngularConstraintMax", 0));
			t.world_space_lock_efficiency = static_cast<float>(number_or(b, "WorldSpaceLockEfficiency", 0));
			t.angular_momentum_multiplier = static_cast<float>(number_or(b, "AngularMomentumMultiplier", 0));
			t.angular_momentum_threshold = static_cast<float>(number_or(b, "AngularMomentumThreshold", 0));
			t.angular_momentum_damping = static_cast<float>(number_or(b, "AngularMomentumDamping", 0));
			t.mass = static_cast<float>(number_or(b, "Mass", 0));
			t.inertia = static_cast<float>(number_or(b, "Inertia", 0));
			t.force_modifier = static_cast<float>(number_or(b, "ForceModifier", 0));
			t.reset_force_modifier = static_cast<float>(number_or(b, "ResetForceModifier", 0));
			t.reset_force_input_threshold = static_cast<float>(number_or(b, "ResetForceInputThreshold", 0));
			t.damping_acc = static_cast<float>(number_or(b, "AngularMomentumDampingAcc", 1));
			t.damping_deacc = static_cast<float>(number_or(b, "AngularMomentumDampingDeacc", 1));
			t.use_linear_input = boolean(b, "UseLinearInput").value_or(false);
			t.use_threshold = boolean(b, "UseAngularMomentumThreshold").value_or(false);
			t.use_damping = boolean(b, "UseAngularMomentumDamping").value_or(false);
			t.post_satisfy = boolean(b, "UsePostSatisfyConstraints").value_or(false);
			if (const std::optional<EbxInstance> ext = p_db.follow(b, "ExtendedConstraints");
					ext && budget.take_type(ext->data())) {
				const EbxStruct x = ext->data();
				t.extended = true;
				t.extended_heading = static_cast<float>(number_or(x, "Heading", 0));
				t.extended_width = static_cast<float>(number_or(x, "Width", 0));
				t.extended_falloff = static_cast<float>(number_or(x, "Falloff", 0));
				t.extended_min_angle = static_cast<float>(number_or(x, "AngularConstraintMin", 0));
				t.extended_max_angle = static_cast<float>(number_or(x, "AngularConstraintMax", 0));
			}
		}
		const EbxStruct component = layout.nodes[static_cast<size_t>(node)].data.data();
		if (const std::optional<EbxInstance> align = p_db.follow(component, "AlignmentSettings");
				align && budget.take_type(align->data())) {
			t.align_action = text(budget, align->data(), "AlignAction");
			t.align_speed = static_cast<float>(number_or(align->data(), "Speed", 0));
			t.align_stop_tolerance = static_cast<float>(number_or(align->data(), "StopTolerance", 0));
		}
		t.frame_turret = t.parent;
		t.frame = model_transform(layout.nodes[static_cast<size_t>(node)].turret_frame);
		t.sound = ref_name(budget, p_db, component, "SoundEffect");
		out.turrets.push_back(std::move(t));
	}

	for (size_t i = 0; i < layout.nodes.size(); ++i) {
		const Node &node = layout.nodes[i];
		const int at = static_cast<int>(i);
		if (node.role == Role::Weapon) {
			VehicleWeaponDef w;
			const EbxStruct d = node.data.data();
			w.damage_giver_name = text(budget, d, "DamageGiverName");
			w.firing = ref_name(budget, p_db, d, "WeaponFiring");
			w.classification = text(budget, d, "Classification");
			w.seat = seat_index(layout.seat_of(at));
			w.turret = turret_index(layout.aiming(at));
			w.damage_multiplier = static_cast<float>(number_or(d, "DamageMultiplier", 1));
			w.explosion_damage_multiplier = static_cast<float>(number_or(d, "ExplosionDamageMultiplier", 1));
			w.frame_turret = turret_index(layout.enclosing(at, Role::Turret));
			w.frame = model_transform(node.turret_frame);
			w.projectile_spawn_offset = vec3(d, "ProjectileSpawnOffset").value_or(core::Vec3{});
			w.impulse_strength = static_cast<float>(number_or(d, "ImpulseStrength", 0));
			w.reload_time_multiplier = static_cast<float>(number_or(d, "ReloadTimeMultiplier", 1));
			w.overheat_drop_multiplier = static_cast<float>(number_or(d, "OverheatDropPerSecondMultiplier", 1));
			w.stances = valid_stances(budget, layout, at);
			if (w.seat >= 0) {
				out.seats[static_cast<size_t>(w.seat)].weapons.push_back(static_cast<int>(out.weapons.size()));
			}
			out.weapons.push_back(std::move(w));
		} else if (node.role == Role::ExitPoint) {
			out.exit_points.push_back(model_transform(node.frame));
		} else if (node.role == Role::Mesh) {
			if (const std::string mesh = ref_name(budget, p_db, node.data.data(), "Mesh"); !mesh.empty()) {
				out.component_meshes.emplace_back(mesh, model_transform(node.frame));
			}
		} else if (node.role == Role::Camera) {
			const int seat = seat_index(layout.camera_seat(at));
			if (seat >= 0) {
				Result<VehicleCameraDef> camera = read_camera(budget, p_db, node.data);
				if (!camera) {
					return error_at("vehicle " + name, camera.error());
				}
				camera->transform = model_transform(node.frame);
				camera->turret = turret_index(layout.enclosing(at, Role::Turret));
				out.seats[static_cast<size_t>(seat)].cameras.push_back(std::move(*camera));
			}
		}
	}
	if (const Status spent = budget.status(); !spent) {
		return error_at("vehicle " + name, spent.error());
	}
	return out;
}

}
