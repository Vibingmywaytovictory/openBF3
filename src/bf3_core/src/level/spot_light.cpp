#include "bf3/core/level/spot_light.h"

#include "bf3/core/util/finite.h"

#include <array>
#include <utility>

namespace bf3::core::level {

namespace {

constexpr std::array<std::pair<std::string_view, SpotLightShape>, 3> kShapes{ {
		{ "SpotLightShape_Cone", SpotLightShape::Cone },
		{ "SpotLightShape_Frustum", SpotLightShape::Frustum },
		{ "SpotLightShape_Ortho", SpotLightShape::Ortho },
} };

}

std::optional<float> ebx_f32(const EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<EbxValue> v = p_instance.get(p_field);
	const std::optional<float> f = v ? v->as_f32() : std::nullopt;
	return f && finite(*f) ? f : std::nullopt;
}

std::optional<Vec3> ebx_vec3(const EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<EbxValue> v = p_instance.get(p_field);
	const std::optional<EbxStruct> s = v ? v->as_struct() : std::nullopt;
	const std::optional<Vec3> out = s ? s->vec3() : std::nullopt;
	if (!out || !finite(*out)) {
		return std::nullopt;
	}
	return out;
}

Result<SpotLight> read_spot_light(const EbxResolver &p_db, const EbxInstance &p_entity) {
	const std::string where = "spot light " + p_entity.guid().to_string();
	if (!p_entity.is_a("SpotLightEntityData")) {
		return make_error(where + " is a " + std::string(p_entity.type_name()));
	}
	SpotLight out;
	const std::optional<Vec3> color = ebx_vec3(p_entity, "Color");
	const std::optional<float> intensity = ebx_f32(p_entity, "Intensity"), radius = ebx_f32(p_entity, "Radius"),
							   inner = ebx_f32(p_entity, "ConeInnerAngle"), outer = ebx_f32(p_entity, "ConeOuterAngle");
	if (!color || !intensity || !radius || !inner || !outer || *radius <= 0.0f || *outer <= 0.0f || *outer >= 180.0f) {
		return make_error(where + " has no usable Color, Intensity, Radius or cone");
	}
	out.color = *color;
	out.intensity = *intensity;
	out.radius = *radius;
	out.cone_inner_deg = *inner;
	out.cone_outer_deg = *outer;
	out.attenuation_offset = ebx_f32(p_entity, "AttenuationOffset").value_or(0.0f);
	const std::optional<EbxValue> shape = p_entity.get("Shape");
	const std::optional<std::string_view> shape_name = shape ? shape->enum_name() : std::nullopt;
	bool known = false;
	for (const auto &[name, value] : kShapes) {
		if (shape_name && *shape_name == name) {
			out.shape = value;
			known = true;
		}
	}
	if (!known) {
		return make_error(where + " has an unknown Shape " + std::string(shape_name.value_or("(none)")));
	}
	out.frustum_fov_deg = ebx_f32(p_entity, "FrustumFov").value_or(0.0f);
	out.frustum_aspect = ebx_f32(p_entity, "FrustumAspect").value_or(1.0f);
	out.ortho_width = ebx_f32(p_entity, "OrthoWidth").value_or(0.0f);
	out.ortho_height = ebx_f32(p_entity, "OrthoHeight").value_or(0.0f);
	if (out.shape == SpotLightShape::Frustum && (out.frustum_fov_deg <= 0.0f || out.frustum_fov_deg >= 180.0f)) {
		return make_error(where + " is a frustum with no usable FrustumFov");
	}
	const std::optional<EbxValue> shadows = p_entity.get("CastShadowsEnable");
	out.cast_shadows = shadows && shadows->as_bool().value_or(false);
	const std::optional<EbxValue> specular = p_entity.get("SpecularEnable");
	out.specular = specular && specular->as_bool().value_or(false);
	if (const std::optional<EbxInstance> texture = p_db.follow(p_entity.data(), "Texture")) {
		out.texture = std::string(p_db.name_of(texture->partition().guid()).value_or(std::string_view()));
	}
	const std::optional<EbxValue> local = p_entity.get("Transform");
	if (const std::optional<Rows> rows = local ? local->as_transform() : std::nullopt) {
		out.local = LevelTransform::from_rows(*rows);
	}
	return out;
}

}
