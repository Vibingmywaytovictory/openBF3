#include "bf3/core/ui/minimap.h"

#include "bf3/core/install.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"
#include "ui_ebx.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace bf3::core::ui {

Result<L8Image> decode_l8_texture(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name) {
	const res::DxTexture &tex = p_texture;
	const std::string where = "res " + std::string(p_name);
	if (tex.format != res::TextureFormat::L8) {
		return make_error(where + ": format " + std::to_string(int(tex.format)) + ", not L8");
	}
	if (tex.kind != res::TextureKind::Texture2D || tex.faces() != 1) {
		return make_error(where + ": not a 2D texture with one face");
	}
	if (tex.width == 0 || tex.height == 0 || tex.width > kMaxMinimapImageSide || tex.height > kMaxMinimapImageSide) {
		return make_error(where + ": size " + std::to_string(tex.width) + "x" + std::to_string(tex.height) + " outside 1.." +
				std::to_string(kMaxMinimapImageSide));
	}
	std::vector<res::MipSlice> mips;
	const res::ResError e = tex.split_mips(res::Bytes(p_chunk.data(), p_chunk.size()), 0, mips);
	if (!e.ok()) {
		return make_error(where + ": " + e.message());
	}
	if (mips.empty()) {
		return make_error(where + ": no mip");
	}
	const res::MipSlice &m = mips.front();
	if (m.width == 0 || m.height == 0 || m.width > kMaxMinimapImageSide || m.height > kMaxMinimapImageSide || m.depth != 1) {
		return make_error(where + ": mip size out of range");
	}
	const size_t pitch = m.row_pitch == 0 ? m.width : m.row_pitch;
	if (pitch < m.width || m.data.size() < pitch * (size_t(m.height) - 1) + m.width) {
		return make_error(where + ": mip " + std::to_string(m.mip) + " holds " + std::to_string(m.data.size()) + " bytes, fewer than " +
				std::to_string(m.width) + "x" + std::to_string(m.height));
	}
	L8Image out;
	out.width = m.width;
	out.height = m.height;
	out.texels.resize(size_t(m.width) * m.height);
	for (uint32_t y = 0; y < m.height; ++y) {
		std::memcpy(out.texels.data() + size_t(y) * m.width, m.data.data() + size_t(y) * pitch, m.width);
	}
	return out;
}

Result<L8Image> load_l8_texture(const LevelMount &p_mount, std::string_view p_res_name) {
	if (p_res_name.empty()) {
		return make_error("L8 texture: no res name");
	}
	Result<res::LoadedTexture> t = res::load_texture(p_mount, p_res_name);
	if (!t) {
		return t.error();
	}
	return decode_l8_texture(t->texture, std::span<const uint8_t>(t->chunk.bytes), p_res_name);
}

Result<LevelMount> mount_minimap_level(const Install &p_install, std::string_view p_level_id) {
	Result<LevelMount> mount = p_install.mount_level(p_level_id);
	if (!mount) {
		return mount.error();
	}
	if (const Status st = mount->load_all_bundles(); !st) {
		return error_at(std::string(p_level_id) + " bundles", st.error());
	}
	return mount;
}

Result<MinimapImages> load_minimap_images(const LevelMount &p_mount, const MinimapLevel &p_level) {
	MinimapImages out;
	Result<L8Image> sat = load_l8_texture(p_mount, p_level.data.satellite);
	if (!sat) {
		return error_at(p_level.data.level_id + " satellite", sat.error());
	}
	out.satellite = std::move(*sat);
	if (!p_level.data.detail.texture.empty()) {
		Result<L8Image> d = load_l8_texture(p_mount, p_level.data.detail.texture);
		if (!d) {
			return error_at(p_level.data.level_id + " detail field", d.error());
		}
		out.detail = std::move(*d);
	}
	if (!p_level.data.vegetation.texture.empty()) {
		Result<L8Image> v = load_l8_texture(p_mount, p_level.data.vegetation.texture);
		if (!v) {
			return error_at(p_level.data.level_id + " vegetation field", v.error());
		}
		out.vegetation = std::move(*v);
	}
	for (const CombatAreaMask &m : p_level.combat_areas) {
		if (m.texture.empty()) {
			out.combat_areas.emplace_back();
			out.surroundings.emplace_back();
			continue;
		}
		Result<L8Image> f = load_l8_texture(p_mount, m.texture);
		if (!f) {
			return error_at(m.asset, f.error());
		}
		out.combat_areas.push_back(std::move(*f));
		if (m.surrounding_texture.empty()) {
			out.surroundings.emplace_back();
			continue;
		}
		Result<L8Image> s = load_l8_texture(p_mount, m.surrounding_texture);
		if (!s) {
			return error_at(m.asset + " surrounding", s.error());
		}
		out.surroundings.push_back(std::move(*s));
	}
	return out;
}

