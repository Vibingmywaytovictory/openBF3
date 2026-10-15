// L1 look: material resolution through the level shader database (look_materials.h; port of the colour / normal
// ranking of IW4L level_bf3 geometry/materials.rs, the user's Apache-2.0 code).
#include "bf3/render/look_materials.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/res/res_types.h"
#include "bf3/core/superbundle_mount.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <span>
#include <tuple>

namespace bf3::render {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint8_t kColor = 0, kNormal = 1, kSpecular = 2, kEmissive = 3;

// MeshVariationDatabase texture parameter names that carry the base colour / normal map, best first (IW4L
// materials.rs COLOR_PARAMETERS / NORMAL_PARAMETERS). Detail, dirt, mask and camo layers come last or not at all.
constexpr std::string_view kColorParameters[] = { "diffuse", "maindiffuse", "color", "difusealpha", "paintingdiffuse", "topdiffuse", "bottomdiffuse",
	"maintexture", "mat1_d", "mat2_d", "diffuseleaves", "diffuseatlas", "frame_d", "awningtexture", "tilediffuse", "detaildiffuse", "background",
	"diffuse2", "dirtdiffuse", "diffusebark", "texture2", "texture3", "camo", "decal", "decaltexture", "brandtexture", "logo", "detailtexture",
	"interiortexture", "enginetexture" };
constexpr std::string_view kNormalParameters[] = { "normal", "mainnormal", "normalmap", "paintingnormal", "mat1_n", "mat2_n_unique", "mat2_n_detail",
	"normalatlas", "frame_n", "tilenormal", "detailnormal", "tilenormaltexcoord1", "masknormal" };
constexpr std::string_view kSpecularParameters[] = { "specular", "spec", "frame_s", "detailspecular" };
constexpr std::string_view kEmissiveParameters[] = { "emissive", "emissivetexture", "emissivemap", "selfillumination" };

constexpr std::string_view kColorSuffixes[] = { "d", "c", "df", "da", "color", "diffuse" };
constexpr std::string_view kNormalSuffixes[] = { "n", "nm", "normal" };
constexpr std::string_view kMaskSuffixes[] = { "m", "rgb" };
constexpr std::string_view kSpecularSuffixes[] = { "s", "spec", "specular" };
constexpr std::string_view kEmissiveSuffixes[] = { "e", "em", "emissive" };
constexpr size_t kSuffixReach = 3;
constexpr uint8_t kUnnamedQuality = 2;
constexpr std::string_view kUnnamedNotColour[] = { "mask", "dirt", "grime", "soot", "ao", "occ", "occlusion", "heatmap", "gradient", "lookup",
	"cubemap", "envmap", "baked" };
constexpr std::string_view kPlaceholderWords[] = { "black", "white", "whitetexel", "noise", "perlin" };
constexpr std::string_view kPlaceholderPrefix = "default";
constexpr std::string_view kTileStrips[] = { "horizontaltile", "verticaltile" };
constexpr std::string_view kOverlayWords[] = { "puddle", "waterpuddle" };
constexpr std::string_view kFoliageWords[] = { "vegetation", "foliage", "leaf", "leaves", "bush", "grass", "ivy", "vine" };
constexpr std::string_view kBackdropWords[] = { "backdrop" };
constexpr std::string_view kGlassWords[] = { "glass", "window" };
constexpr std::string_view kUndrawnShaders[] = { "invisible", "occluder", "shadowmesh", "ui_characterbackdrop", "ui_menu" };
// M1's name rule for a shader the database does not hold (level_scene.cpp kAlphaTestWords / kDoubleSidedWords).
constexpr std::string_view kNameAlphaWords[] = { "foliage", "vegetation", "alphatest", "alpha_test", "fence", "decal", "grass", "leaf", "leaves",
	"tree", "bush", "ivy", "hair", "cutout", "masked" };

std::string lower(std::string_view p_text) {
	std::string out(p_text);
	std::transform(out.begin(), out.end(), out.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
	return out;
}

std::string_view last_part(std::string_view p_name) {
	const size_t at = p_name.rfind('/');
	return at == std::string_view::npos ? p_name : p_name.substr(at + 1);
}

std::string parent(std::string_view p_name) {
	const size_t at = p_name.rfind('/');
	return at == std::string_view::npos ? std::string() : std::string(p_name.substr(0, at));
}

std::vector<std::string> words(std::string_view p_name) {
	std::vector<std::string> out;
	std::string w;
	for (char c : p_name) {
		if (std::isalpha(uint8_t(c))) {
			w += char(std::tolower(uint8_t(c)));
		} else if (!w.empty()) {
			out.push_back(std::move(w));
			w.clear();
		}
	}
	if (!w.empty()) {
		out.push_back(std::move(w));
	}
	return out;
}

bool has_word(std::string_view p_name, std::span<const std::string_view> p_stems) {
	for (const std::string &w : words(p_name)) {
		for (std::string_view s : p_stems) {
			if (w.rfind(s, 0) == 0) {
				return true;
			}
		}
	}
	return false;
}

bool contains_any(std::string_view p_text, std::span<const std::string_view> p_words) {
	for (std::string_view w : p_words) {
		if (p_text.find(w) != std::string_view::npos) {
			return true;
		}
	}
	return false;
}

template <size_t N>
bool listed(std::string_view p_value, const std::string_view (&p_list)[N]) {
	return std::find(std::begin(p_list), std::end(p_list), p_value) != std::end(p_list);
}

std::optional<std::pair<uint8_t, size_t>> parameter_role(std::string_view p_lower_name) {
	const auto find = [&](std::span<const std::string_view> p_list) -> std::optional<size_t> {
		const auto it = std::find(p_list.begin(), p_list.end(), p_lower_name);
		return it == p_list.end() ? std::nullopt : std::optional<size_t>(size_t(it - p_list.begin()));
	};
	if (auto r = find(kColorParameters)) {
		return std::pair<uint8_t, size_t>(kColor, *r);
	}
	if (auto r = find(kNormalParameters)) {
		return std::pair<uint8_t, size_t>(kNormal, *r);
	}
	if (auto r = find(kSpecularParameters)) {
		return std::pair<uint8_t, size_t>(kSpecular, *r);
	}
	if (auto r = find(kEmissiveParameters)) {
		return std::pair<uint8_t, size_t>(kEmissive, *r);
	}
	return std::nullopt;
}

bool has_alpha(core::res::TextureFormat p_format) {
	using F = core::res::TextureFormat;
	return p_format == F::Dxt3 || p_format == F::Dxt5 || p_format == F::Argb8888 || p_format == F::Argb1555 || p_format == F::Argb4444;
}

struct Sampling {
	uint32_t coord_type = 1;
	uint32_t usage = kUsageTexCoord0;
	float factor = 1.0f;
};

struct Rank {
	LookTier tier = LookTier::Parameter;
	uint8_t quality = 0;
	size_t graph = 0;
	bool resident = false;
	size_t order = 0;
};

struct Candidate {
	uint8_t role = kColor;
	Rank rank;
	LookTexture texture;
};

bool same_uv(const LookTexture &p_a, const LookTexture &p_b) {
	return p_a.channel == p_b.channel && p_a.scale == p_b.scale;
}

bool tiled(const LookTexture &p_t) {
	return p_t.scale != 1.0f || contains_any(last_part(p_t.name), kTileStrips);
}

auto rank_key(const Candidate &p_c) {
	return std::make_tuple(uint8_t(p_c.rank.tier), p_c.texture.channel == kLookUvOverlay, p_c.rank.quality, p_c.rank.graph, tiled(p_c.texture),
			p_c.rank.resident, p_c.rank.order);
}

const Candidate *best(uint8_t p_role, const std::vector<Candidate> &p_candidates) {
	const Candidate *out = nullptr;
	for (const Candidate &c : p_candidates) {
		if (c.role == p_role && (out == nullptr || rank_key(c) < rank_key(*out))) {
			out = &c;
		}
	}
	return out;
}

} // namespace

const char *look_tier_name(LookTier p_tier) {
	switch (p_tier) {
		case LookTier::Parameter:
			return "parameter";
		case LookTier::Graph:
			return "graph";
		case LookTier::Neighbour:
			return "neighbour";
	}
	return "?";
}

std::optional<NameRole> suffix_role(std::string_view p_name) {
	size_t at = p_name.find_last_of("_/");
	std::string_view suffix = at == std::string_view::npos ? p_name : p_name.substr(at + 1);
	while (!suffix.empty() && std::isdigit(uint8_t(suffix.back()))) {
		suffix.remove_suffix(1);
	}
	if (listed(suffix, kMaskSuffixes)) {
		return NameRole{ kColor, 1 };
	}
	if (listed(suffix, kColorSuffixes)) {
		return NameRole{ kColor, 0 };
	}
	if (listed(suffix, kNormalSuffixes)) {
		return NameRole{ kNormal, 0 };
	}
	if (listed(suffix, kSpecularSuffixes)) {
		return NameRole{ kSpecular, 0 };
	}
	if (listed(suffix, kEmissiveSuffixes)) {
		return NameRole{ kEmissive, 0 };
	}
	return std::nullopt;
}

std::optional<NameRole> graph_role(std::string_view p_name) {
	if (std::optional<NameRole> r = suffix_role(p_name)) {
		return r;
	}
	// A role part shortly before the end: `water_d_01`, `ivyleafs01_d_xp4`.
	std::string_view rest = last_part(p_name);
	for (size_t part = 0; part < kSuffixReach && !rest.empty(); ++part) {
		const size_t at = rest.rfind('_');
		const std::string_view piece = at == std::string_view::npos ? rest : rest.substr(at + 1);
		if (part > 0) {
			if (std::optional<NameRole> r = suffix_role(piece)) {
				return r;
			}
		}
		if (at == std::string_view::npos) {
			break;
		}
		rest = rest.substr(0, at);
	}
	for (const std::string &w : words(last_part(p_name))) {
		for (std::string_view stem : kUnnamedNotColour) {
			if (w == stem || (stem.size() > 3 && w.find(stem) != std::string::npos)) {
				return std::nullopt;
			}
		}
	}
	return NameRole{ kColor, kUnnamedQuality };
}

bool placeholder_texture(std::string_view p_name) {
	const std::vector<std::string> w = words(last_part(p_name));
	if (w.empty()) {
		return false;
	}
	return w.front().rfind(kPlaceholderPrefix, 0) == 0 || listed(std::string_view(w.front()), kPlaceholderWords);
}

std::unique_ptr<LookResolver> LookResolver::open(const core::LevelMount &p_mount) {
	const Clock::time_point start = Clock::now();
	std::unique_ptr<LookResolver> out(new LookResolver());
	out->mount_ = &p_mount;
	std::vector<std::string> databases;
	for (const std::string &name : p_mount.res_names()) {
		const std::optional<core::ResInfo> info = p_mount.res_info(name);
		if (!info) {
			continue;
		}
		if (info->res_type == core::res::kResTypeDxTexture) {
			out->texture_names_.push_back(lower(name));
		} else if (info->res_type == kShaderDatabaseResType) {
			const std::string l = lower(name);
			if (l.size() >= 9 && l.compare(l.size() - 9, 9, "/shaderdb") == 0) {
				databases.push_back(name);
			}
		}
	}
	std::sort(out->texture_names_.begin(), out->texture_names_.end());
	out->texture_names_.erase(std::unique(out->texture_names_.begin(), out->texture_names_.end()), out->texture_names_.end());
	std::sort(databases.begin(), databases.end());
	ShaderDatabase merged;
	std::vector<uint32_t> seen;
	for (const std::string &name : databases) {
		core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(name);
		if (!res) {
			out->failures_.emplace_back(name, res.error().message);
			continue;
		}
		uint64_t work = 0;
		core::Result<std::vector<ShaderDatabase>> parsed = parse_shader_database(res->second, &work);
		out->stats_.shader_work += work;
		if (!parsed) {
			out->failures_.emplace_back(name, parsed.error().message);
			continue;
		}
		++out->stats_.shader_databases;
		if (parsed->empty()) {
			continue;
		}
		// The first render path, as IW4L reads it; a shader's first database wins.
		for (SurfaceShader &s : parsed->front().shaders) {
			if (std::find(seen.begin(), seen.end(), s.name_hash) != seen.end()) {
				continue;
			}
			seen.push_back(s.name_hash);
			merged.shaders.push_back(std::move(s));
		}
		std::sort(seen.begin(), seen.end());
	}
	out->stats_.shaders = merged.shaders.size();
	out->shaders_ = ShaderLookup(std::move(merged));
	out->stats_.seconds_database = std::chrono::duration<double>(Clock::now() - start).count();
	return out;
}

LookResolver::LookResolver(ShaderLookup p_shaders, std::map<std::string, HeaderInfo> p_headers, std::vector<std::string> p_texture_names) :
		shaders_(std::move(p_shaders)), texture_names_(std::move(p_texture_names)) {
	for (auto &[name, info] : p_headers) {
		headers_.emplace(lower(name), info);
	}
	std::sort(texture_names_.begin(), texture_names_.end());
}

const LookResolver::HeaderInfo *LookResolver::header(const std::string &p_name) {
	const auto found = headers_.find(p_name);
	if (found != headers_.end()) {
		return found->second ? &*found->second : nullptr;
	}
	std::optional<HeaderInfo> info;
	if (mount_ != nullptr) {
		++stats_.texture_headers;
		core::Result<core::res::RawRes> raw = core::res::read_raw_res(*mount_, p_name);
		core::res::DxTexture texture;
		if (raw && raw->res_type == core::res::kResTypeDxTexture && core::res::DxTexture::parse(raw->bytes(), texture).ok()) {
			info = HeaderInfo{ texture.kind, texture.format };
		} else {
			++stats_.texture_headers_failed;
		}
	}
	const auto inserted = headers_.emplace(p_name, info).first;
	return inserted->second ? &*inserted->second : nullptr;
}

const std::vector<std::string> &LookResolver::folder(const std::string &p_folder) {
	if (!folders_built_) {
		folders_built_ = true;
		for (const std::string &name : texture_names_) {
			folders_[parent(name)].push_back(name);
		}
	}
	static const std::vector<std::string> kNone;
	const auto found = folders_.find(p_folder);
	return found == folders_.end() ? kNone : found->second;
}

LookMaterial LookResolver::resolve(const MaterialSlot &p_slot, std::string_view p_mesh) {
	const Clock::time_point start = Clock::now();
	LookMaterial out;
	std::vector<std::string> shader_names;
	for (const std::string *name : { &p_slot.variation_shader, &p_slot.shader }) {
		if (!name->empty() && (shader_names.empty() || shader_names.back() != lower(*name))) {
			shader_names.push_back(lower(*name));
		}
	}
	for (const std::string &name : shader_names) {
		if (contains_any(name, kUndrawnShaders)) {
			out.drawn = false;
			return out;
		}
	}
	std::vector<const SurfaceShader *> graphs;
	for (const std::string &name : shader_names) {
		graphs.push_back(shaders_.by_name(name));
	}
	const SurfaceShader *shader = nullptr;
	for (const SurfaceShader *g : graphs) {
		if (g != nullptr) {
			shader = g;
			break;
		}
	}
	out.shader_in_database = shader != nullptr;
	const std::string shader_name = shader_names.empty() ? std::string() : shader_names.front();

	std::vector<Candidate> candidates;
	const auto candidate = [&](const std::string &p_name, uint8_t p_role, Rank p_rank, Sampling p_sampling, const std::string &p_parameter) {
		LookTexture t;
		float factor = p_sampling.factor;
		if (p_sampling.coord_type == kCoordWorld) {
			t.channel = kLookUvProjected;
		} else if (p_sampling.usage == kUsageTexCoord0) {
			t.channel = kLookUv0;
		} else if (p_sampling.usage == kUsageTexCoord1) {
			t.channel = kLookUv1;
		} else if (p_sampling.usage == kUsageTexCoord2) {
			t.channel = kLookUvOverlay;
			factor = 1.0f;
		} else {
			return;
		}
		t.name = lower(p_name);
		if (t.name.empty() || placeholder_texture(t.name)) {
			return;
		}
		const HeaderInfo *h = header(t.name);
		if (h == nullptr || h->kind != core::res::TextureKind::Texture2D) {
			return;
		}
		uint8_t role = p_role;
		if ((role == kColor || role == kSpecular) && core::res::texture_format_is_normal_map(h->format)) {
			role = kNormal;
		}
		if (role == kColor && has_word(last_part(t.name), kOverlayWords)) {
			p_rank.quality = std::max<uint8_t>(p_rank.quality, 1);
		}
		t.scale = factor > 0.0f && factor != 1.0f && std::isfinite(factor) ? std::clamp(1.0f / factor, 1.0f / 256.0f, 64.0f) : 1.0f;
		t.tier = p_rank.tier;
		t.quality = p_rank.quality;
		t.parameter = p_parameter;
		candidates.push_back(Candidate{ role, p_rank, std::move(t) });
	};

	// 1. The slot's parameters.
	for (const auto &[parameter, texture] : p_slot.textures) {
		const std::string name = lower(parameter);
		const std::optional<std::pair<uint8_t, size_t>> role = parameter_role(name);
		if (!role) {
			continue;
		}
		Sampling sampling;
		if (shader != nullptr) {
			if (const ShaderTextureParameter *known = shader->parameter(name)) {
				sampling = Sampling{ known->coord_type, known->vertex_usage, known->factor };
			}
		}
		candidate(texture, role->first, Rank{ LookTier::Parameter, 0, 0, false, role->second }, sampling, parameter);
	}
	// 2. The graphs' constants.
	for (size_t g = 0; g < graphs.size(); ++g) {
		if (graphs[g] == nullptr) {
			continue;
		}
		const SurfaceShader &s = *graphs[g];
		for (size_t order = 0; order < s.textures.size(); ++order) {
			const std::string name = lower(s.textures[order].name);
			const std::optional<NameRole> role = graph_role(name);
			if (!role) {
				continue;
			}
			candidate(name, role->role, Rank{ LookTier::Graph, role->quality, g, false, order },
					Sampling{ s.textures[order].coord_type, s.textures[order].vertex_usage, s.textures[order].factor }, "");
		}
		for (size_t order = 0; order < s.sampled_textures.size(); ++order) {
			const std::string name = lower(s.sampled_textures[order]);
			const bool streamed = std::any_of(s.textures.begin(), s.textures.end(), [&](const ShaderTexture &t) { return lower(t.name) == name; });
			if (streamed) {
				continue;
			}
			const std::optional<NameRole> role = graph_role(name);
			if (!role) {
				continue;
			}
			candidate(name, role->role, Rank{ LookTier::Graph, role->quality, g, true, order }, Sampling{}, "");
		}
	}
	// 3. Neighbours, for a shader the database lacks and no colour so far.
	const bool missing_graph = graphs.empty() || std::any_of(graphs.begin(), graphs.end(), [](const SurfaceShader *g) { return g == nullptr; });
	if (missing_graph && std::none_of(candidates.begin(), candidates.end(), [](const Candidate &c) { return c.role == kColor; })) {
		std::vector<std::string> folders;
		for (const std::string &name : shader_names) {
			folders.push_back(parent(name));
		}
		folders.push_back(parent(lower(p_mesh)));
		folders.erase(std::unique(folders.begin(), folders.end()), folders.end());
		const std::string mesh = lower(p_mesh); // owns the bytes last_part views (a temporary here dangled)
		const std::string stem_copy(last_part(mesh));
		for (size_t tier = 0; tier < folders.size(); ++tier) {
			if (folders[tier].empty()) {
				continue;
			}
			for (const std::string &name : folder(folders[tier])) {
				const std::optional<NameRole> role = suffix_role(name);
				if (!role || role->quality != 0) {
					continue;
				}
				const std::string_view part = last_part(name);
				size_t shared = 0;
				while (shared < part.size() && shared < stem_copy.size() && part[shared] == stem_copy[shared]) {
					++shared;
				}
				candidate(name, role->role, Rank{ LookTier::Neighbour, 0, tier, false, SIZE_MAX - shared }, Sampling{}, "");
			}
		}
	}

	for (const Candidate &c : candidates) {
		out.color_candidates += c.role == kColor ? 1 : 0;
		out.normal_candidates += c.role == kNormal ? 1 : 0;
	}
	// Colour: the best, unless it is an untiled graph trim with no normal map in a masked graph whose tiled colour
	// layers have one (IW4L unmasked_color; its packed-mask colour statistics are not ported).
	std::optional<LookTexture> color;
	if (const Candidate *chosen = best(kColor, candidates)) {
		color = chosen->texture;
		if (chosen->rank.tier == LookTier::Graph && chosen->texture.scale == 1.0f) {
			const auto shaded = [&](const LookTexture &p_t) {
				return std::any_of(candidates.begin(), candidates.end(), [&](const Candidate &c) { return c.role == kNormal && same_uv(c.texture, p_t); });
			};
			const bool masked = std::any_of(candidates.begin(), candidates.end(),
					[](const Candidate &c) { return c.role == kColor && c.rank.tier == LookTier::Graph && c.rank.quality == 1; });
			if (masked && !shaded(chosen->texture)) {
				std::vector<Candidate> layers;
				for (const Candidate &c : candidates) {
					if (c.role == kColor && c.rank.tier == LookTier::Graph && c.rank.quality == 0 && c.texture.scale != 1.0f &&
							c.texture.name != chosen->texture.name && shaded(c.texture)) {
						layers.push_back(c);
					}
				}
				if (const Candidate *layer = best(kColor, layers)) {
					color = layer->texture;
				}
			}
		}
	}
	std::optional<LookTexture> uv_owner = color;
	if (!uv_owner) {
		if (const Candidate *n = best(kNormal, candidates)) {
			uv_owner = n->texture;
		}
	}
	// Normal: the best that shares the colour's UV set and tiling (IW4L's rule); failing that (L1, the shader samples
	// each texture with its own UV), the best on a mesh UV set, which the mesh tangents frame, e.g. a uv0 tile normal
	// under a uv1 main colour (facadepreset2uvs).
	{
		std::vector<Candidate> paired, meshed;
		for (const Candidate &c : candidates) {
			if (c.role != kNormal) {
				continue;
			}
			if (!uv_owner || same_uv(*uv_owner, c.texture)) {
				paired.push_back(c);
			} else if (c.texture.channel == kLookUv0 || c.texture.channel == kLookUv1) {
				meshed.push_back(c);
			}
		}
		if (const Candidate *n = best(kNormal, paired)) {
			out.normal = n->texture;
		} else if (const Candidate *m = best(kNormal, meshed)) {
			out.normal = m->texture;
		}
	}

	const bool foliage = has_word(shader_name, kFoliageWords);
	const HeaderInfo *color_header = color ? header(color->name) : nullptr;
	const bool color_alpha = color_header != nullptr && has_alpha(color_header->format);
	if (shader != nullptr && is_alpha_tested(shader->surface_type)) {
		out.blend = LookBlend::AlphaTest;
	} else if (shader != nullptr && is_transparent(shader->surface_type)) {
		out.blend = LookBlend::Blend;
	} else if (shader == nullptr && color_alpha && (foliage || contains_any(shader_name, kNameAlphaWords))) {
		out.blend = LookBlend::AlphaTest;
	} else if (color_alpha && has_word(shader_name, kBackdropWords)) {
		out.blend = LookBlend::AlphaTest;
	}
	if (out.blend == LookBlend::Blend && !color_alpha && color) {
		for (const std::string &w : words(last_part(color->name))) {
			if (w.find("mask") != std::string::npos) {
				out.drawn = false; // a transparent overlay drawn by its mask alone (IW4L drops it)
				return out;
			}
		}
	}
	out.double_sided = shader != nullptr ? shader->double_sided() : foliage;
	// Glass: a glass / window shader with no colour, or a transparent one whose colour map has no alpha. BF3 computes
	// its opacity in the compiled pixel shader (OpacityMul, Fresnel), which the database does not expose: L1 keeps the
	// colour map as the tint and draws it at a fixed opacity (level_upload.cpp kGlassOpacity).
	out.glass = has_word(shader_name, kGlassWords) && (!color || (out.blend == LookBlend::Blend && !color_alpha));
	if (out.glass) {
		out.blend = LookBlend::Blend;
	}
	out.color = std::move(color);
	stats_.seconds_resolve += std::chrono::duration<double>(Clock::now() - start).count();
	return out;
}

} // namespace bf3::render
