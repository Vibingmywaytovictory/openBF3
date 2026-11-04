#include "bf3/render/weapon_lights.h"

#include <utility>

namespace bf3::render {

core::Result<std::vector<core::level::SpotLight>> read_prefab_spot_lights(
		const core::EbxResolver &p_db, std::string_view p_prefab) {
	const core::EbxPartition *part = p_db.partition_by_name(p_prefab);
	if (part == nullptr || !part->primary().is_a("PrefabBlueprint")) {
		return core::make_error("prefab " + std::string(p_prefab) + " not found");
	}
	std::vector<core::level::SpotLight> out;
	for (const core::EbxInstance &object : p_db.follow_all(part->primary().data(), "Objects")) {
		if (!object.is_a("SpotLightEntityData")) {
			continue;
		}
		core::Result<core::level::SpotLight> light = core::level::read_spot_light(p_db, object);
		if (!light) {
			return core::error_at(std::string(p_prefab), light.error());
		}
		out.push_back(std::move(*light));
	}
	return out;
}

core::Result<std::vector<WeaponSocketLight>> weapon_socket_lights(const core::EbxResolver &p_db,
		std::span<const WeaponSocket> p_sockets, const WeaponAccessories &p_accessories, bool p_first_person) {
	std::vector<WeaponSocketLight> out;
	for (const WeaponSocket &socket : p_sockets) {
		if (!socket_drawn(socket.unlock, p_accessories)) {
			continue;
		}
		for (const WeaponSocketObject &object : socket.objects) {
			const std::string &prefab = p_first_person ? object.prefab_1p : object.prefab_3p;
			if (prefab.empty() || object.skinned) {
				continue;
			}
			core::Result<std::vector<core::level::SpotLight>> lights = read_prefab_spot_lights(p_db, prefab);
			if (!lights) {
				return core::error_at(socket.unlock, lights.error());
			}
			const anim::Mat34 &placed =
					!p_first_person && object.transform_3p ? *object.transform_3p : object.transform;
			for (core::level::SpotLight &light : *lights) {
				WeaponSocketLight l;
				l.socket_unlock = socket.unlock;
				l.prefab = prefab;
				l.bone = socket.bone;
				l.transform = anim::mat_mul(placed, anim::mat_from_linear_transform(light.local.rows()));
				l.light = std::move(light);
				out.push_back(std::move(l));
			}
		}
	}
	return out;
}

float spot_light_half_angle_deg(const core::level::SpotLight &p_light) {
	return p_light.shape == core::level::SpotLightShape::Cone ? p_light.cone_outer_deg : 0.5f * p_light.frustum_fov_deg;
}

}
