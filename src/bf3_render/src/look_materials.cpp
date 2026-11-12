#include "bf3/render/look_materials.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/res/res_types.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/text.h"
#include "bf3/render/dxbc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <iterator>
#include <span>
#include <tuple>

namespace bf3::render {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint8_t kColor = 0, kNormal = 1, kSpecular = 2, kEmissive = 3;

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
constexpr std::string_view kUndrawnShaders[] = { "invisible", "occluder", "shadowmesh", "ui_menu" };
constexpr std::string_view kCharacterBackdropWords[] = { "charbackdrop" };
constexpr std::string_view kLightConeWords[] = { "lightcone" };
constexpr std::string_view kDustWords[] = { "lightdust" };
constexpr LookLayerRole kCharacterBackdropRoles[] = { LookLayerRole::Base, LookLayerRole::Lit, LookLayerRole::Noise,
	LookLayerRole::Flow, LookLayerRole::Mask };
constexpr LookLayerRole kLightConeRoles[] = { LookLayerRole::Base, LookLayerRole::Dust };

constexpr std::string_view kNameAlphaWords[] = { "foliage", "vegetation", "alphatest", "alpha_test", "fence", "decal", "grass", "leaf", "leaves",
	"tree", "bush", "ivy", "hair", "cutout", "masked" };

using bf3::core::to_lower_ascii;

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

template <class Header>
std::optional<Candidate> make_candidate(Header &&p_header, std::string_view p_name, uint8_t p_role, Rank p_rank, Sampling p_sampling,
		const std::string &p_parameter) {
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
		return std::nullopt;
	}
	t.name = to_lower_ascii(p_name);
	if (t.name.empty() || placeholder_texture(t.name)) {
		return std::nullopt;
	}
	const LookResolver::HeaderInfo *h = p_header(t.name);
	if (h == nullptr || h->kind != core::res::TextureKind::Texture2D) {
		return std::nullopt;
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
	return Candidate{ role, p_rank, std::move(t) };
}

size_t shared_prefix(std::string_view p_a, std::string_view p_b) {
	size_t n = 0;
	while (n < p_a.size() && n < p_b.size() && p_a[n] == p_b[n]) {
		++n;
	}
	return n;
}

constexpr size_t kNeighbourClasses = 8;
std::optional<size_t> neighbour_class(const Candidate &p_c) {
	if (p_c.role != kColor && p_c.role != kNormal) {
		return std::nullopt;
	}
	return size_t(p_c.role == kNormal ? 4 : 0) + size_t(std::min<uint8_t>(p_c.rank.quality, 1) * 2) + size_t(tiled(p_c.texture) ? 1 : 0);
}

LookLayerRole layer_role(std::string_view p_name, bool p_first_colour) {
	const std::vector<std::string> w = words(last_part(p_name));
	const auto named = [&](std::string_view p_word) { return std::find(w.begin(), w.end(), p_word) != w.end(); };
	if (named("flow")) {
		return LookLayerRole::Flow;
	}
	if (named("mask")) {
		return LookLayerRole::Mask;
	}
	if (named("perlin") || named("noise")) {
		return LookLayerRole::Noise;
	}
	if (has_word(last_part(p_name), kDustWords)) {
		return LookLayerRole::Dust;
	}
	return p_first_colour ? LookLayerRole::Base : LookLayerRole::Lit;
}

template <size_t N>
std::vector<LookLayer> program_layers(const SurfaceShader &p_shader, const LookLayerRole (&p_roles)[N]) {
	std::vector<LookLayer> out;
	bool first_colour = true;
	for (const std::string &sampled : p_shader.sampled_textures) {
		const std::string name = to_lower_ascii(sampled);
		const LookLayerRole role = layer_role(name, first_colour);
		if (role == LookLayerRole::Base || role == LookLayerRole::Lit) {
			first_colour = false;
		}
		out.push_back(LookLayer{ role, name });
	}
	for (LookLayerRole r : p_roles) {
		if (std::count_if(out.begin(), out.end(), [&](const LookLayer &p_l) { return p_l.role == r; }) != 1) {
			return {};
		}
	}
	if (out.size() != N) {
		return {};
	}
	return out;
}

}

