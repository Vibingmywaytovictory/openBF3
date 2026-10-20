#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/render/texture_prep.h"
#include "bf3/render/viewmodel_assets.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace bf3::render;

TEST_SUITE("render-unit") {
	TEST_CASE("render: Godot mip chain lengths and sizes") {
		CHECK(godot_mip_levels(GpuFormat::Bc1, 2048, 2048) == 12);
		CHECK(godot_mip_levels(GpuFormat::Bc3, 1024, 256) == 11);
		CHECK(godot_mip_levels(GpuFormat::Rgba8, 256, 256) == 9);
		CHECK(godot_mip_levels(GpuFormat::Bc1, 4, 4) == 3);
		CHECK(level_bytes(GpuFormat::Bc1, 2, 2) == 8);
		CHECK(level_bytes(GpuFormat::Bc5, 8, 4) == 32);
		CHECK(level_bytes(GpuFormat::Rg8, 3, 5) == 30);
	}

	TEST_CASE("render: BC4 encode/decode and the BC3 normal -> BC5 transcode") {
		std::array<uint8_t, 16> values{};
		for (int i = 0; i < 16; ++i) {
			values[size_t(i)] = uint8_t(40 + i * 9);
		}
		const std::array<uint8_t, 8> block = bc4_encode(values);
		const std::array<uint8_t, 16> back = bc4_values(block.data());
		int worst = 0;
		for (int i = 0; i < 16; ++i) {
			worst = std::max(worst, std::abs(int(back[size_t(i)]) - int(values[size_t(i)])));
		}
		CHECK(worst <= (135 + 13) / 14);
		std::array<uint8_t, 16> flat{};
		flat.fill(77);
		CHECK(bc4_values(bc4_encode(flat).data()) == flat);

		uint8_t bc3[16] = {};
		const std::array<uint8_t, 8> x = bc4_encode(values);
		std::memcpy(bc3, x.data(), 8);
		const unsigned c0 = 63u << 5;
		bc3[8] = uint8_t(c0 & 0xFFu);
		bc3[9] = uint8_t((c0 >> 8) & 0xFFu);
		bc3[10] = 0;
		bc3[11] = 0;
		bc3[12] = 0b11100100;
		bc3[13] = 0b11100100;
		bc3[14] = 0b11100100;
		bc3[15] = 0b11100100;
		const std::array<uint8_t, 16> green = bc_color_green(bc3 + 8);
		CHECK(green[0] == 255);
		CHECK(green[1] == 0);
		CHECK(green[2] == 170);
		CHECK(green[3] == 85);
		const std::array<uint8_t, 16> bc5 = bc3_normal_to_bc5(bc3);
		CHECK(std::memcmp(bc5.data(), bc3, 8) == 0);
		const std::array<uint8_t, 16> y = bc4_values(bc5.data() + 8);
		for (int i = 0; i < 16; ++i) {
			CHECK(std::abs(int(y[size_t(i)]) - int(green[size_t(i)])) <= 19);
		}
	}
}

TEST_SUITE("render") {
	TEST_CASE("render: the M9 viewmodel loads from the install") {
		bf3::core::InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		LoadProgress progress;
		bf3::core::Result<std::unique_ptr<ViewmodelAssets>> r = load_viewmodel(reader, ViewmodelSpec{}, &progress);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
		const ViewmodelAssets &a = **r;
		for (const std::string &n : a.notes) {
			MESSAGE(n);
		}
		MESSAGE("seconds: ant " << a.seconds_ant << ", ebx " << a.seconds_ebx << ", meshes " << a.seconds_meshes << ", textures " << a.seconds_textures
								<< ", total " << a.seconds_total);
		CHECK(progress.step() == LoadProgress::kSteps - 1);
		CHECK(a.arms.mesh == "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh");
		CHECK(a.arms.variation == "characters/arms/arms1p_bareglove03/arms1p_bareglove03_afro");
		CHECK(a.arms.variation_hash == 3684293736u);
		CHECK(a.rig.size() == 181);
		CHECK(a.tracks.channels_bound == 135);
		CHECK(a.binds.size() == 69 + 5);
		CHECK(a.weapon.prepared.surfaces.size() == 1);
		CHECK(a.weapon.prepared.vertices == 4606);
		CHECK(a.arms.materials.slots.size() == 4);
		CHECK(a.weapon.materials.slots.size() == 2);
		CHECK(a.weapon.materials.slots[0].texture("Diffuse") == "weapons/m9/m9_d");
		CHECK(a.weapon.materials.slots[0].shader == "weapons/shaders/weaponpresetfp");
		CHECK(a.arms.materials.slots[2].texture("Diffuse") == "characters/arms/arms1p_bareglove03/textures/arms1p_bareglove03_afro_c");
		for (const ViewmodelPart *p : { &a.arms, &a.weapon }) {
			for (const PreparedSurface &s : p->prepared.surfaces) {
				CHECK(s.bones.size() == s.vertex_count() * 4);
				CHECK(s.weights.size() == s.vertex_count() * 4);
				CHECK(s.tangents.size() == s.vertex_count() * 4);
				CHECK(s.uv0.size() == s.vertex_count() * 2);
				CHECK(s.indices.size() % 3 == 0);
				CHECK(*std::max_element(s.bones.begin(), s.bones.end()) < int32_t(a.binds.size()));
			}
		}
		CHECK(a.texture("weapons/m9/m9_d") != nullptr);
		for (const PreparedTexture &t : a.textures) {
			CHECK(t.levels == godot_mip_levels(t.format, t.width, t.height));
		}
	}
}

