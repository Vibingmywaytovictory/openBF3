// Level traversal: port of IW4L level_bf3 world.rs and variations.rs (Apache-2.0).
//
// Cost model. One WorkBudget (budget.h, 2^22 units by default) pays for a level's load, its walk,
// the property resolver and every helper run on the World. Each unit is charged BEFORE its work,
// at the place the work happens; an exhausted budget fails every later charge (sticky), so the
// walk and each helper end with the budget error, and World::traverse checks it once more after
// the walk returns.
// Text: a data string or name costs its length to copy, key, hash, compare with other data, parse
// or print, and any number of slots may name one long string. Decoding one is O(log n) whatever
// its length (EbxPartition::string looks the NUL up in an index built at parse; names are spans),
// and the level code takes every data-controlled text through charged_str / charged_enum_name /
// charge_text (budget.h), which charge text_units = 1 + length/16 before handing it out; no other
// code in src/level reads EbxValue::as_str or enum_name (Property::as_str, a view for callers that
// charge it themselves, is not called by the walk). So text work and every kept copy are at most
// 16 bytes per unit, and long names that are then skipped (over 1,024 bytes) are paid first.
// Partition names come from the bundle manifests, not the EBX bytes, but are text all the same:
// read_subworld charges the sub-world's name before copying it, the census each mesh name it copies
// and keys and each shader name it searches, and bf3dump level each partition name it prints.
// Every loop reachable from the public entry points, and what bounds it:
//   load_level, level_info (level.cpp)
//     subworld_references: 1 per container visited, then the raw length of its Objects before the
//       slots are resolved; also at most 2^16 containers per search, layers 8 deep.
//     includes: the raw length of InclusionSettings.Settings, then of each setting's
//       EnabledOptions, each before it is scanned (shared arrays are charged per scan).
//     includes also charges the text of every criterion Name and EnabledOptions entry it reads.
//     each BundleName a reference wants: its text before it is keyed or copied, then one hash
//       lookup in the mount's bundle index (built once per load from the install's names). A
//       sub-world bundle loads once per key, so bundle reads and partition parsing are bounded by
//       the superbundle (each bundle at most once), not by the graph.
//     read_level_info: the raw lengths of Categories, of each category's Mode array and of both
//       Components arrays, each before it is scanned; the text of every Category and Mode name
//       read (each mode kept is a charged copy, at most 4,096 of them) and of Name / Description;
//       count_imports: once over every import.
//   Walker (this file)
//     visit: 1 per call, then the raw length of Objects before it is scanned; recursion depth is at
//       most 48. Every object slot then does a constant amount of EBX work (is_a against two fixed
//       tables, get, follow), plus what the branch below charges.
//     excluded, blueprint_transform, entity_transform: Scopes::property (below).
//     enter_subworld: the BundleName's text, includes() as above, referenced_blueprint (its text
//       again, then one lookup); then the sub-world's MeshVariationDatabase raw Entries +
//       RedirectEntries before they are read (re-read and re-charged on every entry).
//     place_reference: its slot and the resolver; a prefab recurses into visit.
//     expand_group: the PhysicsData asset name's text on every visit (it is hashed into the res
//       cache each time; names over 1,024 bytes fail once charged); 1 per Havok res read (each
//       distinct res name is read and parsed once per traversal); the raw length of
//       MemberDatas before the members are copied; 1 per instance slot before it is placed (a
//       hostile InstanceCount past the Havok subparts is counted, not walked).
//     emit / classify: constant.
//   Scopes (connections.cpp)
//     property / interface: 1 per connection candidate examined, before it is followed (hop()),
//       also capped at 2^16 per resolution and depth 32. Between two hops a resolution does a
//       constant amount of work: hash lookups, the Descriptor follow, data_field (one search in
//       the source type's lookup table, by stored hash); a hop whose source is a sub-world
//       reference charges its BundleName's text before the name is copied and looked up.
//     Connections::of: the raw lengths of a blueprint's three connection lists, once per blueprint
//       per traversal (cached by scope push and by connections_of).
//     descriptor_field: the raw length of a descriptor's Fields, once per descriptor (cached).
//     an interface constant: its text before Property::parse reads it.
//   Helpers run on the World (census.cpp), the World's budget:
//     starts_hidden: the number of connections into (reference, Visible) before they are checked.
//     hidden_parts: the raw length of each Components and HealthStates array before it is
//       scanned; at most 4,096 components per call.
//     drawn_slots: the shader's partition name text per drawn material slot.
//     mesh_census: legacy variation databases as above; per mesh object 1 for the variation
//       lookup (a parent chain of at most 49 sub-worlds) plus hidden_parts; per variant the raw
//       lengths of the asset's Materials and the entry's Materials, 1 per LOD subset examined
//       (before it is examined), and 1 per triangle examined,
//       paid in doubling blocks before each block (at most twice what is examined). Each distinct
//       MeshSet is read and decoded once (install-bound).
//     collision_placements: per group its res name's text; per non-group mesh object the raw
//       RigidBodies and ScaledAssets lengths, each body's enumerator name, and each asset name's
//       text twice (read, then looked up and copied; names over 1,024 bytes are skipped, charged).
//   bf3dump level (tools/bf3dump/level_commands.cpp) charges the type and partition names it prints.
// Constant factors, which bound the EBX work one unit pays for whatever the type layouts (each
// object slot, hop or text charge does a fixed number of these accesses; LevelTransform reads are
// 16 gets): EbxStruct::get / follow / data_field / as_vec3 / as_transform are one binary search
// per get in the type's lookup table, which EbxPartition::parse flattens along the base chain
// (O(log n), n at most 1,024: no walk over base types or descriptors); is_a walks at most 32 base
// types by stored hash; an enumerator name is one binary search; a string decodes in O(log n);
// compares against fixed names stop at the fixed name's length; no data is hashed. The parser
// rejects types with more than 1,024 descriptors along their base chain and type tables of more
// than 4 lookups per descriptor (retail: 96 and 1.96), and its validation pays for each type's
// descriptors before scanning them (at most 16 steps per partition byte), so a partition's parse
// is linear in its size; each bundle is parsed once per load. Before the lookup tables, a hostile
// type 30 deep and 254 wide made each unit cost about 130k descriptor steps (197 s for a 360 KB
// partition); now deep, wide and shallow layouts run at the same rate (1.3-1.4 s at the full budget).
// Structural caps beside the budget: objects 2^18, scopes 2^18, static model groups 2^16, kept
// sub-world names 4,096 of at most 1,024 bytes, BundleName and res lookups 1,024 bytes, level
// description modes 4,096.
// Retail use (all 230 level x mode runs of the 29 MP levels): see README "Level traversal".
#include "bf3/core/level/world.h"

