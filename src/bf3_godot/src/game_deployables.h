#pragma once

#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/render/deployed_gadgets.h"
#include "game_frame.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

struct GameDeployablesStats {
	uint32_t parts = 0;
	uint32_t shown = 0;
	uint32_t unknown_defs = 0;
	uint64_t created = 0;
	uint64_t freed = 0;
};

struct GameDeployableView {
	uint16_t def = 0;
	bool in_tree = false;
	bool visible = false;
	bool def_mesh = false;
	int32_t surfaces = 0;
	godot::Vector3 position;
};

class GameDeployables {
public:
	GameDeployables() = default;
	GameDeployables(const GameDeployables &) = delete;
	GameDeployables &operator=(const GameDeployables &) = delete;
	~GameDeployables();
	core::Status load(const GameLoadInputs &p_in);
	bool loaded() const { return set_ != nullptr; }
	bool build(godot::Node3D *p_parent, std::string *r_error);
	bool built() const { return root_ != nullptr; }
	void step(const host::PresentationFrame &p_frame);
	const GameDeployablesStats &stats() const { return stats_; }
	std::optional<GameDeployableView> view(uint16_t p_id) const;

private:
	std::unique_ptr<render::DeployedGadgetSet> set_;
	std::map<uint16_t, godot::Ref<godot::ArrayMesh>> meshes_;
	struct Shown {
		uint64_t node_id = 0;
		uint16_t def = 0;
	};
	std::map<uint16_t, Shown> shown_;
	godot::Node3D *root_ = nullptr;
	uint64_t root_id_ = 0;
	GameDeployablesStats stats_;
};

}
