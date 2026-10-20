#include "bf3/core/ui/swf_census.h"

#include "bf3/core/thread_pool.h"
#include "bf3/core/ui/swf_atlas.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace bf3::core::ui {

namespace {

struct Lane {
	SwfMovieCensus row;
	std::vector<std::string> exports;
	std::vector<std::pair<std::string, std::vector<std::string>>> imports;
	std::set<std::string> font_names;
	size_t atlas_budget = 0;
};

void count_labels_past(const SwfTimeline &p_tl, uint32_t &r_count) {
	for (const SwfFrameLabel &l : p_tl.labels) {
		r_count += l.frame >= p_tl.frames ? 1 : 0;
	}
}

void append_error(std::string &r_error, const std::string &p_what) {
	if (!r_error.empty()) {
		r_error += "; ";
	}
	r_error += p_what;
}

Lane census_one(const SwfLibrary &p_lib, const SwfMovieSource &p_source, const SwfCensusOptions &p_options) {
	Lane lane;
	SwfMovieCensus &c = lane.row;
	c.name = p_source.name;
	c.superbundle = p_source.superbundle;
	c.holders = p_source.holders;
	c.distinct_sha1s = p_source.distinct_sha1s;
	const auto start = std::chrono::steady_clock::now();
	Result<std::vector<uint8_t>> payload = p_lib.read_payload(p_source.name);
	if (!payload) {
		c.error = payload.error().message;
		return lane;
	}
	c.payload_bytes = payload->size();
	Result<SwfMovie> parsed = SwfMovie::parse(*payload);
	c.parse_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	if (!parsed) {
		c.error = parsed.error().message;
		return lane;
	}
	const SwfMovie &m = *parsed;
	c.compressed = m.compressed;
	c.version = m.version;
	c.body_bytes = m.body.size();
	c.stage = m.stage;
	c.frame_rate = m.frame_rate;
	c.frame_count = m.frame_count;
	c.tags = m.tag_counts;
	c.sprites = uint32_t(m.sprites.size());
	c.labels = uint32_t(m.root.labels.size());
	count_labels_past(m.root, c.labels_past_last_frame);
	auto placements = [](const SwfTimeline &p_tl) {
		uint32_t n = 0;
		for (const SwfDisplayEvent &e : p_tl.events) {
			n += e.op != SwfDisplayOp::Remove ? 1 : 0;
		}
		return n;
	};
	c.placements = placements(m.root);
	for (const SwfSprite &s : m.sprites) {
		c.labels += uint32_t(s.timeline.labels.size());
		c.placements += placements(s.timeline);
		count_labels_past(s.timeline, c.labels_past_last_frame);
	}
	c.edit_texts = uint32_t(m.edit_texts.size());
	c.shapes = uint32_t(m.shapes.size());
	c.scaling_grids = uint32_t(m.scaling_grids.size());
	c.exports = uint32_t(m.exports.size());
	c.external_images = m.external_images;
	c.sub_images = m.sub_images;
	c.fonts = m.fonts;
	c.action_bytes = m.action_bytes;
	c.abc_bytes = m.abc_bytes;
	c.unknown_tags = m.unknown_tags;
	c.budget_used = m.budget_used;
	c.retained_bytes = m.retained_bytes;
	for (const SwfExport &e : m.exports) {
		lane.exports.push_back(e.name);
	}
	for (const SwfImport &im : m.imports) {
		std::vector<std::string> symbols;
		for (const SwfExport &s : im.symbols) {
			symbols.push_back(s.name);
			if (is_font_library_url(im.url)) {
				lane.font_names.insert(s.name);
			}
		}
		lane.imports.push_back({ im.url, std::move(symbols) });
	}

	for (const SwfFontInfo &f : m.fonts) {
		if (f.kind != SwfFontKind::Outlines) {
			lane.font_names.insert(f.name);
		}
	}
	for (const SwfEditText &t : m.edit_texts) {
		if (!t.font_class.empty()) {
			lane.font_names.insert(t.font_class);
		} else if (t.has(SwfEditText::kHasFont)) {
			const SwfExport *sym = nullptr;
			if (const SwfFontInfo *f = m.find_font(t.font_id)) {
				lane.font_names.insert(f->name);
			} else if (m.find_import(t.font_id, &sym) != nullptr && sym != nullptr) {
				lane.font_names.insert(sym->name);
			}
		}
	}

	for (const SwfExternalImage &im : m.external_images) {
		if (!p_options.check_atlas_textures) {
			break;
		}
		Result<res::DxTexture> t = p_lib.read_atlas_texture_header(p_source.name, im);
		if (!t || t->width != im.width || t->height != im.height) {
			c.atlas_textures_missing++;
			append_error(c.error, t ? "atlas " + im.file_name + " is " + std::to_string(t->width) + "x" + std::to_string(t->height) : t.error().message);
		}
	}
	for (const SwfSubImage &s : m.sub_images) {
		const SwfExternalImage *im = m.find_external_image(s.image_id);
		if (im == nullptr || s.x2 > im->width || s.y2 > im->height) {
			c.sub_images_outside++;
		}
	}
	if (c.sub_images_outside != 0) {
		append_error(c.error, std::to_string(c.sub_images_outside) + " sub-images outside their atlas");
	}
	if (p_options.build_atlas_maps && !m.external_images.empty()) {
		WorkBudget budget(kSwfAtlasBudgetUnits);
		Result<SwfAtlasMap> map = build_atlas_map(m, p_source.name, budget);
		lane.atlas_budget = budget.spent();
		if (!map) {
			append_error(c.error, map.error().message);
		}
	}
	return lane;
}

std::string tsv_escape(const std::string &p_text) {
	std::string out;
	for (const char ch : p_text) {
		if (ch == '\t' || ch == '\n' || ch == '\r') {
			out.push_back(' ');
		} else {
			out.push_back(ch);
		}
	}
	return out;
}

}

