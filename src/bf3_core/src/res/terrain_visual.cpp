#include "bf3/core/res/terrain_visual.h"

#include "bf3/core/util/text.h"
#include "res_reader.h"

#include <cmath>
#include <cstring>

namespace bf3::core::res {

namespace {

constexpr size_t kMaxMeshName = 512;
constexpr size_t kDensityAt = 4;
constexpr size_t kScaleAt = 21;
constexpr size_t kTreeLevelAt = 53;

float record_float(const std::array<uint8_t, kTerrainScatterRecordBytes> &p_record, size_t p_at) {
	float f;
	std::memcpy(&f, p_record.data() + p_at, 4);
	return f;
}

bool positive(float p_value) {
	return std::isfinite(p_value) && p_value >= 0.0f;
}

}

std::string terrain_visual_name(std::string_view p_terrain_asset) {
	return to_lower_ascii(p_terrain_asset) + ".visual";
}

ResError TerrainVisual::parse(Bytes p_res, TerrainVisual &r_visual) {
	detail::Reader reader(p_res);
	TerrainVisual visual;
	for (size_t w = 0; w < kTerrainVisualHeaderWords; ++w) {
		visual.header[w] = reader.u32(4 * w);
	}
	uint64_t at = kTerrainVisualHeaderBytes;
	if (!reader.cstr(at, visual.surface_shader)) {
		return reader.error();
	}
	at += visual.surface_shader.size() + 1;
	const uint32_t layers = reader.u32(at);
	at += 4;
	if (!reader.ok()) {
		return reader.error();
	}
	if (layers > kMaxTerrainVisualLayers) {
		return ResError::unsupported("visual terrain layers", layers);
	}
	for (uint32_t layer = 0; layer < layers; ++layer) {
		visual.layer_flags.push_back(reader.u8(at));
		const uint32_t count = reader.u32(at + 1);
		at += 5;
		if (!reader.ok()) {
			return reader.error();
		}
		if (count > kMaxTerrainScatterTypesPerLayer) {
			return ResError::unsupported("scattering types of a layer", count);
		}
		for (uint32_t i = 0; i < count; ++i) {
			TerrainScatterType type;
			type.layer = uint8_t(layer);
			if (!reader.cstr(at, type.mesh)) {
				return reader.error();
			}
			if (type.mesh.empty() || type.mesh.size() > kMaxMeshName) {
				return ResError::malformed("scattering mesh name", at);
			}
			at += type.mesh.size() + 1;
			const Bytes record = reader.slice(at, kTerrainScatterRecordBytes);
			if (!reader.ok()) {
				return reader.error();
			}
			std::memcpy(type.record.data(), record.data(), kTerrainScatterRecordBytes);
			type.density = record_float(type.record, kDensityAt);
			type.scale_min = { record_float(type.record, kScaleAt), record_float(type.record, kScaleAt + 4) };
			type.scale_max = { record_float(type.record, kScaleAt + 8), record_float(type.record, kScaleAt + 12) };
			type.tree_level = type.record[kTreeLevelAt];
			if (!positive(type.density) || !positive(type.scale_min[0]) || !positive(type.scale_min[1]) ||
					!(type.scale_max[0] >= type.scale_min[0]) || !(type.scale_max[1] >= type.scale_min[1]) ||
					!std::isfinite(type.scale_max[0]) || !std::isfinite(type.scale_max[1])) {
				return ResError::malformed("scattering type density or scale", at);
			}
			if (type.tree_level > kMaxTerrainScatterTreeLevel) {
				return ResError::unsupported("scattering tree level", type.tree_level);
			}
			at += kTerrainScatterRecordBytes;
			visual.types.push_back(std::move(type));
		}
	}
	if (!reader.cstr(at, visual.streaming_tree)) {
		return reader.error();
	}
	r_visual = std::move(visual);
	return {};
}

}
