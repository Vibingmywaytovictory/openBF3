// bf3dump swf commands (UI ui1; swf_commands.h, docs/design/ui-swf-fonts.md §12.4).
#include "swf_commands.h"

#include "prepare_commands.h"

#include "bf3/core/cache/digest.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/read_only_file.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_census.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/ui_fonts.h"

#include <windows.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace bf3dump {

namespace {

std::wstring widen(std::string_view p_text) {
	const int need = MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(need), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), out.data(), need);
	return out;
}

std::string movie_arg(const std::string &p_name) {
	std::string n = to_lower_ascii(p_name);
	return n.find('/') == std::string::npos ? "ui/assets/" + n : n;
}

std::string mat(const SwfMatrix &p_m) {
	char buf[160];
	if (p_m.b == 0.0f && p_m.c == 0.0f) {
		std::snprintf(buf, sizeof(buf), "(%g, %g) scale %g x %g", double(p_m.tx), double(p_m.ty), double(p_m.a), double(p_m.d));
	} else {
		std::snprintf(buf, sizeof(buf), "(%g, %g) [%g %g %g %g]", double(p_m.tx), double(p_m.ty), double(p_m.a), double(p_m.b), double(p_m.c), double(p_m.d));
	}
	return buf;
}

bool flag_value(const std::string &p_arg, const char *p_name, std::string &r_value) {
	const std::string prefix = std::string(p_name) + "=";
	if (p_arg.rfind(prefix, 0) == 0) {
		r_value = p_arg.substr(prefix.size());
		return true;
	}
	return false;
}

unsigned parse_threads(const std::string &p_value) {
	unsigned n = 0;
	std::from_chars(p_value.data(), p_value.data() + p_value.size(), n);
	return n;
}

Result<SwfLibrary> open_library(const std::shared_ptr<Install> &p_install, unsigned p_threads) {
	SwfLibraryOptions o;
	o.threads = p_threads;
	Result<SwfLibrary> lib = SwfLibrary::open(p_install, o);
	if (lib) {
		std::fprintf(stderr, "swf: %zu movies from %zu superbundles in %.3f s\n", lib->movies().size(), lib->superbundles_listed(), lib->open_seconds());
	}
	return lib;
}

Result<std::unique_ptr<EbxLibrary>> open_ui_ebx(const std::shared_ptr<Install> &p_install) {
	return EbxLibrary::open(p_install, { "Win32/UI" });
}

