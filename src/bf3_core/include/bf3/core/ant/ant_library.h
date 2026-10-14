#pragma once
// Ant library: the static package (s_BasicAssets) plus per-package lookups that bind a clip to its
// ClipControllerAsset, DOF set, rig and skeleton joints.
//
// Clean-room implementation from research/probes/ant-animation-probe.md (§3.2 references, §5 mapping).
// No code from Rime, IceBloc or AssetBankPlugin was used.
//
// Mapping rules (§5):
//  * Guid references: bit 0x80000 of the big-endian index = object of the static package, else an object of
//    the referencing package. Each package resolves against itself + the static package only.
//  * A clip's controller is the ClipControllerAsset whose Anim references it (package-local first, then static).
//  * DOF set = Controller.Target (LayoutHierarchyAsset) -> LayoutAssets in order, each LayoutAsset's Slots in
//    stored order; a DeltaTrajLayoutAsset occupies 8 DOFs without slots. Children hierarchies are masks.
//  * ChannelToDofAsset: StorageType 1 = one u8 DOF index per channel, 2 = one big-endian u16 per channel.
//    Channel order: quaternions, vec3s, floats.
//  * Rig: the RigAsset whose DofSetLists hierarchies (with their Children) use most of the target's
//    LayoutAssets; ties go to the rig with most joints named by the DOFs.
//  * Slot "<Joint>.q|.t|.s" drives the Ant SkeletonAsset joint of that name; rest pose = the rig's
//    DefaultVector4Values / DefaultVector3Values entry "<Layout>.<Joint>.q|.t".

#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/ant/ant_package.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::ant {

enum class DofKind {
	Rotation, // LayoutEntry.Type 14 (quaternion)
	Translation, // 2049856663 (0x7A2E5497)
	Scale, // 2049856454 (0x7A2E53C6)
	Float, // 10
	Int, // 6
	DeltaTrajectory, // one of the 8 slot-less DOFs of a DeltaTrajLayoutAsset
	Other,
};
const char *dof_kind_name(DofKind p_kind);
DofKind dof_kind_from_slot_type(uint32_t p_type);

struct AntDof {
	std::string_view layout; // LayoutAsset name
	std::string_view slot; // e.g. "Neck.q" ("" for DeltaTrajectory DOFs)
	std::string_view joint; // slot without ".q"/".t"/".s" ("" for floats and ints)
	DofKind kind = DofKind::Other;
	uint32_t type = 0; // LayoutEntry.Type
};

struct AntJoint {
	std::string_view name;
	int32_t parent = -1;
	int32_t type = 0;
	float rest_t[3] = { 0.0f, 0.0f, 0.0f };
	float rest_q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct AntRig {
	std::string_view name; // RigAsset name, e.g. "Venice1pAntSke01"
	std::string_view skeleton_name; // Ant SkeletonAsset name, e.g. "Venice1pAntSke01_New"
	std::vector<AntJoint> joints;
	std::unordered_map<std::string_view, int32_t> joint_index;
	std::vector<uint32_t> hierarchies; // static reference indices of every LayoutHierarchyAsset under DofSetLists
	std::vector<uint32_t> layouts; // sorted reference indices of the LayoutAssets those hierarchies use

	int32_t find_joint(std::string_view p_name) const;
};

// One clip channel and where it lands.
struct ChannelTrack {
	uint32_t channel = 0; // codec channel index
	char kind = 'q'; // 'q' rotation, 'v' vec3, 'f' float (codec side)
	uint32_t dof = 0;
	int32_t joint = -1; // rig joint index (-1: no joint)
	bool kind_ok = false; // the DOF's kind matches the channel's kind
};

struct ClipBinding {
	GdStruct clip;
	GdStruct controller; // ClipControllerAsset (invalid: no controller found)
	GdStruct target; // LayoutHierarchyAsset
	GdStruct channel_to_dof;
	std::string_view controller_name;
	std::string_view target_name;
	float fps = 30.0f;
	float fps_scale = 1.0f;
	float num_ticks = 0.0f;
	const AntRig *rig = nullptr;
	uint32_t rig_layout_hits = 0; // target layouts that the chosen rig's DOF sets use
	std::vector<AntDof> dofs;
	std::vector<ChannelTrack> tracks; // one per channel, channel order
	uint32_t kind_mismatches = 0;
	uint32_t delta_traj_channels = 0;

	bool has_controller() const { return controller.valid(); }
};

class AntLibrary {
public:
	AntLibrary() = default;
	AntLibrary(const AntLibrary &) = delete;
	AntLibrary &operator=(const AntLibrary &) = delete;

	// Takes the parsed static package (s_BasicAssets) and indexes its rigs.
	AntResult set_static(std::unique_ptr<AntPackage> p_static);
	const AntPackage *static_package() const { return static_.get(); }
	const std::vector<AntRig> &rigs() const { return rigs_; }

	// Resolves a Guid reference made by an object of p_from (static bit -> static package).
	GdStruct resolve(const AntPackage &p_from, const GdGuid &p_guid) const;
	const AntPackage &package_of(const AntPackage &p_from, const GdGuid &p_guid) const;

	// Clips of a package (top-level objects deriving from AnimationAsset), in archive order.
	static std::vector<uint32_t> clips(const AntPackage &p_package);
	// Finds a clip by its "__name" (first match).
	static GdStruct find_clip(const AntPackage &p_package, std::string_view p_name);

	// Controller, DOF set, channel map and rig of a clip of p_package. Fails with NotFound when no
	// controller references the clip, Malformed when references or indices do not resolve.
	AntResult bind(const AntPackage &p_package, const GdStruct &p_clip, ClipBinding &r_binding) const;

	// DOF list of a LayoutHierarchyAsset (rule 1 + 2).
	AntResult dofs_of(const AntPackage &p_from, const GdStruct &p_hierarchy, std::vector<AntDof> &r_dofs) const;

private:
	std::unique_ptr<AntPackage> static_;
	std::vector<AntRig> rigs_;
};

} // namespace bf3::core::ant
