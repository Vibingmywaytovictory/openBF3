#include "bf3/render/tree_wind.h"

#include "bf3/core/level/world.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::render {

namespace {

using core::EbxInstance;
using core::EbxValue;
using core::Transform;
using core::Vec3;
namespace vec = core::vec;

core::Result<float> field_number(const EbxInstance &p_tree, std::string_view p_field) {
	const std::optional<EbxValue> v = p_tree.get(p_field);
	const std::optional<float> f = v ? v->as_f32() : std::nullopt;
	if (!f || !core::finite(*f)) {
		return core::make_error("VegetationTreeEntityData." + std::string(p_field) + " is not a finite number");
	}
	return *f;
}

core::Result<std::vector<Vec3>> field_translations(const EbxInstance &p_tree) {
	const std::optional<EbxValue> v = p_tree.get("BasePoseTranslations");
	const core::OptionalEbxArray items = v ? v->as_array() : core::OptionalEbxArray();
	if (!items || items->empty()) {
		return core::make_error("VegetationTreeEntityData.BasePoseTranslations is missing or empty");
	}
	std::vector<Vec3> out;
	for (size_t i = 0; i < items->size(); ++i) {
		const std::optional<EbxValue> item = items->get(i);
		const std::optional<Vec3> t = item ? item->as_vec3() : std::nullopt;
		if (!t || !core::finite(t->x) || !core::finite(t->y) || !core::finite(t->z)) {
			return core::make_error(
					"VegetationTreeEntityData.BasePoseTranslations[" + std::to_string(i) + "] is not a finite Vec3");
		}
		out.push_back(*t);
	}
	return out;
}

// SOURCE: notes R6-VG2-bones S1: rest frames, the R1 and R2 rigid nodes, stiffness and wind by stem, branch and L2, and
// the height lever restarted at stem forks.
struct RigBuilder {
	TreeRig &rig;
	const TreeRigData &data;
	std::vector<uint8_t> short_tip;

	void visit(uint32_t p_parent, float p_height) {
		const Transform &parent = rig.rest_frames[p_parent];
		const Transform parent_inverse = rig.rest_frames_inverse[p_parent];
		float running = p_height;
		for (const uint32_t c : rig.children[p_parent]) {
			const uint32_t t = rig.children[c].size() == 1 ? rig.children[c][0] : c;
			const Vec3 base = rig.rest[c].trans;
			const Vec3 reach = vec::sub(rig.rest[t].trans, base);
			const float reach_length = vec::length(reach);
			rig.tips[c] = t;
			rig.length[c] = vec::length(data.translations[t]);
			const bool fork = rig.children[p_parent].size() > 1;
			if (fork) {
				rig.rigid[c] = 1;
			}
			if (rig.children[c].size() == 1 && rig.length[c] < data.joint_threshold) {
				rig.rigid[t] = 1;
				short_tip[t] = 1;
			}
			const Vec3 y = reach_length > kTreeDegenerateBone ? vec::scale(reach, 1.0f / reach_length) : parent.up;
			const Vec3 x = vec::normalize(vec::cross(y, parent.forward));
			const Vec3 z = vec::normalize(vec::cross(x, y));
			rig.rest_frames[c] = Transform{ x, y, z, base };
			rig.rest_frames_inverse[c] = rig.rest_frames[c].inverse();
			rig.offsets[c] = parent_inverse.transform_point(base);
			rig.rest_directions[c] = parent_inverse.transform_vector(y);
			rig.sides[c] = parent_inverse.transform_vector(z);
			if (c <= rig.stem_bones) {
				rig.stem[c] = 1;
				rig.stiffness[c] = data.stem_stiffness *
						(1.0f - (1.0f - data.stem_stiffness_spread) * float(c) / float(rig.stem_bones));
				rig.wind[c] = data.wind_stem;
			} else if (p_parent <= rig.stem_bones) {
				rig.stiffness[c] = data.branch_stiffness;
				rig.wind[c] = data.wind_branch;
			} else {
				rig.stiffness[c] = data.branch_stiffness_l2;
				rig.wind[c] = data.wind_branch_l2;
			}
			rig.height[c] = running;
			float out = std::max(0.0f, vec::dot(parent.up, y)) * reach_length + p_height;
			if (fork && short_tip[c] == 0 && p_parent <= rig.stem_bones) {
				out = 0.0f;
			}
			running = out;
			visit(c, out);
		}
	}
};
}

