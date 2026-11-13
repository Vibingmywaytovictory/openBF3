#pragma once

#include "bf3/core/result.h"
#include "bf3/fx/simulator.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/present/vehicle_damage_fx.h"
#include "bf3/present/vehicle_turrets.h"
#include "bf3/present/vehicle_zoom.h"
#include "bf3/render/vehicle_meshes.h"
#include "death_camera.h"
#include "game_frame.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf3::godot_side {

class Bf3GameFx;

struct GameVehiclesStats {
	uint32_t kinds = 0;
	uint32_t parts = 0;
	uint32_t wheel_parts = 0;
	uint32_t glass_surfaces = 0;
	uint32_t turret_parts = 0;
	uint32_t shown = 0;
	uint32_t unknown = 0;
	uint64_t created = 0;
	uint64_t freed = 0;
	uint64_t wrecks = 0;
	uint32_t wreck_kinds = 0;
	uint64_t damage_starts = 0;
	uint64_t damage_stops = 0;
	uint32_t damage_live = 0;
	uint64_t husks = 0;
	uint64_t husk_effects = 0;
	uint32_t husks_live = 0;
	uint64_t fx_failures = 0;
};

struct OwnVehicleSeat {
	uint16_t spawner = 0;
	uint8_t seat = 0;
	uint16_t kind = 0;
	godot::Transform3D body;
	float heading = 0.0f;
	godot::Vector3 velocity;
	std::vector<godot::Transform3D> turrets;
};

struct VehicleSeatView {
	godot::Transform3D camera;
	float fov_deg = 0.0f;
	bool zoomed = false;
	float black = 0.0f;
	bool first_person = true;
	float left_yaw = 0.0f, right_yaw = 0.0f, up_pitch = 0.0f, down_pitch = 0.0f;
};

struct GameVehicleTurretPart {
	int turret = -1;
	godot::Transform3D rest;
	godot::Transform3D drawn;
};

struct GameVehicleView {
	bool in_tree = false;
	bool visible = false;
	int32_t parts = 0;
	godot::Transform3D body;
	std::vector<godot::Transform3D> wheels;
	std::vector<GameVehicleTurretPart> turret_parts;
};

class GameVehicles {
public:
	GameVehicles() = default;
	GameVehicles(const GameVehicles &) = delete;
	GameVehicles &operator=(const GameVehicles &) = delete;
	~GameVehicles();
	core::Status load(const GameLoadInputs &p_in);
	bool loaded() const { return set_ != nullptr; }
	bool build(godot::Node3D *p_parent, std::string *r_error);
	bool built() const { return root_ != nullptr; }
	void step(const host::PresentationFrame &p_frame, uint64_t p_now_ns, uint8_t p_own_slot, Bf3GameFx *p_fx);
	std::optional<OwnVehicleSeat> own_seat() const { return own_; }
	const host::LevelVehicles *level() const { return level_.get(); }
	const GameVehiclesStats &stats() const { return stats_; }
	std::optional<GameVehicleView> view(uint16_t p_spawner) const;
	VehicleSeatView seat_view(const OwnVehicleSeat &p_seat, uint32_t p_camera, float p_yaw, float p_pitch,
			float p_fov_deg, bool p_zoom, double p_dt, const sim::CollisionWorld *p_world);

private:
	void present_damage(std::span<const present::VehicleFxSight> p_sights,
			const std::map<uint16_t, std::pair<godot::Transform3D, godot::Vector3>> &p_placed, double p_now_s,
			Bf3GameFx &r_fx);

	struct Part {
		godot::Ref<godot::ArrayMesh> mesh;
		int8_t wheel = -1;
		godot::Transform3D rest;
		int8_t turret = -1;
	};
	struct Kind {
		std::vector<Part> parts;
	};
	struct DamageEffect {
		fx::EffectHandle handle;
		godot::Transform3D local;
	};
	struct Husk {
		uint16_t spawner = 0;
		uint8_t generation = 0;
		uint64_t node_id = 0;
		godot::Transform3D at;
		std::vector<fx::EffectHandle> effects;
	};
	struct Shown {
		uint64_t node_id = 0;
		uint16_t kind = 0;
		uint8_t generation = 0;
		std::vector<uint64_t> part_ids;
	};

	std::shared_ptr<const host::LevelVehicles> level_;
	std::unique_ptr<render::VehicleMeshSet> set_;
	std::vector<Kind> kinds_;
	std::map<uint16_t, Shown> shown_;
	std::map<std::string, std::vector<Part>> wrecks_;
	std::optional<present::VehicleDamageFx> damage_fx_;
	std::map<std::pair<uint16_t, uint16_t>, DamageEffect> damage_effects_;
	std::vector<Husk> husks_;
	host::VehicleWrecks wrecked_;
	std::optional<OwnVehicleSeat> own_;
	ChaseCamera chase_;
	std::optional<std::array<uint32_t, 3>> chase_key_;
	present::VehicleZoom zoom_;
	std::optional<std::array<uint32_t, 3>> zoom_key_;
	godot::Node3D *root_ = nullptr;
	uint64_t root_id_ = 0;
	GameVehiclesStats stats_;
};

godot::Transform3D vehicle_body_transform(const sim::VehicleModel &p_model, const host::PresentedVehiclePose &p_pose);

godot::Transform3D vehicle_wheel_transform(const sim::VehicleWheelModel &p_wheel,
		const host::PresentedVehiclePose &p_pose, size_t p_index, const godot::Transform3D &p_rest);

}
