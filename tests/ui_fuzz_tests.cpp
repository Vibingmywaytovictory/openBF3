// UI ui2 readers under seeded in-memory mutations of real install bytes (docs/design/ui-minimap-icons.md §9, §13):
//   suite "ui-minimap-fuzz" (ctest ui.minimap.fuzz): levels/mp_subway/mp_subway through read_minimap_data, the combat-area
//     asset levels/mp_subway/ui/minimap/cacs1 through read_combat_area, and three DxTexture headers (the satellite, a
//     mask, the detail field) through decode_l8_texture;
//   suite "ui-hud-fuzz" (ctest ui.hud.fuzz): ui/static/hud3dicons through read_hud_icon_atlas, ui/uiweaponmetadata and
//     ui/uikititemmetadata through ItemIconIndex::read, ui/flow/state/ingamestatemp and ui/uicomponents/uiweaponcomp
//     through read_hud_source_table, the atlas and M16A4 _small headers through decode_rgba8_texture / decode_item_icon.
// A mutated partition replaces the real one in an overlay resolver over the real library, so its references still
// resolve. Every mutant must fail with an error or read to the end; nothing crashes, hangs or allocates past the caps
// (meant for the asan preset). Mutants per input: 250, or OPENBF3_FUZZ_ITERS. Nothing is written anywhere.
#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_sources.h"
#include "bf3/core/ui/minimap.h"
#include "ui2_test_support.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
namespace t2 = bf3::test::ui2;

namespace {

using Bytes = std::vector<uint8_t>;

void mutate(Bytes &r_bytes, std::mt19937_64 &r_rng) {
	if (r_bytes.empty()) {
		return;
	}
	const unsigned mode = unsigned(r_rng() % 6);
	if (mode == 0) {
		r_bytes.resize(size_t(r_rng() % r_bytes.size()));
		return;
	}
	const unsigned count = 1 + unsigned(r_rng() % 8);
	for (unsigned i = 0; i < count; ++i) {
		// A third of the hits land in the first 512 bytes (headers, tables); the rest anywhere, so payload values
		// (floats, indices, references) change in partitions that still parse.
		const size_t at = (r_rng() % 3 == 0) ? size_t(r_rng() % std::min<size_t>(r_bytes.size(), 0x200)) : size_t(r_rng() % r_bytes.size());
		if (mode == 1) {
			r_bytes[at] ^= uint8_t(1u << (r_rng() % 8));
		} else if (mode == 2) {
			r_bytes[at] = uint8_t(r_rng());
		} else if (mode == 3 && at + 4 <= r_bytes.size()) {
			const uint32_t v = (r_rng() % 2) ? 0xFFFFFFFFu : uint32_t(r_rng());
			std::memcpy(r_bytes.data() + at, &v, 4);
		} else if (mode == 4 && at + 4 <= r_bytes.size()) {
			const float f = (r_rng() % 3 == 0) ? std::nanf("") : (r_rng() % 2 ? 1e30f : -1.0f);
			std::memcpy(r_bytes.data() + at, &f, 4);
		} else {
			r_bytes[at] = 0;
		}
	}
}

// One partition replaced by a mutant; everything else from the base resolver.
class OverlayResolver final : public EbxResolver {
public:
	OverlayResolver(const EbxResolver &p_base, std::string p_name, const EbxPartition &p_partition) :
			base_(p_base), name_(std::move(p_name)), partition_(p_partition) {}
	const EbxPartition *partition(const Guid &p_guid) const override {
		return p_guid == partition_.guid() ? &partition_ : base_.partition(p_guid);
	}
	const EbxPartition *partition_by_name(std::string_view p_name) const override {
		return iequals_ascii(p_name, name_) ? &partition_ : base_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const Guid &p_guid) const override {
		return p_guid == partition_.guid() ? std::optional<std::string_view>(name_) : base_.name_of(p_guid);
	}

private:
	const EbxResolver &base_;
	std::string name_;
	const EbxPartition &partition_;
};

struct Tally {
	size_t mutants = 0;
	size_t parsed = 0; // partitions / headers that still parsed
	size_t read_ok = 0; // readers that returned a value
};

// Mutants of partition p_name of p_lib through p_read (which gets a resolver holding the mutant).
Tally fuzz_partition(const EbxLibrary &p_lib, const LevelMount &p_mount, const std::string &p_name, uint64_t p_seed,
		const std::function<bool(const EbxResolver &, const EbxPartition &)> &p_read) {
	Tally t;
	Result<std::vector<uint8_t>> bytes = p_mount.read_ebx(p_name);
	REQUIRE_MESSAGE(bytes.ok(), p_name);
	{
		// The unmutated partition reads through the overlay (so the mutants exercise the readers, not a broken setup).
		Result<EbxPartition> original = EbxPartition::parse(*bytes);
		REQUIRE(original.ok());
		const OverlayResolver overlay(p_lib, p_name, *original);
		REQUIRE_MESSAGE(p_read(overlay, *original), p_name);
	}
	std::mt19937_64 rng(p_seed);
	const size_t iters = t2::fuzz_iters(250);
	for (size_t i = 0; i < iters; ++i) {
		Bytes m = *bytes;
		mutate(m, rng);
		++t.mutants;
		Result<EbxPartition> parsed = EbxPartition::parse(std::move(m));
		if (!parsed) {
			continue;
		}
		++t.parsed;
		const OverlayResolver overlay(p_lib, p_name, *parsed);
		t.read_ok += p_read(overlay, *parsed) ? 1 : 0;
	}
	return t;
}

// Mutants of a DxTexture res header, decoded against the real chunk.
Tally fuzz_header(const LevelMount &p_mount, const std::string &p_res, uint64_t p_seed,
		const std::function<bool(const res::DxTexture &, std::span<const uint8_t>)> &p_decode) {
	Tally t;
	Result<res::LoadedTexture> real = res::load_texture(p_mount, p_res);
	REQUIRE_MESSAGE(real.ok(), p_res);
	REQUIRE(p_decode(real->texture, real->chunk.bytes)); // the unmutated header decodes
	std::mt19937_64 rng(p_seed);
	const size_t iters = t2::fuzz_iters(250);
	for (size_t i = 0; i < iters; ++i) {
		Bytes m = real->res.payload;
		mutate(m, rng);
		++t.mutants;
		res::DxTexture tex;
		if (!res::DxTexture::parse(res::Bytes(m.data(), m.size()), tex).ok()) {
			continue;
		}
		++t.parsed;
		t.read_ok += p_decode(tex, real->chunk.bytes) ? 1 : 0;
	}
	return t;
}

std::string tally_text(const char *p_what, const Tally &p_t) {
	return std::string(p_what) + " " + std::to_string(p_t.mutants) + " mutants, " + std::to_string(p_t.parsed) + " parsed, " + std::to_string(p_t.read_ok) + " read";
}

} // namespace