Result<SwfCensus> run_swf_census(const SwfLibrary &p_movies, const SwfCensusOptions &p_options) {
	const auto start = std::chrono::steady_clock::now();
	const std::vector<SwfMovieSource> &sources = p_movies.movies();
	std::vector<Lane> lanes =
			parallel_map<Lane>(sources.size(), [&](size_t i) { return census_one(p_movies, sources[i], p_options); }, p_options.threads);

	SwfCensus census;
	SwfCensusTotals &t = census.totals;
	std::unordered_map<std::string, std::set<std::string>> exports;
	for (size_t i = 0; i < lanes.size(); ++i) {
		exports[sources[i].name].insert(lanes[i].exports.begin(), lanes[i].exports.end());
	}
	std::map<uint16_t, uint32_t> tags;
	std::set<std::string> font_names;
	for (size_t i = 0; i < lanes.size(); ++i) {
		Lane &lane = lanes[i];
		SwfMovieCensus &c = lane.row;
		for (const auto &[url, symbols] : lane.imports) {
			if (is_font_library_url(url)) {
				continue;
			}
			const std::string target = swf_movie_name_for_url(url);
			const auto it = exports.find(target);
			for (const std::string &s : symbols) {
				c.imported_symbols++;
				if (it == exports.end() || it->second.count(s) == 0) {
					c.missing_imports++;
					c.missing_import_symbols.push_back(url + ":" + s);
				}
			}
		}
		const bool is_library = c.name.rfind("ui/static/", 0) == 0 && c.name.size() > 8 && c.name.compare(c.name.size() - 8, 8, "_fontlib") == 0;
		const bool own_library = p_options.font_mapping != nullptr && c.name == p_options.font_mapping->font_library;
		if (!is_library || own_library) {
			for (const std::string &n : lane.font_names) {
				font_names.insert(std::string(font_name_count_key(n)));
			}
		} else if (p_options.library_mappings != nullptr) {
			const FontMappingCollection *own = nullptr;
			for (const FontMappingCollection &fm : *p_options.library_mappings) {
				own = fm.font_library == c.name ? &fm : own;
			}
			for (const std::string &n : lane.font_names) {
				t.library_font_names++;
				const bool exact = own != nullptr && resolve_font_name(*own, n).how == FontResolution::Exact;
				t.library_font_names_exact += exact ? 1 : 0;
				if (!exact) {
					t.library_font_names_not_exact.push_back(c.name + ": " + n);
				}
			}
		}
		t.movies++;
		const bool ok = c.body_bytes != 0;
		t.parsed += ok ? 1 : 0;
		t.errors += c.error.empty() ? 0 : 1;
		if (ok) {
			(c.compressed ? t.cfx : t.gfx)++;
		}
		for (const SwfTagCount &tc : c.tags) {
			tags[tc.code] += tc.count;
		}
		t.action_bytes += c.action_bytes;
		t.abc_bytes += c.abc_bytes;
		t.payload_bytes += c.payload_bytes;
		t.body_bytes += c.body_bytes;
		t.labels += c.labels;
		t.sub_images += uint32_t(c.sub_images.size());
		t.external_images += uint32_t(c.external_images.size());
		t.imported_symbols += c.imported_symbols;
		t.missing_imports += c.missing_imports;
		t.atlas_textures_missing += c.atlas_textures_missing;
		for (const SwfFontInfo &f : c.fonts) {
			(f.kind == SwfFontKind::Outlines ? t.fonts_outlines : f.kind == SwfFontKind::NoGlyphs ? t.fonts_no_glyphs : t.fonts_codes_only)++;
		}
		t.multi_sha1_names += c.distinct_sha1s > 1 ? 1 : 0;
		t.labels_past_last_frame += c.labels_past_last_frame;
		t.max_budget_used = std::max(t.max_budget_used, c.budget_used);
		t.max_retained_bytes = std::max(t.max_retained_bytes, c.retained_bytes);
		t.sum_retained_bytes += c.retained_bytes;
		t.max_atlas_budget_used = std::max(t.max_atlas_budget_used, lane.atlas_budget);
		census.movies.push_back(std::move(c));
	}
	for (const auto &[code, count] : tags) {
		t.tags.push_back({ code, count });
	}
	if (p_options.font_mapping != nullptr) {
		for (const std::string &n : font_names) {
			const ResolvedFont r = resolve_font_name(*p_options.font_mapping, n);
			t.font_names_by_rule[size_t(r.how)]++;
			t.font_names.push_back({ n, r.how });
			if (r.how == FontResolution::Family) {
				t.family_names.push_back(n);
			}
		}
	}
	census.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return census;
}

