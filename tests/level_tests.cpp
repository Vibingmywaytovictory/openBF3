// Level traversal (bf3_core/level). Suite level_unit: transforms, interface constants and the bake
// split on synthetic values. Suite level: the retail install (read-only; exits 77 = skipped without
// one) against handoff §9 and sections/03 counts; nothing from the install is stored.
//
// Handoff §9 definitions (see README "Level traversal"): Metro TDM "10,873 placements -> 842
// meshes (762 model, 80 world), 10,571 instances" and Bazaar CQL "620 meshes, 5,783 instances" are
// the IW4L geometry builder's counts at bab7c1a, which mesh_census reproduces (Bazaar with
// legacy_variations; the current MeshVariationDatabase rule gives 613). Caspian ConquestSmall0
// "20,311 placements on 2,277 meshes, 12,725 placed hidden" is the IW engine's later world report
// (static-model instances after destruction states, projected decals and scatter models were added
// as instances), not a traversal count: the traversal places 6,847 mesh objects there.
#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/connections.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

using namespace bf3::core;
using namespace bf3::core::level;

namespace {

uint32_t bits(float p_value) {
	uint32_t b = 0;
	std::memcpy(&b, &p_value, 4);
	return b;
}

std::shared_ptr<Install> open_or_skip() {
	static InstallReader reader;
	static std::shared_ptr<Install> install;
	static bool tried = false;
	if (!tried) {
		tried = true;
		if (reader.locate({})) {
			Result<std::shared_ptr<Install>> opened = Install::open(reader);
			REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
			install = *opened;
		}
	}
	if (!install) {
		bf3::test::skip_install_case(reader.describe().c_str());
	}
	return install;
}

struct Traversed {
	std::unique_ptr<LoadedLevel> level;
	std::unique_ptr<World> world;
};

Traversed traverse(const std::shared_ptr<Install> &p_install, const char *p_level, const char *p_mode) {
	Traversed t;
	Result<std::unique_ptr<LoadedLevel>> loaded = load_level(p_install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	t.level = std::move(*loaded);
	Result<std::unique_ptr<World>> world = World::traverse(*t.level);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	t.world = std::move(*world);
	return t;
}

size_t count_mesh_objects(const World &p_world) {
	size_t n = 0;
	for (const PlacedObject &o : p_world.objects) {
		n += o.kind == ObjectKind::Mesh ? 1 : 0;
	}
	return n;
}

// Composite placements whose first health state hides parts (handoff sections/03 §2.5).
size_t composite_hiding(const LoadedLevel &p_level, const World &p_world) {
	size_t n = 0;
	for (const PlacedObject &o : p_world.objects) {
		if (o.mesh && o.mesh->asset.is_a("CompositeMeshAsset")) {
			const std::optional<std::vector<uint16_t>> parts = hidden_parts(p_level.db(), o.entity);
			n += parts && !parts->empty() ? 1 : 0;
		}
	}
	return n;
}

size_t count_start_hidden(const World &p_world) {
	size_t n = 0;
	for (const PlacedObject &o : p_world.objects) {
		n += starts_hidden(p_world, o) ? 1 : 0;
	}
	return n;
}

// A partition set with some partitions replaced: lookups try the overlay first, then the base.
class OverlayDb final : public EbxResolver {
public:
	explicit OverlayDb(const EbxResolver &p_base) :
			base_(p_base) {}

	void add(const std::string &p_name, std::unique_ptr<EbxPartition> p_partition) {
		const uint32_t at = uint32_t(partitions_.size());
		by_guid_.emplace(p_partition->guid(), at);
		by_name_.emplace(p_name, at);
		partitions_.emplace_back(p_name, std::move(p_partition));
	}
	const EbxPartition *partition(const Guid &p_guid) const override {
		const auto found = by_guid_.find(p_guid);
		return found != by_guid_.end() ? partitions_[found->second].second.get() : base_.partition(p_guid);
	}
	const EbxPartition *partition_by_name(std::string_view p_name) const override {
		const auto found = by_name_.find(to_lower_ascii(p_name));
		return found != by_name_.end() ? partitions_[found->second].second.get() : base_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const Guid &p_guid) const override {
		const auto found = by_guid_.find(p_guid);
		return found != by_guid_.end() ? std::optional<std::string_view>(partitions_[found->second].first) : base_.name_of(p_guid);
	}

private:
	const EbxResolver &base_;
	std::vector<std::pair<std::string, std::unique_ptr<EbxPartition>>> partitions_;
	std::unordered_map<Guid, uint32_t, GuidHash> by_guid_;
	std::unordered_map<std::string, uint32_t> by_name_;
};

} // namespace

TEST_SUITE("level_unit") {
	TEST_CASE("level_unit: child in parent is the row-vector product") {
		// Parent: 90 degrees about Y (right -> -Z, forward -> +X), scale 2, at (10, 0, 0).
		LevelTransform parent;
		parent.right = { 0.0f, 0.0f, -1.0f };
		parent.up = { 0.0f, 1.0f, 0.0f };
		parent.forward = { 1.0f, 0.0f, 0.0f };
		parent.trans = { 10.0f, 0.0f, 0.0f };
		parent.scale = 2.0f;
		LevelTransform child;
		child.trans = { 1.0f, 2.0f, 3.0f };
		const LevelTransform world = child.in_parent(parent);
		// p' = trans + scale * (x*right + y*up + z*forward) = (10,0,0) + 2*((0,0,-1) + (0,2,0) + (3,0,0))
		CHECK(world.trans == level::Vec3{ 16.0f, 4.0f, -2.0f });
		CHECK(world.right == parent.right);
		CHECK(world.scale == 2.0f);
		// The +0 start of the accumulation turns a -0 product into +0, as IW4L's loop does.
		LevelTransform negative_zero;
		negative_zero.right = { -0.0f, 1.0f, 0.0f };
		CHECK(bits(LevelTransform().rotate(negative_zero.right)[0]) == 0u);
		CHECK(LevelTransform().transform_point({ 1.0f, 2.0f, 3.0f }) == level::Vec3{ 1.0f, 2.0f, 3.0f });
	}

	TEST_CASE("level_unit: scale in the axes") {
		LevelTransform t;
		t.right = { 2.0f, 0.0f, 0.0f };
		t.up = { 0.0f, 2.0f, 0.0f };
		t.forward = { 0.0f, 0.0f, 2.0f };
		CHECK(t.is_uniform(1e-3f));
		CHECK(t.normalized().scale == 2.0f);
		CHECK(t.normalized().right == level::Vec3{ 1.0f, 0.0f, 0.0f });
		t.forward = { 0.0f, 0.0f, 59.8f };
		CHECK_FALSE(t.is_uniform(1e-3f));
		CHECK(t.axis_scales()[2] == 59.8f);
		CHECK(t.determinant() == 2.0f * 2.0f * 59.8f);
	}

	TEST_CASE("level_unit: interface constants") {
		const std::optional<Property> transform = Property::parse("LinearTransform ((1,0,0),(0,1,0),(0,0,1),(-194.6,64.6,-403.3))");
		REQUIRE(transform.has_value());
		REQUIRE(transform->as_transform().has_value());
		CHECK(transform->as_transform()->trans == level::Vec3{ -194.6f, 64.6f, -403.3f });
		CHECK(transform->as_vec3() == level::Vec3{ -194.6f, 64.6f, -403.3f });
		CHECK(Property::parse("Boolean True")->as_bool() == true);
		CHECK(Property::parse("Boolean false")->as_bool() == false);
		CHECK(Property::parse("Int32 55")->as_i64() == 55);
		CHECK(Property::parse("Int32 +7")->as_i64() == 7);
		CHECK(Property::parse("Float32 0.5")->as_f32() == 0.5f);
		CHECK(Property::parse("CString \"ID_H_MAP_CAFE\"")->as_str() == "ID_H_MAP_CAFE");
		CHECK(Property::parse("TeamId Team2")->as_team() == 2u);
		CHECK(Property::parse("TeamId TeamNeutral")->as_team() == 0u);
		CHECK(Property::parse("Vec3 (1, 2, 3)")->as_vec3() == level::Vec3{ 1.0f, 2.0f, 3.0f });
		// Unparsed values stay constants as written; empty means no constant.
		CHECK(Property::parse("LinearTransform ((1,0,0),(0,1,0),(0,0,1),(1,2))")->kind == Property::Kind::Constant);
		CHECK(Property::parse("Boolean maybe")->kind == Property::Kind::Constant);
		CHECK(Property::parse("Int32 12x")->kind == Property::Kind::Constant);
		CHECK(Property::parse("Unknown")->as_str() == "Unknown");
		CHECK_FALSE(Property::parse("   ").has_value());
		CHECK(field_id("Visible") == int32_t(ebx_name_hash("Visible")));
	}

	TEST_CASE("level_unit: bake split") {
		LevelTransform uniform;
		uniform.right = { 0.0f, 0.0f, -3.0f };
		uniform.up = { 0.0f, 3.0f, 0.0f };
		uniform.forward = { 3.0f, 0.0f, 0.0f };
		CHECK_FALSE(bake_split(uniform).linear.has_value());

		LevelTransform stretched;
		stretched.right = { 1.68f, 0.0f, 0.0f };
		stretched.up = { 0.0f, 1.68f, 0.0f };
		stretched.forward = { 0.0f, 0.0f, 59.8f };
		stretched.trans = { 5.0f, 6.0f, 7.0f };
		const BakeSplit split = bake_split(stretched);
		REQUIRE(split.linear.has_value());
		CHECK(split.placed.scale == 1.0f);
		CHECK(split.placed.trans == stretched.trans);
		const std::array<int32_t, 9> key = bake_key(*split.linear);
		CHECK(key[0] == int32_t(std::lround(1.68f * 4096.0f)));
		CHECK(key[8] == int32_t(std::lround(59.8f * 4096.0f)));
		CHECK(key[1] == 0);

		LevelTransform mirrored = uniform;
		mirrored.right = { 0.0f, 0.0f, 3.0f };
		CHECK(bake_split(mirrored).linear.has_value()); // negative determinant is baked

		LevelTransform degenerate;
		degenerate.right = { 0.0f, 0.0f, 0.0f };
		degenerate.up = { 0.0f, 0.0f, 0.0f };
		degenerate.forward = { 0.0f, 0.0f, 0.0f };
		CHECK_FALSE(bake_split(degenerate).linear.has_value()); // not invertible: placed as it is

		std::array<std::array<float, 3>, 3> huge{};
		huge[0][0] = 1e30f;
		huge[0][1] = -1e30f;
		huge[0][2] = std::numeric_limits<float>::quiet_NaN();
		const std::array<int32_t, 9> saturated = bake_key(huge);
		CHECK(saturated[0] == INT32_MAX);
		CHECK(saturated[1] == INT32_MIN);
		CHECK(saturated[2] == 0);
	}

	TEST_CASE("level_unit: modes and names") {
		CHECK(mode_matches("ConquestLarge0", "conquestlarge0"));
		CHECK_FALSE(mode_matches("ConquestLarge0", "ConquestSmall0"));
		CHECK(level_display_name("mp_subway") == "Operation Metro");
		CHECK(level_display_name("XP5_004") == "Sabalan Pipeline");
		CHECK(zone_name("MP_Subway") == "mp_subway");
		CHECK(zone_name("XP2_Palace") == "mp_xp2_palace");
	}
}

TEST_SUITE("level") {
	TEST_CASE("level: Metro TeamDeathMatch0 (handoff §9)") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_Subway", "TeamDeathMatch0");
		const LoadedLevel &level = *t.level;
		const World &world = *t.world;
		CHECK(level.info.id == "MP_Subway");
		CHECK(level.info.display_name == "Operation Metro");
		CHECK(level.stats.partitions == 12315); // sections/03 §1: level + mode sub-world
		CHECK(level.stats.unresolved_imports == 0);
		CHECK(level.stats.failed.empty());
		REQUIRE(level.bundles.subworlds.size() == 1);
		CHECK(bundle_key(level.bundles.subworlds[0]) == "levels/mp_subway/teamdeathmatch");
		CHECK(world.objects.size() == 13280);
		CHECK(world.subworlds.size() == 2);
		CHECK(world.scopes.size() == 447);
		CHECK(world.stats.excluded == 9); // 8 of them through interface constants (gate door variants)
		CHECK(world.stats.excluded_overrides == 8);
		CHECK(world.stats.unresolved_objects == 0);
		CHECK(world.stats.depth_limited == 0);
		CHECK(count_mesh_objects(world) == 10873);
		// Static model groups (sections/03 §2.3): the level's and the TDM sub-world's.
		REQUIRE(world.groups.size() == 2);
		const StaticModelGroup &main = world.groups[1];
		CHECK(main.members == 499);
		CHECK(main.instances == 7869);
		CHECK(main.explicit_count == 877);
		CHECK(main.havok_placed == 6992);
		CHECK(main.subparts == 6992);
		CHECK(main.part_mismatches == 0);
		CHECK(world.groups[0].instances == 20);
		// Composite placements that hide destroyed-state parts, and backdrops that start hidden
		// (sections/03 §2.4: backdrop_park + backdrop_city 42 + 22).
		CHECK(composite_hiding(level, world) == 710);
		CHECK(count_start_hidden(world) == 64);

		const MeshCensus census = mesh_census(level, world);
		CHECK(census.failures.empty());
		CHECK(census.placements == 10873);
		CHECK(census.scene_meshes == 842);
		CHECK(census.model_meshes == 762);
		CHECK(census.world_meshes == 80);
		CHECK(census.scene_instances == 10571);
		CHECK(census.meshes.size() == 903);

		const std::vector<CollisionPlacement> collision = collision_placements(level, world);
		size_t groups = 0, objects = 0, present = 0;
		for (const CollisionPlacement &c : collision) {
			groups += c.kind == CollisionPlacement::Kind::Group ? 1 : 0;
			objects += c.kind == CollisionPlacement::Kind::Object && !c.res.empty() ? 1 : 0;
			present += c.present ? 1 : 0;
		}
		CHECK(groups == 2);
		CHECK(objects == 2120);
		CHECK(present == groups + objects);
	}