TEST_SUITE("ui-minimap-fuzz") {
	TEST_CASE("minimap readers under mutation") {
		t2::InstallFixture *f = t2::install();
		if (f == nullptr) {
			return;
		}
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(f->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		REQUIRE(lib.ok());
		const LevelMount &mount = (*lib)->mount(0);
		const Tally a = fuzz_partition(**lib, mount, "levels/mp_subway/mp_subway", 1, [](const EbxResolver &p_db, const EbxPartition &p_part) {
			level::WorkBudget budget(1u << 16);
			Result<MinimapData> d = read_minimap_data(p_db, p_part.primary(), "MP_Subway", budget);
			if (d) {
				CHECK(std::isfinite(d->world_size));
				CHECK(d->world_size > 0.0f);
				CHECK(d->world_rotation == 0.0f);
			}
			return d.ok();
		});
		const Tally b = fuzz_partition(**lib, mount, "levels/mp_subway/ui/minimap/cacs1", 2, [](const EbxResolver &p_db, const EbxPartition &) {
			level::WorkBudget budget(1u << 16);
			return read_combat_area(p_db, "levels/mp_subway/ui/minimap/cacs1", budget).ok();
		});
		const auto decode = [](const res::DxTexture &p_t, std::span<const uint8_t> p_chunk) {
			Result<L8Image> img = decode_l8_texture(p_t, p_chunk, "mutant");
			if (img) {
				CHECK(img->texels.size() == size_t(img->width) * img->height);
				CHECK(img->width <= kMaxMinimapImageSide);
			}
			return img.ok();
		};
		const Tally c = fuzz_header(mount, "levels/mp_subway/ui/minimap/mp15_satellite", 3, decode);
		const Tally d = fuzz_header(mount, "levels/mp_subway/ui/combatarea/dfcs1_win32_sdf", 4, decode);
		const Tally e = fuzz_header(mount, "levels/mp_subway/ui/minimap/mp15_detail_distancefield_win32_sdf", 5, decode);
		t2::row("minimap.fuzz", true,
				tally_text("LevelData", a) + "; " + tally_text("cacs1", b) + "; " + tally_text("satellite header", c) + "; " + tally_text("dfcs1 header", d) + "; " +
						tally_text("detail header", e));
	}

	TEST_CASE("G13 install.unchanged") {
		t2::check_install_unchanged();
	}
}

TEST_SUITE("ui-hud-fuzz") {
	TEST_CASE("HUD icon and source readers under mutation") {
		t2::InstallFixture *f = t2::install();
		if (f == nullptr) {
			return;
		}
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(f->install, { "Win32/UI" });
		REQUIRE(lib.ok());
		const LevelMount &mount = (*lib)->mount(0);
		const Tally a = fuzz_partition(**lib, mount, std::string(kHudIconAtlasPartition), 11, [](const EbxResolver &p_db, const EbxPartition &) {
			gameplay::WorkBudget budget;
			Result<HudIconAtlas> atlas = read_hud_icon_atlas(p_db, &budget);
			if (atlas) {
				for (const HudIcon &icon : atlas->icons) {
					CHECK(icon.states.size() <= kMaxHudIconStates);
					for (const HudIconStateFrames &s : icon.states) {
						for (const HudIconUv &uv : s.frames) {
							const HudIconRect r = hud_icon_rect(uv, 256, 1024);
							CHECK(r.width >= 0);
							CHECK(r.height >= 0);
							CHECK(r.x + r.width <= 256);
							CHECK(r.y + r.height <= 1024);
						}
					}
				}
			}
			return atlas.ok();
		});
		const auto index = [](const EbxResolver &p_db, const EbxPartition &) {
			gameplay::WorkBudget budget;
			Result<ItemIconIndex> i = ItemIconIndex::read(p_db, &budget);
			return i.ok();
		};
		const Tally b = fuzz_partition(**lib, mount, std::string(kWeaponMetadataPartition), 12, index);
		const Tally c = fuzz_partition(**lib, mount, std::string(kKitItemMetadataPartition), 13, index);
		const std::vector<std::string> comps = hud_component_partitions(std::vector<const LevelMount *>{ &mount });
		const auto table = [&](const EbxResolver &p_db, const EbxPartition &) {
			gameplay::WorkBudget budget;
			Result<HudSourceTable> t = read_hud_source_table(p_db, comps, &budget);
			if (!t) {
				MESSAGE(t.error().message);
			}
			return t.ok();
		};
		const Tally d = fuzz_partition(**lib, mount, std::string(kInGameStatePartition), 14, table);
		const Tally e = fuzz_partition(**lib, mount, "ui/uicomponents/uiweaponcomp", 15, table);
		const Tally g = fuzz_header(mount, "ui/static/hud3diconswin32", 16, [](const res::DxTexture &p_t, std::span<const uint8_t> p_chunk) {
			Result<Rgba8Image> img = decode_rgba8_texture(p_t, p_chunk, "mutant");
			if (img) {
				CHECK(img->rgba.size() == size_t(img->width) * img->height * 4);
			}
			return img.ok();
		});
		const Tally h = fuzz_header(mount, "ui/art/persistence/weapons/m16a4_small", 17, [](const res::DxTexture &p_t, std::span<const uint8_t> p_chunk) {
			return decode_item_icon(p_t, p_chunk, "mutant").ok();
		});
		t2::row("hud.fuzz", true,
				tally_text("hud3dicons", a) + "; " + tally_text("weapon metadata", b) + "; " + tally_text("kit-item metadata", c) + "; " +
						tally_text("ingamestatemp", d) + "; " + tally_text("uiweaponcomp", e) + "; " + tally_text("atlas header", g) + "; " +
						tally_text("m16a4_small header", h));
	}

	TEST_CASE("G13 install.unchanged") {
		t2::check_install_unchanged();
	}
}
