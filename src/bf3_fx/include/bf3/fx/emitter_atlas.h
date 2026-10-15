#pragma once
// The per-level emitter texture atlas (docs/design/m2a-vfx1.md §6.9; plan §3.14 "Emitter atlas per level"; gap VFX
// §5). Godot-free: reads the level's EmitterSystemAsset and the two atlas DxTexture headers (the 128-byte res, no
// pixel chunk), never decodes pixels (VFX2 uploads them).
//
// Data [verified, bf3dump types/dump/reshead 2026-10-15]: each of the 29 MP levels has one
// `levels/<l>/<l>/emittersystem_win32` (EmitterSystemAsset {BaseAtlasTexture, NormalAtlasTexture, TextureInfos[]
// EmitterTextureAtlasInfo {TextureName, MinUv, MaxUv}}); 48..53 entries per level. Metro (MP_Subway): 52 entries,
// base `emittertexturebase_win32` = DXT5 (format 2), flags 0x22 (sRGB), 4096 x 4096, 13 mips; normal
// `emittertexturenormal_win32` = NormalDXT5RGA (format 20), flags 0x20 (linear), 1024 x 2048, 12 mips.
//
// One list, two coordinate spaces: an entry whose name ends in `_n`, `_nr` or `_nrm` or contains `normalmap` has UVs
// in the normal atlas, every other entry in the base atlas. On Metro that gives 42 base and 10 normal entries; every
// non-degenerate rect lands on whole pixels of its own atlas with a 3 px gutter (min corner = 3 mod 4 px) and three
// `defaultnormalmap` aliases have inverted (min > max) rects on the same tiny flat patch. Other levels' normal atlases
// have other sizes, so the pixel rect always comes from the texture header, never from a constant.
// Names compare case-insensitively ('\' = '/'): emitters write "FX/Textures/Muzzleflash/muzzleflash_01", the atlas
// "fx/textures/muzzleflash/muzzleflash_01". On Metro every diffuse (42) and normal-map (10) name of the 1,053 Metro
// EmitterDocuments resolves and every entry is used.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/fx/fx_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

enum class AtlasKind : uint8_t {
	Base, // diffuse / colour flipbooks (BC3, sRGB)
	Normal, // NormalDXT5RGA (VFX2 decodes x = 2R-1, y = 2A-1 [inferred], plan §3.14)
};

struct AtlasTextureInfo {
	std::string partition; // the TextureAsset partition (= its res name)
	core::res::TextureFormat format = core::res::TextureFormat::Dxt5;
	bool srgb = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mips = 0;
};

struct AtlasEntry {
	std::string name; // lowercase
	AtlasKind kind = AtlasKind::Base;
	Vec2 min_uv;
	Vec2 max_uv;
	bool degenerate = false; // min > max on an axis (kept as data; resolve() still returns it)
	// The rect in pixels of its atlas (min inclusive, max exclusive), rounded; pixel_exact is false when a corner is
	// more than 0.01 px from a whole pixel.
	uint32_t px_min_x = 0, px_min_y = 0, px_max_x = 0, px_max_y = 0;
	bool pixel_exact = false;
};

class EmitterAtlas {
public:
	// p_system: "levels/mp_subway/mp_subway/emittersystem_win32" (emitter_system_of finds it for a level).
	// p_mount: the level superbundle's mount, for the two texture res headers (read_raw_res; no chunk is read).
	static core::Result<EmitterAtlas> load(const core::EbxResolver &p_db, const core::LevelMount &p_mount, std::string_view p_system,
			core::gameplay::WorkBudget &r_budget);
	// "levels/<l>/<l>/emittersystem_win32" for "MP_Subway", lowercase.
	static std::string emitter_system_of(std::string_view p_level);
	// The atlas an entry name belongs to (the suffix rule above).
	static AtlasKind kind_of_name(std::string_view p_name);

	const AtlasTextureInfo &base() const { return base_; }
	const AtlasTextureInfo &normal() const { return normal_; }
	const std::vector<AtlasEntry> &entries() const { return entries_; }
	// By TextureName / NormalmapName as an emitter writes it (any case, either slash); nullptr when absent.
	const AtlasEntry *resolve(std::string_view p_name) const;
	// Flipbook frame f of an entry: frame_count frames in frame_columns columns, row-major [assumed], inside the rect.
	static void frame_rect(const AtlasEntry &p_entry, uint32_t p_frame, uint32_t p_frame_count, uint32_t p_frame_columns, Vec2 &r_min, Vec2 &r_max);

private:
	AtlasTextureInfo base_;
	AtlasTextureInfo normal_;
	std::vector<AtlasEntry> entries_; // data order
	std::vector<uint32_t> by_name_; // indices sorted by name
};

} // namespace bf3::fx
