#pragma once

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
const char *skin_backend_name(SkinBackend p_backend);

struct SkinUploadStats {
	uint32_t soldiers_uploaded = 0;
	uint32_t calls = 0;
	double seconds = 0.0;
};

class SkinUploader {
public:
	virtual ~SkinUploader() = default;
	virtual SkinBackend backend() const = 0;

	virtual bool build(const bf3::render::SoldierAssets &p_assets, const bf3::anim::CrowdEvaluator &p_eval, std::span<const uint8_t> p_team_of,
			godot::RID p_scenario, std::string *r_error) = 0;

	virtual void upload(const bf3::anim::CrowdEvaluator &p_eval, std::span<const godot::Transform3D> p_roots, SkinUploadStats &r_stats) = 0;
	virtual void set_visible(bool p_visible) = 0;

	virtual void set_soldier_visible(uint32_t p_soldier, bool p_visible) = 0;

	virtual void clear() = 0;

	virtual void set_soldier_kit(uint32_t p_soldier, uint8_t p_kit) { (void)p_soldier, (void)p_kit; }

	virtual uint32_t kit_meshes() const { return 0; }

	void set_palette_upload(bool p_on) { palette_upload_ = p_on; }

protected:
	bool palette_upload_ = true;
};

std::unique_ptr<SkinUploader> make_skin_uploader(SkinBackend p_backend, godot::Node *p_parent);

}