core::Result<TreeRig> build_tree_rig(const TreeRigData &p_data) {
	const size_t n = p_data.translations.size();
	if (n == 0 || p_data.parents.size() != n || p_data.locals.size() != n) {
		return core::make_error(
				"VegetationTreeEntityData: Hierarchy, BasePoseTransforms and BasePoseTranslations differ in count");
	}
	if (p_data.stem_bones < 1) {
		return core::make_error("VegetationTreeEntityData.StemBoneCount is not a positive count");
	}
	TreeRig rig;
	rig.stem_bones = p_data.stem_bones;
	rig.ground = p_data.ground;
	rig.children.resize(n);
	rig.tips.assign(n, 0);
	rig.rest_frames.assign(n, Transform{});
	rig.rest_frames_inverse.assign(n, Transform{});
	rig.offsets.assign(n, Vec3{});
	rig.rest_directions.assign(n, Vec3{ 0.0f, 1.0f, 0.0f });
	rig.sides.assign(n, Vec3{ 0.0f, 0.0f, 1.0f });
	rig.stiffness.assign(n, p_data.stem_stiffness);
	rig.wind.assign(n, p_data.wind_stem);
	rig.height.assign(n, 0.0f);
	rig.length.assign(n, 0.0f);
	rig.stem.assign(n, 0);
	rig.rigid.assign(n, 0);
	rig.descendants.resize(n);
	rig.stem[0] = 1;
	for (size_t i = 0; i < n; ++i) {
		const int32_t parent = p_data.parents[i];
		if (i == 0 ? parent != -1 : (parent < 0 || parent >= int32_t(i))) {
			return core::make_error("VegetationTreeEntityData.Hierarchy[" + std::to_string(i) +
					"] is not an earlier bone (the root's is not -1)");
		}
		rig.parents.push_back(parent);
		rig.rest.push_back(i == 0 ? Transform{} : rig.rest[size_t(parent)].then_inner(p_data.locals[i]));
		if (i > 0 && rig.children[size_t(parent)].size() < kTreeMaxChildren) {
			rig.children[size_t(parent)].push_back(uint32_t(i));
		}
	}
	RigBuilder builder{ rig, p_data, std::vector<uint8_t>(n, 0) };
	builder.visit(0, 0.0f);
	for (size_t i = 0; i < n; ++i) {
		if (!(rig.stiffness[i] > 0.0f)) {
			return core::make_error("VegetationTreeEntityData: bone " + std::to_string(i) + " has stiffness " +
					std::to_string(rig.stiffness[i]));
		}
		for (int32_t a = rig.parents[i]; a >= 0; a = rig.parents[size_t(a)]) {
			rig.descendants[size_t(a)].push_back(uint32_t(i));
			if (a == 1 && i >= 2) {
				rig.driven.push_back(uint32_t(i));
			}
		}
	}
	return rig;
}

