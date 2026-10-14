// Level traversal: port of IW4L level_bf3 world.rs and variations.rs (Apache-2.0).
#include "bf3/core/level/world.h"

namespace bf3::core::level {

namespace {

constexpr uint32_t kMaxDepth = 48;
// Placed objects and scopes one traversal may create. The depth limit bounds nesting, not fan-out:
// a hostile prefab listing itself many times would otherwise grow without bound. Retail levels
// place at most ~16k objects in ~650 scopes.
constexpr size_t kMaxObjects = size_t(1) << 22;
constexpr size_t kMaxScopes = size_t(1) << 20;

struct KindName {
	std::string_view type;
	ObjectKind kind;
};

// Checked in order, by is_a.
constexpr KindName kKinds[15] = {
	{ "TerrainEntityData", ObjectKind::Terrain },
	{ "WaterEntityData", ObjectKind::Water },
	{ "LocalLightEntityData", ObjectKind::Light },
	{ "LightEntityData", ObjectKind::Light },
	{ "DecalEntityData", ObjectKind::Decal },
	{ "TerrainQuadDecalData", ObjectKind::Decal },
	{ "EffectEntityData", ObjectKind::Effect },
	{ "AlternateSpawnEntityData", ObjectKind::AlternateSpawn },
	{ "CharacterSpawnReferenceObjectData", ObjectKind::CharacterSpawnReference },
	{ "VehicleSpawnReferenceObjectData", ObjectKind::VehicleSpawnReference },
	{ "CapturePointEntityData", ObjectKind::CapturePoint },
	{ "CombatAreaTriggerEntityData", ObjectKind::CombatArea },
	{ "VolumeVectorShapeData", ObjectKind::VolumeVectorShape },
	{ "VisualEnvironmentEntityData", ObjectKind::VisualEnvironment },
	{ "VisualEnvironmentReferenceObjectData", ObjectKind::VisualEnvironment },
};

struct MeshEntityName {
	std::string_view type;
	MeshEntityType entity_type;
};

constexpr MeshEntityName kMeshEntities[7] = {
	{ "VegetationTreeEntityData", MeshEntityType::VegetationTree },
	{ "BangerEntityData", MeshEntityType::Banger },
	{ "BreakableModelEntityData", MeshEntityType::Breakable },
	{ "DynamicModelEntityData", MeshEntityType::Dynamic },
	{ "StaticModelEntityData", MeshEntityType::StaticModel },
	{ "MeshProxyEntityData", MeshEntityType::MeshProxy },
	{ "VehicleEntityData", MeshEntityType::Vehicle },
};

std::optional<std::string_view> str_field(const EbxInstance &p_object, std::string_view p_field) {
	const std::optional<EbxValue> v = p_object.get(p_field);
	return v ? v->as_str() : std::nullopt;
}

std::optional<uint32_t> u32_of(const std::optional<EbxValue> &p_value) {
	return p_value ? p_value->as_u32() : std::nullopt;
}

void classify(const EbxResolver &p_db, const EbxInstance &p_entity, PlacedObject &r_object) {
	const std::optional<EbxInstance> asset = p_db.follow(p_entity.data(), "Mesh");
	if (asset && asset->is_a("MeshAsset")) {
		MeshObject mesh;
		mesh.asset = *asset;
		for (const MeshEntityName &m : kMeshEntities) {
			if (p_entity.is_a(m.type)) {
				mesh.entity_type = m.entity_type;
				break;
			}
		}
		mesh.physics = p_db.follow(p_entity.data(), "PhysicsData");
		r_object.kind = ObjectKind::Mesh;
		r_object.mesh = mesh;
		return;
	}
	for (const KindName &k : kKinds) {
		if (p_entity.is_a(k.type)) {
			r_object.kind = k.kind;
			return;
		}
	}
	r_object.kind = ObjectKind::Other;
}

SubWorld read_subworld(const EbxResolver &p_db, const EbxInstance &p_data, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent,
		uint32_t p_scope) {
	SubWorld sw;
	sw.name = std::string(p_db.name_of(p_data.partition().guid()).value_or(""));
	sw.data = p_data;
	sw.reference = p_reference;
	sw.parent = p_parent;
	sw.scope = p_scope;
	if (const EbxPartition *mvdb = p_db.partition_by_name(sw.name + "/meshvariationdb_win32")) {
		sw.variations = MeshVariationDb::read(p_db, mvdb->primary());
	}
	return sw;
}

} // namespace

std::string_view mesh_entity_type_name(MeshEntityType p_type) {
	switch (p_type) {
		case MeshEntityType::StaticModel:
			return "StaticModel";
		case MeshEntityType::MeshProxy:
			return "MeshProxy";
		case MeshEntityType::Breakable:
			return "Breakable";
		case MeshEntityType::Dynamic:
			return "Dynamic";
		case MeshEntityType::VegetationTree:
			return "VegetationTree";
		case MeshEntityType::GroupMember:
			return "GroupMember";
		case MeshEntityType::Banger:
			return "Banger";
		case MeshEntityType::Vehicle:
			return "Vehicle";
		default:
			return "Other";
	}
}

std::string_view MeshObject::name() const {
	return str_field(asset, "Name").value_or("");
}

std::string_view PlacedObject::label() const {
	switch (kind) {
		case ObjectKind::Mesh:
			return "Mesh";
		case ObjectKind::Terrain:
			return "Terrain";
		case ObjectKind::Water:
			return "Water";
		case ObjectKind::Light:
			return "Light";
		case ObjectKind::Decal:
			return "Decal";
		case ObjectKind::Effect:
			return "Effect";
		case ObjectKind::AlternateSpawn:
			return "AlternateSpawn";
		case ObjectKind::CharacterSpawnReference:
			return "CharacterSpawnReference";
		case ObjectKind::VehicleSpawnReference:
			return "VehicleSpawnReference";
		case ObjectKind::CapturePoint:
			return "CapturePoint";
		case ObjectKind::CombatArea:
			return "CombatArea";
		case ObjectKind::VolumeVectorShape:
			return "VolumeVectorShape";
		case ObjectKind::VisualEnvironment:
			return "VisualEnvironment";
		default:
			// A group member without a mesh is placed as Other with the member type's name.
			return entity.type_name();
	}
}

// --- MeshVariationDb ---------------------------------------------------------------------------

MeshVariationDb MeshVariationDb::read(const EbxResolver &p_db, const EbxInstance &p_database, bool p_keep_empty) {
	MeshVariationDb out;
	out.database_ = p_database;
	for (const char *list : { "Entries", "RedirectEntries" }) {
		for (const EbxInstance &entry : p_db.follow_all(p_database.data(), list)) {
			const std::optional<EbxInstance> mesh = p_db.follow(entry.data(), "Mesh");
			if (!mesh) {
				continue;
			}
			if (!p_keep_empty) {
				const std::optional<EbxValue> materials = entry.get("Materials");
				const OptionalEbxArray array = materials ? materials->as_array() : OptionalEbxArray();
				if (array && array->empty()) {
					continue;
				}
			}
			const uint32_t hash = u32_of(entry.get("VariationAssetNameHash")).value_or(0);
			out.entries_.emplace(Key{ mesh->partition().guid(), mesh->guid(), hash }, entry); // first one wins
		}
	}
	return out;
}

size_t MeshVariationDb::variant_count() const {
	size_t n = 0;
	for (const auto &[key, entry] : entries_) {
		n += std::get<2>(key) != 0 ? 1 : 0;
	}
	return n;
}

std::optional<EbxInstance> MeshVariationDb::entry(const EbxInstance &p_mesh, uint32_t p_variation) const {
	const auto found = entries_.find(Key{ p_mesh.partition().guid(), p_mesh.guid(), p_variation });
	return found == entries_.end() ? std::nullopt : std::optional<EbxInstance>(found->second);
}

// --- World -------------------------------------------------------------------------------------

const PlacedObject *World::placed(uint32_t p_scope, const EbxInstance &p_object) const {
	const auto found = placed_.find(PlacedKey{ p_scope, InstanceKey::of(p_object) });
	return found == placed_.end() ? nullptr : &objects[found->second];
}

std::optional<EbxInstance> World::mesh_variation(uint32_t p_subworld, const EbxInstance &p_mesh, uint32_t p_variation) const {
	const auto lookup = [&](uint32_t p_hash) -> std::optional<EbxInstance> {
		std::optional<uint32_t> at = p_subworld;
		// Parents always precede their children, so the chain ends; the bound guards anyway.
		for (size_t hops = 0; at && *at < subworlds.size() && hops <= subworlds.size(); ++hops) {
			if (std::optional<EbxInstance> e = subworlds[*at].variations.entry(p_mesh, p_hash)) {
				return e;
			}
			at = subworlds[*at].parent;
		}
		return std::nullopt;
	};
	if (std::optional<EbxInstance> e = lookup(p_variation)) {
		return e;
	}
	return p_variation != 0 ? lookup(0) : std::nullopt;
}

class Walker {
public:
	Walker(const EbxResolver &p_db, const LevelMount &p_mount, std::string_view p_mode, World &p_world) :
			db_(p_db), mount_(p_mount), mode_(p_mode), world_(p_world) {}

