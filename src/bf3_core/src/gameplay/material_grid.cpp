// MaterialGridData + MaterialContainerAsset reader and the soldier hit capsules (see material_grid.h).
#include "bf3/core/gameplay/material_grid.h"

#include "ebx_fields.h"

#include <cmath>
#include <limits>

namespace bf3::core::gameplay {

using detail::Fields;

namespace {

// Retail grids have 102 rows; a hostile row count must not make rows^2 cells explode.
constexpr size_t kMaxGridRows = 1024;

// Adds the MaterialContainerPair instances of a container partition that are not known yet.
size_t add_container_pairs(const EbxPartition &p_container, std::unordered_map<Guid, MaterialPairRef, GuidHash> &r_pairs) {
	size_t added = 0;
	for (const EbxInstance &inst : p_container.instances_of("MaterialContainerPair")) {
		const Guid id = inst.guid();
		if (r_pairs.contains(id)) {
			continue;
		}
		const std::optional<EbxValue> flags = inst.get("FlagsAndIndex");
		const std::optional<EbxValue> material = inst.get("PhysicsMaterialIndex");
		const std::optional<EbxValue> property = inst.get("PhysicsPropertyIndex");
		if (!flags || !material || !property || !flags->as_u32() || !material->as_i32() || !property->as_i32()) {
			continue; // not a usable pair; stays unresolved
		}
		MaterialPairRef p;
		p.present = true;
		p.resolved = true;
		p.partition = p_container.guid();
		p.instance = id;
		p.flags_and_index = *flags->as_u32();
		p.physics_material_index = *material->as_i32();
		p.physics_property_index = *property->as_i32();
		r_pairs.emplace(id, p);
		++added;
	}
	return added;
}

std::vector<std::string> container_names(const EbxPartition &p_container) {
	std::vector<std::string> out;
	const std::optional<EbxValue> names = p_container.primary().get("MaterialNames");
	const OptionalEbxArray list = names ? names->as_array() : OptionalEbxArray();
	if (!list) {
		return out;
	}
	out.reserve(list->size());
	for (const EbxValue n : *list) {
		const std::optional<std::string_view> s = n.as_str();
		out.emplace_back(s ? *s : std::string_view());
	}
	return out;
}

DamageRelation read_cell(Fields &r_f, const EbxStruct &p_item) {
	DamageRelation d;
	const OptionalEbxArray props = r_f.array(p_item, "PhysicsPropertyProperties");
	if (!props) {
		return d;
	}
	for (const EbxValue v : *props) {
		const std::optional<EbxRef> r = v.as_ref();
		if (!r || r->is_null()) {
			continue;
		}
		const std::optional<EbxInstance> rel = r_f.follow_ref(p_item.partition(), *r, "", r_f.where(p_item, "PhysicsPropertyProperties"));
		if (!rel) {
			return d;
		}
		const EbxStruct s = rel->data();
		if (rel->is_a("MaterialRelationDamageData")) {
			d.present = true;
			d.protection_multiplier = r_f.f32(s, "DamageProtectionMultiplier");
			d.protection_threshold = r_f.f32(s, "DamageProtectionThreshold");
			d.penetration_multiplier = r_f.f32(s, "DamagePenetrationMultiplier");
			d.collision_multiplier = r_f.f32(s, "CollisionDamageMultiplier");
			d.collision_threshold = r_f.f32(s, "CollisionDamageThreshold");
			d.explosion_cover_modifier = r_f.f32(s, "ExplosionCoverDamageModifier");
			d.inflicts_demolition = r_f.flag(s, "InflictsDemolitionDamage");
		} else if (rel->is_a("MaterialRelationPenetrationData")) {
			d.has_penetration = true;
			d.never_penetrate = r_f.flag(s, "NeverPenetrate");
		}
	}
	return d;
}

} // namespace

std::optional<std::string> MaterialGrid::grid_of_level(const EbxResolver &p_db, std::string_view p_level) {
	const EbxPartition *level = p_db.partition_by_name(p_level);
	if (level == nullptr) {
		return std::nullopt;
	}
	const std::optional<EbxValue> v = level->primary().get("RuntimeMaterialGrid");
	const std::optional<EbxRef> r = v ? v->as_ref() : std::nullopt;
	if (!r || r->is_null()) {
		return std::nullopt;
	}
	const std::optional<EbxInstance> grid = p_db.resolve(*level, *r);
	if (!grid) {
		return std::nullopt;
	}
	const std::optional<std::string_view> name = p_db.name_of(grid->partition().guid());
	return name ? std::optional<std::string>(std::string(*name)) : std::nullopt;
}

Result<MaterialGrid> MaterialGrid::load(const EbxResolver &p_db, std::string_view p_grid, std::span<const EbxResolver *const> p_fallbacks) {
	Fields f(p_db);
	const EbxPartition *gp = p_db.partition_by_name(p_grid);
	if (gp == nullptr) {
		return make_error(std::string(p_grid) + ": partition not found");
	}
	const std::vector<EbxInstance> grids = gp->instances_of("MaterialGridData");
	if (grids.empty()) {
		return make_error(std::string(p_grid) + ": no MaterialGridData");
	}
	const EbxStruct g = grids.front().data();
	MaterialGrid out;
	out.report_.grid = f.partition_name(*gp);

	if (const OptionalEbxArray map = f.array(g, "MaterialIndexMap")) {
		out.index_map_.reserve(map->size());
		for (const EbxValue v : *map) {
			out.index_map_.push_back(v.as_i64().value_or(-1));
		}
	}
	const OptionalEbxArray rows = f.array(g, "InteractionGrid");
	if (!f.ok()) {
		return f.error();
	}
	const size_t n = rows->size();
	if (n > kMaxGridRows) {
		return make_error(out.report_.grid + ": InteractionGrid has " + std::to_string(n) + " rows (limit " + std::to_string(kMaxGridRows) + ")");
	}
	// Check every row's width before allocating rows^2 cells.
	std::vector<EbxArray> items;
	items.reserve(n);
	for (size_t r = 0; r < n; ++r) {
		const std::optional<EbxStruct> row = rows->get(r)->as_struct();
		if (!row) {
			return make_error(out.report_.grid + ": InteractionGrid[" + std::to_string(r) + "] is not a struct");
		}
		const OptionalEbxArray it = f.array(*row, "Items");
		if (!f.ok()) {
			return f.error();
		}
		if (it->size() != n) {
			return make_error(out.report_.grid + ": InteractionGrid[" + std::to_string(r) + "] has " + std::to_string(it->size()) + " items, expected " +
					std::to_string(n));
		}
		items.push_back(*it);
	}
	out.rows_ = n;
	out.cells_.resize(n * n);
	for (size_t r = 0; r < n; ++r) {
		for (size_t c = 0; c < n; ++c) {
			const std::optional<EbxStruct> item = items[r].get(c)->as_struct();
			if (!item) {
				return make_error(out.report_.grid + ": InteractionGrid[" + std::to_string(r) + "].Items[" + std::to_string(c) + "] is not a struct");
			}
			out.cells_[r * n + c] = read_cell(f, *item);
			if (!f.ok()) {
				return f.error();
			}
		}
	}

	// The container: the partition the grid's pairs point into.
	const std::optional<EbxRef> def = f.ref(g, "DefaultMaterial");
	const OptionalEbxArray grid_pairs = f.array(g, "MaterialPairs");
	if (!f.ok()) {
		return f.error();
	}
	if (!def || def->kind != EbxRef::Kind::External) {
		return make_error(out.report_.grid + ": DefaultMaterial is not a reference into the material container");
	}
	const Guid container = def->partition;
	if (const EbxPartition *cp = p_db.partition(container)) {
		add_container_pairs(*cp, out.pairs_);
		out.names_ = container_names(*cp);
	}
	auto count_unresolved = [&]() {
		size_t missing = 0;
		for (const EbxValue v : *grid_pairs) {
			const std::optional<EbxRef> r = v.as_ref();
			if (!r || r->kind != EbxRef::Kind::External || !out.pairs_.contains(r->instance)) {
				++missing;
			}
		}
		return missing;
	};
	out.report_.grid_pairs = grid_pairs->size();
	out.report_.unresolved_in_level = count_unresolved();
	for (const EbxResolver *fb : p_fallbacks) {
		if (fb == nullptr) {
			continue;
		}
		if (const EbxPartition *cp = fb->partition(container)) {
			out.report_.from_fallback += add_container_pairs(*cp, out.pairs_);
			if (out.names_.empty()) {
				out.names_ = container_names(*cp);
			}
		}
	}
	out.report_.unresolved = out.report_.from_fallback == 0 ? out.report_.unresolved_in_level : count_unresolved();
	out.report_.container_pairs = out.pairs_.size();
	if (out.pairs_.empty()) {
		return make_error(out.report_.grid + ": material container " + container.to_string() + " not found");
	}
	return out;
}

std::string_view MaterialGrid::material_name(uint8_t p_index) const {
	return p_index < names_.size() ? std::string_view(names_[p_index]) : std::string_view();
}

std::optional<uint8_t> MaterialGrid::property_by_name(std::string_view p_name) const {
	for (size_t i = 0; i < names_.size() && i < 256; ++i) {
		if (names_[i] == p_name) {
			return uint8_t(i);
		}
	}
	return std::nullopt;
}

std::optional<MaterialPairRef> MaterialGrid::pair(const Guid &p_instance) const {
	const auto it = pairs_.find(p_instance);
	return it == pairs_.end() ? std::nullopt : std::optional<MaterialPairRef>(it->second);
}

bool MaterialGrid::complete(MaterialPairRef &r_ref) const {
	if (r_ref.resolved) {
		return true;
	}
	if (!r_ref.present) {
		return false;
	}
	const std::optional<MaterialPairRef> p = pair(r_ref.instance);
	if (!p) {
		return false;
	}
	r_ref.flags_and_index = p->flags_and_index;
	r_ref.physics_material_index = p->physics_material_index;
	r_ref.physics_property_index = p->physics_property_index;
	r_ref.resolved = true;
	return true;
}

std::optional<uint32_t> MaterialGrid::row_of_index(uint8_t p_index) const {
	if (p_index >= index_map_.size()) {
		return std::nullopt;
	}
	const int64_t row = index_map_[p_index];
	if (row < 0 || uint64_t(row) >= rows_) {
		return std::nullopt;
	}
	return uint32_t(row);
}

DamageRelation MaterialGrid::damage(uint8_t p_projectile_property, uint8_t p_target_property) const {
	const std::optional<uint32_t> a = row_of_index(p_projectile_property);
	const std::optional<uint32_t> b = row_of_index(p_target_property);
	if (!a || !b) {
		return {};
	}
	return cells_[size_t(*a) * rows_ + *b];
}

Result<std::vector<HitCapsule>> read_hit_capsules(const EbxResolver &p_db, std::string_view p_partition) {
	Fields f(p_db);
	const EbxPartition *p = p_db.partition_by_name(p_partition);
	if (p == nullptr) {
		return make_error(std::string(p_partition) + ": partition not found");
	}
	const std::vector<EbxInstance> sets = p->instances_of("SkeletonCollisionData");
	if (sets.empty()) {
		return make_error(std::string(p_partition) + ": no SkeletonCollisionData");
	}
	std::vector<HitCapsule> out;
	const OptionalEbxArray bones = f.array(sets.front().data(), "BoneCollisionData");
	if (!f.ok()) {
		return f.error();
	}
	out.reserve(bones->size());
	for (const EbxValue v : *bones) {
		const std::optional<EbxStruct> b = v.as_struct();
		if (!b) {
			return make_error(std::string(p_partition) + ": BoneCollisionData element is not a struct");
		}
		HitCapsule c;
		c.bone = f.text(*b, "BoneName");
		c.hit_reaction = f.enum_text(*b, "AnimationHitReactionType");
		c.radius = f.f32(*b, "CapsuleRadius");
		c.length = f.f32(*b, "CapsuleLength");
		c.offset = f.vec3(*b, "CapsuleOffset");
		c.bone_axis = f.i32_or(*b, "BoneAxis", 0);
		c.valid_hi_lod = f.flag(*b, "ValidInHiLod");
		c.valid_low_lod = f.flag(*b, "ValidInLowLod");
		c.deactivate_if_behind_wall = f.flag_or(*b, "DeactivateIfBehindWall", false);
		c.use_physics_rotation = f.flag_or(*b, "UsePhysicsRotation", false);
		c.material = f.material_pair(*b, "MaterialPair");
		if (!f.ok()) {
			return f.error();
		}
		out.push_back(std::move(c));
	}
	return out;
}

BodyMultipliers body_multipliers(const MaterialGrid &p_grid, std::span<const HitCapsule> p_capsules, uint8_t p_projectile_property) {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	BodyMultipliers out{ nan, nan, nan, nan };
	for (const HitCapsule &c : p_capsules) {
		if (!c.valid_hi_lod) {
			continue;
		}
		MaterialPairRef m = c.material;
		if (!p_grid.complete(m)) {
			continue;
		}
		const std::string_view zone = p_grid.material_name(m.property());
		float *slot = zone == "Head" ? &out.head : zone == "Body" ? &out.body : zone == "Chest" ? &out.chest : zone == "Foot" ? &out.legs : nullptr;
		if (slot == nullptr || !std::isnan(*slot)) {
			continue;
		}
		const DamageRelation d = p_grid.damage(p_projectile_property, m.property());
		if (d.present) {
			*slot = d.protection_multiplier;
		}
	}
	return out;
}

} // namespace bf3::core::gameplay
