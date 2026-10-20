#include "bf3/core/ui/ui_fonts.h"

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace bf3::core::ui {

namespace {

constexpr std::string_view kMappingType = "UIFontMappingCollection";
constexpr std::string_view kMapSuffix = "_fontmapcollection";
constexpr std::string_view kLibSuffix = "_fontlib";

constexpr std::string_view kLanguages[] = { "cs", "de", "en", "es", "fr", "it", "ja", "ko", "pl", "ru", "zh" };

std::optional<EbxInstance> mapping_instance(const EbxPartition &p_partition) {
	if (p_partition.primary().is_a(kMappingType)) {
		return p_partition.primary();
	}
	const std::vector<EbxInstance> all = p_partition.instances_of(kMappingType);
	if (all.empty()) {
		return std::nullopt;
	}
	return all.front();
}

std::string fold(std::string_view p_name) {
	return to_lower_ascii(font_name_count_key(p_name));
}

bool starts_with(std::string_view p_text, std::string_view p_prefix) {
	return p_text.substr(0, p_prefix.size()) == p_prefix;
}

}

const char *font_resolution_name(FontResolution p_how) {
	switch (p_how) {
		case FontResolution::Exact: return "Exact";
		case FontResolution::CaseFolded: return "CaseFolded";
		case FontResolution::Family: return "Family";
		case FontResolution::Device: return "Device";
		case FontResolution::Unresolved: return "Unresolved";
	}
	return "?";
}

Result<FontMappingCollection> read_font_mapping(const EbxResolver &p_ebx, std::string_view p_partition) {
	const std::string key = to_lower_ascii(p_partition);
	const EbxPartition *partition = p_ebx.partition_by_name(key);
	if (partition == nullptr) {
		return make_error(key + ": no such EBX partition");
	}
	const std::optional<EbxInstance> inst = mapping_instance(*partition);
	if (!inst) {
		return make_error(key + ": no UIFontMappingCollection instance");
	}
	FontMappingCollection c;
	c.partition = key;
	if (const std::optional<EbxValue> name = inst->get("Name"); name && name->as_str()) {
		c.name = std::string(*name->as_str());
	}

	if (const std::optional<EbxValue> db = inst->get("TextDatabase"); db) {
		std::optional<EbxRef> ref = db->as_ref();
		if (!ref && db->as_array() && !(*db->as_array()).empty()) {
			ref = (*db->as_array()).get(0)->as_ref();
		}
		if (ref && !ref->is_null()) {
			const Guid owner = ref->kind == EbxRef::Kind::External ? ref->partition : partition->guid();
			if (const std::optional<std::string_view> n = p_ebx.name_of(owner)) {
				c.text_database = std::string(*n);
			}
		}
	}
	const std::optional<EbxValue> fonts = inst->get("Fonts");
	if (!fonts || !fonts->as_array()) {
		return make_error(key + ": UIFontMappingCollection has no Fonts array");
	}
	for (const EbxValue f : *fonts->as_array()) {
		std::optional<EbxStruct> s = f.as_struct();
		std::optional<EbxInstance> i;
		if (!s) {
			if (const std::optional<EbxRef> r = f.as_ref()) {
				i = p_ebx.resolve(*partition, *r);
			}
		}
		const std::optional<EbxValue> long_name = s ? s->get("FontLongName") : (i ? i->get("FontLongName") : std::nullopt);
		const std::optional<EbxValue> names = s ? s->get("ScaleformFontName") : (i ? i->get("ScaleformFontName") : std::nullopt);
		if (!long_name || !long_name->as_str()) {
			continue;
		}
		FontMapping m;
		m.font_long_name = std::string(*long_name->as_str());
		if (names && names->as_array()) {
			for (const EbxValue n : *names->as_array()) {
				if (n.as_str()) {
					m.scaleform_names.emplace_back(*n.as_str());
				}
			}
		}
		c.fonts.push_back(std::move(m));
	}
	if (key.size() > kMapSuffix.size() && key.compare(key.size() - kMapSuffix.size(), kMapSuffix.size(), kMapSuffix) == 0) {
		c.font_library = key.substr(0, key.size() - kMapSuffix.size()) + std::string(kLibSuffix);
	}
	return c;
}

Result<std::vector<std::string>> font_mapping_partitions(const EbxResolver &p_ebx) {
	std::set<std::string> candidates;
	if (const EbxLibrary *lib = dynamic_cast<const EbxLibrary *>(&p_ebx)) {
		for (size_t i = 0; i < lib->superbundle_count(); ++i) {
			for (const std::string &name : lib->mount(i).ebx_names()) {
				const std::string l = to_lower_ascii(name);
				if (l.find(kMapSuffix) != std::string::npos) {
					candidates.insert(l);
				}
			}
		}
	} else if (const EbxDb *db = dynamic_cast<const EbxDb *>(&p_ebx)) {
		for (size_t i = 0; i < db->size(); ++i) {
			if (db->name_at(i).find(kMapSuffix) != std::string::npos) {
				candidates.insert(db->name_at(i));
			}
		}
	} else {
		for (const std::string_view lang : kLanguages) {
			const std::string l(lang);
			candidates.insert("ui/static/fontcollection_" + l + "_fontmapcollection");
			candidates.insert("ui/static/ondemandfontcollection/eulafontcollection_" + l + "_fontmapcollection");
			candidates.insert("ui/static/ondemandfontcollection/storefontcollection_" + l + "_fontmapcollection");
		}
	}
	std::vector<std::string> out;
	for (const std::string &name : candidates) {
		const EbxPartition *p = p_ebx.partition_by_name(name);
		if (p != nullptr && mapping_instance(*p)) {
			out.push_back(name);
		}
	}
	return out;
}

Result<std::vector<FontMappingCollection>> read_all_font_mappings(const EbxResolver &p_ebx) {
	Result<std::vector<std::string>> names = font_mapping_partitions(p_ebx);
	if (!names) {
		return names.error();
	}
	std::vector<FontMappingCollection> out;
	for (const std::string &name : *names) {
		Result<FontMappingCollection> c = read_font_mapping(p_ebx, name);
		if (!c) {
			return c.error();
		}
		out.push_back(std::move(*c));
	}
	return out;
}

Result<FontMappingCollection> font_mapping_for_text_database(const EbxResolver &p_ebx, std::string_view p_text_database) {
	Result<std::vector<std::string>> names = font_mapping_partitions(p_ebx);
	if (!names) {
		return names.error();
	}
	const std::string want = to_lower_ascii(p_text_database);
	for (const std::string &name : *names) {
		Result<FontMappingCollection> c = read_font_mapping(p_ebx, name);
		if (!c) {
			return c.error();
		}
		if (to_lower_ascii(c->text_database) == want) {
			return c;
		}
	}
	return make_error("no UIFontMappingCollection names the text database " + want + " (" + std::to_string(names->size()) + " collections)");
}

ResolvedFont resolve_font_name(const FontMappingCollection &p_collection, std::string_view p_used_name) {
	ResolvedFont r;

	for (const FontMapping &m : p_collection.fonts) {
		for (const std::string &s : m.scaleform_names) {
			if (s == p_used_name) {
				return { FontResolution::Exact, m.font_long_name, s };
			}
		}
		if (m.font_long_name == p_used_name) {
			return { FontResolution::Exact, m.font_long_name, m.scaleform_names.empty() ? m.font_long_name : m.scaleform_names.front() };
		}
	}

	const std::string folded = fold(p_used_name);
	for (const FontMapping &m : p_collection.fonts) {
		for (const std::string &s : m.scaleform_names) {
			if (to_lower_ascii(s) == folded) {
				return { FontResolution::CaseFolded, m.font_long_name, s };
			}
		}
		if (to_lower_ascii(m.font_long_name) == folded) {
			return { FontResolution::CaseFolded, m.font_long_name, m.scaleform_names.empty() ? m.font_long_name : m.scaleform_names.front() };
		}
	}
	if (!starts_with(folded, "$")) {
		r.how = FontResolution::Device;
		return r;
	}

	std::string target;
	if (starts_with(folded, "$hud")) {
		target = "$hud_medium";
	} else if (starts_with(folded, "$menu")) {
		target = "$menu_medium";
	} else if (starts_with(folded, "$font_title")) {
		target = "$menu_bold";
	}
	if (!target.empty()) {
		for (const FontMapping &m : p_collection.fonts) {
			for (const std::string &s : m.scaleform_names) {
				if (to_lower_ascii(s) == target) {
					return { FontResolution::Family, m.font_long_name, s };
				}
			}
		}
	}
	r.how = FontResolution::Unresolved;
	return r;
}

Result<ConvertedFont> convert_library_font(const SwfLibrary &p_movies, std::string_view p_font_library, std::string_view p_font_long_name, Cache *p_cache,
		WorkBudget &r_budget) {
	const std::string where = "ui font " + std::string(p_font_long_name);
	const SwfMovieSource *source = p_movies.find(p_font_library);
	if (source == nullptr) {
		return make_error(where + ": no font library " + std::string(p_font_library));
	}
	Result<std::shared_ptr<const SwfMovie>> movie = p_movies.load(p_font_library);
	if (!movie) {
		return error_at(where, movie.error());
	}
	const SwfFontInfo *info = nullptr;
	for (const SwfFontInfo &f : (*movie)->fonts) {
		if (f.name == p_font_long_name && f.kind == SwfFontKind::Outlines) {
			info = &f;
			break;
		}
	}
	if (info == nullptr) {
		return make_error(where + ": " + std::string(p_font_library) + " has no DefineFont3 with outlines of that name");
	}
	CacheKeyInputs in;
	in.kind = std::string(kUiFontCacheKind);
	in.sources.push_back(source_digest(source->sha1));
	in.converter_version = kUiFontConverterVersion;
	in.engine = std::string(cache_engine_id());
	in.format_version = kTrueTypeWriterVersion;
	in.params = "font=" + std::string(p_font_long_name) + ";upem=" + std::to_string(kTrueTypeUnitsPerEm);
	const std::optional<CacheKey> key = make_cache_key(in);
	if (!key) {
		return make_error(where + ": no cache key");
	}
	ConvertedFont out;
	out.font_long_name = std::string(p_font_long_name);
	out.source_glyphs = info->glyph_count;
	auto accept = [&](std::span<const uint8_t> p_bytes) -> Status {
		WorkBudget budget(kTrueTypeReadBudgetUnits);
		Result<TrueTypeFont> font = read_truetype(p_bytes, budget);
		if (!font) {
			return font.error();
		}
		if (font->family != p_font_long_name) {
			return make_error("cached font family is " + font->family);
		}
		out.ttf.assign(p_bytes.begin(), p_bytes.end());
		out.codes.clear();
		for (const auto &[code, glyph] : font->cmap) {
			out.codes.push_back(code);
		}
		out.stats = TrueTypeStats{};
		out.stats.glyphs = uint32_t(font->glyphs.size());
		out.stats.cmap_codes = uint32_t(font->cmap.size());
		out.stats.bytes = p_bytes.size();
		return Status::success();
	};
	auto produce = [&]() -> Result<std::vector<uint8_t>> {
		Result<SwfFontOutlines> outlines = load_font_outlines(**movie, *info, r_budget);
		if (!outlines) {
			return outlines.error();
		}
		TrueTypeOptions options;
		TrueTypeStats stats;
		Result<std::vector<uint8_t>> ttf = write_truetype(*outlines, options, r_budget, &stats);
		if (!ttf) {
			return ttf.error();
		}
		Result<TrueTypeFont> back = read_truetype(*ttf, r_budget);
		if (!back) {
			return error_at("read back", back.error());
		}
		out.ttf = *ttf;
		out.stats = stats;
		out.codes.clear();
		for (const auto &[code, glyph] : back->cmap) {
			out.codes.push_back(code);
		}
		return ttf;
	};
	Result<CachedItem> item = cached_convert(p_cache, *key, accept, produce);
	if (!item) {
		return error_at(where, item.error());
	}
	out.cache = *item;
	out.distinct_codes = uint16_t(std::min<size_t>(out.codes.size(), 65535));
	return out;
}

Result<UiFontLibrary> UiFontLibrary::load(const SwfLibrary &p_movies, const EbxResolver &p_ui_ebx, const UiFontLibraryOptions &p_options) {
	const auto start = std::chrono::steady_clock::now();
	Result<FontMappingCollection> mapping = font_mapping_for_text_database(p_ui_ebx, p_options.text_database);
	if (!mapping) {
		return mapping.error();
	}
	if (mapping->font_library.empty()) {
		return make_error(mapping->partition + ": no font library name");
	}
	UiFontLibrary lib;
	lib.mapping_ = std::move(*mapping);
	for (const FontMapping &m : lib.mapping_.fonts) {
		WorkBudget budget(kUiFontConvertBudgetUnits);
		Result<ConvertedFont> f = convert_library_font(p_movies, lib.mapping_.font_library, m.font_long_name, p_options.cache, budget);
		if (!f) {
			return f.error();
		}
		f->scaleform_names = m.scaleform_names;
		lib.fonts_.push_back(std::move(*f));
	}
	lib.load_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return lib;
}

const ConvertedFont *UiFontLibrary::find(std::string_view p_name) const {
	const ResolvedFont r = resolve_font_name(mapping_, p_name);
	if (r.font_long_name.empty()) {
		return nullptr;
	}
	for (const ConvertedFont &f : fonts_) {
		if (f.font_long_name == r.font_long_name) {
			return &f;
		}
	}
	return nullptr;
}

}
