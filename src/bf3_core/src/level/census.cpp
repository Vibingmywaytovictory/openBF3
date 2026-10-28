#include "bf3/core/level/census.h"

#include "bf3/core/res/mesh_set.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

namespace bf3::core::level {

namespace {

constexpr size_t kMaxLods = 4;
constexpr float kWorldRadius = 16.0f;
constexpr size_t kWorldMaxPlacements = 4;
constexpr size_t kMaxComponents = 4096;

constexpr size_t kMaxResName = 1024;
constexpr std::string_view kUndrawnShaders[5] = { "invisible", "occluder", "shadowmesh", "lightcone_particles",
	"ui_menu" };

using Vec3f = core::Vec3;
using Mat3 = std::array<Vec3f, 3>;

float dot(const Vec3f &p_a, const Vec3f &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}

Vec3f cross(const Vec3f &p_a, const Vec3f &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}

Vec3f sub(const Vec3f &p_a, const Vec3f &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

Vec3f scaled(const Vec3f &p_a, float p_s) {
	return { p_a[0] * p_s, p_a[1] * p_s, p_a[2] * p_s };
}

float length(const Vec3f &p_a) {
	return std::sqrt(dot(p_a, p_a));
}

Vec3f normalize(const Vec3f &p_a) {
	const float len = length(p_a);
	return len > 1e-12f ? scaled(p_a, 1.0f / len) : p_a;
}

float determinant(const Mat3 &p_m) {
	return dot(p_m[0], cross(p_m[1], p_m[2]));
}

std::string instance_id(const EbxResolver &p_db, const EbxInstance &p_instance) {
	return std::string(p_db.name_of(p_instance.partition().guid()).value_or("?")) + "#" + std::to_string(p_instance.index());
}

Status drawn_slots(const EbxResolver &p_db, const EbxInstance &p_asset, const std::optional<EbxInstance> &p_entry, WorkBudget &r_budget,
		std::vector<bool> &r_out) {
	std::vector<EbxInstance> materials;
	BF3_TRY(charged_follow_all(p_db, p_asset.data(), "Materials", r_budget, materials));
	std::vector<EbxStruct> variations;
	if (p_entry) {
		const std::optional<EbxValue> list = p_entry->get("Materials");
		const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
		if (array) {
			BF3_TRY(r_budget.charge(array->size()));
			for (const EbxValue v : *array) {
				if (const std::optional<EbxStruct> s = v.as_struct()) {
					variations.push_back(*s);
				}
			}
		}
	}

	std::unordered_map<InstanceKey, size_t, InstanceKeyHash> first_naming;
	for (size_t i = 0; i < variations.size(); ++i) {
		if (const std::optional<EbxInstance> named = p_db.follow(variations[i], "Material")) {
			first_naming.emplace(InstanceKey::of(*named), i);
		}
	}
	const size_t count = std::max(materials.size(), variations.size());
	std::vector<bool> &out = r_out;
	out.assign(count, true);
	for (size_t slot = 0; slot < count; ++slot) {
		const std::optional<EbxInstance> material = slot < materials.size() ? std::optional<EbxInstance>(materials[slot]) : std::nullopt;
		std::optional<EbxStruct> variation;
		if (material) {
			const auto found = first_naming.find(InstanceKey::of(*material));
			if (found != first_naming.end()) {
				variation = variations[found->second];
			}
		}
		if (!variation && slot < variations.size()) {
			variation = variations[slot];
		}
		std::optional<EbxInstance> shader_material = material;
		if (!shader_material && variation) {
			shader_material = p_db.follow(*variation, "Material");
		}
		if (!shader_material) {
			continue;
		}
		const std::optional<EbxValue> shader_value = shader_material->get("Shader");
		const std::optional<EbxStruct> shader_struct = shader_value ? shader_value->as_struct() : std::nullopt;
		const std::optional<EbxInstance> shader = shader_struct ? p_db.follow(*shader_struct, "Shader") : std::nullopt;
		const std::optional<std::string_view> name = shader ? p_db.name_of(shader->partition().guid()) : std::nullopt;
		if (!name) {
			continue;
		}
		BF3_TRY(charge_text(r_budget, *name));
		for (std::string_view word : kUndrawnShaders) {
			if (name->find(word) != std::string_view::npos) {
				out[slot] = false;
				break;
			}
		}
	}
	return Status::success();
}

Status draws_triangle(const res::DecodedSubset &p_subset, const std::vector<uint16_t> &p_hidden, WorkBudget &r_budget, bool &r_draws) {
	r_draws = false;
	const auto hidden = [&](uint32_t p_index) {
		if (!p_subset.parts || p_index >= p_subset.parts->size()) {
			return false;
		}
		return std::binary_search(p_hidden.begin(), p_hidden.end(), (*p_subset.parts)[p_index]);
	};
	const std::vector<uint32_t> &indices = p_subset.indices;
	const size_t triangles = indices.size() / 3;
	size_t paid = 0;
	for (size_t t = 0; t < triangles; ++t) {
		if (t == paid) {
			const size_t block = std::min(std::max<size_t>(paid, 1), triangles - t);
			BF3_TRY(r_budget.charge(block));
			paid += block;
		}
		const size_t i = t * 3;
		const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
		if (!p_hidden.empty() && (hidden(a) || hidden(b) || hidden(c))) {
			continue;
		}
		if (a != b && b != c && a != c) {
			r_draws = true;
			return Status::success();
		}
	}
	return Status::success();
}

int32_t total_order_key(float p_value) {
	int32_t bits = 0;
	std::memcpy(&bits, &p_value, 4);
	return bits ^ int32_t(uint32_t(bits >> 31) >> 1);
}

struct MeshResult {
	float radius = 0.0f;
	std::vector<std::vector<bool>> lods;
	std::string error;
	bool out_of_budget = false;
};

}

Result<std::optional<std::vector<uint16_t>>> hidden_parts(const EbxResolver &p_db, const EbxInstance &p_entity, WorkBudget &r_budget) {
	std::vector<uint16_t> intact;
	std::vector<uint16_t> damaged;
	bool found = false;
	std::vector<EbxInstance> pending;
	BF3_TRY(charged_follow_all(p_db, p_entity.data(), "Components", r_budget, pending));
	size_t seen = 0;
	while (!pending.empty()) {
		const EbxInstance component = pending.back();
		pending.pop_back();
		if (++seen > kMaxComponents) {
			break;
		}
		std::vector<EbxInstance> children;
		BF3_TRY(charged_follow_all(p_db, component.data(), "Components", r_budget, children));
		for (const EbxInstance &child : children) {
			if (pending.size() >= kMaxComponents) {
				break;
			}
			pending.push_back(child);
		}
		if (!component.is_a("PartComponentData")) {
			continue;
		}
		found = true;
		std::vector<EbxInstance> states;
		BF3_TRY(charged_follow_all(p_db, component.data(), "HealthStates", r_budget, states));
		for (size_t state = 0; state < states.size(); ++state) {
			const std::optional<EbxValue> v = states[state].get("PartIndex");
			const std::optional<int64_t> part = v ? v->as_i64() : std::nullopt;
			if (!part || *part < 0 || *part > 0xFFFF) {
				continue;
			}
			(state == 0 ? intact : damaged).push_back(uint16_t(*part));
		}
	}
	std::sort(damaged.begin(), damaged.end());
	damaged.erase(std::unique(damaged.begin(), damaged.end()), damaged.end());
	std::sort(intact.begin(), intact.end());
	std::erase_if(damaged, [&](uint16_t p_part) { return std::binary_search(intact.begin(), intact.end(), p_part); });
	if (!found) {
		return std::optional<std::vector<uint16_t>>();
	}
	return std::optional<std::vector<uint16_t>>(std::move(damaged));
}

Result<bool> starts_hidden(const World &p_world, const PlacedObject &p_object) {
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	if (!p_object.reference) {
		return false;
	}

	const Connections &connections = p_world.scopes.connections(p_object.scope);
	const std::vector<uint32_t> *into = connections.property_into(*p_object.reference, field_id("Visible"));
	if (!into) {
		return false;
	}
	BF3_TRY(budget.charge(into->size()));
	const int32_t value = field_id("Value");
	for (const uint32_t at : *into) {
		const Connection &c = connections.property[at];
		if (c.source.is_a("BoolEntityData") && c.source_field == value) {
			const std::optional<EbxValue> d = c.source.get("DefaultValue");
			if ((d ? d->as_bool() : std::nullopt) == false) {
				return true;
			}
		}
	}
	return false;
}

BakeSplit bake_split(const LevelTransform &p_transform) {
	constexpr float kTolerance = 1e-3f;
	BakeSplit out;
	out.placed = p_transform;
	const Mat3 rows = { scaled(p_transform.right, p_transform.scale), scaled(p_transform.up, p_transform.scale),
		scaled(p_transform.forward, p_transform.scale) };
	const float lengths[3] = { length(rows[0]), length(rows[1]), length(rows[2]) };
	const float largest = std::fmax(std::fmax(lengths[0], lengths[1]), lengths[2]);
	bool orthogonal = true;
	for (int a = 0; a < 3; ++a) {
		const int b = (a + 1) % 3;
		if (!(std::fabs(dot(rows[a], rows[b])) <= kTolerance * lengths[a] * lengths[b])) {
			orthogonal = false;
			break;
		}
	}
	const bool positive = determinant(rows) > 0.0f;
	if (p_transform.is_uniform(kTolerance) && orthogonal && positive && largest > 0.0f) {
		return out;
	}
	const Vec3f x = normalize(rows[0]);
	const Vec3f y = normalize(sub(rows[1], scaled(x, dot(rows[1], x))));
	const Vec3f z = cross(x, y);
	const Mat3 basis = { x, y, z };
	Mat3 linear{};
	for (int i = 0; i < 3; ++i) {
		linear[i] = { dot(rows[0], basis[i]), dot(rows[1], basis[i]), dot(rows[2], basis[i]) };
	}
	if (std::fabs(determinant(linear)) < 1e-12f) {
		return out;
	}
	out.placed.right = x;
	out.placed.up = y;
	out.placed.forward = z;
	out.placed.scale = 1.0f;
	out.linear = linear;
	return out;
}

std::array<int32_t, 9> bake_key(const std::array<core::Vec3, 3> &p_linear) {
	std::array<int32_t, 9> key{};
	for (size_t i = 0; i < 9; ++i) {
		const float v = std::round(p_linear[i / 3][i % 3] * 4096.0f);

		key[i] = std::isnan(v) ? 0 : v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : int32_t(v);
	}
	return key;
}

Result<MeshCensus> mesh_census(const LoadedLevel &p_level, const World &p_world, const CensusOptions &p_options) {
	return mesh_census(p_level.db(), p_level.mount(), p_world, p_options);
}

Result<MeshCensus> mesh_census(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world, const CensusOptions &p_options) {
	const EbxResolver &db = p_db;
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	MeshCensus census;

	std::vector<MeshVariationDb> legacy;
	if (p_options.legacy_variations) {
		for (const SubWorld &sw : p_world.subworlds) {
			if (!sw.variations.database()) {
				legacy.emplace_back();
				continue;
			}
			Result<MeshVariationDb> read = MeshVariationDb::read(db, *sw.variations.database(), budget, true);
			if (!read) {
				return read.error();
			}
			legacy.push_back(std::move(*read));
		}
	}
	const auto variation_entry = [&](uint32_t p_subworld, const EbxInstance &p_mesh, uint32_t p_variation) -> std::optional<EbxInstance> {
		if (!p_options.legacy_variations) {
			return p_world.mesh_variation(p_subworld, p_mesh, p_variation);
		}
		const auto lookup = [&](uint32_t p_hash) -> std::optional<EbxInstance> {
			std::optional<uint32_t> at = p_subworld;
			for (size_t hops = 0; at && *at < legacy.size() && hops <= legacy.size(); ++hops) {
				if (std::optional<EbxInstance> e = legacy[*at].entry(p_mesh, p_hash)) {
					return e;
				}
				at = p_world.subworlds[*at].parent;
			}
			return std::nullopt;
		};
		if (std::optional<EbxInstance> e = lookup(p_variation)) {
			return e;
		}
		return p_variation != 0 ? lookup(0) : std::nullopt;
	};

	const auto variant_key = [](const InstanceKey &p_asset, const std::optional<InstanceKey> &p_entry, const std::vector<uint16_t> &p_hidden,
									 const std::optional<std::array<int32_t, 9>> &p_bake) {
		std::string key;
		const auto add = [&key](uint64_t p_value) {
			key.append(reinterpret_cast<const char *>(&p_value), sizeof(p_value));
		};
		add(uint64_t(reinterpret_cast<uintptr_t>(p_asset.partition)));
		add(p_asset.index);
		add(p_entry ? uint64_t(reinterpret_cast<uintptr_t>(p_entry->partition)) : 0);
		add(p_entry ? p_entry->index : UINT64_MAX);
		add(p_hidden.size());
		for (uint16_t part : p_hidden) {
			add(part);
		}
		add(p_bake ? 1 : 0);
		if (p_bake) {
			for (int32_t v : *p_bake) {
				add(uint32_t(v));
			}
		}
		return key;
	};
	std::map<std::string, uint32_t> by_key;
	std::vector<std::vector<std::pair<uint32_t, LevelTransform>>> placements;
	for (size_t at = 0; at < p_world.objects.size(); ++at) {
		const PlacedObject &object = p_world.objects[at];
		if (object.kind != ObjectKind::Mesh || !object.mesh) {
			continue;
		}
		const EbxInstance &asset = object.mesh->asset;
		const std::optional<std::string_view> name = db.name_of(asset.partition().guid());
		if (!name) {
			continue;
		}
		BF3_TRY(budget.charge(1));
		const std::optional<EbxInstance> entry = variation_entry(object.subworld, asset, object.variation);
		std::vector<uint16_t> hidden;
		if (asset.is_a("CompositeMeshAsset")) {
			Result<std::optional<std::vector<uint16_t>>> parts = hidden_parts(db, object.entity, budget);
			if (!parts) {
				return parts.error();
			}
			hidden = (*parts).value_or(std::vector<uint16_t>());
		}
		const BakeSplit split = bake_split(object.transform);
		++census.placements;
		std::optional<std::array<int32_t, 9>> bake;
		if (split.linear) {
			bake = bake_key(*split.linear);
		}
		std::string key = variant_key(InstanceKey::of(asset), entry ? std::optional<InstanceKey>(InstanceKey::of(*entry)) : std::nullopt, hidden, bake);
		const auto found = by_key.find(key);
		uint32_t variant = 0;
		if (found != by_key.end()) {
			variant = found->second;
		} else {
			variant = uint32_t(census.meshes.size());
			BF3_TRY(budget.charge(text_units(name->size())));
			SceneMesh mesh;
			mesh.asset = asset;
			mesh.name = std::string(*name);
			mesh.entry = entry;
			mesh.hidden = std::move(hidden);
			mesh.bake = split.linear;
			if (bake) {
				mesh.bake_key = *bake;
			}
			census.meshes.push_back(std::move(mesh));
			placements.emplace_back();
			by_key.emplace(std::move(key), variant);
		}
		placements[variant].emplace_back(uint32_t(at), split.placed);
	}

	std::map<std::string, std::vector<uint32_t>> by_name;
	for (uint32_t v = 0; v < census.meshes.size(); ++v) {
		by_name[census.meshes[v].name].push_back(v);
	}
	std::vector<const std::pair<const std::string, std::vector<uint32_t>> *> names;
	for (const auto &entry : by_name) {
		names.push_back(&entry);
	}
	BF3_TRY(budget.charge(names.size()));
	const LevelMount &mount = p_mount;
	std::vector<MeshResult> results = parallel_map<MeshResult>(
			names.size(),
			[&](size_t i) -> MeshResult {
				MeshResult r;
				const std::string &name = names[i]->first;
				Result<std::pair<ResInfo, std::vector<uint8_t>>> res = mount.read_res(name);
				if (!res) {
					r.error = res.error().message;
					return r;
				}
				if (res->first.res_type != res::kResTypeMeshSet) {
					char text[32];
					std::snprintf(text, sizeof(text), "res type %08X", res->first.res_type);
					r.error = text;
					return r;
				}
				res::MeshSet set;
				const res::ResError parsed = res::MeshSet::parse(res->second, res->first.res_meta, set);
				if (!parsed.ok()) {
					r.error = parsed.message();
					return r;
				}
				const std::vector<uint32_t> &variants = names[i]->second;
				std::vector<std::vector<bool>> lods(variants.size());
				std::vector<std::vector<bool>> slots(variants.size());
				for (size_t v = 0; v < variants.size(); ++v) {
					const SceneMesh &mesh = census.meshes[variants[v]];
					if (!drawn_slots(db, mesh.asset, mesh.entry, budget, slots[v])) {
						r.out_of_budget = true;
						return r;
					}
				}
				for (size_t l = 0; l < set.lods.size() && l < kMaxLods; ++l) {
					const res::MeshLod &lod = set.lods[l];
					std::vector<uint8_t> chunk;
					res::Bytes geometry;
					if (lod.geometry.is_inline) {
						const res::ResError inlined = lod.inline_geometry(res->second, geometry);
						if (!inlined.ok()) {
							r.error = inlined.message();
							return r;
						}
					} else {
						Result<std::vector<uint8_t>> bytes = mount.read_chunk(Guid::from_bytes(lod.geometry.chunk.bytes.data()));
						if (!bytes) {
							r.error = "chunk " + lod.geometry.chunk.to_string() + ": " + bytes.error().message;
							return r;
						}
						chunk = std::move(*bytes);
						geometry = chunk;
					}
					std::vector<res::DecodedSubset> subsets;
					const res::ResError decoded = lod.decode(geometry, res::SubsetFilter::Drawn, subsets);
					if (!decoded.ok()) {
						r.error = decoded.message();
						return r;
					}
					for (size_t v = 0; v < variants.size(); ++v) {
						const SceneMesh &mesh = census.meshes[variants[v]];
						bool draws = false;
						for (const res::DecodedSubset &subset : subsets) {

							if (!budget.charge(1)) {
								r.out_of_budget = true;
								return r;
							}
							const bool slot_draws = subset.material_index < slots[v].size() ? slots[v][subset.material_index] : true;
							if (!slot_draws) {
								continue;
							}
							if (!draws_triangle(subset, mesh.hidden, budget, draws)) {
								r.out_of_budget = true;
								return r;
							}
							if (draws) {
								break;
							}
						}
						lods[v].push_back(draws);
					}
				}
				float sum = 0.0f;
				for (int axis = 0; axis < 3; ++axis) {
					const float d = set.bounds_max[axis] - set.bounds_min[axis];
					sum += d * d;
				}
				r.radius = std::sqrt(sum) * 0.5f;
				r.lods = std::move(lods);
				return r;
			},
			p_options.threads);
	BF3_TRY(budget.status());

	for (size_t i = 0; i < names.size(); ++i) {
		const MeshResult &r = results[i];
		const std::vector<uint32_t> &variants = names[i]->second;
		if (!r.error.empty()) {
			census.failures.emplace_back(names[i]->first, r.error);
			for (uint32_t v : variants) {
				census.meshes[v].failed = true;
			}
			continue;
		}
		for (size_t k = 0; k < variants.size(); ++k) {
			SceneMesh &mesh = census.meshes[variants[k]];
			const std::vector<std::pair<uint32_t, LevelTransform>> &placed = placements[variants[k]];
			mesh.placements = placed.size();
			mesh.lods = r.lods[k];
			float bake_scale = 1.0f;
			if (mesh.bake) {
				bake_scale = 0.0f;
				for (const Vec3f &row : *mesh.bake) {
					bake_scale = std::fmax(bake_scale, std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
				}
			}
			float placed_scale = 0.0f;
			for (const auto &[object, transform] : placed) {
				placed_scale = std::fmax(placed_scale, transform.normalized().scale);
			}
			mesh.radius = r.radius * (placed_scale * bake_scale);
			mesh.world_kind = placed.size() <= kWorldMaxPlacements && mesh.radius >= kWorldRadius;
			const bool lod0 = !mesh.lods.empty() && mesh.lods[0];
			const bool lod1 = mesh.lods.size() > 1 && mesh.lods[1];
			mesh.kept = mesh.world_kind ? lod0 : (lod0 || lod1);
		}
	}
	for (uint32_t v = 0; v < census.meshes.size(); ++v) {
		SceneMesh &mesh = census.meshes[v];
		if (mesh.placements == 0) {
			mesh.placements = placements[v].size();
		}
		if (!mesh.kept) {
			continue;
		}
		++census.scene_meshes;
		++(mesh.world_kind ? census.world_meshes : census.model_meshes);
		for (const auto &[object, transform] : placements[v]) {
			census.instances.push_back(SceneInstance{ v, object, transform });
		}
	}
	census.scene_instances = census.instances.size();
	for (const PlacedObject &object : p_world.objects) {
		if (object.kind == ObjectKind::Terrain || object.kind == ObjectKind::Water) {
			++census.scene_meshes;
			++census.world_meshes;
			++census.scene_instances;
		}
	}
	return census;
}

Result<std::vector<CollisionPlacement>> collision_placements(const LoadedLevel &p_level, const World &p_world) {
	return collision_placements(p_level.db(), p_level.mount(), p_world);
}

Result<std::vector<CollisionPlacement>> collision_placements(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world) {
	const EbxResolver &db = p_db;
	const LevelMount &mount = p_mount;
	WorkBudget &budget = *p_world.budget;
	BF3_TRY(budget.status());
	std::vector<CollisionPlacement> out;
	for (size_t at = 0; at < p_world.groups.size(); ++at) {
		const StaticModelGroup &group = p_world.groups[at];
		if (!group.physics_asset) {
			continue;
		}
		BF3_TRY(budget.charge(text_units(group.physics_asset->size())));
		CollisionPlacement c;
		c.kind = CollisionPlacement::Kind::Group;
		c.index = uint32_t(at);
		c.res = *group.physics_asset;
		c.present = mount.res_info(c.res).has_value();
		c.subparts = group.subparts;
		const LevelTransform &t = group.transform;
		c.frame = res::Transform::from_axes(
				{ scaled(t.right, t.scale), scaled(t.up, t.scale), scaled(t.forward, t.scale) }, t.trans);
		out.push_back(std::move(c));
	}
	for (size_t at = 0; at < p_world.objects.size(); ++at) {
		const PlacedObject &object = p_world.objects[at];
		if (object.group || !object.mesh || !object.mesh->physics) {
			continue;
		}
		const EbxInstance &physics = *object.mesh->physics;
		CollisionPlacement c;
		c.kind = CollisionPlacement::Kind::Object;
		c.index = uint32_t(at);
		std::vector<EbxInstance> rigid_bodies;
		BF3_TRY(charged_follow_all(db, physics.data(), "RigidBodies", budget, rigid_bodies));
		for (const EbxInstance &body : rigid_bodies) {

			const Result<std::optional<std::string_view>> name = charged_enum_name(body.get("RigidBodyType"), budget);
			if (!name) {
				return name.error();
			}
			c.bodies.push_back(name->value_or("?"));
		}

		const Vec3f s = object.transform.axis_scales();
		const float placed = std::fmax(std::cbrt(std::fabs(s[0] * s[1] * s[2])), 1e-6f);
		std::vector<std::pair<std::string_view, float>> assets;
		std::vector<EbxInstance> candidates;
		BF3_TRY(charged_follow_all(db, physics.data(), "ScaledAssets", budget, candidates));
		if (const std::optional<EbxInstance> asset = db.follow(physics.data(), "Asset")) {
			candidates.push_back(*asset);
		}
		for (const EbxInstance &asset : candidates) {
			const Result<std::optional<std::string_view>> charged = charged_str(asset.data(), "Name", budget);
			if (!charged) {
				return charged.error();
			}
			const std::optional<std::string_view> name = *charged;
			if (!name || name->size() > kMaxResName) {
				continue;
			}

			BF3_TRY(charge_text(budget, *name));
			const std::optional<EbxValue> scale_value = asset.get("Scale");
			std::optional<float> scale = scale_value ? scale_value->as_f32() : std::nullopt;
			if (!scale || !(*scale > 0.0f)) {
				scale = 1.0f;
			}
			assets.emplace_back(*name, *scale);
		}
		const auto off = [&](float p_scale) { return total_order_key(std::fabs(std::log(p_scale / placed))); };
		std::stable_sort(assets.begin(), assets.end(), [&](const auto &p_a, const auto &p_b) { return off(p_a.second) < off(p_b.second); });
		const std::pair<std::string_view, float> *chosen = nullptr;
		for (const auto &asset : assets) {
			if (mount.res_info(asset.first)) {
				chosen = &asset;
				break;
			}
		}
		if (!chosen && !assets.empty()) {
			chosen = &assets.front();
		}
		if (chosen) {
			c.res = std::string(chosen->first);
			c.present = mount.res_info(c.res).has_value();
			c.asset_scale = chosen->second;
			const float factor = object.transform.scale / chosen->second;
			c.frame = res::Transform::from_axes(
					{ scaled(object.transform.right, factor), scaled(object.transform.up, factor),
							scaled(object.transform.forward, factor) },
					object.transform.trans);
		}
		out.push_back(std::move(c));
	}
	return out;
}

}
