#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "test_support.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::res;

namespace {

constexpr const char *kSubway = "Win32/Levels/MP_Subway/MP_Subway";

std::unique_ptr<LevelMount> mount_subway_or_skip() {
	InstallReader reader;
	if (!reader.locate({})) {
		bf3::test::skip_install_case(reader.describe().c_str());
		return nullptr;
	}
	Result<std::shared_ptr<Install>> install = Install::open(reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? "" : install.error().message));
	Result<LevelMount> mount = (*install)->mount_superbundle(kSubway);
	REQUIRE_MESSAGE(mount.ok(), (mount.ok() ? "" : mount.error().message));
	const Status loaded = mount->load_all_bundles();
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? "" : loaded.error().message));
	return std::make_unique<LevelMount>(std::move(*mount));
}

struct Outcome {
	uint32_t res_type = 0;
	std::string error;
	size_t subsets = 0;
	size_t vertices = 0;
	size_t triangles = 0;
	size_t mips = 0;
	size_t views = 0;
	size_t roots = 0;
	size_t convex = 0;
};

Outcome decode_one(const LevelMount &p_mount, const std::string &p_name) {
	Outcome o;
	const std::optional<ResInfo> info = p_mount.res_info(p_name);
	if (!info) {
		o.error = "no res info";
		return o;
	}
	o.res_type = info->res_type;
	if (o.res_type == kResTypeMeshSet) {
		Result<LoadedMeshSet> mesh = load_mesh_set(p_mount, p_name);
		if (!mesh) {
			o.error = mesh.error().message;
			return o;
		}
		std::vector<DecodedSubset> subsets;
		for (size_t lod = 0; lod < mesh->mesh.lods.size(); lod++) {
			const ResError e = mesh->decode(lod, SubsetFilter::All, subsets);
			if (!e.ok()) {
				o.error = p_name + " lod " + std::to_string(lod) + ": " + e.message();
				return o;
			}
			o.subsets += subsets.size();
			for (const DecodedSubset &d : subsets) {
				o.vertices += d.positions.size();
				o.triangles += d.indices.size() / 3;
			}
		}
	} else if (o.res_type == kResTypeDxTexture) {
		Result<LoadedTexture> tex = load_texture(p_mount, p_name);
		if (!tex) {
			o.error = tex.error().message;
			return o;
		}
		std::vector<MipSlice> mips;
		ResError e = tex->mips(mips);
		o.mips = mips.size();
		const uint32_t view = tex->chunk.view_offset;
		if (e.ok() && view != 0) {

			o.views = 1;
			e = tex->texture.split_mips(tex->chunk.whole().subspan(view), view, mips);
			if (e.ok() && tex->texture.first_mip_at(view) != tex->texture.mip_base) {
				e = ResError::malformed("view not at mip_base", view);
			}
		}
		if (!e.ok()) {
			o.error = p_name + ": " + e.message();
		}
	} else {
		Result<LoadedHavok> hk = load_havok(p_mount, p_name);
		if (!hk) {
			o.error = hk.error().message;
			return o;
		}
		o.roots = hk->shapes.size();
		for (const Shape &shape : hk->shapes) {
			o.triangles += havok_triangles(shape, Transform::identity()).size();
			o.convex += havok_convex_pieces(shape, Transform::identity()).size();
		}
		std::vector<std::array<uint32_t, 2>> pairs;
		std::vector<SubpartTransform> group;
		ResError e = hk->data.part_connections(pairs);
		if (e.ok()) {
			e = hk->data.group_subparts(group);
		}
		if (!e.ok()) {
			o.error = p_name + ": " + e.message();
		}
	}
	return o;
}

}

