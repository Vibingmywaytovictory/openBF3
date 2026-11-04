#include "bf3/core/level/eor_camera.h"

#include "bf3/core/level/connections.h"

#include <cmath>
#include <utility>
#include <vector>

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

std::vector<std::pair<EbxInstance, int32_t>> event_sources(
		const Connections &p_connections, const EbxInstance &p_target, int32_t p_event) {
	std::vector<std::pair<EbxInstance, int32_t>> out;
	for (const Connection &c : p_connections.event) {
		if (c.target == p_target && c.target_field == p_event) {
			out.emplace_back(c.source, c.source_field);
		}
	}
	return out;
}

Result<EorEnvironment> read_environment(
		const EbxResolver &p_db, const Connections &p_connections, uint32_t p_object, const PlacedObject &p_lve) {
	EorEnvironment out;
	out.object = p_object;
	const std::optional<EbxInstance> blueprint = p_db.follow(p_lve.entity.data(), "VisualEnvironment");
	const std::optional<std::string_view> name = blueprint ? p_db.name_of(blueprint->partition().guid()) : std::nullopt;
	if (!name) {
		return make_error("read_eor_camera: visual environment " + p_lve.entity.guid().to_string() +
				" names no VisualEnvironment");
	}
	out.visual_environment = std::string(*name);
	out.visibility = ebx_f32(p_lve.entity, "Visibility").value_or(1.0f);
	const int32_t visibility = field_id("Visibility");
	for (const Connection &c : p_connections.property) {
		if (c.target == p_lve.entity && c.target_field == visibility && c.source.is_a("AreaProximityEntityData")) {
			out.visibility_from_proximity = true;
		}
	}
	return out;
}

Result<EorSpotLight> read_light(const EbxResolver &p_db, const Connections &p_connections, uint32_t p_object,
		const PlacedObject &p_light, const std::optional<EorEnvironment> &p_environment, const World &p_world) {
	Result<SpotLight> light = read_spot_light(p_db, p_light.entity);
	if (!light) {
		return make_error("read_eor_camera: " + light.error().message);
	}
	EorSpotLight out;
	static_cast<SpotLight &>(out) = std::move(*light);
	out.object = p_object;
	out.transform = p_light.transform;
	if (p_environment) {
		const EbxInstance &lve = p_world.objects[p_environment->object].entity;
		const int32_t enable = field_id("Enable"), disable = field_id("Disable");
		out.switched_with_environment = !event_sources(p_connections, p_light.entity, enable).empty() &&
				event_sources(p_connections, p_light.entity, enable) == event_sources(p_connections, lve, enable) &&
				event_sources(p_connections, p_light.entity, disable) == event_sources(p_connections, lve, disable);
	}
	return out;
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
		std::optional<uint32_t> light, environment;
		for (uint32_t j = 0; j < p_world.objects.size(); ++j) {
			const PlacedObject &other = p_world.objects[j];
			if (other.scope != object.scope) {
				continue;
			}
			if (!light && other.entity.is_a("SpotLightEntityData")) {
				light = j;
			} else if (!environment && other.entity.is_a("LogicVisualEnvironmentEntityData")) {
				environment = j;
			}
		}
		if (environment) {
			Result<EorEnvironment> e =
					read_environment(p_db, *connections, *environment, p_world.objects[*environment]);
			BF3_TRY(e);
			out.environment = std::move(*e);
		}
		if (light) {
			Result<EorSpotLight> l =
					read_light(p_db, *connections, *light, p_world.objects[*light], out.environment, p_world);
			BF3_TRY(l);
			out.light = *l;
		}
		return std::optional<EorCamera>(std::move(out));
	}
	return std::optional<EorCamera>();
}

bool CameraCuts::step(const CameraCutState &p_state) {
	const bool alive = p_state.alive && !p_state.eor_camera;
	bool cut = false;
	const auto edge = [&](bool p_was, bool p_now, CameraCut p_kind) {
		if (p_now && !p_was) {
			++counts_[size_t(p_kind)];
			cut = true;
		}
	};
	edge(last_.join_view, p_state.join_view, CameraCut::JoinView);
	edge(last_.alive, alive, CameraCut::Spawn);
	edge(last_.death_camera, p_state.death_camera, CameraCut::DeathCamera);
	edge(last_.eor_camera, p_state.eor_camera, CameraCut::EorTake);
	edge(!last_.eor_camera, !p_state.eor_camera, CameraCut::EorRelease);
	last_ = p_state;
	last_.alive = alive;
	return cut;
}

uint64_t CameraCuts::total() const {
	uint64_t n = 0;
	for (const uint64_t c : counts_) {
		n += c;
	}
	return n;
}

}
