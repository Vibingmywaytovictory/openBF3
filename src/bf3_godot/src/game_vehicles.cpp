#include "game_vehicles.h"

#include "game_fx.h"
#include "render_bridge.h"
#include "viewmodel_muzzle.h"

#include "bf3/host/level_load.h"
#include "bf3/render/viewmodel_set.h"

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

void apply_vehicle_look(const Ref<StandardMaterial3D> &p_m, const render::LookMaterial &p_look) {
	if (p_look.double_sided) {
		p_m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	}
	if (p_look.blend == render::LookBlend::AlphaTest) {
		p_m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
		return;
	}
	if (p_look.blend != render::LookBlend::Blend) {
		return;
	}
	p_m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	if (p_look.glass) {
		if (!p_look.color) {
			p_m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, Ref<Texture2D>());
			p_m->set_albedo(Color(
					render::kLookGlassTint, render::kLookGlassTint, render::kLookGlassTint, render::kLookGlassOpacity));
		} else {
			p_m->set_albedo(Color(1.0f, 1.0f, 1.0f, render::kLookGlassOpacity));
		}
		p_m->set_roughness(0.1f);
		return;
	}
	const render::ViewmodelBlend mode = render::viewmodel_blend_of(p_look.solution_blend.value_or(0));
	p_m->set_blend_mode(mode == render::ViewmodelBlend::Add	   ? BaseMaterial3D::BLEND_MODE_ADD
					: mode == render::ViewmodelBlend::Multiply ? BaseMaterial3D::BLEND_MODE_MUL
															   : BaseMaterial3D::BLEND_MODE_MIX);
}

constexpr uint64_t kWreckSeed = 0x77726563u;

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
	for (const host::LevelVehicleKind &k : level_->kinds) {
		const std::optional<core::gameplay::VehicleWreckDef> &w = k.damage_visuals.wreck;
		if (w && !w->mesh.empty() && std::none_of(specs.begin(), specs.end(), [&](const render::VehicleMeshSpec &p_s) {
				return p_s.blueprint == w->blueprint;
			})) {
			specs.push_back(render::VehicleMeshSpec{ w->blueprint, w->mesh, {} });
		}
	}
	core::Result<std::unique_ptr<render::VehicleMeshSet>> set =
			render::load_vehicle_meshes(p_in.db->level->db(), p_in.db->level->mount(), specs, p_in.cache);
	if (!set) {
		return set.error();
	}
	set_ = std::move(*set);
	stats_.kinds = uint32_t(level_->kinds.size());
	stats_.wreck_kinds = uint32_t(set_->kinds.size() - std::min(set_->kinds.size(), level_->kinds.size()));
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
	const auto make_parts = [&](const render::VehicleMeshKind *mk, const std::vector<int8_t> &wheel_of,
									std::vector<Part> &r_parts) {
		for (size_t p = 0; p < mk->parts.size(); ++p) {
			const render::VehicleMeshPart &part = mk->parts[p];
			const render::MeshMaterials &slots = mk->materials[mk->part_mesh[p]];
			const std::vector<render::LookMaterial> &looks = mk->looks[mk->part_mesh[p]];
			render::PreparedMesh drawn = part.prepared;
			std::erase_if(drawn.surfaces, [&](const render::PreparedSurface &p_s) {
				return p_s.material_index < looks.size() && !looks[p_s.material_index].drawn;
			});
			std::vector<Ref<Material>> materials;
			for (const render::PreparedSurface &s : drawn.surfaces) {
				Ref<StandardMaterial3D> m;
				if (s.material_index < slots.slots.size()) {
					m = make_standard_material(
							slots.slots[s.material_index], !s.tangents.empty(), find_texture, MaterialOptions{});
					if (s.material_index < looks.size()) {
						apply_vehicle_look(m, looks[s.material_index]);
						stats_.glass_surfaces += looks[s.material_index].glass ? 1u : 0u;
					}
				}
				materials.push_back(m);
			}
			const int8_t wheel = p < wheel_of.size() ? wheel_of[p] : int8_t(-1);
			if (drawn.surfaces.empty()) {
				r_parts.push_back(Part{ Ref<ArrayMesh>(), wheel, Transform3D() });
				continue;
			}
			const render::PreparedSurface *first = drawn.surfaces.data();
			Ref<ArrayMesh> mesh = make_array_mesh(drawn, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
				const size_t i = size_t(&p_s - first);
				return i < materials.size() ? materials[i] : Ref<Material>();
			});
			if (mesh.is_null()) {
				*r_error = "vehicles: no mesh made for part " + std::to_string(part.part) + " of " + mk->mesh;
				return false;
			}
			const Transform3D rest = godot_transform(part.rest) * godot_transform(part.part_transform);
			r_parts.push_back(Part{ mesh, wheel, rest });
			++stats_.parts;
		}
		return true;
	};
	kinds_.assign(level_->kinds.size(), Kind{});
	std::vector<core::gameplay::VehicleDamageVisualsDef> visuals;
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
		if (!make_parts(mk, wheel_of, kinds_[k].parts)) {
			return false;
		}
		visuals.push_back(level_->kinds[k].damage_visuals);
		const std::optional<core::gameplay::VehicleWreckDef> &wreck = level_->kinds[k].damage_visuals.wreck;
		if (!wreck || wrecks_.count(wreck->blueprint) != 0) {
			continue;
		}
		const render::VehicleMeshKind *wk = set_->kind(wreck->blueprint);
		if (wk == nullptr) {
			*r_error = "vehicles: no wreck mesh loaded for " + wreck->blueprint;
			return false;
		}
		if (!make_parts(wk, {}, wrecks_[wreck->blueprint])) {
			return false;
		}
	}
	damage_fx_.emplace(std::move(visuals));
	root_ = memnew(Node3D);
	root_->set_name("Vehicles");
	p_parent->add_child(root_);
	root_id_ = root_->get_instance_id();
	return true;
}

