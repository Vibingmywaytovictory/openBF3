#include "tree_bridge.h"

#include "level_upload.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdio>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;

namespace {

Transform3D bone_transform(const bf3::core::Transform &p_t) {
	return Transform3D(Basis(p_t.right.x, p_t.up.x, p_t.forward.x, p_t.right.y, p_t.up.y, p_t.forward.y, p_t.right.z,
							   p_t.up.z, p_t.forward.z),
			Vector3(p_t.trans.x, p_t.trans.y, p_t.trans.z));
}

}

TreeBridge::~TreeBridge() {
	free_all();
}

void TreeBridge::free_all() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (const RID &s : skeletons_) {
			if (s.is_valid()) {
				rs->free_rid(s);
			}
		}
	}
	skeletons_.clear();
	instance_of_tree_.clear();
	forest_.reset();
	plan_.reset();
	posed_ = 0;
}

void TreeBridge::update(const LevelUpload &p_upload, const std::shared_ptr<const render::LevelScenePlan> &p_plan,
		const render::LookFrame *p_look, const Vector3 &p_camera, double p_delta) {
	if (p_plan != plan_) {
		free_all();
		plan_ = p_plan;
		if (plan_ == nullptr || plan_->trees.trees.empty()) {
			return;
		}
		forest_ = std::make_unique<render::TreeForest>(plan_->trees.rigs, plan_->trees.trees);
		instance_of_tree_.assign(plan_->trees.trees.size(), UINT32_MAX);
		skeletons_.assign(plan_->trees.trees.size(), RID());
		for (size_t i = 0; i < plan_->instances.size(); ++i) {
			const uint32_t tree = plan_->instances[i].tree;
			if (tree < instance_of_tree_.size()) {
				instance_of_tree_[tree] = uint32_t(i);
			}
		}
	}
	if (forest_ == nullptr) {
		return;
	}
	const std::optional<render::LevelWind> level = p_look != nullptr && p_look->wind
			? std::optional<render::LevelWind>(render::LevelWind{ p_look->wind_direction, p_look->wind_strength })
			: std::nullopt;
	forest_->frame(
			float(p_delta), { float(p_camera.x), float(p_camera.y), float(p_camera.z) }, render::tree_wind({}, level));
	RenderingServer *rs = RenderingServer::get_singleton();
	for (size_t t = 0; t < forest_->trees(); ++t) {
		const render::TreeSim *sim = forest_->sim(t);
		// SOURCE: notes R6-VG2-rate (new trees): a tree draws with its instance matrix until its pre-simulation has run.
		if (sim == nullptr || !forest_->stepped(t) || !forest_->posed(t)) {
			continue;
		}
		const RID instance = p_upload.plan_instance(instance_of_tree_[t]);
		if (!instance.is_valid()) {
			continue;
		}
		const std::vector<bf3::core::Transform> offsets =
				render::tree_bone_poses(*sim, forest_->placement(t).transform);
		if (!skeletons_[t].is_valid()) {
			skeletons_[t] = rs->skeleton_create();
			rs->skeleton_allocate_data(skeletons_[t], int32_t(offsets.size()), false);
			rs->instance_attach_skeleton(instance, skeletons_[t]);
		}
		for (size_t b = 0; b < offsets.size(); ++b) {
			rs->skeleton_bone_set_transform(skeletons_[t], int32_t(b), bone_transform(offsets[b]));
		}
		++posed_;
	}
}

std::string TreeBridge::describe() const {
	size_t skeletons = 0;
	for (const RID &s : skeletons_) {
		skeletons += s.is_valid() ? 1 : 0;
	}
	char line[256];
	std::snprintf(line, sizeof(line), "[trees] %zu trees, %zu active, %zu posed skeletons, %llu steps, %llu poses",
			forest_ != nullptr ? forest_->trees() : size_t(0), forest_ != nullptr ? forest_->active() : size_t(0),
			skeletons, (unsigned long long)(forest_ != nullptr ? forest_->steps() : 0), (unsigned long long)posed_);
	return line;
}

}