	Status visit(const EbxInstance &p_container, uint32_t p_scope, const LevelTransform &p_frame, uint32_t p_subworld, uint32_t p_variation,
			uint32_t p_depth) {
		if (p_depth > kMaxDepth) {
			++world_.stats.depth_limited;
			return Status::success();
		}
		if (world_.objects.size() > kMaxObjects || world_.scopes.size() > kMaxScopes) {
			return make_error("level traversal: more than " + std::to_string(kMaxObjects) + " placed objects or " + std::to_string(kMaxScopes) +
					" scopes");
		}
		for (const EbxInstance &object : objects(p_container)) {
			if (object.is_a("WorldPartReferenceObjectData")) {
				if (excluded(p_scope, object)) {
					continue;
				}
				const std::optional<EbxInstance> part = db_.follow(object.data(), "Blueprint");
				if (!part) {
					++world_.stats.missing_blueprints;
					continue;
				}
				const LevelTransform frame = blueprint_transform(p_scope, object).in_parent(p_frame);
				BF3_TRY(visit(*part, p_scope, frame, p_subworld, 0, p_depth + 1));
			} else if (object.is_a("SubWorldReferenceObjectData")) {
				BF3_TRY(enter_subworld(object, p_scope, p_frame, p_subworld, p_depth));
			} else if (object.is_a("ReferenceObjectData")) {
				BF3_TRY(place_reference(object, p_scope, p_frame, p_subworld, p_variation, p_depth));
			} else if (object.is_a("StaticModelGroupEntityData")) {
				BF3_TRY(expand_group(object, p_scope, p_frame, p_subworld));
			} else {
				const LevelTransform transform = entity_transform(p_scope, object).in_parent(p_frame);
				emit(object, std::nullopt, transform, p_scope, p_subworld, p_variation);
			}
		}
		return Status::success();
	}

private:
	std::vector<EbxInstance> objects(const EbxInstance &p_container) {
		std::vector<EbxInstance> out;
		const std::optional<EbxValue> list = p_container.get("Objects");
		const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
		if (!array) {
			return out;
		}
		out.reserve(array->size());
		for (const EbxValue value : *array) {
			const std::optional<EbxRef> ref = value.as_ref();
			if (!ref || ref->is_null()) {
				continue;
			}
			if (const std::optional<EbxInstance> object = db_.resolve(p_container.partition(), *ref)) {
				out.push_back(*object);
			} else {
				++world_.stats.unresolved_objects;
			}
		}
		return out;
	}