void GameVehicles::step(
		const host::PresentationFrame &p_frame, uint64_t p_now_ns, uint8_t p_own_slot, Bf3GameFx *p_fx) {
	own_.reset();
	if (root_ == nullptr || ObjectDB::get_instance(root_id_) == nullptr || level_ == nullptr) {
		return;
	}
	const double render_ticks = host::present_render_ticks(p_frame, p_now_ns);
	const double own_ticks = double(p_frame.tick) - 1.0 + host::present_own_fraction(p_frame, p_now_ns);
	std::set<uint16_t> live;
	std::vector<present::VehicleFxSight> sights;
	std::map<uint16_t, std::pair<Transform3D, Vector3>> placed;
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
		const host::VehicleWreckSight wreck = wrecked_.see(v);
		sights.push_back(
				present::VehicleFxSight{ v.spawner, v.generation, kind, v.health, wreck.hidden, wreck.explosion });
		placed[v.spawner] = { body, to_vector(v.linear_velocity) };
		if (wreck.explosion) {
			++stats_.wrecks;
			if (p_fx != nullptr && !level_->kinds[kind].explosion_effect.empty() &&
					!p_fx->start_effect(level_->kinds[kind].explosion_effect, to_fx_transform(body))) {
				++stats_.fx_failures;
			}
		}
		if (wreck.hidden) {
			continue;
		}
		if (own_seat) {
			const Vector3 forward = body.basis.get_column(2);
			own_ = OwnVehicleSeat{ v.spawner, own_seat->seat, kind, body, std::atan2(forward.x, forward.z),
				to_vector(v.linear_velocity) };
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
	if (p_fx != nullptr && damage_fx_) {
		present_damage(sights, placed, double(p_now_ns) * 1e-9, *p_fx);
	}
}

void GameVehicles::present_damage(std::span<const present::VehicleFxSight> p_sights,
		const std::map<uint16_t, std::pair<Transform3D, Vector3>> &p_placed, double p_now_s, Bf3GameFx &r_fx) {
	std::vector<present::VehicleFxCommand> commands;
	damage_fx_->step(p_sights, p_now_s, kWreckSeed, commands);
	const auto start = [&](const std::string &p_effect, const Transform3D &p_at) -> std::optional<fx::EffectHandle> {
		core::Result<fx::EffectHandle> h = r_fx.start_effect(p_effect, to_fx_transform(p_at));
		if (!h) {
			++stats_.fx_failures;
			return std::nullopt;
		}
		return *h;
	};
	for (const present::VehicleFxCommand &c : commands) {
		const core::gameplay::VehicleDamageVisualsDef &def = level_->kinds[c.kind].damage_visuals;
		const auto at = p_placed.find(c.spawner);
		const Transform3D body = at != p_placed.end() ? at->second.first : Transform3D();
		const std::pair<uint16_t, uint16_t> key{ c.spawner, c.effect };
		switch (c.op) {
			case present::VehicleFxOp::StartDamage:
				if (const Transform3D local = godot_transform(def.damage[c.effect].transform);
						const std::optional<fx::EffectHandle> h = start(def.damage[c.effect].effect, body * local)) {
					damage_effects_[key] = DamageEffect{ *h, local };
					++stats_.damage_starts;
				}
				break;
			case present::VehicleFxOp::StopDamage:
				if (const auto it = damage_effects_.find(key); it != damage_effects_.end()) {
					r_fx.stop_effect(it->second.handle);
					damage_effects_.erase(it);
					++stats_.damage_stops;
				}
				break;
			case present::VehicleFxOp::StartWreck: {
				Husk husk{ c.spawner, c.generation, 0, body * godot_transform(def.wreck->transform), {} };
				Node3D *node = memnew(Node3D);
				root_->add_child(node);
				node->set_transform(husk.at);
				for (const Part &part : wrecks_[def.wreck->blueprint]) {
					MeshInstance3D *mi = memnew(MeshInstance3D);
					mi->set_mesh(part.mesh);
					mi->set_transform(part.rest);
					node->add_child(mi);
				}
				husk.node_id = node->get_instance_id();
				husks_.push_back(std::move(husk));
				++stats_.husks;
				break;
			}
			case present::VehicleFxOp::StartWreckEffect:
				for (Husk &h : husks_) {
					if (h.spawner == c.spawner && h.generation == c.generation) {
						const core::gameplay::VehicleWreckEffectDef &e = def.wreck->effects[c.effect];
						if (const std::optional<fx::EffectHandle> started =
										start(e.effect, h.at * godot_transform(e.transform))) {
							h.effects.push_back(*started);
							++stats_.husk_effects;
						}
					}
				}
				break;
			case present::VehicleFxOp::EndWreck:
				for (auto it = husks_.begin(); it != husks_.end();) {
					if (it->spawner != c.spawner || it->generation != c.generation) {
						++it;
						continue;
					}
					for (const fx::EffectHandle e : it->effects) {
						r_fx.stop_effect(e);
					}
					free_node(it->node_id);
					it = husks_.erase(it);
				}
				break;
		}
	}
	for (const auto &[key, effect] : damage_effects_) {
		const auto at = p_placed.find(key.first);
		if (at == p_placed.end()) {
			continue;
		}
		const Vector3 v = at->second.second;
		r_fx.move_effect(effect.handle, to_fx_transform(at->second.first * effect.local), fx::Vec3{ v.x, v.y, v.z });
	}
	stats_.damage_live = uint32_t(damage_effects_.size());
	stats_.husks_live = uint32_t(husks_.size());
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

VehicleSeatView GameVehicles::seat_view(const OwnVehicleSeat &p_seat, uint32_t p_camera, float p_yaw, float p_pitch,
		float p_fov_deg, bool p_zoom, double p_dt, const sim::CollisionWorld *p_world) {
	VehicleSeatView out;
	out.fov_deg = p_fov_deg;
	out.left_yaw = out.right_yaw = sim::kPi;
	out.up_pitch = out.down_pitch = 0.5f * sim::kPi;
	if (level_ == nullptr || p_seat.kind >= level_->kinds.size()) {
		out.camera = p_seat.body;
		chase_key_.reset();
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
		chase_key_.reset();
		return out;
	}
	const uint32_t index = p_camera % uint32_t(cameras.size());
	const core::gameplay::VehicleCameraDef &c = *cameras[index];
	out.first_person = c.first_person;
	const std::array<uint32_t, 3> view_key{ p_seat.spawner, p_seat.seat, index };
	if (zoom_key_ != view_key) {
		zoom_.reset();
		zoom_key_ = view_key;
	}
	std::optional<present::VehicleZoomDef> alternate;
	if (c.zoom_field_of_view) {
		alternate = present::VehicleZoomDef{ *c.zoom_field_of_view, c.zoom_fov_transition_time, c.zoom_fade_to_black,
			c.zoom_fade_out, c.zoom_black, c.zoom_fade_in };
	}
	const present::VehicleZoomView zoomed = zoom_.step(alternate, p_zoom, p_fov_deg, p_dt);
	out.fov_deg = zoomed.fov;
	out.zoomed = zoomed.zoomed;
	out.black = zoomed.black;
	if (!c.first_person && c.chase) {
		const Vector3 pivot = p_seat.body.xform(to_vector(c.transform.trans));
		ChaseTarget target;
		target.eye = fx::Vec3{ pivot.x, pivot.y, pivot.z };
		target.yaw_rad = p_yaw;
		target.pitch_rad = p_pitch;
		target.velocity = fx::Vec3{ p_seat.velocity.x, p_seat.velocity.y, p_seat.velocity.z };
		if (chase_key_ != view_key) {
			chase_ = ChaseCamera(*c.chase);
			chase_.activate(target, p_world);
			chase_key_ = view_key;
		}
		out.camera = camera_transform(chase_.step(p_dt, target, p_world));
		return out;
	}
	chase_key_.reset();
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
