#pragma once
// Skin palette upload backends for the 3p crowd (S3; docs/design/s3.md §7). Main thread only: every call reaches
// RenderingServer or a Node. The palettes come from bf3::anim::CrowdEvaluator (worker lanes) and are read here
// after wait_and_publish.
//
// Every backend draws one combined RS mesh per team (all parts' LOD surfaces: body, head, helmet, weapon; vertex
// bone indices = the team's SkinBinds indices) and one instance per soldier, so one skeleton per soldier
// (research/topics/03-animation.md §4.1 pitfall: one Skin per soldier). They differ in how a palette reaches the
// GPU:
//   A Skeleton3D  one Skeleton3D node per soldier with the rig's bones and a shared Skin of the team's binds; per
//                 frame set_bone_pose_rotation / set_bone_pose_position of the driven bones (local pose). Godot
//                 then recomputes global poses and skin matrices itself (NOTIFICATION_UPDATE_SKELETON, main
//                 thread). This is the M0 viewmodel path, scaled.
//   B RS skeleton one RenderingServer skeleton per soldier (skeleton_create, skeleton_allocate_data(binds, false),
//                 instance_attach_skeleton); per frame skeleton_bone_set_transform(skel, bind, skin matrix) for
//                 every bind, i.e. the final palette with no Godot-side pose math. US 113 / RU 107 calls per
//                 updated soldier.
//   C bone texture one RGBA32F texture of 3 texels per bind per soldier (texture_2d_update once per frame) and a
//                 ShaderMaterial that skins in vertex(). It replaces the material path (L track), so it is
//                 benched only when A and B both miss G1 (s3.md §7.3).
// [verified in engine-api/4.8.dev7.c971f93e7/extension_api.json: RenderingServer skeleton_create,
// skeleton_allocate_data, skeleton_bone_set_transform, instance_attach_skeleton, instance_set_custom_aabb,
// texture_2d_update, viewport_set_measure_render_time, viewport_get_measured_render_time_cpu/_gpu; Skeleton3D
// set_bone_pose_rotation, set_bone_pose_position, register_skin]
// There is no bulk palette setter on RenderingServer in 4.8 dev7, so B costs one call per bind.

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/pose_math.h"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {
struct SoldierAssets;
}

namespace godot {
class Node;
}

namespace bf3::godot_side {

enum class SkinBackend : uint8_t { Skeleton3D, RsSkeleton, BoneTexture };
const char *skin_backend_name(SkinBackend p_backend); // "skeleton3d", "rs", "texture"

// Per-frame upload counters (reports).
struct SkinUploadStats {
	uint32_t soldiers_uploaded = 0; // palettes pushed this frame
	uint32_t calls = 0; // Godot API calls made this frame
	double seconds = 0.0; // main-thread time spent in upload()
};

class SkinUploader {
public:
	virtual ~SkinUploader() = default;
	virtual SkinBackend backend() const = 0;
	// Creates the meshes (once per team), instances and skeletons in p_scenario. Main thread. False with
	// r_error when a resource cannot be created.
	virtual bool build(const bf3::render::SoldierAssets &p_assets, const bf3::anim::CrowdEvaluator &p_eval, std::span<const uint8_t> p_team_of,
			godot::RID p_scenario, std::string *r_error) = 0;
	// Root transforms of every soldier (always) and palettes of the soldiers whose palette changed
	// (CrowdEvaluator::changed). Main thread. Not called at all on a frozen frame (s3.md §1 G1b: the soldiers
	// keep their last roots and palettes and draw the same).
	virtual void upload(const bf3::anim::CrowdEvaluator &p_eval, std::span<const godot::Transform3D> p_roots, SkinUploadStats &r_stats) = 0;
	virtual void set_visible(bool p_visible) = 0;
	// One soldier's instance on or off (the G3 shots hide all but the posed pair). Main thread.
	virtual void set_soldier_visible(uint32_t p_soldier, bool p_visible) = 0;
	// Frees every RID / node it created. Main thread.
	virtual void clear() = 0;
};

// The backend's uploader, or null when it is not built in (C until s3.md §7.3 asks for it). p_parent receives the
// nodes of backends that need the scene tree (A: one Skeleton3D + MeshInstance3D per soldier); B makes RIDs only.
std::unique_ptr<SkinUploader> make_skin_uploader(SkinBackend p_backend, godot::Node *p_parent);

} // namespace bf3::godot_side
