#include "game_vehicles.h"

#include "render_bridge.h"

#include "bf3/host/level_load.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <span>

using namespace godot;

namespace bf3::godot_side {

namespace {

Vector3 to_vector(const core::Vec3 &p_v) {
	return Vector3(p_v[0], p_v[1], p_v[2]);
}

Transform3D godot_transform(const core::Transform &p_t) {
	return Transform3D(Basis(to_vector(p_t.right), to_vector(p_t.up), to_vector(p_t.forward)), to_vector(p_t.trans));
}

void free_node(uint64_t p_id) {
	if (Node *node = Object::cast_to<Node>(ObjectDB::get_instance(p_id))) {
		node->queue_free();
	}
}

}

Transform3D vehicle_body_transform(const sim::VehicleModel &p_model, const host::PresentedVehiclePose &p_pose) {
	const Basis basis(
			Quaternion(p_pose.rotation[0], p_pose.rotation[1], p_pose.rotation[2], p_pose.rotation[3]).normalized());
	return Transform3D(basis, to_vector(p_pose.com_position) - basis.xform(to_vector(p_model.center_of_mass)));
}

Transform3D vehicle_wheel_transform(const sim::VehicleWheelModel &p_wheel, const host::PresentedVehiclePose &p_pose,
		size_t p_index, const Transform3D &p_rest) {
	const Vector3 centre = to_vector(p_wheel.attachment) - Vector3(0.0f, p_pose.suspension_length[p_index], 0.0f);
	const Basis turn = Basis(Vector3(0.0f, 1.0f, 0.0f), p_pose.wheel_steer[p_index]) *
			Basis(Vector3(-1.0f, 0.0f, 0.0f), p_pose.wheel_angle[p_index]);
	return Transform3D(turn * p_rest.basis, p_rest.origin + (centre - to_vector(p_wheel.data_centre)));
}

GameVehicles::~GameVehicles() {
	if (root_ == nullptr) {
		return;
	}
	if (Node3D *root = Object::cast_to<Node3D>(ObjectDB::get_instance(root_id_))) {
		if (root->is_inside_tree()) {
			root->queue_free();
		} else {
			memdelete(root);
		}
	}
}

core::Status GameVehicles::load(const GameLoadInputs &p_in) {
	if (p_in.db == nullptr || p_in.db->level == nullptr || p_in.shared == nullptr || p_in.shared->vehicles == nullptr) {
		return core::make_error("vehicles: no level database or level vehicles");
	}
	level_ = p_in.shared->vehicles;
	std::vector<render::VehicleMeshSpec> specs;
	for (const host::LevelVehicleKind &k : level_->kinds) {
		specs.push_back(render::VehicleMeshSpec{ k.blueprint, k.mesh, k.component_meshes });
	}
	core::Result<std::unique_ptr<render::VehicleMeshSet>> set =
			render::load_vehicle_meshes(p_in.db->level->db(), p_in.db->level->mount(), specs, p_in.cache);
	if (!set) {
		return set.error();
	}
	set_ = std::move(*set);
	stats_.kinds = uint32_t(set_->kinds.size());
	return core::Status::success();
}

bool GameVehicles::build(Node3D *p_parent, std::string *r_error) {
	if (set_ == nullptr || level_ == nullptr || p_parent == nullptr) {
		*r_error = "vehicles: not loaded";
		return false;
	}
	std::map<std::string, Ref<Texture2D>> textures;
	const auto find_texture = [&](const std::string &p_name) -> Ref<Texture2D> {
		const auto it = textures.find(p_name);
		if (it != textures.end()) {
			return it->second;
		}
		const render::PreparedTexture *t = set_->texture(p_name);
		Ref<Texture2D> tex;
		if (t != nullptr) {
			tex = make_texture(*t);
		}
		textures[p_name] = tex;
		return tex;
	};
	kinds_.assign(level_->kinds.size(), Kind{});
	for (size_t k = 0; k < level_->kinds.size(); ++k) {
		const render::VehicleMeshKind *mk = set_->kind(level_->kinds[k].blueprint);
		if (mk == nullptr) {
			*r_error = "vehicles: no mesh loaded for " + level_->kinds[k].blueprint;
			return false;
		}
		const sim::VehicleModel &model = *level_->kinds[k].model;
		std::vector<render::VehicleWheelRest> wheels;
		for (size_t w = 0; w < model.wheel_count; ++w) {
			wheels.push_back(render::VehicleWheelRest{ model.wheels[w].data_centre, model.wheels[w].radius });
		}
		const std::vector<int8_t> wheel_of = render::vehicle_wheel_parts(*mk, wheels);
		for (const int8_t w : wheel_of) {
			stats_.wheel_parts += w >= 0 ? 1u : 0u;
		}
		for (size_t p = 0; p < mk->parts.size(); ++p) {
			const render::VehicleMeshPart &part = mk->parts[p];
			const render::MeshMaterials &slots = mk->materials[mk->part_mesh[p]];
			std::vector<Ref<Material>> materials;
			for (const render::PreparedSurface &s : part.prepared.surfaces) {
				Ref<Material> m;
				if (s.material_index < slots.slots.size()) {
					m = make_standard_material(
							slots.slots[s.material_index], !s.tangents.empty(), find_texture, MaterialOptions{});
				}
				materials.push_back(m);
			}
			const render::PreparedSurface *first = part.prepared.surfaces.data();
			Ref<ArrayMesh> mesh =
					make_array_mesh(part.prepared, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
						const size_t i = size_t(&p_s - first);
						return i < materials.size() ? materials[i] : Ref<Material>();
					});
			if (mesh.is_null()) {
				*r_error = "vehicles: no mesh made for part " + std::to_string(part.part) + " of " + mk->mesh;
				return false;
			}
			const Transform3D rest = godot_transform(part.rest) * godot_transform(part.part_transform);
			kinds_[k].parts.push_back(Part{ mesh, wheel_of[p], rest });
			++stats_.parts;
		}
	}
	root_ = memnew(Node3D);
	root_->set_name("Vehicles");
	p_parent->add_child(root_);
	root_id_ = root_->get_instance_id();
	return true;
}