TEST_SUITE("res_install") {
	TEST_CASE("res install: every MeshSet, DxTexture and Havok res of MP_Subway") {
		const auto t0 = std::chrono::steady_clock::now();
		const std::unique_ptr<LevelMount> mount = mount_subway_or_skip();
		if (!mount) {
			return;
		}
		const std::vector<std::string> names = decodable_res_names(*mount);

		const std::vector<Outcome> outcomes = parallel_map<Outcome>(names.size(), [&](size_t i) { return decode_one(*mount, names[i]); });
		std::map<uint32_t, size_t> counts;
		Outcome total;
		size_t errors = 0;
		for (const Outcome &o : outcomes) {
			counts[o.res_type]++;
			if (!o.error.empty()) {
				if (errors++ < 10) {
					MESSAGE(o.error);
				}
				continue;
			}
			total.subsets += o.subsets;
			total.vertices += o.vertices;
			total.triangles += o.res_type == kResTypeMeshSet ? o.triangles : 0;
			total.mips += o.mips;
			total.views += o.views;
			total.roots += o.roots;
			total.convex += o.convex;
		}
		CHECK(errors == 0);
		CHECK(counts[kResTypeMeshSet] == 1341);
		CHECK(counts[kResTypeDxTexture] == 1717);
		CHECK(counts[kResTypeHavokPhysicsData] == 730);
		CHECK(counts[kResTypeHavokDestructionPhysicsData] == 9);

		CHECK(total.subsets == 9263);
		CHECK(total.triangles == 5366947);
		CHECK(total.mips == 17545);
		CHECK(total.roots == 1130);
		CHECK(total.vertices == 4728511);
		CHECK(total.views == 1313);
		CHECK(total.convex == 53727);
		MESSAGE("MP_Subway from the install: " << total.subsets << " subsets, " << total.vertices << " vertices, " << total.triangles
											   << " triangles, " << total.mips << " mip images (" << total.views << " ranged views), "
											   << total.roots << " havok roots, " << total.convex << " convex pieces, "
											   << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s");
	}

	TEST_CASE("res install: weapons/m9/m9_1p_mesh") {
		const std::unique_ptr<LevelMount> mount = mount_subway_or_skip();
		if (!mount) {
			return;
		}
		Result<LoadedMeshSet> m9 = load_mesh_set(*mount, "weapons/m9/m9_1p_mesh");
		REQUIRE_MESSAGE(m9.ok(), (m9.ok() ? "" : m9.error().message));
		CHECK(m9->res.res_type == kResTypeMeshSet);
		CHECK(m9->mesh.kind == MeshKind::Skinned);
		CHECK(m9->mesh.name_hash == 0x4B2BE8E6u);
		REQUIRE(m9->mesh.lods.size() == 1);
		const MeshLod &lod = m9->mesh.lods[0];
		REQUIRE(lod.bones.size() == 5);
		CHECK(lod.bones[0].name_hash == hash_quick("wep_root"));
		REQUIRE(m9->lod_chunk.size() == 1);
		REQUIRE(m9->lod_chunk[0] == 0);
		CHECK(m9->chunks[0].bytes.size() == 302560);
		std::vector<DecodedSubset> subsets;
		REQUIRE(m9->decode(0, SubsetFilter::All, subsets).ok());
		REQUIRE(subsets.size() == 2);
		CHECK(subsets[0].positions.size() == 4606);
		CHECK(subsets[0].positions.size() + subsets[1].positions.size() == 6705);
		CHECK(subsets[1].category == SubsetCategory::ZOnly);
		REQUIRE(m9->decode(0, SubsetFilter::Drawn, subsets).ok());
		CHECK(subsets.size() == 1);

		CHECK(!load_texture(*mount, "weapons/m9/m9_1p_mesh").ok());
		CHECK(!load_mesh_set(*mount, "weapons/m9/no_such_mesh").ok());
		Result<LoadedTexture> m9_d = load_texture(*mount, "weapons/m9/m9_d");
		REQUIRE_MESSAGE(m9_d.ok(), (m9_d.ok() ? "" : m9_d.error().message));
		CHECK(m9_d->chunk.view_offset == 2621440);
		CHECK(m9_d->texture.first_mip_at(m9_d->chunk.view_offset) == 2);
	}
}