TEST_SUITE("render") {
	TEST_CASE("render: NormalDXT5RGA channel layout census") {
		using namespace bf3::core;
		using namespace bf3::core::res;
		InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		Result<std::shared_ptr<Install>> install = Install::open(reader);
		REQUIRE(install.ok());
		Result<LevelMount> mount = (*install)->mount_superbundle("Win32/Levels/MP_Subway/MP_Subway");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		struct Stats {
			uint64_t texels = 0;
			uint64_t in_disc[4][4] = {};
			double sum[4] = {};
			double z_err[4][4][4] = {};
		};
		auto run = [&](TextureFormat p_format, bool p_opaque_only, Stats &r_stats, std::vector<std::string> &r_names) {
			for (const std::string &name : decodable_res_names(*mount)) {
				const std::optional<ResInfo> info = mount->res_info(name);
				if (!info || info->res_type != kResTypeDxTexture) {
					continue;
				}
				Result<RawRes> raw = read_raw_res(*mount, name);
				DxTexture header;
				if (!raw || !DxTexture::parse(raw->bytes(), header).ok() || header.format != p_format) {
					continue;
				}
				Result<LoadedTexture> t = load_texture(*mount, name);
				if (!t) {
					continue;
				}
				std::vector<MipSlice> mips;
				if (!t->mips(mips).ok()) {
					continue;
				}
				const MipSlice *pick = nullptr;
				for (const MipSlice &m : mips) {
					if (m.face == 0 && (pick == nullptr || (m.width >= 64 && m.width < pick->width))) {
						pick = &m;
					}
				}
				std::vector<std::array<float, 4>> px;
				if (pick == nullptr || !pick->texels(TextureFormat::Dxt5, px)) {
					continue;
				}
				r_names.push_back(name + " " + std::to_string(header.width) + "x" + std::to_string(header.height));
				for (const std::array<float, 4> &p : px) {
					if (p_opaque_only && p[3] < 0.5f) {
						continue;
					}
					++r_stats.texels;
					for (int a = 0; a < 4; ++a) {
						r_stats.sum[a] += p[size_t(a)];
						for (int b = 0; b < 4; ++b) {
							const double x = 2.0 * p[size_t(a)] - 1.0, y = 2.0 * p[size_t(b)] - 1.0;
							const double r2 = x * x + y * y;
							r_stats.in_disc[a][b] += r2 <= 1.05 ? 1 : 0;
							const double z = std::sqrt(std::max(0.0, 1.0 - r2)) * 0.5 + 0.5;
							for (int c = 0; c < 4; ++c) {
								r_stats.z_err[a][b][c] += std::fabs(p[size_t(c)] - z);
							}
						}
					}
				}
			}
		};
		const char *ch = "RGBA";
		for (auto [f, opaque] : { std::pair{ TextureFormat::NormalDxt5Rga, false }, std::pair{ TextureFormat::NormalDxt5Rga, true }, std::pair{ TextureFormat::NormalDxt5, false } }) {
			Stats s;
			std::vector<std::string> names;
			run(f, opaque, s, names);
			const std::string label = std::string(f == TextureFormat::NormalDxt5Rga ? "NormalDXT5RGA" : "NormalDXT5") + (opaque ? " (texels with A >= 0.5)" : "");
			MESSAGE(label << ": " << names.size() << " textures, " << s.texels << " texels sampled");
			for (size_t i = 0; i < std::min<size_t>(names.size(), 12); ++i) {
				MESSAGE("  " << names[i]);
			}
			if (s.texels == 0) {
				continue;
			}
			std::string means = "  channel means:";
			for (int a = 0; a < 4; ++a) {
				means += std::string(" ") + ch[a] + "=" + std::to_string(s.sum[a] / double(s.texels)).substr(0, 5);
			}
			MESSAGE(means);
			for (int a = 0; a < 4; ++a) {
				for (int b = a + 1; b < 4; ++b) {
					std::string line = std::string("  X,Y = ") + ch[a] + "," + ch[b] + ": in unit disc " +
							std::to_string(100.0 * double(s.in_disc[a][b]) / double(s.texels)).substr(0, 5) + " %; mean |c - z| for";
					for (int c = 0; c < 4; ++c) {
						if (c != a && c != b) {
							line += std::string(" ") + ch[c] + " " + std::to_string(s.z_err[a][b][c] / double(s.texels)).substr(0, 5);
						}
					}
					MESSAGE(line);
				}
			}
		}
		CHECK(true);
	}
}
