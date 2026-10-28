#include "bf3/core/level/eor_camera.h"

#include "bf3/core/level/connections.h"

#include <cmath>

namespace bf3::core::level {

namespace {

bool takes_control_on_eor(const Connections &p_connections, const EbxInstance &p_camera) {
	const int32_t show = field_id(kEorCameraEvent);
	const int32_t take = field_id(kCameraTakeControl);
	for (const Connection &c : p_connections.event) {
		if (c.target == p_camera && c.target_field == take && c.source_field == show &&
				c.source.is_a("InterfaceDescriptorData")) {
			return true;
		}
	}
	return false;
}

}

Result<std::optional<EorCamera>> read_eor_camera(const EbxResolver &p_db, const World &p_world) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	for (uint32_t i = 0; i < p_world.objects.size(); ++i) {
		const PlacedObject &object = p_world.objects[i];
		if (!object.entity.is_a("CameraEntityData")) {
			continue;
		}
		BF3_TRY(budget.charge(1));
		Result<Connections> connections = Connections::of(p_db, object.entity.partition().primary(), budget);
		BF3_TRY(connections);
		if (!takes_control_on_eor(*connections, object.entity)) {
			continue;
		}
		EorCamera out;
		out.object = i;
		out.transform = object.transform;
		const std::optional<EbxValue> fov = object.entity.get("Fov");
		const std::optional<float> f = fov ? fov->as_f32() : std::nullopt;
		if (!f || !std::isfinite(*f) || *f <= 0.0f || *f >= 180.0f) {
			return make_error("read_eor_camera: camera " + object.entity.guid().to_string() + " has no usable Fov");
		}
		out.fov_deg = *f;
		const std::optional<EbxValue> priority = object.entity.get("Priority");
		out.priority = priority ? priority->as_i32().value_or(0) : 0;
		return std::optional<EorCamera>(out);
	}
	return std::optional<EorCamera>();
}

}
