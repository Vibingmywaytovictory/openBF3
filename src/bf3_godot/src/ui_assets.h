#pragma once
// Godot side of the UI groundwork (docs/design/ui-swf-fonts.md §10): BF3's UI fonts as FontFiles and BF3's SWF
// atlases as AtlasTextures, read from the install at run time, for the Control-based HUD of the later waves
// (plan §3.8, D16: own Controls, no SWF renderer).
//
// Bf3UiAssets (a Node; one per scene that draws BF3 UI, after Bf3Localisation) takes the Install that
// Bf3Localisation opened, then on its own std::thread (no Godot API there): SwfLibrary::open, the language's
// UiFontLibrary (through the derived-data cache: acquire_cache(default_cache_root(), Bf3Localisation's
// reader().write_guard()) on that thread; in one process it shares the Cache with LevelSession, and a root another
// process holds (Busy, e.g. a parallel worktree's Godot test) means the fonts are converted without a cache, logged
// once; nothing here depends on a cache hit), the atlas maps and prepared
// atlas textures (bf3_render prepare_texture) of the movies it was asked for, and the HUD placement table. Back on
// the main thread it builds one FontFile per converted font (FontFile.set_data with the TrueType bytes), one
// ImageTexture per atlas (render_bridge make_texture) and emits "assets_loaded".
//
// The seams for the HUD screens (later M2a/M2b waves):
//   get_font("$HUD_medium")                          the FontFile resolve_font_name() picks; a system font (and a
//                                                    warning) for Device / Unresolved names
//   get_region("ui/assets/iconsingame", "mc_iconsIngameBig/FriendlyPlayer")
//                                                    an AtlasTexture of the region's first piece (filter_clip on);
//                                                    get_region_pieces() for multi-piece regions. Draw regions with
//                                                    the canvas default TEXTURE_FILTER_LINEAR (mip 0 only): the
//                                                    atlas's lower mips mix neighbouring sub-images, so a mipmap
//                                                    filter bleeds at mip > 0 even with filter_clip
//   get_hud_placements("ui/assets/hudmpscreen")      Array of Dictionaries {instance, widget, widget_movie, x, y,
//                                                    scale_x, scale_y, rotation, anchor, anchor_dx, anchor_dy}
//   get_layout("ui/assets/ammo")                     the widget's layout tree as nested Dictionaries
// Every getter is main-thread only and valid after "assets_loaded"; a missing item returns null / empty and logs
// once (route C per item, plan §3.8: own fallback art, a system font).

#include "bf3/core/install.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/ui_fonts.h"
#include "bf3/render/texture_prep.h"

#include <godot_cpp/classes/atlas_texture.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bf3::godot_side {

// A FontFile from converted TrueType bytes (set_data); null when Godot refuses them (logged). Main thread.
// Fixed settings (docs §10), shared by the HUD and the G4 check, which measures this very FontFile:
// antialiasing GRAY, hinting NONE (hinted FreeType advances are whole pixels), subpixel_positioning ONE_QUARTER
// (AUTO turns it off above 20 px and TextServerAdvanced then rounds every shaped advance, carrying the remainder
// only to the next blank), keep_rounding_remainders left at its default, allow_system_fallback true (characters a
// BF3 font lacks, e.g. letters in Venice, are shaped from a system font; the font's own glyph lookups are
// unaffected), MSDF off. Then font name read back == the long name.
godot::Ref<godot::FontFile> make_font_file(const bf3::core::ui::ConvertedFont &p_font);
// An AtlasTexture over p_atlas for one piece's rectangle (x2 / y2 exclusive -> Rect2(x1, y1, x2 - x1, y2 - y1)),
// filter_clip on: sampling is clamped to the region at the sampled mip level, which removes bleeding at mip 0 (the
// canvas default filter); at mip > 0 the atlas's own mips already mix neighbours. Main thread.
godot::Ref<godot::AtlasTexture> make_atlas_region(const godot::Ref<godot::Texture2D> &p_atlas, const bf3::core::ui::SwfAtlasPiece &p_piece);

class Bf3UiAssets : public godot::Node {
	GDCLASS(Bf3UiAssets, godot::Node)

public:
	Bf3UiAssets();
	~Bf3UiAssets() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	// The node in the tree (main thread); null when none.
	static Bf3UiAssets *current();

	// Movies whose atlases are loaded (set before the node enters the tree; default: iconsingame, hudtextures,
	// compass, ammo).
	void set_atlas_movies(const godot::PackedStringArray &p_movies);
	bool is_loaded() const;
	bool has_failed() const;
	godot::String get_error() const;

	godot::Ref<godot::Font> get_font(const godot::String &p_name) const;
	godot::Ref<godot::FontFile> get_font_file(const godot::String &p_name) const; // null for Device / Unresolved
	godot::Ref<godot::AtlasTexture> get_region(const godot::String &p_movie, const godot::String &p_region) const;
	godot::Array get_region_pieces(const godot::String &p_movie, const godot::String &p_region) const;
	godot::Array get_hud_placements(const godot::String &p_movie) const;
	godot::Dictionary get_layout(const godot::String &p_movie) const;

	// Godot-free results the checks read (main thread, after "assets_loaded").
	const bf3::core::ui::UiFontLibrary *fonts() const;
	const bf3::core::ui::SwfAtlasMap *atlas_map(const std::string &p_movie) const;
	double load_seconds() const;

protected:
	static void _bind_methods();

private:
	struct Loaded;
	void join_loader();

	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	std::unique_ptr<Loaded> loaded_; // written by the loader before loader_done_, read on the main thread after
	std::vector<std::string> atlas_movies_;
	bool finished_ = false;
};

} // namespace bf3::godot_side