core::Result<TreeRig> read_tree_rig(const EbxInstance &p_tree) {
	TreeRigData data;
	core::Result<std::vector<Vec3>> translations = field_translations(p_tree);
	if (!translations) {
		return translations.error();
	}
	data.translations = std::move(*translations);
	const std::optional<EbxValue> hierarchy = p_tree.get("Hierarchy");
	const std::optional<EbxValue> poses = p_tree.get("BasePoseTransforms");
	const core::OptionalEbxArray parents = hierarchy ? hierarchy->as_array() : core::OptionalEbxArray();
	const core::OptionalEbxArray locals = poses ? poses->as_array() : core::OptionalEbxArray();
	if (!parents || !locals) {
		return core::make_error("VegetationTreeEntityData: Hierarchy or BasePoseTransforms is missing");
	}
	for (size_t i = 0; i < parents->size(); ++i) {
		const std::optional<EbxValue> p = parents->get(i);
		const std::optional<int64_t> parent = p ? p->as_i64() : std::nullopt;
		if (!parent || *parent < -1 || *parent > int64_t(INT32_MAX)) {
			return core::make_error(
					"VegetationTreeEntityData.Hierarchy[" + std::to_string(i) + "] is not a bone index");
		}
		data.parents.push_back(int32_t(*parent));
	}
	for (size_t i = 0; i < locals->size(); ++i) {
		const std::optional<EbxValue> l = locals->get(i);
		const std::optional<std::array<Vec3, 4>> rows = l ? l->as_transform() : std::nullopt;
		if (!rows) {
			return core::make_error(
					"VegetationTreeEntityData.BasePoseTransforms[" + std::to_string(i) + "] is not a LinearTransform");
		}
		data.locals.push_back(Transform::from_axes({ (*rows)[0], (*rows)[1], (*rows)[2] }, (*rows)[3]));
	}
	const std::optional<EbxValue> stem_count = p_tree.get("StemBoneCount");
	const std::optional<int64_t> stem_bones = stem_count ? stem_count->as_i64() : std::nullopt;
	if (!stem_bones || *stem_bones < 1 || *stem_bones > int64_t(UINT32_MAX)) {
		return core::make_error("VegetationTreeEntityData.StemBoneCount is not a positive count");
	}
	data.stem_bones = uint32_t(*stem_bones);
	struct Field {
		std::string_view name;
		float TreeRigData::*value;
	};
	constexpr Field kFields[9] = { { "StemStiffness", &TreeRigData::stem_stiffness },
		{ "StemStiffnessSpread", &TreeRigData::stem_stiffness_spread },
		{ "BranchStiffness", &TreeRigData::branch_stiffness },
		{ "BranchStiffnessL2", &TreeRigData::branch_stiffness_l2 }, { "WindStem", &TreeRigData::wind_stem },
		{ "WindBranch", &TreeRigData::wind_branch }, { "WindBranchL2", &TreeRigData::wind_branch_l2 },
		{ "BreakableJointThreshold", &TreeRigData::joint_threshold }, { "ColiPlaneDepth", &TreeRigData::ground } };
	for (const Field &f : kFields) {
		core::Result<float> v = field_number(p_tree, f.name);
		if (!v) {
			return v.error();
		}
		data.*f.value = *v;
	}
	return build_tree_rig(data);
}

// SOURCE: notes R5-VG2 A3: direction (cos a, 0, sin a) and R = (S x 0.5 + 1) x 0.5 x WindVariationRate.
// INFERRED: the level's wind is WindComponentData's WindDirection and WindStrength. Settle: the blended state traced.
TreeWind tree_wind(const VegetationSystemSettings &p_settings, const std::optional<LevelWind> &p_level) {
	TreeWind w;
	w.variation = p_settings.wind_variation;
	if (p_level) {
		const float a = p_level->direction_degrees * core::kDegToRadF;
		w.direction = { std::cos(a), 0.0f, std::sin(a) };
		w.strength = p_level->strength;
		w.rate = (w.strength * 0.5f + 1.0f) * 0.5f * p_settings.wind_variation_rate;
	} else {
		w.direction = kTreeDefaultWindDirection;
		w.strength = p_settings.wind_strength;
		w.rate = p_settings.wind_variation_rate;
	}
	return w;
}

// SOURCE: notes R5-VG2 A4: four sines of a travelling phase, G held at V - 1 at least, and g x 0.004 x S along the wind.
Vec3 tree_gust(const TreeWind &p_wind, const Vec3 &p_tree_position, float p_clock) {
	const Vec3 p = vec::add(p_tree_position, Vec3{ 0.0f, kTreeWindHeight, 0.0f });
	const float u = p_wind.rate * p_clock - kTreeGustTravel * vec::dot(p_wind.direction, p);
	float g = 2.0f;
	for (const float f : kTreeGustFrequencies) {
		g += std::sin(f * u);
	}
	const float v = p_wind.variation;
	if (g + (1.0f - v) < 0.0f) {
		g = v - 1.0f;
	}
	return vec::scale(p_wind.direction, (g * v + (1.0f - v)) * kTreeForceScale * p_wind.strength);
}

