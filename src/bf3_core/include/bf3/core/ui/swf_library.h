#pragma once
// Every SwfMovie res of the install, found through the bundle manifests and read through the container layer
// (docs/design/ui-swf-fonts.md §4.1, §5). Godot-free; read-only on the install like every bf3_core reader.
//
// Where the movies are (measured 2026-10-15): 434 distinct names, 10,303 manifest entries over 50 superbundles;
// the holders compared so far (the 11 superbundles that cover all names) carry identical bytes per name; over all
// 10,303 entries that is unverified until the census counts distinct manifest SHA-1s per name (distinct_sha1s; a
// name with two fails gate G1, there is no silent first-holder fallback). Win32/UI holds 58 (the 33 font libraries under ui/static and the
// shared widget libraries of the ui/flow bundles), Levels/FrontEnd 232, each of the 29 MP levels 194-227 (its
// <level>_uiplaying, _uiendofround, _uipreendofround and _uiloadingmp bundles). No single superbundle holds all: the movies some
// level or mode needs live only in that level's bundles. Each movie's atlas textures (ui/assets/<swf>/<swf>_i<N>,
// DxTexture) sit in the same bundle as the movie (135 of 135). <N> is a string, not a number: _i1.._i9, _i12, _i23
// and _ia.._ie all occur, so the name always comes from the tag's file name (swf_atlas_texture_name).
//
// Search order (the first holder wins): Win32/UI, Win32/Levels/FrontEnd/FrontEnd, the MP/XP level superbundles in
// layout order, then every other non-chunk, non-Loc superbundle in layout order. SP/COOP-only movies are found too;
// the census covers all 434.
//
// Threads: open() reads the manifests on parallel_for lanes; afterwards every method is thread-safe. Mounts are
// created lazily, one LevelMount per holder superbundle, each behind its own slot guard (a mutex + created flag per
// holder; parallel callers needing the same holder wait for the one creation, and a failed mount is memoised as its
// Status); after creation a mount is shared read-only (LevelMount reads are internally locked). load() memoises
// parsed movies under a mutex and parses outside it (each parse with swf_default_budget_units).

#include "bf3/core/ids.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
}

namespace bf3::core::ui {

struct SwfMovieSource {
	std::string name; // lowercase res name, e.g. "ui/assets/iconsingame"
	std::string superbundle; // first holder in search order
	std::string bundle; // the holder's bundle
	Sha1 sha1; // manifest SHA-1 of the stored payload (the cache key source)
	uint64_t size = 0; // original (res payload) size
	uint32_t holders = 0; // manifest entries naming it, all superbundles
	uint32_t distinct_sha1s = 0; // 1 for every retail movie
};

// The superbundle search order described above.
std::vector<std::string> swf_superbundle_order(const Install &p_install);

// "Ammo.swf" / "HudTextures.swf" -> "ui/assets/ammo" / "ui/assets/hudtextures" (lowercase file stem). The 1,268
// retail ImportAssets(2) tags that name a movie (one symbol each) all resolve this way; the 216 tags that import
// fonts from gfxfontlib are not among them and return empty here (is_font_library_url).
std::string swf_movie_name_for_url(std::string_view p_url);
// "../Static/gfxfontlib.swf" (any case, either slash): Scaleform's font library, resolved through
// UIFontMappingCollection (ui_fonts.h), not a movie.
bool is_font_library_url(std::string_view p_url);
// "ui/assets/iconsingame" + "iconsingame_i1.tga" -> "ui/assets/iconsingame/iconsingame_i1" (DxTexture res; the
// file name's stem is used as a string, "killcam_ie.tga" -> ".../killcam_ie").
std::string swf_atlas_texture_name(std::string_view p_movie_name, std::string_view p_file_name);

struct SwfLibraryOptions {
	unsigned threads = 0; // manifest lanes; 0 = all cores
};

class SwfLibrary {
public:
	// Lists every SwfMovie of the install (manifests only; no payload is read).
	static Result<SwfLibrary> open(std::shared_ptr<const Install> p_install, const SwfLibraryOptions &p_options = {});

	SwfLibrary();
	~SwfLibrary();
	SwfLibrary(SwfLibrary &&) noexcept;
	SwfLibrary &operator=(SwfLibrary &&) noexcept;

	const std::shared_ptr<const Install> &install() const;
	// Sorted by name.
	const std::vector<SwfMovieSource> &movies() const;
	// p_name matched case-insensitively ('\' = '/'); nullptr when absent.
	const SwfMovieSource *find(std::string_view p_name) const;

	// The res payload from the first holder.
	Result<std::vector<uint8_t>> read_payload(std::string_view p_name) const;
	// Parsed once per library (a failure is memoised too); shared, immutable.
	Result<std::shared_ptr<const SwfMovie>> load(std::string_view p_name) const;
	// An atlas DxTexture from the movie's holder superbundle (res_loader's load_texture, chunk included).
	Result<res::LoadedTexture> load_atlas_texture(std::string_view p_movie_name, const SwfExternalImage &p_image) const;

	double open_seconds() const;
	size_t superbundles_listed() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::core::ui
