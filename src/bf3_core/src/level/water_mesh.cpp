#include "bf3/core/level/water_mesh.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/superbundle_mount.h"

#include <cmath>
#include <optional>
#include <string_view>

namespace bf3::core::level {

Result<std::vector<WaterMesh>> read_water_meshes(const LoadedLevel &p_level, const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	std::vector<WaterMesh> out;
	size_t triangles = 0;
	for (const PlacedObject &object : p_world.objects) {
		BF3_TRY(budget.charge(1));
		if (!object.entity.is_a("WaterEntityData")) {
			continue;
		}
		const std::optional<EbxInstance> asset = p_world.scopes.db().follow(object.entity.data(), "Asset");
		if (!asset || !asset->is_a("WaterAsset")) {
			continue;
		}
		const Result<std::optional<std::string_view>> name = charged_str(asset->data(), "Name", budget);
		if (!name) {
			return name.error();
		}
		if (!*name || (*name)->empty()) {
			continue;
		}
		if (out.size() >= kMaxWaterMeshes) {
			return make_error("water: more than " + std::to_string(kMaxWaterMeshes) + " water meshes");
		}
		const std::string res_name(**name);
		Result<std::pair<ResInfo, std::vector<uint8_t>>> res = p_level.mount().read_res(res_name);
		if (!res) {
			return error_at("water res " + res_name, res.error());
		}
		res::HavokPhysicsData havok;
		const res::ResError parsed = res::HavokPhysicsData::parse(res->second, res->first.res_meta, havok);
		if (!parsed.ok()) {
			return make_error("water res " + res_name + ": " + parsed.message());
		}
		std::vector<res::Shape> shapes;
		const res::ResError listed = havok.shapes(shapes);
		if (!listed.ok()) {
			return make_error("water res " + res_name + ": " + listed.message());
		}
		WaterMesh mesh;
		mesh.asset = res_name;
		for (const res::Shape &shape : shapes) {
			for (const res::Tri &t : res::havok_triangles(shape, res::Transform::identity())) {
				std::array<Vec3, 3> w;
				for (size_t k = 0; k < 3; ++k) {
					w[k] = object.transform.transform_point(t.v[k]);
					for (const float f : w[k]) {
						if (!std::isfinite(f) || std::fabs(f) > 1e6f) {
							return make_error("water res " + res_name + ": a vertex is out of range");
						}
					}
				}
				if (++triangles > kMaxWaterTriangles) {
					return make_error("water: more than " + std::to_string(kMaxWaterTriangles) + " triangles");
				}
				mesh.triangles.push_back(w);
			}
		}
		BF3_TRY(budget.charge(mesh.triangles.size()));
		out.push_back(std::move(mesh));
	}
	return out;
}

}