namespace bf3::core::level {

namespace {

constexpr uint32_t kMaxDepth = 48;
// Placed objects, scopes and static model groups one traversal may create. The depth limit bounds
// nesting, not fan-out: a hostile prefab listing itself many times would otherwise grow without
// bound. Retail levels place at most 15,178 objects in 773 scopes and 3 groups (all 230 level x
// mode runs); 2^18 objects keep a hostile World under about 80 MB (2^20 reached 300 MB).
constexpr size_t kMaxObjects = size_t(1) << 18;
constexpr size_t kMaxScopes = size_t(1) << 18;
constexpr size_t kMaxGroups = size_t(1) << 16;
// Skipped and absent sub-world names kept in WorldStats (retail levels list about a dozen), and
// the bytes kept of each (retail names are under 100).
constexpr size_t kMaxNamedSubworlds = 4096;
constexpr size_t kMaxSubworldName = 1024;
// Longest HavokPhysicsData res name a static model group may name (retail names are under 100).
constexpr size_t kMaxResName = 1024;

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

Result<SubWorld> read_subworld(const EbxResolver &p_db, const EbxInstance &p_data, std::optional<EbxInstance> p_reference, std::optional<uint32_t> p_parent,
		uint32_t p_scope, WorkBudget &r_budget) {
	SubWorld sw;
	// The partition name comes from the bundle manifest: charged before it is copied and extended.
	const std::string_view name = p_db.name_of(p_data.partition().guid()).value_or("");
	BF3_TRY(charge_text(r_budget, name));
	sw.name = std::string(name);
	sw.data = p_data;
	sw.reference = p_reference;
	sw.parent = p_parent;
	sw.scope = p_scope;
	if (const EbxPartition *mvdb = p_db.partition_by_name(sw.name + "/meshvariationdb_win32")) {
		Result<MeshVariationDb> variations = MeshVariationDb::read(p_db, mvdb->primary(), r_budget);
		if (!variations) {
			return variations.error();
		}
		sw.variations = std::move(*variations);
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

Result<std::string_view> MeshObject::name(WorkBudget &r_budget) const {
	const Result<std::optional<std::string_view>> name = charged_str(asset.data(), "Name", r_budget);
	if (!name) {
		return name.error();
	}
	return name->value_or("");
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

Result<MeshVariationDb> MeshVariationDb::read(const EbxResolver &p_db, const EbxInstance &p_database, WorkBudget &r_budget, bool p_keep_empty) {
	MeshVariationDb out;
	out.database_ = p_database;
	for (const char *list : { "Entries", "RedirectEntries" }) {
		std::vector<EbxInstance> entries;
		BF3_TRY(charged_follow_all(p_db, p_database.data(), list, r_budget, entries));
		out.scanned_ += array_length(p_database.data(), list);
		for (const EbxInstance &entry : entries) {
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
		BF3_TRY(charge(1));
		if (p_depth > kMaxDepth) {
			++world_.stats.depth_limited;
			return Status::success();
		}
		std::vector<EbxInstance> container_objects;
		BF3_TRY(objects(p_container, container_objects));
		for (const EbxInstance &object : container_objects) {
			if (object.is_a("WorldPartReferenceObjectData")) {
				Result<bool> skip = excluded(p_scope, object);
				if (!skip) {
					return skip.error();
				}
				if (*skip) {
					continue;
				}
				const std::optional<EbxInstance> part = db_.follow(object.data(), "Blueprint");
				if (!part) {
					++world_.stats.missing_blueprints;
					continue;
				}
				Result<LevelTransform> local = blueprint_transform(p_scope, object);
				if (!local) {
					return local.error();
				}
				const LevelTransform frame = local->in_parent(p_frame);
				BF3_TRY(visit(*part, p_scope, frame, p_subworld, 0, p_depth + 1));
			} else if (object.is_a("SubWorldReferenceObjectData")) {
				BF3_TRY(enter_subworld(object, p_scope, p_frame, p_subworld, p_depth));
			} else if (object.is_a("ReferenceObjectData")) {
				BF3_TRY(place_reference(object, p_scope, p_frame, p_subworld, p_variation, p_depth));
			} else if (object.is_a("StaticModelGroupEntityData")) {
				BF3_TRY(expand_group(object, p_scope, p_frame, p_subworld));
			} else {
				Result<LevelTransform> local = entity_transform(p_scope, object);
				if (!local) {
					return local.error();
				}
				emit(object, std::nullopt, local->in_parent(p_frame), p_scope, p_subworld, p_variation);
			}
		}
		return Status::success();
	}

private:
	// Pays for p_steps of walker work about to be done, and checks the structural caps.
	Status charge(size_t p_steps) {
		world_.stats.steps += p_steps;
		BF3_TRY(world_.budget->charge(p_steps));
		if (world_.objects.size() > kMaxObjects || world_.scopes.size() > kMaxScopes || world_.groups.size() > kMaxGroups) {
			return make_error("level traversal: more than " + std::to_string(kMaxObjects) + " placed objects, " + std::to_string(kMaxScopes) +
					" scopes or " + std::to_string(kMaxGroups) + " static model groups");
		}
		return Status::success();
	}

	// The resolved Objects of p_container; each slot costs a step, null or unresolved ones too.
	Status objects(const EbxInstance &p_container, std::vector<EbxInstance> &r_out) {
		const std::optional<EbxValue> list = p_container.get("Objects");
		const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
		if (!array) {
			return Status::success();
		}
		BF3_TRY(charge(array->size()));
		r_out.reserve(array->size());
		for (const EbxValue value : *array) {
			const std::optional<EbxRef> ref = value.as_ref();
			if (!ref || ref->is_null()) {
				continue;
			}
			if (const std::optional<EbxInstance> object = db_.resolve(p_container.partition(), *ref)) {
				r_out.push_back(*object);
			} else {
				++world_.stats.unresolved_objects;
			}
		}
		return Status::success();
	}

	Status enter_subworld(const EbxInstance &p_reference, uint32_t p_scope, const LevelTransform &p_frame, uint32_t p_subworld, uint32_t p_depth) {
		const Result<std::optional<std::string_view>> bundle_name = charged_str(p_reference.data(), "BundleName", *world_.budget);
		if (!bundle_name) {
			return bundle_name.error();
		}
		const std::string_view name = bundle_name->value_or("");
		const Result<bool> included = includes(db_, p_reference, mode_, *world_.budget);
		if (!included) {
			return included.error();
		}
		if (!*included) {
			note_subworld(world_.stats.skipped_subworlds, name);
			return Status::success();
		}
		Result<bool> skip = excluded(p_scope, p_reference);
		if (!skip) {
			return skip.error();
		}
		if (*skip) {
			return Status::success();
		}
		const Result<std::optional<EbxInstance>> referenced = world_.scopes.referenced_blueprint(p_reference);
		if (!referenced) {
			return referenced.error();
		}
		const std::optional<EbxInstance> data = *referenced;
		if (!data) {
			note_subworld(world_.stats.absent_subworlds, name);
			return Status::success();
		}
		Result<LevelTransform> local = blueprint_transform(p_scope, p_reference);
		if (!local) {
			return local.error();
		}
		const LevelTransform frame = local->in_parent(p_frame);
		const uint32_t child = uint32_t(world_.subworlds.size());
		Result<uint32_t> child_scope = world_.scopes.push(*data, p_reference, p_scope, frame, child, 0);
		if (!child_scope) {
			return child_scope.error();
		}
		Result<SubWorld> sw = read_subworld(db_, *data, p_reference, p_subworld, *child_scope, *world_.budget);
		if (!sw) {
			return sw.error();
		}
		world_.stats.steps += sw->variations.scanned();
		world_.subworlds.push_back(std::move(*sw));
		return visit(*data, *child_scope, frame, child, 0, p_depth + 1);
	}

	// Records a skipped or absent sub-world's BundleName. A hostile graph can reach the same
	// reference millions of times, so the lists keep the first kMaxNamedSubworlds names, each cut
	// to kMaxSubworldName bytes.
	void note_subworld(std::vector<std::string> &r_names, std::string_view p_name) {
		if (r_names.size() < kMaxNamedSubworlds) {
			r_names.emplace_back(p_name.substr(0, kMaxSubworldName));
		} else {
			++world_.stats.unnamed_subworlds;
		}
	}

	Status place_reference(const EbxInstance &p_reference, uint32_t p_scope, const LevelTransform &p_frame, uint32_t p_subworld, uint32_t p_variation,
			uint32_t p_depth) {
		Result<bool> skip = excluded(p_scope, p_reference);
		if (!skip) {
			return skip.error();
		}
		if (*skip) {
			return Status::success();
		}
		Result<LevelTransform> own = blueprint_transform(p_scope, p_reference);
		if (!own) {
			return own.error();
		}
		const LevelTransform transform = own->in_parent(p_frame);
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
			Result<uint32_t> child = world_.scopes.push(*blueprint, p_reference, p_scope, transform, p_subworld, variation);
			if (!child) {
				return child.error();
			}
			return visit(*blueprint, *child, transform, p_subworld, variation, p_depth + 1);
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
		Result<LevelTransform> own = entity_transform(p_scope, p_group);
		if (!own) {
			return own.error();
		}
		const LevelTransform transform = own->in_parent(p_frame);
		StaticModelGroup group;
		group.entity = p_group;
		group.subworld = p_subworld;
		group.scope = p_scope;
		group.transform = transform;
		if (const std::optional<EbxInstance> physics = db_.follow(p_group.data(), "PhysicsData")) {
			if (const std::optional<EbxInstance> asset = db_.follow(physics->data(), "Asset")) {
				const Result<std::optional<std::string_view>> asset_name = charged_str(asset->data(), "Name", *world_.budget);
				if (!asset_name) {
					return asset_name.error();
				}
				if (const std::optional<std::string_view> name = *asset_name) {
					if (name->size() > kMaxResName) {
						// No res has such a name; do not copy it on every visit.
						return make_error("static model group " + p_group.guid().to_string() + ": PhysicsData asset name of " + std::to_string(name->size()) +
								" bytes");
					}
					group.physics_asset = std::string(*name);
				}
			}
		}
		const std::string group_name = group.physics_asset.value_or(p_group.guid().to_string());
		const std::vector<res::SubpartTransform> *subparts_of = &kNoSubparts;
		if (group.physics_asset) {
			// Each res is read and parsed once per traversal, however often the group is visited.
			auto cached = havok_.find(*group.physics_asset);
			if (cached == havok_.end()) {
				BF3_TRY(charge(1));
				Result<std::pair<ResInfo, std::vector<uint8_t>>> res = mount_.read_res(*group.physics_asset);
				if (!res) {
					return error_at("res " + *group.physics_asset, res.error());
				}
				auto havok = std::make_shared<res::HavokPhysicsData>();
				const res::ResError parsed = res::HavokPhysicsData::parse(res->second, res->first.res_meta, *havok);
				if (!parsed.ok()) {
					return make_error(*group.physics_asset + ": " + parsed.message());
				}
				std::vector<res::SubpartTransform> listed_subparts;
				const res::ResError listed = havok->group_subparts(listed_subparts);
				if (!listed.ok()) {
					return make_error(group_name + ": " + listed.message());
				}
				cached = havok_.emplace(*group.physics_asset, ParsedHavok{ std::move(havok), std::move(listed_subparts) }).first;
			}
			group.havok = cached->second.havok;
			subparts_of = &cached->second.subparts;
		}
		const std::vector<res::SubpartTransform> &subparts = *subparts_of;

		const uint32_t index = uint32_t(world_.groups.size());
		std::vector<EbxStruct> members;
		const std::optional<EbxValue> member_list = p_group.get("MemberDatas");
		const OptionalEbxArray member_array = member_list ? member_list->as_array() : OptionalEbxArray();
		if (member_array) {
			// Each member slot costs a step: members that place nothing still cost their lookups.
			BF3_TRY(charge(member_array->size()));
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
				BF3_TRY(charge(1)); // every instance slot, placed or not
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

	Result<bool> excluded(uint32_t p_scope, const EbxInstance &p_reference) {
		bool excluded = false;
		const Result<std::optional<Property>> fed = world_.scopes.property(p_scope, p_reference, field_id("Excluded"));
		if (!fed) {
			return fed.error();
		}
		const std::optional<bool> value = *fed ? (*fed)->as_bool() : std::nullopt;
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

	Result<LevelTransform> blueprint_transform(uint32_t p_scope, const EbxInstance &p_reference) {
		return overridable_transform(p_scope, p_reference, "BlueprintTransform");
	}

	Result<LevelTransform> entity_transform(uint32_t p_scope, const EbxInstance &p_entity) {
		return overridable_transform(p_scope, p_entity, "Transform");
	}

	Result<LevelTransform> overridable_transform(uint32_t p_scope, const EbxInstance &p_object, std::string_view p_field) {
		const std::optional<EbxValue> data = p_object.get(p_field);
		const auto rows = data ? data->as_transform() : std::nullopt;
		if (!rows) {
			return LevelTransform::identity();
		}
		const Result<std::optional<Property>> fed = world_.scopes.property(p_scope, p_object, field_id(p_field));
		if (!fed) {
			return fed.error();
		}
		if (const std::optional<LevelTransform> transform = *fed ? (*fed)->as_transform() : std::nullopt) {
			++world_.stats.transform_overrides;
			return *transform;
		}
		return LevelTransform::from_rows(*rows);
	}

	struct ParsedHavok {
		std::shared_ptr<const res::HavokPhysicsData> havok;
		std::vector<res::SubpartTransform> subparts;
	};
	static inline const std::vector<res::SubpartTransform> kNoSubparts;

	const EbxResolver &db_;
	const LevelMount &mount_;
	std::string_view mode_;
	World &world_;
	std::unordered_map<std::string, ParsedHavok> havok_; // by res name
};

Result<std::unique_ptr<World>> World::traverse(const LoadedLevel &p_level) {
	return traverse(p_level.db(), p_level.root(), p_level.mount(), p_level.mode, p_level.budget);
}

Result<std::unique_ptr<World>> World::traverse(const EbxResolver &p_db, const EbxInstance &p_root, const LevelMount &p_mount, std::string_view p_mode,
		std::shared_ptr<WorkBudget> p_budget) {
	const EbxResolver &db = p_db;
	const EbxInstance root = p_root;
	std::shared_ptr<WorkBudget> budget = p_budget ? std::move(p_budget) : std::make_shared<WorkBudget>();
	auto world = std::make_unique<World>(db, budget);
	BF3_TRY(budget->status());
	const Result<uint32_t> scope = world->scopes.push(root, std::nullopt, std::nullopt, LevelTransform::identity(), 0, 0);
	if (!scope) {
		return scope.error();
	}
	Result<SubWorld> level = read_subworld(db, root, std::nullopt, std::nullopt, *scope, *budget);
	if (!level) {
		return level.error();
	}
	world->stats.steps += level->variations.scanned();
	world->subworlds.push_back(std::move(*level));
	Walker walker(db, p_mount, p_mode, *world);
	BF3_TRY(walker.visit(root, *scope, LevelTransform::identity(), 0, 0, 0));
	// Every charge that failed also failed the walk; this only makes sure nothing ends ok on an
	// exhausted budget.
	BF3_TRY(budget->status());
	world->stats.resolve_hops = world->scopes.hops();
	world->stats.capped_resolutions = world->scopes.capped_resolutions();
	world->stats.work = budget->spent();
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