void GameVehicles::step(const host::PresentationFrame &p_frame, uint64_t p_now_ns, uint8_t p_own_slot) {
	own_.reset();
	if (root_ == nullptr || ObjectDB::get_instance(root_id_) == nullptr || level_ == nullptr) {
		return;
	}
	const double render_ticks = host::present_render_ticks(p_frame, p_now_ns);
	const double own_ticks = double(p_frame.tick) - 1.0 + host::present_own_fraction(p_frame, p_now_ns);
	std::set<uint16_t> live;
	stats_.unknown = 0;
	for (size_t i = 0; i < p_frame.vehicle_count && i < p_frame.vehicles.size(); ++i) {
		const host::PresentedVehicle &v = p_frame.vehicles[i];
		if (v.spawner >= level_->spawners.size()) {
			++stats_.unknown;
			continue;
		}
		const uint16_t kind = level_->spawners[v.spawner].kind;
		const sim::VehicleModel &model = *level_->kinds[kind].model;
		const std::optional<host::PresentedSeat> own_seat = host::vehicle_seat_of(std::span(&v, 1), p_own_slot);
		const host::PresentedVehiclePose pose = host::vehicle_pose_at(v, own_seat ? own_ticks : render_ticks);
		const Transform3D body = vehicle_body_transform(model, pose);
		if (own_seat) {
			const Vector3 forward = body.basis.get_column(2);
			own_ = OwnVehicleSeat{ v.spawner, own_seat->seat, kind, body, std::atan2(forward.x, forward.z) };
		}
		live.insert(v.spawner);
		Shown &shown = shown_[v.spawner];
		Node3D *node = Object::cast_to<Node3D>(ObjectDB::get_instance(shown.node_id));
		if (node != nullptr && shown.kind != kind) {
			free_node(shown.node_id);
			node = nullptr;
		}
		if (node == nullptr) {
			node = memnew(Node3D);
			root_->add_child(node);
			shown.node_id = node->get_instance_id();
			shown.kind = kind;
			shown.part_ids.clear();
			for (const Part &part : kinds_[kind].parts) {
				MeshInstance3D *mi = memnew(MeshInstance3D);
				mi->set_mesh(part.mesh);
				mi->set_transform(part.rest);
				node->add_child(mi);
				shown.part_ids.push_back(mi->get_instance_id());
			}
			++stats_.created;
		}
		shown.generation = v.generation;
		node->set_transform(body);
		const std::vector<Part> &parts = kinds_[kind].parts;
		for (size_t p = 0; p < parts.size() && p < shown.part_ids.size(); ++p) {
			if (parts[p].wheel < 0 || size_t(parts[p].wheel) >= v.wheel_count) {
				continue;
			}
			if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(shown.part_ids[p]))) {
				mi->set_transform(vehicle_wheel_transform(
						model.wheels[size_t(parts[p].wheel)], pose, size_t(parts[p].wheel), parts[p].rest));
			}
		}
	}
	for (auto it = shown_.begin(); it != shown_.end();) {
		if (live.count(it->first) != 0) {
			++it;
			continue;
		}
		free_node(it->second.node_id);
		++stats_.freed;
		it = shown_.erase(it);
	}
	stats_.shown = uint32_t(shown_.size());
}

