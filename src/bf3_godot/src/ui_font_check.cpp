// The ui1 Godot check (bf3_godot ui_font_check.h; docs/design/ui-swf-fonts.md §1 G4/G5, §11): rows for BF3's fonts as
// Godot FontFiles, the iconsingame atlas, the HUD table and the install; the three screenshot pages.
#include "ui_font_check.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "ui_assets.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/font_ttf.h"
#include "bf3/core/ui/swf_census.h"
#include "bf3/core/ui/swf_font.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/text_server_manager.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

std::string fmt(const char *p_fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, p_fmt);
	vsnprintf(buf, sizeof(buf), p_fmt, args);
	va_end(args);
	return buf;
}

constexpr const char *kMetro = "OPERATION M\xC3\x89TRO";
constexpr int kSize = 64;
constexpr double kSwfToPx = double(kSize) / double(ui::kSwfFontUnitsPerEm);
constexpr const char *kShotFiles[3] = { "ui1_fonts.png", "ui1_coverage.png", "ui1_icons.png" };

// Decodes UTF-8 (the text database's strings) into code points; invalid bytes are skipped.
void utf8_codes(std::string_view p_text, std::vector<uint32_t> &r_out) {
	r_out.clear();
	for (size_t i = 0; i < p_text.size();) {
		const uint8_t c = uint8_t(p_text[i]);
		uint32_t cp = 0;
		size_t n = 0;
		if (c < 0x80) {
			cp = c;
			n = 1;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			n = 2;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			n = 3;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			n = 4;
		} else {
			++i;
			continue;
		}
		if (i + n > p_text.size()) {
			break;
		}
		for (size_t k = 1; k < n; ++k) {
			cp = (cp << 6) | (uint8_t(p_text[i + k]) & 0x3F);
		}
		r_out.push_back(cp);
		i += n;
	}
}

double signed_area2(const std::vector<std::pair<double, double>> &p_poly) {
	double a = 0.0;
	for (size_t i = 0; i < p_poly.size(); ++i) {
		const auto &p = p_poly[i];
		const auto &q = p_poly[(i + 1) % p_poly.size()];
		a += p.first * q.second - q.first * p.second;
	}
	return a;
}

struct FontSource {
	std::string name;
	std::string error;
	ui::SwfFontOutlines outlines;
	std::map<uint16_t, uint32_t> first_glyph; // code -> source glyph
	std::map<uint32_t, int32_t> kerning; // left << 16 | right (codes) -> adjust
	ui::TrueTypeFont ttf;
	std::string ttf_error;
};

} // namespace

struct Bf3UiFontCheck::Work {
	std::vector<FontSource> fonts;
	std::array<uint32_t, 5> rules{};
	std::vector<std::string> family;
	std::string census_error;
	size_t library_names = 0;
	size_t library_exact = 0;
	size_t strings = 0;
	std::map<uint32_t, size_t> missing; // code point -> occurrences, characters Purista EA Medium lacks
	std::string text_error;
};

Bf3UiFontCheck::Bf3UiFontCheck() = default;

Bf3UiFontCheck::~Bf3UiFontCheck() {
	if (worker_.joinable()) {
		worker_.join();
	}
}

void Bf3UiFontCheck::_bind_methods() {}

void Bf3UiFontCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	check_ = has_user_arg("ui-check");
	shot_dir_ = user_arg_value("shot-dir");
	const std::string page = user_arg_value("page");
	page_ = page.empty() ? 0 : std::clamp(std::atoi(page.c_str()), 0, 2);
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	if (!shot_dir_.empty()) {
		// 1280 x 720 at 1:1 pixels, so the shots show the fonts at their nominal sizes.
		Window *w = get_window();
		w->set_content_scale_mode(Window::CONTENT_SCALE_MODE_DISABLED);
		w->set_size(Vector2i(1280, 720));
		page_ = 0;
	}
	set_process(true);
}