// SOURCE: notes R6-VG2-bones S0: the force is taken into the tree frame divided by the instance scale.
Vec3 tree_local_force(const core::level::LevelTransform &p_tree, const Vec3 &p_force) {
	const float scale = p_tree.axis_scales().x;
	return scale > 0.0f ? vec::scale(p_force, 1.0f / scale) : p_force;
}

Transform tree_root_frame(const core::level::LevelTransform &p_tree) {
	return Transform{ vec::scale(p_tree.right, p_tree.scale), vec::scale(p_tree.up, p_tree.scale),
		vec::scale(p_tree.forward, p_tree.scale), Vec3{} };
}

TreeSim::TreeSim(std::shared_ptr<const TreeRig> p_rig, const Transform &p_root) : rig_(std::move(p_rig)) {
	const size_t n = rig_->bones();
	positions_.reserve(n);
	for (const Transform &t : rig_->rest) {
		positions_.push_back(t.trans);
	}
	velocities_.assign(n, Vec3{});
	frames_ = rig_->rest_frames;
	if (n > 0) {
		frames_[0] = Transform{ p_root.right, p_root.up, p_root.forward, Vec3{} };
	}
}

void TreeSim::hold_above_ground(uint32_t p_node) {
	if (rig_->stem[p_node] != 0 || positions_[p_node].y >= rig_->ground) {
		return;
	}
	Vec3 &v = velocities_[p_node];
	v = { v.x * kTreeGroundFriction, 0.0f, v.z * kTreeGroundFriction };
	positions_[p_node].y = rig_->ground;
}

// SOURCE: notes R6-VG2-bones S2 3d: f = dt (1 - c^2) 200 m K toward the rest direction in the parent's current frame.
void TreeSim::restore(uint32_t p_bone, const Vec3 &p_direction, const Transform &p_parent) {
	const TreeRig &r = *rig_;
	const Vec3 rest = vec::normalize(p_parent.transform_vector(r.rest_directions[p_bone]));
	const float c = std::fabs(vec::dot(p_direction, rest));
	const float bend = 1.0f - c * c;
	if (!(bend > kTreeThreshold)) {
		return;
	}
	const Vec3 axis = vec::cross(p_direction, rest);
	if (vec::length(axis) < kTreeThreshold) {
		return;
	}
	const Vec3 toward = vec::cross(axis, p_direction);
	if (vec::length(toward) < kTreeThreshold) {
		return;
	}
	const Vec3 n = vec::normalize(toward);
	const float f = kTreeStep * bend * kTreeRestoring * r.length[p_bone] * r.stiffness[p_bone];
	const Vec3 base = frames_[p_bone].trans;
	for (const uint32_t j : r.descendants[p_bone]) {
		const float lever = vec::dot(vec::sub(frames_[j].trans, base), p_direction);
		const Vec3 d = vec::scale(n, lever * f * r.stiffness[j]);
		velocities_[j] = vec::add(velocities_[j], d);
		positions_[j] = vec::add(positions_[j], vec::scale(d, kTreeRestoringStep));
		hold_above_ground(j);
	}
}

// SOURCE: notes R6-VG2-bones S2 3: ground clamp, tip re-projection, R2 hold, R1 snap, restoring, then the bone frame.
void TreeSim::rebuild(uint32_t p_parent) {
	const TreeRig &r = *rig_;
	const Transform parent = frames_[p_parent];
	const bool fork = r.children[p_parent].size() > 1;
	for (const uint32_t c : r.children[p_parent]) {
		const uint32_t t = r.tips[c];
		hold_above_ground(t);
		const Vec3 reach = vec::sub(positions_[t], positions_[c]);
		const float reach_length = vec::length(reach);
		Vec3 a = reach_length > kTreeDegenerateBone ? vec::scale(reach, 1.0f / reach_length) : parent.up;
		if (r.rigid[t] != 0) {
			a = parent.transform_vector(r.rest_directions[c]);
			positions_[t] = vec::add(positions_[c], vec::scale(a, r.length[c]));
		} else if (r.children[c].size() == 1) {
			positions_[t] = vec::add(positions_[c], vec::scale(a, r.length[c]));
		}
		if (fork || r.rigid[c] != 0) {
			positions_[c] = parent.transform_point(r.offsets[c]);
		}
		if (r.rigid[t] == 0) {
			restore(c, a, parent);
		}
		const Vec3 side = parent.transform_vector(r.sides[c]);
		const Vec3 x = vec::normalize(vec::cross(a, side));
		frames_[c] = Transform{ x, a, vec::normalize(vec::cross(x, a)), positions_[c] };
		rebuild(c);
	}
}