	Status enter_subworld(const EbxInstance &p_reference, uint32_t p_scope, const LevelTransform &p_frame, uint32_t p_subworld, uint32_t p_depth) {
		const std::string name(str_field(p_reference, "BundleName").value_or(""));
		if (!includes(db_, p_reference, mode_)) {
			world_.stats.skipped_subworlds.push_back(name);
			return Status::success();
		}
		if (excluded(p_scope, p_reference)) {
			return Status::success();
		}
		const std::optional<EbxInstance> data = world_.scopes.referenced_blueprint(p_reference);
		if (!data) {
			world_.stats.absent_subworlds.push_back(name);
			return Status::success();
		}
		const LevelTransform frame = blueprint_transform(p_scope, p_reference).in_parent(p_frame);
		const uint32_t child = uint32_t(world_.subworlds.size());
		const uint32_t child_scope = world_.scopes.push(*data, p_reference, p_scope, frame, child, 0);
		world_.subworlds.push_back(read_subworld(db_, *data, p_reference, p_subworld, child_scope));
		return visit(*data, child_scope, frame, child, 0, p_depth + 1);
	}

	Status place_reference(const EbxInstance &p_reference, uint32_t p_scope, const LevelTransform &p_frame, uint32_t p_subworld, uint32_t p_variation,
			uint32_t p_depth) {
		if (excluded(p_scope, p_reference)) {
			return Status::success();
		}
		LevelTransform transform = blueprint_transform(p_scope, p_reference).in_parent(p_frame);
		uint32_t variation = p_variation;
		if (const std::optional<EbxInstance> object_variation = db_.follow(p_reference.data(), "ObjectVariation")) {
			variation = u32_of(object_variation->get("NameHash")).value_or(p_variation);
		}
		if (p_reference.is_a("SpawnReferenceObjectData")) {
			emit(p_reference, std::nullopt, transform, p_scope, p_subworld, variation);
			return Status::success();
		}
		const std::optional<EbxInstance> blueprint = db_.follow(p_reference.data(), "Blueprint");
		if (!blueprint) {
			++world_.stats.missing_blueprints;
			emit(p_reference, std::nullopt, transform, p_scope, p_subworld, variation);
			return Status::success();
		}
		if (blueprint->is_a("PrefabBlueprint")) {
			const uint32_t child = world_.scopes.push(*blueprint, p_reference, p_scope, transform, p_subworld, variation);
			return visit(*blueprint, child, transform, p_subworld, variation, p_depth + 1);
		}
		if (blueprint->is_a("ObjectBlueprint")) {
			if (const std::optional<EbxInstance> object = db_.follow(blueprint->data(), "Object")) {
				const std::optional<EbxValue> local_value = object->get("Transform");
				const auto rows = local_value ? local_value->as_transform() : std::nullopt;
				const LevelTransform local = rows ? LevelTransform::from_rows(*rows) : LevelTransform::identity();
				emit(*object, p_reference, local.in_parent(transform), p_scope, p_subworld, variation);
				return Status::success();
			}
		}
		emit(p_reference, std::nullopt, transform, p_scope, p_subworld, variation);
		return Status::success();
	}