void Bf3UiFontCheck::start_work(Bf3UiAssets &p_assets) {
	work_ = std::make_unique<Work>();
	Work *w = work_.get();
	const ui::SwfLibrary *movies = p_assets.movies();
	const ui::UiFontLibrary *fonts = p_assets.fonts();
	std::shared_ptr<const bf3::core::ui::TextDatabase> db;
	if (Bf3Localisation *loc = Bf3Localisation::current()) {
		db = loc->database();
	}
	worker_ = std::thread([this, w, movies, fonts, db]() {
		// Source outlines of each converted font, and its TrueType read back.
		const std::string &lib_name = fonts->mapping().font_library;
		bf3::core::Result<std::shared_ptr<const ui::SwfMovie>> lib = movies->load(lib_name);
		for (const ui::ConvertedFont &f : fonts->fonts()) {
			FontSource s;
			s.name = f.font_long_name;
			if (!lib) {
				s.error = lib.error().message;
			} else {
				const ui::SwfFontInfo *info = nullptr;
				for (const ui::SwfFontInfo &i : (*lib)->fonts) {
					if (i.name == f.font_long_name && i.kind == ui::SwfFontKind::Outlines) {
						info = &i;
					}
				}
				if (info == nullptr) {
					s.error = "no DefineFont3 " + f.font_long_name;
				} else {
					bf3::core::level::WorkBudget budget(ui::kUiFontConvertBudgetUnits);
					bf3::core::Result<ui::SwfFontOutlines> o = ui::load_font_outlines(**lib, *info, budget);
					if (!o) {
						s.error = o.error().message;
					} else {
						s.outlines = std::move(*o);
						for (size_t g = 0; g < s.outlines.codes.size(); ++g) {
							s.first_glyph.emplace(s.outlines.codes[g], uint32_t(g));
						}
						for (const ui::KerningPair &k : s.outlines.kerning) {
							s.kerning[(uint32_t(k.left) << 16) | k.right] = k.adjust;
						}
					}
				}
			}
			bf3::core::level::WorkBudget rb(ui::kTrueTypeReadBudgetUnits);
			bf3::core::Result<ui::TrueTypeFont> t = ui::read_truetype(f.ttf, rb);
			if (t) {
				s.ttf = std::move(*t);
			} else {
				s.ttf_error = t.error().message;
			}
			w->fonts.push_back(std::move(s));
		}
		// The font names of every movie (G6's census; atlases and textures are G1's business).
		ui::SwfCensusOptions co;
		co.check_atlas_textures = false;
		co.build_atlas_maps = false;
		co.font_mapping = &fonts->mapping();
		bf3::core::Result<ui::SwfCensus> census = ui::run_swf_census(*movies, co);
		if (!census) {
			w->census_error = census.error().message;
		} else {
			w->rules = census->totals.font_names_by_rule;
			w->family = census->totals.family_names;
		}
		// Coverage of the text database by Purista EA Medium.
		const ui::ConvertedFont *purista = fonts->find("Purista EA Medium");
		if (db && purista != nullptr) {
			std::set<uint32_t> have(purista->codes.begin(), purista->codes.end());
			std::vector<uint32_t> cps;
			w->strings = db->size();
			for (size_t i = 0; i < db->size(); ++i) {
				utf8_codes(db->text_at(i), cps);
				for (const uint32_t cp : cps) {
					if (cp >= 0x20 && have.count(cp) == 0) {
						w->missing[cp]++;
					}
				}
			}
		} else {
			w->text_error = db ? "no Purista EA Medium" : "no text database";
		}
		work_done_.store(true, std::memory_order_release);
	});
}

void Bf3UiFontCheck::_process(double p_delta) {
	(void)p_delta;
	if (done_) {
		return;
	}
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr) {
		UtilityFunctions::print("[ui1] no Bf3UiAssets in the scene");
		exit_code_ = 1;
		finish();
		return;
	}
	if (!assets->is_loaded() && !assets->has_failed()) {
		return;
	}
	if (assets->has_failed()) {
		const std::string e = to_std(assets->get_error());
		UtilityFunctions::print(to_godot("[ui1] UI assets failed: " + e));
		UtilityFunctions::print(to_godot(std::string("UI1 CHECK: FAIL (") + e + ")"));
		exit_code_ = 1;
		if (check_ || !shot_dir_.empty()) {
			finish();
			return;
		}
		done_ = true;
		queue_redraw();
		return;
	}
	if (!work_) {
		start_work(*assets);
		return;
	}
	if (!work_done_.load(std::memory_order_acquire)) {
		return;
	}
	if (worker_.joinable()) {
		worker_.join();
	}
	if (!checked_) {
		checked_ = true;
		run_checks(*assets);
		queue_redraw();
		if (shot_dir_.empty()) {
			if (check_) {
				finish();
			}
			return;
		}
		page_ = 0;
		frames_on_page_ = 0;
		return;
	}
	if (shot_dir_.empty()) {
		return;
	}
	// One page per few frames, captured after it has been drawn.
	queue_redraw();
	if (++frames_on_page_ < 8) {
		return;
	}
	if (!save_shot(kShotFiles[page_])) {
		exit_code_ = 1;
	}
	frames_on_page_ = 0;
	if (++page_ >= 3) {
		finish();
	}
}

