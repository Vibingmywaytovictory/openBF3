#include "bf3/fx/emitter_atlas.h"

#include "bf3/core/res/res_loader.h"
#include "fx_ebx.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace bf3::fx {

namespace {

constexpr size_t kMaxTextureInfos = 4096;
constexpr uint32_t kMaxAtlasSize = 16384;
constexpr uint32_t kMaxAtlasMips = 15;

bool ends_with(std::string_view p_s, std::string_view p_suffix) {
	return p_s.size() >= p_suffix.size() && p_s.compare(p_s.size() - p_suffix.size(), p_suffix.size(), p_suffix) == 0;
}

uint32_t to_pixel(float p_uv, uint32_t p_size, bool &r_exact) {
	const double px = double(p_uv) * double(p_size);
	const double rounded = std::round(px);
	if (std::fabs(px - rounded) > 0.01) {
		r_exact = false;
	}
	return uint32_t(std::clamp(rounded, 0.0, double(p_size)));
}

}

core::Result<AtlasTextureInfo> EmitterAtlas::texture_info(const std::string &p_partition, std::span<const uint8_t> p_res) {
	core::res::DxTexture tex;
	const core::res::ResError e = core::res::DxTexture::parse(p_res, tex);
	if (!e.ok()) {
		return core::make_error(p_partition + ": " + e.message());
	}
	AtlasTextureInfo out;
	out.partition = p_partition;
	out.format = tex.format;
	out.srgb = tex.is_srgb();
	out.width = tex.width;
	out.height = tex.height;
	out.mips = tex.mip_count;
	if (out.width == 0 || out.height == 0 || out.width > kMaxAtlasSize || out.height > kMaxAtlasSize || out.mips > kMaxAtlasMips) {
		return core::make_error(p_partition + ": atlas header " + std::to_string(out.width) + " x " + std::to_string(out.height) + ", " +
				std::to_string(out.mips) + " mips out of range");
	}
	return out;
}

std::string EmitterAtlas::emitter_system_of(std::string_view p_level) {
	const std::string l = detail::canonical_name(p_level);
	return "levels/" + l + "/" + l + "/emittersystem_win32";
}

AtlasKind EmitterAtlas::kind_of_name(std::string_view p_name) {
	const std::string n = detail::canonical_name(p_name);
	if (ends_with(n, "_n") || ends_with(n, "_nr") || ends_with(n, "_nrm") || n.find("normalmap") != std::string::npos) {
		return AtlasKind::Normal;
	}
	return AtlasKind::Base;
}

core::Result<EmitterAtlas> EmitterAtlas::load(const core::EbxResolver &p_db, const core::LevelMount &p_mount, std::string_view p_system,
		core::gameplay::WorkBudget &r_budget) {
	const ResSource res = [&p_mount](const std::string &p_partition) -> core::Result<std::vector<uint8_t>> {
		core::Result<core::res::RawRes> raw = core::res::read_raw_res(p_mount, p_partition);
		if (!raw) {
			return core::make_error(p_partition + ": " + raw.error().message);
		}
		return std::move(raw->payload);
	};
	return load(p_db, res, p_system, r_budget);
}