	Status expand_group(const EbxInstance &p_group, uint32_t p_scope, const LevelTransform &p_frame, uint32_t p_subworld) {
		const LevelTransform transform = entity_transform(p_scope, p_group).in_parent(p_frame);
		StaticModelGroup group;
		group.entity = p_group;
		group.subworld = p_subworld;
		group.scope = p_scope;
		group.transform = transform;
		if (const std::optional<EbxInstance> physics = db_.follow(p_group.data(), "PhysicsData")) {
			if (const std::optional<EbxInstance> asset = db_.follow(physics->data(), "Asset")) {
				if (const std::optional<std::string_view> name = str_field(*asset, "Name")) {
					group.physics_asset = std::string(*name);
				}
			}
		}
		const std::string group_name = group.physics_asset.value_or(p_group.guid().to_string());
		std::vector<res::SubpartTransform> subparts;
		if (group.physics_asset) {
			Result<std::pair<ResInfo, std::vector<uint8_t>>> res = mount_.read_res(*group.physics_asset);
			if (!res) {
				return error_at("res " + *group.physics_asset, res.error());
			}
			auto havok = std::make_shared<res::HavokPhysicsData>();
			const res::ResError parsed = res::HavokPhysicsData::parse(res->second, res->first.res_meta, *havok);
			if (!parsed.ok()) {
				return make_error(*group.physics_asset + ": " + parsed.message());
			}
			const res::ResError listed = havok->group_subparts(subparts);
			if (!listed.ok()) {
				return make_error(group_name + ": " + listed.message());
			}
			group.havok = std::move(havok);
		}

		const uint32_t index = uint32_t(world_.groups.size());
		std::vector<EbxStruct> members;
		const std::optional<EbxValue> member_list = p_group.get("MemberDatas");
		const OptionalEbxArray member_array = member_list ? member_list->as_array() : OptionalEbxArray();
		if (member_array) {
			for (const EbxValue m : *member_array) {
				if (const std::optional<EbxStruct> s = m.as_struct()) {
					members.push_back(*s);
				}
			}
		}
		size_t instances = 0, explicit_count = 0, cursor = 0, part_mismatches = 0, needed = 0;
		for (size_t member_at = 0; member_at < members.size(); ++member_at) {
			const EbxStruct &member = members[member_at];
			const auto list = [&](std::string_view p_field) -> OptionalEbxArray {
				const std::optional<EbxValue> v = member.get(p_field);
				return v ? v->as_array() : OptionalEbxArray();
			};
			const uint32_t count = u32_of(member.get("InstanceCount")).value_or(0);
			const OptionalEbxArray transforms = list("InstanceTransforms");
			const OptionalEbxArray scales = list("InstanceScale");
			const OptionalEbxArray variations = list("InstanceObjectVariation");
			const std::optional<EbxInstance> member_type = db_.follow(member, "MemberType");
			const std::optional<EbxInstance> mesh_entity = db_.follow(member, "MeshEntityType");
			std::optional<EbxInstance> mesh = mesh_entity ? db_.follow(mesh_entity->data(), "Mesh") : std::nullopt;
			if (!mesh && member_type) {
				mesh = db_.follow(member_type->data(), "Mesh");
			}
			const std::optional<EbxInstance> entity = member_type ? member_type : mesh_entity;
			if (!entity) {
				continue;
			}
			const std::optional<EbxInstance> physics = member_type ? db_.follow(member_type->data(), "PhysicsData") : std::nullopt;
			std::optional<uint32_t> first_part = u32_of(member.get_path("PhysicsPartRange.First"));
			if (first_part == UINT32_MAX) {
				first_part.reset();
			}
			const uint32_t parts_per_instance = u32_of(member.get("PhysicsPartCountPerInstance")).value_or(0);
			const size_t transform_count = transforms ? transforms->size() : 0;

			for (uint32_t instance = 0; instance < count; ++instance) {
				const std::optional<EbxValue> explicit_value = instance < transform_count ? transforms->get(instance) : std::nullopt;
				const auto explicit_rows = explicit_value ? explicit_value->as_transform() : std::nullopt;
				LevelTransform local;
				std::optional<uint32_t> havok_subpart;
				if (explicit_rows) {
					++explicit_count;
					local = LevelTransform::from_rows(*explicit_rows);
				} else {
					++needed;
					if (cursor >= subparts.size()) {
						// Out of subparts: the group fails below. Count the rest without walking a
						// hostile InstanceCount one by one.
						if (instance >= transform_count) {
							needed += size_t(count - instance - 1);
							break;
						}
						continue;
					}
					local = LevelTransform::from_res(subparts[cursor].transform());
					havok_subpart = uint32_t(cursor);
					++cursor;
				}
				std::optional<uint32_t> physics_part;
				if (first_part) {
					physics_part = *first_part + instance * parts_per_instance; // wrapping, as u32 in Rust release
				}
				if (havok_subpart && physics_part && subparts[*havok_subpart].user_data != *physics_part) {
					++part_mismatches;
				}
				const std::optional<EbxValue> scale_value = scales ? scales->get(instance) : std::nullopt;
				const float scale = (scale_value ? scale_value->as_f32() : std::nullopt).value_or(1.0f);
				const std::optional<EbxValue> variation_value = variations ? variations->get(instance) : std::nullopt;
				const uint32_t variation = (variation_value ? variation_value->as_u32() : std::nullopt).value_or(0);
				local.scale = local.scale * scale;

				PlacedObject placed;
				if (mesh) {
					MeshObject m;
					m.asset = *mesh;
					m.entity_type = MeshEntityType::GroupMember;
					m.physics = physics;
					placed.kind = ObjectKind::Mesh;
					placed.mesh = m;
				} else {
					placed.kind = ObjectKind::Other;
				}
				++instances;
				placed.transform = local.in_parent(transform);
				placed.entity = *entity;
				placed.group = GroupSlot{ index, uint32_t(member_at), instance, havok_subpart, physics_part };
				placed.subworld = p_subworld;
				placed.scope = p_scope;
				placed.variation = variation;
				world_.objects.push_back(std::move(placed));
			}
		}
		if (needed > subparts.size()) {
			return make_error("static model group " + group_name + ": " + std::to_string(needed) + " Havok-placed instances, " +
					std::to_string(subparts.size()) + " subparts");
		}
		group.members = members.size();
		group.instances = instances;
		group.explicit_count = explicit_count;
		group.havok_placed = cursor;
		group.subparts = subparts.size();
		group.part_mismatches = part_mismatches;
		world_.groups.push_back(std::move(group));
		return Status::success();
	}

