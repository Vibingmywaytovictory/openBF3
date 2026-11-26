#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::level {
class World;
struct PlacedObject;
}

namespace bf3::render {

inline constexpr float kTreeStep = 0.015f;
inline constexpr float kTreeMaxAdvance = 2.0f * kTreeStep;
inline constexpr float kTreePreSimulation = 2.0f;
inline constexpr float kTreePreSimulationMove = 5.0f;
inline constexpr uint32_t kTreeIdleFrames = 10;
inline constexpr unsigned kTreeLanes = 4;
inline constexpr float kTreeWindHeight = 0.1f;
inline constexpr float kTreeGustTravel = 0.005f;
inline constexpr float kTreeForceScale = 0.005f * 0.8f;
inline constexpr float kTreeGravity = 0.2f;
inline constexpr float kTreeDampingBase = 1.06f;
inline constexpr float kTreeRestoring = 200.0f;
inline constexpr float kTreeRestoringStep = 0.015f;
inline constexpr float kTreeGroundFriction = 0.4f;
inline constexpr float kTreeThreshold = 1e-6f;
inline constexpr float kTreeDegenerateBone = 0.01f;
inline constexpr size_t kTreeMaxChildren = 10;
inline constexpr core::Vec3 kTreeDefaultWindDirection{ 0.4f, 0.0f, 0.8434434f };
inline constexpr float kTreeGustFrequencies[4] = { 4.585867f, 1.485746f, 9.654985f, 31.567549f };

struct VegetationSystemSettings {
	float wind_strength = 2.0f;
	float wind_variation = 1.1f;
	float wind_variation_rate = 0.1f;
	float time_scale = 1.0f;
	float max_active_distance = 250.0f;
	uint32_t max_pre_sims_per_job = 4;
	uint32_t job_count = 6;
	bool draw = true;
};

struct TreeRig {
	std::vector<int32_t> parents;
	std::vector<std::vector<uint32_t>> children;
	std::vector<uint32_t> tips;
	std::vector<core::Transform> rest;
	std::vector<core::Transform> rest_frames;
	std::vector<core::Transform> rest_frames_inverse;
	std::vector<core::Vec3> offsets;
	std::vector<core::Vec3> rest_directions;
	std::vector<core::Vec3> sides;
	std::vector<float> stiffness;
	std::vector<float> wind;
	std::vector<float> height;
	std::vector<float> length;
	std::vector<uint8_t> stem;
	std::vector<uint8_t> rigid;
	std::vector<uint32_t> driven;
	std::vector<std::vector<uint32_t>> descendants;
	uint32_t stem_bones = 0;
	float ground = 0.0f;

	size_t bones() const { return parents.size(); }
};

struct TreeRigData {
	std::vector<int32_t> parents;
	std::vector<core::Transform> locals;
	std::vector<core::Vec3> translations;
	uint32_t stem_bones = 0;
	float stem_stiffness = 0.0f, stem_stiffness_spread = 0.0f, branch_stiffness = 0.0f, branch_stiffness_l2 = 0.0f;
	float wind_stem = 0.0f, wind_branch = 0.0f, wind_branch_l2 = 0.0f;
	float joint_threshold = 0.0f, ground = 0.0f;
};

core::Result<TreeRig> build_tree_rig(const TreeRigData &p_data);

core::Result<TreeRig> read_tree_rig(const core::EbxInstance &p_tree);

struct TreeWind {
	core::Vec3 direction = kTreeDefaultWindDirection;
	float strength = 0.0f;
	float rate = 0.0f;
	float variation = 0.0f;
};

struct LevelWind {
	float direction_degrees = 0.0f;
	float strength = 0.0f;
};

TreeWind tree_wind(const VegetationSystemSettings &p_settings, const std::optional<LevelWind> &p_level);

core::Vec3 tree_gust(const TreeWind &p_wind, const core::Vec3 &p_tree_position, float p_clock);

core::Vec3 tree_local_force(const core::level::LevelTransform &p_tree, const core::Vec3 &p_force);

core::Transform tree_root_frame(const core::level::LevelTransform &p_tree);

class TreeSim {
public:
	explicit TreeSim(std::shared_ptr<const TreeRig> p_rig, const core::Transform &p_root = {});

	void step(const core::Vec3 &p_local_force);

	const TreeRig &rig() const { return *rig_; }
	const std::vector<core::Vec3> &positions() const { return positions_; }
	const std::vector<core::Vec3> &velocities() const { return velocities_; }
	const std::vector<core::Transform> &frames() const { return frames_; }

	std::vector<core::Transform> bone_offsets() const;

private:
	void rebuild(uint32_t p_parent);
	void restore(uint32_t p_bone, const core::Vec3 &p_direction, const core::Transform &p_parent);
	void hold_above_ground(uint32_t p_node);

	std::shared_ptr<const TreeRig> rig_;
	std::vector<core::Vec3> positions_;
	std::vector<core::Vec3> velocities_;
	std::vector<core::Transform> frames_;
};

std::vector<core::Transform> tree_bone_poses(const TreeSim &p_sim, const core::level::LevelTransform &p_tree);

struct TreePlacement {
	uint32_t rig = 0;
	core::level::LevelTransform transform;
};

struct LevelTrees {
	std::vector<std::shared_ptr<const TreeRig>> rigs;
	std::vector<core::EbxInstance> entities;
	std::vector<TreePlacement> trees;
	std::vector<size_t> objects;
	std::vector<std::string> failures;
};

std::optional<uint32_t> add_level_tree(LevelTrees &r_trees, const core::level::PlacedObject &p_object,
		const core::level::LevelTransform &p_transform, size_t p_object_index);

LevelTrees level_trees(const core::level::World &p_world);

class TreeForest {
public:
	TreeForest(std::vector<std::shared_ptr<const TreeRig>> p_rigs, std::vector<TreePlacement> p_trees,
			const VegetationSystemSettings &p_settings = {});

	void frame(float p_dt, const core::Vec3 &p_camera, const TreeWind &p_wind);

	size_t trees() const { return trees_.size(); }
	const TreePlacement &placement(size_t p_tree) const { return trees_[p_tree]; }
	const TreeSim *sim(size_t p_tree) const { return states_[p_tree].sim.get(); }
	bool stepped(size_t p_tree) const { return states_[p_tree].stepped_frame == frames_; }
	float clock() const { return clock_; }
	uint64_t steps() const { return steps_; }
	size_t active() const;

private:
	struct State {
		std::unique_ptr<TreeSim> sim;
		float pending = 0.0f;
		uint64_t touched_frame = 0;
		uint64_t stepped_frame = 0;
	};

	std::vector<std::shared_ptr<const TreeRig>> rigs_;
	std::vector<TreePlacement> trees_;
	std::vector<State> states_;
	VegetationSystemSettings settings_;
	std::optional<core::Vec3> last_camera_;
	float clock_ = 0.0f;
	uint64_t frames_ = 0;
	uint64_t steps_ = 0;
};

}
