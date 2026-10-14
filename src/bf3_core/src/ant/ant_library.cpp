// Ant library: references, controllers, DOF sets, rigs. Clean-room implementation from
// research/probes/ant-animation-probe.md (§3.2, §5); see ant_library.h.
#include "bf3/core/ant/ant_library.h"

#include <algorithm>

namespace bf3::core::ant {

namespace {

constexpr uint32_t kSlotQuat = 14;
constexpr uint32_t kSlotFloat = 10;
constexpr uint32_t kSlotInt = 6;
// LayoutEntry.Type values as stored (2049856663 and 2049856454; research/topics/03-animation.md §1.4).
// The probe report's hex spelling (0x7A2E5A97 / 0x7A2E59C6) is a typo; the data uses these.
constexpr uint32_t kSlotTranslation = 0x7A2E5497;
constexpr uint32_t kSlotScale = 0x7A2E53C6;
constexpr uint32_t kDeltaTrajDofs = 8;
constexpr int kMaxHierarchyDepth = 64;

std::string_view joint_of_slot(std::string_view p_slot) {
	if (p_slot.size() > 2 && p_slot[p_slot.size() - 2] == '.') {
		const char c = p_slot.back();
		if (c == 'q' || c == 't' || c == 's') {
			return p_slot.substr(0, p_slot.size() - 2);
		}
	}
	return {};
}

// "<Layout>.<Joint>.q" -> "<Joint>.q"
std::string_view strip_layout(std::string_view p_dof_name) {
	const size_t dot = p_dof_name.find('.');
	return dot == std::string_view::npos ? std::string_view() : p_dof_name.substr(dot + 1);
}

} // namespace

const char *dof_kind_name(DofKind p_kind) {
	switch (p_kind) {
		case DofKind::Rotation:
			return "rotation";
		case DofKind::Translation:
			return "translation";
		case DofKind::Scale:
			return "scale";
		case DofKind::Float:
			return "float";
		case DofKind::Int:
			return "int";
		case DofKind::DeltaTrajectory:
			return "deltaTrajectory";
		case DofKind::Other:
			break;
	}
	return "other";
}

DofKind dof_kind_from_slot_type(uint32_t p_type) {
	switch (p_type) {
		case kSlotQuat:
			return DofKind::Rotation;
		case kSlotTranslation:
			return DofKind::Translation;
		case kSlotScale:
			return DofKind::Scale;
		case kSlotFloat:
			return DofKind::Float;
		case kSlotInt:
			return DofKind::Int;
		default:
			return DofKind::Other;
	}
}

int32_t AntRig::find_joint(std::string_view p_name) const {
	const auto it = joint_index.find(p_name);
	return it == joint_index.end() ? -1 : it->second;
}

const AntPackage &AntLibrary::package_of(const AntPackage &p_from, const GdGuid &p_guid) const {
	return (p_guid.is_static() && static_) ? *static_ : p_from;
}

GdStruct AntLibrary::resolve(const AntPackage &p_from, const GdGuid &p_guid) const {
	if (p_guid.is_null()) {
		return {};
	}
	return package_of(p_from, p_guid).find(p_guid.index());
}

std::vector<uint32_t> AntLibrary::clips(const AntPackage &p_package) {
	std::vector<uint32_t> out;
	for (size_t i = 0; i < p_package.object_count(); ++i) {
		if (is_clip(p_package.object(i))) {
			out.push_back(uint32_t(i));
		}
	}
	return out;
}

GdStruct AntLibrary::find_clip(const AntPackage &p_package, std::string_view p_name) {
	for (size_t i = 0; i < p_package.object_count(); ++i) {
		const GdStruct o = p_package.object(i);
		if (o.name() == p_name && is_clip(o)) {
			return o;
		}
	}
	return {};
}

AntResult AntLibrary::set_static(std::unique_ptr<AntPackage> p_static) {
	static_ = std::move(p_static);
	rigs_.clear();
	if (!static_) {
		return AntResult::fail(AntStatus::NotFound, "no static package");
	}
	const AntPackage &s = *static_;
	for (size_t i = 0; i < s.object_count(); ++i) {
		const GdStruct rig = s.object(i);
		if (rig.type_name() != "RigAsset") {
			continue;
		}
		AntRig r;
		r.name = rig.name();
		GdGuid g;
		const GdStruct skeleton = rig.read_guid("Skeleton", g) ? resolve(s, g) : GdStruct();
		if (skeleton.valid()) {
			r.skeleton_name = skeleton.name();
			const GdArray joints = skeleton.array("Joints");
			r.joints.reserve(joints.size());
			for (uint32_t j = 0; j < joints.size(); ++j) {
				const GdStruct info = joints.element(j);
				AntJoint joint;
				int64_t v = -1;
				info.read_string("JointName", joint.name);
				if (info.read_int("ParentIndex", v)) {
					joint.parent = int32_t(v);
				}
				if (info.read_int("JointType", v)) {
					joint.type = int32_t(v);
				}
				r.joint_index.emplace(joint.name, int32_t(r.joints.size()));
				r.joints.push_back(joint);
			}
		}
		// Rest pose from the rig's DOF defaults ("<Layout>.<Joint>.q/.t"); the first entry per joint wins.
		std::unordered_map<std::string_view, uint32_t> seen;
		for (const char *list : { "DefaultVector4Values", "DefaultVector3Values" }) {
			const GdArray values = rig.array(list);
			for (uint32_t k = 0; k < values.size(); ++k) {
				const GdStruct e = values.element(k);
				std::string_view dof_name;
				float value[4];
				if (!e.read_string("DofName", dof_name) || !e.read_vec4("Value", value)) {
					continue;
				}
				const std::string_view slot = strip_layout(dof_name);
				const std::string_view joint = joint_of_slot(slot);
				const int32_t ji = r.find_joint(joint);
				if (ji < 0 || !seen.emplace(slot, 1).second) {
					continue;
				}
				if (slot.back() == 'q') {
					std::copy(value, value + 4, r.joints[size_t(ji)].rest_q);
				} else if (slot.back() == 't') {
					std::copy(value, value + 3, r.joints[size_t(ji)].rest_t);
				}
			}
		}
		// Every hierarchy reachable from DofSetLists through Children, and the layouts they use.
		const GdArray lists = rig.array("DofSetLists");
		std::vector<std::pair<GdGuid, int>> stack;
		for (uint32_t k = 0; k < lists.size(); ++k) {
			if (lists.read_guid(k, g)) {
				stack.emplace_back(g, 0);
			}
		}
		while (!stack.empty()) {
			const auto [hg, depth] = stack.back();
			stack.pop_back();
			if (!hg.is_static() || depth > kMaxHierarchyDepth) {
				continue;
			}
			const GdStruct h = resolve(s, hg);
			if (h.type_name() != "LayoutHierarchyAsset" || std::find(r.hierarchies.begin(), r.hierarchies.end(), hg.index()) != r.hierarchies.end()) {
				continue;
			}
			r.hierarchies.push_back(hg.index());
			const GdArray layouts = h.array("LayoutAssets");
			for (uint32_t k = 0; k < layouts.size(); ++k) {
				GdGuid l;
				if (layouts.read_guid(k, l)) {
					r.layouts.push_back(l.index());
				}
			}
			const GdArray children = h.array("Children");
			for (uint32_t k = 0; k < children.size(); ++k) {
				GdGuid c;
				if (children.read_guid(k, c)) {
					stack.emplace_back(c, depth + 1);
				}
			}
		}
		std::sort(r.layouts.begin(), r.layouts.end());
		r.layouts.erase(std::unique(r.layouts.begin(), r.layouts.end()), r.layouts.end());
		rigs_.push_back(std::move(r));
	}
	return {};
}

AntResult AntLibrary::dofs_of(const AntPackage &p_from, const GdStruct &p_hierarchy, std::vector<AntDof> &r_dofs) const {
	r_dofs.clear();
	if (p_hierarchy.type_name() != "LayoutHierarchyAsset") {
		return AntResult::fail(AntStatus::Malformed, "target is not a LayoutHierarchyAsset");
	}
	const GdArray layouts = p_hierarchy.array("LayoutAssets");
	for (uint32_t i = 0; i < layouts.size(); ++i) {
		GdGuid g;
		if (!layouts.read_guid(i, g)) {
			return AntResult::fail(AntStatus::Malformed, "bad LayoutAssets entry");
		}
		const GdStruct layout = resolve(p_from, g);
		if (!layout.valid()) {
			return AntResult::fail(AntStatus::Malformed, "unresolved LayoutAssets reference");
		}
		if (layout.type_name() == "DeltaTrajLayoutAsset") {
			for (uint32_t k = 0; k < kDeltaTrajDofs; ++k) {
				r_dofs.push_back(AntDof{ layout.name(), {}, {}, DofKind::DeltaTrajectory, 0 });
			}
			continue;
		}
		const GdArray slots = layout.array("Slots");
		for (uint32_t k = 0; k < slots.size(); ++k) {
			const GdStruct e = slots.element(k);
			AntDof dof;
			dof.layout = layout.name();
			e.read_string("Name", dof.slot);
			e.read_u32("Type", dof.type);
			dof.kind = dof_kind_from_slot_type(dof.type);
			if (dof.kind == DofKind::Rotation || dof.kind == DofKind::Translation || dof.kind == DofKind::Scale) {
				dof.joint = joint_of_slot(dof.slot);
			}
			r_dofs.push_back(dof);
		}
	}
	return {};
}

AntResult AntLibrary::bind(const AntPackage &p_package, const GdStruct &p_clip, ClipBinding &r_binding) const {
	r_binding = {};
	r_binding.clip = p_clip;
	const GdGuid clip_guid = p_clip.guid();
	// Package-local controller first, then the static package's.
	GdStruct controller = p_package.controller_for(clip_guid.index());
	const AntPackage *controller_package = &p_package;
	if (!controller.valid() && static_ && static_.get() != &p_package) {
		controller = static_->controller_for(clip_guid.index());
		controller_package = static_.get();
	}
	if (!controller.valid()) {
		return AntResult::fail(AntStatus::NotFound, "no ClipControllerAsset references this clip");
	}
	r_binding.controller = controller;
	r_binding.controller_name = controller.name();
	controller.read_float("FPS", r_binding.fps);
	controller.read_float("FPSScale", r_binding.fps_scale);
	controller.read_float("NumTicks", r_binding.num_ticks);
	GdGuid target_guid;
	if (!controller.read_guid("Target", target_guid)) {
		return AntResult::fail(AntStatus::Malformed, "controller without Target");
	}
	const AntPackage &target_package = package_of(*controller_package, target_guid);
	r_binding.target = target_package.find(target_guid.index());
	if (!r_binding.target.valid()) {
		return AntResult::fail(AntStatus::Malformed, "unresolved controller Target");
	}
	r_binding.target_name = r_binding.target.name();
	AntResult r = dofs_of(target_package, r_binding.target, r_binding.dofs);
	if (!r.ok()) {
		return r;
	}

	// Channel -> DOF.
	ClipHeader header;
	if (!read_clip_header(p_clip, header)) {
		return AntResult::fail(AntStatus::Malformed, "clip without AnimationAsset header");
	}
	r_binding.channel_to_dof = resolve(p_package, header.channel_to_dof);
	if (r_binding.channel_to_dof.type_name() != "ChannelToDofAsset") {
		return AntResult::fail(AntStatus::Malformed, "unresolved ChannelToDofAsset");
	}
	uint32_t storage = 0;
	std::vector<uint8_t> index_data;
	if (!r_binding.channel_to_dof.read_u32("StorageType", storage) || !r_binding.channel_to_dof.array("IndexData").copy_u8(index_data)) {
		return AntResult::fail(AntStatus::Malformed, "bad ChannelToDofAsset");
	}
	if (storage != 1 && storage != 2) {
		return AntResult::fail(AntStatus::Unsupported, "ChannelToDofAsset.StorageType " + std::to_string(storage));
	}
	uint32_t nq = 0, nv = 0, nf = 0;
	const bool counts = clip_channel_counts(p_clip, nq, nv, nf);
	const size_t channels = storage == 1 ? index_data.size() : index_data.size() / 2;
	if (counts && channels != size_t(nq) + nv + nf) {
		return AntResult::fail(AntStatus::Malformed, "ChannelToDofAsset has " + std::to_string(channels) + " channels, the clip " +
				std::to_string(size_t(nq) + nv + nf));
	}

	// Rig: the one whose DofSetLists hierarchies use most of the target's layouts (the targets themselves
	// are not in those trees); ties go to the most joints found by name, then to the first rig.
	{
		std::vector<uint32_t> target_layouts;
		const GdArray layouts = r_binding.target.array("LayoutAssets");
		for (uint32_t k = 0; k < layouts.size(); ++k) {
			GdGuid l;
			if (layouts.read_guid(k, l) && l.is_static()) {
				target_layouts.push_back(l.index());
			}
		}
		size_t best_layouts = 0, best_joints = 0;
		for (const AntRig &rig : rigs_) {
			size_t layout_hits = 0, joint_hits = 0;
			for (uint32_t l : target_layouts) {
				layout_hits += std::binary_search(rig.layouts.begin(), rig.layouts.end(), l) ? 1 : 0;
			}
			for (const AntDof &d : r_binding.dofs) {
				joint_hits += (!d.joint.empty() && rig.find_joint(d.joint) >= 0) ? 1 : 0;
			}
			if (layout_hits + joint_hits == 0) {
				continue;
			}
			if (r_binding.rig == nullptr || layout_hits > best_layouts || (layout_hits == best_layouts && joint_hits > best_joints)) {
				r_binding.rig = &rig;
				best_layouts = layout_hits;
				best_joints = joint_hits;
			}
		}
		r_binding.rig_layout_hits = uint32_t(best_layouts);
	}

	r_binding.tracks.resize(channels);
	for (size_t c = 0; c < channels; ++c) {
		ChannelTrack &t = r_binding.tracks[c];
		t.channel = uint32_t(c);
		t.kind = !counts ? '?' : c < nq ? 'q' : c < size_t(nq) + nv ? 'v' : 'f';
		t.dof = storage == 1 ? index_data[c] : (uint32_t(index_data[2 * c]) << 8) | index_data[2 * c + 1];
		if (t.dof >= r_binding.dofs.size()) {
			return AntResult::fail(AntStatus::Malformed, "channel " + std::to_string(c) + " maps outside the DOF set");
		}
		const AntDof &dof = r_binding.dofs[t.dof];
		switch (dof.kind) {
			case DofKind::Rotation:
				t.kind_ok = t.kind == 'q';
				break;
			case DofKind::Translation:
			case DofKind::Scale:
				t.kind_ok = t.kind == 'v';
				break;
			case DofKind::Float:
				t.kind_ok = t.kind == 'f';
				break;
			case DofKind::DeltaTrajectory:
				++r_binding.delta_traj_channels;
				break;
			default:
				break;
		}
		r_binding.kind_mismatches += t.kind_ok ? 0 : 1;
		t.joint = (r_binding.rig && !dof.joint.empty()) ? r_binding.rig->find_joint(dof.joint) : -1;
	}
	return {};
}

} // namespace bf3::core::ant