core::Result<EmitterAtlas> EmitterAtlas::load(const core::EbxResolver &p_db, const ResSource &p_res, std::string_view p_system,
		core::gameplay::WorkBudget &r_budget) {
	detail::Reader r(p_db, r_budget);
	const std::string system = detail::canonical_name(p_system);
	r.set_where(system);
	const core::EbxPartition *p = p_db.partition_by_name(system);
	if (p == nullptr) {
		return core::make_error(system + ": partition not found");
	}
	const core::EbxInstance primary = p->primary();
	if (primary.type_name() != "EmitterSystemAsset") {
		return core::make_error(system + ": primary instance is " + std::string(primary.type_name()) + ", not an EmitterSystemAsset");
	}
	const core::EbxStruct s = primary.data();
	EmitterAtlas out;
	const std::string base = r.external_name(s, "BaseAtlasTexture");
	const std::string normal = r.external_name(s, "NormalAtlasTexture");
	const core::OptionalEbxArray infos = r.array(s, "TextureInfos", kMaxTextureInfos);
	if (!r.ok()) {
		return r.error();
	}
	if (base.empty() || normal.empty()) {
		return core::make_error(system + ": atlas texture reference missing");
	}
	if (!p_res) {
		return core::make_error(system + ": no texture res source");
	}
	const core::Result<std::vector<uint8_t>> base_res = p_res(base);
	if (!base_res) {
		return base_res.error();
	}
	core::Result<AtlasTextureInfo> b = texture_info(base, *base_res);
	if (!b) {
		return b.error();
	}
	const core::Result<std::vector<uint8_t>> normal_res = p_res(normal);
	if (!normal_res) {
		return normal_res.error();
	}
	core::Result<AtlasTextureInfo> n = texture_info(normal, *normal_res);
	if (!n) {
		return n.error();
	}
	out.base_ = std::move(*b);
	out.normal_ = std::move(*n);
	out.entries_.reserve(infos->size());
	for (const core::EbxValue v : *infos) {
		const std::optional<core::EbxStruct> info = v.as_struct();
		if (!info) {
			return core::make_error(system + ": TextureInfos element is not a struct");
		}
		AtlasEntry e;
		e.name = detail::canonical_name(r.text(*info, "TextureName"));
		e.min_uv = r.vec2(*info, "MinUv");
		e.max_uv = r.vec2(*info, "MaxUv");
		if (!r.ok()) {
			return r.error();
		}
		e.kind = kind_of_name(e.name);
		e.degenerate = e.min_uv.x > e.max_uv.x || e.min_uv.y > e.max_uv.y;
		const AtlasTextureInfo &t = e.kind == AtlasKind::Normal ? out.normal_ : out.base_;
		bool exact = true;
		e.px_min_x = to_pixel(e.min_uv.x, t.width, exact);
		e.px_min_y = to_pixel(e.min_uv.y, t.height, exact);
		e.px_max_x = to_pixel(e.max_uv.x, t.width, exact);
		e.px_max_y = to_pixel(e.max_uv.y, t.height, exact);
		e.pixel_exact = exact;
		out.entries_.push_back(std::move(e));
	}
	out.by_name_.resize(out.entries_.size());
	for (uint32_t i = 0; i < out.by_name_.size(); ++i) {
		out.by_name_[i] = i;
	}

	if (!r.charge(1 + out.by_name_.size() * size_t(std::bit_width(out.by_name_.size())))) {
		return r.error();
	}
	std::stable_sort(out.by_name_.begin(), out.by_name_.end(), [&](uint32_t p_a, uint32_t p_b) { return out.entries_[p_a].name < out.entries_[p_b].name; });
	return out;
}

const AtlasEntry *EmitterAtlas::resolve(std::string_view p_name) const {
	const std::string n = detail::canonical_name(p_name);
	const auto it = std::lower_bound(by_name_.begin(), by_name_.end(), n, [&](uint32_t p_i, const std::string &p_n) { return entries_[p_i].name < p_n; });
	if (it == by_name_.end() || entries_[*it].name != n) {
		return nullptr;
	}
	return &entries_[*it];
}

void EmitterAtlas::frame_rect(const AtlasEntry &p_entry, uint32_t p_frame, uint32_t p_frame_count, uint32_t p_frame_columns, Vec2 &r_min, Vec2 &r_max) {
	const uint32_t count = std::max(1u, p_frame_count);
	const uint32_t columns = std::clamp(p_frame_columns, 1u, count);
	const uint32_t rows = (count + columns - 1) / columns;
	const uint32_t f = p_frame % count;
	const uint32_t col = f % columns;
	const uint32_t row = f / columns;
	const float w = (p_entry.max_uv.x - p_entry.min_uv.x) / float(columns);
	const float h = (p_entry.max_uv.y - p_entry.min_uv.y) / float(rows);
	r_min = Vec2{ p_entry.min_uv.x + w * float(col), p_entry.min_uv.y + h * float(row) };
	r_max = Vec2{ r_min.x + w, r_min.y + h };
}

}