std::optional<GameVehicleView> GameVehicles::view(uint16_t p_spawner) const {
	const auto it = shown_.find(p_spawner);
	if (it == shown_.end()) {
		return std::nullopt;
	}
	const Node3D *node = Object::cast_to<Node3D>(ObjectDB::get_instance(it->second.node_id));
	if (node == nullptr) {
		return std::nullopt;
	}
	GameVehicleView v;
	v.in_tree = node->is_inside_tree();
	v.visible = v.in_tree && node->is_visible_in_tree();
	v.parts = int32_t(it->second.part_ids.size());
	v.body = node->get_transform();
	const std::vector<Part> &parts = kinds_[it->second.kind].parts;
	for (size_t p = 0; p < parts.size() && p < it->second.part_ids.size(); ++p) {
		if (parts[p].wheel < 0) {
			continue;
		}
		if (const MeshInstance3D *mi =
						Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(it->second.part_ids[p]))) {
			v.wheels.push_back(mi->get_transform());
		}
	}
	return v;
}

}

namespace bf3::godot_side {

VehicleSeatView GameVehicles::seat_view(
		const OwnVehicleSeat &p_seat, uint32_t p_camera, float p_yaw, float p_pitch, float p_fov_deg) const {
	VehicleSeatView out;
	out.fov_deg = p_fov_deg;
	out.left_yaw = out.right_yaw = sim::kPi;
	out.up_pitch = out.down_pitch = 0.5f * sim::kPi;
	if (level_ == nullptr || p_seat.kind >= level_->kinds.size()) {
		out.camera = p_seat.body;
		return out;
	}
	const host::LevelVehicleKind &kind = level_->kinds[p_seat.kind];
	const core::gameplay::VehicleSeatDef *seat =
			p_seat.seat < kind.seat_defs.size() ? &kind.seat_defs[p_seat.seat] : nullptr;
	std::vector<const core::gameplay::VehicleCameraDef *> cameras;
	if (seat != nullptr) {
		for (const bool first : { true, false }) {
			for (const core::gameplay::VehicleCameraDef &c : seat->cameras) {
				if (c.first_person == first) {
					cameras.push_back(&c);
				}
			}
		}
	}
	const float relative = std::remainder(p_yaw - p_seat.heading, sim::kTwoPi);
	const Basis look = Basis(Vector3(0, 1, 0), relative + sim::kPi) * Basis(Vector3(1, 0, 0), p_pitch);
	if (cameras.empty()) {
		const Vector3 at = seat != nullptr ? to_vector(seat->transform.trans) : Vector3();
		out.camera = p_seat.body * Transform3D(look, at);
		return out;
	}
	const core::gameplay::VehicleCameraDef &c = *cameras[p_camera % cameras.size()];
	out.first_person = c.first_person;
	if (!c.first_person && c.chase_distance > 0.0f) {
		const Vector3 target = to_vector(c.transform.trans);
		const Vector3 ahead = Basis(Vector3(0, 1, 0), relative).xform(Vector3(0, 0, 1));
		const float rise = std::clamp(c.chase_angle * core::kDegToRadF - p_pitch, -0.4f * sim::kPi, 0.45f * sim::kPi);
		const Vector3 eye =
				target - ahead * (std::cos(rise) * c.chase_distance) + Vector3(0, std::sin(rise) * c.chase_distance, 0);
		const Vector3 at = target + ahead * c.chase_look_ahead;
		out.camera = p_seat.body * Transform3D(Basis(), eye).looking_at(at, Vector3(0, 1, 0));
		return out;
	}
	if (c.left_yaw > 0.0f || c.right_yaw > 0.0f) {
		out.left_yaw = c.left_yaw * core::kDegToRadF;
		out.right_yaw = c.right_yaw * core::kDegToRadF;
	}
	if (c.up_pitch > 0.0f || c.down_pitch > 0.0f) {
		out.up_pitch = c.up_pitch * core::kDegToRadF;
		out.down_pitch = c.down_pitch * core::kDegToRadF;
	}
	out.camera = p_seat.body * Transform3D(look, to_vector(c.transform.trans));
	return out;
}

}