int cmd_census(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	std::string tsv;
	unsigned threads = 0;
	for (size_t i = 0; i < p_args.size(); ++i) {
		std::string v;
		if (p_args[i] == "--tsv" && i + 1 < p_args.size()) {
			tsv = p_args[++i];
		} else if (flag_value(p_args[i], "--threads", v)) {
			threads = parse_threads(v);
		} else {
			std::fprintf(stderr, "swf census: unknown argument %s\n", p_args[i].c_str());
			return 2;
		}
	}
	Result<SwfLibrary> lib = open_library(p_install, threads);
	if (!lib) {
		std::fprintf(stderr, "swf census: %s\n", lib.error().message.c_str());
		return 1;
	}
	std::optional<FontMappingCollection> mapping;
	std::vector<FontMappingCollection> all;
	if (Result<std::unique_ptr<EbxLibrary>> ui = open_ui_ebx(p_install); ui) {
		Result<FontMappingCollection> m = font_mapping_for_text_database(**ui, "localization/us_loc");
		if (m) {
			mapping = std::move(*m);
		} else {
			std::fprintf(stderr, "swf census: font mapping: %s\n", m.error().message.c_str());
		}
		if (Result<std::vector<FontMappingCollection>> a = read_all_font_mappings(**ui); a) {
			all = std::move(*a);
		}
	}
	SwfCensusOptions options;
	options.threads = threads;
	options.font_mapping = mapping ? &*mapping : nullptr;
	options.library_mappings = &all;
	Result<SwfCensus> census = run_swf_census(*lib, options);
	if (!census) {
		std::fprintf(stderr, "swf census: %s\n", census.error().message.c_str());
		return 1;
	}
	std::printf("%s", format_census_totals(*census).c_str());
	std::printf("library open %.3f s + census %.3f s\n", lib->open_seconds(), census->seconds);
	if (!tsv.empty()) {
		const std::filesystem::path path(widen(tsv));
		const PathGuard guard = p_reader.write_guard();
		if (guard.check_write_target(path) != PathGuard::Verdict::Allowed) {
			std::fprintf(stderr, "refusing to write %s\n", tsv.c_str());
			return 1;
		}
		FILE *f = nullptr;
		if (_wfopen_s(&f, to_extended_length(path).c_str(), L"wb") != 0 || f == nullptr) {
			std::fprintf(stderr, "cannot create %s\n", tsv.c_str());
			return 1;
		}
		const std::string text = format_census_tsv(*census);
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
	const SwfCensusTotals &t = census->totals;
	return (t.errors == 0 && t.parsed == t.movies && t.multi_sha1_names == 0 && t.font_names_by_rule[size_t(FontResolution::Unresolved)] == 0) ? 0 : 1;
}

int cmd_movie(const std::shared_ptr<Install> &p_install, const std::string &p_name) {
	Result<SwfLibrary> lib = open_library(p_install, 0);
	if (!lib) {
		std::fprintf(stderr, "swf movie: %s\n", lib.error().message.c_str());
		return 1;
	}
	const std::string name = movie_arg(p_name);
	const SwfMovieSource *src = lib->find(name);
	Result<std::shared_ptr<const SwfMovie>> loaded = lib->load(name);
	if (src == nullptr || !loaded) {
		std::fprintf(stderr, "swf movie: %s\n", loaded ? "no such movie" : loaded.error().message.c_str());
		return 1;
	}
	const SwfMovie &m = **loaded;
	std::printf("movie %s in %s / %s: %u holders, %u SHA-1s, %llu bytes\n", src->name.c_str(), src->superbundle.c_str(), src->bundle.c_str(), src->holders,
			src->distinct_sha1s, static_cast<unsigned long long>(src->size));
	std::printf("header %s v%u, body %zu bytes, stage %g x %g, %g fps, %u frames; %zu tags, budget %zu units, retained %zu bytes\n",
			m.compressed ? "CFX" : "GFX", m.version, m.body.size(), double(m.stage.width()), double(m.stage.height()), double(m.frame_rate), m.frame_count,
			m.tag_counts.size(), m.budget_used, m.retained_bytes);
	if (m.exporter.present) {
		std::printf("exporter version 0x%x flags 0x%x bitmap format %u prefix '%s' swf '%s' (+%u bytes)\n", m.exporter.version, m.exporter.flags,
				m.exporter.bitmap_format, m.exporter.prefix.c_str(), m.exporter.swf_name.c_str(), m.exporter.extra_bytes);
	}
	std::printf("tags:");
	for (const SwfTagCount &t : m.tag_counts) {
		const char *n = swf_tag_name(t.code);
		std::printf(" %s %u", n != nullptr ? n : std::to_string(t.code).c_str(), t.count);
	}
	std::printf("\nAS2 %llu bytes (%u DoInitAction), AS3 %llu bytes, unknown tags %u, trailing %u bytes\n", static_cast<unsigned long long>(m.action_bytes),
			m.do_init_actions, static_cast<unsigned long long>(m.abc_bytes), m.unknown_tags, m.trailing_bytes);
	for (const SwfImport &im : m.imports) {
		for (const SwfExport &s : im.symbols) {
			std::printf("import %s:%s as #%u (ImportAssets%s)\n", im.url.c_str(), s.name.c_str(), s.id, im.tag_version == 2 ? "2" : "");
		}
	}
	for (const SwfExport &e : m.exports) {
		std::printf("export #%u %s (%s)\n", e.id, e.name.c_str(), swf_character_kind_name(m.character_kind(e.id)));
	}
	for (const SwfExternalImage &e : m.external_images) {
		std::printf("external image %u: %s %ux%u format %u field_2 %u v%u (+%u bytes) -> %s\n", e.image_id, e.file_name.c_str(), e.width, e.height,
				e.bitmap_format, e.field_2, e.tag_version, e.extra_bytes, swf_atlas_texture_name(src->name, e.file_name).c_str());
	}
	for (const SwfSubImage &s : m.sub_images) {
		std::printf("sub-image #%u image %u (%u, %u, %u, %u)\n", s.id, s.image_id, s.x1, s.y1, s.x2, s.y2);
	}
	for (const SwfFontInfo &f : m.fonts) {
		std::printf("font #%u '%s' %s, %u glyphs, flags 0x%02x, language %u, trailing %u bytes\n", f.id, f.name.c_str(), swf_font_kind_name(f.kind),
				f.glyph_count, f.flags, f.language, f.trailing_bytes);
	}
	for (const SwfEditText &t : m.edit_texts) {
		std::string font = t.font_class.empty() ? std::string() : "class:" + t.font_class;
		if (font.empty() && t.has(SwfEditText::kHasFont)) {
			const SwfExport *sym = nullptr;
			if (const SwfFontInfo *f = m.find_font(t.font_id)) {
				font = f->name;
			} else if (m.find_import(t.font_id, &sym) != nullptr && sym != nullptr) {
				font = sym->name;
			}
		}
		std::printf("edit text #%u font '%s' %g px colour %02x%02x%02x%02x align %u bounds (%g, %g)-(%g, %g) flags 0x%04x var '%s' text '%s'\n", t.id,
				font.c_str(), double(t.height), t.color.r, t.color.g, t.color.b, t.color.a, t.align, double(t.bounds.x_min), double(t.bounds.y_min),
				double(t.bounds.x_max), double(t.bounds.y_max), t.flags, t.variable.c_str(), t.initial_text.substr(0, 60).c_str());
	}
	for (const SwfScalingGrid &g : m.scaling_grids) {
		std::printf("scaling grid #%u (%g, %g)-(%g, %g)\n", g.id, double(g.splitter.x_min), double(g.splitter.y_min), double(g.splitter.x_max),
				double(g.splitter.y_max));
	}
	for (const SwfSprite &s : m.sprites) {
		std::printf("sprite #%u %s: %u frames (declared %u), %zu labels, %zu events\n", s.id, std::string(m.export_name(s.id)).c_str(), s.timeline.frames,
				s.timeline.declared_frames, s.timeline.labels.size(), s.timeline.events.size());
	}
	for (const SwfDisplayEvent &e : m.root.events) {
		std::printf("root frame %u depth %u op %d char #%u '%s' %s\n", e.frame, e.depth, int(e.op), e.character, e.name.c_str(),
				e.has_matrix ? mat(e.matrix).c_str() : "");
	}
	return 0;
}

void print_region(const SwfAtlasRegion &p_r) {
	std::printf("%s\t%u\t%zu", p_r.name.c_str(), p_r.frame, p_r.pieces.size());
	for (const SwfAtlasPiece &p : p_r.pieces) {
		std::printf("\t#%u(%u,%u,%u,%u) at %s", p.sub_image, p.x1, p.y1, p.x2, p.y2, mat(p.placement).c_str());
	}
	std::printf("\n");
}

int cmd_labels(const std::shared_ptr<Install> &p_install, const std::string &p_name) {
	Result<SwfLibrary> lib = open_library(p_install, 0);
	if (!lib) {
		std::fprintf(stderr, "swf labels: %s\n", lib.error().message.c_str());
		return 1;
	}
	const std::string name = movie_arg(p_name);
	Result<std::shared_ptr<const SwfMovie>> m = lib->load(name);
	if (!m) {
		std::fprintf(stderr, "swf labels: %s\n", m.error().message.c_str());
		return 1;
	}
	WorkBudget budget(kSwfAtlasBudgetUnits);
	Result<SwfAtlasMap> map = build_atlas_map(**m, name, budget);
	if (!map) {
		std::fprintf(stderr, "swf labels: %s\n", map.error().message.c_str());
		return 1;
	}
	for (const SwfAtlasImage &im : map->images) {
		std::string fmt = "missing";
		for (const SwfExternalImage &e : (*m)->external_images) {
			if (e.image_id == im.image_id) {
				Result<res::DxTexture> t = lib->read_atlas_texture_header(name, e);
				fmt = t ? std::to_string(t->width) + "x" + std::to_string(t->height) + " format " + std::to_string(int(t->format)) : t.error().message;
			}
		}
		std::printf("image %u %s %ux%u -> %s (%s)\n", im.image_id, im.file_name.c_str(), im.width, im.height, im.texture_res.c_str(), fmt.c_str());
	}
	const SwfAtlasStats &s = map->stats;
	std::printf("labels %u: %u with art (%u one piece, %u several), %u empty; sub-images %u: %u from labels, %u with symbols, %u from frames; %u outside, "
				"%u without image; budget %zu units\n",
			s.labels, s.labels_with_art, s.labels_one_piece, s.labels_several_pieces, s.labels_empty, s.sub_images, s.sub_images_from_labels,
			s.sub_images_from_symbols, s.sub_images_from_frames, s.rects_outside_atlas, s.pieces_without_image, budget.spent());
	std::printf("# region\tframe\tpieces\t...\n");
	for (const SwfAtlasRegion &r : map->labels) {
		print_region(r);
	}
	for (const SwfAtlasRegion &r : map->symbols) {
		print_region(r);
	}
	for (const SwfAtlasRegion &r : map->frames) {
		print_region(r);
	}
	for (const SwfAtlasRegion &r : map->bare) {
		print_region(r);
	}
	return 0;
}

const char *kind_name(SwfLayoutKind p_kind) {
	switch (p_kind) {
		case SwfLayoutKind::Sprite: return "sprite";
		case SwfLayoutKind::Shape: return "shape";
		case SwfLayoutKind::EditText: return "text";
		case SwfLayoutKind::StaticText: return "static";
		case SwfLayoutKind::Import: return "import";
		case SwfLayoutKind::Other: return "other";
	}
	return "?";
}

void print_node(const SwfLayoutNode &p_n, int p_indent) {
	std::printf("%*s%s %s %s:%s stage %s", p_indent * 2, "", p_n.instance.empty() ? "-" : p_n.instance.c_str(), kind_name(p_n.kind), p_n.movie.c_str(),
			p_n.symbol.c_str(), mat(p_n.stage).c_str());
	if (p_n.kind == SwfLayoutKind::EditText) {
		std::printf(" font '%s' %g px colour %02x%02x%02x%02x align %u text '%s'", p_n.text.font.c_str(), double(p_n.text.size), p_n.text.color.r,
				p_n.text.color.g, p_n.text.color.b, p_n.text.color.a, p_n.text.align, p_n.text.initial_text.substr(0, 40).c_str());
	}
	if (p_n.has_scaling_grid) {
		std::printf(" grid (%g, %g)-(%g, %g)", double(p_n.scaling_grid.x_min), double(p_n.scaling_grid.y_min), double(p_n.scaling_grid.x_max),
				double(p_n.scaling_grid.y_max));
	}
	std::printf("\n");
	for (const SwfLayoutNode &c : p_n.children) {
		print_node(c, p_indent + 1);
	}
}

int cmd_layout(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		return 2;
	}
	SwfLayoutOptions options;
	for (size_t i = 1; i < p_args.size(); ++i) {
		std::string v;
		if (flag_value(p_args[i], "--depth", v)) {
			options.max_depth = parse_threads(v);
		} else {
			return 2;
		}
	}
	Result<SwfLibrary> lib = open_library(p_install, 0);
	if (!lib) {
		std::fprintf(stderr, "swf layout: %s\n", lib.error().message.c_str());
		return 1;
	}
	WorkBudget budget(kSwfLayoutBudgetUnits);
	Result<SwfLayoutNode> tree = read_layout(*lib, movie_arg(p_args[0]), options, budget);
	if (!tree) {
		std::fprintf(stderr, "swf layout: %s\n", tree.error().message.c_str());
		return 1;
	}
	print_node(*tree, 0);
	std::fprintf(stderr, "budget %zu units\n", budget.spent());
	return 0;
}