bool Bf3UiFontCheck::save_shot(const std::string &p_file) {
	const std::filesystem::path dir = std::filesystem::path(std::u8string(shot_dir_.begin(), shot_dir_.end()));
	const std::filesystem::path target = dir / p_file;
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr) {
		return false;
	}
	const bf3::core::PathGuard guard = loc->reader().write_guard();
	if (!guard.allows_write(target)) {
		UtilityFunctions::push_error(to_godot("[ui1] screenshot refused (inside the install): " + target.string()));
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	Ref<Image> img = get_viewport()->get_texture()->get_image();
	const std::u8string u8 = target.u8string();
	const std::string path(u8.begin(), u8.end());
	if (img.is_null() || img->save_png(to_godot(path)) != OK) {
		UtilityFunctions::push_error(to_godot("[ui1] screenshot failed: " + path));
		return false;
	}
	UtilityFunctions::print(to_godot(fmt("[ui1] screenshot %s (%dx%d)", path.c_str(), img->get_width(), img->get_height())));
	return true;
}

void Bf3UiFontCheck::finish() {
	done_ = true;
	if (worker_.joinable()) {
		worker_.join();
	}
	set_process(false);
	get_tree()->quit(exit_code_);
}

void Bf3UiFontCheck::run_checks(Bf3UiAssets &p_assets) {
	rows_.clear();
	auto add = [&](const std::string &p_name, bool p_pass, const std::string &p_detail) { rows_.push_back({ p_name, p_pass, p_detail }); };
	Ref<TextServer> ts = TextServerManager::get_singleton()->get_primary_interface();
	const ui::UiFontLibrary &lib = *p_assets.fonts();
	// The width of one shaped line, unrounded: the sum of its shaped glyph advances (HarfBuzz's 26.6 values with the
	// kerning applied; measured fractional, e.g. 39.8125 px for Purista's "0" at 64 px). Font.get_string_size() and
	// TextServer.shaped_text_get_width() both return the line width ceil()ed to whole pixels on Godot 4.8 dev7
	// (measured: 367.000 px for a 366.519 px line, 404.000 px for Venice's 10 x 40.3125 px), so they cannot carry a
	// 0.5 px or 0.25 px tolerance.
	auto shaped_width = [&](const Ref<FontFile> &p_font, const String &p_text) {
		const RID sh = ts->create_shaped_text();
		ts->shaped_text_add_string(sh, p_text, p_font->get_rids(), kSize);
		ts->shaped_text_shape(sh);
		const Array glyphs = ts->shaped_text_get_glyphs(sh);
		double w = 0.0;
		for (int64_t i = 0; i < glyphs.size(); ++i) {
			const Dictionary g = glyphs[i];
			const int64_t repeat = g.has("repeat") ? int64_t(g["repeat"]) : 1;
			w += double(g["advance"]) * double(std::max<int64_t>(1, repeat));
		}
		ts->free_rid(sh);
		return w;
	};
	const Work &w = *work_;

	for (const FontSource &src : w.fonts) {
		const std::string &name = src.name;
		const ui::ConvertedFont *cf = lib.find(name);
		Ref<FontFile> ff = p_assets.get_font_file(to_godot(name));
		// fonts.set_data
		add("fonts.set_data " + name, ff.is_valid() && cf != nullptr,
				ff.is_valid() ? fmt("FontFile.set_data(%zu bytes) accepted, get_font_name() '%s'", cf ? cf->ttf.size() : 0, to_std(ff->get_font_name()).c_str())
							  : std::string("FontFile refused"));
		if (ff.is_null() || cf == nullptr || !src.error.empty() || !src.ttf_error.empty()) {
			add("fonts.source " + name, false, src.error + src.ttf_error);
			continue;
		}
		const TypedArray<RID> rids = ff->get_rids();
		const RID rid = rids.size() > 0 ? RID(rids[0]) : RID();
		const ui::SwfFontOutlines &o = src.outlines;
		const ui::TrueTypeFont &tt = src.ttf;
		// fonts.glyphs
		{
			const String supported = ff->get_supported_chars();
			size_t missing = 0;
			for (const uint16_t c : cf->codes) {
				missing += ff->has_char(c) ? 0 : 1;
			}
			const int64_t ts_glyphs = ts->font_get_supported_glyphs(rid).size();
			const bool pass = tt.glyphs.size() == o.glyphs.size() + 1 && size_t(supported.length()) == cf->codes.size() && missing == 0;
			add("fonts.glyphs " + name, pass,
					fmt("TrueType numGlyphs %zu (source %zu + .notdef), get_supported_chars() %lld (distinct codes %zu), has_char misses %zu, "
						"TextServer.font_get_supported_glyphs %lld",
							tt.glyphs.size(), o.glyphs.size(), static_cast<long long>(supported.length()), cf->codes.size(), missing,
							static_cast<long long>(ts_glyphs)));
		}
		// fonts.contours: count and orientation of every glyph's contours as FreeType reads them.
		{
			size_t glyphs_bad = 0;
			size_t contours = 0;
			size_t orientation_bad = 0;
			int sign = 0; // +1 / -1: Godot's y axis relative to TrueType's, fixed by the first non-degenerate contour
			for (size_t g = 0; g < tt.glyphs.size(); ++g) {
				const ui::TrueTypeGlyph &tg = tt.glyphs[g];
				const Dictionary d = ts->font_get_glyph_contours(rid, kSize, int64_t(g));
				const PackedVector3Array pts = d.has("points") ? PackedVector3Array(d["points"]) : PackedVector3Array();
				const PackedInt32Array ends = d.has("contours") ? PackedInt32Array(d["contours"]) : PackedInt32Array();
				if (size_t(ends.size()) != tg.contour_ends.size() || size_t(pts.size()) != tg.xs.size()) {
					++glyphs_bad;
					continue;
				}
				size_t first = 0;
				for (size_t c = 0; c < tg.contour_ends.size(); ++c) {
					const size_t last = tg.contour_ends[c];
					std::vector<std::pair<double, double>> a;
					std::vector<std::pair<double, double>> b;
					for (size_t k = first; k <= last; ++k) {
						a.push_back({ double(tg.xs[k]), double(tg.ys[k]) });
						b.push_back({ double(pts[int64_t(k)].x), double(pts[int64_t(k)].y) });
					}
					const double sa = signed_area2(a);
					const double sb = signed_area2(b);
					++contours;
					if (sa != 0.0 && sb != 0.0) {
						const int rel = (sa > 0) == (sb > 0) ? 1 : -1;
						if (sign == 0) {
							sign = rel;
						} else if (rel != sign) {
							++orientation_bad;
						}
					}
					first = last + 1;
				}
			}
			add("fonts.contours " + name, glyphs_bad == 0 && orientation_bad == 0 && contours > 0,
					fmt("%zu glyphs, %zu contours via TextServer.font_get_glyph_contours(%d px): %zu glyphs with another contour or point count, %zu contours "
						"with another orientation than the writer's (Godot y %s)",
							tt.glyphs.size(), contours, kSize, glyphs_bad, orientation_bad, sign < 0 ? "down" : "up"));
		}
		// fonts.metrics
		{
			const double ascent = ff->get_ascent(kSize);
			const double want_ascent = double(tt.ascender) * kSize / double(tt.units_per_em);
			double worst = 0.0;
			size_t worst_glyph = 0;
			for (size_t g = 0; g < o.glyphs.size() && g < o.advances.size(); ++g) {
				const double got = ts->font_get_glyph_advance(rid, kSize, int64_t(g + 1)).x;
				const double want = double(o.advances[g]) * kSwfToPx;
				if (std::fabs(got - want) > worst) {
					worst = std::fabs(got - want);
					worst_glyph = g + 1;
				}
			}
			auto source_width = [&](const std::vector<uint32_t> &p_codes) {
				double sum = 0.0;
				for (size_t i = 0; i < p_codes.size(); ++i) {
					const auto it = src.first_glyph.find(uint16_t(p_codes[i]));
					if (it != src.first_glyph.end() && it->second < o.advances.size()) {
						sum += double(o.advances[it->second]);
					}
					if (i + 1 < p_codes.size()) {
						const auto k = src.kerning.find((p_codes[i] << 16) | p_codes[i + 1]);
						if (k != src.kerning.end()) {
							sum += double(k->second);
						}
					}
				}
				return sum * kSwfToPx;
			};
			std::vector<uint32_t> digits_cp;
			utf8_codes("0123456789", digits_cp);
			const double digits = shaped_width(ff, "0123456789");
			const double want_digits = source_width(digits_cp);
			bool pass = std::fabs(ascent - want_ascent) <= 1.0 && worst <= 0.05 && std::fabs(digits - want_digits) <= 0.5;
			std::string detail = fmt("at %d px: ascent %.2f (TrueType %.2f); %zu glyph advances, worst |TextServer - source| %.4f px (glyph %zu); "
									 "shaped \"0123456789\" %.3f px (source %.3f)",
					kSize, ascent, want_ascent, o.glyphs.size(), worst, worst_glyph, digits, want_digits);
			if (name.rfind("Purista", 0) == 0) {
				std::vector<uint32_t> metro_cp;
				utf8_codes(kMetro, metro_cp);
				const double metro = shaped_width(ff, to_godot(kMetro));
				const double want_metro = source_width(metro_cp);
				pass = pass && std::fabs(metro - want_metro) <= 0.5;
				detail += fmt("; \"%s\" %.3f px (source + kerning %.3f)", kMetro, metro, want_metro);
			}
			add("fonts.metrics " + name, pass, detail);
		}
		// fonts.kerning (Purista)
		if (name.rfind("Purista", 0) == 0) {
			const double a = shaped_width(ff, "A");
			const double v = shaped_width(ff, "V");
			const double av = shaped_width(ff, "AV");
			const auto k = src.kerning.find((uint32_t('A') << 16) | uint32_t('V'));
			const double want = k == src.kerning.end() ? 0.0 : -double(k->second) * kSwfToPx;
			const double narrower = a + v - av;
			add("fonts.kerning " + name, std::fabs(narrower - want) <= 0.25 && want > 0.0,
					fmt("\"AV\" %.3f px vs \"A\" + \"V\" %.3f px: %.3f px narrower (source kerning %d units = %.3f px)", av, a + v, narrower,
							k == src.kerning.end() ? 0 : k->second, want));
		}
	}
	// fonts.metro
	{
		const String text = tr(StringName("ID_M_LEVELNAME_MP_SUBWAY"));
		Ref<FontFile> ff = p_assets.get_font_file("Purista EA Medium");
		size_t zero = 0;
		if (ff.is_valid()) {
			const RID rid = RID(ff->get_rids()[0]);
			for (int64_t i = 0; i < text.length(); ++i) {
				zero += ts->font_get_glyph_index(rid, kSize, text[i], 0) == 0 ? 1 : 0;
			}
		}
		const bool same = to_std(text) == kMetro;
		add("fonts.metro", ff.is_valid() && same && zero == 0,
				fmt("tr(ID_M_LEVELNAME_MP_SUBWAY) = \"%s\"%s; %zu characters without a Purista EA Medium glyph", to_std(text).c_str(),
						same ? "" : " (expected OPERATION MÉTRO)", zero));
	}
	// fonts.mapping
	{
		std::string fam;
		for (const std::string &n : w.family) {
			fam += " " + n;
		}
		add("fonts.mapping", w.census_error.empty() && w.rules[size_t(ui::FontResolution::Unresolved)] == 0,
				w.census_error.empty() ? fmt("font names of the %zu movies: Exact %u, CaseFolded %u, Family %u, Device %u, Unresolved %u; family-inferred:%s",
												 p_assets.movies()->movies().size(), w.rules[0], w.rules[1], w.rules[2], w.rules[3], w.rules[4], fam.c_str())
									   : w.census_error);
	}
	// fonts.coverage (reported)
	{
		size_t occurrences = 0;
		std::string first;
		size_t listed = 0;
		for (const auto &[cp, n] : w.missing) {
			occurrences += n;
			if (listed++ < 12) {
				first += fmt(" U+%04X", cp);
			}
		}
		add("fonts.coverage", true,
				w.text_error.empty() ? fmt("(reported) %zu strings: %zu distinct characters Purista EA Medium lacks (%zu occurrences; system fallback):%s",
											   w.strings, w.missing.size(), occurrences, first.c_str())
									 : "(reported) " + w.text_error);
	}
	// atlas.iconsingame
	const ui::SwfAtlasMap *map = p_assets.atlas_map("ui/assets/iconsingame");
	{
		Ref<Texture2D> tex = map != nullptr && !map->images.empty() ? p_assets.atlas_texture(map->movie, map->images[0].image_id) : Ref<Texture2D>();
		if (map == nullptr || tex.is_null()) {
			add("atlas.iconsingame", false, "no atlas map or texture");
		} else {
			const ui::SwfAtlasStats &s = map->stats;
			const bool pass = s.sub_images == 162 && s.rects_outside_atlas == 0 && s.labels == 427 && s.labels_with_art == 135 && s.labels_one_piece == 110 &&
					s.labels_several_pieces == 25 && s.labels_empty == 292 && s.sub_images_from_labels == 95 && s.sub_images_from_symbols == 96 &&
					s.sub_images_from_frames == 162 && tex->get_width() == 4096 && tex->get_height() == 128;
			add("atlas.iconsingame", pass,
					fmt("%s %dx%d ImageTexture; %u sub-images (%u outside), %u labels: %u with art (%u one piece, %u several), %u empty; sub-images from labels "
						"%u, with symbols %u, from frames %u",
							map->images[0].file_name.c_str(), int(tex->get_width()), int(tex->get_height()), s.sub_images, s.rects_outside_atlas, s.labels,
							s.labels_with_art, s.labels_one_piece, s.labels_several_pieces, s.labels_empty, s.sub_images_from_labels, s.sub_images_from_symbols,
							s.sub_images_from_frames));
		}
	}
	// atlas.regions
	{
		struct Spot {
			const char *region;
			int x1, y1, x2, y2;
		};
		const Spot spots[] = {
			{ "mc_iconsIngameBig/LocalPlayer", 576, 58, 599, 83 },
			{ "mc_iconsIngameBig/FriendlyPlayer", 1070, 58, 1089, 80 },
			{ "mc_iconsIngameBig/EnemyPlayer", 1028, 58, 1047, 80 },
			{ "mc_iconsIngameBig/AmmoCrate", 498, 0, 550, 52 },
			{ "mc_iconsIngameBig/EnemyFlag", 114, 0, 168, 54 },
			{ "mc_iconsIngameBig/Tank", 2386, 0, 2411, 34 },
			{ "mc_iconsIngameBig/CTFFlagEnemyDropped", 780, 0, 811, 43 },
			{ "mc_iconHUDVehicleFriend/friendDriver", 1091, 58, 1115, 79 },
			{ "mc_iconsIngameSmall/EnemyFlag", 2013, 0, 2047, 34 },
		};
		size_t bad = 0;
		for (const Spot &s : spots) {
			Ref<AtlasTexture> t = p_assets.get_region("ui/assets/iconsingame", s.region);
			if (t.is_null() || t->get_region() != Rect2(real_t(s.x1), real_t(s.y1), real_t(s.x2 - s.x1), real_t(s.y2 - s.y1)) || !t->has_filter_clip()) {
				++bad;
			}
		}
		const Array two = p_assets.get_region_pieces("ui/assets/iconsingame", "mc_iconsIngameBig/CTFFlagEnemyDropped");
		const bool empty_ok = p_assets.get_region_pieces("ui/assets/iconsingame", "mc_iconsIngameSmall/FriendlyPlayer").is_empty();
		add("atlas.regions", bad == 0 && two.size() == 2 && empty_ok,
				fmt("%zu spot regions as AtlasTextures (filter_clip), %zu with another rectangle; CTFFlagEnemyDropped %lld pieces; "
					"mc_iconsIngameSmall/FriendlyPlayer %s",
						std::size(spots), bad, static_cast<long long>(two.size()), empty_ok ? "empty" : "not empty"));
	}
	// hud.table
	{
		size_t total = 0;
		size_t screens = 0;
		for (const std::string_view s : ui::hud_screen_movies()) {
			if (const std::vector<ui::HudPlacement> *rows = p_assets.hud_placements(std::string(s))) {
				total += rows->size();
				++screens;
			}
		}
		auto at = [&](const char *p_screen, const char *p_instance, double p_x, double p_y) {
			const Array rows = p_assets.get_hud_placements(p_screen);
			for (int64_t i = 0; i < rows.size(); ++i) {
				const Dictionary d = rows[i];
				if (String(d["instance"]) == String(p_instance)) {
					return std::fabs(double(d["x"]) - p_x) <= 0.05 && std::fabs(double(d["y"]) - p_y) <= 0.05;
				}
			}
			return false;
		};
		const bool spots = at("hudmpscreen", "Ammo_01", 1194, 669) && at("hudmpscreen", "Health_01", 1193, 605) &&
				at("hudmpscreen", "LatencyIndicator_01", 1274, 3) && at("hudtdmscreen", "Minimap_01", -3, 527) &&
				at("hudtdmscreen", "SquadList_00", 179, 619) && at("killscreen", "HudMessage_02", 640, 604);
		add("hud.table", total == 86 && screens == 14 && spots,
				fmt("%zu placements on %zu screens; Ammo_01, Health_01, LatencyIndicator_01, Minimap_01, SquadList_00, HudMessage_02 %s", total, screens,
						spots ? "at their §8.2 positions" : "NOT at their §8.2 positions"));
	}
	// install.unchanged
	{
		Bf3Localisation *loc = Bf3Localisation::current();
		if (loc == nullptr || !loc->install_snapshot_at_start().valid) {
			add("install.unchanged", false, "no snapshot at start");
		} else {
			const bf3::core::InstallSnapshot &before = loc->install_snapshot_at_start();
			const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, loc->reader().snapshot());
			add("install.unchanged", d.clean(), d.clean() ? fmt("%zu files: sizes and mtimes equal", before.files.size()) : d.summary());
		}
	}

	size_t passed = 0;
	UtilityFunctions::print("");
	UtilityFunctions::print("UI1 CHECK");
	for (const Row &r : rows_) {
		passed += r.pass ? 1 : 0;
		UtilityFunctions::print(to_godot(fmt("  %-4s %-34s %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	char line[160];
	std::snprintf(line, sizeof(line), "[ui1] UI assets loaded in %.3f s", p_assets.load_seconds());
	UtilityFunctions::print(line);
	const bool ok = passed == rows_.size();
	UtilityFunctions::print(to_godot(fmt("UI1 CHECK: %s (%zu/%zu)", ok ? "PASS" : "FAIL", passed, rows_.size())));
	exit_code_ = ok ? exit_code_ : 1;
}

void Bf3UiFontCheck::_draw() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const Vector2 size = get_size();
	draw_rect(Rect2(Vector2(), size), Color(0.16f, 0.16f, 0.17f));
	Bf3UiAssets *assets = Bf3UiAssets::current();
	Ref<Font> label = get_theme_default_font();
	if (assets == nullptr || !assets->is_loaded()) {
		const String msg = assets != nullptr && assets->has_failed() ? "UI assets failed: " + assets->get_error() : String("loading BF3 UI assets...");
		draw_string(label, Vector2(16, 32), msg, HORIZONTAL_ALIGNMENT_LEFT, -1, 16, Color(1, 1, 1));
		return;
	}
	switch (page_) {
		case 0: draw_fonts_page(*assets); break;
		case 1: draw_coverage_page(*assets); break;
		default: draw_icons_page(*assets); break;
	}
}

void Bf3UiFontCheck::draw_fonts_page(Bf3UiAssets &p_assets) {
	Ref<Font> label = get_theme_default_font();
	const Color white(1, 1, 1);
	const Color red(1, 0.15f, 0.15f);
	const Color grey(0.6f, 0.6f, 0.65f);
	float y = 6.0f;
	auto line = [&](const Ref<Font> &p_font, const String &p_text, int p_size) {
		const float ascent = float(p_font->get_ascent(p_size));
		const float descent = float(p_font->get_descent(p_size));
		const float baseline = y + ascent;
		const float width = float(p_font->get_string_size(p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size).x);
		draw_line(Vector2(16, baseline + 0.5f), Vector2(16 + width, baseline + 0.5f), red, 1.0f);
		draw_string(p_font, Vector2(16, baseline), p_text, HORIZONTAL_ALIGNMENT_LEFT, -1, p_size, white);
		y = baseline + descent + 2.0f;
	};
	struct Sample {
		const char *font;
		bool letters;
	};
	const Sample samples[] = { { "Purista EA Medium", true }, { "Purista EA Semibold", true }, { "Venice-Medium", false }, { "Venice-SemiBold", false } };
	for (const Sample &s : samples) {
		Ref<FontFile> f = p_assets.get_font_file(s.font);
		draw_string(label, Vector2(16, y + 11), to_godot(std::string(s.font) + "  (16 / 32 / 64 px; red line = baseline)"), HORIZONTAL_ALIGNMENT_LEFT, -1, 11,
				grey);
		y += 14.0f;
		if (f.is_null()) {
			continue;
		}
		if (s.letters) {
			line(f, to_godot(std::string(kMetro) + "  0123456789  31 / 155"), 64);
			line(f, "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz", 32);
			line(f, to_godot(std::string("AV AT To LT Yo   ") + kMetro + "   ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789"), 16);
		} else {
			line(f, "0123456789  31/155 100%", 64);
			line(f, "0123456789  31/155 100%", 32);
			line(f, "0123456789  31/155 100%", 16);
		}
		y += 4.0f;
	}
}

void Bf3UiFontCheck::draw_coverage_page(Bf3UiAssets &p_assets) {
	Ref<Font> label = get_theme_default_font();
	const ui::UiFontLibrary *lib = p_assets.fonts();
	const float cell_w = 30.0f;
	const float cell_h = 22.0f;
	const int columns = int((get_size().x - 16) / cell_w);
	float y = 4.0f;
	for (const ui::ConvertedFont &cf : lib->fonts()) {
		Ref<FontFile> f = p_assets.get_font_file(to_godot(cf.font_long_name));
		draw_string(label, Vector2(8, y + 11), to_godot(fmt("%s: %zu codes", cf.font_long_name.c_str(), cf.codes.size())), HORIZONTAL_ALIGNMENT_LEFT, -1, 11,
				Color(0.6f, 0.6f, 0.65f));
		y += 14.0f;
		for (size_t i = 0; i < cf.codes.size(); ++i) {
			const float x = 8.0f + float(int(i) % columns) * cell_w;
			const float cy = y + float(int(i) / columns) * cell_h;
			draw_rect(Rect2(x, cy, cell_w - 1, cell_h - 1), Color(0.22f, 0.22f, 0.24f));
			if (f.is_valid()) {
				String ch;
				ch += char32_t(cf.codes[i]);
				draw_string(f, Vector2(x + 2, cy + 13), ch, HORIZONTAL_ALIGNMENT_LEFT, -1, 13, Color(1, 1, 1));
			}
			draw_string(label, Vector2(x + 2, cy + cell_h - 2), to_godot(fmt("%04X", cf.codes[i])), HORIZONTAL_ALIGNMENT_LEFT, -1, 7, Color(0.55f, 0.75f, 1.0f));
		}
		y += float((int(cf.codes.size()) + columns - 1) / columns) * cell_h + 4.0f;
	}
}

void Bf3UiFontCheck::draw_icons_page(Bf3UiAssets &p_assets) {
	Ref<Font> label = get_theme_default_font();
	const ui::SwfAtlasMap *map = p_assets.atlas_map("ui/assets/iconsingame");
	if (map == nullptr) {
		return;
	}
	const float cell_w = 104.0f;
	const float cell_h = 58.0f;
	const int columns = int((get_size().x - 8) / cell_w);
	int n = 0;
	for (const ui::SwfAtlasRegion &r : map->labels) {
		if (r.pieces.empty()) {
			continue;
		}
		const float x = 6.0f + float(n % columns) * cell_w;
		const float y = 6.0f + float(n / columns) * cell_h;
		++n;
		draw_rect(Rect2(x, y, cell_w - 2, cell_h - 2), Color(0.22f, 0.22f, 0.24f));
		// Bounds of the pieces in the symbol's space, then fit into the cell (at most 1.5x).
		float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
		for (const ui::SwfAtlasPiece &p : r.pieces) {
			const float w = float(p.x2 - p.x1);
			const float h = float(p.y2 - p.y1);
			const float xs[4] = { 0, w, 0, w };
			const float ys[4] = { 0, 0, h, h };
			for (int k = 0; k < 4; ++k) {
				const float px = p.placement.a * xs[k] + p.placement.c * ys[k] + p.placement.tx;
				const float py = p.placement.b * xs[k] + p.placement.d * ys[k] + p.placement.ty;
				bx0 = std::min(bx0, px);
				bx1 = std::max(bx1, px);
				by0 = std::min(by0, py);
				by1 = std::max(by1, py);
			}
		}
		const float bw = std::max(1.0f, bx1 - bx0);
		const float bh = std::max(1.0f, by1 - by0);
		const float scale = std::min({ 1.5f, (cell_w - 8) / bw, (cell_h - 16) / bh });
		const float ox = x + (cell_w - bw * scale) / 2 - bx0 * scale;
		const float oy = y + 2 + (cell_h - 16 - bh * scale) / 2 - by0 * scale;
		for (const ui::SwfAtlasPiece &p : r.pieces) {
			Ref<AtlasTexture> t = make_atlas_region(p_assets.atlas_texture(map->movie, p.image_id), p);
			if (t.is_null()) {
				continue;
			}
			const Transform2D place(p.placement.a, p.placement.b, p.placement.c, p.placement.d, p.placement.tx, p.placement.ty);
			draw_set_transform_matrix(Transform2D(scale, 0, 0, scale, ox, oy) * place);
			draw_texture(t, Vector2());
		}
		draw_set_transform_matrix(Transform2D());
		draw_string(label, Vector2(x + 2, y + cell_h - 5), to_godot(r.label), HORIZONTAL_ALIGNMENT_LEFT, cell_w - 4, 9, Color(0.8f, 0.8f, 0.85f));
	}
}

} // namespace bf3::godot_side