	TEST_CASE("level: Grand Bazaar ConquestLarge0 (handoff §9)") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_001", "ConquestLarge0");
		const World &world = *t.world;
		CHECK(count_mesh_objects(world) == 5855);
		REQUIRE(world.groups.size() == 1);
		CHECK(world.groups[0].members == 381);
		CHECK(world.groups[0].instances == 3646);
		CHECK(world.groups[0].havok_placed == 3292);
		CHECK(world.groups[0].part_mismatches == 0);
		CHECK(composite_hiding(*t.level, world) == 368);
		// Vehicles of the mode (sections/04 / IW4L core report): 8 VehicleSpawnReferences.
		size_t vehicles = 0;
		for (const PlacedObject &o : world.objects) {
			vehicles += o.kind == ObjectKind::VehicleSpawnReference ? 1 : 0;
		}
		CHECK(vehicles == 8);

		const MeshCensus current = mesh_census(*t.level, world);
		CHECK(current.failures.empty());
		CHECK(current.scene_instances == 5783);
		CHECK(current.scene_meshes == 613); // empty-Materials entries fall back to the default
		CensusOptions legacy;
		legacy.legacy_variations = true;
		const MeshCensus handoff = mesh_census(*t.level, world, legacy);
		CHECK(handoff.scene_meshes == 620);
		CHECK(handoff.model_meshes == 579);
		CHECK(handoff.world_meshes == 41);
		CHECK(handoff.scene_instances == 5783);
	}

	TEST_CASE("level: Caspian Border ConquestSmall0") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_007", "ConquestSmall0");
		const World &world = *t.world;
		CHECK(t.level->info.display_name == "Caspian Border");
		CHECK(count_mesh_objects(world) == 6847);
		REQUIRE(world.groups.size() == 1);
		CHECK(world.groups[0].members == 170); // sections/03 §1 table
		CHECK(world.groups[0].instances == 4444);
		CHECK(world.groups[0].havok_placed == 4264);
		CHECK(composite_hiding(*t.level, world) == 538); // sections/03 §2.5
		const MeshCensus census = mesh_census(*t.level, world);
		CHECK(census.failures.empty());
		CHECK(census.placements == 6847);
		CHECK(census.scene_meshes == 304);
		CHECK(census.scene_instances == 6846);
		// Handoff §9's 20,311 / 2,277 / 12,725 are IW-engine scene counts (see the file header).
	}

	TEST_CASE("level: mutated level partitions fail or traverse, never crash") {
		// Seeded damage to the level's own partitions (LevelData, layers, sub-world, variation
		// databases): values that still pass the EBX validator reach the traversal, the census and
		// the collision placements (hostile counts, references, transforms, connection graphs).
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		const Traversed t = traverse(install, "MP_Subway", "TeamDeathMatch0");
		const LoadedLevel &level = *t.level;
		std::vector<size_t> candidates;
		for (size_t i = 0; i < level.db().size(); ++i) {
			if (level.db().name_at(i).rfind("levels/mp_subway/", 0) == 0) {
				candidates.push_back(i);
			}
		}
		REQUIRE(candidates.size() > 10);
		size_t root_index = 0;
		for (size_t i = 0; i < level.db().size(); ++i) {
			if (level.db().name_at(i) == level.root_name) {
				root_index = i;
			}
		}
		constexpr uint32_t kSeeds = 96;
		size_t mutated = 0, traversed = 0, failed = 0;
		for (uint32_t seed = 0; seed < kSeeds; ++seed) {
			std::mt19937 rng(seed * 2654435761u + 17u);
			OverlayDb overlay(level.db());
			for (int pick = 0; pick < 4; ++pick) {
				const size_t index = (pick == 0 && seed % 2 == 0) ? root_index : candidates[rng() % candidates.size()];
				const EbxPartition &original = level.db().partition_at(index);
				std::vector<uint8_t> bytes(original.bytes().begin(), original.bytes().end());
				const size_t edits = 1 + rng() % 3;
				for (size_t e = 0; e < edits && bytes.size() > 96; ++e) {
					const size_t at = 80 + rng() % (bytes.size() - 84);
					static constexpr uint32_t kValues[] = { 0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x3F800000u, 0x7F800000u, 0x7FC00000u, 0xFF7FFFFFu };
					const uint32_t value = rng() % 3 == 0 ? uint32_t(rng()) : kValues[rng() % std::size(kValues)];
					std::memcpy(bytes.data() + at, &value, 4);
				}
				Result<EbxPartition> parsed = EbxPartition::parse(std::move(bytes));
				if (!parsed) {
					continue;
				}
				++mutated;
				overlay.add(level.db().name_at(index), std::make_unique<EbxPartition>(std::move(*parsed)));
			}
			const EbxPartition *root = overlay.partition_by_name(level.root_name);
			REQUIRE(root != nullptr);
			Result<std::unique_ptr<World>> world = World::traverse(overlay, root->primary(), level.mount(), level.mode);
			if (!world) {
				++failed;
				continue;
			}
			++traversed;
			CHECK((*world)->objects.size() < (size_t(1) << 23));
			const std::vector<CollisionPlacement> collision = collision_placements(overlay, level.mount(), **world);
			CHECK(collision.size() <= (*world)->objects.size() + (*world)->groups.size());
			if (seed % 8 == 0) {
				const MeshCensus census = mesh_census(overlay, level.mount(), **world);
				CHECK(census.scene_instances <= census.placements + 2 * (*world)->objects.size());
			}
		}
		MESSAGE("mutated partitions that parsed: ", mutated, ", traversals: ", traversed, " ok / ", failed, " failed");
		CHECK(mutated > 0);
		CHECK(traversed + failed == kSeeds);
	}

	TEST_CASE("level: modes and unknown levels") {
		const std::shared_ptr<Install> install = open_or_skip();
		if (!install) {
			return;
		}
		Result<LevelInfo> info = level_info(*install, "MP_Subway");
		REQUIRE(info.ok());
		CHECK(std::find(info->modes.begin(), info->modes.end(), "TeamDeathMatch0") != info->modes.end());
		CHECK(std::find(info->modes.begin(), info->modes.end(), "ConquestLarge0") != info->modes.end());
		CHECK_FALSE(load_level(install, "MP_999", "ConquestLarge0").ok());
		// A mode the level does not offer still loads the level with no mode sub-world.
		Result<std::unique_ptr<LoadedLevel>> none = load_level(install, "MP_Subway", "NoSuchMode0");
		REQUIRE(none.ok());
		CHECK((*none)->bundles.subworlds.empty());
	}
}