std::string format_census_tsv(const SwfCensus &p_census) {
	std::string out = "name\tsuperbundle\tholders\tsha1s\tsignature\tversion\tpayload_bytes\tbody_bytes\tstage\tframes\tsprites\tlabels\tplacements\t"
					  "edit_texts\tshapes\tscaling_grids\texports\timported_symbols\tmissing_imports\texternal_images\tsub_images\tfonts\tas2_bytes\t"
					  "budget_units\tretained_bytes\tparse_ms\terror\n";
	char buf[512];
	for (const SwfMovieCensus &c : p_census.movies) {
		std::snprintf(buf, sizeof(buf), "%s\t%s\t%u\t%u\t%s\t%u\t%llu\t%llu\t%gx%g\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%zu\t%zu\t%zu\t%llu\t%zu\t%zu\t%.3f\t",
				c.name.c_str(), c.superbundle.c_str(), c.holders, c.distinct_sha1s, c.body_bytes == 0 ? "-" : (c.compressed ? "CFX" : "GFX"), c.version,
				static_cast<unsigned long long>(c.payload_bytes), static_cast<unsigned long long>(c.body_bytes), double(c.stage.width()), double(c.stage.height()),
				c.frame_count, c.sprites, c.labels, c.placements, c.edit_texts, c.shapes, c.scaling_grids, c.exports, c.imported_symbols, c.missing_imports,
				c.external_images.size(), c.sub_images.size(), c.fonts.size(), static_cast<unsigned long long>(c.action_bytes), c.budget_used, c.retained_bytes,
				c.parse_ms);
		out += buf;
		out += tsv_escape(c.error);
		out += '\n';
	}
	return out;
}

