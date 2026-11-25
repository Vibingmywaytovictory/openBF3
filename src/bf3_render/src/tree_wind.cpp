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

Vec3 rotated(const Vec3 &p_v, const Vec3 &p_from, const Vec3 &p_to) {
	const Vec3 k = vec::cross(p_from, p_to);
	const float c = vec::dot(p_from, p_to);
	if (c < -1.0f + kTreeThreshold) {
		const Vec3 side = std::fabs(p_from.x) < 0.9f ? Vec3{ 1.0f, 0.0f, 0.0f } : Vec3{ 0.0f, 1.0f, 0.0f };
		const Vec3 axis = vec::normalize(vec::cross(p_from, side));
		return vec::sub(vec::scale(axis, 2.0f * vec::dot(axis, p_v)), p_v);
	}
	return vec::add(vec::add(vec::scale(p_v, c), vec::cross(k, p_v)), vec::scale(k, vec::dot(k, p_v) / (1.0f + c)));
}

Vec3 direction_or(const Vec3 &p_from, const Vec3 &p_to, const Vec3 &p_fallback) {
	const Vec3 d = vec::sub(p_to, p_from);
	const float len = vec::length(d);
	return len > kTreeThreshold ? vec::scale(d, 1.0f / len) : p_fallback;
}

}

core::Result<TreeRig> read_tree_rig(const EbxInstance &p_tree) {
	const std::optional<EbxValue> hierarchy = p_tree.get("Hierarchy");
	const std::optional<EbxValue> poses = p_tree.get("BasePoseTransforms");
	const core::OptionalEbxArray parents = hierarchy ? hierarchy->as_array() : core::OptionalEbxArray();
	const core::OptionalEbxArray locals = poses ? poses->as_array() : core::OptionalEbxArray();
	if (!parents || !locals || parents->empty() || parents->size() != locals->size()) {
		return core::make_error("VegetationTreeEntityData: Hierarchy and BasePoseTransforms differ or are empty");
	}
	const std::optional<EbxValue> stem_count = p_tree.get("StemBoneCount");
	const std::optional<int64_t> stem_bones = stem_count ? stem_count->as_i64() : std::nullopt;
	if (!stem_bones || *stem_bones < 1) {
		return core::make_error("VegetationTreeEntityData.StemBoneCount is not a positive count");
	}
	constexpr std::string_view kFields[7] = { "StemStiffness", "StemStiffnessSpread", "BranchStiffness",
		"BranchStiffnessL2", "WindStem", "WindBranch", "WindBranchL2" };
	float values[7] = {};
	for (size_t f = 0; f < 7; ++f) {
		core::Result<float> v = field_number(p_tree, kFields[f]);
		if (!v) {
			return v.error();
		}
		values[f] = *v;
	}
	const float stem_stiffness = values[0], stem_spread = values[1], branch_stiffness = values[2],
				branch_stiffness_l2 = values[3], wind_stem = values[4], wind_branch = values[5],
				wind_branch_l2 = values[6];

	TreeRig rig;
	rig.stem_bones = uint32_t(*stem_bones);
	const size_t n = parents->size();
	for (size_t i = 0; i < n; ++i) {
		const std::optional<EbxValue> p = parents->get(i);
		const std::optional<int64_t> parent = p ? p->as_i64() : std::nullopt;
		if (!parent || (i == 0 ? *parent != -1 : (*parent < 0 || *parent >= int64_t(i)))) {
			return core::make_error("VegetationTreeEntityData.Hierarchy[" + std::to_string(i) +
					"] is not an earlier bone (the root's is not -1)");
		}
		const std::optional<EbxValue> l = locals->get(i);
		const std::optional<std::array<Vec3, 4>> rows = l ? l->as_transform() : std::nullopt;
		if (!rows) {
			return core::make_error(
					"VegetationTreeEntityData.BasePoseTransforms[" + std::to_string(i) + "] is not a LinearTransform");
		}
		const Transform local = Transform::from_axes({ (*rows)[0], (*rows)[1], (*rows)[2] }, (*rows)[3]);
		rig.parents.push_back(int32_t(*parent));
		rig.rest.push_back(i == 0 ? local : rig.rest[size_t(*parent)].then_inner(local));
		rig.length.push_back(i == 0 ? 0.0f : vec::length(local.trans));
		rig.height.push_back(rig.rest.back().trans.y);
		const float node = float(i + 1);
		const bool stem = i + 1 <= rig.stem_bones;
		rig.stem.push_back(stem ? 1 : 0);
		if (stem) {
			rig.stiffness.push_back(stem_stiffness * (1.0f - (1.0f - stem_spread) * node / float(rig.stem_bones)));
			rig.wind.push_back(wind_stem);
		} else if (uint64_t(*parent) + 1 <= rig.stem_bones) {
			rig.stiffness.push_back(branch_stiffness);
			rig.wind.push_back(wind_branch);
		} else {
			rig.stiffness.push_back(branch_stiffness_l2);
			rig.wind.push_back(wind_branch_l2);
		}
		if (!(rig.stiffness.back() > 0.0f)) {
			return core::make_error("VegetationTreeEntityData: bone " + std::to_string(i) + " has stiffness " +
					std::to_string(rig.stiffness.back()));
		}
	}
	return rig;
}

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

Vec3 tree_local_force(const core::level::LevelTransform &p_tree, const Vec3 &p_force) {
	return { vec::dot(vec::normalize(p_tree.right), p_force), vec::dot(vec::normalize(p_tree.up), p_force),
		vec::dot(vec::normalize(p_tree.forward), p_force) };
}