int cmd_hud(const std::shared_ptr<Install> &p_install) {
	Result<SwfLibrary> lib = open_library(p_install, 0);
	if (!lib) {
		std::fprintf(stderr, "swf hud: %s\n", lib.error().message.c_str());
		return 1;
	}
	size_t total = 0;
	int rc = 0;
	std::printf("screen\tstage\tscreen_symbol\tdepth\tinstance\twidget\twidget_movie\tx\ty\tscale_x\tscale_y\tskew0\tskew1\tanchor\tanchor_dx\tanchor_dy\n");
	for (const std::string_view screen : hud_screen_movies()) {
		WorkBudget budget(kSwfLayoutBudgetUnits);
		Result<std::vector<HudPlacement>> rows = read_hud_placements(*lib, screen, budget);
		if (!rows) {
			std::fprintf(stderr, "swf hud %.*s: %s\n", int(screen.size()), screen.data(), rows.error().message.c_str());
			rc = 1;
			continue;
		}
		for (const HudPlacement &p : *rows) {
			std::printf("%.*s\t%gx%g\t%s\t%u\t%s\t%s\t%s\t%g\t%g\t%g\t%g\t%g\t%g\t%s\t%g\t%g\n", int(screen.size()), screen.data(), double(p.stage.width()),
					double(p.stage.height()), p.screen_symbol.c_str(), p.depth, p.instance.c_str(), p.widget.c_str(), p.widget_movie.c_str(),
					double(p.matrix.tx), double(p.matrix.ty), double(p.matrix.a), double(p.matrix.d), double(p.matrix.b), double(p.matrix.c),
					hud_anchor_name(p.anchor), double(p.anchor_dx), double(p.anchor_dy));
		}
		std::fprintf(stderr, "%.*s: %zu placements\n", int(screen.size()), screen.data(), rows->size());
		total += rows->size();
	}
	std::fprintf(stderr, "hud: %zu placements over %zu screens\n", total, hud_screen_movies().size());
	return rc;
}