	void emit(const EbxInstance &p_entity, std::optional<EbxInstance> p_reference, const LevelTransform &p_transform, uint32_t p_scope,
			uint32_t p_subworld, uint32_t p_variation) {
		PlacedObject placed;
		classify(db_, p_entity, placed);
		placed.transform = p_transform;
		placed.entity = p_entity;
		placed.reference = p_reference;
		placed.subworld = p_subworld;
		placed.scope = p_scope;
		placed.variation = p_variation;
		world_.objects.push_back(std::move(placed));
	}

	bool excluded(uint32_t p_scope, const EbxInstance &p_reference) {
		bool excluded = false;
		const std::optional<Property> fed = world_.scopes.property(p_scope, p_reference, field_id("Excluded"));
		const std::optional<bool> value = fed ? fed->as_bool() : std::nullopt;
		if (value) {
			++world_.stats.excluded_overrides;
			excluded = *value;
		} else {
			const std::optional<EbxValue> own = p_reference.get("Excluded");
			excluded = (own ? own->as_bool() : std::nullopt).value_or(false);
		}
		if (excluded) {
			++world_.stats.excluded;
		}
		return excluded;
	}

	LevelTransform blueprint_transform(uint32_t p_scope, const EbxInstance &p_reference) {
		return overridable_transform(p_scope, p_reference, "BlueprintTransform");
	}

