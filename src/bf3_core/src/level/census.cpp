// Mesh census and collision placements: the counting parts of IW4L level_bf3 geometry/mod.rs
// (variants, models, hidden_parts, placed_scale at bab7c1a), geometry/meshes.rs (SourceMesh,
// Bake::split, surfaces), geometry/materials.rs (slots, resolve's undrawn shaders) and
// collision/mod.rs (pick_asset, frame_of), Apache-2.0. Float operations keep the Rust order.
#include "bf3/core/level/census.h"

#include "bf3/core/res/mesh_set.h"
#include "bf3/core/thread_pool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace bf3::core::level {

namespace {

constexpr size_t kMaxLods = 4;
constexpr float kWorldRadius = 16.0f;
constexpr size_t kWorldMaxPlacements = 4;
constexpr size_t kMaxComponents = 4096;
constexpr std::string_view kUndrawnShaders[5] = { "invisible", "occluder", "shadowmesh", "ui_characterbackdrop", "ui_menu" };

using Vec3f = std::array<float, 3>;
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

// Per MeshAsset.Materials slot: whether its shader draws (IW4L materials.rs slots + resolve).
std::vector<bool> drawn_slots(const EbxResolver &p_db, const EbxInstance &p_asset, const std::optional<EbxInstance> &p_entry) {
	const std::vector<EbxInstance> materials = p_db.follow_all(p_asset.data(), "Materials");
	std::vector<EbxStruct> variations;
	if (p_entry) {
		const std::optional<EbxValue> list = p_entry->get("Materials");
		const OptionalEbxArray array = list ? list->as_array() : OptionalEbxArray();
		if (array) {
			for (const EbxValue v : *array) {
				if (const std::optional<EbxStruct> s = v.as_struct()) {
					variations.push_back(*s);
				}
			}
		}
	}
	const size_t count = std::max(materials.size(), variations.size());
	std::vector<bool> out(count, true);
	for (size_t slot = 0; slot < count; ++slot) {
		const std::optional<EbxInstance> material = slot < materials.size() ? std::optional<EbxInstance>(materials[slot]) : std::nullopt;
		std::optional<EbxStruct> variation;
		if (material) {
			for (const EbxStruct &v : variations) {
				const std::optional<EbxInstance> named = p_db.follow(v, "Material");
				if (named && *named == *material) {
					variation = v;
					break;
				}
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
		for (std::string_view word : kUndrawnShaders) {
			if (name->find(word) != std::string_view::npos) {
				out[slot] = false;
				break;
			}
		}
	}
	return out;
}

// Whether one triangle of the decoded subset survives the hidden parts and the degenerate test
// (IW4L geometry/meshes.rs surfaces).
bool draws_triangle(const res::DecodedSubset &p_subset, const std::vector<uint16_t> &p_hidden) {
	const auto hidden = [&](uint32_t p_index) {
		if (!p_subset.parts || p_index >= p_subset.parts->size()) {
			return false;
		}
		return std::binary_search(p_hidden.begin(), p_hidden.end(), (*p_subset.parts)[p_index]);
	};
	const std::vector<uint32_t> &indices = p_subset.indices;
	for (size_t i = 0; i + 3 <= indices.size(); i += 3) {
		const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
		if (!p_hidden.empty() && (hidden(a) || hidden(b) || hidden(c))) {
			continue;
		}
		if (a != b && b != c && a != c) {
			return true;
		}
	}
	return false;
}

// Rust f32::total_cmp as an integer key.
int32_t total_order_key(float p_value) {
	int32_t bits = 0;
	std::memcpy(&bits, &p_value, 4);
	return bits ^ int32_t(uint32_t(bits >> 31) >> 1);
}

struct MeshResult {
	float radius = 0.0f; // of the MeshSet's bounding box
	std::vector<std::vector<bool>> lods; // per variant of this mesh, per LOD
	std::string error;
};

} // namespace

std::optional<std::vector<uint16_t>> hidden_parts(const EbxResolver &p_db, const EbxInstance &p_entity) {
	std::vector<uint16_t> intact;
	std::vector<uint16_t> damaged;
	bool found = false;
	std::vector<EbxInstance> pending = p_db.follow_all(p_entity.data(), "Components");
	size_t seen = 0;
	while (!pending.empty()) {
		const EbxInstance component = pending.back();
		pending.pop_back();
		if (++seen > kMaxComponents) {
			break;
		}
		for (const EbxInstance &child : p_db.follow_all(component.data(), "Components")) {
			if (pending.size() >= kMaxComponents) {
				break; // more than the walk can visit (a hostile, self-listing component tree)
			}
			pending.push_back(child);
		}
		if (!component.is_a("PartComponentData")) {
			continue;
		}
		found = true;
		const std::vector<EbxInstance> states = p_db.follow_all(component.data(), "HealthStates");
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
	std::erase_if(damaged, [&](uint16_t p_part) { return std::find(intact.begin(), intact.end(), p_part) != intact.end(); });
	if (!found) {
		return std::nullopt;
	}
	return damaged;
}

bool starts_hidden(const World &p_world, const PlacedObject &p_object) {
	if (!p_object.reference) {
		return false;
	}
	const int32_t visible = field_id("Visible");
	const int32_t value = field_id("Value");
	for (const Connection &c : p_world.scopes.connections(p_object.scope).property) {
		if (c.target == *p_object.reference && c.target_field == visible && c.source.is_a("BoolEntityData") && c.source_field == value) {
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
		return out; // not invertible: placed as it is
	}
	out.placed.right = x;
	out.placed.up = y;
	out.placed.forward = z;
	out.placed.scale = 1.0f;
	out.linear = linear;
	return out;
}

std::array<int32_t, 9> bake_key(const std::array<std::array<float, 3>, 3> &p_linear) {
	std::array<int32_t, 9> key{};
	for (size_t i = 0; i < 9; ++i) {
		const float v = std::round(p_linear[i / 3][i % 3] * 4096.0f);
		// Rust `as i32`: saturating, NaN -> 0.
		key[i] = std::isnan(v) ? 0 : v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : int32_t(v);
	}
	return key;
}

MeshCensus mesh_census(const LoadedLevel &p_level, const World &p_world, const CensusOptions &p_options) {
	return mesh_census(p_level.db(), p_level.mount(), p_world, p_options);
}

MeshCensus mesh_census(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world, const CensusOptions &p_options) {
	const EbxResolver &db = p_db;
	MeshCensus census;

	// Legacy lookup: every sub-world's database read again keeping empty entries.
	std::vector<MeshVariationDb> legacy;
	if (p_options.legacy_variations) {
		for (const SubWorld &sw : p_world.subworlds) {
			legacy.push_back(sw.variations.database() ? MeshVariationDb::read(db, *sw.variations.database(), true) : MeshVariationDb());
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

	// Group the placements into variants, keyed by (asset, entry, hidden parts, bake key) as text.
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
	std::vector<std::vector<std::pair<uint32_t, LevelTransform>>> placements; // per variant: (object, placed)
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
		const std::optional<EbxInstance> entry = variation_entry(object.subworld, asset, object.variation);
		std::vector<uint16_t> hidden;
		if (asset.is_a("CompositeMeshAsset")) {
			hidden = hidden_parts(db, object.entity).value_or(std::vector<uint16_t>());
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

	// Decode each distinct MeshSet once (in parallel) and test its variants.
	std::map<std::string, std::vector<uint32_t>> by_name;
	for (uint32_t v = 0; v < census.meshes.size(); ++v) {
		by_name[census.meshes[v].name].push_back(v);
	}
	std::vector<const std::pair<const std::string, std::vector<uint32_t>> *> names;
	for (const auto &entry : by_name) {
		names.push_back(&entry);
	}
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
					slots[v] = drawn_slots(db, mesh.asset, mesh.entry);
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
							const bool slot_draws = subset.material_index < slots[v].size() ? slots[v][subset.material_index] : true;
							if (slot_draws && draws_triangle(subset, mesh.hidden)) {
								draws = true;
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

std::vector<CollisionPlacement> collision_placements(const LoadedLevel &p_level, const World &p_world) {
	return collision_placements(p_level.db(), p_level.mount(), p_world);
}

std::vector<CollisionPlacement> collision_placements(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world) {
	const EbxResolver &db = p_db;
	const LevelMount &mount = p_mount;
	std::vector<CollisionPlacement> out;
	for (size_t at = 0; at < p_world.groups.size(); ++at) {
		const StaticModelGroup &group = p_world.groups[at];
		if (!group.physics_asset) {
			continue;
		}
		CollisionPlacement c;
		c.kind = CollisionPlacement::Kind::Group;
		c.index = uint32_t(at);
		c.res = *group.physics_asset;
		c.present = mount.res_info(c.res).has_value();
		c.subparts = group.subparts;
		const LevelTransform &t = group.transform;
		c.frame.axis = { scaled(t.right, t.scale), scaled(t.up, t.scale), scaled(t.forward, t.scale) };
		c.frame.origin = t.trans;
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
		for (const EbxInstance &body : db.follow_all(physics.data(), "RigidBodies")) {
			const std::optional<EbxValue> type = body.get("RigidBodyType");
			const std::optional<std::string_view> name = type ? type->enum_name() : std::nullopt;
			c.bodies.push_back(name.value_or("?"));
		}
		// pick_asset: the asset prebaked closest to the placement's scale that the bundles carry.
		const Vec3f s = object.transform.axis_scales();
		const float placed = std::fmax(std::cbrt(std::fabs(s[0] * s[1] * s[2])), 1e-6f);
		std::vector<std::pair<std::string_view, float>> assets;
		std::vector<EbxInstance> candidates = db.follow_all(physics.data(), "ScaledAssets");
		if (const std::optional<EbxInstance> asset = db.follow(physics.data(), "Asset")) {
			candidates.push_back(*asset);
		}
		for (const EbxInstance &asset : candidates) {
			const std::optional<EbxValue> name_value = asset.get("Name");
			const std::optional<std::string_view> name = name_value ? name_value->as_str() : std::nullopt;
			if (!name) {
				continue;
			}
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
			c.frame.axis = { scaled(object.transform.right, factor), scaled(object.transform.up, factor), scaled(object.transform.forward, factor) };
			c.frame.origin = object.transform.trans;
		}
		out.push_back(std::move(c));
	}
	return out;
}

} // namespace bf3::core::level
