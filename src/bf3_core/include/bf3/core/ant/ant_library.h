#pragma once

#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/ant/ant_package.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::ant {

enum class DofKind {
	Rotation,
	Translation,
	Scale,
	Float,
	Int,
	DeltaTrajectory,
	Other,
};
const char *dof_kind_name(DofKind p_kind);
DofKind dof_kind_from_slot_type(uint32_t p_type);

struct AntDof {
	std::string_view layout;
	std::string_view slot;
	std::string_view joint;
	DofKind kind = DofKind::Other;
	uint32_t type = 0;
};

struct AntJoint {
	std::string_view name;
	int32_t parent = -1;
	int32_t type = 0;
	float rest_t[3] = { 0.0f, 0.0f, 0.0f };
	float rest_q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct AntRig {
	std::string_view name;
	std::string_view skeleton_name;
	std::vector<AntJoint> joints;
	std::unordered_map<std::string_view, int32_t> joint_index;
	uint32_t bad_parents = 0;
	std::vector<uint32_t> hierarchies;
	std::vector<uint32_t> layouts;

	int32_t find_joint(std::string_view p_name) const;
};

struct ChannelTrack {
	uint32_t channel = 0;
	char kind = 'q';
	uint32_t dof = 0;
	int32_t joint = -1;
	bool kind_ok = false;
};

struct ClipBinding {
	GdStruct clip;
	GdStruct controller;
	GdStruct target;
	GdStruct channel_to_dof;
	std::string_view controller_name;
	std::string_view target_name;
	float fps = 30.0f;
	float fps_scale = 1.0f;
	float num_ticks = 0.0f;

	const AntRig *rig = nullptr;
	uint32_t rig_layout_hits = 0;
	std::vector<AntDof> dofs;
	std::vector<ChannelTrack> tracks;
	uint32_t kind_mismatches = 0;
	uint32_t delta_traj_channels = 0;

	bool has_controller() const { return controller.valid(); }
};

class AntLibrary {
public:
	AntLibrary() = default;
	AntLibrary(const AntLibrary &) = delete;
	AntLibrary &operator=(const AntLibrary &) = delete;

	AntResult set_static(std::unique_ptr<AntPackage> p_static);
	const AntPackage *static_package() const { return static_.get(); }
	const std::vector<AntRig> &rigs() const { return rigs_; }

	GdStruct resolve(const AntPackage &p_from, const GdGuid &p_guid) const;
	const AntPackage &package_of(const AntPackage &p_from, const GdGuid &p_guid) const;

	static std::vector<uint32_t> clips(const AntPackage &p_package);

	static GdStruct find_clip(const AntPackage &p_package, std::string_view p_name);

	AntResult bind(const AntPackage &p_package, const GdStruct &p_clip, ClipBinding &r_binding) const;

	AntResult dofs_of(const AntPackage &p_from, const GdStruct &p_hierarchy, std::vector<AntDof> &r_dofs) const;

private:
	std::unique_ptr<AntPackage> static_;
	std::vector<AntRig> rigs_;
};

}
