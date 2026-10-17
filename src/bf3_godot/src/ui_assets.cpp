// Godot side of the UI groundwork (bf3_godot ui_assets.h; docs/design/ui-swf-fonts.md §3, §10): BF3's UI fonts as
// FontFiles and its SWF atlases as AtlasTextures, loaded on a loader thread from the install Bf3Localisation opened.
#include "ui_assets.h"

#include "bf3_localisation.h"
#include "godot_cache.h"
#include "godot_util.h"
#include "render_bridge.h"

#include "bf3/core/ebx_db.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/system_font.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

Bf3UiAssets *g_current = nullptr; // main thread only

std::string movie_key(const std::string &p_name) {
	std::string k = bf3::core::to_lower_ascii(p_name);
	for (char &c : k) {
		c = c == '\\' ? '/' : c;
	}
	return k.find('/') == std::string::npos ? "ui/assets/" + k : k;
}

Dictionary layout_dict(const ui::SwfLayoutNode &p_node) {
	Dictionary d;
	d["instance"] = to_godot(p_node.instance);
	static const char *kinds[] = { "sprite", "shape", "edit_text", "static_text", "import", "other" };
	d["kind"] = String(kinds[size_t(p_node.kind)]);
	d["movie"] = to_godot(p_node.movie);
	d["symbol"] = to_godot(p_node.symbol);
	d["depth"] = int64_t(p_node.depth);
	d["transform"] = Transform2D(p_node.local.a, p_node.local.b, p_node.local.c, p_node.local.d, p_node.local.tx, p_node.local.ty);
	d["stage_transform"] = Transform2D(p_node.stage.a, p_node.stage.b, p_node.stage.c, p_node.stage.d, p_node.stage.tx, p_node.stage.ty);
	if (p_node.kind == ui::SwfLayoutKind::EditText) {
		Dictionary t;
		t["font"] = to_godot(p_node.text.font);
		t["size"] = double(p_node.text.size);
		t["color"] = Color(p_node.text.color.r / 255.0f, p_node.text.color.g / 255.0f, p_node.text.color.b / 255.0f, p_node.text.color.a / 255.0f);
		t["align"] = int64_t(p_node.text.align);
		t["bounds"] = Rect2(p_node.text.bounds.x_min, p_node.text.bounds.y_min, p_node.text.bounds.width(), p_node.text.bounds.height());
		t["initial_text"] = to_godot(p_node.text.initial_text);
		t["html"] = p_node.text.html;
		d["text"] = t;
	}
	if (p_node.has_scaling_grid) {
		const ui::SwfRect &g = p_node.scaling_grid;
		d["scaling_grid"] = Rect2(g.x_min, g.y_min, g.width(), g.height());
	}
	Array children;
	for (const ui::SwfLayoutNode &c : p_node.children) {
		children.push_back(layout_dict(c));
	}
	d["children"] = children;
	return d;
}

} // namespace

struct Bf3UiAssets::Loaded {
	// Loader thread (Godot-free).
	std::string error;
	std::shared_ptr<const bf3::core::Install> install;
	std::optional<ui::SwfLibrary> movies;
	std::unique_ptr<bf3::core::EbxLibrary> ui_ebx;
	std::optional<ui::UiFontLibrary> fonts;
	std::map<std::string, ui::SwfAtlasMap> atlas_maps;
	std::map<std::string, std::map<uint16_t, bf3::render::PreparedTexture>> prepared;
	std::map<std::string, std::vector<ui::HudPlacement>> hud;
	std::vector<std::string> warnings;
	std::string cache_note;
	double seconds = 0.0;
	// Main thread, after the loader.
	std::map<std::string, Ref<FontFile>> font_files; // by long name
	std::map<std::string, std::map<uint16_t, Ref<ImageTexture>>> atlases;
	mutable std::set<std::string> logged; // missing items logged once
	mutable std::map<std::string, Ref<SystemFont>> system_fonts;
	mutable std::mutex layout_mutex;
	mutable std::map<std::string, Dictionary> layouts;
};