struct LookResolver::GraphInfo {
	std::vector<std::pair<std::string, size_t>> parameters;
	std::vector<Candidate> candidates;
	bool double_sided = false;
};

struct ProgramBinding {
	std::string name;
	std::string parameter;
};

struct LookResolver::ProgramInfo {
	std::string refusal;
	ProgramTarget target = ProgramTarget::Deferred;
	const SurfaceShader *shader = nullptr;
	const ShaderSolution *solution = nullptr;
	dxbc::Program pixel;
	dxbc::Program vertex;
	std::map<uint32_t, ProgramBinding> bindings;
	std::vector<ShaderValue> defaults;
	std::vector<uint32_t> used;
	std::map<uint64_t, std::shared_ptr<const GraphProgram>> variants;
	ProgramAnalysis analysis;
	std::vector<ProgramBinding> mask_bindings;
};

struct LookResolver::FolderIndex {

	std::array<std::vector<std::pair<std::string, Candidate>>, kNeighbourClasses> classes;
};

const char *look_program_name(LookProgram p_program) {
	switch (p_program) {
		case LookProgram::Standard:
			return "standard";
		case LookProgram::CharacterBackdrop:
			return "character backdrop";
		case LookProgram::LightCone:
			return "light cone";
	}
	return "?";
}

const char *look_layer_role_name(LookLayerRole p_role) {
	switch (p_role) {
		case LookLayerRole::Base:
			return "base";
		case LookLayerRole::Lit:
			return "lit";
		case LookLayerRole::Noise:
			return "noise";
		case LookLayerRole::Flow:
			return "flow";
		case LookLayerRole::Mask:
			return "mask";
		case LookLayerRole::Dust:
			return "dust";
	}
	return "?";
}

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

bool mask_texture(std::string_view p_name) {
	const std::optional<NameRole> suffix = suffix_role(p_name);
	if (suffix && suffix->role == kColor && suffix->quality == 1) {
		return true;
	}
	const std::vector<std::string> w = words(last_part(p_name));
	return std::any_of(
			w.begin(), w.end(), [](const std::string &p_word) { return p_word.find("mask") != std::string::npos; });
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
			out->texture_names_.push_back(to_lower_ascii(name));
		} else if (info->res_type == kShaderDatabaseResType) {
			const std::string l = to_lower_ascii(name);
			if (l.size() >= 9 && l.compare(l.size() - 9, 9, "/shaderdb") == 0) {
				databases.push_back(name);
			}
		}
	}
	std::sort(out->texture_names_.begin(), out->texture_names_.end());
	out->texture_names_.erase(std::unique(out->texture_names_.begin(), out->texture_names_.end()), out->texture_names_.end());
	std::sort(databases.begin(), databases.end());
	std::vector<std::vector<SurfaceShader>> paths;
	for (const std::string &name : databases) {
		core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(name);
		if (!res) {
			out->failures_.emplace_back(name, res.error().message);
			continue;
		}
		uint64_t work = 0;
		core::Result<std::vector<ShaderDatabase>> parsed = parse_shader_database(res->second, &work, true);
		out->stats_.shader_work += work;
		if (!parsed) {
			out->failures_.emplace_back(name, parsed.error().message);
			continue;
		}
		++out->stats_.shader_databases;
		if (parsed->empty()) {
			continue;
		}

		paths.push_back(std::move(parsed->front().shaders));
	}
	ShaderDatabase merged = merge_first_wins(std::move(paths));
	out->stats_.shaders = merged.shaders.size();
	out->shaders_ = ShaderLookup(std::move(merged));
	out->stats_.seconds_database = std::chrono::duration<double>(Clock::now() - start).count();
	return out;
}

LookResolver::LookResolver() = default;
LookResolver::~LookResolver() = default;