TreeSim::TreeSim(std::shared_ptr<const TreeRig> p_rig) : rig_(std::move(p_rig)) {
	const size_t n = rig_->bones();
	positions_.reserve(n);
	for (const Transform &t : rig_->rest) {
		positions_.push_back(t.trans);
	}
	velocities_.assign(n, Vec3{});
	subtrees_.resize(n);
	for (size_t j = 0; j < n; ++j) {
		for (int32_t a = int32_t(j); a >= 0; a = rig_->parents[size_t(a)]) {
			subtrees_[size_t(a)].push_back(uint32_t(j));
		}
	}
}

void TreeSim::rebuild() {
	for (size_t i = 1; i < positions_.size(); ++i) {
		const Vec3 &pivot = positions_[size_t(rig_->parents[i])];
		const Vec3 rest_dir =
				direction_or(rig_->rest[size_t(rig_->parents[i])].trans, rig_->rest[i].trans, Vec3{ 0.0f, 1.0f, 0.0f });
		positions_[i] = vec::add(pivot, vec::scale(direction_or(pivot, positions_[i], rest_dir), rig_->length[i]));
	}
}

void TreeSim::restore() {
	const TreeRig &r = *rig_;
	for (size_t i = 1; i < positions_.size(); ++i) {
		const size_t p = size_t(r.parents[i]);
		const Vec3 bone = vec::sub(positions_[i], positions_[p]);
		const Vec3 rest_bone = vec::sub(r.rest[i].trans, r.rest[p].trans);
		if (vec::length(bone) <= kTreeThreshold || vec::length(rest_bone) <= kTreeThreshold) {
			continue;
		}
		const Vec3 a = vec::normalize(bone), rest = vec::normalize(rest_bone);
		const float c = std::fabs(vec::dot(a, rest));
		const float bend = 1.0f - c * c;
		if (bend <= kTreeThreshold) {
			continue;
		}
		const Vec3 toward = vec::sub(rest, vec::scale(a, vec::dot(a, rest)));
		if (vec::length(toward) <= kTreeThreshold) {
			continue;
		}
		const Vec3 tangent = vec::normalize(toward);
		const float f = kTreeStep * bend * kTreeRestoring * r.length[i] * r.stiffness[i];
		const Vec3 pivot = positions_[p];
		for (const uint32_t j : subtrees_[i]) {
			const float lever = vec::dot(vec::sub(positions_[j], pivot), a);
			const Vec3 d = vec::scale(tangent, lever * f * r.stiffness[j]);
			velocities_[j] = vec::add(velocities_[j], d);
			positions_[j] = vec::add(positions_[j], vec::scale(d, kTreeStep));
		}
	}
	for (size_t i = 1; i < positions_.size(); ++i) {
		if (r.stem[i] == 0 && positions_[i].y < 0.0f) {
			positions_[i].y = 0.0f;
			velocities_[i] = { velocities_[i].x * kTreeGroundFriction, 0.0f, velocities_[i].z * kTreeGroundFriction };
		}
	}
}

void TreeSim::step(const Vec3 &p_local_force) {
	const TreeRig &r = *rig_;
	for (size_t i = 1; i < positions_.size(); ++i) {
		const float weight = (r.height[i] + 1.0f) * 0.5f * r.wind[i] / r.stiffness[i];
		velocities_[i] = vec::add(velocities_[i], vec::scale(p_local_force, weight));
	}
	for (size_t i = 1; i < positions_.size(); ++i) {
		Vec3 v = velocities_[i];
		if (i + 1 > 2) {
			v.y -= kTreeGravity;
		}
		positions_[i] = vec::add(positions_[i], vec::scale(v, kTreeStep));
		velocities_[i] = vec::scale(v, 1.0f / (kTreeStep + kTreeDampingBase));
	}
	rebuild();
	restore();
	rebuild();
}

std::vector<Transform> TreeSim::bone_offsets() const {
	const TreeRig &r = *rig_;
	std::vector<Transform> out(positions_.size());
	for (size_t i = 0; i < positions_.size(); ++i) {
		Vec3 from{ 0.0f, 1.0f, 0.0f }, to = from;
		if (i > 0) {
			const size_t p = size_t(r.parents[i]);
			from = direction_or(r.rest[p].trans, r.rest[i].trans, from);
			to = direction_or(positions_[p], positions_[i], from);
		}
		Transform &t = out[i];
		t.right = rotated({ 1.0f, 0.0f, 0.0f }, from, to);
		t.up = rotated({ 0.0f, 1.0f, 0.0f }, from, to);
		t.forward = rotated({ 0.0f, 0.0f, 1.0f }, from, to);
		t.trans = vec::sub(positions_[i], t.transform_vector(r.rest[i].trans));
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
	uint32_t activated = 0;
	std::vector<uint32_t> due;
	for (size_t t = 0; t < trees_.size(); ++t) {
		State &s = states_[t];
		const TreePlacement &tree = trees_[t];
		if (vec::length(vec::sub(tree.transform.trans, p_camera)) <= settings_.max_active_distance &&
				tree.rig < rigs_.size() && rigs_[tree.rig] != nullptr) {
			if (s.sim == nullptr && activated < budget) {
				s.sim = std::make_unique<TreeSim>(rigs_[tree.rig]);
				s.pending = moved ? kTreePreSimulation : 0.0f;
				++activated;
			}
			if (s.sim != nullptr) {
				s.touched_frame = frames_;
			}
		}
		if (s.sim == nullptr || frames_ - s.touched_frame > kTreeIdleFrames) {
			continue;
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
