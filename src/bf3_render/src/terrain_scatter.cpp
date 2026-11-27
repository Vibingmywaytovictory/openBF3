#include "bf3/render/terrain_scatter.h"

#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/core/util/text.h"
#include "bf3/render/shader_database.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <numbers>
#include <random>
#include <unordered_map>

namespace bf3::render {

namespace {

constexpr std::string_view kComboSuffix = "__maskscale2d__0";
constexpr std::string_view kScattered = "ms";
constexpr std::string_view kMasking = "m";
constexpr size_t kMaxScatterImages = 64;
// INFERRED: record byte 66 set marks a type that stands upright; notes R5-VG2 C read it only as the gate of the
// billboard flag at byte 67. Settle: the scatter placement traced.
constexpr size_t kUprightByte = 66;

using core::to_lower_ascii;

float unit(std::mt19937 &r_rng) {
	return float(r_rng() >> 8) * (1.0f / 16777216.0f);
}

// STAND-IN: the candidates' place, yaw and size come from a Mersenne twister seeded by cell and type.
// Settle: the engine's scatter generator traced.
uint32_t cell_seed(const ScatterCellId &p_cell, uint32_t p_type) {
	uint64_t h = core::splitmix64(uint64_t(p_cell.level));
	h = core::splitmix64(h ^ uint64_t(uint32_t(p_cell.x)));
	h = core::splitmix64(h ^ uint64_t(uint32_t(p_cell.z)));
	h = core::splitmix64(h ^ uint64_t(p_type));
	return uint32_t(h ^ (h >> 32));
}

}

std::array<float, 12> scatter_instance_rows(const ScatterInstance &p_instance) {
	const float c = std::cos(p_instance.yaw) * p_instance.scale[0];
	const float s = std::sin(p_instance.yaw) * p_instance.scale[0];
	const std::array<std::array<float, 3>, 3> upright = { { { c, 0.0f, s }, { 0.0f, p_instance.scale[1], 0.0f },
			{ -s, 0.0f, c } } };
	const auto &[ux, uy, uz] = p_instance.up;
	const float k = 1.0f / (1.0f + uy);
	const std::array<std::array<float, 3>, 3> tilt = { { { 1.0f - ux * ux * k, ux, -ux * uz * k }, { -ux, uy, -uz },
			{ -ux * uz * k, uz, 1.0f - uz * uz * k } } };
	std::array<float, 12> out{};
	for (size_t r = 0; r < 3; ++r) {
		for (size_t col = 0; col < 3; ++col) {
			out[r * 4 + col] =
					tilt[r][0] * upright[0][col] + tilt[r][1] * upright[1][col] + tilt[r][2] * upright[2][col];
		}
		out[r * 4 + 3] = p_instance.position[r];
	}
	return out;
}

std::optional<ScatterComboName> terrain_scatter_combo(std::string_view p_shader, std::string_view p_asset) {
	const std::string name = to_lower_ascii(p_shader);
	const std::string prefix = to_lower_ascii(p_asset) + "__";
	if (name.size() <= prefix.size() + kComboSuffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
			name.compare(name.size() - kComboSuffix.size(), kComboSuffix.size(), kComboSuffix) != 0) {
		return std::nullopt;
	}
	const std::string_view middle =
			std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - kComboSuffix.size());
	ScatterComboName out;
	size_t at = 0;
	while (at <= middle.size()) {
		const size_t end = std::min(middle.find('_', at), middle.size());
		const std::string_view token = middle.substr(at, end - at);
		size_t digits = 0;
		while (digits < token.size() && token[digits] >= '0' && token[digits] <= '9') {
			++digits;
		}
		unsigned value = 0;
		const std::string_view kind = token.substr(digits);
		if (digits == 0 || std::from_chars(token.data(), token.data() + digits, value).ec != std::errc() ||
				value > 63 || (kind != kScattered && kind != kMasking)) {
			return std::nullopt;
		}
		out.layers.push_back(uint8_t(value));
		if (kind == kScattered) {
			out.scattered.push_back(uint8_t(value));
		}
		at = end + 1;
	}
	std::sort(out.layers.begin(), out.layers.end());
	std::sort(out.scattered.begin(), out.scattered.end());
	if (std::adjacent_find(out.layers.begin(), out.layers.end()) != out.layers.end() || out.scattered.empty() ||
			out.scattered.size() > 8) {
		return std::nullopt;
	}
	return out;
}