LookResolver::LookResolver(ShaderLookup p_shaders, std::map<std::string, HeaderInfo> p_headers, std::vector<std::string> p_texture_names) :
		shaders_(std::move(p_shaders)), texture_names_(std::move(p_texture_names)) {
	for (auto &[name, info] : p_headers) {
		headers_.emplace(to_lower_ascii(name), info);
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
			info = HeaderInfo{ texture.kind, texture.format, texture.is_srgb() };
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

const LookResolver::GraphInfo &LookResolver::graph_info(const SurfaceShader &p_shader) {
	const auto found = graphs_.find(&p_shader);
	if (found != graphs_.end()) {
		return *found->second;
	}
	auto info = std::make_unique<GraphInfo>();
	const SurfaceShader &s = p_shader;
	const auto hdr = [this](const std::string &p_name) { return header(p_name); };
	info->parameters.reserve(s.texture_parameters.size());
	for (size_t i = 0; i < s.texture_parameters.size(); ++i) {
		info->parameters.emplace_back(to_lower_ascii(s.texture_parameters[i].name), i);
	}
	std::stable_sort(info->parameters.begin(), info->parameters.end(), [](const auto &p_a, const auto &p_b) { return p_a.first < p_b.first; });
	std::vector<std::string> streamed;
	streamed.reserve(s.textures.size());
	for (size_t order = 0; order < s.textures.size(); ++order) {
		std::string name = to_lower_ascii(s.textures[order].name);
		if (const std::optional<NameRole> role = graph_role(name)) {
			std::optional<Candidate> c = make_candidate(hdr, name, role->role, Rank{ LookTier::Graph, role->quality, 0, false, order },
					Sampling{ s.textures[order].coord_type, s.textures[order].vertex_usage, s.textures[order].factor }, std::string());
			if (c && (c->role == kColor || c->role == kNormal)) {
				info->candidates.push_back(std::move(*c));
			}
		}
		streamed.push_back(std::move(name));
	}
	std::sort(streamed.begin(), streamed.end());
	for (size_t order = 0; order < s.sampled_textures.size(); ++order) {
		const std::string name = to_lower_ascii(s.sampled_textures[order]);
		if (std::binary_search(streamed.begin(), streamed.end(), name)) {
			continue;
		}
		if (const std::optional<NameRole> role = graph_role(name)) {
			std::optional<Candidate> c = make_candidate(hdr, name, role->role, Rank{ LookTier::Graph, role->quality, 0, true, order }, Sampling{}, std::string());
			if (c && (c->role == kColor || c->role == kNormal)) {
				info->candidates.push_back(std::move(*c));
			}
		}
	}
	stats_.resolve_work += s.texture_parameters.size() + s.textures.size() + s.sampled_textures.size() + s.solutions.size();
	stats_.graph_candidates_max = std::max(stats_.graph_candidates_max, info->candidates.size());
	if (info->candidates.size() > kLookMaxGraphCandidates) {
		stats_.graph_candidates_dropped += info->candidates.size() - kLookMaxGraphCandidates;
		info->candidates.resize(kLookMaxGraphCandidates);
	}
	info->double_sided = s.double_sided();
	return *graphs_.emplace(&p_shader, std::move(info)).first->second;
}

const LookResolver::FolderIndex &LookResolver::folder_index(const std::string &p_folder) {
	const auto found = folder_indices_.find(p_folder);
	if (found != folder_indices_.end()) {
		return *found->second;
	}
	auto index = std::make_unique<FolderIndex>();
	const auto hdr = [this](const std::string &p_name) { return header(p_name); };
	const std::vector<std::string> &names = folder(p_folder);
	for (const std::string &name : names) {
		const std::optional<NameRole> role = suffix_role(name);
		if (!role || role->quality != 0) {
			continue;
		}
		std::optional<Candidate> c = make_candidate(hdr, name, role->role, Rank{ LookTier::Neighbour, 0, 0, false, 0 }, Sampling{}, std::string());
		if (!c) {
			continue;
		}
		if (const std::optional<size_t> k = neighbour_class(*c)) {
			index->classes[*k].emplace_back(std::string(last_part(c->texture.name)), std::move(*c));
		}
	}
	for (auto &list : index->classes) {
		std::stable_sort(list.begin(), list.end(), [](const auto &p_a, const auto &p_b) { return p_a.first < p_b.first; });
	}
	stats_.resolve_work += names.size();
	return *folder_indices_.emplace(p_folder, std::move(index)).first->second;
}

LookResolver::ProgramInfo &LookResolver::program_info(const SurfaceShader &p_shader) {
	const auto found = programs_.find(&p_shader);
	if (found != programs_.end()) {
		return *found->second;
	}
	const Clock::time_point start = Clock::now();
	auto info = std::make_unique<ProgramInfo>();
	info->target = is_transparent(p_shader.surface_type) ? ProgramTarget::Forward : ProgramTarget::Deferred;
	info->shader = &p_shader;
	const uint32_t target_register = info->target == ProgramTarget::Deferred ? 1u : 0u;
	const auto finish = [&]() -> ProgramInfo & {
		++(info->refusal.empty() ? stats_.programs_translated : stats_.programs_refused);
		stats_.seconds_programs += std::chrono::duration<double>(Clock::now() - start).count();
		return *programs_.emplace(&p_shader, std::move(info)).first->second;
	};
	constexpr std::string_view kTexturePrefix = "texture_";
	std::string last = "no solution draws its target with a texture";
	std::optional<TranslatedProgram> probe;
	for (const ShaderSolution &s : p_shader.solutions) {
		if (!s.pixel_program || !s.vertex_program) {
			continue;
		}
		core::Result<dxbc::Program> pixel = dxbc::parse(*s.pixel_program);
		if (!pixel) {
			last = pixel.error().message;
			continue;
		}
		const bool target =
				std::any_of(pixel->outputs.begin(), pixel->outputs.end(), [&](const dxbc::SignatureElement &p_e) {
					return p_e.name == "SV_Target" && p_e.reg == target_register;
				});
		const bool samples = std::any_of(pixel->code.begin(), pixel->code.end(),
				[](const dxbc::Instruction &p_i) { return p_i.is(dxbc::Op::Sample); });
		if (!target || !samples) {
			continue;
		}
		core::Result<dxbc::Program> vertex = dxbc::parse(s.vertex_program->bytes);
		if (!vertex) {
			last = vertex.error().message;
			continue;
		}
		info->solution = &s;
		info->pixel = std::move(*pixel);
		info->vertex = std::move(*vertex);
		info->bindings.clear();
		info->defaults.clear();
		if (s.pixel_constants) {
			for (const auto &[reg, name] : s.pixel_constants->textures) {
				info->bindings[reg] = ProgramBinding{ to_lower_ascii(name), std::string() };
			}
			info->defaults = s.pixel_constants->values;
		}
		for (const dxbc::Binding &b : info->pixel.bindings) {
			if (b.type != uint32_t(dxbc::BindingType::Texture) || info->bindings.count(b.bind_point) != 0 ||
					b.name.rfind(kTexturePrefix, 0) != 0) {
				continue;
			}
			const std::string parameter = b.name.substr(kTexturePrefix.size());
			if (p_shader.parameter(parameter) != nullptr) {
				info->bindings[b.bind_point] = ProgramBinding{ std::string(), parameter };
			}
		}
		ProgramAnalysis analysis;
		core::Result<TranslatedProgram> t = translate(*info, 0, &analysis);
		if (analysis.sliced && (t || !info->analysis.sliced)) {
			info->analysis = analysis;
			info->mask_bindings.clear();
			for (uint32_t reg : analysis.mask_only) {
				const auto b = info->bindings.find(reg);
				if (b != info->bindings.end()) {
					info->mask_bindings.push_back(b->second);
				}
			}
		}
		if (t) {
			probe = std::move(*t);
			break;
		}
		last = t.error().message;
	}
	if (!probe) {
		info->solution = nullptr;
		info->refusal = last;
		return finish();
	}
	info->used = probe->textures;
	info->variants.emplace(0, std::make_shared<GraphProgram>(GraphProgram{ std::move(*probe) }));
	return finish();
}

core::Result<TranslatedProgram> LookResolver::translate(
		const ProgramInfo &p_info, uint64_t p_srgb, ProgramAnalysis *r_analysis) {
	std::vector<uint32_t> registers;
	for (const auto &[reg, binding] : p_info.bindings) {
		registers.push_back(reg);
	}
	ProgramRequest request;
	request.pixel = &p_info.pixel;
	request.vertex = &p_info.vertex;
	request.vertex_formats = p_info.solution->vertex_program->element_formats;
	request.vertex_lanes = p_info.solution->vertex_program->element_lanes;
	request.bound_textures = registers;
	request.target = p_info.target;
	const uint8_t blend = p_info.solution->blend_mode;
	request.blend = blend == 2			 ? ProgramBlend::Multiply
			: (blend == 1 || blend == 3) ? ProgramBlend::Add
										 : ProgramBlend::Mix;
	request.double_sided = (p_info.solution->flags & 1u) != 0;
	request.texture_usage = [&](uint32_t p_reg) -> uint32_t {
		const auto b = p_info.bindings.find(p_reg);
		if (b == p_info.bindings.end()) {
			return 0;
		}
		if (!b->second.parameter.empty()) {
			const ShaderTextureParameter *parameter = p_info.shader->parameter(b->second.parameter);
			return parameter != nullptr && parameter->coord_type != kCoordWorld ? parameter->vertex_usage : 0;
		}
		for (const ShaderTexture &t : p_info.shader->textures) {
			if (core::iequals_ascii(t.name, b->second.name)) {
				return t.coord_type != kCoordWorld ? t.vertex_usage : 0;
			}
		}
		return 0;
	};
	const std::vector<uint32_t> &used = p_info.used;
	request.srgb = [&](uint32_t p_reg) {
		const auto at = std::find(used.begin(), used.end(), p_reg);
		return at != used.end() && ((p_srgb >> size_t(at - used.begin())) & 1u) != 0;
	};
	request.analysis = r_analysis;
	return translate_program(request);
}

void LookResolver::attach_program(
		LookMaterial &r_out, const SurfaceShader &p_shader, const MaterialSlot &p_slot, bool p_hidden_mask) {
	ProgramInfo &info = program_info(p_shader);
	if (!info.refusal.empty()) {
		r_out.graph_refusal = info.refusal;
		for (const ProgramBinding &b : info.mask_bindings) {
			const std::string name = b.parameter.empty() ? b.name : to_lower_ascii(p_slot.texture(b.parameter));
			r_out.graph_mask_colour = r_out.graph_mask_colour || (r_out.color && r_out.color->name == name);
		}
		return;
	}
	if (info.target == ProgramTarget::Forward && !p_hidden_mask) {
		return;
	}
	std::vector<std::string> textures;
	uint64_t srgb = 0;
	for (size_t i = 0; i < info.used.size(); ++i) {
		const ProgramBinding &b = info.bindings.at(info.used[i]);
		const std::string name = b.parameter.empty() ? b.name : to_lower_ascii(p_slot.texture(b.parameter));
		const HeaderInfo *h = name.empty() ? nullptr : header(name);
		if (h == nullptr || h->kind != core::res::TextureKind::Texture2D) {
			r_out.graph_refusal =
					"texture " + (b.parameter.empty() ? name : b.parameter + " " + name) + " is not a 2D texture";
			return;
		}
		srgb |= h->srgb ? uint64_t(1) << i : 0;
		textures.push_back(name);
	}
	bool mask_colour = false;
	for (const ProgramBinding &b : info.mask_bindings) {
		const std::string name = b.parameter.empty() ? b.name : to_lower_ascii(p_slot.texture(b.parameter));
		mask_colour = mask_colour || (r_out.color && r_out.color->name == name);
	}
	if (info.target == ProgramTarget::Deferred && !mask_colour && textures.size() == 1 && r_out.color &&
			textures.front() == r_out.color->name) {
		r_out.graph_plain = true;
		return;
	}
	if (info.target == ProgramTarget::Deferred && !mask_colour) {
		return;
	}
	auto variant = info.variants.find(srgb);
	if (variant == info.variants.end()) {
		core::Result<TranslatedProgram> t = translate(info, srgb);
		if (!t) {
			r_out.graph_refusal = t.error().message;
			return;
		}
		variant = info.variants.emplace(srgb, std::make_shared<GraphProgram>(GraphProgram{ std::move(*t) })).first;
	}
	std::vector<std::array<float, 4>> constants;
	for (const ProgramConstant &c : variant->second->translated.constants) {
		std::array<float, 4> v{};
		for (size_t lane = 0; lane < 4; ++lane) {
			const ProgramConstantLane &l = c.lanes[lane];
			if (l.value.empty()) {
				continue;
			}
			const std::array<float, 4> *given = p_slot.vector(l.value);
			const auto fallback = std::find_if(info.defaults.begin(), info.defaults.end(),
					[&](const ShaderValue &p_v) { return core::iequals_ascii(p_v.name, l.value); });
			if (given == nullptr && fallback == info.defaults.end()) {
				r_out.graph_refusal = "constant " + l.value + " has no value";
				return;
			}
			v[lane] = given != nullptr ? (*given)[l.component] : fallback->value[l.component];
		}
		constants.push_back(v);
	}
	r_out.graph = variant->second;
	r_out.graph_textures = std::move(textures);
	r_out.graph_constants = std::move(constants);
}

LookMaterial LookResolver::resolve(const MaterialSlot &p_slot, std::string_view p_mesh) {
	const Clock::time_point start = Clock::now();
	LookMaterial out;
	std::vector<std::string> shader_names;
	for (const std::string *name : { &p_slot.variation_shader, &p_slot.shader }) {
		if (!name->empty() && (shader_names.empty() || shader_names.back() != to_lower_ascii(*name))) {
			shader_names.push_back(to_lower_ascii(*name));
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
	const auto hdr = [this](const std::string &p_name) { return header(p_name); };
	const GraphInfo *shader_info = shader != nullptr ? &graph_info(*shader) : nullptr;

	for (const auto &[parameter, texture] : p_slot.textures) {
		const std::string name = to_lower_ascii(parameter);
		const std::optional<std::pair<uint8_t, size_t>> role = parameter_role(name);
		if (!role) {
			continue;
		}
		Sampling sampling;
		if (shader_info != nullptr) {
			const auto known = std::lower_bound(shader_info->parameters.begin(), shader_info->parameters.end(), name,
					[](const std::pair<std::string, size_t> &p_e, const std::string &p_name) { return p_e.first < p_name; });
			if (known != shader_info->parameters.end() && known->first == name) {
				const ShaderTextureParameter &p = shader->texture_parameters[known->second];
				sampling = Sampling{ p.coord_type, p.vertex_usage, p.factor };
			}
		}
		if (std::optional<Candidate> c = make_candidate(hdr, texture, role->first, Rank{ LookTier::Parameter, 0, 0, false, role->second }, sampling, parameter)) {
			candidates.push_back(std::move(*c));
		}
		++stats_.resolve_work;
	}

	for (size_t g = 0; g < graphs.size(); ++g) {
		if (graphs[g] == nullptr) {
			continue;
		}
		for (const Candidate &c : graph_info(*graphs[g]).candidates) {
			candidates.push_back(c);
			candidates.back().rank.graph = g;
			++stats_.resolve_work;
		}
	}

	const bool missing_graph = graphs.empty() || std::any_of(graphs.begin(), graphs.end(), [](const SurfaceShader *g) { return g == nullptr; });
	if (missing_graph && std::none_of(candidates.begin(), candidates.end(), [](const Candidate &c) { return c.role == kColor; })) {
		std::vector<std::string> folders;
		for (const std::string &name : shader_names) {
			folders.push_back(parent(name));
		}
		folders.push_back(parent(to_lower_ascii(p_mesh)));
		folders.erase(std::unique(folders.begin(), folders.end()), folders.end());
		const std::string mesh = to_lower_ascii(p_mesh);
		const std::string_view stem = last_part(mesh);

		const auto before = [](const std::pair<std::string, Candidate> &p_e, std::string_view p_s) { return std::string_view(p_e.first) < p_s; };
		for (size_t tier = 0; tier < folders.size(); ++tier) {
			if (folders[tier].empty()) {
				continue;
			}
			for (const auto &list : folder_index(folders[tier]).classes) {
				if (list.empty()) {
					continue;
				}
				const auto at = std::lower_bound(list.begin(), list.end(), stem, before);
				size_t shared = 0;
				if (at != list.end()) {
					shared = shared_prefix(at->first, stem);
				}
				if (at != list.begin()) {
					shared = std::max(shared, shared_prefix(std::prev(at)->first, stem));
				}
				const auto first = shared == 0 ? list.begin() : std::lower_bound(list.begin(), list.end(), stem.substr(0, shared), before);
				candidates.push_back(first->second);
				candidates.back().rank.graph = tier;
				candidates.back().rank.order = SIZE_MAX - shared;
				++stats_.resolve_work;
			}
		}
	}

	for (const Candidate &c : candidates) {
		out.color_candidates += c.role == kColor ? 1 : 0;
		out.normal_candidates += c.role == kNormal ? 1 : 0;
	}

	std::optional<LookTexture> color;
	if (const Candidate *chosen = best(kColor, candidates)) {
		color = chosen->texture;
		if (chosen->rank.tier == LookTier::Graph && chosen->texture.scale == 1.0f) {
			std::vector<std::pair<uint8_t, float>> normal_uvs;
			for (const Candidate &c : candidates) {
				if (c.role == kNormal) {
					normal_uvs.emplace_back(c.texture.channel, c.texture.scale);
				}
			}
			std::sort(normal_uvs.begin(), normal_uvs.end());
			const auto shaded = [&](const LookTexture &p_t) {
				return std::binary_search(normal_uvs.begin(), normal_uvs.end(), std::pair<uint8_t, float>(p_t.channel, p_t.scale));
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
	if (out.blend == LookBlend::Blend && !color_alpha && color && mask_texture(color->name)) {
		if (!has_word(shader_name, kGlassWords)) {
			if (shader != nullptr) {
				attach_program(out, *shader, p_slot, true);
			} else {
				out.graph_refusal = "shader not in the database";
			}
			if (!out.graph) {
				out.drawn = false;
				return out;
			}
		}
		color.reset();
	}
	out.double_sided = shader_info != nullptr ? shader_info->double_sided : foliage;

	out.glass = has_word(shader_name, kGlassWords) && (!color || (out.blend == LookBlend::Blend && !color_alpha));
	if (out.glass) {
		out.blend = LookBlend::Blend;
	}
	out.color = std::move(color);
	if (shader != nullptr && !out.graph && out.blend != LookBlend::Blend &&
			!contains_any(shader_name, kCharacterBackdropWords)) {
		attach_program(out, *shader, p_slot, false);
	}
	if (shader != nullptr) {
		if (!shader->solutions.empty()) {
			out.solution_blend = shader->solutions.front().blend_mode;
		}
		out.values = shader->values;
		if (contains_any(shader_name, kCharacterBackdropWords)) {
			out.layers = program_layers(*shader, kCharacterBackdropRoles);
			out.program = out.layers.empty() ? LookProgram::Standard : LookProgram::CharacterBackdrop;
		} else if (contains_any(shader_name, kLightConeWords) && out.blend == LookBlend::Blend) {
			out.layers = program_layers(*shader, kLightConeRoles);
			out.program = out.layers.empty() ? LookProgram::Standard : LookProgram::LightCone;
		}
	}
	stats_.seconds_resolve += std::chrono::duration<double>(Clock::now() - start).count();
	return out;
}

}