std::string format_census_totals(const SwfCensus &p_census) {
	const SwfCensusTotals &t = p_census.totals;
	std::string out;
	char buf[512];
	std::snprintf(buf, sizeof(buf), "movies %u, parsed %u, errors %u (CFX %u, GFX %u); census %.3f s\n", t.movies, t.parsed, t.errors, t.cfx, t.gfx,
			p_census.seconds);
	out += buf;
	std::snprintf(buf, sizeof(buf), "payload %llu bytes, body %llu bytes; AS2 %llu bytes, AS3 %llu bytes\n", static_cast<unsigned long long>(t.payload_bytes),
			static_cast<unsigned long long>(t.body_bytes), static_cast<unsigned long long>(t.action_bytes), static_cast<unsigned long long>(t.abc_bytes));
	out += buf;
	std::snprintf(buf, sizeof(buf),
			"labels %u (%u past their timeline's last ShowFrame), external images %u (%u atlas textures missing), sub-images %u, imported symbols %u (%u "
			"missing)\n",
			t.labels, t.labels_past_last_frame, t.external_images, t.atlas_textures_missing, t.sub_images, t.imported_symbols, t.missing_imports);
	out += buf;
	std::snprintf(buf, sizeof(buf), "fonts: %u outlines, %u no glyphs, %u codes only; names with several SHA-1s %u\n", t.fonts_outlines, t.fonts_no_glyphs,
			t.fonts_codes_only, t.multi_sha1_names);
	out += buf;
	std::snprintf(buf, sizeof(buf), "parse budget max %zu units; atlas budget max %zu units; retained bytes max %zu, sum %llu\n", t.max_budget_used,
			t.max_atlas_budget_used, t.max_retained_bytes, static_cast<unsigned long long>(t.sum_retained_bytes));
	out += buf;
	out += "tags:";
	for (const SwfTagCount &tc : t.tags) {
		const char *name = swf_tag_name(tc.code);
		std::snprintf(buf, sizeof(buf), " %s %u", name != nullptr ? name : std::to_string(tc.code).c_str(), tc.count);
		out += buf;
	}
	out += '\n';
	for (const SwfMovieCensus &c : p_census.movies) {
		if (c.distinct_sha1s > 1) {
			out += "several SHA-1s: " + c.name + " (" + std::to_string(c.distinct_sha1s) + ")\n";
		}
		for (const std::string &m : c.missing_import_symbols) {
			out += "missing import: " + c.name + " imports " + m + "\n";
		}
		if (!c.error.empty()) {
			out += "error: " + c.name + ": " + c.error + "\n";
		}
	}
	if (!t.font_names.empty()) {
		std::snprintf(buf, sizeof(buf), "font names: Exact %u, CaseFolded %u, Family %u, Device %u, Unresolved %u\n", t.font_names_by_rule[0],
				t.font_names_by_rule[1], t.font_names_by_rule[2], t.font_names_by_rule[3], t.font_names_by_rule[4]);
		out += buf;
		for (const auto &[name, how] : t.font_names) {
			out += "  font name " + name + " -> " + font_resolution_name(how) + "\n";
		}
		std::snprintf(buf, sizeof(buf), "other font libraries: %u names, %u Exact under their own collection\n", t.library_font_names,
				t.library_font_names_exact);
		out += buf;
		for (const std::string &n : t.library_font_names_not_exact) {
			out += "  not exact: " + n + "\n";
		}
		out += "G6 family-inferred:";
		for (const std::string &n : t.family_names) {
			out += " " + n;
		}
		out += '\n';
	}
	return out;
}

}