	LevelTransform entity_transform(uint32_t p_scope, const EbxInstance &p_entity) {
		return overridable_transform(p_scope, p_entity, "Transform");
	}

	LevelTransform overridable_transform(uint32_t p_scope, const EbxInstance &p_object, std::string_view p_field) {
		const std::optional<EbxValue> data = p_object.get(p_field);
		const auto rows = data ? data->as_transform() : std::nullopt;
		if (!rows) {
			return LevelTransform::identity();
		}
		const std::optional<Property> fed = world_.scopes.property(p_scope, p_object, field_id(p_field));
		if (const std::optional<LevelTransform> transform = fed ? fed->as_transform() : std::nullopt) {
			++world_.stats.transform_overrides;
			return *transform;
		}
		return LevelTransform::from_rows(*rows);
	}

	const EbxResolver &db_;
	const LevelMount &mount_;
	std::string_view mode_;
	World &world_;
};

Result<std::unique_ptr<World>> World::traverse(const LoadedLevel &p_level) {
	return traverse(p_level.db(), p_level.root(), p_level.mount(), p_level.mode);
}

Result<std::unique_ptr<World>> World::traverse(const EbxResolver &p_db, const EbxInstance &p_root, const LevelMount &p_mount, std::string_view p_mode) {
	const EbxResolver &db = p_db;
	const EbxInstance root = p_root;
	auto world = std::make_unique<World>(db);
	const uint32_t scope = world->scopes.push(root, std::nullopt, std::nullopt, LevelTransform::identity(), 0, 0);
	world->subworlds.push_back(read_subworld(db, root, std::nullopt, std::nullopt, scope));
	Walker walker(db, p_mount, p_mode, *world);
	BF3_TRY(walker.visit(root, scope, LevelTransform::identity(), 0, 0, 0));
	for (size_t at = 0; at < world->objects.size(); ++at) {
		const PlacedObject &object = world->objects[at];
		world->placed_.emplace(PlacedKey{ object.scope, InstanceKey::of(object.entity) }, uint32_t(at));
		if (object.reference) {
			world->placed_.emplace(PlacedKey{ object.scope, InstanceKey::of(*object.reference) }, uint32_t(at));
		}
	}
	return world;
}

} // namespace bf3::core::level