int cmd_fonts(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	std::string text_db = "localization/us_loc";
	std::filesystem::path root;
	bool no_cache = false;
	for (const std::string &a : p_args) {
		std::string v;
		if (flag_value(a, "--text-db", v)) {
			text_db = v;
		} else if (flag_value(a, "--cache", v)) {
			root = std::filesystem::path(widen(v));
		} else if (a == "--no-cache") {
			no_cache = true;
		} else {
			return 2;
		}
	}
	Result<SwfLibrary> lib = open_library(p_install, 0);
	if (!lib) {
		std::fprintf(stderr, "swf fonts: %s\n", lib.error().message.c_str());
		return 1;
	}
	Result<std::unique_ptr<EbxLibrary>> ui = open_ui_ebx(p_install);
	if (!ui) {
		std::fprintf(stderr, "swf fonts: %s\n", ui.error().message.c_str());
		return 1;
	}
	Cache cache;
	if (!no_cache) {
		if (root.empty()) {
			root = default_cache_root();
		}
		std::error_code ec;
		root = std::filesystem::absolute(root, ec);
		CacheOptions co;
		co.root = root;
		co.guard = p_reader.write_guard(); // the install is refused
		std::string detail;
		const CacheStatus st = cache.open(std::move(co), &detail);
		if (st != CacheStatus::Ok) {
			std::fprintf(stderr, "swf fonts: cache %s %s; converting without it\n", cache_status_name(st), detail.c_str());
		}
	}
	UiFontLibraryOptions options;
	options.text_database = text_db;
	options.cache = cache.is_open() ? &cache : nullptr;
	Result<UiFontLibrary> fonts = UiFontLibrary::load(*lib, **ui, options);
	if (!fonts) {
		std::fprintf(stderr, "swf fonts: %s\n", fonts.error().message.c_str());
		return 1;
	}
	const FontMappingCollection &m = fonts->mapping();
	std::printf("mapping %s (%s), text database %s, font library %s\n", m.partition.c_str(), m.name.c_str(), m.text_database.c_str(), m.font_library.c_str());
	for (const ConvertedFont &f : fonts->fonts()) {
		std::string names;
		for (const std::string &n : f.scaleform_names) {
			names += (names.empty() ? "" : " ") + n;
		}
		std::printf("font '%s' -> %s: %u source glyphs, %u distinct codes; TrueType %zu bytes, %u glyphs, %u cmap codes, %u contours, %u points, %u reversed, "
					"%u kerning pairs, max %u points / %u contours; SHA-256 %s; cache %s%s\n",
				f.font_long_name.c_str(), names.c_str(), f.source_glyphs, f.distinct_codes, f.ttf.size(), f.stats.glyphs, f.stats.cmap_codes, f.stats.contours,
				f.stats.points, f.stats.contours_reversed, f.stats.kerning_pairs, f.stats.max_points, f.stats.max_contours, sha256_of(f.ttf).hex().c_str(),
				cache_outcome_name(f.cache.outcome), f.cache.dropped_corrupt ? " (dropped corrupt)" : "");
		uint32_t latin1 = 0;
		bool e_acute = false;
		std::string beyond;
		for (const uint16_t c : f.codes) {
			latin1 += (c >= 0x20 && c <= 0xFF) ? 1 : 0;
			e_acute = e_acute || c == 0x00C9;
		}
		std::printf("  coverage: %u codes in U+0020..U+00FF, U+00C9 %s, first U+%04X, last U+%04X\n", latin1, e_acute ? "present" : "absent",
				f.codes.empty() ? 0 : f.codes.front(), f.codes.empty() ? 0 : f.codes.back());
	}
	std::printf("load %.3f s\n", fonts->load_seconds());
	// The name-resolution table over every movie (G6).
	SwfCensusOptions co;
	co.check_atlas_textures = false;
	co.build_atlas_maps = false;
	co.font_mapping = &m;
	Result<std::vector<FontMappingCollection>> all = read_all_font_mappings(**ui);
	co.library_mappings = all ? &*all : nullptr;
	Result<SwfCensus> census = run_swf_census(*lib, co);
	if (!census) {
		std::fprintf(stderr, "swf fonts: %s\n", census.error().message.c_str());
		return 1;
	}
	const SwfCensusTotals &t = census->totals;
	for (const auto &[name, how] : t.font_names) {
		const ResolvedFont r = resolve_font_name(m, name);
		std::printf("name %-22s %-10s %s%s%s\n", name.c_str(), font_resolution_name(how), r.font_long_name.c_str(), r.via.empty() ? "" : " via ",
				r.via.c_str());
	}
	std::printf("names: Exact %u, CaseFolded %u, Family %u, Device %u, Unresolved %u\n", t.font_names_by_rule[0], t.font_names_by_rule[1],
			t.font_names_by_rule[2], t.font_names_by_rule[3], t.font_names_by_rule[4]);
	std::printf("G6 family-inferred:");
	for (const std::string &n : t.family_names) {
		std::printf(" %s", n.c_str());
	}
	std::printf("\n");
	cache.close();
	return t.font_names_by_rule[size_t(FontResolution::Unresolved)] == 0 ? 0 : 1;
}

} // namespace

const char *swf_usage() {
	return "  swf census [--tsv <out.tsv>] [--threads=N]\n"
		   "  swf movie <name>\n"
		   "  swf labels <name>\n"
		   "  swf layout <name> [--depth=N]\n"
		   "  swf hud\n"
		   "  swf fonts [--text-db=<partition>] [--cache=<dir> | --no-cache]\n";
}

int cmd_swf(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		return 2;
	}
	const std::string &sub = p_args[0];
	const std::vector<std::string> rest(p_args.begin() + 1, p_args.end());
	if (sub == "census") {
		return cmd_census(p_reader, p_install, rest);
	}
	if (sub == "movie" && rest.size() == 1) {
		return cmd_movie(p_install, rest[0]);
	}
	if (sub == "labels" && rest.size() == 1) {
		return cmd_labels(p_install, rest[0]);
	}
	if (sub == "layout" && !rest.empty()) {
		return cmd_layout(p_install, rest);
	}
	if (sub == "hud" && rest.empty()) {
		return cmd_hud(p_install);
	}
	if (sub == "fonts") {
		return cmd_fonts(p_reader, p_install, rest);
	}
	return 2;
}

} // namespace bf3dump