// SOURCE: notes R6-VG2-bones S2 1 and 2: the wind push on node 1's subtree, then the free nodes integrated.
// STAND-IN: no detached or frozen nodes, local influences or shader wiggle (notes R5-VG2 A5). Settle: those built.
void TreeSim::step(const Vec3 &p_local_force) {
	const TreeRig &r = *rig_;
	for (const uint32_t j : r.driven) {
		const float weight = (r.height[j] + 1.0f) * 0.5f * r.wind[j] / r.stiffness[j];
		velocities_[j] = vec::add(velocities_[j], vec::scale(p_local_force, weight));
	}
	for (const uint32_t j : r.driven) {
		if (r.rigid[j] != 0) {
			continue;
		}
		Vec3 v = velocities_[j];
		if (j > 2) {
			v.y -= kTreeGravity;
		}
		positions_[j] = vec::add(positions_[j], vec::scale(v, kTreeStep));
		velocities_[j] = vec::scale(v, 1.0f / (kTreeStep + kTreeDampingBase));
	}
	if (!positions_.empty()) {
		rebuild(0);
	}
}

std::vector<Transform> TreeSim::bone_offsets() const {
	std::vector<Transform> out(frames_.size());
	for (size_t i = 1; i < frames_.size(); ++i) {
		out[i] = frames_[i].then_inner(rig_->rest_frames_inverse[i]);
	}
	return out;
}

// SOURCE: notes R6-VG2-bones S2 3f: a bone's skin is its rest frame's inverse, its frame and the tree matrix.
// WHY: Godot poses a skeleton under the instance matrix, so the skin is taken back through its inverse.
std::vector<Transform> tree_bone_poses(const TreeSim &p_sim, const core::level::LevelTransform &p_tree) {
	const float scale = p_tree.axis_scales().x;
	const Transform tree{ Vec3{ scale, 0.0f, 0.0f }, Vec3{ 0.0f, scale, 0.0f }, Vec3{ 0.0f, 0.0f, scale },
		p_tree.trans };
	const Transform instance{ vec::scale(p_tree.right, p_tree.scale), vec::scale(p_tree.up, p_tree.scale),
		vec::scale(p_tree.forward, p_tree.scale), p_tree.trans };
	const Transform back = instance.inverse().then_inner(tree);
	std::vector<Transform> out = p_sim.bone_offsets();
	for (Transform &t : out) {
		t = back.then_inner(t);
	}
	return out;
}

std::optional<uint32_t> add_level_tree(LevelTrees &r_trees, const core::level::PlacedObject &p_object,
		const core::level::LevelTransform &p_transform, size_t p_object_index) {
	if (!p_object.mesh || p_object.mesh->entity_type != core::level::MeshEntityType::VegetationTree ||
			!p_object.entity.is_a("VegetationTreeEntityData")) {
		return std::nullopt;
	}
	const auto seen = std::find(r_trees.entities.begin(), r_trees.entities.end(), p_object.entity);
	const size_t rig = size_t(seen - r_trees.entities.begin());
	if (seen == r_trees.entities.end()) {
		core::Result<TreeRig> parsed = read_tree_rig(p_object.entity);
		r_trees.entities.push_back(p_object.entity);
		r_trees.rigs.push_back(parsed ? std::make_shared<const TreeRig>(std::move(*parsed)) : nullptr);
		if (!parsed) {
			r_trees.failures.push_back(parsed.error().message);
		}
	}
	if (r_trees.rigs[rig] == nullptr) {
		return std::nullopt;
	}
	r_trees.trees.push_back(TreePlacement{ uint32_t(rig), p_transform });
	r_trees.objects.push_back(p_object_index);
	return uint32_t(r_trees.trees.size() - 1);
}