Ref<FontFile> make_font_file(const ui::ConvertedFont &p_font) {
	Ref<FontFile> f;
	f.instantiate();
	PackedByteArray bytes;
	bytes.resize(int64_t(p_font.ttf.size()));
	if (!p_font.ttf.empty()) {
		std::memcpy(bytes.ptrw(), p_font.ttf.data(), p_font.ttf.size());
	}
	f->set_data(bytes);
	f->set_antialiasing(TextServer::FONT_ANTIALIASING_GRAY);
	f->set_hinting(TextServer::HINTING_NONE);
	f->set_subpixel_positioning(TextServer::SUBPIXEL_POSITIONING_ONE_QUARTER);
	f->set_allow_system_fallback(true);
	f->set_multichannel_signed_distance_field(false);
	const std::string name = to_std(f->get_font_name());
	if (name != p_font.font_long_name) {
		UtilityFunctions::push_error(to_godot("[ui1] FontFile.set_data for '" + p_font.font_long_name + "' reads back as '" + name + "'"));
		return Ref<FontFile>();
	}
	return f;
}

Ref<AtlasTexture> make_atlas_region(const Ref<Texture2D> &p_atlas, const ui::SwfAtlasPiece &p_piece) {
	if (p_atlas.is_null()) {
		return Ref<AtlasTexture>();
	}
	Ref<AtlasTexture> t;
	t.instantiate();
	t->set_atlas(p_atlas);
	t->set_region(Rect2(real_t(p_piece.x1), real_t(p_piece.y1), real_t(p_piece.x2 - p_piece.x1), real_t(p_piece.y2 - p_piece.y1)));
	t->set_filter_clip(true);
	return t;
}

Bf3UiAssets::Bf3UiAssets() {
	atlas_movies_ = { "ui/assets/iconsingame", "ui/assets/hudtextures", "ui/assets/compass", "ui/assets/ammo" };
}

Bf3UiAssets::~Bf3UiAssets() {
	join_loader();
	if (g_current == this) {
		g_current = nullptr;
	}
}

Bf3UiAssets *Bf3UiAssets::current() {
	return g_current;
}

void Bf3UiAssets::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_atlas_movies", "movies"), &Bf3UiAssets::set_atlas_movies);
	ClassDB::bind_method(D_METHOD("is_loaded"), &Bf3UiAssets::is_loaded);
	ClassDB::bind_method(D_METHOD("has_failed"), &Bf3UiAssets::has_failed);
	ClassDB::bind_method(D_METHOD("get_error"), &Bf3UiAssets::get_error);
	ClassDB::bind_method(D_METHOD("get_font", "name"), &Bf3UiAssets::get_font);
	ClassDB::bind_method(D_METHOD("get_font_file", "name"), &Bf3UiAssets::get_font_file);
	ClassDB::bind_method(D_METHOD("get_region", "movie", "region"), &Bf3UiAssets::get_region);
	ClassDB::bind_method(D_METHOD("get_region_pieces", "movie", "region"), &Bf3UiAssets::get_region_pieces);
	ClassDB::bind_method(D_METHOD("get_hud_placements", "movie"), &Bf3UiAssets::get_hud_placements);
	ClassDB::bind_method(D_METHOD("get_layout", "movie"), &Bf3UiAssets::get_layout);
	ADD_SIGNAL(MethodInfo("assets_loaded", PropertyInfo(Variant::BOOL, "ok")));
}

void Bf3UiAssets::set_atlas_movies(const PackedStringArray &p_movies) {
	atlas_movies_.clear();
	for (int64_t i = 0; i < p_movies.size(); ++i) {
		atlas_movies_.push_back(movie_key(to_std(p_movies[i])));
	}
}

void Bf3UiAssets::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (g_current == nullptr) {
		g_current = this;
	}
	set_process(true);
}