SceneScatterPlan plan_terrain_scatter(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_mesh,
		std::vector<std::pair<std::string, std::string>> &r_failures) {
	SceneScatterPlan plan;
	const std::string res_name = core::res::terrain_visual_name(p_asset);
	core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(res_name);
	if (!res) {
		return plan;
	}
	core::res::TerrainVisual visual;
	const core::res::ResError e = core::res::TerrainVisual::parse(res->second, visual);
	if (!e.ok()) {
		r_failures.emplace_back("res " + res_name, e.message());
		return plan;
	}
	plan.cell_instances = visual.cell_instances();
	std::map<std::string, int32_t> mesh_of;
	for (core::res::TerrainScatterType &type : visual.types) {
		const std::string mesh = to_lower_ascii(type.mesh);
		auto found = mesh_of.find(mesh);
		if (found == mesh_of.end()) {
			found = mesh_of.emplace(mesh, p_mesh(mesh)).first;
		}
		plan.types.push_back(SceneScatterType{ std::move(type), found->second });
	}
	if (plan.types.empty()) {
		return plan;
	}
	std::vector<std::string> names;
	for (const std::string &name : p_mount.ebx_names()) {
		if (terrain_scatter_combo(name, p_asset)) {
			names.push_back(to_lower_ascii(name));
		}
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	for (const std::string &name : names) {
		const SurfaceShader *shader = p_shaders.by_name(name);
		if (shader == nullptr) {
			r_failures.emplace_back("scattering combination " + name, "not in the shader database");
			continue;
		}
		ScatterComboName parsed = *terrain_scatter_combo(name, p_asset);
		SceneScatterCombo combo;
		combo.shader = name;
		combo.layers = std::move(parsed.layers);
		combo.scattered = std::move(parsed.scattered);
		const ShaderSolution *solution = nullptr;
		// STAND-IN: the first solution with a pixel program stands for the one the engine picks. Settle: the solution choice read.
		for (const ShaderSolution &s : shader->solutions) {
			if (s.pixel_program) {
				solution = &s;
				break;
			}
		}
		if (solution == nullptr) {
			r_failures.emplace_back("scattering combination " + name, "no pixel program");
			continue;
		}
		combo.program = solution->pixel_program;
		const std::vector<std::pair<uint32_t, std::string>> &slots =
				solution->pixel_constants != nullptr ? solution->pixel_constants->textures : shader->texture_slots;
		for (const auto &[slot, texture] : slots) {
			combo.textures.emplace_back(slot, to_lower_ascii(texture));
		}
		plan.combos.push_back(std::move(combo));
	}
	return plan;
}

core::Result<std::shared_ptr<const TerrainScatter>> TerrainScatter::build(
		const Input &p_input, std::vector<std::pair<std::string, std::string>> &r_failures) {
	if (p_input.plan == nullptr || p_input.tree == nullptr || p_input.heights == nullptr ||
			!p_input.tree->heightfield || p_input.tree->heightfield->nodes.empty()) {
		return core::make_error("terrain scatter needs its plan, the streaming tree and the heights");
	}
	std::shared_ptr<TerrainScatter> out = std::make_shared<TerrainScatter>();
	const core::res::ResError e = core::res::TerrainMaskField::build(*p_input.tree, p_input.fetch, out->masks_);
	if (!e.ok()) {
		return core::make_error("terrain scatter masks: " + e.message());
	}
	out->heights_ = ScatterHeights::from_grid(*p_input.heights, p_input.crop);
	const core::res::HeightfieldNode &root = p_input.tree->heightfield->nodes.front();
	out->bounds_ = { root.bounds_min[0], root.bounds_min[2], root.bounds_max[0], root.bounds_max[2] };
	if (!(out->bounds_[2] > out->bounds_[0]) || !(out->bounds_[3] > out->bounds_[1])) {
		return core::make_error("terrain scatter: empty terrain bounds");
	}

	std::map<std::string, int32_t> image_of;
	const auto image = [&](const std::string &p_name) -> int32_t {
		const auto found = image_of.find(p_name);
		if (found != image_of.end()) {
			return found->second;
		}
		int32_t index = -1;
		if (out->images_.size() < kMaxScatterImages && p_input.image) {
			core::Result<ScatterImage> loaded = p_input.image(p_name);
			if (loaded) {
				index = int32_t(out->images_.size());
				out->images_.push_back(std::move(*loaded));
			} else {
				r_failures.emplace_back("scattering texture " + p_name, loaded.error().message);
			}
		}
		image_of.emplace(p_name, index);
		return index;
	};
	struct Pending {
		const SceneScatterCombo *combo = nullptr;
		std::vector<std::pair<uint32_t, int32_t>> images;
	};
	std::vector<Pending> pending;
	for (const SceneScatterCombo &combo : p_input.plan->combos) {
		if (combo.program == nullptr) {
			continue;
		}
		Pending p;
		p.combo = &combo;
		for (const auto &[slot, name] : combo.textures) {
			p.images.emplace_back(slot, image(name));
		}
		pending.push_back(std::move(p));
	}
	const ScatterSamplers samplers{ &out->masks_, &out->heights_, out->images_ };
	for (const Pending &p : pending) {
		const SceneScatterCombo &combo = *p.combo;
		if (std::any_of(combo.layers.begin(), combo.layers.end(),
					[&](uint8_t p_l) { return p_l >= out->masks_.layers(); })) {
			r_failures.emplace_back("scattering combination " + combo.shader, "names a layer the masks lack");
			continue;
		}
		core::Result<MaskScaleProgram> code = MaskScaleProgram::build(*combo.program, combo.layers, out->masks_,
				out->heights_, [&](uint32_t p_slot) -> std::optional<int32_t> {
					const auto found = std::find_if(p.images.begin(), p.images.end(),
							[&](const std::pair<uint32_t, int32_t> &p_x) { return p_x.first == p_slot; });
					if (found == p.images.end()) {
						return std::nullopt;
					}
					return found->second;
				});
		if (!code) {
			r_failures.emplace_back("scattering combination " + combo.shader, code.error().message);
			continue;
		}
		code->measure(samplers);
		out->programs_.push_back(Program{ combo.layers, combo.scattered, std::move(*code) });
	}

	// INFERRED: a cell holds density x area candidates, at most the visual header's fourth word.
	// Settle: the per-cell count traced (notes R5-VG2 found the settings only).
	const double cap = p_input.plan->cell_instances > 0 ? double(p_input.plan->cell_instances) : double(UINT32_MAX);
	for (uint32_t i = 0; i < p_input.plan->types.size(); ++i) {
		const SceneScatterType &source = p_input.plan->types[i];
		ScatterType type;
		type.source = i;
		type.mesh = source.mesh;
		type.layer = source.data.layer;
		type.tree_level = source.data.tree_level;
		type.density = source.data.density;
		type.scale_min = source.data.scale_min;
		type.scale_max = source.data.scale_max;
		type.cell = out->cell_size(type.tree_level);
		const std::array<float, 4> rect = out->cell_rect(ScatterCellId{ type.tree_level, 0, 0 });
		const double area = double(rect[2] - rect[0]) * double(rect[3] - rect[1]);
		type.candidates = uint32_t(std::clamp(std::round(double(type.density) * area), 0.0, cap));
		type.footprint = type.candidates > 0 ? float(std::sqrt(area / double(type.candidates))) : 0.0f;
		type.upright = source.data.record[kUprightByte] != 0;
		out->types_.push_back(type);
		if (type.mesh >= 0 && type.candidates > 0 && type.layer < out->masks_.layers() &&
				std::find(out->levels_.begin(), out->levels_.end(), type.tree_level) == out->levels_.end()) {
			out->levels_.push_back(type.tree_level);
		}
	}
	std::sort(out->levels_.begin(), out->levels_.end());
	return std::shared_ptr<const TerrainScatter>(std::move(out));
}

float TerrainScatter::cell_size(uint8_t p_level) const {
	return (bounds_[2] - bounds_[0]) / float(uint32_t(1) << p_level);
}

std::array<float, 4> TerrainScatter::cell_rect(const ScatterCellId &p_cell) const {
	const double cells = double(uint32_t(1) << p_cell.level);
	const double sx = (double(bounds_[2]) - double(bounds_[0])) / cells;
	const double sz = (double(bounds_[3]) - double(bounds_[1])) / cells;
	return { float(double(bounds_[0]) + double(p_cell.x) * sx), float(double(bounds_[1]) + double(p_cell.z) * sz),
		float(double(bounds_[0]) + double(p_cell.x + 1) * sx), float(double(bounds_[1]) + double(p_cell.z + 1) * sz) };
}

std::vector<ScatterCellId> TerrainScatter::cells_within(uint8_t p_level, float p_x, float p_z, float p_distance) const {
	std::vector<ScatterCellId> out;
	const int64_t cells = int64_t(1) << p_level;
	const double sx = (double(bounds_[2]) - double(bounds_[0])) / double(cells);
	const double sz = (double(bounds_[3]) - double(bounds_[1])) / double(cells);
	const auto index = [&](double p_v, double p_o, double p_s) {
		return std::clamp<int64_t>(int64_t(std::floor((p_v - p_o) / p_s)), 0, cells - 1);
	};
	const int64_t x0 = index(double(p_x) - double(p_distance), bounds_[0], sx);
	const int64_t x1 = index(double(p_x) + double(p_distance), bounds_[0], sx);
	const int64_t z0 = index(double(p_z) - double(p_distance), bounds_[1], sz);
	const int64_t z1 = index(double(p_z) + double(p_distance), bounds_[1], sz);
	for (int64_t z = z0; z <= z1; ++z) {
		for (int64_t x = x0; x <= x1; ++x) {
			const ScatterCellId cell{ p_level, int32_t(x), int32_t(z) };
			const std::array<float, 4> rect = cell_rect(cell);
			const float dx = std::fmax(std::fmax(rect[0] - p_x, p_x - rect[2]), 0.0f);
			const float dz = std::fmax(std::fmax(rect[1] - p_z, p_z - rect[3]), 0.0f);
			if (dx * dx + dz * dz <= p_distance * p_distance) {
				out.push_back(cell);
			}
		}
	}
	return out;
}

// STAND-IN: the program is the one with the most present layers above the type's, then the most present, then the fewest.
// Settle: the engine's program choice read.
int32_t TerrainScatter::program_for(uint8_t p_layer, uint64_t p_present) const {
	int32_t best = -1;
	int best_above = -1, best_present = -1;
	size_t best_size = 0;
	for (size_t i = 0; i < programs_.size(); ++i) {
		const Program &p = programs_[i];
		if (std::find(p.scattered.begin(), p.scattered.end(), p_layer) == p.scattered.end()) {
			continue;
		}
		int above = 0, present = 0;
		for (uint8_t l : p.layers) {
			if (l != 0 && (p_present & (uint64_t(1) << l)) != 0) {
				++present;
				above += l > p_layer ? 1 : 0;
			}
		}
		if (best < 0 || above > best_above || (above == best_above && present > best_present) ||
				(above == best_above && present == best_present && p.layers.size() < best_size)) {
			best = int32_t(i);
			best_above = above;
			best_present = present;
			best_size = p.layers.size();
		}
	}
	return best;
}

std::vector<ScatterBatch> TerrainScatter::generate(
		const ScatterCellId &p_cell, ScatterCellStats *r_stats, std::span<const ScatterFootprint> p_footprints) const {
	std::vector<ScatterBatch> out;
	const std::array<float, 4> rect = cell_rect(p_cell);
	const ScatterSamplers samplers{ &masks_, &heights_, images_ };
	const uint8_t layers = masks_.layers();
	std::unordered_map<uint64_t, int32_t> chosen;
	ScatterCellStats stats;
	for (uint32_t t = 0; t < types_.size(); ++t) {
		const ScatterType &type = types_[t];
		if (type.tree_level != p_cell.level || type.mesh < 0 || type.candidates == 0 || type.layer >= layers) {
			continue;
		}
		std::mt19937 rng(cell_seed(p_cell, type.source));
		ScatterBatch batch;
		batch.type = t;
		batch.candidates = type.candidates;
		for (uint32_t i = 0; i < type.candidates; ++i) {
			const float u = unit(rng);
			const float v = unit(rng);
			const float accept = unit(rng);
			const float size = unit(rng);
			const float turn = unit(rng);
			++stats.candidates;
			const float x = rect[0] + u * (rect[2] - rect[0]);
			const float z = rect[1] + v * (rect[3] - rect[1]);
			if (type.layer != 0 && !(masks_.coverage(type.layer, x, z) > 0.0f)) {
				++stats.masked;
				continue;
			}
			const std::optional<float> y = heights_.surface(x, z);
			if (!y) {
				++stats.masked;
				continue;
			}
			uint64_t present = 1;
			for (uint8_t l = 1; l < layers; ++l) {
				if (masks_.coverage(l, x, z) > 0.0f) {
					present |= uint64_t(1) << l;
				}
			}
			const uint64_t key = present | (uint64_t(type.layer) << 56);
			auto found = chosen.find(key);
			if (found == chosen.end()) {
				found = chosen.emplace(key, program_for(type.layer, present)).first;
			}
			if (found->second < 0) {
				++stats.unmatched;
				continue;
			}
			const Program &program = programs_[size_t(found->second)];
			const size_t slot = size_t(std::find(program.scattered.begin(), program.scattered.end(), type.layer) -
					program.scattered.begin());
			const MaskScaleOutput result = program.code.run(samplers, x, z, type.footprint);
			++stats.evaluated;
			++batch.evaluated;
			const float mask = result.mask[slot];
			if (mask > 0.0f) {
				batch.coverage += double(std::fmin(mask, 1.0f));
			}
			// INFERRED: a candidate stays with the probability its program's mask gives. Settle: the scatter placement traced.
			if (!(accept < mask)) {
				continue;
			}
			const float scale = result.scale[slot];
			// INFERRED: the size is drawn between the two scale pairs, times the program's scale. Settle: the placement traced.
			const float sx = (type.scale_min[0] + (type.scale_max[0] - type.scale_min[0]) * size) * scale;
			const float sy = (type.scale_min[1] + (type.scale_max[1] - type.scale_min[1]) * size) * scale;
			if (!(sx > 0.0f) || !(sy > 0.0f) || !std::isfinite(sx) || !std::isfinite(sy)) {
				continue;
			}
			// INFERRED: a uniform yaw, then a tilt onto the drawn terrain and a drop until the footprint meets it, but for upright
			// types. Settle: the placement traced (notes R5-VG2 did not find it).
			ScatterInstance instance{ { x, *y, z }, turn * 2.0f * std::numbers::pi_v<float>, { sx, sy } };
			if (const std::optional<std::array<float, 3>> up = type.upright ? std::nullopt : heights_.normal(x, z)) {
				instance.up = *up;
				if (size_t(type.mesh) < p_footprints.size() && p_footprints[size_t(type.mesh)]) {
					const std::array<float, 4> &f = *p_footprints[size_t(type.mesh)];
					const std::array<float, 12> rows = scatter_instance_rows(instance);
					std::array<std::array<float, 2>, 4> quad{};
					const std::array<std::array<float, 2>, 4> corners = { { { f[0], f[1] }, { f[2], f[1] },
							{ f[2], f[3] }, { f[0], f[3] } } };
					for (size_t c = 0; c < 4; ++c) {
						quad[c] = { rows[0] * corners[c][0] + rows[2] * corners[c][1] + rows[3],
							rows[8] * corners[c][0] + rows[10] * corners[c][1] + rows[11] };
					}
					const float drop = heights_.drop_under(quad, instance.position, instance.up);
					for (size_t k = 0; k < 3; ++k) {
						instance.position[k] -= instance.up[k] * drop;
					}
				}
			}
			batch.instances.push_back(instance);
		}
		stats.instances += batch.instances.size();
		stats.coverage += batch.coverage;
		out.push_back(std::move(batch));
	}
	if (r_stats != nullptr) {
		*r_stats = stats;
	}
	return out;
}

std::optional<std::array<float, 2>> TerrainScatter::mask_scale(uint32_t p_type, float p_x, float p_z) const {
	if (p_type >= types_.size()) {
		return std::nullopt;
	}
	const ScatterType &type = types_[p_type];
	uint64_t present = 1;
	for (uint8_t l = 1; l < masks_.layers(); ++l) {
		if (masks_.coverage(l, p_x, p_z) > 0.0f) {
			present |= uint64_t(1) << l;
		}
	}
	const int32_t index = program_for(type.layer, present);
	if (index < 0) {
		return std::nullopt;
	}
	const Program &program = programs_[size_t(index)];
	const size_t slot = size_t(
			std::find(program.scattered.begin(), program.scattered.end(), type.layer) - program.scattered.begin());
	const ScatterSamplers samplers{ &masks_, &heights_, images_ };
	const MaskScaleOutput result = program.code.run(samplers, p_x, p_z, type.footprint);
	return std::array<float, 2>{ result.mask[slot], result.scale[slot] };
}

uint64_t TerrainScatter::bytes() const {
	uint64_t total = masks_.bytes() + heights_.samples.size() * 2 + heights_.holes.size();
	for (const ScatterImage &image : images_) {
		for (const std::vector<uint8_t> &mip : image.mips) {
			total += mip.size();
		}
	}
	return total;
}

}
