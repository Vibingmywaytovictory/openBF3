#include "bf3/core/gameplay/deployed_object.h"

#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <optional>

namespace bf3::core::gameplay {

namespace {

std::string partition_of(const EbxResolver &p_db, const EbxInstance &p_instance) {
	return std::string(p_db.name_of(p_instance.partition().guid()).value_or(std::string_view()));
}

bool started_on_spawn(const LogicConnections &p_links, const EbxInstance &p_object, const EbxInstance &p_component) {
	return std::any_of(p_links.event.begin(), p_links.event.end(), [&](const LogicConnection &l) {
		return l.source == p_object && l.source_field == kOnSpawnedEvent && l.target == p_component &&
				l.target_field == kEffectStartEvent;
	});
}

}

Result<DeployedObject> read_deployed_object(const EbxResolver &p_db, const EbxInstance &p_projectile) {
	const std::string where =
			"deployed object " + partition_of(p_db, p_projectile) + " " + p_projectile.guid().to_string();
	std::optional<EbxInstance> blueprint;
	if (p_projectile.is_a("VehicleProjectileEntityData")) {
		blueprint = p_db.follow(p_projectile.data(), "Vehicle");
	} else {
		blueprint = p_projectile.partition().primary();
	}
	if (!blueprint || !blueprint->is_a("ObjectBlueprint")) {
		return make_error(where + ": no object blueprint");
	}
	const std::optional<EbxInstance> object = p_db.follow(blueprint->data(), "Object");
	if (!object) {
		return make_error(where + ": no Object");
	}
	if (!p_projectile.is_a("VehicleProjectileEntityData") && !(*object == p_projectile)) {
		return make_error(where + ": the blueprint's Object is another entity");
	}
	DeployedObject out;
	out.projectile = partition_of(p_db, *blueprint);
	out.entity_type = std::string(object->type_name());
	if (const std::optional<EbxInstance> mesh = p_db.follow(object->data(), "Mesh")) {
		out.mesh = partition_of(p_db, *mesh);
	}
	WorkBudget budget;
	Result<LogicConnections> links = LogicConnections::of(p_db, *blueprint, budget);
	if (!links) {
		return core::error_at(where, links.error());
	}
	std::vector<EbxInstance> open{ *object };
	for (size_t i = 0; i < open.size(); ++i) {
		if (open.size() > kMaxDeployedComponents) {
			return make_error(where + ": more than " + std::to_string(kMaxDeployedComponents) + " components");
		}
		for (const EbxInstance &c : p_db.follow_all(open[i].data(), "Components")) {
			open.push_back(c);
			if (!c.is_a("EffectComponentData")) {
				continue;
			}
			const std::optional<EbxInstance> effect = p_db.follow(c.data(), "Effect");
			const std::optional<EbxValue> autostart = c.get("AutoStart");
			const bool starts =
					(autostart && autostart->as_bool().value_or(false)) || started_on_spawn(*links, *object, c);
			if (effect && starts) {
				out.effects.push_back(partition_of(p_db, *effect));
			}
		}
	}
	return out;
}

Result<DeployedObject> read_fired_object(const EbxResolver &p_db, std::string_view p_firing_data) {
	const size_t hash = p_firing_data.find('#');
	const std::optional<Guid> guid =
			hash == std::string_view::npos ? std::nullopt : Guid::parse(p_firing_data.substr(hash + 1));
	const EbxPartition *part = guid ? p_db.partition_by_name(to_lower_ascii(p_firing_data.substr(0, hash))) : nullptr;
	const std::optional<EbxInstance> firing = part != nullptr ? part->instance_by_guid(*guid) : std::nullopt;
	if (!firing) {
		return make_error("firing data " + std::string(p_firing_data) + " not found");
	}
	const std::optional<EbxInstance> primary = p_db.follow(firing->data(), "PrimaryFire");
	const std::optional<EbxValue> shot = primary ? primary->get("Shot") : std::nullopt;
	const std::optional<EbxStruct> s = shot ? shot->as_struct() : std::nullopt;
	const std::optional<EbxInstance> projectile = s ? p_db.follow(*s, "ProjectileData") : std::nullopt;
	if (!projectile) {
		return make_error("firing data " + std::string(p_firing_data) + " fires no projectile");
	}
	return read_deployed_object(p_db, *projectile);
}

}
