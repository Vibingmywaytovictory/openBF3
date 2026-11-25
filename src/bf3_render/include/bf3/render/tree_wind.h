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
inline constexpr float kTreeGroundFriction = 0.4f;
inline constexpr float kTreeThreshold = 1e-6f;
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
	std::vector<core::Transform> rest;
	std::vector<float> stiffness;
	std::vector<float> wind;
	std::vector<float> height;
	std::vector<float> length;
	std::vector<uint8_t> stem;
	uint32_t stem_bones = 0;

	size_t bones() const { return parents.size(); }
};

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

class TreeSim {
public:
	explicit TreeSim(std::shared_ptr<const TreeRig> p_rig);

	void step(const core::Vec3 &p_local_force);

	const TreeRig &rig() const { return *rig_; }
	const std::vector<core::Vec3> &positions() const { return positions_; }
	const std::vector<core::Vec3> &velocities() const { return velocities_; }

	std::vector<core::Transform> bone_offsets() const;

private:
	void rebuild();
	void restore();

	std::shared_ptr<const TreeRig> rig_;
	std::vector<core::Vec3> positions_;
	std::vector<core::Vec3> velocities_;
	std::vector<std::vector<uint32_t>> subtrees_;
};

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
