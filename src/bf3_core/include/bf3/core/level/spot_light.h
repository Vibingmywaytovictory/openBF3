#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::core::level {

enum class SpotLightShape : uint8_t { Cone, Frustum, Ortho };

struct SpotLight {
	Vec3 color{};
	float intensity = 0.0f;
	float radius = 0.0f;
	float attenuation_offset = 0.0f;
	float cone_inner_deg = 0.0f;
	float cone_outer_deg = 0.0f;
	SpotLightShape shape = SpotLightShape::Cone;
	float frustum_fov_deg = 0.0f;
	float frustum_aspect = 1.0f;
	float ortho_width = 0.0f;
	float ortho_height = 0.0f;
	bool cast_shadows = false;
	bool specular = false;
	std::string texture;
	LevelTransform local;
};

std::optional<float> ebx_f32(const EbxInstance &p_instance, std::string_view p_field);
std::optional<Vec3> ebx_vec3(const EbxInstance &p_instance, std::string_view p_field);

Result<SpotLight> read_spot_light(const EbxResolver &p_db, const EbxInstance &p_entity);

}