std::vector<L8Image> l8_mip_chain(const L8Image &p_image) {
	std::vector<L8Image> out;
	if (p_image.empty() || p_image.texels.size() != size_t(p_image.width) * p_image.height) {
		return out;
	}
	out.push_back(p_image);
	while (out.back().width > 1 || out.back().height > 1) {
		const L8Image &src = out.back();
		L8Image dst;
		dst.width = std::max(1u, src.width / 2);
		dst.height = std::max(1u, src.height / 2);
		dst.texels.resize(size_t(dst.width) * dst.height);
		for (uint32_t y = 0; y < dst.height; ++y) {
			const uint32_t y0 = std::min(src.height - 1, y * 2);
			const uint32_t y1 = std::min(src.height - 1, y * 2 + 1);
			for (uint32_t x = 0; x < dst.width; ++x) {
				const uint32_t x0 = std::min(src.width - 1, x * 2);
				const uint32_t x1 = std::min(src.width - 1, x * 2 + 1);
				const uint32_t sum = uint32_t(src.texels[size_t(y0) * src.width + x0]) + src.texels[size_t(y0) * src.width + x1] +
						src.texels[size_t(y1) * src.width + x0] + src.texels[size_t(y1) * src.width + x1];
				dst.texels[size_t(y) * dst.width + x] = uint8_t((sum + 2) / 4);
			}
		}
		out.push_back(std::move(dst));
	}
	return out;
}

namespace {

const LevelMount *mount_of(const EbxLibrary &p_library, std::string_view p_name) {
	const std::optional<EbxLibrary::Origin> origin = p_library.origin(p_name);
	if (!origin) {
		return nullptr;
	}
	for (size_t i = 0; i < p_library.superbundle_count(); ++i) {
		if (iequals_ascii(p_library.superbundle(i), origin->superbundle)) {
			return &p_library.mount(i);
		}
	}
	return nullptr;
}

bool level_ui_name(std::string_view p_name) {
	return p_name.starts_with("levels/") && p_name.find("/ui/") != std::string_view::npos;
}

}

Result<MinimapCensus> minimap_census(const EbxLibrary &p_library) {
	MinimapCensus out;
	std::set<std::string> names;
	for (size_t i = 0; i < p_library.superbundle_count(); ++i) {
		for (const std::string &n : p_library.mount(i).ebx_names()) {
			std::string key = asset_key(n);
			if (level_ui_name(key)) {
				names.insert(std::move(key));
			}
		}
	}
	std::set<std::string> levels;
	std::map<std::string, std::string> referenced;
	std::vector<std::pair<std::string, std::string>> field_textures;
	for (const std::string &name : names) {
		const EbxPartition *p = p_library.partition_by_name(name);
		++out.partitions_scanned;
		if (p == nullptr) {
			continue;
		}
		const EbxInstance primary = p->primary();
		if (primary.is_a("UIDistanceFieldAsset")) {
			++out.distance_field_assets;
			const std::optional<EbxRef> tex = detail::ref(primary.data(), "DistanceFieldTexture");
			const std::optional<std::string> tex_name = tex ? detail::ref_partition(p_library, *p, *tex) : std::nullopt;
			if (!tex_name || tex_name->empty()) {
				out.failures.push_back(name + ": DistanceFieldTexture does not resolve");
				continue;
			}
			field_textures.emplace_back(*tex_name, name);
			continue;
		}
		if (!primary.is_a("VeniceUICombatAreaAsset")) {
			continue;
		}
		++out.combat_area_assets;
		level::WorkBudget budget;
		Result<CombatAreaMask> m = read_combat_area(p_library, name, budget);
		if (!m) {
			out.failures.push_back(name + ": " + m.error().message);
			continue;
		}
		++out.combat_area_read;
		levels.insert(name.substr(0, name.find('/', 7)));
		if (m->surrounding_texture.empty()) {
			++out.null_surroundings;
		}
		if (m->texture.empty()) {
			++out.null_distance_fields;
		}
		if (!m->texture.empty()) {
			referenced.emplace(m->texture, name);
		}
		if (!m->surrounding_texture.empty()) {
			referenced.emplace(m->surrounding_texture, name);
		}
	}
	out.levels_with_areas = levels.size();
	out.referenced_fields = referenced.size();

	std::map<std::string, bool> decoded;
	const auto decode = [&](const std::string &p_texture, const std::string &p_owner) -> bool {
		if (const auto it = decoded.find(p_texture); it != decoded.end()) {
			return it->second;
		}
		const LevelMount *mount = mount_of(p_library, p_texture);
		bool ok = false;
		if (mount == nullptr) {
			out.failures.push_back(p_owner + ": no mount holds " + p_texture);
		} else if (Result<L8Image> img = load_l8_texture(*mount, p_texture); !img) {
			out.failures.push_back(p_owner + ": " + img.error().message);
		} else {
			ok = true;
		}
		decoded.emplace(p_texture, ok);
		return ok;
	};
	for (const auto &[texture, asset] : field_textures) {
		out.distance_field_decoded += decode(texture, asset) ? 1 : 0;
	}
	for (const auto &[texture, asset] : referenced) {
		out.referenced_decoded += decode(texture, asset) ? 1 : 0;
	}
	return out;
}

}
