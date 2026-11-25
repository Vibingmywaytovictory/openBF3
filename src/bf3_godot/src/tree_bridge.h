#pragma once

#include "bf3/render/level_scene.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/tree_wind.h"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

class LevelUpload;

class TreeBridge {
public:
	TreeBridge() = default;
	~TreeBridge();
	TreeBridge(const TreeBridge &) = delete;
	TreeBridge &operator=(const TreeBridge &) = delete;

	void update(const LevelUpload &p_upload, const std::shared_ptr<const bf3::render::LevelScenePlan> &p_plan,
			const bf3::render::LookFrame *p_look, const godot::Vector3 &p_camera, double p_delta);
	void free_all();

	std::string describe() const;

private:
	std::shared_ptr<const bf3::render::LevelScenePlan> plan_;
	std::unique_ptr<bf3::render::TreeForest> forest_;
	std::vector<uint32_t> instance_of_tree_;
	std::vector<godot::RID> skeletons_;
	uint64_t posed_ = 0;
};

}
