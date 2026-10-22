#pragma once

#include "bf3/core/install.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_text.h"
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

godot::Ref<godot::FontFile> make_font_file(const bf3::core::ui::ConvertedFont &p_font);

godot::Ref<godot::AtlasTexture> make_atlas_region(const godot::Ref<godot::Texture2D> &p_atlas, const bf3::core::ui::SwfAtlasPiece &p_piece);

class Bf3UiAssets : public godot::Node {
	GDCLASS(Bf3UiAssets, godot::Node)

public:
	Bf3UiAssets();
	~Bf3UiAssets() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	static Bf3UiAssets *current();

	void set_atlas_movies(const godot::PackedStringArray &p_movies);
	bool is_loaded() const;
	bool has_failed() const;
	godot::String get_error() const;

	godot::Ref<godot::Font> get_font(const godot::String &p_name) const;
	godot::Ref<godot::FontFile> get_font_file(const godot::String &p_name) const;
	godot::Ref<godot::AtlasTexture> get_region(const godot::String &p_movie, const godot::String &p_region) const;
	godot::Array get_region_pieces(const godot::String &p_movie, const godot::String &p_region) const;
	godot::Array get_hud_placements(const godot::String &p_movie) const;
	godot::Dictionary get_layout(const godot::String &p_movie) const;

	const bf3::core::ui::UiFontLibrary *fonts() const;
	const bf3::core::ui::SwfFontSet *swf_fonts() const;
	const bf3::core::ui::SwfAtlasMap *atlas_map(const std::string &p_movie) const;

	const bf3::core::ui::SwfLibrary *movies() const;
	const std::vector<bf3::core::ui::HudPlacement> *hud_placements(const std::string &p_movie) const;
	godot::Ref<godot::Texture2D> atlas_texture(const std::string &p_movie, uint16_t p_image_id) const;
	bool ensure_atlas_movie(const std::string &p_movie);
	double load_seconds() const;

protected:
	static void _bind_methods();

private:
	struct Loaded;
	void join_loader();

	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	std::unique_ptr<Loaded> loaded_;
	std::vector<std::string> atlas_movies_;
	bool finished_ = false;
};

}