void Bf3UiAssets::_process(double p_delta) {
	(void)p_delta;
	if (finished_) {
		return;
	}
	if (!loader_.joinable() && !loaded_) {
		// Wait for Bf3Localisation: it locates and opens the install.
		Bf3Localisation *loc = Bf3Localisation::current();
		if (loc == nullptr) {
			loaded_ = std::make_unique<Loaded>();
			loaded_->error = "no Bf3Localisation in the scene";
			loader_done_.store(true, std::memory_order_release);
		} else if (!loc->is_loaded() && !loc->has_failed()) {
			return;
		} else if (!loc->install()) {
			loaded_ = std::make_unique<Loaded>();
			loaded_->error = "no usable install (" + to_std(loc->get_error()) + ")";
			loader_done_.store(true, std::memory_order_release);
		} else {
			loaded_ = std::make_unique<Loaded>();
			loaded_->install = loc->install();
			// --bf3-cache=<dir> as in level_session.cpp (the per-build root of the headless godot.ui_fonts row), else
			// user://bf3cache. ProjectSettings and the user args: main thread.
			const std::string cache_arg = user_arg_value("bf3-cache");
			const std::filesystem::path cache_root = cache_arg.empty() ? default_cache_root()
																	   : std::filesystem::path(std::u8string(cache_arg.begin(), cache_arg.end()));
			const bf3::core::PathGuard guard = loc->reader().write_guard();
			const std::vector<std::string> atlas_movies = atlas_movies_;
			Loaded *out = loaded_.get();
			loader_ = std::thread([this, out, cache_root, guard, atlas_movies]() {
				const auto start = std::chrono::steady_clock::now();
				bf3::core::Result<ui::SwfLibrary> movies = ui::SwfLibrary::open(out->install);
				bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> ebx = bf3::core::EbxLibrary::open(out->install, { "Win32/UI" });
				if (!movies) {
					out->error = "swf library: " + movies.error().message;
				} else if (!ebx) {
					out->error = "Win32/UI EBX: " + ebx.error().message;
				} else {
					out->movies.emplace(std::move(*movies));
					out->ui_ebx = std::move(*ebx);
					AcquiredCache cache = acquire_cache(cache_root, guard);
					if (cache.status != bf3::core::CacheStatus::Ok) {
						out->cache_note = std::string("cache ") + bf3::core::cache_status_name(cache.status) + " " + cache.detail + ": fonts converted without it";
					}
					ui::UiFontLibraryOptions fo;
					fo.cache = cache.cache.get();
					bf3::core::Result<ui::UiFontLibrary> fonts = ui::UiFontLibrary::load(*out->movies, *out->ui_ebx, fo);
					if (!fonts) {
						out->error = "fonts: " + fonts.error().message;
					} else {
						out->fonts.emplace(std::move(*fonts));
					}
					for (const std::string &m : atlas_movies) {
						bf3::core::Result<std::shared_ptr<const ui::SwfMovie>> movie = out->movies->load(m);
						if (!movie) {
							out->warnings.push_back(m + ": " + movie.error().message);
							continue;
						}
						bf3::core::level::WorkBudget budget(ui::kSwfAtlasBudgetUnits);
						bf3::core::Result<ui::SwfAtlasMap> map = ui::build_atlas_map(**movie, m, budget);
						if (!map) {
							out->warnings.push_back(m + ": " + map.error().message);
							continue;
						}
						for (const ui::SwfExternalImage &im : (*movie)->external_images) {
							bf3::core::Result<bf3::core::res::LoadedTexture> tex = out->movies->load_atlas_texture(m, im);
							if (!tex) {
								out->warnings.push_back(m + " " + im.file_name + ": " + tex.error().message);
								continue;
							}
							bf3::core::Result<bf3::render::PreparedTexture> prep = bf3::render::prepare_texture(*tex, bf3::render::TextureRole::Color);
							if (!prep) {
								out->warnings.push_back(m + " " + im.file_name + ": " + prep.error().message);
								continue;
							}
							out->prepared[m][im.image_id] = std::move(*prep);
						}
						out->atlas_maps[m] = std::move(*map);
					}
					for (const std::string_view screen : ui::hud_screen_movies()) {
						const std::string key = "ui/assets/" + std::string(screen);
						bf3::core::level::WorkBudget budget(ui::kSwfLayoutBudgetUnits);
						bf3::core::Result<std::vector<ui::HudPlacement>> rows = ui::read_hud_placements(*out->movies, key, budget);
						if (!rows) {
							out->warnings.push_back(key + ": " + rows.error().message);
							continue;
						}
						out->hud[key] = std::move(*rows);
					}
				}
				out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				loader_done_.store(true, std::memory_order_release);
			});
		}
	}
	if (!loader_done_.load(std::memory_order_acquire)) {
		return;
	}
	join_loader();
	finished_ = true;
	set_process(false);
	Loaded &l = *loaded_;
	for (const std::string &w : l.warnings) {
		UtilityFunctions::push_warning(to_godot("[ui1] " + w));
	}
	if (!l.cache_note.empty()) {
		UtilityFunctions::print(to_godot("[ui1] " + l.cache_note));
	}
	if (!l.error.empty()) {
		UtilityFunctions::push_warning(to_godot("[ui1] UI assets: " + l.error));
		emit_signal("assets_loaded", false);
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	if (l.fonts) {
		for (const ui::ConvertedFont &f : l.fonts->fonts()) {
			Ref<FontFile> ff = make_font_file(f);
			if (ff.is_valid()) {
				l.font_files[f.font_long_name] = ff;
			}
		}
	}
	for (auto &[movie, images] : l.prepared) {
		for (auto &[id, prep] : images) {
			Ref<ImageTexture> t = make_texture(prep);
			if (t.is_valid()) {
				l.atlases[movie][id] = t;
			}
		}
	}
	l.prepared.clear();
	const double main_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	char line[256];
	std::snprintf(line, sizeof(line), "[ui1] UI assets: %zu fonts, %zu atlas movies, %zu HUD screens in %.3f s (loader) + %.3f s (main thread)",
			l.font_files.size(), l.atlas_maps.size(), l.hud.size(), l.seconds, main_s);
	UtilityFunctions::print(line);
	emit_signal("assets_loaded", true);
}

void Bf3UiAssets::_exit_tree() {
	join_loader();
	if (g_current == this) {
		g_current = nullptr;
	}
}

void Bf3UiAssets::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

bool Bf3UiAssets::is_loaded() const {
	return finished_ && loaded_ && loaded_->error.empty();
}

bool Bf3UiAssets::has_failed() const {
	return finished_ && loaded_ && !loaded_->error.empty();
}

String Bf3UiAssets::get_error() const {
	return loaded_ ? to_godot(loaded_->error) : String();
}

const ui::UiFontLibrary *Bf3UiAssets::fonts() const {
	return is_loaded() && loaded_->fonts ? &*loaded_->fonts : nullptr;
}

const ui::SwfAtlasMap *Bf3UiAssets::atlas_map(const std::string &p_movie) const {
	if (!is_loaded()) {
		return nullptr;
	}
	const auto it = loaded_->atlas_maps.find(movie_key(p_movie));
	return it == loaded_->atlas_maps.end() ? nullptr : &it->second;
}

const ui::SwfLibrary *Bf3UiAssets::movies() const {
	return is_loaded() && loaded_->movies ? &*loaded_->movies : nullptr;
}

const std::vector<ui::HudPlacement> *Bf3UiAssets::hud_placements(const std::string &p_movie) const {
	if (!is_loaded()) {
		return nullptr;
	}
	const auto it = loaded_->hud.find(movie_key(p_movie));
	return it == loaded_->hud.end() ? nullptr : &it->second;
}

Ref<Texture2D> Bf3UiAssets::atlas_texture(const std::string &p_movie, uint16_t p_image_id) const {
	if (!is_loaded()) {
		return Ref<Texture2D>();
	}
	const auto it = loaded_->atlases.find(movie_key(p_movie));
	if (it == loaded_->atlases.end()) {
		return Ref<Texture2D>();
	}
	const auto jt = it->second.find(p_image_id);
	return jt == it->second.end() ? Ref<Texture2D>() : Ref<Texture2D>(jt->second);
}

double Bf3UiAssets::load_seconds() const {
	return loaded_ ? loaded_->seconds : 0.0;
}

Ref<FontFile> Bf3UiAssets::get_font_file(const String &p_name) const {
	const ui::UiFontLibrary *lib = fonts();
	if (lib == nullptr) {
		return Ref<FontFile>();
	}
	const ui::ConvertedFont *f = lib->find(to_std(p_name));
	if (f == nullptr) {
		return Ref<FontFile>();
	}
	const auto it = loaded_->font_files.find(f->font_long_name);
	return it == loaded_->font_files.end() ? Ref<FontFile>() : it->second;
}

Ref<Font> Bf3UiAssets::get_font(const String &p_name) const {
	Ref<FontFile> f = get_font_file(p_name);
	if (f.is_valid()) {
		return f;
	}
	if (!loaded_) {
		return Ref<Font>();
	}
	// Device and unresolved names: a system font of that name (Godot falls back further on its own).
	const std::string name = to_std(p_name);
	if (loaded_->logged.insert("font:" + name).second) {
		const ui::UiFontLibrary *lib = fonts();
		const ui::ResolvedFont r = lib != nullptr ? ui::resolve_font_name(lib->mapping(), name) : ui::ResolvedFont{};
		UtilityFunctions::push_warning(to_godot("[ui1] font '" + name + "' (" + ui::font_resolution_name(r.how) + "): a system font is used"));
	}
	Ref<SystemFont> &sf = loaded_->system_fonts[name];
	if (sf.is_null()) {
		sf.instantiate();
		PackedStringArray names;
		names.push_back(p_name.begins_with("$") ? String("Arial") : p_name);
		sf->set_font_names(names);
	}
	return sf;
}

Ref<AtlasTexture> Bf3UiAssets::get_region(const String &p_movie, const String &p_region) const {
	const ui::SwfAtlasMap *map = atlas_map(to_std(p_movie));
	const ui::SwfAtlasRegion *r = map != nullptr ? map->find(to_std(p_region)) : nullptr;
	if (r == nullptr || r->pieces.empty()) {
		if (loaded_ && loaded_->logged.insert("region:" + to_std(p_movie) + ":" + to_std(p_region)).second) {
			UtilityFunctions::push_warning(to_godot("[ui1] no atlas region " + to_std(p_movie) + " " + to_std(p_region)));
		}
		return Ref<AtlasTexture>();
	}
	return make_atlas_region(atlas_texture(map->movie, r->pieces[0].image_id), r->pieces[0]);
}

Array Bf3UiAssets::get_region_pieces(const String &p_movie, const String &p_region) const {
	Array out;
	const ui::SwfAtlasMap *map = atlas_map(to_std(p_movie));
	const ui::SwfAtlasRegion *r = map != nullptr ? map->find(to_std(p_region)) : nullptr;
	if (r == nullptr) {
		return out;
	}
	for (const ui::SwfAtlasPiece &p : r->pieces) {
		Dictionary d;
		d["texture"] = make_atlas_region(atlas_texture(map->movie, p.image_id), p);
		d["transform"] = Transform2D(p.placement.a, p.placement.b, p.placement.c, p.placement.d, p.placement.tx, p.placement.ty);
		d["sub_image"] = int64_t(p.sub_image);
		out.push_back(d);
	}
	return out;
}

Array Bf3UiAssets::get_hud_placements(const String &p_movie) const {
	Array out;
	const std::vector<ui::HudPlacement> *rows = hud_placements(to_std(p_movie));
	if (rows == nullptr) {
		return out;
	}
	for (const ui::HudPlacement &p : *rows) {
		Dictionary d;
		d["instance"] = to_godot(p.instance);
		d["widget"] = to_godot(p.widget);
		d["widget_movie"] = to_godot(p.widget_movie);
		d["x"] = double(p.matrix.tx);
		d["y"] = double(p.matrix.ty);
		d["scale_x"] = std::sqrt(double(p.matrix.a) * p.matrix.a + double(p.matrix.b) * p.matrix.b);
		d["scale_y"] = std::sqrt(double(p.matrix.c) * p.matrix.c + double(p.matrix.d) * p.matrix.d);
		d["rotation"] = std::atan2(double(p.matrix.b), double(p.matrix.a));
		d["anchor"] = String(ui::hud_anchor_name(p.anchor));
		d["anchor_dx"] = double(p.anchor_dx);
		d["anchor_dy"] = double(p.anchor_dy);
		d["depth"] = int64_t(p.depth);
		out.push_back(d);
	}
	return out;
}

Dictionary Bf3UiAssets::get_layout(const String &p_movie) const {
	const ui::SwfLibrary *lib = movies();
	if (lib == nullptr) {
		return Dictionary();
	}
	const std::string key = movie_key(to_std(p_movie));
	std::lock_guard<std::mutex> lock(loaded_->layout_mutex);
	const auto it = loaded_->layouts.find(key);
	if (it != loaded_->layouts.end()) {
		return it->second;
	}
	bf3::core::level::WorkBudget budget(ui::kSwfLayoutBudgetUnits);
	bf3::core::Result<ui::SwfLayoutNode> tree = ui::read_layout(*lib, key, {}, budget);
	Dictionary d;
	if (!tree) {
		if (loaded_->logged.insert("layout:" + key).second) {
			UtilityFunctions::push_warning(to_godot("[ui1] layout " + key + ": " + tree.error().message));
		}
	} else {
		d = layout_dict(*tree);
	}
	loaded_->layouts[key] = d;
	return d;
}

} // namespace bf3::godot_side