LevelTrees level_trees(const core::level::World &p_world) {
	LevelTrees out;
	for (size_t o = 0; o < p_world.objects.size(); ++o) {
		(void)add_level_tree(out, p_world.objects[o], p_world.objects[o].transform, o);
	}
	return out;
}

TreeForest::TreeForest(std::vector<std::shared_ptr<const TreeRig>> p_rigs, std::vector<TreePlacement> p_trees,
		const VegetationSystemSettings &p_settings) :
		rigs_(std::move(p_rigs)), trees_(std::move(p_trees)), states_(trees_.size()), settings_(p_settings) {}

size_t TreeForest::active() const {
	return size_t(std::count_if(states_.begin(), states_.end(),
			[this](const State &p_s) { return p_s.sim != nullptr && frames_ - p_s.touched_frame <= kTreeIdleFrames; }));
}

// SOURCE: notes R5-VG2 A1 and A2, and R6-VG2-rate rule 1 with its checker's pre-simulation budget.
// INFERRED: the activation budget's worker count is JobCount. Settle: the job system's worker count traced.
void TreeForest::frame(float p_dt, const Vec3 &p_camera, const TreeWind &p_wind) {
	++frames_;
	const float advance = std::clamp(settings_.time_scale * p_dt, 0.0f, kTreeMaxAdvance);
	clock_ += advance;
	const bool moved = !last_camera_ || vec::length(vec::sub(p_camera, *last_camera_)) > kTreePreSimulationMove;
	last_camera_ = p_camera;
	if (!settings_.draw) {
		return;
	}
	const uint32_t budget = (settings_.job_count + 1) * settings_.max_pre_sims_per_job;
	// STAND-IN: one pre-simulation budget for the frame (MaxPreSimsPerJob x 8), not one per job call.
	// Settle: how the job calls split the trees traced.
	const uint32_t pre_simulations = settings_.max_pre_sims_per_job * kTreeJobsPerFrame;
	uint32_t activated = 0, pre_simulated = 0;
	std::vector<uint32_t> due;
	for (size_t t = 0; t < trees_.size(); ++t) {
		State &s = states_[t];
		const TreePlacement &tree = trees_[t];
		if (vec::length(vec::sub(tree.transform.trans, p_camera)) <= settings_.max_active_distance &&
				tree.rig < rigs_.size() && rigs_[tree.rig] != nullptr) {
			if (s.sim == nullptr && activated < budget) {
				s.sim = std::make_unique<TreeSim>(rigs_[tree.rig], tree_root_frame(tree.transform));
				s.pending = moved ? kTreePreSimulation : 0.0f;
				s.wants_pre_simulation = !moved;
				s.posed = moved;
				++activated;
			}
			// INFERRED: a tree within MaxActiveDistance counts as refreshed by the draw pass, drawn or not.
			// Settle: the draw/cull pass's frame stamp traced.
			if (s.sim != nullptr) {
				s.touched_frame = frames_;
			}
		}
		if (s.sim == nullptr || frames_ - s.touched_frame > kTreeIdleFrames) {
			continue;
		}
		if (s.wants_pre_simulation && pre_simulated < pre_simulations) {
			s.pending += kTreePreSimulation;
			s.wants_pre_simulation = false;
			s.posed = true;
			++pre_simulated;
		}
		s.pending += advance;
		s.stepped_frame = frames_;
		due.push_back(uint32_t(t));
	}
	std::vector<uint64_t> stepped(due.size(), 0);
	core::parallel_for(
			due.size(),
			[&](size_t k) {
				State &s = states_[due[k]];
				const TreePlacement &tree = trees_[due[k]];
				const Vec3 force = tree_local_force(tree.transform, tree_gust(p_wind, tree.transform.trans, clock_));
				while (s.pending >= kTreeStep) {
					s.sim->step(force);
					s.pending -= kTreeStep;
					++stepped[k];
				}
			},
			kTreeLanes);
	for (const uint64_t n : stepped) {
		steps_ += n;
	}
}

}
